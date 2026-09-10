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
    Undo: one transaction per applied command, and the three things it does not
    undo (namespace draft §14.9).

    TWO PROPERTIES HERE ARE UNUSUAL AND ARE THE REASON THE FILE EXISTS.

    The first is the identifier registry after a delete is taken back. The
    document is self-consistent on its own — JUCE's remove action holds the
    subtree and its undo puts back the same objects with the identifiers they
    had — but `remove` gave every one of those identifiers back to the registry
    on the way out, so the registry is what has to be told. The failure it
    prevents is not the delete: it is the NEXT create, which can be handed an
    identifier a restored cue is already using, and which then fails at the next
    save or the next GO rather than at the gesture anybody would remember
    making.

    The second is the agreement between `toVar` and the reader's hand-written
    copy of its switch. With an UndoManager attached, `ValueTree::setProperty`
    compares through `var::equals`, where "1" == 1, so the day one row's reader
    type drifts from its writer type is the day the first write to that
    attribute is dropped in silence. Nothing in the code enforces the agreement
    and no reviewer can see it in a diff, so it is pinned here.

    UNDO STEPS ARE COUNTED AND ACTIONS NEVER ARE. JUCE refuses to merge an
    action that ADDS a property, and this document omits defaults — an absent
    attribute is its default — so the first write to an attribute a cue does not
    yet carry produces an adding action that never merges with the next. Ten
    writes to one fresh address are one transaction of two actions. The undo
    STEP is the transaction, so that is what is asserted; a case written to the
    action count would be a case that fails on a fresh show and passes on a
    saved one.

    A serialisation surface, so every case here runs under fr_FR as well as C.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>

#include <juce_data_structures/juce_data_structures.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace wfg;

namespace
{
    /*  Engine, document, cue commands and the transaction hook, wired exactly
        as `serve` and `replay` wire them.

        The hook is the point of assembling an engine at all rather than driving
        the document directly: where it fires and what it is handed is half of
        what this file is about, and a case that called `beginTransaction` by
        hand would be testing a call it had made itself. */
    struct Rig
    {
        Rig()
        {
            engine.log().openInMemory ({});

            doc::registerDocumentCommands (engine.commands(), document);
            cue::registerCueCommands (engine.commands(), document, focus);
            cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);

            /*  NO PLAYER, deliberately. A Runner with no audio side moves the
                standby and writes the same log — only the sound is missing,
                which is the configuration `wfg replay` runs in and is exactly
                enough to ask whether a GO puts anything on the stack. */
            runner.setSamplesPerTick (960);

            engine.setBeforeApply ([this] (const Command& appliedCommand,
                                           const Event& submitted,
                                           const std::vector<osc::Value>& coerced,
                                           std::int64_t tickIndex)
                                   {
                                       document.beginTransaction (appliedCommand.name, tickIndex,
                                                                  submitted.origin, coerced);
                                   });
        }

        /*  One command through the engine, from a named origin. Two functions
            rather than one with a default, because coalescing is keyed on the
            origin and a case about it should have to say which. */
        Engine::TickResult applyFrom (std::int64_t tick, const std::string& from,
                                      const std::string& command,
                                      std::vector<osc::Value> args = {})
        {
            REQUIRE (engine.submit (from, command, std::move (args)));
            return engine.processTick (tick);
        }

        /** The same, from the command line, which is most cases. */
        Engine::TickResult apply (std::int64_t tick, const std::string& command,
                                  std::vector<osc::Value> args = {})
        {
            return applyFrom (tick, std::string (origin::cli), command, std::move (args));
        }

        /*  The transaction Undo would unmake, READ AND NOT POPPED, so that a
            case can ask what the top of the stack is without changing it. It is
            also exactly what `/godot/document/undoName` publishes. */
        std::string undoName() const
        {
            return document.history (doc::UndoDomain::document)
                       .getUndoDescription().toStdString();
        }

        bool canUndo() const
        {
            return document.history (doc::UndoDomain::document).canUndo();
        }

        /*  HOW MANY STEPS THE STACK HOLDS, by taking all of them. Destructive,
            and named so, because there is no non-destructive way to ask JUCE
            and a case that wants the number is finished with the document. */
        int drainUndoSteps()
        {
            int steps = 0;

            while (document.undo (doc::UndoDomain::document).has_value())
                ++steps;

            return steps;
        }

