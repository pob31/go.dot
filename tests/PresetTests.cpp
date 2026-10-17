// This file is part of Go.dot — https://github.com/pob31/go.dot
//
// Copyright (C) 2026 Pierre-Olivier Boulant
//
// Go.dot is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. Go.dot is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
// (LICENSE, at the repository root) for more details.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/tree/PresetTable.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace wfg::tree;

namespace
{
    /*  A preset the way a hand writes one (namespace draft §57, AFG): two
        roots, the GODOT key naming it, and its sources. */
    const char* twoRootDesk = R"JSON({
      "FULL_PATH": "/",
      "GODOT": { "PRESET": "desk-osc", "VERSION": 2, "VENDOR": "Desk Co", "MODEL": "Desk (OSC)",
                 "TRANSPORT": "udp", "WIRE": "osc", "PORT": 10023,
                 "SOURCES": ["Desk Co, Desk OSC Protocol, rev. 3, table 2"] },
      "CONTENTS": {
        "ch":  {"FULL_PATH": "/ch",  "CONTENTS": {"01": {"FULL_PATH": "/ch/01",  "CONTENTS": {"fader": {"FULL_PATH": "/ch/01/fader",  "TYPE": "f", "ACCESS": 3, "VALUE": [0.5]}}}}},
        "bus": {"FULL_PATH": "/bus", "CONTENTS": {"01": {"FULL_PATH": "/bus/01", "CONTENTS": {"fader": {"FULL_PATH": "/bus/01/fader", "TYPE": "f", "ACCESS": 3, "VALUE": [0.5]}}}}}
      }
    })JSON";

    std::string withGodot (const char* godot)
    {
        return std::string (R"JSON({"FULL_PATH": "/eos", "GODOT": )JSON") + godot
             + R"JSON(, "CONTENTS": {"cmd": {"FULL_PATH": "/eos/cmd", "TYPE": "s", "ACCESS": 2}}})JSON";
    }
}

TEST_CASE ("preset: one is read from its text, and what is wrong with it is said in a sentence")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    SUBCASE ("a good one: named, versioned, its roots the file's, its nodes counted")
    {
        const auto preset = readPreset ("desk-osc", twoRootDesk);
        INFO ("problem: " << preset.problem);
        REQUIRE (preset.usable());
        CHECK (preset.vendor == "Desk Co");
        CHECK (preset.model == "Desk (OSC)");
        CHECK (preset.version == 2);
        CHECK (preset.transport == "udp");
        CHECK (preset.wire == "osc");
        CHECK (preset.framing == "length");
        CHECK (preset.port == 10023);

        auto roots = preset.roots;
        std::sort (roots.begin(), roots.end());
        CHECK (roots == std::vector<std::string> { "/bus", "/ch" });
        CHECK (preset.rootRow().find ("/ch") != std::string::npos);
        CHECK (preset.nodeCount == 6);
        CHECK (preset.sources.size() == 1u);
        CHECK (preset.text == twoRootDesk);
    }

    SUBCASE ("a file named otherwise than its PRESET says")
    {
        const auto preset = readPreset ("desk", twoRootDesk);
        CHECK_FALSE (preset.usable());
        INFO ("said: " << preset.problem);
        CHECK (preset.problem.find ("desk-osc") != std::string::npos);
        CHECK (preset.problem.find ("\"desk\"") != std::string::npos);
    }

    SUBCASE ("no sources: refused, whatever else it has")
    {
        const auto preset = readPreset ("eos-osc", withGodot (R"JSON({"PRESET": "eos-osc", "VERSION": 1, "VENDOR": "ETC", "MODEL": "Eos (OSC)"})JSON"));
        CHECK_FALSE (preset.usable());
        INFO ("said: " << preset.problem);
        CHECK (preset.problem.find ("SOURCES") != std::string::npos);
    }

    SUBCASE ("a wire the transport cannot carry")
    {
        const auto preset = readPreset ("eos-osc", withGodot (R"JSON({"PRESET": "eos-osc", "VERSION": 1, "VENDOR": "ETC", "MODEL": "Eos (OSC)",
                                                                "TRANSPORT": "udp", "WIRE": "rcp", "SOURCES": ["a document"]})JSON"));
        CHECK_FALSE (preset.usable());
        INFO ("said: " << preset.problem);
        CHECK (preset.problem.find ("udp") != std::string::npos);
        CHECK (preset.problem.find ("rcp") != std::string::npos);
    }

    SUBCASE ("a file rooted at one address answers at that one, over TCP with its framing")
    {
        const auto preset = readPreset ("eos-osc", withGodot (R"JSON({"PRESET": "eos-osc", "VERSION": 1, "VENDOR": "ETC", "MODEL": "Eos (OSC)",
                                                                "TRANSPORT": "tcp", "WIRE": "osc", "FRAMING": "slip", "PORT": 3037,
                                                                "SOURCES": ["ETC, Eos Family Show Control User Guide, OSC"]})JSON"));
        INFO ("problem: " << preset.problem);
        REQUIRE (preset.usable());
        CHECK (preset.roots == std::vector<std::string> { "/eos" });
        CHECK (preset.framing == "slip");
        CHECK (preset.port == 3037);
        CHECK (preset.nodeCount == 2);
    }

    SUBCASE ("not a description at all")
    {
        CHECK_FALSE (readPreset ("x", "not json").usable());
        CHECK_FALSE (readPreset ("x", "[1, 2]").usable());
        CHECK_FALSE (readPreset ("x", R"JSON({"FULL_PATH": "/x", "CONTENTS": {}})JSON").usable());
        CHECK (readPreset ("x", R"JSON({"FULL_PATH": "/x", "CONTENTS": {}})JSON").problem.find ("GODOT") != std::string::npos);
    }
}

