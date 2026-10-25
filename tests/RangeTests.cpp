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

#include <cmath>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

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

TEST_CASE ("range: a split cuts one in two, in place and in one edit")
{
    /*  The author, 2026-09-21: *"even if the ranges amount to the full file,
        pressing the [+] range button will split the range where the cursor is.
        No split if the cursor is already on a cut or either the start or end
        of the file."*

        ONE COMMAND AND NOT THREE. A split assembled in the window - create the
        other half, shorten this one, move the new one into place - would be
        three undo steps for one gesture and would leave the document briefly
        holding two ranges claiming the same seconds. */
    RangeRig rig;

    const auto whole = rig.add (0.0, 30.0);
    const auto second = rig.document.splitRange (rig.cueId, 12.0);

    REQUIRE (second.ok);
    CHECK (second.id != whole);

    //  The first keeps its identity and stops where the cut is.
    CHECK (rig.stored (whole, "in") == "0");
    CHECK (rig.stored (whole, "out") == "12");

    //  And the second half picks up where it left off.
    CHECK (rig.stored (second.id, "in") == "12");
    CHECK (rig.stored (second.id, "out") == "30");

    /*  IN PLACE, which is the part that needs the engine: document order is
        playlist order (§3.24), so the new half sits directly after the one it
        was cut from rather than at the end of the list. */
    CHECK (rig.published (whole, "index") == "0");
    CHECK (rig.published (second.id, "index") == "1");

    //  Cut the FIRST half again, and the third piece lands between them.
    const auto middle = rig.document.splitRange (rig.cueId, 6.0);

    REQUIRE (middle.ok);
    CHECK (rig.published (whole, "index") == "0");
    CHECK (rig.published (middle.id, "index") == "1");
    CHECK (rig.published (second.id, "index") == "2");
    CHECK (rig.stored (whole, "out") == "6");
    CHECK (rig.stored (middle.id, "in") == "6");
    CHECK (rig.stored (middle.id, "out") == "12");
}

TEST_CASE ("range: there is nothing to divide on a cut, at the top of the file or at its end")
{
    RangeRig rig;

    const auto whole = rig.add (0.0, 30.0);

    //  The two ends of the material: a cut there would make a region of no length.
    CHECK_FALSE (rig.document.splitRange (rig.cueId, 0.0).ok);
    CHECK_FALSE (rig.document.splitRange (rig.cueId, 30.0).ok);

    //  Past it entirely, which is not inside any range either.
    CHECK_FALSE (rig.document.splitRange (rig.cueId, 44.0).ok);

    REQUIRE (rig.document.splitRange (rig.cueId, 12.0).ok);

    //  And now 12 is a cut, so cutting there again does nothing.
    const auto again = rig.document.splitRange (rig.cueId, 12.0);
    CHECK_FALSE (again.ok);
    CHECK (again.reason == reason::badValue);

    //  Within a millisecond of it is on it: a hand on a bar does not land on the sample.
    CHECK_FALSE (rig.document.splitRange (rig.cueId, 12.0004).ok);

    //  Only a media cue has a file to cut up, as with `createRange`.
    CHECK_FALSE (rig.document.splitRange (rig.memoId, 1.0).ok);
    CHECK_FALSE (rig.document.splitRange ("NOTANID1", 1.0).ok);

    //  Nothing was made by any of the refusals: still the two the split gave.
    CHECK (rig.stored (whole, "out") == "12");
}

TEST_CASE ("range: cutting a looping slice gives two that each go round")
{
    /*  A designer slicing a bed that loops for ever means two pieces that each
        loop, not one that quietly stops. The half that is made inherits the
        loop count of the half it came from. */
    RangeRig rig;

    const auto bed = rig.add (0.0, 30.0);
    REQUIRE (rig.document.setAttribute ("/godot/range/" + bed + "/loops", "0").ok);

    const auto half = rig.document.splitRange (rig.cueId, 10.0);

    REQUIRE (half.ok);
    CHECK (rig.stored (bed, "loops") == "0");
    CHECK (rig.stored (half.id, "loops") == "0");
}

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

    /*  No door makes one any more (namespace draft §81, ATZ): the first range
        took the offset in. Put there by hand, as a file could hold it. */
    CHECK (rig.document.getAttribute ("/godot/cue/" + rig.cueId + "/startOffset").value_or ("?") == "0");
    rig.document.findById (rig.cueId).setProperty ("startOffset", 2.5, nullptr);

    const auto problems = rig.document.validate();

    REQUIRE (problems.size() == 1);
    CHECK (problems[0].find ("startOffset") != std::string::npos);
    CHECK (problems[0].find (rig.cueId) != std::string::npos);

    /*  Nought is the resting value and says nothing, so it does not collide:
        a cue with ranges and an offset nobody set is an ordinary cue. */
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.cueId + "/startOffset", "0").ok);
    CHECK (rig.document.validate().empty());
}

