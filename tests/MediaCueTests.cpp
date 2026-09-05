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

/*  The media cue and its destinations, in the document and in the tree.

    Two author decisions of 2026-09-05 are asserted here rather than described
    somewhere nobody opens.

    ONE ELEMENT PER CUE KIND. A media cue is a <Media>, so `kind` stays derived
    from the element and read-only, and the grammar can refuse a `file`
    attribute on a cue that plays nothing. The alternative - every cue a <Cue>
    with a stored kind - would have made every cue carry every kind's
    attributes and left the grammar with nothing to refuse.

    A DESTINATION IS AN OBJECT. PRD §3.9b says a cue's destinations are a list
    rather than a choice, so <Route> repeats; it is identified rather than
    positional so that changing one destination's gains is a write to one node,
    and because an index is a position - deleting the first route would
    silently re-point a client holding the second.

    `Route/@gains` is also the first list-typed attribute in the tree, so the
    cases that cover it are covering `d*` at the same time: one value per
    element, a TYPE that grows to match, and nothing at all when it does not
    parse.

    A serialisation surface, so every case runs under fr_FR as well as C - a
    gains list is the worst possible thing to hand to a locale that writes a
    decimal comma, since it is numbers separated by spaces.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/Touches.h>
#include <wfg/engine/tree/ParameterTree.h>

#include <juce_core/juce_core.h>

using namespace wfg;

namespace
{
    /*  A show with one list, built through the ordinary command path so that
        what is tested is what a client would get rather than a tree assembled
        by hand. */
    struct Rig
    {
        Rig()
        {
            doc::registerDocumentCommands (engine.commands(), document);

            listId = document.createList ("Main").id;
        }

        std::string createMedia (const std::string& name)
        {
            return document.createCue (listId, 0, "media", name).id;
        }

        std::shared_ptr<const tree::TreeSnapshot> publish()
        {
            tree::EngineState state;
            return parameters.publish (0, state);
        }

        Engine engine;
        doc::ShowDocument document;
        tree::TouchTable touches;
        tree::MountTable mounts;
        tree::ParameterTree parameters { document, engine.commands(), mounts };

        std::string listId;
    };

    const tree::Node* nodeAt (const tree::TreeSnapshot& snapshot, const std::string& address)
    {
        return snapshot.find (address);
    }
}

//==============================================================================
TEST_CASE ("media cue: a media cue is its own element, and says so")
{
    Rig rig;

    const auto id = rig.createMedia ("Thunder");
    REQUIRE_FALSE (id.empty());

    /*  The element, not a stored word. This is the whole of what the author's
        decision buys: a client cannot turn a memo into a media cue by writing
        to `kind`, because there is nothing there to write. */
    const auto cue = rig.document.findById (id);
    REQUIRE (cue.isValid());
    CHECK (cue.getType().toString() == "Media");

    const auto snapshot = rig.publish();
    REQUIRE (snapshot != nullptr);

    const auto* kind = nodeAt (*snapshot, "/godot/cue/" + id + "/kind");
    REQUIRE (kind != nullptr);
    REQUIRE (kind->soleValue().has_value());
    CHECK (kind->soleValue()->getString() == "media");
    CHECK (kind->access == tree::Access::read);
}

TEST_CASE ("media cue: it is addressed as a cue, and carries a cue's rows as well as its own")
{
    /*  A media cue has a number, a name and a pre-wait like any other, and it
        lives at /godot/cue/<id> - so a client holding an identifier never has
        to know which kind it got. That is the same arrangement a Group has. */
    Rig rig;

    const auto id = rig.createMedia ("Thunder");
    const auto snapshot = rig.publish();
    const auto base = "/godot/cue/" + id;

    for (const auto* row : { "name", "number", "preWait", "postWait", "enabled" })
    {
        INFO ("the cue row " << row);
        CHECK (nodeAt (*snapshot, base + "/" + row) != nullptr);
    }

    for (const auto* row : { "file", "level", "startOffset" })
    {
        INFO ("the media row " << row);
        CHECK (nodeAt (*snapshot, base + "/" + row) != nullptr);
    }
}

