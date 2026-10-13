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

    No audio side for those: the engine, the document, the run table and the
    Runner's hook, wired as `serve --no-audio` wires them. A member that started
    would show as a run of its cue; none may - except one fired by hand while
    its group is armed, which TG.7 starts (AJB).

    AND THE GROUP FIRING ON ITS CODE (TG.7, the second half of this file):
    `code.heard` records submitted as serve's watchers submit them - no decoder
    - and an audio side that records the sample every launch is placed on, a
    picture side that records where each picture comes up. A member is loaded
    two seconds before the code reaches its time and launched on exactly the
    sample the code's map gives for it, at 25 and across 29.97 drop-frame's
    dropped minute, from MTC and from the wall clock; a stopped code fires
    nothing until it comes back, a jump past a member never fires it, a jump
    back fires it again and stops the first; a group armed after a member's time
    leaves it; a member loaded too close to its time starts late and says so; a
    scene's members are each launched once, on the code's sample plus their
    offset; a picture comes up on its sample, read ahead while it waits; a
    Persistent section's group fires all show; and a replay reaches the same
    runs and maps.

    AND THE MEMBERS THAT CHASE (TG.8, the third part): a member whose `chase`
    is `jump`, put out of step by a re-anchored code, jumps once over the
    crossfade to the code's second - a move on its slot's reader the fake
    records - and not at all when it is inside the tolerance, nor when it is a
    free runner; a locate seats a chaser at its offset, the runs handed back on
    the record, ends one whose time is ahead, jumps one still in range, cuts a
    free runner asked for on the old line and leaves one playing; a code that
    stops leaves its chasers playing and its return jumps them; a movie cuts and
    its locked sound jumps with it; a sound whose code is in another slice is
    sought there, and says so; a scene is re-seated around the second; a group
    armed while its code runs seats a chaser past its time by `code.seat`; and a
    replay seats the same.
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
#include <wfg/engine/timecode/CodeMap.h>
#include <wfg/engine/timecode/Timecode.h>
#include <wfg/engine/video/VideoSink.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
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

        /*  GO with the standby on `cueId`, and the hook's tick after it. The
            standby is put there by a record, as a client puts it, so a replay of
            the log steps from the same place. */
        Engine::TickResult goOn (const std::string& cueId)
        {
            REQUIRE (engine.submit ("cli", "node.set", { osc::Value::string (cue::standbyAddressOf (listId)),
                                                         osc::Value::string (cueId) }));
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

TEST_CASE ("time group: a member fired by name or by a trigger is refused needs-go while its group is not armed, and started from its top while it is")
{
    /*  TG.2 refused both, armed or not; TG.7 (AJB, the implementer's call):
        while the group is armed, a member fired by name or by a trigger is an
        operator's manual start - a run of its own under the armed group, begun
        now, from its top - and the group is still armed after it. */
    TimeRig rig;

    const auto trigger = rig.document.createTrigger (rig.sound, "osc");
    REQUIRE (trigger.ok);
    rig.set ("/godot/trigger/" + trigger.id + "/address", "/rain");

    for (const auto& member : { rig.sound, rig.picture, rig.scene })
    {
        INFO ("member " << member);
        CHECK (rig.send ("cue.fire", { osc::Value::string (member) }).rejected == 1);
        CHECK (rig.engine.lastError().find (reason::needsGo) != std::string::npos);
    }

    CHECK (rig.send ("trigger.fire", { osc::Value::string (trigger.id) }).rejected == 1);
    CHECK (rig.engine.lastError().find (reason::needsGo) != std::string::npos);
    CHECK (rig.noMemberRan());

    REQUIRE (rig.goOn (rig.group).rejected == 0);
    REQUIRE (rig.tickUntil ([&rig] { return rig.armed(); }, 10));
    const auto armedAs = rig.liveRunOf (rig.group)->id;

    //  Armed: each started at once, under the armed group.
    for (const auto& member : { rig.sound, rig.picture, rig.scene })
    {
        INFO ("member " << member);
        CHECK (rig.send ("cue.fire", { osc::Value::string (member) }).rejected == 0);
        REQUIRE (rig.runsOf (member) == 1u);

        const auto* made = rig.runs.liveRunOf (member);
        REQUIRE (made != nullptr);
        CHECK (made->parent == armedAs);
        CHECK (made->launchRequestedAtTick != 0);
    }

    //  The scene plays its own member as written.
    CHECK (rig.tickUntil ([&rig] { return rig.runsOf (rig.inScene) == 1u; }, 20));

    //  By its trigger: a second run of the sound, its own.
    CHECK (rig.send ("trigger.fire", { osc::Value::string (trigger.id) }).rejected == 0);
    CHECK (rig.runsOf (rig.sound) == 2u);

    //  And the group is still armed, and the standby where the GO left it.
    rig.ticks (10);
    CHECK (rig.armed());
    CHECK (rig.liveRunOf (rig.group)->id == armedAs);
    CHECK (rig.standby() == rig.after);
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
    rig.send ("cue.fire", { osc::Value::string (rig.picture) });         // a member by hand, armed: started (TG.7)
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
    CHECK (rig.runsOf (rig.picture) == 1u);
    CHECK (rig.runsOf (rig.sound) == 0u);

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
        CHECK (again->cue == run.cue);
        CHECK (again->state == run.state);
        CHECK (again->kind == run.kind);
        CHECK (again->endedAtTick == run.endedAtTick);
    }

    CHECK (fresh.runs.all().size() == rig.runs.all().size());
}

//==============================================================================
/*  TG.7: THE GROUP FIRING ON ITS CODE (namespace draft §60; AIH, AIQ, AIR, AIV,
    AIW, AJB). The code is driven by submitting `code.heard` as serve's watchers
    submit it, from the source's own origin - no decoder - on an audio side that
    records every launch's sample and a picture side that records where each
    picture comes up. 48 kHz, 960 samples a tick, 128-sample blocks: a launch
    horizon of two ticks.
*/
namespace
{
    constexpr std::int64_t perTick = 960;
    constexpr std::int64_t preloadSamples = 2 * 48000;

    osc::Value word (std::string value) { return osc::Value::string (std::move (value)); }

    std::int64_t frameNamed (const char* label, timecode::Rate rate)
    {
        const auto read = timecode::parseLabel (label);
        REQUIRE (read.has_value());
        return timecode::frameOf (*read, rate);
    }

    /*  AN AUDIO SIDE THAT SAYS WHERE EACH LAUNCH WENT: every launch placed, by
        track and sample; every arm asked, with the offset it starts its file
        at; an arm ready when `completeArms` says the disk answered - at once,
        unless `slowDisk` holds it - and a voice launched playing until it is
        stopped. */
    struct TimePlayer final : cue::Player
    {
        //  A MOVE ON A SLOT'S READER (TG.8's jump), each numbered as the voice numbers them.
        struct Loop
        {
            int track = -1;
            int slot = -1;
            cue::Player::LoopMove move;
        };

        std::uint64_t placeLoop (int track, int slot, const LoopMove& move) override
        {
            loops.push_back ({ track, slot, move });
            return loops.size();
        }

        void endsOutside (int track, bool outside) override
        {
            if (outside)
                endedOutside.insert (track);
            else
                endedOutside.erase (track);
        }

        /*  A READER THAT MET MOVE `adoptMove` LATE, `adoptLateBy` seconds past
            where it was placed, the file there where the move put it: what a
            stretcher reading further ahead than the horizon reports. */
        std::optional<LoopTaken> loopTaken (int track, int slot) override
        {
            if (adoptMove == 0 || adoptMove > loops.size())
                return std::nullopt;

            const auto& loop = loops[adoptMove - 1];

            if (loop.track != track || loop.slot != slot)
                return std::nullopt;

            return LoopTaken { adoptMove, loop.move.from + adoptLateBy, loop.move.fileAt };
        }

        std::vector<Loop> loops;
        std::set<int> endedOutside;
        std::uint64_t adoptMove = 0;
        double adoptLateBy = 0.0;

        int trackCount() const override                 { return 16; }
        int slotCount() const override                  { return 4; }
        int sampleRate() const override                 { return 48000; }
        int blockSize() const override                  { return 128; }
        int channelsPerTrack() const override           { return 2; }
        std::int64_t samplesElapsed() const override    { return samples; }

        void requestArm (const cue::ArmRequest& request) override
        {
            arms.push_back (request);
            asked.push_back (request);
        }

        bool launchAtSample (int track, int, std::int64_t sample) override
        {
            launches.emplace_back (track, sample);
            playing.insert (track);
            return true;
        }

        bool stop (int track) override
        {
            playing.erase (track);
            stops.push_back (track);
            return true;
        }

        bool stopAtSample (int track, int, std::int64_t) override
        {
            playing.erase (track);
            return true;
        }

        void setLevelDb (int, double) override {}
        void setRouting (int, const std::vector<cue::Coefficient>&) override {}
        bool isPlaying (int track) const override       { return playing.count (track) > 0; }
        bool isArmReady (int track) const override      { return ready.count (track) > 0; }

        void completeArms (Engine& engine)
        {
            if (slowDisk)
                return;

            for (const auto& arm : arms)
            {
                engine.submit (origin::engine, "audio.armed", { word (arm.runId), osc::Value::int32 (arm.track) });
                ready.insert (arm.track);
            }

            arms.clear();
        }

        std::int64_t samples = 0;
        bool slowDisk = false;
        std::vector<cue::ArmRequest> arms, asked;
        std::set<int> ready, playing;
        std::vector<int> stops;
        std::vector<std::pair<int, std::int64_t>> launches;
    };

    /*  A PICTURE SIDE THAT SAYS WHERE EACH PICTURE CAME UP - its first opacity
        point's sample - and what it was last told to read ahead. */
    struct PictureSink final : video::Sink
    {
        void show (const video::LayerSpec& spec) override { shown.push_back (spec.id); }

