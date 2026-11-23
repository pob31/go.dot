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
    WHAT THE WINDOW AND A PLUGIN'S EDITING HELPER SAY TO EACH OTHER (the
    author's decision of 2026-09-25: the plugin's own window, in a helper
    process of its own, following the pick).

    One memory-mapped file per open helper, laid out by the parent (the
    desktop client, through EditorHost) and read by both as the structure
    below. It is the voice child's region in miniature and in the same
    discipline - every field a lock-free atomic or a byte array published by
    one. The helper's copy of the plugin is there to be LOOKED AT and turned,
    never heard - but since namespace draft §146 it is FED: what the voice
    playing the subject's insert was given, relayed by the parent into a ring
    here, so the plugin's own analyser and meters show the cue.

    PARENT TO HELPER. Which cue the window is about (the SUBJECT: its title,
    whether it is greyed and why, and the cue's values for every parameter),
    handed over one at a time - the parent writes a subject only when the
    helper has taken the last, and keeps the newest one waiting otherwise.
    Values moved ELSEWHERE while the window is open - an undo, the page, a
    surface - are the `live` array, which the helper reconciles against its
    own plugin a pass at a time. Whether the window is shown, a request to
    bring it forward, and leave.

    HELPER TO PARENT. Ready or failed-and-why; then a ring of events the
    parent drains on its own timer: a parameter the plugin's window moved (with
    the SUBJECT SEQUENCE it was moved under, so a turn made just before the
    pick moved still lands on the cue it was made on), the window closed by
    its own button, a key the window was given that is the show's - Space and
    Esc - and not the plugin's.

    NAMES NO JUCE TYPE, so a test can lay one out in a vector and drive both
    ends with no file and no process.
*/

