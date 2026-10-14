/* This file is part of Go.dot — https://github.com/pob31/go.dot
 *
 * Copyright (C) 2026 Pierre-Olivier Boulant
 *
 * Go.dot is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. Go.dot is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * (LICENSE, at the repository root) for more details.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*  A run: the live instance of a launched cue, and every transition it makes.

    NO AUDIO IN THIS FILE, on purpose. A run's whole life is commands applied by
    the tick thread, and the audio side takes part by SUBMITTING them rather
    than by reaching in - so the lifecycle is testable, and replayable, on a
    machine with no sound card. That is not a convenience: it is the property
    that makes a log of a performance reproducible somewhere else, and it would
    not be true of a model the message thread wrote into directly.

    What is asserted here is the rule from §3.15 - state transitions are events,
    continuous readouts are not - and the handful of decisions that fall out of
    it. Reports that arrive out of order do not undo one another. A failed run
    stays failed and keeps saying why. `late` records the worst, not the last.
    And decision B of 2026-09-05: arming a cue that is already running is
    APPLIED and changes nothing, because an operator who pressed GO twice made a
    legal request the show had already honoured.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/log/Replay.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/tree/Touches.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace wfg;

namespace
{
    struct Rig
    {
        Rig()
        {
            engine.log().openInMemory ({});

            doc::registerDocumentCommands (engine.commands(), document);
            cue::registerRunCommands (engine.commands(), runs);

            /*  The Runner with NO PLAYER, which is a complete configuration and
                not a degraded one: `audio.arm` creates the run and stops there,
                because there is no audio side to reserve a voice on. That is
                exactly the shape a replay runs in, and it is why every case in
                this file is about the model rather than about a sound. */
            cue::registerGoCommands (engine.commands(), engine, runner,
                                     document, focus, runIds);

            listId = document.createList ("Sound").id;
            mediaId = document.createCue (listId, 0, "media", "Thunder").id;
            memoId = document.createCue (listId, 1, "memo", "House to half").id;
        }

        /** Submits one command and applies it, returning the tick's outcome. */
        Engine::TickResult run (const std::string& name, std::vector<osc::Value> args)
        {
            REQUIRE (engine.submit ("cli", name, std::move (args)));
            return engine.processTick (tick++);
        }

        /*  The identifier the LOG recorded, not the one the table happens to
            hold. That is the whole assertion: a generated value only makes a
            session reproducible if it reaches the record, and reading it back
            out of the record is the only way to know that it did. */
        std::string lastAppliedArgument (std::size_t index)
        {
            const auto parsed = LogFile::parse (engine.log().contents());
            REQUIRE_FALSE (parsed.records.empty());

            const auto& args = parsed.records.back().args;
            REQUIRE (args.size() > index);

            return args[index].getString();
        }

        std::string arm (const std::string& cueId)
        {
            run ("audio.arm", { osc::Value::string (cueId) });
            return lastAppliedArgument (1);
        }

        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::Focus focus;
        doc::IdRegistry runIds { doc::IdRegistry::withSeed (7) };
        cue::Runner runner { document, runs, runIds, focus };

        std::string listId, mediaId, memoId;
        std::int64_t tick = 0;
    };
}

//==============================================================================
TEST_CASE ("run: arming a media cue creates one, and the log carries the identifier it drew")
{
    Rig rig;

    const auto outcome = rig.run ("audio.arm", { osc::Value::string (rig.mediaId) });
    CHECK (outcome.applied == 1);

    /*  The generated identifier is written back into the record as the argument
        the caller left out, so a replay re-supplies it rather than drawing
        again - which is what lets a session reproduce without the engine's
        randomness having to be deterministic. */
    const auto id = rig.lastAppliedArgument (1);
    REQUIRE_FALSE (id.empty());

    REQUIRE (rig.runs.all().size() == 1u);

    const auto* created = rig.runs.find (id);
    REQUIRE (created != nullptr);
    CHECK (created->cue == rig.mediaId);
    CHECK (created->kind == "media");
    CHECK (created->state == cue::runState::armed);

    /*  No track yet. Arming creates the run that will carry what happens; the
        audio side says which voice it got, and says it in its own record. */
    CHECK (created->track == -1);
}

TEST_CASE ("run: a cue that plays nothing cannot be armed")
{
    /*  A memo cue armed would create a run that could never leave `armed`,
        which looks like progress and is not. */
    Rig rig;

    const auto outcome = rig.run ("audio.arm", { osc::Value::string (rig.memoId) });

    CHECK (outcome.applied == 0);
    CHECK (outcome.rejected == 1);
    CHECK (rig.runs.all().empty());
}

TEST_CASE ("run: arming a cue that is already running is applied and changes nothing")
{
    /*  DECISION B, 2026-09-05. Pressing GO twice is not a malformed request -
        it is a legal request the show has already honoured - so it is applied,
        the log says so, and the playing run carries on untouched. Rejecting it
        would put an R record in the log for an operator doing something
        ordinary. */
    Rig rig;

    const auto first = rig.arm (rig.mediaId);
    rig.run ("run.started", { osc::Value::string (first) });

    const auto outcome = rig.run ("audio.arm", { osc::Value::string (rig.mediaId) });

    CHECK (outcome.applied == 1);
    CHECK (outcome.rejected == 0);

    /*  One run, still playing, and the record names the run that already
        existed rather than one nobody created. */
    CHECK (rig.runs.all().size() == 1u);
    CHECK (rig.lastAppliedArgument (1) == first);
    CHECK (rig.runs.find (first)->state == cue::runState::playing);
}

TEST_CASE ("run: a finished cue can be armed again")
{
    /*  The other half of decision B. A cue that has played and ended is not
        running, so GO fires it again - which is the ordinary thing a cue does
        twice in a show. */
    Rig rig;

    const auto first = rig.arm (rig.mediaId);
    rig.run ("run.started", { osc::Value::string (first) });
    rig.run ("run.ended", { osc::Value::string (first) });

    const auto second = rig.arm (rig.mediaId);

    CHECK (second != first);
    CHECK (rig.runs.all().size() == 2u);
}

