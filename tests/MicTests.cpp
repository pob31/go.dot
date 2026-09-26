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

/*  PHASE 9b'S MIC CUE IN THE DOCUMENT (namespace draft 18.2, decisions BW, BX
    and CE): a live input played as a cue. What the show says about one before
    it makes a sound - that it is a cue first, a sound second and a live input
    third, published with a media cue's sound rows and its own three and
    nothing about a file; that its inserts are its channel's plugins and never
    the set's, and a media cue's never a channel's; that it takes the children
    a sound is made of and refuses the ones that name a file; and that `wfg
    validate` says, before the show, each reason one would fail when fired.

    Driven through the engine, because what a replay reproduces is the applied
    record.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/client/model/Text.h>
#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace wfg;

namespace
{
    juce::File micBundle()
    {
        const juce::File folder { juce::String (std::string (WFG_TEST_FIXTURES_DIR)) + "/bundles/mic" };

        REQUIRE_MESSAGE (folder.isDirectory(), "missing fixture bundle: " << folder.getFullPathName());
        return folder;
    }

    /*  The fixture: list MC000001 holding mic cue MC000002 ("Voix solo", on
        input MC000021, through channel MC000011 "Vox 1", mono to stereo, with
        its test-gain plugin MC000012 switched in as Fx MC000005) and memo
        MC000006; a stereo input MC000022 "Keys"; bus MC000004. */
    struct Rig
    {
        Rig()
        {
            engine.log().openInMemory ({});
            REQUIRE (doc::Bundle::open (micBundle(), document).ok);
            doc::registerDocumentCommands (engine.commands(), document);
            cue::registerCueCommands (engine.commands(), document, focus);

            engine.setBeforeApply ([this] (const Command& appliedCommand, const Event& submitted,
                                           const std::vector<osc::Value>& coerced,
                                           std::int64_t tickIndex)
                                   {
                                       document.beginTransaction (appliedCommand.name, tickIndex,
                                                                  submitted.origin, coerced);
                                   });
        }

        Engine::TickResult apply (const std::string& command, std::vector<osc::Value> args = {})
        {
            REQUIRE (engine.submit (std::string (origin::cli), command, std::move (args)));
            return engine.processTick (tick++);
        }

        bool applied (const std::string& command, std::vector<osc::Value> args = {})
        {
            return apply (command, std::move (args)).applied == 1;
        }

        std::string at (const std::string& address)
        {
            parameters.markStale();
            snapshot = parameters.publish (tick, state);
            return client::model::text (*snapshot, address);
        }

        bool exists (const std::string& address)
        {
            parameters.markStale();
            snapshot = parameters.publish (tick, state);
            return snapshot->find (address) != nullptr;
        }

        /*  What `wfg validate` says that mentions this text: the errors and
            the warnings both, as the verb prints them. */
        std::vector<std::string> problemsAbout (const std::string& text) const
        {
            std::vector<std::string> out;

            for (const auto& said : { document.validate(), document.warnings() })
                for (const auto& problem : said)
                    if (problem.find (text) != std::string::npos)
                        out.push_back (problem);

            return out;
        }

        Engine engine;
        doc::ShowDocument document;
        cue::Focus focus;
        tree::MountTable mounts;
        cue::RunTable runs;
        tree::ParameterTree parameters { document, engine.commands(), mounts, runs };
        tree::EngineState state;
        std::shared_ptr<const tree::TreeSnapshot> snapshot;
        std::int64_t tick = 1;
    };

    osc::Value text (const char* value) { return osc::Value::string (value); }
    osc::Value text (const std::string& value) { return osc::Value::string (value); }
}

