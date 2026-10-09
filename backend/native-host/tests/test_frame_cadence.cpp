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

#include "core/FrameCadence.h"
#include "native_test_framework.h"

#include <vector>

using mw::native::FrameCadence;

namespace {

/// Drive a cadence with presents at a fixed refresh rate for @p seconds, the
/// way the capture loop does: an admitted present is encoded on the spot, a
/// skipped one is never encoded. Returns the encode times, in µs.
struct Sim
{
    std::vector<int64_t> encodedAt;
    int presents = 0;
    int64_t skipped = 0;
};

/// Presents at @p presentHz — which need not be the display's nominal rate —
/// with an optional alternating jitter of ±@p jitterUs.
Sim simulateOn(FrameCadence cadence, double presentHz, double seconds, int64_t jitterUs = 0,
               int64_t startUs = 0)
{
    Sim sim;
    const double presentStep = 1e6 / presentHz;
    for (double t = 0; t < seconds * 1e6; t += presentStep) {
        const int64_t presentUs =
            startUs + static_cast<int64_t>(t) + ((sim.presents % 2) ? jitterUs : -jitterUs);
        sim.presents++;
        if (cadence.admit(presentUs)) sim.encodedAt.push_back(presentUs);
    }
    sim.skipped = cadence.skipped();
    return sim;
}

Sim simulate(int fps, int displayHz, double seconds, int64_t startUs = 0)
{
    return simulateOn(FrameCadence(fps, displayHz), displayHz, seconds, 0, startUs);
}

/// Content at @p contentHz shown on a @p displayHz display, as the capture
/// loop sees it: each frame lands on the display's next refresh, a refresh that
/// shows two frames is one present, and the loop wakes up to @p wakeJitterUs
/// after the refresh. @p frameJitterUs spreads each frame's own time (± that):
/// a frame due near a refresh lands on it or on the next one. @p drift
/// stretches the content's period (+0.001: a game a thousandth slower).
/// Deterministic for a given @p seed.
std::vector<int64_t> shownOn(double contentHz, double displayHz, double seconds, double phaseUs,
                             int64_t frameJitterUs, int64_t wakeJitterUs, uint32_t seed,
                             double drift = 0.0)
{
    auto uniform = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<double>(seed >> 8) / 16777216.0;
    };
    const double framePeriod = 1e6 / contentHz * (1.0 + drift);
    const double refresh = 1e6 / displayHz;
    std::vector<int64_t> presents;
    int64_t lastRefresh = -1;
    for (int n = 0;; ++n) {
        const double t = phaseUs + n * framePeriod + (2 * uniform() - 1) * frameJitterUs;
        if (t > phaseUs + seconds * 1e6) break;
        const auto shown = static_cast<int64_t>(t / refresh) + 1;
        if (shown <= lastRefresh) continue;
        lastRefresh = shown;
        presents.push_back(static_cast<int64_t>(shown * refresh + uniform() * wakeJitterUs));
    }
    return presents;
}

/// What a gate carries of @p presents: frames a second over their span, and
/// how many it skipped.
struct Carried
{
    double fps = 0;
    double contentFps = 0;
    int64_t skipped = 0;
};

Carried carry(FrameCadence gate, const std::vector<int64_t>& presents)
{
    int admitted = 0;
    for (int64_t t : presents)
        if (gate.admit(t)) admitted++;
    const double span = (presents.back() - presents.front()) / 1e6;
    return {admitted / span, presents.size() / span, gate.skipped()};
}

void gaps(const Sim& sim, int64_t& minGap, int64_t& maxGap)
{
    minGap = 1 << 30;
    maxGap = 0;
    for (size_t i = 1; i < sim.encodedAt.size(); ++i) {
        const int64_t gap = sim.encodedAt[i] - sim.encodedAt[i - 1];
        if (gap < minGap) minGap = gap;
        if (gap > maxGap) maxGap = gap;
    }
}

} // namespace

