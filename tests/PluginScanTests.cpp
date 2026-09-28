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

/*  THE MACHINE'S KNOWN LIST (2026-09-26): read from Go.dot's own known.xml
    with no engine and no plugin, imported once from Tracktion's Settings.xml
    on a machine that scanned before the file existed, the format word the
    show's own (`AU`, not JUCE's `AudioUnit`), each plugin carrying the
    description a child makes it from, and the skipped files beside them; and
    the list serve holds, whose revision moves only when a reader could see
    a change.

    AND THE APP'S SCAN: its progress file round-trips; its table takes
    progress only while a scan runs; and its three commands refuse where a
    replay would refuse too - locked, a scan already running, a word that is
    not a format - because whether a scan runs is their own state.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/plugin/KnownList.h>
#include <wfg/engine/plugin/PluginCommands.h>
#include <wfg/engine/plugin/PluginScan.h>
#include <wfg/engine/plugin/PluginTable.h>
#include <wfg/engine/plugin/ScanTable.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

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
                     .getChildFile ("wfg-known-" + juce::String (juce::Random::getSystemRandom().nextInt64()));
            root.createDirectory();
        }

        ~Folder() { root.deleteRecursively(); }

        std::string path() const { return root.getFullPathName().toStdString(); }

        juce::File root;
    };

    juce::PluginDescription describe (const char* name, const char* format, const char* file, int uid)
    {
        juce::PluginDescription out;
        out.name = name;
        out.descriptiveName = name;
        out.pluginFormatName = format;
        out.category = "Fx";
        out.manufacturerName = "Someone";
        out.version = "1.0";
        out.fileOrIdentifier = file;
        out.uniqueId = uid;
        out.deprecatedUid = uid;
        out.numInputChannels = 2;
        out.numOutputChannels = 2;
        return out;
    }

    /** A list of two, one of them an AU, and a file a scan gave up on. */
    std::unique_ptr<juce::XmlElement> listXml()
    {
        juce::KnownPluginList list;
        list.addType (describe ("Verb", "VST3", "C:/plugins/verb.vst3", 0x0badf00d));
        list.addType (describe ("Delay", "AudioUnit", "AudioUnit:Effects/aufx,dely,appl", 0x1234abcd));
        list.addToBlacklist ("C:/plugins/hangs.vst3");
        return list.createXml();
    }
}

//==============================================================================
TEST_CASE ("known list: the format word is the show's - AudioUnit reads AU, the rest as they are")
{
    CHECK (plugin::formatWordOf ("AudioUnit") == "AU");
    CHECK (plugin::formatWordOf ("VST3") == "VST3");
    CHECK (plugin::formatWordOf ("LV2") == "LV2");
    CHECK (plugin::formatWordOf ("") == "");
}

TEST_CASE ("known list: read from known.xml with no engine, sorted, with descriptions and the skipped files")
{
    Folder folder;
    const juce::File file { juce::String (plugin::knownListPath (folder.path())) };
    CHECK (file == folder.root.getChildFile ("plugins").getChildFile ("known.xml"));

    //  Nothing there yet: an empty list, which is not an error.
    CHECK (plugin::knownPlugins (folder.path()).empty());
    CHECK (plugin::skippedPlugins (folder.path()).empty());

    file.getParentDirectory().createDirectory();
    REQUIRE (listXml()->writeTo (file));

    const auto known = plugin::knownPlugins (folder.path());
    REQUIRE (known.size() == 2u);

    //  By name: Delay before Verb.
    CHECK (known[0].name == "Delay");
    CHECK (known[0].format == "AU");
    CHECK (known[0].path == "AudioUnit:Effects/aufx,dely,appl");
    CHECK (known[1].name == "Verb");
    CHECK (known[1].format == "VST3");
    CHECK (known[1].manufacturer == "Someone");

    /*  THE DESCRIPTION TRAVELS WITH EACH ENTRY, and it is the scan's own,
        with JUCE's word in it: what a child makes the plugin from. */
    for (const auto& plugin : known)
    {
        const auto xml = juce::parseXML (juce::String (plugin.description));
        REQUIRE (xml != nullptr);

        juce::PluginDescription back;
        REQUIRE (back.loadFromXml (*xml));
        CHECK (back.createIdentifierString().toStdString() == plugin.identifier);
    }

    CHECK (plugin::describePlugin (folder.path(), known[1].identifier) == known[1].description);
    CHECK (plugin::describePlugin (folder.path(), "VST3-nowhere").empty());
    CHECK (plugin::skippedPlugins (folder.path()) == std::vector<std::string> { "C:/plugins/hangs.vst3" });
}

