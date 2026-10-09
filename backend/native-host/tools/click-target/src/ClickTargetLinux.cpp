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

// Linux: Vulkan on Wayland — what a game on a Wayland desktop presents with
// (SDL and Proton's Vulkan go the same way). Nothing is drawn but cleared
// rectangles: a dynamic-rendering pass that clears the background, then
// vkCmdClearAttachments for a bar that moves (every frame a new picture) and
// the flag's three bands. No shader, no pipeline.
//
// The click is the surface's own wl_pointer.button: `downUs` when it was
// dispatched, `eventUs` the compositor's stamp of it (whole milliseconds of
// the monotonic clock). When the picture reached the screen comes from
// wp_presentation: a feedback asked for before each present attaches to the
// commit Vulkan's WSI makes inside vkQueuePresentKHR. Its clock is the
// monotonic one (the clock_id event says so; another is converted), the
// steady clock MoonlightWeb stamps with, so the log lines up with the host's
// click trace. Its flags say whether the compositor sent our buffer to the
// screen as it is (zero-copy, direct scanout); a virtual monitor of Mutter's
// has no scanout at all.
//
// Full screen is xdg_toplevel.set_fullscreen on the chosen output; --window a
// maximized window, always composed (the shell's panels stay around it).
// Wayland lets a client neither place its window nor move the pointer: the
// pointer is brought onto the window by a uinput absolute pointer of the
// tool's own (the right the host's own input has, the udev rule's uaccess),
// and a maximized window opens on the output the pointer is on.

#include "ClickTarget.h"

#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include <wayland-client.h>

#include "presentation-time-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace clicktarget {
namespace {

std::atomic<bool> g_Signalled{false};

void onSignal(int)
{
    g_Signalled = true;
}

std::string jsonText(const std::string& s)
{
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) < 0x20) continue;
        out += c;
    }
    return out;
}

std::string lower(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

/// The click that has not yet been seen on the screen.
struct PendingClick
{
    int id = 0;
    int64_t downUs = 0;
    int64_t eventUs = 0;
    int64_t renderUs = 0;
    int64_t presentCallUs = 0;
    int64_t presentUs = 0;
    uint64_t frame = 0;
    bool presented = false;
};

/// A rectangle of the swapchain's images, in pixels.
struct PixelRect
{
    int x = 0, y = 0, w = 0, h = 0;
};

/// One wl_output, as the compositor describes it.
struct Output
{
    wl_output* handle = nullptr;
    uint32_t global = 0;
    std::string name;
    std::string description;
    std::string make;
    std::string model;
    /// Its place in the desktop's layout (logical pixels).
    int x = 0, y = 0;
    /// Its current mode, in pixels, and the refresh in mHz.
    int width = 0, height = 0;
    int refreshMilliHz = 0;
    int scale = 1;
};

/// A pointer of the tool's own, absolute over the whole desktop: the probe
/// clicks wherever the host's pointer is, so the tool brings it onto its
/// window as the Windows and macOS ones warp it. The compositor maps an
/// absolute pointer over its whole stage, as it does the host's own
/// (UinputInput, "MoonlightWeb Pointer").
class UinputPointer
{
public:
    static constexpr int kAbsMax = 65535;

    ~UinputPointer() { close(); }

    bool open(std::string& error)
    {
        m_Fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (m_Fd < 0) {
            error = std::string("/dev/uinput: ") + std::strerror(errno);
            return false;
        }
        ::ioctl(m_Fd, UI_SET_EVBIT, EV_ABS);
        ::ioctl(m_Fd, UI_SET_ABSBIT, ABS_X);
        ::ioctl(m_Fd, UI_SET_ABSBIT, ABS_Y);
        // A button, or the compositor does not take it for a pointer at all.
        ::ioctl(m_Fd, UI_SET_EVBIT, EV_KEY);
        ::ioctl(m_Fd, UI_SET_KEYBIT, BTN_LEFT);
        uinput_abs_setup abs = {};
        abs.absinfo.minimum = 0;
        abs.absinfo.maximum = kAbsMax;
        abs.code = ABS_X;
        ::ioctl(m_Fd, UI_ABS_SETUP, &abs);
        abs.code = ABS_Y;
        ::ioctl(m_Fd, UI_ABS_SETUP, &abs);
        uinput_setup setup = {};
        setup.id.bustype = BUS_VIRTUAL;
        setup.id.vendor = 0x1D6B;  // Linux Foundation: not a real device
        setup.id.product = 0x0105; // distinct from the host's
        setup.id.version = 1;
        std::snprintf(setup.name, sizeof(setup.name), "mw-click-target pointer");
        if (::ioctl(m_Fd, UI_DEV_SETUP, &setup) < 0 || ::ioctl(m_Fd, UI_DEV_CREATE) < 0) {
            error = std::string("uinput refused the pointer: ") + std::strerror(errno);
            ::close(m_Fd);
            m_Fd = -1;
            return false;
        }
        return true;
    }

    void close()
    {
        if (m_Fd < 0) return;
        ::ioctl(m_Fd, UI_DEV_DESTROY);
        ::close(m_Fd);
        m_Fd = -1;
    }

    bool isOpen() const { return m_Fd >= 0; }

    /// To (@p x, @p y) of the desktop whose bounding box is @p desk.
    void moveTo(int x, int y, const PixelRect& desk)
    {
        if (m_Fd < 0 || desk.w <= 0 || desk.h <= 0) return;
        // libinput scales an axis as (value - min) * size / (max - min + 1).
        const auto axis = [](int at, int origin, int size) {
            const int64_t v = (static_cast<int64_t>(at - origin) * (kAbsMax + 1) + size / 2) / size;
            return static_cast<int>(std::clamp<int64_t>(v, 0, kAbsMax));
        };
        const int ax = axis(x, desk.x, desk.w);
        const int ay = axis(y, desk.y, desk.h);
        // The kernel drops an absolute value equal to the axis's last one: the
        // pointer would stay where something else took it. A step aside first.
        emit(ax == kAbsMax ? ax - 1 : ax + 1, ay);
        emit(ax, ay);
    }

private:
    void emit(int ax, int ay)
    {
        input_event ev[3] = {};
        ev[0].type = EV_ABS;
        ev[0].code = ABS_X;
        ev[0].value = ax;
        ev[1].type = EV_ABS;
        ev[1].code = ABS_Y;
        ev[1].value = ay;
        ev[2].type = EV_SYN;
        ev[2].code = SYN_REPORT;
        const ssize_t n = ::write(m_Fd, ev, sizeof(ev));
        (void)n;
    }

    int m_Fd = -1;
};

const char* presentModeName(VkPresentModeKHR mode)
{
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
    case VK_PRESENT_MODE_FIFO_KHR: return "fifo";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo-relaxed";
    default: return "other";
    }
}

