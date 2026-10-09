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

#include "CgInput.h"

#include "../../core/Log.h"
#include "MacKeyMap.h"

#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <IOKit/hidsystem/IOHIDParameter.h>
#include <IOKit/hidsystem/IOHIDShared.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace mw::native::input {
namespace {

int64_t steadyNowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Presses closer than this on the same button count as one multi-click.
constexpr int64_t kDoubleClickUs = 500 * 1000;

const char* describe(InputEvent::Type type)
{
    switch (type) {
    case InputEvent::Type::KeyDown: return "key press";
    case InputEvent::Type::KeyUp: return "key release";
    case InputEvent::Type::CharDown: return "character press";
    case InputEvent::Type::CharUp: return "character release";
    case InputEvent::Type::Utf8Text: return "text";
    case InputEvent::Type::MouseMoveRelative: return "relative mouse move";
    case InputEvent::Type::MouseMoveAbsolute: return "absolute mouse move";
    case InputEvent::Type::MouseButtonDown: return "mouse button press";
    case InputEvent::Type::MouseButtonUp: return "mouse button release";
    case InputEvent::Type::MouseScrollVertical: return "scroll";
    case InputEvent::Type::MouseScrollHorizontal: return "horizontal scroll";
    case InputEvent::Type::ControllerArrival: return "controller arrival";
    case InputEvent::Type::ControllerState: return "controller state";
    case InputEvent::Type::ControllerRemoval: return "controller removal";
    case InputEvent::Type::LockKeySync: return "lock-key sync";
    case InputEvent::Type::SecureAttention: return "Ctrl+Alt+Suppr";
    }
    return "event";
}

// ── Keyboard diagnostics ────────────────────────────────────────────────────
//
// Only when NativeHost::setKeyboardDiagnostics(true) was called, only on a key
// down. Two verdicts, because a keystroke has two jobs that fail separately:
// Notepad (the character a text field shows) and Game (the physical key a title
// reading raw HID sees, named by its US label, which is how bindings read).
//
// As on Linux and unlike Windows, there is no round trip to make: m_CharMap is
// built FORWARD, by asking UCKeyTranslate what each key code of the active
// layout produces, so a character found in it is one this layout really types
// there. What the line adds is which key it landed on.

/// The US label of a CGKeyCode, found by searching the US table rather than
/// storing a second one the two could drift apart on.
std::string usKeyLabel(uint16_t code)
{
    for (int vk = 0x08; vk <= 0xFE; ++vk) {
        if (macKeyCode(vk) != code) continue;
        if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
            return std::string(1, static_cast<char>(vk));
        switch (vk) {
        case 0x20: return "Space";
        case 0x0D: return "Return";
        case 0x09: return "Tab";
        case 0x08: return "Delete";
        case 0xBA: return ";";
        case 0xBB: return "=";
        case 0xBC: return ",";
        case 0xBD: return "-";
        case 0xBE: return ".";
        case 0xBF: return "/";
        case 0xC0: return "`";
        case 0xDB: return "[";
        case 0xDC: return "\\";
        case 0xDD: return "]";
        case 0xDE: return "'";
        default: break;
        }
        break;
    }
    return "key code " + std::to_string(code);
}

/// One UTF-16 unit as UTF-8, for a log line.
std::string utf8Of(uint16_t unit)
{
    const UniChar value = unit;
    CFStringRef text = CFStringCreateWithCharacters(kCFAllocatorDefault, &value, 1);
    if (!text) return std::string();
    char buffer[8] = {};
    const bool ok = CFStringGetCString(text, buffer, sizeof(buffer), kCFStringEncodingUTF8);
    CFRelease(text);
    return ok ? std::string(buffer) : std::string();
}

/// The level modifiers a character's key needs, spelled out.
std::string modifierText(uint64_t flags)
{
    std::string out;
    if (flags & kCGEventFlagMaskShift) out += "+Shift";
    if (flags & kCGEventFlagMaskAlternate) out += "+Option";
    if (flags & kCGEventFlagMaskControl) out += "+Control";
    if (flags & kCGEventFlagMaskCommand) out += "+Command";
    return out;
}

/// The same, for a line that has to say something when nothing is held.
std::string modifierLabel(uint64_t flags)
{
    const std::string out = modifierText(flags);
    return out.empty() ? std::string("none") : out.substr(1);
}

/// Each held modifier's flag and the key that carries it on a Mac board, in
/// the order a correction posts them. Left-hand keys: a flag says which
/// modifier is down, never which side of the keyboard it came from.
constexpr struct
{
    uint64_t flag;
    uint16_t code;
} kFlagKeys[] = {
    {kMacFlagShift, 0x38},     // kVK_Shift
    {kMacFlagControl, 0x3B},   // kVK_Control
    {kMacFlagAlternate, 0x3A}, // kVK_Option
    {kMacFlagCommand, 0x37},   // kVK_Command
};

/// The modifier flag a virtual key sets, or 0 for an ordinary key.
CGEventFlags modifierFlag(int vk)
{
    switch (vk) {
    case 0x10:
    case 0xA0:
    case 0xA1: return kCGEventFlagMaskShift;
    case 0x11:
    case 0xA2:
    case 0xA3: return kCGEventFlagMaskControl;
    case 0x12:
    case 0xA4:
    case 0xA5: return kCGEventFlagMaskAlternate;
    case 0x5B:
    case 0x5C: return kCGEventFlagMaskCommand;
    case 0x14: return kCGEventFlagMaskAlphaShift;
    default: return 0;
    }
}

/// CoreGraphics' button number for the browser's: 0 left, 1 right, 2 middle,
/// then the extras.
CGMouseButton cgButton(int button)
{
    switch (button) {
    case 1: return kCGMouseButtonLeft;
    case 3: return kCGMouseButtonRight;
    case 2: return kCGMouseButtonCenter;
    case 4: return static_cast<CGMouseButton>(3);
    case 5: return static_cast<CGMouseButton>(4);
    default: return kCGMouseButtonLeft;
    }
}

void post(CGEventRef event)
{
    if (!event) return;
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}

/// Where the pointer is, in points of the global display space — read from the
/// window server, not from our own account of what we posted. An event created
/// out of nothing carries the current location, which is the documented way to
/// ask without an NSApplication around.
bool pointerLocation(double& x, double& y)
{
    CGEventRef probe = CGEventCreate(nullptr);
    if (!probe) return false;
    const CGPoint at = CGEventGetLocation(probe);
    CFRelease(probe);
    x = at.x;
    y = at.y;
    return true;
}

/// A connection to IOHIDSystem, the door a relative move goes through when it
/// should be treated like a mouse's (see injectMouseMove). 0 when it cannot be
/// opened, and the caller then falls back to posting positions.
io_connect_t openHidSystem()
{
    io_service_t service =
        IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(kIOHIDSystemClass));
    if (!service) return 0;
    io_connect_t connect = 0;
    if (IOServiceOpen(service, mach_task_self(), kIOHIDParamConnectType, &connect) != KERN_SUCCESS)
        connect = 0;
    IOObjectRelease(service);
    return connect;
}

} // namespace

