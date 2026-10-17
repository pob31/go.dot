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

/*
    YOUR PRESETS, AND DEVICES FOUND ON THE NETWORK (namespace draft §76).

    The commands this round adds, against a real engine and document as serve
    registers them: your folder read again (`preset.rescan`), a device saved
    as a preset (`preset.save`), a device made from an address
    (`mount.describeAt`, `mount.discovered`). Every folder is a temporary one of
    the case's own: the developer's presets are no test's business (ASB).
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/Engine.h>
#include <wfg/engine/command/Command.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/log/Replay.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/PresetCommands.h>
#include <wfg/engine/tree/PresetScanner.h>
#include <wfg/engine/tree/PresetTable.h>
#include <wfg/engine/tree/TreeCommands.h>

#include "TestSupport.h"

#include <juce_core/juce_core.h>

#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace wfg;

namespace
{
    osc::Value text (const std::string& s) { return osc::Value::string (s); }

    /*  UNIQUE PER PROCESS, not only per call: the C and fr_FR runs of this
        suite may run at once. */
    juce::File freshFolder (const char* stem)
    {
        return juce::File::getSpecialLocation (juce::File::tempDirectory)
                 .getNonexistentChildFile (juce::String (stem) + "-" + juce::Uuid().toString().substring (0, 8), {}, false);
    }

    /*  A preset the size of a hand-written one, in a file named for it. */
    std::string smallPreset (const std::string& slug, int version)
    {
        return R"JSON({"FULL_PATH": "/desk", "GODOT": {"PRESET": ")JSON" + slug + R"JSON(", "VERSION": )JSON"
             + std::to_string (version)
             + R"JSON(, "VENDOR": "Desk Co", "MODEL": "Desk", "SOURCES": ["by hand"]},
                 "CONTENTS": {"go": {"FULL_PATH": "/desk/go", "TYPE": "N", "ACCESS": 2}}})JSON";
    }

    /*  An engine and a document as serve has them, one undo step a command. */
    struct Rig
    {
        Rig()
        {
            engine.log().openInMemory ({});
            doc::registerDocumentCommands (engine.commands(), document);

            engine.setBeforeApply ([this] (const Command& command, const Event& submitted,
                                           const std::vector<osc::Value>& coerced, std::int64_t tickIndex)
                                   {
                                       document.beginTransaction (command.name, tickIndex, submitted.origin, coerced);
                                   });
        }

        Engine::TickResult apply (const std::string& origin, const std::string& command, std::vector<osc::Value> args)
        {
            REQUIRE (engine.submit (origin, command, std::move (args)));
            return engine.processTick (tick++);
        }

        LogRecord lastRecord()
        {
            const auto parsed = LogFile::parse (engine.log().contents());
            REQUIRE (! parsed.records.empty());
            return parsed.records.back();
        }

        std::string attribute (const std::string& address) const
        {
            return document.getAttribute (address).value_or (std::string {});
        }

        Engine engine;
        doc::ShowDocument document;
        tree::MountTable mounts;
        std::int64_t tick = 1;
    };

    /*  What the scanner has read, waited for as the tick waits: taken when
        it is there, or nothing after a few seconds. */
    std::optional<std::vector<tree::Preset>> takeWithin (tree::PresetScanner& scanner, int milliseconds = 4000)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds (milliseconds);

        while (std::chrono::steady_clock::now() < deadline)
        {
            if (auto read = scanner.take())
                return read;

            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        }

        return std::nullopt;
    }
}

//==============================================================================
TEST_CASE ("discovery: your folder is read again on a thread of its own when asked, and taken once")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    /*  Namespace draft §76, ARZ: the reading is the scanner's, the taking the
        tick's, and nothing is read until somebody asks. */
    const auto yours = freshFolder ("wfg-presets-scan");
    REQUIRE (yours.createDirectory());
    REQUIRE (yours.getChildFile ("my-desk.json").replaceWithText (juce::String (smallPreset ("my-desk", 1))));

    tree::PresetScanner scanner;
    scanner.setFolder (yours.getFullPathName().toStdString());
    REQUIRE (scanner.start());
    CHECK_FALSE (scanner.start());

    //  Nothing asked, nothing read.
    std::this_thread::sleep_for (std::chrono::milliseconds (50));
    CHECK_FALSE (scanner.take().has_value());

    scanner.rescan();
    auto first = takeWithin (scanner);
    REQUIRE (first.has_value());
    REQUIRE (first->size() == 1u);
    CHECK (first->front().slug == "my-desk");
    CHECK (first->front().origin == "yours");
    CHECK (first->front().usable());

    //  Taken once: the next take has nothing until another reading.
    CHECK_FALSE (scanner.take().has_value());

    //  A file added, asked again: both are read.
    REQUIRE (yours.getChildFile ("other-desk.json").replaceWithText (juce::String (smallPreset ("other-desk", 2))));
    scanner.rescan();
    auto second = takeWithin (scanner);
    REQUIRE (second.has_value());
    CHECK (second->size() == 2u);

    //  Into a table: yours swapped in, the revision moved.
    tree::PresetTable table;
    const auto before = table.revision();
    table.adoptYours (std::move (*second), scanner.folder());
    CHECK (table.revision() == before + 1);
    CHECK (table.find ("other-desk") != nullptr);
    CHECK (table.find ("other-desk")->version == 2);

    //  A folder not there reads as nothing of yours, not as a failure.
    tree::PresetScanner nowhere;
    nowhere.setFolder (yours.getChildFile ("not-there").getFullPathName().toStdString());
    REQUIRE (nowhere.start());
    nowhere.rescan();
    auto none = takeWithin (nowhere);
    REQUIRE (none.has_value());
    CHECK (none->empty());
    CHECK (nowhere.status().problem.empty());

    scanner.stop();
    nowhere.stop();
    yours.deleteRecursively();
}

TEST_CASE ("discovery: preset.rescan asks the hook, edits no show, and is this machine's alone")
{
    Rig rig;
    int asked = 0;
    tree::registerPresetCommands (rig.engine.commands(), rig.document, rig.mounts, freshFolder ("wfg-bundle"), nullptr,
                                  { [&asked] { ++asked; } });

    const auto* command = rig.engine.commands().find ("preset.rescan");
    REQUIRE (command != nullptr);
    CHECK (command->machineLocal);
    CHECK_FALSE (command->mutates);

    const auto revision = rig.document.showRevision();
    REQUIRE (rig.apply ("window", "preset.rescan", {}).applied == 1u);
    CHECK (asked == 1);
    CHECK (rig.document.showRevision() == revision);

    //  Under the lock too: reading a folder is no edit of the show.
    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
    REQUIRE (rig.apply ("window", "preset.rescan", {}).applied == 1u);
    CHECK (asked == 2);

    //  With no hook - a replay - it is taken and does nothing.
    Rig replay;
    tree::registerPresetCommands (replay.engine.commands(), replay.document, replay.mounts, freshFolder ("wfg-bundle"), nullptr);
    CHECK (replay.apply ("window", "preset.rescan", {}).applied == 1u);
}
