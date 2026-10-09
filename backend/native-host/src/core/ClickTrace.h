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

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// The click's way through the host, for the bench (MW_NATIVE_TUNING
// clicktrace=1, plan "attente" A0).
//
// The click → flag on the client measured 13 to 15 ms between the host
// receiving the click and the present of the picture that shows the flag, at
// 120 Hz, and nothing said where. Two kinds of rows, all on the engine's
// steady clock, which every process of the machine shares, so they line up
// with the relay's stamps and the latency flag's log (QueryPerformanceCounter
// on Windows; on macOS the monotonic clock, which ticks with mach time):
//
//  - press: a mouse button press handed to the OS. `startUs` and `us` frame
//    the call (SendInput, CGEventPost), `queuedUs` is when it was queued for
//    the thread that follows the desktop (a SYSTEM worker), 0 when injected
//    directly.
//  - capture: one wake-up of the capture. `startUs` is when it began to wait,
//    `us` when it returned, `status` what it brought. For a frame: `presentUs`
//    as the session stamps it, `presentRawUs` the OS's own stamp before any
//    clamp (WGC's runs ahead), `mouseUs` the pointer's last update (DDA),
//    `accumulated` the presents folded into it (DDA; on macOS the frames
//    ScreenCaptureKit handed over since the last one taken, this one
//    included), `deliveredUs` when the OS handed it to the engine, ahead of
//    the capture's thread taking it (ScreenCaptureKit's callback). Then the
//    compositor's timing read right after: its last vblank, its refresh
//    period, its last composition and its frame count (DWM; on macOS the
//    captured display's CVDisplayLink, which knows no composition).
//
// A field the platform cannot tell is left empty. Only the first kMaxRows rows
// are kept: minutes of a bench pass, not a session left running.
namespace mw::native {

class ClickTrace
{
public:
    enum class Kind : uint8_t
    {
        Press,
        Capture,
    };

    struct Row
    {
        Kind kind = Kind::Capture;
        int64_t us = 0;
        int64_t startUs = 0;
        int64_t queuedUs = 0;
        /// Capture: "ok", "pointer", "timeout", "lost" or "failed".
        const char* status = "";
        int64_t presentUs = 0;
        int64_t presentRawUs = 0;
        int64_t mouseUs = 0;
        int accumulated = -1;
        int64_t vblankUs = 0;
        int64_t periodUs = 0;
        int64_t composeUs = 0;
        int64_t composedFrames = -1;
        int64_t deliveredUs = 0;
    };

    static constexpr size_t kMaxRows = size_t(1) << 18;

    void add(const Row& row)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Rows.size() >= kMaxRows) {
            ++m_Dropped;
            return;
        }
        if (m_Rows.empty()) m_Rows.reserve(8192);
        m_Rows.push_back(row);
    }

    void press(int64_t queuedUs, int64_t startUs, int64_t doneUs)
    {
        Row row;
        row.kind = Kind::Press;
        row.us = doneUs;
        row.startUs = startUs;
        row.queuedUs = queuedUs;
        add(row);
    }

    size_t rows() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Rows.size();
    }

    size_t dropped() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Dropped;
    }

    std::string csv() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        std::string out = "kind,us,startUs,queuedUs,status,presentUs,presentRawUs,mouseUs,"
                          "accumulated,vblankUs,periodUs,composeUs,composedFrames,deliveredUs\n";
        out.reserve(out.size() + m_Rows.size() * 96);
        const auto num = [&out](int64_t v, bool known) {
            if (known) out += std::to_string(v);
            out += ',';
        };
        for (const Row& r : m_Rows) {
            out += r.kind == Kind::Press ? "press," : "capture,";
            num(r.us, true);
            num(r.startUs, r.startUs != 0);
            num(r.queuedUs, r.queuedUs != 0);
            out += r.status;
            out += ',';
            num(r.presentUs, r.presentUs != 0);
            num(r.presentRawUs, r.presentRawUs != 0);
            num(r.mouseUs, r.mouseUs != 0);
            num(r.accumulated, r.accumulated >= 0);
            num(r.vblankUs, r.vblankUs != 0);
            num(r.periodUs, r.periodUs != 0);
            num(r.composeUs, r.composeUs != 0);
            num(r.composedFrames, r.composedFrames >= 0);
            if (r.deliveredUs != 0) out += std::to_string(r.deliveredUs);
            out += '\n';
        }
        return out;
    }

private:
    mutable std::mutex m_Mutex;
    std::vector<Row> m_Rows;
    size_t m_Dropped = 0;
};

} // namespace mw::native
