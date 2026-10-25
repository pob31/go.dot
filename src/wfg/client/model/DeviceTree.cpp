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

#include <wfg/client/model/DeviceTree.h>

#include <wfg/client/model/OscMessages.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/Node.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <map>
#include <set>

namespace wfg::client::model
{
    namespace
    {
        std::vector<std::string> partsOf (const std::string& text)
        {
            std::vector<std::string> parts;
            std::string part;

            for (const auto c : text)
            {
                if (c == '/')
                {
                    parts.push_back (part);
                    part.clear();
                    continue;
                }

                part += c;
            }

            if (! part.empty())
                parts.push_back (part);

            return parts;
        }

        bool isDigit (char c) { return std::isdigit (static_cast<unsigned char> (c)) != 0; }
    }

    bool naturalLess (const std::string& a, const std::string& b)
    {
        std::size_t i = 0;
        std::size_t j = 0;

        while (i < a.size() && j < b.size())
        {
            if (isDigit (a[i]) && isDigit (b[j]))
            {
                auto endA = i;
                auto endB = j;

                while (endA < a.size() && isDigit (a[endA])) ++endA;
                while (endB < b.size() && isDigit (b[endB])) ++endB;

                // The shorter run without leading zeros is the smaller number.
                auto startA = i;
                auto startB = j;
                while (startA + 1 < endA && a[startA] == '0') ++startA;
                while (startB + 1 < endB && b[startB] == '0') ++startB;

                if (endA - startA != endB - startB)
                    return endA - startA < endB - startB;

                if (const auto order = a.compare (startA, endA - startA, b, startB, endB - startB); order != 0)
                    return order < 0;

                i = endA;
                j = endB;
                continue;
            }

            // Numbers before text, then text by its characters.
            if (isDigit (a[i]) != isDigit (b[j]))
                return isDigit (a[i]);

            if (a[i] != b[j])
                return a[i] < b[j];

            ++i;
            ++j;
        }

        return a.size() - i < b.size() - j;
    }

    namespace
    {
        /*  The root of the described device `address` is aimed at, or nothing:
            under no device, or under an opaque one, there is no tree. */
        std::string describedRootOf (const std::string& address, const std::vector<DeviceRow>& devices)
        {
            auto deviceId = deviceOf (address, devices);

            /*  AN ADDRESS THAT IS A ROOT, EXACTLY, is what picking a device writes
                when the cue had nothing to carry over - and `deviceOf` answers for
                what is UNDER a root. It is that device, with nothing picked yet. */
            if (deviceId.empty())
                for (const auto& row : devices)
                    for (const auto& each : row.prefixes())
                        if (each == address)
                            deviceId = row.id;

            const auto device = std::find_if (devices.begin(), devices.end(),
                                              [&deviceId] (const DeviceRow& row) { return row.id == deviceId; });

            if (deviceId.empty() || device == devices.end() || device->opaque())
                return {};

            /*  THE ROOT THAT MATCHED, among several (namespace draft §57, AFK:
                an X32's /ch, /bus and /dca are one described device): the one
                the address is, else the longest one it is under. */
            std::string root = device->firstPrefix();
            std::size_t covered = 0;

            for (const auto& each : device->prefixes())
            {
                if (each == address)
                    return each;

                if (const auto length = tree::prefixMatchLength (address, each); length > covered)
                {
                    covered = length;
                    root = each;
                }
            }

            return root;
        }

        using Children = std::map<std::string, std::map<std::string, bool>>;   // parent -> name -> holds others

        /*  THE TREE AS THE MENUS READ IT: what each part holds, and the parts
            that take what they do not list (GODOT.OPEN) or whose level may be
            left out (GODOT.SKIP) - the author, 2026-10-24, for an Eos cue that
            is a decimal and one fired without its list. */
        struct Walk
        {
            Children children;
            std::set<std::string> open;
            std::map<std::string, std::string> skips;     ///< part -> the word it is offered by
        };

        bool isUnder (const std::string& at, const std::string& root)
        {
            return at.size() > root.size() + 1 && at.compare (0, root.size(), root) == 0 && at[root.size()] == '/';
        }

        /*  EVERY NODE UNDER THE ROOT, AND EVERY LEVEL ABOVE IT: a leaf registers
            itself with its parent and each ancestor with its own, so a level the
            description never declared as a container still reads as one. */
        Walk walkUnder (const tree::TreeSnapshot& snapshot, const std::string& root)
        {
            Walk walk;
            auto& children = walk.children;

            for (const auto* node : snapshot.all())
            {
                const auto& at = node->address;

                if (at == root || isUnder (at, root))
                {
                    if (node->open())  walk.open.insert (at);
                    if (node->skips()) walk.skips[at] = node->skipWord().empty() ? std::string ("skip") : node->skipWord();
                }

                if (! isUnder (at, root))
                    continue;

                auto parent = root;
                const auto parts = partsOf (at.substr (root.size() + 1));

                for (std::size_t level = 0; level < parts.size(); ++level)
                {
                    const auto last = level + 1 == parts.size();
                    auto& holdsOthers = children[parent][parts[level]];
                    holdsOthers = holdsOthers || ! last || node->isContainer();
                    parent += "/" + parts[level];
                }
            }

            return walk;
        }

