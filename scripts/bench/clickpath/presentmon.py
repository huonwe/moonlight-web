"""Every frame of an application, from its present to the capture (plan « attente », AW2).

hostpath.py follows the click's frame only. This follows all of them, from a
PresentMon capture taken beside the pass (`<tag>.presentmon.csv`, PresentMon 2
with --qpc_time) and the host's click trace (`<tag>.click-trace.csv`, a pass with
clicktrace=1). Both are on QueryPerformanceCounter, the host's steady clock.

For each process PresentMon saw (mw-click-target, a game):
  - its frames a second, its present modes (composed by DWM, or flipped on
    their own) and how many frames never reached the screen;
  - per frame, in ms:
      shown     its present → the moment PresentMon says it reached the screen
                (the composition that carried it, on a virtual display)
      dda       that moment → the present DDA reported for the picture that
                carried it (LastPresentTime), the two clocks' agreement
      handoff   DDA's present → the capture handed the picture over
      total     the frame's present → handed over
      next      the frame's present → the first present DDA reported after it,
                for frames PresentMon could not place on the screen;
  - the compositions' own rhythm: the gaps between the moments frames reached
    the screen.
With --target, mw-click-target's clicks are looked up among the frames, which
also checks the two logs against each other. A game driven beside the pass
(<tag>.re9.txt, lines "steady=<µs> scene from here" and "... scene to here")
limits the frames to its scene; --since and --until do it by hand.

The present is PresentMon's CPU start plus its CPU busy time, the moment the
application called Present.

Usage: presentmon.py <tag> [<tag>...] [--dir bench-out/content-age] [--process name]
                     [--target file] [--since us] [--until us] [--qpc-freq hz]
"""
import argparse
import bisect
import csv
import json
import os
import re
import statistics
import sys


def qpc_frequency():
    if sys.platform == "win32":
        import ctypes
        f = ctypes.c_int64()
        if ctypes.windll.kernel32.QueryPerformanceFrequency(ctypes.byref(f)) and f.value:
            return f.value
    return 10_000_000


def num(v):
    try:
        return float(v) if v not in ("", None, "NA") else None
    except ValueError:
        return None


def q(xs, p):
    xs = sorted(x for x in xs if x is not None)
    return xs[min(len(xs) - 1, int(p * len(xs)))] if xs else None


def row(name, xs):
    xs = [x for x in xs if x is not None]
    if not xs:
        return "    %-9s n=0" % name
    return "    %-9s p10 %7.2f  p50 %7.2f  p90 %7.2f  mean %7.2f  (n %d)" % (
        name, q(xs, .1), q(xs, .5), q(xs, .9), statistics.mean(xs), len(xs))


def read_frames(path, freq):
    """PresentMon's rows, by application: present and shown in µs."""
    apps = {}
    with open(path, newline="", encoding="utf-8-sig", errors="replace") as f:
        for r in csv.DictReader(f):
            start = num(r.get("CPUStartQPC"))
            if start is None:
                continue
            start_us = start * 1e6 / freq
            busy = num(r.get("CPUBusy")) or 0.0
            shown = num(r.get("DisplayLatency"))
            apps.setdefault(r["Application"], []).append({
                "present": start_us + busy * 1000,
                "shown": start_us + shown * 1000 if shown is not None else None,
                "mode": r.get("PresentMode", "?"),
                "tearing": r.get("AllowsTearing"),
                "sync": r.get("SyncInterval"),
                "click": num(r.get("ClickToPhotonLatency")),
            })
    for frames in apps.values():
        frames.sort(key=lambda fr: fr["present"])
    return apps


def read_captures(path):
    caps = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            if r["kind"] == "capture" and r["status"] == "ok" and num(r["presentUs"]):
                caps.append((num(r["presentUs"]), num(r["us"])))
    caps.sort()
    return caps


def read_clicks(path):
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            try:
                j = json.loads(line)
            except ValueError:
                continue
            if "click" in j:
                out.append(j)
    return out


def scene(base):
    """The game's scene, from the script that drove it: (from, to) in µs."""
    lo = hi = None
    if os.path.exists(base + ".re9.txt"):
        for line in open(base + ".re9.txt", encoding="utf-8", errors="replace"):
            m = re.search(r"steady=(\d+) scene (from|to) here", line)
            if m and m.group(2) == "from":
                lo = float(m.group(1))
            elif m:
                hi = float(m.group(1))
    return lo, hi


