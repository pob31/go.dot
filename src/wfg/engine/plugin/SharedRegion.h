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
    THE SHARED REGION between the engine and a plugin's child process (Phase 9a,
    §17.6, decision AF): one memory-mapped file per plugin of the set, laid out
    once by the parent and read by both sides as the structures below.

    WHAT CROSSES THE BOUNDARY, AND ONLY THAT. A header the child fills in when
    it is up - ready, failed and why, the plugin's latency and parameter count,
    every parameter's baseline - and one lane per voice: a request and a
    response sequence, the block's shape, the parameter values with a revision
    the child watches, a reset request, and the audio itself. Whether a lane is
    switched in, how many blocks it missed, the deadline - all of that is the
    parent's and lives in ProxyLane, not here; the child is told only whether
    any lane wants it awake (`wantSpin`), which is what its spin policy reads.

    EVERY FIELD IS A LOCK-FREE ATOMIC, asserted rather than assumed. The audio
    thread publishes a request with a release store and spins on the response
    with acquire loads; a locking atomic would put a kernel call on that thread
    and break PRD §4.2. spike 07 measured the shape at 0.9 µs a round trip.

    THE LAYOUT IS THE PARENT'S CHOICE AND THE CHILD CHECKS IT. Channels, the
    widest block and the lane count are fixed when the region is made, and a
    hash of them and of these structures' sizes sits in the header: a child
    built from a different revision of this file refuses the region rather
    than reading another layout as this one.

    NAMES NO JUCE TYPE. The mapping itself is JUCE's (MemoryMappedFile) and is
    made by ProxyHost and the child; this header is what both cast it to, so a
    test can lay a region out in a std::vector<char> and drive a lane with no
    file and no process.
