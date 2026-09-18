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
    Which cues are picked: one, or several, and which of them is the anchor.

    THE SELECTION IS THE CLIENT'S AND NEVER THE ENGINE'S (§14.1): what somebody
    has highlighted on their screen is not a decision about the show, so none
    of this reaches `submit`. What it decides is what the inspector is about
    and what a batch edit, a delete or a copy acts on.

    THE GESTURES ARE THE ONES EVERY LIST HAS. A plain click picks one cue and
    makes it the anchor. Ctrl/⌘-click toggles a cue in or out and leaves the
    rest. Shift-click picks everything between the anchor and the cue, in
    the order the list draws them, cues only - a band is a heading and cannot
    be picked. The anchor is the cue somebody clicked last on purpose, and it
    is what "after the picked cue" means when a new cue is made.

    std only, like the rest of model/.
*/

#include <wfg/client/model/ShowModel.h>

#include <cstddef>
#include <string>
#include <vector>

namespace wfg::client::model
{
    class Selection
    {
    public:
        /*  A click on `id`, with the modifiers a hand held, over the rows as
            the list draws them. `extend` is shift, `toggle` is ctrl/⌘; both
            held reads as extend. */
        void click (const std::string& id, bool extend, bool toggle, const std::vector<Row>& rows);

        /** Exactly this cue, as a plain click would. Empty clears. */
        void set (const std::string& id);

        /** Every cue row the list draws, in its order; the anchor stays if it is among them. */
        void all (const std::vector<Row>& rows);

        void clear();

        /*  Drops what the show no longer has: after a delete, an undo or a
            revert the ids that went are not picked any more. */
        void retain (const std::vector<Row>& rows);

        bool contains (const std::string& id) const;
        bool empty() const noexcept { return chosen.empty(); }
        std::size_t size() const noexcept { return chosen.size(); }

        /** In the order they were picked, which is the order a batch acts in. */
        const std::vector<std::string>& ids() const noexcept { return chosen; }

        /** The cue clicked last on purpose; empty when nothing is picked. */
        const std::string& anchor() const noexcept { return anchorId; }

    private:
        std::vector<std::string> chosen;
        std::string anchorId;
    };
}
