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
    A row dragged inside the cue list: what letting go would do.

    The author (2026-09-18): "Can we have drag and drop reordering? Can we
    also have drag and drop onto a fade to set its target?" The page declined
    dragging - a row that moves under the pointer while the tree is re-fetched
    is a fight nobody wins - and a compiled window has no poll to fight, so
    this is the first place the gesture is offered.

    THREE ANSWERS, TOLD APART BY WHERE ON THE ROW THE HAND IS, and said while
    it is still in the air. The middle band of a fade or a stop cue means
    "aim at this one": the fade's target becomes the dragged cue. The middle
    band of a group means "into this one": the dragged cue goes to the end of
    the group's members. Everywhere else means "after this one", in the row's
    own container - the same reading a dropped file gets, and for the same
    reason: it is the one that needs no second gesture.

    THE INDEX IS `object.move`'S, which is a MEMBER POSITION in the list as it
    stands with the dragged cue still in it. Dropped after a member that is
    BELOW it in the same container, the cue lands at that member's position
    and ends up directly after it (the document's own arithmetic, in
    ShowDocument::move); dropped after one ABOVE it, or into another
    container, it takes the position after that member. One rule here, so the
    drawing and the dropping cannot disagree.

    AND A CUE MAY BE NAMED BY ITS NUMBER OR ITS NAME, not only its identifier
    (author: "can we also use its user ID"): a fade's target typed as "1.3"
    or "Thunder" is resolved HERE to the identifier the document stores. The
    number is the operator's and "never an identity" (the parameter table's
    own words); what is written is the identity, so renumbering during tech
    breaks nothing. Ambiguity - two cues called Thunder - answers nothing
    rather than guessing, and the window says so.

    std only, like the rest of model/.
*/

#include <wfg/client/model/ShowModel.h>

#include <string>
#include <vector>

namespace wfg::client::model
{
    enum class DropKind
    {
        none,       ///< letting go here does nothing: the row itself, or a band
        after,      ///< `object.move` into `container` at `index`
        into,       ///< `object.move` into the group `container`, at its end
        target      ///< `node.set <cueId>/target <dragged>`
    };

    struct Drop
    {
        DropKind kind = DropKind::none;
        std::string container;   ///< for after/into: the list or group moved into
        int index = -1;          ///< for after: the member position; -1 is the end
        std::string cueId;       ///< for target: the fade or stop being aimed
    };

    /*  What letting go of `dragged` over `over` would do, `fraction` being how
        far down the row the pointer is (0 at the top, 1 at the bottom). */
    Drop dropFor (const Row& over, const Row& dragged, double fraction);

    /*  The identifier of the one cue `text` names: by identifier, else by
        number, else by name. Empty when none does, or more than one. */
    std::string resolveCueRef (const std::string& text, const std::vector<Row>& rows);

    /** The words a drop is announced with, for the reader; empty for none. */
    std::string describe (const Drop& drop, const Row& over);
}
