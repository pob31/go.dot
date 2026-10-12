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

/*
    A time group's run (namespace draft §60, TG.2): an IDLE group. GO arms it -
    a group run in its members phase, as a sampler bank is armed - and nothing
    fires yet, since nothing reads a code until TG.3-TG.7; it stays armed until
    it is stopped, and Esc, a double Esc, a stop cue and its footer end it as
    they end any group (AIX). The standby steps past it at the GO and never into
    it, a second GO adopts the armed run, a member fired by name or by a
    trigger is refused while the group itself fires, Doh! takes back the GO
    that armed it so the next GO arms it from its top, and a Persistent section
    keeps one armed all show (AIW). A replay reaches the same runs.

    What made each of these red before TG.2: a time group read by the old
    compares was a manual sequence - `advance` reads `manual` by default - so
    GO descended into it and fired its first member, `cue.fire` of the group was
    refused `needs-go`, and its members phase spawned and waited as a sequence.

    No audio side: the engine, the document, the run table and the Runner's
    hook, wired as `serve --no-audio` wires them. A member that started would
    show as a run of its cue; none may.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/command/Command.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/cue/GroupJob.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/log/Replay.h>
#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace wfg;

namespace
{
    /*  A list of three rows - a memo, a time group, a memo - and the group's
        members: a sound, a picture and a timeline scene holding a sound of its
        own, each with a time on the wall clock; and a footer with a memo in
        it, which tells a graceful end from an immediate one. */
    struct TimeRig
    {
        TimeRig()
        {
            engine.log().openInMemory ({});
            doc::registerDocumentCommands (engine.commands(), document);
            cue::registerCueCommands (engine.commands(), document, focus);
            cue::registerRunCommands (engine.commands(), runs);
            cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);

            listId = document.createList ("Show").id;
            set ("/godot/list/goDebounce", "0");

            before = make (listId, 0, "memo", "Before");
            group = make (listId, 1, "group", "Clock", { { "mode", "time" }, { "timeSource", "clock" } });
            after = make (listId, 2, "memo", "After");

            sound = make (group, 0, "media", "Rain", { { "at", "20:00:00" } });
            picture = make (group, 1, "video", "Moon", { { "at", "20:01:00" } });
            scene = make (group, 2, "group", "Storm", { { "mode", "timeline" }, { "at", "20:02:00" } });
            inScene = make (scene, 0, "media", "Thunder");

            const auto footer = document.createRole (group, "footer");
            REQUIRE (footer.ok);
            release = make (footer.id, 0, "memo", "Lights down");
        }

        std::string make (const std::string& parent, int index, const std::string& kind, const std::string& name,
                          const doc::ShowDocument::Attributes& born = {})
        {
            const auto made = document.createCue (parent, index, kind, name, {}, born);
            REQUIRE_MESSAGE (made.ok, kind << " " << name << ": " << made.reason);
            return made.id;
        }

        void set (const std::string& address, const std::string& text)
        {
            REQUIRE_MESSAGE (document.setAttribute (address, text).ok, address << " = " << text);
        }

        Engine::TickResult tickOnce()
        {
            runner.beforeTick (engine, tick);
            return engine.processTick (tick++);
        }

        Engine::TickResult send (const std::string& name, std::vector<osc::Value> args = {})
        {
            REQUIRE (engine.submit ("cli", name, std::move (args)));
            return tickOnce();
        }

        void ticks (int count)
        {
            for (int n = 0; n < count; ++n)
                tickOnce();
        }

        bool tickUntil (const std::function<bool()>& done, int bound = 200)
        {
            for (int n = 0; n < bound; ++n)
            {
                if (done())
                    return true;

                tickOnce();
            }

            return done();
        }

        std::string standby() const
        {
            return document.getAttribute (cue::standbyAddressOf (listId)).value_or (std::string {});
        }

        /*  GO with the standby on `cueId`, and the hook's tick after it. */
        Engine::TickResult goOn (const std::string& cueId)
        {
            set (cue::standbyAddressOf (listId), cueId);
            tickOnce();
            const auto result = send ("go");
            tickOnce();
            return result;
        }

        const cue::Run* liveRunOf (const std::string& cueId) const { return runs.liveRunOf (cueId); }

        std::size_t runsOf (const std::string& cueId) const
        {
            return static_cast<std::size_t> (std::count_if (runs.all().begin(), runs.all().end(),
                                                            [&cueId] (const cue::Run& run) { return run.cue == cueId; }));
        }

        /*  No member, and nothing inside a member, has a run: the code starts
            them, and nothing reads a code yet. */
        bool noMemberRan() const
        {
            return runsOf (sound) == 0 && runsOf (picture) == 0 && runsOf (scene) == 0 && runsOf (inScene) == 0;
        }

        /*  Armed: a live group run, playing, in its members phase. */
        bool armed() const
        {
            const auto* run = liveRunOf (group);
            return run != nullptr && run->state == cue::runState::playing
                     && run->phase == cue::groupPhase::members;
        }

        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::Focus focus;
        doc::IdRegistry runIds = doc::IdRegistry::withSeed (60);
        cue::Runner runner { document, runs, runIds, focus };

        std::string listId, before, group, after, sound, picture, scene, inScene, release;
        std::int64_t tick = 1;
    };
}