//==============================================================================
TEST_CASE ("run: the whole lifecycle, in the order the audio side reports it")
{
    Rig rig;

    const auto id = rig.arm (rig.mediaId);
    const auto* run = rig.runs.find (id);
    REQUIRE (run != nullptr);

    CHECK (run->state == cue::runState::armed);
    CHECK_FALSE (run->holdsTrack());

    rig.run ("audio.armed", { osc::Value::string (id), osc::Value::int32 (3) });
    CHECK (run->track == 3);
    CHECK (run->holdsTrack());

    rig.run ("run.started", { osc::Value::string (id) });
    CHECK (run->state == cue::runState::playing);

    rig.run ("run.ended", { osc::Value::string (id) });
    CHECK (run->state == cue::runState::done);
    CHECK (run->isFinished());

    /*  Finished, so the track it was holding is free - but the run keeps its
        number, because "which voice did that cue use" is a question worth
        being able to answer afterwards. */
    CHECK_FALSE (run->holdsTrack());
    CHECK (run->track == 3);
}

TEST_CASE ("run: a report that arrives too late does not undo one that arrived on time")
{
    /*  The message thread and the tick thread run at their own speeds, so a
        start that overtook a stop is an ordinary race rather than a defect.
        What must not happen is a finished run coming back to life. */
    Rig rig;

    const auto id = rig.arm (rig.mediaId);
    rig.run ("run.ended", { osc::Value::string (id) });

    const auto late = rig.run ("run.started", { osc::Value::string (id) });

    /*  Applied - it is in the log, where somebody can see that it arrived and
        meant nothing - and the state does not move. */
    CHECK (late.applied == 1);
    CHECK (rig.runs.find (id)->state == cue::runState::done);

    const auto lateTrack = rig.run ("audio.armed", { osc::Value::string (id), osc::Value::int32 (5) });
    CHECK (lateTrack.applied == 1);
    CHECK (rig.runs.find (id)->track == -1);
}

TEST_CASE ("run: a failed run stays failed, and keeps saying why")
{
    /*  Both done and failed are finished, but only one of them carries an
        account of what went wrong. Overwriting it with `done` would throw away
        the only thing the operator can act on. */
    Rig rig;

    const auto id = rig.arm (rig.mediaId);

    rig.run ("run.failed", { osc::Value::string (id),
                             osc::Value::string (cue::runError::noTrack) });

    const auto* run = rig.runs.find (id);
    CHECK (run->state == cue::runState::failed);
    CHECK (run->error == cue::runError::noTrack);
    CHECK (run->track == -1);

    rig.run ("run.ended", { osc::Value::string (id) });

    CHECK (run->state == cue::runState::failed);
    CHECK (run->error == cue::runError::noTrack);
}

TEST_CASE ("run: killing is stopping, not done, because the sound has not stopped yet")
{
    /*  run.kill is the primitive Esc and double-Esc will be built on (PRD §4.4).
        It says a stop was asked for; the audio side says when it landed.
        Publishing `done` here would be publishing a silence that had not
        happened. */
    Rig rig;

    const auto id = rig.arm (rig.mediaId);
    rig.run ("run.started", { osc::Value::string (id) });

    rig.run ("run.kill", { osc::Value::string (id) });
    CHECK (rig.runs.find (id)->state == cue::runState::stopping);
    CHECK_FALSE (rig.runs.find (id)->isFinished());

    rig.run ("run.ended", { osc::Value::string (id) });
    CHECK (rig.runs.find (id)->state == cue::runState::done);
}

TEST_CASE ("run: killing something already finished is applied and does nothing")
{
    /*  An operator hitting the button twice is not making a mistake worth an R
        record, and a client that reconnected has no way to know what is still
        running. */
    Rig rig;

    const auto id = rig.arm (rig.mediaId);
    rig.run ("run.ended", { osc::Value::string (id) });

    const auto outcome = rig.run ("run.kill", { osc::Value::string (id) });

    CHECK (outcome.applied == 1);
    CHECK (rig.runs.find (id)->state == cue::runState::done);
}

TEST_CASE ("run: lateness records the worst it was, not the last thing said about it")
{
    /*  A run that was three blocks late and then reported zero was still three
        blocks late. A number that can be talked down is not worth publishing. */
    Rig rig;

    const auto id = rig.arm (rig.mediaId);

    rig.run ("run.late", { osc::Value::string (id), osc::Value::int32 (3) });
    CHECK (rig.runs.find (id)->late == 3);

    rig.run ("run.late", { osc::Value::string (id), osc::Value::int32 (0) });
    CHECK (rig.runs.find (id)->late == 3);

    rig.run ("run.late", { osc::Value::string (id), osc::Value::int32 (7) });
    CHECK (rig.runs.find (id)->late == 7);
}

TEST_CASE ("run: a report about a run nobody created is refused, not invented")
{
    Rig rig;

    for (const auto* name : { "run.started", "run.ended", "run.kill" })
    {
        INFO (name);
        const auto outcome = rig.run (name, { osc::Value::string ("NOSUCHID") });
        CHECK (outcome.rejected == 1);
    }

    CHECK (rig.runs.all().empty());
}

//==============================================================================
TEST_CASE ("run table: the lowest free track, and a playing one is never stolen")
{
    /*  LOWEST rather than round-robin, and it matters for reading a log: a show
        replayed puts the same cue on the same track, so two logs of one session
        compare line for line. */
    cue::RunTable table;

    CHECK (table.lowestFreeTrack (4) == 0);

    table.create ("R1", "C1", "media");
    table.find ("R1")->track = 0;
    CHECK (table.lowestFreeTrack (4) == 1);

    table.create ("R2", "C2", "media");
    table.find ("R2")->track = 1;
    CHECK (table.lowestFreeTrack (4) == 2);

    /*  The first finishes; its voice comes back, and comes back FIRST. */
    table.find ("R1")->state = cue::runState::done;
    CHECK (table.lowestFreeTrack (4) == 0);
    CHECK_FALSE (table.isTrackBusy (0));
    CHECK (table.isTrackBusy (1));
}