TEST_CASE ("start offset: the first range takes it in, a write beside ranges is refused, and a show saved with both opens without it")
{
    /*  THE BUG §81 FOUND (ATZ): the table's + made a cue's first range from
        nought and left the offset, and the reader refuses a show with both -
        so a show saved after one press of + would not open again. */
    RangeRig rig;
    const auto offset = "/godot/cue/" + rig.cueId + "/startOffset";

    REQUIRE (rig.document.setAttribute (offset, "2.5").ok);
    const auto id = rig.add (2.5, 30.0);

    CHECK (rig.document.getAttribute (offset).value_or ("?") == "0");
    CHECK (rig.stored (id, "in") == "2.5");
    CHECK (rig.document.validate().empty());

    //  Beside ranges an offset is refused; nought is always taken.
    CHECK_FALSE (rig.document.setAttribute (offset, "4").ok);
    CHECK (rig.document.setAttribute (offset, "0").ok);

    //  A morning's show, saved with both: it opens, playing as it did.
    auto xml = doc::CanonicalXml::write (rig.document);
    const auto at = xml.find ("<Media ");
    REQUIRE (at != std::string::npos);
    xml.insert (at + 7, "startOffset=\"7\" ");

    doc::ShowDocument reopened;
    REQUIRE (doc::CanonicalXml::read (xml, reopened).ok);
    CHECK (reopened.getAttribute (offset).value_or ("0") == "0");
    CHECK (reopened.getAttribute ("/godot/range/" + id + "/in").value_or ("?") == "2.5");
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
        void setRouting (int, const std::vector<cue::Coefficient>&) override {}
        bool isPlaying (int track) const override  { return playing.count (track) > 0; }
        bool isArmReady (int) const override       { return armsReady; }

        /*  A slice's points moved under it (namespace draft §33), written down
            and numbered from one, as the real player numbers them. */
        std::uint64_t placeLoop (int track, int slot, const LoopMove& move) override
        {
            moves.push_back ({ track, slot, move });
            return moves.size();
        }

        std::optional<LoopTaken> loopTaken (int, int) override { return taken; }

        struct Moved
        {
            int track = 0;
            int slot = 0;
            LoopMove move;
        };

        std::vector<Moved> moves;
        std::optional<LoopTaken> taken;

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

TEST_CASE ("range scheduler: Esc, panic and the cross stop a cue whose range loops for ever, and nothing comes back")
{
    /*  Found by the author on 2026-09-25: a media cue with one range looping
        for ever (`loops` nought, an ambience bed) could not be stopped - the
        sound went, but the run stayed `stopping`, holding its voice and
        sitting on the running pane, because silence at a range boundary is
        not an ending and a range that never finishes never has a last
        boundary. And a cue with a range still to come would have had it
        launched after the stop. §4.4: Esc stops. */
    for (const auto* command : { "run.kill", "run.killAll", "run.stopAll" })
    {
        CAPTURE (command);

        SUBCASE ("one range, looping for ever")
        {
            SchedulerRig rig;
            rig.addRange (0.0, 1.0, 0);

            const auto id = rig.goAndLaunch();
            rig.ticks (120);
            REQUIRE_FALSE (rig.run (id)->isFinished());

            if (std::string (command) == "run.kill")
                rig.submitAndTick (command, { osc::Value::string (id) });
            else
                rig.submitAndTick (command);

            /*  Esc fades for the show's panic fade first - a second, unless the
                show says otherwise (2026-09-28) - and the two kills cut. */
            rig.ticks (std::string (command) == "run.stopAll" ? 60 : 5);
            CHECK (rig.run (id)->isFinished());
            CHECK (rig.audio.playing.empty());
        }

        SUBCASE ("two ranges, stopped before the boundary into the second")
        {
            SchedulerRig rig;
            rig.addRange (0.0, 2.0, 1);
            rig.addRange (2.0, 4.0, 1);

            const auto id = rig.goAndLaunch();
            const auto launched = rig.audio.launches.size();

            if (std::string (command) == "run.kill")
                rig.submitAndTick (command, { osc::Value::string (id) });
            else
                rig.submitAndTick (command);

            //  Past where the boundary would have been: nothing is launched.
            rig.ticks (150);
            CHECK (rig.audio.launches.size() == launched);
            CHECK (rig.run (id)->isFinished());
            CHECK (rig.audio.playing.empty());
        }
    }
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

    const auto mover = rig.document.createCue (rig.listId, 1, "transport", "Move it on").id;
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

TEST_CASE ("range scheduler: a transport cue can name the slice to go to, not only the next")
{
    /*  The author, 2026-09-22: an advance should be able to name a slice. On a
        bed whose first range loops for ever, stepping to the fourth means
        three more passes of whatever lies between; naming it is the difference
        between a gesture and a wait.

        THE PASS IT IS ON STILL FINISHES. That is what makes an advance
        graceful and it is the same boundary either way - only where it lands
        differs. */
    SchedulerRig rig;
    rig.addRange (0.0, 1.0, 0);             // the bed, looping for ever
    rig.addRange (1.0, 2.0, 1);
    const auto outro = rig.addRange (2.0, 3.0, 1);

    const auto mover = rig.document.createCue (rig.listId, 1, "transport", "To the outro").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + mover + "/target", rig.cueId).ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + mover + "/verb", "advance").ok);

    SUBCASE ("named, it goes there and not to the next one")
    {
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + mover + "/range", outro).ok);

        const auto id = rig.goAndLaunch();
        rig.ticks (150);
        REQUIRE (rig.audio.stops.empty());

        rig.submitAndTick ("cue.fire", { osc::Value::string (mover) });
        rig.ticks (60);

        REQUIRE (rig.audio.stops.size() == 1u);
        CHECK (rig.run (id)->range == 2);

        //  And it launched the slice it named, not the one in between.
        REQUIRE (rig.audio.launches.size() == 2u);
        CHECK (rig.audio.launches.back().slot == 2);
    }

    SUBCASE ("naming nothing is the next one, as it always was")
    {
        const auto id = rig.goAndLaunch();
        rig.ticks (150);

        rig.submitAndTick ("cue.fire", { osc::Value::string (mover) });
        rig.ticks (60);

        CHECK (rig.run (id)->range == 1);
    }

    SUBCASE ("naming a slice of some other cue falls back rather than doing nothing")
    {
        /*  A reference somebody has to fix - `wfg validate` says so through
            the `refers` column - and the run still has somewhere to go. */
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + mover + "/range", "NOSUCHRG").ok);

        const auto id = rig.goAndLaunch();
        rig.ticks (150);

        rig.submitAndTick ("cue.fire", { osc::Value::string (mover) });
        rig.ticks (60);

        CHECK (rig.run (id)->range == 1);
    }

    SUBCASE ("and naming the slice it is already on is the next one")
    {
        /*  A request with no boundary in it. Answering it where it stands
            would be an advance that advanced nothing. */
        const auto first = rig.document.findById (rig.cueId)
                              .getChild (0)[juce::Identifier ("id")].toString().toStdString();

        REQUIRE_FALSE (first.empty());
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + mover + "/range", first).ok);

        const auto id = rig.goAndLaunch();
        rig.ticks (150);

        rig.submitAndTick ("cue.fire", { osc::Value::string (mover) });
        rig.ticks (60);

        CHECK (rig.run (id)->range == 1);
    }
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

TEST_CASE ("go.doh: an advance the GO asked for is withdrawn while its boundary is unplaced")
{
    /*  Doh! D3 (2026-10-03, PRD §3.32, namespace draft §24.13; the design's test
        20): a transport cue's advance on a slice that loops for ever, GO'd and
        Doh'd before the scheduler has placed its boundary - the request as it
        was before the GO, so the slice goes on looping. Run on the engine
        sources of 5fd0e76 with this case, it failed: the boundary was placed
        and the next slice entered. */
    SchedulerRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/list/goDebounce", "0").ok);
    rig.addRange (0.0, 1.0, 0);
    rig.addRange (1.0, 2.0, 1);

    const auto mover = rig.document.createCue (rig.listId, 1, "transport", "Move it on").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + mover + "/target", rig.cueId).ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + mover + "/verb", "advance").ok);

    const auto id = rig.goAndLaunch();
    rig.ticks (150);
    REQUIRE (rig.audio.stops.empty());

    REQUIRE (rig.engine.submit (origin::cli, "go", {}));
    REQUIRE (rig.engine.submit (origin::cli, "go.doh", {}));
    CHECK (rig.tickOnce().rejected == 0);

    CHECK_FALSE (rig.run (id)->advanceRequested);

    rig.ticks (120);
    CHECK (rig.audio.stops.empty());
    CHECK (rig.run (id)->range == 0);
}

//==============================================================================
/*  A SLICE MOVED WHILE IT SOUNDS (namespace draft §33, the author's ruling of
    2026-10-06: "immediately"). An in or out edited under a sounding slice is
    placed on its slot a launch horizon ahead, and the run's clock moves with
    it - so the playhead, the pass count and the boundary say what the voice
    plays. The render half is in AudioTests; this is the arithmetic.
*/
namespace
{
    /*  Where the run's clock says the voice is, `ticksAgo` ticks before the
        last one ran: the sample counter it read is the one before the
        tick's increment. */
    double secondsSinceLaunch (const SchedulerRig& rig, std::int64_t launchedAt)
    {
        return secondsOf (rig.audio.samples - 960 - launchedAt);
    }
}

