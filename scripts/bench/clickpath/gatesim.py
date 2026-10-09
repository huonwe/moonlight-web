"""The stream's gate replayed on a pass's captures (plan « attente », 09/10/2026).

The native host encodes only the presents its gate admits
(backend/native-host/src/core/FrameCadence.h): the first present in each
tick's window of the stream's grid, from a quarter of an interval before the
tick to a quarter before the next. A window with nothing in it moves the grid
to the present that ends the gap; two, and the grid restarts on it.
This replays that gate, offline, on the captures a pass recorded, at any
stream rate and display rate, and says what the stream would have carried.

⚠️ Gate below is a COPY of FrameCadence::admit() (and of ceiling(), for a
stream at or above the display's rate): the nanosecond grid, kSlackDivisor 4,
kCeilingHeadroom 50, the windows, a ceiling's re-anchor past one display
period. Change it with the class. --before is the gate of the passes of 08-09/10
(re-anchored on any present later than one display period); checked on
09/10/2026, on every pass of benches 2 and c1 it gives the host's own "N not
carried" count to within 2 (7731 for 7733).

What it needs: a pass run with clicktrace=1 in MW_NATIVE_TUNING, whose capture
wake-ups were copied as bench-out/content-age/<tag>.click-trace.csv (see the
README). Traces of 08-09/10/2026 there:
  *-re9-vis-*      RE9 alone, ~77 pictures a second, virtual display at 120
                   and 240 Hz (<tag>.re9.txt bounds the game's scene)
  *-aw21-*         mw-click-target at 240 fps, virtual display at 120 / 240 Hz
  *-aw2c-*, *-aw2s-*  the same at 240 Hz, the stream at 120 or 240 fps

    python gatesim.py <tag> [<tag>...] [--fps 60] [--hz 240] [--before | --advance]

--fps / --hz: the stream's rate and the display's (default: both from the
pass's log). --before: the gate until the fix of 09/10/2026 (Cadence session),
for a pass recorded with it. --advance: the change the Android TV bench
proposed (B, 08/10/2026), a present later than one display period moving the
grid on by whole intervals from the tick it missed; it gives the rate back but
lets a game at the stream's rate beat against the grid. Per pass: the new pictures,
those the gate skipped, how long a skipped picture's change waited for the
next admitted one, and that wait averaged over every instant (what a click at
a random moment pays for the gate).
"""
import argparse
import csv
import os
import re

OUT = os.path.join("bench-out", "content-age")


