/* This file is part of Go.dot — https://github.com/pob31/go.dot
 *
 * Copyright (C) 2026 Pierre-Olivier Boulant
 *
 * Go.dot is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. Go.dot is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * (LICENSE, at the repository root) for more details.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <wfg/engine/rt/RtCheck.h>

#include <atomic>
#include <memory>
#include <vector>

/*  WHERE THE SENDS INTO THE AUXES MEET (namespace draft §93, AVW).

    An aux is a rack channel whose input is the sum of what the cues send it
    rather than a microphone. Every track - a voice, a rack channel, an aux -
    has an aux tap that writes its sends into a SLOT OF ITS OWN here: one row of
    samples per aux input channel, the "columns". An aux's input stage then adds
    up its columns across every writer's slot.

    ONE WRITER A SLOT, so nothing is shared while it is written. Tracktion may
    render two tracks on two threads at once, and a sum that several of them
    added into would need a lock or an atomic per sample; a slot each needs
    neither. What makes the reads safe is ORDER, not this class: the host puts a
    silent Tracktion aux send after every tap and an aux return before every aux
    input, chained aux to aux in the rack's order, so the graph runs every voice
    and rack channel before the first aux, and each aux before the next.

    So an aux reads what a track wrote IN THIS BLOCK when the track runs before
    it - every voice, every rack channel, every aux above it in the list - and
    what it wrote IN THE BLOCK BEFORE when the track runs after it: an aux below
    it, or itself. That is the whole of the loop rule (one block on a path back
    up the list) and nothing here implements it; it falls out of the order.

    A WRITER WITH NOTHING TO SAY says so, and is skipped. A voice with no send
    into any aux - nearly every voice, nearly always - marks its slot silent
    rather than writing zeros, and no aux reads it.

    THREADS. `prepare` on the message thread, before any block, and never while
    one runs. Everything else on the audio thread; it allocates nothing, locks
    nothing (PRD §4.2).
*/
namespace wfg::audio
{
    class AuxBus
    {
    public:
        AuxBus() = default;

        /*  Sizes every slot: `writers` tracks, `columns` aux input channels,
            `maxBlock` samples each. Message thread. Every slot starts silent. */
        void prepare (int writers, int columns, int maxBlock);

        int writerCount() const noexcept  { return writers; }
        int columnCount() const noexcept  { return columns; }
        int blockLimit() const noexcept   { return maxSamples; }

        /*  One writer's row for one column, `maxBlock` samples long, or null
            for an index this bus was not sized for. The audio thread; the
            writer's own. */
        float* row (int writer, int column) noexcept WFG_AUDIO_THREAD;
        const float* row (int writer, int column) const noexcept WFG_AUDIO_THREAD;

        /*  Whether a writer wrote anything this block. The writer says it after
            writing, the reader asks before reading; the graph's order between
            them is what makes the answer the one for the rows. */
        void setSilent (int writer, bool silent) noexcept WFG_AUDIO_THREAD;
        bool isSilent (int writer) const noexcept WFG_AUDIO_THREAD;

        /*  ADDS one column of every writer into `destination`, `numSamples`
            from `start` of each row. The aux input's read. */
        void addColumn (int column, float* destination, int start, int numSamples) const noexcept WFG_AUDIO_THREAD;

    private:
        int writers = 0;
        int columns = 0;
        int maxSamples = 0;

        std::vector<float> samples;
        std::unique_ptr<std::atomic<bool>[]> silence;
    };
}
