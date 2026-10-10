// PyroWave decoder on WebGPU compute, without subgroups (POC Ultra, U3.4).
//
// A port of the decoder of PyroWave by Hans-Kristian Arntzen
// (https://github.com/Themaister/pyrowave, commit 509e4f88): the packet parser
// of pyrowave_decoder.cpp and the shaders wavelet_dequant.comp and idwt.comp,
// rewritten in JS and WGSL. Upstream notice, which covers the ported parts:
//
//   Copyright (c) 2025 Hans-Kristian Arntzen
//   SPDX-License-Identifier: MIT
//
// The bitstream is upstream's (bitstream/bitstream.md). What differs from the
// upstream GPU code, on purpose:
// - No subgroup operation: the scans run on workgroup memory, so the decoder
//   works where WebGPU has no `subgroups` (Safari, most mobiles).
// - Coefficients live in one storage buffer instead of FP16/FP32 images, and
//   the inverse wavelet reads them by index. Upstream's mirrored sampler
//   with its coordinate offsets is the JPEG 2000 whole-sample symmetric
//   extension of the interleaved signal; `mirror()` below does it directly.
// - As upstream's desktop default (PYROWAVE_PRECISION=1), the two finest
//   levels and the output planes are stored in FP16, two samples of a row in
//   a word (pack2x16float, which needs no `shader-f16`), the coarser levels in
//   f32, and the arithmetic is f32 everywhere. `fp16: false` keeps it all in
//   f32, for the bench's A/B.
// - The inverse wavelet works on 32x32 tiles in workgroup memory, like
//   upstream's, but in one dispatch per level for all components. Its
//   default shader (idwt2Wgsl, shader 3) lifts in registers as upstream's
//   does, and loads the tiles inside the level without mirroring them. The
//   same without that load (shader 2) and the first port (IDWT_WGSL), which
//   give the same values more slowly, stay for the bench's A/B.
//
// Output: the decoded planes (Y at the aligned size, Cb and Cr at half of it,
// before the DC shift), then `pack` turns them into 8-bit 4:2:0 bytes cropped
// to the picture, which is what the reference decoder writes.
//
// The packet parser and the frame layout are in PyroWaveFrame.js, shared with
// the WebGL2 fallback.

import { LEVELS, NONE, PyroWaveFrame, alignUp } from './PyroWaveFrame.js';

// The levels stored in FP16 when `fp16` (upstream's WaveletFP16Levels), and
// the flag that marks their planes in the blocks' metadata.
const FP16_LEVELS = 2;
const F16_PLANE = 0x80000000;

// An IEEE binary16 value from its bits.
function halfToFloat(h) {
    const e = (h >> 10) & 31;
    const m = h & 1023;
    let v;
    if (e === 0) v = m * 2 ** -24;
    else if (e === 31) v = m ? NaN : Infinity;
    else v = (1024 + m) * 2 ** (e - 25);
    return h & 0x8000 ? -v : v;
}

