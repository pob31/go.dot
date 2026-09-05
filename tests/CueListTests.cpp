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
    The standby pointer: where GO will act, and which list it will act on.

    THREE OF THESE TESTS ARE NAMED FOR A CHOICE RATHER THAN A RULE, because the
    sources do not settle them and the author did (2026-09-06): a group is an
    opaque sibling, `next` and `previous` stay put from an empty standby, and a
    disabled cue is not skipped. Each is a Phase 1 answer that Phase 3 may
    revisit when there is a GO to justify a different one, and each says so
    where it is asserted rather than in a document nobody opens.

    A serialisation surface at its edges - standby is persisted, and replay
    fixture #2 compares both files - so every case runs under fr_FR as well as C.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/EphemeralState.h>
#include <wfg/engine/log/Replay.h>

#include <juce_core/juce_core.h>

#include <string>
#include <vector>

using namespace wfg;
using namespace wfg::cue;

namespace
{
    /*  The committed bundle: one list, a cue and a group side by side at the
        top level, and two more cues inside the group. That shape is what makes
        "a group is one sibling" testable - E4GP6QSC and F7HR8TVD exist and
        traversal must never reach them. */
    const std::string mainList = "7K2QM9X4";
    const std::string houseToHalf = "B3N8R5TW";       // top-level cue
    const std::string preshow = "D9FH2JKA";           // top-level GROUP
    const std::string walkIn = "E4GP6QSC";            // inside the group
    const std::string announce = "F7HR8TVD";          // inside the group
    const std::string wfsMount = "G1JS4VWE";

    juce::File fixtureBundle()
    {
        const juce::File folder { juce::String (std::string (WFG_TEST_FIXTURES_DIR))
                                    + "/bundles/minimal" };

        REQUIRE_MESSAGE (folder.isDirectory(), "missing fixture bundle: "
                                                 << folder.getFullPathName());
        return folder;
    }

    struct Rig
    {
        explicit Rig (bool openFixture = true)
        {
            if (openFixture)
                REQUIRE (doc::Bundle::open (fixtureBundle(), document).ok);

            doc::registerDocumentCommands (engine.commands(), document);
            registerCueCommands (engine.commands(), document, focus);
        }

        /** Applies one command and returns its result. */
        Engine::TickResult run (std::int64_t tick, const std::string& command,
                                std::vector<osc::Value> args = {})
        {
            REQUIRE (engine.submit (origin::cli, command, std::move (args)));
            return engine.processTick (tick);
        }

        std::string standbyOf (const std::string& listId) const
        {
            return document.getAttribute ("/godot/list/" + listId + "/standby")
                     .value_or (std::string ("<unresolved>"));
        }

        juce::ValueTree listNode (const std::string& listId) const
        {
            return document.findById (listId);
        }

        Engine engine;
        doc::ShowDocument document;
        Focus focus;
    };
}

//==============================================================================
TEST_CASE ("standby: traversal walks the top-level children, in order")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    Rig rig;
    const auto list = rig.listNode (mainList);

    CHECK (childrenOf (list) == std::vector<std::string> { houseToHalf, preshow });

    CHECK (nextOf (list, houseToHalf) == preshow);
    CHECK (previousOf (list, preshow) == houseToHalf);

    CHECK (isTopLevelChild (list, houseToHalf));
    CHECK (isTopLevelChild (list, preshow));
    CHECK_FALSE (isTopLevelChild (list, walkIn));       // it is inside the group
}

TEST_CASE ("standby: a group is one sibling and is never descended into (a Phase 1 choice)")
{
    /*  PRD §3.6 says the pointer descends into a manual sequence group, and
        Phase 3 will do that. A Phase 1 group has no runtime behaviour to
        descend into, and the namespace draft's standby.set constraint already
        requires a top-level child - so traversal steps over it as one sibling.

        Named for the choice, not for a rule: when Phase 3 makes this fail, the
        failure is the point. */
    Rig rig;
    const auto list = rig.listNode (mainList);

    // The group is reached...
    CHECK (nextOf (list, houseToHalf) == preshow);

    // ...and stepping again leaves the list, rather than entering the group.
    CHECK (nextOf (list, preshow) == preshow);

    // Its children are never a destination.
    CHECK (nextOf (list, walkIn) == walkIn);
    CHECK (previousOf (list, announce) == announce);
}