TEST_CASE ("media cue: a memo cue carries no media rows at all")
{
    /*  The other half of the decision, and the half that would be impossible
        with a stored kind: a cue that plays nothing has no `file` to be wrong
        about, in the document or in the tree. */
    Rig rig;

    const auto id = rig.document.createCue (rig.listId, 0, "memo", "House to half").id;
    REQUIRE_FALSE (id.empty());

    const auto snapshot = rig.publish();
    const auto base = "/godot/cue/" + id;

    REQUIRE (nodeAt (*snapshot, base + "/name") != nullptr);

    for (const auto* row : { "file", "level", "startOffset" })
    {
        INFO ("the media row " << row << " must not be on a memo cue");
        CHECK (nodeAt (*snapshot, base + "/" + row) == nullptr);
    }
}

//==============================================================================
TEST_CASE ("route: a destination has an address of its own, not a position")
{
    Rig rig;

    const auto cueId = rig.createMedia ("Thunder");
    const auto first = rig.document.createRoute (cueId, "J3MT5XYA");
    const auto second = rig.document.createRoute (cueId, "K4NV6ZB1");

    REQUIRE (first.ok);
    REQUIRE (second.ok);
    CHECK (first.id != second.id);

    const auto snapshot = rig.publish();

    for (const auto& id : { first.id, second.id })
    {
        INFO ("route " << id);
        CHECK (nodeAt (*snapshot, "/godot/route/" + id + "/bus") != nullptr);
        CHECK (nodeAt (*snapshot, "/godot/route/" + id + "/gains") != nullptr);
    }

    /*  And it is NOT a nested cue. The tree's recursion takes any identified
        child, so without a case for it a route would have been published at
        /godot/cue/<route id> and carried a cue's whole row set. */
    CHECK (nodeAt (*snapshot, "/godot/cue/" + first.id + "/name") == nullptr);

    const auto* bus = nodeAt (*snapshot, "/godot/route/" + first.id + "/bus");
    REQUIRE (bus->soleValue().has_value());
    CHECK (bus->soleValue()->getString() == "J3MT5XYA");
}

TEST_CASE ("route: only a cue that plays something can have somewhere to play it")
{
    Rig rig;

    const auto memo = rig.document.createCue (rig.listId, 0, "memo", "House to half").id;

    /*  Refused here rather than left to the grammar, so the client is told
        which of its two identifiers was wrong and told it when it asked. */
    const auto onMemo = rig.document.createRoute (memo, "J3MT5XYA");
    CHECK_FALSE (onMemo.ok);
    CHECK (onMemo.reason == reason::typeMismatch);

    const auto onNothing = rig.document.createRoute ("NOSUCHID", "J3MT5XYA");
    CHECK_FALSE (onNothing.ok);
    CHECK (onNothing.reason == reason::unknownId);
}

TEST_CASE ("route: route.create is a command, like every other gesture")
{
    /*  PRD §4.11: every gesture-reachable action exists as a named command.
        Adding a destination is a gesture. */
    Rig rig;

    const auto cueId = rig.createMedia ("Thunder");

    REQUIRE (rig.engine.submit ("cli", "route.create",
                                { osc::Value::string (cueId),
                                  osc::Value::string ("J3MT5XYA") }));

    const auto outcome = rig.engine.processTick (0);
    CHECK (outcome.applied == 1);

    const auto cue = rig.document.findById (cueId);
    REQUIRE (cue.getNumChildren() == 1);
    CHECK (cue.getChild (0).getType().toString() == "Route");
}