/// Everything the loop, the Wayland listeners and the renderer share. One
/// thread: listeners run inside pump(), between frames.
struct App
{
    const Options* options = nullptr;
    Log* log = nullptr;
    std::string error;

    // Wayland.
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    xdg_wm_base* wmBase = nullptr;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    wl_keyboard* keyboard = nullptr;
    wp_presentation* presentation = nullptr;
    clockid_t presentationClock = CLOCK_MONOTONIC;
    bool presentationClockKnown = false;
    std::vector<Output> outputs;
    wl_surface* surface = nullptr;
    xdg_surface* xdgSurface = nullptr;
    xdg_toplevel* toplevel = nullptr;
    bool configured = false;
    bool closed = false;
    int configuredWidth = 0;
    int configuredHeight = 0;
    bool pointerInside = false;

    // The window, on its output.
    const Output* screen = nullptr;
    PixelRect desktop;
    /// Where the window sits on its output: (0, 0) and the output's size full
    /// screen; maximized, the work area, assumed at the bottom right of the
    /// output (GNOME's panel at the top, Ubuntu's dock at the left).
    PixelRect window;
    PixelRect flag;

    // Vulkan.
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR vkSurface = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    std::string gpuName;
    uint32_t queueFamily = 0;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    /// Per swapchain image: signalled by the render, waited on by the present.
    std::vector<VkSemaphore> rendered;
    static constexpr int kSlots = 2;
    VkCommandBuffer commands[kSlots] = {};
    VkFence fences[kSlots] = {};
    VkSemaphore acquired[kSlots] = {};
    int slot = 0;
    bool swapchainStale = false;

    // The loop.
    int64_t endUs = 0;
    int64_t flagUntilUs = 0;
    bool flagShown = false;
    int64_t nextFrameUs = 0;
    int64_t periodUs = 0;
    int clicks = 0;
    bool drawRequested = false;
    std::deque<PendingClick> pending;
    uint64_t frames = 0;
    uint64_t shownFrames = 0;
    uint64_t discardedFrames = 0;
    uint64_t zeroCopyFrames = 0;
    int64_t lastStatsUs = 0;
    uint64_t lastStatsFrames = 0;
    int64_t lastCursorUs = 0;
    bool broughtBack = false;
    bool coveredLogged = false;
    bool quit = false;
    UinputPointer uinput;
};

App* g_App = nullptr;

/// A stamp of the presentation clock onto the steady (monotonic) one.
int64_t presentationToSteadyUs(const App& a, int64_t us)
{
    if (a.presentationClock == CLOCK_MONOTONIC) return us;
    timespec other = {};
    clock_gettime(a.presentationClock, &other);
    const int64_t otherUs = static_cast<int64_t>(other.tv_sec) * 1000000 + other.tv_nsec / 1000;
    return steadyNowUs() - (otherUs - us);
}

/// A wl_pointer/wl_keyboard time (milliseconds, 32 bits, monotonic in Mutter
/// and wlroots) onto the steady clock, unwrapped against now.
int64_t eventTimeUs(uint32_t ms)
{
    const int64_t nowMs = steadyNowUs() / 1000;
    int64_t t = (nowMs & ~int64_t(0xFFFFFFFF)) | ms;
    if (t > nowMs + (int64_t(1) << 31)) t -= int64_t(1) << 32;
    return t * 1000;
}

/// @p shownFrame the frame that reached the screen (0: none known), with
/// wp_presentation's flags (vsync 1, hw clock 2, hw completion 4, zero-copy 8).
std::string clickJson(const PendingClick& c, int64_t displayedUs, const char* how,
                      uint64_t shownFrame = 0, int flags = -1)
{
    return "{\"click\":" + std::to_string(c.id) + ",\"downUs\":" + std::to_string(c.downUs) +
           ",\"eventUs\":" + std::to_string(c.eventUs) +
           ",\"renderUs\":" + std::to_string(c.renderUs) +
           ",\"presentCallUs\":" + std::to_string(c.presentCallUs) +
           ",\"presentUs\":" + std::to_string(c.presentUs) +
           ",\"presentId\":" + std::to_string(c.frame) +
           ",\"displayedUs\":" + (displayedUs ? std::to_string(displayedUs) : std::string("null")) +
           ",\"shownId\":" + std::to_string(shownFrame) + ",\"displayed\":\"" + how +
           "\",\"flags\":" + (flags >= 0 ? std::to_string(flags) : std::string("null")) + "}";
}

// ── Wayland listeners ───────────────────────────────────────────────────────

void onOutputGeometry(void* data, wl_output*, int32_t x, int32_t y, int32_t, int32_t, int32_t,
                      const char* make, const char* model, int32_t)
{
    auto* o = static_cast<Output*>(data);
    o->x = x;
    o->y = y;
    o->make = make ? make : "";
    o->model = model ? model : "";
}

void onOutputMode(void* data, wl_output*, uint32_t flags, int32_t width, int32_t height,
                  int32_t refresh)
{
    auto* o = static_cast<Output*>(data);
    if (!(flags & WL_OUTPUT_MODE_CURRENT)) return;
    o->width = width;
    o->height = height;
    o->refreshMilliHz = refresh;
}

void onOutputDone(void*, wl_output*) {}

void onOutputScale(void* data, wl_output*, int32_t factor)
{
    static_cast<Output*>(data)->scale = factor;
}