TEST_CASE ("standby: at either end it stays put, and from empty it stays empty")
{
    /*  "Next past the end stays put" is the approved plan's. Staying put from
        EMPTY is the author's (2026-09-06), and it is what makes standby.set the
        only thing that arms a list: there is no gesture that turns "nowhere"
        into "the first cue", and no wrap anywhere. */
    Rig rig;
    const auto list = rig.listNode (mainList);

    CHECK (nextOf (list, preshow) == preshow);              // last, stays
    CHECK (previousOf (list, houseToHalf) == houseToHalf);  // first, stays

    CHECK (nextOf (list, "") == "");                        // empty, stays empty
    CHECK (previousOf (list, "") == "");
}

TEST_CASE ("standby: a disabled cue is not skipped (a Phase 1 choice)")
{
    /*  A disabled cue is still a row in the list. Skipping is a
        running-behaviour decision, and Phase 1 has no runner to justify it;
        Phase 3 revisits it when a GO that does nothing becomes a real failure.
        Asserted so the choice is visible rather than incidental. */
    Rig rig;

    REQUIRE (rig.run (1, "node.set",
                      { osc::Value::string ("/godot/cue/" + preshow + "/enabled"),
                        osc::Value::boolean (false) }).applied == 1);

    const auto list = rig.listNode (mainList);
    CHECK (nextOf (list, houseToHalf) == preshow);
}

//==============================================================================
TEST_CASE ("standby.set: it parks on a top-level cue of the focused list, and nothing else")
{
    Rig rig;

    CHECK (rig.standbyOf (mainList) == houseToHalf);        // as state.xml left it

    CHECK (rig.run (1, "standby.set", { osc::Value::string (preshow) }).applied == 1);
    CHECK (rig.standbyOf (mainList) == preshow);

    // A group is a legal target: a Group is a Cue.
    CHECK (rig.run (2, "standby.set", { osc::Value::string (preshow) }).applied == 1);

    /*  Setting the standby it already holds is applied, not refused. A surface
        that sends the same gesture twice is doing something reasonable. */
    CHECK (rig.standbyOf (mainList) == preshow);
}

TEST_CASE ("standby.set: the cue must exist, be a cue, and be at the list's top level")
{
    Rig rig;

    // Nothing with that identifier.
    CHECK (rig.run (1, "standby.set", { osc::Value::string ("ZZZZZZZZ") }).rejected == 1);
    CHECK (rig.engine.lastError().find (reason::unknownId) != std::string::npos);

    // A mount is not a cue, however valid its identifier.
    CHECK (rig.run (2, "standby.set", { osc::Value::string (wfsMount) }).rejected == 1);
    CHECK (rig.engine.lastError().find (reason::unknownId) != std::string::npos);

    /*  A real cue, in this list, but nested inside a group - which is where the
        opaque-sibling choice and the referential invariant meet. */
    CHECK (rig.run (3, "standby.set", { osc::Value::string (walkIn) }).rejected == 1);
    CHECK (rig.engine.lastError().find (reason::notInList) != std::string::npos);

    // And none of it moved the standby.
    CHECK (rig.standbyOf (mainList) == houseToHalf);
}

TEST_CASE ("standby.clear: an empty standby is a resting state, not a failure")
{
    Rig rig;

    CHECK (rig.run (1, "standby.clear").applied == 1);
    CHECK (rig.standbyOf (mainList) == "");

    // Clearing an already-empty one is applied too.
    CHECK (rig.run (2, "standby.clear").applied == 1);
    CHECK (rig.standbyOf (mainList) == "");

    // And from there, traversal stays put rather than arming the list.
    CHECK (rig.run (3, "standby.next").applied == 1);
    CHECK (rig.standbyOf (mainList) == "");
}

