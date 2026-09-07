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
    The whole tree at one tick, frozen.

    WHY A SNAPSHOT AT ALL. The tick thread owns the model and is its only
    writer; the OSCQuery server threads have to answer a GET while that is
    going on. Locking the model for the length of an HTTP response would put a
    web request in the way of the GO path, which PRD §4.1 does not allow. So the
    tick thread publishes an immutable object and the server threads read it,
    and neither waits for the other.

    ONCE PUBLISHED, A SNAPSHOT NEVER CHANGES. That is the property everything
    else rests on, and it is why the tree replaces its parts rather than editing
    them: a reader holding the snapshot from tick 12 still sees tick 12's values
    after tick 13 has been and gone, however long it takes to serialise them.

    TWO PARTS, FOR A REASON THAT IS ABOUT COST. The document side of the tree is
    the big one - a show with four hundred cues is several thousand nodes - and
    it changes only when someone edits the show. The engine's own counters are a
    dozen nodes and change every single tick. Copying the first of those fifty
    times a second to keep the second up to date would be silly, so the document
    part is shared between snapshots by pointer and only the runtime part is
    copied. A snapshot is therefore cheap to publish and still completely
    immutable.

    Both parts are sorted by address, so lookup is a binary search and merging
    them for a full walk is linear.
*/

#include <wfg/engine/tree/Node.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::tree
{
    class TreeSnapshot
    {
    public:
        /*  All three must be sorted by address and must not share an address
            between them. ParameterTree is what builds them; nothing else
            should be constructing one of these.

            THREE AND NOT TWO SINCE M9. The show's own nodes and a mounted
            processor's change at completely different rates - a cue edit moves
            the first, a `mount.load` moves the second - and holding them
            together meant re-materialising a megabyte of somebody else's
            namespace every time a cue was renamed. Measured at 3.1 ms of every
            applied mutation with WFS-DIY's own capture, against 0.1 ms for
            everything else. */
        TreeSnapshot (std::int64_t tickIndex,
                      std::shared_ptr<const std::vector<Node>> documentNodes,
                      std::shared_ptr<const std::vector<Node>> mountedNodes,
                      std::vector<Node> runtimeNodes);

        /** The tick this was published at. */
        std::int64_t tick() const noexcept { return tickAt; }

        /** The node at that exact address, or nullptr. */
        const Node* find (std::string_view address) const;

        std::size_t size() const noexcept;

        /*  Every node, in address order. Allocates a vector of pointers, so it
            is for serialising and diffing rather than for anything that runs
            every tick. */
        std::vector<const Node*> all() const;

        /** The immediate children of a container, in address order. */
        std::vector<const Node*> childrenOf (std::string_view address) const;

    private:
        std::int64_t tickAt = 0;
        std::shared_ptr<const std::vector<Node>> document;
        std::shared_ptr<const std::vector<Node>> mounted;
        std::vector<Node> runtime;
    };

    //==============================================================================
    /*  What changed between two snapshots.

        This is what PR 1.9 turns into the OSCQuery notifications a listening
        client receives - `PATH_ADDED`, `PATH_REMOVED` and a value push - and it
        is computed here rather than there so that the rule can be tested
        without a socket.

        `valueChanged` compares values EXACTLY, type included: an integer 1 and
        a double 1.0 are a change, because a client that asked for the type is
        entitled to notice when it moves. */
    struct TreeDiff
    {
        std::vector<std::string> added;
        std::vector<std::string> removed;
        std::vector<std::string> valueChanged;

        bool empty() const noexcept
        {
            return added.empty() && removed.empty() && valueChanged.empty();
        }
    };

    TreeDiff diff (const TreeSnapshot& before, const TreeSnapshot& after);
}
