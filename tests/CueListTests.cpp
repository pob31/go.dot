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

    /*  The group is a MANUAL sequence by default, so the pointer goes inside
        it - to its first member rather than to the group row (decision M). */
    CHECK (nextStandby (list, houseToHalf) == walkIn);
    CHECK (previousStandby (list, walkIn) == houseToHalf);

    CHECK (isTopLevelChild (list, houseToHalf));
    CHECK (isTopLevelChild (list, preshow));
    CHECK_FALSE (isTopLevelChild (list, walkIn));       // it is inside the group

    // ...but it IS somewhere the pointer may stand, which is a different question.
    CHECK (isOnManualPath (list, walkIn));
}

TEST_CASE ("standby: a manual group IS descended into, which was the Phase 1 choice reversed")
{
    /*  THE FAILURE THIS TEST WAS NAMED FOR. Phase 1 asserted the opposite and
        said so in as many words: "a Phase 1 group has no runtime behaviour to
        descend into ... Named for the choice, not for a rule: when Phase 3
        makes this fail, the failure is the point."

        It has. PRD §3.6: in a manual sequence group "a member starts on GO. The
        standby pointer DESCENDS INTO the group; the operator is the parent."
        The fixture's group is a manual sequence, because that is what both
        attributes default to. */
    Rig rig;
    const auto list = rig.listNode (mainList);

    // Into it, one member at a time - the group ROW is not a stop of its own.
    CHECK (nextStandby (list, houseToHalf) == walkIn);
    CHECK (nextStandby (list, walkIn) == announce);

    // And out again: after the last member comes whatever follows the group.
    CHECK (nextStandby (list, announce) == announce);   // it is the end of the list
    CHECK (previousStandby (list, announce) == walkIn);
    CHECK (previousStandby (list, walkIn) == houseToHalf);
}

TEST_CASE ("standby: at either end it stays put, and from empty it stays empty")
{
    /*  "Next past the end stays put" is the approved plan's. Staying put from
        EMPTY is the author's (2026-09-06), and it is what makes standby.set the
        only thing that arms a list: there is no gesture that turns "nowhere"
        into "the first cue", and no wrap anywhere. */
    Rig rig;
    const auto list = rig.listNode (mainList);

    CHECK (nextStandby (list, announce) == announce);       // last, stays
    CHECK (previousStandby (list, houseToHalf) == houseToHalf);  // first, stays

    CHECK (nextStandby (list, "") == "");                   // empty, stays empty
    CHECK (previousStandby (list, "") == "");
}