TEST_CASE ("standby: with no list at all, the commands are refused rather than silently applied")
{
    /*  A pointer with nowhere to GO is applied - there is a list, and the
        command did what it does. A command on a list when there is no list is a
        different thing, and saying so is what keeps the first case honest. */
    Rig rig { false };
    REQUIRE (rig.document.root().getChildWithName ("Lists").getNumChildren() == 0);

    for (const auto& command : { "standby.next", "standby.previous", "standby.clear" })
    {
        INFO ("command: " << command);
        CHECK (rig.run (1, command).rejected == 1);
        CHECK (rig.engine.lastError().find (reason::notInList) != std::string::npos);
    }

    CHECK (rig.run (2, "standby.set", { osc::Value::string ("ZZZZZZZZ") }).rejected == 1);
}

//==============================================================================
TEST_CASE ("standby: a direct node write goes through the same invariant")
{
    /*  The namespace makes /godot/list/<id>/standby a read-write node, so a
        client can write it without knowing the command vocabulary. It had
        better be checked identically - and before this PR a bare string row
        accepted anything at all. */
    Rig rig;
    const auto address = standbyAddressOf (mainList);

    CHECK (rig.run (1, "node.set",
                    { osc::Value::string (address), osc::Value::string (preshow) }).applied == 1);
    CHECK (rig.standbyOf (mainList) == preshow);

    // A word that is not an identifier.
    CHECK (rig.run (2, "node.set",
                    { osc::Value::string (address), osc::Value::string ("banana") }).rejected == 1);
    CHECK (rig.engine.lastError().find (reason::notInList) != std::string::npos);

    // A real cue that is not a top-level child of THIS list.
    CHECK (rig.run (3, "node.set",
                    { osc::Value::string (address), osc::Value::string (walkIn) }).rejected == 1);

    CHECK (rig.standbyOf (mainList) == preshow);

    // Emptying it is always legal.
    CHECK (rig.run (4, "node.set",
                    { osc::Value::string (address), osc::Value::string ("") }).applied == 1);
    CHECK (rig.standbyOf (mainList) == "");
}

TEST_CASE ("standby: one list cannot be parked on another list's cue")
{
    /*  The discrimination the invariant actually performs. Every other test of
        it uses a value that is illegal for EVERY list - a word, a nested cue, a
        mount - so a predicate that searched the whole show instead of this list
        would pass them all. This is the case that tells the two apart, and the
        corruption it prevents is a standby that GO on this list could never
        reach. */
    Rig rig;

    REQUIRE (rig.run (1, "list.create", { osc::Value::string ("Second") }).applied == 1);

    const auto secondList = rig.document.root().getChildWithName ("Lists").getChild (1)
                              .getProperty ("id").toString().toStdString();

    REQUIRE (rig.run (2, "cue.create",
                      { osc::Value::string (secondList), osc::Value::int32 (0),
                        osc::Value::string ("memo"),
                        osc::Value::string ("Elsewhere") }).applied == 1);

    const auto elsewhere = rig.document.findById (secondList).getChild (0)
                             .getProperty ("id").toString().toStdString();

    /*  A perfectly good cue, at the top level of a perfectly good list - just
        not THIS one. */
    CHECK (rig.run (3, "node.set",
                    { osc::Value::string (standbyAddressOf (mainList)),
                      osc::Value::string (elsewhere) }).rejected == 1);
    CHECK (rig.engine.lastError().find (reason::notInList) != std::string::npos);
    CHECK (rig.standbyOf (mainList) == houseToHalf);

    // And the command door refuses it for the same reason.
    REQUIRE (rig.focus.listId (rig.document) == mainList);
    CHECK (rig.run (4, "standby.set", { osc::Value::string (elsewhere) }).rejected == 1);
    CHECK (rig.engine.lastError().find (reason::notInList) != std::string::npos);
    CHECK (rig.standbyOf (mainList) == houseToHalf);

    // The other way round too, so this is not an accident of ordering.
    CHECK (rig.run (5, "node.set",
                    { osc::Value::string (standbyAddressOf (secondList)),
                      osc::Value::string (houseToHalf) }).rejected == 1);
    CHECK (rig.standbyOf (secondList) == "");
}