const DEQUANT_WGSL = /* wgsl */ `
// plane: the word offset of the block's band in coef; its top bit set, an
// FP16 plane.
struct BlockMeta { plane: u32, width: u32, height: u32, xy: u32 }

@group(0) @binding(0) var<storage, read> payload: array<u32>;
@group(0) @binding(1) var<storage, read> offsets: array<u32>;
@group(0) @binding(2) var<storage, read> metas: array<BlockMeta>;
@group(0) @binding(3) var<storage, read_write> coef: array<u32>;
// x: the first block of the dispatch, in send order (a slice starts past the
// blocks before it); order[] turns a send position into a block index.
@group(0) @binding(4) var<uniform> range: vec4u;
@group(0) @binding(5) var<storage, read> order: array<u32>;

var<workgroup> sharedOffset: u32;
var<workgroup> planeOffsets: array<u32, 16>;
var<workgroup> costs: array<u32, 16>;
var<workgroup> signStart: u32;
var<workgroup> scanA: array<u32, 128>;
var<workgroup> scanB: array<u32, 128>;

fn u8at(i: u32) -> u32 { return (payload[i >> 2u] >> ((i & 3u) * 8u)) & 0xffu; }
fn u16at(i: u32) -> u32 { return (payload[i >> 1u] >> ((i & 1u) * 16u)) & 0xffffu; }

// The planes of a 2-bit-per-subblock control word below a bit position.
fn planesBelow(code: u32, bits: u32) -> u32 {
    let mask = select((1u << bits) - 1u, 0xffffffffu, bits >= 32u);
    let lsbs = code & 0x5555u;
    var msbs = code & 0xaaaau;
    msbs = msbs | (msbs >> 1u);
    return countOneBits(lsbs & mask) + countOneBits(msbs & mask);
}

fn decodeQuant(q: u32) -> f32 {
    let e = 4 - i32(q >> 3u);
    let m = i32(q & 7u);
    return f32(8 + m) * exp2(f32(e - 3));
}

@compute @workgroup_size(128)
fn main(@builtin(workgroup_id) wg: vec3u, @builtin(local_invocation_index) li: u32) {
    let blockIndex = order[wg.x + range.x];
    let bm = metas[blockIndex];
    if (li == 0u) { sharedOffset = offsets[blockIndex]; }
    let off = workgroupUniformLoad(&sharedOffset);

    let sub = li & 7u;
    let bx = (li >> 3u) & 3u;
    let by = (li >> 5u) & 3u;
    let lb = by * 4u + bx;
    let x0 = (bm.xy & 0xffffu) * 32u + 8u * bx + 4u * (sub >> 2u);
    let y0 = (bm.xy >> 16u) * 32u + 8u * by + 2u * (sub & 3u);

    var v: array<f32, 8>;
    for (var k = 0u; k < 8u; k++) { v[k] = 0.0; }

    if (off != ${NONE}u) {
        let ballot = payload[off] & 0xffffu;
        let qCode = payload[off + 1u] & 0xffu;
        let pc = countOneBits(ballot);
        let codeBase = off * 2u + 4u;          // u16 index of CodeWords[]
        let qsBase = off * 4u + 8u + pc * 2u;  // byte index of QScale[]
        let dataBase = off * 4u + 8u + pc * 3u;

        if (li < 16u) {
            var cost = 0u;
            if (((ballot >> li) & 1u) != 0u) {
                let idx = countOneBits(ballot & ((1u << li) - 1u));
                let cw = u16at(codeBase + idx);
                let qb = u8at(qsBase + idx) & 0xfu;
                cost = planesBelow(cw, 32u) + qb * 8u;
            }
            costs[li] = cost;
        }
        workgroupBarrier();
        if (li == 0u) {
            var acc = dataBase;
            for (var k = 0u; k < 16u; k++) {
                planeOffsets[k] = acc;
                acc += costs[k];
            }
            signStart = acc * 8u;
        }
        workgroupBarrier();

        var count = 0u;
        if (((ballot >> lb) & 1u) != 0u) {
            let idx = countOneBits(ballot & ((1u << lb) - 1u));
            let cw = u16at(codeBase + idx);
            let qs = u8at(qsBase + idx);
            let qBits = qs & 0xfu;
            let lcode = (cw >> (2u * sub)) & 3u;
            if (cw != 0u) {
                var byteOffset = planesBelow(cw, 2u * sub) + qBits * sub + planeOffsets[lb];
                var mag: array<u32, 8>;
                for (var k = 0u; k < 8u; k++) { mag[k] = 0u; }
                let planes = qBits + lcode;
                for (var q = i32(planes) - 1; q >= 0; q--) {
                    let bv = u8at(byteOffset);
                    for (var b = 0u; b < 8u; b++) {
                        mag[b] = mag[b] | (((bv >> b) & 1u) << u32(q));
                    }
                    byteOffset++;
                }
                let scale = decodeQuant(qCode) * (f32((qs >> 4u) & 0xfu) / 8.0 + 0.25);
                for (var k = 0u; k < 8u; k++) {
                    if (mag[k] != 0u) {
                        v[k] = (f32(mag[k]) + 0.5) * scale;
                        count++;
                    }
                }
            }
        }

        // Sign bits follow all magnitudes, in thread order (8x8 block, then
        // subblock, then pixel): an exclusive scan of the non-zero counts.
        scanA[li] = count;
        workgroupBarrier();
        for (var s = 1u; s < 128u; s = s * 2u) {
            var t = scanA[li];
            if (li >= s) { t += scanA[li - s]; }
            workgroupBarrier();
            scanB[li] = t;
            workgroupBarrier();
            scanA[li] = scanB[li];
            workgroupBarrier();
        }
        let signOffset = signStart + scanA[li] - count;
        let w = signOffset >> 5u;
        let sh = signOffset & 31u;
        var signs = payload[w] >> sh;
        if (sh != 0u) { signs = signs | (payload[w + 1u] << (32u - sh)); }
        var n = 0u;
        for (var k = 0u; k < 8u; k++) {
            if (v[k] != 0.0) {
                if (((signs >> n) & 1u) != 0u) { v[k] = -v[k]; }
                n++;
            }
        }
    }

    // Pixel order inside a 4x2 subblock: column-major, two rows. An FP16
    // plane takes two columns a word; its sides are multiples of 8, so a
    // subblock is all in or all out.
    if ((bm.plane >> 31u) != 0u) {
        if (x0 < bm.width && y0 < bm.height) {
            let row = bm.width >> 1u;
            let at = (bm.plane & 0x7fffffffu) + y0 * row + (x0 >> 1u);
            coef[at] = pack2x16float(vec2f(v[0], v[2]));
            coef[at + 1u] = pack2x16float(vec2f(v[4], v[6]));
            coef[at + row] = pack2x16float(vec2f(v[1], v[3]));
            coef[at + row + 1u] = pack2x16float(vec2f(v[5], v[7]));
        }
    } else {
        for (var k = 0u; k < 8u; k++) {
            let x = x0 + (k >> 1u);
            let y = y0 + (k & 1u);
            if (x < bm.width && y < bm.height) {
                coef[bm.plane + y * bm.width + x] = bitcast<u32>(v[k]);
            }
        }
    }
}
`;

// The inverse wavelet of one level, by 32x32 output tiles, the components
// of the level along z.
// A workgroup loads the four bands under its tile plus 4 samples of apron on
// each side into workgroup memory as the interleaved 2D signal (LL at even x
// and y, HL odd x, LH odd y, HH both odd), already scaled by K / 1/K in each
// direction. It then runs the four CDF 9/7 lifting steps along the rows, then
// along the columns, and writes the tile. Constants and step order are
// upstream's (dwt_common.h, idwt.comp). Scaling and mirroring before the row
// pass is the same as upstream's after it: both are per-row and linear.
const IDWT_WGSL = /* wgsl */ `
// y0: the first row of tiles (the finest level goes by stripes, as its blocks come).
struct Pass { w: u32, h: u32, comps: u32, y0: u32, bands: array<vec4u, 3>, outs: vec4u }
@group(0) @binding(0) var<uniform> p: Pass;
@group(0) @binding(1) var<storage, read_write> coef: array<f32>;

const ALPHA: f32 = -1.586134342059924;
const BETA: f32 = -0.052980118572961;
const GAMMA: f32 = 0.882911075530934;
const DELTA: f32 = 0.443506852043971;
const K: f32 = 1.230174104914001;
const INV_K: f32 = 1.0 / 1.230174104914001;
const SPAN: u32 = 40u;  // 32 + 2 x 4 of apron
const T: u32 = 16u;  // the workgroup is T x T threads

var<workgroup> S: array<f32, 1600>;

// JPEG 2000 whole-sample symmetric extension of a signal of n samples.
fn mirror(i: i32, n: i32) -> i32 {
    var j = i;
    if (j < 0) { j = -j; }
    if (j > n - 1) { j = 2 * (n - 1) - j; }
    return j;
}

// One lifting step on positions first, first+2 .. last of every row (all 40)
// or of the 32 tile columns. Threads walk 2D, with no integer division: the
// AMD iGPU (2 CUs) pays dearly for a division by a value known at run time.
fn liftRows(t: vec2u, first: u32, last: u32, c: f32) {
    for (var r = t.y; r < SPAN; r += T) {
        for (var j = first + 2u * t.x; j <= last; j += 2u * T) {
            let i = r * SPAN + j;
            S[i] -= c * (S[i - 1u] + S[i + 1u]);
        }
    }
    workgroupBarrier();
}

fn liftCols(t: vec2u, first: u32, last: u32, c: f32) {
    for (var col = 4u + t.x; col < 36u; col += T) {
        for (var j = first + 2u * t.y; j <= last; j += 2u * T) {
            let i = j * SPAN + col;
            S[i] -= c * (S[i - SPAN] + S[i + SPAN]);
        }
    }
    workgroupBarrier();
}

@compute @workgroup_size(16, 16)
fn main(@builtin(workgroup_id) wg: vec3u, @builtin(local_invocation_id) lid: vec3u) {
    let t = lid.xy;
    let w2 = 2u * p.w;
    let h2 = 2u * p.h;
    let bands = p.bands[wg.z];  // LL, HL, LH, HH of this component
    let dst = p.outs[wg.z];
    let ox = i32(wg.x * 32u) - 4;
    let ty = wg.y + p.y0;
    let oy = i32(ty * 32u) - 4;
    for (var y = t.y; y < SPAN; y += T) {
        let sy = mirror(oy + i32(y), i32(h2));
        let yOdd = (sy & 1) == 1;
        let row = u32(sy >> 1) * p.w;
        for (var x = t.x; x < SPAN; x += T) {
            let sx = mirror(ox + i32(x), i32(w2));
            let xOdd = (sx & 1) == 1;
            // select(), not bands[i]: a vector indexed at run time can turn
            // into a scratch array in the HLSL that Chrome makes of this.
            let band = select(select(bands.x, bands.y, xOdd), select(bands.z, bands.w, xOdd), yOdd);
            let scale = select(K, INV_K, xOdd) * select(K, INV_K, yOdd);
            S[y * SPAN + x] = coef[band + row + u32(sx >> 1)] * scale;
        }
    }
    workgroupBarrier();

    // Each step only where its inputs are right: the tile's 32 samples need
    // 4 of apron after four steps.
    liftRows(t, 2u, 38u, DELTA);
    liftRows(t, 3u, 37u, GAMMA);
    liftRows(t, 4u, 36u, BETA);
    liftRows(t, 5u, 35u, ALPHA);
    liftCols(t, 2u, 38u, DELTA);
    liftCols(t, 3u, 37u, GAMMA);
    liftCols(t, 4u, 36u, BETA);
    liftCols(t, 5u, 35u, ALPHA);

    for (var y = t.y; y < 32u; y += T) {
        let gy = ty * 32u + y;
        for (var x = t.x; x < 32u; x += T) {
            let gx = wg.x * 32u + x;
            if (gx < w2 && gy < h2) {
                coef[dst + gy * w2 + gx] = S[(y + 4u) * SPAN + x + 4u];
            }
        }
    }
}
`;