void run_frame_cadence_tests()
{
    SECTION("FrameCadence — zero means every present");
    {
        FrameCadence off(0);
        CHECK(!off.enabled());
        for (int i = 0; i < 10; ++i)
            CHECK(off.admit(i * 6060));
        CHECK_EQ(off.skipped(), int64_t(0));

        FrameCadence negative(-5, 165);
        CHECK(!negative.enabled());
        CHECK(negative.admit(0));
    }

    SECTION("FrameCadence — 165 Hz presents into a 60 fps stream");
    {
        // Sub-microsecond drift is not what is being tested: a 60 fps interval
        // is 16666 µs and the presents step 6060.6.
        const Sim sim = simulate(60, 165, 10.0);
        // 165 presents/s in, 60 frames/s out. The grid advances by its interval
        // on every admitted present rather than restarting on it, so the rate
        // holds — within a frame either way for the edges of the run.
        CHECK(sim.presents >= 1649 && sim.presents <= 1651);
        CHECK(sim.encodedAt.size() >= 599 && sim.encodedAt.size() <= 601);
        CHECK_EQ(sim.skipped, int64_t(sim.presents) - int64_t(sim.encodedAt.size()));

        // Nothing is ever held, so the emission is as regular as the presents
        // allow: every gap between two encodes is within one present period of
        // the stream interval.
        int64_t minGap, maxGap;
        gaps(sim, minGap, maxGap);
        CHECK(minGap > 16666 - 6061);
        CHECK(maxGap < 16666 + 6061);
    }

    SECTION("FrameCadence — 165 Hz presents into a 120 fps stream");
    {
        // The present period (6.06 ms) is more than half the interval (8.33):
        // an early present and a late one can both land inside one wall-clock
        // interval, but each consumes one tick of the grid, so the rate is
        // still the setting's.
        const Sim sim = simulate(120, 165, 10.0);
        CHECK(sim.encodedAt.size() >= 1199 && sim.encodedAt.size() <= 1201);
    }

    SECTION("FrameCadence — 100 Hz presents into a 60 fps stream");
    {
        const Sim sim = simulate(60, 100, 10.0);
        CHECK(sim.encodedAt.size() >= 599 && sim.encodedAt.size() <= 601);
        int64_t minGap, maxGap;
        gaps(sim, minGap, maxGap);
        CHECK(minGap > 16666 - 10001);
        CHECK(maxGap < 16666 + 10001);
    }

    SECTION("FrameCadence — a display slower than the stream is never throttled");
    {
        // 60 Hz presents into a 120 fps stream: every present is past due when
        // it arrives, nothing is skipped.
        const Sim sim = simulate(120, 60, 5.0);
        CHECK_EQ(sim.encodedAt.size(), size_t(sim.presents));
        CHECK_EQ(sim.skipped, int64_t(0));
    }

    SECTION("FrameCadence — the first present at or after the tick, the others skipped");
    {
        FrameCadence c(60, 165); // 16666 µs interval, slack 4166
        CHECK(c.admit(0));       // first frame goes straight through
        CHECK_EQ(c.nextDueUs(), int64_t(16666));
        CHECK(!c.admit(6000));
        CHECK(!c.admit(12000)); // 12000 + 4166 < 16666: still too early
        CHECK_EQ(c.skipped(), int64_t(2));
        CHECK(c.admit(18000)); // encoded the moment it arrives
        // The grid advanced by one interval from where it was, not from the
        // slightly late present.
        CHECK_EQ(c.nextDueUs(), int64_t(33332));
    }

    SECTION("FrameCadence — a present within the slack of the tick goes through now");
    {
        FrameCadence c(60, 165);
        CHECK(c.admit(0));
        // Due at 16666; a present at 13000 is within a quarter interval of it.
        CHECK(c.admit(13000));
        CHECK_EQ(c.skipped(), int64_t(0));
        // …and the grid kept its phase rather than restarting on it.
        CHECK_EQ(c.nextDueUs(), int64_t(33332));
    }

    SECTION("FrameCadence — after a still screen the first change is immediate and re-anchors");
    {
        FrameCadence c(60, 165);
        CHECK(c.admit(0));
        // Two seconds of nothing, then a present: encoded now, and the grid
        // restarts on it rather than on a tick nobody had been keeping.
        CHECK(c.admit(2000000));
        CHECK_EQ(c.nextDueUs(), int64_t(2016666));
    }

    SECTION("FrameCadence — a late present keeps the grid, only an empty window moves it");
    {
        // Late by 7 ms after a skipped present: more than a refresh of a
        // 165 Hz display, yet nothing was missing — the present at 9666 was
        // there, too early. The gate used to re-anchor here (to 40332) and
        // throw the 7 ms away; the grid now keeps its phase, display or not.
        for (int hz : {165, 0}) {
            FrameCadence c(60, hz);
            CHECK(c.admit(0));
            CHECK(!c.admit(9666));
            CHECK(c.admit(23666));
            CHECK_EQ(c.nextDueUs(), int64_t(33332));
        }

        // Nothing in the window [12500, 29166): the present at 30000 counts
        // for the tick it missed, and the next window opens at it.
        FrameCadence c(60, 165);
        CHECK(c.admit(0));
        CHECK(c.admit(30000));
        CHECK_EQ(c.nextDueUs(), int64_t(30000 + 4166));
        // So the tick is made up by the next present, however soon…
        CHECK(c.admit(31000));
        // …but by that one only.
        CHECK(!c.admit(40000));
        CHECK_EQ(c.nextDueUs(), int64_t(30000 + 4166 + 16666));

        // Two windows with nothing — [12500, 45832) — and the grid restarts on
        // the present, one interval on, as after a still screen.
        FrameCadence still(60, 165);
        CHECK(still.admit(0));
        CHECK(still.admit(46000));
        CHECK_EQ(still.nextDueUs(), int64_t(46000 + 16666));
        CHECK(!still.admit(47000));
    }

    SECTION("FrameCadence — a game at the stream's rate goes through whole");
    {
        // 60 fps presents 10 ms after the grid's ticks: the first lands early
        // and is skipped, the rest all fall in their windows. One drop, not a
        // beat, and the grid kept its phase.
        FrameCadence c(60, 165);
        CHECK(c.admit(0));
        int encoded = 0, skipped = 0;
        for (int i = 0; i < 300; ++i) {
            if (c.admit(10000 + i * 16666))
                encoded++;
            else
                skipped++;
        }
        CHECK_EQ(encoded, 299);
        CHECK_EQ(skipped, 1);
        CHECK_EQ(c.nextDueUs(), int64_t(300 * 16666));

        // A game at 60 on a 120, 144, 165 or 240 Hz display, its frames due
        // right on a refresh so that each lands on it or on the next one —
        // the test pattern of the Android TV bench (N4, 08/10/2026), which
        // the gate used to cut to 41 fps for 60 — exactly at the stream's
        // rate, a thousandth faster or a thousandth slower, at 16 phases.
        // Every frame goes, but for the first few and the thousandth a faster
        // game has to lose.
        for (double hz : {120.0, 144.0, 165.0, 240.0}) {
            for (double drift : {-0.001, 0.0, 0.001}) {
                for (uint32_t seed = 1; seed <= 16; ++seed) {
                    const double refresh = 1e6 / hz;
                    const double phase = (seed * 7919 % 1000) * refresh;
                    const auto presents = shownOn(60, hz, 20.0, phase, 700, 300, seed, drift);
                    const Carried got = carry(FrameCadence(60, static_cast<int>(hz)), presents);
                    CHECK(got.skipped <= 4);
                    // With frames 2 ms either side of their time, too.
                    const auto loose = shownOn(60, hz, 20.0, seed * 997.0, 2000, 300, seed, drift);
                    CHECK(carry(FrameCadence(60, static_cast<int>(hz)), loose).skipped <= 4);
                }
            }
        }
    }

    SECTION("FrameCadence — content faster than the stream carries the stream's rate");
    {
        // The Android TV bench (B, 08/10/2026): 60 fps content on a 144 Hz
        // display streamed at 50 carried 40. The same at 240 Hz, the virtual
        // display's default. 20 s at 16 phases, frames on time or due near a
        // refresh: 50 a second, within one percent.
        for (double hz : {144.0, 240.0}) {
            for (uint32_t seed = 1; seed <= 16; ++seed) {
                for (int64_t frameJitter : {int64_t(0), int64_t(700), int64_t(3000)}) {
                    const auto presents =
                        shownOn(60, hz, 20.0, seed * 997.0, frameJitter, 300, seed);
                    const Carried got = carry(FrameCadence(50, static_cast<int>(hz)), presents);
                    CHECK(got.fps > 49.5 && got.fps < 50.2);
                }
            }
        }

        // RE9 on the virtual display at 240 Hz: ~77 fps, frame times uneven by
        // a few milliseconds. Replayed from the bench, a 60 fps stream carried
        // 48.4 of them; now 60, within one percent.
        for (uint32_t seed = 1; seed <= 16; ++seed) {
            for (int64_t frameJitter : {int64_t(0), int64_t(3000)}) {
                const auto presents = shownOn(77, 240, 20.0, seed * 997.0, frameJitter, 300, seed);
                const Carried got = carry(FrameCadence(60, 240), presents);
                CHECK(got.fps > 59.4 && got.fps < 60.2);
                CHECK(carry(FrameCadence(50, 240), presents).fps > 49.5);
            }
        }

        // And never more than the stream's rate, whatever comes in: 65, 90,
        // 120 and 240 a second into 60.
        for (double content : {65.0, 90.0, 120.0, 240.0}) {
            const auto presents = shownOn(content, 240, 20.0, 1234.0, 700, 300, 7);
            const Carried got = carry(FrameCadence(60, 240), presents);
            CHECK(got.fps > 59.4 && got.fps < 60.2);
        }
    }

    SECTION("FrameCadence — no catching up after a pause or a stall");
    {
        // The desktop at 240 Hz into 60, a still second, then movement again:
        // the first change goes at once, the next one a whole interval later
        // (less the slack) — nothing of the second of ticks is made up.
        FrameCadence c(60, 240);
        for (int i = 0; i < 240; ++i)
            c.admit(i * 4166);
        const int64_t resume = 2000000;
        CHECK(c.admit(resume));
        std::vector<int64_t> admitted{resume};
        for (int i = 1; i < 60; ++i)
            if (c.admit(resume + i * 4166)) admitted.push_back(resume + i * 4166);
        CHECK(admitted[1] - admitted[0] >= 16666 - 4166);
        // 250 ms of movement: 15 intervals, 15 or 16 pictures.
        CHECK(admitted.size() >= 15 && admitted.size() <= 16);

        // The loop stalls 100 ms, then catches up: five presents half a
        // millisecond apart. One goes; the burst does not.
        FrameCadence s(60, 240);
        for (int i = 0; i < 60; ++i)
            s.admit(i * 4166);
        const int64_t back = 59 * 4166 + 100000;
        CHECK(s.admit(back));
        for (int i = 1; i < 5; ++i)
            CHECK(!s.admit(back + i * 500));
        // From then on, the stream's rate: 6 or 7 in the next 100 ms.
        int next = 0;
        for (int i = 1; i <= 24; ++i)
            if (s.admit(back + 2000 + i * 4166)) next++;
        CHECK(next >= 5 && next <= 7);
    }

    SECTION("FrameCadence — a ceiling never skips a display that keeps to its rate");
    {
        const FrameCadence c = FrameCadence::ceiling(1000000000LL / 60, 60);
        CHECK(c.enabled());
        CHECK(c.isCeiling());
        CHECK(!FrameCadence(60, 165).isCeiling());
        // A fiftieth faster than the stream: 16339 µs for 16666.
        CHECK_EQ(c.intervalUs(), int64_t(16339));

        // A 60 Hz stream on a 60 Hz display, whether the panel runs a little
        // slow (59.94, 59.95 here on DualRTX), exactly, or a little fast:
        // every present goes through, for minutes.
        for (double hz : {59.94, 60.0, 60.06}) {
            const Sim sim = simulateOn(c, hz, 120.0);
            CHECK_EQ(sim.skipped, int64_t(0));
            CHECK_EQ(sim.encodedAt.size(), size_t(sim.presents));
        }
        // Presents a millisecond and a half either side of their refresh —
        // far more than Desktop Duplication's timestamps stray — still all go.
        const Sim jittered = simulateOn(c, 60.0, 60.0, 1500);
        CHECK_EQ(jittered.skipped, int64_t(0));

        // A present the loop picks up late, then the next one on time: 3 ms
        // apart. Both go — the one skipped would have been a picture the
        // viewer waited a whole frame for.
        FrameCadence pair = FrameCadence::ceiling(1000000000LL / 120, 120);
        CHECK_EQ(pair.slackUs(), pair.intervalUs());
        for (int i = 0; i < 10; ++i)
            CHECK(pair.admit(i * 16666));
        CHECK(pair.admit(10 * 16666 + 5000));
        CHECK(pair.admit(10 * 16666 + 8000));
        CHECK_EQ(pair.skipped(), int64_t(0));
    }

    SECTION("FrameCadence — a ceiling holds presents beyond the refresh to the stream's rate");
    {
        // What Desktop Duplication reported on a 59.95 Hz screen while a
        // browser on it ticked with a 120 Hz primary: 73 to 104 presents a
        // second. The stream is set to 60: no more than 61.2 go out.
        for (double hz : {73.0, 104.0, 120.0, 300.0}) {
            const Sim sim =
                simulateOn(FrameCadence::ceiling(1000000000LL / 60, 60), hz, 10.0, 0, 5000);
            CHECK(sim.encodedAt.size() >= 590 && sim.encodedAt.size() <= 615);
        }
        // A stream set ABOVE the display's rate: the display's own refreshes
        // all go through, a runaway source stops at the setting.
        const FrameCadence at120 = FrameCadence::ceiling(1000000000LL / 120, 60);
        CHECK_EQ(simulateOn(at120, 60.0, 10.0).skipped, int64_t(0));
        const Sim runaway = simulateOn(at120, 300.0, 10.0);
        CHECK(runaway.encodedAt.size() >= 1190 && runaway.encodedAt.size() <= 1230);
    }

    SECTION("FrameCadence — no interval, no ceiling");
    {
        const FrameCadence c = FrameCadence::ceiling(0, 60);
        CHECK(!c.enabled());
        CHECK(!c.isCeiling());
    }
}
