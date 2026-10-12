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
    THE LINE BETWEEN GO.DOT'S SAMPLES AND THE MACHINE'S STEADY CLOCK: which
    sample is playing at a moment, and at which moment a sample plays.

    Written for the pictures first (Phase 8a, namespace draft 35.4; PRD §3.19d:
    "video frames are presented against the audio position"), and moved here
    from `video/` for MIDI Time Code (namespace draft §59, TC.5; AHP), which
    asks it the other way round: a quarter frame is due on a SAMPLE, and the
    thread that sends it can only wait for a MOMENT. RD.9, the same sample on
    two machines, builds its map between two sound clocks on this same line -
    which is why it lives with the clocks and names nothing of either reader.

    Nobody asks the system what time it is for the show: whoever keeps one of
    these is told, fifty times a second, where Go.dot's samples were and when
    they were read, and from those pairs it keeps an estimate - a line, sample
    against the machine's steady nanoseconds - that it can read at any moment,
    or at any sample.

    A SLOW LOOP, because each pair is read by a thread that wakes when it
    wakes, off a counter that moves a block at a time: a pair is up to a block
    and a scheduling delay off, and a picture or a quarter frame that jumped by
    that every tick would shimmer. So each pair moves the line a little - its
    offset by a tenth of the error, its slope by less, held within half a
    percent of the rate it was told - and the noise averages out while a real
    drift (two crystals, §3.19d) is followed. A pair a quarter of a second off
    the line is not noise: the clock was reset or the interface changed, and
    the line starts again from it.

    ONE ESTIMATE A READER, never one shared: each reader feeds its own from
    the pairs it is handed - the video renderer in its own process, the MTC
    lane on its own thread - so nothing here is ever read and written by two
    threads at once, and it needs no lock.

    Pure, no clock of its own, no allocation: what makes it testable on a
    made-up clock.
*/

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wfg
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

        /*  THE MOMENT A SAMPLE IS AT, on the steady clock the pairs were read
            from: the inverse of `sampleAt`, the two agreeing to within a
            sample. -1 before the first pair - which no steady clock ever
            reads, since it counts up from the machine's start; a reader asks
            `hasStarted` rather than lean on that. A sample before the line's
            base is a moment before it, read off the same slope. */
        std::int64_t nanosAt (std::int64_t sample) const noexcept
        {
            if (! started || ! (slope > 0.0))
                return -1;

            return static_cast<std::int64_t> (std::llround (baseNanos + (static_cast<double> (sample) - baseSample) / slope));
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