        void move (const std::string& id, video::Property property, const video::Point& point) override
        {
            if (property == video::Property::opacity)
                firstPoint.emplace (id, point.sample);

            if (property == video::Property::time)
                times[id].push_back (point);
        }

        void remove (const std::string&, std::int64_t) override {}
        void clear() override {}

        void prepare (const std::vector<video::Preload>& files) override
        {
            readAhead.clear();

            for (const auto& file : files)
                readAhead.push_back (file.path);
        }

        bool readsAhead (const std::string& name) const
        {
            return std::any_of (readAhead.begin(), readAhead.end(),
                                [&name] (const std::string& path) { return path.find (name) != std::string::npos; });
        }

        std::vector<std::string> shown;
        std::map<std::string, std::int64_t> firstPoint;
        std::map<std::string, std::vector<video::Point>> times;
        std::vector<std::string> readAhead;
    };

    enum class Source { ltc, mtc, clock };

    /*  A list - a memo, a time group on `source`, a memo - with the code's input
        named, an audio side and a picture side, the lengths of the sounds the
        test gives, and nothing in the group: each case adds its members. */
    struct CodeRig
    {
        explicit CodeRig (Source kind = Source::ltc, const char* frameRate = "25")
        {
            engine.log().openInMemory ({});
            doc::registerDocumentCommands (engine.commands(), document);
            cue::registerCueCommands (engine.commands(), document, focus);
            cue::registerRunCommands (engine.commands(), runs);
            cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);

            runner.setPlayer (&audio);
            runner.setVideo (&pictures);
            runner.setSamplesPerTick (static_cast<int> (perTick));
            runner.setMediaDurations (&lengths);

            listId = document.createList ("Show").id;
            set ("/godot/list/goDebounce", "0");
            set ("/godot/sync/frameRate", frameRate);

            std::string sourceWord = "ltc";

            if (kind == Source::ltc)
            {
                const auto input = document.createInput (1);
                REQUIRE (input.ok);
                set ("/godot/sync/ltcIn", input.id);
                source = "ltc:" + input.id;
            }
            else if (kind == Source::mtc)
            {
                const auto port = document.createPort ("Time code");
                REQUIRE (port.ok);
                set ("/godot/sync/mtcIn", port.id);
                source = "mtc:" + port.id;
                sourceWord = "mtc";
            }
            else
            {
                source = "clock";
                sourceWord = "clock";
            }

            before = make (listId, 0, "memo", "Before");
            group = make (listId, 1, "group", "On the code", { { "mode", "time" }, { "timeSource", sourceWord } });
            after = make (listId, 2, "memo", "After");
        }

        std::string make (const std::string& parent, int index, const std::string& kind, const std::string& name,
                          const doc::ShowDocument::Attributes& born = {})
        {
            const auto made = document.createCue (parent, index, kind, name, {}, born);
            REQUIRE_MESSAGE (made.ok, kind << " " << name << ": " << made.reason);
            return made.id;
        }

        void set (const std::string& address, const std::string& value)
        {
            REQUIRE_MESSAGE (document.setAttribute (address, value).ok, address << " = " << value);
        }

        //  A sound in `parent`, at `at` when it is a time group's member, playing `file`.
        std::string sound (const std::string& parent, int index, const std::string& name, const std::string& at,
                           const std::string& file)
        {
            const auto id = at.empty() ? make (parent, index, "media", name)
                                       : make (parent, index, "media", name, { { "at", at } });
            set ("/godot/cue/" + id + "/file", file);
            return id;
        }

        //  A turn of the code, as a watcher submits it: applied in the next tick's drain.
        void heard (const char* what, const char* rate, std::int64_t frame, std::int64_t sample, double speed = 1.0)
        {
            REQUIRE (engine.submit (source, "code.heard",
                                    { word (source), word (what), word (rate), osc::Value::int64 (frame),
                                      osc::Value::int64 (sample), osc::Value::float64 (speed) }));
        }

        void tickOnce()
        {
            audio.samples = tick * perTick;
            runner.beforeTick (engine, tick);
            rejected += engine.processTick (tick++).rejected;
            audio.completeArms (engine);
        }

        //  Runs ticks until the next one to run is `next`.
        void ticksUntil (std::int64_t next)
        {
            while (tick < next)
                tickOnce();
        }

        /*  GO with the standby on `cueId`, and the ticks after it until a
            group it arms reads armed: its job enters its members phase on the
            tick after the GO, and the run says so on the one after that. */
        void goOn (const std::string& cueId)
        {
            REQUIRE (engine.submit ("cli", "node.set", { word (cue::standbyAddressOf (listId)), word (cueId) }));
            tickOnce();
            REQUIRE (engine.submit ("cli", "go", {}));
            tickOnce();
            tickOnce();
            tickOnce();
        }

        //  A command from the command line, and the tick it is applied on.
        bool send (const std::string& name, std::vector<osc::Value> args = {})
        {
            const auto taken = engine.submit ("cli", name, std::move (args));
            tickOnce();
            return taken;
        }

        bool tickUntil (const std::function<bool()>& done, int bound = 200)
        {
            for (int n = 0; n < bound && ! done(); ++n)
                tickOnce();

            return done();
        }

        std::vector<const cue::Run*> runsOf (const std::string& cueId) const
        {
            std::vector<const cue::Run*> out;

            for (const auto& run : runs.all())
                if (run.cue == cueId)
                    out.push_back (&run);

            return out;
        }

        std::size_t countOf (const std::string& cueId) const { return runsOf (cueId).size(); }

        bool armed (const std::string& groupId) const
        {
            const auto* run = runs.liveRunOf (groupId);
            return run != nullptr && run->state == cue::runState::playing && run->phase == cue::groupPhase::members;
        }

        std::string standby() const
        {
            return document.getAttribute (cue::standbyAddressOf (listId)).value_or (std::string {});
        }

        std::size_t recordsOf (const std::string& command)
        {
            std::size_t count = 0;

            for (const auto& record : LogFile::parse (engine.log().contents()).records)
                if (record.command == command && record.kind == LogRecord::Kind::applied)
                    ++count;

            return count;
        }

        TimePlayer audio;
        PictureSink pictures;
        std::map<std::string, double> lengths;

        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::Focus focus;
        doc::IdRegistry runIds = doc::IdRegistry::withSeed (77);
        cue::Runner runner { document, runs, runIds, focus };

        std::string source, listId, before, group, after;
        std::int64_t tick = 1;
        std::size_t rejected = 0;
    };

    //  The first tick whose hook sees `start` within two seconds, once the code is heard by `from`.
    std::int64_t loadTickFor (std::int64_t start, std::int64_t from)
    {
        return std::max (from, (start - preloadSamples + perTick - 1) / perTick);
    }
}

TEST_CASE ("time group on its code: an LTC member is loaded two seconds ahead and launched on exactly the sample the code reaches its time on - at 25, and across 29.97 drop-frame's dropped minute")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    const char* rateWord = "25";
    auto rate = timecode::Rate::fps25;
    const char* from = "01:00:00:00";
    const char* at = "01:00:10:00";

    /*  How far the code's frame is from its anchor, in samples: worked by hand
        from FrameClock's rule, not asked of CodeMap. 250 frames at 25 are
        480 000 samples. At 29.97 drop-frame, 01:00:58:00 to 01:01:00:02 is 60
        frames - :00 and :01 of minute one are dropped - and 60 frames of
        1001/30000 s are 96 096 samples exactly. */
    std::int64_t offset = 480'000;

    SUBCASE ("25 frames") {}

    SUBCASE ("29.97 drop-frame, across the minute it drops two labels")
    {
        rateWord = "29.97df";
        rate = timecode::Rate::fps2997df;
        from = "01:00:58:00";
        at = "01:01:00:02";
        offset = 96'096;
    }

    /*  A SHOW AT 25 HEARING A CODE AT 30 (a warned show, AIL): the label is
        read at the show's rate - frame 12 of 25 is 0.48 s - and the member
        starts on the first frame of the code at or after that moment, 14.4
        frames of 30 rounded up to 15: 315 frames from the anchor, 1600 samples
        each. */
    const char* showWord = nullptr;

    SUBCASE ("a show at 25 hearing a code at 30")
    {
        showWord = "25";
        rateWord = "30";
        rate = timecode::Rate::fps30;
        at = "01:00:10:12";
        offset = 315 * 1600;
    }

    CodeRig rig (Source::ltc, showWord != nullptr ? showWord : rateWord);
    const auto rain = rig.sound (rig.group, 0, "Rain", at, "rain.wav");

    rig.goOn (rig.group);
    REQUIRE (rig.armed (rig.group));
    REQUIRE (rig.standby() == rig.after);
    const auto armedAs = rig.runs.liveRunOf (rig.group)->id;

    //  THE CODE, heard from tick 100: its first frame on that tick's sample.
    const std::int64_t anchor = 100 * perTick;
    rig.ticksUntil (100);
    rig.heard ("jumped", rateWord, frameNamed (from, rate), anchor);

    const auto start = anchor + offset;
    const auto loadTick = loadTickFor (start, 101);

    //  NOT LOADED UNTIL TWO SECONDS AHEAD, then loaded once, under the group, its voice asked for.
    rig.ticksUntil (loadTick);
    CHECK (rig.countOf (rain) == 0);
    rig.tickOnce();
    REQUIRE (rig.countOf (rain) == 1);

    const auto* loaded = rig.runsOf (rain).front();
    const auto loadedAs = loaded->id;
    CHECK (loaded->parent == armedAs);
    CHECK (loaded->state == cue::runState::armed);
    CHECK (loaded->launchRequestedAtTick == 0);
    CHECK (std::any_of (rig.audio.asked.begin(), rig.audio.asked.end(),
                        [&loadedAs] (const cue::ArmRequest& arm) { return arm.runId == loadedAs; }));

    //  LAUNCHED ON THE SAMPLE, and on nothing else; on time, so not late.
    rig.ticksUntil (start / perTick + 30);
    REQUIRE (rig.countOf (rain) == 1);
    const auto* played = rig.runs.find (loadedAs);
    REQUIRE (played != nullptr);
    CHECK (played->launchedAtSample == start);
    CHECK (played->late == 0);
    REQUIRE (rig.audio.launches.size() == 1u);
    CHECK (rig.audio.launches.front().second == start);

    //  THE STANDBY NEVER MOVED, and the group is armed after its last member, a minute on.
    rig.ticksUntil (rig.tick + 3000);
    CHECK (rig.standby() == rig.after);
    CHECK (rig.armed (rig.group));
    CHECK (rig.runs.liveRunOf (rig.group)->id == armedAs);
    CHECK (rig.countOf (rain) == 1);
    CHECK (rig.audio.launches.size() == 1u);
    CHECK (rig.rejected == 0);
}