// WGSL lines, one per k < n, indented for the shader body.
const lines = (n, f) => Array.from({ length: n }, (_, k) => f(k)).join('\n    ');

// The four lifting steps of the inverse CDF 9/7 on the samples v0 .. v(n-1)
// of one run, unrolled so that they stay in registers: v4 .. v(n-5) come out
// right (each step leaves one sample less at each end).
function lift(v, n) {
    const out = [];
    for (const [first, c] of [
        [2, 'DELTA'],
        [3, 'GAMMA'],
        [4, 'BETA'],
        [5, 'ALPHA'],
    ]) {
        for (let i = first; i <= n - first; i += 2)
            out.push(`${v}${i} -= ${c} * (${v}${i - 1} + ${v}${i + 1});`);
    }
    return out.join('\n    ');
}

// The same transform as IDWT_WGSL, value for value, laid out as upstream's
// idwt.comp: 64 threads per 32x32 tile, each lifting a run of 8 outputs
// (16 samples with their apron) of two rows at once in registers, then of
// two columns. Workgroup memory only holds the tile between the steps, three
// barriers instead of nine. Entries of S pair two rows (a pair of rows i:
// rows 2i and 2i + 1), then, after the row pass, two columns.
// in16 / out16: the level's bands / its output are FP16 planes, two columns
// a word; else f32, a word each.
// fast (shader 3): a tile whose window lies inside the level, with nothing
// to mirror, loads the runs of its row pass straight from the bands into
// the registers, a word at a time, without the mirror's arithmetic and the
// window's trip through S. The values are the same. The mirrored load was
// most of the shader's instructions: an AMD iGPU's transform takes 40 % less.
const idwt2Wgsl = (in16, out16, fast = false) => {
    // The mirrored load of the whole window into S, any tile.
    const mirrored = `for (var i = li; i < 20u * SPAN; i += 64u) {
        let pair = i / SPAN;
        let x = i - pair * SPAN;
        let xOdd = (x & 1u) == 1u;
        let col = u32(mirror(ox + i32(x), i32(w2)) >> 1);
        let y = oy + i32(2u * pair);
        let row0 = u32(mirror(y, i32(h2)) >> 1);
        let row1 = u32(mirror(y + 1, i32(h2)) >> 1);
        let sx = select(K, INV_K, xOdd);
        let a = ld(select(bands.x, bands.y, xOdd), row0, col) * (sx * K);
        let b = ld(select(bands.z, bands.w, xOdd), row1, col) * (sx * INV_K);
        S[pair * PITCH + x] = vec2f(a, b);
    }`;
    // Words j < n at word offset `at` of the four bands, named <b><v><j>
    // (b: ll, hl, lh, hh): two samples of a row in FP16, one in f32.
    const words = (at, n, v) =>
        lines(n, (j) =>
            ['ll', 'hl', 'lh', 'hh']
                .map(
                    (b, k) =>
                        `let ${b}${v}${j} = ${in16 ? 'unpack2x16float' : 'bitcast<f32>'}(coef[bands.${'xyzw'[k]} + ${at} + ${j}u]);`,
                )
                .join('\n    '),
        );
    // The registers v<k> of the run that word j fills: each sample at even
    // then odd x, scaled as the mirrored load does (four from an FP16 word,
    // two from an f32 one).
    const entries = (j, v) => {
        const pair = (h) => [
            `vec2f(ll${v}${j}${h} * (K * K), lh${v}${j}${h} * (K * INV_K))`,
            `vec2f(hl${v}${j}${h} * (INV_K * K), hh${v}${j}${h} * (INV_K * INV_K))`,
        ];
        const all = in16 ? [...pair('.x'), ...pair('.y')] : pair('');
        return all.map((e, k) => `${v}${all.length * j + k} = ${e};`).join('\n    ');
    };
    // An interior tile's runs: r0-r15 (pairs 0-15, columns c0 + rx / 2 on)
    // and, for the apron (pairs 16-19, columns c0 + qx / 2 on), q0-q11.
    const runs = in16
        ? `let at = (br + rp) * wrow + m0 + 2u * (li & 3u);
        ${words('at', 4, 'r')}
        ${lines(4, (j) => entries(j, 'r'))}
        if (apron) {
            let qat = (br + qp) * wrow + m0 + (li & 7u);
            ${words('qat', 3, 'q')}
            ${lines(3, (j) => entries(j, 'q'))}
        }`
        : `let at = (br + rp) * p.w + c0 + 4u * (li & 3u);
        ${words('at', 8, 'r')}
        ${lines(8, (j) => entries(j, 'r'))}
        if (apron) {
            let qat = (br + qp) * p.w + c0 + 2u * (li & 7u);
            ${words('qat', 6, 'q')}
            ${lines(6, (j) => entries(j, 'q'))}
        }`;
    const rows = fast
        ? `
    // Inside the level: band rows br .. br + 19, columns c0 = ox / 2 ..
    // c0 + 19 (in FP16, words m0 .. m0 + 9 of a row of wrow).
    let interior = ox >= 0 && oy >= 0 && ox + i32(SPAN) <= i32(w2) && oy + i32(SPAN) <= i32(h2);
    let br = u32(max(oy, 0)) >> 1u;
    ${in16 ? 'let wrow = p.w >> 1u;\n    let m0 = u32(max(ox, 0)) >> 2u;' : 'let c0 = u32(max(ox, 0)) >> 1u;'}

    // Rows: pairs 0-15 by runs of 8 (all threads), pairs 16-19 by runs of 4
    // (half of them). A run starts 4 samples before its outputs. Inside the
    // level, a run comes straight from the bands (8 samples of each, 6 for
    // the apron); else from the mirrored window in S.
    let rp = li >> 2u;
    let rx = 8u * (li & 3u);
    let apron = li < 32u;
    let qp = 16u + (li >> 3u);
    let qx = 4u * (li & 7u);
    ${lines(16, (k) => `var r${k}: vec2f;`)}
    ${lines(12, (k) => `var q${k} = vec2f(0.0);`)}
    if (interior) {
        ${runs}
    } else {
        ${mirrored}
        workgroupBarrier();
        let rb = rp * PITCH + rx;
        ${lines(16, (k) => `r${k} = S[rb + ${k}u];`)}
        if (apron) {
            let qb = qp * PITCH + qx;
            ${lines(12, (k) => `q${k} = S[qb + ${k}u];`)}
        }
    }
    ${lift('r', 16)}
    if (apron) {
        ${lift('q', 12)}
    }
    // An interior tile has read nothing from S.
    if (!interior) { workgroupBarrier(); }`
        : `
    // The interleaved signal and its apron, 20 pairs of rows of 40, mirrored
    // and scaled as IDWT_WGSL does. The mirror keeps parity (the tile starts
    // even), so a sample's band follows from its place in the tile.
    ${mirrored}
    workgroupBarrier();

    // Rows: pairs 0-15 by runs of 8 (all threads), pairs 16-19 by runs of 4
    // (half of them). A run starts 4 samples before its outputs.
    let rp = li >> 2u;
    let rx = 8u * (li & 3u);
    let rb = rp * PITCH + rx;
    ${lines(16, (k) => `var r${k} = S[rb + ${k}u];`)}
    ${lift('r', 16)}
    let apron = li < 32u;
    let qp = 16u + (li >> 3u);
    let qx = 4u * (li & 7u);
    ${lines(12, (k) => `var q${k} = vec2f(0.0);`)}
    if (apron) {
        let qb = qp * PITCH + qx;
        ${lines(12, (k) => `q${k} = S[qb + ${k}u];`)}
        ${lift('q', 12)}
    }
    workgroupBarrier();`;
    return /* wgsl */ `
struct Pass { w: u32, h: u32, comps: u32, y0: u32, bands: array<vec4u, 3>, outs: vec4u }
@group(0) @binding(0) var<uniform> p: Pass;
@group(0) @binding(1) var<storage, read_write> coef: array<u32>;

const ALPHA: f32 = -1.586134342059924;
const BETA: f32 = -0.052980118572961;
const GAMMA: f32 = 0.882911075530934;
const DELTA: f32 = 0.443506852043971;
const K: f32 = 1.230174104914001;
const INV_K: f32 = 1.0 / 1.230174104914001;
const SPAN: u32 = 40u;  // 32 + 2 x 4 of apron
const PITCH: u32 = 41u;  // a line of S, padded

var<workgroup> S: array<vec2f, 820>;  // 20 x PITCH

fn mirror(i: i32, n: i32) -> i32 {
    var j = i;
    if (j < 0) { j = -j; }
    if (j > n - 1) { j = 2 * (n - 1) - j; }
    return j;
}

// Sample (col, row) of the band at word offset b, p.w samples a row.
fn ld(b: u32, row: u32, col: u32) -> f32 {
${
    in16
        ? `    let two = unpack2x16float(coef[b + row * (p.w >> 1u) + (col >> 1u)]);
    return select(two.x, two.y, (col & 1u) == 1u);`
        : `    return bitcast<f32>(coef[b + row * p.w + col]);`
}
}

@compute @workgroup_size(64)
fn main(@builtin(workgroup_id) wg: vec3u, @builtin(local_invocation_index) li: u32) {
    let w2 = 2u * p.w;
    let h2 = 2u * p.h;
    let bands = p.bands[wg.z];
    let dst = p.outs[wg.z];
    let ox = i32(wg.x * 32u) - 4;
    let ty = wg.y + p.y0;
    let oy = i32(ty * 32u) - 4;
${rows}
    // Back transposed: entry c * PITCH + y pairs the tile's columns 2c and
    // 2c + 1 of row y (the rows with their apron, 0-39).
    ${lines(4, (j) => `S[(rx / 2u + ${j}u) * PITCH + 2u * rp] = vec2f(r${4 + 2 * j}.x, r${5 + 2 * j}.x);`)}
    ${lines(4, (j) => `S[(rx / 2u + ${j}u) * PITCH + 2u * rp + 1u] = vec2f(r${4 + 2 * j}.y, r${5 + 2 * j}.y);`)}
    if (apron) {
        ${lines(2, (j) => `S[(qx / 2u + ${j}u) * PITCH + 2u * qp] = vec2f(q${4 + 2 * j}.x, q${5 + 2 * j}.x);`)}
        ${lines(2, (j) => `S[(qx / 2u + ${j}u) * PITCH + 2u * qp + 1u] = vec2f(q${4 + 2 * j}.y, q${5 + 2 * j}.y);`)}
    }
    workgroupBarrier();

    // Columns: 16 pairs by runs of 8, written straight from the registers.
    let cp = li & 15u;
    let cy = 8u * (li >> 4u);
    let cb = cp * PITCH + cy;
    ${lines(16, (k) => `var v${k} = S[cb + ${k}u];`)}
    ${lift('v', 16)}
    let gx = wg.x * 32u + 2u * cp;
    let gy = ty * 32u + cy;
    if (gx < w2) {
        ${lines(8, (j) =>
            out16
                ? `if (gy + ${j}u < h2) { coef[dst + (gy + ${j}u) * p.w + (gx >> 1u)] = pack2x16float(v${4 + j}); }`
                : `if (gy + ${j}u < h2) { let o = dst + (gy + ${j}u) * w2 + gx; coef[o] = bitcast<u32>(v${4 + j}.x); coef[o + 1u] = bitcast<u32>(v${4 + j}.y); }`,
        )}
    }
}
`;
};