TEST_CASE ("slice move: an out pulled in ahead of the playhead is placed at once, and the playhead wraps at it")
{
    SchedulerRig rig;
    const auto bed = rig.addRange (0.0, 2.0, 0);            // a bed that loops for ever

    const auto id = rig.goAndLaunch();
    const auto launchedAt = rig.audio.launches.front().sample;

    rig.ticks (50);                                         // about a second in
    REQUIRE (rig.run (id)->range == 0);
    REQUIRE (rig.audio.moves.empty());

    REQUIRE (rig.document.setAttribute ("/godot/range/" + bed + "/out", "1.5").ok);
    rig.tickOnce();

    /*  ONE MOVE, on the sounding slot, and not a jump: the file is still
        inside the new loop, so it carries on where it is. */
    REQUIRE (rig.audio.moves.size() == 1u);

    const auto& moved = rig.audio.moves.front();
    CHECK (moved.track == rig.run (id)->track);
    CHECK (moved.slot == 0);
    CHECK (moved.move.loopIn == doctest::Approx (0.0));
    CHECK (moved.move.loopOut == doctest::Approx (1.5));
    CHECK (moved.move.crossfade == doctest::Approx (0.0));
    CHECK (moved.move.fileAt == doctest::Approx (moved.move.from));
    CHECK (moved.move.from > 1.0);
    CHECK (moved.move.from < 1.5);

    /*  Past the new out, the playhead is back at the in-point and counting:
        the second of the file is the time since the launch less one pass of
        the new loop. */
    rig.ticks (40);

    const auto t = secondsSinceLaunch (rig, launchedAt);
    REQUIRE (t > 1.5);
    CHECK (rig.run (id)->position == doctest::Approx (t - 1.5).epsilon (1.0e-4));
    CHECK (rig.run (id)->rangeIteration == 2);

    /*  The same edit read again places nothing more. */
    rig.ticks (10);
    CHECK (rig.audio.moves.size() == 1u);
}

TEST_CASE ("slice move: an out pulled in behind the playhead jumps to the in-point, faded")
{
    SchedulerRig rig;
    const auto bed = rig.addRange (0.0, 2.0, 0);

    const auto id = rig.goAndLaunch();
    const auto launchedAt = rig.audio.launches.front().sample;

    rig.ticks (60);                                         // about 1.2 s into a 2 s loop

    REQUIRE (rig.document.setAttribute ("/godot/range/" + bed + "/out", "1.0").ok);
    rig.tickOnce();

    REQUIRE (rig.audio.moves.size() == 1u);

    const auto& moved = rig.audio.moves.front();
    CHECK (moved.move.fileAt == doctest::Approx (0.0));    // the new in-point
    CHECK (moved.move.loopOut == doctest::Approx (1.0));
    CHECK (moved.move.crossfade == doctest::Approx (0.010));

    /*  A jump begins a pass: the one it left was the first. */
    rig.ticks (10);
    CHECK (rig.run (id)->rangeIteration == 2);

    /*  And the playhead counts the new loop from where the jump landed. */
    const auto landedAt = moved.move.from;                  // seconds since the launch, at speed one
    const auto t = secondsSinceLaunch (rig, launchedAt);
    CHECK (rig.run (id)->position == doctest::Approx (std::fmod (t - landedAt, 1.0)).epsilon (1.0e-4));
}

TEST_CASE ("slice move: the passes played before a move still count towards its loops")
{
    /*  Three passes of the first second; at one and a half seconds - the
        second pass, half a second in - its out is pulled in to 0.8. One pass
        is behind it and two are owed: the rest of this one, to 0.8, and one
        whole pass of 0.8 after it. The boundary is at 1.8 + 0.8 seconds. */
    SchedulerRig rig;
    const auto bed = rig.addRange (0.0, 1.0, 3);
    rig.addRange (1.0, 2.0, 1);

    rig.goAndLaunch();
    const auto launchedAt = rig.audio.launches.front().sample;

    rig.ticks (55);

    REQUIRE (rig.document.setAttribute ("/godot/range/" + bed + "/out", "0.8").ok);
    rig.ticks (120);

    REQUIRE (rig.audio.moves.size() >= 1u);
    CHECK (rig.audio.moves.front().move.crossfade == doctest::Approx (0.0));
    REQUIRE_FALSE (rig.audio.stops.empty());

    INFO ("boundary at " << secondsOf (rig.audio.stops.front().sample - launchedAt) << " s");
    CHECK (std::llabs (rig.audio.stops.front().sample - launchedAt - 124800) <= 1);
}

TEST_CASE ("slice move: a slice not yet entered takes its new points when it is")
{
    SchedulerRig rig;
    rig.addRange (0.0, 1.0, 1);
    const auto second = rig.addRange (1.0, 2.0, 1);

    const auto id = rig.goAndLaunch();

    rig.ticks (10);

    REQUIRE (rig.document.setAttribute ("/godot/range/" + second + "/out", "3.0").ok);
    REQUIRE (rig.document.setAttribute ("/godot/range/" + second + "/in", "2.5").ok);

    /*  Nothing moves under the slice sounding: it is not the one edited. */
    rig.ticks (5);
    CHECK (rig.audio.moves.empty());

    rig.ticks (60);
    REQUIRE (rig.run (id)->range == 1);

    /*  At the boundary, the second slot is told before its launch: from its
        clip's first frame, the file is at the new in-point. */
    REQUIRE (rig.audio.moves.size() == 1u);

    const auto& moved = rig.audio.moves.front();
    CHECK (moved.slot == 1);
    CHECK (moved.move.from == doctest::Approx (1.0));
    CHECK (moved.move.fileAt == doctest::Approx (2.5));
    CHECK (moved.move.loopIn == doctest::Approx (2.5));
    CHECK (moved.move.loopOut == doctest::Approx (3.0));

    rig.ticks (5);
    CHECK (rig.run (id)->position >= 2.5);
    CHECK (rig.run (id)->position < 3.0);
}

TEST_CASE ("slice move: deleting the slice that sounds moves nothing - it finishes its pass")
{
    SchedulerRig rig;
    const auto bed = rig.addRange (0.0, 2.0, 1);
    rig.addRange (2.0, 3.0, 1);

    rig.goAndLaunch();
    rig.ticks (20);

    /*  The list is now the second range alone, at the first place. Read by
        its place, the sounding slot would have been moved onto it. */
    REQUIRE (rig.document.remove (bed).ok);
    rig.ticks (10);

    CHECK (rig.audio.moves.empty());
}

TEST_CASE ("slice move: a cue armed at standby is armed again once its edits stop")
{
    SchedulerRig rig;
    const auto bed = rig.addRange (0.0, 2.0, 0);

    rig.ticks (5);
    REQUIRE (rig.audio.arms.size() == 1u);
    rig.audio.completeArms (rig.engine);
    rig.ticks (2);

    /*  A drag: a write a tick for a while. No arm while it goes on. */
    for (const auto* out : { "1.9", "1.8", "1.7", "1.6", "1.5" })
    {
        REQUIRE (rig.document.setAttribute ("/godot/range/" + bed + "/out", out).ok);
        rig.tickOnce();
    }

    CHECK (rig.audio.arms.empty());

    /*  A fifth of a second after the last, one arm with where it ended. */
    rig.ticks (12);

    REQUIRE (rig.audio.arms.size() == 1u);
    REQUIRE (rig.audio.arms.front().ranges.size() == 1u);
    CHECK (rig.audio.arms.front().ranges.front().out == doctest::Approx (1.5));

    /*  And the run waits on that arm before a GO can launch it. */
    rig.ticks (12);
    CHECK (rig.audio.arms.size() == 1u);
}

