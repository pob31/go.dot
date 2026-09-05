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

/*  The audio section of the show document: the track count that sets the
    polyphony ceiling, and the buses that name ranges of output channels.

    WHAT THESE CASES ARE REALLY ABOUT. `tracks` is the first attribute in the
    parameter table with no default, and that is not a gap - it is how the
    table says REQUIRED. PRD 3.25 makes the track count the polyphony ceiling,
    and the author's Phase 2 decision was that no number could be right for
    every rig, so every show states its own. The rule enforcing it is general
    rather than a named exception, so half of what follows is about the rule
    and only incidentally about audio.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/Engine.h>
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/tree/TreeCommands.h>

#include "TestSupport.h"

#include <memory>
#include <string>
#include <vector>

using namespace wfg;

namespace
{
    juce::File fixtureBundle()
    {
        const juce::File folder { juce::String (std::string (WFG_TEST_FIXTURES_DIR))
                                    + "/bundles/minimal" };

        REQUIRE_MESSAGE (folder.isDirectory(), "missing fixture bundle: "
                                                 << folder.getFullPathName());
        return folder;
    }

    bool mentions (const std::vector<std::string>& problems, const std::string& fragment)
    {
        for (const auto& problem : problems)
            if (problem.find (fragment) != std::string::npos)
                return true;

        return false;
    }

    std::string firstProblem (const doc::ReadResult& result)
    {
        return result.problems.empty() ? std::string ("(none)") : result.problems.front();
    }

    /*  Document, commands and tree, wired the way `wfg tree` and `serve` wire
        them. No mounts loaded: these cases are about /godot/audio and
        /godot/bus, and an empty mount table is a valid one. */
    struct Rig
    {
        Rig()
        {
            REQUIRE (doc::Bundle::open (fixtureBundle(), document).ok);
            doc::registerDocumentCommands (engine.commands(), document);
            tree::registerTreeCommands (engine.commands(), touches);
        }

        std::shared_ptr<const tree::TreeSnapshot> publish()
        {
            tree::EngineState state;
            state.version = "test";
            return parameters.publish (0, state);
        }

        Engine engine;
        doc::ShowDocument document;
        tree::TouchTable touches;
        tree::MountTable mounts;
        tree::ParameterTree parameters { document, engine.commands(), mounts };
    };
}

//==============================================================================
TEST_CASE ("audio: a fresh document says zero tracks rather than nothing")
{
    /*  Zero is an answer - a show with no audio - and having no default is
        what makes the file always carry the number somebody meant. A document
        that omitted it would be one the canonical writer could not round-trip
        back to itself. */
    doc::ShowDocument document;

    CHECK (document.getAttribute ("/godot/audio/tracks") == "0");

    const auto text = doc::CanonicalXml::write (document);

    INFO ("written: " << text);
    CHECK (text.find ("<Audio tracks=\"0\"/>") != std::string::npos);
}

TEST_CASE ("audio: an Audio element with no tracks is refused, and says what is missing")
{
    doc::ShowDocument document;

    const auto result = doc::CanonicalXml::read ("<Show>\n"
                                                 "  <Lists/>\n"
                                                 "  <Mounts/>\n"
                                                 "  <Audio/>\n"
                                                 "</Show>\n", document);

    INFO ("problems: " << firstProblem (result));
    CHECK_FALSE (result.problems.empty());
    CHECK (mentions (result.problems, "tracks"));
}

TEST_CASE ("audio: a string with no default stays optional, because empty is a value")
{
    /*  The other half of the same rule, and why it is written in terms of the
        type rather than of hasDefault alone. `notes` has no declared default
        either; an absent note is the empty note somebody meant, so requiring
        every no-default attribute would reject every cue ever written. */
    doc::ShowDocument document;

    const auto result = doc::CanonicalXml::read ("<Show>\n"
                                                 "  <Lists>\n"
                                                 "    <List id=\"7K2QM9X4\"/>\n"
                                                 "  </Lists>\n"
                                                 "  <Mounts/>\n"
                                                 "  <Audio tracks=\"0\"/>\n"
                                                 "</Show>\n", document);

    INFO ("problems: " << firstProblem (result));
    CHECK (result.problems.empty());
}