//==============================================================================
TEST_CASE ("time group: GO arms it - a run in its members, no member run - and the standby steps past it")
{
    TimeRig rig;

    REQUIRE (rig.goOn (rig.group).rejected == 0);

    REQUIRE (rig.tickUntil ([&rig] { return rig.armed(); }, 10));
    CHECK (rig.noMemberRan());

    //  Past it, never into it: the next row of the list.
    CHECK (rig.standby() == rig.after);

    //  Still armed after thirty seconds of ticks, with nothing fired and nothing ended.
    const auto armedAs = rig.liveRunOf (rig.group)->id;
    rig.ticks (1500);

    REQUIRE (rig.armed());
    CHECK (rig.liveRunOf (rig.group)->id == armedAs);
    CHECK (rig.noMemberRan());
    CHECK (rig.runsOf (rig.release) == 0);
    CHECK (rig.runsOf (rig.group) == 1u);
}

TEST_CASE ("time group: a second GO adopts the armed run, and fired by name or a trigger it arms - never needs-go")
{
    TimeRig rig;

    SUBCASE ("a second GO on it")
    {
        REQUIRE (rig.goOn (rig.group).rejected == 0);
        REQUIRE (rig.tickUntil ([&rig] { return rig.armed(); }, 10));
        const auto first = rig.liveRunOf (rig.group)->id;

        REQUIRE (rig.goOn (rig.group).rejected == 0);
        rig.ticks (10);

        REQUIRE (rig.armed());
        CHECK (rig.liveRunOf (rig.group)->id == first);
        CHECK (rig.runsOf (rig.group) == 1u);
        CHECK (rig.standby() == rig.after);
        CHECK (rig.noMemberRan());
    }

    SUBCASE ("fired by name, its advance manual by default")
    {
        CHECK (rig.document.getAttribute ("/godot/cue/" + rig.group + "/advance") == std::optional<std::string> ("manual"));

        CHECK (rig.send ("cue.fire", { osc::Value::string (rig.group) }).rejected == 0);
        REQUIRE (rig.tickUntil ([&rig] { return rig.armed(); }, 10));
        CHECK (rig.noMemberRan());

        //  Fired again by name: the same run.
        CHECK (rig.send ("cue.fire", { osc::Value::string (rig.group) }).rejected == 0);
        rig.ticks (5);
        CHECK (rig.runsOf (rig.group) == 1u);
    }

    SUBCASE ("by a trigger on it")
    {
        const auto trigger = rig.document.createTrigger (rig.group, "osc");
        REQUIRE (trigger.ok);
        rig.set ("/godot/trigger/" + trigger.id + "/address", "/show/clock");

        const auto pointer = rig.standby();

        CHECK (rig.send ("trigger.fire", { osc::Value::string (trigger.id) }).rejected == 0);
        REQUIRE (rig.tickUntil ([&rig] { return rig.armed(); }, 10));
        CHECK (rig.noMemberRan());

        //  And the standby has not moved: a trigger never moves it.
        CHECK (rig.standby() == pointer);
    }
}