CgInput::~CgInput()
{
    stop();
}

bool CgInput::start(std::string& error)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Started) return true;

    // Accessibility. Without it every CGEventPost is accepted and ignored,
    // which from the browser is a stream that shows but does not answer.
    //
    // The QUESTION is no longer asked here first: MacProbe asks it at startup,
    // so it reaches the user with the install rather than mid-stream (§20.15).
    // This is the check that tells the truth about THIS session — a grant can
    // have been revoked, or lost to a signature change, since the probe last
    // looked — and asking again costs nothing: macOS shows its prompt once per
    // program, ever. The switch itself is the user's to flip
    // (System Settings → Privacy & Security → Accessibility).
    if (!CGPreflightPostEventAccess()) {
        CGRequestPostEventAccess();
        log::warning("[native] input: macOS has not granted Accessibility to this program — "
                     "keyboard and mouse will not reach the desktop until it is allowed in "
                     "System Settings › Privacy & Security › Accessibility");
    }

    // Where the pointer is now, so relative motion starts from the truth.
    pointerLocation(m_X, m_Y);
    m_PointerStale = false;
    m_HidSystem = openHidSystem();
    if (!m_HidSystem)
        log::warning("[native] input: IOHIDSystem could not be opened — relative motion is "
                     "posted as positions, and a game that holds the pointer still will see "
                     "it move");
    m_Accel.start();
    m_Recentre.reset();
    m_Modifiers = 0;
    m_ModifierFixes = 0;
    m_Started = true;
    log::info("[native] input: Quartz events on the display at " + std::to_string(m_Left) + "," +
              std::to_string(m_Top) + " " + std::to_string(m_Right - m_Left) + "x" +
              std::to_string(m_Bottom - m_Top) + " pt");
    (void)error;
    return true;
}

void CgInput::stop()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Started) return;
    releaseAll();
    log::info("[native] input: " + std::to_string(m_Injected.load(std::memory_order_relaxed)) +
              " event(s) injected this session");
    if (m_HidSystem) {
        IOServiceClose(m_HidSystem);
        m_HidSystem = 0;
    }
    m_Started = false;
}