        std::vector<std::pair<std::string, bool>> sortedNames (const std::map<std::string, bool>& names)
        {
            std::vector<std::pair<std::string, bool>> sorted (names.begin(), names.end());
            std::sort (sorted.begin(), sorted.end(),
                       [] (const auto& x, const auto& y) { return naturalLess (x.first, y.first); });
            return sorted;
        }

        //  The first of what `parent` holds, as a person counts; empty when it holds nothing.
        std::string firstUnder (const Children& children, const std::string& parent)
        {
            const auto found = children.find (parent);

            if (found == children.end() || found->second.empty())
                return {};

            return sortedNames (found->second).front().first;
        }

        /*  FROM `described` DOWN THROUGH EACH FIRST PART TO A LEAF, as the parts
            to add: "/fire" under an Eos cue. What a choice past a skipped level
            writes, so the address it leaves is whole and reads back the way it
            was picked - "/eos/cue/3" alone would read as list 3. */
        std::string firstLeafTail (const Children& children, const std::string& described)
        {
            std::string tail;
            auto at = described;

            for (std::size_t depth = 0; depth < 64; ++depth)
            {
                const auto first = firstUnder (children, at);

                if (first.empty())
                    break;

                tail += "/" + first;
                at += "/" + first;
            }

            return tail;
        }

        //  Whether `described` or a part above it takes what it does not list.
        bool openAt (const Walk& walk, const std::string& described)
        {
            for (auto at = described; ! at.empty(); at = at.substr (0, at.rfind ('/')))
                if (walk.open.count (at) > 0)
                    return true;

            return false;
        }

        bool holds (const Children& children, const std::string& parent, const std::string& name)
        {
            const auto found = children.find (parent);
            return found != children.end() && found->second.count (name) > 0;
        }

        /*  The choice that leaves a level out, by the word its part offers it
            by: "[skip]", the author's first word (2026-10-24), or the preset's
            own - "[current]" on Eos (2026-10-25). */
        std::string skipLabel (const Walk& walk, const std::string& described)
        {
            const auto found = walk.skips.find (described);
            return "[" + (found != walk.skips.end() ? found->second : std::string ("skip")) + "]";
        }

        constexpr const char* opens = " \xe2\x80\xba";

        //  A part spelled as a number: "98", "98.1".
        bool numeric (const std::string& name)
        {
            if (name.empty())
                return false;

            auto dots = 0;

            for (const auto c : name)
                if (c == '.')
                    ++dots;
                else if (! isDigit (c))
                    return false;

            return dots <= 1 && name != ".";
        }

        /*  `described` is where the file's tree is read, `real` what the
            address says: the same until a level is skipped, when the menu goes
            on reading under that level's first part while the address leaves
            it out. */
        std::vector<TreeMenuItem> itemsUnder (const Walk& walk, const std::string& described, const std::string& real,
                                              bool afterSkip)
        {
            std::vector<TreeMenuItem> items;
            const auto found = walk.children.find (described);

            if (found == walk.children.end())
                return items;

            for (const auto& [name, holdsOthers] : sortedNames (found->second))
            {
                TreeMenuItem item;
                item.label = name;

                if (holdsOthers)
                    item.children = itemsUnder (walk, described + "/" + name, real + "/" + name, afterSkip);
                else
                    item.address = real + "/" + name;

                items.push_back (std::move (item));
            }

            if (walk.skips.count (described) > 0 && ! afterSkip)
                if (const auto first = firstUnder (walk.children, described); ! first.empty())
                {
                    TreeMenuItem item;
                    item.label = skipLabel (walk, described);
                    item.children = itemsUnder (walk, described + "/" + first, real, true);

                    if (! item.children.empty())
                        items.push_back (std::move (item));
                }

            return items;
        }
    }

