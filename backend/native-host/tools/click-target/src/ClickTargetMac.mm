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

// macOS: Metal into a CAMetalLayer — what a game made for the Mac presents
// with. Nothing is drawn but the cleared background and copied rectangles (a
// bar that moves so every frame differs, the flag's three bands): a render
// pass that only clears, then a blit from small buffers filled once with each
// colour. No shader, no pipeline state.
//
// The click is the window's own mouseDown: `downUs` when it ran, `eventUs` the
// event's own timestamp (when the injector made it, in mach time like
// everything else here). When the picture reached the screen comes from the
// drawable's presentedTime (Metal's presented handler), on that clock too, so
// the log lines up with the host's click trace.
//
// Full screen is a borderless window covering the screen, above every other
// window (NSScreenSaverWindowLevel); --fullscreen-space puts it in a Space of
// its own instead, the way a game's full-screen mode does. Whether the window
// server then sends it to the screen without compositing it, the API does not
// say.

#include "ClickTarget.h"

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <CoreGraphics/CoreGraphics.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace clicktarget {
namespace {

/// Seconds of CACurrentMediaTime / NSEvent.timestamp / presentedTime (all
/// mach time) onto the steady clock, through "how long ago": the two tick
/// together, whatever their origins.
int64_t mediaToSteadyUs(double seconds)
{
    const double agoS = CACurrentMediaTime() - seconds;
    return steadyNowUs() - static_cast<int64_t>(agoS * 1e6);
}

std::string jsonText(const std::string& s)
{
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
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

/// A rectangle of the drawable, in pixels.
struct PixelRect
{
    int x = 0, y = 0, w = 0, h = 0;
};

class Renderer
{
public:
    std::string error;
    std::string deviceName;

    bool init(CAMetalLayer* layer, int width, int height, int syncInterval)
    {
        m_Layer = layer;
        m_Width = width;
        m_Height = height;
        m_Device = MTLCreateSystemDefaultDevice();
        if (!m_Device) return fail("no Metal device");
        deviceName = [[m_Device name] UTF8String] ?: "?";
        m_Queue = [m_Device newCommandQueue];
        if (!m_Queue) return fail("no command queue");
        layer.device = m_Device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        // The blit writes into the drawable itself.
        layer.framebufferOnly = NO;
        layer.drawableSize = CGSizeMake(width, height);
        layer.maximumDrawableCount = 2;
        layer.displaySyncEnabled = syncInterval != 0;
        layer.opaque = YES;
        // Device colours, as the host's flag paints: the probe classifies by
        // "clearly blue / white / red" after a chroma-subsampled encode.
        layer.colorspace = nil;
        return true;
    }

    /// Draw one frame — the flag in @p flag when it is up — and present it.
    /// @p onShown runs with the drawable's presentedTime (0: never shown).
    bool frame(const PixelRect* flag, int64_t& presentCallUs, int64_t& presentUs,
               uint64_t& presentId, void (^onShown)(uint64_t frame, double presentedTime))
    {
        id<CAMetalDrawable> drawable = [m_Layer nextDrawable];
        if (!drawable) return false;
        id<MTLTexture> target = drawable.texture;
        id<MTLCommandBuffer> cb = [m_Queue commandBuffer];

        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0.06, 0.06, 0.08, 1.0);
        [[cb renderCommandEncoderWithDescriptor:pass] endEncoding];

        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        // A bar along the bottom, a step further each frame: every present is
        // a new picture, as a game's is.
        const int barW = std::max(8, m_Width / 64);
        const int barH = std::max(2, m_Height / 40);
        const int x = static_cast<int>((m_Frames * 7) % static_cast<uint64_t>(m_Width - barW + 1));
        fill(blit, target, {x, m_Height - barH, barW, barH}, Colour::Grey);
        if (flag) {
            const int w = flag->w / 3;
            fill(blit, target, {flag->x, flag->y, w, flag->h}, Colour::Blue);
            fill(blit, target, {flag->x + w, flag->y, w, flag->h}, Colour::White);
            fill(blit, target, {flag->x + 2 * w, flag->y, flag->w - 2 * w, flag->h}, Colour::Red);
        }
        [blit endEncoding];

        const uint64_t frameId = ++m_Frames;
        [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
            onShown(frameId, shown.presentedTime);
        }];
        presentCallUs = steadyNowUs();
        [cb presentDrawable:drawable];
        [cb commit];
        presentUs = steadyNowUs();
        presentId = frameId;
        return true;
    }

    uint64_t frames() const { return m_Frames; }

private:
    enum class Colour
    {
        Grey,
        Blue,
        White,
        Red,
        Count
    };

    bool fail(const std::string& why)
    {
        error = why;
        return false;
    }

    /// A buffer of one colour, large enough for any rectangle up to @p w x @p
    /// h, made once. BGRA bytes.
    id<MTLBuffer> colourBuffer(Colour c, int w, int h)
    {
        const size_t i = static_cast<size_t>(c);
        const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
        if (m_Fill[i] && m_FillBytes[i] >= bytes && m_FillRow[i] >= static_cast<size_t>(w) * 4)
            return m_Fill[i];
        static const uint8_t bgra[4][4] = {
            {90, 90, 90, 255}, {255, 0, 0, 255}, {255, 255, 255, 255}, {0, 0, 255, 255}};
        m_Fill[i] = [m_Device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        auto* p = static_cast<uint8_t*>([m_Fill[i] contents]);
        for (size_t k = 0; k < bytes; k += 4)
            std::memcpy(p + k, bgra[i], 4);
        m_FillBytes[i] = bytes;
        m_FillRow[i] = static_cast<size_t>(w) * 4;
        return m_Fill[i];
    }

    void fill(id<MTLBlitCommandEncoder> blit, id<MTLTexture> target, PixelRect r, Colour c)
    {
        r.w = std::min(r.w, m_Width - r.x);
        r.h = std::min(r.h, m_Height - r.y);
        if (r.w <= 0 || r.h <= 0 || r.x < 0 || r.y < 0) return;
        id<MTLBuffer> source = colourBuffer(c, r.w, r.h);
        const size_t i = static_cast<size_t>(c);
        [blit copyFromBuffer:source
                   sourceOffset:0
              sourceBytesPerRow:m_FillRow[i]
            sourceBytesPerImage:m_FillRow[i] * static_cast<size_t>(r.h)
                     sourceSize:MTLSizeMake(static_cast<NSUInteger>(r.w),
                                            static_cast<NSUInteger>(r.h), 1)
                      toTexture:target
               destinationSlice:0
               destinationLevel:0
              destinationOrigin:MTLOriginMake(static_cast<NSUInteger>(r.x),
                                              static_cast<NSUInteger>(r.y), 0)];
    }

    CAMetalLayer* m_Layer = nil;
    id<MTLDevice> m_Device = nil;
    id<MTLCommandQueue> m_Queue = nil;
    id<MTLBuffer> m_Fill[static_cast<size_t>(Colour::Count)] = {};
    size_t m_FillBytes[static_cast<size_t>(Colour::Count)] = {};
    size_t m_FillRow[static_cast<size_t>(Colour::Count)] = {};
    int m_Width = 0;
    int m_Height = 0;
    uint64_t m_Frames = 0;
};

std::string clickJson(const PendingClick& c, int64_t displayedUs, const char* how)
{
    return "{\"click\":" + std::to_string(c.id) + ",\"downUs\":" + std::to_string(c.downUs) +
           ",\"eventUs\":" + std::to_string(c.eventUs) +
           ",\"renderUs\":" + std::to_string(c.renderUs) +
           ",\"presentCallUs\":" + std::to_string(c.presentCallUs) +
           ",\"presentUs\":" + std::to_string(c.presentUs) +
           ",\"presentId\":" + std::to_string(c.frame) +
           ",\"displayedUs\":" + (displayedUs ? std::to_string(displayedUs) : std::string("null")) +
           ",\"displayed\":\"" + how + "\"}";
}

/// Everything the window, the timer and the presented handlers share. The
/// handlers run on a Metal thread; the rest on the main thread.
struct State
{
    const Options* options = nullptr;
    Log* log = nullptr;
    Renderer renderer;
    NSWindow* window = nil;
    PixelRect flag;
    int64_t endUs = 0;
    int64_t flagUntilUs = 0;
    bool flagShown = false;
    int64_t nextFrameUs = 0;
    int64_t periodUs = 0;
    int clicks = 0;
    std::deque<PendingClick> pending;
    std::mutex mutex; ///< pending, and the log, against the presented handlers
    int64_t lastStatsUs = 0;
    uint64_t lastStatsFrames = 0;
    int64_t lastCursorUs = 0;
    NSInteger coveredBy = -1;
    bool quit = false;
};

State* g_State = nullptr;

void drawNow(int64_t now);

} // namespace
} // namespace clicktarget