        /** One argument of the most recent record, as it was APPLIED. */
        std::string lastAppliedArg (std::size_t index)
        {
            const auto parsed = LogFile::parse (engine.log().contents());

            REQUIRE (! parsed.records.empty());

            const auto& last = parsed.records.back();

            REQUIRE (last.kind == LogRecord::Kind::applied);
            REQUIRE (last.args.size() > index);

            return last.args[index].getString();
        }

        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::Focus focus;
        doc::IdRegistry runIds { doc::IdRegistry::withSeed (11) };
        cue::Runner runner { document, runs, runIds, focus };
    };

    osc::Value text (const std::string& s) { return osc::Value::string (s); }

    /*  The JUCE type a var is actually holding, as a word, so that a failure
        says "reader int64, writer string" rather than "false". */
    std::string varType (const juce::var& value)
    {
        if (value.isVoid())   return "void";
        if (value.isString()) return "string";
        if (value.isBool())   return "bool";
        if (value.isInt())    return "int";
        if (value.isInt64())  return "int64";
        if (value.isDouble()) return "double";

        return "other";
    }

    const std::string mainList = "7K2QM9X4";
    const std::string firstCue = "B3N8R5TW";
    const std::string theGroup = "D9FH2JKA";
    const std::string inGroupA = "E4GP6QSC";
    const std::string inGroupB = "F7HR8TVD";
}

//==============================================================================
TEST_CASE ("undo: a value written is taken back, and put back again")
{
    Rig rig;

    REQUIRE (rig.apply (0, "list.create", { text ("Main"), text (mainList) }).applied == 1);
    REQUIRE (rig.apply (1, "cue.create", { text (mainList), osc::Value::int32 (0),
                                           text ("memo"), text ("House to half"),
                                           text (firstCue) }).applied == 1);

    const auto name = "/godot/cue/" + firstCue + "/name";

    /*  Far enough apart that the two writes are two gestures: within the window
        they would be one drag and one step, which is the next case. */
    REQUIRE (rig.apply (10, "node.set", { text (name), text ("House to full") }).applied == 1);
    REQUIRE (rig.apply (200, "node.set", { text (name), text ("Houselights out") }).applied == 1);

    CHECK (rig.document.getAttribute (name) == std::string ("Houselights out"));

    /*  The name of the transaction Undo would unmake is the command that made
        it — which is what a client puts after the word "Undo". */
    CHECK (rig.undoName() == "node.set");

    REQUIRE (rig.document.undo (doc::UndoDomain::document) == std::string ("node.set"));
    CHECK (rig.document.getAttribute (name) == std::string ("House to full"));

    REQUIRE (rig.document.redo (doc::UndoDomain::document) == std::string ("node.set"));
    CHECK (rig.document.getAttribute (name) == std::string ("Houselights out"));

    /*  And back through the create, which is its own step named after itself. */
    REQUIRE (rig.document.undo (doc::UndoDomain::document).has_value());
    REQUIRE (rig.document.undo (doc::UndoDomain::document).has_value());
    CHECK (rig.undoName() == "cue.create");
}

TEST_CASE ("undo: a create taken back and put back keeps the identifier it drew")
{
    Rig rig;

    REQUIRE (rig.apply (0, "list.create", { text ("Main"), text (mainList) }).applied == 1);

    /*  DRAWN BY THE ENGINE, not supplied, because the property under test is
        that a redo does not draw a second one. The record carries what was
        applied, so the log is where the identifier is read back from. */
    REQUIRE (rig.apply (1, "cue.create", { text (mainList), osc::Value::int32 (0),
                                           text ("memo"), text ("House to half") }).applied == 1);

    const auto drawn = rig.lastAppliedArg (4);

    REQUIRE (doc::Id::isValid (drawn));
    REQUIRE (rig.document.findById (drawn).isValid());

    REQUIRE (rig.document.undo (doc::UndoDomain::document) == std::string ("cue.create"));
    CHECK_FALSE (rig.document.findById (drawn).isValid());

    REQUIRE (rig.document.redo (doc::UndoDomain::document) == std::string ("cue.create"));

    /*  THE SAME IDENTIFIER, ALWAYS. It is a property of the object JUCE put
        back, not something the redo went and asked for. */
    REQUIRE (rig.document.findById (drawn).isValid());
    CHECK (rig.document.ids().isTaken (drawn));
    CHECK (rig.document.getAttribute ("/godot/cue/" + drawn + "/name")
             == std::string ("House to half"));
}