TEST_CASE ("known list: a machine that scanned before known.xml imports Tracktion's list once, and known.xml wins")
{
    Folder folder;

    /*  WHERE PHASE 9a'S SCANS LEFT IT: Tracktion's properties file, whose
        VALUE element holds the list as its child. */
    juce::XmlElement properties ("PROPERTIES");
    auto* value = properties.createNewChildElement ("VALUE");
    value->setAttribute ("name", "knownPluginList64");
    value->addChildElement (listXml().release());
    REQUIRE (properties.writeTo (folder.root.getChildFile ("Settings.xml")));

    CHECK (plugin::knownPlugins (folder.path()).size() == 2u);
    CHECK (plugin::skippedPlugins (folder.path()).size() == 1u);

    //  And once Go.dot's own file exists, it is the list - Settings.xml is not read.
    juce::KnownPluginList one;
    one.addType (describe ("Amp", "LV2", "urn:someone:amp", 0x00000011));
    const juce::File own { juce::String (plugin::knownListPath (folder.path())) };
    own.getParentDirectory().createDirectory();
    REQUIRE (one.createXml()->writeTo (own));

    const auto known = plugin::knownPlugins (folder.path());
    REQUIRE (known.size() == 1u);
    CHECK (known[0].name == "Amp");
    CHECK (known[0].format == "LV2");
    CHECK (plugin::skippedPlugins (folder.path()).empty());
}

TEST_CASE ("scan: the progress file round-trips, and a torn or absent one is not read")
{
    plugin::ScanProgress progress;
    progress.state = "scanning";
    progress.format = "VST3";
    progress.file = "C:/Program Files/Common Files/VST3/Verb, big.vst3";
    progress.done = 12;
    progress.total = 140;
    progress.found = 38;
    progress.skipped = { "C:/plugins/hangs.vst3" };

    plugin::ScanProgress back;
    REQUIRE (plugin::ScanProgress::fromJson (progress.toJson(), back));
    CHECK (back.state == "scanning");
    CHECK (back.format == "VST3");
    CHECK (back.file == progress.file);
    CHECK (back.done == 12);
    CHECK (back.total == 140);
    CHECK (back.found == 38);
    CHECK (back.skipped == progress.skipped);

    CHECK_FALSE (plugin::ScanProgress::fromJson ("{\"state\": \"scann", back));
    CHECK_FALSE (plugin::ScanProgress::fromJson ("{}", back));

    Folder folder;
    const auto path = folder.root.getChildFile ("scan-progress.json");
    CHECK_FALSE (plugin::readScanProgress (path.getFullPathName().toStdString(), back));
    REQUIRE (path.replaceWithText (juce::String (progress.toJson())));
    REQUIRE (plugin::readScanProgress (path.getFullPathName().toStdString(), back));
    CHECK (back.done == 12);
}

TEST_CASE ("scan: the table takes progress only while a scan runs, and ends finished or failed")
{
    plugin::ScanTable table;
    CHECK (table.reading().state == "idle");
    CHECK_FALSE (table.scanning());

    //  Progress with no scan under way is the tail of one already over: dropped.
    table.progress ("C:/late.vst3", 3, 4, 5, 0);
    CHECK (table.reading().file.empty());

    table.begin ("lv2");
    CHECK (table.scanning());
    CHECK (table.reading().format == "lv2");

    const auto before = table.revision();
    table.progress ("/usr/lib/lv2/amp.lv2", 1, 2, 7, 0);
    CHECK (table.revision() > before);
    CHECK (table.reading().file == "/usr/lib/lv2/amp.lv2");
    CHECK (table.reading().done == 1);
    CHECK (table.reading().total == 2);

    table.end (8, 1, {});
    CHECK_FALSE (table.scanning());
    CHECK (table.reading().state == "finished");
    CHECK (table.reading().file.empty());
    CHECK (table.reading().found == 8);
    CHECK (table.reading().skipped == 1);

    table.begin ({});
    table.end (8, 0, "the scan ended on C:/crash.vst3");
    CHECK (table.reading().state == "failed");
    CHECK (table.reading().problem == "the scan ended on C:/crash.vst3");
}

