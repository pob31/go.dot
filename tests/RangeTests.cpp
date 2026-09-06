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
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>

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
