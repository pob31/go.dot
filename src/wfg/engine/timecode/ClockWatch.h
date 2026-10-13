/*
    This file is part of Go.dot — https://github.com/pob31/go.dot

    Copyright (C) 2026 Pierre-Olivier Boulant

    Go.dot is free software: you can redistribute it and/or modify it under the
    terms of the GNU General Public License as published by the Free Software
    Foundation, either version 3 of the License, or (at your option) any later
    version. Go.dot is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
    or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
    (LICENSE, at the repository root) for more details.

    SPDX-License-Identifier: GPL-3.0-or-later
*/

#pragma once

/*
    THE WALL CLOCK WATCHED AS A CODE (namespace draft §60, TG.6; AII, AIO,
    AIQ, AIY).

    A time group whose source is `clock` starts its members at times of day,
    and its chasers follow the clock - on the very map an LTC or MTC group
    fires and chases on (CodeMap.h): this millisecond of the day falls on this
    Go.dot sample, and from there the day runs at this speed. This watcher
    makes that map out of the machine's own clocks and, as CodeWatch does for a
    code, says only its TURNS, each one `code.heard` record from origin `clock`,
    so that a replay - which reads no clock at all - and a backup following its
    main rebuild the same map from a few records an hour (AIO). The unit is the
    millisecond of the day (`ms`, 86 400 000 of them); the speed is wall-clock
    milliseconds per Go.dot millisecond - the interface's crystal against the
    system clock, a few tens of parts per million either way, and whatever a
    time-sync service slews the system clock by.

    THREE CLOCKS, READ EACH TICK:
      - the WALL clock, local time of day to the millisecond: what a member's
        `at` names, and the one clock somebody else may move - a time-sync
        service, the night summer time ends, a hand on the clock;
      - the STEADY clock, read beside it, which nobody moves. The two are read
        back to back (serve brackets the wall clock between two steady
        readings and drops a reading the thread was interrupted in), so the
        wall clock's offset from the steady clock is known to the wall clock's
        own millisecond, and a STEP of the wall clock is told from time passing
        by that offset alone, whatever the tick did;
      - GO.DOT'S SAMPLE, which the map is on. Not taken raw beside the other
        two: the counter moves a block at a time, so a sample read at a moment
        is anything up to a block - 3 to 40 ms - behind it. The tick's clock
        pair - the count and the steady moment it was read at, the one the MTC
        lane and the MTC reader are fed - goes into a straight line of this
        watcher's own, the COUNT'S LINE, fitted by least squares through the
        last two minutes of pairs (CountLine.h, which the MTC reader keeps one
        of too since TG.11's faults), and a reading's sample is that line's at
        the steady moment the wall clock was read at. Not the `ClockEstimate`
        the MTC lane keeps: its slope is followed pair by pair, and pairs a
        block apart in their noise walk it to its clamp of half a percent
        (measured, at 256 and at 512 samples) - harmless a tick ahead, where it
        is used, but milliseconds across a three-second gap, and the readings a
        line is fitted through must not wander so. Readings wait a second
        (`CountLine::quietSeconds`) while the count's line is young - which is
        why the first `jumped` comes a second after the watching begins.

    THE LINE. Each reading is a point: the wall clock's millisecond of the day,
    plus half a millisecond - a reading of .123 is a clock somewhere in .123 to
    .124, and its middle is the unbiased guess - against that sample, unwrapped
    across midnight. A straight line is fitted through the last `windowSeconds`
    (thirty) of them, by the same arithmetic, twice a second; its slope against
    the nominal, 1000 milliseconds of the day per `sampleRate` samples, is the
    speed. Half a minute where a code has a second: the wall clock changes
    speed only as a slew does, slowly, while what it is read through - a
    millisecond's rounding, and what the count's line leaves of the blocks - is
    noisier than a code's frames, and the second is noisy in a way the
    textbook's errors miss: it moves slowly, so neighbouring readings are wrong
    together. So the errors are counted in the scatter the long run shows
    (`fit` says how).

    THE TURNS:
      - `jumped` AT THE FIRST READING, and when the wall clock STEPS: its
        offset from the steady clock moving by more than `stepMilliseconds` in
        one reading. Fifty milliseconds - big enough that no slew trips it (a
        time-sync service slews at most a few hundred parts per million, chrony
        by default up to 8 %, which is 1.7 ms between two ticks; a tick three
        seconds late at 500 ppm is 1.5 ms; the reading itself is a millisecond),
        small enough that a chaser following the clock is told: five times the
        10 ms a chaser acts at (`sync/chaseTolerance`). The line starts again
        from the reading the step landed on, at the speed it had - a step moves
        the clock, not its rate. TG.1's rule for the clock TRIGGERS is another
        and stays as it is: a step of more than two seconds fires none of them.
      - A SMALLER STEP, more than `nudgeMilliseconds` (5 ms) and up to fifty: a
        time-sync service stepping by a few milliseconds. The line starts again
        from it at the speed it had, as for a step, but it is said as
        `running` - a re-anchor, which a chaser catches up rather than
        re-seats. Fitted through instead, a step inside the window would read
        as a speed - twenty milliseconds in a thirty-second window is a
        thousand parts per million for as long as it is in the window - and a
        speed is what a varispeed chaser plays at.
      - WHEN GO.DOT'S CLOCK SLIPS under the steady one - an interface that
        dropped a buffer and lost its samples, a count that paused or reset -
        the count's line is started again and waited for (CountLine.h says how
        a slip is told), and the clock's line with it: the readings before the
        slip are on another line. Then, as the clock is found: `jumped` if it
        is more than `stepMilliseconds` from the line last written, `running`
        if more than a millisecond, nothing if not.
      - `running` when the line, at its newest point, is more than
        `driftMilliseconds` (1 ms, AIO's threshold for MTC: the wall clock is
        read to a millisecond) from the line last written, once the line has
        `settleSeconds` (ten) of readings since it started - a younger one
        swings by more than that, and the anchor said at its first reading
        stands meanwhile; or when the latency changed. The speed written is
        the window's or, when the two agree within their errors, that weighed
        with the speed shown since the line was last written, as CodeWatch
        weighs them; a speed that cannot be told from one is written as
        exactly one, so the map is FrameClock's exact rule.
      - NEVER `stopped`: the clock does not stop. A tick that did not come for
        three seconds is three seconds of the day passed on both clocks alike,
        and nothing to say; the count's line, straight, is where it was.

    Measured on a made-up interface read a block at a time at random moments,
    the tick up to 3 ms late, through twelve hours of a wall clock fifty parts
    per million fast or slow against an interface twenty slow, each hour with
    a gap of three or ten seconds every five minutes: two to five `running` an
    hour at 256- and 512-sample blocks, each map within 1.2 ms of the clock,
    nothing said at any gap; a dropped buffer of either size one `running` at
    the speed the clock had. A first draft fitted a four-second window through
    a ClockEstimate with the textbook's errors and wrote 59 to 782 an hour
    from 128- to 1024-sample blocks, its speed wandering by tens of parts per
    million from one record to the next.

    MIDNIGHT is no turn: the wall clock's advance is taken the near way round
    the dial, as `clockCrossings` takes it, the points run on past 86 400 000,
    and a map is written with its frame wrapped into the day; CodeMap reads any
    millisecond of the day the nearer way round from the anchor, so a map
    written at 23:59:58 places 00:00:01 three seconds on.

    LATENCY (AIQ), as for MTC: the sample a reading falls on is the one PLAYING
    when the clock read it, and what a member must match is the clock as the
    sound is HEARD, so a record's sample is the reading's less the interface's
    output latency. The watcher's own line stays on the samples it read; a
    latency changed re-anchors it.

    THE TICK THREAD'S, AND PURE: no clock of its own, no JUCE, no document.
    Fed in order, asked once a tick; its windows allocate as they grow to two
    minutes of pairs and half a minute of readings, and reuse that.
*/

