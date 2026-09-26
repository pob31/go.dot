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

/*  PHASE 9a'S CATALOGUE (§17.7): what a plugin's parameters are, without an
    instance in the room. A catalogue round-trips through its JSON under both
    locales; the store keeps one file per identifier under the folder it is
    given and nowhere else, and knows the test child's without a file; the
    tree publishes eight nodes per parameter under /godot/plugin/<id>/param/<n>
    and the machine's known list under /godot/plugin/known/<n>; and the bipolar
    guess reads what the texts at the ends say.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/client/model/Text.h>
#include <wfg/engine/Engine.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/plugin/Catalogue.h>
#include <wfg/engine/plugin/PluginScan.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>

#include <juce_core/juce_core.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace wfg;

namespace
{
    struct Folder
    {
        Folder()
        {
            root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                     .getChildFile ("wfg-catalogue-" + juce::String (juce::Random::getSystemRandom().nextInt64()));
            root.createDirectory();
        }

        ~Folder() { root.deleteRecursively(); }

        std::string path() const { return root.getFullPathName().toStdString(); }

        juce::File root;
    };

    plugin::Catalogue verb()
    {
        plugin::Catalogue out;
        out.identifier = "VST3-0badf00d-verb";
        out.name = "Verb";
        out.latencySamples = 64;

        plugin::Parameter size;
        size.name = "Room size";
        size.shortName = "Size";
        size.unit = "%";
        size.defaultValue = 0.25f;

        for (int i = 0; i <= 100; ++i)
            size.text[static_cast<std::size_t> (i)] = std::to_string (i) + " %";

        plugin::Parameter tilt;
        tilt.name = "Tilt";
        tilt.shortName = "Tilt";
        tilt.unit = "dB";
        tilt.defaultValue = 0.5f;

        for (int i = 0; i <= 100; ++i)
        {
            const auto db = (i - 50) * 0.24;
            tilt.text[static_cast<std::size_t> (i)] = juce::String (db, 1).toStdString() + " dB";
        }

        tilt.bipolar = plugin::Catalogue::guessBipolar (tilt.defaultValue, tilt.text[0], tilt.text[100]);

        plugin::Parameter mode;
        mode.name = "Mode";
        mode.shortName = "Mode";
        mode.discrete = true;
        mode.steps = 3;
        mode.stepText = { "hall", "plate", "spring" };

        for (int i = 0; i <= 100; ++i)
            mode.text[static_cast<std::size_t> (i)] = mode.stepText[static_cast<std::size_t> (i < 34 ? 0 : i < 67 ? 1 : 2)];

        out.params = { size, tilt, mode };
        return out;
    }

    struct Rig
    {
        Rig()
        {
            engine.log().openInMemory ({});
            doc::registerDocumentCommands (engine.commands(), document);
        }

        Engine::TickResult apply (const std::string& command, std::vector<osc::Value> args = {})
        {
            REQUIRE (engine.submit (std::string (origin::cli), command, std::move (args)));
            return engine.processTick (tick++);
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

        Engine engine;
        doc::ShowDocument document;
        tree::MountTable mounts;
        cue::RunTable runs;
        tree::ParameterTree parameters { document, engine.commands(), mounts, runs };
        tree::EngineState state;
        std::shared_ptr<const tree::TreeSnapshot> snapshot;
        std::int64_t tick = 1;
    };
}

//==============================================================================
TEST_CASE ("catalogue: a plugin's parameters round-trip through JSON, under either locale")
{
    const auto original = verb();
    const auto text = original.toJson();

    plugin::Catalogue back;
    std::string problem;

    REQUIRE (plugin::Catalogue::fromJson (text, back, problem));
    CHECK (problem.empty());
    CHECK (back.identifier == original.identifier);
    CHECK (back.name == "Verb");
    CHECK (back.latencySamples == 64);
    REQUIRE (back.params.size() == 3u);

    CHECK (back.params[0].name == "Room size");
    CHECK (back.params[0].unit == "%");
    CHECK (back.params[0].defaultValue == doctest::Approx (0.25f));
    CHECK (back.params[0].text[100] == "100 %");
    CHECK (back.params[0].textFor (0.5f) == "50 %");

    CHECK (back.params[1].bipolar);
    CHECK (back.params[1].textFor (0.0f).front() == '-');

    CHECK (back.params[2].discrete);
    CHECK (back.params[2].steps == 3);
    REQUIRE (back.params[2].stepText.size() == 3u);
    CHECK (back.params[2].textFor (0.0f) == "hall");
    CHECK (back.params[2].textFor (0.5f) == "plate");
    CHECK (back.params[2].textFor (1.0f) == "spring");

    //  And the same bytes again: the writer is deterministic.
    CHECK (back.toJson() == text);

    CHECK_FALSE (plugin::Catalogue::fromJson ("not json", back, problem));
    CHECK_FALSE (problem.empty());
}

TEST_CASE ("catalogue: the bipolar guess reads the texts at the ends")
{
    CHECK (plugin::Catalogue::guessBipolar (0.5f, "-12.0 dB", "+12.0 dB"));
    CHECK (plugin::Catalogue::guessBipolar (0.5f, "-100", "100"));
    CHECK_FALSE (plugin::Catalogue::guessBipolar (0.0f, "-inf dB", "0.0 dB"));     // rests at an end
    CHECK_FALSE (plugin::Catalogue::guessBipolar (0.5f, "L50", "R50"));           // a pan says it otherwise
    CHECK_FALSE (plugin::Catalogue::guessBipolar (0.5f, "20 Hz", "20 kHz"));
}

TEST_CASE ("catalogue: the store keeps one file per identifier under its folder, and knows the test child without one")
{
    Folder folder;
    plugin::CatalogueStore store { folder.path() };

    //  The test child's, always, with two parameters: a gain and the kill switch.
    const auto testGain = store.find (plugin::Catalogue::testGainIdentifier());
    REQUIRE (testGain != nullptr);
    REQUIRE (testGain->params.size() == 2u);
    CHECK (testGain->params[0].name == "Gain");
    CHECK (testGain->params[0].defaultValue == doctest::Approx (0.5f));
    CHECK (testGain->params[0].textFor (0.5f) == "-6.0 dB");
    CHECK (testGain->params[0].textFor (1.0f) == "0.0 dB");
    CHECK (testGain->params[1].discrete);
    CHECK (testGain->params[1].textFor (1.0f) == "dead");

    CHECK (store.find ("VST3-0badf00d-verb") == nullptr);
    CHECK_FALSE (store.ensureLoaded ("VST3-0badf00d-verb"));

    CHECK (store.put (verb()));
    CHECK_FALSE (store.put (verb()));                 // the same again changes nothing

    const juce::File file { juce::String (store.fileFor ("VST3-0badf00d-verb")) };
    CHECK (file.existsAsFile());
    CHECK (file.isAChildOf (folder.root));

    //  A second store on the same folder reads it back, on demand.
    plugin::CatalogueStore again { folder.path() };
    CHECK (again.find ("VST3-0badf00d-verb") == nullptr);
    REQUIRE (again.ensureLoaded ("VST3-0badf00d-verb"));
    REQUIRE (again.find ("VST3-0badf00d-verb") != nullptr);
    CHECK (again.find ("VST3-0badf00d-verb")->params.size() == 3u);

    /*  A FILE THAT NAMES ANOTHER IDENTIFIER is a file somebody edited, and is
        not believed: the hash the name is keyed by must agree with the inside. */
    auto other = verb();
    other.identifier = "VST3-deadbeef-other";
    file.replaceWithText (juce::String (other.toJson()));

    plugin::CatalogueStore third { folder.path() };
    CHECK_FALSE (third.ensureLoaded ("VST3-0badf00d-verb"));
}

