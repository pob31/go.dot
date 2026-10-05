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
    WHETHER A CUE RUNS TONIGHT (namespace draft §27).

    A cue's `enabled` is what the designer decided. An enable or disable cue
    switches it for the evening (PK), and the switch is held on the cue's own
    node as a `tonight` mark - `on` or `off`, absent when nothing has switched
    it or when what was switched is what the file says anyway - which no file
    holds: `cue/tonight` persists nowhere, so neither writer writes it and the
    unsaved dot does not light (ShowDocument::valueTreePropertyChanged). A
    revert or a reopen builds a new tree, and the marks go with the old one.

    EVERY LIVE READER ASKS THIS, never `enabled` alone: where the pointer may
    stand, which members a group plays, what is armed, what the sampler lays
    out, whether a fire is let through. A solve does not: it works the evening
    out from the show (`Reader::pass`, ShowWalk.h), because the evening is what
    it is computing.

    Tick thread only, as the document is.
*/

#include <juce_data_structures/juce_data_structures.h>

#include <string>

namespace wfg::cue
{
    /** The node property the mark is held in, and the row it is published as. */
    inline const juce::Identifier tonightProperty { "tonight" };

    /*  What the file says. A cue nobody has disabled has no `enabled` property
        at all, and one somebody has holds a boolean false - every value goes
        into the tree typed, through the schema. */
    inline bool enabledInFile (const juce::ValueTree& cue)
    {
        const juce::Identifier enabled { "enabled" };

        return ! cue.hasProperty (enabled) || static_cast<bool> (cue[enabled]);
    }

    /** `file`, `on` or `off`: the published `cue/tonight`. */
    inline std::string tonightWord (const juce::ValueTree& cue)
    {
        const auto word = cue[tonightProperty].toString();
        return word == "on" || word == "off" ? word.toStdString() : std::string ("file");
    }

    /** Whether the cue runs tonight: the mark when there is one, the file otherwise. */
    inline bool runsTonight (const juce::ValueTree& cue)
    {
        const auto word = cue[tonightProperty].toString();

        if (word == "on")
            return true;

        if (word == "off")
            return false;

        return enabledInFile (cue);
    }
}
