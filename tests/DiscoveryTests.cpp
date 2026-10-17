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
#include <wfg/engine/json/JsonValue.h>
#include <wfg/engine/tree/DiscoveryCommands.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/MountFetcher.h>
#include <wfg/engine/tree/PresetCommands.h>
#include <wfg/engine/tree/PresetScanner.h>
#include <wfg/engine/tree/PresetTable.h>
#include <wfg/engine/tree/TreeCommands.h>

#include "TestSupport.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
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

    /*  A DESCRIPTION AS A DEVICE SERVES IT: pretty-printed, no GODOT key, one
        root - WFS-DIY's shape. */
    const char* servedDescription = R"JSON({
  "FULL_PATH": "/wfs",
  "DESCRIPTION": "WFS-DIY",
  "CONTENTS": {
    "input": {"FULL_PATH": "/wfs/input", "CONTENTS": {
      "gain": {"FULL_PATH": "/wfs/input/gain", "TYPE": "f", "ACCESS": 3, "RANGE": [{"MIN": -60, "MAX": 12}], "DESCRIPTION": "a \"gain\""}
    }}
  }
})JSON";

    /*  The GODOT key of a file, parsed. */
    json::Object godotOf (const juce::File& file)
    {
        const auto parsed = json::parse (file.loadFileAsString().toStdString());
        REQUIRE (parsed.ok());
        const auto* godot = parsed.value->find ("GODOT");
        REQUIRE (godot != nullptr);
        REQUIRE (godot->isObject());
        return godot->asObject();
    }

    std::vector<std::string> stringsOf (const json::Object& fields, const char* key)
    {
        std::vector<std::string> out;
        const auto found = fields.find (key);
        REQUIRE (found != fields.end());

        for (const auto& each : found->second.asArray())
            out.push_back (each.asString());

        return out;
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

TEST_CASE ("discovery: a description saved as a preset gains its header, keeps the rest, and reads back as a preset")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    /*  Namespace draft §76, ARY: the device's description as it came, a GODOT
        header put in, the whole compact, written to your folder. */
    const auto root = freshFolder ("wfg-presets-save");
    const auto bundle = root.getChildFile ("show");
    const auto yours = root.getChildFile ("yours");
    REQUIRE (bundle.getChildFile ("namespaces").createDirectory());
    const auto described = bundle.getChildFile ("namespaces/wfs.json");
    REQUIRE (described.replaceWithText (servedDescription));

    tree::PresetSave job;
    job.source = described.getFullPathName().toStdString();
    job.slug = "wfs-diy";
    job.deviceName = "WFS-DIY";
    job.host = "10.0.0.7";
    job.port = 8000;
    job.queryPort = 5005;
    job.readback = "oscquery";
    job.date = "2026-10-17";

    std::string savedAs;
    REQUIRE (tree::savePresetNow (job, yours.getFullPathName().toStdString(), savedAs).empty());
    CHECK (savedAs == "wfs-diy@1");

    const auto file = yours.getChildFile ("wfs-diy.json");
    REQUIRE (file.existsAsFile());

    const auto fields = godotOf (file);
    CHECK (fields.at ("PRESET").asString() == "wfs-diy");
    CHECK (fields.at ("VERSION").asInt() == 1);
    CHECK (fields.at ("VENDOR").asString() == "WFS-DIY");
    CHECK (fields.at ("MODEL").asString() == "WFS-DIY (OSCQuery)");
    CHECK (fields.at ("TRANSPORT").asString() == "udp");
    CHECK (fields.at ("WIRE").asString() == "osc");
    CHECK (fields.at ("PORT").asInt() == 8000);
    CHECK (fields.at ("QUERY_PORT").asInt() == 5005);
    CHECK (fields.at ("READBACK").asString() == "oscquery");
    CHECK (fields.count ("FRAMING") == 0u);
    CHECK (stringsOf (fields, "SOURCES") == std::vector<std::string> { "captured from WFS-DIY at 10.0.0.7:5005 on 2026-10-17" });

    //  Compact, the rest of the description kept: its strings, escapes and all.
    const auto written = file.loadFileAsString().toStdString();
    CHECK (written.find ('\n') == std::string::npos);
    CHECK (written.find (R"JSON("DESCRIPTION":"a \"gain\"")JSON") != std::string::npos);
    CHECK (written.find (R"JSON("RANGE":[{"MIN":-60,"MAX":12}])JSON") != std::string::npos);

    //  It reads as a preset, whole and quickly, with the query port.
    const auto preset = tree::readPreset ("wfs-diy", written);
    INFO ("problem: " << preset.problem);
    REQUIRE (preset.usable());
    CHECK (preset.queryPort == 5005);
    CHECK (preset.roots == std::vector<std::string> { "/wfs" });
    CHECK (tree::readPresetQuickly ("wfs-diy", written).queryPort == 5005);

    //  Saved again over a version this machine holds: one more.
    job.knownVersion = 1;
    REQUIRE (tree::savePresetNow (job, yours.getFullPathName().toStdString(), savedAs).empty());
    CHECK (savedAs == "wfs-diy@2");
    CHECK (godotOf (file).at ("VERSION").asInt() == 2);

    //  A copy of a preset: its own header kept and added to, GENERATED gone, the name it was saved under.
    const auto copied = bundle.getChildFile ("namespaces/adm-osc-v1.json");
    REQUIRE (juce::File (juce::String (WFG_REPO_ROOT) + "/presets/devices/adm-osc.json").copyFileTo (copied));
    tree::PresetSave copy;
    copy.source = copied.getFullPathName().toStdString();
    copy.slug = "adm-osc";
    copy.deviceName = "Spat";
    copy.host = "10.0.0.9";
    copy.port = 9000;
    copy.fromPreset = "adm-osc@1";
    copy.knownVersion = 1;
    copy.date = "2026-10-17";
    REQUIRE (tree::savePresetNow (copy, yours.getFullPathName().toStdString(), savedAs).empty());
    CHECK (savedAs == "adm-osc@2");

    const auto copyFields = godotOf (yours.getChildFile ("adm-osc.json"));
    CHECK (copyFields.at ("VENDOR").asString() == "ADM-OSC");
    CHECK (copyFields.at ("PORT").asInt() == 9000);
    CHECK (copyFields.count ("GENERATED") == 0u);
    CHECK (copyFields.count ("QUERY_PORT") == 0u);
    const auto copySources = stringsOf (copyFields, "SOURCES");
    REQUIRE (copySources.size() >= 2u);
    CHECK (copySources.back() == "copied from adm-osc@1 on 2026-10-17");

    //  What cannot be read says so, and writes nothing.
    tree::PresetSave missing = job;
    missing.source = bundle.getChildFile ("namespaces/none.json").getFullPathName().toStdString();
    missing.slug = "none";
    CHECK (tree::savePresetNow (missing, yours.getFullPathName().toStdString(), savedAs).rfind ("could not read", 0) == 0);
    CHECK_FALSE (yours.getChildFile ("none.json").existsAsFile());

    root.deleteRecursively();
}

TEST_CASE ("discovery: preset.save decides the name and what the rows say, edits no show, and refuses what has nothing to save")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    Rig rig;
    const auto bundle = freshFolder ("wfg-bundle-save");
    REQUIRE (bundle.getChildFile ("namespaces").createDirectory());
    REQUIRE (bundle.getChildFile ("namespaces/wfs.json").replaceWithText (servedDescription));

    std::vector<tree::PresetSave> asked;
    tree::registerPresetCommands (rig.engine.commands(), rig.document, rig.mounts, bundle, nullptr,
                                  { {}, [&asked] (tree::PresetSave job) { asked.push_back (std::move (job)); } });

    const auto* command = rig.engine.commands().find ("preset.save");
    REQUIRE (command != nullptr);
    CHECK (command->machineLocal);
    CHECK_FALSE (command->mutates);

    const auto device = rig.document.createMount ("/wfs", "namespaces/wfs.json").id;
    const auto base = "/godot/mount/" + device + "/";
    REQUIRE (rig.document.setAttribute (base + "name", "WFS-DIY").ok);
    REQUIRE (rig.document.setAttribute (base + "host", "10.0.0.7").ok);
    REQUIRE (rig.document.setAttribute (base + "port", "8000").ok);
    REQUIRE (rig.document.setAttribute (base + "queryPort", "5005").ok);

    const auto revision = rig.document.showRevision();

    SUBCASE ("the name from the device's, the facts from its rows")
    {
        REQUIRE (rig.apply ("window", "preset.save", { text (device) }).applied == 1u);
        REQUIRE (asked.size() == 1u);
        CHECK (asked[0].slug == "wfs-diy");
        CHECK (asked[0].deviceName == "WFS-DIY");
        CHECK (asked[0].host == "10.0.0.7");
        CHECK (asked[0].port == 8000);
        CHECK (asked[0].queryPort == 5005);
        CHECK (asked[0].transport == "udp");
        CHECK (asked[0].source == bundle.getChildFile ("namespaces/wfs.json").getFullPathName().toStdString());
        CHECK (asked[0].date.size() == 10u);
        CHECK (asked[0].knownVersion == 0);

        //  The record says the name decided; the show is as it was.
        const auto record = rig.lastRecord();
        REQUIRE (record.args.size() == 4u);
        CHECK (record.args[1].getString() == "wfs-diy");
        CHECK (rig.document.showRevision() == revision);
    }

    SUBCASE ("a name given, a vendor and a model given")
    {
        REQUIRE (rig.apply ("window", "preset.save", { text (device), text ("my-wfs"), text ("Me"), text ("Rig") }).applied == 1u);
        REQUIRE (asked.size() == 1u);
        CHECK (asked[0].slug == "my-wfs");
        CHECK (asked[0].vendor == "Me");
        CHECK (asked[0].model == "Rig");
    }

    SUBCASE ("under the lock: saving a preset is no edit of the show")
    {
        REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
        CHECK (rig.apply ("window", "preset.save", { text (device) }).applied == 1u);
        CHECK (asked.size() == 1u);
    }

    SUBCASE ("refused: a name that is not one, a device with no description, a device nobody made, a serial device")
    {
        for (const auto* bad : { "My Desk", "../desk", "desk/x", "DESK" })
        {
            INFO ("name: " << bad);
            REQUIRE (rig.apply ("window", "preset.save", { text (device), text (bad) }).rejected == 1u);
            CHECK (rig.lastRecord().reason == reason::badValue);
        }

        const auto opaque = rig.document.createMount ("/desk", {}).id;
        REQUIRE (rig.apply ("window", "preset.save", { text (opaque) }).rejected == 1u);
        CHECK (rig.lastRecord().reason == reason::badValue);

        REQUIRE (rig.apply ("window", "preset.save", { text ("ZZZZZZZZ") }).rejected == 1u);
        CHECK (rig.lastRecord().reason == reason::unknownId);

        const auto gone = rig.document.createMount ("/gone", "namespaces/gone.json").id;
        REQUIRE (rig.apply ("window", "preset.save", { text (gone) }).rejected == 1u);
        CHECK (rig.lastRecord().reason == reason::badNamespace);

        REQUIRE (rig.document.setAttribute (base + "transport", "serial").ok);
        REQUIRE (rig.apply ("window", "preset.save", { text (device) }).rejected == 1u);
        CHECK (rig.lastRecord().reason == reason::badValue);

        CHECK (asked.empty());
    }

    CHECK (tree::presetSlugFrom ("WFS-DIY") == "wfs-diy");
    CHECK (tree::presetSlugFrom ("  Desk  A / B ") == "desk-a-b");
    CHECK (tree::presetSlugFrom ("/wfs") == "wfs");
    CHECK (tree::presetSlugFrom ("***").empty());
    CHECK (tree::isPresetSlug ("a-1"));
    CHECK_FALSE (tree::isPresetSlug (std::string (65, 'a')));

    bundle.deleteRecursively();
}

