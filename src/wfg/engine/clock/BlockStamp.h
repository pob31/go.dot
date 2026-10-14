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
    WHEN A BLOCK BEGAN: THE AUDIO SIDE'S OWN STAMP OF ITS SAMPLE AGAINST THE
    HOST'S CLOCK (namespace draft §58, RD.9; decision AFS).

    WHAT IT IS FOR. Two machines that start a sound on "the same sample" need a
    shared notion of time: each says, of its own sound clock, which host moment
    each of its samples was at, and the two host clocks are compared over the
    link. The tick thread cannot say the first by itself: it reads the sample
    count between blocks, so a sample read at a moment is anything up to a
    block behind it (timecode/CountLine.h). The audio side can - at the top of
    each block it knows the block's first sample exactly, and the moment it
    was asked for it - and writes the pair here, once a block, for the tick
    thread to put a straight line through (pair/ClockMap.h).

    THE MOMENT IS THE STEADY CLOCK'S, in nanoseconds of
    `std::chrono::steady_clock` - the clock the link already times everything
    on (`pair::LinkClock`), so nothing is converted between the two and a
    machine's stamps and its pings speak one clock. A device's callback stamps
    the moment it was entered; the hosted pump and the dummy clock, which pace
    blocks to that same clock, stamp the moment each block was DUE - their
    schedule, which a block run late catches up with - so a loaded machine's
    late wake-ups are not read as its sound clock wandering.

    THE LIPOGRAM (PRD §4.2). The writer is the audio thread: two relaxed
    atomic stores between two stores of a sequence number, and a release fence
    - no allocation, no lock, no system call, nothing that can wait. It is a
    SEQLOCK: the sequence is odd while a write is under way, and the reader -
    the tick thread, never the audio one - reads the sequence, the pair and
    the sequence again, and believes the pair only when the two sequences are
    the same even number. A torn pair - one sample from one block and the
    moment of another - is never handed out; a reader that keeps meeting a
    write under way gives up after a few tries and reads nothing this time,
    rather than spinning against the audio thread. Written as Boehm's "Can
    Seqlocks Get Along With Programming Language Memory Models?" has it, every
    field an atomic, so there is no data race for a sanitizer to find either.

    Vendor-free, like SampleClock.h, which carries it.
*/

#include <atomic>
#include <cstdint>

namespace wfg
{
    class BlockStamp
    {
    public:
        //  A block's first sample, and the steady clock's nanoseconds it was asked for at.
        struct Reading
        {
            std::int64_t sample = -1;
            std::int64_t nanos = 0;
        };

        BlockStamp() = default;

        BlockStamp (const BlockStamp&) = delete;
        BlockStamp& operator= (const BlockStamp&) = delete;

        /*  THE AUDIO THREAD, once a block, before the count moves past the
            block: one writer only. Allocation-free and lock-free. */
        void write (std::int64_t firstSample, std::int64_t hostNanos) noexcept
        {
            const auto was = sequence.load (std::memory_order_relaxed);
            sequence.store (was + 1, std::memory_order_relaxed);
            std::atomic_thread_fence (std::memory_order_release);

            sample.store (firstSample, std::memory_order_relaxed);
            nanos.store (hostNanos, std::memory_order_relaxed);

            sequence.store (was + 2, std::memory_order_release);
        }

        /*  ANY OTHER THREAD: the last pair written, whole, or false when none
            has been, or when a write was under way every time it looked. */
        bool read (Reading& out) const noexcept
        {
            for (int attempt = 0; attempt < 8; ++attempt)
            {
                const auto before = sequence.load (std::memory_order_acquire);

                if ((before & 1u) != 0u)
                    continue;

                const auto firstSample = sample.load (std::memory_order_relaxed);
                const auto hostNanos = nanos.load (std::memory_order_relaxed);
                std::atomic_thread_fence (std::memory_order_acquire);

                if (sequence.load (std::memory_order_relaxed) != before)
                    continue;

                if (before == 0)
                    return false;

                out.sample = firstSample;
                out.nanos = hostNanos;
                return true;
            }

            return false;
        }

        //  Blocks stamped so far: how a test sees the writer was called once a block.
        std::uint64_t writes() const noexcept { return sequence.load (std::memory_order_relaxed) / 2u; }

    private:
        std::atomic<std::uint64_t> sequence { 0 };
        std::atomic<std::int64_t> sample { 0 };
        std::atomic<std::int64_t> nanos { 0 };
    };
}
