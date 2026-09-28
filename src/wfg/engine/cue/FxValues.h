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
    A CUE'S VALUES FOR ONE PLUGIN, AS ONE ROW (Phase 9a, §17.4): `fx/values`,
    `index:value` pairs, space-separated, sorted by index, each value through
    the canonical number formatter - so the row round-trips under every locale
    and a diff of two shows reads. Only what somebody set is in it; a
    parameter the row does not mention rests at the set entry's preset.

    ITS OWN FILE, AND STD ONLY, because three readers want it and one of them
    may not see a JUCE type: the engine's door (FxRows), the runner, and the
    desktop client's model, which hands a cue's values to the plugin's own
    window (author's decision, 2026-09-25).
*/

#include <map>
#include <string>

namespace wfg::cue
{
    /*  The sparse row, parsed: index to normalised value, sorted. Doubles,
        as the document holds numbers, so what was written is what is spelled
        back; the audio side takes a float at its own boundary. */
    std::map<int, double> parseFxValues (const std::string& text);

    /** The row's spelling for a map, canonical. */
    std::string formatFxValues (const std::map<int, double>& values);
}