TEST_CASE ("standby: a direct write works on a list that is not focused")
{
    /*  The load path depends on it. EphemeralState restores every list's
        standby with no focus involved, so refusing a non-focused list's node
        write would make an rw node unwritable for every list but one and would
        break opening a show with two of them. */
    Rig rig;

    REQUIRE (rig.run (1, "list.create", { osc::Value::string ("Second") }).applied == 1);

    const auto secondList = rig.document.root().getChildWithName ("Lists").getChild (1)
                              .getProperty ("id").toString().toStdString();

    REQUIRE (rig.run (2, "cue.create",
                      { osc::Value::string (secondList), osc::Value::int32 (0),
                        osc::Value::string ("memo"), osc::Value::string ("Standalone") }).applied == 1);

    const auto otherCue = rig.document.findById (secondList).getChild (0)
                            .getProperty ("id").toString().toStdString();

    // The first list is still the focused one.
    REQUIRE (rig.focus.listId (rig.document) == mainList);

    CHECK (rig.run (3, "node.set",
                    { osc::Value::string (standbyAddressOf (secondList)),
                      osc::Value::string (otherCue) }).applied == 1);

    CHECK (rig.standbyOf (secondList) == otherCue);
}

//==============================================================================
TEST_CASE ("standby: deleting the cue it is parked on advances it to the next one")
{
    /*  The author's choice (2026-09-06): during tech, deleting the cue you are
        parked on leaves you parked on the next one. Done inside the applied
        command, so a replay reproduces it without a repair record nobody sent. */
    Rig rig;
    REQUIRE (rig.standbyOf (mainList) == houseToHalf);

    CHECK (rig.run (1, "object.delete", { osc::Value::string (houseToHalf) }).applied == 1);
    CHECK (rig.standbyOf (mainList) == preshow);
}

TEST_CASE ("standby: deleting the last cue it is parked on empties it")
{
    Rig rig;

    REQUIRE (rig.run (1, "standby.set", { osc::Value::string (preshow) }).applied == 1);
    REQUIRE (rig.standbyOf (mainList) == preshow);

    CHECK (rig.run (2, "object.delete", { osc::Value::string (preshow) }).applied == 1);
    CHECK (rig.standbyOf (mainList) == "");
}

TEST_CASE ("standby: deleting some other cue leaves it exactly where it was")
{
    Rig rig;
    REQUIRE (rig.standbyOf (mainList) == houseToHalf);

    // A cue inside the group: not the standby, and not a sibling of it.
    CHECK (rig.run (1, "object.delete", { osc::Value::string (walkIn) }).applied == 1);
    CHECK (rig.standbyOf (mainList) == houseToHalf);

    // And a whole group that is not the standby.
    CHECK (rig.run (2, "object.delete", { osc::Value::string (preshow) }).applied == 1);
    CHECK (rig.standbyOf (mainList) == houseToHalf);
}

TEST_CASE ("standby: moving the cue out of the list's top level clears it")
{
    /*  Advancing would be guessing that the operator meant to stay where they
        were. Clearing says plainly that what they were parked on has gone
        somewhere else. */
    Rig rig;
    REQUIRE (rig.standbyOf (mainList) == houseToHalf);

    CHECK (rig.run (1, "object.move",
                    { osc::Value::string (houseToHalf), osc::Value::string (preshow),
                      osc::Value::int32 (0) }).applied == 1);

    CHECK (rig.standbyOf (mainList) == "");
}

