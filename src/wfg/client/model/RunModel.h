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

#include <wfg/client/model/Ranges.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <set>

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

        /*  WHETHER A GROUP RUN CAN BE SCRUBBED: its cue is a timeline, or a
            sequence that advances on its own, so the engine can say where
            every member is at a second of it. A manual sequence has an
            operator between its members and no second to seek to. */
        bool timedGroup = false;

        /*  WHETHER A SURFACE'S ROTARIES ARE AIMED AT THIS RUN'S CUE (author,
            2026-09-25): a click on a media run's name aims them, and the row
            says so in a mark and not a colour alone. */
        bool aimed = false;
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

        /*  WHICH STRETCH OF THE FILE THIS CUE ACTUALLY PLAYS (author,
            2026-09-21: *"in the running cues, the part of the waveform between
            the first in point and last out point should be displayed"*).

            A cue with ranges walks regions of its file and may touch none of
            the rest of it; a strip drawn over the whole file then spends most
            of its width on material nobody will hear, and the playhead crawls
            across a picture that is mostly irrelevant. So the strip is the
            span between the EARLIEST in-point and the LATEST out-point -
            earliest and latest rather than first and last in the playlist,
            because 3.24 lets a cue walk its file out of order and the span is
            a fact about the FILE rather than about the running order.

            With no ranges it is the start offset to the end of the file, which
            is the same rule said of a cue that plays straight through: the
            seconds before the offset are never heard either.

            Nought to nought when nothing is known, and the strip then draws
            what it drew before: the whole of whatever it has. */
        double playFrom = 0.0;
        double playTo = 0.0;

        /*  WHICH RANGE IS SOUNDING AND WHICH PASS OF IT, from the engine's own
            readouts. `rangeIndex` is -1 for every kind but media and for a
            media cue with no ranges; `rangeIteration` counts from one.

            They are read so the strip can say WHY the playhead jumped: a loop
            sends it back to its range's in-point on every pass (the engine
            wraps it, `Runner::updatePositions`), and a head that leaps
            backwards over a picture with no marks on it reads as a fault
            rather than as a repeat. */
        int rangeIndex = -1;
        int rangeIteration = 0;

        /** This cue's regions, so the strip can draw where one ends and the next begins. */
        std::vector<RangeRow> ranges;

        double remaining = 0.0;    ///< seconds left of the wait it is in
        double waitTotal = 0.0;    ///< how long that wait is, from the cue
        std::int64_t started = 0;  ///< the tick GO was applied on, or 0 before one was

        /** The run that holds this one, or empty at the top. What the nesting is drawn from. */
        std::string parentRun;

        /*  WHAT IT IS WAITING FOR, as `run/pending` spells it: the slots it
            has claimed and not been given, by identifier, or the word `voice`
            for a sampler member armed onto a strip while every track is busy
            (§16.5). Empty when it waits for nothing. */
        std::string pending;

        /*  WHAT A SAMPLER RUN READS, in words beside its name (namespace
            draft §16.7). A sampler group's run counts its members - "armed 5
            · pending 3 · playing 1" - because a bank of pads is a dozen rows
            that say the same thing, and the count is what somebody looks for.
            A member says which strip it is on - "on 3", fader three - and,
            when there is more to say than its mark does, what that strip is
            doing: pending, held, stopping, closing. Empty for every other run. */
        std::string samplerWords;

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

    /*  A SAMPLER GROUP'S MEMBERS, COUNTED IN WORDS: "armed 5 · pending 3 ·
        playing 1", from the runs whose parent is `groupRunId`. A member
        waiting for anything - its strip, or a voice - counts as pending
        whatever its state says, since it cannot be played until it is given
        one; one that has been let go and is sounding, or counting down to
        sounding, is playing. A count of nought is left out, and a group with
        nothing to count reads empty. On its own so a test can hand it rows. */
    std::string samplerCounts (const std::vector<RunRow>& rows, const std::string& groupRunId);

    /** Session-local history, retained after failed runs leave the active pane.
        Dismissing entries keeps visible failures marked as already observed. */
    class CueErrorLog
    {
    public:
        bool observe (const std::vector<RunRow>& rows);
        void clear() { entries.clear(); }
        void dismiss (std::size_t index)
        { if (index < entries.size()) entries.erase (entries.begin() + static_cast<std::ptrdiff_t> (index)); }
        const std::vector<RunRow>& errors() const noexcept { return entries; }
    private:
        std::vector<RunRow> entries;
        std::map<std::string, std::set<std::string>> seen;
    };
}
