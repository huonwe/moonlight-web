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

#include "EiInput.h"

#include "../../core/Log.h"
#include "EvdevKeyMap.h"

#include <dlfcn.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <type_traits>
#include <vector>

namespace mw::native::input {
namespace {

int64_t steadyNowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// libei's values (libei.h, 1.x): the capabilities are bits, the events count
// from 1. Fixed ABI — the library has kept them since 1.0.
constexpr uint32_t kCapPointer = 1u << 0;
constexpr uint32_t kCapPointerAbsolute = 1u << 1;
constexpr uint32_t kCapKeyboard = 1u << 2;
constexpr uint32_t kCapScroll = 1u << 4;
constexpr uint32_t kCapButton = 1u << 5;

constexpr int kEventConnect = 1;
constexpr int kEventDisconnect = 2;
constexpr int kEventSeatAdded = 3;
constexpr int kEventDeviceAdded = 5;
constexpr int kEventDeviceRemoved = 6;
constexpr int kEventDevicePaused = 7;
constexpr int kEventDeviceResumed = 8;

// evdev's button codes, which libei takes as they are.
constexpr uint32_t kBtnLeft = 0x110;
constexpr uint32_t kBtnRight = 0x111;
constexpr uint32_t kBtnMiddle = 0x112;
constexpr uint32_t kBtnSide = 0x113;
constexpr uint32_t kBtnExtra = 0x114;

/// The browser's numbering (1 left, 2 middle, 3 right, 4/5 side), as uinput's
/// sink maps it.
uint32_t buttonCode(int button)
{
    switch (button) {
    case 1: return kBtnLeft;
    case 2: return kBtnMiddle;
    case 3: return kBtnRight;
    case 4: return kBtnSide;
    case 5: return kBtnExtra;
    default: return 0;
    }
}

// evdev's modifier keys, for a character typed with them held.
constexpr uint16_t kKeyLeftShift = 42;
constexpr uint16_t kKeyRightShift = 54;
constexpr uint16_t kKeyRightAlt = 100;

} // namespace

/// The calls this sink makes, resolved once. Every handle is opaque.
struct EiInput::Api
{
    void* (*newSender)(void*) = nullptr;
    void (*configureName)(void*, const char*) = nullptr;
    int (*setupBackendSocket)(void*, const char*) = nullptr;
    int (*getFd)(void*) = nullptr;
    void (*dispatch)(void*) = nullptr;
    void* (*getEvent)(void*) = nullptr;
    int (*eventGetType)(void*) = nullptr;
    void* (*eventGetSeat)(void*) = nullptr;
    void* (*eventGetDevice)(void*) = nullptr;
    void* (*eventUnref)(void*) = nullptr;
    void (*seatBindCapabilities)(void*, ...) = nullptr;
    void* (*deviceRef)(void*) = nullptr;
    void* (*deviceUnref)(void*) = nullptr;
    bool (*deviceHasCapability)(void*, int) = nullptr;
    const char* (*deviceGetName)(void*) = nullptr;
    void (*startEmulating)(void*, uint32_t) = nullptr;
    void (*stopEmulating)(void*) = nullptr;
    void (*frame)(void*, uint64_t) = nullptr;
    uint64_t (*now)(void*) = nullptr;
    void (*pointerMotion)(void*, double, double) = nullptr;
    void (*pointerMotionAbsolute)(void*, double, double) = nullptr;
    void (*button)(void*, uint32_t, bool) = nullptr;
    void (*scrollDiscrete)(void*, int32_t, int32_t) = nullptr;
    void (*keyboardKey)(void*, uint32_t, bool) = nullptr;
    void* (*unref)(void*) = nullptr;

    bool complete() const
    {
        return newSender && configureName && setupBackendSocket && getFd && dispatch && getEvent &&
               eventGetType && eventGetSeat && eventGetDevice && eventUnref &&
               seatBindCapabilities && deviceRef && deviceUnref && deviceHasCapability &&
               startEmulating && stopEmulating && frame && now && pointerMotion &&
               pointerMotionAbsolute && button && scrollDiscrete && keyboardKey && unref;
    }

