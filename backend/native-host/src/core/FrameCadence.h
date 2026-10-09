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

#pragma once

#include <cstdint>

namespace mw::native {

/// Holds the capture loop to the stream's frame rate — without ever holding a
/// picture.
///
/// ── Why the loop needs this at all ──────────────────────────────────────────
///
/// Desktop Duplication wakes the loop on every present of the display, and
/// until this existed every one of them was encoded. On a 165 Hz panel with the
/// stream set to 60 that is 165 frames a second through a rate control that was
/// told 60: the constant bitrate is a budget PER FRAME, so the wire carried 2.75
/// times the configured rate on a moving screen — 63 Mbit/s for 20 set, the
/// moment a window was dragged — and the VBV bounded nothing it was meant to.
/// In a LAN nobody notices. Over the Internet the excess sits in the SCTP queue
/// and is felt as the pointer trailing the hand.
///
/// ── Nothing waits: the first present at or after each tick ──────────────────
///
/// The stream's interval is a grid. A present that arrives before the next tick
/// is skipped — converted, so the freshest picture is always in the converter's
/// texture for the still-screen paths, but not encoded. The first present at or
/// after the tick is encoded THE MOMENT IT ARRIVES, and the grid advances by one
/// interval. Nothing is ever kept for a later wake-up.
///
/// The first version of this gate did the opposite: it held the last present of
/// each interval and encoded it when the interval fell due, for a perfectly
/// regular emission. Measured over the Internet, that wait was two thirds of
/// the host's time — 5.4 ms on average, a full 17 ms interval at the p99, for a
/// present that arrived just after a tick and had to sit out the next one. A
/// picture waiting on the host is latency the viewer feels; an emission that is
/// early or late by a present period (6 ms at 165 Hz, nothing when the game
/// itself runs at the stream's rate) is not, and with tearing on the client is
/// invisible altogether. So: latency first, regularity second.
///
/// ── The slack, and how the grid keeps its rate ──────────────────────────────
///
/// A present a little BEFORE the tick — within kSlackFraction of the interval —
/// goes through too, because the next one would arrive a whole present period
/// later for nothing. So each tick owns a window, from a quarter interval
/// before it to a quarter interval before the next, and the first present in
/// that window is the one encoded, however late in it it comes. The grid
/// advances by exactly one interval from where it was, never "now plus one":
/// that is what keeps the rate at the setting when every admitted present is
/// early or late by a few milliseconds (anchoring on the present itself
/// measured 56 fps for 60 on a 165 Hz panel).
///
/// Only a window that closed with nothing in it moves the grid — a present was
/// MISSING where the grid expected one. When the present that ends the gap
/// comes within the next interval — a game at the stream's own rate that
/// drifted, or landed a refresh late, past the window's end; content a little
/// faster than the stream with a longer gap than usual — it is counted for
/// the tick it missed, and the grid moves so that its window ends there: the
/// next window opens at the present. Later than that — two windows with
/// nothing, the screen had been still or the loop had stalled — the grid
/// restarts on it, one interval on. So the loop never catches up with a burst
/// (one missed tick at most is made up, and only while the content keeps
/// coming), a first change after a pause is on the wire at once, and a game at
/// 60 fps under a 60 fps stream locks onto the grid instead of beating against
/// it: its latest present sits at the end of a window, the others inside.
///
/// The grid used to be re-anchored on any present later than one refresh of
/// the display, taken for a missing one. Measured on the Android TV bench (B,
/// 08/10/2026): content at 60 fps on a 144 Hz display streamed at 50 comes
/// every 16.7 ms, so the first present past a tick is often more than a
/// refresh (6.9 ms) late with none missing; each re-anchor threw that lateness
/// away, the next present fell too early and was skipped — 40 fps carried for
/// 50. Replayed on RE9 (77 fps) on the virtual display at 240 Hz, the default
/// since 30/09: 48.4 for 60, 40.7 for 50. The same rule cost a 60 fps test
/// pattern on a 120 Hz screen, streamed at 60, a third of its frames: landing
/// a refresh early or late, it re-anchored on the late ones and skipped the
/// early ones. Moving the grid by whole intervals only, never its phase, gives
/// the rate back to the first case and not to the second: a game at the
/// stream's rate a refresh early or late then beats against the grid, down to
/// 45 fps for 60 in simulation.
///
/// ── A stream at the display's own rate: a ceiling, not a grid ───────────────
///
/// A stream at the display's own rate — the setting "0" resolves to it — was
/// long left without a gate, on the belief that a display presents no faster
/// than it refreshes. Desktop Duplication disagrees. Measured on DualRTX
/// (22/09/2026): a 60 Hz screen driven by the iGPU, 59.95 Hz by its own
/// vblanks, reported 73 to 104 presents a second while a browser on it ticked
/// with the 120 Hz primary — every one encoded, 72 to 91 frames a second for
/// 60 set and 21 to 25 Mbit/s for 20. An uncapped game on a 60 Hz screen is
/// the same case.
///
/// A gate at exactly the display's rate would be wrong the other way: a panel a
/// hair faster than its nominal rate would run early against the grid and lose
/// a present every few seconds. So the gate is a CEILING (ceiling()): the
/// stream's interval shortened by kCeilingHeadroom, which no real refresh ever
/// comes under — every present of a display that keeps to its rate is past due
/// when it arrives and goes straight through — while presents that come faster
/// than the display can show them are held to the stream's rate, give or take
/// that headroom.
///
/// And its slack is a whole interval rather than a quarter. What the loop is
/// handed is not a clean refresh train: on the Arc's 120 Hz screen streamed at
/// 120, one present in twenty-five came less than 6 ms after the one before —
/// the loop had been busy and caught up, or the browser presented twice — and a
/// quarter-interval slack skipped two of them a second, each a picture the
/// viewer then waited a whole content frame for. A ceiling is there to stop a
/// runaway source, not to space frames; over anything longer than a pair it
/// holds the rate all the same.
///
/// ── The grid is kept in nanoseconds ─────────────────────────────────────────
///
/// A cadence aligned on the client's refresh (CadenceAlign.h) is an exact
/// multiple of a measured period — three 165 Hz periods are 18181.8 µs — and a
/// grid that truncated it to whole microseconds would slip a present every two
/// minutes. The interval and the grid are held in nanoseconds; the loop's
/// stamps stay in microseconds, the unit everything else here speaks.
///
/// Pure and clocked by the caller, so it can be tested without a display.
class FrameCadence
{
public:
    /// How early a present may be, as a fraction of the interval, and still go
    /// through: a quarter. A window admits one present at most, whatever the
    /// slack, so the slack costs no rate — it only decides how far the emission
    /// may run ahead of the grid before the next present is preferred.
    static constexpr int kSlackDivisor = 4;

