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

/*  A MEDIA CUE THAT PLAYS PART OF ITS FILE, AND THEN ANOTHER PART.

    PRD §3.24: a media cue may carry a list of ranges of its file, each one a
    region with a name and a loop count, and what the cue plays is the list
    rather than the whole recording. This file is the document half of that -
    what a range IS, where it may sit, what it is published as, and the show a
    range makes impossible.

    The audio half - a launcher slot per range, every range clip armed looping,
    and Go.dot placing the boundary between them - is in AudioTests. What a
    range means to the graph is measured; what a range is to the document is
    checked here.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>

#include <set>
#include <string>

using namespace wfg;

namespace
{
    /*  A show with one media cue and however many ranges a case wants.

        It publishes a tree as well as holding a document, because half of what
        a range says about itself is derived: `cue` and `index` are refused
        storage, so the document cannot be asked for them and the tree is where
        they appear. Reading each from where it actually lives is the point -
        a test that read both out of the document would be testing a document
        that had stored what it must not. */
    struct RangeRig
    {
        RangeRig()
        {
            listId = document.createList ("Main").id;
            cueId = document.createCue (listId, 0, "media", "Rain").id;
            memoId = document.createCue (listId, 1, "memo", "House to half").id;
        }

        std::string add (double in, double out, const std::string& onCue = {})
        {
            const auto edit = document.createRange (onCue.empty() ? cueId : onCue, in, out);
            REQUIRE (edit.ok);
            return edit.id;
        }

        /** A stored row, from the document. */
        std::string stored (const std::string& rangeId, const char* name) const
        {
            return document.getAttribute ("/godot/range/" + rangeId + "/" + name)
                     .value_or ("(absent)");
        }

        /** A published node, stored or derived, from the tree. */
        std::string published (const std::string& rangeId, const char* name)
        {
            parameters.markStale();

            tree::EngineState state;
            state.version = "test";

            const auto snapshot = parameters.publish (0, state);
            const auto* node = snapshot->find ("/godot/range/" + rangeId + "/" + name);

            if (node == nullptr || ! node->soleValue().has_value())
                return "(absent)";

            return node->soleValue()->isString()
                     ? node->soleValue()->getString()
                     : std::to_string (node->soleValue()->getInt32());
        }

        Engine engine;
        doc::ShowDocument document;
        tree::MountTable mounts;
        cue::RunTable runs;
        tree::ParameterTree parameters { document, engine.commands(), mounts, runs };

        std::string listId, cueId, memoId;
    };
}

//==============================================================================
TEST_CASE ("range: only a media cue has a file to cut up")
{
    /*  A range is a region of the cue's OWN file (§3.24), so a cue that plays
        nothing cannot hold one. That is the difference between a range and a
        trigger, which every kind carries because every kind can be fired. */
    RangeRig rig;

    CHECK (rig.document.createRange (rig.cueId, 0.0, 4.0).ok);
    CHECK_FALSE (rig.document.createRange (rig.memoId, 0.0, 4.0).ok);
    CHECK_FALSE (rig.document.createRange (rig.listId, 0.0, 4.0).ok);
    CHECK_FALSE (rig.document.createRange ("NOTANID1", 0.0, 4.0).ok);
}

TEST_CASE ("range: one that ends before it begins is refused, and that is all that can be judged here")
{
    /*  THE ONE THING ABOUT A RANGE THAT CAN BE ANSWERED WITHOUT THE FILE. An
        `out` past the end of the media is a question for the arm, which is when
        the file is opened; a show whose media has not been copied onto this
        machine yet still has to open, and refusing it at load would turn a
        missing file into a show nobody can work on. */
    RangeRig rig;

    CHECK_FALSE (rig.document.createRange (rig.cueId, 4.0, 4.0).ok);   // no length
    CHECK_FALSE (rig.document.createRange (rig.cueId, 6.0, 4.0).ok);   // backwards
    CHECK_FALSE (rig.document.createRange (rig.cueId, -1.0, 4.0).ok);  // before the file

    /*  And an `out` beyond any plausible file is NOT refused here,
        deliberately: nothing in the document layer has opened the media. */
    CHECK (rig.document.createRange (rig.cueId, 0.0, 1.0e6).ok);
}

