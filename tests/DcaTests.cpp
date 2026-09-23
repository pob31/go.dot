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

/*  DCAS AND THE ROWS A HAND RIDES (PRD §3.28, Phase 6).

    A run's level is one sum: its own, what a hand on its strip adds, every DCA
    marked on its cue and on the DCAs those sit inside, and the same terms for
    every group above it. These cases take the sum apart term by term, then ask
    the two things that make riding a fader different from editing a show: the
    write goes through `node.set` in front of a document that cannot hold it,
    and it leaves nothing on the undo stack.

    NO PLAYER, deliberately. A media run with no audio side is created, stays
    armed and is never ended by a sound - so its level is computed every tick
    and nothing else moves it, which is exactly what a case about arithmetic
    wants to hold still.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/client/model/Text.h>
#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/DcaTable.h>
#include <wfg/engine/cue/LiveRows.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace wfg;

namespace
{
    /*  AN AUDIO SIDE THAT ARMS AND NEVER GETS READY. The arm is what gives a
        media run the cue's level as its own, so it has to happen; the launch
        is not, so nothing is ever ready and every run stays armed - held still
        for the arithmetic, which is all these cases ask of it. */
    struct HeldPlayer final : cue::Player
    {
        int trackCount() const override                                  { return 8; }
        void requestArm (const cue::ArmRequest&) override                {}
        int slotCount() const override                                   { return 1; }
        bool launchAtSample (int, int, std::int64_t) override            { return true; }
        bool stop (int) override                                         { return true; }
        bool stopAtSample (int, int, std::int64_t) override              { return true; }
        void setLevelDb (int track, double levelDb) override             { levels[track] = levelDb; }
        void setRouting (int, const std::vector<cue::Coefficient>&) override {}
        bool isPlaying (int) const override                              { return false; }
        bool isArmReady (int) const override                             { return false; }
        std::int64_t samplesElapsed() const override                     { return 0; }
        int blockSize() const override                                   { return 128; }
        int sampleRate() const override                                  { return 48000; }
        int channelsPerTrack() const override                            { return 2; }

        /** What reached each voice last, which is the sum as the audio hears it. */
        std::map<int, double> levels;
    };

    struct Rig
    {
        Rig()
        {
            engine.log().openInMemory ({});

            doc::registerDocumentCommands (engine.commands(), document, {},
                                           cue::liveWriteFor (runs, dcas, document));
            cue::registerCueCommands (engine.commands(), document, focus);
            cue::registerRunCommands (engine.commands(), runs);
            cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);

            /*  serve's transaction hook, rides skipped, so a case can ask what
                a ride leaves on the stack. */
            engine.setBeforeApply ([this] (const Command& appliedCommand, const Event& submitted,
                                           const std::vector<osc::Value>& coerced,
                                           std::int64_t tickIndex)
                                   {
                                       if (cue::isLiveWrite (appliedCommand.name, coerced))
                                           return;

                                       document.beginTransaction (appliedCommand.name, tickIndex,
                                                                  submitted.origin, coerced);
                                   });

            runner.setDcas (&dcas);
            runner.setPlayer (&audio);
            runner.setSamplesPerTick (960);
            parameters.setDcas (&dcas);

            listId = document.createList ("Sound").id;
        }

        Engine::TickResult tickOnce()
        {
            runner.beforeTick (engine, tick);
            return engine.processTick (tick++);
        }

        Engine::TickResult submitAndTick (const std::string& name, std::vector<osc::Value> args = {})
        {
            REQUIRE (engine.submit ("cli", name, std::move (args)));
            return tickOnce();
        }

        template <typename Predicate>
        bool tickUntil (Predicate ready, int bound = 400)
        {
            for (int n = 0; n < bound; ++n)
            {
                if (ready())
                    return true;

                tickOnce();
            }

            return ready();
        }

        /** A command whose applied record must be accepted, checked. */
        void apply (const std::string& name, std::vector<osc::Value> args)
        {
            const auto result = submitAndTick (name, std::move (args));
            REQUIRE_MESSAGE (result.applied >= 1, name << " was refused: " << engine.lastError());
        }

        void set (const std::string& address, const std::string& text)
        {
            REQUIRE (document.setAttribute (address, text).ok);
        }

        std::string media (const std::string& parent, int index, const std::string& name, double level)
        {
            const auto id = document.createCue (parent, index, "media", name).id;
            set ("/godot/cue/" + id + "/file", name + ".wav");
            set ("/godot/cue/" + id + "/level", std::to_string (static_cast<int> (level)));
            return id;
        }

        std::string dca (const std::string& name)
        {
            return document.createDca (name).id;
        }

        const cue::Run* runOf (const std::string& cueId) const
        {
            const cue::Run* newest = nullptr;

            for (const auto& run : runs.all())
                if (run.cue == cueId)
                    newest = runs.find (run.id);

            return newest;
        }

        double levelOf (const std::string& cueId) const
        {
            const auto* run = runOf (cueId);
            REQUIRE_MESSAGE (run != nullptr, "no run of " << cueId);
            return run->level;
        }

        std::string published (const std::string& address)
        {
            parameters.markStale();
            snapshot = parameters.publish (tick, state);
            return client::model::text (*snapshot, address);
        }

        HeldPlayer audio;
        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::DcaTable dcas;
        doc::IdRegistry runIds { doc::IdRegistry::withSeed (11) };
        cue::Focus focus;
        cue::Runner runner { document, runs, runIds, focus };
        tree::MountTable mounts;
        tree::ParameterTree parameters { document, engine.commands(), mounts, runs };
        tree::EngineState state;
        std::shared_ptr<const tree::TreeSnapshot> snapshot;
        std::string listId;
        std::int64_t tick = 1;
    };

    bool near (double a, double b) { return std::abs (a - b) < 1.0e-9; }
}

