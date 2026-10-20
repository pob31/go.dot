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
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/import/QlabImport.h>

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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
    //  Its own name in this process: the C and fr_FR shards run this case side by side (2026-10-13).
    const auto room = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("wfg-qlab-media-" + juce::Uuid().toDashedString());
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

//==============================================================================
/*  QF.9: QLAB'S PICTURES AND WORDS WRITTEN (namespace draft §62): canvases
    and their outputs, a picture, a movie, a text and a fade; and a whole
    import from a folder, a still's own size and a movie's codec read. */

namespace
{
    std::vector<std::uint8_t> box (const std::string& type, const std::vector<std::uint8_t>& contents)
    {
        std::vector<std::uint8_t> out;
        const auto size = static_cast<std::uint32_t> (contents.size() + 8);
        for (const auto shift : { 24, 16, 8, 0 })
            out.push_back (static_cast<std::uint8_t> ((size >> shift) & 0xFF));
        out.insert (out.end(), type.begin(), type.end());
        out.insert (out.end(), contents.begin(), contents.end());
        return out;
    }

    std::vector<std::uint8_t> joined (std::initializer_list<std::vector<std::uint8_t>> parts)
    {
        std::vector<std::uint8_t> out;
        for (const auto& part : parts)
            out.insert (out.end(), part.begin(), part.end());
        return out;
    }

    /*  A MOVIE'S BOXES, AND NO FRAMES: a QuickTime file whose one video
        track says its size and its codec, after a box of frames to skip. */
    std::vector<std::uint8_t> movieBoxes (int width, int height, const std::string& codec)
    {
        std::vector<std::uint8_t> tkhd (84, 0);                 // version 0: the size at 76 and 80
        const auto put = [&tkhd] (std::size_t at, std::uint32_t v)
        {
            for (int k = 0; k < 4; ++k)
                tkhd[at + static_cast<std::size_t> (k)] = static_cast<std::uint8_t> ((v >> (24 - 8 * k)) & 0xFF);
        };
        put (76, static_cast<std::uint32_t> (width) << 16);
        put (80, static_cast<std::uint32_t> (height) << 16);

        std::vector<std::uint8_t> hdlr (24, 0);
        const std::string video = "vide";
        std::copy (video.begin(), video.end(), hdlr.begin() + 8);

        std::vector<std::uint8_t> entry (8, 0);
        entry[3] = 16;
        std::copy (codec.begin(), codec.end(), entry.begin() + 4);
        entry.resize (16, 0);
        std::vector<std::uint8_t> stsd { 0, 0, 0, 0, 0, 0, 0, 1 };
        stsd.insert (stsd.end(), entry.begin(), entry.end());

        const auto trak = box ("trak", joined ({ box ("tkhd", tkhd),
                                                  box ("mdia", joined ({ box ("hdlr", hdlr),
                                                                         box ("minf", box ("stbl", box ("stsd", stsd))) })) }));
        return joined ({ box ("ftyp", { 'q', 't', ' ', ' ', 0, 0, 0, 0 }),
                         box ("mdat", std::vector<std::uint8_t> (5000, 0x55)),
                         box ("moov", trak) });
    }
}