// The view: a CAMetalLayer of its own, the click, and Escape.
@interface MWClickTargetView : NSView
@end

@implementation MWClickTargetView

- (BOOL)wantsUpdateLayer
{
    return YES;
}

- (CALayer*)makeBackingLayer
{
    return [CAMetalLayer layer];
}

- (BOOL)acceptsFirstMouse:(NSEvent*)event
{
    (void)event;
    // The first click of an inactive window is a click, not an activation.
    return YES;
}

- (BOOL)acceptsFirstResponder
{
    return YES;
}

- (void)mouseDown:(NSEvent*)event
{
    using namespace clicktarget;
    State* s = g_State;
    if (!s) return;
    if (!s->options->drawFlag) {
        // Only a place for the clicks to land: counted, nothing drawn.
        std::lock_guard<std::mutex> lock(s->mutex);
        ++s->clicks;
        return;
    }
    PendingClick c;
    c.downUs = steadyNowUs();
    c.eventUs = mediaToSteadyUs(event.timestamp);
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        c.id = ++s->clicks;
        s->pending.push_back(c);
    }
    s->flagUntilUs = c.downUs + int64_t(kFlagMs) * 1000;
    if (s->options->reactAtOnce) drawNow(steadyNowUs());
}

- (void)keyDown:(NSEvent*)event
{
    if (event.keyCode == 53 && clicktarget::g_State) clicktarget::g_State->quit = true; // Escape
}