void onOutputName(void* data, wl_output*, const char* name)
{
    static_cast<Output*>(data)->name = name ? name : "";
}

void onOutputDescription(void* data, wl_output*, const char* description)
{
    static_cast<Output*>(data)->description = description ? description : "";
}

const wl_output_listener& outputListener()
{
    static const wl_output_listener l = [] {
        wl_output_listener v{};
        v.geometry = &onOutputGeometry;
        v.mode = &onOutputMode;
        v.done = &onOutputDone;
        v.scale = &onOutputScale;
        v.name = &onOutputName;
        v.description = &onOutputDescription;
        return v;
    }();
    return l;
}

void onPointerEnter(void*, wl_pointer* pointer, uint32_t serial, wl_surface* surface, wl_fixed_t,
                    wl_fixed_t)
{
    App& a = *g_App;
    if (surface != a.surface) return;
    a.pointerInside = true;
    // No pointer over the picture, as the Windows and macOS windows hide it.
    wl_pointer_set_cursor(pointer, serial, nullptr, 0, 0);
    if (a.coveredLogged) {
        a.coveredLogged = false;
        a.log->line("{\"covered\":\"none\",\"at\":" + std::to_string(steadyNowUs()) + "}");
    }
}

void onPointerLeave(void*, wl_pointer*, uint32_t, wl_surface* surface)
{
    if (surface == g_App->surface) g_App->pointerInside = false;
}

void onPointerMotion(void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) {}

void onPointerButton(void*, wl_pointer*, uint32_t, uint32_t time, uint32_t button, uint32_t state)
{
    if (button != BTN_LEFT || state != WL_POINTER_BUTTON_STATE_PRESSED) return;
    App& a = *g_App;
    const int64_t downUs = steadyNowUs();
    if (!a.options->drawFlag) {
        // Only a place for the clicks to land: counted, nothing drawn.
        ++a.clicks;
        return;
    }
    PendingClick c;
    c.downUs = downUs;
    c.eventUs = eventTimeUs(time);
    c.id = ++a.clicks;
    a.pending.push_back(c);
    a.flagUntilUs = downUs + int64_t(kFlagMs) * 1000;
    a.drawRequested = true;
}

void onPointerAxis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {}
void onPointerFrame(void*, wl_pointer*) {}
void onPointerAxisSource(void*, wl_pointer*, uint32_t) {}
void onPointerAxisStop(void*, wl_pointer*, uint32_t, uint32_t) {}
void onPointerAxisDiscrete(void*, wl_pointer*, uint32_t, int32_t) {}

const wl_pointer_listener& pointerListener()
{
    // Bound at version 5 at most: the later events are never sent.
    static const wl_pointer_listener l = [] {
        wl_pointer_listener v{};
        v.enter = &onPointerEnter;
        v.leave = &onPointerLeave;
        v.motion = &onPointerMotion;
        v.button = &onPointerButton;
        v.axis = &onPointerAxis;
        v.frame = &onPointerFrame;
        v.axis_source = &onPointerAxisSource;
        v.axis_stop = &onPointerAxisStop;
        v.axis_discrete = &onPointerAxisDiscrete;
        return v;
    }();
    return l;
}

void onKeymap(void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t)
{
    ::close(fd);
}
void onKeyEnter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {}
void onKeyLeave(void*, wl_keyboard*, uint32_t, wl_surface*) {}

void onKey(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state)
{
    if (key == KEY_ESC && state == WL_KEYBOARD_KEY_STATE_PRESSED) g_App->quit = true;
}

void onModifiers(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
void onRepeatInfo(void*, wl_keyboard*, int32_t, int32_t) {}

const wl_keyboard_listener& keyboardListener()
{
    static const wl_keyboard_listener l = [] {
        wl_keyboard_listener v{};
        v.keymap = &onKeymap;
        v.enter = &onKeyEnter;
        v.leave = &onKeyLeave;
        v.key = &onKey;
        v.modifiers = &onModifiers;
        v.repeat_info = &onRepeatInfo;
        return v;
    }();
    return l;
}

void onSeatCapabilities(void*, wl_seat* seat, uint32_t caps)
{
    App& a = *g_App;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !a.pointer) {
        a.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(a.pointer, &pointerListener(), nullptr);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !a.keyboard) {
        a.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(a.keyboard, &keyboardListener(), nullptr);
    }
}

void onSeatName(void*, wl_seat*, const char*) {}

const wl_seat_listener kSeatListener = {&onSeatCapabilities, &onSeatName};

void onPing(void*, xdg_wm_base* base, uint32_t serial)
{
    xdg_wm_base_pong(base, serial);
}

const xdg_wm_base_listener kWmBaseListener = {&onPing};

void onSurfaceConfigure(void*, xdg_surface* surface, uint32_t serial)
{
    xdg_surface_ack_configure(surface, serial);
    g_App->configured = true;
}

const xdg_surface_listener kSurfaceListener = {&onSurfaceConfigure};

void onToplevelConfigure(void*, xdg_toplevel*, int32_t width, int32_t height, wl_array*)
{
    App& a = *g_App;
    if (width > 0 && height > 0 && (width != a.configuredWidth || height != a.configuredHeight)) {
        a.configuredWidth = width;
        a.configuredHeight = height;
        a.swapchainStale = a.swapchain != VK_NULL_HANDLE;
    }
}

void onToplevelClose(void*, xdg_toplevel*)
{
    g_App->closed = true;
}

const xdg_toplevel_listener& toplevelListener()
{
    // Bound at version 2: configure_bounds and wm_capabilities never come.
    static const xdg_toplevel_listener l = [] {
        xdg_toplevel_listener v{};
        v.configure = &onToplevelConfigure;
        v.close = &onToplevelClose;
        return v;
    }();
    return l;
}

void onClockId(void*, wp_presentation*, uint32_t clock)
{
    g_App->presentationClock = static_cast<clockid_t>(clock);
    g_App->presentationClockKnown = true;
}

const wp_presentation_listener kPresentationListener = {&onClockId};

void onSyncOutput(void*, struct wp_presentation_feedback*, wl_output*) {}