TEST_CASE ("time group on its code: an MTC source and the wall clock fire the same way - the clock's member at its millisecond of the day")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    auto kind = Source::mtc;
    const char* rateWord = "25";
    std::int64_t frame = frameNamed ("01:00:00:00", timecode::Rate::fps25);
    std::string at = "01:00:10:00";

    SUBCASE ("MTC at 25") {}

    SUBCASE ("the wall clock")
    {
        kind = Source::clock;
        rateWord = "ms";
        frame = 72'000'000;             // 20:00:00 in milliseconds of the day
        at = "20:00:10";
    }

    CodeRig rig (kind, "25");
    const auto chime = rig.sound (rig.group, 0, "Chime", at, "chime.wav");
    rig.goOn (rig.group);
    REQUIRE (rig.armed (rig.group));

    const std::int64_t anchor = 100 * perTick;
    rig.ticksUntil (100);
    rig.heard ("jumped", rateWord, frame, anchor);

    //  Ten seconds of either: 480 000 samples.
    const auto start = anchor + 480'000;
    const auto loadTick = loadTickFor (start, 101);

    rig.ticksUntil (loadTick);
    CHECK (rig.countOf (chime) == 0);
    rig.tickOnce();
    REQUIRE (rig.countOf (chime) == 1);

    rig.ticksUntil (start / perTick + 30);
    REQUIRE (rig.countOf (chime) == 1);
    CHECK (rig.runsOf (chime).front()->launchedAtSample == start);
    REQUIRE (rig.audio.launches.size() == 1u);
    CHECK (rig.audio.launches.front().second == start);
    CHECK (rig.standby() == rig.after);
    CHECK (rig.armed (rig.group));
}

TEST_CASE ("time group on its code: a code stopped before a member's time fires nothing; come back, it fires the member when it crosses its time")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    CodeRig rig;
    const auto rain = rig.sound (rig.group, 0, "Rain", "01:00:10:00", "rain.wav");
    rig.goOn (rig.group);

    const auto f0 = frameNamed ("01:00:00:00", timecode::Rate::fps25);
    rig.ticksUntil (100);
    rig.heard ("jumped", "25", f0, 100 * perTick);

    //  Loaded at its two seconds, then the code stops 1.6 s short of its time.
    rig.ticksUntil (520);
    REQUIRE (rig.countOf (rain) == 1);
    rig.heard ("stopped", "25", f0 + 210, 520 * perTick, 0.0);

    //  Long past where its time would have been: still loaded, never asked for, never launched.
    rig.ticksUntil (900);
    REQUIRE (rig.countOf (rain) == 1);
    const auto loadedAs = rig.runsOf (rain).front()->id;
    CHECK (rig.runsOf (rain).front()->launchRequestedAtTick == 0);
    CHECK (rig.audio.launches.empty());

    //  BACK, from where it stopped: the member crossed forty frames later, on that sample.
    rig.heard ("jumped", "25", f0 + 210, 900 * perTick);
    const auto start = 900 * perTick + 40 * 1920;

    rig.ticksUntil (start / perTick + 30);
    REQUIRE (rig.countOf (rain) == 1);
    CHECK (rig.runsOf (rain).front()->id == loadedAs);
    CHECK (rig.runsOf (rain).front()->launchedAtSample == start);
    REQUIRE (rig.audio.launches.size() == 1u);
    CHECK (rig.audio.launches.front().second == start);
}

TEST_CASE ("time group on its code: a jump past a free runner's time never fires it; a jump back before it fires it, and a second crossing restarts it")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    CodeRig rig;
    const auto rain = rig.sound (rig.group, 0, "Rain", "01:00:10:00", "rain.wav");
    rig.goOn (rig.group);

    const auto f0 = frameNamed ("01:00:00:00", timecode::Rate::fps25);
    rig.ticksUntil (100);
    rig.heard ("jumped", "25", f0, 100 * perTick);

    //  A JUMP PAST IT, before it was loaded: never loaded, never fired.
    rig.ticksUntil (300);
    rig.heard ("jumped", "25", f0 + 300, 300 * perTick);
    rig.ticksUntil (1000);
    CHECK (rig.countOf (rain) == 0);
    CHECK (rig.audio.launches.empty());

    //  A JUMP BACK, two seconds before it, the code running on: fired on its sample.
    rig.heard ("jumped", "25", f0 + 200, 1000 * perTick);
    const auto first = 1000 * perTick + 50 * 1920;
    rig.ticksUntil (first / perTick + 30);
    REQUIRE (rig.countOf (rain) == 1);
    const auto firstRun = rig.runsOf (rain).front()->id;
    CHECK (rig.runs.find (firstRun)->launchedAtSample == first);

    //  AGAIN: a second crossing starts it again, and the run of the first is stopped as it does.
    rig.heard ("jumped", "25", f0 + 200, 1200 * perTick);
    const auto second = 1200 * perTick + 50 * 1920;
    rig.ticksUntil (second / perTick + 30);
    REQUIRE (rig.countOf (rain) == 2);
    REQUIRE (rig.audio.launches.size() == 2u);
    CHECK (rig.audio.launches.back().second == second);
    CHECK (rig.runs.find (firstRun)->isFinished());
    CHECK (! rig.audio.stops.empty());

    /*  AND A MEMBER LOADED FOR A TIME THE CODE THEN JUMPS AWAY FROM is let go
        idle: loaded at its two seconds, the code jumps an hour on. */
    rig.heard ("jumped", "25", f0 + 200, 1400 * perTick);
    rig.ticksUntil (1402);
    REQUIRE (rig.countOf (rain) == 3);
    const auto idle = rig.runsOf (rain).back()->id;
    rig.heard ("jumped", "25", f0 + 90'000, 1402 * perTick);
    rig.ticksUntil (1450);
    CHECK (rig.runs.find (idle)->isFinished());
    CHECK (rig.runs.find (idle)->launchedAtSample == 0);
    CHECK (rig.audio.launches.size() == 2u);
}

TEST_CASE ("time group on its code: armed while the code is already past a member's time, it does not fire that one - and fires the one still ahead")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    CodeRig rig;
    const auto early = rig.sound (rig.group, 0, "Early", "01:00:05:00", "early.wav");
    const auto late = rig.sound (rig.group, 1, "Late", "01:00:20:00", "late.wav");

    //  The code runs from tick 100; the GO comes at tick 500, after Early's time (tick 350).
    const auto f0 = frameNamed ("01:00:00:00", timecode::Rate::fps25);
    rig.ticksUntil (100);
    rig.heard ("jumped", "25", f0, 100 * perTick);
    rig.ticksUntil (500);
    rig.goOn (rig.group);
    REQUIRE (rig.armed (rig.group));

    const auto lateStart = 100 * perTick + 500 * 1920;
    rig.ticksUntil (lateStart / perTick + 30);

    CHECK (rig.countOf (early) == 0);
    REQUIRE (rig.countOf (late) == 1);
    CHECK (rig.runsOf (late).front()->launchedAtSample == lateStart);
    CHECK (rig.audio.launches.size() == 1u);
}

TEST_CASE ("time group on its code: a member loaded too close to its time starts late, from its top, and says so")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    CodeRig rig;
    const auto rain = rig.sound (rig.group, 0, "Rain", "01:00:10:00", "rain.wav");
    const auto f0 = frameNamed ("01:00:00:00", timecode::Rate::fps25);
    const auto start = 100 * perTick + 480'000;

    rig.ticksUntil (100);
    rig.heard ("jumped", "25", f0, 100 * perTick);

    SUBCASE ("the group armed a few ticks before the member's time")
    {
        rig.ticksUntil (start / perTick - 5);
        rig.goOn (rig.group);
    }

    SUBCASE ("loaded in time, and the disk slower than its time")
    {
        rig.goOn (rig.group);
        rig.ticksUntil (start / perTick - 200);
        rig.audio.slowDisk = true;
        rig.ticksUntil (start / perTick + 3);
        rig.audio.slowDisk = false;
    }

    rig.ticksUntil (start / perTick + 40);
    REQUIRE (rig.countOf (rain) == 1);
    const auto* played = rig.runsOf (rain).front();

    //  Started, as soon as it could: later than its sample, and the run says by how many blocks.
    CHECK (played->launchedAtSample > start);
    CHECK (played->launchedAtSample - start < 20 * perTick);
    CHECK (played->late == static_cast<int> ((played->launchedAtSample - start) / 128));
    CHECK (played->late > 0);
    CHECK (rig.recordsOf ("run.late") >= 1u);

    //  From its top: its arm asked from the file's own start.
    for (const auto& arm : rig.audio.asked)
        if (arm.runId == played->id)
            CHECK (arm.startOffset == doctest::Approx (0.0));
}

