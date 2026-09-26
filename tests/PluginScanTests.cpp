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
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/plugin/KnownList.h>
#include <wfg/engine/plugin/PluginScan.h>

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
