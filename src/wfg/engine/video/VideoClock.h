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
    WHICH SAMPLE A FRAME IS SHOWN AT (Phase 8a, namespace draft 35.4; PRD
    §3.19d: "video frames are presented against the audio position").

    The renderer never asks the system what time it is for the show: it is
    told, fifty times a second, where Go.dot's samples were and when they were
    read, and from those pairs it keeps an estimate - a line, sample against
    the machine's steady nanoseconds - that it can read at the moment a frame
    will reach the screen.

    A SLOW LOOP, because each pair is read by a thread that wakes when it
    wakes, off a counter that moves a block at a time: a pair is up to a block
    and a scheduling delay off, and a picture that jumped by that every tick
    would shimmer. So each pair moves the line a little - its offset by a
    tenth of the error, its slope by less, held within half a percent of the
    rate it was told - and the noise averages out while a real drift (two
    crystals, §3.19d) is followed. A pair a quarter of a second off the line
    is not noise: the clock was reset or the interface changed, and the line
    starts again from it.

    Pure, no clock of its own, no allocation: what makes it testable on a
    made-up clock.
*/

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wfg::video
{
    class ClockEstimate
    {
    public:
        void feed (std::int64_t sample, std::int64_t nanos, int sampleRate) noexcept
        {
            if (sample < 0 || sampleRate <= 0)
                return;

            const auto nominal = static_cast<double> (sampleRate) * 1.0e-9;

            if (! started || sampleRate != rateTold)
            {
                restart (sample, nanos, sampleRate, nominal);
                return;
            }

            const auto dt = static_cast<double> (nanos) - baseNanos;

            if (dt <= 0.0)
                return;

            const auto predicted = baseSample + dt * slope;
            const auto error = static_cast<double> (sample) - predicted;

            if (std::abs (error) > 0.25 * static_cast<double> (sampleRate))
            {
                restart (sample, nanos, sampleRate, nominal);
                return;
            }

            baseSample = predicted + error * offsetGain;
            baseNanos = static_cast<double> (nanos);
            slope = std::clamp (slope + (error / dt) * slopeGain, nominal * 0.995, nominal * 1.005);
        }

        /** The sample at `nanos`, or -1 before the first pair. */
        std::int64_t sampleAt (std::int64_t nanos) const noexcept
        {
            if (! started)
                return -1;

            return static_cast<std::int64_t> (std::llround (baseSample + (static_cast<double> (nanos) - baseNanos) * slope));
        }

        bool hasStarted() const noexcept       { return started; }
        double samplesPerSecond() const noexcept  { return slope * 1.0e9; }

    private:
        void restart (std::int64_t sample, std::int64_t nanos, int sampleRate, double nominal) noexcept
        {
            started = true;
            rateTold = sampleRate;
            baseSample = static_cast<double> (sample);
            baseNanos = static_cast<double> (nanos);
            slope = nominal;
        }

        static constexpr double offsetGain = 0.1;
        static constexpr double slopeGain = 0.01;

        bool started = false;
        int rateTold = 0;
        double baseSample = 0.0;
        double baseNanos = 0.0;
        double slope = 0.0;
    };
}