TEST_CASE ("discovery: a device saved as a preset is in the menu on the next reading, and made again from it is asked where it was")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    /*  The whole way, as serve wires it: preset.save hands the scanner the
        job, the scanner writes and reads your folder again, the table takes
        it, and a device made from the new preset has the query port the
        first one had. */
    const auto root = freshFolder ("wfg-presets-way");
    const auto bundle = root.getChildFile ("show");
    const auto yours = root.getChildFile ("yours");
    REQUIRE (bundle.getChildFile ("namespaces").createDirectory());
    REQUIRE (bundle.getChildFile ("namespaces/wfs.json").replaceWithText (servedDescription));

    tree::PresetTable table;
    table.scan (std::string (WFG_REPO_ROOT) + "/presets/devices", yours.getFullPathName().toStdString());
    CHECK (table.find ("wfs-diy") == nullptr);

    tree::PresetScanner scanner;
    scanner.setFolder (yours.getFullPathName().toStdString());
    REQUIRE (scanner.start());

    Rig rig;
    tree::registerPresetCommands (rig.engine.commands(), rig.document, rig.mounts, bundle, &table,
                                  { [&scanner] { scanner.rescan(); },
                                    [&scanner] (tree::PresetSave job) { scanner.save (std::move (job)); } });

    const auto device = rig.document.createMount ("/wfs", "namespaces/wfs.json").id;
    REQUIRE (rig.document.setAttribute ("/godot/mount/" + device + "/name", "WFS-DIY").ok);
    REQUIRE (rig.document.setAttribute ("/godot/mount/" + device + "/port", "8000").ok);
    REQUIRE (rig.document.setAttribute ("/godot/mount/" + device + "/queryPort", "5005").ok);

    const auto statusBefore = scanner.statusRevision();
    REQUIRE (rig.apply ("window", "preset.save", { text (device) }).applied == 1u);

    auto read = takeWithin (scanner);
    REQUIRE (read.has_value());
    CHECK (scanner.statusRevision() > statusBefore);
    CHECK (scanner.status().problem.empty());
    CHECK (scanner.status().saved == "wfs-diy@1");

    table.adoptYours (std::move (*read), scanner.folder());
    const auto* saved = table.find ("wfs-diy");
    REQUIRE (saved != nullptr);
    CHECK (saved->origin == "yours");
    CHECK (saved->usable());
    CHECK (saved->queryPort == 5005);

    //  Made again from it, in another show: the port and the query port the first one had.
    REQUIRE (rig.apply ("window", "mount.createFromPreset", { text ("wfs-diy") }).applied == 1u);
    const auto record = rig.lastRecord();
    REQUIRE (record.args.size() == 11u);
    CHECK (record.args[10].getInt32() == 5005);
    const auto again = record.args[1].getString();
    CHECK (rig.attribute ("/godot/mount/" + again + "/queryPort") == "5005");
    CHECK (rig.attribute ("/godot/mount/" + again + "/port") == "8000");
    CHECK (rig.attribute ("/godot/mount/" + again + "/preset") == "wfs-diy@1");
    CHECK (rig.mounts.isLoaded (again));

    //  A save that fails says why, and the reading after it still comes.
    REQUIRE (bundle.getChildFile ("namespaces/wfs.json").replaceWithText ("[1, 2]"));
    REQUIRE (rig.apply ("window", "preset.save", { text (device), text ("broken") }).applied == 1u);
    REQUIRE (takeWithin (scanner).has_value());
    CHECK_FALSE (scanner.status().problem.empty());
    CHECK (scanner.status().saved == "wfs-diy@1");
    CHECK_FALSE (yours.getChildFile ("broken.json").existsAsFile());

    scanner.stop();
    root.deleteRecursively();
}