TEST_CASE ("time group: a member fired by name or by a trigger is refused needs-go, armed or not")
{
    TimeRig rig;

    const auto trigger = rig.document.createTrigger (rig.sound, "osc");
    REQUIRE (trigger.ok);
    rig.set ("/godot/trigger/" + trigger.id + "/address", "/rain");

    const auto refusedBoth = [&rig, &trigger]
    {
        for (const auto& member : { rig.sound, rig.picture, rig.scene })
        {
            INFO ("member " << member);
            CHECK (rig.send ("cue.fire", { osc::Value::string (member) }).rejected == 1);
            CHECK (rig.engine.lastError().find (reason::needsGo) != std::string::npos);
        }

        CHECK (rig.send ("trigger.fire", { osc::Value::string (trigger.id) }).rejected == 1);
        CHECK (rig.engine.lastError().find (reason::needsGo) != std::string::npos);
        CHECK (rig.noMemberRan());
    };

    refusedBoth();

    REQUIRE (rig.goOn (rig.group).rejected == 0);
    REQUIRE (rig.tickUntil ([&rig] { return rig.armed(); }, 10));

    refusedBoth();
    CHECK (rig.armed());

    //  A cue inside a member scene is not the time group's member: fired by name, it plays as anywhere.
    CHECK (rig.send ("cue.fire", { osc::Value::string (rig.inScene) }).rejected == 0);
    CHECK (rig.runsOf (rig.inScene) == 1u);
}

TEST_CASE ("time group: Esc ends it and its footer runs; a double Esc ends it with no footer; a stop cue ends it")
{
    TimeRig rig;
    REQUIRE (rig.goOn (rig.group).rejected == 0);
    REQUIRE (rig.tickUntil ([&rig] { return rig.armed(); }, 10));
    const auto armedAs = rig.liveRunOf (rig.group)->id;

    SUBCASE ("Esc: through its footer")
    {
        REQUIRE (rig.send ("run.stopAll").rejected == 0);
        REQUIRE (rig.tickUntil ([&rig, &armedAs] { return rig.runs.find (armedAs)->isFinished(); }, 200));
        CHECK (rig.runs.find (armedAs)->state == cue::runState::done);
        CHECK (rig.runsOf (rig.release) == 1u);
    }

    SUBCASE ("a double Esc: at once, no footer")
    {
        REQUIRE (rig.send ("run.killAll").rejected == 0);
        REQUIRE (rig.tickUntil ([&rig, &armedAs] { return rig.runs.find (armedAs)->isFinished(); }, 10));
        rig.ticks (20);
        CHECK (rig.runsOf (rig.release) == 0);
    }

    SUBCASE ("a stop cue aimed at it: graceful, as Esc")
    {
        const auto stop = rig.make (rig.listId, 3, "transport", "Stop the clock", { { "target", rig.group } });

        REQUIRE (rig.send ("cue.fire", { osc::Value::string (stop) }).rejected == 0);
        REQUIRE (rig.tickUntil ([&rig, &armedAs] { return rig.runs.find (armedAs)->isFinished(); }, 200));
        CHECK (rig.runsOf (rig.release) == 1u);
    }

    CHECK (rig.liveRunOf (rig.group) == nullptr);
    CHECK (rig.noMemberRan());
}

TEST_CASE ("time group: Doh! takes back the GO that armed it, and the next GO arms it from its top")
{
    TimeRig rig;
    REQUIRE (rig.goOn (rig.group).rejected == 0);
    REQUIRE (rig.tickUntil ([&rig] { return rig.armed(); }, 10));
    const auto armedAs = rig.liveRunOf (rig.group)->id;
    REQUIRE (rig.standby() == rig.after);

    REQUIRE (rig.send ("go.doh").rejected == 0);

    //  No longer armed, and the pointer back on it.
    CHECK (rig.tickUntil ([&rig] { return rig.liveRunOf (rig.group) == nullptr; }, 200));
    CHECK (rig.standby() == rig.group);
    CHECK (rig.noMemberRan());

    //  The corrected GO arms it afresh.
    REQUIRE (rig.send ("go").rejected == 0);
    REQUIRE (rig.tickUntil ([&rig] { return rig.armed(); }, 10));
    CHECK (rig.standby() == rig.after);
    CHECK (rig.noMemberRan());

    /*  Armed by the corrected GO and by nothing else: one live run, not the
        one taken back - which nobody heard, so it may be the same run handed
        back and adopted again (§24.12), and is never a second beside it. */
    const auto* now = rig.liveRunOf (rig.group);
    REQUIRE (now != nullptr);
    CHECK_FALSE (now->takenBack);

    const auto live = std::count_if (rig.runs.all().begin(), rig.runs.all().end(),
                                     [&rig] (const cue::Run& run) { return run.cue == rig.group && ! run.isFinished(); });
    CHECK (live == 1);

    if (now->id != armedAs)
        CHECK (rig.runs.find (armedAs)->isFinished());
}

