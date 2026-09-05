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
                                std::vector<Node> runtimeNodes)
        : tickAt (tickIndex),
          document (std::move (documentNodes)),
          runtime (std::move (runtimeNodes))
    {
    }

    const Node* TreeSnapshot::find (std::string_view address) const
    {
        /*  Runtime first: it is the smaller of the two and the one a diagnostic
            client polls, so the common lookup is the cheap one. */
        if (const auto* node = findIn (runtime, address))
            return node;

        return document != nullptr ? findIn (*document, address) : nullptr;
    }

    std::size_t TreeSnapshot::size() const noexcept
    {
        return runtime.size() + (document != nullptr ? document->size() : 0);
    }

    std::vector<const Node*> TreeSnapshot::all() const
    {
        std::vector<const Node*> result;
        result.reserve (size());

        const std::vector<Node> empty;
        const auto& docNodes = document != nullptr ? *document : empty;

        /*  A merge rather than a concatenate-and-sort: both sides are already in
            address order, so this is linear and the result is too. */
        auto a = docNodes.begin();
        auto b = runtime.begin();

        while (a != docNodes.end() && b != runtime.end())
            result.push_back (a->address <= b->address ? &*a++ : &*b++);

        for (; a != docNodes.end(); ++a)
            result.push_back (&*a);

        for (; b != runtime.end(); ++b)
            result.push_back (&*b);

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

    //==============================================================================
    TreeDiff diff (const TreeSnapshot& before, const TreeSnapshot& after)
    {
        TreeDiff result;

        const auto oldNodes = before.all();
        const auto newNodes = after.all();

        /*  Both sides are in address order, so this walks them once rather than
            looking each address up in the other. A show with several thousand
            nodes gets diffed on every tick that changed something. */
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

        return result;
    }
}