TEST_CASE ("discovery: network.browse asks the hook, edits no show, and is this machine's alone")
{
    Rig rig;
    int asked = 0;
    tree::registerDiscoveryCommands (rig.engine.commands(), rig.document, rig.mounts, freshFolder ("wfg-bundle"),
                                     { [&asked] { ++asked; }, {}, {} });

    const auto* command = rig.engine.commands().find ("network.browse");
    REQUIRE (command != nullptr);
    CHECK (command->machineLocal);
    CHECK_FALSE (command->mutates);

    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
    REQUIRE (rig.apply ("window", "network.browse", {}).applied == 1u);
    CHECK (asked == 1);

    Rig replay;
    tree::registerDiscoveryCommands (replay.engine.commands(), replay.document, replay.mounts, freshFolder ("wfg-bundle"));
    CHECK (replay.apply ("window", "network.browse", {}).applied == 1u);
}

//==============================================================================
//  A DEVICE FROM AN ADDRESS (namespace draft §76, UD.8, ARX).
namespace
{
    /*  An OSCQuery server of the test's own: a listener on 127.0.0.1 that
        answers each GET with the page named by its target, or 404. Written
        from the HTTP it has to speak, sharing nothing with Go.dot's server. */
    struct CannedHttp
    {
        explicit CannedHttp (std::map<std::string, std::string> pagesToServe) : pages (std::move (pagesToServe))
        {
            REQUIRE (listener.createListener (0, "127.0.0.1"));
            thread = std::thread ([this] { run(); });
        }