TEST_CASE ("time group on its code: a timeline scene and an automatic sequence as members - each sound launched once, on the code's sample plus its offset")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    CodeRig rig;
    const auto f0 = frameNamed ("01:00:00:00", timecode::Rate::fps25);
    const auto start = 100 * perTick + 480'000;

    std::vector<std::pair<std::string, std::int64_t>> expected;
    std::string scene, memo;

    SUBCASE ("a timeline: each at its pre-wait from the scene's start")
    {
        scene = rig.make (rig.group, 0, "group", "Storm", { { "mode", "timeline" }, { "at", "01:00:10:00" } });
        const auto thunder = rig.sound (scene, 0, "Thunder", "", "thunder.wav");
        const auto rain = rig.sound (scene, 1, "Rain", "", "rain.wav");
        rig.set ("/godot/cue/" + rain + "/preWait", "1.5");
        const auto wind = rig.sound (scene, 2, "Wind", "", "wind.wav");
        rig.set ("/godot/cue/" + wind + "/preWait", "3.25");
        memo = rig.make (scene, 3, "memo", "Lights");
        rig.set ("/godot/cue/" + memo + "/preWait", "0.5");

        expected = { { thunder, start }, { rain, start + 72'000 }, { wind, start + 156'000 } };
    }

    SUBCASE ("an automatic sequence: each after the one before, by the lengths the show knows - the one before still sounding")
    {
        scene = rig.make (rig.group, 0, "group", "Walk", { { "advance", "auto" }, { "at", "01:00:10:00" } });
        const auto thunder = rig.sound (scene, 0, "Thunder", "", "thunder.wav");
        rig.set ("/godot/cue/" + thunder + "/postWait", "0.5");
        const auto rain = rig.sound (scene, 1, "Rain", "", "rain.wav");
        rig.set ("/godot/cue/" + rain + "/preWait", "0.25");
        rig.lengths["thunder.wav"] = 2.0;
        rig.lengths["rain.wav"] = 1.0;

        //  2.0 s of thunder, its post-wait and the rain's pre-wait: 2.75 s.
        expected = { { thunder, start }, { rain, start + 132'000 } };
    }

    rig.goOn (rig.group);
    rig.ticksUntil (100);
    rig.heard ("jumped", "25", f0, 100 * perTick);

    //  EACH LOADED TWO SECONDS BEFORE ITS OWN START, not all at the scene's.
    const auto& [lastCue, lastStart] = expected.back();
    rig.ticksUntil (loadTickFor (lastStart, 101));
    CHECK (rig.countOf (lastCue) == 0);
    rig.ticksUntil (lastStart / perTick + 400);

    for (const auto& [cueId, at] : expected)
    {
        INFO ("cue " << cueId);
        REQUIRE (rig.countOf (cueId) == 1);
        CHECK (rig.runsOf (cueId).front()->launchedAtSample == at);
        CHECK (rig.runsOf (cueId).front()->late == 0);
    }

    //  ONCE EACH: nothing launched a second time, by a counter or by the code.
    CHECK (rig.audio.launches.size() == expected.size());
    CHECK (rig.countOf (scene) == 1);

    //  A memo of the scene fired on the tick its moment falls in.
    if (! memo.empty())
    {
        REQUIRE (rig.countOf (memo) == 1);
        CHECK (rig.runsOf (memo).front()->launchRequestedAtTick == (start + 24'000) / perTick);
    }

    CHECK (rig.armed (rig.group));
    CHECK (rig.standby() == rig.after);
    CHECK (rig.rejected == 0);
}

TEST_CASE ("time group on its code: a picture is read ahead while it waits and comes up on the code's sample")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    CodeRig rig;
    const auto moon = rig.make (rig.group, 0, "video", "Moon", { { "at", "01:00:10:00" } });
    rig.set ("/godot/cue/" + moon + "/source", "picture");
    rig.set ("/godot/cue/" + moon + "/file", "moon.png");

    rig.goOn (rig.group);
    rig.ticksUntil (100);
    rig.heard ("jumped", "25", frameNamed ("01:00:00:00", timecode::Rate::fps25), 100 * perTick);

    const auto start = 100 * perTick + 480'000;
    rig.ticksUntil (loadTickFor (start, 101) + 3);
    REQUIRE (rig.countOf (moon) == 1);
    CHECK (rig.pictures.readsAhead ("moon.png"));
    CHECK (rig.pictures.shown.empty());

    rig.ticksUntil (start / perTick + 20);
    const auto runId = rig.runsOf (moon).front()->id;
    REQUIRE (rig.pictures.firstPoint.count (runId) == 1u);
    CHECK (rig.pictures.firstPoint.at (runId) == start);
    CHECK (rig.runsOf (moon).front()->late == 0);
}

TEST_CASE ("time group on its code: in a Persistent section it is armed at the first step and fires on its code")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    CodeRig rig;
    const auto section = rig.document.createPersistent (rig.listId);
    REQUIRE (section.ok);
    const auto kept = rig.make (section.id, 0, "group", "Always", { { "mode", "time" }, { "timeSource", "ltc" } });
    const auto chime = rig.sound (kept, 0, "Chime", "01:00:10:00", "chime.wav");

    rig.goOn (rig.before);
    REQUIRE (rig.tickUntil ([&rig, &kept] { return rig.armed (kept); }));

    rig.ticksUntil (100);
    rig.heard ("jumped", "25", frameNamed ("01:00:00:00", timecode::Rate::fps25), 100 * perTick);

    const auto start = 100 * perTick + 480'000;
    rig.ticksUntil (start / perTick + 30);
    REQUIRE (rig.countOf (chime) == 1);
    CHECK (rig.runsOf (chime).front()->launchedAtSample == start);
    CHECK (rig.armed (kept));

    //  The list's own time group, never stepped onto, armed nothing.
    CHECK (rig.runs.liveRunOf (rig.group) == nullptr);
}

TEST_CASE ("time group on its code: a replay of a session with code.heard records reaches the same runs and the same maps")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    CodeRig rig;
    const auto f0 = frameNamed ("01:00:00:00", timecode::Rate::fps25);
    const auto rain = rig.sound (rig.group, 0, "Rain", "01:00:05:00", "rain.wav");
    const auto scene = rig.make (rig.group, 1, "group", "Storm", { { "mode", "timeline" }, { "at", "01:00:12:00" } });
    rig.sound (scene, 0, "Thunder", "", "thunder.wav");
    const auto later = rig.sound (scene, 1, "Hail", "", "hail.wav");
    rig.set ("/godot/cue/" + later + "/preWait", "1");

    rig.goOn (rig.group);
    rig.ticksUntil (100);
    rig.heard ("jumped", "25", f0, 100 * perTick);
    rig.ticksUntil (700);
    rig.heard ("stopped", "25", f0 + 300, 700 * perTick, 0.0);
    rig.ticksUntil (760);
    rig.heard ("jumped", "25", f0 + 280, 760 * perTick);
    rig.ticksUntil (1100);
    REQUIRE (rig.send ("run.stopAll"));
    rig.ticksUntil (1200);

    REQUIRE (rig.countOf (rain) == 1);
    REQUIRE (rig.countOf (later) >= 1);
    CHECK (rig.rejected == 0);

    const auto show = doc::CanonicalXml::write (rig.document);
    const auto original = LogFile::parse (rig.engine.log().contents());
    REQUIRE (original.errors.empty());

    //  `wfg replay`: no audio side, no picture side, no hooks.
    Engine engine;
    engine.log().openInMemory ({});
    doc::ShowDocument document;
    REQUIRE (doc::CanonicalXml::read (show, document).ok);
    cue::RunTable runs;
    cue::Focus focus;
    doc::IdRegistry runIds = doc::IdRegistry::withSeed (77);
    cue::Runner runner { document, runs, runIds, focus };
    doc::registerDocumentCommands (engine.commands(), document);
    cue::registerCueCommands (engine.commands(), document, focus);
    cue::registerRunCommands (engine.commands(), runs);
    cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);
    runner.setSamplesPerTick (static_cast<int> (perTick));

    const auto result = replay (engine, original);

    for (const auto& mismatch : result.mismatches)
        MESSAGE (mismatch);

    CHECK (result.ok);

    for (const auto& run : rig.runs.all())
    {
        INFO ("run " << run.id << " of " << run.cue);
        const auto* again = runs.find (run.id);
        REQUIRE (again != nullptr);
        CHECK (again->cue == run.cue);
        CHECK (again->state == run.state);
        CHECK (again->parent == run.parent);
        CHECK (again->endedAtTick == run.endedAtTick);
    }

    CHECK (runs.all().size() == rig.runs.all().size());
    CHECK (runner.codeMaps() == rig.runner.codeMaps());
}

TEST_CASE ("time group on its code: a member's pre-wait counts from its time, on Go.dot's clock - and still runs out when the code stops just after the time")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    CodeRig rig;
    const auto rain = rig.sound (rig.group, 0, "Rain", "01:00:10:00", "rain.wav");
    rig.set ("/godot/cue/" + rain + "/preWait", "1");
    rig.goOn (rig.group);

    const auto f0 = frameNamed ("01:00:00:00", timecode::Rate::fps25);
    rig.ticksUntil (100);
    rig.heard ("jumped", "25", f0, 100 * perTick);

    //  Its time is on tick 600; the code stops five frames after it, at tick 605.
    const auto time = 100 * perTick + 250 * 1920;
    const auto start = time + 48'000;

    SUBCASE ("the code running on") {}

    SUBCASE ("the code stopped a fifth of a second after the member's time")
    {
        rig.ticksUntil (605);
        rig.heard ("stopped", "25", f0 + 255, 605 * perTick, 0.0);
    }

    rig.ticksUntil (start / perTick + 30);
    REQUIRE (rig.countOf (rain) == 1);
    CHECK (rig.runsOf (rain).front()->launchedAtSample == start);
    CHECK (rig.runsOf (rain).front()->late == 0);
    CHECK (rig.audio.launches.size() == 1u);
}

