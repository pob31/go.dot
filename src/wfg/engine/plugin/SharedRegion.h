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
    constexpr std::uint32_t version = 2;

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

        /** Parent to child: some lane is switched in, so the worker spins hot;
            nought lets it sleep between polls (plan decision 14). */
        std::atomic<std::uint32_t> wantSpin;

        /** Child to parent, once: the catalogue file beside the region is
            complete (PR 9a.7); the test child never writes one. */
        std::atomic<std::uint32_t> catalogueReady;

        /** What the plugin declares, uncompensated (§17.6). */
        std::atomic<std::uint32_t> latencySamples;

        /** How many parameters the instances have, at most `maxParams`. */
        std::atomic<std::uint32_t> paramCount;

        /** The child's sentence, NUL-terminated, written before `childFailed`. */
        char problem[problemChars];

        /** Every parameter's value after the preset was applied: what a value
            a cue does not set rests at (§17.4). Written before `childReady`. */
        std::atomic<float> baseline[maxParams];
    };

    struct Lane
    {
        /** Bumped by the parent with release once the block is in `audio`. */
        std::atomic<std::uint64_t> requestSeq;

        /** Stored by the child with release once its answer is in `audio`. */
        std::atomic<std::uint64_t> responseSeq;

        std::atomic<std::uint32_t> numChannels;
        std::atomic<std::uint32_t> numSamples;

        /** Bumped by the parent after a parameter store; the child applies
            every value whose revision it has not seen. */
        std::atomic<std::uint32_t> paramRevision;

        /** Bumped by the parent at an arm; the child resets the instance
            before the next block so the previous cue's tail is not in it. */
        std::atomic<std::uint32_t> resetSeq;

        /*  A CUE'S WHOLE STATE, loaded onto this lane's instance before the cue
            may launch (the author's decision of 2026-09-25). One in flight:
            the parent writes `statePath` - absolute, empty for the preset's
            own state - only while `stateDoneSeq` has caught up, then stores
            `stateRequestSeq` with release. The child loads it on its message
            thread with the lane parked (answered dry, never missed), puts the
            lane's values back on top, and answers with the sequence in
            `stateDoneSeq`, having written first whether it failed, why, and
            how long it took. */
        std::atomic<std::uint64_t> stateRequestSeq;
        std::atomic<std::uint64_t> stateDoneSeq;
        std::atomic<std::uint32_t> stateFailed;
        std::atomic<std::uint32_t> stateLoadMicros;
        char statePath[pathChars];
        char stateProblem[problemChars];

        /** Normalised 0..1, or `useBaseline`. */
        std::atomic<float> params[maxParams];

        /*  The audio follows this structure at `audioOf`, channel-major:
            `channels * maxSamples` floats, of which `numChannels * numSamples`
            are the block. */
    };

    static_assert (std::atomic<std::uint64_t>::is_always_lock_free
                     && std::atomic<std::uint32_t>::is_always_lock_free
                     && std::atomic<float>::is_always_lock_free,
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
                        + static_cast<std::size_t> (channels) * static_cast<std::size_t> (maxSamples) * sizeof (float));
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