        ~CannedHttp()
        {
            stopping.store (true);
            listener.close();
            thread.join();
        }

        int port() const { return listener.getBoundPort(); }

        void run()
        {
            while (! stopping.load())
            {
                std::unique_ptr<juce::StreamingSocket> client (listener.waitForNextConnection());

                if (client == nullptr)
                    return;

                std::string request;

                while (request.find ("\r\n\r\n") == std::string::npos && client->waitUntilReady (true, 1000) == 1)
                {
                    char buffer[1024];
                    const auto got = client->read (buffer, static_cast<int> (sizeof (buffer)), false);

                    if (got <= 0)
                        break;

                    request.append (buffer, static_cast<std::size_t> (got));
                }

                const auto line = request.substr (0, request.find ("\r\n"));
                const auto first = line.find (' ');
                const auto second = line.find (' ', first + 1);
                const auto target = first == std::string::npos || second == std::string::npos
                                      ? std::string {} : line.substr (first + 1, second - first - 1);

                const auto page = pages.find (target);
                const auto response = page == pages.end()
                    ? std::string ("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
                    : "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                        + std::to_string (page->second.size()) + "\r\nConnection: close\r\n\r\n" + page->second;

                client->write (response.data(), static_cast<int> (response.size()));
                client->close();
            }
        }

        std::map<std::string, std::string> pages;
        juce::StreamingSocket listener;
        std::atomic<bool> stopping { false };
        std::thread thread;
    };