TEST_CASE ("slice move: a move the reader met late puts the clock where the reader applied it")
{
    SchedulerRig rig;
    const auto bed = rig.addRange (0.0, 2.0, 0);

    const auto id = rig.goAndLaunch();
    const auto launchedAt = rig.audio.launches.front().sample;

    rig.ticks (50);

    REQUIRE (rig.document.setAttribute ("/godot/range/" + bed + "/out", "1.5").ok);
    rig.tickOnce();
    REQUIRE (rig.audio.moves.size() == 1u);

    /*  The reader applied it 30 ms after where it was placed, the file
        carrying on from there. */
    const auto placed = rig.audio.moves.front().move.from;
    rig.audio.taken = cue::Player::LoopTaken { 1, placed + 0.030, placed + 0.030 };
    rig.tickOnce();

    /*  Past the new out the playhead is back at the in-point - by the
        reader's account, not the placement's: the first pass ran to 1.5 from
        where the reader took it, so it ends at 1.5 seconds of the file all the
        same, at speed one the same instant. */
    rig.ticks (30);

    const auto t = secondsSinceLaunch (rig, launchedAt);
    REQUIRE (t > 1.5);
    CHECK (rig.run (id)->position == doctest::Approx (t - 1.5).epsilon (1.0e-4));
}

//==============================================================================
/*  A BOUNCE TURNED ON OR OFF WHILE THE LOOP SOUNDS (namespace draft §41, BCX):
    heard with no stop and no step. On, from a horizon ahead; off, from a
    horizon ahead while the file goes out, and on its way back from where it
    reaches the in-point; the count and the playhead carry on through it. And
    a loop point dragged while it bounces keeps it bouncing (BCY). */
namespace
{
    void runToSecond (SchedulerRig& rig, std::int64_t launchedAt, double seconds)
    {
        while (secondsSinceLaunch (rig, launchedAt) < seconds)
            rig.tickOnce();
    }

    void setBounce (SchedulerRig& rig, const std::string& range, bool bounces)
    {
        REQUIRE (rig.document.setAttribute ("/godot/range/" + range + "/pingPong", bounces ? "true" : "false").ok);
    }

    //  Within a few samples, as the playhead is read on a tick's counter.
    bool playheadNear (double got, double want)
    {
        return std::abs (got - want) < 0.002;
    }
}

TEST_CASE ("bounce flip: a bounce turned on under a sounding loop turns at its out-point, with no step (§41, BCX)")
{
    SchedulerRig rig;
    const auto bed = rig.addRange (0.0, 2.0, 0);

    const auto id = rig.goAndLaunch();
    const auto launchedAt = rig.audio.launches.front().sample;

    runToSecond (rig, launchedAt, 0.5);
    REQUIRE (rig.audio.moves.empty());

    setBounce (rig, bed, true);
    rig.tickOnce();

    /*  ONE MOVE, a horizon ahead, the file where it is and going on out. */
    REQUIRE (rig.audio.moves.size() == 1u);

    const auto& flipped = rig.audio.moves.front().move;
    CHECK (flipped.pingPong);
    CHECK (flipped.direction == 1);
    CHECK (flipped.crossfade == doctest::Approx (0.0));
    CHECK (flipped.fileAt == doctest::Approx (flipped.from));
    CHECK (flipped.loopIn == doctest::Approx (0.0));
    CHECK (flipped.loopOut == doctest::Approx (2.0));
    CHECK (flipped.from > 0.5);

    /*  Past the out-point it comes back - a pass each way - and out again. */
    runToSecond (rig, launchedAt, 2.5);
    auto t = secondsSinceLaunch (rig, launchedAt);
    INFO ("at " << t << " s the playhead is at " << rig.run (id)->position);
    CHECK (playheadNear (rig.run (id)->position, 4.0 - t));
    CHECK (rig.run (id)->rangeIteration == 2);

    runToSecond (rig, launchedAt, 4.5);
    t = secondsSinceLaunch (rig, launchedAt);
    CHECK (playheadNear (rig.run (id)->position, t - 4.0));
    CHECK (rig.run (id)->rangeIteration == 3);

    //  Nothing more was placed: the bounce is the reader's.
    CHECK (rig.audio.moves.size() == 1u);
}

TEST_CASE ("bounce flip: a bounce turned off on its way out goes on to its out-point and wraps (§41, BCX)")
{
    SchedulerRig rig;
    const auto bed = rig.addRange (0.0, 2.0, 0);
    setBounce (rig, bed, true);

    const auto id = rig.goAndLaunch();
    const auto launchedAt = rig.audio.launches.front().sample;

    //  The slice was entered bouncing: its slot told so at its launch.
    REQUIRE (rig.audio.moves.size() == 1u);
    CHECK (rig.audio.moves.front().move.pingPong);

    runToSecond (rig, launchedAt, 0.5);
    setBounce (rig, bed, false);
    rig.tickOnce();

    REQUIRE (rig.audio.moves.size() == 2u);

    const auto& flipped = rig.audio.moves.back().move;
    CHECK_FALSE (flipped.pingPong);
    CHECK (flipped.direction == 1);
    CHECK (flipped.fileAt == doctest::Approx (flipped.from));

    runToSecond (rig, launchedAt, 2.5);
    const auto t = secondsSinceLaunch (rig, launchedAt);
    CHECK (playheadNear (rig.run (id)->position, t - 2.0));
    CHECK (rig.run (id)->rangeIteration == 2);
    CHECK_FALSE (rig.run (id)->bent);
}