//==============================================================================
TEST_CASE ("gains: a list publishes one value per element, and a type to match")
{
    /*  The first list-typed attribute in the tree. OSCQuery's TYPE is per node
        and its VALUE is an array, so four coefficients are "dddd" and four
        numbers - which is what an OSC client receives as four arguments, with
        no encoding of Go.dot's own to undo. */
    Rig rig;

    const auto cueId = rig.createMedia ("Thunder");
    const auto route = rig.document.createRoute (cueId, "J3MT5XYA");
    REQUIRE (route.ok);

    /*  The row says it is a list; everything below is what that means once it
        reaches a client. */
    const doc::AttributeRow* gainsRow = nullptr;

    for (const auto* row : doc::Schema::rowsForOwner ("route"))
        if (row->name == "gains")
            gainsRow = row;

    REQUIRE (gainsRow != nullptr);
    CHECK (gainsRow->isList);

    auto node = rig.document.findById (route.id);
    node.setProperty (juce::Identifier ("gains"), "1 0 0 0.5", nullptr);

    const auto snapshot = rig.publish();
    const auto* published = nodeAt (*snapshot, "/godot/route/" + route.id + "/gains");

    REQUIRE (published != nullptr);
    CHECK (published->typeTags == "dddd");
    REQUIRE (published->values.size() == 4u);

    CHECK (published->values[0] == osc::Value::float64 (1.0));
    CHECK (published->values[1] == osc::Value::float64 (0.0));
    CHECK (published->values[2] == osc::Value::float64 (0.0));
    CHECK (published->values[3] == osc::Value::float64 (0.5));

    /*  And it is not a single value wearing a list's clothes: asking for "the
        value" of four gains has no answer, and soleValue says so rather than
        handing back the first one. */
    CHECK_FALSE (published->soleValue().has_value());
}

TEST_CASE ("gains: an empty list is zero values, not one empty one")
{
    /*  A cue routed nowhere yet is an ordinary state for a show being written,
        and the grammar accepts it. The node exists and carries nothing. */
    Rig rig;

    const auto cueId = rig.createMedia ("Thunder");
    const auto route = rig.document.createRoute (cueId, "J3MT5XYA");

    const auto snapshot = rig.publish();
    const auto* published = nodeAt (*snapshot, "/godot/route/" + route.id + "/gains");

    REQUIRE (published != nullptr);
    CHECK (published->values.empty());
    CHECK (published->typeTags.empty());
}

TEST_CASE ("gains: a list that fails anywhere publishes nothing, not the part that parsed")
{
    /*  Half a routing matrix is not a smaller routing matrix, it is a different
        one. A client handed three of four gains has been told something untrue
        about where a cue goes, and it has no way to know. */
    Rig rig;

    const auto cueId = rig.createMedia ("Thunder");
    const auto route = rig.document.createRoute (cueId, "J3MT5XYA");

    auto node = rig.document.findById (route.id);
    node.setProperty (juce::Identifier ("gains"), "1 0 banana 0.5", nullptr);

    const auto snapshot = rig.publish();
    const auto* published = nodeAt (*snapshot, "/godot/route/" + route.id + "/gains");

    REQUIRE (published != nullptr);
    CHECK (published->values.empty());
}

//==============================================================================
TEST_CASE ("media cue: a show with media and routes round-trips byte for byte")
{
    /*  The locale case in disguise, and the reason this file runs twice. A
        gains list is numbers separated by spaces, which is the worst thing to
        hand to a locale that writes a decimal comma: one `1,5` in the middle of
        `1 0 0 1.5` and the list silently becomes a different length. */
    Rig rig;

    const auto cueId = rig.createMedia ("Thunder");
    auto cue = rig.document.findById (cueId);
    cue.setProperty (juce::Identifier ("file"), "thunder.wav", nullptr);
    cue.setProperty (juce::Identifier ("level"), -3.5, nullptr);
    cue.setProperty (juce::Identifier ("startOffset"), 0.25, nullptr);

    const auto route = rig.document.createRoute (cueId, "J3MT5XYA");
    auto node = rig.document.findById (route.id);
    node.setProperty (juce::Identifier ("gains"), "1 0 0 1.5", nullptr);

    const auto written = doc::CanonicalXml::write (rig.document);

    INFO (written);
    CHECK (written.find ("<Media") != std::string::npos);
    CHECK (written.find ("<Route") != std::string::npos);
    CHECK (written.find ("gains=\"1 0 0 1.5\"") != std::string::npos);

    doc::ShowDocument reloaded;
    const auto result = doc::CanonicalXml::read (written, reloaded);

    for (const auto& problem : result.problems)
        INFO (problem);
    REQUIRE (result.ok);

    CHECK (doc::CanonicalXml::write (reloaded) == written);
}