TEST_CASE ("run table: every voice busy is a real answer, not a wrapped one")
{
    /*  Minus one, and the caller turns that into run.failed no-track. Wrapping
        round to track 0 would steal a playing cue, which is the one thing a
        show must never do on its own. */
    cue::RunTable table;

    for (int track = 0; track < 3; ++track)
    {
        const auto id = "R" + std::to_string (track);
        table.create (id, "C", "media");
        table.find (id)->track = track;
    }

    CHECK (table.lowestFreeTrack (3) == -1);
    CHECK (table.lowestFreeTrack (4) == 3);

    /*  A rig with no tracks at all is a show with no audio, which is legal. */
    CHECK (table.lowestFreeTrack (0) == -1);
}

TEST_CASE ("run table: the live run of a cue is the newest unfinished one")
{
    cue::RunTable table;

    table.create ("R1", "C1", "media");
    table.find ("R1")->state = cue::runState::done;

    CHECK (table.liveRunOf ("C1") == nullptr);

    table.create ("R2", "C1", "media");
    REQUIRE (table.liveRunOf ("C1") != nullptr);
    CHECK (table.liveRunOf ("C1")->id == "R2");

    CHECK (table.liveRunOf ("C2") == nullptr);
}

//==============================================================================
/*  WHAT THE TICK READS, AND WHAT IT LETS GO OF (2026-10-13, namespace draft
    §31.5, M68).

    The table keeps every run of the session, so a replay agrees; until today
    every pass of the tick walked them all, and six looked each one up again by
    a walk, so an evening's thousands of finished runs made the Debug tick two
    seconds long. `current` is what the passes walk now: every unfinished run,
    and each finished one until `settle` finds it older than its retention.

    The cases below hold the table to the answers the walk of every run gave.
    The oracle is that walk, written out again here over `all()`, and every
    question is asked of a table whose settled runs it no longer looks at. */
namespace
{
    namespace oracle
    {
        std::vector<std::string> childrenOf (const cue::RunTable& table, const std::string& parent)
        {
            std::vector<std::string> out;

            for (const auto& run : table.all())
                if (run.parent == parent)
                    out.push_back (run.id);

            return out;
        }

        std::vector<std::string> descendantsOf (const cue::RunTable& table, const std::string& parent)
        {
            std::vector<std::string> out;

            for (const auto& child : childrenOf (table, parent))
            {
                out.push_back (child);

                for (const auto& deeper : descendantsOf (table, child))
                    out.push_back (deeper);
            }

            return out;
        }

        std::string liveRunOf (const cue::RunTable& table, const std::string& cueId)
        {
            std::string out;

            for (const auto& run : table.all())
                if (run.cue == cueId && ! run.isFinished() && run.state != cue::runState::preparing)
                    out = run.id;

            return out;
        }

        std::string preparedRunOf (const cue::RunTable& table, const std::string& cueId)
        {
            std::string out;

            for (const auto& run : table.all())
                if (run.cue == cueId && run.state == cue::runState::preparing)
                    out = run.id;

            return out;
        }

        std::string newestOf (const cue::RunTable& table, const std::string& cueId)
        {
            std::string out;

            for (const auto& run : table.all())
                if (run.cue == cueId)
                    out = run.id;

            return out;
        }

        std::string holderOf (const cue::RunTable& table, const std::string& slot)
        {
            for (const auto& run : table.all())
                if (! run.isFinished() && std::find (run.claims.begin(), run.claims.end(), slot) != run.claims.end())
                    return run.id;

            return {};
        }

        std::vector<std::string> waitersFor (const cue::RunTable& table, const std::string& slot)
        {
            std::vector<std::string> out;

            for (const auto& run : table.all())
                if (! run.isFinished() && std::find (run.pending.begin(), run.pending.end(), slot) != run.pending.end())
                    out.push_back (run.id);

            return out;
        }

        bool allChildrenFinished (const cue::RunTable& table, const std::string& parent)
        {
            for (const auto& run : table.all())
                if (run.parent == parent && ! run.isFinished())
                    return false;

            return true;
        }

        //  What `current` must hold at least: every unfinished run, in creation order.
        std::vector<std::string> unfinished (const cue::RunTable& table)
        {
            std::vector<std::string> out;

            for (const auto& run : table.all())
                if (! run.isFinished())
                    out.push_back (run.id);

            return out;
        }
    }

    std::vector<std::string> namesOf (const std::vector<const cue::Run*>& runs)
    {
        std::vector<std::string> out;

        for (const auto* run : runs)
            out.push_back (run->id);

        return out;
    }

    std::string nameOf (const cue::Run* run)
    {
        return run != nullptr ? run->id : std::string();
    }

    std::vector<std::string> currentOf (const cue::RunTable& table)
    {
        std::vector<std::string> out;

        for (const auto& run : table.current())
            out.push_back (run.id);

        return out;
    }

    bool reads (const cue::RunTable& table, const std::string& id)
    {
        const auto now = currentOf (table);
        return std::find (now.begin(), now.end(), id) != now.end();
    }