TEST_CASE ("time group on its code: a scene entered ahead and stopped before its time gives its members back with no footer; one stopped after runs its footer")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    CodeRig rig;
    const auto scene = rig.make (rig.group, 0, "group", "Storm", { { "mode", "timeline" }, { "at", "01:00:10:00" } });
    const auto thunder = rig.sound (scene, 0, "Thunder", "", "thunder.wav");
    const auto footer = rig.document.createRole (scene, "footer");
    REQUIRE (footer.ok);
    const auto release = rig.make (footer.id, 0, "memo", "Release");

    rig.goOn (rig.group);
    rig.ticksUntil (100);
    rig.heard ("jumped", "25", frameNamed ("01:00:00:00", timecode::Rate::fps25), 100 * perTick);

    auto stopAt = std::int64_t { 550 };
    auto footerRuns = false;

    SUBCASE ("stopped a second before its time: loaded, never begun") {}

    SUBCASE ("stopped a second after it")
    {
        stopAt = 650;
        footerRuns = true;
    }

    rig.ticksUntil (stopAt);
    REQUIRE (rig.countOf (scene) == 1);
    REQUIRE (rig.countOf (thunder) == 1);
    REQUIRE (rig.send ("run.stopAll"));
    rig.ticksUntil (stopAt + 200);

    CHECK (rig.runsOf (scene).front()->isFinished());
    CHECK (rig.runsOf (thunder).front()->isFinished());
    CHECK (rig.countOf (release) == (footerRuns ? 1u : 0u));
    CHECK (rig.audio.launches.size() == (footerRuns ? 1u : 0u));
}

//==============================================================================
/*  TG.8: THE MEMBERS THAT CHASE (namespace draft §60; AIF, AIG, AIH, AIN, AIT,
    AIV). The code is driven by `code.heard` as above; a member's `chase` row
    says how it keeps up. The fake audio side records each move placed on a
    slot's reader - where from, where to, over what crossfade - and the picture
    side each point on a movie's time ring. 48 kHz, 960 samples a tick, 128-sample
    blocks: a launch horizon of two ticks, 1920 samples, and a seat's lead of ten
    ticks, 9600 samples, after the record's sample.
*/
namespace
{
    constexpr std::int64_t horizonSamples = 2 * perTick;
    constexpr std::int64_t seatLeadSamples = 10 * perTick;

    //  The applied records of a command, oldest first.
    std::vector<LogRecord> appliedOf (Engine& engine, const std::string& command)
    {
        std::vector<LogRecord> out;

        for (auto& record : LogFile::parse (engine.log().contents()).records)
            if (record.command == command && record.kind == LogRecord::Kind::applied)
                out.push_back (std::move (record));

        return out;
    }

    //  A sound in the group at `at`, chasing by `chase`, of a file `seconds` long.
    std::string chaser (CodeRig& rig, int index, const std::string& name, const std::string& at,
                        const std::string& file, const std::string& chase, double seconds = 60.0)
    {
        const auto id = rig.sound (rig.group, index, name, at, file);
        rig.set ("/godot/cue/" + id + "/chase", chase);
        rig.lengths[file] = seconds;
        return id;
    }

    //  The moves placed on one track's reader.
    std::vector<TimePlayer::Loop> loopsOn (const CodeRig& rig, int track)
    {
        std::vector<TimePlayer::Loop> out;

        for (const auto& loop : rig.audio.loops)
            if (loop.track == track)
                out.push_back (loop);

        return out;
    }

    //  The arm last asked for a run.
    std::optional<cue::ArmRequest> armOf (const CodeRig& rig, const std::string& runId)
    {
        std::optional<cue::ArmRequest> found;

        for (const auto& arm : rig.audio.asked)
            if (arm.runId == runId)
                found = arm;

        return found;
    }

    const cue::Run* liveOf (const CodeRig& rig, const std::string& cueId)
    {
        const cue::Run* found = nullptr;

        for (const auto& run : rig.runs.all())
            if (run.cue == cueId && ! run.isFinished())
                found = &run;

        return found;
    }

    constexpr double toTheSample = 1.0e-9;
}

TEST_CASE ("time group chasing: a jump chaser 30 ms behind its code jumps once, over the 10 ms crossfade, to the code's second - 5 ms behind it does nothing, nor does a free runner 30 ms behind")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    std::int64_t shift = 1440;          // 30 ms at 48 kHz
    std::string chase = "jump";
    auto jumps = true;

    SUBCASE ("30 ms behind, chasing by jump") {}

    SUBCASE ("5 ms behind, inside the tolerance")
    {
        shift = 240;
        jumps = false;
    }

    SUBCASE ("30 ms behind, a free runner")
    {
        chase = "free";
        jumps = false;
    }

    /*  A CATCH-UP METHOD jumps only past `sync/chaseJump`: under it, it is
        catching - TG.9's to steer. */
    SUBCASE ("30 ms behind, a varispeed chaser")
    {
        chase = "varispeed";
        jumps = false;
    }

    CodeRig rig;
    const auto rain = chaser (rig, 0, "Rain", "01:00:10:00", "rain.wav", chase);
    rig.goOn (rig.group);

    const auto f0 = frameNamed ("01:00:00:00", timecode::Rate::fps25);
    const std::int64_t anchor = 100 * perTick;
    rig.ticksUntil (100);
    rig.heard ("jumped", "25", f0, anchor);

    //  STARTED ON ITS SAMPLE, by TG.7, and in step: nothing moved.
    const auto start = anchor + 480'000;
    rig.ticksUntil (start / perTick + 50);
    REQUIRE (rig.countOf (rain) == 1);
    const auto runId = rig.runsOf (rain).front()->id;
    const auto track = rig.runs.find (runId)->track;
    REQUIRE (rig.runs.find (runId)->launchedAtSample == start);
    CHECK (rig.audio.loops.empty());
    CHECK (rig.runs.find (runId)->chaseState == (chase == "free" ? "free" : "inStep"));
    CHECK (std::abs (rig.runs.find (runId)->chaseError) < 0.001);

    /*  THE CODE RE-ANCHORED: the frame twenty seconds on heard `shift` samples
        sooner than the line had it, so the member is that far behind. Applied
        in tick 1000's drain; measured by tick 1001's hook, a launch horizon
        ahead. */
    rig.ticksUntil (1000);
    rig.heard ("running", "25", f0 + 500, anchor + 960'000 - shift);
    rig.tickOnce();
    rig.tickOnce();

    const auto* run = rig.runs.find (runId);
    REQUIRE (run != nullptr);

    if (! jumps)
    {
        rig.ticksUntil (1100);
        CHECK (rig.audio.loops.empty());
        CHECK (rig.runs.find (runId)->chaseState == (chase == "free" ? "free" : chase == "jump" ? "inStep" : "catching"));

        if (chase != "free")
            CHECK (rig.runs.find (runId)->chaseError == doctest::Approx (-1000.0 * static_cast<double> (shift) / 48000.0).epsilon (1e-6));
        else
            CHECK (rig.runs.find (runId)->chaseError == doctest::Approx (0.0));

        CHECK_FALSE (rig.runs.find (runId)->isFinished());
        return;
    }

    //  ONE MOVE ON ITS READER, from where the reader is to the code's second, over 10 ms.
    REQUIRE (rig.audio.loops.size() == 1u);
    const auto loop = rig.audio.loops.front();
    const auto at = 1001 * perTick + horizonSamples;

    CHECK (loop.track == track);
    CHECK (loop.slot == 0);
    CHECK (loop.move.crossfade == doctest::Approx (0.010));
    CHECK (loop.move.loopIn == doctest::Approx (0.0));
    CHECK (loop.move.loopOut == doctest::Approx (0.0));
    CHECK (loop.move.direction == 1);
    CHECK (loop.move.from == doctest::Approx (static_cast<double> (at - start) / 48000.0).epsilon (toTheSample));
    CHECK (loop.move.fileAt == doctest::Approx (static_cast<double> (at - start + shift) / 48000.0).epsilon (toTheSample));
    CHECK (run->chaseState == "jumped");
    CHECK (run->chaseError == doctest::Approx (-30.0).epsilon (1e-6));

    //  The file's end the show knows is Go.dot's to place now: the clip's own is counted in reads.
    CHECK (rig.audio.endedOutside.count (track) == 1u);

    //  AND THEN IN STEP: no second move, the same run, its playhead where the code says.
    rig.ticksUntil (1100);
    CHECK (rig.audio.loops.size() == 1u);
    CHECK (rig.countOf (rain) == 1);
    CHECK (rig.audio.launches.size() == 1u);
    run = rig.runs.find (runId);
    CHECK_FALSE (run->isFinished());
    CHECK (run->chaseState == "inStep");
    CHECK (std::abs (run->chaseError) < 0.05);
    CHECK (run->position == doctest::Approx (static_cast<double> (1099 * perTick - start + shift) / 48000.0).epsilon (toTheSample));
}