TEST_CASE ("standby: reordering the list does not move it")
{
    /*  A regression guard rather than a proof of PRD §3.5's "never as a side
        effect of selection or scrolling" - the pointer stores an identifier,
        so a reorder cannot move it by construction and this would stay green
        under any implementation that kept that. It is here because storing an
        index instead is the obvious mistake, and this is what would catch it. */
    Rig rig;
    REQUIRE (rig.standbyOf (mainList) == houseToHalf);

    CHECK (rig.run (1, "object.move",
                    { osc::Value::string (houseToHalf), osc::Value::string (mainList),
                      osc::Value::int32 (1) }).applied == 1);

    const auto list = rig.listNode (mainList);

    CHECK (childrenOf (list) == std::vector<std::string> { preshow, houseToHalf });
    CHECK (rig.standbyOf (mainList) == houseToHalf);        // same cue, new position
    CHECK (previousOf (list, houseToHalf) == preshow);      // and traversal followed the list
}

//==============================================================================
TEST_CASE ("focus: it falls back to the first list rather than being maintained")
{
    /*  Runtime and resolved, not stored and maintained (author, 2026-09-06).
        Nothing has to remember to move it when a list appears or disappears,
        which is what makes "exactly one list is focused whenever a list exists"
        true by construction. */
    Rig rig;

    // Never requested: the first list.
    CHECK (rig.focus.requested().empty());
    CHECK (rig.focus.listId (rig.document) == mainList);

    REQUIRE (rig.run (1, "list.create", { osc::Value::string ("Second") }).applied == 1);

    const auto secondList = rig.document.root().getChildWithName ("Lists").getChild (1)
                              .getProperty ("id").toString().toStdString();

    // Creating a list does not steal focus.
    CHECK (rig.focus.listId (rig.document) == mainList);

    CHECK (rig.run (2, "list.focus", { osc::Value::string (secondList) }).applied == 1);
    CHECK (rig.focus.listId (rig.document) == secondList);

    /*  Exclusive by construction: it is one value, so focusing another does not
        leave a flag set anywhere. */
    CHECK (rig.run (3, "list.focus", { osc::Value::string (mainList) }).applied == 1);
    CHECK (rig.focus.listId (rig.document) == mainList);

    // Deleting the focused list falls back rather than leaving it dangling.
    CHECK (rig.run (4, "list.focus", { osc::Value::string (secondList) }).applied == 1);
    CHECK (rig.run (5, "object.delete", { osc::Value::string (secondList) }).applied == 1);
    CHECK (rig.focus.listId (rig.document) == mainList);
}

TEST_CASE ("focus: it must name a list, not a cue and not nothing")
{
    /*  Every rejection is checked while the focus is on the SECOND list, not
        the first. Asserting the first would prove nothing: it is also the
        fallback, so a request that cleared the focus on the way to failing
        would be indistinguishable from one that left it alone. */
    Rig rig;

    REQUIRE (rig.run (1, "list.create", { osc::Value::string ("Second") }).applied == 1);

    const auto secondList = rig.document.root().getChildWithName ("Lists").getChild (1)
                              .getProperty ("id").toString().toStdString();

    REQUIRE (rig.run (2, "list.focus", { osc::Value::string (secondList) }).applied == 1);
    REQUIRE (rig.focus.listId (rig.document) == secondList);

    // Nothing with that identifier.
    CHECK (rig.run (3, "list.focus", { osc::Value::string ("ZZZZZZZZ") }).rejected == 1);
    CHECK (rig.engine.lastError().find (reason::unknownId) != std::string::npos);
    CHECK (rig.focus.listId (rig.document) == secondList);

    // A cue, and a mount: valid identifiers naming the wrong kind of object.
    CHECK (rig.run (4, "list.focus", { osc::Value::string (houseToHalf) }).rejected == 1);
    CHECK (rig.focus.listId (rig.document) == secondList);

    CHECK (rig.run (5, "list.focus", { osc::Value::string (wfsMount) }).rejected == 1);
    CHECK (rig.focus.listId (rig.document) == secondList);

    // And the failures left nothing behind, not even an unresolvable request.
    CHECK (rig.focus.requested() == secondList);
}