// The output planes (before the DC shift) to 8-bit 4:2:0, cropped: one thread
// per 4 bytes of a row, rows of Y, then Cb, then Cr (the width must be a
// multiple of 8, so a chroma row is whole words). fp16: the planes are FP16.
const packWgsl = (fp16) => /* wgsl */ `
struct Pack { yOff: u32, cbOff: u32, crOff: u32, alignedW: u32, width: u32, height: u32, pad0: u32, pad1: u32 }
@group(0) @binding(0) var<uniform> p: Pack;
@group(0) @binding(1) var<storage, read_write> planes: array<u32>;
@group(0) @binding(2) var<storage, read_write> outBytes: array<u32>;
fn q(v: f32) -> u32 { return u32(round(clamp(v + 0.5, 0.0, 1.0) * 255.0)); }
// Samples i .. i + 3 of the plane at word offset at (i a multiple of 4).
fn four(at: u32, i: u32) -> vec4f {
${
    fp16
        ? `    return vec4f(unpack2x16float(planes[at + (i >> 1u)]), unpack2x16float(planes[at + (i >> 1u) + 1u]));`
        : `    return bitcast<vec4f>(vec4u(planes[at + i], planes[at + i + 1u], planes[at + i + 2u], planes[at + i + 3u]));`
}
}
@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) id: vec3u) {
    let r = id.y;
    let ch = p.height / 2u;
    var at: u32;
    var src: u32;
    var dst: u32;
    var words: u32;
    if (r < p.height) {
        words = p.width / 4u;
        at = p.yOff;
        src = r * p.alignedW;
        dst = r * words;
    } else if (r < p.height + ch) {
        words = p.width / 8u;
        at = p.cbOff;
        src = (r - p.height) * (p.alignedW / 2u);
        dst = p.width * p.height / 4u + (r - p.height) * words;
    } else if (r < p.height + 2u * ch) {
        words = p.width / 8u;
        at = p.crOff;
        src = (r - p.height - ch) * (p.alignedW / 2u);
        dst = p.width * p.height / 4u + ch * words + (r - p.height - ch) * words;
    } else {
        return;
    }
    let x = id.x;
    if (x >= words) { return; }
    let s = four(at, src + 4u * x);
    outBytes[dst + x] = q(s.x) | (q(s.y) << 8u) | (q(s.z) << 16u) | (q(s.w) << 24u);
}
`;

