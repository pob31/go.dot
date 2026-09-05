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

#include <wfg/engine/clock/SampleTime.h>

namespace wfg
{
    namespace
    {
        constexpr std::int64_t nanosPerSecond = 1'000'000'000;
    }

    std::chrono::nanoseconds samplesToDuration (std::int64_t samples, int sampleRate) noexcept
    {
        if (sampleRate <= 0)
            return std::chrono::nanoseconds { 0 };

        const auto rate = static_cast<std::int64_t> (sampleRate);

        /*  Truncating division on a negative sample count would round towards
            zero and the remainder would come back negative, which still sums to
            the right answer here - seconds and nanoseconds would both be
            negative. It is correct either way, and negative durations are
            meaningful (how long ago something was). */
        const auto whole = samples / rate;
        const auto remainder = samples % rate;

        return std::chrono::seconds (whole)
             + std::chrono::nanoseconds (remainder * nanosPerSecond / rate);
    }

    std::int64_t durationToSamples (std::chrono::nanoseconds elapsed, int sampleRate) noexcept
    {
        const auto nanos = elapsed.count();

        if (nanos <= 0 || sampleRate <= 0)
            return 0;

        const auto rate = static_cast<std::int64_t> (sampleRate);

        return (nanos / nanosPerSecond) * rate
             + ((nanos % nanosPerSecond) * rate) / nanosPerSecond;
    }
}