void onPresented(void* data, struct wp_presentation_feedback* feedback, uint32_t secHi,
                 uint32_t secLo, uint32_t nsec, uint32_t, uint32_t, uint32_t, uint32_t flags)
{
    App& a = *g_App;
    const auto frame = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(data));
    wp_presentation_feedback_destroy(feedback);
    ++a.shownFrames;
    if (flags & WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY) ++a.zeroCopyFrames;
    const int64_t sec = static_cast<int64_t>((static_cast<uint64_t>(secHi) << 32) | secLo);
    const int64_t shownUs = presentationToSteadyUs(a, sec * 1000000 + nsec / 1000);
    // A frame the compositor never showed (a newer one took its refresh, as
    // most do at 240 fps on a 120 Hz screen) is discarded and says nothing:
    // the click waits for the first frame shown at or after its own, which
    // still carries the flag while it is up.
    while (!a.pending.empty() && a.pending.front().presented && a.pending.front().frame <= frame) {
        const PendingClick& c = a.pending.front();
        a.log->line(clickJson(c, shownUs, c.frame == frame ? "exact" : "later", frame,
                              static_cast<int>(flags)));
        a.pending.pop_front();
    }
}

void onDiscarded(void*, struct wp_presentation_feedback* feedback)
{
    wp_presentation_feedback_destroy(feedback);
    ++g_App->discardedFrames;
}

const wp_presentation_feedback_listener kFeedbackListener = {&onSyncOutput, &onPresented,
                                                             &onDiscarded};

void onGlobal(void*, wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    App& a = *g_App;
    const std::string i = interface;
    if (i == wl_compositor_interface.name) {
        a.compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, 4u)));
    } else if (i == xdg_wm_base_interface.name) {
        a.wmBase = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, std::min(version, 2u)));
        xdg_wm_base_add_listener(a.wmBase, &kWmBaseListener, nullptr);
    } else if (i == wl_seat_interface.name && !a.seat) {
        a.seat = static_cast<wl_seat*>(
            wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5u)));
        wl_seat_add_listener(a.seat, &kSeatListener, nullptr);
    } else if (i == wp_presentation_interface.name) {
        a.presentation = static_cast<wp_presentation*>(
            wl_registry_bind(registry, name, &wp_presentation_interface, 1));
        wp_presentation_add_listener(a.presentation, &kPresentationListener, nullptr);
    } else if (i == wl_output_interface.name) {
        a.outputs.reserve(16);
        if (a.outputs.size() >= 16) return; // pointers into the vector stay valid
        a.outputs.push_back(Output{});
        Output& o = a.outputs.back();
        o.global = name;
        o.handle = static_cast<wl_output*>(
            wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 4u)));
        wl_output_add_listener(o.handle, &outputListener(), &o);
    }
}

void onGlobalRemove(void*, wl_registry*, uint32_t) {}

const wl_registry_listener kRegistryListener = {&onGlobal, &onGlobalRemove};

/// Read and dispatch what the compositor sent, waiting up to @p timeoutUs for
/// it (0: not at all). The WSI reads the same socket for its own queue; the
/// prepare/read dance keeps the two from stealing each other's events.
void pump(App& a, int64_t timeoutUs)
{
    while (wl_display_prepare_read(a.display) != 0)
        wl_display_dispatch_pending(a.display);
    wl_display_flush(a.display);
    pollfd p = {wl_display_get_fd(a.display), POLLIN, 0};
    timespec ts = {static_cast<time_t>(timeoutUs / 1000000),
                   static_cast<long>((timeoutUs % 1000000) * 1000)};
    const int r = ppoll(&p, 1, &ts, nullptr);
    if (r > 0 && (p.revents & POLLIN))
        wl_display_read_events(a.display);
    else
        wl_display_cancel_read(a.display);
    wl_display_dispatch_pending(a.display);
    if (wl_display_get_error(a.display) != 0 || (r > 0 && (p.revents & (POLLERR | POLLHUP)))) {
        a.error = "the compositor closed the connection";
        a.quit = true;
    }
}

// ── Vulkan ──────────────────────────────────────────────────────────────────

bool fail(App& a, const std::string& why, VkResult r = VK_SUCCESS)
{
    a.error = why;
    if (r != VK_SUCCESS) a.error += " (VkResult " + std::to_string(static_cast<int>(r)) + ")";
    return false;
}