TEST_CASE ("range: it is published at an address of its own, with the cue and the position derived")
{
    /*  The Route and Trigger precedent - flat, so a client watching one range
        watches one node - with the difference that a range HAS a position and
        the position is the playlist. Both `cue` and `index` are read off the
        document rather than stored, so no copy of either can come to disagree
        with where the range actually sits. */
    RangeRig rig;

    const auto first = rig.add (0.0, 4.0);
    const auto second = rig.add (10.0, 12.5);

    CHECK (rig.published (first, "cue") == rig.cueId);
    CHECK (rig.published (second, "cue") == rig.cueId);
    CHECK (rig.published (first, "index") == "0");
    CHECK (rig.published (second, "index") == "1");

    CHECK (rig.stored (first, "in") == "0");
    CHECK (rig.stored (first, "out") == "4");
    CHECK (rig.stored (second, "in") == "10");
    CHECK (rig.stored (second, "out") == "12.5");

    /*  The rows a range gets by default: unnamed, played once. */
    CHECK (rig.published (first, "name").empty());
    CHECK (rig.published (first, "loops") == "1");
}

TEST_CASE ("range: the position is the playlist, and a trigger between two of them does not move it")
{
    /*  A media cue's children are ranges, routes and triggers in whatever order
        they were created. The range index counts RANGES, so adding a trigger
        between the first and the second does not renumber the second - which it
        would if the index were the child position. */
    RangeRig rig;

    const auto first = rig.add (0.0, 4.0);

    REQUIRE (rig.document.createTrigger (rig.cueId, "osc").ok);

    const auto second = rig.add (4.0, 8.0);

    REQUIRE (rig.document.createRoute (rig.cueId, "NOTABUS1").ok);   // a destination, between two ranges

    const auto third = rig.add (8.0, 12.0);

    CHECK (rig.published (first, "index") == "0");
    CHECK (rig.published (second, "index") == "1");
    CHECK (rig.published (third, "index") == "2");
}

TEST_CASE ("range: deleting one renumbers the rest, because the number was never stored")
{
    RangeRig rig;

    const auto first = rig.add (0.0, 4.0);
    const auto second = rig.add (4.0, 8.0);
    const auto third = rig.add (8.0, 12.0);

    REQUIRE (rig.document.remove (first).ok);

    CHECK (rig.published (second, "index") == "0");
    CHECK (rig.published (third, "index") == "1");

    /*  And the address of the range that went is gone with it, rather than
        left behind holding the values it had. */
    CHECK (rig.published (first, "in") == "(absent)");
}

TEST_CASE ("range: a name and a loop count are what a designer writes on one")
{
    RangeRig rig;

    const auto id = rig.add (12.0, 30.0);

    REQUIRE (rig.document.setAttribute ("/godot/range/" + id + "/name", "The approach").ok);
    REQUIRE (rig.document.setAttribute ("/godot/range/" + id + "/loops", "0").ok);

    CHECK (rig.stored (id, "name") == "The approach");
    CHECK (rig.stored (id, "loops") == "0");

    /*  Nought is for ever, which is what an ambience bed is, so it is inside
        the row's range rather than an escape from it. A negative count is not
        a shorter way of saying anything. */
    CHECK_FALSE (rig.document.setAttribute ("/godot/range/" + id + "/loops", "-1").ok);
}

TEST_CASE ("range: `cue` and `index` are read-only, being the containment read back")
{
    RangeRig rig;

    const auto id = rig.add (0.0, 4.0);

    CHECK_FALSE (rig.document.setAttribute ("/godot/range/" + id + "/cue", rig.memoId).ok);
    CHECK_FALSE (rig.document.setAttribute ("/godot/range/" + id + "/index", "7").ok);
}

//==============================================================================
TEST_CASE ("validate: a start offset and a list of ranges are two answers to one question")
{
    /*  `startOffset` says where in the file playback begins; a range says the
        same thing, and says where it ends and how many times. A cue with both
        is a cue whose author was told two different things would happen, and
        the honest answer is to refuse the show rather than to pick one.

        REFUSED WHEN THE SHOW IS READ, like an OSC trigger listening inside
        /godot, because there is no reading of the file under which the cue does
        what both attributes say. */
    RangeRig rig;

    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.cueId + "/startOffset", "2.5").ok);
    CHECK (rig.document.validate().empty());

    rig.add (0.0, 4.0);

    const auto problems = rig.document.validate();

    REQUIRE (problems.size() == 1);
    CHECK (problems[0].find ("startOffset") != std::string::npos);
    CHECK (problems[0].find (rig.cueId) != std::string::npos);

    /*  Nought is the resting value and says nothing, so it does not collide:
        a cue with ranges and an offset nobody set is an ordinary cue. */
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.cueId + "/startOffset", "0").ok);
    CHECK (rig.document.validate().empty());
}

