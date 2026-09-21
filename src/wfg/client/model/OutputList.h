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
    The show's outputs, as a list somebody can read and rearrange.

    One row per bus, in `firstChannel` order — which the engine's layout
    commands keep the same as document order, so this is also the order the
    file reads. Each row says what the output is for (a direct out, where one
    cue's channels land, or a mix channel many cues send into), how wide it is
    in the words a designer uses rather than a number, and which interface
    channels it occupies.

    IT ALSO SAYS WHICH REGIME THE PATCH IS IN, because that is the one thing
    about this list that is not obvious from looking at it. While a show is
    fresh the interface patch follows the list: adding a stereo mix at the top
    moves everything below it, and that is what the designer wants while they
    are arranging the rig. Once the patch has settled — somebody has patched by
    hand, or a cue has played — the outputs keep the channels they are plugged
    into and a new one takes the next free ones. The two behave completely
    differently and look identical, so the window says which it is in a
    sentence (WFS-DIY's own answer: it prints a drag hint per regime).

    std only, and a pure reading: it takes the snapshot the window already
    holds and answers with plain strings, so a test can check the sentence and
    the ordering with no window and no engine.
*/

#include <cstddef>
#include <string>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    struct OutputRow
    {
        std::string id;
        std::string name;

        /** "direct" or "mix", as the engine spells it. */
        std::string kind;

        int width = 1;
        int firstChannel = 0;

        /** "Direct out" / "Mix channel", for a column somebody reads. */
        std::string kindWord() const;

        /*  "Mono", "Stereo", or "6 channels" for a processor send. The two
            words are what the author asked for; the number is the honest
            answer for anything else and is not offered in the menu. */
        std::string widthWord() const;

        /*  The interface channels it occupies, one-based for a person:
            "1", "3-4", "9-20". Channel numbers a designer counts from one, and
            every address inside the engine counts from nought. */
        std::string channelWord() const;
    };

    /*  Every bus, in list order. Buses whose `kind` the engine has not
        published read as direct outs, which is that row's own default. */
    std::vector<OutputRow> readOutputs (const tree::TreeSnapshot&);

    /** How many interface channels the outputs occupy altogether. */
    int outputChannelCount (const std::vector<OutputRow>&);

    /*  Whether the patch has stopped following the list. True when the engine
        says so, and also when a patch has been written or the layout is not
        packed — the same three facts `doc::applyLayoutEdit` reads, asked here
        so the sentence and the engine cannot disagree about which regime is
        in force. */
    bool patchHasSettled (const tree::TreeSnapshot&);

    /** The sentence under the output list, which regime it is in. */
    std::string outputRegime (bool settled);

    /*  One label per LOGICAL output channel, for the rows of the patch matrix:
        "Main L/R · L", "Main L/R · R", "Voice", "WFS send · 3". A channel no
        output claims — which a hand-written layout can leave — reads "Output 7"
        so the row still says what it is. */
    std::vector<std::string> channelLabels (const std::vector<OutputRow>&, int atLeast);
}
