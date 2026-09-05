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
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/log/Replay.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/tree/Touches.h>

using namespace wfg;

namespace
{
    struct Rig
    {
        Rig()
        {
            engine.log().openInMemory ({});

            doc::registerDocumentCommands (engine.commands(), document);
            cue::registerRunCommands (engine.commands(), document, runs, runIds);

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
        doc::IdRegistry runIds { doc::IdRegistry::withSeed (7) };

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