bool initVulkan(App& a)
{
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "mw-click-target";
    app.apiVersion = VK_API_VERSION_1_3;
    const char* instanceExtensions[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                        VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = instanceExtensions;
    VkResult r = vkCreateInstance(&ici, nullptr, &a.instance);
    if (r != VK_SUCCESS) return fail(a, "vkCreateInstance", r);

    VkWaylandSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR};
    sci.display = a.display;
    sci.surface = a.surface;
    r = vkCreateWaylandSurfaceKHR(a.instance, &sci, nullptr, &a.vkSurface);
    if (r != VK_SUCCESS) return fail(a, "vkCreateWaylandSurfaceKHR", r);

    // The GPU: by a piece of its name ("warp": the software rasterizer, as on
    // Windows), else the first that presents to this surface and is not the
    // CPU — Mesa's device-select layer puts the compositor's own first.
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(a.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> gpus(count);
    vkEnumeratePhysicalDevices(a.instance, &count, gpus.data());
    std::string want = lower(a.options->adapter);
    if (want == "warp") want = "llvmpipe";
    VkPhysicalDevice cpu = VK_NULL_HANDLE;
    uint32_t cpuFamily = 0;
    for (VkPhysicalDevice g : gpus) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(g, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) continue;
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g, &families, nullptr);
        std::vector<VkQueueFamilyProperties> fp(families);
        vkGetPhysicalDeviceQueueFamilyProperties(g, &families, fp.data());
        int family = -1;
        for (uint32_t f = 0; f < families && family < 0; ++f) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(g, f, a.vkSurface, &present);
            if ((fp[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) family = static_cast<int>(f);
        }
        if (family < 0) continue;
        const std::string name = props.deviceName;
        if (!want.empty()) {
            if (lower(name).find(want) == std::string::npos) continue;
        } else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
            if (!cpu) {
                cpu = g;
                cpuFamily = static_cast<uint32_t>(family);
            }
            continue;
        }
        a.gpu = g;
        a.gpuName = name;
        a.queueFamily = static_cast<uint32_t>(family);
        break;
    }
    if (!a.gpu && cpu) {
        a.gpu = cpu;
        a.queueFamily = cpuFamily;
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(cpu, &props);
        a.gpuName = props.deviceName;
    }
    if (!a.gpu) return fail(a, "no Vulkan 1.3 device presents to this surface");

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = a.queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13.dynamicRendering = VK_TRUE;
    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &v13;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = deviceExtensions;
    r = vkCreateDevice(a.gpu, &dci, nullptr, &a.device);
    if (r != VK_SUCCESS) return fail(a, "vkCreateDevice", r);
    vkGetDeviceQueue(a.device, a.queueFamily, 0, &a.queue);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = a.queueFamily;
    r = vkCreateCommandPool(a.device, &pci, nullptr, &a.pool);
    if (r != VK_SUCCESS) return fail(a, "vkCreateCommandPool", r);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = a.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = App::kSlots;
    r = vkAllocateCommandBuffers(a.device, &cai, a.commands);
    if (r != VK_SUCCESS) return fail(a, "vkAllocateCommandBuffers", r);
    for (int i = 0; i < App::kSlots; ++i) {
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vkCreateFence(a.device, &fci, nullptr, &a.fences[i]) != VK_SUCCESS ||
            vkCreateSemaphore(a.device, &semi, nullptr, &a.acquired[i]) != VK_SUCCESS)
            return fail(a, "fences and semaphores");
    }

    // Present mode: at once, tearing when allowed (immediate; Mesa's Wayland
    // WSI offers it where the compositor takes tearing), else at once without
    // tearing (mailbox), else on the refresh (fifo, --sync 1).
    uint32_t modes = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(a.gpu, a.vkSurface, &modes, nullptr);
    std::vector<VkPresentModeKHR> available(modes);
    vkGetPhysicalDeviceSurfacePresentModesKHR(a.gpu, a.vkSurface, &modes, available.data());
    const auto has = [&](VkPresentModeKHR m) {
        return std::find(available.begin(), available.end(), m) != available.end();
    };
    a.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (a.options->syncInterval == 0) {
        if (a.options->tearing && has(VK_PRESENT_MODE_IMMEDIATE_KHR))
            a.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        else if (has(VK_PRESENT_MODE_MAILBOX_KHR))
            a.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (has(VK_PRESENT_MODE_IMMEDIATE_KHR))
            a.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }

    // Device colours, as the host's flag paints: the probe classifies by
    // "clearly blue / white / red" after a chroma-subsampled encode.
    uint32_t formats = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(a.gpu, a.vkSurface, &formats, nullptr);
    std::vector<VkSurfaceFormatKHR> sf(formats);
    vkGetPhysicalDeviceSurfaceFormatsKHR(a.gpu, a.vkSurface, &formats, sf.data());
    if (sf.empty()) return fail(a, "no surface format");
    a.format = sf[0].format;
    a.colorSpace = sf[0].colorSpace;
    for (const VkSurfaceFormatKHR& f : sf) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
            a.format = f.format;
            a.colorSpace = f.colorSpace;
            break;
        }
    }
    return true;
}

void destroySwapchainViews(App& a)
{
    for (VkImageView v : a.views)
        vkDestroyImageView(a.device, v, nullptr);
    for (VkSemaphore s : a.rendered)
        vkDestroySemaphore(a.device, s, nullptr);
    a.views.clear();
    a.rendered.clear();
    a.images.clear();
}

bool createSwapchain(App& a)
{
    vkDeviceWaitIdle(a.device);
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(a.gpu, a.vkSurface, &caps);
    // On Wayland the surface has no size of its own (0xFFFFFFFF): the client
    // picks it, here the size the compositor configured.
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        extent.width = static_cast<uint32_t>(a.window.w);
        extent.height = static_cast<uint32_t>(a.window.h);
    }
    extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height =
        std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    uint32_t count = std::max(caps.minImageCount, 2u);
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = a.vkSurface;
    ci.minImageCount = count;
    ci.imageFormat = a.format;
    ci.imageColorSpace = a.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    ci.presentMode = a.presentMode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = a.swapchain;
    VkSwapchainKHR made = VK_NULL_HANDLE;
    const VkResult r = vkCreateSwapchainKHR(a.device, &ci, nullptr, &made);
    if (a.swapchain) {
        destroySwapchainViews(a);
        vkDestroySwapchainKHR(a.device, a.swapchain, nullptr);
        a.swapchain = VK_NULL_HANDLE;
    }
    if (r != VK_SUCCESS) return fail(a, "vkCreateSwapchainKHR", r);
    a.swapchain = made;
    a.extent = extent;
    a.swapchainStale = false;

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(a.device, a.swapchain, &n, nullptr);
    a.images.resize(n);
    vkGetSwapchainImagesKHR(a.device, a.swapchain, &n, a.images.data());
    for (VkImage image : a.images) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = a.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(a.device, &vci, nullptr, &view) != VK_SUCCESS)
            return fail(a, "vkCreateImageView");
        a.views.push_back(view);
        VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s = VK_NULL_HANDLE;
        if (vkCreateSemaphore(a.device, &semi, nullptr, &s) != VK_SUCCESS)
            return fail(a, "vkCreateSemaphore");
        a.rendered.push_back(s);
    }
    return true;
}

void destroyVulkan(App& a)
{
    if (a.device) {
        vkDeviceWaitIdle(a.device);
        destroySwapchainViews(a);
        if (a.swapchain) vkDestroySwapchainKHR(a.device, a.swapchain, nullptr);
        for (int i = 0; i < App::kSlots; ++i) {
            if (a.fences[i]) vkDestroyFence(a.device, a.fences[i], nullptr);
            if (a.acquired[i]) vkDestroySemaphore(a.device, a.acquired[i], nullptr);
        }
        if (a.pool) vkDestroyCommandPool(a.device, a.pool, nullptr);
        vkDestroyDevice(a.device, nullptr);
    }
    if (a.vkSurface) vkDestroySurfaceKHR(a.instance, a.vkSurface, nullptr);
    if (a.instance) vkDestroyInstance(a.instance, nullptr);
}