#include <wfg/engine/timecode/CodeMap.h>
#include <wfg/engine/timecode/CodeWatch.h>
#include <wfg/engine/timecode/CountLine.h>
#include <wfg/engine/timecode/LineFit.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace wfg::timecode
{
    /*  ONE READING OF THE WALL CLOCK: the local time of day in milliseconds,
        0 to 86 399 999 (negative is no reading), and the steady clock's
        nanoseconds at the moment it was read. */
    struct WallReading
    {
        std::int64_t millisecondOfDay = -1;
        std::int64_t steadyNanos = 0;
    };

    class ClockWatch
    {
    public:
        //======================================================================
        /*  THE TICK'S CLOCK PAIR, before each reading: Go.dot's sample and the
            steady moment it was read at, at `rate` samples a second. A rate
            changed forgets everything: the samples counted something else. */
        void feedClock (std::int64_t sample, std::int64_t nanos, int rate);

        //  The interface's output latency, in samples (AIQ).
        void setLatency (std::int64_t samples) noexcept;

        /*  Everything forgotten - both lines, what was written - so the next
            reading is a first one: the show stopped watching the clock, or
            Go.dot's clock was away (an outage). */
        void forget() noexcept;

        //======================================================================
        //  A READING, on the tick, after the clock pair.
        void hear (const WallReading& reading);

        //  ONCE A TICK, after the reading: the turn to write, if this tick has one.
        std::optional<CodeSaid> update();

        /*  A TAKEOVER (AIP): the line it knows, said in full as `jumped`;
            nothing when it has never read the clock. */
        std::optional<CodeSaid> resay();

        //======================================================================
        //  What it last said: absent until the first reading.
        CodeState state() const noexcept { return current; }

        //  The readings' scatter about the line, in milliseconds; nought until three.
        double jitterMilliseconds() const noexcept;

        //  The readings the line is fitted through now.
        std::size_t windowSize() const noexcept { return window.size(); }

        //======================================================================
        static constexpr double stepMilliseconds = 50.0;
        static constexpr double nudgeMilliseconds = 5.0;
        static constexpr double driftMilliseconds = 1.0;
        static constexpr double windowSeconds = 30.0;
        static constexpr double settleSeconds = 10.0;
        static constexpr double checkSeconds = 0.5;

        //  Within this of one, or three times its own error, a speed is written as exactly one.
        static constexpr double unitTolerance = 1.0e-6;

        /*  The scatter assumed until three readings measure it - a millisecond,
            the wall clock's own step - and the least ever assumed, so a line
            through readings that happen to sit on it still has an error. */
        static constexpr double jitterAssumedMilliseconds = 1.0;
        static constexpr double jitterFloorMilliseconds = 0.05;

        //  The readings averaged together to measure the scatter over the long run: a second's.
        static constexpr std::size_t batchReadings = 50;

    private:
        using Point = linefit::Point;
        using Line = linefit::Line;

        //  The line as last written: the map before its latency shift, and its slope and error.
        struct Written
        {
            bool valid = false;
            CodeMap map;                    // its sample the one READ on, before the shift
            double position = 0.0;          // the anchor millisecond, unwrapped as the window is
            double began = 0.0;             // where the line put it, before its sample was rounded up
            double slope = 0.0;             // milliseconds of the day per sample, as written
            double positionError = 0.0;     // milliseconds, at the anchor, when it was written
            std::int64_t latency = 0;
        };

        void restart (Point point, std::optional<CodeTurn> turn);
        void trim();
        void fit();

        double nominalSlope() const noexcept;
        double sigma() const noexcept;
        double snapped (double slope, double error) const noexcept;
        double keptSlope() const noexcept;

        CodeSaid say (CodeTurn turn, double slope);

        int sampleRate = 0;
        std::int64_t latency = 0;

        /*  THE COUNT'S LINE (CountLine.h), readings waiting while it is young,
            and a slip it told that the clock's line has still to start again
            for. */
        CountLine count;
        bool slipped = false;

        //  The reading before, and its position unwrapped.
        bool havePrevious = false;
        WallReading previous;
        double previousPosition = 0.0;

        //  A turn a reading decided, said at the next update: a step's `jumped`, a nudge's `running`.
        std::optional<CodeTurn> pending;

        //  THE CLOCK'S LINE: the readings, each a millisecond of the day against a Go.dot sample.
        CodeState current = CodeState::absent;
        std::deque<Point> window;
        Line live;
        Written written;

        //  The sample the line was last fitted and asked at.
        double lastCheck = 0.0;

        //  The readings' scatter as the long run sees it, in milliseconds a reading: nought until measured.
        double longRun = 0.0;
    };
}
