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

#include <atomic>
#include <cstdint>

/*
    WHEN THE AUDIO THREAD HAS LET GO OF WHAT IT WAS READING (namespace draft
    §133, stage 2).

    A voice's insert place points at a lane, and the pointer is changed from
    the message thread while audio runs. The audio thread reads the pointer
    once a block and may be inside the lane it found for the rest of that
    block - spinning on its child for up to the deadline. So the lane taken
    away must not be handed to another voice, or its child stopped, until the
    block that might still be in it has ended. That is all this answers.

    The audio thread calls `begin` before anything in the block reads such a
    pointer and `end` after the last thing that could. The other side changes
    its pointer FIRST, then takes a `stamp`, and the old value is free once
    `passed (stamp)`:

      - a block that read the old pointer began before the change, so its
        number is at most the stamp, and `passed` waits for its end;
      - a block that begins after the change reads the new pointer, and
        waiting for it too is only caution;
      - with no audio running the two counts are equal and it has passed at
        once.

    Every operation is sequentially consistent: the argument above orders a
    store on one thread against a load on the other, which is the case the
    weaker orders do not promise. Two atomics, no lock, no allocation, no
    system call (PRD §4.2). One audio thread: Go.dot's graph runs on the
    device's callback and no other (`getNumberOfCPUsToUseForAudio` is one).
*/
namespace wfg::plugin
{
    class BlockFence
    {
    public:
        /** The audio thread, as a block starts. */
        void begin() noexcept    { begun.fetch_add (1, std::memory_order_seq_cst); }

        /** The audio thread, when nothing more of this block can read a fenced pointer. */
        void end() noexcept      { ended.store (begun.load (std::memory_order_seq_cst), std::memory_order_seq_cst); }

        /** Any other thread, AFTER it has changed the pointer. */
        std::uint64_t stamp() const noexcept    { return begun.load (std::memory_order_seq_cst); }

        /** Whether every block that could have read the old value has ended. */
        bool passed (std::uint64_t stamped) const noexcept
        {
            return ended.load (std::memory_order_seq_cst) >= stamped;
        }

    private:
        std::atomic<std::uint64_t> begun { 0 };
        std::atomic<std::uint64_t> ended { 0 };
    };
}
