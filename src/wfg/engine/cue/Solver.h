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
    WHAT THE SHOW WOULD BE, AT A POSITION SOMEBODY NAMED.

    PRD §3.13. An operator says "take it back to cue 12" and what they mean is a
    state: which cues are sounding and how far in, what values the desk should
    be holding, which faders are partway through a move. Reconstructing that is
    what makes a rehearsal jump possible at all, and it is a function of the
    document rather than of anything that happened.

    THE COORDINATE IS A CUE AND AN OFFSET, NEVER A WALL TIME. A manual list has
    no time in it - between two GOs there is however long the actor took - so
    "the state at 04:12" is a question the document cannot answer. What it can
    answer is *cue C has been running for `offset` seconds*, with **-1 meaning
    before C has fired at all**: standby on C, nothing of C done. That is the
    position an operator actually asks for, and it is what a step in the list's
    history is.

    ONE FORWARD PASS, AND §3.13 SAYS BACKWARD. The section's step 1 is "walk
    back through the list accumulating the last writer of each parameter", which
    describes the ANSWER rather than the direction - and the direction turns out
    to matter. A backward walk cannot know when it is finished, because the set
    of parameters the show writes is not known until the whole list has been
    read; so it walks to the top in the general case and pays for the machinery
    of stopping early without ever stopping early. A forward pass from the top
    accumulates last-writers into a map, tracks lifetimes as it goes, and
    answers both halves in one sweep at O(cues). The state it computes is
    identical, and the wording is a PRD amendment to propose rather than a
    disagreement about behaviour.

    WHAT A STRUCTURAL WAYPOINT BOUNDS, WHICH IS HALF OF WHAT IT LOOKS LIKE.
    §3.13 makes group boundaries structural waypoints so the walk "can stop at
    one rather than going to the top of the show". That is true of WHAT IS
    RUNNING and false of WHAT VALUES ARE SET: when a group completes, its footer
    has run and nothing it started is still going - that is what blocking
    footers buy - but the values its cues wrote are still there. So the run half
    starts at the last completed boundary and the VALUE HALF IS THE FULL PASS,
    which is cheap because it is one map and one sweep. Saying so is what keeps
    somebody from later optimising the value half to stop at a boundary and
    quietly losing a level somebody set in act one.

    THE CONFUSED LIST IS THE FEATURE AND NOT THE APOLOGY. §3.24: "a confused
    solver that says so is better than one that guesses." Four things the walk
    cannot know - an infinite range fired at an earlier manual step, a shuffled
    group drawn on a night nobody recorded, a media file whose length will not
    read, a manual loop the operator was inside - and each of them lands
    somewhere stated and says which.

    THREADING: none of its own, and no side effects at all. Tick thread, or a
    test with no engine anywhere.
*/

#include <wfg/engine/osc/OscValue.h>

#include <map>
#include <string>
#include <vector>

namespace wfg::doc { class ShowDocument; }
namespace wfg::tree { class MountTable; }

namespace wfg::cue
{
    /** Where the operator is pointing: a cue, and how far into it. */
    struct Aim
    {
        std::string list;
        std::string cue;

        /*  Seconds into the cue, or -1 for BEFORE it has fired - standby on it,
            nothing of it done. The two are genuinely different positions and an
            operator asks for both: "back to cue 12" is -1, and "back to where
            we were in the underscore" is a number. */
        double offset = -1.0;
    };

    /** One cue that should be sounding, and where it should have got to. */
    struct PlannedRun
    {
        std::string cue;

        /** Its groups, outermost first. Empty for a cue at the top of a list. */
        std::vector<std::string> ancestors;

        /** Seconds into the cue's own material. */
        double offset = 0.0;

        /*  Which of the cue's ranges, counting from nought, and which pass of
            it. -1 and 1 for a cue with no ranges, which plays its file once. */
        int range = -1;
        int pass = 1;
    };

    /*  One value the desk should be holding, and who put it there.

        EVENT-KIND NODES ARE NOT IN THIS LIST (§3.13 step 4). A jump is a
        statement about state, and re-sending a value whose meaning is "do this
        now" would fire the pyro again on the way past. */
    struct PlannedValue
    {
        std::string address;
        osc::Value value;

        /** The cue that wrote it last, so a client can say where it came from. */
        std::string writer;
    };

    /*  A level that is not where its cue says, because a fade moved it.

        `base + Σ trims`, which is PR 3.12's shape: a fade that completed before
        the aim contributes its whole change, and one the aim lands inside
        contributes the part of it that has happened. */
    struct PlannedTrim
    {
        std::string cue;
        double decibels = 0.0;
    };

    /** Something the walk could not know, and what it did instead. */
    struct Confusion
    {
        std::string cue;
        std::string reason;
        std::string took;
    };

    /*  Why a solve was confused. A short vocabulary, because each of these
        sends a different person to look at a different thing. */
    namespace confusion
    {
        /** A range that plays for ever, fired at an earlier manual step. */
        inline constexpr const char* endlessRange = "endless-range";

        /** A shuffled group whose order was drawn on a night nobody recorded. */
        inline constexpr const char* drawnOrder = "drawn-order";

        /** A media file whose length this build could not read. */
        inline constexpr const char* unknownLength = "unknown-length";

        /** A looping group: standby says which member and nothing says which
            round. */
        inline constexpr const char* unknownRound = "unknown-round";
    }

    //==============================================================================
    struct Plan
    {
        Aim aim;

        /** Where the pointer should land: positionally after the target (§3.5). */
        std::string standby;

        std::vector<PlannedRun> runs;
        std::vector<PlannedValue> values;
        std::vector<PlannedTrim> trims;
        std::vector<Confusion> confused;

        /** Whether the aim named anything this list holds. */
        bool ok = false;

        /** The plan as JSON, for `/godot/list/<id>/solve`. */
        std::string toJson() const;
    };

    //==============================================================================
    /*  The whole of it: a document, the media lengths, the mounted namespaces
        and an aim.

        `durations` may be nullptr - a replay, a test, a bundle with no media
        folder - and then every media length is unknown, which is a confused
        entry rather than a guess. `mounts` may be nullptr too, and then no node
        is known to be event-kind: every value is planned, which is the
        direction that re-fires something, so a caller with a mount table is
        expected to hand it over. Both absences are stated in the plan. */
    Plan solve (const doc::ShowDocument& document,
                const std::map<std::string, double>* durations,
                const tree::MountTable* mounts,
                const Aim& aim);
}