//==============================================================================
TEST_CASE ("mic: a mic cue is a cue, then a sound, then a live input - and nothing about a file")
{
    Rig rig;

    /*  THE FIXTURE AS PUBLISHED: its kind, its own three rows, and a media
        cue's sound rows at their defaults. */
    const std::string cue = "/godot/cue/MC000002/";
    CHECK (rig.at (cue + "kind") == "mic");
    CHECK (rig.at (cue + "input") == "MC000021");
    CHECK (rig.at (cue + "channel") == "MC000011");
    CHECK (rig.at (cue + "fadeIn") == "0.5");
    CHECK (rig.at (cue + "level") == "0");
    CHECK (rig.at (cue + "eqB1Freq") == "100");
    CHECK (rig.exists (cue + "directOut"));
    CHECK (rig.exists (cue + "sends"));

    for (const auto* fileRow : { "file", "startOffset", "channels", "duration", "hash", "initialLevel", "strip" })
    {
        INFO (fileRow);
        CHECK_FALSE (rig.exists (cue + fileRow));
    }

    /*  MADE AS ANY CUE IS, from the new-cue bar's command: empty input and
        channel, which is what a new mic cue starts as, and a fade-in of nought. */
    REQUIRE (rig.applied ("cue.create", { text ("MC000001"), osc::Value::int32 (2), text ("mic"),
                                          text ("Keys"), text ("MC000030") }));

    const std::string made = "/godot/cue/MC000030/";
    CHECK (rig.at (made + "kind") == "mic");
    CHECK (rig.at (made + "input").empty());
    CHECK (rig.at (made + "channel").empty());
    CHECK (rig.at (made + "fadeIn") == "0");

    REQUIRE (rig.applied ("node.set", { text (made + "input"), text ("MC000022") }));
    REQUIRE (rig.applied ("node.set", { text (made + "channel"), text ("MC000011") }));
    REQUIRE (rig.applied ("node.set", { text (made + "fadeIn"), text ("2") }));
    CHECK (rig.at (made + "input") == "MC000022");
    CHECK (rig.at (made + "fadeIn") == "2");

    /*  THE CHILDREN A SOUND IS MADE OF, and none of those that name a file. */
    CHECK (rig.applied ("route.create", { text ("MC000030"), text ("MC000004") }));
    CHECK (rig.applied ("send.create", { text ("MC000030"), text ("MC000004") }));
    CHECK_FALSE (rig.applied ("range.create", { text ("MC000030"), osc::Value::float64 (0.0),
                                                osc::Value::float64 (1.0) }));

    /*  Its EQ is a sound row like the rest, and eq.reset takes it back to flat. */
    REQUIRE (rig.applied ("node.set", { text (made + "eqB2Gain"), text ("6") }));
    CHECK (rig.at (made + "eqB2Gain") == "6");
    CHECK (rig.applied ("eq.reset", { text ("MC000030") }));
    CHECK (rig.at (made + "eqB2Gain") == "0");
}

TEST_CASE ("mic: its inserts are its channel's plugins, never the set's - and a media cue's never a channel's")
{
    Rig rig;

    /*  The fixture's insert names the channel's plugin, and is published with
        that plugin's name and its place in the channel's chain. */
    CHECK (rig.at ("/godot/cue/MC000002/fx") == "MC000005");
    CHECK (rig.at ("/godot/fx/MC000005/name") == "Test gain");
    CHECK (rig.at ("/godot/fx/MC000005/index") == "0");

    /*  A second plugin on Vox 1, one in the show's set, and a media cue. */
    REQUIRE (rig.applied ("channel.plugin", { text ("MC000011"), text ("Verb"), text ("godot:test-gain"),
                                              text ("VST3"), text (""), text ("MC000013") }));
    REQUIRE (rig.applied ("plugin.create", { text ("Set gain"), text ("godot:test-gain"), text ("VST3"),
                                             text (""), text ("MC000040") }));
    REQUIRE (rig.applied ("cue.create", { text ("MC000001"), osc::Value::int32 (2), text ("media"),
                                          text ("A file"), text ("MC000041") }));

    /*  THE CHANNEL'S SECOND PLUGIN: taken, at its place in the channel's chain. */
    REQUIRE (rig.applied ("fx.create", { text ("MC000002"), text ("MC000013"), text ("MC000050") }));
    CHECK (rig.at ("/godot/fx/MC000050/index") == "1");
    CHECK (rig.at ("/godot/cue/MC000002/fx") == "MC000005 MC000050");

    /*  THE SET'S PLUGIN ON A MIC CUE, and the channel's on a media cue: each is
        the wrong kind of plugin for that cue, bad-value, and nothing is made. */
    const auto setOnMic = rig.apply ("fx.create", { text ("MC000002"), text ("MC000040") });
    CHECK (setOnMic.applied == 0);
    CHECK (rig.engine.lastError().find ("bad-value") != std::string::npos);

    const auto channelOnMedia = rig.apply ("fx.create", { text ("MC000041"), text ("MC000013") });
    CHECK (channelOnMedia.applied == 0);
    CHECK (rig.engine.lastError().find ("bad-value") != std::string::npos);

    /*  And the set's plugin on the media cue, as ever. */
    CHECK (rig.applied ("fx.create", { text ("MC000041"), text ("MC000040") }));

    /*  A MIC CUE WHOSE CHANNEL CHANGES: its insert still names the plugin it
        named, which is no longer on its channel, and so names nothing - its
        name and index are empty rather than another channel's. */
    REQUIRE (rig.applied ("channel.create", { text ("stereo"), text ("MC000060") }));
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/MC000002/channel"), text ("MC000060") }));
    CHECK (rig.at ("/godot/cue/MC000002/fx").empty());
    CHECK (rig.at ("/godot/fx/MC000005/name").empty());
}