// The decoded planes (before the DC shift) drawn as RGB: a full-screen
// triangle whose fragment reads Y at its pixel and Cb, Cr at half resolution
// (nearest), BT.709, limited or full range. fp16: the planes are FP16.
const presentWgsl = (fp16) => /* wgsl */ `
struct Present { yOff: u32, cbOff: u32, crOff: u32, alignedW: u32, width: u32, height: u32, limited: u32, pad: u32 }
@group(0) @binding(0) var<uniform> p: Present;
@group(0) @binding(1) var<storage, read> planes: array<u32>;

// Sample i of the plane at word offset at.
fn ld(at: u32, i: u32) -> f32 {
${
    fp16
        ? `    let two = unpack2x16float(planes[at + (i >> 1u)]);
    return select(two.x, two.y, (i & 1u) == 1u);`
        : `    return bitcast<f32>(planes[at + i]);`
}
}

@vertex
fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    let uv = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
    return vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
}

@fragment
fn fs(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    let x = min(u32(pos.x), p.width - 1u);
    let y = min(u32(pos.y), p.height - 1u);
    let c = (y >> 1u) * (p.alignedW >> 1u) + (x >> 1u);
    var yv = clamp(ld(p.yOff, y * p.alignedW + x) + 0.5, 0.0, 1.0);
    var cb = clamp(ld(p.cbOff, c) + 0.5, 0.0, 1.0) - 0.5;
    var cr = clamp(ld(p.crOff, c) + 0.5, 0.0, 1.0) - 0.5;
    if (p.limited != 0u) {
        yv = (yv * 255.0 - 16.0) / 219.0;
        cb = cb * 255.0 / 224.0;
        cr = cr * 255.0 / 224.0;
    }
    let r = yv + 1.5748 * cr;
    let g = yv - 0.1873 * cb - 0.4681 * cr;
    let b = yv + 1.8556 * cb;
    return vec4f(clamp(vec3f(r, g, b), vec3f(0.0), vec3f(1.0)), 1.0);
}
`;

export class PyroWaveDecoder extends PyroWaveFrame {
    /**
     * @param {GPUDevice} device
     * @param {number} width picture width (even)
     * @param {number} height picture height (even)
     * @param {{idwt?: number, fp16?: boolean}} [options] idwt: the inverse
     *        wavelet's shader, 3 (idwt2Wgsl with its fast load), 2 (without
     *        it) or 1 (IDWT_WGSL, f32 only), all giving the same values, 2 and
     *        1 more slowly; fp16: the two finest levels and the output planes
     *        stored in FP16 (shaders 2-3), else everything in f32
     */
    constructor(device, width, height, { idwt = 3, fp16 = true } = {}) {
        super(width, height);
        this.device = device;
        this.idwtVersion = idwt === 1 || idwt === 2 ? idwt : 3;
        this.fp16 = !!fp16 && this.idwtVersion !== 1;
        this._gpuLayout();
        this._resources();
    }