*/

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace wfg::plugin::region
{
    /** "Gdot", read as a little-endian word. */
    constexpr std::uint32_t magic = 0x746f6447u;

    /** Bumped whenever a structure below changes shape. */
    /*  3 since 2026-09-26: the plugin's main widths and the layout in words in
        the header, the latency after a cue's state on each lane. 4 since
        2026-10-22: whether the worker is in the interface's audio workgroup.
        5 since 2026-10-27: the beat clock in the header and each block's first
        sample on its lane, for the plugins' playhead (namespace draft §123).
        6 since 2026-11-20: a reset is answered (`resetDoneSeq`), so the parent
        can wait for it without calling the lane (namespace draft §139).
        7 since 2026-11-22: a key beside the audio on every lane, and whether
        the plugin's sidechain input is to carry it (namespace draft §141).
        8 since 2026-11-23: what a lane was given, kept a moment for the
        plugin's window to hear (the tap, namespace draft §146). */
    constexpr std::uint32_t version = 8;

    /*  THE WIDEST KEY A LANE CARRIES (namespace draft §141, BHT): one channel
        or two. A wider input or cue is folded to two before it is sent. */
    constexpr int keyChannelsMax = 2;

    /*  THE TAP (namespace draft §146, the author 2026-11-23: the plugin's
        window is a second copy of it that never heard a block, so its
        analyser and its meters showed nothing): what a lane was given - at
        most two channels of the cue, and its key - kept in a ring after the
        key, while the parent asks for it. 4096 frames is 85 ms at 48 kHz,
        four of the parent's 20 ms looks. */
    constexpr int tapChannelsMax = 2;
    constexpr int tapFrames = 4096;

    /** The most parameters a plugin may expose through the proxy; a plugin
        with more has the rest neither published nor written. */
    constexpr int maxParams = 1024;

    /** Room for the child's one sentence about why it is not up. */
    constexpr int problemChars = 256;

    /** Room for the path of a cue's state file. */
    constexpr int pathChars = 1024;

    /** A parameter value the child reads as "use the baseline" (§17.4). */
    constexpr float useBaseline = -1.0f;

    //==============================================================================
    struct Header
    {
        std::atomic<std::uint32_t> magic;
        std::atomic<std::uint32_t> version;
        std::atomic<std::uint32_t> layoutHash;

        /** The shape the region was laid out for; every lane's audio is
            `channels * maxSamples` floats. */
        std::atomic<std::uint32_t> channels;
        std::atomic<std::uint32_t> maxSamples;
        std::atomic<std::uint32_t> lanes;

        std::atomic<std::uint32_t> sampleRate;
        std::atomic<std::uint32_t> blockSize;

        /** Parent to child: leave. */
        std::atomic<std::uint32_t> childShouldExit;

        /** Child to parent: every instance is up and the worker is polling. */
        std::atomic<std::uint32_t> childReady;

        /** Child to parent: it could not come up, and `problem` says why.
            Stored with release after the sentence is written. */
        std::atomic<std::uint32_t> childFailed;

        /*  Parent to child: some lane is switched in, so the worker looks for
            requests a tenth of a millisecond apart; nought lets it look once a
            millisecond. The name is from when it spun without pause (plan
            decision 14, until namespace draft §139, BHG). */
        std::atomic<std::uint32_t> wantSpin;

        /** Child to parent, once: the catalogue file beside the region is
            complete (PR 9a.7); the test child never writes one. */
        std::atomic<std::uint32_t> catalogueReady;

        /** What the plugin declares, uncompensated (§17.6). */
        std::atomic<std::uint32_t> latencySamples;

        /** How many parameters the instances have, at most `maxParams`. */
        std::atomic<std::uint32_t> paramCount;

        /*  The plugin's main input and output widths as it took them
            (2026-09-26), written before `childReady`: what decides whether a
            cue of a given width goes through it (LaneMapping.h). */
        std::atomic<std::uint32_t> inputs;
        std::atomic<std::uint32_t> outputs;

        /*  HOW WIDE A SIDECHAIN INPUT THE PLUGIN OFFERS (namespace draft §141,
            BHR): one or two channels, or nought for a plugin that has none or
            will not switch one on beside the main buses it took. Asked once,
            when the child makes its instances, and written before
            `childReady`. */
        std::atomic<std::uint32_t> keyInputs;

        /*  Child to parent: one while the worker is in the audio interface's
            workgroup the parent shares (macOS, 2026-10-22; WorkgroupShare.h),
            nought otherwise - and always nought elsewhere. */
        std::atomic<std::uint32_t> inWorkgroup;

        /*  THE BEAT CLOCK (namespace draft §123, BEI), parent to child: the
            numbers of `cue::BeatClock` and the Go.dot sample its anchor falls
            on, which the child's playhead reads a block's tempo and beat from.
            Written under `gridSeq` as a seqlock: odd while the parent writes,
            even once the numbers hold, so a child reading an odd count or one
            that moved under it reads again. `gridPlaying` is the transport a
            plugin sees. */
        std::atomic<std::uint64_t> gridSeq;
        std::atomic<std::uint64_t> gridAnchorSample;
        std::atomic<double> gridAnchorBeat;
        std::atomic<double> gridBpmFrom;
        std::atomic<double> gridBpmTo;
        std::atomic<double> gridRampStart;
        std::atomic<double> gridRampLength;
        std::atomic<double> gridShape;
        std::atomic<std::uint32_t> gridBeatsPerBar;
        std::atomic<std::uint32_t> gridPlaying;

        /** The child's sentence, NUL-terminated, written before `childFailed`. */
        char problem[problemChars];

        /** The layout in words - "stereo in, stereo out" - written before `childReady`. */
        char layout[problemChars];

        /** Every parameter's value after the preset was applied: what a value
            a cue does not set rests at (§17.4). Written before `childReady`. */
        std::atomic<float> baseline[maxParams];
    };

    struct Lane
    {
        /*  Bumped by the parent with release once the block is in `audio` -
            and only once the last request has been answered (namespace draft
            §139, BHC): while `responseSeq` is behind, the audio is the child's
            to write its answer into, and the parent neither reads nor writes
            it. */
        std::atomic<std::uint64_t> requestSeq;

        /** Stored by the child with release once its answer is in `audio`. A
            new child starts with it at `requestSeq`: what the last one was
            asked and never answered is nobody's block now. */
        std::atomic<std::uint64_t> responseSeq;

        std::atomic<std::uint32_t> numChannels;
        std::atomic<std::uint32_t> numSamples;

        /** Bumped by the parent after a parameter store; the child applies
            every value whose revision it has not seen. */
        std::atomic<std::uint32_t> paramRevision;

        /** Bumped by the parent at an arm; the child resets the instance so
            the previous cue's tail is not in it. */
        std::atomic<std::uint32_t> resetSeq;

        /*  THE RESET, ANSWERED (namespace draft §139, BHC): the count the
            child last reset the instance for, stored with release once it has.
            A reset can take milliseconds - a VST3's is a whole deactivation -
            so it is made on the child's message thread with the lane parked,
            as a state is loaded, and until this has caught up with `resetSeq`
            the parent neither calls the lane nor counts a late block: it is
            silent, as while a state loads. A new child starts with the two
            equal, its instances being fresh. */
        std::atomic<std::uint32_t> resetDoneSeq;

        /*  A KEY FOR THIS LANE'S PLUGIN (namespace draft §141, BHR). Parent to
            child, `keyWanted`: one when the cue lent this copy has a key
            picked, so the plugin's sidechain input is to be switched on;
            stored before `resetSeq` moves, and acted on in that reset - the
            instance laid out again on the child's message thread with the
            lane parked, the one place a plugin may be. Child to parent,
            `keyHeld`: how wide the sidechain input is now, nought when it is
            off or the plugin would not take it, stored before `resetDoneSeq`.
            And each request's `keyChannels`, stored before `requestSeq`: how
            many channels of key the block carries in its key area - nought,
            and the sidechain is fed silence. */
        std::atomic<std::uint32_t> keyWanted;
        std::atomic<std::uint32_t> keyHeld;
        std::atomic<std::uint32_t> keyChannels;

        /*  WHERE THIS PIECE STARTS in Go.dot's count of samples (namespace
            draft §123, BEI), stored before the request: what the playhead
            places the beat against. */
        std::atomic<std::uint64_t> blockStartSample;

        /*  THE TAP (namespace draft §146). Parent to child, `tapPulse`: moved
            by the parent each time it looks while a plugin's window wants
            this lane; the child keeps the lane's blocks while it has moved in
            the last second of the lane's audio, and nought costs it nothing.
            Child to parent: `tapWritten`, how many frames the ring has ever
            had, stored with release once they are in it; `tapChannels` and
            `tapKeyChannels`, how many of the cue's channels and of its key
            the last block carried, stored before it. */
        std::atomic<std::uint32_t> tapPulse;
        std::atomic<std::uint32_t> tapChannels;
        std::atomic<std::uint32_t> tapKeyChannels;
        std::atomic<std::uint64_t> tapWritten;

        /*  A CUE'S WHOLE STATE, loaded onto this lane's instance before the cue
            may launch (the author's decision of 2026-09-25). One in flight:
            the parent writes `statePath` - absolute, empty for the preset's
            own state - only while `stateDoneSeq` has caught up, then stores
            `stateRequestSeq` with release. The child loads it on its message
            thread with the lane parked (answered dry, never missed - though
            the parent, silent while a state loads, does not call it), puts the
            lane's values back on top, and answers with the sequence in
            `stateDoneSeq`, having written first whether it failed, why, and
            how long it took. */
        std::atomic<std::uint64_t> stateRequestSeq;
        std::atomic<std::uint64_t> stateDoneSeq;
        std::atomic<std::uint32_t> stateFailed;
        std::atomic<std::uint32_t> stateLoadMicros;

        /** What the plugin declares after this lane's last state, uncompensated:
            a state can change a look-ahead (2026-09-26). Written before `stateDoneSeq`. */
        std::atomic<std::uint32_t> latencySamples;
        char statePath[pathChars];
        char stateProblem[problemChars];

        /** Normalised 0..1, or `useBaseline`. */
        std::atomic<float> params[maxParams];

        /*  The audio follows this structure at `audioOf`, channel-major:
            `channels * maxSamples` floats, of which `numChannels * numSamples`
            are the block; the key after it at `keyOf`, `keyChannelsMax *
            maxSamples` floats, of which `keyChannels * numSamples` are the
            block's (§141); and the tap's ring after that at `tapOf`,
            `(tapChannelsMax + keyChannelsMax) * tapFrames` floats, the cue's
            channels first and the key's after them (§146). */
    };

    static_assert (std::atomic<std::uint64_t>::is_always_lock_free
                     && std::atomic<std::uint32_t>::is_always_lock_free
                     && std::atomic<float>::is_always_lock_free
                     && std::atomic<double>::is_always_lock_free,
                   "the whole design depends on these being lock-free: a locking atomic"
                   " would put a kernel call on the audio thread and break PRD 4.2");

    static_assert (sizeof (Lane) % alignof (float) == 0,
                   "the audio follows the lane structure directly");

    //==============================================================================
    /** Cache-line alignment for every lane, so two voices never share one. */
    constexpr std::size_t alignUp (std::size_t bytes) noexcept
    {
        return (bytes + 63u) & ~static_cast<std::size_t> (63u);
    }

    constexpr std::size_t headerBytes() noexcept
    {
        return alignUp (sizeof (Header));
    }

    constexpr std::size_t laneStride (int channels, int maxSamples) noexcept
    {
        return alignUp (sizeof (Lane)
                        + (static_cast<std::size_t> (channels + keyChannelsMax) * static_cast<std::size_t> (maxSamples)
                             + static_cast<std::size_t> (tapChannelsMax + keyChannelsMax) * static_cast<std::size_t> (tapFrames))
                            * sizeof (float));
    }

    constexpr std::size_t regionBytes (int channels, int maxSamples, int lanes) noexcept
    {
        return headerBytes() + static_cast<std::size_t> (lanes) * laneStride (channels, maxSamples);
    }

    inline Header* headerOf (void* base) noexcept
    {
        return static_cast<Header*> (base);
    }

    inline Lane* laneAt (void* base, int channels, int maxSamples, int index) noexcept
    {
        return reinterpret_cast<Lane*> (static_cast<char*> (base) + headerBytes()
                                        + static_cast<std::size_t> (index) * laneStride (channels, maxSamples));
    }

    inline float* audioOf (Lane* lane) noexcept
    {
        return reinterpret_cast<float*> (reinterpret_cast<char*> (lane) + sizeof (Lane));
    }

    /** The lane's key, after its audio (namespace draft §141). */
    inline float* keyOf (Lane* lane, int channels, int maxSamples) noexcept
    {
        return audioOf (lane) + static_cast<std::size_t> (channels) * static_cast<std::size_t> (maxSamples);
    }

    /** The lane's tap ring, after its key (namespace draft §146): slot `c` is `tapFrames` floats at `c * tapFrames`. */
    inline float* tapOf (Lane* lane, int channels, int maxSamples) noexcept
    {
        return keyOf (lane, channels, maxSamples) + static_cast<std::size_t> (keyChannelsMax) * static_cast<std::size_t> (maxSamples);
    }

    inline const float* tapOf (const Lane* lane, int channels, int maxSamples) noexcept
    {
        return tapOf (const_cast<Lane*> (lane), channels, maxSamples);
    }

    /*  ONE BLOCK INTO THE TAP, by the child, before the plugin writes its
        answer over it: the first `tapChannelsMax` of the cue's channels, then
        the key's. A copy into memory both sides already have - no lock, no
        allocation, no call (PRD §4.2). */
    inline void tapBlock (Lane* lane, int channels, int maxSamples, int numChannels, int numSamples, int keyChannels) noexcept
    {
        if (numSamples <= 0 || numSamples > tapFrames)
            return;

        const auto cue = numChannels < tapChannelsMax ? numChannels : tapChannelsMax;
        const auto key = keyChannels < keyChannelsMax ? keyChannels : keyChannelsMax;
        const auto written = lane->tapWritten.load (std::memory_order_relaxed);
        const auto at = static_cast<int> (written % static_cast<std::uint64_t> (tapFrames));
        const auto first = numSamples < tapFrames - at ? numSamples : tapFrames - at;
        auto* ring = tapOf (lane, channels, maxSamples);

        const auto copy = [ring, at, first, numSamples] (const float* from, int slot) noexcept
        {
            auto* into = ring + static_cast<std::size_t> (slot) * static_cast<std::size_t> (tapFrames);

            for (int n = 0; n < first; ++n)
                into[at + n] = from[n];

            for (int n = first; n < numSamples; ++n)
                into[n - first] = from[n];
        };

        for (int c = 0; c < cue; ++c)
            copy (audioOf (lane) + static_cast<std::size_t> (c) * static_cast<std::size_t> (maxSamples), c);

        for (int c = 0; c < key; ++c)
            copy (keyOf (lane, channels, maxSamples) + static_cast<std::size_t> (c) * static_cast<std::size_t> (maxSamples),
                  tapChannelsMax + c);

        lane->tapChannels.store (static_cast<std::uint32_t> (cue < 0 ? 0 : cue), std::memory_order_relaxed);
        lane->tapKeyChannels.store (static_cast<std::uint32_t> (key < 0 ? 0 : key), std::memory_order_relaxed);
        lane->tapWritten.store (written + static_cast<std::uint64_t> (numSamples), std::memory_order_release);
    }

    /*  A LANE'S TAP AS THE PARENT SEES IT: the lane, and its ring. */
    struct TapView
    {
        const Lane* lane = nullptr;
        const float* ring = nullptr;
    };

    /*  WHETHER THE CHILD KEEPS A LANE'S BLOCKS: while the parent's pulse has
        moved within the last second of the lane's own audio. One of these a
        lane, the child's, read on its worker with nothing but loads. */
    struct TapGate
    {
        std::uint32_t seen = 0;
        std::int64_t quietSamples = std::int64_t { 1 } << 40;

        bool open (const Lane& lane, int numSamples, int sampleRate) noexcept
        {
            if (const auto pulse = lane.tapPulse.load (std::memory_order_relaxed); pulse != seen)
            {
                seen = pulse;
                quietSamples = 0;
            }
            else if (quietSamples < (std::int64_t { 1 } << 40))
            {
                quietSamples += numSamples;
            }

            return quietSamples < static_cast<std::int64_t> (sampleRate > 0 ? sampleRate : 48000);
        }
    };

    /** FNV-1a over everything a layout depends on. */
    constexpr std::uint32_t layoutHashFor (int channels, int maxSamples, int lanes) noexcept
    {
        std::uint32_t hash = 2166136261u;

        const auto mix = [&hash] (std::uint32_t value)
        {
            for (int i = 0; i < 4; ++i)
            {
                hash ^= (value >> (8 * i)) & 0xffu;
                hash *= 16777619u;
            }
        };

        mix (version);
        mix (static_cast<std::uint32_t> (sizeof (Header)));
        mix (static_cast<std::uint32_t> (sizeof (Lane)));
        mix (static_cast<std::uint32_t> (maxParams));
        mix (static_cast<std::uint32_t> (keyChannelsMax));
        mix (static_cast<std::uint32_t> (tapChannelsMax));
        mix (static_cast<std::uint32_t> (tapFrames));
        mix (static_cast<std::uint32_t> (channels));
        mix (static_cast<std::uint32_t> (maxSamples));
        mix (static_cast<std::uint32_t> (lanes));
        return hash;
    }

    /** What a child checks before it believes a region. */
    inline bool looksValid (const Header& header) noexcept
    {
        const auto channels = static_cast<int> (header.channels.load (std::memory_order_relaxed));
        const auto samples = static_cast<int> (header.maxSamples.load (std::memory_order_relaxed));
        const auto lanes = static_cast<int> (header.lanes.load (std::memory_order_relaxed));

        return header.magic.load (std::memory_order_relaxed) == magic
            && header.version.load (std::memory_order_relaxed) == version
            && channels > 0 && samples > 0 && lanes > 0
            && header.layoutHash.load (std::memory_order_relaxed) == layoutHashFor (channels, samples, lanes);
    }
}
