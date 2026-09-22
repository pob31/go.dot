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
    Which of the show's direct outs are free where a given cue plays, and which
    are not.

    IT COMPUTES NOTHING. The engine's liveness analysis does the work - it is
    the only half that can, since it owns the walk, the release rules and the
    group lengths a client never sees - and publishes the answer per cue as
    `outsBusy` and `outsMaybe`. This reads those two rows and turns them into
    the words a menu shows.

    THREE STATES AND NOT TWO (author, 2026-09-22: *"mark clearly the available
    direct outs and the ones taken... when uncertain, cue playing out until its
    end, then just don't mark it, the user will decide"*):

      taken     - something is provably on it while this cue plays
      undecided - something might be, and nothing in the document decides
      free      - nothing is

    AND NEVER A REFUSAL. The menu offers every output whatever its mark: two
    cues on one out sum, which is better than a cue that plays nowhere
    (*"better have two summed signals in the same channel than none or a broken
    cue or cuelist"*). The mark informs the decision; it does not take it.
*/

#include <wfg/client/model/OutputList.h>

#include <optional>
#include <string>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    struct OutMark
    {
        enum class State { free, undecided, taken };

        std::string busId;

        /** The cue in the way, when there is one. Its name, ready to show. */
        std::string byCue;

        State state = State::free;
    };

    /*  Every direct out of the show, in output-list order, marked for this
        cue. Empty when the cue is not a media cue or the show has no direct
        outs.
    */
    std::vector<OutMark> readOutMarks (const tree::TreeSnapshot&, const std::string& cueId);

    /*  The same, for a caller that has already read the output list. Every
        read of the tree here is a linear scan, and the inspector takes several
        per click already - so the one caller that has the rows in hand passes
        them rather than making this find them again. */
    std::vector<OutMark> readOutMarks (const tree::TreeSnapshot&, const std::string& cueId,
                                       const std::vector<OutputRow>& outputs);

    /*  The label a menu item carries: "Main L/R · Stereo — taken by "Rain"",
        or nothing after the name where the answer is undecided. §4.8: the word
        does the telling, so this reads without a legend and without colour.
    */
    std::string markedLabel (const std::string& name, const std::string& widthWord,
                             const OutMark&);

    /*  WHAT A MOVE JUST DID TO ONE CUE'S OWN OUTPUT, in a sentence, or nothing
        when it did nothing.

        Called with the cue's marks from before the move and after it: an
        output that has gone from free or undecided to taken is an overlap the
        move created, and that is worth one line on the notice strip. Anything
        that was already true stays quiet - a warning that fires for a state
        somebody has already seen is a warning they learn to ignore.

        `busName` and `cueName` are asked of the caller rather than read here,
        because the reading that has them is the one the window already took.
    */
    std::optional<std::string> newClash (const std::vector<OutMark>& before,
                                         const std::vector<OutMark>& after,
                                         const std::string& ownOut,
                                         const std::string& busName,
                                         const std::string& cueName);
}
