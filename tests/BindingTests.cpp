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

/*  BINDINGS (namespace draft §74, AQX-ARD): a control in the room - a MIDI
    controller, a note, an OSC address - wired to a number in the tree, scaled
    into its range, live while the show is open or for each run of a cue. */

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/tree/TreeCommands.h>
#include <wfg/engine/tree/Touches.h>
#include <wfg/client/model/Text.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace wfg;

namespace
{
    struct Rig
    {
        Rig()
        {
            doc::registerDocumentCommands (engine.commands(), document);
            tree::registerTreeCommands (engine.commands(), touches);

            listId = document.createList ("Main").id;
            cueId = document.createCue (listId, 0, "media", "Door").id;
            REQUIRE_FALSE (cueId.empty());
        }

        Engine::TickResult send (const std::string& command, std::vector<osc::Value> args)
        {
            REQUIRE (engine.submit (origin::cli, command, std::move (args)));
            const auto result = engine.processTick (tick++);
            parameters.markStale();
            return result;
        }

        std::string published (const std::string& address)
        {
            tree::EngineState state;
            state.documentRevision = document.showRevision();
            snapshot = parameters.publish (tick, state);
            return client::model::text (*snapshot, address);
        }

        Engine engine;
        doc::ShowDocument document;
        tree::TouchTable touches;
        tree::MountTable mounts;
        cue::RunTable runs;
        tree::ParameterTree parameters { document, engine.commands(), mounts, runs };
        std::shared_ptr<const tree::TreeSnapshot> snapshot;
        std::string listId, cueId;
        std::int64_t tick = 1;
    };

    osc::Value s (const std::string& text) { return osc::Value::string (text); }
}

//==============================================================================
TEST_CASE ("binding: a show has none until one is made, and the first makes the show's collection")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));
    Rig rig;

    //  A show without bindings writes none - an old show saves unchanged.
    CHECK (doc::CanonicalXml::write (rig.document).find ("Bindings") == std::string::npos);

    REQUIRE (rig.send ("binding.create", { s (""), s ("BND00001"), s ("number"), s ("7"),
                                            s ("target"), s ("/godot/cue/" + rig.cueId + "/level") }).rejected == 0);

    CHECK (rig.published ("/godot/binding/order") == "BND00001");
    CHECK (rig.published ("/godot/binding/BND00001/number") == "7");
    CHECK (rig.published ("/godot/binding/BND00001/target") == "/godot/cue/" + rig.cueId + "/level");
    CHECK (rig.published ("/godot/binding/BND00001/cue").empty());

    //  Its defaults: a MIDI controller, absolute, over the target's own range, picked up.
    CHECK (rig.published ("/godot/binding/BND00001/source") == "midi");
    CHECK (rig.published ("/godot/binding/BND00001/type") == "controlChange");
    CHECK (rig.published ("/godot/binding/BND00001/control") == "absolute");
    CHECK (rig.published ("/godot/binding/BND00001/ownRange") == "true");
    CHECK (rig.published ("/godot/binding/BND00001/pickup") == "true");
    CHECK (rig.published ("/godot/binding/BND00001/feedback") == "false");
    CHECK (rig.published ("/godot/binding/BND00001/enabled") == "true");

    //  Written before the timecode settings, and read back as it was.
    const auto written = doc::CanonicalXml::write (rig.document);
    const auto bindings = written.find ("<Bindings>");
    REQUIRE (bindings != std::string::npos);
    CHECK (bindings < written.find ("<Sync"));

    doc::ShowDocument reopened;
    REQUIRE (doc::CanonicalXml::read (written, reopened).ok);
    CHECK (doc::CanonicalXml::write (reopened) == written);
    CHECK (reopened.getAttribute ("/godot/binding/BND00001/number") == std::optional<std::string> ("7"));
}

TEST_CASE ("binding: one of a cue names it, and what is not a cue, or not there, is refused")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));
    Rig rig;

    REQUIRE (rig.send ("binding.create", { s (rig.cueId), s ("BND00002"), s ("target"), s ("level") }).rejected == 0);
    CHECK (rig.published ("/godot/binding/BND00002/cue") == rig.cueId);
    CHECK (rig.published ("/godot/binding/BND00002/target") == "level");

    CHECK (rig.send ("binding.create", { s ("ZZZZZZZZ") }).rejected == 1);
    CHECK (rig.send ("binding.create", { s (rig.listId) }).rejected == 1);

    //  Its cue and its identifier have their own places; a word the row does not take is refused.
    CHECK (rig.send ("binding.create", { s (""), s (""), s ("cue"), s (rig.cueId) }).rejected == 1);
    CHECK (rig.send ("binding.create", { s (""), s (""), s ("control"), s ("sideways") }).rejected == 1);
    CHECK (rig.send ("binding.create", { s (""), s (""), s ("source") }).rejected == 1);
}

TEST_CASE ("binding: an OSC address is one arriving on Go.dot's port, never one of the tree's")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));
    Rig rig;

    REQUIRE (rig.send ("binding.create", { s (""), s ("BND00003"), s ("source"), s ("osc"),
                                            s ("address"), s ("/desk/fader/1") }).rejected == 0);
    CHECK (rig.published ("/godot/binding/BND00003/address") == "/desk/fader/1");

    CHECK (rig.send ("binding.create", { s (""), s (""), s ("address"), s ("/godot/cue/x/level") }).rejected == 1);
    CHECK_FALSE (rig.document.setAttribute ("/godot/binding/BND00003/address", "/godot").ok);
    CHECK_FALSE (rig.document.setAttribute ("/godot/binding/BND00003/address", "fader").ok);
    CHECK (rig.document.setAttribute ("/godot/binding/BND00003/address", "").ok);

    //  And it goes as anything goes.
    CHECK (rig.send ("object.delete", { s ("BND00003") }).rejected == 0);
    CHECK (rig.published ("/godot/binding/order").empty());
}

TEST_CASE ("binding: a locked show gains none, nor an empty collection")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));
    Rig rig;

    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
    CHECK (rig.send ("binding.create", { s ("") }).rejected == 1);
    CHECK (doc::CanonicalXml::write (rig.document).find ("Bindings") == std::string::npos);
}
