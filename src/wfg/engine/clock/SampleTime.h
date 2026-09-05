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
    Samples to wall-clock time and back.

    Two functions, shared by the dummy clock (which turns a block index into a
    deadline) and the tick thread (which turns "how many samples until the next
    boundary" into how long to sleep). One copy, because the reason they are
    written the way they are is not obvious and two copies of a subtlety are two
    chances to lose it.

    THE SUBTLETY IS OVERFLOW. The obvious spelling is

        samples * 1'000'000'000 / sampleRate

    and it is wrong in a way that takes about fifty hours at 48 kHz to show up:
    the numerator passes 2^63 and the result goes negative, so a deadline lands
    in the past and the clock free-runs. Fifty hours is exactly the kind of
    limit that never appears in a test and appears in a rig somebody left
    running over a weekend.

    So both directions split the value into whole seconds and a remainder, and
    only the remainder - which is bounded by the sample rate - is multiplied by
    a billion. At 192 kHz that product tops out around 1.9e14, with four orders
    of magnitude of headroom, permanently.

    Integer arithmetic throughout, and no floating point anywhere near a
    deadline: a double carries 53 bits of mantissa, which stops being exact for
    sample counts past about 104 days at 48 kHz, and "exact" is the property
    this whole clock is built on.
*/

#include <chrono>
#include <cstdint>

namespace wfg
{
    /** How long `samples` lasts at `sampleRate`. A non-positive rate gives
        zero rather than dividing by it. */
    std::chrono::nanoseconds samplesToDuration (std::int64_t samples, int sampleRate) noexcept;

    /** The inverse. A negative duration gives zero: something that happened
        before its deadline is on time, and a negative lateness would only
        invite somebody to average it away. */
    std::int64_t durationToSamples (std::chrono::nanoseconds elapsed, int sampleRate) noexcept;
}