TEST_CASE ("standby: a disabled cue is not skipped (a Phase 1 choice)")
{
    /*  THE OTHER CHOICE PHASE 1 NAMED FOR THIS MOMENT: "a disabled cue is
        still a row in the list. Skipping is a running-behaviour decision, and
        Phase 1 has no runner to justify it; Phase 3 revisits it when a GO that
        does nothing becomes a real failure rather than a hypothetical one."

        It has a runner now, and the runner already skips a disabled member
        (§3.6's completion table cannot wait on a cue that will never play). So
        a pointer that stopped on one would be a pointer standing where nothing
        is going to happen - which is a GO that does nothing, and the failure
        the sentence above was written about. */
    Rig rig;

    REQUIRE (rig.run (1, "node.set",
                      { osc::Value::string ("/godot/cue/" + preshow + "/enabled"),
                        osc::Value::boolean (false) }).applied == 1);

    // The disabled group is stepped over entirely, members and all.
    const auto list = rig.listNode (mainList);
    CHECK (nextStandby (list, houseToHalf) == houseToHalf);   // nothing after it
    CHECK_FALSE (isOnManualPath (list, walkIn));

    // And a disabled MEMBER is skipped without the group being skipped.
    REQUIRE (rig.run (2, "node.set",
                      { osc::Value::string ("/godot/cue/" + preshow + "/enabled"),
                        osc::Value::boolean (true) }).applied == 1);
    REQUIRE (rig.run (3, "node.set",
                      { osc::Value::string ("/godot/cue/" + walkIn + "/enabled"),
                        osc::Value::boolean (false) }).applied == 1);

    CHECK (nextStandby (rig.listNode (mainList), houseToHalf) == announce);
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

    /*  A cue nested inside a MANUAL group is now legal, because that is where
        §3.6 puts the pointer - "the operator is the parent". Phase 1 refused it
        and this is the assertion turning over. */
    CHECK (rig.run (3, "standby.set", { osc::Value::string (walkIn) }).applied == 1);
    CHECK (rig.standbyOf (mainList) == walkIn);

    /*  Inside an AUTOMATIC one it is refused, and with a code of its own. The
        machine advances that chain, so a pointer in it would be a pointer two
        things move - which is how an operator presses GO expecting cue 12 and
        gets 14 (§3.5). `not-in-list` would have sent them to look at the wrong
        thing: the cue is in this list, and what is wrong is the group. */
    REQUIRE (rig.run (4, "node.set",
                      { osc::Value::string ("/godot/cue/" + preshow + "/advance"),
                        osc::Value::string ("auto") }).applied == 1);

    CHECK (rig.run (5, "standby.set", { osc::Value::string (announce) }).rejected == 1);
    CHECK (rig.engine.lastError().find (reason::notManualPath) != std::string::npos);

    // And the refusal left the pointer where it was.
    CHECK (rig.standbyOf (mainList) == walkIn);
}

