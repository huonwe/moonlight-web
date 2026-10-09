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

#include "GlReadback.h"

#include "../../core/Log.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <unistd.h>

#include <cstring>

namespace mw::native::convert {

struct GlReadback::Impl
{
    int renderFd = -1;
    gbm_device* gbm = nullptr;
    gbm_bo* luma = nullptr;
    gbm_bo* chroma = nullptr;

    ~Impl()
    {
        if (luma) gbm_bo_destroy(luma);
        if (chroma) gbm_bo_destroy(chroma);
        if (gbm) gbm_device_destroy(gbm);
        if (renderFd >= 0) ::close(renderFd);
    }
};

GlReadback::GlReadback()
    : d(std::make_unique<Impl>())
{}

GlReadback::~GlReadback()
{
    stop();
}

void GlReadback::stop()
{
    // GL first: its images of the planes go before the planes do.
    m_Gl.stop();
    d = std::make_unique<Impl>();
}

bool GlReadback::takes(const capture::KmsFrame& frame)
{
    return frame.mapped == nullptr && frame.planeCount >= 1 && frame.fds[0] >= 0;
}

bool GlReadback::init(const std::string& renderNode, uint32_t sourceFourcc, int sourceWidth,
                      int sourceHeight, int outputWidth, int outputHeight, std::string& error)
{
    stop();
    if (renderNode.empty()) {
        error = "no render node";
        return false;
    }
    // Bilinear: the CPU route has no budget to time a resample against, and
    // OpenH264 is the cost that matters here.
    if (!m_Gl.init(renderNode, sourceFourcc, sourceWidth, sourceHeight, outputWidth, outputHeight,
                   ScaleFilter::Bilinear, error))
        return false;
    const int w = m_Gl.outputWidth();
    const int h = m_Gl.outputHeight();

    // The planes on a GBM device of our own over the same node: GL imports
    // them by fd, so whose device allocated them does not matter.
    d->renderFd = ::open(renderNode.c_str(), O_RDWR | O_CLOEXEC);
    if (d->renderFd < 0) {
        error = "cannot open the render node " + renderNode;
        return false;
    }
    d->gbm = gbm_create_device(d->renderFd);
    if (!d->gbm) {
        error = "GBM refused the render node";
        return false;
    }
    // LINEAR is the point: a layout the CPU knows. RENDERING: GL draws into it.
    const uint32_t use = GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR;
    d->luma = gbm_bo_create(d->gbm, static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                            GBM_FORMAT_R8, use);
    d->chroma = gbm_bo_create(d->gbm, static_cast<uint32_t>(w / 2), static_cast<uint32_t>(h / 2),
                              GBM_FORMAT_GR88, use);
    if (!d->luma || !d->chroma) {
        error = "GBM cannot allocate linear R8/GR88 planes on " + renderNode;
        return false;
    }

    Nv12Target target;
    target.width = w;
    target.height = h;
    target.modifier = DRM_FORMAT_MOD_LINEAR;
    target.fdY = gbm_bo_get_fd(d->luma);
    target.pitchY = gbm_bo_get_stride(d->luma);
    target.fdUV = gbm_bo_get_fd(d->chroma);
    target.pitchUV = gbm_bo_get_stride(d->chroma);
    // EGL keeps its own reference to the buffers it imports: the fds are ours
    // to close whatever bindTarget says.
    const bool bound = target.fdY >= 0 && target.fdUV >= 0 && m_Gl.bindTarget(target, error);
    if (target.fdY >= 0) ::close(target.fdY);
    if (target.fdUV >= 0) ::close(target.fdUV);
    if (!bound) {
        if (error.empty()) error = "GBM gave no fd for the planes";
        return false;
    }

    const size_t lumaBytes = static_cast<size_t>(w) * h;
    m_Planes.assign(lumaBytes * 3 / 2, 0);
    m_Picture.y = m_Planes.data();
    m_Picture.u = m_Picture.y + lumaBytes;
    m_Picture.v = m_Picture.u + lumaBytes / 4;
    m_Picture.strideY = w;
    m_Picture.strideU = m_Picture.strideV = w / 2;
    m_Picture.width = w;
    m_Picture.height = h;

    log::info("[native] colour conversion on the GPU, read back for the CPU encoder: " +
              std::to_string(w) + "x" + std::to_string(h) +
              " NV12 into linear planes, then I420 in system memory");
    return true;
}

bool GlReadback::convert(const capture::KmsFrame& frame, const capture::CursorState& cursor,
                         const CursorDraw& draw, std::string& error)
{
    if (!d->luma) {
        error = "GL read-back is not initialized";
        return false;
    }
    // GlConvert ends on a glFinish: the planes hold this frame when it returns.
    if (!m_Gl.convert(frame, cursor, draw, error)) return false;

    const int w = m_Picture.width;
    const int h = m_Picture.height;

    uint32_t stride = 0;
    void* mapData = nullptr;
    const auto* y = static_cast<const uint8_t*>(
        gbm_bo_map(d->luma, 0, 0, static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                   GBM_BO_TRANSFER_READ, &stride, &mapData));
    if (!y) {
        error = "the luma plane cannot be mapped for reading";
        return false;
    }
    auto* dstY = const_cast<uint8_t*>(m_Picture.y);
    for (int row = 0; row < h; ++row)
        std::memcpy(dstY + static_cast<size_t>(row) * w, y + static_cast<size_t>(row) * stride,
                    static_cast<size_t>(w));
    gbm_bo_unmap(d->luma, mapData);

    const int cw = w / 2;
    const int ch = h / 2;
    mapData = nullptr;
    const auto* uv = static_cast<const uint8_t*>(
        gbm_bo_map(d->chroma, 0, 0, static_cast<uint32_t>(cw), static_cast<uint32_t>(ch),
                   GBM_BO_TRANSFER_READ, &stride, &mapData));
    if (!uv) {
        error = "the chroma plane cannot be mapped for reading";
        return false;
    }
    // GR88 is R in the low byte: U then V, NV12's own order.
    auto* dstU = const_cast<uint8_t*>(m_Picture.u);
    auto* dstV = const_cast<uint8_t*>(m_Picture.v);
    for (int row = 0; row < ch; ++row) {
        const uint8_t* src = uv + static_cast<size_t>(row) * stride;
        uint8_t* u = dstU + static_cast<size_t>(row) * cw;
        uint8_t* v = dstV + static_cast<size_t>(row) * cw;
        for (int x = 0; x < cw; ++x) {
            u[x] = src[2 * x];
            v[x] = src[2 * x + 1];
        }
    }
    gbm_bo_unmap(d->chroma, mapData);
    return true;
}

} // namespace mw::native::convert