void clearRect(VkCommandBuffer cb, const App& a, PixelRect r, float red, float green, float blue)
{
    const int w = static_cast<int>(a.extent.width);
    const int h = static_cast<int>(a.extent.height);
    if (r.x < 0) {
        r.w += r.x;
        r.x = 0;
    }
    if (r.y < 0) {
        r.h += r.y;
        r.y = 0;
    }
    r.w = std::min(r.w, w - r.x);
    r.h = std::min(r.h, h - r.y);
    if (r.w <= 0 || r.h <= 0) return;
    VkClearAttachment clear{};
    clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clear.colorAttachment = 0;
    clear.clearValue.color = {{red, green, blue, 1.0f}};
    VkClearRect rect{};
    rect.rect = {{r.x, r.y}, {static_cast<uint32_t>(r.w), static_cast<uint32_t>(r.h)}};
    rect.baseArrayLayer = 0;
    rect.layerCount = 1;
    vkCmdClearAttachments(cb, 1, &clear, 1, &rect);
}

/// Draw one frame — the flag when it is up — and present it, a presentation
/// feedback attached. With inputAfterWait, the input is read once the
/// swapchain gave an image, so a click that came during the wait makes it.
void drawNow(App& a)
{
    if (a.swapchainStale) {
        a.window.w = a.configuredWidth;
        a.window.h = a.configuredHeight;
        if (!createSwapchain(a)) {
            a.quit = true;
            return;
        }
    }
    const int slot = a.slot;
    vkWaitForFences(a.device, 1, &a.fences[slot], VK_TRUE, UINT64_MAX);
    uint32_t index = 0;
    VkResult r = vkAcquireNextImageKHR(a.device, a.swapchain, UINT64_MAX, a.acquired[slot],
                                       VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        a.swapchainStale = true;
        return;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        fail(a, "vkAcquireNextImageKHR", r);
        a.quit = true;
        return;
    }
    if (a.options->inputAfterWait) pump(a, 0);

    const int64_t renderUs = steadyNowUs();
    const bool up = a.options->drawFlag && renderUs < a.flagUntilUs;
    vkResetFences(a.device, 1, &a.fences[slot]);
    VkCommandBuffer cb = a.commands[slot];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);

    VkImageMemoryBarrier toDraw{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toDraw.srcAccessMask = 0;
    toDraw.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toDraw.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDraw.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toDraw.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDraw.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDraw.image = a.images[index];
    toDraw.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &toDraw);

    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = a.views[index];
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue.color = {{0.06f, 0.06f, 0.08f, 1.0f}};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, a.extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cb, &ri);
    // A bar along the bottom, a step further each frame: every present is a
    // new picture, as a game's is.
    const int width = static_cast<int>(a.extent.width);
    const int height = static_cast<int>(a.extent.height);
    const int barW = std::max(8, width / 64);
    const int barH = std::max(2, height / 40);
    const int x = static_cast<int>((a.frames * 7) % static_cast<uint64_t>(width - barW + 1));
    const float grey = 90.0f / 255.0f;
    clearRect(cb, a, {x, height - barH, barW, barH}, grey, grey, grey);
    if (up) {
        const PixelRect& f = a.flag;
        const int w = f.w / 3;
        clearRect(cb, a, {f.x, f.y, w, f.h}, 0.0f, 0.0f, 1.0f);
        clearRect(cb, a, {f.x + w, f.y, w, f.h}, 1.0f, 1.0f, 1.0f);
        clearRect(cb, a, {f.x + 2 * w, f.y, f.w - 2 * w, f.h}, 1.0f, 0.0f, 0.0f);
    }
    vkCmdEndRendering(cb);

    VkImageMemoryBarrier toPresent = toDraw;
    toPresent.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toPresent.dstAccessMask = 0;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &toPresent);
    vkEndCommandBuffer(cb);

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &a.acquired[slot];
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &a.rendered[index];
    r = vkQueueSubmit(a.queue, 1, &si, a.fences[slot]);
    if (r != VK_SUCCESS) {
        fail(a, "vkQueueSubmit", r);
        a.quit = true;
        return;
    }
    a.slot = (slot + 1) % App::kSlots;

    // The feedback goes with the surface's next commit: the one the WSI makes
    // inside vkQueuePresentKHR, on this thread.
    const uint64_t frameId = ++a.frames;
    if (a.presentation) {
        struct wp_presentation_feedback* fb = wp_presentation_feedback(a.presentation, a.surface);
        wp_presentation_feedback_add_listener(fb, &kFeedbackListener,
                                              reinterpret_cast<void*>(uintptr_t(frameId)));
    }
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &a.rendered[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &a.swapchain;
    pi.pImageIndices = &index;
    const int64_t presentCallUs = steadyNowUs();
    r = vkQueuePresentKHR(a.queue, &pi);
    const int64_t presentUs = steadyNowUs();
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        a.swapchainStale = true;
    } else if (r != VK_SUCCESS) {
        fail(a, "vkQueuePresentKHR", r);
        a.quit = true;
        return;
    }
    a.flagShown = up;
    if (!up) return;
    for (PendingClick& c : a.pending) {
        if (c.presented) continue;
        c.presented = true;
        c.renderUs = renderUs;
        c.presentCallUs = presentCallUs;
        c.presentUs = presentUs;
        c.frame = frameId;
    }
}

