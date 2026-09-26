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
#include <string>
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

        /*  HOW WIDE THE CUE IS HERE (2026-09-26, cue/InsertChain.h): the
            channels sent - nought, and the block passes dry - and how many are
            taken back, wider when the plugin makes a mono cue stereo. -1 for
            either is the voice's own width, as before any of this. Any thread. */
        void setShape (int feed, int back) noexcept;

        //======================================================================
        /*  A CUE'S WHOLE STATE (the author's decision of 2026-09-25): what the
            plugin's own window changed that is not a parameter, kept per cue
            as a file, loaded onto this voice's instance before the cue may
            launch. The lane keeps which state it was asked for, which it
            holds, and the one in flight; the arm waits on `stateSettled`.

            Message thread, at an arm: this lane should hold that state - a
            file, absolute, or empty for the preset's own. */
        void wantState (const std::string& path);

        /*  The tick thread, when a cue's state changes after its arm and
            before its launch: the path follows from the message thread, but
            the launch must wait from NOW, so the count moves here. */
        void expectState() noexcept;

        /*  Any thread, the tick thread's question at every launch: nothing
            this lane was asked to load is still loading. A lane that is not
            switched in, not called or not bound has nothing to wait for. */
        bool stateSettled() const noexcept;

        /** What one pass of the host's poll found. */
        struct StateNews
        {
            bool arrived = false;       ///< a load finished - `failed` and `problem` say how
            bool failed = false;
            bool late = false;          ///< one has been loading for longer than it may
            double loadMs = 0.0;
            std::string problem;

            /** What the plugin declares after the state, uncompensated (2026-09-26). */
            int latencySamples = 0;
        };

        /*  The host's poll, message thread: sends the wanted state when none
            is in flight (settling at once when the lane holds it already),
            and reads the child's answer when it comes. */
        StateNews serviceState (std::uint32_t nowMs);

        /*  A new child holds the preset's own state and nothing in flight:
            the one that was loading when the last child died is not sent
            again - it may be what killed it. */
        void forgetState();

        /** How long a state may load before the host calls the child hung. */
        static constexpr std::uint32_t stateLoadLimitMs = 5000;

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
        std::atomic<int> shapeFeed { -1 };
        std::atomic<int> shapeBack { -1 };
        region::Header* header = nullptr;
        float* audio = nullptr;
        int regionChannels = 0;
        int regionMaxSamples = 0;

        std::atomic<int> enabled { 0 };
        std::atomic<int> callEnabled { 1 };
        std::atomic<std::int64_t> deadlineUs { defaultDeadlineMicroseconds };

        std::atomic<float> shadow[region::maxParams];

        std::atomic<std::uint64_t> wantedStateSeq { 0 };
        std::atomic<std::uint64_t> settledStateSeq { 0 };
        std::string wantedStatePath, heldStatePath, flightStatePath;
        std::uint64_t flightStateSeq = 0;
        std::uint64_t pathStateSeq = 0;     ///< the count `wantedStatePath` belongs to
        std::uint32_t flightSentAt = 0;
        bool stateInFlight = false;

        std::atomic<std::uint64_t> blockCount { 0 };
        std::atomic<std::uint64_t> answeredCount { 0 };
        std::atomic<std::uint32_t> missCount { 0 };
        std::atomic<std::uint32_t> consecutive { 0 };
    };
}
