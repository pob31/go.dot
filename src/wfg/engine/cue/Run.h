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
    A run: the live instance of a launched cue.

    WHY RUNS EXIST IN PHASE 2 AT ALL, when the plan could have waited. Because a
    fade has to target a live level and a stop has to target a live instance,
    and neither of those is the cue. The document says what somebody DECIDED -
    a media cue's `level` is the level it was authored at - and the run says
    what is HAPPENING. PRD §4.10 is the whole reason they are two things: a
    fade writes `/godot/run/<id>/level` and leaves the document alone, so the
    show still says what the designer chose after a night of riding faders.

    ONE CUE CAN HAVE SEVERAL RUNS, eventually. Not in Phase 2 - decision B of
    2026-09-05 says a GO on a media cue that is already running is ignored - but
    the identifier is a run's own rather than the cue's precisely so that Phase
    3 can make them plural per group without renaming anything.

    THE TICK THREAD OWNS THIS, exclusively. Every field is written by a command
    handler and read by the tree's publish, both on that thread. Nothing here is
    atomic and nothing needs to be; the audio side reports what it did by
    SUBMITTING a command, not by reaching in.

    NOTHING HERE IS PERSISTED. Every row is `persist = none`. A run is what the
    machine happened to be doing, which §4.10 keeps out of the document - and a
    show reloaded is a show with nothing running, which is the truth.

    WHAT PHASE 2 DOES NOT DO. Runs are not pruned: a finished run stays as
    `done` and keeps its address, so a client that asked what happened can still
    be told. The plan puts kill/advance/prune in Phase 3, where a group's
    several runs make "which one" a real question.
*/

#include <string>
#include <vector>

namespace wfg::cue
{
    /*  The states a run passes through, spelled as the parameter table spells
        them. They are strings on the wire because that is what a client reads;
        the enum is what the engine switches on.
    */
    namespace runState
    {
        /** Its track is reserved and its media is being made ready. */
        inline constexpr const char* armed = "armed";

        /** The launch has been placed, or has happened. */
        inline constexpr const char* playing = "playing";

        /** A stop has been asked for and has not landed yet. */
        inline constexpr const char* stopping = "stopping";

        /** It finished, or it was stopped. Its track is free. */
        inline constexpr const char* done = "done";

        /** It never played, and `error` says why. Its track is free. */
        inline constexpr const char* failed = "failed";
    }

    /*  Why a run failed, spelled once so the engine and a client agree.

        These are not command rejection reasons: a GO that fails this way was
        APPLIED - it was a legal thing to ask for at a moment when it could not
        be done - and the log says so. A rejection means the request was
        malformed; a failed run means the show is not in a state to honour it.
    */
    namespace runError
    {
        /** Every track was busy. A playing track is never stolen. */
        inline constexpr const char* noTrack = "no-track";

        /** The cue names a file the bundle does not have. */
        inline constexpr const char* mediaMissing = "media-missing";

        /** The cue names a bus, or a width, the rig cannot honour. */
        inline constexpr const char* badRoute = "bad-route";
    }

    //==============================================================================
    struct Run
    {
        /** Generated, and logged as applied like every generated identifier. */
        std::string id;

        /** The cue this instantiates, and that cue's kind. */
        std::string cue;
        std::string kind;

        std::string state { runState::armed };

        /*  The fixed track it plays on, or -1 before one is reserved and after
            it is given back. Media only: a fade occupies no track. */
        int track = -1;

        /*  Seconds into the file. A READOUT, never a model input and never
            logged - §3.15's rule that transitions are events and continuous
            values are not. */
        double position = 0.0;

        /*  The level it is playing at, in dB. This is what a fade writes, and
            it is NOT the cue's authored `level` - that one stays where the
            designer left it. */
        double level = 0.0;

        /*  How many blocks the launch was late by, when GO arrived before the
            arm had finished. Zero is the ordinary case and the number is worth
            having: it is the difference between "GO is instant" as a claim and
            as a measurement. */
        int late = 0;

        /** From `runError`, when `state` is failed. Empty otherwise. */
        std::string error;

        bool isFinished() const noexcept
        {
            return state == runState::done || state == runState::failed;
        }

        /** Whether it is holding a track, and so whether that track is busy. */
        bool holdsTrack() const noexcept
        {
            return track >= 0 && ! isFinished();
        }
    };

    //==============================================================================
    /*  Every run of this session, in the order they were created.

        A VECTOR AND NOT A MAP, deliberately. The tree publishes them in address
        order every tick and the count is small - a show is hundreds of cues,
        not millions - so the lookup this saves would cost the ordering the
        publish needs, and an ordering computed per tick from a map would be the
        thing that showed up in a profile.

        Tick thread only.
    */
    class RunTable
    {
    public:
        /*  Adds a run in `armed` with no track. The caller supplies the id,
            because generating it is the command layer's business and the log
            has to carry it.

            IT RETURNS NOTHING, and that is a fix rather than an omission. It
            returned `Run&` first, and the second create moved the vector out
            from under the first caller's reference - a use-after-free that a
            test found by crashing, and that would have found the Runner later
            and less kindly. Every caller looks a run up by identifier, which is
            what the command handlers were all doing anyway. */
        void create (std::string id, std::string cueId, std::string kind);

        Run* find (const std::string& id);
        const Run* find (const std::string& id) const;

        /** The run currently instantiating `cueId` and not yet finished, or
            null. Decision B of 2026-09-05 is what asks this question: a GO on a
            cue that is already running is applied and does nothing. */
        const Run* liveRunOf (const std::string& cueId) const;

        /** Whether any unfinished run holds this track. */
        bool isTrackBusy (int track) const;

        /*  The lowest track no unfinished run holds, from a rig of `trackCount`
            tracks, or -1 when every one is busy.

            LOWEST RATHER THAN ROUND-ROBIN, and it matters for reading a log: a
            show replayed puts the same cue on the same track, so two logs of
            one session compare line for line. A playing track is never stolen. */
        int lowestFreeTrack (int trackCount) const;

        const std::vector<Run>& all() const noexcept { return runs; }

        void clear() { runs.clear(); }

    private:
        std::vector<Run> runs;
    };
}