TEST_CASE ("catalogue: the tree publishes a plugin's parameters and the machine's known list")
{
    Rig rig;

    REQUIRE (rig.apply ("plugin.create", { osc::Value::string ("Test gain"),
                                           osc::Value::string (plugin::Catalogue::testGainIdentifier()),
                                           osc::Value::string ("VST3"), osc::Value::string ("") }).applied == 1);
    REQUIRE (rig.apply ("plugin.create", { osc::Value::string ("Verb"),
                                           osc::Value::string ("VST3-0badf00d-verb"),
                                           osc::Value::string ("VST3"), osc::Value::string ("") }).applied == 1);

    Folder folder;
    plugin::CatalogueStore store { folder.path() };
    rig.parameters.setCatalogues (&store);

    const auto order = rig.at ("/godot/plugin/order");
    REQUIRE (order.size() == 17u);
    const auto gainId = order.substr (0, 8);
    const auto verbId = order.substr (9, 8);

    /*  THE TEST CHILD'S, from the store's built-in: eight nodes a parameter
        and the count beside them, though nothing has loaded anything. */
    const auto param = "/godot/plugin/" + gainId + "/param/";
    CHECK (rig.at ("/godot/plugin/" + gainId + "/paramCount") == "2");
    CHECK (rig.at (param + "0/name") == "Gain");
    CHECK (rig.at (param + "0/shortName") == "Gain");
    CHECK (rig.at (param + "0/unit") == "dB");
    CHECK (rig.at (param + "0/default") == "0.5");
    CHECK (rig.at (param + "0/min") == "0");
    CHECK (rig.at (param + "0/max") == "1");
    CHECK (rig.at (param + "0/steps") == "0");
    CHECK (rig.at (param + "0/bipolar") == "false");
    CHECK (rig.at (param + "1/name") == "Die");
    CHECK (rig.at (param + "1/steps") == "2");
    CHECK_FALSE (rig.exists (param + "2/name"));

    //  The verb's are not known yet: no file, no nodes, and an honest nought.
    CHECK (rig.at ("/godot/plugin/" + verbId + "/paramCount") == "0");
    CHECK_FALSE (rig.exists ("/godot/plugin/" + verbId + "/param/0/name"));

    SUBCASE ("and a catalogue arriving is a rebuild, with a bipolar parameter saying so")
    {
        store.put (verb());
        const auto verbParam = "/godot/plugin/" + verbId + "/param/";

        CHECK (rig.at ("/godot/plugin/" + verbId + "/paramCount") == "3");
        CHECK (rig.at (verbParam + "1/name") == "Tilt");
        CHECK (rig.at (verbParam + "1/bipolar") == "true");
        CHECK (rig.at (verbParam + "2/steps") == "3");
    }

    SUBCASE ("and the machine's known list is published beside the set")
    {
        plugin::KnownList known;
        known.set ({ { "Verb", "VST3-0badf00d-verb", "VST3", "Someone", "C:/plugins/verb.vst3", {} },
                     { "Comp", "VST3-c0ffee00-comp", "VST3", "Somebody", "C:/plugins/comp.vst3", {} } });

        rig.parameters.setKnownList (&known);

        CHECK (rig.at ("/godot/plugin/known/0/name") == "Verb");
        CHECK (rig.at ("/godot/plugin/known/0/identifier") == "VST3-0badf00d-verb");
        CHECK (rig.at ("/godot/plugin/known/0/format") == "VST3");
        CHECK (rig.at ("/godot/plugin/known/0/manufacturer") == "Someone");
        CHECK (rig.at ("/godot/plugin/known/1/name") == "Comp");
        CHECK_FALSE (rig.exists ("/godot/plugin/known/2/name"));

        /*  A LIST REFILLED ON ANOTHER THREAD reaches the next publish with
            nobody marking the tree stale: the revision is compared. */
        const auto before = rig.parameters.publish (rig.tick, rig.state);
        known.set ({ { "Amp", "LV2-amp", "LV2", "Someone else", "/usr/lib/lv2/amp.lv2", {} } });
        const auto after = rig.parameters.publish (rig.tick, rig.state);

        CHECK (client::model::text (*before, "/godot/plugin/known/0/name") == "Verb");
        CHECK (client::model::text (*after, "/godot/plugin/known/0/name") == "Amp");
        CHECK (client::model::text (*after, "/godot/plugin/known/0/format") == "LV2");
    }
}

TEST_CASE ("catalogue: the format words, and a scan of nowhere knows nothing")
{
    const auto words = plugin::formatWords();
    REQUIRE (words.size() == 3u);
    CHECK (words[0] == "vst3");

    /*  A folder nothing was ever scanned into: the known list is empty, which
        is what `wfg plugins --list` turns into its sentence. Stands an engine
        up with no device, which every CI runner can do. */
    Folder folder;
    CHECK (plugin::knownPlugins (folder.path()).empty());

    std::string problem;
    std::vector<std::string> skipped;
    CHECK (plugin::scanPlugins (folder.path(), "nope", "", false, skipped, problem).empty());
    CHECK_FALSE (problem.empty());
    CHECK (plugin::skippedPlugins (folder.path()).empty());
}
