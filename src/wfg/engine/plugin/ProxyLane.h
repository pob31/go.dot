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

#include <wfg/engine/cue/BeatClock.h>
#include <wfg/engine/plugin/SharedRegion.h>
#include <wfg/engine/rt/RtCheck.h>

#include <array>
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
    round trip. Past the deadline the rest of the block is SILENT, faded down
    from where the last answer left it, and the miss is counted for the host to
    judge - never the dry block (the author's decisions of 2026-09-26, CU and
    CV, overruling the passthrough spike 07 built: a wet-only reverb's dry side
    is the original signal, louder and in the wrong place, where a hole is only
    a hole). A SWITCHED-OFF LANE COSTS NOTHING: with `enabled` nought process()
    returns before it reads the region - no copy, no signal, no spin.

    THE HOST'S SWITCH is separate from the cue's. `callEnabled` is the proxy
    host's: cleared when the entry has failed, so a failed strip is not called
    at all (§3.18: the cost of a failure is bounded) and is silent, and set
    again on a restart. `enabled` is the cue's: which of the set this cue
    switches in.

    PARAMETERS ARE SHADOWED. A cue's values may be written before the child is
    up - an arm on a show that is still loading - so every store lands in a
    shadow here and, once the lane is bound, in the region too; bind() copies
    the shadow across and bumps the revision, so nothing written early is
    lost. `useBaseline` is a value like any other: the child reads it as "what
    the preset left".
*/
namespace wfg::plugin
{
    /*  WHERE A BLOCK IS ON THE CLOCK (namespace draft §123, BEI): the first
        sample of the block in Go.dot's count, and the beat clock as the tick
        thread last gave it with a count that moves when the clock does.
        Written by the host's audio thread before Tracktion's block and read by
        every proxy during it, as a live input's tap is (LiveInputTap). */
    struct BlockClock
    {
        std::int64_t blockStart = 0;
        cue::BeatClockState state;
        std::uint64_t seq = 0;

        /*  WHEN THE PLUGINS' TIME IN THIS CALLBACK IS UP (namespace draft
            §139, BHB), on the steady clock in nanoseconds: the moment the
            device asked for the block plus the callback's share of its
            period. Nought when nobody said when the block was asked for - a
            test driving blocks by hand - and each lane's own cap is then the
            only limit. */
        std::int64_t budgetEndNanos = 0;
    };

    class ProxyLane
    {
    public:
        ProxyLane();

        /** Consecutive misses at which the host marks the entry failed. */
        static constexpr int missesBeforeFailure = 8;

        /** The deadline a lane starts with; the host sets the real one. */
        static constexpr std::int64_t defaultDeadlineMicroseconds = 250;

        /*  THE ALLOWANCE (namespace draft §139, BHB). A plugin's time comes
            from the buffer it is processing: a lane may wait for a share of
            the block period, and every wait in one device callback ends a
            larger share of the period after the callback began, so that
            plugins that cannot keep up are muted and the interface itself
            never runs dry. Until 2026-11-20 the limit was 250 microseconds
            whatever the buffer - set from a gain's round trip - which a real
            reverb at the buffer a Mac opens with needs three times over
            (M86, docs/spikes/spike09-plugin-allowance.md). */
        static constexpr double laneCapFraction = 0.30;
        static constexpr double callbackFraction = 0.60;

        /*  THE QUICK FADE (CV): down to silence when the plugin cannot answer,
            and up again when it does - a millisecond at 48 kHz, a dip and
            never a click. */
        static constexpr int fadeSamples = 48;

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

        /*  A FIXED LIMIT, whatever the callback: what `--proxy-deadline-us`
            and a test ask for. */
        void setDeadlineMicroseconds (std::int64_t microseconds) noexcept;
        std::int64_t deadlineMicroseconds() const noexcept;