    /*  A processor with two roots, as GET / answers it, and its HOST_INFO. */
    const char* twoRoots = R"JSON({"FULL_PATH": "/", "CONTENTS": {
        "wfs": {"FULL_PATH": "/wfs", "CONTENTS": {"gain": {"FULL_PATH": "/wfs/gain", "TYPE": "f", "ACCESS": 3}}},
        "console": {"FULL_PATH": "/console", "CONTENTS": {"ping": {"FULL_PATH": "/console/ping", "TYPE": "N", "ACCESS": 2}}}
    }})JSON";

    const char* hostInfo = R"JSON({"NAME": "WFS-DIY", "OSC_PORT": 8000, "OSC_TRANSPORT": "UDP",
                                   "EXTENSIONS": {"VALUE": true, "LISTEN": true}})JSON";

    /*  What `mount.discovered` carries for a fetch that worked, as the
        fetcher's thread submits it. */
    std::vector<osc::Value> discoveredArgs (const std::string& id, const std::string& host, const std::string& roots,
                                            const std::string& transport = "udp", const std::string& problem = {})
    {
        return { text (id), text (tree::MountFetcher::fileFor (id)), text (host), text (transport),
                 osc::Value::int32 (8000), osc::Value::int32 (5005), text ("oscquery"), text ("WFS-DIY"),
                 text (roots), osc::Value::int32 (2), text (problem) };
    }
}