TEST_CASE ("undo: a deleted group comes back whole, and no later create can take its identifiers")
{
    /*  THE CASE THAT MATTERS, and the one the black-box driver repeats against
        a shipped binary. `remove` releases every identifier under the node on
        the way out; undo puts the objects back and touches the registry not at
        all, so without the rebuild `findById` answers and `isTaken` says no —
        and the next create can be handed an identifier a restored cue is
        already using. Two objects with one identity fail at the next save or
        the next GO, which is a long way from the gesture that caused it. */
    Rig rig;

    REQUIRE (rig.apply (0, "list.create", { text ("Main"), text (mainList) }).applied == 1);
    REQUIRE (rig.apply (1, "cue.create", { text (mainList), osc::Value::int32 (0),
                                           text ("group"), text ("Preshow"),
                                           text (theGroup) }).applied == 1);
    REQUIRE (rig.apply (2, "cue.create", { text (theGroup), osc::Value::int32 (0),
                                           text ("memo"), text ("Walk-in"),
                                           text (inGroupA) }).applied == 1);
    REQUIRE (rig.apply (3, "cue.create", { text (theGroup), osc::Value::int32 (1),
                                           text ("memo"), text ("Announce"),
                                           text (inGroupB) }).applied == 1);

    REQUIRE (rig.apply (4, "object.delete", { text (theGroup) }).applied == 1);

    for (const auto& gone : { theGroup, inGroupA, inGroupB })
    {
        CHECK_FALSE (rig.document.findById (gone).isValid());
        CHECK_FALSE (rig.document.ids().isTaken (gone));
    }

    REQUIRE (rig.document.undo (doc::UndoDomain::document) == std::string ("object.delete"));

    for (const auto& back : { theGroup, inGroupA, inGroupB })
    {
        INFO ("restored " << back);
        CHECK (rig.document.findById (back).isValid());

        /*  The half nothing else would have noticed. `generate` inserts
            whatever it finds free, so an identifier the registry has forgotten
            is free — and this is the assertion that says it has not. */
        CHECK (rig.document.ids().isTaken (back));
    }

    /*  Said again the way a show would find out: a run of creates, none of
        which may be handed one of the three that came back. */
    std::vector<std::string> madeAfterwards;

    for (int n = 0; n < 20; ++n)
    {
        REQUIRE (rig.apply (5 + n, "cue.create", { text (mainList), osc::Value::int32 (0),
                                                   text ("memo"), text ("Extra") }).applied == 1);
        madeAfterwards.push_back (rig.lastAppliedArg (4));
    }

    for (const auto& fresh : madeAfterwards)
    {
        INFO ("created after the undo: " << fresh);
        CHECK (fresh != theGroup);
        CHECK (fresh != inGroupA);
        CHECK (fresh != inGroupB);
    }

    /*  And the restored group is still where it was, with its members in the
        order it had them. */
    CHECK (rig.document.getAttribute ("/godot/cue/" + theGroup + "/name")
             == std::string ("Preshow"));
    CHECK (rig.document.findById (inGroupA).getParent() == rig.document.findById (theGroup));
    CHECK (rig.document.findById (theGroup).indexOf (rig.document.findById (inGroupB)) == 1);
}

TEST_CASE ("undo: a move inside one parent and a move to another both come back")
{
    Rig rig;

    REQUIRE (rig.apply (0, "list.create", { text ("Main"), text (mainList) }).applied == 1);
    REQUIRE (rig.apply (1, "cue.create", { text (mainList), osc::Value::int32 (0),
                                           text ("memo"), text ("First"),
                                           text (firstCue) }).applied == 1);
    REQUIRE (rig.apply (2, "cue.create", { text (mainList), osc::Value::int32 (1),
                                           text ("group"), text ("Preshow"),
                                           text (theGroup) }).applied == 1);
    REQUIRE (rig.apply (3, "cue.create", { text (theGroup), osc::Value::int32 (0),
                                           text ("memo"), text ("Walk-in"),
                                           text (inGroupA) }).applied == 1);

    const auto list = rig.document.findById (mainList);

    SUBCASE ("within one parent")
    {
        REQUIRE (rig.apply (10, "object.move", { text (firstCue), text (mainList),
                                                 osc::Value::int32 (1) }).applied == 1);

        CHECK (list.indexOf (rig.document.findById (firstCue)) == 1);
        CHECK (rig.undoName() == "object.move");

        REQUIRE (rig.document.undo (doc::UndoDomain::document) == std::string ("object.move"));
        CHECK (list.indexOf (rig.document.findById (firstCue)) == 0);

        REQUIRE (rig.document.redo (doc::UndoDomain::document).has_value());
        CHECK (list.indexOf (rig.document.findById (firstCue)) == 1);
    }

    SUBCASE ("and across two, which is two actions in one step")
    {
        REQUIRE (rig.apply (10, "object.move", { text (firstCue), text (theGroup),
                                                 osc::Value::int32 (0) }).applied == 1);

        CHECK (rig.document.findById (firstCue).getParent()
                 == rig.document.findById (theGroup));

        /*  ONE STEP AND NOT TWO, although the door made a remove and an add:
            the step is the transaction, and the transaction is the command. */
        REQUIRE (rig.document.undo (doc::UndoDomain::document) == std::string ("object.move"));

        CHECK (rig.document.findById (firstCue).getParent() == list);
        CHECK (list.indexOf (rig.document.findById (firstCue)) == 0);
        CHECK (rig.document.findById (theGroup).getNumChildren() == 1);
    }
}

