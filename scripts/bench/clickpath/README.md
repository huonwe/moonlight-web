# clickpath — the click's two waits outside the codec (plan « attente »)

The bench of 06/10/2026 (120 fps, the RTX encoding, a 2560×1440 virtual display
at 120 Hz, the UM790Pro on the cable) split a click into stages. Two of them
dominate and nothing targeted them:

- **A. Host → captured picture, 13-15 ms**: from the host receiving the click
  to the present of the picture that shows the flag. Every codec pays it.
- **B. The page's GPU wait in PyroWave's WebGPU decode, ~4.5 ms of 6.3**: the
  GPU works ~1.8 ms of decode and ~0.3 ms of display.

Two scripts split each one further. Both read a content-age pass
(`scripts/bench/content-age/pass.py`, through `local_matrix.py` or
`wifi/series.py`) in `bench-out/content-age/`.

## A — hostpath.py

What the pass needs:

- `clicktrace=1` in `MW_NATIVE_TUNING` (or `native_tuning`). The host's
  engine then writes `click-trace-<pid>-<ms>.csv` next to the worker's log when
  the session ends (copied as `<tag>.click-trace.csv`):
  - each press handed to the OS (`SendInput`, and when it was queued for a
    SYSTEM worker's follower thread);
  - each wake-up of the capture: when it started waiting, when it returned and
    with what; for a frame, its present (DDA's `LastPresentTime`, WGC's
    `SystemRelativeTime` before the clamp), DDA's `LastMouseUpdateTime` and
    `AccumulatedFrames`; then DWM's timing (`DwmGetCompositionTimingInfo`: last
    vblank, period, last composition, frame count).

  The relay also logs each stamped input (`click trace: input stamp …
  received at …, handled at …`).
- `MW_LATENCY_FLAG_TRACE=1` in the server's environment (read when the flag
  arms, at startup): the flag's line gains the hook's moment, and after each
  flag the overlay waits for DWM's next composition (`DwmFlush`) and logs it
  with DWM's timing. It blocks the overlay's thread for at most a frame.
- To compare DDA and WGC on the same bench: `MW_CAPTURE=wgc` in the worker's
  environment (DDA is the default).

```
python scripts/bench/clickpath/hostpath.py <tag> [--clicks]
python scripts/bench/clickpath/hostpath.py <tag> --target <mw-click-target log>
```

The legs, all on the host's clock: `queue` (relay → SendInput), `sendinput`,
`hook` (→ the flag's hook, or the application's WM_LBUTTONDOWN), `raise` (→
flag painted, or Present returned), `composed` (→ DWM composed it, or the OS
says it reached the screen), `present` (→ the present of the frame that showed
it), `handoff` (that present → the capture handed it over), `host` (the
host's whole share). And `vblank` (where in DWM's refresh the flag went up),
`wait` (to the next vblank), `between` (frames presented after the flag that
did not carry it).

The frame that showed the flag is the client's, when the pass has
`<tag>.json` and `<tag>.clicks.frames.csv`; otherwise the first presented after
the flag was composed.

## A, every frame — presentmon.py

`hostpath.py` follows the click's frame. `presentmon.py` follows all the frames
of an application or a game, from its present to the capture, from PresentMon
run beside the pass:

```
PresentMon-x64.exe --process_name mw-click-target.exe --process_name re9.exe ^
  --output_file bench-out\content-age\<tag>.presentmon.csv --qpc_time --no_console_stats ^
  --session_name mw-attente --stop_existing_session --timed 900 --terminate_after_timed
PresentMon-x64.exe --session_name mw-attente --terminate_existing_session   (at the end)
python scripts/bench/clickpath/presentmon.py <tag> [--target <mw-click-target log>]
```

PresentMon 2 (AMD's build ships with its driver, in `C:\Program Files\AMD\CNext\CNext`)
traces without elevation for a member of « Performance Log Users ». The pass
needs `clicktrace=1` too: DDA's `LastPresentTime` of every picture. Per
process: frames a second, present modes, frames never shown, then per frame
`shown` (present → on the screen, by PresentMon), `dda` (that moment → DDA's
present for the picture that carried it: the two agree when ~0), `handoff`,
`total` (present → handed over), and the compositions' rhythm. A game driven
beside the pass writes `<tag>.re9.txt`, whose `scene from here` and `scene to
here` lines bound the frames counted.

## B — gpuwait.py

What the pass needs: `localStorage.mw_ultra_trace = '1'` in the client's page
before the stream starts. `UltraPlayer` then keeps every frame's timeline
(`__mwUltraPlayer.trace`): in, decode starts, submitted, work done, VideoFrame
made, and the GPU's own timestamps around the decode and the present passes on
every frame; and after one frame in eight, when nothing waits, an empty
timestamped pass, the reference for Chrome's own submit → work done. `pass.py`
writes it as `<tag>.ultratrace.json`.

Chrome rounds timestamp queries to 100 µs unless its bench profile runs with
`--enable-webgpu-developer-features`; the script says when it sees rounded
values.

```
python scripts/bench/clickpath/gpuwait.py <tag>
```

The GPU's clock is put on the page's from what the two cannot break: the GPU
starts no earlier than the submit and ends no later than the work-done
callback. Over each 2 s window, frames and references together, that bounds
the offset; both bounds are printed, and the split of submit → work done into
before the GPU starts, the GPU at work, and after it ends.
