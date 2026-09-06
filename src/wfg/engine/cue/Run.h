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

#include <cstdint>
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
        /*  Its pre-wait is running and nothing has fired yet.

            A STATE AND NOT A GAP. A cue with a two-second pre-wait is a cue
            somebody can see coming, so it has an address and a state from the
            moment GO is pressed rather than appearing when it finally makes a
            sound. It is also what a group waits on: a member in its pre-wait is
            a member that has not finished. */
        inline constexpr const char* waiting = "waiting";

        /** Its track is reserved and its media is being made ready. */
        inline constexpr const char* armed = "armed";

        /** The launch has been placed, or has happened. */
        inline constexpr const char* playing = "playing";

        /** A stop has been asked for and has not landed yet. */
        inline constexpr const char* stopping = "stopping";

        /*  Its own work is over and its post-wait is running.

            §3.6: a post-wait is "how long after completion this cue reports done
            to its parent", so the run is genuinely not finished yet - a sequence
            group holding on it must keep holding. Publishing it as `done` and
            keeping a private timer would tell every client the opposite of what
            the group is doing. */
        inline constexpr const char* postWait = "postWait";

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

        /*  A message could not be put on the wire.

            Distinct from every refusal above it because it is the only one that
            is not about the show: the cue was right, the node was right, and
            the socket would not take the bytes. It is also the only failure a
            `sent` wait can produce that a `none` wait cannot - which is the
            entire practical difference between the two, and the reason `sent`
            is worth having. */
        inline constexpr const char* sendFailed = "send-failed";
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

        //======================================================================
        /*  THE TREE. A group run is the live instance of a group, and its
            members are runs of their own with it as their parent.

            RUNS AND NOT CUES, because the same cue can be a member of a group
            that is running twice - a shuffled loop plays it in round three and
            again in round five - and "which one finished" has to have an
            answer. §3.6's completion table is read off this: a sequence group
            advances when its current child is `done`, and is itself done when
            the last one is.

            The parent is an identifier rather than a pointer for the reason
            everything else here is: `RunTable` is a vector and a vector moves. */
        std::string parent;
        std::vector<std::string> children;

        //======================================================================
        /*  THE WAITS, IN TICKS, COPIED FROM THE CUE WHEN THE RUN IS CREATED.

            Copied rather than looked up, so that editing a cue under a run that
            is already waiting changes the NEXT run and not this one - the same
            rule §4.10 applies to every other authored value a run instantiates.
            (A group's mode and advance are deliberately not copied: §3.6 says a
            mid-run toggle takes effect at the next member boundary, which means
            reading them there.)

            In ticks rather than seconds because that is the clock every
            deadline in the engine is kept in, and converting once at creation
            is one place to be wrong instead of one per tick. */
        int preWaitTicks = 0;
        int postWaitTicks = 0;

        /*  When the wait it is in comes due, as an ABSOLUTE tick.

            The `FadeJob::stopsAtTick` shape, for the reason that one gives:
            nothing between here and there can move it. Meaningful only while
            `state` is `waiting` or `postWait`.

            It is computed in a command HANDLER, from the command's own tick,
            and never from the Runner's idea of the current tick - a handler
            runs during a replay and the hooks do not, so the Runner's tick is
            zero there and a deadline built on it would be due immediately. */
        std::int64_t dueTick = 0;

        //======================================================================
        /*  ENGINE STATE, PUBLISHED NOWHERE. These three are how the Runner
            remembers what it is in the middle of; a client has no use for them
            and no business writing them, so they carry no parameter row.

            They are here rather than in a table beside the Runner because they
            are facts about this run, and two places to look for a run's state
            is one place too many. */

        //======================================================================
        /*  ROUNDS. A group plays its members in rounds - one round is one pass
            through them - and `loops` says how many. Everything here belongs to
            a group run and is left at its default by every other kind.

            THE ROUND IS MATERIALISED RATHER THAN COMPUTED, which is what makes
            a shuffled show reproduce. It is drawn once, written into the log as
            `run.round`, and read back from there on replay - so the random
            number generator is consulted on the night and never again. */
        std::vector<std::string> round;

        /** Which round is in progress, counting from one; 0 before the first. */
        int iteration = 0;

        /*  How many rounds this run will play, copied from the group when it
            started - so an edit to `loops` changes the next run and not this
            one, the same rule the waits follow. Zero is for ever. */
        int iterations = 1;

        /*  The members dropped from this run, and it is RUN-LOCAL by design
            (§3.6): what an operator does at 22:40 when one of eight ambiences
            is wrong tonight. It evaporates with the run, because it is not an
            edit to the show - tomorrow the cue is back. */
        std::vector<std::string> pruned;

        /*  What the rounds are drawn from. The group's own seed when it has
            one, and a fresh one otherwise - drawn when the run starts and
            written into the log, so a shuffled night reproduces exactly and is
            still different from the next one. */
        std::int32_t seed = 0;

        /*  A boundary this run is to stop at, or empty: "member" for the end of
            the one playing now, "iteration" for the end of this round.

            ENGINE STATE AND PUBLISHED NOWHERE, like the three below it. It is
            set by a stop that asked for a graceful exit and read at the next
            boundary; a client watching the group sees the group still playing,
            which is exactly what is happening. */
        std::string stopAfter;

        /*  WHERE A GROUP RUN IS TO START, when the pointer was not on its
            first member. A cue identifier, and always one of this group's own
            members - so descending three levels sets it on three runs, each
            naming the member that the level below is inside of.

            ON THE RUN RATHER THAN ON THE JOB, which is the point. GO creates a
            run for every manual group between the pointer and the list, because
            the record has to carry all of them and a replay never draws an
            identifier of its own - but it FIRES only the outermost, and each of
            the others is started by its parent's job at the moment §3.6 puts
            it, which is after that parent's header. A job that does not exist
            yet cannot be told where to enter; the run it will belong to can.

            Empty means the first member, which is the ordinary case: the
            pointer descends to member one and GO there is what created the
            group. */
        std::string enterAt;

        /** A GO has happened and the launch has not been placed yet. */
        bool launchRequested = false;

        /*  The tick that GO was applied on, so that lateness can be MEASURED.

            `run.late` has been declared, documented and tested since PR 2.3 and
            nothing has ever produced it, because `launchIfDue` has no way to
            know what the launch instant SHOULD have been - it computes one from
            the tick it happens to run on, which is by construction never late.
            The difference between that tick and this one, in blocks, is the
            number the command was written for. */
        std::int64_t launchRequestedAtTick = 0;

        /*  The tick it reached `done` or `failed`, or -1 while it has not.

            Set in the handler that finishes the run, so it is the same number
            live and on replay. Retention reads it - see `retentionTicks`. */
        std::int64_t endedAtTick = -1;

        /** The audio side has confirmed a voice and made the media ready. */
        bool armConfirmed = false;

        /** The sample the launch was placed at. Zero before it is placed. */
        std::int64_t launchedAtSample = 0;

        /*  Whether the last poll saw it sounding. The edge from true to false
            is what ends a run - a launcher clip stops itself at the end of its
            length, so this is the ordinary finish as well as how a stop is
            noticed. */
        bool sawPlaying = false;

        /*  Whether this run was KILLED rather than stopped, which for a group
            decides whether its footer runs.

            §4.4 draws the distinction before Phase 10 builds the keys for it.
            Esc is graceful: it stops what is running and RUNS FOOTERS, "the
            same code path as normal completion, entered early - a group aborted
            at 04:12 releases its channels and kills its LFOs exactly as it
            would have at 06:00". Double Esc is immediate and skips them.

            A stop cue is the first; `run.kill` is the second. Both write
            `stopping`, because both are true statements about the run - so the
            state alone cannot say which was meant, and this is what does. */
        bool skipFooter = false;

        /*  Whether the tick thread has already told the audio side to stop this
            run's voice.

            ONCE, not every tick it reads `stopping`. Tracktion takes a repeated
            stop on a stopped voice quietly, so the repetition would be harmless
            in the show - but a stop is an OBSERVABLE, and a Player that counted
            two of them is a Player whose tests cannot say whether the second
            one was meant. Issuing it once is also the only version that can be
            described in one sentence. */
        bool stopIssued = false;

        bool isFinished() const noexcept
        {
            return state == runState::done || state == runState::failed;
        }

        /** Whether this run is a group's, and so has children rather than a
            voice. A group organises time, order and lifetime and owns no
            output of its own (§4.12). */
        bool isGroup() const noexcept { return kind == "group"; }

        /** Whether it is holding on a wait, either end. */
        bool isWaiting() const noexcept
        {
            return state == runState::waiting || state == runState::postWait;
        }

        /** Whether it is holding a track, and so whether that track is busy. */
        bool holdsTrack() const noexcept
        {
            return track >= 0 && ! isFinished();
        }
    };

    //==============================================================================
    /*  HOW LONG A FINISHED RUN KEEPS ITS ADDRESS, in ticks. Five seconds.

        Gogo is the pure present tense (PRD §7) and a four-hour show would
        otherwise publish four hours of finished runs on every one of its
        720 000 ticks. Five seconds is long enough for a client polling at the
        tick rate to see the `done` it was waiting for, and for a person to read
        it.

        IT RETIRES FROM THE TREE AND NOT FROM THE TABLE, and the difference is
        the whole reason this is a constant here rather than an erase somewhere.
        Pruning the table would make the model depend on a hook, and hooks do not
        run during a replay: a `run.kill` arriving six seconds after its run
        finished would be REJECTED live (the run is gone) and APPLIED on replay
        (it is not), which is a session that does not reproduce itself. Nothing
        else can tell the difference - `liveRunOf` and `lowestFreeTrack` both
        skip finished runs already - so what is left is a tree that stops
        growing, which was the only real problem.
    */
    inline constexpr int retentionTicks = 250;

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
        void create (std::string id, std::string cueId, std::string kind,
                     std::string parentRun = {});

        /*  Every unfinished child of a group run, in the order they were
            spawned. Answered by scanning rather than by trusting the parent's
            own list, because the two could disagree and only one of them is
            what the runs actually say. */
        std::vector<const Run*> childrenOf (const std::string& parentRun) const;

        /** Whether every child of this group run has finished. An empty group
            is complete, which is the honest answer and not a special case. */
        bool allChildrenFinished (const std::string& parentRun) const;

        /*  Whether this group run has ever spawned a run for that cue -
            finished or not.

            What tells a manual group that is WAITING for its next GO from one
            that is OVER. The two look identical from the outside: no child is
            running either way. */
        bool hasChildFor (const std::string& parentRun, const std::string& cueId) const;

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
