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

#include <wfg/engine/plugin/SharedRegion.h>
#include <wfg/engine/rt/RtCheck.h>

#include <atomic>
#include <cstdint>
#include <utility>
#include <vector>

/*
    ONE VOICE'S SIDE OF THE ROUND TRIP to a plugin's child process (Phase 9a,
    §17.6): the parent's half of a lane in the shared region.

    WHY THIS IS A SEPARATE CLASS FROM THE PLUGIN THAT HOLDS IT - CueEq's reason,
    exactly. It names no Tracktion type and no JUCE type, so the spin, the
    deadline, the passthrough on a miss and the miss counting are tested in
    microseconds against a region laid out in a vector, with no engine and no
    process; and the Tracktion plugin around it is fifty lines that copy
    pointers.

    THE AUDIO THREAD (PRD §4.2). process() allocates nothing, takes no lock and
    makes no syscall. It copies the block into the lane, publishes the request
    with a release store, and spins on the response with acquire loads, reading
    the clock once every sixty-four turns - spike 07's cadence; the clock is not
    free and reading it every pass would measure the clock rather than the
    round trip. Past the deadline the block is left as it was: a momentarily
    dry strip is better than a hole, and the miss is counted for the host to
    judge. A SWITCHED-OFF LANE COSTS NOTHING: with `enabled` nought process()
    returns before it reads the region - no copy, no signal, no spin.

    THE HOST'S SWITCH is separate from the cue's. `callEnabled` is the proxy
    host's: cleared when the entry has failed, so a failed strip is not called
    at all (§3.18: the cost of a failure is bounded), and set again on a
    restart. `enabled` is the cue's: which of the set this cue switches in.

    PARAMETERS ARE SHADOWED. A cue's values may be written before the child is
    up - an arm on a show that is still loading - so every store lands in a
    shadow here and, once the lane is bound, in the region too; bind() copies
    the shadow across and bumps the revision, so nothing written early is
    lost. `useBaseline` is a value like any other: the child reads it as "what
    the preset left".
*/
namespace wfg::plugin
{
    class ProxyLane
    {
    public:
        ProxyLane();

        /** Consecutive misses at which the host marks the entry failed. */
        static constexpr int missesBeforeFailure = 8;

        /** The deadline a lane starts with; the host sets the real one. */
        static constexpr std::int64_t defaultDeadlineMicroseconds = 250;

        //======================================================================
        /*  Points the lane at its part of a region. Message thread, and the
            region must outlive the binding: a bound lane is read by the audio
            thread from the next block on. The shadowed parameters are copied
            across and the child told to apply them. */
        void bind (region::Header* header, region::Lane* lane, float* audio,
                   int channels, int maxSamples) noexcept;

        /** Forgets the region. Only with the audio stopped. */
        void unbind() noexcept;

        bool isBound() const noexcept;

        /** The spin's limit; the host computes it from the block period. */
        void setDeadlineMicroseconds (std::int64_t microseconds) noexcept;
        std::int64_t deadlineMicroseconds() const noexcept;

        //======================================================================
        /** The cue's switch: whether this voice's block goes to the child. Any thread. */
        void setEnabled (bool shouldBeEnabled) noexcept;
        bool isEnabled() const noexcept;

        /** The host's switch: cleared for a failed entry, set on a restart. */
        void setCallEnabled (bool shouldBeCalled) noexcept;
        bool isCallEnabled() const noexcept;

        /** A value, normalised 0..1, or `region::useBaseline`. Any thread but
            the audio one; one writer at a time by contract. */
        void setParameter (int index, float normalised) noexcept;
        float parameter (int index) const noexcept;

        /*  The whole set at once, at an arm: every parameter back to the
            baseline, then the pairs given, and one revision for the lot.
            Message thread, while the voice is silent. */
        void setValues (const std::vector<std::pair<int, float>>& values) noexcept;

        /** Asks the child to reset the instance before its next block. */
        void requestReset() noexcept;

        //======================================================================
        /*  The audio thread. Sends `numChannels` channels of `numSamples`
            frames, up to the region's shape, and takes the answer in place;
            leaves the block untouched when the lane is off, unbound, not to
            be called, or the child is late. */
        void process (float* const* channelData, int numChannels, int numSamples) noexcept WFG_AUDIO_THREAD;

        //======================================================================
        /** Blocks sent to the child. */
        std::uint64_t blocks() const noexcept;

        /** Blocks the child answered in time. */
        std::uint64_t answered() const noexcept;

        /** Blocks the child was late for, in all. */
        std::uint32_t misses() const noexcept;

        /** Late blocks since the last one that was in time. */
        std::uint32_t consecutiveMisses() const noexcept;

        /** The host, on a restart: the new child starts with a clean sheet. */
        void clearMisses() noexcept;

    private:
        std::atomic<region::Lane*> lane { nullptr };
        region::Header* header = nullptr;
        float* audio = nullptr;
        int regionChannels = 0;
        int regionMaxSamples = 0;

        std::atomic<int> enabled { 0 };
        std::atomic<int> callEnabled { 1 };
        std::atomic<std::int64_t> deadlineUs { defaultDeadlineMicroseconds };

        std::atomic<float> shadow[region::maxParams];

        std::atomic<std::uint64_t> blockCount { 0 };
        std::atomic<std::uint64_t> answeredCount { 0 };
        std::atomic<std::uint32_t> missCount { 0 };
        std::atomic<std::uint32_t> consecutive { 0 };
    };
}
