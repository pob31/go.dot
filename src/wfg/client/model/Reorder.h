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

#include <optional>
#include <string>
#include <vector>

namespace wfg::client::model
{
    enum class DropKind
    {
        none,       ///< letting go here does nothing: the row itself, or a band
        after,      ///< `object.move` into `container` at `index`
        into,       ///< `object.move` into the group `container`, at its end
        target,     ///< `node.set <cueId>/target <dragged>`
        preset,     ///< `node.set <dragged>/preset <cueId>`: prepared by that group's header
        footer,     ///< move into the footer of the group `cueId`, made first if it has none
        clearPreset ///< `node.set <dragged>/preset ""`: no longer prepared ahead
    };

    struct Drop
    {
        DropKind kind = DropKind::none;
        std::string container;   ///< for after/into: the list or group moved into
        int index = -1;          ///< for after: the member position; -1 is the end
        std::string cueId;       ///< for target: the fade or stop being aimed
    };

    /*  WHICH COLOUR SAYS WHAT LETTING GO WOULD DO (author, 2026-09-21: "so the
        drag and drop has a clear colour coding for the user to be sure what
        they're doing").

        Four of these kinds land ON a row rather than between two rows, and
        every one of them used to light it the same green - so the hand had to
        remember which modifier it was holding to know which of four quite
        different things was about to happen. They are told apart by tone now:
        green into a group, blue for a fade being aimed, red for a mark that
        this group's HEADER prepares the cue, purple into a footer.

        A THEME TOKEN AND NOT A COLOUR, because this library names no JUCE type
        and because the four are rethemeable like everything else. The kinds
        that draw a LINE between two rows rather than lighting one - `after` -
        and the ones that draw nothing keep the plain tone; `dropTone` answers
        `drop-into` for them, which nothing reads.

        Colour is not the only carrier (§4.8): `describe` below says the same
        thing in a sentence under the list, and always has. */
    std::string dropTone (DropKind kind);

    /*  THE CONTAINER A ROW IS IN, as `object.move` names it: a member's is its
        parent; a row inside a header, a footer or a persistent section is in
        that section, whose own identifier the row carries. */
    std::string containerOf (const Row& row);

    /*  SHIFT+ALT ONTO A GROUP TITLE MOVES THE CUE INTO ITS FOOTER (author,
        2026-09-18: "the footer items are moved to the footer with shift+alt
        drag and drop on the group title, or drag and drop directly in the
        footer if it already exists"). A group's footer takes any cue; the
        window makes the footer first when the group has none. */
    Drop footerDropFor (const Row& over, const Row& dragged);

    /*  A DERIVED HEADER LINE DRAGGED (author, 2026-09-18: "dragging a preset
        line out of the header should remove it from the header; if it falls
        on a different group top line or header, then move this preset").
        The line is the mark, so dragging it moves the mark: onto a group the
        cue is inside, or that group's header band, and the cue is prepared
        there instead; onto the group it already names, nothing; anywhere else
        - another row, or no row at all - and the mark is cleared. `over` is
        null when the hand let go on nothing. */
    Drop presetLineDropFor (const Row* over, const std::string& cueId, const std::string& current,
                            const std::vector<Row>& rows);

    /*  EDITING IN THE LIST (author, 2026-09-18: "edit the userID, name,
        prewait, duration and postwait right in the cue list by double
        clicking"). Which attribute a column writes, or nothing when that
        column is not the cue's to write: a media cue's duration is its
        file's and a memo has none, so only a fade's or a stop's is a box. */
    enum class EditCell { none, number, name, preWait, duration, postWait };

    std::string editAttributeFor (EditCell cell, const std::string& kind);

    /*  What letting go of `dragged` over `over` would do, `fraction` being how
        far down the row the pointer is (0 at the top, 1 at the bottom). */
    Drop dropFor (const Row& over, const Row& dragged, double fraction);

    /*  The identifier of the one cue `text` names: by identifier, else by
        number, else by name. Empty when none does, or more than one. */
    std::string resolveCueRef (const std::string& text, const std::vector<Row>& rows);

    /*  THE PRESET GESTURE (author, 2026-09-18: "drag and drop with alt onto
        a group label adds this cue to the header"): with alt held, letting
        go on a group the dragged cue is INSIDE marks the cue as prepared by
        that group's header - one write to the cue's `preset`, which is the
        decision; the header line is a reading of it (§13.7). A group the cue
        is not inside is refused here, because the engine would only warn
        that no header will ever prepare it. */
    Drop presetDropFor (const Row& over, const Row& dragged, const std::vector<Row>& rows);

    /*  The groups `cueId` is inside, innermost first, by the rows' parent
        chain; the list itself is not among them. */
    std::vector<std::string> ancestorsOf (const std::string& cueId, const std::vector<Row>& rows);

    /*  CTRL/⌘-UP AND -DOWN MOVE THE PRESET THROUGH THE ANCESTORS (author:
        "this way we can move the preload/preset up or down nested groups").
        Up is outward, towards the list: from no preset to the innermost
        group, then each group further out, stopping at the outermost. Down
        is inward, ending at no preset. Nothing to move to answers nothing. */
    std::optional<std::string> presetStep (const std::string& cueId, const std::string& current,
                                           int direction, const std::vector<Row>& rows);

    /*  The words a drop is announced with, for the reader; empty for none.

        `intoTimeline` says the container the cue would land in is a TIMELINE
        group, whose members start together at entry, each offset by its own
        pre-wait - so their order on screen is not the order they play in,
        and a reorder there changes the reading and nothing else. The author
        moved cues about in one and found "discrepancies between the displayed
        order and the playing order" (2026-09-18); this is where the window
        says so, before the hand lets go. */
    std::string describe (const Drop& drop, const Row& over, bool intoTimeline);
}
