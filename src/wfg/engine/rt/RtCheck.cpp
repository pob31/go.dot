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

#include <wfg/engine/rt/RtCheck.h>

#include <wfg/engine/rt/RtCounters.h>

namespace wfg::rt
{
   #if WFG_RT_CHECKS

    std::uint64_t violations() noexcept         { return detail::ourAllocations.load (std::memory_order_relaxed); }
    std::uint64_t foreignAllocations() noexcept { return detail::foreignAllocationCount.load (std::memory_order_relaxed); }
    std::uint64_t foreignRegions() noexcept     { return detail::foreignRegionCount.load (std::memory_order_relaxed); }
    bool isCounting() noexcept                  { return true; }

    void resetCounts() noexcept
    {
        detail::ourAllocations.store (0, std::memory_order_relaxed);
        detail::foreignAllocationCount.store (0, std::memory_order_relaxed);
        detail::foreignRegionCount.store (0, std::memory_order_relaxed);
    }

    ScopedRealtimeCheck::ScopedRealtimeCheck (Region region) noexcept
        : previous (detail::currentRegion)
    {
        detail::currentRegion = region == Region::ours ? detail::regionOurs
                                                       : detail::regionForeign;

        /*  Counted on the way IN rather than out, so a region that ended in a
            way nobody planned for still shows up in the denominator. A per-block
            average computed from regions that completed would flatter exactly
            the blocks worth knowing about. */
        if (region == Region::foreign)
            detail::foreignRegionCount.fetch_add (1, std::memory_order_relaxed);
    }

    ScopedRealtimeCheck::~ScopedRealtimeCheck() noexcept
    {
        /*  RESTORED, NOT CLEARED. Tracktion's block runs inside Go.dot's
            callback, so leaving this at "no region" on the way out of the inner
            scope would stop counting the outer one - and the epilogue, which is
            Go.dot's code and the part that must be zero, would go unmeasured. */
        detail::currentRegion = previous;
    }

   #else

    /*  The switch is off. Everything below is what a build with no
        instrumentation answers: nothing was counted, and it says so rather than
        reporting a confident zero. isCounting() is the difference, and it is
        why a test can tell "the audio thread allocated nothing" apart from
        "nobody was looking". */
    std::uint64_t violations() noexcept         { return 0; }
    std::uint64_t foreignAllocations() noexcept { return 0; }
    std::uint64_t foreignRegions() noexcept     { return 0; }
    bool isCounting() noexcept                  { return false; }
    void resetCounts() noexcept                 {}

    ScopedRealtimeCheck::ScopedRealtimeCheck (Region) noexcept {}
    ScopedRealtimeCheck::~ScopedRealtimeCheck() noexcept {}

   #endif
}