//==============================================================================
/*  Raw string literals, so the XML below is the XML - no escaped quotes and no
    line continuations between a reader and what it is reading. */
namespace
{
    doc::ReadResult readShow (const std::string& text, doc::ShowDocument& into)
    {
        return doc::CanonicalXml::read (text, into);
    }
}

TEST_CASE ("media cue: the reader refuses media attributes on a cue that plays nothing")
{
    /*  The negative half of "one element per kind", and the thing a stored kind
        could never have given us. `file` is a media row, a <Cue> carries only
        cue rows, so a memo cue with a file is not a cue with a harmless extra -
        it is a document that does not describe a show.

        The generated RELAX NG refuses it too; the positive case is committed as
        tests/fixtures/documents/media-cue.xml and every build runs lxml over
        it, which is the outside opinion. This is our own reader giving the same
        answer, and it is the half that produces a message somebody can act on. */
    const std::string memoWithMedia = R"(<Show>
  <Lists>
    <List id="7WBV41P3" name="Sound">
      <Cue id="D9FH2JKA" file="thunder.wav" name="House to half"/>
    </List>
  </Lists>
  <Mounts/>
  <Audio tracks="0"/>
</Show>
)";

    doc::ShowDocument document;
    const auto result = readShow (memoWithMedia, document);

    for (const auto& problem : result.problems)
        INFO (problem);

    CHECK_FALSE (result.ok);
    REQUIRE_FALSE (result.problems.empty());
    CHECK (result.problems.front().find ("file") != std::string::npos);
}

TEST_CASE ("media cue: a gains list is canonicalised, so one show is one file")
{
    /*  `1.50` and `1.5` are the same coefficient, and a document that could
        spell them two ways would round-trip to different bytes depending on who
        typed it. Every element goes through the same formatter a scalar goes
        through, and the run comes back single-spaced. */
    const std::string spelt = R"(<Show>
  <Lists>
    <List id="7WBV41P3" name="Sound">
      <Media id="JMS7SB5T" name="Thunder">
        <Route id="Z04EH7PH" bus="J3MT5XYA" gains="1.50   0    0.250"/>
      </Media>
    </List>
  </Lists>
  <Mounts/>
  <Audio tracks="0"/>
</Show>
)";

    doc::ShowDocument document;
    const auto result = readShow (spelt, document);

    for (const auto& problem : result.problems)
        INFO (problem);

    REQUIRE (result.ok);

    const auto written = doc::CanonicalXml::write (document);

    INFO (written);
    CHECK (written.find (R"(gains="1.5 0 0.25")") != std::string::npos);
}

TEST_CASE ("media cue: one bad coefficient refuses the whole document, and says which")
{
    const std::string bad = R"(<Show>
  <Lists>
    <List id="7WBV41P3" name="Sound">
      <Media id="JMS7SB5T" name="Thunder">
        <Route id="Z04EH7PH" bus="J3MT5XYA" gains="1 0 banana 1"/>
      </Media>
    </List>
  </Lists>
  <Mounts/>
  <Audio tracks="0"/>
</Show>
)";

    doc::ShowDocument document;
    const auto result = readShow (bad, document);

    CHECK_FALSE (result.ok);
    REQUIRE_FALSE (result.problems.empty());

    /*  And it says WHICH one. A routing matrix is long, and "gains is wrong" is
        not something anybody can act on at 2 a.m. */
    INFO (result.problems.front());
    CHECK (result.problems.front().find ("element 2") != std::string::npos);
}