TEST_CASE ("undo: one drag is one step")
{
    /*  What a client emits when a number field is dragged: one `node.set` per
        change event, from one origin, on one address. An undo that took back
        one of those would be a keystroke that has to be held down, which is how
        an operator overshoots into the edit before the one they meant. */
    Rig rig;

    REQUIRE (rig.apply (0, "list.create", { text ("Main"), text (mainList) }).applied == 1);
    REQUIRE (rig.apply (1, "cue.create", { text (mainList), osc::Value::int32 (0),
                                           text ("memo"), text ("House to half"),
                                           text (firstCue) }).applied == 1);

    const auto preWait = "/godot/cue/" + firstCue + "/preWait";

    for (int frame = 0; frame < 10; ++frame)
        REQUIRE (rig.apply (10 + frame, "node.set",
                            { text (preWait),
                              osc::Value::float64 (0.5 + 0.1 * static_cast<double> (frame)) })
                   .applied == 1);

    CHECK (rig.undoName() == "node.set");

    REQUIRE (rig.document.undo (doc::UndoDomain::document).has_value());

    /*  Back to what it was before the drag, in one press — and `preWait`
        defaults to 0, so what "before" means is the row's default, written as
        the canonical text rather than compared as a double. */
    CHECK (rig.document.getAttribute (preWait) == std::string ("0"));

    /*  Two steps left: the cue and the list. The drag was one of three and not
        one of twelve. */
    CHECK (rig.drainUndoSteps() == 2);
}

TEST_CASE ("undo: a different address, a different origin and a pause each split the run")
{
    Rig rig;

    REQUIRE (rig.apply (0, "list.create", { text ("Main"), text (mainList) }).applied == 1);
    REQUIRE (rig.apply (1, "cue.create", { text (mainList), osc::Value::int32 (0),
                                           text ("memo"), text ("House to half"),
                                           text (firstCue) }).applied == 1);

    const auto preWait  = "/godot/cue/" + firstCue + "/preWait";
    const auto postWait = "/godot/cue/" + firstCue + "/postWait";

    SUBCASE ("two fields are two edits")
    {
        REQUIRE (rig.apply (10, "node.set", { text (preWait), osc::Value::float64 (1.0) }).applied == 1);
        REQUIRE (rig.apply (11, "node.set", { text (postWait), osc::Value::float64 (2.0) }).applied == 1);
        REQUIRE (rig.apply (12, "node.set", { text (preWait), osc::Value::float64 (3.0) }).applied == 1);

        //  Three writes, three addresses in sequence, three steps — plus two creates.
        CHECK (rig.drainUndoSteps() == 5);
    }

    SUBCASE ("two operators are two decisions, however close together")
    {
        REQUIRE (rig.applyFrom (10, "ws:192.168.1.20:51234", "node.set",
                                { text (preWait), osc::Value::float64 (1.0) }).applied == 1);
        REQUIRE (rig.applyFrom (11, "udp:10.0.0.5:9000", "node.set",
                                { text (preWait), osc::Value::float64 (2.0) }).applied == 1);

        CHECK (rig.drainUndoSteps() == 4);
    }

    SUBCASE ("a pause is where a person stopped and looked")
    {
        REQUIRE (rig.apply (10, "node.set", { text (preWait), osc::Value::float64 (1.0) }).applied == 1);

        //  One tick inside the window, and one tick outside it.
        REQUIRE (rig.apply (10 + doc::ShowDocument::coalescingWindowTicks, "node.set",
                            { text (preWait), osc::Value::float64 (2.0) }).applied == 1);
        REQUIRE (rig.apply (11 + 2 * doc::ShowDocument::coalescingWindowTicks, "node.set",
                            { text (preWait), osc::Value::float64 (3.0) }).applied == 1);

        CHECK (rig.drainUndoSteps() == 4);
    }

    SUBCASE ("and anything that is not node.set")
    {
        REQUIRE (rig.apply (10, "node.set", { text (preWait), osc::Value::float64 (1.0) }).applied == 1);
        REQUIRE (rig.apply (11, "cue.create", { text (mainList), osc::Value::int32 (1),
                                                text ("memo"), text ("Second"),
                                                text (theGroup) }).applied == 1);
        REQUIRE (rig.apply (12, "node.set", { text (preWait), osc::Value::float64 (2.0) }).applied == 1);

        CHECK (rig.undoName() == "node.set");
        CHECK (rig.drainUndoSteps() == 5);
    }
}