TEST_CASE ("standby: the commands act on the focused list and on no other")
{
    /*  With a second list that has children AND a standby of its own, so that
        an implementation walking every list would be caught rather than
        passing on an empty one. */
    Rig rig;

    REQUIRE (rig.run (1, "list.create", { osc::Value::string ("Second") }).applied == 1);

    const auto secondList = rig.document.root().getChildWithName ("Lists").getChild (1)
                              .getProperty ("id").toString().toStdString();

    for (int i = 0; i < 3; ++i)
        REQUIRE (rig.run (2 + i, "cue.create",
                          { osc::Value::string (secondList), osc::Value::int32 (i),
                            osc::Value::string ("memo"),
                            osc::Value::string ("Cue " + std::to_string (i)) }).applied == 1);

    const auto secondNode = rig.document.findById (secondList);
    const auto secondChildren = childrenOf (secondNode);
    REQUIRE (secondChildren.size() == 3);

    // Arm the second list at its middle cue, through the node it publishes.
    REQUIRE (rig.run (10, "node.set",
                      { osc::Value::string (standbyAddressOf (secondList)),
                        osc::Value::string (secondChildren[1]) }).applied == 1);

    REQUIRE (rig.focus.listId (rig.document) == mainList);
    REQUIRE (rig.standbyOf (mainList) == houseToHalf);

    // Traverse the focused list, twice.
    CHECK (rig.run (11, "standby.next").applied == 1);
    CHECK (rig.run (12, "standby.next").applied == 1);

    CHECK (rig.standbyOf (mainList) == preshow);              // moved, then stayed at the end
    CHECK (rig.standbyOf (secondList) == secondChildren[1]);  // and the other did not move

    // Now focus the second and traverse it; the first must hold still.
    CHECK (rig.run (13, "list.focus", { osc::Value::string (secondList) }).applied == 1);
    CHECK (rig.run (14, "standby.previous").applied == 1);

    CHECK (rig.standbyOf (secondList) == secondChildren[0]);
    CHECK (rig.standbyOf (mainList) == preshow);
}

//==============================================================================
TEST_CASE ("standby: a saved state naming a cue that is not there is reported and skipped")
{
    /*  The invariant lives at the single write door, so the load path gets it
        too - which is the case that matters, because a state file written
        against a different show is exactly where a nonsense standby comes
        from. The show still opens; the pointer does not. */
    Rig rig;

    const auto stateText = "<State formatVersion=\"1\">\n"
                           "  <List id=\"" + mainList + "\" standby=\"" + walkIn + "\"/>\n"
                           "</State>\n";

    doc::ShowDocument reopened;
    REQUIRE (doc::Bundle::open (fixtureBundle(), reopened).ok);
    REQUIRE (reopened.setAttribute (standbyAddressOf (mainList), "").ok);

    const auto result = doc::EphemeralState::read (stateText, reopened);

    CHECK_FALSE (result.ok);
    REQUIRE (! result.problems.empty());
    INFO ("problem: " << result.problems.front());
    CHECK (result.problems.front().find ("standby") != std::string::npos);

    // The show is intact and the standby simply did not take.
    CHECK (reopened.getAttribute ("/godot/cue/" + houseToHalf + "/name")
             == std::string ("House to half"));
    CHECK (reopened.getAttribute (standbyAddressOf (mainList)) == std::string (""));
}