void CgInput::setDisplayRect(int left, int top, int right, int bottom)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (left == m_Left && top == m_Top && right == m_Right && bottom == m_Bottom) return;
    m_Left = left;
    m_Top = top;
    m_Right = right;
    m_Bottom = bottom;
    // A spot the pointer kept returning to on the old rectangle means nothing
    // on the new one, and only a spot on this one can be learnt from now on.
    m_Recentre.setDisplay(left, top, right, bottom);
    if (m_Started)
        log::info("[native] input: display now at " + std::to_string(left) + "," +
                  std::to_string(top) + " " + std::to_string(right - left) + "x" +
                  std::to_string(bottom - top) + " pt");
}

void CgInput::releaseAll()
{
    // The caller holds m_Mutex.
    std::set<int> keys;
    std::set<int> buttons;
    std::set<uint16_t> charCodes;
    keys.swap(m_HeldKeys);
    buttons.swap(m_HeldButtons);
    charCodes.swap(m_HeldCharCodes);
    if (keys.empty() && buttons.empty() && charCodes.empty()) return;

    // Characters first: they were pressed as real keys of the host's layout and
    // would otherwise stay down, and their release wants the modifiers still in
    // the state they were pressed under.
    for (uint16_t code : charCodes) {
        CGEventRef up = CGEventCreateKeyboardEvent(nullptr, code, false);
        CGEventSetFlags(up, static_cast<CGEventFlags>(m_Modifiers));
        post(up);
    }

    for (int vk : keys) {
        const uint16_t code = macKeyCode(vk);
        if (code == kMacNoKey) continue;
        CGEventRef up = CGEventCreateKeyboardEvent(nullptr, code, false);
        if (const CGEventFlags flag = modifierFlag(vk)) {
            m_Modifiers &= ~flag;
            CGEventSetType(up, kCGEventFlagsChanged);
        }
        CGEventSetFlags(up, static_cast<CGEventFlags>(m_Modifiers));
        post(up);
    }
    syncPointer();
    for (int button : buttons)
        postButton(button, false, m_X, m_Y);
    m_Modifiers = 0;
    log::info("[native] input: released " + std::to_string(keys.size() + charCodes.size()) +
              " key(s) and " + std::to_string(buttons.size()) + " button(s) at session end");
}

void CgInput::inject(const InputEvent& event)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Started) return;

    const uint32_t bit = 1u << static_cast<int>(event.type);
    if ((m_SeenTypes.fetch_or(bit, std::memory_order_relaxed) & bit) == 0)
        log::info(std::string("[native] input: first ") + describe(event.type) + " injected");
    m_Injected.fetch_add(1, std::memory_order_relaxed);

    switch (event.type) {
    case InputEvent::Type::KeyDown: injectKey(event, true); break;
    case InputEvent::Type::KeyUp: injectKey(event, false); break;
    case InputEvent::Type::CharDown:
    case InputEvent::Type::CharUp:
        // A character is never a modifier itself, so the client's mask is the
        // whole truth about what is held around it.
        if (event.modifiersKnown) reconcileModifiers(event.modifiers);
        injectChar(event.text, event.type == InputEvent::Type::CharDown);
        break;
    case InputEvent::Type::Utf8Text: injectText(event.text); break;
    case InputEvent::Type::MouseMoveRelative: injectMouseMove(event.deltaX, event.deltaY); break;
    case InputEvent::Type::MouseMoveAbsolute: injectMousePosition(event); break;
    case InputEvent::Type::MouseButtonDown: injectMouseButton(event.button, true); break;
    case InputEvent::Type::MouseButtonUp: injectMouseButton(event.button, false); break;
    case InputEvent::Type::MouseScrollVertical: injectScroll(event.scrollAmount, false); break;
    case InputEvent::Type::MouseScrollHorizontal: injectScroll(event.scrollAmount, true); break;
    case InputEvent::Type::LockKeySync: syncLockKeys(event); break;
    // Windows' secure desktop has no macOS counterpart to reach.
    case InputEvent::Type::SecureAttention: break;
    // No virtual gamepad on macOS (see §8 of the plan: a signed DriverKit
    // extension, which needs an Apple entitlement). Ignored, never mapped
    // onto the mouse.
    case InputEvent::Type::ControllerArrival:
    case InputEvent::Type::ControllerState:
    case InputEvent::Type::ControllerRemoval: break;
    }
}