    //  Every question the table answers, against the walk of every run.
    void answersAsTheWalk (const cue::RunTable& table, const std::vector<std::string>& parents,
                           const std::vector<std::string>& cues, const std::vector<std::string>& slots, int tracks)
    {
        for (const auto& parent : parents)
        {
            INFO ("under " << parent);
            CHECK (namesOf (table.childrenOf (parent)) == oracle::childrenOf (table, parent));
            CHECK (namesOf (table.descendantsOf (parent)) == oracle::descendantsOf (table, parent));
            CHECK (table.allChildrenFinished (parent) == oracle::allChildrenFinished (table, parent));

            for (const auto& cueId : cues)
            {
                const auto children = oracle::childrenOf (table, parent);
                const auto expected = std::any_of (children.begin(), children.end(), [&] (const std::string& id)
                                                   { return table.find (id)->cue == cueId; });
                CHECK (table.hasChildFor (parent, cueId) == expected);
            }
        }

        for (const auto& cueId : cues)
        {
            INFO ("of " << cueId);
            CHECK (nameOf (table.liveRunOf (cueId)) == oracle::liveRunOf (table, cueId));
            CHECK (nameOf (table.preparedRunOf (cueId)) == oracle::preparedRunOf (table, cueId));
            CHECK (nameOf (table.newestOf (cueId)) == oracle::newestOf (table, cueId));
        }

        for (const auto& slot : slots)
        {
            INFO ("slot " << slot);
            CHECK (nameOf (table.holderOf (slot)) == oracle::holderOf (table, slot));
            CHECK (namesOf (table.waitersFor (slot)) == oracle::waitersFor (table, slot));
        }

        for (int track = 0; track < tracks; ++track)
        {
            const auto busy = std::any_of (table.all().begin(), table.all().end(), [track] (const cue::Run& run)
                                           { return run.holdsTrack() && run.track == track; });
            CHECK (table.isTrackBusy (track) == busy);
        }

        //  Every unfinished run is read, in creation order, and every name still found.
        const auto readNow = currentOf (table);
        std::vector<std::string> readUnfinished;

        for (const auto& id : readNow)
            if (! table.find (id)->isFinished())
                readUnfinished.push_back (id);

        CHECK (readUnfinished == oracle::unfinished (table));

        for (const auto& run : table.all())
            CHECK (table.find (run.id) == &run);
    }

    /*  AN EVENING IN MINIATURE: an act with a scene in it, members finished
        long ago and lately, a sound holding a slot another waits for, a cue run
        three times, a preparation - each ending at the tick given, or not. */
    void buildEvening (cue::RunTable& table)
    {
        const auto make = [&table] (const std::string& id, const std::string& cueId, const std::string& kind,
                                    const std::string& parent, const char* state, std::int64_t endedAt)
        {
            table.create (id, cueId, kind, parent);
            auto* run = table.find (id);
            REQUIRE (run != nullptr);
            run->state = state;
            run->endedAtTick = endedAt;
        };

        make ("ACT", "CACT", "group", "", cue::runState::playing, -1);
        make ("A1", "CA1", "memo", "ACT", cue::runState::done, 10);
        make ("SCN", "CSCN", "group", "ACT", cue::runState::done, 20);
        make ("S1", "CS1", "media", "SCN", cue::runState::done, 15);
        make ("S2", "CS2", "osc", "SCN", cue::runState::failed, 20);
        make ("K1", "CK", "media", "", cue::runState::done, 30);
        make ("A2", "CA2", "media", "ACT", cue::runState::playing, -1);
        make ("K2", "CK", "media", "", cue::runState::playing, -1);
        make ("HLD", "CH", "media", "", cue::runState::playing, -1);
        make ("WT", "CW", "media", "", cue::runState::armed, -1);
        make ("PRP", "CK", "group", "", cue::runState::preparing, -1);
        make ("K3", "CK", "media", "", cue::runState::done, 900);
        make ("OLD", "CH", "media", "", cue::runState::done, 40);

        table.find ("K2")->track = 0;
        table.find ("HLD")->track = 1;
        table.find ("K1")->track = 1;
        table.find ("HLD")->claims = { "SLOT" };
        table.find ("OLD")->claims = { "SLOT" };
        table.find ("WT")->pending = { "SLOT" };
    }

    const std::vector<std::string> eveningParents { "", "ACT", "SCN", "K1", "NOBODY" };
    const std::vector<std::string> eveningCues { "CACT", "CA1", "CSCN", "CS1", "CS2", "CK", "CA2", "CH", "CW", "NONE" };
    const std::vector<std::string> eveningSlots { "SLOT", "NOSLOT" };
}

TEST_CASE ("run table: a run ended longer ago than its retention leaves what the tick reads, and stays in the table")
{
    cue::RunTable table;
    buildEvening (table);

    REQUIRE (table.all().size() == 13u);
    CHECK (table.current().size() == 13u);
    answersAsTheWalk (table, eveningParents, eveningCues, eveningSlots, 3);

    /*  At tick 1000 every run that ended before 750 settles: A1, SCN, S1, S2,
        K1 and OLD. K3 ended at 900, inside its retention, and is read still,
        as the tree still publishes it. */
    table.settle (1000);

    CHECK (currentOf (table) == std::vector<std::string> { "ACT", "A2", "K2", "HLD", "WT", "PRP", "K3" });
    CHECK (table.all().size() == 13u);
    answersAsTheWalk (std::as_const (table), eveningParents, eveningCues, eveningSlots, 3);

    //  Nothing a const look moves: still settled.
    CHECK (std::as_const (table).find ("SCN") != nullptr);
    CHECK_FALSE (reads (table, "SCN"));

    /*  The retention's edge is the tree's: ended at 900, read through 1150 and
        gone at 1151. */
    table.settle (1150);
    CHECK (reads (table, "K3"));
    table.settle (1151);
    CHECK_FALSE (reads (table, "K3"));

    //  A run not over stays, whatever its ending tick says.
    table.find ("WT")->endedAtTick = 3;
    table.settle (5000);
    CHECK (reads (table, "WT"));
    CHECK (currentOf (table) == std::vector<std::string> { "ACT", "A2", "K2", "HLD", "WT", "PRP" });
    answersAsTheWalk (std::as_const (table), eveningParents, eveningCues, eveningSlots, 3);
}

TEST_CASE ("run table: a settled run found to be changed is read again, in its place, until it settles again")
{
    cue::RunTable table;
    buildEvening (table);
    table.settle (1000);

    REQUIRE_FALSE (reads (table, "SCN"));

    /*  BROUGHT BACK, as Doh! brings back an act ended six seconds ago: found to
        be changed, it is read again at once, between the act and its member,
        where it was made. */
    auto* scene = table.find ("SCN");
    REQUIRE (scene != nullptr);
    scene->state = cue::runState::playing;
    scene->endedAtTick = -1;

    CHECK (currentOf (table) == std::vector<std::string> { "ACT", "SCN", "A2", "K2", "HLD", "WT", "PRP", "K3" });
    answersAsTheWalk (std::as_const (table), eveningParents, eveningCues, eveningSlots, 3);

    table.settle (2000);
    CHECK (reads (table, "SCN"));                     // alive: it stays

    /*  FOUND AND LEFT AS IT WAS: read until the next settle, then let go. And
        moved under another parent while it was thawed - an adoption - it is
        filed under the new one when it settles again. */
    auto* member = table.find ("S1");
    REQUIRE (member != nullptr);
    CHECK (reads (table, "S1"));
    member->parent = "K1";

    answersAsTheWalk (std::as_const (table), eveningParents, eveningCues, eveningSlots, 3);

    table.settle (2000);
    CHECK_FALSE (reads (table, "S1"));
    CHECK (namesOf (table.childrenOf ("K1")) == std::vector<std::string> { "S1" });
    CHECK (namesOf (table.childrenOf ("SCN")) == std::vector<std::string> { "S2" });
    answersAsTheWalk (std::as_const (table), eveningParents, eveningCues, eveningSlots, 3);
}