TEST_CASE ("standby.set: every kind of cue can be parked on, not only a memo and a group")
{
    /*  THE BUG THIS IS NAMED FOR. The handler asked whether the element was
        literally `Cue` or `Group`, which was the whole list of cue elements when
        it was written and stopped being it the moment Phase 2 added Media, Fade,
        Stop and Osc. Every one of those was refused as `unknown-id` - of a cue
        the engine had just found by that identifier - while `node.set` on the
        same node accepted it, because the document's door asks a different and
        correct question.

        No fixture caught it: the shows that play restore their standby from
        state.xml, which does not take this path, and the shows that exercise
        this path have nothing in them but memos and a group. So the first
        symptom would have been a UI that could not park on a sound cue.

        Every media cue in the bundle, then, and the general question rather than
        one example - a list of element names is what went stale the first time. */
    Rig rig { false };

    const juce::File soundBundle { juce::String (std::string (WFG_TEST_FIXTURES_DIR))
                                     + "/bundles/first-sound" };
    REQUIRE (doc::Bundle::open (soundBundle, rig.document).ok);

    const std::string soundList = "7K2QM9X4";
    const std::string thunder = "B3N8R5TW";              // a Media cue
    const std::string rain = "E4GP6QSC";                 // and another

    REQUIRE (rig.document.findById (thunder).getType().toString() == "Media");

    CHECK (rig.run (1, "standby.set", { osc::Value::string (rain) }).applied == 1);
    CHECK (rig.standbyOf (soundList) == rain);

    CHECK (rig.run (2, "standby.set", { osc::Value::string (thunder) }).applied == 1);
    CHECK (rig.standbyOf (soundList) == thunder);

    /*  And the two doors agree, which is the property that was broken: a write
        the command refuses must be one the node refuses too. */
    CHECK (rig.run (3, "node.set", { osc::Value::string ("/godot/list/" + soundList + "/standby"),
                                     osc::Value::string (rain) }).applied == 1);
    CHECK (rig.standbyOf (soundList) == rain);
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

    /*  A cue inside a MANUAL group is accepted here for the same reason
        `standby.set` accepts it: both doors ask `isOnManualPath`, which is the
        same walk the cursor takes. Two answers to "where may the pointer be"
        would eventually be two different answers, and the one that went stale
        would be whichever this door used. */
    CHECK (rig.run (3, "node.set",
                    { osc::Value::string (address), osc::Value::string (walkIn) }).applied == 1);
    CHECK (rig.standbyOf (mainList) == walkIn);

    // And inside an automatic one it is refused here too, with the same code.
    REQUIRE (rig.run (31, "node.set",
                      { osc::Value::string ("/godot/cue/" + preshow + "/advance"),
                        osc::Value::string ("auto") }).applied == 1);

    CHECK (rig.run (32, "node.set",
                    { osc::Value::string (address), osc::Value::string (announce) }).rejected == 1);
    CHECK (rig.engine.lastError().find (reason::notManualPath) != std::string::npos);

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

TEST_CASE ("standby: moving the cue off the manual path clears it, and along it does not")
{
    /*  WIDENED IN PR 3.4 from "out of the list's top level", because the
        pointer can now stand inside a manual sequence group. What matters is
        not the depth the cue moved to but whether the pointer is still allowed
        to be there.

        Clearing says plainly that what they were parked on has gone somewhere
        else; advancing would be guessing that the operator meant to stay where
        they were. */
    Rig rig;
    REQUIRE (rig.standbyOf (mainList) == houseToHalf);

    /*  Into a MANUAL group: still on the path, so the pointer follows the cue.
        It stores an identifier, and §3.5 says it does not move as a side effect
        of the show being edited around it. */
    CHECK (rig.run (1, "object.move",
                    { osc::Value::string (houseToHalf), osc::Value::string (preshow),
                      osc::Value::int32 (0) }).applied == 1);

    CHECK (rig.standbyOf (mainList) == houseToHalf);

    // Into an AUTOMATIC one: off the path, so it clears.
    REQUIRE (rig.run (2, "node.set",
                      { osc::Value::string ("/godot/cue/" + preshow + "/advance"),
                        osc::Value::string ("auto") }).applied == 1);

    CHECK (rig.run (3, "object.move",
                    { osc::Value::string (houseToHalf), osc::Value::string (preshow),
                      osc::Value::int32 (1) }).applied == 1);

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
    CHECK (previousStandby (list, houseToHalf) == announce);  // traversal followed the list
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
    CHECK (rig.focus.requested (rig.document).empty());
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
    CHECK (rig.focus.requested (rig.document) == secondList);
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

    /*  Two steps down the manual path: into the group's first member, then to
        its second. The group row is not a stop of its own (decision M). */
    CHECK (rig.standbyOf (mainList) == announce);
    CHECK (rig.standbyOf (secondList) == secondChildren[1]);  // and the other did not move

    // Now focus the second and traverse it; the first must hold still.
    CHECK (rig.run (13, "list.focus", { osc::Value::string (secondList) }).applied == 1);
    CHECK (rig.run (14, "standby.previous").applied == 1);

    CHECK (rig.standbyOf (secondList) == secondChildren[0]);
    CHECK (rig.standbyOf (mainList) == announce);
}

//==============================================================================
TEST_CASE ("standby: a saved state naming a cue that is not there is reported and skipped")
{
    /*  The invariant lives at the single write door, so the load path gets it
        too - which is the case that matters, because a state file written
        against a different show is exactly where a nonsense standby comes
        from. The show still opens; the pointer does not.

        IT NAMES A MOUNT, which is valid, real, and not a cue of this list at
        all. It used to name a cue nested inside the group - illegal in Phase 1,
        legal since PR 3.4, because the pointer descends into a manual sequence
        group (§3.6). What this case is about has not changed, so it names
        something that still cannot be a standby. */
    Rig rig;

    const auto stateText = "<State formatVersion=\"1\">\n"
                           "  <List id=\"" + mainList + "\" standby=\"" + wfsMount + "\"/>\n"
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

        /*  Something valid that is not a cue of this list, for the refusal. */
        engine.submit (origin::cli, "mount.create",
                       { osc::Value::string ("/desk"), osc::Value::string ("desk.json") });
        engine.processTick (5);

        const auto mountId = document.root().getChildWithName ("Mounts")
                               .getChild (0).getProperty ("id").toString().toStdString();

        /*  A cue INSIDE the group. Since PR 3.4 that is a place the pointer
            goes rather than one it refuses: the group is a manual sequence -
            what both attributes default to - and §3.6 puts the pointer inside
            one, because there the operator is the parent. */
        engine.submit (origin::cli, "cue.create",
                       { osc::Value::string (ids[1]), osc::Value::int32 (0),
                         osc::Value::string ("memo"), osc::Value::string ("Nested") });
        engine.processTick (6);

        const auto nested = childrenOf (document.findById (ids[1])).front();

        engine.submit (origin::cli, "list.focus", { osc::Value::string (listId) });
        engine.submit (origin::cli, "standby.set", { osc::Value::string (ids[0]) });
        engine.submit ("ws:127.0.0.1:51234", "standby.next");     // -> INTO the group
        engine.submit (origin::cli, "standby.next");              // -> out, to the third cue
        engine.processTick (9);

        // Past the end: applied, and it stays put. There is no wrap.
        engine.submit (origin::cli, "standby.next");

        /*  Refused, and it has to be something the pointer still cannot reach
            or the rejection record would stop being one: a mount is a valid
            identifier that is not a cue of this list. */
        engine.submit (origin::cli, "standby.set", { osc::Value::string (mountId) });

        // Back the way it came: out of the third cue, into the group again.
        engine.submit (origin::cli, "standby.previous");
        engine.processTick (14);

        /*  THE REPAIR, and it has to delete the cue the standby is actually on
            - which after that sequence is the NESTED one, inside the group.
            Deleting anything else would leave the repair branch in
            ShowDocument::remove() unentered and this fixture would pass with
            the whole repair deleted.

            It is also the case the repair had to grow for: the pointer is
            several levels down, so the list that is parked on the cue is not
            its parent, and asking only the parent would have left a standby
            naming a cue that had gone. */
        engine.submit (origin::cli, "object.delete", { osc::Value::string (nested) });
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
    /*  Fourteen since PR 3.4: the session gained a `mount.create`, because the
        refusal it needs has to name something the pointer still cannot reach
        and a cue inside a manual group is no longer one of those. The count is
        pinned rather than derived so that a record appearing or disappearing is
        something somebody has to look at. */
    REQUIRE (original.records.size() == 14);

    /*  The session must END somewhere specific, or the state comparison below
        would be comparing two empty files and passing. The last thing it did
        was delete the cue it was parked on - the only member of the group - so
        the repair had nowhere to advance to inside it and climbed out to what
        follows the group. That is the claim the replay then has to reproduce
        without any repair record in the log. */
    const auto sessionIds = childrenOf (sessionDocument.root()
                                          .getChildWithName ("Lists").getChild (0));

    REQUIRE (sessionIds.size() == 3);       // the group is still there; its member is not

    INFO ("state.xml: " << sessionState);
    CHECK (sessionState.find ("standby=\"" + sessionIds[2] + "\"") != std::string::npos);

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

//==============================================================================
/*  THE CONTAINER NODES: what a collection says about itself.

    `/godot/list/order` and `/godot/list/focus` were drawn in §2.3 of the
    namespace draft during Phase 1 and could not be built then. They need a
    parameter-table owner for the CONTAINER rather than for its members, and
    Phase 1 had one list — so there was nothing for a focus to be exclusive
    about, and the author settled it at engine state, unpublished, for as long
    as that was true.

    Parallel lists are what makes it untrue.
*/

TEST_CASE ("container nodes: the collection of lists carries a roster and a focus")
{
    Rig rig;

    /*  ADDRESSED WITHOUT AN IDENTIFIER, because there is one of it. Three parts
        rather than four is what tells the resolver that `/godot/list/focus`
        names the collection and `/godot/list/<id>/standby` names a member -
        the word is the same on purpose, because they are one container read two
        ways and a client walking the tree should not have to learn that one of
        them is spelled differently.

        (`order` is the collection's other node and is DERIVED, so it exists in
        the tree and not in the document - TreeTests is where it is asserted,
        beside every other computed value.) */
    CHECK (rig.document.getAttribute ("/godot/list/focus").value_or ("?").empty());

    REQUIRE (rig.run (1, "list.create", { osc::Value::string ("Second") }).applied == 1);

    const auto secondList = rig.document.root().getChildWithName ("Lists").getChild (1)
                              .getProperty ("id").toString().toStdString();

    // And the focus is a node, so a client can write it as it writes any other.
    REQUIRE (rig.run (2, "node.set", { osc::Value::string ("/godot/list/focus"),
                                       osc::Value::string (secondList) }).applied == 1);
    CHECK (rig.focus.listId (rig.document) == secondList);
}

TEST_CASE ("container nodes: focus is remembered across a save and a load")
{
    /*  The same argument that persisted the standby, applied to the pointer
        that says which standby is being pointed at: a rehearsal reopened where
        it was left is the kinder default. And the same limit - losing state.xml
        costs only where somebody had got to, never the show. */
    Rig rig;

    REQUIRE (rig.run (1, "list.create", { osc::Value::string ("Second") }).applied == 1);

    const auto secondList = rig.document.root().getChildWithName ("Lists").getChild (1)
                              .getProperty ("id").toString().toStdString();

    REQUIRE (rig.run (2, "list.focus", { osc::Value::string (secondList) }).applied == 1);

    /*  IT IS IN state.xml AND NOT IN show.xml, which is the split the persist
        column decides and the canonical writer enforces in both directions.
        Which list somebody was working in is not a decision about the show. */
    const auto state = doc::EphemeralState::write (rig.document);
    const auto show = doc::CanonicalXml::write (rig.document);

    INFO ("state.xml:\n" << state);
    CHECK (state.find ("<Lists focus=\"" + secondList + "\"/>") != std::string::npos);
    CHECK (show.find ("focus") == std::string::npos);

    /*  AN ENTRY WITH NO IDENTIFIER, which is the shape this file could not
        carry before. A `<List>` is found by its id; `<Lists>` has none and
        needs none, because there is one of it and it is the collection. */
    doc::ShowDocument reopened;
    REQUIRE (doc::CanonicalXml::read (show, reopened).ok);

    const auto restored = doc::EphemeralState::read (state, reopened);
    INFO ("problems: " << (restored.problems.empty() ? std::string ("none")
                                                     : restored.problems.front()));
    CHECK (restored.ok);
    CHECK (restored.problems.empty());

    Focus reopenedFocus;
    CHECK (reopenedFocus.listId (reopened) == secondList);
}

TEST_CASE ("container nodes: a focus naming a list the show no longer has resolves to the first")
{
    /*  Resolved rather than maintained, which is what PR 3.2 kept while moving
        the value into the document. Nothing has to remember to move the focus
        when a list is deleted, so "exactly one list is focused whenever a list
        exists" stays true by construction rather than by upkeep - and a
        state.xml written against a different show cannot leave the engine
        pointed at nothing. */
    Rig rig;

    REQUIRE (rig.run (1, "list.create", { osc::Value::string ("Second") }).applied == 1);

    const auto secondList = rig.document.root().getChildWithName ("Lists").getChild (1)
                              .getProperty ("id").toString().toStdString();

    REQUIRE (rig.run (2, "list.focus", { osc::Value::string (secondList) }).applied == 1);
    CHECK (rig.focus.listId (rig.document) == secondList);

    REQUIRE (rig.run (3, "object.delete", { osc::Value::string (secondList) }).applied == 1);

    // The request is still on file and no longer resolves, so the first list has it.
    CHECK (rig.focus.listId (rig.document) == mainList);
}

//==============================================================================
/*  THE STANDBY CURSOR: the manual path through a show.

    PRD §3.6, on a manual sequence group: "a member starts on GO. The standby
    pointer DESCENDS INTO the group; the operator is the parent." And on the
    other two kinds, the machine is the parent - a timeline schedules everything
    at entry and an automatic sequence advances itself - so there is nothing
    inside for the pointer to do, and §3.5 sends it positionally past the whole
    chain the instant GO is pressed.

    Phase 1 stepped over all of them and named the test for the choice so that
    this moment would be visible. The test below is that one, rewritten.
*/
namespace
{
    /*  A list shaped to have somewhere to descend into and somewhere to step
        over: a cue, a MANUAL group of three, an AUTO group of two, a cue. */
    struct CursorRig : Rig
    {
        CursorRig() : Rig (false)
        {
            doc::registerDocumentCommands (engine.commands(), document);

            listId = document.createList ("Main").id;

            top = document.createCue (listId, 0, "memo", "Top").id;

            manual = document.createCue (listId, 1, "group", "Manual").id;
            m1 = document.createCue (manual, 0, "memo", "M1").id;
            m2 = document.createCue (manual, 1, "memo", "M2").id;
            m3 = document.createCue (manual, 2, "memo", "M3").id;

            auto_ = document.createCue (listId, 2, "group", "Auto").id;
            document.setAttribute ("/godot/cue/" + auto_ + "/advance", "auto");
            a1 = document.createCue (auto_, 0, "memo", "A1").id;
            a2 = document.createCue (auto_, 1, "memo", "A2").id;

            tail = document.createCue (listId, 3, "memo", "Tail").id;
        }

        juce::ValueTree list() const { return document.findById (listId); }

        std::string listId, top, manual, m1, m2, m3, auto_, a1, a2, tail;
    };
}

TEST_CASE ("cursor: it descends into a manual group and climbs back out")
{
    CursorRig rig;
    const auto list = rig.list();

    /*  Down: the top cue, then the manual group's members one at a time -
        the group ROW is not a stop, because the operator's next press is the
        first member and a press that only entered would be a GO that did
        nothing (decision M, 2026-09-06). */
    CHECK (nextStandby (list, rig.top) == rig.m1);
    CHECK (nextStandby (list, rig.m1) == rig.m2);
    CHECK (nextStandby (list, rig.m2) == rig.m3);

    // Out: after the last member comes what follows the GROUP.
    CHECK (nextStandby (list, rig.m3) == rig.auto_);

    // And back up, the same path in reverse.
    CHECK (previousStandby (list, rig.auto_) == rig.m3);
    CHECK (previousStandby (list, rig.m3) == rig.m2);
    CHECK (previousStandby (list, rig.m1) == rig.top);
}

TEST_CASE ("cursor: an automatic group is one sibling, and its members are not stops")
{
    /*  §3.5: standby lands on the cue positionally AFTER the whole automated
        chain, the instant GO is pressed. The machine is the parent there, and a
        pointer inside a chain the machine is also advancing would be two things
        moving one pointer - which is how an operator presses GO expecting cue
        12 and gets 14. */
    CursorRig rig;
    const auto list = rig.list();

    CHECK (nextStandby (list, rig.auto_) == rig.tail);
    CHECK (previousStandby (list, rig.tail) == rig.auto_);

    // Its members are not on the path at all.
    CHECK_FALSE (isOnManualPath (list, rig.a1));
    CHECK_FALSE (isOnManualPath (list, rig.a2));

    // Nor is a timeline group's, whatever its advance says.
    rig.document.setAttribute ("/godot/cue/" + rig.manual + "/mode", "timeline");
    CHECK (nextStandby (rig.list(), rig.top) == rig.manual);
    CHECK_FALSE (isOnManualPath (rig.list(), rig.m2));
}

TEST_CASE ("cursor: a disabled cue is not a stop, and a disabled group is not entered")
{
    /*  Phase 1 asserted the opposite and said why: "skipping is a
        running-behaviour decision that Phase 1 has no runner to justify; Phase
        3 revisits it when a GO that does nothing becomes a real failure rather
        than a hypothetical one."

        It has. The scheduler already skips a disabled member, so a pointer that
        stopped on one would be a pointer standing where nothing will happen. */
    CursorRig rig;

    rig.document.setAttribute ("/godot/cue/" + rig.m2 + "/enabled", "false");
    CHECK (nextStandby (rig.list(), rig.m1) == rig.m3);
    CHECK (previousStandby (rig.list(), rig.m3) == rig.m1);

    rig.document.setAttribute ("/godot/cue/" + rig.manual + "/enabled", "false");
    CHECK (nextStandby (rig.list(), rig.top) == rig.auto_);
    CHECK_FALSE (isOnManualPath (rig.list(), rig.m1));
}

TEST_CASE ("cursor: a header and a footer are never entered")
{
    /*  They are cue lists the group runs for ITSELF (§3.6). The pointer is the
        operator's position in the show, and the operator does not step through
        a group's preparation. */
    CursorRig rig;

    const auto header = rig.document.createRole (rig.manual, "header");
    REQUIRE (header.ok);
    const auto opening = rig.document.createCue (header.id, 0, "memo", "Pre-arm").id;

    const auto footer = rig.document.createRole (rig.manual, "footer");
    REQUIRE (footer.ok);
    const auto closing = rig.document.createCue (footer.id, 0, "memo", "Release").id;

    CHECK (nextStandby (rig.list(), rig.top) == rig.m1);
    CHECK (nextStandby (rig.list(), rig.m3) == rig.auto_);

    CHECK_FALSE (isOnManualPath (rig.list(), opening));
    CHECK_FALSE (isOnManualPath (rig.list(), closing));
}

TEST_CASE ("cursor: nested manual groups, and an empty one the pointer stands on")
{
    CursorRig rig;

    // A manual group inside the manual group, between M1 and M2.
    const auto inner = rig.document.createCue (rig.manual, 1, "group", "Inner").id;
    const auto i1 = rig.document.createCue (inner, 0, "memo", "I1").id;
    const auto i2 = rig.document.createCue (inner, 1, "memo", "I2").id;

    CHECK (nextStandby (rig.list(), rig.m1) == i1);
    CHECK (nextStandby (rig.list(), i1) == i2);
    CHECK (nextStandby (rig.list(), i2) == rig.m2);       // out one level, not two
    CHECK (previousStandby (rig.list(), rig.m2) == i2);

    /*  AN EMPTY MANUAL GROUP IS ITS OWN STOP. There is nowhere inside to
        descend to, and a pointer that skipped it would make a container
        somebody has not filled in yet invisible; GO on it completes it, which
        is what an empty group does (§3.6). */
    const auto empty = rig.document.createCue (rig.listId, 4, "group", "Empty").id;
    CHECK (nextStandby (rig.list(), rig.tail) == empty);
    CHECK (nextStandby (rig.list(), empty) == empty);     // last, and stays put
}

TEST_CASE ("cursor: at either end it stays put, and from empty it stays empty")
{
    /*  Unchanged from Phase 1, and it has to be: there is no wrap anywhere, and
        only standby.set arms a list. */
    CursorRig rig;
    const auto list = rig.list();

    CHECK (previousStandby (list, rig.top) == rig.top);
    CHECK (nextStandby (list, rig.tail) == rig.tail);

    CHECK (nextStandby (list, "") == "");
    CHECK (previousStandby (list, "") == "");

    // And something that is not in this list at all does not move it either.
    CHECK (nextStandby (list, "ZZZZZZZZ") == "ZZZZZZZZ");
}
