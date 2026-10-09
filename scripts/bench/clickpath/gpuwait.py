"""Where a PyroWave frame waits on the page's GPU (plan "attente", B0).

Reads <tag>.ultratrace.json (a pass with localStorage mw_ultra_trace=1, written
by content-age/pass.py) and, when it is there, <tag>.frames.csv for the draw.

Each frame: submitted (t1), work done (t2), VideoFrame made (t3), all on the
page's performance.now(); and the GPU's own timestamps, on its own clock, around
the decode and the present passes. The two clocks are put together from what
they cannot break: the GPU starts no earlier than the submit, and ends no later
than the work-done callback. Over a window of time, frames and references
together, that gives the offset a lower bound (the earliest start seen is the
submit itself) and an upper one (the latest end seen is the callback itself).
Both are printed: the truth lies between, and the empty references, which start
soon and end soon, are what pull the two together.

Submit → done then splits into: before the GPU starts, the GPU at work (the two
passes and the gap between them), and after it ends until the callback fires.
The empty references (no work, one empty pass) give Chrome's own round trip.

By slices (mw_ultra_slices=1), each piece's pass is timed too (parts: in,
submitted, GPU begin, GPU end): a piece starts no earlier than its own submit,
one more bound for the offset. What the last submit waits for then splits in
two: the GPU still at work on the pieces before it (their last end after the
submit), or idle, the time Chrome takes to bring the submit to the GPU.

Usage: gpuwait.py <tag> [--dir bench-out/content-age] [--window-ms 2000]
"""
import argparse
import csv
import json
import os
import statistics

QUANTUM_NS = 100_000  # Chrome rounds timestamp queries to 100 µs unless told not to


def q(xs, p):
    xs = sorted(x for x in xs if x is not None)
    return xs[min(len(xs) - 1, int(p * len(xs)))] if xs else float("nan")


def row(name, xs):
    xs = [x for x in xs if x is not None]
    if not xs:
        return "  %-38s n=0" % name
    return "  %-38s p50 %7.3f  p90 %7.3f  mean %7.3f  (n %d)" % (
        name, q(xs, .5), q(xs, .9), statistics.mean(xs), len(xs))


def drift(recs):
    """How fast the GPU's clock runs against the page's: the slope of the
    midpoint of the two bounds over time, by least squares. The 780M's ran
    0.2 % apart on 08/10/2026 (16 ms in 8 s), more than a window holds."""
    pts = [(r["t1"], ((r["t1"] - r["gpu"][0] / 1e6) + (r["t2"] - r["gpu"][-1] / 1e6)) / 2)
           for r in recs]
    if len(pts) < 2:
        return 0.0
    mx = statistics.mean(p[0] for p in pts)
    my = statistics.mean(p[1] for p in pts)
    den = sum((p[0] - mx) ** 2 for p in pts)
    return sum((p[0] - mx) * (p[1] - my) for p in pts) / den if den else 0.0


def offsets(recs, window_ms, slope=0.0):
    """Per record, the GPU -> page offset bounds (ms), from every record (frames
    and references alike: one pair of clocks) in its window of time, the
    clocks' drift taken out (the bounds are of offset - slope x t1)."""
    recs = sorted(recs, key=lambda r: r["t1"])
    out = {}
    i = 0
    while i < len(recs):
        j = i
        while j < len(recs) and recs[j]["t1"] - recs[i]["t1"] < window_ms:
            j += 1
        chunk = recs[i:j]
        lo = max(r["t1"] - r["gpu"][0] / 1e6 - slope * r["t1"] for r in chunk)
        # A piece by slices starts no earlier than its own submit.
        lo = max([lo] + [p[1] - p[2] / 1e6 - slope * r["t1"]
                         for r in chunk for p in stamped(r)])
        hi = min(r["t2"] - r["gpu"][-1] / 1e6 - slope * r["t1"] for r in chunk)
        for r in chunk:
            out[id(r)] = (lo, hi)
        i = j
    return out


def stamped(r):
    """A frame's pieces with their GPU times ([in, submitted, begin, end]), all
    or none: a frame whose read-back had no room for them has none."""
    ps = r.get("parts") or []
    return ps if ps and all(len(p) == 4 for p in ps) else []


def pieces(frames, off, slope):
    """By slices: the pieces on the GPU, and what the last submit waits for."""
    fs = [r for r in frames if r.get("sliced") and stamped(r) and len(r.get("gpu") or []) == 4]
    if not fs:
        return
    print("by slices, %d frames with their pieces timed, %s pieces per frame at the median"
          % (len(fs), q([len(stamped(r)) for r in fs], .5)))
    print(" page clock (ms):")
    print(row("piece in -> submitted", [p[1] - p[0] for r in fs for p in stamped(r)]))
    print(row("first piece in -> last submit", [r["t1"] - stamped(r)[0][0] for r in fs]))
    print(row("last piece submitted -> last submit", [r["t1"] - stamped(r)[-1][1] for r in fs]))
    print(" GPU clock (ms):")
    print(row("a piece's pass", [(p[3] - p[2]) / 1e6 for r in fs for p in stamped(r)]))
    print(row("the pieces' passes, summed per frame",
              [sum(p[3] - p[2] for p in stamped(r)) / 1e6 for r in fs]))
    print(row("first piece begins -> last piece ends",
              [(stamped(r)[-1][3] - stamped(r)[0][2]) / 1e6 for r in fs]))
    print(row("last piece ends -> last pass begins",
              [(r["gpu"][0] - stamped(r)[-1][3]) / 1e6 for r in fs]))
    for which, k in (("offset at its lower bound", 0), ("offset at its upper bound", 1)):
        def page(r, ns):
            return ns / 1e6 + off[id(r)][k] + slope * r["t1"]
        busy = [page(r, stamped(r)[-1][3]) - r["t1"] for r in fs]
        print(" " + which)
        print(row("piece: submit -> GPU starts",
                  [page(r, p[2]) - p[1] for r in fs for p in stamped(r)]))
        print(row("last piece: submit -> GPU starts",
                  [page(r, stamped(r)[-1][2]) - stamped(r)[-1][1] for r in fs]))
        print(row("last submit -> its pass begins", [page(r, r["gpu"][0]) - r["t1"] for r in fs]))
        print(row("  of it, the GPU still on the pieces", [max(0.0, b) for b in busy]))
        print(row("  of it, after the GPU is free",
                  [page(r, r["gpu"][0]) - max(r["t1"], page(r, stamped(r)[-1][3])) for r in fs]))
        print("  GPU still on the pieces at the last submit: %d of %d frames"
              % (sum(1 for b in busy if b > 0), len(fs)))


