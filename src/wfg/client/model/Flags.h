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
    A CUE FLAGGED TO BE LOOKED AT AFTERWARDS (namespace draft §63; the author,
    2026-10-15: "flag a line so the operator can fix something afterwards. Use
    'f' on the keyboard to mark the cue and [shift]+F to mark the last cue that
    was triggered").

    The flag is the cue's `flagged` row, a state row: a locked show takes it,
    it lights no unsaved dot and Undo does not take it back. F writes it over
    the picked cues from here, as one `node.setMany`; Shift+F is the engine's
    `go.flag`, because which cue the last GO fired is the engine's to know.

    Kept apart from the row, as the readiness marks are: a state write moves
    no document revision, so the rows are not built again when a flag changes
    and the list reads the flags every pass instead. No JUCE.
*/

#include <wfg/client/model/Icons.h>
#include <wfg/client/model/ShowModel.h>

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    /*  THE MARK A FLAGGED ROW WEARS, before its name: a drawn flag, and in
        words what it means and how it comes off - the shape and the words,
        never the colour alone (PRD §4.8). */
    Mark flagMark();

    /*  EVERY FLAGGED CUE THE LIST DRAWS, by identifier: read off each cue
        row's own `flagged`, one address a row, as the readiness is. A derived
        header line is the member's own cue and is not counted twice. */
    std::set<std::string> flaggedOf (const tree::TreeSnapshot& snapshot, const std::vector<Row>& rows);

    /*  WHAT F DOES TO THE PICKED CUES, and what the foot says it did.

        ONE WAY FOR ALL OF THEM: every picked cue already flagged, and F takes
        the flags off; otherwise it flags the ones that are not, so a pick of
        three where one was flagged ends with three flagged rather than a
        mixture turned inside out. `writes` holds only the rows that change -
        one `node.setMany`, one record - and is empty when nothing would. */
    struct FlagToggle
    {
        std::vector<std::pair<std::string, std::string>> writes;
        bool flagging = true;
        std::string said;
    };

    FlagToggle flagToggle (const std::set<std::string>& flagged, const std::vector<std::string>& picked,
                           const std::vector<Row>& rows);

    /*  The foot's sentence for F with nothing picked. */
    std::string nothingPickedToFlag();
}