TEST_CASE ("run table: a run found while the tick walks is reached by that walk, and the next walk is in order")
{
    cue::RunTable table;
    buildEvening (table);
    table.settle (1000);

    std::vector<std::string> walked;
    auto thawed = false;

    for (const auto& run : table.current())
    {
        walked.push_back (run.id);

        //  From inside the walk, as a pass that looks up a run's parent does.
        if (! thawed)
        {
            REQUIRE (table.find ("A1") != nullptr);
            thawed = true;

            //  Asked inside the walk, the table still answers as the walk of every run.
            CHECK (nameOf (table.liveRunOf ("CK")) == "K2");
            CHECK (namesOf (table.childrenOf ("ACT")) == std::vector<std::string> { "A1", "SCN", "A2" });
        }
    }

    CHECK (walked == std::vector<std::string> { "ACT", "A2", "K2", "HLD", "WT", "PRP", "K3", "A1" });
    CHECK (currentOf (table) == std::vector<std::string> { "ACT", "A1", "A2", "K2", "HLD", "WT", "PRP", "K3" });

    //  And let go again at the next settle - which never runs under a walk.
    for (const auto& run : table.current())
    {
        table.settle (1000);
        CHECK (! run.id.empty());
    }

    CHECK (reads (table, "A1"));
    table.settle (1000);
    CHECK_FALSE (reads (table, "A1"));
}

TEST_CASE ("run table: runs taken out at once are taken out as one at a time takes them")
{
    cue::RunTable atOnce, oneByOne;
    buildEvening (atOnce);
    buildEvening (oneByOne);
    atOnce.settle (1000);
    oneByOne.settle (1000);

    //  A seat takes out every run the leader's position does not hold: settled, read, a parent and its child.
    const std::set<std::string> gone { "A1", "SCN", "S1", "K2", "OLD", "NOBODY" };

    atOnce.remove (gone);

    for (const auto& id : gone)
        oneByOne.remove (id);

    REQUIRE (atOnce.all().size() == oneByOne.all().size());
    CHECK (atOnce.all().size() == 8u);

    for (std::size_t n = 0; n < atOnce.all().size(); ++n)
    {
        const auto& left = atOnce.all()[n];
        const auto& right = oneByOne.all()[n];
        INFO ("run " << left.id);
        CHECK (left.id == right.id);
        CHECK (left.parent == right.parent);
        CHECK (left.children == right.children);
    }

    /*  The act no longer names the children taken out; S2 keeps the parent's
        name, as a pruned parent's child does. Asked of the const table: a look
        that could change S2 would put it back where the tick reads it. */
    CHECK (std::as_const (atOnce).find ("ACT")->children == std::vector<std::string> { "A2" });
    CHECK (std::as_const (atOnce).find ("S2")->parent == "SCN");

    //  What had settled stays settled, and every question still answers as the walk.
    CHECK (currentOf (atOnce) == currentOf (oneByOne));
    CHECK (currentOf (atOnce) == std::vector<std::string> { "ACT", "A2", "HLD", "WT", "PRP", "K3" });
    answersAsTheWalk (std::as_const (atOnce), eveningParents, eveningCues, eveningSlots, 3);

    //  A name made again after it was taken out is found as the new run.
    atOnce.create ("K2", "CK", "media");
    CHECK (atOnce.find ("K2") == &atOnce.all().back());
    CHECK (nameOf (atOnce.newestOf ("CK")) == "K2");
}

TEST_CASE ("run: an evening of cues fired leaves the tick reading what is live and lately ended, and the tree as it was")
{
    /*  A memo fired every other tick for a thousand two hundred ticks, the
        Runner's hook run as the tick thread runs it: six hundred runs, each
        ended the tick after it fired. What the tick reads stays at what ended
        in the last five seconds; the table holds all six hundred; and the tree
        publishes exactly the runs the walk of every run would have. */
    Rig rig;

    tree::MountTable mounts;
    tree::ParameterTree parameters { rig.document, rig.engine.commands(), mounts, rig.runs };
    tree::EngineState state;

    std::size_t mostRead = 0;

    for (int n = 0; n < 600; ++n)
    {
        REQUIRE (rig.engine.submit ("cli", "cue.fire", { osc::Value::string (rig.memoId) }));

        for (int k = 0; k < 2; ++k)
        {
            rig.runner.beforeTick (rig.engine, rig.tick);
            rig.engine.processTick (rig.tick++);
            mostRead = std::max (mostRead, rig.runs.current().size());
        }
    }

    REQUIRE (rig.runs.all().size() == 600u);

    const auto published = [&rig, &parameters, &state]
    {
        state.tick = rig.tick - 1;
        const auto snapshot = parameters.publish (rig.tick - 1, state);
        REQUIRE (snapshot != nullptr);

        const auto* order = snapshot->find ("/godot/run/order");
        REQUIRE (order != nullptr);
        return order->soleValue().has_value() ? order->soleValue()->getString() : std::string();
    };

    const auto walked = [&rig]
    {
        std::string out;

        for (const auto& run : rig.runs.all())
            if (! (run.endedAtTick >= 0 && (rig.tick - 1) - run.endedAtTick > cue::retentionTicks))
                out += (out.empty() ? "" : " ") + run.id;

        return out;
    };

    for (const auto& run : rig.runs.all())
        REQUIRE (run.state == cue::runState::done);

    //  A run ended every other tick, read for 250 ticks after: some 126, never the six hundred.
    CHECK (mostRead <= 130u);
    CHECK (rig.runs.current().size() <= 127u);
    CHECK (published() == walked());

    //  Five quiet seconds and a tick more: nothing is read, nothing published, everything kept.
    for (int n = 0; n < 252; ++n)
    {
        rig.runner.beforeTick (rig.engine, rig.tick);
        rig.engine.processTick (rig.tick++);
    }

    CHECK (rig.runs.current().size() == 0u);
    CHECK (rig.runs.all().size() == 600u);
    CHECK (published().empty());
    CHECK (walked().empty());

    //  And the next one fired is read, and found, as the first was.
    REQUIRE (rig.engine.submit ("cli", "cue.fire", { osc::Value::string (rig.memoId) }));
    rig.runner.beforeTick (rig.engine, rig.tick);
    rig.engine.processTick (rig.tick++);

    REQUIRE (rig.runs.all().size() == 601u);
    CHECK (currentOf (rig.runs) == std::vector<std::string> { rig.runs.all().back().id });
    CHECK (rig.runs.liveRunOf (rig.memoId) == &rig.runs.all().back());
}