TEST_CASE ("range: it survives a save and a reload, being show state")
{
    /*  §4.10: the document holds what somebody decided. A range is a decision -
        which part of the recording, called what, played how many times - so it
        is in the show file, and `cue` and `index` are not, being facts the
        structure already carries. */
    RangeRig rig;

    const auto id = rig.add (12.0, 30.5);
    REQUIRE (rig.document.setAttribute ("/godot/range/" + id + "/name", "The approach").ok);
    REQUIRE (rig.document.setAttribute ("/godot/range/" + id + "/loops", "3").ok);

    const auto xml = doc::CanonicalXml::write (rig.document);

    CHECK (xml.find ("<Range") != std::string::npos);
    CHECK (xml.find ("The approach") != std::string::npos);
    CHECK (xml.find ("index=") == std::string::npos);

    doc::ShowDocument reopened;
    REQUIRE (doc::CanonicalXml::read (xml, reopened).ok);

    CHECK (reopened.getAttribute ("/godot/range/" + id + "/name").value_or ("?")
             == "The approach");
    CHECK (reopened.getAttribute ("/godot/range/" + id + "/out").value_or ("?") == "30.5");
    CHECK (reopened.getAttribute ("/godot/range/" + id + "/loops").value_or ("?") == "3");
}

//==============================================================================
TEST_CASE ("range.create: the command draws the identifier and the record carries it")
{
    /*  A generated identifier is written INTO the applied record, because a
        replay never draws one of its own - it is handed back the identifier the
        live session used. */
    RangeRig rig;

    doc::registerDocumentCommands (rig.engine.commands(), rig.document);

    rig.engine.log().openInMemory ({});

    REQUIRE (rig.engine.submit (origin::cli, "range.create",
                                { osc::Value::string (rig.cueId),
                                  osc::Value::float64 (1.0),
                                  osc::Value::float64 (5.0) }));

    REQUIRE (rig.engine.processTick (0).applied == 1);

    const auto parsed = LogFile::parse (rig.engine.log().contents());

    REQUIRE (parsed.records.size() == 1);
    REQUIRE (parsed.records[0].kind == LogRecord::Kind::applied);
    REQUIRE (parsed.records[0].args.size() == 4);

    const auto id = parsed.records[0].args[3].getString();

    CHECK (doc::Id::isValid (id));
    CHECK (rig.stored (id, "in") == "1");
    CHECK (rig.stored (id, "out") == "5");
}

//==============================================================================
/*  THE SCHEDULER'S HALF: passes counted, boundaries placed, edits honoured.

    M13 in AudioTests puts a real graph under all of this and reads the answer
    off a render, which is the only way to know a boundary LANDS. What a render
    cannot show cheaply is the rules around it - what a loop count of two does,
    what happens to a `loops` an operator changes while the range plays, what a
    deleted range does - because each of those wants a run several seconds long
    and a rendered second costs a second.

    So they are asked here, with a Player that is a record of what it was told
    and a sample counter the test advances. Nothing sounds; everything is
    decided.
*/
namespace
{
    /*  The audio side as a notebook. Its sample counter moves when the rig
        ticks, which is what lets a case put a boundary four seconds away and
        reach it in four hundred ticks rather than in four seconds. */
    struct NotePlayer final : cue::Player
    {
        int trackCount() const override              { return 2; }
        int slotCount() const override               { return 8; }
        int blockSize() const override               { return 128; }
        int channelsPerTrack() const override        { return 1; }
        int sampleRate() const override              { return 48000; }
        std::int64_t samplesElapsed() const override { return samples; }

        void requestArm (const cue::ArmRequest& request) override
        {
            arms.push_back (request);
        }

        bool launchAtSample (int track, int slot, std::int64_t sample) override
        {
            launches.push_back ({ track, slot, sample });
            playing.insert (track);
            return true;
        }

        bool stop (int track) override
        {
            playing.erase (track);
            return true;
        }