    std::vector<PathStep> pathSteps (const tree::TreeSnapshot& snapshot,
                                     const std::vector<DeviceRow>& devices,
                                     const std::string& address)
    {
        const auto root = describedRootOf (address, devices);

        if (root.empty())
            return {};

        const auto walk = walkUnder (snapshot, root);
        const auto& children = walk.children;
        const auto wanted = address.size() > root.size() + 1 ? partsOf (address.substr (root.size() + 1))
                                                             : std::vector<std::string> {};
        std::vector<PathStep> steps;
        auto described = root;
        auto real = root;
        auto afterSkip = false;

        for (std::size_t level = 0; steps.size() < 64;)
        {
            const auto found = children.find (described);

            if (found == children.end() || found->second.empty())
                break;

            const auto part = level < wanted.size() ? wanted[level] : std::string {};

            /*  A CHOICE'S KEY: the address it writes. Past a skipped level a
                part that holds others writes down to its first leaf, so the
                address stays whole and is read back as it was picked. */
            const auto keyOf = [&] (const std::string& name, bool holdsOthers, const std::string& under)
            {
                return real + "/" + name + (afterSkip && holdsOthers ? firstLeafTail (children, under) : std::string {});
            };

            PathStep step;

            for (const auto& [name, holdsOthers] : sortedNames (found->second))
                step.choices.push_back ({ keyOf (name, holdsOthers, described + "/" + name),
                                          name + (holdsOthers ? opens : "") });

            /*  [skip], WHERE THE LEVEL MAY BE LEFT OUT (GODOT.SKIP): it writes
                the first of what the level's first part holds, down to a leaf
                - an Eos cue 1 in the current list, "/eos/cue/1/fire". The
                address reads as skipped when its part here is none the level
                lists, or when what follows it fits under a skip and not under
                that part: "/eos/cue/3/fire" is cue 3, "/eos/cue/3/5/fire" cue
                5 of list 3. */
            const auto first = firstUnder (children, described);
            const auto shape = described + "/" + first;
            const auto skippable = ! afterSkip && walk.skips.count (described) > 0 && ! first.empty()
                                   && ! firstUnder (children, shape).empty();

            if (skippable)
            {
                const auto firstInside = firstUnder (children, shape);
                const auto skipKey = real + "/" + firstInside + firstLeafTail (children, shape + "/" + firstInside);
                step.choices.push_back ({ skipKey, skipLabel (walk, described) + opens });

                auto skipped = false;

                if (! part.empty())
                {
                    if (! holds (children, described, part))
                        skipped = true;
                    else if (level + 1 < wanted.size())
                        skipped = ! holds (children, described + "/" + part, wanted[level + 1])
                                  && holds (children, shape + "/" + part, wanted[level + 1]);
                }

                if (skipped)
                {
                    step.picked = skipKey;
                    steps.push_back (std::move (step));
                    described = shape;
                    afterSkip = true;
                    continue;
                }
            }

            /*  A LEVEL OF NUMBERS UNDER AN OPEN PART IS TYPED (PathStep::typable):
                whatever followed the part stays after it, or the first leaf
                when nothing did. */
            if (openAt (walk, described)
                  && std::all_of (found->second.begin(), found->second.end(),
                                  [] (const auto& child) { return numeric (child.first); }))
            {
                step.typable = true;
                step.typedBefore = real;

                for (auto after = level + 1; after < wanted.size(); ++after)
                    step.typedAfter += "/" + wanted[after];

                if (step.typedAfter.empty())
                    step.typedAfter = firstLeafTail (children, described + "/" + first);
            }

            auto partHoldsOthers = false;
            auto next = described;

            if (const auto listed = found->second.find (part); ! part.empty() && listed != found->second.end())
            {
                partHoldsOthers = listed->second;
                next = described + "/" + part;
                step.picked = keyOf (part, partHoldsOthers, next);
            }
            else if (! part.empty() && openAt (walk, described))
            {
                /*  A PART TYPED THAT THE FILE DOES NOT LIST, under an open one:
                    a decimal cue. Shown as picked, among the rest, and the
                    menus after it read as its first sibling's do. */
                partHoldsOthers = found->second.at (first);
                next = shape;
                step.picked = keyOf (part, partHoldsOthers, next);
                step.choices.push_back ({ step.picked, part + (partHoldsOthers ? opens : "") });
            }

            steps.push_back (std::move (step));

            if (steps.back().picked.empty() || ! partHoldsOthers)
                break;

            real += "/" + part;
            described = next;
            ++level;
        }

        return steps;
    }

    std::vector<TreeMenuItem> treeMenu (const tree::TreeSnapshot& snapshot,
                                        const std::vector<DeviceRow>& devices,
                                        const std::string& address)
    {
        const auto root = describedRootOf (address, devices);
        return root.empty() ? std::vector<TreeMenuItem> {} : itemsUnder (walkUnder (snapshot, root), root, root, false);
    }

    std::vector<std::string> valueOptions (const tree::TreeSnapshot& snapshot, const std::string& address)
    {
        const auto* node = snapshot.find (address);

        if (node == nullptr || node->enumValues().empty() || node->typeTags.size() != 1)
            return {};

        std::vector<std::string> options;

        for (const auto& value : node->enumValues())
            if (const auto atom = atomFor (node->typeTags.front(), value))
                options.push_back (*atom);

        return options;
    }
}