//==============================================================================
TEST_CASE ("run: the tree publishes every run, and republishes it as it changes")
{
    /*  Runs are on the tree's RUNTIME side, not its cached document side. A run
        changes several times a second while nothing about the show does, so
        published from the cached half it would have been frozen at whatever it
        read the last time somebody edited a cue. */
    Rig rig;

    tree::TouchTable touches;
    tree::MountTable mounts;
    tree::ParameterTree parameters { rig.document, rig.engine.commands(), mounts, rig.runs };

    const auto id = rig.arm (rig.mediaId);
    rig.run ("audio.armed", { osc::Value::string (id), osc::Value::int32 (2) });

    tree::EngineState state;
    const auto armed = parameters.publish (0, state);
    const auto base = "/godot/run/" + id;

    REQUIRE (armed != nullptr);

    const auto* stateNode = armed->find (base + "/state");
    REQUIRE (stateNode != nullptr);
    REQUIRE (stateNode->soleValue().has_value());
    CHECK (stateNode->soleValue()->getString() == "armed");

    const auto* trackNode = armed->find (base + "/track");
    REQUIRE (trackNode->soleValue().has_value());
    CHECK (trackNode->soleValue()->getInt32() == 2);

    CHECK (armed->find (base + "/cue")->soleValue()->getString() == rig.mediaId);
    CHECK (armed->find (base + "/kind")->soleValue()->getString() == "media");

    /*  Read-only, all of it. A run is what the machine is doing, and a client
        that could write to it would be telling the engine what it did. */
    CHECK (stateNode->access == tree::Access::read);

    rig.run ("run.started", { osc::Value::string (id) });

    const auto playing = parameters.publish (1, state);
    CHECK (playing->find (base + "/state")->soleValue()->getString() == "playing");

    /*  Nothing about the SHOW changed between those two publishes, which is the
        point: the document half was never rebuilt and the run moved anyway. */
    CHECK (playing->find ("/godot/cue/" + rig.mediaId + "/name") != nullptr);
}

TEST_CASE ("run: a finished run keeps its address, so what happened can still be read")
{
    Rig rig;

    tree::TouchTable touches;
    tree::MountTable mounts;
    tree::ParameterTree parameters { rig.document, rig.engine.commands(), mounts, rig.runs };

    const auto id = rig.arm (rig.mediaId);
    rig.run ("run.failed", { osc::Value::string (id),
                             osc::Value::string (cue::runError::mediaMissing) });

    tree::EngineState state;
    const auto snapshot = parameters.publish (0, state);
    const auto base = "/godot/run/" + id;

    CHECK (snapshot->find (base + "/state")->soleValue()->getString() == "failed");
    CHECK (snapshot->find (base + "/error")->soleValue()->getString() == "media-missing");
}

//==============================================================================
/*  THE PLAYHEAD, WHICH IS THE ONE READOUT THIS FILE CANNOT ASSERT WITHOUT A
    CLOCK.

    Everything above is a model a machine with no sound card reaches the same
    answers about, and keeping that true is why this file has no audio in it.
    `position` is the honest exception: it is where the playhead is IN THE FILE,
    which is an origin plus a count of samples, and the sample counter belongs
    to the Player. There is no version of the assertion that does without one.

    So these cases BOLT A CLOCK TO THE RIG ABOVE rather than replace it. The
    document, the commands, the identifiers and the run table are the same ones
    every case above uses; what is added is a counter the test moves by hand,
    which is also the only way to write down the number one expects and then
    check that the engine wrote it.
*/
namespace
{
    /*  The audio side as a metronome and nothing else: it accepts every arm and
        every launch, records the instant, and answers the two questions the
        position pass asks - what the counter says, and what rate it is counting
        at. It plays nothing, and `isPlaying` staying false is deliberate: a run
        that never reports sounding is never ended by `observeEdges`, so a case
        can watch one playhead for as long as it likes. */
    struct CountingPlayer final : cue::Player
    {
        int trackCount() const override              { return 4; }
        int slotCount() const override               { return 4; }
        int channelsPerTrack() const override        { return 2; }
        int blockSize() const override               { return block; }
        int sampleRate() const override              { return rate; }
        std::int64_t samplesElapsed() const override { return samples; }

        void requestArm (const cue::ArmRequest& request) override { arms.push_back (request); }

        bool launchAtSample (int, int slot, std::int64_t sample) override
        {
            launchedSlots.push_back (slot);
            launches.push_back (sample);
            return true;
        }

        bool stop (int) override                            { return true; }
        bool stopAtSample (int, int, std::int64_t) override { return true; }
        void setLevelDb (int, double) override              {}
        void setRouting (int, const std::vector<cue::Coefficient>&) override {}
        bool isPlaying (int) const override                 { return false; }
        bool isArmReady (int track) const override          { return ready.count (track) > 0; }