//==============================================================================
TEST_CASE ("dca: a run plays at its own level plus every DCA above its cue")
{
    Rig rig;

    const auto everything = rig.dca ("Everything");
    const auto band = rig.dca ("Band");
    rig.set ("/godot/dca/" + band + "/dca", everything);           // Band inside Everything

    const auto guitar = rig.media (rig.listId, 0, "guitar", -3);
    rig.set ("/godot/cue/" + guitar + "/dca", band);

    rig.apply ("cue.fire", { osc::Value::string (guitar) });
    REQUIRE (rig.runOf (guitar) != nullptr);
    REQUIRE_FALSE (rig.runOf (guitar)->isFinished());

    rig.tickOnce();
    CHECK (near (rig.levelOf (guitar), -3.0));        // nothing trimming yet

    /*  THREE TERMS IN ONE SUM: the cue's -3, Band's -6, Everything's -2. The
        order they were set in is irrelevant, which is the property that
        matters - cues arrive in whatever order the operator pressed GO. */
    rig.apply ("node.set", { osc::Value::string ("/godot/dca/" + band + "/trim"),
                             osc::Value::float64 (-6.0) });
    rig.apply ("node.set", { osc::Value::string ("/godot/dca/" + everything + "/trim"),
                             osc::Value::float64 (-2.0) });
    rig.tickOnce();

    CHECK (near (rig.levelOf (guitar), -11.0));

    /*  And the DCA back to nought takes its term away, with nothing in the
        run having remembered it: a trim, not a write. */
    rig.apply ("node.set", { osc::Value::string ("/godot/dca/" + band + "/trim"),
                             osc::Value::string ("0") });
    rig.tickOnce();
    CHECK (near (rig.levelOf (guitar), -5.0));
}

TEST_CASE ("dca: a group marked with a DCA trims every member through the group")
{
    Rig rig;

    const auto ambiences = rig.dca ("Ambiences");
    const auto scene = rig.document.createCue (rig.listId, 0, "group", "The scene").id;
    rig.set ("/godot/cue/" + scene + "/mode", "timeline");
    rig.set ("/godot/cue/" + scene + "/dca", ambiences);

    const auto rain = rig.media (scene, 0, "rain", 0);
    const auto wind = rig.media (scene, 1, "wind", -4);

    rig.apply ("node.set", { osc::Value::string ("/godot/dca/" + ambiences + "/trim"),
                             osc::Value::float64 (-10.0) });
    rig.apply ("cue.fire", { osc::Value::string (scene) });

    REQUIRE (rig.tickUntil ([&rig, &rain, &wind]
                            {
                                return rig.runOf (rain) != nullptr && rig.runOf (wind) != nullptr;
                            }));
    rig.tickOnce();

    CHECK (near (rig.levelOf (rain), -10.0));
    CHECK (near (rig.levelOf (wind), -14.0));
}