class Gate:
    """FrameCadence::admit(); rule "before" or "advance" for the older ones."""

    SLACK_DIVISOR = 4
    CEILING_HEADROOM = 50

    def __init__(self, fps, display_hz, rule="now"):
        self.interval_ns = (1000000 // fps) * 1000
        self.ceiling = fps >= display_hz
        if self.ceiling:
            self.interval_ns = self.interval_ns * self.CEILING_HEADROOM // (self.CEILING_HEADROOM + 1)
        self.reanchor_ns = (1000000 // display_hz) * 1000
        interval_us = self.interval_ns // 1000
        self.slack_us = interval_us if self.ceiling else interval_us // self.SLACK_DIVISOR
        self.next_due_ns = 0
        self.rule = rule

    def admit(self, now_us):
        now_ns = now_us * 1000
        slack_ns = self.slack_us * 1000
        if now_ns + slack_ns < self.next_due_ns:
            return False
        late_ns = now_ns - self.next_due_ns
        if self.ceiling or self.rule != "now":
            if late_ns <= self.reanchor_ns:
                self.next_due_ns += self.interval_ns
            elif self.rule == "advance" and not self.ceiling and self.next_due_ns:
                self.next_due_ns += (late_ns // self.interval_ns + 1) * self.interval_ns
            else:
                self.next_due_ns = now_ns + self.interval_ns
            return True
        window_end_ns = self.interval_ns - slack_ns
        if late_ns < window_end_ns:
            self.next_due_ns += self.interval_ns
        elif late_ns < window_end_ns + self.interval_ns:
            self.next_due_ns = now_ns + slack_ns
        else:
            self.next_due_ns = now_ns + self.interval_ns
        return True


def pass_rates(base):
    """The stream's and the display's rates the pass ran at, from its log."""
    try:
        with open(base + ".server.log", encoding="utf-8", errors="replace") as f:
            for line in f:
                m = re.search(r"cadence: (\d+)(?:\.\d+)? Hz display, (\d+) fps stream", line)
                if m:
                    return int(m.group(2)), int(m.group(1))
    except OSError:
        pass
    return None, None


def scene(base):
    """A game pass's scene, on the host's clock (µs), or None for the whole pass."""
    try:
        text = open(base + ".re9.txt", encoding="utf-8", errors="replace").read()
    except OSError:
        return None
    lo = re.search(r"steady=(\d+) scene from here", text)
    hi = re.search(r"steady=(\d+) scene to here", text)
    return (int(lo.group(1)), int(hi.group(1))) if lo and hi else None


def q(xs, p):
    xs = sorted(xs)
    return xs[int(p * (len(xs) - 1))] if xs else float("nan")


def one(tag, a):
    base = os.path.join(a.dir, tag)
    caps = [int(r["us"]) for r in csv.DictReader(open(base + ".click-trace.csv"))
            if r["kind"] == "capture" and r["status"] == "ok"]
    if not caps:
        print("%s: no captures" % tag)
        return
    fps, hz = pass_rates(base)
    fps, hz = a.fps or fps, a.hz or hz
    if not fps or not hz:
        print("%s: no rates in the log, give --fps and --hz" % tag)
        return
    gate = Gate(fps, hz, a.rule)
    # The gate runs over the whole pass, as the host's did; only the scene counts.
    admitted = [(t, gate.admit(t)) for t in caps]
    lo, hi = scene(base) or (caps[0], caps[-1])
    kept = [(t, ok) for t, ok in admitted if lo <= t <= hi]
    span = (hi - lo) / 1e6
    skipped_waits, weighted, total, next_ok = [], 0.0, 0.0, None
    # Backwards: for each new picture, the first admitted one at or after it.
    for i in range(len(kept) - 1, -1, -1):
        t, ok = kept[i]
        if ok:
            next_ok = t
        if next_ok is None:
            continue
        wait = next_ok - t
        if not ok:
            skipped_waits.append(wait / 1000)
        if i > 0:
            # Every instant since the picture before sees this one first.
            seg = t - kept[i - 1][0]
            weighted += seg * wait
            total += seg
    skipped = sum(1 for _, ok in kept if not ok)
    print("%s: %d fps stream, %d Hz display%s: %.1f new pictures/s, %.1f carried/s, %d skipped (%.1f %%), "
          "a skipped one waits p50 %.1f p90 %.1f ms, mean cost %.2f ms" % (
              tag, fps, hz, "" if a.rule == "now" else ", --" + a.rule,
              len(kept) / span, (len(kept) - skipped) / span, skipped, 100 * skipped / len(kept),
              q(skipped_waits, .5), q(skipped_waits, .9), weighted / total / 1000 if total else 0))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("tags", nargs="+")
    ap.add_argument("--dir", default=OUT)
    ap.add_argument("--fps", type=int, default=0, help="the stream's rate (default: the pass's)")
    ap.add_argument("--hz", type=int, default=0, help="the display's rate (default: the pass's)")
    rule = ap.add_mutually_exclusive_group()
    rule.add_argument("--before", dest="rule", action="store_const", const="before",
                      help="the gate until 09/10/2026: re-anchored on a present later than a display period")
    rule.add_argument("--advance", dest="rule", action="store_const", const="advance",
                      help="a late present moves the grid on by whole intervals (Android TV bench, 08/10)")
    ap.set_defaults(rule="now")
    a = ap.parse_args()
    for tag in a.tags:
        one(tag, a)


if __name__ == "__main__":
    main()
