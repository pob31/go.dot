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
#include <cstdint>
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
        /*  WHAT IT IS PLAYING, AND HOW FAR THROUGH IT IS, as numbers rather
            than as the words above - because a bar has to be drawn from them
            (author, 2026-09-18: "can we have the waveform beneath the media
            cues in the active cue panel with a progress bar showing where the
            playhead is? Pre-waits and post-waits can also have progress bars,
            maybe running the opposite way, right to left, as a countdown").

            `file` is what the CUE names, which is also the key the analyser
            holds its table under - so two cues playing one file share one
            analysis and one bar. A file with no analysis yet has no bar, which
            is the engine's own answer for "not been round to it" and is drawn
            as nothing rather than as a flat line (§3.30): a file being
            analysed is not a file with no sound in it.

            `length` is the cue's duration and NOT the run's: a run knows where
            it has got to and the document knows how long the thing is. It
            reads nought for a file imported in this session, because the
            duration map is frozen at load - so the playhead stays at the left
            rather than sliding across a bar nobody has measured. */
        std::string file;
        double seconds = 0.0;      ///< `position`, as a number
        double length = 0.0;       ///< the cue's `duration`, or 0 when unknown
        double remaining = 0.0;    ///< seconds left of the wait it is in
        double waitTotal = 0.0;    ///< how long that wait is, from the cue
        std::int64_t started = 0;  ///< the tick GO was applied on, or 0 before one was

        /** The run that holds this one, or empty at the top. What the nesting is drawn from. */
        std::string parentRun;

        /** Whether it is counting down: a pre-wait or a post-wait, which read alike. */
        bool isWaiting() const noexcept;

        std::string mark() const;

        /** "playing" or "armed" as a word, whatever `mark()` draws. */
        const std::string& stateWord() const noexcept { return state; }

        /** True once a launch has happened, which is when `position` means anything. */
        bool launched() const noexcept;
    };

    /*  Every run the engine is publishing, nested by parent and ordered the
        way the show HAPPENED (author, 2026-09-18: "order items in the active
        cues by start time", and "the yellow ring marked next cue should always
        sit at the bottom to reflect the structure of the cuelist").

        NOT THE ENGINE'S OWN ORDER, which is the order runs were CREATED - and
        those differ exactly where it matters. A cue the anticipation window
        prepared is created before the things already sounding, so the run
        table puts the NEXT cue above them: upside down from where an operator
        looks for it, and the one row in the pane whose position carries a
        meaning. Ordered by `started`, with the not-yet-started last, the pane
        reads top to bottom like the list it came from.

        NESTING SURVIVES IT. Siblings are ordered among themselves and each
        parent keeps its children directly under it, so a group's members do
        not scatter up the pane among things that started between them - the
        structure is what the indent is drawing, and an order that broke it
        would be two claims about the same rows. */
    std::vector<RunRow> readRuns (const tree::TreeSnapshot& snapshot);

    /*  The ordering `readRuns` finishes with, on its own so a test can hand it
        rows and assert the rule rather than building a tree to imply it. */
    std::vector<RunRow> inShowOrder (const std::vector<RunRow>& rows);
}