TEST_CASE ("time group: a Persistent section arms it at the first step and again after Esc, the next step")
{
    TimeRig rig;

    const auto section = rig.document.createPersistent (rig.listId);
    REQUIRE (section.ok);
    const auto kept = rig.make (section.id, 0, "group", "Always", { { "mode", "time" }, { "timeSource", "clock" } });
    const auto member = rig.make (kept, 0, "media", "Chime", { { "at", "12:00:00" } });

    //  Nothing before a step: the section asserts after an applied trigger, never on its own clock.
    rig.ticks (20);
    CHECK (rig.liveRunOf (kept) == nullptr);

    REQUIRE (rig.goOn (rig.before).rejected == 0);
    REQUIRE (rig.tickUntil ([&rig, &kept] { return rig.liveRunOf (kept) != nullptr; }, 60));

    const auto* armed = rig.liveRunOf (kept);
    CHECK (armed->asserted);
    REQUIRE (rig.tickUntil ([&rig, &kept]
                            {
                                const auto* run = rig.liveRunOf (kept);
                                return run != nullptr && run->phase == cue::groupPhase::members;
                            }, 10));
    CHECK (rig.runsOf (member) == 0);
    const auto first = armed->id;

    //  Another step leaves it alone: it is armed already.
    REQUIRE (rig.goOn (rig.after).rejected == 0);
    rig.ticks (30);
    CHECK (rig.liveRunOf (kept)->id == first);
    CHECK (rig.runsOf (kept) == 1u);

    //  Esc is a stop for it, not a pause: down, and not back by itself...
    REQUIRE (rig.send ("run.stopAll").rejected == 0);
    REQUIRE (rig.tickUntil ([&rig, &first] { return rig.runs.find (first)->isFinished(); }, 200));
    rig.ticks (60);
    CHECK (rig.liveRunOf (kept) == nullptr);

    //  ...and armed again at the next step.
    REQUIRE (rig.goOn (rig.before).rejected == 0);
    REQUIRE (rig.tickUntil ([&rig, &kept] { return rig.liveRunOf (kept) != nullptr; }, 60));
    CHECK (rig.liveRunOf (kept)->id != first);
    CHECK (rig.liveRunOf (kept)->asserted);
    CHECK (rig.runsOf (member) == 0);

    //  And a time group outside the section is still the list's: the step did not arm it.
    CHECK (rig.liveRunOf (rig.group) == nullptr);
}

TEST_CASE ("time group: a replay of the session reaches the same runs, ended on the same ticks")
{
    TimeRig rig;

    REQUIRE (rig.goOn (rig.group).rejected == 0);
    rig.ticks (20);
    rig.send ("cue.fire", { osc::Value::string (rig.sound) });           // refused, and logged
    REQUIRE (rig.goOn (rig.group).rejected == 0);                       // adopted
    rig.ticks (20);
    REQUIRE (rig.send ("run.stopAll").rejected == 0);
    REQUIRE (rig.tickUntil ([&rig] { return rig.liveRunOf (rig.group) == nullptr; }, 200));
    REQUIRE (rig.send ("cue.fire", { osc::Value::string (rig.group) }).rejected == 0);
    rig.ticks (20);
    REQUIRE (rig.send ("run.killAll").rejected == 0);
    rig.ticks (20);

    REQUIRE (rig.runsOf (rig.group) == 2u);
    CHECK (rig.runsOf (rig.release) == 1u);
    CHECK (rig.noMemberRan());

    const auto show = doc::CanonicalXml::write (rig.document);
    const auto original = LogFile::parse (rig.engine.log().contents());
    REQUIRE (original.errors.empty());

    TimeRig fresh;
    REQUIRE (doc::CanonicalXml::read (show, fresh.document).ok);

    const auto result = replay (fresh.engine, original);

    for (const auto& mismatch : result.mismatches)
        MESSAGE (mismatch);

    CHECK (result.ok);

    for (const auto& run : rig.runs.all())
    {
        INFO ("run " << run.id << " of " << run.cue);
        const auto* again = fresh.runs.find (run.id);
        REQUIRE (again != nullptr);
        CHECK (again->state == run.state);
        CHECK (again->kind == run.kind);
        CHECK (again->endedAtTick == run.endedAtTick);
    }

    CHECK (fresh.runs.all().size() == rig.runs.all().size());
}
