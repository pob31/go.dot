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
    Every string a cell of the desktop client shows, made in one place.

    The client reads the engine through ParameterTree::snapshot() and nothing
    else (namespace draft §14.16, rule 2), and a snapshot's values are
    osc::Value - the one value type of the control plane. Turning one into the
    text a label shows is the same job the page's views/values.js does, and it
    is done here rather than beside each label so that a number looks the same
    in every pane, so that the formatting is testable with no window, and so
    that a double goes through osc::formatDouble - shortest round-trip, locale
    independent - rather than through anything that might consult fr_FR.

    std only. This library names no JUCE type, and the boundary gate checks.
*/

#include <wfg/engine/osc/OscValue.h>

#include <string>
#include <string_view>
#include <vector>

namespace wfg::tree { struct Node; class TreeSnapshot; }

namespace wfg::client::model
{
    /** The value as a label shows it. Empty for nil and for an impulse. */
    std::string text (const osc::Value& value);

    /*  The node's sole value as text. Empty for nullptr, for a container, for
        an event and for a list - which is Node::soleValue()'s own answer, and
        deliberate: four gains have no single text. */
    std::string text (const tree::Node* node);

    /** find() and text() in one call; empty when nothing is at the address. */
    std::string text (const tree::TreeSnapshot& snapshot, std::string_view address);

    /** A T/F node read as a bool. False when absent, which is the safe answer for every flag a client reads. */
    bool flag (const tree::TreeSnapshot& snapshot, std::string_view address);

    /*  The words of a space-separated node, which is how every `order` node
        lists identifiers - the page's tree.ids() does the same split. */
    std::vector<std::string> words (std::string_view line);
}