        /*  THE ALLOWANCE AS THE HOST COMPUTES IT (BHB): the lane's cap, and
            the least it is ever given however late in the callback it is
            asked - the old rule, so no lane has less than it had. Between
            the two, the callback's end (`BlockClock::budgetEndNanos`) cuts
            the wait short. */
        void setAllowance (std::int64_t capMicroseconds, std::int64_t floorMicroseconds) noexcept;
        std::int64_t floorMicroseconds() const noexcept;

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

        /*  Asks the child to reset the instance. NOT A BLOCK'S WORK (namespace
            draft §139, BHC): a VST3's reset is a whole deactivation and can
            take milliseconds, so the child makes it apart from its answering
            and says when it has (`resetDoneSeq`); until then the lane is
            silent, not called, and counts no late block - and an arm waits
            for it as for a state (`stateSettled`). The one reset that is not
            silent is a switch-in's own (`setEnabled`): the block passes as it
            did while the insert was out, until the plugin is ready. */
        void requestReset() noexcept;

        /*  The host's poll, message thread: true once a reset has been owed
            for longer than a state may load - a child that is stuck. */
        bool resetOverdue (std::uint32_t nowMs) noexcept;

        /*  HOW WIDE THE CUE IS HERE (2026-09-26, cue/InsertChain.h): the
            channels sent - nought, and the block passes dry - and how many are
            taken back, wider when the plugin makes a mono cue stereo. -1 for
            either is the voice's own width, as before any of this. Any thread. */
        void setShape (int feed, int back) noexcept;

        /*  A KEY FOR THIS PLUGIN (namespace draft §141, BHR): whether the cue
            this lane plays has one picked, so the plugin's sidechain input is
            to be switched on. Unkeyed - the default, and every insert before
            §141 - the plugin is laid out exactly as it always was. A change is
            made in a reset: the child lays the instance out again on its own
            thread with the lane parked, and an insert switched in meanwhile
            passes the block as it was (BHS) - never a hole, never half wet.
            Any thread but the audio one. */
        void setKeyed (bool keyed) noexcept;
        bool isKeyed() const noexcept;

        /*  How wide a sidechain the plugin offers (one or two channels), and
            how wide this lane's is laid out now - nought for none, or for a
            plugin that would not take it. Any thread. */
        int keyOffered() const noexcept;
        int keyHeld() const noexcept;

        //======================================================================
        /*  A CUE'S WHOLE STATE (the author's decision of 2026-09-25): what the
            plugin's own window changed that is not a parameter, kept per cue
            as a file, loaded onto this voice's instance before the cue may
            launch. The lane keeps which state it was asked for, which it
            holds, and the one in flight; the arm waits on `stateSettled`.

            Message thread, at an arm: this lane should hold that state - a
            file, absolute, or empty for the preset's own. */
        void wantState (const std::string& path);

        /** What this lane was last asked to hold - message thread, as `wantState`. */
        const std::string& wantedState() const noexcept { return wantedStatePath; }

        /*  The tick thread, when a cue's state changes after its arm and
            before its launch: the path follows from the message thread, but
            the launch must wait from NOW, so the count moves here. */
        void expectState() noexcept;

        /*  Any thread, the tick thread's question at every launch: nothing
            this lane was asked to load is still loading, and no reset it was
            asked for is still to be made. A lane that is not switched in, not
            called or not bound has nothing to wait for. */
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
            again - it may be what killed it. What the lane HELD is kept aside
            for `restoreState`. */
        void forgetState();

        /*  THE CUE COMES BACK AS IT WAS (the author's decision of 2026-09-26,
            CU): the relaunched child is asked for the state this lane held
            when the last one went down - unless a newer one was asked for
            meanwhile, which goes instead - and the lane is silent until the
            child holds it. Message thread, before the lane is called again. */
        void restoreState();

        /** How long a state may load before the host calls the child hung. */
        static constexpr std::uint32_t stateLoadLimitMs = 5000;