    /// libei.so.1, once per process. nullptr, said once, when it is missing.
    static const Api* get()
    {
        static Api api;
        static const bool loaded = [] {
            void* lib = ::dlopen("libei.so.1", RTLD_NOW | RTLD_LOCAL);
            if (!lib) {
                const char* why = ::dlerror();
                log::warning(std::string("[native] input: libei.so.1 is not installed — an app in "
                                         "gamescope cannot be driven") +
                             (why ? std::string(" (") + why + ")" : std::string()));
                return false;
            }
            const auto load = [lib](auto& fn, const char* name) {
                fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(::dlsym(lib, name));
            };
            load(api.newSender, "ei_new_sender");
            load(api.configureName, "ei_configure_name");
            load(api.setupBackendSocket, "ei_setup_backend_socket");
            load(api.getFd, "ei_get_fd");
            load(api.dispatch, "ei_dispatch");
            load(api.getEvent, "ei_get_event");
            load(api.eventGetType, "ei_event_get_type");
            load(api.eventGetSeat, "ei_event_get_seat");
            load(api.eventGetDevice, "ei_event_get_device");
            load(api.eventUnref, "ei_event_unref");
            load(api.seatBindCapabilities, "ei_seat_bind_capabilities");
            load(api.deviceRef, "ei_device_ref");
            load(api.deviceUnref, "ei_device_unref");
            load(api.deviceHasCapability, "ei_device_has_capability");
            load(api.deviceGetName, "ei_device_get_name");
            load(api.startEmulating, "ei_device_start_emulating");
            load(api.stopEmulating, "ei_device_stop_emulating");
            load(api.frame, "ei_device_frame");
            load(api.now, "ei_now");
            load(api.pointerMotion, "ei_device_pointer_motion");
            load(api.pointerMotionAbsolute, "ei_device_pointer_motion_absolute");
            load(api.button, "ei_device_button_button");
            load(api.scrollDiscrete, "ei_device_scroll_discrete");
            load(api.keyboardKey, "ei_device_keyboard_key");
            load(api.unref, "ei_unref");
            if (!api.complete()) {
                log::warning("[native] input: libei.so.1 lacks calls this host needs — an app in "
                             "gamescope cannot be driven");
                return false;
            }
            return true;
        }();
        return loaded ? &api : nullptr;
    }
};

/// One of gamescope's devices.
struct EiInput::Device
{
    void* handle = nullptr;
    uint32_t capabilities = 0;
    bool resumed = false;
    bool emulating = false;
};

namespace {
constexpr int kMaxDevices = 8;
} // namespace

EiInput::EiInput(std::string socketPath, int width, int height)
    : m_SocketPath(std::move(socketPath))
    , m_Width(width)
    , m_Height(height)
    , m_Devices(new Device[kMaxDevices])
{}

EiInput::~EiInput()
{
    stop();
}

bool EiInput::start(std::string& error)
{
    const Api* api = Api::get();
    if (!api) {
        error = "libei.so.1 is not installed";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Ei = api->newSender(nullptr);
        if (!m_Ei) {
            error = "libei could not make a sender";
            return false;
        }
        api->configureName(m_Ei, "MoonlightWeb");
        const int rc = api->setupBackendSocket(m_Ei, m_SocketPath.c_str());
        if (rc != 0) {
            error = "cannot reach gamescope's input socket " + m_SocketPath + " (" +
                    std::strerror(rc < 0 ? -rc : rc) + ")";
            api->unref(m_Ei);
            m_Ei = nullptr;
            return false;
        }
    }
    m_WakeFd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    m_Stopping.store(false);
    m_Thread = std::thread([this] { run(); });

    // The device comes a few round trips later: seat, bind, added, resumed.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (deviceFor(kCapKeyboard) || deviceFor(kCapPointer)) break;
            if (m_Disconnected) {
                error = "gamescope closed its input socket";
                break;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            log::warning("[native] input: gamescope has not offered its device yet — input "
                         "follows when it does");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!error.empty()) {
        stop();
        return false;
    }
    log::info("[native] input: keyboard and mouse go to gamescope (libei, " + m_SocketPath + ")");
    return true;
}

void EiInput::run()
{
    const Api* api = Api::get();
    int eiFd = -1;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Ei) eiFd = api->getFd(m_Ei);
    }
    while (!m_Stopping.load()) {
        pollfd fds[2] = {{eiFd, POLLIN, 0}, {m_WakeFd, POLLIN, 0}};
        const int ready = ::poll(fds, 2, 250);
        if (m_Stopping.load()) break;
        if (ready < 0) continue;
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Ei) break;
        api->dispatch(m_Ei);
        drain();
        if (m_Disconnected) break;
    }
}

