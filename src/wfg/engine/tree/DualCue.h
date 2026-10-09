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
    A MOVIE AND THE SOUND LOCKED TO IT, AS ONE CUE OF TWO LINES (namespace
    draft 37.5, WM), read from the tree alone: the sound locked to the movie,
    straight after it in the same section of the same container - the rule the
    cue list draws the pair by.

    One rule for the two that ask: the window, whose pick of a movie aims the
    control surfaces at its sound (§47, AAA, `client/model/Dual`), and the
    surfaces themselves, whose SELECT on a movie's strip does the same (§49,
    ABG). No JUCE.
*/

#include <string>

namespace wfg::tree
{
    class TreeSnapshot;

    struct DualCue
    {
        std::string movie;      ///< the video cue
        std::string sound;      ///< the media cue locked to it, on the line below

        bool isPair() const noexcept  { return ! movie.empty() && ! sound.empty(); }
    };

    /*  The pair `cueId` is either half of, or nothing: a movie with no sound
        locked to it, a sound locked to a movie that is not straight above it,
        or any other cue. */
    DualCue dualCueOf (const TreeSnapshot& snapshot, const std::string& cueId);
}
