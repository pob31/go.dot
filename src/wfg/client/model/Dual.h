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
    A MOVIE AND THE SOUND LOCKED TO IT, as one cue of two lines (namespace
    draft 37.5, WM; §47, AAD).

    The list draws the pair as one cue when the sound sits straight after its
    movie in the same container (ShowModel's rule); the window then lets either
    line edit both halves - the author, 2026-10-09: "linked audio tracks should
    also enable to edit the video track so it doesn't require the user to
    switch back and forth." `dualOf` answers the same question from the tree
    alone, so the inspector, the foot panel and the surfaces agree with what is
    drawn without a list model in the room.

    And what a pick in the window aims the control surfaces at (§47, AAA): the
    author, 2026-10-09, "when a cue is selected for editing, the controller's
    EQ, sends, FX buttons should act as physical short cuts". A sound or a mic
    cue is aimed at itself; a movie at the sound locked to it, which is the
    half that has an EQ, sends and inserts; anything else leaves the aim where
    it was, so picking a memo while a page is up does not take the page away.
*/

#include <string>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    struct Dual
    {
        std::string movie;      ///< the video cue
        std::string sound;      ///< the media cue locked to it, on the line below

        bool isPair() const noexcept  { return ! movie.empty() && ! sound.empty(); }
    };

    /*  The pair `cueId` is either half of, or nothing: a movie with no sound
        locked to it, a sound locked to a movie that is not straight above it,
        or any other cue. */
    Dual dualOf (const tree::TreeSnapshot& snapshot, const std::string& cueId);

    /*  The cue a window pick of `cueId` aims the surfaces at, or empty for a
        pick that leaves the aim alone. */
    std::string aimForPick (const tree::TreeSnapshot& snapshot, const std::string& cueId);
}
