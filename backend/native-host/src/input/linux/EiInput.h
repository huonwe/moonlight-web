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

#include "../../core/ClickTrace.h"
#include "../IInputSink.h"
#include "XkbTextMap.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

// Keyboard and mouse for an app in its own gamescope, through libei.
//
// ── Why not uinput ──────────────────────────────────────────────────────────
//
// gamescope without a screen opens no input device at all: a uinput keyboard
// would type on the desktop beside it, not in it. What it opens instead is an
// EIS socket — libei's server side, named in LIBEI_SOCKET — and this is its
// client, a "sender" (bench §8s.13: one device, "Gamescope Virtual Input",
// with relative and absolute pointer, keyboard, buttons and wheel). The
// desktop never sees these events, which is the point: someone can use it
// while the stream plays.
//
// ── Loaded, not linked ──────────────────────────────────────────────────────
//
// libei.so.1 is opened on first use, and its few calls are declared here
// rather than taken from libei.h: the release builds on Ubuntu 22.04, which
// has no libei at all. Without the library this sink fails to start, and the
// session says so — it is the gamescope route's only way in.
//
// ── Positions ───────────────────────────────────────────────────────────────
//
// gamescope's device has no region (0,0 and INT32_MAX wide), and reads an
// absolute position in the focused window's own pixels, keeping the pointer
// inside it (bench §8s.13). Big Picture and a game in full screen fill the
// output, so the client's position is scaled onto the output's size; a small
// window centred in it would see the pointer offset — not seen with Steam.
//
// ── Keys ────────────────────────────────────────────────────────────────────
//
// Evdev codes, the same table uinput takes (EvdevKeyMap.h): gamescope turns
// them into characters with ITS keymap, which is US whatever the desktop's —
// upstream hands its clients none of its own (Punktfunk carries a patch). So
// characters the client's layout makes are found on a US map.

namespace mw::native::input {

class EiInput final : public IInputSink
{
public:
    /// @p socketPath gamescope's EIS socket; @p width x @p height the size of
    /// its output, which absolute positions are scaled onto.
    EiInput(std::string socketPath, int width, int height);
    ~EiInput() override;

    EiInput(const EiInput&) = delete;
    EiInput& operator=(const EiInput&) = delete;

    /// Connect and wait for gamescope's device, 3 s at most.
    bool start(std::string& error) override;
    void stop() override;
    void inject(const InputEvent& event) override;
    /// The bench's click trace (clicktrace=1): each press timed around its
    /// libei button and frame. Set before start(); null, the product.
    void setClickTrace(ClickTrace* trace) { m_ClickTrace = trace; }

private:
    struct Api;
    struct Device;

    void run();
    /// Every event libei has queued. Under m_Mutex.
    void drain();
    /// The device for @p capability, resumed and emulating; nullptr when none.
    Device* deviceFor(uint32_t capability);
    void frame(Device* device);
    void key(uint16_t code, bool down);
    void text(const std::string& utf8);
    void character(const std::string& utf8, bool down);
    bool ensureTextMap();
    void releaseAll();

    std::string m_SocketPath;
    int m_Width = 0;
    int m_Height = 0;

    std::mutex m_Mutex;
    std::thread m_Thread;
    std::atomic<bool> m_Stopping{false};
    int m_WakeFd = -1;

    /// libei's context and devices, opaque here (no libei.h, see above).
    void* m_Ei = nullptr;
    std::unique_ptr<Device[]> m_Devices;
    int m_DeviceCount = 0;
    uint32_t m_Sequence = 0;
    bool m_Connected = false;
    bool m_Disconnected = false;

    std::set<uint16_t> m_HeldKeys;
    std::set<uint16_t> m_HeldButtons;

    XkbTextMap m_TextMap;
    bool m_TextMapTried = false;

    ClickTrace* m_ClickTrace = nullptr;
};

} // namespace mw::native::input