TEST_CASE ("bounce flip: a bounce turned off on its way back waits for the in-point, then loops forwards (§41, BCX)")
{
    SchedulerRig rig;
    const auto bed = rig.addRange (0.0, 1.0, 0);
    setBounce (rig, bed, true);

    const auto id = rig.goAndLaunch();
    const auto launchedAt = rig.audio.launches.front().sample;
    REQUIRE (rig.audio.moves.size() == 1u);

    /*  At 1.4 s the file is at 0.6 on its way back. Turned off, nothing is
        placed: a loop starting here would jump. */
    runToSecond (rig, launchedAt, 1.4);
    setBounce (rig, bed, false);
    rig.tickOnce();
    CHECK (rig.audio.moves.size() == 1u);
    CHECK (rig.run (id)->bounceOwed);

    //  It still bounces meanwhile, and says so.
    rig.ticks (5);
    auto t = secondsSinceLaunch (rig, launchedAt);
    REQUIRE (t < 2.0);
    CHECK (playheadNear (rig.run (id)->position, 2.0 - t));

    /*  PLACED AS THE TURN COMES INSIDE THE HORIZON, at the turn: the file at
        the in-point, the reader at two seconds, going forwards and plain. */
    while (rig.audio.moves.size() == 1u && secondsSinceLaunch (rig, launchedAt) < 2.5)
        rig.tickOnce();

    REQUIRE (rig.audio.moves.size() == 2u);
    CHECK (secondsSinceLaunch (rig, launchedAt) < 2.0);

    const auto& flipped = rig.audio.moves.back().move;
    CHECK_FALSE (flipped.pingPong);
    CHECK (flipped.direction == 1);
    CHECK (flipped.crossfade == doctest::Approx (0.0));
    CHECK (flipped.fileAt == doctest::Approx (0.0));
    CHECK (std::abs (flipped.from - 2.0) < 2.0 / 48000.0);
    CHECK_FALSE (rig.run (id)->bounceOwed);

    //  Forwards from there, and round from the in-point at the out.
    runToSecond (rig, launchedAt, 2.5);
    t = secondsSinceLaunch (rig, launchedAt);
    CHECK (playheadNear (rig.run (id)->position, t - 2.0));

    runToSecond (rig, launchedAt, 3.3);
    t = secondsSinceLaunch (rig, launchedAt);
    CHECK (playheadNear (rig.run (id)->position, t - 3.0));
    CHECK (rig.run (id)->rangeIteration == 4);
}

TEST_CASE ("bounce flip: turned off and on again before the in-point, nothing is placed (§41, BCX)")
{
    SchedulerRig rig;
    const auto bed = rig.addRange (0.0, 1.0, 0);
    setBounce (rig, bed, true);

    const auto id = rig.goAndLaunch();
    const auto launchedAt = rig.audio.launches.front().sample;

    runToSecond (rig, launchedAt, 1.3);
    setBounce (rig, bed, false);
    rig.tickOnce();
    setBounce (rig, bed, true);
    rig.tickOnce();

    CHECK_FALSE (rig.run (id)->bounceOwed);
    runToSecond (rig, launchedAt, 2.5);
    CHECK (rig.audio.moves.size() == 1u);

    //  Still bouncing: out again from the in-point at two seconds.
    CHECK (playheadNear (rig.run (id)->position, secondsSinceLaunch (rig, launchedAt) - 2.0));
}

TEST_CASE ("bounce flip: the passes before and after the flip count towards its loops (§41, BCX)")
{
    /*  Four passes of a bouncing second - out, back, out, back - then the next
        range: the boundary at four seconds. Turned off at 1.4 s, on the second
        pass's way back, it waits for the in-point at two, then plays two passes
        forwards. The boundary is where it was. */
    SchedulerRig rig;
    const auto bed = rig.addRange (0.0, 1.0, 4);
    rig.addRange (1.0, 2.0, 1);
    setBounce (rig, bed, true);

    rig.goAndLaunch();
    const auto launchedAt = rig.audio.launches.front().sample;

    runToSecond (rig, launchedAt, 1.4);
    setBounce (rig, bed, false);
    runToSecond (rig, launchedAt, 4.5);

    REQUIRE_FALSE (rig.audio.stops.empty());
    INFO ("boundary at " << secondsOf (rig.audio.stops.front().sample - launchedAt) << " s");
    CHECK (std::llabs (rig.audio.stops.front().sample - launchedAt - 4 * 48000) <= 1);
}

TEST_CASE ("bounce flip: an out pulled in under a bouncing loop keeps it bouncing (§41, BCY)")
{
    SUBCASE ("behind the playhead on its way out: it turns at the new out, faded")
    {
        SchedulerRig rig;
        const auto bed = rig.addRange (0.0, 2.0, 0);
        setBounce (rig, bed, true);

        const auto id = rig.goAndLaunch();
        const auto launchedAt = rig.audio.launches.front().sample;

        runToSecond (rig, launchedAt, 1.5);
        REQUIRE (rig.document.setAttribute ("/godot/range/" + bed + "/out", "1.2").ok);
        rig.tickOnce();

        REQUIRE (rig.audio.moves.size() == 2u);

        /*  Placed going back from the new out: in the reader's mirror, just
            past the out-point. */
        const auto& moved = rig.audio.moves.back().move;
        CHECK (moved.pingPong);
        CHECK (moved.direction == 1);
        CHECK (moved.loopOut == doctest::Approx (1.2));
        CHECK (moved.crossfade == doctest::Approx (0.010));
        CHECK (moved.fileAt == doctest::Approx (1.2 + 1.0 / 48000.0));

        rig.ticks (25);
        const auto t = secondsSinceLaunch (rig, launchedAt);
        INFO ("at " << t << " s, " << t - moved.from << " s after the move, the playhead is at " << rig.run (id)->position);
        CHECK (playheadNear (rig.run (id)->position, 1.2 - (t - moved.from)));
    }

    SUBCASE ("ahead of the playhead on its way back: it carries on back")
    {
        SchedulerRig rig;
        const auto bed = rig.addRange (0.0, 2.0, 0);
        setBounce (rig, bed, true);

        const auto id = rig.goAndLaunch();
        const auto launchedAt = rig.audio.launches.front().sample;

        runToSecond (rig, launchedAt, 2.5);
        REQUIRE (rig.document.setAttribute ("/godot/range/" + bed + "/out", "1.8").ok);
        rig.tickOnce();

        REQUIRE (rig.audio.moves.size() == 2u);

        //  The file at 4 - t going back, so in the mirror of the new loop at 3.6 - (4 - t).
        const auto& moved = rig.audio.moves.back().move;
        CHECK (moved.pingPong);
        CHECK (moved.crossfade == doctest::Approx (0.0));
        CHECK (moved.fileAt == doctest::Approx (moved.from - 0.4));

        //  Back to the in-point at four seconds, then out to the new out.
        runToSecond (rig, launchedAt, 4.5);
        CHECK (playheadNear (rig.run (id)->position, secondsSinceLaunch (rig, launchedAt) - 4.0));
    }
}

TEST_CASE ("bounce flip: a cue armed at standby is armed again when its bounce is turned (§41, BCX)")
{
    SchedulerRig rig;
    const auto bed = rig.addRange (0.0, 2.0, 0);

    rig.ticks (5);
    REQUIRE (rig.audio.arms.size() == 1u);
    rig.audio.completeArms (rig.engine);
    rig.ticks (2);

    setBounce (rig, bed, true);
    rig.ticks (12);

    REQUIRE (rig.audio.arms.size() == 1u);
    REQUIRE (rig.audio.arms.front().ranges.size() == 1u);
    CHECK (rig.audio.arms.front().ranges.front().pingPong);
}

//==============================================================================
/*  THE WAVEFORM'S MENUS (namespace draft §81): where a cue starts and ends, a
    selection made slices, the first slice of a cue with none, and markers.
    Each through its command, one transaction each as serve makes them, so an
    undo is asked of the step the menu took. */