void CgInput::injectKey(const InputEvent& event, bool down)
{
    const int vk = event.keyCode;
    if (vk <= 0 || vk > 0xFF) return;
    const uint16_t code = macKeyCode(vk);
    if (code == kMacNoKey) return;

    // The client states its whole modifier state on every keystroke; ours is
    // a shadow that drifts the moment a key-up is eaten — the Windows key
    // opening the Start menu is the everyday one, and it left Command down
    // for the rest of the stream, where Space read as ⌘Space and opened
    // Spotlight instead of making the character jump. Put the flags back
    // before the key goes out. Never from a modifier's own event: that event
    // IS the statement about its flag, and is applied below.
    if (event.modifiersKnown && modifierFlag(vk) == 0) reconcileModifiers(event.modifiers);

    if (down && keyboardDiagnostics())
        log::info("[KBD] host flags " + modifierLabel(m_Modifiers) + " | client says " +
                  (event.modifiersKnown ? modifierLabel(reconcileModifierFlags(0, event.modifiers))
                                        : std::string("(not stated)")) +
                  " | key code " + std::to_string(code));

    // A heartbeat re-press of a key still held would be an extra character;
    // a real repeat is passed through (the user holding a key wants typematic).
    if (down && event.resync && m_HeldKeys.count(vk)) return;
    if (down)
        m_HeldKeys.insert(vk);
    else
        m_HeldKeys.erase(vk);

    CGEventRef key = CGEventCreateKeyboardEvent(nullptr, code, down);
    if (!key) return;
    if (const CGEventFlags flag = modifierFlag(vk)) {
        // A modifier is a change of flags, not a keystroke: the event says
        // which key moved and carries the whole modifier state after it.
        if (vk == 0x14) {
            // Caps Lock toggles on press; its flag reflects the lock state.
            if (down) m_Modifiers ^= flag;
        } else if (down) {
            m_Modifiers |= flag;
        } else {
            m_Modifiers &= ~flag;
        }
        CGEventSetType(key, kCGEventFlagsChanged);
    }
    CGEventSetFlags(key, static_cast<CGEventFlags>(m_Modifiers));
    post(key);

    if (down && keyboardDiagnostics() && ensureCharMap()) {
        // Positional path: the key's US position went out and the host's layout
        // decides. No verdict — there is no client character here to check it
        // against; the transport's line for the same keystroke carries that,
        // and the two read together.
        for (const auto& entry : m_CharMap) {
            if (entry.second.code != code || entry.second.flags != 0) continue;
            log::info(
                "[KBD] host position key code " + std::to_string(code) + " (US '" +
                usKeyLabel(code) + "') -> " + m_CharMapSource + " types '" + utf8Of(entry.first) +
                "' | Notepad: no client character to check against | Game: OK real key, US '" +
                usKeyLabel(code) + "'");
            break;
        }
    }
}

void CgInput::reconcileModifiers(uint8_t clientMask)
{
    const uint64_t was = m_Modifiers;
    const uint64_t wanted = reconcileModifierFlags(was, clientMask);
    if (wanted == was) return;

    // A virtual key whose flag just fell is not held any more: left in
    // m_HeldKeys, releaseAll() would post a key-up for a key the viewer let
    // go of long ago.
    const uint64_t dropped = was & ~wanted;
    for (auto it = m_HeldKeys.begin(); it != m_HeldKeys.end();) {
        if (modifierFlag(*it) & dropped)
            it = m_HeldKeys.erase(it);
        else
            ++it;
    }

    // The correction goes out as its own events, one per modifier that moved
    // and naming that modifier's key, which is exactly what the key-up we
    // never got would have looked like. An application that follows
    // flags-changed events — which is how a game reads the modifier keys —
    // keeps its own account of them, and fixing only the flags of the next
    // keystroke would leave that account wrong.
    uint64_t flags = was;
    for (const auto& fk : kFlagKeys) {
        if ((was & fk.flag) == (wanted & fk.flag)) continue;
        flags = (flags & ~fk.flag) | (wanted & fk.flag);
        const bool held = (wanted & fk.flag) != 0;
        if (CGEventRef ev = CGEventCreateKeyboardEvent(nullptr, fk.code, held)) {
            CGEventSetType(ev, kCGEventFlagsChanged);
            CGEventSetFlags(ev, static_cast<CGEventFlags>(flags));
            post(ev);
        }
    }
    m_Modifiers = wanted;

    // Said once at full voice, then only under the keyboard diagnostics: it
    // is an anomaly worth a line in an ordinary log, not a running commentary.
    if (m_ModifierFixes++ == 0 || keyboardDiagnostics())
        log::warning("[KBD] modifier drift: the host held " + modifierLabel(was & kMacHeldFlags) +
                     ", the client says " + modifierLabel(wanted & kMacHeldFlags) +
                     " — flags corrected (" + std::to_string(m_ModifierFixes) + ")");
}

