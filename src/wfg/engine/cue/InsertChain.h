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
    HOW WIDE A CUE IS AFTER ITS INSERTS (2026-09-26, the author's decision:
    a plugin can make a mono cue stereo).

    A cue enters its voice as wide as its file. Every insert it switches in,
    in the set's order, either takes it - the cue no wider than the plugin's
    main inputs, the plugin's outputs at least as wide as the cue - and the
    cue comes out as wide as the plugin's outputs (never wider than the
    voice) when it filled the plugin's inputs, all of them or, mono, fed into
    every one; as wide as it went in otherwise. Or the insert passes it dry,
    whole, with a sentence saying why. A mono cue
    through a stereo reverb comes out stereo; the routing then reads that
    width rather than the file's (Runner::resolveRouting), so where the cue
    is sent somewhere with room for two sides it plays in stereo, and where
    there is room for one the sides are summed at a half each. A cue is
    never made narrower than its file. An insert switched out changes
    nothing, and the cue sounds exactly as it did before any of this.

    An insert whose plugin has not said what it takes - loading, missing,
    failed, or no table at all, as in a replay - is counted as taking the cue
    at its width and giving it back so: the routing is then the routing of
    before, which is what such a voice plays anyway (the lane is not called).

    The rule is `chainOf`, std-only and testable with plain numbers; the rest
    reads it off a cue, the show's set and the plugin table, the same way the
    runner sends a cue's inserts (slots are the graph's when there is one).
*/

#include <wfg/engine/plugin/PluginTable.h>

#include <juce_data_structures/juce_data_structures.h>

#include <string>
#include <vector>

namespace wfg::cue
{
    /** What one slot's plugin said it takes; `known` false when it has not. */
    struct InsertShape
    {
        bool known = false;
        int inputs = 0;
        int outputs = 0;
        int latencySamples = 0;
    };

    struct InsertStep
    {
        /** The cue switches this insert in. */
        bool switchedIn = false;

        /*  The channels the cue sends it - nought when it passes dry (switched
            out, or it will not take the cue) - and how many come back. */
        int feed = 0;
        int back = 0;

        /** Why a switched-in insert passes this cue dry, in one sentence. */
        std::string dryWhy;
    };

    struct InsertChain
    {
        std::vector<InsertStep> steps;

        /** How wide the cue is after its inserts: what the routing reads. */
        int channels = 0;

        /** What the inserts that take it declare between them, uncompensated. */
        int latencySamples = 0;
    };

    /*  THE RULE. `fileChannels` the cue's own width, `trackChannels` the
        voice's; one entry per slot in `switchedIn` and `shapes`. */
    InsertChain chainOf (int fileChannels, int trackChannels,
                         const std::vector<bool>& switchedIn, const std::vector<InsertShape>& shapes);

    /*  WHICH SET ENTRY IS WHICH SLOT: the graph's, in slot order, when the
        table says there is one; the set's own order otherwise. */
    std::vector<std::string> slotIdsOf (const juce::ValueTree& plugins, const plugin::PluginTable* table);

    /** Each slot's shape off the table; unknown for an entry not loaded, or with no table. */
    std::vector<InsertShape> shapesOf (const std::vector<std::string>& slotIds, const plugin::PluginTable* table);

    /*  One media cue's chain, read off the document the way the runner sends
        its inserts: which of the slots it switches in, their shapes, its
        file's width and the voice's. */
    InsertChain chainOfCue (const juce::ValueTree& cue, const plugin::PluginTable* table, int trackChannels);
}
