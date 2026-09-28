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
    The show's named inputs, as a list somebody can read, rearrange and watch
    (Phase 9b, namespace draft §18.2).

    The output list's twin for the other side of the interface: one row per
    named input, in first-channel order, each saying how wide it is in words,
    which logical inputs it takes - and, what the outputs have no use for, how
    loud it is right now and why it is not arriving when it is not. That last
    pair is the soundcheck: a microphone that is dead is seen on this list
    before anybody presses GO.

    The patch rule is the outputs' too: while the show is fresh the input patch
    follows this list, and once somebody patches by hand it is settled and each
    input keeps the interface channels it is on. The same sentence says which.

    std only, and a pure reading of the snapshot the window already holds, so a
    test can check the rows, the words and the labels with no window at all.
*/

#include <string>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    struct InputRow
    {
        std::string id;
        std::string name;

        int width = 1;
        int firstChannel = 0;

        /*  The loudest sample on any of its channels over the last tick, in
            dBFS, -120 for silence, and why it is not arriving - empty when it
            is. Both the engine's readings, never the show's. */
        double meterDb = -120.0;
        std::string problem;

        /** "Mono", "Stereo", or "4 channels". */
        std::string widthWord() const;

        /*  The logical inputs it takes, one-based for a person: "1", "3-4".
            Logical, not the interface's: the input patch is what maps them. */
        std::string channelWord() const;

        /*  How much of a meter to light, nought to one: -60 dB and below is
            nothing and full scale is everything, a fader's own sense of what
            is worth drawing. */
        double meterFill() const;
    };

    /*  Every named input, in list order: first logical input, ties broken by
        identifier. An input with no name reads "Input N" by its first channel. */
    std::vector<InputRow> readInputs (const tree::TreeSnapshot&);

    /** How many logical inputs the named inputs take altogether. */
    int inputChannelCount (const std::vector<InputRow>&);

    /*  Whether the input patch has stopped following the list - the engine's
        flag, a written patch, or a layout that is not packed: the three facts
        `doc::applyLayoutEdit` reads, asked here so the window's sentence and
        the engine cannot disagree. */
    bool inputPatchHasSettled (const tree::TreeSnapshot&);

    /** The sentence under the input list, which regime it is in. */
    std::string inputRegime (bool settled);

    /*  One label per logical input, for the rows of the input patch: "Voix
        solo", "Keys · L", "Keys · R". A logical input no named input takes
        reads "Input 7". */
    std::vector<std::string> inputChannelLabels (const std::vector<InputRow>&, int atLeast);
}
