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
    THE SHOW'S KEY TRIGGERS, as the window matches them (namespace draft §74,
    AQK, AQW). A key fires its cue where it arrives, so the window holds the
    show's key triggers and answers a key press from them, after every key of
    its own: a key the show keeps never reaches here, and the door refuses one
    anyway (engine/cue/KeyChord.h).

    READ ONCE A REVISION: the triggers are found by walking the tree, which is
    a pass over every node, so the window reads them when the show changes and
    keeps the answer - never per key, never per row.

    std only, like the rest of model/.
*/

#include <wfg/client/model/Inspector.h>

#include <string>
#include <string_view>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    struct KeyTrigger
    {
        std::string id;
        std::string cue;        ///< the cue it fires
        std::string key;        ///< its key, spelled as KeyChord spells one; empty for none yet
        bool enabled = true;

        /*  A STREAM DECK'S KEY instead (namespace draft §104): `kind` deck, the
            surface, the key from nought, and the surface's name for the words. */
        std::string kind = "key";
        std::string surface {};
        int button = 0;
        std::string surfaceName {};

        bool operator== (const KeyTrigger&) const = default;
    };

    /** Every key trigger the show has, the computer's and the Stream Decks', in the tree's order. */
    std::vector<KeyTrigger> readKeyTriggers (const tree::TreeSnapshot& snapshot);

    /*  THE TRIGGERS A KEY FIRES: every switched-on trigger with that key,
        whatever cue it is on - one key may fire several cues, as one note may.
        Empty for a key nothing is bound to, and for one the show keeps. */
    std::vector<std::string> triggersForKey (const std::vector<KeyTrigger>& triggers, std::string_view key);

    /*  A CUE'S KEY TRIGGER, the first, for the inspector's Key line: null when
        the cue has none. */
    const KeyTrigger* keyTriggerOf (const std::vector<KeyTrigger>& triggers, const std::string& cueId);

    /*  THE INSPECTOR'S KEY LINE, for one cue picked (namespace draft §74): its
        own drawer, last, holding the cue's key or nothing yet. Several cues
        picked get none - a key fires one cue's trigger, and choosing it for
        eight at once is eight decisions nobody made. */
    void addKeyLine (Inspection& inspection, const std::vector<KeyTrigger>& triggers);
}
