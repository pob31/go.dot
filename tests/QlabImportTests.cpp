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
    The QLab import's last stage (namespace draft §46, QL.6): the plan written
    into a show through the document's checked writes - groups, sounds with
    their files, ranges and routes, buses on the interface, devices, OSC cues
    and their curves, fades aimed at cues written AFTER them - identifiers
    drawn from QLab's UUIDs the same every time (ZY), and a sound found where
    QLab 4 says, from the folder above the workspace. A serialisation surface,
    so every case runs under fr_FR too.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "QlabFixture.h"

#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/import/QlabImport.h>

#include <juce_core/juce_core.h>

#include <map>
#include <memory>
#include <string>
#include <utility>

using namespace wfg;
using namespace wfg::import::qlab;
namespace fixture = wfg::test::qlab;
using fixture::Value;

namespace
{
    struct Built
    {
        Workspace workspace;
        Plan plan;
        doc::ShowDocument document;
        BuiltShow show;
        std::string soundId, fadeId, faderId;
    };

    /*  A scene whose fade comes BEFORE the sound it fades - so the build must
        fill targets in once every cue exists - a timeline, a #v# fade and a
        playlist of two messages; the sound given two channels. */
    std::unique_ptr<Built> buildScene()
    {
        fixture::Workspace w (5);
        const auto sound = w.audio ("Rain", "audio/rain.wav", { { 0, 0, 0.0 }, { 0, 1, 0.0 }, { 0, 2, -6.0 },
                                                                { 1, 0, 0.0 }, { 1, 1, 0.0 }, { 2, 0, 0.0 }, { 2, 2, 0.0 } },
                                    { { "infiniteLoop", Value::boolean (true) }, { "endTime", Value::number (12.5) } });
        const auto early = w.fade ("Rain down", sound, 2.0, true, { { 0, 0, -20.0 } });
        const auto rain = w.group ("Rain", 3, { sound });
        const auto fader = w.networkFade ("Fader", "/channel/114/fader #v#", "PATCH-S21", -18.0, 0.0, 5.0);
        const auto mute = w.network ("Mute", "/channel/118/mute \\T", "PATCH-S21");
        const auto place = w.network ("Place", "/wfs/input/21/positionXYZ -3.0 1.0 3.", "PATCH-WFS");
        const auto doors = w.group ("Doors", 6, { mute, place });
        const auto scene = w.group ("1 - Doors", 1, { early, rain, fader, doors });
        const auto list = w.group ("Main", 0, { scene });
        const auto bytes = w.bytes ({ list });

        auto built = std::make_unique<Built>();
        const auto read = readWorkspaceBytes (bytes.data(), bytes.size());
        REQUIRE (read.workspace.has_value());
        built->workspace = *read.workspace;
        built->soundId = w.idOf (sound);
        built->fadeId = w.idOf (early);
        built->faderId = w.idOf (fader);

        WalkOptions options;
        options.channels[built->soundId] = 2;
        built->plan = walk (built->workspace, options);

        const std::map<std::string, std::pair<std::string, int>> media { { built->soundId, { "rain.wav", 2 } } };
        built->show = build (built->workspace, built->plan, media, built->document);
        return built;
    }

    std::string at (const doc::ShowDocument& document, const std::string& address)
    {
        return document.getAttribute (address).value_or ("?");
    }

    std::string elementOf (const doc::ShowDocument& document, const std::string& id)
    {
        return document.findById (id).getType().toString().toStdString();
    }
}

TEST_CASE ("qlab import: identifiers drawn from QLab's UUIDs, the same show twice, byte for byte (ZY)")
{
    CHECK (doc::Id::isValid (idFor ("cue:ABCD-1234")));
    CHECK (idFor ("cue:ABCD-1234") == idFor ("cue:ABCD-1234"));
    CHECK (idFor ("cue:ABCD-1234") != idFor ("cue:ABCD-1235"));
    CHECK (idFor ("cue:ABCD-1234") != import::idFor ("wfg-als:", "cue:ABCD-1234"));

    const auto first = buildScene();
    const auto second = buildScene();
    CHECK (doc::CanonicalXml::write (first->document) == doc::CanonicalXml::write (second->document));
    CHECK (first->document.validate().empty());
}