        bool stopAtSample (int track, int slot, std::int64_t sample) override
        {
            stops.push_back ({ track, slot, sample });
            return true;
        }

        void setLevelDb (int, double) override {}
        bool isPlaying (int track) const override  { return playing.count (track) > 0; }
        bool isArmReady (int) const override       { return armsReady; }

        /** The disk answers: every outstanding arm is reported as ready. */
        void completeArms (Engine& engine)
        {
            for (const auto& arm : arms)
                engine.submit (origin::engine, "audio.armed",
                               { osc::Value::string (arm.runId),
                                 osc::Value::int32 (arm.track) });

            arms.clear();
            armsReady = true;
        }

        struct Placed
        {
            int track = 0;
            int slot = 0;
            std::int64_t sample = 0;
        };

        std::int64_t samples = 0;
        bool armsReady = false;

        std::vector<cue::ArmRequest> arms;
        std::vector<Placed> launches;
        std::vector<Placed> stops;
        std::set<int> playing;
    };

    /*  A show with one media cue and however many ranges a case wants, plus the
        loop that turns ticks into samples. */
    struct SchedulerRig
    {
        SchedulerRig()
        {
            engine.log().openInMemory ({});

            doc::registerDocumentCommands (engine.commands(), document);
            cue::registerCueCommands (engine.commands(), document, focus);
            cue::registerRunCommands (engine.commands(), runs);
            cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);

            runner.setPlayer (&audio);
            runner.setSamplesPerTick (960);          // 48 kHz at 50 Hz

            listId = document.createList ("Sound").id;
            cueId = document.createCue (listId, 0, "media", "Bed").id;

            document.setAttribute ("/godot/cue/" + cueId + "/file", "night.wav");
            document.setAttribute (cue::standbyAddressOf (listId), cueId);
        }

        std::string addRange (double in, double out, int loops = 1)
        {
            const auto edit = document.createRange (cueId, in, out);
            REQUIRE (edit.ok);

            REQUIRE (document.setAttribute ("/godot/range/" + edit.id + "/loops",
                                            std::to_string (loops)).ok);
            return edit.id;
        }

        /** One tick, with the sample counter moving as an audio device would. */
        Engine::TickResult tickOnce()
        {
            runner.beforeTick (engine, tick);
            const auto result = engine.processTick (tick++);
            audio.samples += 960;
            return result;
        }

        Engine::TickResult submitAndTick (const std::string& name,
                                          std::vector<osc::Value> args = {})
        {
            REQUIRE (engine.submit (origin::cli, name, std::move (args)));
            return tickOnce();
        }

        /** GO, then the disk answers, then it is launched and in range nought. */
        std::string goAndLaunch()
        {
            submitAndTick ("go");
            audio.completeArms (engine);

            for (int i = 0; i < 20; ++i)
                tickOnce();

            REQUIRE_FALSE (runs.all().empty());
            return runs.all().front().id;
        }

        void ticks (int howMany)
        {
            for (int i = 0; i < howMany; ++i)
                tickOnce();
        }

        const cue::Run* run (const std::string& id) const { return runs.find (id); }

        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::Focus focus;
        doc::IdRegistry runIds = doc::IdRegistry::withSeed (23);
        cue::Runner runner { document, runs, runIds, focus };
        NotePlayer audio;
        std::int64_t tick = 0;

        std::string listId, cueId;
    };

    /** Everything the run's first range was told to do, as seconds. */
    double secondsOf (std::int64_t samples)  { return static_cast<double> (samples) / 48000.0; }
}

TEST_CASE ("range scheduler: the launch enters range nought and says so")
{
    SchedulerRig rig;
    rig.addRange (0.0, 1.0);
    rig.addRange (1.0, 2.0);

    const auto id = rig.goAndLaunch();

    const auto* run = rig.run (id);
    REQUIRE (run != nullptr);

    CHECK (run->range == 0);
    CHECK (run->rangeIteration >= 1);

    /*  Out of slot nought, which is where range nought was armed. */
    REQUIRE_FALSE (rig.audio.launches.empty());
    CHECK (rig.audio.launches.front().slot == 0);

    /*  And it holds a voice, which is what the arm that carried the ranges into
        the slots did for it. */
    CHECK (run->track >= 0);
}