- (void)resetCursorRects
{
    // No pointer over the picture, as the Windows window hides it.
    [self addCursorRect:[self bounds]
                 cursor:[[NSCursor alloc]
                            initWithImage:[[NSImage alloc] initWithSize:NSMakeSize(1, 1)]
                                  hotSpot:NSZeroPoint]];
}

@end

// Borderless windows refuse key status unless told otherwise; Escape needs it.
@interface MWClickTargetWindow : NSWindow
@end

@implementation MWClickTargetWindow
- (BOOL)canBecomeKeyWindow
{
    return YES;
}
- (BOOL)canBecomeMainWindow
{
    return YES;
}
@end

namespace clicktarget {
namespace {

void drawNow(int64_t now)
{
    State* s = g_State;
    const bool up = now < s->flagUntilUs;
    const int64_t renderUs = steadyNowUs();
    int64_t callUs = 0, doneUs = 0;
    uint64_t presentId = 0;
    const bool ok = s->renderer.frame(
        up ? &s->flag : nullptr, callUs, doneUs, presentId,
        ^(uint64_t frame, double presentedTime) {
            // A Metal thread: the click this frame carried, now known shown.
            const int64_t shownUs = presentedTime > 0 ? mediaToSteadyUs(presentedTime) : 0;
            std::lock_guard<std::mutex> lock(s->mutex);
            while (!s->pending.empty() && s->pending.front().presented &&
                   s->pending.front().frame <= frame) {
                const PendingClick& c = s->pending.front();
                if (c.frame == frame)
                    s->log->line(clickJson(c, shownUs, shownUs ? "exact" : "dropped"));
                else
                    s->log->line(clickJson(c, 0, "passed"));
                s->pending.pop_front();
            }
        });
    if (!ok) return;
    s->flagShown = up;
    if (!up) return;
    std::lock_guard<std::mutex> lock(s->mutex);
    for (PendingClick& c : s->pending) {
        if (c.presented) continue;
        c.presented = true;
        c.renderUs = renderUs;
        c.presentCallUs = callUs;
        c.presentUs = doneUs;
        c.frame = presentId;
    }
}

/// The probe clicks wherever the host's pointer is and never moves it: put
/// it on the window, and back whenever something takes it away. And say
/// whose window is under it when it is not this one (it takes the clicks).
void keepCursor(const char* why)
{
    State* s = g_State;
    const NSRect frame = [s->window frame];
    // AppKit's coordinates: origin at the bottom left of the main screen.
    const NSPoint p = [NSEvent mouseLocation];
    if (!NSPointInRect(p, frame)) {
        s->log->line("{\"cursor\":\"" + std::string(why) +
                     "\",\"at\":" + std::to_string(steadyNowUs()) + ",\"was\":\"" +
                     std::to_string(static_cast<int>(p.x)) + "," +
                     std::to_string(static_cast<int>(p.y)) + "\"}");
        // CoreGraphics' coordinates: origin at the TOP left of the main screen.
        const CGFloat mainHeight = [[[NSScreen screens] firstObject] frame].size.height;
        CGWarpMouseCursorPosition(CGPointMake(NSMidX(frame), mainHeight - NSMidY(frame)));
    }
    const NSInteger under = [NSWindow windowNumberAtPoint:[NSEvent mouseLocation]
                              belowWindowWithWindowNumber:0];
    const NSInteger other = under == [s->window windowNumber] ? 0 : under;
    if (other == s->coveredBy) return;
    s->coveredBy = other;
    std::string who = "none";
    if (other) {
        who = "window " + std::to_string(static_cast<long>(other));
        CFArrayRef info = CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow,
                                                     static_cast<CGWindowID>(other));
        if (info && CFArrayGetCount(info) > 0) {
            NSDictionary* d = (__bridge NSDictionary*)CFArrayGetValueAtIndex(info, 0);
            NSString* owner = d[(__bridge NSString*)kCGWindowOwnerName];
            NSString* name = d[(__bridge NSString*)kCGWindowName];
            who = std::string(owner ? [owner UTF8String] : "?") + " " +
                  (name ? [name UTF8String] : "");
        }
        if (info) CFRelease(info);
        [s->window orderFrontRegardless];
    }
    s->log->line("{\"covered\":\"" + jsonText(who) + "\",\"at\":" + std::to_string(steadyNowUs()) +
                 "}");
}

/// One turn of the main loop's timer: a frame when due, the pointer, the
/// counts, the end.
void tick()
{
    State* s = g_State;
    const int64_t now = steadyNowUs();
    if (s->quit || now >= s->endUs) {
        [NSApp stop:nil];
        // stop: takes effect after the next event: post one.
        [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                            location:NSZeroPoint
                                       modifierFlags:0
                                           timestamp:0
                                        windowNumber:0
                                             context:nil
                                             subtype:0
                                               data1:0
                                               data2:0]
                 atStart:YES];
        return;
    }
    const Options& o = *s->options;
    const bool flagChanged = (now < s->flagUntilUs) != s->flagShown;
    bool waiting = false;
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        for (const PendingClick& c : s->pending)
            if (!c.presented) waiting = true;
        // A click never presented (or never reported) for a second: said, dropped.
        while (!s->pending.empty() && now - s->pending.front().downUs > 1000000) {
            s->log->line(clickJson(s->pending.front(), 0, "unknown"));
            s->pending.pop_front();
        }
    }
    const bool due = o.continuous && now >= s->nextFrameUs;
    if (due || (waiting && !o.reactAtOnce) || (flagChanged && !o.continuous)) {
        drawNow(now);
        if (o.continuous)
            s->nextFrameUs = s->periodUs > 0 ? std::max(s->nextFrameUs + s->periodUs, now) : now;
    }
    if (now - s->lastCursorUs >= 250000) {
        keepCursor("brought back");
        s->lastCursorUs = now;
    }
    if (now - s->lastStatsUs >= 5000000) {
        const uint64_t f = s->renderer.frames();
        int clicks = 0;
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            clicks = s->clicks;
            s->log->line("{\"at\":" + std::to_string(now) + ",\"fps\":" +
                         std::to_string((f - s->lastStatsFrames) * 1000000 /
                                        static_cast<uint64_t>(now - s->lastStatsUs)) +
                         ",\"clicks\":" + std::to_string(clicks) + "}");
        }
        s->lastStatsUs = now;
        s->lastStatsFrames = f;
    }
}

