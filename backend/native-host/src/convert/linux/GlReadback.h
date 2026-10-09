/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "../../capture/linux/KmsCapture.h"
#include "../../encode/OpenH264Encoder.h"
#include "../CursorDraw.h"
#include "GlConvert.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mw::native::convert {

/// A tiled scanout to I420 in system memory: GL converts, the CPU reads back.
/// For a machine that HAS a GPU but no encoder on it — the CPU pair's other
/// customer (issue #34).
///
/// ── Why it exists ───────────────────────────────────────────────────────────
///
/// CpuConvert reads linear buffers only, on the reasoning that a machine with
/// a render node encodes on its GPU. Not so: Fedora's own Mesa ships radeonsi
/// without H.264/HEVC encoding (patents; RPM Fusion's "freeworld" has it), and
/// a libva that cannot load the driver gives the same picture. The session then
/// falls back to OpenH264 and is handed amdgpu's tiled scanout, whose layout
/// the CPU cannot know and which refuses the mmap anyway (EPERM).
///
/// The GPU can read it, though: GlConvert imports exactly that modifier. So
/// GlConvert runs as it does in front of VA-API — scaling, colour, pointer —
/// but renders into two LINEAR buffers allocated here through GBM (R8 luma,
/// GR88 interleaved chroma, the layout VA-API's NV12 surface has). After its
/// glFinish the planes are mapped and copied into I420, chroma de-interleaved,
/// which is what OpenH264 eats.
///
/// ── What it costs ───────────────────────────────────────────────────────────
///
/// The GL pass as on the GPU route, then one read of 1.5 bytes a pixel out of
/// the GPU's memory (gbm_bo_map: the driver blits to a staging buffer the CPU
/// can read fast). Less CPU than CpuConvert, which reads 4 bytes a pixel and
/// does the colour maths itself.
class GlReadback
{
public:
    GlReadback();
    ~GlReadback();

    GlReadback(const GlReadback&) = delete;
    GlReadback& operator=(const GlReadback&) = delete;

    /// Bring up GlConvert on @p renderNode and the two linear planes it renders
    /// into. Output dimensions are rounded down to even. False, with @p error,
    /// when the GPU cannot: the caller is then left with CpuConvert alone.
    bool init(const std::string& renderNode, uint32_t sourceFourcc, int sourceWidth,
              int sourceHeight, int outputWidth, int outputHeight, std::string& error);

    /// Whether GL can take @p frame: a DMA-BUF, which is everything but the
    /// portal's shared memory.
    static bool takes(const capture::KmsFrame& frame);

    /// Convert @p frame on the GPU and read it back into picture().
    bool convert(const capture::KmsFrame& frame, const capture::CursorState& cursor,
                 const CursorDraw& draw, std::string& error);

    const encode::I420Picture& picture() const { return m_Picture; }
    int outputWidth() const { return m_Gl.outputWidth(); }
    int outputHeight() const { return m_Gl.outputHeight(); }
    bool highPriority() const { return m_Gl.highPriority(); }

    /// GlConvert's context follows the capture thread; given back before the
    /// thread ends, as on the GPU route.
    void detachThread() { m_Gl.detachThread(); }
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
    GlConvert m_Gl;
    std::vector<uint8_t> m_Planes;
    encode::I420Picture m_Picture;
};

} // namespace mw::native::convert