        //======================================================================
        /*  The audio thread. Sends `numChannels` channels of `numSamples`
            frames, up to the region's shape, and takes the answer in place;
            leaves the block untouched when the cue has the insert switched
            out, or passes it dry by the insert's shape (said on the insert).

            NEVER THE DRY BLOCK OTHERWISE (CU, CV): a plugin that is not there,
            not to be called, still taking a whole state or being reset, or
            late, leaves the block SILENT over a quick fade from where the last
            answer left each channel, and the first block it answers again
            fades back in.

            A REQUEST IS NEVER WRITTEN OVER (namespace draft §139, BHC). The
            child answers in the audio it was sent, so while one it was late
            for is still unanswered that audio is its own: the next block
            waits for the answer inside its own limit, throws it away - it
            answers a block long gone - and only then sends. If it does not
            come the block is late too, and nothing is sent. */
        void process (float* const* channelData, int numChannels, int numSamples) noexcept WFG_AUDIO_THREAD;

        /*  THE SAME, PLACED ON THE CLOCK (namespace draft §123, BEI): the beat
            clock put in the region's header when its count moved, and each
            piece's first sample - `blockStart` on - on the lane before the
            request, for the child's playhead. */
        void process (float* const* channelData, int numChannels, int numSamples,
                      const BlockClock* clock, std::int64_t blockStart) noexcept WFG_AUDIO_THREAD;

        /*  AND WITH A KEY (namespace draft §141): `keyChannels` of it - one or
            two, more are not sent - beside each piece in the region, for the
            plugin's sidechain. A null key, or nought channels, is no key, and
            the sidechain is fed silence; so is any key to a lane whose cue
            picked none. A key that sounds keeps the lane from its rest, as
            sound arriving does. */
        void process (float* const* channelData, int numChannels, int numSamples,
                      const BlockClock* clock, std::int64_t blockStart,
                      const float* const* key, int keyChannels) noexcept WFG_AUDIO_THREAD;

        //======================================================================
        /** Blocks sent to the child. */
        std::uint64_t blocks() const noexcept;

        /** Blocks the child answered in time. */
        std::uint64_t answered() const noexcept;

        /** Blocks the child was late for, in all - one it was still answering
            the last request through, and so was never sent, among them. */
        std::uint32_t misses() const noexcept;

        /** Late blocks since the last one that was in time. */
        std::uint32_t consecutiveMisses() const noexcept;

        /*  Blocks cut short by the callback's end before the lane's own cap
            (BHB): muted like a late one, and said, but not the plugin's
            failing - other plugins had used the callback's time - so they
            count in neither of the two above and never fail it. */
        std::uint32_t starved() const noexcept;

        /** The host, on a restart: the new child starts with a clean sheet. */
        void clearMisses() noexcept;

        /*  AT REST (namespace draft §139, BHF). A lane that has played a cue
            and has had silence in and silence out for ten seconds since is
            not called again until sound arrives: the cue is over and its
            tail has rung out, and nothing gives a voice's copy back when its
            run ends (§133.8), so until this every plugin that had ever played
            went on being asked for silence, a block at a time and one after
            another, for the rest of the session. Sound arriving is played
            through it in that very block - never dry - and a lane armed and
            not yet heard does not rest at all: a cue at standby finds its
            plugin as warm as it always did. Any thread. */
        bool isResting() const noexcept;

        /*  WHERE A BUFFER'S TIME WENT (namespace draft §148): the longest a
            request of this lane waited for the child's worker, the longest
            the worker then took over one, and the processor it was on (one
            more than its number, nought when not noted) - since this was last
            asked, and nought again once it has been. The host's poll, message
            thread; nought everywhere while unbound. */
        struct Worst
        {
            std::uint32_t pickupMicros = 0;
            std::uint32_t workMicros = 0;
            std::uint32_t core = 0;
        };

        Worst takeWorst() noexcept;

        /*  WHOSE INSERT THIS LANE PLAYS (namespace draft §146): the Fx the last
            arm or hold gave it, empty while it is lent to nobody. The message
            thread's, and the parent's alone - the child never reads it. What
            a plugin's window follows to the voice playing its cue. */
        void setOwner (std::string fxId) { ownerId = std::move (fxId); }
        const std::string& owner() const noexcept { return ownerId; }

