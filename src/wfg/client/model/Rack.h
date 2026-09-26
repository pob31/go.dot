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
    The live rack, as the Rack tab reads it (Phase 9b, namespace draft §18.3).

    One row per rack channel, in the show's order: what somebody called it,
    what it takes in and puts out, and its own chain of plugins in the order
    they process - each entry read exactly as the Plugins tab reads one of the
    set's, since the two are the same element. A mic cue names one of these
    channels and switches in what it wants of the chain (decision BX).

    AND THE DELAY, SAID AGAINST THE BUDGET (decision BY): what the chain would
    make a microphone late by with every plugin in, in milliseconds, beside
    the show's budget - never refused, never compensated, said. It is the
    worst case, so it is known when somebody adds a plugin and not discovered
    on the night; a plugin that has not loaded has declared nothing yet, and
    the sentence says which are not counted rather than pretending they add
    nothing.

    std only, and a pure reading of the snapshot the window already holds.
*/

#include <wfg/client/model/Fx.h>

#include <string>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    struct RackChannelRow
    {
        std::string id;
        std::string name;

        /** mono, monoToStereo or stereo: the row's own word. */
        std::string channelClass = "mono";

        /** The chain, in the order it processes. */
        std::vector<PluginRow> chain;

        /*  Every plugin in, in samples: the engine's sum of what each has
            declared. Nought for an empty chain, and for one that has not
            loaded. */
        int latencySamples = 0;

        /** "Mono", "Mono to stereo", "Stereo". */
        std::string classWord() const;

        /** "No plugins", "1 plugin", "3 plugins". */
        std::string chainWord() const;
    };

    struct RackReading
    {
        std::vector<RackChannelRow> channels;

        /** How much delay a mic cue's plugins may add, in milliseconds (`audio/rackBudget`). */
        double budgetMs = 5.0;

        /** The engine's rate, to turn samples into milliseconds; nought with no audio open. */
        int sampleRate = 0;
    };

    /** Every rack channel, in the show's order, each with its chain. */
    RackReading readRack (const tree::TreeSnapshot&);

    /** Whether a channel's worst case is past the budget. Never with no audio open. */
    bool overBudget (const RackChannelRow&, const RackReading&);

    /*  THE WORST CASE AGAINST THE BUDGET, in one sentence for the foot of the
        tab: "7.3 ms at worst, with every plugin in - over the 5 ms budget."
        In samples when no audio is open, since milliseconds need a rate; and
        naming the plugins that have not loaded, which are not counted. */
    std::string budgetWords (const RackChannelRow&, const RackReading&);

    /** "5 ms", "2.5 ms": a tenth of a millisecond at most, never a locale question. */
    std::string millisecondWords (double milliseconds);
}