        /** The disk answers, which is what lets the launch be placed. */
        void completeArms (Engine& engine)
        {
            for (const auto& arm : arms)
            {
                engine.submit (origin::engine, "audio.armed",
                               { osc::Value::string (arm.runId),
                                 osc::Value::int32 (arm.track) });
                ready.insert (arm.track);
            }

            arms.clear();
        }

        /*  48 kHz and 128 samples a block, so that a second is a number a case
            can write down and a block is the tolerance every assertion here
            uses. */
        int rate = 48000;
        int block = 128;
        std::int64_t samples = 0;

        std::vector<cue::ArmRequest> arms;
        std::vector<std::int64_t> launches;
        std::vector<int> launchedSlots;
        std::set<int> ready;
    };

    //==========================================================================
    struct PlayheadRig
    {
        PlayheadRig()
        {
            model.runner.setPlayer (&audio);
            model.runner.setSamplesPerTick (samplesPerTick);

            /*  A name is all the arm needs when no bundle folder is set: the
                file is resolved against the bundle only when there is one, so a
                case here names a sound nobody has to have. */
            model.document.setAttribute ("/godot/cue/" + model.mediaId + "/file", "thunder.wav");
        }

        /*  ONE TICK OF A CLOCK THAT RUNS: the counter moves FIRST and the tick
            thread then works on the sample count it will have had. Moved
            afterwards it would leave every readout one tick - twenty
            milliseconds, seven blocks - behind the counter it was computed
            from, and a case that tolerated that could not tell a lag from a
            bug. */
        Engine::TickResult tickOnce()
        {
            audio.samples += samplesPerTick;
            model.runner.beforeTick (model.engine, model.tick);
            return model.engine.processTick (model.tick++);
        }

        void tickFor (int ticks)
        {
            for (int n = 0; n < ticks; ++n)
                tickOnce();
        }

        cue::Run* runOf (const std::string& cueId)
        {
            for (const auto& run : model.runs.all())
                if (run.cue == cueId)
                    return model.runs.find (run.id);

            return nullptr;
        }

        /** Arms a cue and stops there, which is a cue parked on a standby. */
        cue::Run* armOnly (const std::string& cueId)
        {
            REQUIRE (model.engine.submit ("cli", "audio.arm", { osc::Value::string (cueId) }));
            tickOnce();
            audio.completeArms (model.engine);
            tickOnce();

            return runOf (cueId);
        }

        /*  Fires it and ticks until the launch has been PLACED, bounded because
            what a launch that never comes looks like from here is a suite that
            hangs and says nothing. */
        cue::Run* fire (const std::string& cueId)
        {
            REQUIRE (model.engine.submit ("cli", "cue.fire", { osc::Value::string (cueId) }));
            tickOnce();
            audio.completeArms (model.engine);

            for (int n = 0; n < 40; ++n)
            {
                tickOnce();

                if (auto* run = runOf (cueId); run != nullptr && run->launchedAtSample > 0)
                    return run;
            }

            return nullptr;
        }

        /** Seconds, from the counter, between an instant and now. */
        double secondsSince (std::int64_t sample) const
        {
            return static_cast<double> (audio.samples - sample)
                     / static_cast<double> (audio.rate);
        }

        /** One audio block, in seconds, which is the tolerance throughout. */
        double oneBlock() const
        {
            return static_cast<double> (audio.block) / static_cast<double> (audio.rate);
        }

        static constexpr int samplesPerTick = 960;      // 48 kHz at 50 Hz

        Rig model;
        CountingPlayer audio;
    };
}

TEST_CASE ("run: the playhead is where in the FILE the sound is, and it advances with the counter")
{
    /*  §3.30's coloured bar, and the debt underneath it: `position` was
        declared, published and assigned by nothing at all, so what every client
        read for two phases was the literal nought. What it has to be is the
        start offset plus the time since the launch was placed - a place in the
        file, and not a stopwatch on the run. */
    PlayheadRig rig;

    REQUIRE (rig.model.document.setAttribute ("/godot/cue/" + rig.model.mediaId + "/startOffset",
                                              "2.5").ok);

    auto* run = rig.fire (rig.model.mediaId);
    REQUIRE (run != nullptr);

    /*  BEFORE THE INSTANT ITSELF. A launch is placed a few ticks into the
        future, and through that window the playhead sits at the offset the cue
        asked for rather than somewhere before the beginning of the file. */
    REQUIRE (rig.audio.samples < run->launchedAtSample);
    CHECK (std::fabs (run->position - 2.5) < 1.0e-9);

    const auto launched = run->launchedAtSample;

    rig.tickFor (100);                               // two seconds of clock

    REQUIRE (rig.audio.samples > launched);

    const auto expected = 2.5 + rig.secondsSince (launched);

    CHECK (std::fabs (run->position - expected) < rig.oneBlock());
    CHECK (run->position > 2.5);

    /*  AND IT KEEPS ADVANCING BY THE CLOCK'S OWN AMOUNT rather than by
        whatever the pass felt like: a second of clock is a second of file. */
    const auto before = run->position;

    rig.tickFor (50);

    CHECK (std::fabs ((run->position - before) - 1.0) < rig.oneBlock());
}

TEST_CASE ("run: a cue parked on a standby reads nought, and not the whole session")
{
    /*  THE GUARD THAT MAKES THIS A READOUT RATHER THAN A LIE.
        `launchedAtSample` is nought until the launch has been placed, so a run
        that is armed and waiting for its GO - which is every cue on a standby,
        for as long as the operator takes over the next line - would otherwise
        publish the session's whole elapsed time and be drawn as a sound that
        has been playing since the show opened. */
    PlayheadRig rig;

    auto* run = rig.armOnly (rig.model.mediaId);
    REQUIRE (run != nullptr);
    REQUIRE (run->launchedAtSample == 0);

    rig.tickFor (500);                               // ten seconds of session

    REQUIRE (rig.audio.samples > 10 * rig.audio.rate);
    CHECK (std::fabs (run->position) < 1.0e-9);

    /*  AND THE NUMBER A CLIENT ACTUALLY READS, through the tree, because that
        is the whole of what this debt was about: the address has existed since
        Phase 2 and what reaches it has to be the run's own. */
    tree::MountTable mounts;
    tree::ParameterTree parameters { rig.model.document, rig.model.engine.commands(),
                                     mounts, rig.model.runs };

    tree::EngineState state;
    const auto snapshot = parameters.publish (0, state);
    const auto* node = snapshot->find ("/godot/run/" + run->id + "/position");

    REQUIRE (node != nullptr);
    REQUIRE (node->soleValue().has_value());
    CHECK (std::fabs (node->soleValue()->asDouble()) < 1.0e-9);
}