        /*  THE TAP, ASKED FOR (namespace draft §146): the child keeps this
            lane's blocks for the next second of its audio, and here is where
            they are. Null while unbound. The message thread's, each time it
            looks; the pointers last until the region is let go of. */
        region::TapView askTap() noexcept;

        /** How long silence in and out must last before a lane rests. */
        static constexpr double restAfterSeconds = 10.0;

        /** Under this, a sample is silence for the rest's purposes: a hundred decibels down. */
        static constexpr float restThreshold = 1.0e-5f;

    private:
        void publishClock (const cue::BeatClockState& state) noexcept WFG_AUDIO_THREAD;

        std::atomic<region::Lane*> lane { nullptr };
        std::uint64_t clockSeqSent = 0;     ///< the audio thread's: the count of the clock in the header
        std::atomic<int> shapeFeed { -1 };
        std::atomic<int> shapeBack { -1 };
        region::Header* header = nullptr;
        float* audio = nullptr;
        float* keyArea = nullptr;
        int regionChannels = 0;
        int regionMaxSamples = 0;

        std::atomic<int> enabled { 0 };
        std::atomic<int> callEnabled { 1 };
        std::atomic<int> keyed { 0 };

        /*  THE FADES (CV): where the last answer left each channel, and
            whether the lane is silent now - the audio thread's own. A new cue
            on the voice starts them over through `clearRequests`, since the
            message thread asks it and the audio thread owns the rest. */
        static constexpr int maxFadedChannels = 64;
        std::array<float, maxFadedChannels> lastSample {};
        bool silenced = false;
        std::atomic<std::uint32_t> clearRequests { 0 };
        std::uint32_t clearsSeen = 0;

        void silenceFrom (float* const* channelData, int channels, int from, int numSamples) noexcept;

        /** What a relaunched child is to be given back (`restoreState`); the message thread's. */
        std::string restoreStatePath;

        /** The Fx this lane plays (§146); the message thread's. */
        std::string ownerId;
        std::atomic<std::int64_t> deadlineUs { defaultDeadlineMicroseconds };
        std::atomic<std::int64_t> floorUs { defaultDeadlineMicroseconds };

        std::atomic<float> shadow[region::maxParams];

        std::atomic<std::uint64_t> wantedStateSeq { 0 };
        std::atomic<std::uint64_t> settledStateSeq { 0 };
        std::string wantedStatePath, heldStatePath, flightStatePath;
        std::uint64_t flightStateSeq = 0;
        std::uint64_t pathStateSeq = 0;     ///< the count `wantedStatePath` belongs to
        std::uint32_t flightSentAt = 0;
        bool stateInFlight = false;

        /*  THE RESET A SWITCH-IN ASKED FOR (BHC), by its count: until that one
            is made the block passes as it did while the insert was out, so an
            insert switched in under a sounding cue is a step from dry to wet
            and no hole. Any other reset finds the insert in, and is silent. */
        std::atomic<std::uint32_t> dryWhileReset { 0 };

        /** Asks the reset and says its count; nought when there is no child to ask. */
        std::uint32_t askReset() noexcept;

        /** Since when a reset has been owed, as the host's poll saw it; the message thread's. */
        std::uint32_t resetOwedSince = 0;
        bool resetOwed = false;

        std::atomic<std::uint64_t> blockCount { 0 };
        std::atomic<std::uint64_t> answeredCount { 0 };
        std::atomic<std::uint32_t> missCount { 0 };
        std::atomic<std::uint32_t> consecutive { 0 };
        std::atomic<std::uint32_t> starvedCount { 0 };

        /*  THE REST (BHF), the audio thread's own but for the flag: how long
            in and out have both been silent, whether the lane has heard
            sound since its last reset, and what ten seconds is in samples. */
        std::int64_t quietSamples = 0;
        std::int64_t restAfterSamples = 480000;
        bool heardSinceReset = false;
        std::atomic<int> resting { 0 };
    };
}