TEST_CASE ("undo: the step is the transaction and never the action")
{
    /*  JUCE returns no coalesced action at all when one of the two is ADDING a
        property, and this document omits defaults — so the first write to an
        attribute a cue does not yet carry is an adding action that never merges
        with the next. Ten writes to one fresh address are one transaction of
        TWO actions.

        The design is unharmed, because the undo step is the transaction. What
        would be harmed is a case written to the action count, so this one says
        out loud that the number it asserts is steps. */
    Rig rig;

    REQUIRE (rig.apply (0, "list.create", { text ("Main"), text (mainList) }).applied == 1);
    REQUIRE (rig.apply (1, "cue.create", { text (mainList), osc::Value::int32 (0),
                                           text ("memo"), text ("House to half"),
                                           text (firstCue) }).applied == 1);

    const auto notes = "/godot/cue/" + firstCue + "/notes";

    for (int n = 0; n < 10; ++n)
        REQUIRE (rig.apply (10 + n, "node.set",
                            { text (notes), text ("draft " + std::to_string (n)) }).applied == 1);

    REQUIRE (rig.canUndo());
    REQUIRE (rig.document.undo (doc::UndoDomain::document).has_value());

    /*  ONE PRESS TAKES BACK ALL TEN, including the adding action underneath
        them — and `notes` has no declared default, so what it goes back to is
        the empty text an absent attribute reads as. */
    CHECK (rig.document.getAttribute (notes) == std::string (""));
    CHECK (rig.drainUndoSteps() == 2);
}

TEST_CASE ("undo: where the operator is standing is not an edit")
{
    /*  Two state rows and one GO, none of which may reach the stack. The
        operator's position is not something they decided about the show (PRD
        §4.10), and a pointer that jumped backwards on Ctrl-Z would be the
        machine moving standby, which PRD §3.5 forbids for the same reason it
        forbids a trigger doing it. */
    Rig rig;

    /*  The last real edit is a `cue.create`, on purpose: its transaction has a
        name no `node.set` could produce, so a state write that quietly became a
        step would change the top of the stack to "node.set" and be caught by a
        reading rather than by a count. */
    REQUIRE (rig.apply (0, "list.create", { text ("Main"), text (mainList) }).applied == 1);
    REQUIRE (rig.apply (1, "cue.create", { text (mainList), osc::Value::int32 (0),
                                           text ("memo"), text ("House to half"),
                                           text (firstCue) }).applied == 1);
    REQUIRE (rig.apply (2, "cue.create", { text (mainList), osc::Value::int32 (1),
                                           text ("memo"), text ("Houselights out"),
                                           text (theGroup) }).applied == 1);

    REQUIRE (rig.canUndo());
    CHECK (rig.undoName() == "cue.create");

    //  The standby, written the way a client writes any other node.
    REQUIRE (rig.apply (10, "node.set", { text (cue::standbyAddressOf (mainList)),
                                          text (firstCue) }).applied == 1);

    CHECK (rig.document.getAttribute (cue::standbyAddressOf (mainList))
             == std::string (firstCue));
    CHECK (rig.undoName() == "cue.create");

    //  The lock, which is also a state row, and whose release has to get through.
    REQUIRE (rig.apply (11, "node.set", { text ("/godot/document/locked"),
                                          osc::Value::boolean (true) }).applied == 1);
    REQUIRE (rig.document.isLocked());
    REQUIRE (rig.apply (12, "node.set", { text ("/godot/document/locked"),
                                          osc::Value::boolean (false) }).applied == 1);
    REQUIRE_FALSE (rig.document.isLocked());
    CHECK (rig.undoName() == "cue.create");

    //  And a GO, which writes precisely one thing into the document: the pointer.
    REQUIRE (rig.apply (20, "go").applied == 1);

    CHECK (rig.document.getAttribute (cue::standbyAddressOf (mainList))
             == std::string (theGroup));
    CHECK (rig.undoName() == "cue.create");

    /*  Three edits and nothing else: the list and the two cues. The presses are
        not among them. */
    CHECK (rig.drainUndoSteps() == 3);
}

