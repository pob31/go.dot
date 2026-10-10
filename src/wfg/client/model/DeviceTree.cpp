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
#include <wfg/engine/tree/Node.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <map>

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

            return device->firstPrefix();
        }

        using Children = std::map<std::string, std::map<std::string, bool>>;   // parent -> name -> holds others

        /*  EVERY NODE UNDER THE ROOT, AND EVERY LEVEL ABOVE IT: a leaf registers
            itself with its parent and each ancestor with its own, so a level the
            description never declared as a container still reads as one. */
        Children childrenUnder (const tree::TreeSnapshot& snapshot, const std::string& root)
        {
            Children children;

            for (const auto* node : snapshot.all())
            {
                const auto& at = node->address;

                if (at.size() <= root.size() + 1 || at.compare (0, root.size(), root) != 0 || at[root.size()] != '/')
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

            return children;
        }

        std::vector<std::pair<std::string, bool>> sortedNames (const std::map<std::string, bool>& names)
        {
            std::vector<std::pair<std::string, bool>> sorted (names.begin(), names.end());
            std::sort (sorted.begin(), sorted.end(),
                       [] (const auto& x, const auto& y) { return naturalLess (x.first, y.first); });
            return sorted;
        }

        std::vector<TreeMenuItem> itemsUnder (const Children& children, const std::string& parent)
        {
            std::vector<TreeMenuItem> items;
            const auto found = children.find (parent);

            if (found == children.end())
                return items;

            for (const auto& [name, holdsOthers] : sortedNames (found->second))
            {
                TreeMenuItem item;
                item.label = name;

                if (holdsOthers)
                    item.children = itemsUnder (children, parent + "/" + name);
                else
                    item.address = parent + "/" + name;

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

        const auto children = childrenUnder (snapshot, root);
        const auto wanted = address.size() > root.size() + 1 ? partsOf (address.substr (root.size() + 1))
                                                             : std::vector<std::string> {};
        std::vector<PathStep> steps;
        auto parent = root;

        for (std::size_t level = 0;; ++level)
        {
            const auto found = children.find (parent);

            if (found == children.end() || found->second.empty())
                break;

            PathStep step;

            for (const auto& [name, holdsOthers] : sortedNames (found->second))
                step.choices.push_back ({ parent + "/" + name, name + (holdsOthers ? " \xe2\x80\xba" : "") });

            const auto picked = level < wanted.size() ? found->second.find (wanted[level]) : found->second.end();

            if (picked != found->second.end())
                step.picked = parent + "/" + picked->first;

            steps.push_back (std::move (step));

            if (picked == found->second.end() || ! picked->second)
                break;

            parent += "/" + picked->first;
        }

        return steps;
    }

    std::vector<TreeMenuItem> treeMenu (const tree::TreeSnapshot& snapshot,
                                        const std::vector<DeviceRow>& devices,
                                        const std::string& address)
    {
        const auto root = describedRootOf (address, devices);
        return root.empty() ? std::vector<TreeMenuItem> {} : itemsUnder (childrenUnder (snapshot, root), root);
    }

    std::vector<std::string> valueOptions (const tree::TreeSnapshot& snapshot, const std::string& address)
    {
        const auto* node = snapshot.find (address);

        if (node == nullptr || node->enumValues.empty() || node->typeTags.size() != 1)
            return {};

        std::vector<std::string> options;

        for (const auto& value : node->enumValues)
            if (const auto atom = atomFor (node->typeTags.front(), value))
                options.push_back (*atom);

        return options;
    }
}