TEST_CASE ("audio: the track count and its buses survive a save and a load")
{
    doc::ShowDocument document;

    const auto source = std::string ("<Show>\n"
                                     "  <Lists/>\n"
                                     "  <Mounts/>\n"
                                     "  <Audio tracks=\"12\">\n"
                                     "    <Bus id=\"J3MT5XYA\" name=\"Main L/R\" width=\"2\"/>\n"
                                     "    <Bus id=\"K4NV6ZB1\" firstChannel=\"2\" name=\"Sub\" width=\"1\"/>\n"
                                     "  </Audio>\n"
                                     "</Show>\n");

    REQUIRE (doc::CanonicalXml::read (source, document).ok);

    CHECK (document.getAttribute ("/godot/audio/tracks") == "12");
    CHECK (document.getAttribute ("/godot/bus/J3MT5XYA/name") == "Main L/R");
    CHECK (document.getAttribute ("/godot/bus/K4NV6ZB1/firstChannel") == "2");
    CHECK (document.getAttribute ("/godot/bus/K4NV6ZB1/width") == "1");

    const auto written = doc::CanonicalXml::write (document);

    doc::ShowDocument reloaded;
    REQUIRE (doc::CanonicalXml::read (written, reloaded).ok);

    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));
    CHECK (doc::CanonicalXml::write (reloaded) == written);
}

TEST_CASE ("audio: the tree publishes the track count and every bus")
{
    Rig rig;
    const auto snapshot = rig.publish();

    const auto* tracks = snapshot->find ("/godot/audio/tracks");

    REQUIRE (tracks != nullptr);
    CHECK (tracks->typeTags == "i");
    CHECK (tracks->access == tree::Access::read);

    /*  Read out of the fixture rather than remembered, so that changing the
        bundle changes what this expects. */
    REQUIRE (tracks->value.has_value());
    CHECK (std::to_string (tracks->value->getInt32())
             == rig.document.getAttribute ("/godot/audio/tracks"));

    for (const auto& address : { "/godot/audio", "/godot/bus" })
    {
        INFO ("address: " << address);
        const auto* container = snapshot->find (address);

        REQUIRE (container != nullptr);
        CHECK (container->isContainer());
    }

    const auto* name = snapshot->find ("/godot/bus/J3MT5XYA/name");

    REQUIRE (name != nullptr);
    CHECK (name->access == tree::Access::readWrite);
    REQUIRE (name->value.has_value());
    CHECK (name->value->getString() == "Main L/R");
}

TEST_CASE ("audio: the runtime nodes answer before anything has opened a device")
{
    /*  status, device and outputs describe what the audio side is doing. With
        no device they still exist and still answer, because "stopped" is the
        truthful answer and an absent node would leave a client guessing
        whether it had asked the wrong question. */
    Rig rig;
    const auto snapshot = rig.publish();

    const auto* status = snapshot->find ("/godot/audio/status");

    REQUIRE (status != nullptr);
    REQUIRE (status->value.has_value());
    CHECK (status->value->getString() == "stopped");

    const auto* outputs = snapshot->find ("/godot/audio/outputs");

    REQUIRE (outputs != nullptr);
    REQUIRE (outputs->value.has_value());
    CHECK (outputs->value->getInt32() == 0);
}

TEST_CASE ("audio: a write to the track count is refused as read-only, not as unknown")
{
    /*  Which refusal arrives is the point. `tracks` is a real address that
        simply is not writable over the wire, and answering "bad-address"
        would send a client looking for a spelling mistake it did not make. */
    Rig rig;

    const auto edit = rig.document.setAttribute ("/godot/audio/tracks", "16");

    CHECK_FALSE (edit.ok);
    CHECK (edit.reason == reason::readOnly);

    const auto missing = rig.document.setAttribute ("/godot/audio/noSuchThing", "16");

    CHECK_FALSE (missing.ok);
    CHECK (missing.reason == reason::badAddress);
}