void EiInput::drain()
{
    const Api* api = Api::get();
    while (void* event = api->getEvent(m_Ei)) {
        const int type = api->eventGetType(event);
        switch (type) {
        case kEventConnect: m_Connected = true; break;
        case kEventDisconnect:
            m_Disconnected = true;
            log::info("[native] input: gamescope closed its input socket");
            break;
        case kEventSeatAdded:
            // Everything a stream can send; gamescope's one device has it all.
            api->seatBindCapabilities(api->eventGetSeat(event), static_cast<int>(kCapPointer),
                                      static_cast<int>(kCapPointerAbsolute),
                                      static_cast<int>(kCapKeyboard), static_cast<int>(kCapScroll),
                                      static_cast<int>(kCapButton), 0);
            break;
        case kEventDeviceAdded: {
            void* handle = api->eventGetDevice(event);
            if (m_DeviceCount >= kMaxDevices || !handle) break;
            Device& d = m_Devices[m_DeviceCount++];
            d = Device{};
            d.handle = api->deviceRef(handle);
            for (uint32_t cap :
                 {kCapPointer, kCapPointerAbsolute, kCapKeyboard, kCapScroll, kCapButton})
                if (api->deviceHasCapability(d.handle, static_cast<int>(cap)))
                    d.capabilities |= cap;
            const char* name = api->deviceGetName(d.handle);
            log::info(std::string("[native] input: gamescope offers \"") + (name ? name : "?") +
                      "\" (capabilities " + std::to_string(d.capabilities) + ")");
            break;
        }
        case kEventDeviceRemoved:
        case kEventDevicePaused:
        case kEventDeviceResumed: {
            void* handle = api->eventGetDevice(event);
            for (int i = 0; i < m_DeviceCount; ++i) {
                Device& d = m_Devices[i];
                if (d.handle != handle) continue;
                if (type == kEventDeviceResumed) {
                    d.resumed = true;
                    // Emulating from now until paused: a sequence per span.
                    api->startEmulating(d.handle, ++m_Sequence);
                    d.emulating = true;
                } else {
                    // Paused or gone: anything sent now would be thrown away,
                    // and the keys held on it are released by gamescope.
                    d.resumed = false;
                    d.emulating = false;
                }
                if (type == kEventDeviceRemoved) {
                    api->deviceUnref(d.handle);
                    m_Devices[i] = m_Devices[m_DeviceCount - 1];
                    --m_DeviceCount;
                }
                break;
            }
            break;
        }
        default: break;
        }
        api->eventUnref(event);
    }
}

EiInput::Device* EiInput::deviceFor(uint32_t capability)
{
    for (int i = 0; i < m_DeviceCount; ++i) {
        Device& d = m_Devices[i];
        if (d.resumed && d.emulating && (d.capabilities & capability)) return &d;
    }
    return nullptr;
}

void EiInput::frame(Device* device)
{
    const Api* api = Api::get();
    api->frame(device->handle, api->now(m_Ei));
}

void EiInput::key(uint16_t code, bool down)
{
    if (code == 0) return;
    Device* d = deviceFor(kCapKeyboard);
    if (!d) return;
    Api::get()->keyboardKey(d->handle, code, down);
    frame(d);
    if (down)
        m_HeldKeys.insert(code);
    else
        m_HeldKeys.erase(code);
}

bool EiInput::ensureTextMap()
{
    if (!m_TextMapTried) {
        m_TextMapTried = true;
        if (m_TextMap.open("us"))
            log::info("[native] input: text typed into gamescope with its US keymap, " +
                      std::to_string(m_TextMap.size()) + " characters reachable");
    }
    return m_TextMap.isOpen();
}