TEST_CASE ("qlab import: the plan written - groups, a sound and its routes, a device, a curve, a target written later")
{
    const auto built = buildScene();
    const auto& document = built->document;

    CHECK (built->show.lists == 1);

    const auto sound = idFor ("cue:" + built->soundId);
    CHECK (elementOf (document, sound) == "Media");
    CHECK (at (document, "/godot/cue/" + sound + "/file") == "rain.wav");
    CHECK (at (document, "/godot/cue/" + sound + "/channels") == "2");

    //  The fade sits before its sound in the list, and still finds it.
    const auto fade = idFor ("cue:" + built->fadeId);
    CHECK (elementOf (document, fade) == "Fade");
    CHECK (at (document, "/godot/cue/" + fade + "/target") == sound);
    CHECK (at (document, "/godot/cue/" + fade + "/level") == "-20");

    //  A range for ever, and two routes to two mono outputs on the interface.
    const auto media = document.findById (sound);
    int ranges = 0, routes = 0;

    for (const auto& child : media)
    {
        if (child.getType().toString() == "Range")
        {
            ++ranges;
            CHECK (child.getProperty ("loops").toString() == "0");
        }

        if (child.getType().toString() == "Route")
            ++routes;
    }

    CHECK (ranges == 1);
    CHECK (routes == 2);
    CHECK (at (document, "/godot/audio/outputPatch") == "0 1");
    CHECK (at (document, "/godot/bus/" + idFor ("out:PATCH-AUDIO:1") + "/name") == "FOH L");

    //  A device per patch, under the segments its cues send.
    const auto s21 = idFor ("patch:PATCH-S21");
    CHECK (elementOf (document, s21) == "Mount");
    CHECK (at (document, "/godot/mount/" + s21 + "/host") == "192.168.1.221");
    CHECK (at (document, "/godot/mount/" + s21 + "/port") == "8024");
    CHECK (at (document, "/godot/mount/" + s21 + "/prefix") == "/channel");

    //  The #v# fade a curve on the cue's own clock.
    const auto fader = idFor ("cue:" + built->faderId);
    CHECK (elementOf (document, fader) == "Osc");
    CHECK (at (document, "/godot/cue/" + fader + "/value") == "f:-18");
    CHECK (at (document, "/godot/cue/" + fader + "/duration") == "5");
    CHECK (at (document, "/godot/curve/" + idFor ("cue:" + built->faderId + ":curve:0") + "/points") == "0 -18 5 0");
}

TEST_CASE ("qlab import: a QLab 4 sound found from the folder above the workspace, or from its own")
{
    const auto room = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getNonexistentChildFile ("wfg-qlab-media", "", false);
    const auto workspaceFolder = room.getChildFile ("Show");
    REQUIRE (workspaceFolder.getChildFile ("audio").createDirectory());

    const auto sound = workspaceFolder.getChildFile ("audio").getChildFile ("rain.wav");
    REQUIRE (sound.replaceWithText ("not really a sound"));

    FileRef fromAbove;
    fromAbove.relativePath = "Show/audio/rain.wav";
    fromAbove.relativeToParent = true;
    CHECK (findMedia (fromAbove, workspaceFolder) == sound);

    //  The folder renamed since QLab saved it: the rest of the path, from here.
    FileRef renamed;
    renamed.relativePath = "Show v2/audio/rain.wav";
    renamed.relativeToParent = true;
    CHECK (findMedia (renamed, workspaceFolder) == sound);

    FileRef five;
    five.relativePath = "audio/rain.wav";
    CHECK (findMedia (five, workspaceFolder) == sound);

    FileRef byName;
    byName.absolutePath = "/Volumes/Gone/somewhere/rain.wav";
    CHECK (findMedia (byName, workspaceFolder) == sound);

    room.deleteRecursively();
}

TEST_CASE ("qlab import: cue list numbers as QLab shows them")
{
    const auto parsed = parseLists ("1-2, 4");
    REQUIRE (parsed.has_value());
    CHECK (*parsed == std::set<int> { 0, 1, 3 });

    CHECK_FALSE (parseLists ("").has_value());
    CHECK_FALSE (parseLists ("0").has_value());
    CHECK_FALSE (parseLists ("2-1").has_value());
    CHECK_FALSE (parseLists ("main").has_value());
}

TEST_CASE ("qlab import: the author's own workspaces build into shows that validate, when WFG_QLAB_CORPUS names a folder of them")
{
    /*  REAL SHOWS, NOT COMMITTED (ZQ). Every `.qlab4` and `.qlab5` under the
        folder - backups and AppleDouble files aside - is walked and built as
        the import builds it, without its media, and the document must
        validate; what each came to is printed. */
    const auto folder = juce::SystemStats::getEnvironmentVariable ("WFG_QLAB_CORPUS", {});

    if (folder.isEmpty())
        return;

    int workspaces = 0;

    for (const auto& entry : juce::RangedDirectoryIterator (juce::File (folder), true, "*.qlab4;*.qlab5"))
    {
        const auto file = entry.getFile();

        if (file.getFileName().startsWith ("._") || file.getFullPathName().contains ("backups"))
            continue;

        ++workspaces;
        INFO (file.getFullPathName().toStdString());

        const auto read = readWorkspace (file);
        REQUIRE (read.workspace.has_value());

        const auto plan = walk (*read.workspace);
        doc::ShowDocument document;
        const auto built = build (*read.workspace, plan, {}, document);
        const auto problems = document.validate();

        INFO ((problems.empty() ? std::string {} : problems.front()));
        CHECK (problems.empty());

        int approximated = 0, dropped = 0;

        for (const auto& note : plan.notes)
        {
            approximated += note.kind == import::Note::Kind::approximated ? 1 : 0;
            dropped += note.kind == import::Note::Kind::dropped ? 1 : 0;
        }

        MESSAGE (file.getFileName().toStdString() << ": " << plan.cues << " cues, " << built.cues << " written, "
                 << plan.placeholders << " memos in place, " << approximated << " approximated, " << dropped
                 << " not imported, " << plan.devices.size() << " devices");
    }

    CHECK (workspaces > 0);
}
