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
    WHERE A LEADER IS, AS A FOLLOWER IS SEATED THERE (namespace draft §58,
    RD.8; AGE, AGF).

    A machine that starts following a leader already running cues - it was
    started late, it waited for its main and its own clients moved things
    meanwhile (AGK), or it is a main started again after a crash that follows
    the backup that took over (AFR) - is not where the leader is, and applying
    the leader's records from now on would never bring it there: the cues that
    were running stay unplayed, and the next GO lands on a standby that is not
    the leader's. So the leader sends its POSITION, and the follower is seated
    at it in one step - one record, `pair.seat`, logged, so a replay of the
    follower's session is seated where the session was - and applies the
    records after it on top.

    WHAT A POSITION CARRIES, and what it leaves out (each said where it is
    made, cue/RunnerSeat.cpp):
      - the rows `state.xml` holds, every one the show has - standby, the
        focus, a list run out, the lock, the folded flags - at its value or its
        default, so a standby the leader cleared is cleared here; never a row
        that names this machine's hardware (RD.3's `machine`), which each
        machine keeps for itself;
      - what a list keeps that `state.xml` does not: its history of steps, the
        aim and where the last jump landed, the GO debounce's and Doh!'s
        clocks, the count of GOs;
      - every run that is live - not finished, and not something the horizon
        only made ready, which each machine makes for itself - and the
        finished runs of every live scene, which the scene's job counts, with
        everything a run carries that its cue does not say: its state, its
        clocks, its playhead and the slice and pass it is in, its rounds and
        seed, its level, the hand on its fader, a held clip, what a fade moved;
      - the jobs carrying them on: each live scene's place among its members,
        each fade's progress, the runs that end on the next tick by
        themselves (a memo, a start cue, a stop cue's own run);
      - tonight's numbers that are not the show's: the DCA trims, the faders
        flipped to a cue and the lanes armed, the beds Esc paused, the cues a
        kill suspended;
      - the run-name salt and counts (RD.2's `Position`), less what the
        horizon made, so the follower's own horizon names what it makes as the
        leader's did (AGE);
      - the stream's counter and the tick it is true at.

    THE LOG'S OWN ATOM GRAMMAR, ONE LINE A THING. Not JSON: a position carries
    the salt and counters as 64-bit integers, which JSON reads as doubles and
    would cut, and every number here goes through the grammar the log and the
    document already write the same way on a French machine (`osc::formatDouble`)
    and read back exactly. A line is a word and its atoms (`run s:"R8M2K4Q1"
    ...`), so the text is read by the reader that already reads the log, and it
    rides inside `pair.seat`'s one string argument, its newlines escaped by
    that same grammar.

    TICKS ARE THE MACHINE'S OWN, and `rebase` moves every one of them: the
    leader writes its ticks, and the follower moves them by the offset its
    records are applied at (AGF's slot, RD.6) before it submits the seat - so
    the record it logs speaks this machine's ticks, and a replay of it needs
    nothing else.
*/

#include <wfg/engine/cue/FadeJob.h>
#include <wfg/engine/cue/GroupJob.h>
#include <wfg/engine/cue/ListState.h>
#include <wfg/engine/cue/RunNames.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::pair
{
    /*  ONE RUN, as the leader holds it. Every tick is absolute, on the clock of
        the machine the position is in; `position` and `slicePlayed` are the
        readouts at the position's tick. */
    struct SeatedRun
    {
        std::string id, cue, kind, parent, state;

        std::int64_t launchTick = 0;        // launchRequestedAtTick
        std::int64_t dueTick = 0;           // while waiting or in a post-wait
        std::int64_t postWaitBegan = -1;
        std::int64_t startedTick = -1;      // `run.started`
        std::int64_t endedTick = -1;

        bool launchRequested = false;       // asked to launch, the launch not placed yet
        bool launched = false;              // its launch placed: the sound has a playhead

        double position = 0.0;              // seconds into the file, the scene, or since its GO
        double slicePlayed = 0.0;           // seconds into the slice it plays, passes and all
        int range = -1;
        int pass = 0;
        double rate = 1.0;                  // the speed now, which the playhead moves at
        double ownRate = 1.0;

        //  A scene's rounds.
        int iteration = 0;
        int iterations = 1;
        std::int32_t seed = 0;
        std::vector<std::string> round;
        std::int64_t roundStartedTick = 0;
        std::int64_t firstRoundTick = 0;
        std::vector<std::string> pruned;
        std::string stopAfter;
        std::string enterAt;

        //  Tonight's on it.
        double ownLevel = 0.0;
        double trim = 0.0;
        std::string strip;
        bool sampler = false;
        bool held = false;
        std::string heldBy;
        bool solo = false;
        bool closing = false;
        std::vector<std::string> lostStrips;
        std::map<std::string, double> moved;

        bool stopAsked = false;
        bool stopEndsWait = false;
        bool skipFooter = false;
        bool killed = false;
        bool asserted = false;
        bool advanceRequested = false;
        int advanceTo = -1;
        std::uint64_t goSerial = 0;

        /** An OSC cue whose curves are playing: it has a playhead to be sought to. */
        bool curving = false;

        bool isFinished() const noexcept    { return state == "done" || state == "failed"; }
    };

    struct Position
    {
        /*  THE TEXT'S OWN FORMAT: a position from a build that writes another is
            refused, not half read. */
        static constexpr int format = 1;

        std::uint64_t counter = 0;          // the last of the leader's stream applied by then
        std::int64_t tick = 0;              // the tick it is true at, after that tick ran

        struct Row
        {
            std::string address, value;
        };

        std::vector<Row> rows;

        struct List
        {
            std::string id;
            cue::ListAim aim, landed;
            std::vector<cue::Step> history;
        };

        std::vector<List> lists;
        std::int64_t lastGoTick = -1;
        std::int64_t lastDohTick = -1;
        std::uint64_t goCount = 0;

        std::vector<SeatedRun> runs;
        std::vector<cue::GroupJob> groups;
        std::vector<cue::FadeJob> fades;

        /*  The runs that end on the next tick by themselves - a memo, a start
            cue, a stop cue's own run - which the engine's next tick reports
            ended: they end on the follower's next tick as they do here. */
        std::vector<std::string> finishing;

        std::map<std::string, double> dcas;
        std::string laneCue;
        std::vector<std::string> laneArmed;

        std::vector<std::string> suspended;
        std::int64_t liftedAt = -1;

        struct Paused
        {
            std::string cue, run;
            bool resumes = false;
            double from = 0.0;
            int range = -1;
            std::string media;
        };

        std::vector<Paused> paused;

        cue::RunNames::Position names;

        /** Every tick in it moved by `by`: the leader's clock read as this machine's. */
        void rebase (std::int64_t by);

        const SeatedRun* runNamed (const std::string& id) const;
    };

    /** The position as text, one line a thing, ending `end`. */
    std::string toText (const Position& position);

    /*  The text read back, or nothing - and `why` says what - for a line that
        is not one, a format this build does not write, or a text cut short. */
    std::optional<Position> readPosition (std::string_view text, std::string* why = nullptr);

    /** Read, moved by `by`, and written again: what a follower submits. */
    std::optional<std::string> rebasedText (std::string_view text, std::int64_t by);
}
