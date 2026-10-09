"""The click's way through the host, click by click (plan « attente », A0).

The click → flag measured 13 to 15 ms at 120 Hz between the host receiving the
click and the present of the picture that shows the flag. This splits it, all
on the host's steady clock, from a pass run with:

  - MW_NATIVE_TUNING (or native_tuning) clicktrace=1: the host writes
    click-trace-<pid>-<ms>.csv (each press handed to the OS, each wake-up of the
    capture with the OS's and DWM's stamps), copied as <tag>.click-trace.csv,
    and the relay logs each stamped input (received, handled);
  - MW_LATENCY_FLAG_TRACE=1 in the server's environment: the flag's log line
    has the hook's moment, and a second line when DWM composed it;
  - or, instead of the flag, mw-click-target's log (--target): the click as the
    application got it, its present, and when the OS says it reached the screen.

The same on a macOS host (plan « attente », AM0): CGEventPost for SendInput, the
flag's event tap for its hook (no DwmFlush: no `composed` leg for the flag),
ScreenCaptureKit's display time for the present, its callback for `deliver`,
and the captured display's CVDisplayLink for DWM's timing. mw-click-target's
`displayedUs` is then Metal's presentedTime.

Files of the pass, in bench-out/content-age: <tag>.server.log, <tag>.click-trace.csv,
and when the client's are there (<tag>.json, <tag>.clicks.frames.csv) they name
the frame that showed the flag; without them it is the first frame presented
after the flag was composed.

The legs, in ms:
  queue      received by the relay → SendInput called (a SYSTEM worker's hop)
  sendinput  the SendInput call — which returns only after every low-level
             mouse hook ran, the flag's included
  hook       SendInput called → the flag's hook ran    (app: → WM_LBUTTONDOWN)
  raise      hook → flag shown (its window painted)     (app: → Present returned)
  composed   shown → DWM composed it (DwmFlush returned) (app: → on the screen)
  present    shown → the present of the frame that showed it (LastPresentTime)
  deliver    that present → the OS handed the frame to the engine (macOS:
             ScreenCaptureKit's callback; Windows: empty, the same as handoff)
  handoff    that present → the capture handed the frame over
  host       received → handed over: the host's whole share
and: `vblank` where in the captured screen's refresh the flag went up (0: at a
refresh, 1: just before the next), `wait` shown → its next refresh, `late` how
many refreshes after that one its frame was presented, `between` frames
presented after the flag went up that did not carry it. The refresh grid comes
from the presents themselves: DWM's own timing follows another screen's clock.

Usage: hostpath.py <tag> [<tag>...] [--dir bench-out/content-age] [--target file] [--clicks] [--pool]
"""
import argparse
import bisect
import cmath
import csv
import json
import math
import os
import re
import statistics

FLAG = re.compile(r"\[LatencyFlag\] injected click.*?(?:hooked at steady (\d+) us, )?"
                  r"shown at steady (\d+) us")
FLAG_TRACE = re.compile(r"\[LatencyFlag\] trace: flushed at steady (\d+) us(?:.*?DWM vblank "
                        r"(\d+) us, period (\d+) us, composed (\d+) us)?")
RELAY = re.compile(r"click trace: input stamp (\d+) received at steady (\d+) us, handled at "
                   r"(\d+) us")
LEGS = ["queue", "sendinput", "hook", "raise", "composed", "present", "deliver", "handoff",
        "host", "vblank", "wait", "late", "between"]
RATES = (60, 75, 90, 100, 120, 144, 165, 180, 240, 360, 480, 500)


def num(v):
    try:
        return float(v) if v not in ("", None) else None
    except ValueError:
        return None


def q(xs, p):
    xs = sorted(x for x in xs if x is not None)
    return xs[min(len(xs) - 1, int(p * len(xs)))] if xs else None


def fmt(v):
    return "   -  " if v is None else "%6.2f" % v