TEST_CASE ("dca: one DCA trims two cues in two different groups")
{
    /*  The devplan's own clause for Phase 6: a DCA assigned to two cues in
        different groups trims both. */
    Rig rig;

    const auto band = rig.dca ("Band");

    const auto first = rig.document.createCue (rig.listId, 0, "group", "Act one").id;
    const auto second = rig.document.createCue (rig.listId, 1, "group", "Act two").id;
    rig.set ("/godot/cue/" + first + "/mode", "timeline");
    rig.set ("/godot/cue/" + second + "/mode", "timeline");

    const auto one = rig.media (first, 0, "one", -1);
    const auto two = rig.media (second, 0, "two", -2);
    rig.set ("/godot/cue/" + one + "/dca", band);
    rig.set ("/godot/cue/" + two + "/dca", band);

    rig.apply ("cue.fire", { osc::Value::string (first) });
    rig.apply ("cue.fire", { osc::Value::string (second) });
    REQUIRE (rig.tickUntil ([&rig, &one, &two]
                            {
                                return rig.runOf (one) != nullptr && rig.runOf (two) != nullptr;
                            }));

    rig.apply ("node.set", { osc::Value::string ("/godot/dca/" + band + "/trim"),
                             osc::Value::float64 (-20.0) });
    rig.tickOnce();

    CHECK (near (rig.levelOf (one), -21.0));
    CHECK (near (rig.levelOf (two), -22.0));
}

TEST_CASE ("dca: a hand's trim on a run is its own term, and a fade leaves it alone")
{
    Rig rig;

    const auto bed = rig.media (rig.listId, 0, "bed", -6);
    rig.apply ("cue.fire", { osc::Value::string (bed) });
    const auto runId = rig.runOf (bed)->id;

    rig.apply ("node.set", { osc::Value::string ("/godot/run/" + runId + "/trim"),
                             osc::Value::float64 (-12.0) });
    rig.tickOnce();

    CHECK (near (rig.runOf (bed)->trim, -12.0));
    CHECK (near (rig.levelOf (bed), -18.0));

    /*  A FADE MOVES `ownLevel` AND THE HAND MOVES `trim`: a fade to -20 over
        nothing lands the cue at -20 plus the hand's -12, and the hand's term is
        exactly what the hand left. */
    const auto fade = rig.document.createCue (rig.listId, 1, "fade", "Down").id;
    rig.set ("/godot/cue/" + fade + "/target", bed);
    rig.set ("/godot/cue/" + fade + "/level", "-20");

    rig.apply ("cue.fire", { osc::Value::string (fade) });
    rig.tickOnce();
    rig.tickOnce();

    CHECK (near (rig.runOf (bed)->trim, -12.0));
    CHECK (near (rig.levelOf (bed), -32.0));
}

TEST_CASE ("dca: the live door refuses what it cannot take, and ignores what has gone")
{
    Rig rig;

    const auto band = rig.dca ("Band");
    const auto address = "/godot/dca/" + band + "/trim";

    /*  OUT OF THE ROW'S RANGE: the answer the document gives for the same
        mistake on a level. */
    CHECK (rig.submitAndTick ("node.set", { osc::Value::string (address),
                                            osc::Value::float64 (50.0) }).rejected == 1);
    CHECK (rig.engine.lastError().find ("type-mismatch") != std::string::npos);
    CHECK (rig.submitAndTick ("node.set", { osc::Value::string (address),
                                            osc::Value::string ("loud") }).rejected == 1);

    /*  A DCA THE SHOW DOES NOT DECLARE: aimed at nothing on every write. */
    CHECK (rig.submitAndTick ("node.set", { osc::Value::string ("/godot/dca/ZZZZZZZ9/trim"),
                                            osc::Value::float64 (-1.0) }).rejected == 1);
    CHECK (rig.engine.lastError().find ("unknown-id") != std::string::npos);

    /*  A RUN THAT HAS GONE IS APPLIED AND IGNORED: a hand a tick behind a clip
        that just ended is not making a mistake. */
    CHECK (rig.submitAndTick ("node.set", { osc::Value::string ("/godot/run/ZZZZZZZ9/trim"),
                                            osc::Value::float64 (-1.0) }).applied == 1);

    /*  AND THE DOOR IS NO WIDER THAN ITS TWO ROWS: another runtime row of a
        list is still the document's to refuse. */
    CHECK (rig.submitAndTick ("node.set",
                              { osc::Value::string ("/godot/list/" + rig.listId + "/aim"),
                                osc::Value::string ("x") }).rejected == 1);
}