bool CgInput::ensureCharMap()
{
    // Rebuilt whenever the active input source changes, because the viewer — or
    // the host's own user — can switch layouts mid-stream and every key code in
    // the map would then mean a different character.
    TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
    if (!source) return !m_CharMap.empty();

    std::string id;
    if (auto* name = static_cast<CFStringRef>(
            TISGetInputSourceProperty(source, kTISPropertyInputSourceID))) {
        char buffer[256] = {};
        if (CFStringGetCString(name, buffer, sizeof(buffer), kCFStringEncodingUTF8)) id = buffer;
    }
    if (!id.empty() && id == m_CharMapSource) {
        CFRelease(source);
        return !m_CharMap.empty();
    }

    auto* data =
        static_cast<CFDataRef>(TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData));
    if (!data) {
        CFRelease(source);
        return false;
    }
    const auto* layout = reinterpret_cast<const UCKeyboardLayout*>(CFDataGetBytePtr(data));

    m_CharMap.clear();
    m_CharMapSource = id;
    // The four levels a single key can carry on a Mac layout. Listed cheapest
    // first so a character reachable without modifiers wins over the same
    // character behind Option — first writer keeps the slot.
    static constexpr struct
    {
        uint32_t carbon; ///< UCKeyTranslate's modifier field (already >> 8)
        CGEventFlags flags;
    } kLevels[] = {
        {0, 0},
        {shiftKey >> 8, kCGEventFlagMaskShift},
        {optionKey >> 8, kCGEventFlagMaskAlternate},
        {(shiftKey | optionKey) >> 8, kCGEventFlagMaskShift | kCGEventFlagMaskAlternate},
    };

    for (uint16_t code = 0; code < 128; ++code) {
        for (const auto& level : kLevels) {
            UInt32 deadState = 0;
            UniChar chars[4] = {};
            UniCharCount length = 0;
            if (UCKeyTranslate(layout, code, kUCKeyActionDown, level.carbon, LMGetKbdType(),
                               kUCKeyTranslateNoDeadKeysBit, &deadState, 4, &length,
                               chars) != noErr)
                continue;
            // One unit only: a dead key produces none, and anything longer is
            // not something a single key press expresses.
            if (length != 1 || chars[0] < 0x20 || chars[0] == 0x7f) continue;
            m_CharMap.emplace(chars[0], CharKey{code, level.flags});
        }
    }
    CFRelease(source);
    log::info("[native] input: host keyboard layout " +
              (id.empty() ? std::string("(unnamed)") : id) + ", " +
              std::to_string(m_CharMap.size()) + " characters reachable as real keys");
    return !m_CharMap.empty();
}

void CgInput::injectChar(const std::string& utf8, bool down)
{
    if (utf8.empty()) return;

    CFStringRef text =
        CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(utf8.data()),
                                static_cast<CFIndex>(utf8.size()), kCFStringEncodingUTF8, false);
    if (!text) return;
    const bool single = CFStringGetLength(text) == 1;
    const UniChar unit = single ? CFStringGetCharacterAtIndex(text, 0) : 0;
    CFRelease(text);

    const auto it = single && ensureCharMap() ? m_CharMap.find(unit) : m_CharMap.end();
    if (it == m_CharMap.end()) {
        // No key on this layout carries the character — type it as Unicode
        // instead, which needs no key at all. The press does it; the release
        // has nothing left to do.
        if (down) {
            injectText(utf8);
            if (keyboardDiagnostics())
                log::warning("[KBD] host '" + utf8 +
                             "' -> no key on this layout | Notepad: OK as Unicode text | Game: KO "
                             "nothing was pressed");
        }
        return;
    }

    CGEventRef key = CGEventCreateKeyboardEvent(nullptr, it->second.code, down);
    if (!key) return;
    // The LEVEL modifiers — Shift and Option — are the layout's decision, not
    // the viewer's hand's: the viewer pressed Shift for their own layout, where
    // "1" on AZERTY is Shift+&, and on a US host that character wants no Shift
    // at all. Left in the flags, the viewer's Shift turned every AZERTY digit
    // into a US symbol. So the viewer's level flags are replaced by the map's,
    // and only those: Command and Control are chords (Cmd+A) and ride through
    // untouched. On macOS a modifier is a flag on the event, so nothing has to
    // be pressed or released around the key — and nothing has to be put back.
    constexpr CGEventFlags kLevel = kCGEventFlagMaskShift | kCGEventFlagMaskAlternate;
    CGEventSetFlags(key, (static_cast<CGEventFlags>(m_Modifiers) & ~kLevel) | it->second.flags);
    post(key);

    if (down)
        m_HeldCharCodes.insert(it->second.code);
    else
        m_HeldCharCodes.erase(it->second.code);

    if (down && keyboardDiagnostics()) {
        const std::string label = usKeyLabel(it->second.code);
        log::info("[KBD] host '" + utf8 + "' -> key code " + std::to_string(it->second.code) +
                  modifierText(it->second.flags) + " on " + m_CharMapSource +
                  " | Notepad: OK | Game: OK real key, US '" + label + "'");
    }
}

