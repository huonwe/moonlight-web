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

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

// mw-click-target: the "ideal game" of plan « attente » (A1), and the marker
// where the latency flag cannot go (Wayland). A window that, on a click,
// draws the latency flag — the same bands at the same place on its screen as
// backend/src/LatencyFlag.cpp, so the client's probe sees no difference — and
// presents at once, then says when it got the click, when it presented and
// when the OS says the picture reached the screen.
//
// One renderer per OS, each with the API a game there would use: D3D12 on
// Windows (ClickTargetWin.cpp), Metal on macOS (ClickTargetMac.mm). Linux
// (Vulkan) follows the same Options and the same log.
namespace clicktarget {

/// The flag, as fractions of the screen: LatencyFlag::kLeft..kBottom.
constexpr double kFlagLeft = 0.44;
constexpr double kFlagRight = 0.56;
constexpr double kFlagTop = 0.0;
constexpr double kFlagBottom = 0.05;
/// How long it stays up: LatencyFlag::kShowMs.
constexpr int kFlagMs = 100;

struct Options
{
    /// The screen, by its position in the OS's list (0 the first) or by its
    /// device name (\\.\DISPLAY5); empty: the primary.
    std::string display;
    /// Covering the whole screen (the OS may then let it skip composition),
    /// or a window over the top 90 % of it (always composed).
    bool fullscreen = true;
    /// macOS: full screen in a Space of its own, as a game's full-screen mode
    /// goes, rather than a borderless window over the desktop's Space.
    bool fullscreenSpace = false;
    /// Present synchronised to the screen's refresh (1), or at once (0).
    int syncInterval = 0;
    /// With syncInterval 0, let the picture tear rather than wait.
    bool tearing = true;
    /// Draw frames continuously, like a game, at most this many a second (0:
    /// as fast as the swap chain takes them); or only when the flag changes.
    bool continuous = true;
    int fps = 240;
    /// A click is drawn at once (the ideal game), or at the next frame due
    /// (a game that samples its input once per frame).
    bool reactAtOnce = true;
    /// Read the input once the swap chain lets the next frame go, as a game
    /// tuned for latency does; or before waiting for it, as a simple loop
    /// does (with --sync 1, a click then waits a frame before it is seen).
    bool inputAfterWait = true;
    /// The GPU, by a piece of its name; empty: the one driving the screen,
    /// else the first. "warp": the software rasterizer.
    std::string adapter;
    /// Seconds before it quits on its own (Escape quits sooner).
    int durationS = 600;
    /// Where the log goes; empty: stdout.
    std::string out;
};

/// A microsecond count on the OS's monotonic clock — on Windows the
/// QueryPerformanceCounter that every MoonlightWeb stamp uses, on macOS the
/// monotonic clock that ticks with mach time — so this log lines up with the
/// host's click trace.
inline int64_t steadyNowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// One JSON object per line.
class Log
{
public:
    bool open(const std::string& path)
    {
        if (path.empty()) {
            m_File = stdout;
            return true;
        }
        m_File = std::fopen(path.c_str(), "w");
        return m_File != nullptr;
    }
    ~Log()
    {
        if (m_File && m_File != stdout) std::fclose(m_File);
    }
    void line(const std::string& json)
    {
        if (!m_File) return;
        std::fputs(json.c_str(), m_File);
        std::fputc('\n', m_File);
        std::fflush(m_File);
    }

private:
    std::FILE* m_File = nullptr;
};

/// The platform's renderer: runs until Escape or the duration, logging each
/// click. Returns the process's exit code.
int run(const Options& options, Log& log);

} // namespace clicktarget