void EiInput::character(const std::string& utf8, bool down)
{
    if (utf8.empty() || !ensureTextMap()) return;
    std::vector<char32_t> points;
    decodeUtf8(utf8, points);
    if (points.size() != 1) return;
    XkbStroke strokes[2];
    int count = 0;
    // A character the US map has no single key for — an accented letter — is
    // dropped: gamescope's keymap could not type it either.
    if (!m_TextMap.find(points[0], strokes, count) || count != 1) return;
    const XkbStroke& stroke = strokes[0];
    // The level modifiers the US map wants, around the key; the viewer's own
    // Shift and AltGr lifted when the map wants none (as uinput's sink does).
    const auto wants = [&stroke](uint16_t mod) {
        return stroke.mods[0] == mod || stroke.mods[1] == mod;
    };
    std::vector<uint16_t> lift;
    if (!wants(kKeyLeftShift)) {
        if (m_HeldKeys.count(kKeyLeftShift)) lift.push_back(kKeyLeftShift);
        if (m_HeldKeys.count(kKeyRightShift)) lift.push_back(kKeyRightShift);
    }
    if (!wants(kKeyRightAlt) && m_HeldKeys.count(kKeyRightAlt)) lift.push_back(kKeyRightAlt);
    std::vector<uint16_t> mine;
    for (uint16_t mod : stroke.mods)
        if (mod != 0 && !m_HeldKeys.count(mod)) mine.push_back(mod);

    Device* d = deviceFor(kCapKeyboard);
    if (!d) return;
    const Api* api = Api::get();
    if (down) {
        for (uint16_t mod : lift)
            api->keyboardKey(d->handle, mod, false);
        for (uint16_t mod : mine)
            api->keyboardKey(d->handle, mod, true);
        api->keyboardKey(d->handle, stroke.code, true);
        frame(d);
    } else {
        api->keyboardKey(d->handle, stroke.code, false);
        for (auto it = mine.rbegin(); it != mine.rend(); ++it)
            api->keyboardKey(d->handle, *it, false);
        for (auto it = lift.rbegin(); it != lift.rend(); ++it)
            api->keyboardKey(d->handle, *it, true);
        frame(d);
    }
}

void EiInput::text(const std::string& utf8)
{
    if (utf8.empty() || !ensureTextMap()) return;
    std::vector<char32_t> points;
    decodeUtf8(utf8, points);
    Device* d = deviceFor(kCapKeyboard);
    if (!d) return;
    const Api* api = Api::get();
    for (char32_t cp : points) {
        XkbStroke strokes[2];
        int count = 0;
        if (!m_TextMap.find(cp, strokes, count)) continue;
        for (int s = 0; s < count; ++s) {
            const XkbStroke& stroke = strokes[s];
            std::vector<uint16_t> pressed;
            for (uint16_t mod : stroke.mods) {
                if (mod == 0 || m_HeldKeys.count(mod)) continue;
                api->keyboardKey(d->handle, mod, true);
                pressed.push_back(mod);
            }
            api->keyboardKey(d->handle, stroke.code, true);
            frame(d);
            api->keyboardKey(d->handle, stroke.code, false);
            for (auto it = pressed.rbegin(); it != pressed.rend(); ++it)
                api->keyboardKey(d->handle, *it, false);
            frame(d);
        }
    }
}