    // Where the planes sit in coefBuf, in 32-bit words, in the order of
    // PyroWaveFrame's float offsets: an f32 sample a word, or, for the levels
    // below FP16_LEVELS and the output planes when fp16, two FP16 samples of
    // a row a word (every row there has a multiple of 8 samples).
    _gpuLayout() {
        const W = this.alignedW;
        const H = this.alignedH;
        const half = (level) => this.fp16 && level < FP16_LEVELS;
        this.wordOf = {}; // "level,comp,band" -> word offset
        const metaPlane = new Map(); // a float offset of planeOf -> its plane in the metadata
        let words = 0;
        for (let level = 0; level < LEVELS; level++) {
            const samples = (W >> (level + 1)) * (H >> (level + 1));
            for (let comp = 0; comp < 3; comp++) {
                if (level === 0 && comp !== 0) continue;
                for (let band = 0; band < 4; band++) {
                    const key = `${level},${comp},${band}`;
                    this.wordOf[key] = words;
                    metaPlane.set(this.planeOf[key], words | (half(level) ? F16_PLANE : 0));
                    words += half(level) ? samples / 2 : samples;
                }
            }
        }
        const perWord = this.fp16 ? 2 : 1;
        this.yWord = words;
        words += (W * H) / perWord;
        this.cbWord = words;
        words += (W * H) / 4 / perWord;
        this.crWord = words;
        words += (W * H) / 4 / perWord;
        this.coefWords = words;
        this.gpuMetas = this.metas.slice();
        for (let i = 0; i < this.blockCount; i++)
            this.gpuMetas[4 * i] = metaPlane.get(this.metas[4 * i]);
    }

    _resources() {
        const d = this.device;
        const S = GPUBufferUsage.STORAGE;
        const C = GPUBufferUsage.COPY_DST;
        this.payloadBuf = d.createBuffer({ size: this.payloadCpu.byteLength + 16, usage: S | C });
        this.offsetsBuf = d.createBuffer({ size: this.offsetsCpu.byteLength, usage: S | C });
        this.metaBuf = d.createBuffer({ size: this.gpuMetas.byteLength, usage: S | C });
        d.queue.writeBuffer(this.metaBuf, 0, this.gpuMetas);
        this.orderBuf = d.createBuffer({ size: this.sendOrder.byteLength, usage: S | C });
        d.queue.writeBuffer(this.orderBuf, 0, this.sendOrder);
        this.rangeBuf = d.createBuffer({ size: 16, usage: GPUBufferUsage.UNIFORM | C });
        this._rangeFirst = 0;
        this.coefBuf = d.createBuffer({
            size: this.coefWords * 4,
            usage: S | GPUBufferUsage.COPY_SRC,
        });
        const packedBytes = (this.width * this.height * 3) / 2;
        this.packedBuf = d.createBuffer({
            size: alignUp(packedBytes, 4),
            usage: S | GPUBufferUsage.COPY_SRC,
        });

        const mod = (code) => d.createShaderModule({ code });
        this.dequantPipe = d.createComputePipeline({
            layout: 'auto',
            compute: { module: mod(DEQUANT_WGSL), entryPoint: 'main' },
        });
        // The levels' pipelines differ by the storage of their bands and
        // output, all on one layout so that one bind group per level fits any.
        const idwtLayout = d.createBindGroupLayout({
            entries: [
                { binding: 0, visibility: GPUShaderStage.COMPUTE, buffer: { type: 'uniform' } },
                { binding: 1, visibility: GPUShaderStage.COMPUTE, buffer: { type: 'storage' } },
            ],
        });
        const pipeLayout = d.createPipelineLayout({ bindGroupLayouts: [idwtLayout] });
        const idwtPipes = new Map();
        const idwtPipe = (in16, out16) => {
            const key = this.idwtVersion === 1 ? 'v1' : `${in16},${out16}`;
            if (!idwtPipes.has(key)) {
                const code =
                    this.idwtVersion === 1
                        ? IDWT_WGSL
                        : idwt2Wgsl(in16, out16, this.idwtVersion === 3);
                idwtPipes.set(
                    key,
                    d.createComputePipeline({
                        layout: pipeLayout,
                        compute: { module: mod(code), entryPoint: 'main' },
                    }),
                );
            }
            return idwtPipes.get(key);
        };
        this.packPipe = d.createComputePipeline({
            layout: 'auto',
            compute: { module: mod(packWgsl(this.fp16)), entryPoint: 'main' },
        });
        this._bindDequant();

        // One uniform slot per level, all fixed at creation: decoding a frame
        // only uploads the payload and the block offsets. A level's components
        // go in one dispatch (z): WebGPU puts a barrier between dispatches that
        // write the same buffer, and on the AMD iGPU thirteen of them cost
        // more than the transform itself (4 ms against 0.7 in Vulkan).
        const half = (level) => this.fp16 && level < FP16_LEVELS;
        this.passes = [];
        for (let level = LEVELS - 1; level >= 0; level--) {
            const w = this.alignedW >> (level + 1);
            const h = this.alignedH >> (level + 1);
            const comps = level === 0 ? 1 : 3;
            const bands = [];
            const outs = [];
            for (let comp = 0; comp < 3; comp++) {
                if (comp >= comps) {
                    bands.push(0, 0, 0, 0);
                    outs.push(0);
                    continue;
                }
                for (let b = 0; b < 4; b++) bands.push(this.wordOf[`${level},${comp},${b}`]);
                if (level === 0) outs.push(this.yWord);
                else if (level === 1 && comp !== 0)
                    outs.push(comp === 1 ? this.cbWord : this.crWord);
                else outs.push(this.wordOf[`${level - 1},${comp},0`]);
            }
            // The level's bands are whole once the blocks before the next
            // (finer) level's are: they come in that order.
            const end = level === 0 ? this.blockCount : this.firstBlockOf[`${level - 1},0,1`];
            this.passes.push({
                w,
                h,
                comps,
                end,
                tileRows: Math.ceil((2 * h) / 32),
                // Its output: a band of the finer level, or an output plane
                // (FP16 as the finest level's bands).
                pipe: idwtPipe(half(level), half(level - 1)),
                u: [w, h, comps, 0, ...bands, ...outs, 0],
            });
        }
        const slot = 256;
        this._slot = slot;
        this._fineY0 = 0;
        this.uniformBuf = d.createBuffer({
            size: slot * (this.passes.length + 1),
            usage: GPUBufferUsage.UNIFORM | C,
        });
        const u = new Uint32Array((slot / 4) * (this.passes.length + 1));
        this.passes.forEach((pass, k) => u.set(pass.u, (slot / 4) * k));
        const packSlot = this.passes.length;
        u.set(
            [this.yWord, this.cbWord, this.crWord, this.alignedW, this.width, this.height, 0, 0],
            (slot / 4) * packSlot,
        );
        d.queue.writeBuffer(this.uniformBuf, 0, u);
        const ubo = (k, size) => ({ buffer: this.uniformBuf, offset: slot * k, size });
        for (const [k, pass] of this.passes.entries()) {
            pass.bind = d.createBindGroup({
                layout: idwtLayout,
                entries: [
                    { binding: 0, resource: ubo(k, 80) },
                    { binding: 1, resource: { buffer: this.coefBuf } },
                ],
            });
        }
        this.packBind = d.createBindGroup({
            layout: this.packPipe.getBindGroupLayout(0),
            entries: [
                { binding: 0, resource: ubo(packSlot, 32) },
                { binding: 1, resource: { buffer: this.coefBuf } },
                { binding: 2, resource: { buffer: this.packedBuf } },
            ],
        });
    }

