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

#include <wfg/engine/document/OutputLayout.h>

#include <wfg/engine/audio/AudioSettings.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace wfg::doc
{
    namespace
    {
        /*  A channel slot that has not been given a hardware output yet: a new
            bus's, or the second half of one just widened. Distinct from -1,
            which is a slot somebody deliberately disconnected and which must
            survive an edit exactly as it is. */
        constexpr int unallocated = -2;

        /*  Where a logical output goes today. An EMPTY patch is identity — the
            device layer's own reading (`audio::patchOutputs`) — and a patch
            shorter than the logical space leaves the rest disconnected, which
            is that same function's `row < patch.size() ? … : -1`. Both rules
            are read off there rather than guessed, because a disagreement
            between this file and the callback is a silent re-patching. */
        int hardwareFor (const std::vector<int>& patch, int logical)
        {
            if (logical < 0)
                return -1;

            if (patch.empty())
                return logical;

            return logical < static_cast<int> (patch.size())
                     ? patch[static_cast<std::size_t> (logical)]
                     : -1;
        }

        OutputLayout refused (std::string why)
        {
            OutputLayout out;
            out.problem = std::move (why);
            return out;
        }

        struct Entry
        {
            BusShape shape;

            /*  This bus's hardware outputs, one per channel of its width, in
                its own order. Carried THROUGH the edit: a bus that moves takes
                its outputs with it, which is the whole of what "settled" means.
                `unallocated` marks a channel still to be given one. */
            std::vector<int> hardware;
        };
    }

    bool isPacked (const std::vector<BusShape>& buses)
    {
        auto expected = 0;

        for (const auto& bus : buses)
        {
            if (bus.width < 1 || bus.firstChannel != expected)
                return false;

            expected += bus.width;
        }

        return true;
    }

    OutputLayout applyLayoutEdit (std::vector<BusShape> before,
                                  const std::vector<int>& patch,
                                  bool settled,
                                  const LayoutEdit& edit)
    {
        const auto maximum = audio::maximumPatchChannels;

        if (edit.kind == LayoutEdit::Kind::create || edit.kind == LayoutEdit::Kind::resize)
            if (edit.width < 1 || edit.width > maximum)
                return refused ("an output must be at least one channel wide");

        /*  THE THREE WAYS A SHOW STOPS FOLLOWING ITS LIST, and any one of them
            is enough. The flag is the recorded one — somebody patched by hand,
            or a cue has played. A patch that is already written is the same
            fact arrived at without the flag, which matters for a show saved by
            an older build. And a layout that is NOT PACKED was written by hand
            in the file: its channels are a rig, not a consequence of an order,
            so repacking it silently would move a processor feed. Treating it as
            settled is what preserves it — the channels move out of
            `firstChannel`, where a packed list can no longer express them, and
            into the patch, where "this output is plugged in there" belongs. */
        const auto following = ! settled && patch.empty() && isPacked (before);

        /*  The logical space the buses occupy today. Anything the patch holds
            beyond it belongs to nobody and is kept as it is, at the end: a
            designer who declared more rows in the matrix than the show has
            outputs has said something, and an edit to the bus list is not the
            moment to throw it away. */
        auto oldTotal = 0;

        for (const auto& bus : before)
            oldTotal = std::max (oldTotal, bus.firstChannel + std::max (bus.width, 0));

        std::vector<Entry> entries;
        entries.reserve (before.size() + 1);

        for (const auto& bus : before)
        {
            Entry entry;
            entry.shape = bus;

            if (! following)
                for (auto channel = 0; channel < bus.width; ++channel)
                    entry.hardware.push_back (hardwareFor (patch, bus.firstChannel + channel));

            entries.push_back (std::move (entry));
        }

        const auto find = [&entries] (const std::string& id) -> std::ptrdiff_t
        {
            for (std::size_t at = 0; at < entries.size(); ++at)
                if (entries[at].shape.id == id)
                    return static_cast<std::ptrdiff_t> (at);

            return -1;
        };

        /*  A position in the list as it stands, the moved bus included, clamped
            the way `ShowDocument::move` clamps: `juce::ValueTree::moveChild`
            puts the child back so it ENDS UP at that index in a list of the
            same length, so a client that counts rows gets the row it pointed
            at. One convention for both, or a drag would land differently in the
            two lists a window draws. */
        const auto place = [] (int index, std::size_t size) -> std::size_t
        {
            if (index < 0 || static_cast<std::size_t> (index) >= size)
                return size == 0 ? 0 : size - 1;

            return static_cast<std::size_t> (index);
        };

        auto placedAt = std::ptrdiff_t { -1 };

        switch (edit.kind)
        {
            case LayoutEdit::Kind::create:
            {
                Entry entry;
                entry.shape.width = edit.width;

                if (! following)
                    entry.hardware.assign (static_cast<std::size_t> (edit.width), unallocated);

                const auto at = edit.index < 0 || static_cast<std::size_t> (edit.index) >= entries.size()
                                  ? entries.size()
                                  : static_cast<std::size_t> (edit.index);

                entries.insert (entries.begin() + static_cast<std::ptrdiff_t> (at), std::move (entry));
                placedAt = static_cast<std::ptrdiff_t> (at);
                break;
            }

            case LayoutEdit::Kind::remove:
            {
                const auto at = find (edit.id);

                if (at < 0)
                    return refused ("no output of that name");

                entries.erase (entries.begin() + at);
                break;
            }

            case LayoutEdit::Kind::move:
            {
                const auto at = find (edit.id);

                if (at < 0)
                    return refused ("no output of that name");

                auto entry = std::move (entries[static_cast<std::size_t> (at)]);
                const auto to = static_cast<std::ptrdiff_t> (place (edit.index, entries.size()));

                entries.erase (entries.begin() + at);
                entries.insert (entries.begin() + to, std::move (entry));
                placedAt = to;
                break;
            }

            case LayoutEdit::Kind::resize:
            {
                const auto at = find (edit.id);

                if (at < 0)
                    return refused ("no output of that name");

                auto& entry = entries[static_cast<std::size_t> (at)];

                /*  A NARROWED OUTPUT DROPS ITS LAST CHANNELS AND A WIDENED ONE
                    GAINS NEW ONES AT ITS END. Stereo to mono keeps the left,
                    which is the side a mono fold and a mono file both land on,
                    and mono to stereo keeps what was already plugged in and
                    finds a right. */
                if (! following)
                    entry.hardware.resize (static_cast<std::size_t> (edit.width), unallocated);

                entry.shape.width = edit.width;
                placedAt = at;
                break;
            }
        }

        OutputLayout out;
        out.placedAt = static_cast<int> (placedAt);

        auto total = 0;

        for (const auto& entry : entries)
            total += entry.shape.width;

        if (total > maximum)
            return refused ("more output channels than the patch can name");

        /*  REPACKED: every bus starts where the widths before it end. This is
            the one number this file exists to keep true. */
        auto next = 0;

        for (auto& entry : entries)
        {
            entry.shape.firstChannel = next;
            next += entry.shape.width;
            out.buses.push_back (entry.shape);
        }

        if (following)
            return out;

        /*  THE TAIL: patch entries past the logical space the buses occupied,
            which no bus was feeding and which nothing here has moved. */
        std::vector<int> tail;

        for (auto logical = oldTotal; logical < static_cast<int> (patch.size()); ++logical)
            tail.push_back (patch[static_cast<std::size_t> (logical)]);

        /*  A NEW OUTPUT TAKES THE NEXT INTERFACE CHANNELS PAST EVERYTHING IN
            USE, which is WFS-DIY's rule ("diagonal-continue") and not "the
            lowest free one". The difference shows on a rig whose patch has a
            hole in it: filling the hole would put a new output somewhere the
            designer had deliberately left empty, and the hole is usually empty
            because that pair of interface channels goes somewhere else. */
        auto highest = -1;

        for (const auto& entry : entries)
            for (const auto channel : entry.hardware)
                highest = std::max (highest, channel);

        for (const auto channel : tail)
            highest = std::max (highest, channel);

        for (auto& entry : entries)
            for (auto& channel : entry.hardware)
                if (channel == unallocated)
                {
                    /*  Past the end of the patch's own vocabulary there is no
                        honest answer, so the channel is disconnected rather
                        than wrapped round onto somebody else's output. */
                    channel = highest + 1 < maximum ? ++highest : -1;
                }

        for (const auto& entry : entries)
            for (const auto channel : entry.hardware)
                out.outputPatch.push_back (channel);

        for (const auto channel : tail)
            out.outputPatch.push_back (channel);

        /*  A patch of nothing but identity is written all the same once the
            show has settled: the point of settling is that the outputs stop
            moving when the list does, and an empty string would put them back
            on the list's leash at the next edit. */
        out.patchChanged = out.outputPatch != patch;

        return out;
    }
}