TEST_CASE ("scan: plugin.scan begins one and reaches the hook, is refused locked, running or for a word that is no format; plugin.scanned ends it")
{
    CommandRegistry registry;
    plugin::PluginTable plugins;
    plugin::ScanTable scans;

    auto locked = false;
    struct Launched { std::string word, file, folder; };
    std::vector<Launched> launched;

    plugin::PluginCommandHooks hooks;
    hooks.locked = [&locked] { return locked; };
    hooks.scans = &scans;
    hooks.scan = [&launched] (const std::string& word, const std::string& file, const std::string& folder)
    {
        launched.push_back ({ word, file, folder });
    };
    plugin::registerPluginCommands (registry, plugins, hooks);

    CommandContext context;
    const std::string origin = "cli";
    context.origin = &origin;

    const auto dispatch = [&registry, &context] (const char* name, std::vector<osc::Value> args)
    {
        const auto* command = registry.find (name);
        return command != nullptr ? command->handler (context, args) : Outcome::rejected (reason::unknownCommand);
    };

    SUBCASE ("a locked show refuses both, and launches nothing")
    {
        locked = true;
        CHECK (dispatch ("plugin.scan", {}).reason == reason::locked);
        CHECK (dispatch ("plugin.scanRetry", { osc::Value::string ("C:/plugins/hangs.vst3") }).reason == reason::locked);
        CHECK (launched.empty());
        CHECK (scans.reading().state == "idle");
    }

    SUBCASE ("a word that is not a format is refused")
    {
        CHECK (dispatch ("plugin.scan", { osc::Value::string ("vst2") }).reason == reason::badValue);
        CHECK (dispatch ("plugin.scanRetry", { osc::Value::string ("") }).reason == reason::badValue);
        CHECK (launched.empty());
    }

    SUBCASE ("one scan at a time, until plugin.scanned")
    {
        CHECK (dispatch ("plugin.scan", { osc::Value::string ("lv2"), osc::Value::string ("D:/lv2") }).applied);
        REQUIRE (launched.size() == 1u);
        CHECK (launched[0].word == "lv2");
        CHECK (launched[0].folder == "D:/lv2");
        CHECK (scans.reading().state == "scanning");

        CHECK (dispatch ("plugin.scan", {}).reason == "scan-running");
        CHECK (dispatch ("plugin.scanRetry", { osc::Value::string ("C:/plugins/hangs.vst3") }).reason == "scan-running");
        CHECK (launched.size() == 1u);

        CHECK (dispatch ("plugin.scanned", { osc::Value::int32 (2), osc::Value::int32 (0),
                                             osc::Value::string ("") }).applied);
        CHECK (scans.reading().state == "finished");
        CHECK (scans.reading().found == 2);

        CHECK (dispatch ("plugin.scanRetry", { osc::Value::string ("C:/plugins/hangs.vst3") }).applied);
        REQUIRE (launched.size() == 2u);
        CHECK (launched[1].word.empty());
        CHECK (launched[1].file == "C:/plugins/hangs.vst3");
        CHECK (launched[1].folder.empty());
    }

    SUBCASE ("and a replay, with no hook and no table of its own, refuses in the same places")
    {
        CommandRegistry quiet;
        plugin::PluginCommandHooks replayHooks;
        replayHooks.locked = [&locked] { return locked; };
        plugin::registerPluginCommands (quiet, plugins, replayHooks);

        const auto replayed = [&quiet, &context] (const char* name, std::vector<osc::Value> args)
        {
            return quiet.find (name)->handler (context, args);
        };

        CHECK (replayed ("plugin.scan", {}).applied);
        CHECK (replayed ("plugin.scan", {}).reason == "scan-running");
        CHECK (replayed ("plugin.scanned", { osc::Value::int32 (0), osc::Value::int32 (0),
                                             osc::Value::string ("the scan was stopped before it was over") }).applied);
        CHECK (replayed ("plugin.scan", {}).applied);
        CHECK (launched.empty());
    }
}

TEST_CASE ("known list: serve's list moves its revision only on a change a reader could see, and describes by identifier")
{
    plugin::KnownList list;
    CHECK (list.revision() == 0u);
    CHECK (list.all().empty());

    const std::vector<plugin::KnownPlugin> two {
        { "Verb", "VST3-verb", "VST3", "Someone", "C:/plugins/verb.vst3", "<PLUGIN name=\"Verb\"/>" },
        { "Amp", "LV2-amp", "LV2", "Someone", "urn:someone:amp", "<PLUGIN name=\"Amp\"/>" } };

    CHECK (list.set (two, { "C:/plugins/hangs.vst3" }));
    CHECK (list.revision() == 1u);

    //  The same again is no change.
    CHECK_FALSE (list.set (two, { "C:/plugins/hangs.vst3" }));
    CHECK (list.revision() == 1u);

    //  A skipped file let go of is one.
    CHECK (list.set (two, {}));
    CHECK (list.revision() == 2u);
    CHECK (list.skippedFiles().empty());

    CHECK (list.describe ("LV2-amp") == "<PLUGIN name=\"Amp\"/>");
    CHECK (list.describe ("VST3-nowhere").empty());
    CHECK (list.all().size() == 2u);
}