TEST_CASE ("discovery: a description at an address is fetched whole, with HOST_INFO, its roots its own")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    const auto bundle = freshFolder ("wfg-bundle-discover");
    REQUIRE (bundle.createDirectory());

    SUBCASE ("two roots, and what HOST_INFO says")
    {
        CannedHttp server ({ { "/", twoRoots }, { "/?HOST_INFO", hostInfo } });
        const auto learned = tree::MountFetcher::discoverNow ({ "DSCV0001", "127.0.0.1", server.port(), "", bundle }, 3000);

        INFO ("problem: " << learned.problem);
        REQUIRE (learned.problem.empty());
        CHECK (learned.roots == "/console /wfs");
        CHECK (learned.name == "WFS-DIY");
        CHECK (learned.oscPort == 8000);
        CHECK (learned.transport == "udp");
        CHECK (learned.readback == "oscquery");
        CHECK (learned.nodeCount == 4u);     // the two roots and a node under each, as a declared processor counts
        CHECK (bundle.getChildFile (tree::MountFetcher::fileFor ("DSCV0001")).existsAsFile());
    }

    SUBCASE ("no HOST_INFO: no port, a name of its address, nothing to ask - and a name given wins")
    {
        CannedHttp server ({ { "/", twoRoots } });
        const auto plain = tree::MountFetcher::discoverNow ({ "DSCV0002", "127.0.0.1", server.port(), "", bundle }, 3000);
        REQUIRE (plain.problem.empty());
        CHECK (plain.oscPort == 0);
        CHECK (plain.name == "OSCQuery at 127.0.0.1");
        CHECK (plain.readback == "none");

        const auto named = tree::MountFetcher::discoverNow ({ "DSCV0003", "127.0.0.1", server.port(), "Desk", bundle }, 3000);
        CHECK (named.name == "Desk");
    }

    SUBCASE ("another Go.dot, a root no device may have, a page that is not a description, nothing listening")
    {
        CannedHttp goDot ({ { "/", R"JSON({"FULL_PATH": "/", "CONTENTS": {"godot": {"FULL_PATH": "/godot", "CONTENTS": {
                                         "x": {"FULL_PATH": "/godot/x", "TYPE": "f", "ACCESS": 3}}}}})JSON" } });
        CHECK (tree::MountFetcher::discoverNow ({ "DSCV0004", "127.0.0.1", goDot.port(), "", bundle }, 3000).problem
                 .find ("another Go.dot") != std::string::npos);

        CannedHttp media ({ { "/", R"JSON({"FULL_PATH": "/", "CONTENTS": {"media": {"FULL_PATH": "/media", "CONTENTS": {
                                         "x": {"FULL_PATH": "/media/x", "TYPE": "f", "ACCESS": 3}}}}})JSON" } });
        CHECK (tree::MountFetcher::discoverNow ({ "DSCV0005", "127.0.0.1", media.port(), "", bundle }, 3000).problem
                 .find ("no device may have") != std::string::npos);

        CannedHttp junk ({ { "/", "[1, 2]" } });
        CHECK_FALSE (tree::MountFetcher::discoverNow ({ "DSCV0006", "127.0.0.1", junk.port(), "", bundle }, 3000).problem.empty());
        CHECK_FALSE (bundle.getChildFile (tree::MountFetcher::fileFor ("DSCV0006")).exists());

        CHECK (tree::MountFetcher::discoverNow ({ "DSCV0007", "127.0.0.1", 1, "", bundle }, 1000).problem
                 .rfind ("nothing answered", 0) == 0);
    }

    bundle.deleteRecursively();
}