TEST_CASE ("undo: a locked show refuses undo and redo in their own handlers")
{
    /*  Undo knocks at none of the four doors — it writes through JUCE's own
        actions, underneath every predicate — so the refusal is in the handler
        and namespace draft §14.11 says why. */
    Rig rig;

    REQUIRE (rig.apply (0, "list.create", { text ("Main"), text (mainList) }).applied == 1);
    REQUIRE (rig.apply (1, "cue.create", { text (mainList), osc::Value::int32 (0),
                                           text ("memo"), text ("House to half"),
                                           text (firstCue) }).applied == 1);

    REQUIRE (rig.apply (2, "undo").applied == 1);
    CHECK_FALSE (rig.document.findById (firstCue).isValid());

    REQUIRE (rig.apply (3, "node.set", { text ("/godot/document/locked"),
                                         osc::Value::boolean (true) }).applied == 1);

    const auto lockedUndo = rig.apply (4, "undo");
    CHECK (lockedUndo.applied == 0);
    CHECK (lockedUndo.rejected == 1);
    CHECK (rig.engine.lastError().find (reason::locked) != std::string::npos);

    const auto lockedRedo = rig.apply (5, "redo");
    CHECK (lockedRedo.applied == 0);
    CHECK (lockedRedo.rejected == 1);

    //  And nothing moved while it was refused.
    CHECK_FALSE (rig.document.findById (firstCue).isValid());

    REQUIRE (rig.apply (6, "node.set", { text ("/godot/document/locked"),
                                         osc::Value::boolean (false) }).applied == 1);

    REQUIRE (rig.apply (7, "redo").applied == 1);
    CHECK (rig.document.findById (firstCue).isValid());
}

TEST_CASE ("undo: an empty history and an unknown domain are two different refusals")
{
    Rig rig;

    const auto empty = rig.apply (0, "undo");
    CHECK (empty.rejected == 1);
    CHECK (rig.engine.lastError().find (reason::nothingToUndo) != std::string::npos);

    const auto nothingBack = rig.apply (1, "redo");
    CHECK (nothingBack.rejected == 1);
    CHECK (rig.engine.lastError().find (reason::nothingToRedo) != std::string::npos);

    /*  A word the enum does not carry is a bad value and never a silent fall
        back to `document`: an operator who typed `parameters` a phase early has
        to be told the domain does not exist yet. */
    const auto wrongWord = rig.apply (2, "undo", { text ("parameters") });
    CHECK (wrongWord.rejected == 1);
    CHECK (rig.engine.lastError().find (reason::badValue) != std::string::npos);

    CHECK (doc::undoDomainWord (doc::UndoDomain::document) == "document");
    CHECK (doc::undoDomainForWord ("document").has_value());
    CHECK_FALSE (doc::undoDomainForWord ("Document").has_value());
    CHECK_FALSE (doc::undoDomainForWord ("").has_value());
}

TEST_CASE ("undo: the record carries the domain and the transaction it moved")
{
    /*  Replay compares one line against another and never compares the document
        or the stack, so an undo that popped a differently named transaction
        than the recorded session popped has to write a different line. That is
        what the applied arguments are for — and it is why the transaction name
        is a declared parameter as well as an output: a command whose own record
        fails its own arity check is a session that cannot reproduce itself. */
    Rig rig;

    REQUIRE (rig.apply (0, "list.create", { text ("Main"), text (mainList) }).applied == 1);
    REQUIRE (rig.apply (1, "cue.create", { text (mainList), osc::Value::int32 (0),
                                           text ("memo"), text ("House to half"),
                                           text (firstCue) }).applied == 1);

    REQUIRE (rig.apply (2, "undo").applied == 1);

    CHECK (rig.lastAppliedArg (0) == "document");
    CHECK (rig.lastAppliedArg (1) == "cue.create");

    REQUIRE (rig.apply (3, "redo").applied == 1);

    CHECK (rig.lastAppliedArg (0) == "document");
    CHECK (rig.lastAppliedArg (1) == "cue.create");

    /*  AND THE RECORD PASSES ITS OWN SIGNATURE, which is the half that would
        only be discovered by a replay: the two applied arguments are resubmitted
        as they stand. */
    const auto parsed = LogFile::parse (rig.engine.log().contents());
    REQUIRE (parsed.records.size() >= 4);

    const auto* undoCommand = rig.engine.commands().find ("undo");
    REQUIRE (undoCommand != nullptr);
    CHECK (undoCommand->mutates);

    const auto recheck = CommandRegistry::checkArgs (*undoCommand, parsed.records[2].args);
    CHECK (recheck.ok);
}