TEST_CASE ("preset: every shipped preset loads, is named after its file, and names its sources")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    /*  The folder the installer ships and serve reads (namespace draft §57,
        AFN), read here from the source tree: a preset that does not load is
        a preset nobody can make a device from, and this is where that is
        found before a tester does. */
    PresetTable table;
    table.scan (std::string (WFG_REPO_ROOT) + "/presets/devices");

    REQUIRE_MESSAGE (! table.all().empty(), "no preset found under " << table.folder());

    std::vector<std::string> slugs;

    for (const auto& preset : table.all())
    {
        INFO ("preset: " << preset.slug << " - " << preset.problem);
        CHECK (preset.usable());
        CHECK (! preset.sources.empty());
        CHECK (preset.nodeCount > 0);
        CHECK (! preset.roots.empty());
        CHECK (preset.version >= 1);
        CHECK (table.find (preset.slug) == &preset);
        slugs.push_back (preset.slug);
    }

    CHECK (std::is_sorted (slugs.begin(), slugs.end()));
    CHECK (table.find ("no-such-preset") == nullptr);

    /*  The shipped ones by name, so a generator that stops writing one is
        noticed; each stage adds its own. */
    for (const auto* expected : { "adm-osc", "dbaudio-ds100-osc", "digico-s-osc", "behringer-x32-osc", "behringer-wing-osc",
                                  "holophonix-osc", "lacoustics-lisa-osc", "flux-spat-osc", "malighting-grandma3-osc",
                                  "digico-sd-osc", "yamaha-osc", "etc-eos-osc", "yamaha-rcp",
                                  "malighting-grandma2-line", "allenheath-dlive-midi", "allenheath-avantis-midi",
                                  "allenheath-sq-midi", "allenheath-qu-midi", "msc", "behringer-x32-midi",
                                  "yamaha-midi", "digico-midi", "midas-hd96-midi", "ssl-live-midi" })
    {
        INFO ("expected: " << expected);
        CHECK (table.find (expected) != nullptr);
    }
}

TEST_CASE ("preset: a folder that is not there is an empty table, not a failure")
{
    PresetTable table;
    table.scan (std::string (WFG_REPO_ROOT) + "/presets/nowhere");
    CHECK (table.all().empty());
    CHECK (table.find ("adm-osc") == nullptr);
    CHECK (table.revision() == 1u);
}

TEST_CASE ("preset: the quick read says of every shipped preset what the whole read says")
{
    /*  DP.11: serve reads the folder quickly - the files' heads walked, the
        nodes counted by their FULL_PATH keys - and the Network tab's rows
        must not differ from what `wfg presets --check` reads whole. */
    PresetTable whole, quick;
    whole.scan (std::string (WFG_REPO_ROOT) + "/presets/devices");
    quick.scanQuickly (std::string (WFG_REPO_ROOT) + "/presets/devices");
    REQUIRE (whole.all().size() == quick.all().size());
    REQUIRE_FALSE (whole.all().empty());

    for (std::size_t at = 0; at < whole.all().size(); ++at)
    {
        const auto& a = whole.all()[at];
        const auto& b = quick.all()[at];
        INFO ("preset: " << a.slug);
        CHECK (b.slug == a.slug);
        CHECK (b.usable() == a.usable());
        CHECK (b.vendor == a.vendor);
        CHECK (b.model == a.model);
        CHECK (b.version == a.version);
        CHECK (b.transport == a.transport);
        CHECK (b.wire == a.wire);
        CHECK (b.framing == a.framing);
        CHECK (b.port == a.port);
        CHECK (b.readback == a.readback);
        CHECK (b.roots == a.roots);
        CHECK (b.nodeCount == a.nodeCount);
        CHECK (b.sources == a.sources);
        CHECK (b.text == a.text);
    }

    //  And what is not a preset is said the same way.
    const auto bad = readPresetQuickly ("x", "[1, 2]");
    CHECK_FALSE (bad.usable());
    const auto none = readPresetQuickly ("x", R"JSON({"FULL_PATH": "/x", "CONTENTS": {}})JSON");
    CHECK (none.problem.find ("no GODOT key") != std::string::npos);
}

