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
    THE BEAT CLOCK (namespace draft §122, §123 BEH): where the beat grid is at a
    moment, as one pure function three sides share - the Runner counting in
    ticks, the audio side and a plugin's child counting in samples - so a sound
    placed on a bar by one lands where the other two say the bar is.

    Seconds are counted from the ANCHOR, the moment a quantised GO started the
    grid or a tempo cue last moved it. From there the tempo is `bpmFrom` until
    the ramp starts, moves to `bpmTo` over `rampLength` seconds along a shape -
    straight, the full S, or the blend between - and stays. A length of nought
    is a step at `rampStart`. The beat at a moment is the tempo's integral,
    in closed form; the moment of a beat is that integral inverted.

    NO JUCE, NO ALLOCATION, NO LOCK: read on the audio thread and in a child
    process that reads it out of shared memory.
*/

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace wfg::cue
{
    struct BeatClock
    {
        double anchorBeat = 0.0;        ///< the beat at the anchor, counted from the first anchor's nought
        double bpmFrom = 120.0;         ///< the tempo at the anchor
        double bpmTo = 120.0;           ///< the tempo the ramp reaches
        double rampStart = 0.0;         ///< seconds from the anchor to the ramp's start
        double rampLength = 0.0;        ///< seconds the ramp takes; nought is a step
        double shape = 0.0;             ///< 0 straight, 1 the full S, between the blend
        int beatsPerBar = 4;

        /*  THE RAMP'S PROGRESS SHAPED: a blend of the straight line and the
            smoothstep by `shape`, and its integral from nought - both reach
            one and a half at one, so a ramp of either shape covers the same
            beats as the straight one. */
        static double shaped (double p, double k) noexcept
        {
            p = std::clamp (p, 0.0, 1.0);
            k = std::clamp (k, 0.0, 1.0);
            return (1.0 - k) * p + k * p * p * (3.0 - 2.0 * p);
        }

        static double shapedIntegral (double p, double k) noexcept
        {
            p = std::clamp (p, 0.0, 1.0);
            k = std::clamp (k, 0.0, 1.0);
            return (1.0 - k) * p * p / 2.0 + k * (p * p * p - p * p * p * p / 2.0);
        }

        double safeFrom() const noexcept { return std::isfinite (bpmFrom) && bpmFrom > 0.0 ? bpmFrom : 120.0; }
        double safeTo() const noexcept   { return std::isfinite (bpmTo) && bpmTo > 0.0 ? bpmTo : safeFrom(); }
        double length() const noexcept   { return std::isfinite (rampLength) && rampLength > 0.0 ? rampLength : 0.0; }
        double start() const noexcept    { return std::isfinite (rampStart) ? rampStart : 0.0; }

        /** The tempo at `seconds` from the anchor. */
        double bpmAt (double seconds) const noexcept
        {
            if (! std::isfinite (seconds) || seconds < start())
                return safeFrom();

            //  A step has happened at its moment; a ramp is still at its start there.
            if (length() <= 0.0 || seconds >= start() + length())
                return safeTo();

            return safeFrom() + (safeTo() - safeFrom()) * shaped ((seconds - start()) / length(), shape);
        }

        /** The beat at `seconds` from the anchor: the tempo's integral. */
        double beatAt (double seconds) const noexcept
        {
            if (! std::isfinite (seconds))
                return anchorBeat;

            const auto from = safeFrom();
            const auto to = safeTo();
            const auto s = start();
            const auto l = length();

            if (seconds <= s)
                return anchorBeat + from * seconds / 60.0;

            const auto atStart = anchorBeat + from * s / 60.0;

            if (l <= 0.0)
                return atStart + to * (seconds - s) / 60.0;

            if (seconds >= s + l)
                return atStart + l * (from + to) / 120.0 + to * (seconds - s - l) / 60.0;

            const auto p = (seconds - s) / l;
            return atStart + l * (from * p + (to - from) * shapedIntegral (p, shape)) / 60.0;
        }

        /*  THE MOMENT OF A BEAT, seconds from the anchor: the integral inverted
            - straight where the tempo is steady, bisected inside the ramp,
            where the tempo is positive and the beat climbs with the second. */
        double secondsOfBeat (double beat) const noexcept
        {
            if (! std::isfinite (beat))
                return 0.0;

            const auto from = safeFrom();
            const auto to = safeTo();
            const auto s = start();
            const auto l = length();
            const auto beatAtStart = beatAt (s);

            if (beat <= beatAtStart)
                return (beat - anchorBeat) * 60.0 / from;

            const auto beatAtEnd = beatAt (s + l);

            if (beat >= beatAtEnd)
                return s + l + (beat - beatAtEnd) * 60.0 / to;

            auto low = s;
            auto high = s + l;

            for (int n = 0; n < 64 && high - low > 1.0e-9; ++n)
            {
                const auto mid = 0.5 * (low + high);
                (beatAt (mid) < beat ? low : high) = mid;
            }

            return 0.5 * (low + high);
        }

        /** The beat the bar holding `beat` starts on, bars counted from beat nought. */
        double lastBarStartBeat (double beat) const noexcept
        {
            const auto bar = std::max (1, beatsPerBar);
            return std::floor (beat / bar) * bar;
        }

        /*  THE SAME NUMBERS, bit for bit - a comparison by value would be a
            float-equal, and a clock that changed by a bit is one the audio
            side should hear about. */
        bool sameAs (const BeatClock& other) const noexcept
        {
            const auto bits = [] (double value) { return std::bit_cast<std::uint64_t> (value); };

            return bits (anchorBeat) == bits (other.anchorBeat) && bits (bpmFrom) == bits (other.bpmFrom)
                && bits (bpmTo) == bits (other.bpmTo) && bits (rampStart) == bits (other.rampStart)
                && bits (rampLength) == bits (other.rampLength) && bits (shape) == bits (other.shape)
                && beatsPerBar == other.beatsPerBar;
        }
    };

    /*  WHAT THE AUDIO SIDE IS GIVEN (namespace draft §123, BEI): the clock and
        the Go.dot sample its anchor falls on - nought for a grid not yet
        anchored, whose beats then count from the show's start - and whether
        the transport a plugin sees is playing. */
    struct BeatClockState
    {
        std::int64_t anchorSample = 0;
        BeatClock clock;
        bool playing = true;

        bool sameAs (const BeatClockState& other) const noexcept
        {
            return anchorSample == other.anchorSample && playing == other.playing && clock.sameAs (other.clock);
        }
    };
}