void CgInput::injectText(const std::string& utf8)
{
    if (utf8.empty()) return;
    CFStringRef text =
        CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(utf8.data()),
                                static_cast<CFIndex>(utf8.size()), kCFStringEncodingUTF8, false);
    if (!text) return;
    const CFIndex length = CFStringGetLength(text);
    std::vector<UniChar> units(static_cast<size_t>(length));
    CFStringGetCharacters(text, CFRangeMake(0, length), units.data());
    CFRelease(text);

    // The character itself rather than a key: a soft keyboard's glyph needs
    // no key code to exist for it, and the host layout is irrelevant. One
    // event per UTF-16 unit, down then up, the way the window server wants
    // typed text delivered.
    for (UniChar unit : units) {
        CGEventRef down = CGEventCreateKeyboardEvent(nullptr, 0, true);
        CGEventKeyboardSetUnicodeString(down, 1, &unit);
        CGEventSetFlags(down, static_cast<CGEventFlags>(m_Modifiers));
        post(down);
        CGEventRef up = CGEventCreateKeyboardEvent(nullptr, 0, false);
        CGEventKeyboardSetUnicodeString(up, 1, &unit);
        CGEventSetFlags(up, static_cast<CGEventFlags>(m_Modifiers));
        post(up);
    }
}

void CgInput::moveTo(double x, double y, int deltaX, int deltaY)
{
    // Kept on the display: a pointer that wanders onto another screen from a
    // stream of this one is the classic remote-desktop surprise.
    if (m_Right > m_Left && m_Bottom > m_Top) {
        x = std::min(std::max(x, static_cast<double>(m_Left)), static_cast<double>(m_Right - 1));
        y = std::min(std::max(y, static_cast<double>(m_Top)), static_cast<double>(m_Bottom - 1));
    }
    m_X = x;
    m_Y = y;
    postPointer(x, y, deltaX, deltaY);
}

void CgInput::postPointer(double x, double y, int deltaX, int deltaY)
{
    // A move with a button held is a drag, and the event has to say so — the
    // window server does not infer it from the buttons it saw go down.
    CGEventType type = kCGEventMouseMoved;
    CGMouseButton button = kCGMouseButtonLeft;
    if (m_HeldButtons.count(1)) {
        type = kCGEventLeftMouseDragged;
    } else if (m_HeldButtons.count(3)) {
        type = kCGEventRightMouseDragged;
        button = kCGMouseButtonRight;
    } else if (!m_HeldButtons.empty()) {
        type = kCGEventOtherMouseDragged;
        button = cgButton(*m_HeldButtons.begin());
    }
    CGEventRef move = CGEventCreateMouseEvent(nullptr, type, CGPointMake(x, y), button);
    if (!move) return;
    // The deltas are what a game reading raw motion sees — with the pointer
    // decoupled from the cursor (a captured mouse) the position is ignored
    // and only these move the camera.
    CGEventSetIntegerValueField(move, kCGMouseEventDeltaX, deltaX);
    CGEventSetIntegerValueField(move, kCGMouseEventDeltaY, deltaY);
    CGEventSetFlags(move, static_cast<CGEventFlags>(m_Modifiers));
    post(move);
}

void CgInput::injectMouseMove(int deltaX, int deltaY)
{
    if (deltaX == 0 && deltaY == 0) return;

    // What arrives are the MOUSE'S OWN COUNTS — the client asks its browser
    // for unadjusted movement precisely so that nothing accelerates them
    // twice. Windows and Linux then hand them to the OS, which applies the
    // host's pointer speed on the way in; nothing posted on macOS gets it.
    // The acceleration lives in the HID driver, per device, and both doors
    // below enter after it — measured on macOS 15: a relative post moves the
    // pointer one point per count at every speed. Left alone, aiming through
    // the stream took several times the desk the Mac's own mouse does.
    //
    // So the curve is applied here, before the event exists, which is where
    // the driver would have applied it. m_Accel follows the host's tracking
    // speed and honours a host that has acceleration turned off by doing
    // nothing at all.
    int pointsX = deltaX;
    int pointsY = deltaY;
    m_Accel.apply(deltaX, deltaY, pointsX, pointsY);
    if (pointsX == 0 && pointsY == 0) return;

    if (m_HidSystem && postRelative(pointsX, pointsY)) return;

    syncPointer();
    moveTo(m_X + pointsX, m_Y + pointsY, pointsX, pointsY);
}