TEST_CASE ("undo: toVar's switch and the reader's copy of it agree, type for type")
{
    /*  THE HAZARD NO REVIEWER CAN SEE IN A DIFF. With a manager attached,
        ValueTree::setProperty guards on `var::equals`, which is type-loose, so a
        typed 1 written over a stored "1" is dropped with no refusal and no log
        record. Nothing fires today because every var in the document is built
        from a schema-parsed value — by `toVar` in the writer, and by a
        hand-written copy of the same switch in the reader. Nothing ENFORCES
        that the two agree, and the day one drifts is the day the first write to
        that attribute vanishes. So they are pinned against each other. */
    const std::string xml =
        "<Show>\n"
        "  <Lists>\n"
        "    <List id=\"7K2QM9X4\" name=\"Main\">\n"
        "      <Group id=\"D9FH2JKA\" name=\"Preshow\" loops=\"3\">\n"
        "        <Cue id=\"B3N8R5TW\" enabled=\"false\" name=\"House to half\" preWait=\"1.5\"/>\n"
        "      </Group>\n"
        "    </List>\n"
        "  </Lists>\n"
        "  <Mounts/>\n"
        "  <Audio tracks=\"4\"/>\n"
        "</Show>\n";

    doc::ShowDocument fromReader;
    const auto result = doc::CanonicalXml::read (xml, fromReader);

    for (const auto& problem : result.problems)
        INFO (problem);

    REQUIRE (result.ok);

    doc::ShowDocument fromWriter;
    REQUIRE (fromWriter.createList ("Main", mainList).ok);
    REQUIRE (fromWriter.createCue (mainList, 0, "group", "Preshow", theGroup).ok);
    REQUIRE (fromWriter.createCue (theGroup, 0, "memo", "House to half", firstCue).ok);
    REQUIRE (fromWriter.setAttribute ("/godot/cue/" + theGroup + "/loops", "3").ok);
    REQUIRE (fromWriter.setAttribute ("/godot/cue/" + firstCue + "/enabled", "false").ok);
    REQUIRE (fromWriter.setAttribute ("/godot/cue/" + firstCue + "/preWait", "1.5").ok);

    const auto agree = [&] (const std::string& id, const char* attribute, const char* expected)
    {
        const juce::Identifier property { attribute };
        const auto reader = fromReader.findById (id)[property];
        const auto writer = fromWriter.findById (id)[property];

        INFO (attribute << ": reader " << varType (reader) << ", writer " << varType (writer));

        CHECK (varType (reader) == std::string (expected));
        CHECK (varType (writer) == std::string (expected));
    };

    //  One row of every type the two switches have an arm for.
    agree (firstCue, "name", "string");
    agree (firstCue, "enabled", "bool");
    agree (firstCue, "preWait", "double");
    agree (theGroup, "loops", "int64");

    /*  And the identifier, which neither switch writes: `insertObject` puts it
        in as a raw juce::String, so the reader has to as well or the first
        rewrite of an `id` would be the silent drop. */
    agree (firstCue, "id", "string");

    /*  ValueType::integer64 has no `persist == show` row to test with — nothing
        a document holds is declared `h` — and the writer's switch answers it in
        the same arm as `integer`, so the pin above covers both. A list-typed
        row (`d*`) is not testable either: the write choke point cannot write a
        list yet, so there is only one writer of one and nothing to agree with.
        Both are here so that the row which arrives first is added to this case
        rather than found by a dropped write. */
}

TEST_CASE ("undo: nothing published and nothing logged carries a wall-clock time")
{
    /*  UndoManager stamps each transaction with Time::getCurrentTime(), and that
        read is sanctioned only because the stamp is stored and never observed.
        This is the other end of that bargain: what reaches a client and what
        reaches the log are asserted whole, so a field carrying a time could not
        be added without failing here. */
    Rig rig;

    tree::MountTable mounts;
    tree::ParameterTree parameters { rig.document, rig.engine.commands(), mounts, rig.runs };

    REQUIRE (rig.apply (0, "list.create", { text ("Main"), text (mainList) }).applied == 1);
    REQUIRE (rig.apply (1, "cue.create", { text (mainList), osc::Value::int32 (0),
                                           text ("memo"), text ("House to half"),
                                           text (firstCue) }).applied == 1);
    REQUIRE (rig.apply (2, "node.set", { text ("/godot/cue/" + firstCue + "/name"),
                                         text ("House to full") }).applied == 1);
    REQUIRE (rig.apply (3, "undo").applied == 1);

    tree::EngineState state;
    state.version = "test";
    state.tick = 3;

    const auto& undoHistory = rig.document.history (doc::UndoDomain::document);

    state.documentCanUndo = undoHistory.canUndo();
    state.documentCanRedo = undoHistory.canRedo();
    state.documentUndoName = undoHistory.getUndoDescription().toStdString();
    state.documentRedoName = undoHistory.getRedoDescription().toStdString();

    parameters.markStale();

    const auto snapshot = parameters.publish (3, state);
    REQUIRE (snapshot != nullptr);

    const auto valueAt = [&] (const std::string& address)
    {
        const auto* node = snapshot->find (address);
        REQUIRE_MESSAGE (node != nullptr, "missing node: " << address);
        const auto value = node->soleValue();
        REQUIRE_MESSAGE (value.has_value(), "no value at: " << address);
        return *value;
    };

    CHECK (valueAt ("/godot/document/canUndo") == osc::Value::boolean (true));
    CHECK (valueAt ("/godot/document/canRedo") == osc::Value::boolean (true));

    /*  THE COMMAND'S NAME AND NOTHING ELSE — no clock, no tick, no operator. It
        is the word a client puts after "Undo". */
    CHECK (valueAt ("/godot/document/undoName") == osc::Value::string ("cue.create"));
    CHECK (valueAt ("/godot/document/redoName") == osc::Value::string ("node.set"));

    //  And the whole log line, field for field, with nowhere for a stamp to hide.
    const auto parsed = LogFile::parse (rig.engine.log().contents());
    REQUIRE (parsed.records.size() == 4);

    const auto& undone = parsed.records[3];

    CHECK (undone.toLine() == "A " + std::to_string (undone.tick) + " "
                                + std::to_string (undone.seq)
                                + " cli undo s:\"document\" s:\"node.set\"");
}