NSScreen* pickScreen(const std::string& want)
{
    NSArray<NSScreen*>* screens = [NSScreen screens];
    if (screens.count == 0) return nil;
    if (want.empty()) return screens.firstObject;
    char* end = nullptr;
    const long n = std::strtol(want.c_str(), &end, 10);
    if (end && !*end) {
        // A small number is the position in the list, a large one a display id.
        if (n >= 0 && n < static_cast<long>(screens.count))
            return screens[static_cast<NSUInteger>(n)];
        for (NSScreen* s in screens) {
            NSNumber* number = [[s deviceDescription] objectForKey:@"NSScreenNumber"];
            if (number && number.unsignedLongValue == static_cast<unsigned long>(n)) return s;
        }
        return nil;
    }
    for (NSScreen* s in screens) {
        if (@available(macOS 10.15, *)) {
            const std::string name = [[s localizedName] UTF8String] ?: "";
            if (name.find(want) != std::string::npos) return s;
        }
    }
    return nil;
}

} // namespace

int run(const Options& o, Log& log)
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        // No Dock icon, no menu bar of its own; it still takes clicks and keys.
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        // Never napped, its timers never coalesced: a game in front is not.
        id<NSObject> activity = [[NSProcessInfo processInfo]
            beginActivityWithOptions:NSActivityLatencyCritical | NSActivityUserInitiated
                              reason:@"mw-click-target: the bench's click target"];

        NSScreen* screen = pickScreen(o.display);
        if (!screen) {
            std::fprintf(stderr, "mw-click-target: no screen \"%s\"\n", o.display.c_str());
            return 1;
        }
        const NSRect sf = [screen frame];
        const CGFloat scale = [screen backingScaleFactor];
        NSNumber* displayNumber = [[screen deviceDescription] objectForKey:@"NSScreenNumber"];
        const CGDirectDisplayID displayId = displayNumber ? displayNumber.unsignedIntValue : 0;
        std::string screenName = std::to_string(displayId);
        if (@available(macOS 10.15, *)) screenName = [[screen localizedName] UTF8String] ?: "";
        double hz = 0;
        if (CGDisplayModeRef mode = CGDisplayCopyDisplayMode(displayId)) {
            hz = CGDisplayModeGetRefreshRate(mode);
            CGDisplayModeRelease(mode);
        }
        if (@available(macOS 12.0, *)) {
            if (hz <= 0) hz = static_cast<double>(screen.maximumFramesPerSecond);
        }

        // A window over the top 90 %: never the whole screen, so always composed.
        NSRect wf = sf;
        if (!o.fullscreen) {
            wf.size.height = sf.size.height * 9 / 10;
            wf.origin.y = sf.origin.y + sf.size.height - wf.size.height;
        }
        MWClickTargetWindow* window =
            [[MWClickTargetWindow alloc] initWithContentRect:wf
                                                   styleMask:NSWindowStyleMaskBorderless
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO
                                                      screen:screen];
        // Above every ordinary window: a window that lands on this screen
        // would otherwise take the clicks (09/10 on Windows: 60 of them).
        [window setLevel:NSScreenSaverWindowLevel];
        [window setOpaque:YES];
        [window setHasShadow:NO];
        [window setReleasedWhenClosed:NO];
        [window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
        MWClickTargetView* view = [[MWClickTargetView alloc]
            initWithFrame:NSMakeRect(0, 0, wf.size.width, wf.size.height)];
        [view setWantsLayer:YES];
        [window setContentView:view];
        [window makeFirstResponder:view];

        State state;
        g_State = &state;
        state.options = &o;
        state.log = &log;
        state.window = window;
        const int width = static_cast<int>(wf.size.width * scale);
        const int height = static_cast<int>(wf.size.height * scale);
        CAMetalLayer* layer = (CAMetalLayer*)[view layer];
        layer.contentsScale = scale;
        if (!state.renderer.init(layer, width, height, o.syncInterval)) {
            std::fprintf(stderr, "mw-click-target: %s\n", state.renderer.error.c_str());
            g_State = nullptr;
            return 1;
        }
        // The flag at its fraction of the SCREEN, in the window's pixels
        // (Metal's origin is the top left, as the screen's top is the window's).
        const int sw = static_cast<int>(sf.size.width * scale);
        const int sh = static_cast<int>(sf.size.height * scale);
        state.flag = PixelRect{static_cast<int>(sw * kFlagLeft), static_cast<int>(sh * kFlagTop),
                               static_cast<int>(sw * (kFlagRight - kFlagLeft)),
                               static_cast<int>(sh * (kFlagBottom - kFlagTop))};

        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        if (o.fullscreenSpace) [window toggleFullScreen:nil];

        state.lastCursorUs = steadyNowUs();
        keepCursor("placed");

        log.line("{\"start\":" + std::to_string(steadyNowUs()) + ",\"screen\":\"" +
                 jsonText(screenName) + "\",\"display\":" + std::to_string(displayId) +
                 ",\"size\":\"" + std::to_string(sw) + "x" + std::to_string(sh) +
                 "\",\"hz\":" + std::to_string(static_cast<int>(hz + 0.5)) + ",\"adapter\":\"" +
                 jsonText(state.renderer.deviceName) + "\",\"mode\":\"" +
                 (o.fullscreenSpace ? "space" : (o.fullscreen ? "fullscreen" : "window")) +
                 "\",\"sync\":" + std::to_string(o.syncInterval) + ",\"continuous\":" +
                 (o.continuous ? "true" : "false") + ",\"fps\":" + std::to_string(o.fps) +
                 ",\"react\":\"" + (o.reactAtOnce ? "now" : "frame") +
                 "\",\"flag\":" + (o.drawFlag ? "true" : "false") + "}");

        state.periodUs = o.continuous && o.fps > 0 ? 1000000 / o.fps : 0;
        state.endUs = steadyNowUs() + int64_t(o.durationS) * 1000000;
        state.nextFrameUs = steadyNowUs();
        state.lastStatsUs = steadyNowUs();

        // A first picture, whatever the mode: the screen is ours from here.
        drawNow(steadyNowUs());

        // The main loop's own timer, at a millisecond: frames when due, the
        // pointer, the counts. The clicks come in between, as events, and a
        // click drawn at once does not wait for it.
        NSTimer* timer = [NSTimer timerWithTimeInterval:0.001
                                                repeats:YES
                                                  block:^(NSTimer* t) {
                                                      (void)t;
                                                      tick();
                                                  }];
        timer.tolerance = 0;
        [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
        [NSApp run];
        [timer invalidate];

        log.line("{\"end\":" + std::to_string(steadyNowUs()) +
                 ",\"clicks\":" + std::to_string(state.clicks) +
                 ",\"frames\":" + std::to_string(state.renderer.frames()) + "}");
        [window orderOut:nil];
        g_State = nullptr;
        [[NSProcessInfo processInfo] endActivity:activity];
    }
    return 0;
}

} // namespace clicktarget
