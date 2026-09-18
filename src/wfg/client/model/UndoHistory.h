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
    The undo history as a place to stand in, and what standing somewhere else
    would change (PRD §3.20, §4.3; author, 2026-09-18: "We also need an
    undo/redo history. This is an item in the show menu. This also opens in
    place of the Inspector and shows a diff overlay on the cues as the user
    drags a pointer. This is only applied with an OK button or dismissed with
    a Cancel button.").

    THE ENGINE OWNS THE STACK and publishes it as two lists of names:
    `document/undoHistory`, the transactions Undo would unmake newest first,
    and `document/redoHistory`, the ones Redo would put back nearest first.
    Standing somewhere in it is a NUMBER, the count of transactions applied;
    moving there is that many `undo` or `redo` records, each a transaction
    the log can replay. Nothing here undoes anything.

    THE DIFF IS THE WINDOW'S OWN READING. When the panel opens it takes a
    picture of the rows - what each cue says, by id - and every pass since
    compares the rows now against it: a cue that reads differently has
    changed, a cue that was not there is new, a cue that is gone is named.
    The picture is of the LIST as drawn, which is what the operator is
    looking at; a value only the inspector shows is not in it.

    std only, like the rest of model/.
*/

#include <wfg/client/model/ShowModel.h>

#include <map>
#include <string>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    struct UndoReading
    {
        std::vector<std::string> undo;   ///< newest first: what Undo unmakes, in order
        std::vector<std::string> redo;   ///< nearest first: what Redo puts back, in order

        /** How many transactions are applied: where the show stands. */
        int position() const noexcept { return static_cast<int> (undo.size()); }
    };

    UndoReading readUndoHistory (const tree::TreeSnapshot& snapshot);

    /*  ONE PLACE TO STAND, newest at the top: the transactions Redo would put
        back (not applied), then the ones Undo would unmake (applied), then
        the show as it was opened. `index` is the position standing AFTER the
        transaction means - what to move to when it is clicked. */
    struct Standing
    {
        std::string name;
        int index = 0;
        bool applied = false;
        bool opening = false;   ///< the row for "as opened", index 0
    };

    std::vector<Standing> standings (const UndoReading& reading);

    /*  A picture of the rows: what each cue says, and its name, by id. A
        band and a step row are not cues and are not in it. */
    struct Picture
    {
        std::map<std::string, std::string> saying;
        std::map<std::string, std::string> name;
    };

    Picture pictureOf (const std::vector<Row>& rows);

    struct Diff
    {
        std::vector<std::string> changed;   ///< ids that read differently now
        std::vector<std::string> added;     ///< ids not in the picture
        std::vector<std::string> removed;   ///< NAMES of cues no longer there

        bool empty() const noexcept { return changed.empty() && added.empty() && removed.empty(); }
    };

    Diff diff (const Picture& before, const Picture& now);
}