TEST_CASE ("time group chasing: a locate seats a chaser at its offset and hands its run back on the record, jumps one still in range, ends one whose time is ahead, and leaves a free runner playing")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    CodeRig rig;
    const auto rain = chaser (rig, 0, "Rain", "01:00:10:00", "rain.wav", "jump");
    const auto wind = chaser (rig, 1, "Wind", "01:00:20:00", "wind.wav", "jump");
    const auto bell = chaser (rig, 2, "Bell", "01:00:12:00", "bell.wav", "free");
    rig.goOn (rig.group);
    const auto armedAs = rig.runs.liveRunOf (rig.group)->id;

    const auto f0 = frameNamed ("01:00:00:00", timecode::Rate::fps25);
    rig.ticksUntil (100);
    rig.heard ("jumped", "25", f0, 100 * perTick);

    //  Rain from tick 600 and the bell from tick 650; at tick 800, the wind still ten seconds off.
    rig.ticksUntil (800);
    REQUIRE (rig.countOf (rain) == 1);
    REQUIRE (rig.countOf (bell) == 1);
    REQUIRE (rig.countOf (wind) == 0);
    const auto rainRun = rig.runsOf (rain).front()->id;
    const auto bellRun = rig.runsOf (bell).front()->id;
    const auto rainTrack = rig.runs.find (rainRun)->track;
    const auto bellTrack = rig.runs.find (bellRun)->track;
    const auto rainStart = rig.runs.find (rainRun)->launchedAtSample;
    REQUIRE (rainStart == 100 * perTick + 480'000);
    const auto armsBefore = rig.audio.asked.size();

    //------------------------------------------------------------------------
    /*  A LOCATE FORWARD to 01:00:30:00. The wind is ten seconds into its file
        there - and ten and a fifth where a seated sound starts, a seat's lead
        on: seated at that offset, its run handed back. The rain, twenty in and
        still sounding, jumps there, never started again. The bell plays on. */
    rig.heard ("jumped", "25", f0 + 750, 800 * perTick);
    rig.tickOnce();

    const auto heard = appliedOf (rig.engine, "code.heard");
    REQUIRE (heard.size() == 2u);
    REQUIRE (heard.back().args.size() == 7u);
    const auto windRun = heard.back().args[6].getString();

    const auto* seated = rig.runs.find (windRun);
    REQUIRE (seated != nullptr);
    CHECK (seated->cue == wind);
    CHECK (seated->parent == armedAs);
    CHECK (seated->launchNotBefore == 800 * perTick + seatLeadSamples);

    const auto windArm = armOf (rig, windRun);
    REQUIRE (windArm.has_value());
    CHECK (windArm->startOffset == doctest::Approx (10.2).epsilon (toTheSample));

    //  The rain jumped: one move, to where the code is a launch horizon on; the same run, no new arm.
    const auto rainLoops = loopsOn (rig, rainTrack);
    REQUIRE (rainLoops.size() == 1u);
    CHECK (rainLoops.front().move.crossfade == doctest::Approx (0.010));
    CHECK (rainLoops.front().move.fileAt == doctest::Approx (20.0 + static_cast<double> (horizonSamples) / 48000.0).epsilon (toTheSample));
    CHECK (rainLoops.front().move.from
             == doctest::Approx (static_cast<double> (800 * perTick + horizonSamples - rainStart) / 48000.0).epsilon (toTheSample));
    CHECK (rig.countOf (rain) == 1);
    CHECK_FALSE (rig.runs.find (rainRun)->isFinished());
    CHECK (rig.runs.find (rainRun)->chaseState == "jumped");

    //  The free runner untouched: no move, no stop.
    CHECK (loopsOn (rig, bellTrack).empty());
    CHECK (std::count (rig.audio.stops.begin(), rig.audio.stops.end(), bellTrack) == 0);
    CHECK_FALSE (rig.runs.find (bellRun)->isFinished());

    //  The wind starts on the sample its offset was worked out for, and nothing moves again.
    rig.ticksUntil (850);
    CHECK (rig.runs.find (windRun)->launchedAtSample == 800 * perTick + seatLeadSamples);
    CHECK (rig.runs.find (windRun)->late == 0);
    CHECK (rig.runs.find (windRun)->chaseState == "inStep");
    CHECK (rig.audio.loops.size() == 1u);
    CHECK (rig.audio.asked.size() == armsBefore + 1);
    CHECK (rig.countOf (wind) == 1);

    //------------------------------------------------------------------------
    /*  A LOCATE BACK to 01:00:05:00: the rain's and the wind's times are ahead
        there - both end, as a jump ends what it abandons, their voices stopped;
        nothing is seated, and the bell, a free runner, plays on. */
    rig.heard ("jumped", "25", f0 + 125, 850 * perTick);
    rig.tickOnce();

    CHECK (appliedOf (rig.engine, "code.heard").back().args.size() == 6u);
    CHECK (rig.runs.find (rainRun)->state == cue::runState::done);
    CHECK (rig.runs.find (windRun)->state == cue::runState::done);
    CHECK (std::count (rig.audio.stops.begin(), rig.audio.stops.end(), rainTrack) >= 1);
    CHECK_FALSE (rig.runs.find (bellRun)->isFinished());
    CHECK (std::count (rig.audio.stops.begin(), rig.audio.stops.end(), bellTrack) == 0);

    //  And when the code runs into the rain's time again, five seconds on, it starts there, on its sample.
    const auto again = 850 * perTick + 5 * 48'000;
    rig.ticksUntil (again / perTick + 30);
    REQUIRE (rig.countOf (rain) == 2);
    CHECK (rig.runsOf (rain).back()->launchedAtSample == again);
    CHECK (rig.rejected == 0);
}

TEST_CASE ("time group chasing: a member asked for its sample and not yet heard is cut by a locate past its time - it never sounded, and is not started")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    CodeRig rig;
    const auto bell = chaser (rig, 0, "Bell", "01:00:10:00", "bell.wav", "free");
    rig.goOn (rig.group);

    const auto f0 = frameNamed ("01:00:00:00", timecode::Rate::fps25);
    rig.ticksUntil (100);
    rig.heard ("jumped", "25", f0, 100 * perTick);

    //  Until the code asks for it: a launch horizon and two ticks before its sample.
    REQUIRE (rig.tickUntil ([&rig, &bell]
                            {
                                return rig.countOf (bell) == 1 && rig.runsOf (bell).front()->launchRequestedAtTick != 0;
                            }, 1000));

    const auto askedId = rig.runsOf (bell).front()->id;
    const auto track = rig.runs.find (askedId)->track;
    REQUIRE (rig.runs.find (askedId)->state != cue::runState::playing);

    //  A LOCATE PAST ITS TIME, in the drain before its launch is said: cut.
    rig.heard ("jumped", "25", f0 + 500, rig.tick * perTick);
    rig.tickOnce();

    const auto* cut = rig.runs.find (askedId);
    CHECK (cut->state == cue::runState::done);
    CHECK (cut->startedAtTick == -1);
    CHECK (std::count (rig.audio.stops.begin(), rig.audio.stops.end(), track) >= 1);

    //  And never started again: the code jumped past its time (AIH).
    rig.ticksUntil (rig.tick + 300);
    CHECK (rig.countOf (bell) == 1);
    CHECK (rig.rejected == 0);
}

TEST_CASE ("time group chasing: the code stopped, a chaser plays on; the code back three seconds later is a locate, and the chaser jumps to it")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    CodeRig rig;
    const auto rain = chaser (rig, 0, "Rain", "01:00:10:00", "rain.wav", "jump");
    rig.goOn (rig.group);

    const auto f0 = frameNamed ("01:00:00:00", timecode::Rate::fps25);
    rig.ticksUntil (100);
    rig.heard ("jumped", "25", f0, 100 * perTick);

    rig.ticksUntil (800);
    REQUIRE (rig.countOf (rain) == 1);
    const auto runId = rig.runsOf (rain).front()->id;
    const auto track = rig.runs.find (runId)->track;
    const auto start = rig.runs.find (runId)->launchedAtSample;

    //  STOPPED at 01:00:14:00, four seconds into the rain: it plays on, nothing moves it (AIG).
    rig.heard ("stopped", "25", f0 + 350, 800 * perTick, 0.0);
    rig.ticksUntil (950);
    CHECK_FALSE (rig.runs.find (runId)->isFinished());
    CHECK (rig.audio.loops.empty());
    CHECK (std::count (rig.audio.stops.begin(), rig.audio.stops.end(), track) == 0);

    //  BACK from where it stopped, three seconds on: the rain is three seconds ahead, and jumps back to the code.
    rig.heard ("jumped", "25", f0 + 350, 950 * perTick);
    rig.tickOnce();

    REQUIRE (rig.audio.loops.size() == 1u);
    const auto at = 950 * perTick + horizonSamples;
    CHECK (rig.audio.loops.front().move.fileAt == doctest::Approx (4.0 + static_cast<double> (horizonSamples) / 48000.0).epsilon (toTheSample));
    CHECK (rig.audio.loops.front().move.from == doctest::Approx (static_cast<double> (at - start) / 48000.0).epsilon (toTheSample));
    CHECK (rig.runs.find (runId)->chaseError == doctest::Approx (3000.0).epsilon (1e-6));

    rig.ticksUntil (1050);
    CHECK (rig.audio.loops.size() == 1u);
    CHECK (rig.countOf (rain) == 1);
    CHECK_FALSE (rig.runs.find (runId)->isFinished());
    CHECK (rig.runs.find (runId)->chaseState == "inStep");
}

