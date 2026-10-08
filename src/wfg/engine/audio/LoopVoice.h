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
    ONE SLOT'S LOOP POINTS, MOVED WHILE IT PLAYS (namespace draft §33).

    A slice's in and out are the clip's loop range, which is on Tracktion's
    restart list: written, it rebuilds the graph and starts the clip's reader
    again. So a loop end dragged under a sounding bed is not written to the
    clip. The Runner decides, on the tick thread, where in the file the move
    lands - one launch horizon ahead, as it places a boundary - and posts it
    here as a SEGMENT: from this position of the slot's reader on, the file is
    at this second, plays on to the new out and loops the new in to out. The
    reader (patch 0002's UnrolledLoopReader) asks for the segments at every
    read.

    THE LATEST TWO ARE KEPT: the reader needs the one before to fade out of
    when the latest is a jump, and for what it reads before the latest's start.

    TWO WRITERS AND READERS THAT NEVER WAIT. The tick thread posts; the message
    thread clears the slot at every arm, because the next cue's reader starts
    again at its own in-point and a segment of the last cue's would be read
    against it. The writers share a mutex, which neither of their threads is
    forbidden (PRD §4.2 is the audio thread's). The readers - the audio thread,
    and a reader built on the message thread priming itself - take a seqlock
    and never wait: a read that meets a write in progress says so, and the
    reader keeps the segments it already had for one more read.

    A READER THAT MEETS A SEGMENT LATE - after it has read past its start, the
    horizon having been shorter than how far ahead a stretcher reads - applies
    it from where it is and says so here, and the Runner moves its clock to
    match (`adoption`).

    Vendor-free: the adaptor Tracktion calls is in AudioHost.cpp.
*/

#include <wfg/engine/rt/RtCheck.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>

namespace wfg::audio
{
    class LoopVoice
    {
    public:
        /*  From the reader's position `from` on (seconds of the file, counted
            on through every pass as the reader counts), the file is at
            `fileAt`, plays to `loopOut` and loops `loopIn`..`loopOut`. A
            `crossfade` is a jump: what was read before is faded out over it. */
        struct Segment
        {
            std::uint64_t generation = 0;
            double from = 0.0;
            double fileAt = 0.0;
            double loopIn = 0.0;
            double loopOut = 0.0;
            double crossfade = 0.0;

            /*  WHICH WAY THE FILE PLAYS from `from` on (namespace draft §41, WX):
                +1 forwards, -1 backwards - the reader still counts forwards, at
                the speed's size - and whether the loop bounces between its
                points. A loopOut not after loopIn is no loop. */
            int direction = 1;
            bool pingPong = false;
        };

        /*  What a reader did with a segment it met late: applied it from
            `from`, with the file at `fileAt` there. */
        struct Adoption
        {
            std::uint64_t generation = 0;
            double from = 0.0;
            double fileAt = 0.0;
        };

        LoopVoice() = default;
        LoopVoice (const LoopVoice&) = delete;
        LoopVoice& operator= (const LoopVoice&) = delete;

        /*  Tick thread: a move, which becomes the latest; the one that was the
            latest is kept before it. Returns the move's generation, never
            nought. */
        std::uint64_t post (Segment segment)
        {
            const std::lock_guard<std::mutex> lock { writers };

            segment.generation = ++generations;

            Held next;
            next.count = std::min (held.count + 1, 2);
            next.segments[0] = held.count > 0 ? held.segments[held.count - 1] : Segment {};
            next.segments[next.count - 1] = segment;

            publish (next);
            return segment.generation;
        }

        /*  Message thread, at every arm: the slot plays its clip's own loop
            again. Generations go on counting, so a late answer about a move
            from before the clear is never taken for one after it. */
        void clear()
        {
            const std::lock_guard<std::mutex> lock { writers };
            publish (Held {});
        }

        /*  Any thread, never waiting: the segments in force, the earlier first,
            into `two`; how many, or -1 when a write was in progress - keep what
            you had. The latest carries what its reader adopted, if it did. */
        int read (Segment* two) const noexcept WFG_AUDIO_THREAD
        {
            const auto before = sequence.load (std::memory_order_acquire);

            if ((before & 1u) != 0)
                return -1;

            const auto count = shared.count.load (std::memory_order_relaxed);

            for (int i = 0; i < count && i < 2; ++i)
                two[i] = shared.segments[i].load();

            std::atomic_thread_fence (std::memory_order_acquire);

            if (sequence.load (std::memory_order_relaxed) != before)
                return -1;

            if (count > 0)
                if (const auto took = adoption())
                    if (took->generation == two[count - 1].generation)
                    {
                        two[count - 1].from = took->from;
                        two[count - 1].fileAt = took->fileAt;
                    }

            return count;
        }

        /*  The reader's thread: it met `generation` late and applied it from
            `from`. One writer - the slot's reader - so a sequence of its own. */
        void adopt (std::uint64_t generation, double from, double fileAt) noexcept WFG_AUDIO_THREAD
        {
            const auto at = adoptedSequence.load (std::memory_order_relaxed);
            adoptedSequence.store (at + 1, std::memory_order_relaxed);
            std::atomic_thread_fence (std::memory_order_release);
            adoptedGeneration.store (generation, std::memory_order_relaxed);
            adoptedFrom.store (from, std::memory_order_relaxed);
            adoptedFileAt.store (fileAt, std::memory_order_relaxed);
            adoptedSequence.store (at + 2, std::memory_order_release);
        }

        /*  Any thread: the last move a reader met late, if any. */
        std::optional<Adoption> adoption() const noexcept WFG_AUDIO_THREAD
        {
            const auto before = adoptedSequence.load (std::memory_order_acquire);

            if ((before & 1u) != 0)
                return std::nullopt;

            Adoption took { adoptedGeneration.load (std::memory_order_relaxed),
                            adoptedFrom.load (std::memory_order_relaxed),
                            adoptedFileAt.load (std::memory_order_relaxed) };

            std::atomic_thread_fence (std::memory_order_acquire);

            if (adoptedSequence.load (std::memory_order_relaxed) != before || took.generation == 0)
                return std::nullopt;

            return took;
        }

    private:
        struct Held
        {
            int count = 0;
            Segment segments[2];
        };

        // One segment's numbers, each an atomic so a read across a write is a
        // discarded read and never a torn one.
        struct SharedSegment
        {
            std::atomic<std::uint64_t> generation { 0 };
            std::atomic<double> from { 0.0 }, fileAt { 0.0 }, loopIn { 0.0 }, loopOut { 0.0 }, crossfade { 0.0 };
            std::atomic<int> direction { 1 };
            std::atomic<bool> pingPong { false };

            void store (const Segment& s) noexcept
            {
                generation.store (s.generation, std::memory_order_relaxed);
                from.store (s.from, std::memory_order_relaxed);
                fileAt.store (s.fileAt, std::memory_order_relaxed);
                loopIn.store (s.loopIn, std::memory_order_relaxed);
                loopOut.store (s.loopOut, std::memory_order_relaxed);
                crossfade.store (s.crossfade, std::memory_order_relaxed);
                direction.store (s.direction, std::memory_order_relaxed);
                pingPong.store (s.pingPong, std::memory_order_relaxed);
            }

            Segment load() const noexcept
            {
                return { generation.load (std::memory_order_relaxed),
                         from.load (std::memory_order_relaxed),
                         fileAt.load (std::memory_order_relaxed),
                         loopIn.load (std::memory_order_relaxed),
                         loopOut.load (std::memory_order_relaxed),
                         crossfade.load (std::memory_order_relaxed),
                         direction.load (std::memory_order_relaxed),
                         pingPong.load (std::memory_order_relaxed) };
            }
        };

        struct Shared
        {
            std::atomic<int> count { 0 };
            SharedSegment segments[2];
        };

        // Under `writers`.
        void publish (const Held& next)
        {
            held = next;

            const auto at = sequence.load (std::memory_order_relaxed);
            sequence.store (at + 1, std::memory_order_relaxed);
            std::atomic_thread_fence (std::memory_order_release);

            shared.count.store (next.count, std::memory_order_relaxed);

            for (int i = 0; i < next.count; ++i)
                shared.segments[i].store (next.segments[i]);

            sequence.store (at + 2, std::memory_order_release);
        }

        std::mutex writers;
        Held held;                              // the writers' copy, under `writers`
        std::uint64_t generations = 0;          // under `writers`

        std::atomic<std::uint64_t> sequence { 0 };
        Shared shared;

        std::atomic<std::uint64_t> adoptedSequence { 0 };
        std::atomic<std::uint64_t> adoptedGeneration { 0 };
        std::atomic<double> adoptedFrom { 0.0 }, adoptedFileAt { 0.0 };
    };
}
