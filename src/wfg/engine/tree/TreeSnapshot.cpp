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

#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cstddef>

namespace wfg::tree
{
    namespace
    {
        /*  Binary search over a vector sorted by address. Both parts are built
            sorted by ParameterTree, which is the precondition this relies on
            and the reason nothing else constructs a snapshot. */
        const Node* findIn (const std::vector<Node>& nodes, std::string_view address)
        {
            const auto it = std::lower_bound (nodes.begin(), nodes.end(), address,
                                              [] (const Node& node, std::string_view target)
                                              {
                                                  return node.address < target;
                                              });

            return (it != nodes.end() && it->address == address) ? &*it : nullptr;
        }

        /*  True when `address` is an immediate child of `parent`: it starts with
            `parent/` and has no further separator after that.

            The root is spelled "/" and is the one case where the separator is
            not added, so "/godot" is a child of "/" rather than of "//". */
        bool isImmediateChild (std::string_view address, std::string_view parent)
        {
            const auto prefix = parent == "/" ? std::string ("/") : std::string (parent) + "/";

            if (address.size() <= prefix.size() || address.compare (0, prefix.size(), prefix) != 0)
                return false;

            return address.find ('/', prefix.size()) == std::string_view::npos;
        }
    }

    //==============================================================================
    TreeSnapshot::TreeSnapshot (std::int64_t tickIndex,
                                std::shared_ptr<const std::vector<Node>> documentNodes,
                                std::shared_ptr<const std::vector<Node>> mountedNodes,
                                std::vector<Node> runtimeNodes,
                                std::shared_ptr<const std::vector<Node>> mountedWritten,
                                std::shared_ptr<const std::vector<Node>> messageNodes)
        : tickAt (tickIndex),
          document (std::move (documentNodes)),
          mounted (std::move (mountedNodes)),
          runtime (std::move (runtimeNodes)),
          written (std::move (mountedWritten)),
          messages (std::move (messageNodes))
    {
    }

    const Node* TreeSnapshot::find (std::string_view address) const
    {
        /*  RUNTIME FIRST, THEN THE SHOW, THEN THE MOUNTS, which is smallest to
            largest and also most-polled to least: a diagnostic client watches
            the tick and the runs, an operator watches the cues, and nobody
            polls somebody else's namespace through us. */
        if (const auto* node = findIn (runtime, address))
            return node;

        if (document != nullptr)
            if (const auto* node = findIn (*document, address))
                return node;

        //  The show's messages and curves (§88), beside the show's own nodes.
        if (messages != nullptr)
            if (const auto* node = findIn (*messages, address))
                return node;

        /*  The mounted half LAST, and it is the biggest: a lookup that found
            its answer in the show's own nodes has already returned, and a
            client polling /godot is not polling somebody else's namespace.
            What has been written since it was built is read first (ZC). */
        if (written != nullptr)
            if (const auto* node = findIn (*written, address))
                return node;

        return mounted != nullptr ? findIn (*mounted, address) : nullptr;
    }

    std::size_t TreeSnapshot::size() const noexcept
    {
        return runtime.size()
                 + (document != nullptr ? document->size() : 0)
                 + (messages != nullptr ? messages->size() : 0)
                 + (mounted != nullptr ? mounted->size() : 0);
    }

    std::vector<const Node*> TreeSnapshot::all() const
    {
        std::vector<const Node*> result;
        result.reserve (size());

        const std::vector<Node> empty;
        const auto& docNodes = document != nullptr ? *document : empty;
        const auto& messageNodes = messages != nullptr ? *messages : empty;
        const auto& mountNodes = mounted != nullptr ? *mounted : empty;

        /*  A FOUR-WAY MERGE rather than a concatenate-and-sort: all four are
            already in address order, so this is linear and the result is too.
            Written as "take the smallest head of whichever still has one",
            which is the same rule the two-way version had and reads the same at
            four as it did at two. */
        auto a = docNodes.begin();
        auto b = runtime.begin();
        auto c = mountNodes.begin();
        auto d = messageNodes.begin();

        /*  AND THE WRITTEN MOUNTED NODES, each in the place of its copy in the
            mounted half (ZC): both in address order, so one walks beside the
            other. */
        const auto& writtenNodes = written != nullptr ? *written : empty;
        auto w = writtenNodes.begin();

        for (;;)
        {
            const auto* least = static_cast<const Node*> (nullptr);
            int from = -1;

            if (a != docNodes.end())                                { least = &*a; from = 0; }
            if (b != runtime.end()   && (least == nullptr
                                          || b->address < least->address))
                                                                    { least = &*b; from = 1; }
            if (c != mountNodes.end() && (least == nullptr
                                          || c->address < least->address))
                                                                    { least = &*c; from = 2; }
            if (d != messageNodes.end() && (least == nullptr
                                             || d->address < least->address))
                                                                    { least = &*d; from = 3; }

            if (from < 0)
                break;

            if (from == 2)
            {
                while (w != writtenNodes.end() && w->address < c->address)
                    ++w;

                if (w != writtenNodes.end() && w->address == c->address)
                    least = &*w;
            }

            result.push_back (least);

            if (from == 0)      ++a;
            else if (from == 1) ++b;
            else if (from == 2) ++c;
            else                ++d;
        }

        return result;
    }