TEST_CASE ("qlab import: a stage written as a canvas with its outputs, no display chosen; a picture, a text and a fade on it (§62, ALF-ALJ)")
{
    fixture::Workspace w (5);
    w.stages = { { "VIEW-1", "Stage 1", 1920, 1080, { "Output 1", "Output 2" }, false } };
    const auto still = w.video ("Title card", "video/title.png", "VIEW-1", {},
                                { { "fullScreen", Value::boolean (false) }, { "opacity", Value::number (0.0) } });
    fixture::Workspace::Run run;
    run.length = 7;
    run.colour = w.colour (1.0, 1.0, 1.0);
    const auto words = w.titles ("Act", w.attributed ("Act One", { run }), "VIEW-1");
    const auto up = w.pictureFade ("", still, 2.0, { { "doOpacity", Value::boolean (true) }, { "opacity", Value::number (1.0) } });
    const auto bytes = w.bytes ({ w.group ("Main", 0, { still, words, up }) });

    const auto read = readWorkspaceBytes (bytes.data(), bytes.size());
    REQUIRE (read.workspace.has_value());
    WalkOptions options;
    options.pictureSizes[w.idOf (still)] = { 960, 540 };
    const auto plan = walk (*read.workspace, options);

    doc::ShowDocument document;
    const std::map<std::string, std::pair<std::string, int>> media { { w.idOf (still), { "title.png", 0 } } };
    const auto built = build (*read.workspace, plan, media, document);

    const auto canvas = idFor ("stage:VIEW-1");
    CHECK (elementOf (document, canvas) == "Canvas");
    CHECK (at (document, "/godot/canvas/" + canvas + "/name") == "Stage 1");
    CHECK (at (document, "/godot/canvas/" + canvas + "/width") == "1920");
    CHECK (at (document, "/godot/canvas/" + canvas + "/height") == "1080");

    for (const auto* which : { "Output 1", "Output 2" })
    {
        const auto output = idFor (std::string ("output:") + which);
        CHECK (elementOf (document, output) == "VideoOutput");
        CHECK (at (document, "/godot/videoOutput/" + output + "/canvas") == canvas);
        CHECK (at (document, "/godot/videoOutput/" + output + "/name") == which);
        CHECK (document.getAttribute ("/godot/videoOutput/" + output + "/display").value_or ("") == "");
    }

    const auto picture = idFor ("cue:" + w.idOf (still));
    CHECK (elementOf (document, picture) == "Video");
    CHECK (at (document, "/godot/cue/" + picture + "/source") == "picture");
    CHECK (at (document, "/godot/cue/" + picture + "/canvas") == canvas);
    CHECK (at (document, "/godot/cue/" + picture + "/file") == "title.png");
    CHECK (at (document, "/godot/cue/" + picture + "/scale") == "50");
    CHECK (at (document, "/godot/cue/" + picture + "/opacity") == "0");

    const auto text = idFor ("cue:" + w.idOf (words));
    CHECK (at (document, "/godot/cue/" + text + "/source") == "text");
    CHECK (at (document, "/godot/cue/" + text + "/words") == "Act One");
    CHECK (at (document, "/godot/cue/" + text + "/canvas") == canvas);
    CHECK (at (document, "/godot/cue/" + text + "/paint") == "#FFFFFF");
    CHECK (document.getAttribute ("/godot/cue/" + text + "/file").value_or ("") == "");

    const auto fade = idFor ("cue:" + w.idOf (up));
    CHECK (elementOf (document, fade) == "Fade");
    CHECK (at (document, "/godot/cue/" + fade + "/target") == picture);
    CHECK (at (document, "/godot/cue/" + fade + "/video") == "opacity:100");
    CHECK (built.cues == 3);
}

