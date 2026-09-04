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
    The sample counter every other clock in Go.dot is derived from (PRD 3.4).

    One number, monotonic, advanced by whoever owns the audio side: a dummy
    thread in Phase 1, the real audio callback from Phase 2. The advance is a
    single relaxed atomic add - no allocation, no lock, no syscall, no logging -
    because from Phase 2 it runs inside the callback, where 4.2's lipogram
    applies.

    It never resets. A device change, a sample-rate change mid-show (PRD 6.2's
    Dante domain moving under a running show) and timecode re-anchoring (3.14)
    all have to leave tick indices monotonic, and the only way to guarantee that
    is for the underlying count never to go backwards. The tick clock rebases
    its samples-per-tick ratio at a tick boundary instead.

    Vendor-free on purpose: this header is reachable from the engine facade.
*/

#include <atomic>
#include <cstdint>

namespace wfg
{
    class SampleClock
    {
    public:
        virtual ~SampleClock() = default;

        /** Total samples elapsed since the engine started. Any thread. */
        virtual std::int64_t samplesElapsed() const noexcept = 0;
    };

    /*  A counter advanced by hand: the audio side calls advance() once per
        block, tests and the replay tool call it to place events at exact
        samples. This is the only implementation Phase 1 has; Phase 2 adds one
        driven by the device callback, and this one stays for replay, where
        determinism matters more than realism. */
    class ManualClock final : public SampleClock
    {
    public:
        ManualClock() = default;

        /** The audio side, or a test. Relaxed: nothing is published with it. */
        void advance (std::int64_t numSamples) noexcept
        {
            samples.fetch_add (numSamples, std::memory_order_relaxed);
        }

        /** Jump straight to a sample position. Tests and replay only - it can
            move backwards, which nothing in a running engine may do. */
        void setSamplesElapsed (std::int64_t position) noexcept
        {
            samples.store (position, std::memory_order_relaxed);
        }

        std::int64_t samplesElapsed() const noexcept override
        {
            return samples.load (std::memory_order_relaxed);
        }

    private:
        std::atomic<std::int64_t> samples { 0 };
    };
}