TEST_CASE ("range scheduler: a loop count of two places the boundary after two passes")
{
    /*  §3.24's `loops`, and the thing that makes it more than a document row:
        the boundary out of a range is placed at the end of the LAST pass, not
        of the first. A count read as one would cut every looped range short,
        and a count ignored would never leave one at all. */
    SchedulerRig rig;
    rig.addRange (0.0, 1.0, 2);            // two passes of one second
    rig.addRange (1.0, 2.0, 1);

    const auto id = rig.goAndLaunch();
    const auto* run = rig.run (id);
    REQUIRE (run != nullptr);

    const auto launchedAt = rig.audio.launches.front().sample;

    /*  Not yet at one second: the first pass has ended and the range has not. */
    rig.ticks (60);
    CHECK (rig.run (id)->range == 0);

    rig.ticks (80);

    /*  Two seconds in, the boundary has been placed and the second range
        entered. The FIRST stop is the one this case is about - by now the
        second range's own end may have been placed too, which is the playlist
        doing its job and not this measurement. */
    REQUIRE (rig.run (id)->range >= 1);
    REQUIRE_FALSE (rig.audio.stops.empty());

    const auto boundary = rig.audio.stops.front().sample;

    INFO ("launched at " << launchedAt << ", boundary at " << boundary
           << " (" << secondsOf (boundary - launchedAt) << " s)");

    /*  TWO SECONDS AFTER THE LAUNCH, which is two passes of a one-second range.
        Exactly, because the boundary is arithmetic on the launch instant rather
        than on whichever tick noticed it. */
    CHECK (boundary - launchedAt == 2 * 48000);

    /*  The stop is on the slot that was playing and the play on the next, both
        at the same instant. */
    CHECK (rig.audio.stops.front().slot == 0);
    REQUIRE (rig.audio.launches.size() >= 2u);
    CHECK (rig.audio.launches[1].slot == 1);
    CHECK (rig.audio.launches[1].sample == boundary);
}

TEST_CASE ("range scheduler: a loops changed while the range plays is honoured from the next boundary")
{
    /*  DECISION L, edit-at-next-iteration (author, 2026-09-06). A running
        ranged cue does not copy its range list at launch: at every boundary it
        re-reads it. So an operator who decides during the show that the bed
        should turn after three passes rather than after ten gets three - and
        gets it without the cue stopping.

        What is NOT re-read is the pass length of the range playing now: that is
        what the clip was armed with, and changing it needs the message thread. */
    SchedulerRig rig;
    const auto bed = rig.addRange (0.0, 1.0, 10);      // ten passes, to begin with
    rig.addRange (1.0, 2.0, 1);

    const auto id = rig.goAndLaunch();
    const auto launchedAt = rig.audio.launches.front().sample;

    rig.ticks (100);                                     // two seconds in
    REQUIRE (rig.run (id)->range == 0);

    /*  The operator shortens it while it plays. */
    REQUIRE (rig.document.setAttribute ("/godot/range/" + bed + "/loops", "3").ok);

    rig.ticks (100);

    REQUIRE (rig.run (id)->range >= 1);
    REQUIRE_FALSE (rig.audio.stops.empty());

    /*  THREE PASSES, not ten. The count the range was launched with was never
        copied anywhere, so the edit is what the boundary is computed from. */
    CHECK (rig.audio.stops.front().sample - launchedAt == 3 * 48000);
}

TEST_CASE ("range scheduler: an advance leaves at the end of the pass it is on")
{
    /*  A range that loops for ever - which is what an ambience bed is - and the
        only way out of one. The boundary is at the end of the pass PLAYING, not
        at the moment of the asking, which is the whole difference between this
        verb and a stop. */
    SchedulerRig rig;
    rig.addRange (0.0, 1.0, 0);            // for ever
    rig.addRange (1.0, 2.0, 1);

    const auto id = rig.goAndLaunch();
    const auto launchedAt = rig.audio.launches.front().sample;

    /*  Four seconds of a range that a loop count of one would have ended after
        one. Nothing has been placed, because nothing has asked. */
    rig.ticks (200);
    CHECK (rig.run (id)->range == 0);
    CHECK (rig.audio.stops.empty());
    CHECK (rig.run (id)->rangeIteration >= 4);

    /*  The advance, asked partway through a pass. */
    rig.submitAndTick ("run.advance", { osc::Value::string (id) });
    rig.ticks (60);

    REQUIRE (rig.audio.stops.size() == 1u);

    const auto boundary = rig.audio.stops.front().sample;
    const auto after = boundary - launchedAt;

    INFO ("the advance was asked at about four seconds; the boundary is at "
           << secondsOf (after) << " s");

    /*  A WHOLE NUMBER OF PASSES after the launch, and the next one after the
        asking. Not four and a bit seconds, which is where a stop would have
        landed. */
    CHECK (after % 48000 == 0);
    CHECK (after == 5 * 48000);

    CHECK (rig.run (id)->range == 1);
}

