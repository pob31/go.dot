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
    The compiled client's model half: what a window would show, with no window.

    wfg_client_model names no JUCE type, so everything a label reads, every
    theme token and every gesture's Event can be asserted here with the same
    rig TreeTests.cpp uses - an engine, the minimal bundle and a ParameterTree
    - and under both locales, because the strings a cell shows are a
    serialisation surface like any other (a tick that read "1 234 567" under
    fr_FR would be the page and the window disagreeing about one number).

    What is NOT here, and is written down as untestable in ui/Client.cpp: that
    the window opens, layout, colour on screen, hit-testing, focus, the
    dialogue, timing, the shutdown order. The first person to find a broken
    window is the author, on a build.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/client/model/Gestures.h>
#include <wfg/client/model/Text.h>
#include <wfg/client/model/Theme.h>
#include <wfg/client/model/Transport.h>
#include <wfg/engine/Engine.h>
#include <wfg/engine/command/Event.h>
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/tree/TreeCommands.h>

#include <juce_core/juce_core.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace wfg;
using namespace wfg::client;
using namespace wfg::tree;

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

    /*  TreeTests.cpp's rig, with the readings serve fills in before the loop
        (Console.cpp:2102-2113, :2139-2141, :2699) so that the transport has a
        show name, a clock and a rate to read. */
    struct Rig
    {
        Rig()
        {
            REQUIRE (doc::Bundle::open (fixtureBundle(), document).ok);
            doc::registerDocumentCommands (engine.commands(), document);
            registerTreeCommands (engine.commands(), touches);
        }

        Engine::TickResult apply (std::int64_t tick, const std::string& origin,
                                  const std::string& command, std::vector<osc::Value> args = {})
        {
            REQUIRE (engine.submit (origin, command, std::move (args)));
            const auto result = engine.processTick (tick);
            parameters.markStale();
            return result;
        }

        std::shared_ptr<const TreeSnapshot> publish (std::int64_t tick)
        {
            EngineState state;
            state.version = "test";
            state.tick = tick;
            state.sampleRate = 48000;
            state.blockSize = 256;
            state.clock = "dummy";      // what serve publishes under --hosted too (Console.cpp:2699); the row's range is dummy|device
            state.audioStatus = "running";
            state.documentName = "minimal";
            state.documentRevision = document.showRevision();
            state.errorCount = engine.errorCount();
            state.lastError = engine.lastError();
            return parameters.publish (tick, state);
        }

        Engine engine;
        doc::ShowDocument document;
        TouchTable touches;
        MountTable mounts;
        cue::RunTable runs;
        ParameterTree parameters { document, engine.commands(), mounts, runs };
    };
}

//==============================================================================
TEST_CASE ("client: a value's text is the log's and the page's, under either locale")
{
    CHECK (model::text (osc::Value::float64 (0.5)) == "0.5");
    CHECK (model::text (osc::Value::float64 (1234567.25)) == "1234567.25");
    CHECK (model::text (osc::Value::float32 (0.1f)) == "0.1");
    CHECK (model::text (osc::Value::int64 (-12)) == "-12");
    CHECK (model::text (osc::Value::int32 (1234567)) == "1234567");     // no grouping, fr_FR included
    CHECK (model::text (osc::Value::boolean (true)) == "true");
    CHECK (model::text (osc::Value::boolean (false)) == "false");
    CHECK (model::text (osc::Value::string ("Announce")) == "Announce");
    CHECK (model::text (osc::Value::nil()).empty());
    CHECK (model::text (osc::Value::impulse()).empty());
    CHECK (model::text (static_cast<const Node*> (nullptr)).empty());

    CHECK (model::words ("A B  C") == std::vector<std::string> { "A", "B", "C" });
    CHECK (model::words (" A ") == std::vector<std::string> { "A" });
    CHECK (model::words ("").empty());
}

