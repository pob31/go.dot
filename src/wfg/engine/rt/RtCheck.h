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
    PRD §4.2 made checkable: "the audio thread is a lipogram — no allocation, no
    locks, no exceptions, no syscalls, no logging."

    A rule nothing enforces is a rule that decays. It decays quietly, too: an
    allocation on the audio thread is not a crash, it is a lock inside the
    allocator taken while a block is due, and what the room hears is a click
    once an hour that nobody can reproduce. So the rule is instrumented, and the
    instrumentation ships in every build that asks for it rather than living in
    a test.

    HOW IT WORKS. Under WFG_RT_CHECKS the global operator new and delete are
    replaced by counting versions (RtAllocatorHook.cpp). They are ordinary
    allocators plus one thread-local read and one relaxed atomic increment. A
    ScopedRealtimeCheck marks a region of the audio thread and says whose code
    it is; an allocation inside a region is counted against that region and
    against nothing else.

    THE TWO REGIONS, and the distinction is the whole design.

      `Region::ours` is Go.dot's code, and its count MUST be zero. It is a
      defect, published at /godot/engine/rtViolations, and every PR from here
      adds its scenario.

      `Region::foreign` is Tracktion's block. Its count is MEASURED AND
      REPORTED, never hidden and never asserted to be zero. Tracktion's device
      callback takes a std::shared_lock every block by design
      (tracktion_DeviceManager.cpp) and its node-player pool uses semaphores;
      that is a fact about a dependency Go.dot chose, and the honest thing is to
      publish the number rather than to pretend the lipogram covers code we do
      not write. Whether PRD §4.2 should say so is the author's amendment to
      make, and it is put to them in the Phase 2 namespace draft.

    Regions do not accumulate: the innermost one wins and the previous is
    restored on the way out. Tracktion's block runs INSIDE Go.dot's callback, so
    without that, every allocation TE makes would be charged to us.

    NOTHING IS LOGGED, THROWN OR PRINTED FROM A REGION. A violation is a number
    that goes up. Reporting it from the audio thread would be the second
    violation of the rule the first one broke, and a debug break in a show is
    worse than the click it was trying to tell you about.

    WITH WFG_RT_CHECKS OFF every scope below is an empty inline object and the
    counters read zero, so the markers can stay in the code that matters rather
    than being wrapped in #if at each site.
*/

#include <cstdint>

namespace wfg::rt
{
    /** Whose code a region is, and therefore how its allocations are judged. */
    enum class Region
    {
        /** Go.dot's own. Any allocation here is a defect. */
        ours,

        /** A dependency's, inside our callback. Counted and reported, not judged. */
        foreign
    };

    //==============================================================================
    /** Allocations made inside a `Region::ours` scope since the last reset. */
    std::uint64_t violations() noexcept;

    /** Allocations made inside a `Region::foreign` scope since the last reset. */
    std::uint64_t foreignAllocations() noexcept;

    /** Blocks that have been through a `Region::foreign` scope since the last
        reset, so the count above can be read per block rather than in total. */
    std::uint64_t foreignRegions() noexcept;

    /** Whether the counting allocator is actually compiled in. A test that did
        not ask this could pass by measuring nothing at all. */
    bool isCounting() noexcept;

    /** Any thread, with the audio stopped. */
    void resetCounts() noexcept;

    //==============================================================================
    /*  WFG_AUDIO_THREAD - "nothing in here may block", checked by the compiler
        and by the sanitizer rather than by a reviewer.

        This is the second net under PRD §4.2 and it catches what the counting
        allocator above cannot. The counter answers one question: did anything
        call operator new. Clang's real-time sanitizer answers the whole of
        §4.2 - locks, syscalls, anything that can block - and it does half of it
        at COMPILE time, because `nonblocking` propagates: a function marked
        with it may only call functions that are also nonblocking, so the
        warning arrives when somebody writes the call rather than on the night
        it first contends.

        GUARDED TWICE, and the second guard is the point. `__has_cpp_attribute`
        keeps it off every compiler that is not Clang 20 or newer, which is all
        three of the platforms this project ships from. `WFG_RTSAN` keeps it off
        even on Clang unless the rtsan preset asked for it.

        That is deliberately conservative. The attribute changes what compiles:
        marking a function nonblocking makes the compiler object to a great deal
        of code it was previously happy with, including code inside JUCE and
        Tracktion that Go.dot does not own and cannot annotate. Confining that
        to one CI job means the answer arrives as a report from a machine built
        to produce it, and never as a broken build on somebody's laptop.

        WHERE IT GOES: the audio callback's own entry points and nothing else.
        Not on a helper that happens to be called from one - the attribute is a
        promise about a thread, and a function that is called from both threads
        cannot make it. */
    #if defined (WFG_RTSAN) && defined (__clang__)
     #if __has_cpp_attribute (clang::nonblocking)
      #define WFG_AUDIO_THREAD [[clang::nonblocking]]
     #else
      #define WFG_AUDIO_THREAD
     #endif
    #else
     #define WFG_AUDIO_THREAD
    #endif

    //==============================================================================
    /*  Marks a region of the audio thread. Nothing here allocates, locks or
        makes a syscall - it is two thread-local writes - so the instrument does
        not break the rule it measures.
    */
    class ScopedRealtimeCheck
    {
    public:
        explicit ScopedRealtimeCheck (Region) noexcept;
        ~ScopedRealtimeCheck() noexcept;

        ScopedRealtimeCheck (const ScopedRealtimeCheck&) = delete;
        ScopedRealtimeCheck& operator= (const ScopedRealtimeCheck&) = delete;

    private:
       #if WFG_RT_CHECKS
        int previous = 0;
       #endif
    };
}
