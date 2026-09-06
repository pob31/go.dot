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
    One group in flight: what the scheduler is holding while a group runs.

    A GROUP ORGANISES TIME, ORDER AND LIFETIME AND OWNS NO OUTPUT (§4.12). So
    this carries no level, no track and no media - only the position it has
    reached among its members and what it is waiting for. Everything a member
    does belongs to the member's own run.

    IT HOLDS NO DOCUMENT REFERENCE, deliberately, and reads `mode` and
    `advance` from the show at every boundary rather than copying them once.
    §3.6: "Toggleable during tech; a mid-run change takes effect at the next
    member boundary." A copy would make that sentence false, and a designer
    flipping a group from manual to auto during a plotting session would find
    it took effect tomorrow. The WAITS are copied, because a wait is a duration
    the run instantiated (§4.10) rather than a rule it obeys.

    A VALUE THE RUNNER HOLDS, like FadeJob and OscJob: it owns nothing, touches
    nothing, and everything it decides it decides by submitting.
*/

#include <cstdint>
#include <string>
#include <vector>

namespace wfg::cue
{
    /*  Where a group run has got to. §3.6 gives a group a header, its members
        and a footer, in that order, and the footer BLOCKS - the group is not
        done until the footer's cues report done, so a following scene that
        reallocates the same channels waits for the release rather than racing
        it.
    */
    namespace groupPhase
    {
        /** Before anything: the group's own pre-wait. */
        inline constexpr const char* entering = "entering";

        /*  Its header: an ordinary cue list that runs before the members, as a
            sequence whatever the group's own mode is.

            A SEQUENCE ALWAYS, because a header is preparation and preparation
            has an order: §3.12 puts prepare/commit here, and "pre-position
            sources, preload media, pre-arm bindings for eight cues" is a list
            of steps rather than a set of things to fire at once. The group's
            `mode` describes what it does with its MEMBERS. */
        inline constexpr const char* header = "header";

        /** Its members, scheduled per its mode. */
        inline constexpr const char* members = "members";

        /*  Its footer, which BLOCKS: the group is not done until the footer's
            cues report done (§3.6), so a following scene that reallocates the
            same interface channels waits for the release rather than racing it.

            It runs on the way out however the group is leaving - the end of its
            members, or a stop cue aimed at it. The one thing that skips it is
            `run.kill`, which is the emergency path and asks nothing of the cue. */
        inline constexpr const char* footer = "footer";

        /** Done, and waiting for nothing. The job is retired next tick. */
        inline constexpr const char* complete = "complete";
    }

    //==============================================================================
    struct GroupJob
    {
        /** The group's own run. Its cue is the Group, its children are the
            members' runs. */
        std::string run;

        /** From `groupPhase`. */
        std::string phase { groupPhase::entering };

        /*  How far along the member list it has got, for a sequence. One past
            the last member means every member has been launched - which is not
            the same as every member having FINISHED, and the difference is what
            a blocking parent waits on. */
        std::size_t nextMember = 0;

        /*  How many of this group's children have been told to begin.

            SPAWNING IS NOT LAUNCHING, which is the distinction that makes an
            auto sequence able to pay the disk in advance - and it means a
            timeline group, which schedules everything at entry, has to do both.
            It spawns on one tick and launches on the next, and this is what
            stops it launching the same member every tick afterwards: a media
            child stays `armed` from its launch until the sound starts, so
            "still armed" is not the question. "Not yet told" is. */
        std::size_t launched = 0;

        /*  The member currently waited on, in a sequence. Empty in a timeline
            group, where nothing is waited on individually because everything
            was scheduled at entry. */
        std::string awaiting;

        /*  The cues of the phase in progress, and how far along them it is.

            Reused by the header, the members and the footer rather than three
            sets of fields, because all three are the same job: a list of cues,
            spawned in order, waited on. What differs is only which list, and a
            timeline group's members are the one case that launches them all at
            once instead of one at a time. */
        std::vector<std::string> phaseCues;

        /** Finished and waiting to be forgotten. */
        bool retired = false;
    };
}