TEST_CASE ("time group chasing: a movie that chases cuts on its time ring, and its locked sound jumps with it")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    CodeRig rig;
    const auto moon = rig.make (rig.group, 0, "video", "Moon", { { "at", "01:00:10:00" } });
    rig.set ("/godot/cue/" + moon + "/source", "movie");
    rig.set ("/godot/cue/" + moon + "/file", "moon.mov");
    rig.set ("/godot/cue/" + moon + "/chase", "jump");
    const auto voice = rig.make (rig.group, 1, "media", "Moon sound");
    rig.set ("/godot/cue/" + voice + "/file", "moon.wav");
    rig.set ("/godot/cue/" + voice + "/lockedTo", moon);
    rig.lengths["moon.mov"] = 60.0;
    rig.lengths["moon.wav"] = 60.0;

    rig.goOn (rig.group);
    const auto f0 = frameNamed ("01:00:00:00", timecode::Rate::fps25);
    const std::int64_t anchor = 100 * perTick;
    rig.ticksUntil (100);
    rig.heard ("jumped", "25", f0, anchor);

    rig.ticksUntil (800);
    REQUIRE (rig.countOf (moon) == 1);
    REQUIRE (rig.countOf (voice) == 1);
    const auto movieRun = rig.runsOf (moon).front()->id;
    const auto soundRun = rig.runsOf (voice).front()->id;
    REQUIRE (rig.runs.find (soundRun)->parent == movieRun);
    const auto soundTrack = rig.runs.find (soundRun)->track;
    CHECK (rig.audio.loops.empty());

    //  THE CODE RE-ANCHORED 300 ms sooner: the movie and its sound are that far behind.
    rig.heard ("running", "25", f0 + 500, anchor + 960'000 - 14'400);
    rig.ticksUntil (830);

    /*  THE MOVIE CUT: two points on its time ring on one sample - where it
        was, then the code's second - a launch horizon after tick 801. */
    const auto at = 801 * perTick + horizonSamples;
    std::vector<double> there;

    for (const auto& point : rig.pictures.times[movieRun])
        if (point.sample == at)
            there.push_back (point.value);

    REQUIRE (there.size() >= 2u);
    CHECK (there.back() == doctest::Approx (static_cast<double> (at - anchor - 480'000 + 14'400) / 48000.0).epsilon (toTheSample));
    CHECK (there.back() - there.front() == doctest::Approx (0.3).epsilon (1e-6));
    CHECK (rig.runs.find (movieRun)->chaseState == "inStep");

    //  ITS LOCKED SOUND, the same jump on its reader.
    const auto soundLoops = loopsOn (rig, soundTrack);
    REQUIRE (soundLoops.size() == 1u);
    CHECK (soundLoops.front().move.crossfade == doctest::Approx (0.010));
    CHECK (soundLoops.front().move.fileAt - soundLoops.front().move.from == doctest::Approx (0.3).epsilon (1e-6));
    CHECK (rig.audio.loops.size() == 1u);
    CHECK_FALSE (rig.runs.find (movieRun)->isFinished());
    CHECK_FALSE (rig.runs.find (soundRun)->isFinished());
}

TEST_CASE ("time group chasing: a chaser whose code lands in another of its slices is sought there - the same run, with the gap a seek makes, and it says so")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    CodeRig rig;
    const auto rain = chaser (rig, 0, "Rain", "01:00:10:00", "rain.wav", "jump");
    REQUIRE (rig.document.createRange (rain, 0.0, 10.0).ok);
    REQUIRE (rig.document.createRange (rain, 20.0, 40.0).ok);
    rig.goOn (rig.group);

    const auto f0 = frameNamed ("01:00:00:00", timecode::Rate::fps25);
    rig.ticksUntil (100);
    rig.heard ("jumped", "25", f0, 100 * perTick);

    rig.ticksUntil (800);
    REQUIRE (rig.countOf (rain) == 1);
    const auto runId = rig.runsOf (rain).front()->id;
    const auto track = rig.runs.find (runId)->track;
    REQUIRE (rig.runs.find (runId)->range == 0);
    CHECK (rig.runs.find (runId)->warning.empty());

    /*  THE CODE AT 01:00:25:00 ON TICK 800: fifteen seconds in, in the
        second slice. Not a jump: a slice's move stays inside the slice.

        By a locate, the handler seeks it at once, to where the code will be
        a seat's lead after the record's sample - fifteen and a fifth seconds
        in, 25.2 s of the file. By a re-anchor - no locate - the hook that
        measures it, on the next tick, finds it eleven seconds out across a
        slice and asks for the seek itself, to where the code will be the
        launch latency and the seat's lead on: 25.26 s. */
    std::int64_t launchAt = 800 * perTick + seatLeadSamples;
    double offset = 5.2;

    SUBCASE ("by a locate") { rig.heard ("jumped", "25", f0 + 625, 800 * perTick); }

    SUBCASE ("by a re-anchor, the hook's own seek")
    {
        rig.heard ("running", "25", f0 + 625, 800 * perTick);
        launchAt = 801 * perTick + horizonSamples + seatLeadSamples;
        offset = 5.0 + static_cast<double> (launchAt - 800 * perTick) / 48000.0;
        rig.tickOnce();
    }

    rig.tickOnce();

    const auto* sought = rig.runs.find (runId);
    CHECK (rig.countOf (rain) == 1);
    CHECK (sought->range == 1);
    CHECK (sought->warning == "sought");
    CHECK (sought->launchNotBefore == launchAt);
    CHECK (std::count (rig.audio.stops.begin(), rig.audio.stops.end(), track) >= 1);
    CHECK (loopsOn (rig, track).empty());

    const auto arm = armOf (rig, runId);
    REQUIRE (arm.has_value());
    CHECK (arm->startSlot == 1);
    CHECK (arm->sliceOffset == doctest::Approx (offset).epsilon (toTheSample));

    //  Launched again where its offset is for, and sought once.
    rig.ticksUntil (850);
    CHECK (rig.runs.find (runId)->launchedAtSample == launchAt);
    CHECK_FALSE (rig.runs.find (runId)->isFinished());
    CHECK (appliedOf (rig.engine, "run.seek").size() <= 1u);
    CHECK (loopsOn (rig, track).empty());
}

TEST_CASE ("time group chasing: a timeline scene that chases is re-seated around the code's second at a locate - its members sought within it, handed back")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    CodeRig rig;
    const auto storm = rig.make (rig.group, 0, "group", "Storm", { { "mode", "timeline" }, { "at", "01:00:10:00" } });
    rig.set ("/godot/cue/" + storm + "/chase", "jump");
    const auto thunder = rig.sound (storm, 0, "Thunder", "", "thunder.wav");
    const auto rain = rig.sound (storm, 1, "Rain", "", "rain.wav");
    rig.set ("/godot/cue/" + rain + "/preWait", "1.5");
    rig.lengths["thunder.wav"] = 30.0;
    rig.lengths["rain.wav"] = 30.0;

    rig.goOn (rig.group);
    const auto f0 = frameNamed ("01:00:00:00", timecode::Rate::fps25);
    rig.ticksUntil (100);
    rig.heard ("jumped", "25", f0, 100 * perTick);

    //  Four seconds into the scene: both sounding, each on its sample.
    rig.ticksUntil (800);
    REQUIRE (rig.countOf (storm) == 1);
    REQUIRE (rig.countOf (thunder) == 1);
    REQUIRE (rig.countOf (rain) == 1);
    const auto sceneRun = rig.runsOf (storm).front()->id;

    //  A LOCATE to 01:00:30:00: twenty seconds into the scene.
    rig.heard ("jumped", "25", f0 + 750, 800 * perTick);
    rig.tickOnce();

    //  The scene's own run stands; its members are made again under it, at their offsets.
    CHECK (rig.countOf (storm) == 1);
    CHECK_FALSE (rig.runs.find (sceneRun)->isFinished());
    REQUIRE (rig.countOf (thunder) == 2);
    REQUIRE (rig.countOf (rain) == 2);
    CHECK (rig.runsOf (thunder).front()->isFinished());
    CHECK (rig.runsOf (rain).front()->isFinished());

    const auto* newThunder = liveOf (rig, thunder);
    const auto* newRain = liveOf (rig, rain);
    REQUIRE (newThunder != nullptr);
    REQUIRE (newRain != nullptr);
    CHECK (newThunder->parent == sceneRun);
    CHECK (newRain->parent == sceneRun);

    const auto heard = appliedOf (rig.engine, "code.heard").back();
    REQUIRE (heard.args.size() == 8u);
    CHECK (heard.args[6].getString() == newThunder->id);
    CHECK (heard.args[7].getString() == newRain->id);

    const auto thunderArm = armOf (rig, newThunder->id);
    const auto rainArm = armOf (rig, newRain->id);
    REQUIRE (thunderArm.has_value());
    REQUIRE (rainArm.has_value());
    CHECK (thunderArm->startOffset == doctest::Approx (20.2).epsilon (toTheSample));
    CHECK (rainArm->startOffset == doctest::Approx (18.7).epsilon (toTheSample));

    //  Each launched on the sample its offset is for.
    const auto newThunderId = newThunder->id;
    const auto newRainId = newRain->id;
    rig.ticksUntil (850);
    CHECK (rig.runs.find (newThunderId)->launchedAtSample == 800 * perTick + seatLeadSamples);
    CHECK (rig.runs.find (newRainId)->launchedAtSample == 800 * perTick + seatLeadSamples);
    CHECK (rig.rejected == 0);
}