void EiInput::inject(const InputEvent& event)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Ei) return;
    const Api* api = Api::get();

    using Type = InputEvent::Type;
    switch (event.type) {
    case Type::KeyDown: key(evdevKeyCode(event.keyCode), true); break;
    case Type::KeyUp: key(evdevKeyCode(event.keyCode), false); break;
    case Type::CharDown: character(event.text, true); break;
    case Type::CharUp: character(event.text, false); break;
    case Type::Utf8Text: text(event.text); break;

    case Type::MouseButtonDown:
    case Type::MouseButtonUp: {
        const uint32_t code = buttonCode(event.button);
        Device* d = code ? deviceFor(kCapButton) : nullptr;
        if (!d) break;
        const bool down = event.type == Type::MouseButtonDown;
        const int64_t startUs = m_ClickTrace && down ? steadyNowUs() : 0;
        api->button(d->handle, code, down);
        frame(d);
        if (startUs) m_ClickTrace->press(0, startUs, steadyNowUs());
        if (down)
            m_HeldButtons.insert(static_cast<uint16_t>(code));
        else
            m_HeldButtons.erase(static_cast<uint16_t>(code));
        break;
    }

    case Type::MouseMoveRelative: {
        if (event.deltaX == 0 && event.deltaY == 0) break;
        Device* d = deviceFor(kCapPointer);
        if (!d) break;
        api->pointerMotion(d->handle, event.deltaX, event.deltaY);
        frame(d);
        break;
    }

    case Type::MouseMoveAbsolute: {
        Device* d = deviceFor(kCapPointerAbsolute);
        const int refW = event.referenceWidth > 0 ? event.referenceWidth : m_Width;
        const int refH = event.referenceHeight > 0 ? event.referenceHeight : m_Height;
        if (!d || refW <= 0 || refH <= 0 || m_Width <= 0 || m_Height <= 0) break;
        double x = static_cast<double>(event.positionX) * m_Width / refW;
        double y = static_cast<double>(event.positionY) * m_Height / refH;
        x = x < 0 ? 0 : (x > m_Width - 1 ? m_Width - 1 : x);
        y = y < 0 ? 0 : (y > m_Height - 1 ? m_Height - 1 : y);
        api->pointerMotionAbsolute(d->handle, x, y);
        frame(d);
        break;
    }

    // The wire's 120 units a notch are libei's too. Its vertical axis points
    // down, the wire's (Windows') up.
    case Type::MouseScrollVertical:
    case Type::MouseScrollHorizontal: {
        if (event.scrollAmount == 0) break;
        Device* d = deviceFor(kCapScroll);
        if (!d) break;
        if (event.type == Type::MouseScrollVertical)
            api->scrollDiscrete(d->handle, 0, -event.scrollAmount);
        else
            api->scrollDiscrete(d->handle, event.scrollAmount, 0);
        frame(d);
        break;
    }

    // The gamepad has its own device (UinputGamepad), which games read
    // directly; the lock keys and Ctrl+Alt+Suppr are the desktop's notions.
    case Type::LockKeySync:
    case Type::SecureAttention:
    case Type::ControllerArrival:
    case Type::ControllerState:
    case Type::ControllerRemoval: break;
    }
}

void EiInput::releaseAll()
{
    const Api* api = Api::get();
    if (Device* d = deviceFor(kCapKeyboard)) {
        for (uint16_t code : m_HeldKeys)
            api->keyboardKey(d->handle, code, false);
        if (!m_HeldKeys.empty()) frame(d);
    }
    if (Device* d = deviceFor(kCapButton)) {
        for (uint16_t code : m_HeldButtons)
            api->button(d->handle, code, false);
        if (!m_HeldButtons.empty()) frame(d);
    }
    m_HeldKeys.clear();
    m_HeldButtons.clear();
}

void EiInput::stop()
{
    m_Stopping.store(true);
    if (m_WakeFd >= 0) {
        const uint64_t one = 1;
        const ssize_t written = ::write(m_WakeFd, &one, sizeof(one));
        (void)written;
    }
    if (m_Thread.joinable()) m_Thread.join();
    if (m_WakeFd >= 0) {
        ::close(m_WakeFd);
        m_WakeFd = -1;
    }
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Ei) return;
    const Api* api = Api::get();
    // Lift what is held before the devices go: a key left down in gamescope
    // would repeat in the game for whoever streams next.
    releaseAll();
    for (int i = 0; i < m_DeviceCount; ++i) {
        Device& d = m_Devices[i];
        if (d.emulating) api->stopEmulating(d.handle);
        api->deviceUnref(d.handle);
    }
    m_DeviceCount = 0;
    api->dispatch(m_Ei);
    api->unref(m_Ei);
    m_Ei = nullptr;
}

} // namespace mw::native::input
