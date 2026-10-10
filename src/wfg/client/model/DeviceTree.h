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
    A DESCRIBED DEVICE'S TREE, AS MENUS (namespace draft §56, decision AEP).

    The author, 2026-10-10: "Each of the path comes as a drop down menu.
    Selecting the client then shows a menu with the first item in the path to
    chose from, then the next depending on the previous selection and so on."
    QLab's shape: the cue's `target` menu picks the device; then one menu per
    part of the address - the first lists the children of the device's root,
    the next the children of what the first picked, down to a node that takes
    a value - and a node that enumerates its values makes the value a menu.

    DERIVED FROM THE ADDRESS AND WRITTEN BACK TO IT, as the target menu is
    (`deviceRef`): every choice's key is a whole address, so a pick is the
    `node.set` of the cue's `address` row that already exists. Picking at one
    level drops whatever was below it, because the address now ends there.

    ONE PASS OVER THE SNAPSHOT, filtered by the device's root - the client's
    idiom (`readDevices`, `readOscMessages`) and the boundary's rule: no
    `childrenOf`. A device with no description has no tree and no menus.
*/

#include <wfg/client/model/Devices.h>

#include <string>
#include <utility>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    /*  One menu of the walk: what is picked at this level, if anything, and
        what could be - each choice keyed by the whole address it writes and
        labelled with the child's name, a container's name followed by " ›". */
    struct PathStep
    {
        std::string picked;
        std::vector<std::pair<std::string, std::string>> choices;
    };

    /*  The menus that lead to `address` on its device: the root's children,
        then each picked child's, and one more after a picked container so the
        walk can go on. Empty for an address under no device, under an opaque
        one, or under one whose tree has nothing below the root. */
    std::vector<PathStep> pathSteps (const tree::TreeSnapshot& snapshot,
                                     const std::vector<DeviceRow>& devices,
                                     const std::string& address);

    /*  The values the node at `address` offers - its first argument's VALS -
        spelled as atoms a cue's `value` row holds (`s:"Scene 3"`, `i:4`).
        Empty when the node enumerates nothing, takes more than one argument,
        or is not a node at all. */
    std::vector<std::string> valueOptions (const tree::TreeSnapshot& snapshot, const std::string& address);

    /*  THE SAME TREE AS ONE NESTED MENU, for a further message's address in the
        foot panel's table, where there is no room for a line per level: every
        part that holds others a submenu, every node an item keyed by its whole
        address. Empty when `address` is under no described device. */
    struct TreeMenuItem
    {
        std::string label;
        std::string address;                  ///< what picking it writes; empty for a submenu
        std::vector<TreeMenuItem> children;   ///< a submenu's items
    };

    std::vector<TreeMenuItem> treeMenu (const tree::TreeSnapshot& snapshot,
                                        const std::vector<DeviceRow>& devices,
                                        const std::string& address);

    /*  Names compared as a person counts: "2" before "10", text after numbers. */
    bool naturalLess (const std::string& a, const std::string& b);
}