    /// How much faster than the stream a ceiling lets frames through: one
    /// fiftieth, 61.2 fps for 60. Far above how far a display's clock strays
    /// from its nominal rate (a few hundredths of a hertz), far below what a
    /// runaway source produces.
    static constexpr int kCeilingHeadroom = 50;

    /// @p fps 0 (or negative) disables the gate. @p displayHz is the display's
    /// refresh, which sets how late a ceiling's present may be before it is
    /// re-anchored on (see admit()); 0 falls back to half an interval.
    explicit FrameCadence(int fps, int displayHz = 0)
        : m_IntervalNs(fps > 0 ? static_cast<int64_t>(1000000 / fps) * 1000 : 0)
        , m_ReanchorNs(displayHz > 0 ? static_cast<int64_t>(1000000 / displayHz) * 1000
                                     : m_IntervalNs / 2)
    {}

    /// The same gate on an exact interval rather than a whole number of frames
    /// per second — for a cadence aligned on the client's refresh, where the
    /// interval is a multiple of a measured period and rarely a round number.
    static FrameCadence fromIntervalNs(int64_t intervalNs, int displayHz)
    {
        FrameCadence c(0, displayHz);
        c.m_IntervalNs = intervalNs > 0 ? intervalNs : 0;
        if (displayHz <= 0) c.m_ReanchorNs = c.m_IntervalNs / 2;
        return c;
    }

    /// The gate for a stream at or above the display's own rate, whose interval
    /// is @p intervalNs: a real refresh always goes through, anything faster is
    /// held to the stream's rate (see above).
    static FrameCadence ceiling(int64_t intervalNs, int displayHz)
    {
        FrameCadence c =
            fromIntervalNs(intervalNs * kCeilingHeadroom / (kCeilingHeadroom + 1), displayHz);
        c.m_Ceiling = c.enabled();
        return c;
    }

    bool enabled() const { return m_IntervalNs > 0; }
    /// True for a ceiling(): every refresh of the display is encoded.
    bool isCeiling() const { return m_Ceiling; }
    int64_t intervalUs() const { return m_IntervalNs / 1000; }
    /// A ceiling's slack is a whole interval: it lets a pair of presents
    /// through back to back as long as the grid is not ahead of the clock,
    /// and still holds the rate over any stretch longer than two frames.
    int64_t slackUs() const { return m_Ceiling ? intervalUs() : intervalUs() / kSlackDivisor; }
    int64_t reanchorUs() const { return m_ReanchorNs / 1000; }

    /// A new picture is ready at @p nowUs. True: encode it now. False: skip it
    /// — the next present is the one that will carry the interval.
    bool admit(int64_t nowUs)
    {
        if (!enabled()) return true;
        const int64_t nowNs = nowUs * 1000;
        const int64_t slackNs = slackUs() * 1000;
        if (nowNs + slackNs < m_NextDueNs) {
            m_Skipped++;
            return false;
        }
        const int64_t lateNs = nowNs - m_NextDueNs;
        if (m_Ceiling) {
            // A ceiling keeps its own rule: re-anchored on a present later than
            // one refresh of the display.
            m_NextDueNs = lateNs > m_ReanchorNs ? nowNs + m_IntervalNs : m_NextDueNs + m_IntervalNs;
            return true;
        }
        // How late a present may be and still be in its tick's window.
        const int64_t windowEndNs = m_IntervalNs - slackNs;
        if (lateNs < windowEndNs)
            m_NextDueNs += m_IntervalNs; // in its window: the grid keeps its phase
        else if (lateNs < windowEndNs + m_IntervalNs)
            m_NextDueNs = nowNs + slackNs; // one window empty: the next one opens here
        else
            m_NextDueNs = nowNs + m_IntervalNs; // a pause or a stall: restart on it
        return true;
    }

    /// When the next present will be admitted. Meaningful only when enabled.
    int64_t nextDueUs() const { return m_NextDueNs / 1000; }

    /// Presents that were not encoded — what the display produced that the
    /// stream did not carry.
    int64_t skipped() const { return m_Skipped; }

private:
    int64_t m_IntervalNs = 0;
    int64_t m_ReanchorNs = 0;
    int64_t m_NextDueNs = 0;
    int64_t m_Skipped = 0;
    bool m_Ceiling = false;
};

} // namespace mw::native