TEST_CASE ("preset: yours are read beside the shipped ones, used in their place, and give them back when taken away")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    /*  Namespace draft §76, ART: a folder of the user's own, read beside the
        shipped one, a file there named like a shipped preset used in its
        place - and the shipped one kept aside, so that taking the file away
        gives it back without reading the shipped folder again. */
    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getNonexistentChildFile ("wfg-presets-yours-" + juce::Uuid().toString().substring (0, 8), {}, false);
    const auto shipped = root.getChildFile ("shipped");
    const auto yours = root.getChildFile ("yours");
    REQUIRE (shipped.createDirectory());
    REQUIRE (yours.createDirectory());

    std::string mine = twoRootDesk;
    const auto at = mine.find ("\"VERSION\": 2");
    REQUIRE (at != std::string::npos);
    mine.replace (at, 12, "\"VERSION\": 3");

    REQUIRE (shipped.getChildFile ("desk-osc.json").replaceWithText (twoRootDesk));
    REQUIRE (shipped.getChildFile ("eos-osc.json").replaceWithText (juce::String (withGodot (
        R"JSON({"PRESET": "eos-osc", "VERSION": 1, "VENDOR": "ETC", "MODEL": "Eos", "SOURCES": ["the manual"]})JSON"))));
    REQUIRE (yours.getChildFile ("desk-osc.json").replaceWithText (juce::String (mine)));
    REQUIRE (yours.getChildFile ("my-desk.json").replaceWithText (juce::String (withGodot (
        R"JSON({"PRESET": "my-desk", "VERSION": 1, "VENDOR": "Me", "MODEL": "My desk", "SOURCES": ["by hand"]})JSON"))));

    for (const auto quickly : { false, true })
    {
        INFO ("quickly: " << quickly);
        PresetTable table;

        if (quickly)
            table.scanQuickly (shipped.getFullPathName().toStdString(), yours.getFullPathName().toStdString());
        else
            table.scan (shipped.getFullPathName().toStdString(), yours.getFullPathName().toStdString());

        CHECK (table.yoursFolder() == yours.getFullPathName().toStdString());
        REQUIRE (table.all().size() == 3u);

        //  Yours in the shipped one's place: its version, its origin, the version it hides.
        const auto* desk = table.find ("desk-osc");
        REQUIRE (desk != nullptr);
        CHECK (desk->usable());
        CHECK (desk->version == 3);
        CHECK (desk->origin == "yours");
        CHECK (desk->shadows == 2);
        CHECK (desk->path == yours.getChildFile ("desk-osc.json").getFullPathName().toStdString());

        //  A shipped one nobody stands in for, and one of yours of a new name.
        REQUIRE (table.find ("eos-osc") != nullptr);
        CHECK (table.find ("eos-osc")->origin == "shipped");
        CHECK (table.find ("eos-osc")->shadows == 0);
        REQUIRE (table.find ("my-desk") != nullptr);
        CHECK (table.find ("my-desk")->origin == "yours");
        CHECK (table.find ("my-desk")->shadows == 0);

        std::vector<std::string> slugs;

        for (const auto& preset : table.all())
            slugs.push_back (preset.slug);

        CHECK (std::is_sorted (slugs.begin(), slugs.end()));

        //  Your folder read again with the file gone: the shipped one is back, and the revision moved.
        const auto before = table.revision();
        auto again = readPresetFolder (yours.getFullPathName().toStdString(), quickly, "yours");
        REQUIRE (again.size() == 2u);
        std::erase_if (again, [] (const Preset& each) { return each.slug == "desk-osc"; });
        table.adoptYours (std::move (again), yours.getFullPathName().toStdString());

        CHECK (table.revision() == before + 1);
        REQUIRE (table.all().size() == 3u);
        REQUIRE (table.find ("desk-osc") != nullptr);
        CHECK (table.find ("desk-osc")->origin == "shipped");
        CHECK (table.find ("desk-osc")->version == 2);
        CHECK (table.find ("my-desk") != nullptr);

        //  And nothing of yours at all leaves the shipped folder as it was.
        table.adoptYours ({}, {});
        CHECK (table.all().size() == 2u);
        CHECK (table.find ("my-desk") == nullptr);
        CHECK (table.yoursFolder().empty());
    }

    //  The shipped folder alone, as before: nothing is yours.
    PresetTable alone;
    alone.scan (shipped.getFullPathName().toStdString());
    REQUIRE (alone.all().size() == 2u);
    CHECK (alone.find ("desk-osc")->origin == "shipped");
    CHECK (alone.revision() == 1u);

    root.deleteRecursively();
}
