"""The click split at the capture, on the client's clock (plan « attente »).

For each click of a pass (content-age/pass.py --clicks, the probe since
652fc726, which dates the flag from the draw of the frame that showed it): the
frame that showed the flag is the one whose draw holds the moment the flag was
seen. The click then splits in two:

  click -> capture   the way up and the host's whole share, up to the capture
                     of that frame (its capture stamp, carried to the client's
                     clock by the stream's clock sync)
  capture -> drawn   the encode, the way down, the decode and the draw

Means add up to the click's mean; medians do not. `decode wait` is how long that
frame waited between its arrival and its decode. Every pattern is a glob of
tags in bench-out/content-age, its clicks pooled:

    python scripts/bench/clickpath/clicksplit.py <tag or glob> [...]

The splits of design §8.3 and §9 come from it, and §6.1's click -> capture
(click-waits.md; §6.1's capture -> screen is the frames outside the probe's).
"""
import csv
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "..", "bench-out", "content-age")


def q(xs, p):
    xs = sorted(xs)
    return xs[int(p * (len(xs) - 1))] if xs else float("nan")


def mean(xs):
    return sum(xs) / len(xs) if xs else float("nan")


def split(pattern):
    up, down, clicks, wait = [], [], [], []
    for path in sorted(glob.glob(os.path.join(OUT, pattern + ".json"))):
        base = path[:-5]
        c = (json.load(open(path)) or {}).get("clicks")
        frames_csv = base + ".clicks.frames.csv"
        if not c or not os.path.exists(frames_csv):
            continue
        origin = c["timeOrigin"]
        frames = []
        for r in csv.DictReader(open(frames_csv)):
            try:
                frames.append({k: float(v) for k, v in r.items() if v != ""})
            except ValueError:
                pass
        for s in c["samples"]:
            if not s.get("ok"):
                continue
            click = s["ts"] / 1000 - origin
            seen = click + s["latencyMs"]
            shown = [f for f in frames
                     if "drawnMs" in f and "captureMs" in f and "decodedMs" in f
                     and f["decodedMs"] - 0.05 <= seen < f["drawnMs"]]
            if not shown:
                continue
            f = shown[0]
            up.append(f["captureMs"] - click)
            down.append(seen - f["captureMs"])
            clicks.append(s["latencyMs"])
            if "arrivedMs" in f:
                wait.append(f["decodedMs"] - f["arrivedMs"])
    return up, down, clicks, wait


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    for pattern in sys.argv[1:]:
        up, down, clicks, wait = split(pattern)
        print("%-40s n=%d click p50 %.1f mean %.1f | click->capture p50 %.1f mean %.1f | "
              "capture->drawn p50 %.1f mean %.1f p90 %.1f | decode wait p50 %.1f p90 %.1f" % (
                  pattern, len(clicks), q(clicks, .5), mean(clicks), q(up, .5), mean(up),
                  q(down, .5), mean(down), q(down, .9), q(wait, .5), q(wait, .9)))


if __name__ == "__main__":
    main()