TEST_CASE ("undo: a hatch is not an edit, and a moved document starts with an empty history")
{
    /*  `adopt` replaces the show rather than editing it, and every action on the
        stack holds a handle into the graph it replaces — so an uncleared stack
        would keep the previous show alive in memory and undo into a tree nobody
        can see. */
    Rig rig;

    REQUIRE (rig.apply (0, "list.create", { text ("Main"), text (mainList) }).applied == 1);
    REQUIRE (rig.apply (1, "cue.create", { text (mainList), osc::Value::int32 (0),
                                           text ("memo"), text ("House to half"),
                                           text (firstCue) }).applied == 1);
    REQUIRE (rig.canUndo());

    const std::string xml =
        "<Show>\n"
        "  <Lists>\n"
        "    <List id=\"7K2QM9X4\" name=\"Loaded\"/>\n"
        "  </Lists>\n"
        "  <Mounts/>\n"
        "  <Audio tracks=\"0\"/>\n"
        "</Show>\n";

    const auto result = doc::CanonicalXml::read (xml, rig.document);

    for (const auto& problem : result.problems)
        INFO (problem);

    REQUIRE (result.ok);

    CHECK_FALSE (rig.canUndo());
    CHECK (rig.undoName().empty());
    CHECK_FALSE (rig.document.findById (firstCue).isValid());

    /*  A MOVE REBUILDS THE HISTORIES EMPTY rather than carrying them, which is
        the answer to `juce::UndoManager` being neither copyable nor movable and
        to this class hand-writing its move. */
    doc::ShowDocument source;
    REQUIRE (source.createList ("Main", mainList).ok);
    REQUIRE (source.setAttribute ("/godot/list/" + mainList + "/name", "Renamed").ok);

    doc::ShowDocument moved { std::move (source) };

    CHECK_FALSE (moved.history (doc::UndoDomain::document).canUndo());
    CHECK (moved.getAttribute ("/godot/list/" + mainList + "/name")
             == std::string ("Renamed"));
    CHECK (moved.ids().isTaken (mainList));
}

TEST_CASE ("undo: a suppressed scope writes nothing onto the stack")
{
    doc::ShowDocument document;

    {
        const doc::ShowDocument::ScopedUndoSuppression setup { document };
        REQUIRE (document.createList ("Main", mainList).ok);
    }

    CHECK_FALSE (document.history (doc::UndoDomain::document).canUndo());

    /*  Counted, so that scopes may nest — Phase 6's cue-driven recall will open
        one inside another and neither may end the suppression the other is
        relying on. Today it guards `adopt` and nothing else, which is said
        plainly so that a reviewer finding one call site does not go looking for
        the ones that are missing. */
    {
        const doc::ShowDocument::ScopedUndoSuppression outer { document };

        {
            const doc::ShowDocument::ScopedUndoSuppression inner { document };

            document.beginTransaction ("node.set", 0, "cli",
                                       { text ("/godot/list/" + mainList + "/name"),
                                         text ("Inner") });
            REQUIRE (document.setAttribute ("/godot/list/" + mainList + "/name", "Inner").ok);
        }

        document.beginTransaction ("node.set", 100, "cli",
                                   { text ("/godot/list/" + mainList + "/name"),
                                     text ("Outer") });
        REQUIRE (document.setAttribute ("/godot/list/" + mainList + "/name", "Outer").ok);
    }

    CHECK_FALSE (document.history (doc::UndoDomain::document).canUndo());

    //  And the moment the count reaches nought, writes are recorded again.
    document.beginTransaction ("node.set", 200, "cli",
                               { text ("/godot/list/" + mainList + "/name"),
                                 text ("After") });
    REQUIRE (document.setAttribute ("/godot/list/" + mainList + "/name", "After").ok);

    REQUIRE (document.history (doc::UndoDomain::document).canUndo());
    REQUIRE (document.undo (doc::UndoDomain::document) == std::string ("node.set"));
    CHECK (document.getAttribute ("/godot/list/" + mainList + "/name")
             == std::string ("Outer"));
}