TEST_CASE ("range scheduler: a stop cue whose verb is advance does the same thing")
{
    /*  §3.24's verb reached the way a show reaches it: a cue in the list, fired
        by GO like any other, rather than a command a client sends. */
    SchedulerRig rig;
    rig.addRange (0.0, 1.0, 0);
    rig.addRange (1.0, 2.0, 1);

    const auto mover = rig.document.createCue (rig.listId, 1, "stop", "Move it on").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + mover + "/target", rig.cueId).ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + mover + "/verb", "advance").ok);

    const auto id = rig.goAndLaunch();

    rig.ticks (150);
    REQUIRE (rig.audio.stops.empty());

    rig.submitAndTick ("cue.fire", { osc::Value::string (mover) });
    rig.ticks (60);

    REQUIRE (rig.audio.stops.size() == 1u);
    CHECK (rig.run (id)->range == 1);
}

TEST_CASE ("range scheduler: the last range's end is placed with nothing after it")
{
    /*  The end of a playlist is a stop and no play, and the run ends there
        rather than at the boundary before it - which is what `rangesFinished`
        is for. */
    SchedulerRig rig;
    rig.addRange (0.0, 1.0, 1);
    rig.addRange (1.0, 2.0, 1);

    const auto id = rig.goAndLaunch();

    rig.ticks (120);
    REQUIRE (rig.run (id)->range == 1);

    /*  Its own second passes, and the last boundary is placed. */
    rig.ticks (80);

    REQUIRE (rig.audio.stops.size() == 2u);
    CHECK (rig.audio.stops.back().slot == 1);

    /*  TWO LAUNCHES AND TWO STOPS, not three launches: there is no third range
        to enter. */
    CHECK (rig.audio.launches.size() == 2u);

    const auto* run = rig.run (id);
    REQUIRE (run != nullptr);
    CHECK (run->rangesFinished);
}

TEST_CASE ("range scheduler: a range deleted while it plays is not entered again")
{
    /*  Decision L's other half. The list is re-read at every boundary, so a
        range removed during the show is simply not there when the boundary
        looks - and the range playing when it happened finishes its passes,
        because that one is a clip in a slot rather than a row in a list. */
    SchedulerRig rig;
    rig.addRange (0.0, 1.0, 1);
    const auto second = rig.addRange (1.0, 2.0, 1);
    rig.addRange (2.0, 3.0, 1);

    const auto id = rig.goAndLaunch();

    rig.ticks (30);

    /*  Deleted before the first boundary, so the cue's list is two ranges long
        when the scheduler next looks. */
    REQUIRE (rig.document.remove (second).ok);

    rig.ticks (100);

    /*  It went into range one - which is now the THIRD region, because deleting
        the second renumbered it. What matters is that the playlist is two long
        and the run did not try to enter a range that is not there. */
    const auto* run = rig.run (id);
    REQUIRE (run != nullptr);
    CHECK (run->range == 1);

    rig.ticks (100);

    CHECK (rig.run (id)->rangesFinished);
    CHECK (rig.audio.launches.size() == 2u);
}

TEST_CASE ("range scheduler: a cue with no ranges is never in one")
{
    /*  Every cue Phase 2 knew about, and most cues still. `range` stays -1 and
        the hook does nothing for it, which is what keeps the ordinary media cue
        exactly as cheap as it was. */
    SchedulerRig rig;

    const auto id = rig.goAndLaunch();

    rig.ticks (100);

    const auto* run = rig.run (id);
    REQUIRE (run != nullptr);

    CHECK (run->range == -1);
    CHECK (run->rangeIteration == 0);
    CHECK (rig.audio.stops.empty());
    CHECK (rig.audio.launches.size() == 1u);
}