TEST_CASE ("dca: a ride leaves nothing on the undo stack")
{
    Rig rig;

    const auto band = rig.dca ("Band");
    rig.apply ("node.set", { osc::Value::string ("/godot/dca/" + band + "/name"),
                             osc::Value::string ("The band") });

    const auto& history = rig.document.history (doc::UndoDomain::document);
    const auto before = history.getUndoDescription();
    REQUIRE (history.canUndo());

    /*  A HUNDRED WRITES OF A FADER, from one origin inside the coalescing
        window: none of them is a decision about the show. */
    for (int i = 0; i < 100; ++i)
        rig.apply ("node.set", { osc::Value::string ("/godot/dca/" + band + "/trim"),
                                 osc::Value::float64 (-static_cast<double> (i) / 10.0) });

    CHECK (history.getUndoDescription() == before);

    /*  Undo takes back the RENAME - the last thing anybody decided - and moves
        no fader. */
    const auto trimBefore = rig.dcas.trimOf (band);
    REQUIRE (rig.document.undo (doc::UndoDomain::document).has_value());
    CHECK (rig.published ("/godot/dca/" + band + "/name") == "Band");
    CHECK (near (rig.dcas.trimOf (band), trimBefore));
}

TEST_CASE ("dca: a fade that names a DCA moves its trim, and a second takes over from the first")
{
    Rig rig;

    const auto band = rig.dca ("Band");

    const auto down = rig.document.createCue (rig.listId, 0, "fade", "Band down").id;
    rig.set ("/godot/cue/" + down + "/dca", band);
    rig.set ("/godot/cue/" + down + "/level", "-20");
    rig.set ("/godot/cue/" + down + "/duration", "1");

    rig.apply ("cue.fire", { osc::Value::string (down) });

    for (int i = 0; i < 25; ++i)
        rig.tickOnce();

    const auto halfway = rig.dcas.trimOf (band);
    CHECK (halfway < -5.0);
    CHECK (halfway > -15.0);

    /*  A SECOND FADE ON THE SAME DCA begins where the first had got to, and
        ends the first's run - the takeover a cue fade already does. */
    const auto up = rig.document.createCue (rig.listId, 1, "fade", "Band up").id;
    rig.set ("/godot/cue/" + up + "/dca", band);
    rig.set ("/godot/cue/" + up + "/level", "0");
    rig.set ("/godot/cue/" + up + "/duration", "0.5");

    rig.apply ("cue.fire", { osc::Value::string (up) });
    rig.tickOnce();

    /*  NO JUMP: one tick into the second fade the trim is within a step of
        where the first had taken it, and moving up. */
    const auto afterTakeover = rig.dcas.trimOf (band);
    CHECK (std::abs (afterTakeover - halfway) < 1.0);
    CHECK (afterTakeover < -2.0);

    REQUIRE (rig.tickUntil ([&rig, &down] { return rig.runOf (down)->isFinished(); }));
    REQUIRE (rig.tickUntil ([&rig, &up] { return rig.runOf (up)->isFinished(); }));

    CHECK (near (rig.dcas.trimOf (band), 0.0));
    CHECK (rig.runOf (up)->state == cue::runState::done);
}

TEST_CASE ("dca: a fade that names a DCA the show does not declare fails, and says so")
{
    Rig rig;

    const auto fade = rig.document.createCue (rig.listId, 0, "fade", "Nobody").id;

    // Written around the door: a dangling mark is a load warning, not a refusal.
    auto node = rig.document.findById (fade);
    node.setProperty ("dca", "ZZZZZZZ9", nullptr);

    rig.apply ("cue.fire", { osc::Value::string (fade) });
    REQUIRE (rig.tickUntil ([&rig, &fade] { return rig.runOf (fade)->isFinished(); }));

    CHECK (rig.runOf (fade)->state == cue::runState::failed);
    CHECK (rig.runOf (fade)->error == cue::runError::badTarget);
}

TEST_CASE ("dca: the trims are published beside what the show declares")
{
    Rig rig;

    const auto band = rig.dca ("Band");
    const auto bed = rig.media (rig.listId, 0, "bed", 0);

    CHECK (rig.published ("/godot/dca/" + band + "/trim") == "0");
    CHECK (rig.published ("/godot/dca/" + band + "/name") == "Band");

    rig.apply ("node.set", { osc::Value::string ("/godot/dca/" + band + "/trim"),
                             osc::Value::float64 (-4.5) });
    CHECK (rig.published ("/godot/dca/" + band + "/trim") == "-4.5");

    rig.apply ("cue.fire", { osc::Value::string (bed) });
    const auto runId = rig.runOf (bed)->id;
    rig.apply ("node.set", { osc::Value::string ("/godot/run/" + runId + "/trim"),
                             osc::Value::float64 (-7.0) });

    CHECK (rig.published ("/godot/run/" + runId + "/trim") == "-7");
}