bool CgInput::postRelative(int deltaX, int deltaY)
{
    // Why not a Quartz event: a Quartz event carries a POSITION, and the
    // window server puts the pointer there, whatever the frontmost app asked.
    // A game that aims with the mouse — Roblox while the right button is held,
    // any first-person camera — detaches the pointer from the mouse
    // (CGAssociateMouseAndMouseCursorPosition) and reads motion only; on the
    // Mac the pointer then stands still, and through the stream it went on
    // wandering across the picture, because we kept telling it where to be.
    // Nothing reports whether the pointer is detached, not even privately.
    //
    // A relative post through IOHIDSystem is a mouse's own report: the window
    // server decides where the pointer goes, exactly as it does for the real
    // one. Measured against a frontmost app holding the pointer detached, a
    // position post moved it 50 points and this one 0, while the app still
    // received the motion, one point per count as before.
    //
    // Deprecated since 10.12 like every IOHIDSystem call, and nothing public
    // replaced it. If it ever stops answering, positions take over again.
    UInt32 type = NX_MOUSEMOVED;
    if (m_HeldButtons.count(1))
        type = NX_LMOUSEDRAGGED;
    else if (m_HeldButtons.count(3))
        type = NX_RMOUSEDRAGGED;
    else if (!m_HeldButtons.empty())
        type = NX_OMOUSEDRAGGED;
    // The event type is ours to give: with the buttons pressed through Quartz,
    // IOHIDSystem does not know one is held and would call a drag a move.

    NXEventData data;
    std::memset(&data, 0, sizeof data);
    data.mouseMove.dx = deltaX;
    data.mouseMove.dy = deltaY;
    const IOGPoint unused = {0, 0};
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    const kern_return_t posted =
        IOHIDPostEvent(m_HidSystem, type, unused, &data, kNXEventDataVersion,
                       static_cast<IOOptionBits>(m_Modifiers), kIOHIDSetRelativeCursorPosition);
#pragma clang diagnostic pop
    if (posted != KERN_SUCCESS) {
        log::warning("[native] input: IOHIDSystem refused a relative move (" +
                     std::to_string(posted) +
                     ") — relative motion is posted as positions from now on");
        IOServiceClose(m_HidSystem);
        m_HidSystem = 0;
        return false;
    }
    m_PointerStale = true;

    // Kept on the display, as moveTo does for positions. Where the pointer
    // went is the window server's call now, so it is read back; a pointer the
    // game holds still never leaves, and one that did is not detached, so
    // placing it back cannot fight a game.
    double hereX = 0;
    double hereY = 0;
    if (m_Right > m_Left && m_Bottom > m_Top && pointerLocation(hereX, hereY)) {
        const double x =
            std::clamp(hereX, static_cast<double>(m_Left), static_cast<double>(m_Right - 1));
        const double y =
            std::clamp(hereY, static_cast<double>(m_Top), static_cast<double>(m_Bottom - 1));
        if (x != hereX || y != hereY) moveTo(x, y, 0, 0);
    }
    return true;
}

void CgInput::syncPointer()
{
    if (!m_PointerStale) return;
    m_PointerStale = false;
    pointerLocation(m_X, m_Y);
}

void CgInput::injectMousePosition(const InputEvent& event)
{
    if (event.referenceWidth <= 0 || event.referenceHeight <= 0) return;
    if (m_Right <= m_Left || m_Bottom <= m_Top) return;
    syncPointer();
    double x =
        m_Left + static_cast<double>(event.positionX) * (m_Right - m_Left) / event.referenceWidth;
    double y =
        m_Top + static_cast<double>(event.positionY) * (m_Bottom - m_Top) / event.referenceHeight;
    // Clamped here and not only in moveTo: the detector compares what was
    // asked with what is found, and what was asked is the clamped point.
    x = std::min(std::max(x, static_cast<double>(m_Left)), static_cast<double>(m_Right - 1));
    y = std::min(std::max(y, static_cast<double>(m_Top)), static_cast<double>(m_Bottom - 1));

    // A game that keeps warping the pointer to the middle of its window reads
    // the mouse as the distance from there — placing the client's position
    // would hand it half a screen per frame. Found by looking where the
    // pointer is before each placement; see RecentreDetector for the whole
    // argument. From then on the client's motion goes in as deltas, exactly
    // as gaming mode would send them, until the game lets the pointer be.
    double hereX = 0;
    double hereY = 0;
    const bool haveHere = pointerLocation(hereX, hereY);
    const RecentreDetector::Verdict verdict =
        m_Recentre.observe(haveHere, std::llround(hereX), std::llround(hereY), std::llround(x),
                           std::llround(y), steadyNowUs());
    if (verdict.changed) {
        if (verdict.relative)
            log::info(RecentreDetector::enteredMessage(m_Recentre.anchorX(), m_Recentre.anchorY()));
        else
            log::info(RecentreDetector::leftMessage());
    }
    if (verdict.relative) {
        // A pointer that re-enters the picture far from where it left is a
        // jump the viewer did not make: no game should turn on it.
        if (RecentreDetector::isReentryJump(verdict.deltaX, verdict.deltaY, m_Right - m_Left))
            return;
        if (verdict.deltaX == 0 && verdict.deltaY == 0) return;
        // A Quartz event always carries a position, so the delta is applied
        // from where the pointer IS — the spot the game keeps it at — and
        // stamped as the delta too, which is what a game reading raw motion
        // looks at. Exactly the event a mouse moved from there would produce.
        const double fromX = haveHere ? hereX : m_X;
        const double fromY = haveHere ? hereY : m_Y;
        moveTo(fromX + verdict.deltaX, fromY + verdict.deltaY, static_cast<int>(verdict.deltaX),
               static_cast<int>(verdict.deltaY));
        return;
    }

    moveTo(x, y, static_cast<int>(x - m_X), static_cast<int>(y - m_Y));
}