#include <wfg/engine/plugin/SharedRegion.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace wfg::plugin::editor
{
    /** "GotE", read as a little-endian word. */
    constexpr std::uint32_t magic = 0x45746f47u;

    /** Bumped whenever the structure below changes shape. 3 since 2026-11-23: the tap (§146). */
    constexpr std::uint32_t version = 3;

    constexpr int maxParams = region::maxParams;
    constexpr int idChars = 64;
    constexpr int textChars = 256;
    constexpr int pathChars = 1024;

    /** Events the parent has not drained yet, at most; a power of two. */
    constexpr std::uint32_t ringSize = 1024;

    /*  THE WINDOW'S RING (namespace draft §146): 16384 frames, a third of a
        second at 48 kHz - the parent fills it every 20 ms and the helper
        empties it every 10, so it holds a stalled message thread's worth. */
    constexpr int tapRingFrames = 16384;
    constexpr int tapSlots = region::tapChannelsMax + region::keyChannelsMax;

    /** A value the helper reads as "rest where the preset left it". */
    constexpr float restsAtPreset = -1.0f;

    enum class EventKind : std::uint32_t
    {
        none = 0,
        value,          ///< `index` moved to `value`, in the plugin's own window
        windowClosed,   ///< its close button
        key             ///< `index` is a Key: the show's key, pressed in the plugin's window
    };

    enum class Key : std::uint32_t { space = 0, escape = 1 };

    struct Event
    {
        std::atomic<std::uint32_t> kind;
        std::atomic<std::uint32_t> index;
        std::atomic<float> value;
        std::atomic<std::uint32_t> subjectSeq;
    };

    /*  WHICH CUE, AND WHAT IT SAYS. Written by the parent while
        `subjectSeq == subjectTaken`, then published by bumping `subjectSeq`
        with release; the helper reads it after an acquire and answers by
        storing the sequence into `subjectTaken`. The strings are NUL-ended. */
    struct Subject
    {
        char cueId[idChars];
        char fxId[idChars];
        char title[textChars];
        char reason[textChars];

        /*  The cue's whole state for this plugin, as a file - absolute - or
            empty for the preset (the author's decision of 2026-09-25). */
        char statePath[pathChars];

        /*  Nought: the window is the cue's. One: it is greyed - the window
            hidden, and a hand goes nowhere - and `reason` says why. */
        std::atomic<std::uint32_t> greyed;

        /** How many of `values` mean something; the plugin's parameter count. */
        std::atomic<std::uint32_t> valueCount;

        /** The cue's value for each parameter, 0..1, or `restsAtPreset`. */
        std::atomic<float> values[maxParams];
    };

    struct Region
    {
        std::atomic<std::uint32_t> magic;
        std::atomic<std::uint32_t> version;
        std::atomic<std::uint32_t> layoutHash;

        //==============================================================================
        /** Parent to helper: 1 leave, keeping the plugin's state first if a hand changed it; 2 leave now. */
        std::atomic<std::uint32_t> shouldExit;

        /** Parent to helper: whether the window is on screen. */
        std::atomic<std::uint32_t> visible;

        /** Parent to helper: bumped to bring the window forward. */
        std::atomic<std::uint32_t> raiseSeq;

        std::atomic<std::uint32_t> subjectSeq;
        Subject subject;

        /*  Parent to helper: the subject's values as the tree now has them,
            tagged with the subject sequence they belong to - the helper
            reconciles against them only while that subject is the one it has
            applied, so values meant for the next cue never land on this one. */
        std::atomic<std::uint32_t> liveSeq;
        std::atomic<std::uint32_t> liveSubject;
        std::atomic<float> live[maxParams];

        /*  Parent to helper, FOR TESTS ONLY: a hand on the plugin's own
            window, which CI has none of. The helper moves parameter
            `pokeIndex` to `pokeValue` exactly as the window would, gesture
            and all; -2 is its close button. */
        std::atomic<std::uint32_t> pokeSeq;
        std::atomic<std::int32_t> pokeIndex;
        std::atomic<float> pokeValue;

        //==============================================================================
        /** Helper to parent: the plugin is up, the window made if one was asked for. */
        std::atomic<std::uint32_t> ready;

        /** Helper to parent: it could not come up, and `problem` says why. */
        std::atomic<std::uint32_t> failed;
        char problem[textChars];

        /** Helper to parent: the subject sequence it has applied. */
        std::atomic<std::uint32_t> subjectTaken;

        std::atomic<std::uint32_t> paramCount;

        /** Helper to parent: how long bringing the plugin up took, in microseconds. */
        std::atomic<std::uint32_t> loadMicros;

        /** Helper to parent: every parameter's value as the helper's plugin has it. */
        std::atomic<float> current[maxParams];

        /*  A PLUGIN'S WHOLE STATE, KEPT (the author's decision of 2026-09-25).
            The helper has written the bytes into the bundle, content-addressed,
            and says so here: which insert it was, the file's name as the cue's
            row will hold it, and every parameter's value. One in flight: the
            helper writes only while `captureAck == captureSeq`, then bumps
            `captureSeq`; the parent reads and answers with `captureAck`. It
            outlives the helper - a capture made on the way out is read after
            the process has gone. */
        std::atomic<std::uint32_t> captureSeq;
        std::atomic<std::uint32_t> captureAck;
        char captureFxId[idChars];
        char captureFile[textChars];
        std::atomic<std::uint32_t> captureCount;
        std::atomic<float> captureValues[maxParams];

        /*  THE EVENTS, one producer (the helper's message thread) and one
            consumer (the parent's timer). `ringWrite` is stored with release
            after the entry; `ringRead` is the parent's, stored with release
            after it has read the entry. Full means the parent stopped
            draining, and the helper drops the event rather than wait. */
        std::atomic<std::uint32_t> ringWrite;
        std::atomic<std::uint32_t> ringRead;
        Event ring[ringSize];

        /*  THE TAP (namespace draft §146), parent to helper: what the voice
            playing the subject's insert was given - its cue in the first
            `region::tapChannelsMax` slots, its key in the rest - and how many
            of each the latest frames carry. `tapWritten` counts every frame
            the ring has had, stored with release after them; the helper keeps
            its own place. Helper to parent, `tapBlocks`: the blocks its copy
            of the plugin has processed from it, for tests and nothing else. */
        std::atomic<std::uint64_t> tapWritten;
        std::atomic<std::uint32_t> tapChannels;
        std::atomic<std::uint32_t> tapKeyChannels;
        std::atomic<std::uint32_t> tapBlocks;
        float tap[tapSlots][tapRingFrames];
    };

    static_assert (std::atomic<std::uint32_t>::is_always_lock_free
                     && std::atomic<std::int32_t>::is_always_lock_free
                     && std::atomic<float>::is_always_lock_free,
                   "two processes share these: a locking atomic would lock nothing across them");

    static_assert ((ringSize & (ringSize - 1)) == 0, "the ring wraps with a mask");

    constexpr std::size_t regionBytes() noexcept
    {
        return region::alignUp (sizeof (Region));
    }

    /** FNV-1a over everything the layout depends on. */
    constexpr std::uint32_t layoutHash() noexcept
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
        mix (static_cast<std::uint32_t> (sizeof (Region)));
        mix (static_cast<std::uint32_t> (sizeof (Subject)));
        mix (static_cast<std::uint32_t> (sizeof (Event)));
        mix (static_cast<std::uint32_t> (maxParams));
        mix (ringSize);
        mix (static_cast<std::uint32_t> (tapRingFrames));
        mix (static_cast<std::uint32_t> (tapSlots));
        return hash;
    }

    inline bool looksValid (const Region& r) noexcept
    {
        return r.magic.load (std::memory_order_relaxed) == magic
            && r.version.load (std::memory_order_relaxed) == version
            && r.layoutHash.load (std::memory_order_relaxed) == layoutHash();
    }

    //==============================================================================
    /*  THE TAP'S RELAY (namespace draft §146), the parent's, on its window's
        timer: what a voice's lane has kept since the cursor, into the
        window's ring. A lane it was not following - the pick moved, the cue
        was lent another copy - starts the cursor where the lane stands, so
        nothing older than now is heard. Never more than half the lane's ring
        is taken in one look: the child may be writing the half after it. */
    struct TapCursor
    {
        const region::Lane* lane = nullptr;
        std::uint64_t read = 0;
    };

    inline int relayTap (const region::Lane& lane, const float* laneRing, TapCursor& cursor, Region& into) noexcept
    {
        const auto written = lane.tapWritten.load (std::memory_order_acquire);

        if (cursor.lane != &lane || written < cursor.read)
        {
            cursor.lane = &lane;
            cursor.read = written;
            return 0;
        }

        constexpr auto most = static_cast<std::uint64_t> (region::tapFrames / 2);
        const auto from = written - cursor.read > most ? written - most : cursor.read;
        const auto frames = static_cast<int> (written - from);

        if (frames <= 0)
            return 0;

        const auto at = into.tapWritten.load (std::memory_order_relaxed);

        for (int slot = 0; slot < tapSlots; ++slot)
        {
            const auto* source = laneRing + static_cast<std::size_t> (slot) * static_cast<std::size_t> (region::tapFrames);
            auto* target = into.tap[slot];

            for (int n = 0; n < frames; ++n)
                target[(at + static_cast<std::uint64_t> (n)) % static_cast<std::uint64_t> (tapRingFrames)]
                    = source[(from + static_cast<std::uint64_t> (n)) % static_cast<std::uint64_t> (region::tapFrames)];
        }

        into.tapChannels.store (lane.tapChannels.load (std::memory_order_relaxed), std::memory_order_relaxed);
        into.tapKeyChannels.store (lane.tapKeyChannels.load (std::memory_order_relaxed), std::memory_order_relaxed);
        into.tapWritten.store (at + static_cast<std::uint64_t> (frames), std::memory_order_release);
        cursor.read = written;
        return frames;
    }

    /*  AND ITS OTHER END, the helper's: up to `maxFrames` of what the ring
        has had since `read`, a slot into each of `out` (`tapSlots` of them),
        and how many of the cue's channels and of the key's they carry. A
        helper that fell more than half the ring behind takes only the newest
        half. */
    inline int takeTap (const Region& r, std::uint64_t& read, float* const* out, int maxFrames,
                        int& channels, int& keyChannels) noexcept
    {
        const auto written = r.tapWritten.load (std::memory_order_acquire);

        if (written < read)
            read = written;

        constexpr auto most = static_cast<std::uint64_t> (tapRingFrames / 2);

        if (written - read > most)
            read = written - most;

        const auto frames = static_cast<int> (std::min<std::uint64_t> (written - read, static_cast<std::uint64_t> (maxFrames > 0 ? maxFrames : 0)));
        channels = static_cast<int> (r.tapChannels.load (std::memory_order_relaxed));
        keyChannels = static_cast<int> (r.tapKeyChannels.load (std::memory_order_relaxed));

        for (int slot = 0; slot < tapSlots; ++slot)
            for (int n = 0; n < frames; ++n)
                out[slot][n] = r.tap[slot][(read + static_cast<std::uint64_t> (n)) % static_cast<std::uint64_t> (tapRingFrames)];

        read += static_cast<std::uint64_t> (frames);
        return frames;
    }

    //==============================================================================
    /*  THE RING'S TWO ENDS, here so both sides and a test use the same
        arithmetic. The indices only grow; the mask picks the slot. */
    inline bool push (Region& r, EventKind kind, std::uint32_t index, float value, std::uint32_t subjectSeq) noexcept
    {
        const auto write = r.ringWrite.load (std::memory_order_relaxed);
        const auto read = r.ringRead.load (std::memory_order_acquire);

        if (write - read >= ringSize)
            return false;

        auto& slot = r.ring[write & (ringSize - 1)];
        slot.kind.store (static_cast<std::uint32_t> (kind), std::memory_order_relaxed);
        slot.index.store (index, std::memory_order_relaxed);
        slot.value.store (value, std::memory_order_relaxed);
        slot.subjectSeq.store (subjectSeq, std::memory_order_relaxed);
        r.ringWrite.store (write + 1, std::memory_order_release);
        return true;
    }

    struct Popped
    {
        EventKind kind = EventKind::none;
        std::uint32_t index = 0;
        float value = 0.0f;
        std::uint32_t subjectSeq = 0;
    };

    inline bool pop (Region& r, Popped& out) noexcept
    {
        const auto read = r.ringRead.load (std::memory_order_relaxed);
        const auto write = r.ringWrite.load (std::memory_order_acquire);

        if (read == write)
            return false;

        const auto& slot = r.ring[read & (ringSize - 1)];
        out.kind = static_cast<EventKind> (slot.kind.load (std::memory_order_relaxed));
        out.index = slot.index.load (std::memory_order_relaxed);
        out.value = slot.value.load (std::memory_order_relaxed);
        out.subjectSeq = slot.subjectSeq.load (std::memory_order_relaxed);
        r.ringRead.store (read + 1, std::memory_order_release);
        return true;
    }
}
