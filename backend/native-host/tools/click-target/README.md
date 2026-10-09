# mw-click-target — the ideal game, for the click's way through the host

Plan « attente » (A1). On a click, this window draws the latency flag (the
blue, white and red bands of `backend/src/LatencyFlag.cpp`, at the same place on
its screen: 44-56 % across, the top 5 %) and presents at once. The client's
probe (`mwLatency.run()`) cannot tell it from the host's flag. What it measures
is then a real application's way: the click delivered to a window, a frame
rendered and presented, the compositor, the capture.

It is a lab instrument: built on demand, never installed, never shipped. Not to
be confused with `scripts/bench/click-target.ps1`, the small window the bench
parks the pointer on.

## What it draws, and on what

- **Windows**: D3D12, a flip-model swap chain (FLIP_DISCARD, three buffers, one
  frame of latency at most, tearing allowed when `--sync 0`). Nothing is drawn
  but cleared rectangles — the background, a grey bar that moves along the
  bottom so every frame is a new picture, and the flag's three bands. No
  shader, no pipeline state.
- **macOS**: Metal into a CAMetalLayer, two drawables, the display sync off
  with `--sync 0`. A render pass that only clears, then the bar and the bands
  copied from buffers filled once with each colour: no shader either. The click
  is the view's `mouseDown` (`downUs`; `eventUs` is the event's own stamp), the
  picture on the screen Metal's `presentedTime` of the first drawable shown at
  or after it (`exact` or `later`). On a `CGVirtualDisplay` Metal reports no
  presentation at all: `displayedUs` stays null there (09/10/2026), and the
  capture's display time is the one to read. `--space` goes full screen in a
  Space of its own; `--level screensaver|normal|shielding` sets the window's
  level (screensaver by default, normal with `--no-flag`). Started by
  `pass.py --host mw-mac` through launchd, on the screen named "Virtual
  Display".
- Linux (Vulkan on Wayland) is the plan's next step, with the same options and
  the same log.
- `--no-flag`, everywhere: a window that only takes the clicks, under the
  host's own flag, so that the bench's injected clicks land on it.

## Options

```
--display <n|name>   the screen: 0 the first (the primary), or \\.\DISPLAY5
--window             a window over the top 90% of the screen (always composed);
                     default: covering it (independent flip possible)
--sync 0|1           present at once (default) or on the refresh
--no-tearing         with --sync 0, wait for the refresh rather than tear
--on-click           draw only when the flag changes; default: continuously
--fps <n>            continuous frames a second at most, 0 none (default 240)
--react frame        a click waits for the next frame due; default: at once
--input-first        read the input before waiting for the swap chain, as a
                     simple loop does; default: after, as a game tuned for
                     latency does (with --sync 1 the click then waits a frame
                     less: 08/10, 6 ms before it was seen, then the wait)
--adapter <name>     the GPU, by a piece of its name, or warp; default: the
                     screen's own
--duration <s>       quit after that long, 1-7200 (default 600); Escape sooner
--out <file>         the log; default: stdout
```

## The log

One JSON object a line, all times on the steady clock MoonlightWeb stamps with
(QueryPerformanceCounter, µs), so it lines up with the host's click trace:

- `{"start", "screen", "size", "hz", "adapter", "mode", "sync", "tearing", ...}`
- each click: `downUs` (WM_LBUTTONDOWN), `renderUs`, `presentCallUs`,
  `presentUs` (Present returned), `presentId`, and `displayedUs`: the vblank
  the OS says that present reached the screen at (`GetFrameStatistics`,
  `SyncQPCTime`). `displayed` says how sure: `exact` (that very present),
  `passed` (the statistics had already moved past it), `unknown`.
- every 5 s: `fps` and `clicks`; at the end: `frames` and `clicks`.

Whether a present went through DWM or skipped it (independent flip, MPO) the
API does not say: run PresentMon beside it for that.

## On the bench

- The host's flag must not cover it: keep `latency_flag_enabled` on (the probe
  needs it), but tell the flag to stay off that screen with
  `MW_LATENCY_FLAG_SKIP=\\.\DISPLAYn` in the server's environment, or `*` for
  every screen (the virtual display's name changes each time it is made). On
  macOS the same variable takes `*`, a display id or a screen's name; the Mac
  host's own flag is not in ScreenCaptureKit's picture anyway (09/10/2026,
  `docs/design/click-waits.md` §8.2), so this tool is the Mac's marker.
- The host's pointer must be over the window: an injected click goes to the
  window under it, and the probe never moves it. The tool puts it there at
  start and brings it back every 250 ms if something took it away (a log
  line `{"cursor": "placed" | "brought back", "was": "x,y"}` each time).
- And nothing may sit over it there. The window is topmost: a window of a
  physical screen can land where the virtual display now is (on 09/10 an
  Explorer window took a pass's 60 clicks). Every 250 ms the tool also looks
  at the window under the pointer; when it is another one, it logs
  `{"covered": "<exe> <class>"}` and raises itself again, and logs
  `{"covered": "none"}` once it is on top.
- `scripts/bench/clickpath/hostpath.py <tag> --target <log>` splits the click
  with it (see that folder's README).

## Build

With the backend: `-DMW_BUILD_TOOLS=ON`. On its own:

```
cmake -G Ninja -S backend/native-host/tools/click-target -B build-click-target -DCMAKE_BUILD_TYPE=Release
cmake --build build-click-target
```