def one(tag, a, freq):
    base = os.path.join(a.dir, tag)
    apps = read_frames(base + a.suffix, freq)
    caps = read_captures(base + ".click-trace.csv")
    lo, hi = scene(base)
    lo = a.since or lo
    hi = a.until or hi
    if lo or hi:
        for k in apps:
            apps[k] = [fr for fr in apps[k] if (lo or 0) <= fr["present"] <= (hi or float("inf"))]
        apps = {k: v for k, v in apps.items() if len(v) > 1}
    dda = [c[0] for c in caps]
    print("%s: %d captures, %s%s" % (tag, len(caps), ", ".join(
        "%s %d frames" % (k, len(v)) for k, v in sorted(apps.items())),
        " (from %.0f to %s us)" % (lo or 0, "%.0f" % hi if hi else "the end") if lo or hi else ""))
    for app, frames in sorted(apps.items()):
        if a.process and app.lower() not in [p.lower() for p in a.process]:
            continue
        span = (frames[-1]["present"] - frames[0]["present"]) / 1e6
        modes = {}
        for fr in frames:
            modes[fr["mode"]] = modes.get(fr["mode"], 0) + 1
        lost = sum(1 for fr in frames if fr["shown"] is None)
        print("  %s: %.1f frames a second, %s; %d of %d never shown; tearing %s, sync %s" % (
            app, len(frames) / span if span > 0 else 0,
            ", ".join("%s %d" % kv for kv in sorted(modes.items(), key=lambda kv: -kv[1])),
            lost, len(frames), frames[-1]["tearing"], frames[-1]["sync"]))
        legs = {k: [] for k in ("shown", "dda", "handoff", "total", "next")}
        for fr in frames:
            # Only while the capture ran: its first and last pictures bound it.
            if not caps or not dda[0] <= fr["present"] <= dda[-1]:
                continue
            j = bisect.bisect_left(dda, fr["present"])
            if j < len(caps):
                legs["next"].append((caps[j][0] - fr["present"]) / 1000)
            if fr["shown"] is None:
                continue
            legs["shown"].append((fr["shown"] - fr["present"]) / 1000)
            # The picture that carried it: the DDA present nearest its moment on
            # the screen, within 2 ms.
            k = bisect.bisect_left(dda, fr["shown"] - 2000)
            near = [i for i in range(k, min(k + 3, len(caps))) if abs(caps[i][0] - fr["shown"]) <= 2000]
            if not near:
                continue
            i = min(near, key=lambda i: abs(caps[i][0] - fr["shown"]))
            legs["dda"].append((caps[i][0] - fr["shown"]) / 1000)
            legs["handoff"].append((caps[i][1] - caps[i][0]) / 1000)
            legs["total"].append((caps[i][1] - fr["present"]) / 1000)
        for k in ("shown", "dda", "handoff", "total", "next"):
            print(row(k, legs[k]))
        shown = sorted({round(fr["shown"]) for fr in frames if fr["shown"] is not None})
        gaps = [(b - c) / 1000 for c, b in zip(shown, shown[1:]) if b > c]
        if gaps:
            print("    compositions carrying its frames: %.1f a second, gap p10 %.2f p50 %.2f p90 %.2f ms" % (
                len(shown) / ((shown[-1] - shown[0]) / 1e6), q(gaps, .1), q(gaps, .5), q(gaps, .9)))
        clicks = [fr["click"] for fr in frames if fr["click"] is not None]
        if clicks:
            print(row("click-ph", clicks))
        if a.target and "target" in app.lower():
            presents = [fr["present"] for fr in frames]
            off, seen = [], []
            for c in read_clicks(a.target):
                j = bisect.bisect_left(presents, c["presentCallUs"] - 3000)
                near = [i for i in range(j, min(j + 6, len(frames)))
                        if abs(presents[i] - c["presentCallUs"]) <= 3000]
                if not near:
                    continue
                i = min(near, key=lambda i: abs(presents[i] - c["presentCallUs"]))
                off.append((presents[i] - c["presentCallUs"]) / 1000)
                if frames[i]["shown"] is not None:
                    seen.append((frames[i]["shown"] - c["presentCallUs"]) / 1000)
            print("    clicks found among its frames: %d (PresentMon's present less the tool's: p50 %s ms)"
                  % (len(off), "%.3f" % q(off, .5) if off else "-"))
            print(row("click-sh", seen))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tags", nargs="+")
    ap.add_argument("--dir", default=os.path.join("bench-out", "content-age"))
    ap.add_argument("--process", action="append", help="only this application (repeatable)")
    ap.add_argument("--target", help="mw-click-target's log, to find its clicks among the frames")
    ap.add_argument("--suffix", default=".presentmon.csv",
                    help="the capture's file, after the tag (a second one: .presentmon-nodisplay.csv)")
    ap.add_argument("--since", type=float, default=0, help="frames presented from then on (µs)")
    ap.add_argument("--until", type=float, default=0, help="frames presented until then (µs)")
    ap.add_argument("--qpc-freq", type=int, default=0, help="QPC ticks a second (default: this machine's)")
    a = ap.parse_args()
    freq = a.qpc_freq or qpc_frequency()
    for t in a.tags:
        one(t, a, freq)


if __name__ == "__main__":
    main()
