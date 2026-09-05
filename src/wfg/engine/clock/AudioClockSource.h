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
    The counter the audio side advances, and the only one it is allowed to hold.

    WHY THIS IS NOT ManualClock, which already has an advance(). Because
    ManualClock also has setSamplesElapsed(), which can move the count
    BACKWARDS. That is exactly right for a test placing an event at an exact
    sample and for a replay seeking, and it is exactly wrong for the audio
    thread: SampleClock's whole guarantee is that the number never goes back,
    because tick indices are derived from it and a show whose tick index
    repeats has no ordering left.

    Making that a separate type rather than a rule in a comment means the audio
    callback CANNOT rewind the clock, whatever anybody writes inside it. The
    type is the enforcement.

    THE LIPOGRAM (PRD §4.2). advance() is one relaxed atomic add: no allocation,
    no lock, no exception, no syscall, no logging. From Phase 2 it is called
    from inside the device callback, so it has to stay that way - and there is
    nothing here that could grow into anything else.

    Relaxed rather than release, deliberately. Nothing is PUBLISHED with this
    number: the tick thread reads it to decide whether a boundary has been
    reached and reads nothing else through it, so there is no happens-before to
    establish and the fence would cost something for nothing. What the tick
    thread then does with the model is ordered by the event queue, which has its
    own synchronisation.

    Vendor-free, like SampleClock.h and for the same reason.
*/

#include <wfg/engine/clock/SampleClock.h>

#include <atomic>
#include <cstdint>

namespace wfg
{
    class AudioClockSource final : public SampleClock
    {
    public:
        AudioClockSource() = default;

        /** The audio thread, once per block. The only way this number moves. */
        void advance (std::int64_t numSamples) noexcept
        {
            samples.fetch_add (numSamples, std::memory_order_relaxed);
        }

        std::int64_t samplesElapsed() const noexcept override
        {
            return samples.load (std::memory_order_relaxed);
        }

    private:
        std::atomic<std::int64_t> samples { 0 };
    };
}