TEST_CASE ("client: the transport is read out of one snapshot, at the addresses the page reads")
{
    Rig rig;

    auto reading = model::readTransport (*rig.publish (0));

    CHECK (reading.show == "minimal");
    CHECK (reading.tick == "0");
    CHECK (reading.clock == "dummy");
    CHECK (reading.rate == "48000 / 256");
    CHECK (reading.status == "running");
    CHECK (reading.listId == "7K2QM9X4");             // state.xml names no focus: the first list
    CHECK (reading.listName == "Main");
    CHECK (reading.standbyId == "B3N8R5TW");          // state.xml's standby
    CHECK (reading.standbyName == "House to half");
    CHECK (reading.standbyLine().rfind ("House to half", 0) == 0);
    CHECK (reading.lastError.empty());
    CHECK (reading.revision >= 1);
    CHECK_FALSE (reading.dirty);
    CHECK_FALSE (reading.locked);

    /*  THE TICK IS DIGITS, under fr_FR too: a number that went through a
        locale would read "1 234 567" here and disagree with the page. */
    reading = model::readTransport (*rig.publish (1234567));
    CHECK (reading.tick == "1234567");

    /*  A standby move, applied - a refused write would leave the old name in
        place and let the case pass for the wrong reason. */
    REQUIRE (rig.apply (2, "cli", "node.set",
                        { osc::Value::string ("/godot/list/7K2QM9X4/standby"),
                          osc::Value::string ("F7HR8TVD") }).applied == 1);

    reading = model::readTransport (*rig.publish (2));
    CHECK (reading.standbyId == "F7HR8TVD");
    CHECK (reading.standbyName == "Announce");

    // And a refusal reaches the strip as the engine's own sentence.
    CHECK (rig.apply (3, "cli", "node.set",
                      { osc::Value::string ("/godot/cue/ZZZZZZZZ/name"),
                        osc::Value::string ("x") }).applied == 0);

    reading = model::readTransport (*rig.publish (3));
    CHECK_FALSE (reading.lastError.empty());

    // Two readings of the same state are equal, which is what the window's redraw test relies on.
    CHECK (model::readTransport (*rig.publish (3)) == model::readTransport (*rig.publish (3)));
}

TEST_CASE ("client: the standby line says so in words when there is none")
{
    model::TransportReading reading;
    CHECK (reading.standbyLine() == "no standby");

    reading.standbyId = "X";
    reading.standbyName = "Thunder";
    reading.standbyKind = "media";
    CHECK (reading.standbyLine() == "Thunder  media");

    reading.standbyKind.clear();
    CHECK (reading.standbyLine() == "Thunder");
}

//==============================================================================
TEST_CASE ("client: a theme is complete from construction, and a file changes only what it names")
{
    model::Theme theme;

    for (const auto& name : model::Theme::colourNames())
    {
        CHECK_MESSAGE (theme.colour (name) != 0xFFFF00FFu, name << " is undeclared");
        CHECK_MESSAGE ((theme.colour (name) & 0xFFFFFFu) != 0u, name << " is black");
    }

    CHECK (theme.colour ("nobody-declared-this") == 0xFFFF00FFu);
    CHECK (theme.colour ("standby") == 0xFFE8B04Bu);

    CHECK (theme.apply (R"({ "standby": "#ff0000", "type": 1.5, "about": "a note" })").empty());
    CHECK (theme.colour ("standby") == 0xFFFF0000u);
    CHECK (theme.colour ("ink") == 0xFFE8E6E1u);                 // untouched
    CHECK (theme.type == doctest::Approx (1.5));

    const auto before = theme;

    const auto broken = theme.apply ("{ \"ink\": \"#ffffff\", ");
    CHECK_FALSE (broken.empty());
    CHECK (theme == before);

    /*  REFUSED WHOLE: the ink that came first did not land either, because a
        half-applied look hides the sentence about the half that failed. */
    const auto misnamed = theme.apply (R"({ "ink": "#ffffff", "inc": "#000000" })");
    CHECK (misnamed.find ("inc") != std::string::npos);
    CHECK (theme == before);

    const auto notAColour = theme.apply (R"({ "standby": "red" })");
    CHECK (notAColour.find ("standby") != std::string::npos);
    CHECK (theme == before);

    CHECK_FALSE (theme.apply (R"({ "type": -1 })").empty());
    CHECK_FALSE (theme.apply (R"({ "refreshHz": "fast" })").empty());
    CHECK_FALSE (theme.apply ("[1, 2, 3]").empty());
    CHECK (theme == before);

    CHECK (theme.apply (R"({ "picked": "#9a95e480" })").empty());   // rrggbbaa
    CHECK (theme.colour ("picked") == 0x809A95E4u);
}

TEST_CASE ("client: the committed theme file is the defaults, transcribed")
{
    /*  clients/desktop/theme.json exists so the author can change it; until
        they do, it must say exactly what the defaults say, or the window
        opens looking different from the page for no reason anybody chose. */
    const juce::File file { juce::File (juce::String (std::string (WFG_REPO_ROOT)))
                              .getChildFile ("clients/desktop/theme.json") };

    REQUIRE_MESSAGE (file.existsAsFile(), "missing " << file.getFullPathName());

    model::Theme theme;
    CHECK (theme.apply (file.loadFileAsString().toStdString()).empty());
    CHECK (theme == model::Theme {});
}

//==============================================================================
TEST_CASE ("client: a gesture is the event it submits, with the window's origin")
{
    const auto event = gesture::go();

    CHECK (event.origin == "window");
    CHECK (event.origin == std::string (origin::window));
    CHECK (event.command == "go");
    CHECK (event.args.empty());
}