def split(recs, off, label, slope=0.0):
    windows = {off[id(r)] for r in recs}
    bad = sum(1 for lo, hi in windows if lo > hi)
    width = [hi - lo for lo, hi in windows if hi >= lo]
    print("%s: %d with GPU times, %d windows, %d inconsistent (drift or quantized), "
          "bounds %.3f ms apart at the median" % (
              label, len(recs), len(windows), bad, q(width, .5)))
    for which, k in (("offset at its lower bound (start = earliest)", 0),
                     ("offset at its upper bound (end = latest)", 1)):
        start = [r["gpu"][0] / 1e6 + off[id(r)][k] + slope * r["t1"] - r["t1"] for r in recs]
        tail = [r["t2"] - (r["gpu"][-1] / 1e6 + off[id(r)][k] + slope * r["t1"]) for r in recs]
        print(" " + which)
        print(row("submit -> GPU starts", start))
        print(row("GPU ends -> work done fires", tail))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tag")
    ap.add_argument("--dir", default=os.path.join("bench-out", "content-age"))
    ap.add_argument("--window-ms", type=float, default=2000,
                    help="time over which the two clocks are taken as one offset")
    a = ap.parse_args()
    base = os.path.join(a.dir, a.tag)
    trace = json.load(open(base + ".ultratrace.json"))
    frames = [r for r in trace if not r.get("ref") and r.get("t2") is not None]
    refs = [r for r in trace if r.get("ref") and r.get("t2") is not None]
    print("%s: %d frames, %d references" % (a.tag, len(frames), len(refs)))

    print("page clock (ms):")
    print(row("in -> decode starts (queue)", [r["t0"] - r["at"] for r in frames]))
    print(row("decode starts -> submitted", [r["t1"] - r["t0"] for r in frames]))
    print(row("submitted -> work done", [r["t2"] - r["t1"] for r in frames]))
    print(row("work done -> VideoFrame", [r["t3"] - r["t2"] for r in frames if r["t3"]]))
    print(row("  (by slices) submitted -> done",
              [r["t2"] - r["t1"] for r in frames if r.get("sliced")]))

    gpu = [r for r in frames if r.get("gpu") and len(r["gpu"]) == 4]
    rg = [r for r in refs if r.get("gpu") and len(r["gpu"]) == 2]
    slope = drift(gpu + rg)
    off = offsets(gpu + rg, a.window_ms, slope)
    if gpu or rg:
        print("GPU clock against the page's: %+.0f ppm (taken out)" % (slope * 1e6))
    if gpu:
        quantized = all(v % QUANTUM_NS == 0 for r in gpu for v in r["gpu"])
        if quantized:
            print("! every GPU timestamp is a multiple of 100 us: Chrome quantizes them; run the "
                  "bench Chrome with --enable-webgpu-developer-features for real ones")
        print("GPU clock (ms):")
        print(row("decode pass", [(r["gpu"][1] - r["gpu"][0]) / 1e6 for r in gpu]))
        print(row("gap decode -> present", [(r["gpu"][2] - r["gpu"][1]) / 1e6 for r in gpu]))
        print(row("present pass", [(r["gpu"][3] - r["gpu"][2]) / 1e6 for r in gpu]))
        print(row("first begin -> last end", [(r["gpu"][3] - r["gpu"][0]) / 1e6 for r in gpu]))
        split(gpu, off, "frames", slope)
        pieces(gpu, off, slope)

    if refs:
        print("empty references (ms):")
        print(row("submitted -> work done", [r["t2"] - r["t1"] for r in refs]))
        if rg:
            print(row("empty pass on the GPU", [(r["gpu"][1] - r["gpu"][0]) / 1e6 for r in rg]))
            split(rg, off, "references", slope)

    # The draw: the page's frame log, joined on the host's capture stamp.
    path = base + ".frames.csv"
    if os.path.exists(path):
        drawn = {}
        for r in csv.DictReader(open(path)):
            try:
                drawn[round(float(r["hostMs"]), 1)] = float(r["drawnMs"])
            except (TypeError, ValueError):
                pass
        joined = [(r, drawn.get(round(float(r["host"]), 1))) for r in frames if r["t3"]]
        joined = [(r, d) for r, d in joined if d is not None]
        print("draw, %d frames joined on the host stamp (ms):" % len(joined))
        print(row("VideoFrame -> drawn", [d - r["t3"] for r, d in joined]))
        print(row("in -> drawn", [d - r["at"] for r, d in joined]))


if __name__ == "__main__":
    main()