/// The probe clicks wherever the host's pointer is and never moves it: put it
/// on the window, and back whenever it left. Wayland says only whether it is
/// over this surface, not where it is nor whose window is on top.
void keepCursor(App& a, const char* why)
{
    if (a.pointerInside) {
        a.broughtBack = false;
        return;
    }
    if (a.broughtBack && !a.coveredLogged) {
        // Brought back last time and still not over us: something is on top.
        a.coveredLogged = true;
        a.log->line("{\"covered\":\"another surface\",\"at\":" + std::to_string(steadyNowUs()) +
                    "}");
    }
    if (!a.uinput.isOpen() || !a.screen) return;
    const int cx = a.screen->x + a.window.x + a.window.w / 2;
    const int cy = a.screen->y + a.window.y + a.window.h / 2;
    a.uinput.moveTo(cx, cy, a.desktop);
    a.broughtBack = true;
    a.log->line("{\"cursor\":\"" + std::string(why) + "\",\"at\":" + std::to_string(steadyNowUs()) +
                ",\"to\":\"" + std::to_string(cx) + "," + std::to_string(cy) + "\"}");
}

/// One turn of the loop: a frame when due, the pointer, the counts.
void tick(App& a)
{
    const Options& o = *a.options;
    const int64_t now = steadyNowUs();
    const bool flagChanged = (now < a.flagUntilUs) != a.flagShown;
    bool waiting = false;
    for (const PendingClick& c : a.pending)
        if (!c.presented) waiting = true;
    // A click never presented (or never reported) for a second: said, dropped.
    while (!a.pending.empty() && now - a.pending.front().downUs > 1000000) {
        a.log->line(clickJson(a.pending.front(), 0, "unknown"));
        a.pending.pop_front();
    }
    const bool due = o.continuous && now >= a.nextFrameUs;
    const bool atOnce = a.drawRequested && o.reactAtOnce;
    a.drawRequested = false;
    if (due || atOnce || (!o.continuous && (flagChanged || waiting))) {
        drawNow(a);
        if (due) a.nextFrameUs = a.periodUs > 0 ? std::max(a.nextFrameUs + a.periodUs, now) : now;
    }
    if (now - a.lastCursorUs >= 250000) {
        keepCursor(a, "brought back");
        a.lastCursorUs = now;
    }
    if (now - a.lastStatsUs >= 5000000) {
        a.log->line("{\"at\":" + std::to_string(now) + ",\"fps\":" +
                    std::to_string((a.frames - a.lastStatsFrames) * 1000000 /
                                   static_cast<uint64_t>(now - a.lastStatsUs)) +
                    ",\"clicks\":" + std::to_string(a.clicks) +
                    ",\"shown\":" + std::to_string(a.shownFrames) +
                    ",\"discarded\":" + std::to_string(a.discardedFrames) +
                    ",\"zeroCopy\":" + std::to_string(a.zeroCopyFrames) + "}");
        a.lastStatsUs = now;
        a.lastStatsFrames = a.frames;
    }
}

const Output* pickOutput(const App& a, const std::string& want)
{
    if (a.outputs.empty()) return nullptr;
    if (want.empty()) return &a.outputs.front();
    char* end = nullptr;
    const long n = std::strtol(want.c_str(), &end, 10);
    if (end && !*end) {
        if (n >= 0 && n < static_cast<long>(a.outputs.size()))
            return &a.outputs[static_cast<size_t>(n)];
        return nullptr;
    }
    const std::string w = lower(want);
    for (const Output& o : a.outputs)
        if (lower(o.name) == w) return &o;
    for (const Output& o : a.outputs)
        if (lower(o.name + " " + o.description + " " + o.make + " " + o.model).find(w) !=
            std::string::npos)
            return &o;
    return nullptr;
}

void cleanup(App& a)
{
    destroyVulkan(a);
    if (a.toplevel) xdg_toplevel_destroy(a.toplevel);
    if (a.xdgSurface) xdg_surface_destroy(a.xdgSurface);
    if (a.surface) wl_surface_destroy(a.surface);
    if (a.display) {
        wl_display_flush(a.display);
        wl_display_disconnect(a.display);
    }
    a.uinput.close();
}

} // namespace