namespace
{
    struct MenuRig : RangeRig
    {
        MenuRig()
        {
            engine.setBeforeApply ([this] (const Command& appliedCommand, const Event& submitted,
                                           const std::vector<osc::Value>& coerced, std::int64_t tickIndex)
                                   {
                                       document.beginTransaction (appliedCommand.name, tickIndex,
                                                                  submitted.origin, coerced);
                                   });

            doc::registerDocumentCommands (engine.commands(), document);
            engine.log().openInMemory ({});
        }

        std::size_t apply (const std::string& command, std::vector<osc::Value> args)
        {
            REQUIRE (engine.submit (origin::cli, command, std::move (args)));
            return engine.processTick (++tick).applied;
        }

        /** The cue's ranges in playlist order, as "in-out" words with loops when not one. */
        std::string ranges() const
        {
            std::string out;

            for (const auto& child : document.findById (cueId))
                if (child.hasType ("Range"))
                {
                    out += (out.empty() ? "" : " ") + osc::formatDouble (static_cast<double> (child.getProperty ("in")))
                         + "-" + osc::formatDouble (static_cast<double> (child.getProperty ("out")));

                    if (const auto loops = static_cast<int> (child.getProperty ("loops", 1)); loops != 1)
                        out += "x" + std::to_string (loops);
                }

            return out;
        }

        std::string offset() const
        {
            return document.getAttribute ("/godot/cue/" + cueId + "/startOffset").value_or ("0");
        }

        std::size_t span (double start, double end)
        {
            return apply ("range.span", { osc::Value::string (cueId), osc::Value::float64 (start), osc::Value::float64 (end) });
        }

        std::size_t slices (double from, double to, int count, double length = 0.0)
        {
            std::vector<osc::Value> args { osc::Value::string (cueId), osc::Value::float64 (from),
                                           osc::Value::float64 (to), osc::Value::int32 (count) };

            if (length > 0.0)
                args.push_back (osc::Value::float64 (length));

            return apply ("range.slices", std::move (args));
        }

        std::int64_t tick = 0;
    };
}

TEST_CASE ("range.span: with no ranges the start is the offset, and an end makes the first range from it")
{
    MenuRig rig;

    CHECK (rig.span (4.0, -1.0) == 1u);
    CHECK (rig.offset() == "4");
    CHECK (rig.ranges().empty());

    //  Set to end: from the offset, which goes into the range.
    CHECK (rig.span (-1.0, 20.0) == 1u);
    CHECK (rig.ranges() == "4-20");
    CHECK (rig.offset() == "0");
    CHECK (rig.document.validate().empty());

    REQUIRE (rig.document.undo (doc::UndoDomain::document).has_value());
    CHECK (rig.ranges().empty());
    CHECK (rig.offset() == "4");

    //  Set start and end: the range is the selection's; an end not after the start is refused.
    CHECK (rig.span (2.0, 9.0) == 1u);
    CHECK (rig.ranges() == "2-9");
    CHECK (rig.span (9.0, 9.0) == 0u);
    CHECK (rig.span (-1.0, -1.0) == 0u);
}

TEST_CASE ("range.span: with ranges it cuts to the point, and reaches it across a gap")
{
    MenuRig rig;
    rig.add (0.0, 10.0);
    rig.add (10.0, 20.0);
    rig.add (20.0, 30.0);

    //  Set to start at 15: the first slice gone, the second begins there.
    CHECK (rig.span (15.0, -1.0) == 1u);
    CHECK (rig.ranges() == "15-20 20-30");

    //  Set to end at 25: the last slice ends there.
    CHECK (rig.span (-1.0, 25.0) == 1u);
    CHECK (rig.ranges() == "15-20 20-25");

    //  Before every range, the start comes back to it; past the furthest out, the end goes on to it.
    CHECK (rig.span (12.0, 28.0) == 1u);
    CHECK (rig.ranges() == "12-20 20-28");

    //  One undo for the step.
    REQUIRE (rig.document.undo (doc::UndoDomain::document).has_value());
    CHECK (rig.ranges() == "15-20 20-25");

    //  Past the last out with no end, nothing would be left: refused.
    CHECK (rig.span (40.0, -1.0) == 0u);
    CHECK (rig.span (-1.0, 3.0) == 0u);
    CHECK (rig.ranges() == "15-20 20-25");
}

TEST_CASE ("range.span: a start in a gap begins the next range there; both ends between ranges make the one range")
{
    MenuRig rig;
    rig.add (0.0, 5.0);
    rig.add (12.0, 20.0);

    CHECK (rig.span (8.0, -1.0) == 1u);
    CHECK (rig.ranges() == "8-20");

    REQUIRE (rig.document.undo (doc::UndoDomain::document).has_value());
    CHECK (rig.span (6.0, 10.0) == 1u);
    CHECK (rig.ranges() == "6-10");
}

TEST_CASE ("range.slices: a cue with no ranges keeps playing the rest of its file around the slices")
{
    MenuRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.cueId + "/startOffset", "2").ok);

    //  No length known: there is no whole to keep, refused.
    CHECK (rig.slices (10.0, 18.0, 4) == 0u);

    CHECK (rig.slices (10.0, 18.0, 4, 30.0) == 1u);
    CHECK (rig.ranges() == "2-10 10-12 12-14 14-16 16-18 18-30");
    CHECK (rig.offset() == "0");
    CHECK (rig.document.validate().empty());

    //  The record carries the six identifiers drawn and the length, for a replay.
    const auto parsed = LogFile::parse (rig.engine.log().contents());
    const auto& record = parsed.records.back();
    REQUIRE (record.args.size() == 6u);
    CHECK (record.args[4].asDouble() == doctest::Approx (30.0));

    std::size_t words = 0;

    for (std::size_t at = 0, next = 0; at < record.args[5].getString().size(); at = next + 1)
    {
        next = record.args[5].getString().find (' ', at);
        ++words;

        if (next == std::string::npos)
            break;
    }

    CHECK (words == 6u);

    REQUIRE (rig.document.undo (doc::UndoDomain::document).has_value());
    CHECK (rig.ranges().empty());
    CHECK (rig.offset() == "2");
}

TEST_CASE ("range.slices: the ranges the span overlaps are cut back to it, those inside taken out, and loops carried")
{
    MenuRig rig;
    rig.add (0.0, 10.0);
    rig.add (10.0, 20.0);
    rig.add (20.0, 30.0);

    CHECK (rig.slices (5.0, 25.0, 2) == 1u);
    CHECK (rig.ranges() == "0-5 5-15 15-25 25-30");

    REQUIRE (rig.document.undo (doc::UndoDomain::document).has_value());
    CHECK (rig.ranges() == "0-10 10-20 20-30");

    //  A bed going round for ever, cut into three: every piece goes round, its tail too.
    MenuRig bed;
    const auto id = bed.add (0.0, 30.0);
    REQUIRE (bed.document.setAttribute ("/godot/range/" + id + "/loops", "0").ok);

    CHECK (bed.slices (6.0, 15.0, 3) == 1u);
    CHECK (bed.ranges() == "0-6x0 6-9x0 9-12x0 12-15x0 15-30x0");

    //  Nought slices, or slices shorter than a hundredth of a second: refused.
    CHECK (bed.slices (6.0, 15.0, 0) == 0u);
    CHECK (bed.slices (6.0, 6.005, 1) == 0u);
}