TEST_CASE ("discovery: mount.describeAt draws the identifier, hands the fetch on, and refuses what is not an address")
{
    Rig rig;
    const auto bundle = freshFolder ("wfg-bundle-describe");
    std::vector<std::vector<std::string>> asked;

    tree::registerDiscoveryCommands (rig.engine.commands(), rig.document, rig.mounts, bundle,
                                     { {},
                                       [&asked] (const std::string& id, const std::string& host, int queryPort, const std::string& name)
                                       { asked.push_back ({ id, host, std::to_string (queryPort), name }); },
                                       {} });

    REQUIRE (rig.apply ("window", "mount.describeAt", { text ("10.0.0.7"), osc::Value::int32 (5005), text ("WFS-DIY") }).applied == 1u);
    REQUIRE (asked.size() == 1u);
    CHECK (asked[0][0].size() == 8u);
    CHECK (asked[0][1] == "10.0.0.7");
    CHECK (asked[0][2] == "5005");
    CHECK (asked[0][3] == "WFS-DIY");

    //  The identifier drawn is on the record, which a replay applies asking nobody.
    const auto record = rig.lastRecord();
    REQUIRE (record.args.size() == 4u);
    CHECK (record.args[3].getString() == asked[0][0]);

    for (const auto* host : { "stage.local", "10.0.0", "10.0.0.256", "", "10.0.0.7.1" })
    {
        INFO ("host: " << host);
        REQUIRE (rig.apply ("window", "mount.describeAt", { text (host), osc::Value::int32 (5005) }).rejected == 1u);
        CHECK (rig.lastRecord().reason == reason::badValue);
    }

    REQUIRE (rig.apply ("window", "mount.describeAt", { text ("10.0.0.7"), osc::Value::int32 (0) }).rejected == 1u);

    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
    REQUIRE (rig.apply ("window", "mount.describeAt", { text ("10.0.0.7"), osc::Value::int32 (5005) }).rejected == 1u);
    CHECK (rig.lastRecord().reason == reason::locked);
    CHECK (asked.size() == 1u);

    CHECK (tree::isIpv4Address ("192.168.1.20"));
    CHECK (tree::isIpv4Address ("0.0.0.0"));
    CHECK_FALSE (tree::isIpv4Address ("1.2.3.4 "));
}