TEST_CASE ("qlab import: a whole workspace from a folder - a still's own size read, a movie's codec read from its boxes, the canvases and fonts reported (§62, ALG, ALH, ALK)")
{
    juce::TemporaryFile scratch;
    const auto folder = scratch.getFile().getSiblingFile ("godot-qlab-pictures-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));
    REQUIRE (folder.getChildFile ("video").createDirectory());

    {
        juce::Image still (juce::Image::RGB, 960, 540, true);
        juce::FileOutputStream out (folder.getChildFile ("video/title.png"));
        REQUIRE (out.openedOk());
        juce::PNGImageFormat png;
        REQUIRE (png.writeImageToStream (still, out));
    }
    {
        const auto movie = movieBoxes (1280, 720, "avc1");
        REQUIRE (folder.getChildFile ("video/clip.mov").replaceWithData (movie.data(), movie.size()));
    }

    fixture::Workspace w (5);
    w.stages = { { "VIEW-1", "Stage 1", 1920, 1080, { "Output 1" }, false } };
    const auto still = w.video ("Title card", "video/title.png", "VIEW-1", {}, { { "fullScreen", Value::boolean (false) } });
    const auto clip = w.video ("Clip", "video/clip.mov", "VIEW-1", { { 0, 0, 0.0 } },
                               { { "fullScreen", Value::boolean (false) }, { "scaleX", Value::number (0.5) },
                                 { "scaleY", Value::number (0.5) } });
    fixture::Workspace::Run run;
    run.length = 3;
    run.font = "NoSuchFace-Heavy";
    const auto words = w.titles ("Words", w.attributed ("End", { run }), "VIEW-1");
    const auto bytes = w.bytes ({ w.group ("Main", 0, { still, clip, words }) });
    const auto workspaceFile = folder.getChildFile ("Show.qlab5");
    REQUIRE (workspaceFile.replaceWithData (bytes.data(), bytes.size()));

    ImportOptions options;
    options.into = folder.getChildFile ("Imported");
    const auto outcome = importWorkspace (workspaceFile, options);
    REQUIRE_MESSAGE (outcome.ok, outcome.error);

    doc::ShowDocument document;
    REQUIRE (doc::Bundle::open (options.into, document).ok);
    const auto picture = idFor ("cue:" + w.idOf (still));
    CHECK (at (document, "/godot/cue/" + picture + "/scale") == "50");              // 960 of 1920: half what fits
    const auto movie = idFor ("cue:" + w.idOf (clip));
    CHECK (at (document, "/godot/cue/" + movie + "/source") == "movie");
    CHECK (at (document, "/godot/cue/" + movie + "/scale") == "33.3333");           // 1280 of 1920, halved in QLab
    CHECK (options.into.getChildFile ("media/clip.mov").existsAsFile());

    const auto report = outcome.report.loadFileAsString().toStdString();
    CHECK (report.find ("## The canvases") != std::string::npos);
    CHECK (report.find ("| Stage 1 | 1920 x 1080 | Output 1 |") != std::string::npos);
    CHECK (report.find ("## The fonts") != std::string::npos);
    //  Missing here: its family read from the name QLab last saw it under.
    CHECK (report.find ("| NoSuchFace-Heavy | Helvetica Neue | missing |") != std::string::npos);
    CHECK (report.find ("a movie in avc1, which Go.dot shows as a preview until it is converted to HAP") != std::string::npos);

    folder.deleteRecursively();
}

//==============================================================================
TEST_CASE ("qlab import: a file not where the workspace says is found in a folder pointed at, by name, the nearest path winning (§100, AXX)")
{
    const auto room = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("wfg-qlab-search-" + juce::Uuid().toDashedString());
    const auto workspaceFolder = room.getChildFile ("Show");
    const auto elsewhere = room.getChildFile ("Elsewhere");

    REQUIRE (workspaceFolder.createDirectory());

    const auto write = [] (const juce::File& file)
    {
        REQUIRE (file.getParentDirectory().createDirectory());
        REQUIRE (file.replaceWithText (file.getFullPathName()));
        return file;
    };

    const auto bank = write (elsewhere.getChildFile ("Soundbank/rain.wav"));
    write (elsewhere.getChildFile ("Other/rain.wav"));
    write (elsewhere.getChildFile ("Backup/rain.wav"));
    const auto accented = write (elsewhere.getChildFile ("old/ENTRADA PUBLICO.WAV"));

    std::vector<wfg::import::FolderIndex> folders;
    folders.emplace_back (elsewhere);
    CHECK (folders.front().files() == 3u);         // the Backup folder is never a sound's

    //  QLab remembered it in a folder called Soundbank: that copy, not the other.
    FileRef rain;
    rain.relativePath = "audio/Soundbank/rain.wav";
    CHECK (findMedia (rain, workspaceFolder) == juce::File());
    CHECK (findMedia (rain, workspaceFolder, folders) == bank);

    //  Accents folded and case ignored, as a Mac's name and this disk's differ.
    FileRef entrada;
    entrada.absolutePath = "/Volumes/Show/Entrada pu\xcc\x81" "blico.wav";
    CHECK (findMedia (entrada, workspaceFolder, folders) == accented);

    //  Not anywhere: not found, whatever was pointed at.
    FileRef gone;
    gone.relativePath = "audio/gone.wav";
    CHECK (findMedia (gone, workspaceFolder, folders) == juce::File());

    room.deleteRecursively();
}

TEST_CASE ("qlab import: a workspace's files counted and looked for, then imported with the folder pointed at (§100, AXX)")
{
    const auto room = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("wfg-qlab-found-" + juce::Uuid().toDashedString());
    const auto workspaceFolder = room.getChildFile ("Show");
    REQUIRE (workspaceFolder.getChildFile ("audio").createDirectory());
    REQUIRE (room.getChildFile ("Soundbank").createDirectory());

    //  Found and copied by name: what is in them is not read here.
    const auto silence = [] (const juce::File& file) { REQUIRE (file.replaceWithText ("a sound")); };

    silence (workspaceFolder.getChildFile ("audio/here.wav"));
    silence (room.getChildFile ("Soundbank/there.wav"));

    fixture::Workspace w (5);
    const auto here = w.audio ("Here", "audio/here.wav", { { 0, 0, 0.0 }, { 0, 1, 0.0 } });
    const auto there = w.audio ("There", "audio/there.wav", { { 0, 0, 0.0 }, { 0, 1, 0.0 } });
    const auto gone = w.audio ("Gone", "audio/gone.wav", { { 0, 0, 0.0 }, { 0, 1, 0.0 } });
    const auto bytes = w.bytes ({ w.group ("Main", 0, { here, there, gone }) });
    const auto workspaceFile = workspaceFolder.getChildFile ("Show.qlab5");
    REQUIRE (workspaceFile.replaceWithData (bytes.data(), bytes.size()));

    const auto read = readWorkspace (workspaceFile);
    REQUIRE (read.workspace.has_value());

    const auto missingOf = [] (const std::vector<MediaFound>& found)
    {
        std::vector<std::string> out;
        for (const auto& each : found)
            if (each.found == juce::File())
                out.push_back (each.name);
        return out;
    };

    CHECK (missingOf (scanMedia (*read.workspace, workspaceFile, {}, {}))
             == std::vector<std::string> { "audio/there.wav", "audio/gone.wav" });

    std::vector<wfg::import::FolderIndex> folders;
    folders.emplace_back (room.getChildFile ("Soundbank"));
    CHECK (missingOf (scanMedia (*read.workspace, workspaceFile, {}, folders)) == std::vector<std::string> { "audio/gone.wav" });

    ImportOptions options;
    options.into = room.getChildFile ("Imported");
    options.searchFolders = { room.getChildFile ("Soundbank") };
    const auto outcome = importWorkspace (workspaceFile, options);
    REQUIRE_MESSAGE (outcome.ok, outcome.error);

    CHECK (outcome.missingMedia == std::vector<std::string> { "audio/gone.wav" });
    CHECK (options.into.getChildFile ("media/there.wav").existsAsFile());

    room.deleteRecursively();
}

TEST_CASE ("qlab import: MIDI cues on their port, triggers, devices under their own prefix, a camera's input, a cart - written (§100)")
{
    fixture::Workspace w (5);
    w.remotePatches = true;

    const auto fader = w.cue ("MIDICue", "Elsa", { { "messageType", Value::integer (1) }, { "status", Value::integer (3) },
                                                    { "channel", Value::integer (1) }, { "byte1", Value::integer (14) },
                                                    { "byte2", Value::integer (0) }, { "doFade", Value::boolean (true) },
                                                    { "endValue", Value::integer (102) }, { "duration", Value::number (0.9) },
                                                    { "midiPatchID", Value::string ("PATCH-MIDI") } });
    const auto key = w.cues.object ("F53HotKey", { { "QLActionKeyKeyCharacter", Value::integer ('e') },
                                                   { "QLActionKeyControl", Value::boolean (true) } });
    w.set (fader, "useHotKey", Value::boolean (true));
    w.set (fader, "hotKey", Value::uid (key));

    const auto go1 = w.network ("Go 1", "/go", "PATCH-MAC1");
    const auto go2 = w.network ("Go 2", "/go", "PATCH-MAC2");
    const auto eos = w.network ("Lights", "/eos/cue/11/fire", "PATCH-EOS");
    const auto camera = w.cue ("CameraCue", "Cam", { { "cameraPatchID", Value::string ("PATCH-CAM") } });
    const auto pad = w.memo ("Pad");

    const auto bytes = w.bytes ({ w.group ("Main", 0, { fader, go1, go2, eos, camera }), w.group ("Pads", 5, { pad }) });
    const auto read = readWorkspaceBytes (bytes.data(), bytes.size());
    REQUIRE (read.workspace.has_value());

    const auto plan = walk (*read.workspace);
    doc::ShowDocument document;
    const auto built = build (*read.workspace, plan, {}, document);

    for (const auto& note : built.notes)
        CHECK_MESSAGE (note.text.find ("could not") == std::string::npos, note.text);

    //  The MIDI cue on the port made for its patch, its fade and its key.
    const auto elsa = idFor ("cue:" + w.idOf (fader));
    CHECK (elementOf (document, elsa) == "Midi");
    const auto port = idFor ("midi:PATCH-MIDI");
    CHECK (at (document, "/godot/cue/" + elsa + "/port") == port);
    CHECK (at (document, "/godot/port/" + port + "/name") == "Desk MIDI");
    CHECK (at (document, "/godot/cue/" + elsa + "/fadeTo") == "102");
    CHECK (at (document, "/godot/cue/" + elsa + "/fadeTime") == "0.9");
    CHECK (at (document, "/godot/trigger/" + idFor ("cue:" + w.idOf (fader) + ":trigger:0") + "/key") == "Mod+E");

    //  Each QLab machine its own device, its prefix taken off as it sends.
    const auto mac1 = idFor ("patch:PATCH-MAC1");
    CHECK (at (document, "/godot/mount/" + mac1 + "/prefix") == "/Mac_mini_1");
    CHECK (at (document, "/godot/mount/" + mac1 + "/removePrefix") == "true");
    CHECK (at (document, "/godot/cue/" + idFor ("cue:" + w.idOf (go2)) + "/address") == "/Mac_mini_2/go");
    CHECK (at (document, "/godot/cue/" + idFor ("cue:" + w.idOf (eos)) + "/address") == "/eos/cue/11/fire");

    //  The camera a capture cue on a video input with none chosen.
    const auto cam = idFor ("cue:" + w.idOf (camera));
    CHECK (at (document, "/godot/cue/" + cam + "/source") == "capture");
    const auto input = idFor ("camera:PATCH-CAM");
    CHECK (at (document, "/godot/cue/" + cam + "/videoInput") == input);
    CHECK (at (document, "/godot/videoInput/" + input + "/name") == "Camera 1");

    //  The cart a cart, with no standby.
    const auto pads = idFor ("list:" + read.workspace->lists[1].id);
    CHECK (at (document, "/godot/list/" + pads + "/kind") == "cart");
}