    _bindDequant() {
        this.dequantBind = this.device.createBindGroup({
            layout: this.dequantPipe.getBindGroupLayout(0),
            entries: [
                { binding: 0, resource: { buffer: this.payloadBuf } },
                { binding: 1, resource: { buffer: this.offsetsBuf } },
                { binding: 2, resource: { buffer: this.metaBuf } },
                { binding: 3, resource: { buffer: this.coefBuf } },
                { binding: 4, resource: { buffer: this.rangeBuf } },
                { binding: 5, resource: { buffer: this.orderBuf } },
            ],
        });
    }

    _payloadGrown(words) {
        this.payloadBuf.destroy();
        this.payloadBuf = this.device.createBuffer({
            size: words * 4 + 16,
            usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
        });
        this._bindDequant();
        // A frame by slices uploads its payload again from the start.
        this._sentWords = 0;
    }

    // The first block of the next dequantization dispatch, written only when it changes.
    _setRangeFirst(first) {
        if (first === this._rangeFirst) return;
        this.device.queue.writeBuffer(this.rangeBuf, 0, new Uint32Array([first, 0, 0, 0]));
        this._rangeFirst = first;
    }

    // The first row of tiles of the finest level's next inverse wavelet,
    // written only when it changes (the other levels keep 0).
    _setFineY0(y0) {
        if (y0 === this._fineY0) return;
        const k = this.passes.length - 1;
        this.device.queue.writeBuffer(this.uniformBuf, this._slot * k + 12, new Uint32Array([y0]));
        this._fineY0 = y0;
    }

    // Rows of tiles of the finest level computable from its first `settled`
    // rows of blocks: a 32-row tile reads 16 band rows, plus 2 of apron each
    // side, and the band's last row covers the mirrored bottom.
    _fineTileRows(settled) {
        if (settled >= this.fineRows) return this.passes[this.passes.length - 1].tileRows;
        let t = 0;
        while (Math.min((16 * t + 17) >> 5, this.fineRows - 1) < settled) t++;
        return t;
    }

    /** Starts a frame decoded by slices (after clear()): nothing of it is on the GPU yet. */
    startSlices() {
        this._sentWords = 0;
        this._dequantDone = 0;
        this._levelsDone = 0;
        this._tileRowsDone = 0;
    }

    /**
     * Records, for the frame coming by slices, what its blocks so far allow:
     * the dequantization of the blocks new since the last call, in send order
     * (every block before the frontier is settled, came or not), the inverse
     * wavelet of each coarse level now whole, and of the finest level the
     * stripe of tiles its settled rows of blocks allow. `last`: the frame is
     * all there, everything left is recorded. One call per command encoder
     * (the dispatch offsets are queue writes). False when there was nothing
     * new to record.
     */
    decodeSlice(encoder, last = false, timestampWrites) {
        const d = this.device;
        const end = last ? this.blockCount : this.frontier;
        const from = this._dequantDone;
        const fine = this.passes.length - 1;
        let levels = this._levelsDone;
        while (levels < fine && this.passes[levels].end <= end) levels++;
        let rows = this._tileRowsDone;
        if (levels === fine) {
            rows = last ? this.passes[fine].tileRows : this._fineTileRows(this.fineRowsSettled());
        }
        if (end <= from && levels === this._levelsDone && rows === this._tileRowsDone && !last)
            return false;
        if (this.payloadWords > this._sentWords) {
            d.queue.writeBuffer(
                this.payloadBuf,
                this._sentWords * 4,
                this.payloadCpu,
                this._sentWords,
                this.payloadWords - this._sentWords,
            );
            this._sentWords = this.payloadWords;
        }
        const pass = encoder.beginComputePass(timestampWrites ? { timestampWrites } : undefined);
        if (end > from) {
            // The offsets of the new blocks: one span of indices around them.
            let lo = this.blockCount;
            let hi = 0;
            for (let p = from; p < end; p++) {
                const i = this.sendOrder[p];
                if (i < lo) lo = i;
                if (i >= hi) hi = i + 1;
            }
            d.queue.writeBuffer(this.offsetsBuf, lo * 4, this.offsetsCpu, lo, hi - lo);
            this._setRangeFirst(from);
            pass.setPipeline(this.dequantPipe);
            pass.setBindGroup(0, this.dequantBind);
            pass.dispatchWorkgroups(end - from);
            this._dequantDone = end;
        }
        if (levels > this._levelsDone || rows > this._tileRowsDone) {
            for (let k = this._levelsDone; k < levels; k++) this._idwt(pass, this.passes[k]);
            this._levelsDone = levels;
            if (rows > this._tileRowsDone) {
                this._setFineY0(this._tileRowsDone);
                this._idwt(pass, this.passes[fine], rows - this._tileRowsDone);
                this._tileRowsDone = rows;
            }
        }
        pass.end();
        if (last) this.decodedThisSeq = true;
        return true;
    }