TEST_CASE ("range.split: on a cue with no ranges, given the length, the whole of it from its offset is cut in two")
{
    MenuRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.cueId + "/startOffset", "3").ok);

    //  Without the length, there is nothing to divide - as it always was.
    CHECK (rig.apply ("range.split", { osc::Value::string (rig.cueId), osc::Value::float64 (12.0) }) == 0u);

    CHECK (rig.apply ("range.split", { osc::Value::string (rig.cueId), osc::Value::float64 (12.0),
                                       osc::Value::string ({}), osc::Value::float64 (40.0) }) == 1u);
    CHECK (rig.ranges() == "3-12 12-40");
    CHECK (rig.offset() == "0");

    //  With ranges, the split it always was: the length is not asked.
    CHECK (rig.apply ("range.split", { osc::Value::string (rig.cueId), osc::Value::float64 (20.0) }) == 1u);
    CHECK (rig.ranges() == "3-12 12-20 20-40");
}

TEST_CASE ("marker: made on a sound, published flat with its cue and place, named, moved and taken away")
{
    MenuRig rig;

    CHECK (rig.apply ("marker.create", { osc::Value::string (rig.cueId), osc::Value::float64 (4.25),
                                         osc::Value::string ("Marker 1") }) == 1u);
    CHECK (rig.apply ("marker.create", { osc::Value::string (rig.cueId), osc::Value::float64 (9.0) }) == 1u);

    //  Not on a cue that plays no file; not before the start of it.
    CHECK (rig.apply ("marker.create", { osc::Value::string (rig.memoId), osc::Value::float64 (1.0) }) == 0u);
    CHECK (rig.apply ("marker.create", { osc::Value::string (rig.cueId), osc::Value::float64 (-1.0) }) == 0u);

    std::vector<std::string> ids;

    for (const auto& child : rig.document.findById (rig.cueId))
        if (child.hasType ("Marker"))
            ids.push_back (child["id"].toString().toStdString());

    REQUIRE (ids.size() == 2u);

    rig.parameters.markStale();
    tree::EngineState state;
    state.version = "test";
    const auto snapshot = rig.parameters.publish (0, state);

    const auto text = [&snapshot] (const std::string& address)
    {
        const auto* node = snapshot->find (address);

        if (node == nullptr || ! node->soleValue().has_value())
            return std::string ("(absent)");

        return node->soleValue()->isString() ? node->soleValue()->getString()
                                             : osc::formatDouble (node->soleValue()->asDouble());
    };

    CHECK (text ("/godot/marker/" + ids[0] + "/cue") == rig.cueId);
    CHECK (text ("/godot/marker/" + ids[0] + "/at") == "4.25");
    CHECK (text ("/godot/marker/" + ids[0] + "/name") == "Marker 1");
    CHECK (text ("/godot/marker/" + ids[1] + "/index") == "1");

    CHECK (rig.document.setAttribute ("/godot/marker/" + ids[1] + "/at", "11.5").ok);
    CHECK (rig.document.setAttribute ("/godot/marker/" + ids[1] + "/name", "The slam").ok);
    CHECK_FALSE (rig.document.setAttribute ("/godot/marker/" + ids[1] + "/at", "-2").ok);

    //  It is the show's: saved, read back.
    const auto xml = doc::CanonicalXml::write (rig.document);
    doc::ShowDocument reopened;
    REQUIRE (doc::CanonicalXml::read (xml, reopened).ok);
    CHECK (reopened.getAttribute ("/godot/marker/" + ids[1] + "/name").value_or ("?") == "The slam");
    CHECK (reopened.getAttribute ("/godot/marker/" + ids[1] + "/at").value_or ("?") == "11.5");

    CHECK (rig.apply ("object.delete", { osc::Value::string (ids[0]) }) == 1u);
    CHECK_FALSE (rig.document.findById (ids[0]).isValid());
}

//==============================================================================
/*  A RANGE THAT RUNS TO THE END, SAID SO (namespace draft §99, AXQ-AXS): the
    row the edits keep - a split's second half, a slice's tail, the whole made
    from a cue with none - and the one a number written puts back to false, in
    the same step so an Undo puts both back. */
TEST_CASE ("range end: to the end is kept by the edits that keep the end, and a number written says a number (§99)")
{
    RangeRig rig;

    const auto whole = rig.document.createRange (rig.cueId, 0.0, 30.0, {}, true);
    REQUIRE (whole.ok);
    CHECK (rig.stored (whole.id, "toEnd") == "true");

    //  A split: the second half ends where the whole did - at the end.
    const auto second = rig.document.splitRange (rig.cueId, 12.0);
    REQUIRE (second.ok);
    CHECK (rig.stored (whole.id, "toEnd") == "false");
    CHECK (rig.stored (second.id, "toEnd") == "true");

    //  A number written for its out point is that number; an Undo puts the end back.
    rig.document.beginTransaction ("node.set", 100, "test", {});
    REQUIRE (rig.document.setAttribute ("/godot/range/" + second.id + "/out", "25").ok);
    CHECK (rig.stored (second.id, "toEnd") == "false");
    CHECK (rig.stored (second.id, "out") == "25");

    REQUIRE (rig.document.undo (doc::UndoDomain::document).has_value());
    CHECK (rig.stored (second.id, "toEnd") == "true");
    CHECK (rig.stored (second.id, "out") == "30");

    //  Its in point is no number about the end.
    REQUIRE (rig.document.setAttribute ("/godot/range/" + second.id + "/in", "13").ok);
    CHECK (rig.stored (second.id, "toEnd") == "true");

}

TEST_CASE ("range end: a cue made ranges from the whole of what it plays runs to its end (§99)")
{
    SUBCASE ("Add new slice on a cue with no ranges")
    {
        RangeRig rig;
        std::string id, wholeId;
        REQUIRE (rig.document.splitWhole (rig.cueId, 10.0, 30.0, id, wholeId).ok);
        CHECK (rig.stored (wholeId, "toEnd") == "false");
        CHECK (rig.stored (id, "toEnd") == "true");
    }

    SUBCASE ("Make new slices on a cue with no ranges: the tail after them keeps the end")
    {
        RangeRig rig;
        std::string ids;
        REQUIRE (rig.document.sliceSpan (rig.cueId, 10.0, 20.0, 2, 30.0, ids).ok);

        std::vector<std::string> made;
        std::string word;

        for (const auto c : ids + " ")
        {
            if (c == ' ')
            {
                if (! word.empty())
                    made.push_back (word);

                word.clear();
            }
            else
                word += c;
        }

        //  The whole, cut back to the span's start; two slices; the tail to the end.
        REQUIRE (made.size() == 4u);
        CHECK (rig.stored (made[0], "toEnd") == "false");
        CHECK (rig.stored (made[1], "toEnd") == "false");
        CHECK (rig.stored (made[2], "toEnd") == "false");
        CHECK (rig.stored (made[3], "toEnd") == "true");
        CHECK (rig.stored (made[3], "out") == "30");
    }

    SUBCASE ("a range made to a point is that point")
    {
        RangeRig rig;
        std::string id;
        REQUIRE (rig.document.spanRanges (rig.cueId, std::nullopt, 20.0, id).ok);
        CHECK (rig.stored (id, "toEnd") == "false");
    }
}