TEST_CASE ("time group chasing: a group armed while its code runs seats a chaser already past its time at its offset, by code.seat - and leaves a free runner")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    std::string chase = "jump";

    SUBCASE ("a chaser") {}
    SUBCASE ("a free runner") { chase = "free"; }

    CodeRig rig;
    const auto rain = chaser (rig, 0, "Rain", "01:00:10:00", "rain.wav", chase);

    //  The code from tick 100; the group armed at tick 1000, eight seconds into the rain's time.
    const auto f0 = frameNamed ("01:00:00:00", timecode::Rate::fps25);
    rig.ticksUntil (100);
    rig.heard ("jumped", "25", f0, 100 * perTick);
    rig.ticksUntil (1000);
    rig.goOn (rig.group);
    rig.ticksUntil (1050);

    const auto seats = appliedOf (rig.engine, "code.seat");

    if (chase == "free")
    {
        CHECK (seats.empty());
        CHECK (rig.countOf (rain) == 0);
        return;
    }

    REQUIRE (seats.size() == 1u);
    REQUIRE (seats.front().args.size() == 3u);
    const auto sample = seats.front().args[1].getInt64();
    const auto runId = seats.front().args[2].getString();

    REQUIRE (rig.countOf (rain) == 1);
    CHECK (rig.runsOf (rain).front()->id == runId);
    CHECK (rig.runsOf (rain).front()->parent == rig.runs.liveRunOf (rig.group)->id);

    const auto start = 100 * perTick + 480'000;
    const auto arm = armOf (rig, runId);
    REQUIRE (arm.has_value());
    CHECK (arm->startOffset == doctest::Approx (static_cast<double> (sample + seatLeadSamples - start) / 48000.0).epsilon (toTheSample));
    CHECK (rig.runs.find (runId)->launchedAtSample == sample + seatLeadSamples);

    //  Once: the hook asks no more, and nothing else starts it.
    rig.ticksUntil (1300);
    CHECK (appliedOf (rig.engine, "code.seat").size() == 1u);
    CHECK (rig.countOf (rain) == 1);
    CHECK (rig.audio.loops.empty());
}

TEST_CASE ("time group chasing: a replay of a session with locates and a group armed on a running code seats the same runs under the same names")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    CodeRig rig;
    const auto f0 = frameNamed ("01:00:00:00", timecode::Rate::fps25);
    const auto rain = chaser (rig, 0, "Rain", "01:00:10:00", "rain.wav", "jump");
    const auto wind = chaser (rig, 1, "Wind", "01:00:20:00", "wind.wav", "jump");
    const auto bell = chaser (rig, 2, "Bell", "01:00:12:00", "bell.wav", "free");
    const auto storm = rig.make (rig.group, 3, "group", "Storm", { { "mode", "timeline" }, { "at", "01:00:15:00" } });
    rig.set ("/godot/cue/" + storm + "/chase", "jump");
    rig.sound (storm, 0, "Thunder", "", "thunder.wav");
    rig.lengths["thunder.wav"] = 30.0;

    rig.ticksUntil (100);
    rig.heard ("jumped", "25", f0, 100 * perTick);
    rig.ticksUntil (700);
    rig.goOn (rig.group);                                       // armed on a running code: the rain seated
    rig.ticksUntil (800);
    rig.heard ("jumped", "25", f0 + 750, 800 * perTick);         // forward: the wind seated, the scene re-seated
    rig.ticksUntil (900);
    rig.heard ("stopped", "25", f0 + 775, 900 * perTick, 0.0);
    rig.ticksUntil (1000);
    rig.heard ("jumped", "25", f0 + 125, 1000 * perTick);        // back: all of them ended
    rig.ticksUntil (1400);
    REQUIRE (rig.send ("run.stopAll"));
    rig.ticksUntil (1500);

    CHECK (rig.countOf (rain) >= 2);
    CHECK (rig.countOf (wind) >= 1);
    CHECK (rig.countOf (bell) >= 1);
    CHECK (appliedOf (rig.engine, "code.seat").size() == 1u);
    CHECK (rig.rejected == 0);

    const auto show = doc::CanonicalXml::write (rig.document);
    const auto original = LogFile::parse (rig.engine.log().contents());
    REQUIRE (original.errors.empty());

    //  `wfg replay`: no audio side, no picture side, no hooks.
    Engine engine;
    engine.log().openInMemory ({});
    doc::ShowDocument document;
    REQUIRE (doc::CanonicalXml::read (show, document).ok);
    cue::RunTable runs;
    cue::Focus focus;
    doc::IdRegistry runIds = doc::IdRegistry::withSeed (77);
    cue::Runner runner { document, runs, runIds, focus };
    doc::registerDocumentCommands (engine.commands(), document);
    cue::registerCueCommands (engine.commands(), document, focus);
    cue::registerRunCommands (engine.commands(), runs);
    cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);
    runner.setSamplesPerTick (static_cast<int> (perTick));
    runner.setMediaDurations (&rig.lengths);

    const auto result = replay (engine, original);

    for (const auto& mismatch : result.mismatches)
        MESSAGE (mismatch);

    CHECK (result.ok);

    for (const auto& run : rig.runs.all())
    {
        INFO ("run " << run.id << " of " << run.cue);
        const auto* again = runs.find (run.id);
        REQUIRE (again != nullptr);
        CHECK (again->cue == run.cue);
        CHECK (again->state == run.state);
        CHECK (again->parent == run.parent);
        CHECK (again->endedAtTick == run.endedAtTick);
    }

    CHECK (runs.all().size() == rig.runs.all().size());
}

TEST_CASE ("time group chasing: a jump its reader met late is read back - the clock moved to where the reader took it - and the time it lost brings one more jump")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    CodeRig rig;
    const auto rain = chaser (rig, 0, "Rain", "01:00:10:00", "rain.wav", "jump");
    rig.goOn (rig.group);

    const auto f0 = frameNamed ("01:00:00:00", timecode::Rate::fps25);
    const std::int64_t anchor = 100 * perTick;
    rig.ticksUntil (100);
    rig.heard ("jumped", "25", f0, anchor);

    const auto start = anchor + 480'000;
    rig.ticksUntil (start / perTick + 50);
    REQUIRE (rig.countOf (rain) == 1);
    const auto runId = rig.runsOf (rain).front()->id;

    //  The first move will be met 40 ms late: the file reaches the code's second 40 ms of reading later.
    rig.audio.adoptMove = 1;
    rig.audio.adoptLateBy = 0.040;

    rig.ticksUntil (1000);
    rig.heard ("running", "25", f0 + 500, anchor + 960'000 - 1440);
    rig.ticksUntil (1100);

    /*  TWO MOVES: the first, 30 ms; read back late, the member is 40 ms behind
        again, and the second puts it where the code is - the same file over
        reader as the first, placed once the first was reached. */
    REQUIRE (rig.audio.loops.size() == 2u);
    const auto first = rig.audio.loops[0].move;
    const auto second = rig.audio.loops[1].move;
    CHECK (first.fileAt - first.from == doctest::Approx (0.030).epsilon (1e-9));
    CHECK (second.fileAt - second.from == doctest::Approx (0.030).epsilon (1e-9));
    CHECK (second.from >= first.from + static_cast<double> (horizonSamples) / 48000.0);

    const auto* run = rig.runs.find (runId);
    CHECK (run->chaseState == "inStep");
    CHECK (run->position == doctest::Approx (static_cast<double> (1099 * perTick - start + 1440) / 48000.0).epsilon (toTheSample));
}

TEST_CASE ("time group chasing: a chaser seated mid-file and jumped back before where it was seated reads where it is - one jump, its playhead the code's")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    CodeRig rig;
    const auto wind = chaser (rig, 0, "Wind", "01:00:20:00", "wind.wav", "jump");
    rig.goOn (rig.group);

    const auto f0 = frameNamed ("01:00:00:00", timecode::Rate::fps25);
    rig.ticksUntil (100);
    rig.heard ("jumped", "25", f0, 100 * perTick);

    //  Seated 10.2 s in by a locate to 01:00:30:00, and launched there.
    rig.ticksUntil (800);
    rig.heard ("jumped", "25", f0 + 750, 800 * perTick);
    rig.ticksUntil (850);
    REQUIRE (rig.countOf (wind) == 1);
    const auto runId = rig.runsOf (wind).front()->id;
    const auto track = rig.runs.find (runId)->track;
    REQUIRE (rig.runs.find (runId)->launchedAtSample == 800 * perTick + seatLeadSamples);

    //  BACK A SECOND, to 01:00:29:00 at tick 850: nine seconds in - 9.04 a horizon on - before the 10.2 it was seated at.
    rig.heard ("jumped", "25", f0 + 725, 850 * perTick);
    rig.tickOnce();

    REQUIRE (loopsOn (rig, track).size() == 1u);
    CHECK (loopsOn (rig, track).front().move.fileAt == doctest::Approx (9.04).epsilon (toTheSample));

    //  Read from there, not held at the second it was seated at: in step, and no second jump.
    rig.ticksUntil (950);
    CHECK (loopsOn (rig, track).size() == 1u);
    CHECK (rig.runs.find (runId)->chaseState == "inStep");
    CHECK (rig.runs.find (runId)->position
             == doctest::Approx (9.04 + static_cast<double> (949 * perTick - (850 * perTick + horizonSamples)) / 48000.0).epsilon (toTheSample));
    CHECK (rig.countOf (wind) == 1);
}

TEST_CASE ("time group chasing: a group armed before its code is heard is seated by the code.heard that brings the code mid-chaser, and never again by code.seat")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    CodeRig rig;
    const auto rain = chaser (rig, 0, "Rain", "01:00:10:00", "rain.wav", "jump");
    rig.goOn (rig.group);

    //  The code appears at 01:00:16:00: the rain six seconds in, 6.2 a seat's lead on.
    const auto f0 = frameNamed ("01:00:00:00", timecode::Rate::fps25);
    rig.ticksUntil (100);
    rig.heard ("jumped", "25", f0 + 400, 100 * perTick);
    rig.ticksUntil (200);

    CHECK (appliedOf (rig.engine, "code.seat").empty());
    REQUIRE (rig.countOf (rain) == 1);
    const auto runId = rig.runsOf (rain).front()->id;
    CHECK (appliedOf (rig.engine, "code.heard").back().args.size() == 7u);
    CHECK (appliedOf (rig.engine, "code.heard").back().args[6].getString() == runId);

    const auto arm = armOf (rig, runId);
    REQUIRE (arm.has_value());
    CHECK (arm->startOffset == doctest::Approx (6.2).epsilon (toTheSample));
    CHECK (rig.runs.find (runId)->launchedAtSample == 100 * perTick + seatLeadSamples);
    CHECK (rig.audio.loops.empty());
}