TEST_CASE ("mic: wfg validate says, before the show, each reason a mic cue would fail when fired")
{
    Rig rig;

    /*  The fixture is sound. */
    CHECK (rig.problemsAbout ("Mic[").empty());

    const auto set = [&rig] (const std::string& row, const std::string& value)
    {
        REQUIRE (rig.applied ("node.set", { text ("/godot/cue/MC000002/" + row), text (value) }));
    };

    SUBCASE ("no input, no channel")
    {
        set ("input", "");
        set ("channel", "");

        const auto said = rig.problemsAbout ("Mic[MC000002]");
        REQUIRE (said.size() == 2u);
        CHECK (said[0].find ("takes no input") != std::string::npos);
        CHECK (said[1].find ("plays through no rack channel") != std::string::npos);
    }

    SUBCASE ("a shared channel")
    {
        REQUIRE (rig.applied ("node.set", { text ("/godot/slot/MC000011/access"), text ("shared") }));

        const auto said = rig.problemsAbout ("Mic[MC000002]");
        REQUIRE (said.size() == 1u);
        CHECK (said[0].find ("Vox 1, a shared channel") != std::string::npos);
        CHECK (said[0].find ("bad-channel") != std::string::npos);
    }

    SUBCASE ("an input its channel cannot take")
    {
        //  Keys is stereo; Vox 1 is mono to stereo and takes one channel.
        set ("input", "MC000022");

        auto said = rig.problemsAbout ("Mic[MC000002]");
        REQUIRE (said.size() == 1u);
        CHECK (said[0].find ("takes Keys, stereo, through Vox 1, which takes mono") != std::string::npos);
        CHECK (said[0].find ("bad-width") != std::string::npos);

        //  A stereo channel takes it; the mono voice then does not fit.
        REQUIRE (rig.applied ("node.set", { text ("/godot/slot/MC000011/class"), text ("stereo") }));
        CHECK (rig.problemsAbout ("Mic[MC000002]").empty());

        set ("input", "MC000021");
        said = rig.problemsAbout ("Mic[MC000002]");
        REQUIRE (said.size() == 1u);
        CHECK (said[0].find ("takes Voix solo, mono, through Vox 1, which takes stereo") != std::string::npos);
    }

    SUBCASE ("an input or a channel that is the wrong kind of thing")
    {
        set ("input", "MC000004");
        set ("channel", "MC000021");

        const auto said = rig.problemsAbout ("[MC000002]");
        CHECK (std::any_of (said.begin(), said.end(), [] (const std::string& problem)
                            { return problem.find ("@input: names \"MC000004\", which is a bus and not a input")
                                       != std::string::npos; }));
        CHECK (std::any_of (said.begin(), said.end(), [] (const std::string& problem)
                            { return problem.find ("@channel: names \"MC000021\", which is a input and not a"
                                                   " rackChannel") != std::string::npos; }));
    }
}

TEST_CASE ("mic: a mic cue's direct out is let go with the bus it named, as a media cue's is")
{
    Rig rig;

    REQUIRE (rig.applied ("bus.create", { text ("direct"), osc::Value::int32 (1), osc::Value::int32 (-1),
                                          text ("MC000070") }));
    REQUIRE (rig.applied ("node.set", { text ("/godot/cue/MC000002/directOut"), text ("MC000070") }));
    CHECK (rig.at ("/godot/cue/MC000002/directOut") == "MC000070");

    REQUIRE (rig.applied ("bus.delete", { text ("MC000070") }));
    CHECK (rig.at ("/godot/cue/MC000002/directOut").empty());
}