TEST_CASE ("range end: a sound locked to its movie takes the movie's end as its own (§99)")
{
    RangeRig rig;

    const auto movie = rig.document.createCue (rig.listId, 2, "video", "Faith").id;
    const auto sound = rig.document.createCue (rig.listId, 3, "media", "Faith (sound)").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + movie + "/source", "movie").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + movie + "/file", "faith.mov").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + sound + "/file", "faith (sound).wav").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + sound + "/lockedTo", movie).ok);

    //  The movie's whole, to its end: 162.6625 s of picture.
    REQUIRE (rig.document.createRange (movie, 0.0, 162.6625, {}, true).ok);

    const auto followed = rig.document.findById (sound).getChildWithName ("Range");
    REQUIRE (followed.isValid());
    CHECK (static_cast<bool> (followed.getProperty ("toEnd", false)));

    //  A number on the movie's is a number on the sound's too.
    const auto movieRange = rig.document.findById (movie).getChildWithName ("Range")["id"].toString().toStdString();
    REQUIRE (rig.document.setAttribute ("/godot/range/" + movieRange + "/out", "100").ok);
    CHECK_FALSE (static_cast<bool> (rig.document.findById (sound).getChildWithName ("Range").getProperty ("toEnd", false)));
}

//==============================================================================
/*  A VAMP THAT FIRES THE NEXT CUE AS ITS LOOP COMPLETES (namespace draft §100,
    AYN; the author, 2026-10-20, of La Clairière: "the vamp should have the
    following cue triggered when the loop completes as an option" -
    sample-exact, his pick). A bed looping for ever, a transport cue that
    advances it with `thenNext`, and a song after it: the song starts on the
    very sample the bed's pass ends, standby moves past it, and a Doh! of the
    GO that fired the vamp takes back both. */
namespace
{
    struct VampRig : SchedulerRig
    {
        std::string vamp, song;

        VampRig()
        {
            REQUIRE (document.setAttribute ("/godot/list/goDebounce", "0").ok);
            addRange (0.0, 1.0, 0);             // the vamp, for ever
            addRange (1.0, 2.0, 1);             // its outro

            vamp = document.createCue (listId, 1, "transport", "Devamp").id;
            REQUIRE (document.setAttribute ("/godot/cue/" + vamp + "/target", cueId).ok);
            REQUIRE (document.setAttribute ("/godot/cue/" + vamp + "/verb", "advance").ok);
            REQUIRE (document.setAttribute ("/godot/cue/" + vamp + "/thenNext", "true").ok);

            song = document.createCue (listId, 2, "media", "Song").id;
            REQUIRE (document.setAttribute ("/godot/cue/" + song + "/file", "song.wav").ok);
        }

        //  Ticks, the disk answering every arm as it is asked.
        void play (int howMany)
        {
            for (int i = 0; i < howMany; ++i)
            {
                audio.completeArms (engine);
                tickOnce();
            }
        }

        const cue::Run* songRun() const
        {
            for (const auto& run : runs.all())
                if (run.cue == song)
                    return &run;
            return nullptr;
        }
    };
}

TEST_CASE ("vamp: the cue after an advance that asks for it starts on the sample the loop's pass ends, and standby moves past it (§100)")
{
    VampRig rig;

    const auto bed = rig.goAndLaunch();
    const auto launchedAt = rig.audio.launches.front().sample;

    //  A few passes, then a fifth of a second into one: the GO comes well before its end.
    rig.play (120);
    while ((rig.audio.samples - launchedAt) % 48000 < 9000 || (rig.audio.samples - launchedAt) % 48000 > 11000)
        rig.play (1);

    //  GO on the vamp: standby moves to the song, which is armed ahead as any next cue is.
    rig.submitAndTick ("go");
    rig.play (150);

    REQUIRE_FALSE (rig.audio.stops.empty());          // the vamp's boundary first, then the outro's end
    const auto boundary = rig.audio.stops.front().sample;

    const auto* song = rig.songRun();
    REQUIRE (song != nullptr);
    CHECK (song->launchedAtSample == boundary);
    CHECK (rig.run (bed)->range == 1);

    //  The song was fired as GO fires it: standby is past it, at the list's end.
    CHECK (rig.document.getAttribute (cue::standbyAddressOf (rig.listId)).value_or ("?").empty());

    //  And it was the vamp's GO that fired it.
    CHECK (song->launchedAtSample > 0);
}

TEST_CASE ("vamp: without thenNext the cue after waits for GO; a Doh! of the vamp's GO takes back both (§100)")
{
    SUBCASE ("off")
    {
        VampRig rig;
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.vamp + "/thenNext", "false").ok);

        rig.goAndLaunch();
        rig.play (130);
        rig.submitAndTick ("go");
        rig.play (150);

        REQUIRE_FALSE (rig.audio.stops.empty());
        const auto* song = rig.songRun();
        CHECK ((song == nullptr || song->launchedAtSample <= 0));
        CHECK (rig.document.getAttribute (cue::standbyAddressOf (rig.listId)).value_or ("") == rig.song);
    }

    SUBCASE ("Doh!")
    {
        VampRig rig;

        const auto bed = rig.goAndLaunch();
        rig.play (130);

        REQUIRE (rig.engine.submit (origin::cli, "go", {}));
        REQUIRE (rig.engine.submit (origin::cli, "go.doh", {}));
        rig.tickOnce();

        rig.play (150);

        CHECK (rig.audio.stops.empty());
        CHECK (rig.run (bed)->range == 0);

        const auto* song = rig.songRun();
        CHECK ((song == nullptr || song->launchedAtSample <= 0));
    }
}


TEST_CASE ("vamp: a group after the vamp - La Clairière's shape - starts its first sound on the pass's last sample too (§100)")
{
    /*  In La Clairière every devamp is followed by a group: the next section's
        sounds and fades together. The group is fired as GO fires it, and its
        member with no pre-wait of its own starts where the bed's pass ends. */
    VampRig rig;
    REQUIRE (rig.document.remove (rig.song).ok);

    const auto section = rig.document.createCue (rig.listId, 2, "group", "Section 2").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + section + "/mode", "timeline").ok);
    const auto drums = rig.document.createCue (section, 0, "media", "Drums").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + drums + "/file", "drums.wav").ok);

    const auto bed = rig.goAndLaunch();
    const auto launchedAt = rig.audio.launches.front().sample;

    rig.play (120);
    while ((rig.audio.samples - launchedAt) % 48000 < 9000 || (rig.audio.samples - launchedAt) % 48000 > 11000)
        rig.play (1);

    rig.submitAndTick ("go");
    rig.play (150);

    REQUIRE_FALSE (rig.audio.stops.empty());
    const auto boundary = rig.audio.stops.front().sample;

    const cue::Run* member = nullptr;
    for (const auto& run : rig.runs.all())
        if (run.cue == drums)
            member = &run;

    REQUIRE (member != nullptr);
    CHECK (member->launchedAtSample == boundary);
    CHECK (rig.run (bed)->range == 1);
}
