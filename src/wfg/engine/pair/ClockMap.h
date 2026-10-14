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
    THE LEADER'S SAMPLE ON THIS MACHINE'S CLOCK (namespace draft §58, RD.9;
    decisions AFS, AGF, AGI, AGQ).

    Up to RD.8 a follower applied its leader's records a fixed number of ticks
    later (AGF) and placed every launch on its own tick grid, so each sound it
    started was Δ ticks - twenty to forty milliseconds - behind the leader's.
    The author's AFS asks for the same sample: the two machines start a sound
    together, to within what the two clocks can be known to. Three lines, each
    measured, composed:

      the leader's sample   -> the leader's host clock     its SOUND LINE
      the leader's host     -> this machine's host clock    the HOST OFFSET
      this machine's host   -> this machine's sample        its own sound line

    A SOUND LINE (`SoundLineFit`) is a straight line through the audio side's
    own stamps - each block's first sample against the steady clock's moment it
    was asked for (clock/BlockStamp.h) - by least squares over two minutes,
    `timecode::CountLine`'s line, which is LineFit's, so a slip (a buffer an
    interface dropped) is told and the line starts again at the slope it had,
    and a late callback strung out over a few blocks is not fitted through.
    NOT `ClockEstimate`'s slope: TG.6 measured it walk to its clamp on
    block-quantised pairs and ring after a slip.

    THE HOST OFFSET (`HostOffset`) is NTP's four stamps on the link's own ping:
    the machine that dials sends a ping at t0 on its clock; the other answers at
    once, on its server's thread, with the moment the ping arrived and the
    moment the pong left, t1 and t2 on its clock; the pong is home at t3. The
    other clock less this one is ((t1 - t0) + (t2 - t3)) / 2, out by at most
    half the asymmetry of the two ways, and the trip is (t3 - t0) - (t2 - t1).
    The fastest exchange of each second that holds three is kept - a slow one
    is a queue somewhere, and a queue is never symmetric - and a straight line
    put through two minutes of them, so the two host clocks' own drift is in
    it.
    Only the machine that dials pings (RD.5), so on a main that follows a
    backup that took over the leader measures and says what it measured
    (`MeasuredBy`).

    THE MAP (`ClockMap`) composes the three and is pure: a leader's sample in,
    this machine's out, and back; the drift between the two SOUND clocks in
    parts per million (`pair/drift`, AGI - shown, never corrected: nudging the
    backup's speed is *(proposed)*); and how well it is known, the root of the
    sum of the three lines' squared errors, in microseconds
    (`pair/clockResidual`).

    UNTIL IT IS TRUSTED THE RECORD CLOCK STANDS, so nothing gets worse: the old
    tick-offset arithmetic (`sampleOnThisClock`'s fallback, the follower's own
    tick grid for a launch). Trusted when
      - both sound lines have three stamps or more, so their scatter is known,
        and the host offset three exchanges or more (`fewestExchanges`);
      - the sound clocks are within a thousand parts per million of each other
        (`mostPartsPerMillion`): two clocks ten times further apart than any
        two crystals are is a line still forming or a clock replaced, and
        nothing to place a sound by;
      - and its error is under five milliseconds (`trustedMicroseconds`), a
        quarter of a tick: the record clock it replaces is a tick or two out,
        so a map known that well is four times better at the least, and one
        known worse has a broken input - a sound clock whose stamps scatter by
        milliseconds, a network whose fastest round trip is past ten - rather
        than a rough one. Once trusted, kept until the error passes twice that
        (`untrustedMicroseconds`), so a map at the edge does not flap between
        two placings from one launch to the next.

    THE ONE FUNCTION §60 PROMISED: `sampleOnThisClock`, through the map when it
    is trusted and by the ticks otherwise. A main's `code.heard` (Follow.cpp)
    and a position's code maps and scene samples (Position.cpp) come through
    it; the Runner's launches go through the map it is handed
    (cue::Runner::LaunchPlacing).

    PURE: no clock read, no thread, no JUCE. The fits allocate as their windows
    fill - the tick thread's, never the audio thread's.
*/

#include <wfg/engine/clock/BlockStamp.h>
#include <wfg/engine/timecode/CountLine.h>

#include <cstdint>
#include <deque>

namespace wfg::pair
{
    //==========================================================================
    /*  ONE MACHINE'S SOUND CLOCK AGAINST ITS HOST CLOCK: an anchor sample and
        the steady nanoseconds the line puts it at, the line's slope, how well
        its place is known at its newest stamp, how long its stamps span, and
        the rate the samples are counted at. What a machine says of itself on
        the wire (`/godot/pair/clock`). */
    struct SoundLine
    {
        bool valid = false;
        std::int64_t sample = 0;
        std::int64_t nanos = 0;
        double nanosPerSample = 0.0;
        double errorNanos = 0.0;
        double seconds = 0.0;
        int rate = 0;

        double nanosAt (double atSample) const noexcept;
        double sampleAt (double atNanos) const noexcept;

        //  Where its sample nought sits on the steady clock, to the nanosecond: `engine/sampleZeroAt`.
        std::int64_t sampleZeroNanos() const noexcept;
    };

    /*  THE LINE THROUGH A MACHINE'S OWN STAMPS, the tick thread's: fed once a
        tick the audio side's last stamp (a stamp seen before is not fed twice -
        a block longer than a tick is stamped once), forgotten when the audio
        side is another or its rate changes. */
    class SoundLineFit
    {
    public:
        /*  One block's stamp at `rate` samples a second; true when it told a
            slip and the line started again. A stamp already fed, one with no
            sample, or no rate, says nothing. */
        bool feed (const BlockStamp::Reading& stamp, int rate);

        void forget() noexcept;

        //  The line as it stands; invalid until three stamps.
        SoundLine line() const;

        //  Stamps fed since the line last started.
        std::int64_t stamps() const noexcept { return fed; }

    private:
        timecode::CountLine count { 0.0 };
        std::int64_t lastSample = -1;
        int rateFed = 0;
        std::int64_t fed = 0;
        double firstMillisecond = 0.0;
    };

    //==========================================================================
    /*  ONE PING'S FOUR STAMPS, each on the steady clock of the machine that
        read it: sent and back on the machine that pinged, received and left on
        the one that answered. */
    struct HostSample
    {
        std::int64_t sent = 0, received = 0, left = 0, back = 0;
    };

    /*  THE OTHER MACHINE'S HOST CLOCK LESS THIS ONE'S, as a line in this one's
        time: its value at `at`, how it moves, how well it is known - the fit's
        error and half the fastest trip, which bounds the asymmetry nothing can
        measure - and how many exchanges it is drawn through. */
    struct OffsetLine
    {
        bool valid = false;
        std::int64_t at = 0;
        double offset = 0.0;
        double slope = 0.0;
        double errorNanos = 0.0;
        double bestTripNanos = 0.0;
        int samples = 0;

        double offsetAt (double mine) const noexcept { return offset + slope * (mine - static_cast<double> (at)); }
    };

    /*  THE PINGER'S MEASURE OF THE OTHER HOST CLOCK, the tick thread's: every
        exchange kept for two minutes, the fastest of each second fitted. */
    class HostOffset
    {
    public:
        void add (const HostSample& sample);
        void forget() noexcept;

        const OffsetLine& line() const noexcept { return fitted; }

        static constexpr double windowSeconds = 120.0;
        static constexpr double longestTripSeconds = 1.0;

        //  The exchanges a second must hold for its fastest to be fitted through.
        static constexpr int fewestPerSecond = 3;

        //  How far from nominal two host clocks are believed to drift, until exchanges say better.
        static constexpr double priorPartsPerMillion = 100.0;

    private:
        struct Kept
        {
            std::int64_t at = 0;    // the exchange's middle, this machine's clock
            double offset = 0.0;
            double trip = 0.0;
        };

        void refit();

        std::deque<Kept> kept;
        OffsetLine fitted;
    };

    //==========================================================================
    class ClockMap
    {
    public:
        //  Untrusted: the record clock, the old arithmetic.
        ClockMap() = default;

        /*  WHO MEASURED THE HOST OFFSET: this machine, the follower, which
            dials and pings - the other host clock is the leader's - or the
            leader, which dialled this one and said what it measured, the
            other host clock being this one's. */
        enum class MeasuredBy { follower, leader };

        /*  THE THREE LINES COMPOSED, trusted or not by the rules above;
            `wasTrusted` holds trust until the error passes twice the bar. */
        static ClockMap compose (const SoundLine& leader, const SoundLine& follower, const OffsetLine& hosts,
                                 MeasuredBy measuredBy, bool wasTrusted = false);

        bool trusted() const noexcept { return isTrusted; }

        /*  A LEADER'S SAMPLE ON THIS MACHINE'S CLOCK and back, unrounded, and
            rounded to the sample - whether trusted or not, so a test can
            measure an untrusted map; the engine asks `trusted` first. */
        double followerSampleOf (double leaderSample) const noexcept;
        double leaderSampleOf (double followerSample) const noexcept;
        std::int64_t onThisClock (std::int64_t leaderSample) const noexcept;

        /*  THE DRIFT BETWEEN THE TWO SOUND CLOCKS, parts per million: positive
            when this machine's runs fast against the leader's (AGI). And how
            well the map is known, microseconds. Nought for a map that never
            had its three lines. */
        double driftPartsPerMillion() const noexcept { return drift; }
        double residualMicroseconds() const noexcept { return residual; }

        /*  THE ONE FUNCTION (namespace draft §60, AIP; RD.9): a sample of the
            leader's on this machine's clock - through the map when it is
            trusted; otherwise moved by the ticks the follower applies the
            leader's records behind (`byTicks`) at `sampleRate`, the record
            clock RD.8 seats sounds on, and nothing for a rate that is not a
            sound clock's. */
        std::int64_t sampleOnThisClock (std::int64_t leaderSample, std::int64_t byTicks, int sampleRate) const noexcept;

        static constexpr double trustedMicroseconds = 5000.0;
        static constexpr double untrustedMicroseconds = 10000.0;
        static constexpr int fewestExchanges = 3;
        static constexpr double mostPartsPerMillion = 1000.0;

    private:
        bool isTrusted = false;
        SoundLine leaderLine, followerLine;

        /*  THE HOST CLOCKS: this machine's moment = `followerAnchor` +
            `ratio` × (the leader's moment - `leaderAnchor`). */
        double leaderAnchor = 0.0, followerAnchor = 0.0, ratio = 1.0;

        double drift = 0.0, residual = 0.0;
        bool composed = false;
    };
}