    std::vector<const Node*> TreeSnapshot::childrenOf (std::string_view address) const
    {
        std::vector<const Node*> result;

        for (const auto* node : all())
            if (isImmediateChild (node->address, address))
                result.push_back (node);

        return result;
    }

    std::vector<const Node*> TreeSnapshot::under (std::string_view prefix) const
    {
        std::vector<const Node*> result;
        const auto byAddress = [] (const Node* a, const Node* b) { return a->address < b->address; };

        //  Each half is sorted, so its nodes under the prefix are one run starting at its lower bound.
        const auto take = [&result, &byAddress, prefix] (const std::vector<Node>& nodes, const std::vector<Node>* instead)
        {
            const auto runFrom = static_cast<std::ptrdiff_t> (result.size());

            auto at = std::lower_bound (nodes.begin(), nodes.end(), prefix,
                                        [] (const Node& node, std::string_view target)
                                        {
                                            return node.address < target;
                                        });

            for (; at != nodes.end() && std::string_view (at->address).starts_with (prefix); ++at)
            {
                //  A written mounted node is read in place of its copy, as `all()` reads it (ZC).
                const auto* fresh = instead != nullptr ? findIn (*instead, at->address) : nullptr;
                result.push_back (fresh != nullptr ? fresh : &*at);
            }

            //  The halves share no address, so merging each run into what came before is `all()`'s merge.
            std::inplace_merge (result.begin(), result.begin() + runFrom, result.end(), byAddress);
        };

        take (runtime, nullptr);
        if (document != nullptr) take (*document, nullptr);
        if (messages != nullptr) take (*messages, nullptr);
        if (mounted != nullptr)  take (*mounted, written.get());

        return result;
    }

    //==============================================================================
    namespace
    {
        /*  Two runs of nodes in address order, compared in one walk. */
        void diffSorted (const std::vector<const Node*>& oldNodes, const std::vector<const Node*>& newNodes,
                         TreeDiff& result)
        {
            auto a = oldNodes.begin();
            auto b = newNodes.begin();

            while (a != oldNodes.end() && b != newNodes.end())
            {
                if ((*a)->address < (*b)->address)
                {
                    result.removed.push_back ((*a)->address);
                    ++a;
                }
                else if ((*b)->address < (*a)->address)
                {
                    result.added.push_back ((*b)->address);
                    ++b;
                }
                else
                {
                    if ((*a)->values != (*b)->values)
                        result.valueChanged.push_back ((*b)->address);

                    ++a;
                    ++b;
                }
            }

            for (; a != oldNodes.end(); ++a)
                result.removed.push_back ((*a)->address);

            for (; b != newNodes.end(); ++b)
                result.added.push_back ((*b)->address);
        }

        std::vector<const Node*> pointersTo (const std::vector<Node>* nodes)
        {
            std::vector<const Node*> out;

            if (nodes == nullptr)
                return out;

            out.reserve (nodes->size());

            for (const auto& node : *nodes)
                out.push_back (&node);

            return out;
        }

        /*  The mounted half as a reader sees it: each written node in the
            place of its copy (ZC). */
        std::vector<const Node*> mountedView (const std::vector<Node>* mounted, const std::vector<Node>* written)
        {
            auto out = pointersTo (mounted);

            if (written == nullptr || written->empty())
                return out;

            auto w = written->begin();

            for (auto& node : out)
            {
                while (w != written->end() && w->address < node->address)
                    ++w;

                if (w != written->end() && w->address == node->address)
                    node = &*w;
            }

            return out;
        }
    }

    TreeDiff diff (const TreeSnapshot& before, const TreeSnapshot& after)
    {
        TreeDiff result;

        /*  HALF BY HALF (namespace draft §88, AUV), and a half both snapshots
            share by pointer is skipped unread: a snapshot never changes once
            published, so the same half is the same nodes. The four never share
            an address, so their differences together are the tree's. A GO of a
            cue of 5,300 messages used to compare every one of their thirty
            thousand rows with itself, every tick that published. */
        if (before.document != after.document)
            diffSorted (pointersTo (before.document.get()), pointersTo (after.document.get()), result);

        if (before.messages != after.messages)
            diffSorted (pointersTo (before.messages.get()), pointersTo (after.messages.get()), result);

        diffSorted (pointersTo (&before.runtime), pointersTo (&after.runtime), result);

        if (before.mounted != after.mounted || before.written != after.written)
            diffSorted (mountedView (before.mounted.get(), before.written.get()),
                        mountedView (after.mounted.get(), after.written.get()), result);

        /*  In address order, as the whole-tree walk this replaced gave them: a
            client hears a container added before what it holds. */
        std::sort (result.added.begin(), result.added.end());
        std::sort (result.removed.begin(), result.removed.end());
        std::sort (result.valueChanged.begin(), result.valueChanged.end());

        return result;
    }
}