//==============================================================================
TEST_CASE ("replay: a standby session reproduces itself, its show and its state")
{
    /*  Replay fixture #2, and it makes one claim more than fixture #1 did: the
        log reproduces itself record for record, the SHOW it arrives at is
        byte-identical, and so is the STATE - which is where the standby lives.
        A replay that reproduced the cues and lost the pointer would be a replay
        of a different session. */
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    const auto record = [] (Engine& engine, doc::ShowDocument& document, Focus& focus)
    {
        engine.submit (origin::cli, "list.create", { osc::Value::string ("Main") });
        engine.processTick (0);

        const auto listId = document.root().getChildWithName ("Lists").getChild (0)
                              .getProperty ("id").toString().toStdString();

        for (int i = 0; i < 3; ++i)
            engine.submit (origin::cli, "cue.create",
                           { osc::Value::string (listId), osc::Value::int32 (i),
                             osc::Value::string (i == 1 ? "group" : "memo"),
                             osc::Value::string ("Cue " + std::to_string (i)) });

        engine.processTick (4);

        const auto ids = childrenOf (document.findById (listId));

        /*  A cue INSIDE the group, so the not-in-list refusal below is really
            that refusal and not an unknown identifier wearing its coat. */
        engine.submit (origin::cli, "cue.create",
                       { osc::Value::string (ids[1]), osc::Value::int32 (0),
                         osc::Value::string ("memo"), osc::Value::string ("Nested") });
        engine.processTick (6);

        const auto nested = childrenOf (document.findById (ids[1])).front();

        engine.submit (origin::cli, "list.focus", { osc::Value::string (listId) });
        engine.submit (origin::cli, "standby.set", { osc::Value::string (ids[0]) });
        engine.submit ("ws:127.0.0.1:51234", "standby.next");     // -> the group
        engine.submit (origin::cli, "standby.next");              // -> the third cue
        engine.processTick (9);

        // Past the end: applied, and it stays put.
        engine.submit (origin::cli, "standby.next");

        // Refused: a cue inside the group is not a top-level child.
        engine.submit (origin::cli, "standby.set", { osc::Value::string (nested) });

        engine.submit (origin::cli, "standby.previous");           // -> back to the group
        engine.processTick (14);

        /*  THE REPAIR, and it has to be the cue the standby is actually on -
            which after that sequence is the GROUP, ids[1], not ids[0]. Deleting
            anything else would leave the repair branch in ShowDocument::remove()
            unentered and this fixture would pass with the whole repair deleted. */
        engine.submit (origin::cli, "object.delete", { osc::Value::string (ids[1]) });
        engine.processTick (20);

        (void) focus;
    };

    Engine session;
    doc::ShowDocument sessionDocument;
    Focus sessionFocus;
    doc::registerDocumentCommands (session.commands(), sessionDocument);
    registerCueCommands (session.commands(), sessionDocument, sessionFocus);
    session.log().openInMemory ({});
    record (session, sessionDocument, sessionFocus);

    const auto sessionShow = doc::CanonicalXml::write (sessionDocument);
    const auto sessionState = doc::EphemeralState::write (sessionDocument);
    const auto original = LogFile::parse (session.log().contents());

    REQUIRE (original.errors.empty());
    REQUIRE (original.records.size() == 13);

    /*  The session must END somewhere specific, or the state comparison below
        would be comparing two empty files and passing. The last thing it did
        was delete the cue it was parked on, so the repair must have moved the
        pointer to the third cue - and that is the claim the replay then has to
        reproduce without any repair record in the log. */
    const auto sessionIds = childrenOf (sessionDocument.root()
                                          .getChildWithName ("Lists").getChild (0));

    REQUIRE (sessionIds.size() == 2);       // the group went with the delete

    INFO ("state.xml: " << sessionState);
    CHECK (sessionState.find ("standby=\"" + sessionIds[1] + "\"") != std::string::npos);

    Engine fresh;
    doc::ShowDocument freshDocument;
    Focus freshFocus;
    doc::registerDocumentCommands (fresh.commands(), freshDocument);
    registerCueCommands (fresh.commands(), freshDocument, freshFocus);

    const auto result = replay (fresh, original);

    for (const auto& mismatch : result.mismatches)
        INFO (mismatch);

    CHECK (result.ok);
    CHECK (result.producedLog == session.log().contents());

    CHECK (doc::CanonicalXml::write (freshDocument) == sessionShow);
    CHECK (doc::EphemeralState::write (freshDocument) == sessionState);
}