TEST_CASE ("discovery: mount.discovered makes the device at its roots, moves the one holding them all, and refuses a split")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    const auto bundle = freshFolder ("wfg-bundle-discovered");
    REQUIRE (bundle.getChildFile ("namespaces").createDirectory());
    REQUIRE (bundle.getChildFile (tree::MountFetcher::fileFor ("DSCV0010")).replaceWithText (twoRoots));
    REQUIRE (bundle.getChildFile (tree::MountFetcher::fileFor ("DSCV0011")).replaceWithText (twoRoots));

    Rig rig;
    std::vector<std::string> told;
    tree::registerDiscoveryCommands (rig.engine.commands(), rig.document, rig.mounts, bundle,
                                     { {}, {}, [&told] (const std::string& problem) { told.push_back (problem); } });

    //  Every root free: the device is made, at its roots, rx and tx on, loaded.
    REQUIRE (rig.apply ("mount:DSCV0010", "mount.discovered", discoveredArgs ("DSCV0010", "10.0.0.7", "/console /wfs")).applied == 1u);
    const auto base = std::string ("/godot/mount/DSCV0010/");
    CHECK (rig.attribute (base + "prefix") == "/console /wfs");
    CHECK (rig.attribute (base + "namespace") == tree::MountFetcher::fileFor ("DSCV0010"));
    CHECK (rig.attribute (base + "host") == "10.0.0.7");
    CHECK (rig.attribute (base + "port") == "8000");
    CHECK (rig.attribute (base + "queryPort") == "5005");
    CHECK (rig.attribute (base + "readback") == "oscquery");
    CHECK (rig.attribute (base + "name") == "WFS-DIY");
    CHECK (rig.attribute (base + "rx") == "true");
    CHECK (rig.mounts.isLoaded ("DSCV0010"));
    REQUIRE_FALSE (told.empty());
    CHECK (told.back().empty());

    auto record = rig.lastRecord();
    REQUIRE (record.args.size() == 12u);
    CHECK (record.args[11].getString() == "DSCV0010");
    const auto made = record.args;

    //  Found again elsewhere: the same device moves; the name the operator gave it stays.
    REQUIRE (rig.document.setAttribute (base + "name", "Stage left").ok);
    REQUIRE (rig.apply ("mount:DSCV0011", "mount.discovered", discoveredArgs ("DSCV0011", "10.0.0.9", "/console /wfs")).applied == 1u);
    CHECK (rig.attribute (base + "host") == "10.0.0.9");
    CHECK (rig.attribute (base + "namespace") == tree::MountFetcher::fileFor ("DSCV0011"));
    CHECK (rig.attribute (base + "name") == "Stage left");
    CHECK_FALSE (rig.document.findById ("DSCV0011").isValid());
    CHECK (rig.lastRecord().args[11].getString() == "DSCV0010");

    //  Roots split between two devices: refused, in a sentence.
    const auto other = rig.document.createMount ("/lisa", {}).id;
    REQUIRE (rig.document.setAttribute ("/godot/mount/DSCV0010/prefix", "/wfs").ok);
    REQUIRE (rig.document.setAttribute ("/godot/mount/" + other + "/prefix", "/console").ok);
    REQUIRE (rig.apply ("mount:DSCV0012", "mount.discovered", discoveredArgs ("DSCV0012", "10.0.0.7", "/console /wfs")).rejected == 1u);
    CHECK (rig.lastRecord().reason == reason::badValue);
    CHECK (told.back().find ("more than one device") != std::string::npos);

    //  A fetch that failed: nothing made, its sentence told.
    REQUIRE (rig.apply ("mount:DSCV0013", "mount.discovered",
                        discoveredArgs ("DSCV0013", "10.0.0.7", "", "udp", "nothing answered at http://10.0.0.7:5005")).applied == 1u);
    CHECK_FALSE (rig.document.findById ("DSCV0013").isValid());
    CHECK (told.back() == "nothing answered at http://10.0.0.7:5005");

    //  Over TCP, framed as OSC 1.1 is.
    REQUIRE (bundle.getChildFile (tree::MountFetcher::fileFor ("DSCV0014")).replaceWithText (
        R"JSON({"FULL_PATH": "/", "CONTENTS": {"eos": {"FULL_PATH": "/eos", "CONTENTS": {"go": {"FULL_PATH": "/eos/go", "TYPE": "N", "ACCESS": 2}}}}})JSON"));
    REQUIRE (rig.apply ("mount:DSCV0014", "mount.discovered", discoveredArgs ("DSCV0014", "10.0.0.8", "/eos", "tcp")).applied == 1u);
    CHECK (rig.attribute ("/godot/mount/DSCV0014/transport") == "tcp");
    CHECK (rig.attribute ("/godot/mount/DSCV0014/framing") == "slip");

    //  Locked: nothing made.
    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
    REQUIRE (bundle.getChildFile (tree::MountFetcher::fileFor ("DSCV0015")).replaceWithText (
        R"JSON({"FULL_PATH": "/", "CONTENTS": {"spat": {"FULL_PATH": "/spat", "CONTENTS": {"go": {"FULL_PATH": "/spat/go", "TYPE": "N", "ACCESS": 2}}}}})JSON"));
    REQUIRE (rig.apply ("mount:DSCV0015", "mount.discovered", discoveredArgs ("DSCV0015", "10.0.0.8", "/spat")).rejected == 1u);
    CHECK (rig.lastRecord().reason == reason::locked);

    //  A REPLAY: the first record, in a fresh show over the same bundle, makes the same device with no network.
    Rig replay;
    tree::registerDiscoveryCommands (replay.engine.commands(), replay.document, replay.mounts, bundle);
    REQUIRE (replay.apply ("mount:DSCV0010", "mount.discovered", made).applied == 1u);
    CHECK (replay.attribute ("/godot/mount/DSCV0010/prefix") == "/console /wfs");
    CHECK (replay.attribute ("/godot/mount/DSCV0010/host") == "10.0.0.7");
    CHECK (replay.mounts.isLoaded ("DSCV0010"));

    bundle.deleteRecursively();
}