void CgInput::injectMouseButton(int button, bool down)
{
    if (button < 1 || button > 5) return;
    // Never pressed twice without a release (see Win32Input: a trackpad tap
    // arrived as a double click through the heartbeat).
    if (down == (m_HeldButtons.count(button) != 0)) return;
    if (down)
        m_HeldButtons.insert(button);
    else
        m_HeldButtons.erase(button);
    // Pressed where the pointer IS: after relative moves only the window
    // server knows, and a click at a stale spot would move a detached pointer.
    syncPointer();
    if (!m_ClickTrace || !down) {
        postButton(button, down, m_X, m_Y);
        return;
    }
    const int64_t startUs = steadyNowUs();
    postButton(button, down, m_X, m_Y);
    m_ClickTrace->press(0, startUs, steadyNowUs());
}

void CgInput::postButton(int button, bool down, double x, double y)
{
    CGEventType type;
    switch (button) {
    case 1: type = down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp; break;
    case 3: type = down ? kCGEventRightMouseDown : kCGEventRightMouseUp; break;
    default: type = down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp; break;
    }
    // The click count macOS wants declared: a second press of the same button
    // within the interval is click 2, and its release repeats the count.
    if (down) {
        const int64_t now = steadyNowUs();
        if (button == m_LastButton && now - m_LastPressUs < kDoubleClickUs)
            m_ClickCount = std::min(m_ClickCount + 1, 3);
        else
            m_ClickCount = 1;
        m_LastButton = button;
        m_LastPressUs = now;
    }
    CGEventRef click = CGEventCreateMouseEvent(nullptr, type, CGPointMake(x, y), cgButton(button));
    if (!click) return;
    CGEventSetIntegerValueField(click, kCGMouseEventClickState,
                                m_ClickCount > 0 ? m_ClickCount : 1);
    CGEventSetFlags(click, static_cast<CGEventFlags>(m_Modifiers));
    post(click);
}

void CgInput::injectScroll(int amount, bool horizontal)
{
    if (amount == 0) return;
    // 120-unit notches from the relay, one line per notch here. Vertical
    // signs agree between Windows and Quartz (positive is away from the
    // user); horizontal is mirrored.
    const int32_t lines = amount / 120 != 0 ? amount / 120 : (amount > 0 ? 1 : -1);
    CGEventRef wheel =
        horizontal ? CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitLine, 2, 0, -lines)
                   : CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitLine, 1, lines);
    if (!wheel) return;
    CGEventSetFlags(wheel, static_cast<CGEventFlags>(m_Modifiers));
    post(wheel);
}

void CgInput::syncLockKeys(const InputEvent& event)
{
    // Only Caps Lock exists on a Mac. It is state, not a keystroke, and IOKit
    // sets it directly — a synthetic press would toggle a host that already
    // matched.
    io_service_t service =
        IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(kIOHIDSystemClass));
    if (!service) return;
    io_connect_t connect = 0;
    if (IOServiceOpen(service, mach_task_self(), kIOHIDParamConnectType, &connect) ==
        KERN_SUCCESS) {
        bool current = false;
        if (IOHIDGetModifierLockState(connect, kIOHIDCapsLockState, &current) == KERN_SUCCESS &&
            current != event.capsLock)
            IOHIDSetModifierLockState(connect, kIOHIDCapsLockState, event.capsLock);
        IOServiceClose(connect);
    }
    IOObjectRelease(service);
    if (event.capsLock)
        m_Modifiers |= kCGEventFlagMaskAlphaShift;
    else
        m_Modifiers &= ~static_cast<uint64_t>(kCGEventFlagMaskAlphaShift);
}

} // namespace mw::native::input