def read_flags(log_path):
    """Each flag: {hook, shown, flushed, vblank, period, composed} in µs."""
    flags = []
    with open(log_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = FLAG.search(line)
            if m:
                flags.append({"hook": num(m.group(1)), "shown": float(m.group(2))})
                continue
            m = FLAG_TRACE.search(line)
            if m and flags and "flushed" not in flags[-1]:
                flags[-1].update(flushed=float(m.group(1)), vblank=num(m.group(2)),
                                 period=num(m.group(3)), composed=num(m.group(4)))
    return flags


def read_target(path):
    """mw-click-target's clicks, as flags: the app's own moments."""
    flags = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            try:
                j = json.loads(line)
            except ValueError:
                continue
            if "click" in j:
                flags.append({"hook": j["downUs"], "shown": j["presentUs"],
                              "flushed": j.get("displayedUs")})
    return flags


def read_relay(log_path):
    out = []
    with open(log_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = RELAY.search(line)
            if m:
                out.append((float(m.group(2)), float(m.group(3))))
    return sorted(out)


def client_flag_frames(base, from_draw=False):
    """Per click the client measured, the host stamp (ms) of the frame that
    showed the flag, and the click's host time estimated (µs). @p from_draw:
    the probe dated the flag from its frame's draw (since 652fc726)."""
    if not (os.path.exists(base + ".json") and os.path.exists(base + ".clicks.frames.csv")):
        return [], []
    j = json.load(open(base + ".json"))
    c = j.get("clicks") or {}
    org = c.get("timeOrigin")
    fr = []
    for r in csv.DictReader(open(base + ".clicks.frames.csv")):
        r = {k: num(v) for k, v in r.items()}
        if r.get("drawnMs") is not None and r.get("hostMs") is not None:
            fr.append(r)
    if not fr or org is None:
        return [], []
    fr.sort(key=lambda r: r["drawnMs"])
    drawn = [r["drawnMs"] for r in fr]
    off = statistics.median(r["hostMs"] - r["captureMs"] for r in fr)
    out = []
    # The client's stamp is the present less a constant of the session (the
    # first frame's sub-millisecond, cut off), then truncated to the ms: on
    # the host's clock, each frame's present lies in [ms + x, ms + x + 1000)
    # µs, x found below against the trace.
    for s in c.get("samples") or []:
        if not s.get("ok"):
            continue
        ck = s["ts"] / 1000 - org
        seen = ck + s["latencyMs"]
        i = bisect.bisect_right(drawn, seen) - 1
        # Since 652fc726 the probe dates the flag from its frame's draw, before
        # the renderer reads it back, while drawnMs is logged after that read:
        # the click is then seen inside the draw of the frame that showed it
        # (decoded <= seen < drawn), the one after the last drawn before it.
        # Not for a pass before: its flag was seen just after its own frame's
        # drawnMs, and the next frame may already be decoded by then.
        nxt = fr[i + 1] if from_draw and i + 1 < len(fr) else None
        if nxt and nxt.get("decodedMs") is not None and nxt["decodedMs"] - 0.05 <= seen < nxt["drawnMs"]:
            i += 1
        if i < 0:
            continue
        out.append({"frameMs": fr[i]["hostMs"], "clickUs": (ck + off) * 1000})
    return out, [r["hostMs"] for r in fr]


def refresh_grid(present):
    """The captured screen's refresh, from its presents: the rate (of RATES)
    whose phases gather most, then its period fitted and its phase. DWM's own
    timing cannot say it: on 08/10/2026 it gave 144 Hz, another screen's clock,
    while the virtual display presented on a 120 Hz grid."""
    if len(present) < 100:
        return None
    best = None
    for hz in RATES:
        per = 1e6 / hz
        v = sum(cmath.exp(2j * math.pi * (p % per) / per) for p in present) / len(present)
        if best is None or abs(v) > best[0]:
            best = (abs(v), per)
    per = best[1]
    # The period refined from end to end, the phase from the mean of the phases.
    n = round((present[-1] - present[0]) / per)
    if n > 0:
        cands = [per * (1 + e / 1e4) for e in range(-20, 21)]
        per = max(cands, key=lambda c: abs(sum(cmath.exp(2j * math.pi * (p % c) / c)
                                                for p in present[::5])))
    v = sum(cmath.exp(2j * math.pi * (p % per) / per) for p in present) / len(present)
    phase = (cmath.phase(v) / (2 * math.pi)) % 1.0 * per
    return per, phase, abs(v)


def stamp_offset(client_ms, present):
    """x of client_flag_frames, in µs: the one that puts a present in
    [stamp + x, stamp + x + 1 ms) for the most frames the client logged. The
    least distance to the nearest present, used before, held while the presents
    were 7-8 ms apart; at 240 Hz (09/10/2026) a present 4 ms apart came closer
    than the frame's own and gave -1.5 ms instead of 0.65."""
    sample = client_ms[::3]
    best = (0, 0)
    for x in range(-3000, 3000, 25):
        hit = 0
        for h in sample:
            lo = h * 1000 + x
            j = bisect.bisect_left(present, lo)
            if j < len(present) and present[j] < lo + 1000:
                hit += 1
        best = max(best, (hit, -x))
    return -best[1]


def one(tag, a):
    base = os.path.join(a.dir, tag)
    trace = list(csv.DictReader(open(base + ".click-trace.csv")))
    presses = sorted((num(r["startUs"]), num(r["us"])) for r in trace if r["kind"] == "press")
    caps = [{k: num(v) for k, v in r.items() if k not in ("kind", "status")}
            for r in trace if r["kind"] == "capture" and r["status"] == "ok"]
    caps.sort(key=lambda r: r["presentUs"])
    present = [r["presentUs"] for r in caps]
    log = base + ".server.log"
    flags = read_target(a.target) if a.target else read_flags(log)
    relay = read_relay(log) if os.path.exists(log) else []
    seen, client_ms = client_flag_frames(base, a.from_draw)
    seen.sort(key=lambda s: s["frameMs"])
    x = stamp_offset(client_ms, present)
    grid = refresh_grid(present)
    loose = None
    if grid and grid[2] < 0.5:
        # Presents on no grid: on 08/10/2026 an application drawing at 240 fps
        # on the virtual display had its frames handed over every ~7 ms, give
        # or take half a millisecond, on no refresh of the screen's own. There
        # is then no refresh to place the flag in.
        loose, grid = grid, None
    seen_frames = [s["frameMs"] for s in seen]

    rows = []
    for fl in flags:
        s_us = fl["shown"]
        hook = fl.get("hook") or None
        # The press that raised it: the last SendInput begun before the hook
        # (before the flag, when the hook was not logged), within 50 ms. Not
        # the last one ended: SendInput returns only once every low-level
        # hook has run, the flag's among them.
        ref = hook or s_us
        i = bisect.bisect_right([p[0] for p in presses], ref) - 1
        press = presses[i] if i >= 0 and ref - presses[i][0] < 50000 else None
        rel = None
        if press:
            for recv, done in relay:
                if recv <= press[0] <= done + 1:
                    rel = (recv, done)
        # The frame that showed it: the client's, when the pass has it (its
        # stamp is the present less under a millisecond), else the first
        # presented after the flag was composed (or shown).
        frame = None
        k = bisect.bisect_left(seen_frames, s_us / 1000 - 1)
        if k < len(seen) and seen_frames[k] * 1000 - s_us < 200000:
            lo = seen_frames[k] * 1000 + x - 50
            j = bisect.bisect_left(present, lo)
            if j < len(caps) and caps[j]["presentUs"] < lo + 1100:
                frame = caps[j]
        if frame is None and not seen:
            after = fl.get("flushed") or s_us
            j = bisect.bisect_left(present, after)
            if j < len(caps):
                frame = caps[j]
        row = {k: None for k in LEGS}
        if press:
            row["sendinput"] = (press[1] - press[0]) / 1000
            if hook:
                row["hook"] = (hook - press[0]) / 1000
        if rel and press:
            row["queue"] = (press[0] - rel[0]) / 1000
        if hook:
            row["raise"] = (s_us - hook) / 1000
        if fl.get("flushed"):
            row["composed"] = (fl["flushed"] - s_us) / 1000
        if frame:
            row["present"] = (frame["presentUs"] - s_us) / 1000
            if frame.get("deliveredUs"):
                row["deliver"] = (frame["deliveredUs"] - frame["presentUs"]) / 1000
            row["handoff"] = (frame["us"] - frame["presentUs"]) / 1000
            if rel:
                row["host"] = (frame["us"] - rel[0]) / 1000
            j0 = bisect.bisect_right(present, s_us)
            j1 = bisect.bisect_left(present, frame["presentUs"])
            row["between"] = max(0, j1 - j0)
        # Where in the captured screen's refresh the flag went up, how long to
        # its next refresh, and how many refreshes after that one its frame
        # was presented (0: the very next).
        if grid:
            per, ph, _ = grid
            phase = ((s_us - ph) % per) / per
            row["vblank"] = phase
            row["wait"] = (1 - phase) * per / 1000
            if frame:
                row["late"] = round((frame["presentUs"] - (s_us + (1 - phase) * per)) / per)
        rows.append(row)

    gaps = sorted(b - a for a, b in zip(present, present[1:]) if b > a)
    if grid:
        where = ", refresh %.3f Hz (presents on its grid: %.2f)" % (1e6 / grid[0], grid[2])
    elif loose and gaps:
        where = (", presents on no refresh grid (%.1f a second, median gap %.2f ms; the best"
                 " grid, %.0f Hz, holds %.2f)" % (
                     len(present) * 1e6 / (present[-1] - present[0]), gaps[len(gaps) // 2] / 1000,
                     1e6 / loose[0], loose[2]))
    else:
        where = ""
    print("%s: %d flags, %d presses, %d frames, %d client clicks (stamp offset %d us)%s" % (
        tag, len(flags), len(presses), len(caps), len(seen), x, where))
    summary(rows, a.clicks)
    return rows


def summary(rows, every=False):
    if every:
        print("  " + " ".join("%9s" % k for k in LEGS))
        for r in rows:
            print("  " + " ".join("%9s" % fmt(r[k]).strip() for k in LEGS))
    for label, p in (("p50", .5), ("p90", .9)):
        print("  %-4s " % label + " ".join("%s=%s" % (k, fmt(q([r[k] for r in rows], p)).strip())
                                          for k in LEGS))
    print("  mean " + " ".join(
        "%s=%s" % (k, fmt(statistics.mean(v)).strip() if v else "-")
        for k, v in ((k, [r[k] for r in rows if r[k] is not None]) for k in LEGS)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tags", nargs="+")
    ap.add_argument("--dir", default=os.path.join("bench-out", "content-age"))
    ap.add_argument("--target", help="mw-click-target's log, for its clicks instead of the flag's")
    ap.add_argument("--clicks", action="store_true", help="every click, not only the summary")
    ap.add_argument("--pool", action="store_true", help="all the tags' clicks in one summary too")
    ap.add_argument("--from-draw", action="store_true",
                    help="the client's probe dated the flag from the draw (passes since 652fc726)")
    a = ap.parse_args()
    pooled = []
    for t in a.tags:
        pooled += one(t, a)
    if a.pool and len(a.tags) > 1:
        print("pooled, %d clicks:" % len(pooled))
        summary(pooled)


if __name__ == "__main__":
    main()