int run(const Options& o, Log& log)
{
    App a;
    g_App = &a;
    a.options = &o;
    a.log = &log;
    struct sigaction sa = {};
    sa.sa_handler = &onSignal;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    const auto bail = [&](const std::string& why) {
        std::fprintf(stderr, "mw-click-target: %s\n", why.c_str());
        cleanup(a);
        g_App = nullptr;
        return 1;
    };

    a.display = wl_display_connect(nullptr);
    if (!a.display) return bail("no Wayland display (WAYLAND_DISPLAY, XDG_RUNTIME_DIR?)");
    a.registry = wl_display_get_registry(a.display);
    wl_registry_add_listener(a.registry, &kRegistryListener, nullptr);
    // Twice: the globals, then what each bound one says (outputs, seat, clock).
    wl_display_roundtrip(a.display);
    wl_display_roundtrip(a.display);
    if (!a.compositor || !a.wmBase) return bail("the compositor offers no xdg_wm_base");

    std::string outputs;
    int minX = 0, minY = 0, maxX = 0, maxY = 0;
    bool first = true;
    for (const Output& out : a.outputs) {
        const int lw = out.width / std::max(1, out.scale);
        const int lh = out.height / std::max(1, out.scale);
        if (first) {
            minX = out.x;
            minY = out.y;
            maxX = out.x + lw;
            maxY = out.y + lh;
            first = false;
        }
        minX = std::min(minX, out.x);
        minY = std::min(minY, out.y);
        maxX = std::max(maxX, out.x + lw);
        maxY = std::max(maxY, out.y + lh);
        outputs += std::string(outputs.empty() ? "" : ",") + "{\"name\":\"" + jsonText(out.name) +
                   "\",\"description\":\"" + jsonText(out.description) + "\",\"at\":\"" +
                   std::to_string(out.x) + "," + std::to_string(out.y) + "\",\"size\":\"" +
                   std::to_string(out.width) + "x" + std::to_string(out.height) +
                   "\",\"mHz\":" + std::to_string(out.refreshMilliHz) +
                   ",\"scale\":" + std::to_string(out.scale) + "}";
    }
    log.line("{\"outputs\":[" + outputs + "]}");
    a.desktop = PixelRect{minX, minY, maxX - minX, maxY - minY};
    a.screen = pickOutput(a, o.display);
    if (!a.screen) return bail("no output \"" + o.display + "\"");
    if (a.screen->scale != 1)
        log.line("{\"warning\":\"output scale " + std::to_string(a.screen->scale) +
                 ": the flag is placed in buffer pixels\"}");

    // The pointer onto the output first: a maximized window opens where it is.
    std::string uinputError;
    if (a.uinput.open(uinputError)) {
        // The compositor takes a moment to open a new input device.
        for (int i = 0; i < 6; ++i)
            pump(a, 100000);
        a.uinput.moveTo(a.screen->x + a.screen->width / 2, a.screen->y + a.screen->height / 2,
                        a.desktop);
        pump(a, 50000);
    } else {
        log.line("{\"warning\":\"" + jsonText(uinputError) +
                 ": the pointer is not brought onto the window\"}");
    }

    a.surface = wl_compositor_create_surface(a.compositor);
    a.xdgSurface = xdg_wm_base_get_xdg_surface(a.wmBase, a.surface);
    xdg_surface_add_listener(a.xdgSurface, &kSurfaceListener, nullptr);
    a.toplevel = xdg_surface_get_toplevel(a.xdgSurface);
    xdg_toplevel_add_listener(a.toplevel, &toplevelListener(), nullptr);
    xdg_toplevel_set_title(a.toplevel, "mw-click-target");
    xdg_toplevel_set_app_id(a.toplevel, "mw-click-target");
    if (o.fullscreen)
        xdg_toplevel_set_fullscreen(a.toplevel, a.screen->handle);
    else
        xdg_toplevel_set_maximized(a.toplevel);
    wl_surface_commit(a.surface);
    for (int i = 0; i < 40 && !a.configured && !a.quit; ++i)
        pump(a, 50000);
    if (!a.configured) return bail("the compositor never configured the window");
    pump(a, 50000);

    const int sw = a.screen->width;
    const int sh = a.screen->height;
    a.window = PixelRect{0, 0, a.configuredWidth > 0 ? a.configuredWidth : sw,
                         a.configuredHeight > 0 ? a.configuredHeight : sh};
    if (!o.fullscreen) {
        a.window.x = std::max(0, sw - a.window.w);
        a.window.y = std::max(0, sh - a.window.h);
    }
    if (!initVulkan(a) || !createSwapchain(a)) return bail(a.error);

    // The flag at its fraction of the SCREEN, in the window's pixels.
    a.flag = PixelRect{static_cast<int>(sw * kFlagLeft) - a.window.x,
                       static_cast<int>(sh * kFlagTop) - a.window.y,
                       static_cast<int>(sw * (kFlagRight - kFlagLeft)),
                       static_cast<int>(sh * (kFlagBottom - kFlagTop))};
    if (a.flag.y < 0)
        log.line("{\"warning\":\"the window starts " + std::to_string(a.window.y) +
                 " px down its output: the flag is partly under the shell's panel\"}");

    const char* clock = a.presentationClock == CLOCK_MONOTONIC ? "monotonic" : "other";
    log.line(
        "{\"start\":" + std::to_string(steadyNowUs()) + ",\"screen\":\"" +
        jsonText(a.screen->name) + "\",\"description\":\"" + jsonText(a.screen->description) +
        "\",\"size\":\"" + std::to_string(sw) + "x" + std::to_string(sh) +
        "\",\"hz\":" + std::to_string((a.screen->refreshMilliHz + 500) / 1000) + ",\"window\":\"" +
        std::to_string(a.extent.width) + "x" + std::to_string(a.extent.height) + "+" +
        std::to_string(a.window.x) + "+" + std::to_string(a.window.y) + "\",\"adapter\":\"" +
        jsonText(a.gpuName) + "\",\"mode\":\"" + (o.fullscreen ? "fullscreen" : "window") +
        "\",\"sync\":" + std::to_string(o.syncInterval) + ",\"presentMode\":\"" +
        presentModeName(a.presentMode) + "\",\"images\":" + std::to_string(a.images.size()) +
        ",\"continuous\":" + (o.continuous ? "true" : "false") +
        ",\"fps\":" + std::to_string(o.fps) + ",\"react\":\"" + (o.reactAtOnce ? "now" : "frame") +
        "\",\"flag\":" + (o.drawFlag ? "true" : "false") + ",\"presentation\":" +
        (a.presentation ? std::string("\"") + (a.presentationClockKnown ? clock : "unknown") + "\""
                        : std::string("null")) +
        "}");

    a.periodUs = o.continuous && o.fps > 0 ? 1000000 / o.fps : 0;
    a.endUs = steadyNowUs() + int64_t(o.durationS) * 1000000;
    a.nextFrameUs = steadyNowUs();
    a.lastStatsUs = steadyNowUs();
    a.lastCursorUs = steadyNowUs();

    // A first picture, whatever the mode: the screen is ours from here.
    drawNow(a);
    pump(a, 20000);
    keepCursor(a, "placed");

    while (!a.quit && !a.closed && !g_Signalled) {
        const int64_t now = steadyNowUs();
        if (now >= a.endUs) break;
        // Until the next frame due, a few milliseconds at most: the flag that
        // goes down, the pointer, the counts.
        int64_t waitUs = 4000;
        if (o.continuous)
            waitUs = std::min<int64_t>(waitUs, std::max<int64_t>(0, a.nextFrameUs - now));
        pump(a, waitUs);
        if (a.quit) break;
        tick(a);
    }

    log.line("{\"end\":" + std::to_string(steadyNowUs()) +
             ",\"clicks\":" + std::to_string(a.clicks) + ",\"frames\":" + std::to_string(a.frames) +
             ",\"shown\":" + std::to_string(a.shownFrames) +
             ",\"discarded\":" + std::to_string(a.discardedFrames) +
             ",\"zeroCopy\":" + std::to_string(a.zeroCopyFrames) +
             (a.error.empty() ? std::string() : ",\"error\":\"" + jsonText(a.error) + "\"") + "}");
    cleanup(a);
    g_App = nullptr;
    return a.error.empty() ? 0 : 1;
}

} // namespace clicktarget
