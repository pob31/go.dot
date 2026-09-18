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
    What is running, as rows: the present tense of the show.

    WHY THIS IS NOT CACHED AND THE CUE LIST IS. The cue list is the document,
    which changes when somebody edits and not otherwise, so it is keyed on
    `/godot/document/revision` and rebuilt almost never. Runs are the opposite:
    they appear and end and move on every tick by design, and there is no
    revision to key them on because they are not a decision anybody recorded
    (§4.10). So this is read afresh every pass - which costs what it costs
    because a pane full of runs is a dozen rows, not five hundred.

    PRESENT TENSE, which is the engine's rule and not this file's: a finished
    run is kept for five seconds and then stops being published at all, so
    nothing here decides what to forget.

    A RUN'S DEPTH COMES FROM ITS PARENT CHAIN. A group's run holds its members'
    runs; `/godot/run/<id>/parent` names the holder and `/godot/run/order` gives
    them in the order the engine keeps, so the rows are drawn in that order and
    indented by how far their parents go up. Nothing is re-sorted here: the
    engine's order is the one the operator saw things start in.

    std only, like the rest of model/.
*/

#include <cstddef>
#include <string>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    struct RunRow
    {
        std::string id;
        std::string cueId;
        std::string cueName;     ///< read through the run's cue, so a row says what it IS
        std::string kind;
        std::string state;       ///< preparing, waiting, armed, playing, postWait, stopping, failed, done
        std::string position;    ///< seconds, one decimal; empty before a launch
        std::string error;       ///< why it failed, when it did
        std::string round;       ///< which round of a group, when it is in one
        bool pruned = false;
        bool asserted = false;
        int late = 0;            ///< samples, when the engine had to place a launch in the past
        int depth = 0;

        /*  THE TWO STATES A BUSY PANE IS FULL OF ARE A MARK, and every other
            one keeps its word - the author's own instruction to the page
            (2026-09-16: "'Armed' can be an icon or just the yellow mark and
            'Playing' can be just the green mark"). The words that are left -
            waiting, preparing, postWait, stopping, failed, done - are the ones
            an operator reads rather than recognises, and there are never many
            at once.

            THE MARKS DIFFER IN SHAPE BEFORE HUE (§4.8): a filled triangle for
            a cue that is sounding, a ring for one that is ready and has not
            been let go. A photographed screen, a warm projector or an eye that
            does not sort green from amber still tells them apart. */
        std::string mark() const;

        /** "playing" or "armed" as a word, whatever `mark()` draws. */
        const std::string& stateWord() const noexcept { return state; }

        /** True once a launch has happened, which is when `position` means anything. */
        bool launched() const noexcept;
    };

    /** Every run the engine is publishing, in its order, nested by parent. */
    std::vector<RunRow> readRuns (const tree::TreeSnapshot& snapshot);
}