TEST_CASE ("run: a run playing a range reads a position inside that range, and wraps with it")
{
    /*  §3.24: what a ranged cue plays is a region of its file, so the playhead
        a client draws is a place in the FILE and not a place in the range - the
        range's `in`, plus how far into the pass it has got. The in-point is the
        awkward half, because it is not on the run at all: it is re-read from
        the document at every boundary, so what this pass reads is the copy the
        scheduler left behind when it entered the range.

        AND IT WRAPS. Go.dot places nothing inside a range - the clip loops
        itself - so a looping range's playhead goes back to its in-point on
        every pass and never leaves the region somebody marked. */
    PlayheadRig rig;

    const auto range = rig.model.document.createRange (rig.model.mediaId, 10.0, 12.0);
    REQUIRE (range.ok);

    /*  Nought passes is FOR EVER (§3.24), which is what makes the wrap below
        the clip's own rather than a boundary the scheduler placed. */
    REQUIRE (rig.model.document.setAttribute ("/godot/range/" + range.id + "/loops", "0").ok);

    auto* run = rig.fire (rig.model.mediaId);
    REQUIRE (run != nullptr);

    rig.tickFor (5);                                 // past the launch instant
    REQUIRE (run->range == 0);

    const auto entered = run->rangeStartedAtSample;
    REQUIRE (entered > 0);

    /*  Into the first pass: a second of a two-second range that begins ten
        seconds into the file, which is a playhead at eleven and not at one. */
    rig.tickFor (50);

    CHECK (run->position > 10.0);
    CHECK (std::fabs (run->position - (10.0 + std::fmod (rig.secondsSince (entered), 2.0)))
             < rig.oneBlock());

    /*  AND STILL INSIDE IT, three passes later. This is the assertion a
        client's bar depends on: a position outside the marked region is a
        playhead drawn off the end of the waveform it was handed. */
    for (int n = 0; n < 300; ++n)
    {
        rig.tickOnce();

        /*  RE-READ RATHER THAN HELD, because the run table is a vector and a
            vector moves - the same reason every identifier in the engine is a
            string rather than a pointer. */
        auto* sounding = rig.runOf (rig.model.mediaId);

        REQUIRE (sounding != nullptr);
        REQUIRE_FALSE (sounding->isFinished());
        CHECK (sounding->position >= 10.0);
        CHECK (sounding->position < 12.0);
    }
}

//==============================================================================
TEST_CASE ("run: a cue nobody has shaped arms with every sound row at the table's default")
{
    /*  THE OWNER SPLIT'S TRIPWIRE (Phase 9b, namespace draft 18.2). The
        thirty-five rows a cue that sounds carries whatever its source - its
        level, routing, EQ, inserts and sends - moved from the `media` owner
        to `sound`, and the Reader keys a row's DEFAULT by owner and name: a
        read left asking `media` for a row that moved gets an empty string -
        a band at nought hertz, a filter at nought - with no error anywhere,
        because a stored value is found whatever owner is asked. A fresh cue
        stores none of those rows, so its arm reads every one of them from
        the defaults; this compares the arm with the table. */
    PlayheadRig rig;

    REQUIRE (rig.model.engine.submit ("cli", "audio.arm", { osc::Value::string (rig.model.mediaId) }));
    rig.tickOnce();
    REQUIRE (rig.audio.arms.size() == 1u);

    const auto& arm = rig.audio.arms.back();

    const auto defaultOf = [] (const std::string& name)
    {
        for (const auto* row : doc::Schema::rowsForOwner ("sound"))
            if (row->name == name)
                return std::string (row->defaultText);

        return std::string ("(no sound row called ") + name + ")";
    };

    const auto number = [&] (const std::string& name)
    {
        INFO (name << " defaults to " << defaultOf (name));
        const auto parsed = osc::parseDouble (defaultOf (name));
        REQUIRE (parsed.has_value());
        return static_cast<float> (*parsed);
    };

    const auto flag = [&] (const std::string& name) { return defaultOf (name) == "true"; };

    CHECK (arm.eq.on == flag ("eqOn"));
    CHECK (arm.eq.hpf == flag ("eqHpf"));
    CHECK (arm.eq.lpf == flag ("eqLpf"));
    CHECK (arm.eq.hpfFreq == doctest::Approx (number ("eqHpfFreq")));
    CHECK (arm.eq.lpfFreq == doctest::Approx (number ("eqLpfFreq")));

    for (int band = 0; band < audio::EqSettings::numBands; ++band)
    {
        const auto row = "eqB" + std::to_string (band + 1);
        INFO ("band " << band + 1);

        CHECK (arm.eq.band[band].on == flag (row + "On"));
        CHECK (arm.eq.band[band].freq == doctest::Approx (number (row + "Freq")));
        CHECK (arm.eq.band[band].gain == doctest::Approx (number (row + "Gain")));
        CHECK (arm.eq.band[band].q == doctest::Approx (number (row + "Q")));

        /*  What a missed default would read as, said outright: no band
            of a cue nobody shaped sits at nought hertz. */
        CHECK (arm.eq.band[band].freq > 0.0f);
    }

    CHECK ((arm.eq.band[0].shape == audio::EqSettings::Shape::lowShelf) == (defaultOf ("eqB1Shape") == "lowShelf"));
    CHECK ((arm.eq.band[3].shape == audio::EqSettings::Shape::highShelf) == (defaultOf ("eqB4Shape") == "highShelf"));
    CHECK (arm.levelDb == doctest::Approx (number ("level")));
}