    _idwt(pass, p, rows = p.tileRows) {
        pass.setPipeline(p.pipe);
        pass.setBindGroup(0, p.bind);
        pass.dispatchWorkgroups(Math.ceil((2 * p.w) / 32), rows, p.comps);
    }

    /**
     * Records the decode of the frame in progress into `encoder`, ending with
     * the 8-bit planes in `packedBuf`. `timestampWrites` (optional) brackets the pass.
     * `stages` ('dequant', 'idwt', 'pack', a list of them, or all when absent):
     * the lab times one stage alone, the player skips the packing.
     */
    decode(encoder, timestampWrites, stages) {
        const d = this.device;
        const run = (s) => !stages || (Array.isArray(stages) ? stages.includes(s) : stages === s);
        d.queue.writeBuffer(this.payloadBuf, 0, this.payloadCpu, 0, alignUp(this.payloadWords, 1));
        d.queue.writeBuffer(this.offsetsBuf, 0, this.offsetsCpu);
        const pass = encoder.beginComputePass(timestampWrites ? { timestampWrites } : undefined);
        if (run('dequant')) {
            this._setRangeFirst(0);
            pass.setPipeline(this.dequantPipe);
            pass.setBindGroup(0, this.dequantBind);
            pass.dispatchWorkgroups(this.blockCount);
        }
        if (run('idwt')) {
            this._setFineY0(0);
            for (const p of this.passes) this._idwt(pass, p);
        }
        if (run('pack')) this._pack(pass);
        pass.end();
        this.decodedThisSeq = true;
    }

    _pack(pass) {
        pass.setPipeline(this.packPipe);
        pass.setBindGroup(0, this.packBind);
        // Rows: H of Y, then H/2 of Cb and H/2 of Cr.
        pass.dispatchWorkgroups(Math.ceil(this.width / 4 / 64), this.height * 2);
    }

    /** The stages of decodeSplit(), in their order. */
    splitStages() {
        return ['dequant', ...this.passes.map((p, k) => `idwt${LEVELS - 1 - k}`), 'pack'];
    }

    /**
     * The lab's decode(): the same work, each stage in a compute pass of its
     * own (splitStages()), bracketed by `stamp(stage)` when that returns
     * timestampWrites.
     */
    decodeSplit(encoder, stamp) {
        const d = this.device;
        d.queue.writeBuffer(this.payloadBuf, 0, this.payloadCpu, 0, alignUp(this.payloadWords, 1));
        d.queue.writeBuffer(this.offsetsBuf, 0, this.offsetsCpu);
        const begin = (stage) => {
            const tw = stamp(stage);
            return encoder.beginComputePass(tw ? { timestampWrites: tw } : undefined);
        };
        let pass = begin('dequant');
        this._setRangeFirst(0);
        pass.setPipeline(this.dequantPipe);
        pass.setBindGroup(0, this.dequantBind);
        pass.dispatchWorkgroups(this.blockCount);
        pass.end();
        this._setFineY0(0);
        this.passes.forEach((p, k) => {
            pass = begin(`idwt${LEVELS - 1 - k}`);
            this._idwt(pass, p);
            pass.end();
        });
        pass = begin('pack');
        this._pack(pass);
        pass.end();
        this.decodedThisSeq = true;
    }

    /**
     * Draws the last decoded frame into a WebGPU canvas context (configured
     * by the caller, any 8-bit RGBA format), recorded into `encoder` after
     * decode(). `limited`: the planes are BT.709 limited range (the host's
     * NV12), else full range. `timestampWrites` (optional) brackets the pass.
     */
    present(encoder, context, limited = true, timestampWrites) {
        const d = this.device;
        const format =
            context.getConfiguration?.()?.format || navigator.gpu.getPreferredCanvasFormat();
        if (!this._presentPipe || this._presentFormat !== format) {
            const module = d.createShaderModule({ code: presentWgsl(this.fp16) });
            this._presentPipe = d.createRenderPipeline({
                layout: 'auto',
                vertex: { module, entryPoint: 'vs' },
                fragment: { module, entryPoint: 'fs', targets: [{ format }] },
                primitive: { topology: 'triangle-list' },
            });
            this._presentFormat = format;
            this._presentUbo = d.createBuffer({
                size: 32,
                usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
            });
            this._presentBind = d.createBindGroup({
                layout: this._presentPipe.getBindGroupLayout(0),
                entries: [
                    { binding: 0, resource: { buffer: this._presentUbo } },
                    { binding: 1, resource: { buffer: this.coefBuf } },
                ],
            });
            this._presentLimited = undefined;
        }
        if (this._presentLimited !== limited) {
            d.queue.writeBuffer(
                this._presentUbo,
                0,
                new Uint32Array([
                    this.yWord,
                    this.cbWord,
                    this.crWord,
                    this.alignedW,
                    this.width,
                    this.height,
                    limited ? 1 : 0,
                    0,
                ]),
            );
            this._presentLimited = limited;
        }
        const pass = encoder.beginRenderPass({
            colorAttachments: [
                {
                    view: context.getCurrentTexture().createView(),
                    loadOp: 'clear',
                    storeOp: 'store',
                    clearValue: { r: 0, g: 0, b: 0, a: 1 },
                },
            ],
            ...(timestampWrites ? { timestampWrites } : {}),
        });
        pass.setPipeline(this._presentPipe);
        pass.setBindGroup(0, this._presentBind);
        pass.draw(3);
        pass.end();
    }

    /** The output planes (Y, Cb, Cr) in coefBuf, in bytes: for the lab's read-back. */
    planesRange() {
        return { offset: this.yWord * 4, size: (this.coefWords - this.yWord) * 4 };
    }

    /** A read-back of planesRange() as f32 samples, before the DC shift. */
    planesF32(buffer) {
        if (!this.fp16) return new Float32Array(buffer);
        const halves = new Uint16Array(buffer);
        const out = new Float32Array(halves.length);
        for (let i = 0; i < halves.length; i++) out[i] = halfToFloat(halves[i]);
        return out;
    }

    destroy() {
        for (const b of [
            this.payloadBuf,
            this.offsetsBuf,
            this.metaBuf,
            this.orderBuf,
            this.coefBuf,
            this.packedBuf,
            this.uniformBuf,
            this.rangeBuf,
            this._presentUbo,
        ].filter(Boolean)) {
            b.destroy();
        }
    }
}
