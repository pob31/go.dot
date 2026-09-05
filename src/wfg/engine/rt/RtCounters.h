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
    The counters themselves, shared by the scope and by the allocator that
    replaces the global operator new.

    THEY ARE IN A HEADER AND NOT A .cpp, and that is not a style choice. The
    replaced operator new is called during static initialisation - before main,
    by whatever the C++ runtime and JUCE allocate on the way up - and a counter
    living in another translation unit may not have been constructed yet when
    the first call arrives. `constinit` on a zero-initialised atomic puts these
    in .bss with no initialiser to run at all, so they are valid from the first
    instruction of the process.

    The thread-local is `int` rather than the enum for the same reason: a scalar
    with a constant initialiser, on the fast path of every allocation the
    process makes while the checks are compiled in.
*/

#include <atomic>
#include <cstdint>

namespace wfg::rt::detail
{
   #if WFG_RT_CHECKS

    inline constexpr int regionNone = 0;
    inline constexpr int regionOurs = 1;
    inline constexpr int regionForeign = 2;

    /** Which region this thread is inside, if any. Audio thread, no atomics
        needed: only that thread reads or writes it. */
    inline thread_local constinit int currentRegion = regionNone;

    /** Written from the audio thread with relaxed increments, read anywhere. */
    inline constinit std::atomic<std::uint64_t> ourAllocations { 0 };
    inline constinit std::atomic<std::uint64_t> foreignAllocationCount { 0 };
    inline constinit std::atomic<std::uint64_t> foreignRegionCount { 0 };

    /** Called by the replaced operator new, on every allocation the process
        makes. Everything off the audio thread takes the first branch and pays
        one thread-local read. */
    inline void noteAllocation() noexcept
    {
        if (currentRegion == regionNone)
            return;

        if (currentRegion == regionOurs)
            ourAllocations.fetch_add (1, std::memory_order_relaxed);
        else
            foreignAllocationCount.fetch_add (1, std::memory_order_relaxed);
    }

   #endif
}
