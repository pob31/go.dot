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
    An Ableton Live set, read into facts (namespace draft §29.1): the first of
    the importer's stages, and the one that knows Live's spelling.

    THE FIXTURES ARE HAND-WRITTEN, in the shape of the author's real sets -
    `session.als.xml` a Live 11 session with a source track feeding a
    monitor-In "track in" track, `live10.als.xml` a Live 10 one with a group -
    the element names copied from the real files and the names in them made
    up (`tests/fixtures/README.md`). Plain XML, so a diff reads; one case
    gzips one to prove the reader takes the file as Live writes it. Numbers are
    read through the engine's own parser, so every case runs under fr_FR too.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/import/AlsReader.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace wfg;
using namespace wfg::import::als;

namespace
{
    juce::File fixture (const char* name)
    {
        return juce::File { juce::String (std::string (WFG_TEST_FIXTURES_DIR)) }
                   .getChildFile ("als").getChildFile (name);
    }

    LiveSet readFixture (const char* name)
    {
        const auto result = readSet (fixture (name));
        INFO ("error: " << result.error);
        REQUIRE (result.set.has_value());
        return *result.set;
    }

    const Track& trackNamed (const LiveSet& set, const std::string& name)
    {
        const auto found = std::find_if (set.tracks.begin(), set.tracks.end(),
                                         [&name] (const Track& track) { return track.name == name; });
        REQUIRE (found != set.tracks.end());
        return *found;
    }

    double db (double gain)
    {
        return 20.0 * std::log10 (gain);
    }
}

TEST_CASE ("als: a Live 11 set is read scene by scene, in Live's own words")
{
    const auto set = readFixture ("session.als.xml");

    CHECK (set.creator == "Ableton Live 11.2.6");
    CHECK (set.tempo == doctest::Approx (120.0));
    CHECK (set.problems.empty());

    /*  THE SCENES, in order, with the annotation word for word - the
        operator's text, accents, quotes and line breaks as Live kept them. */
    REQUIRE (set.scenes.size() == 3u);
    CHECK (set.scenes[0].id == "37");
    CHECK (set.scenes[0].name == "top start");
    CHECK (set.scenes[0].annotation == "MISE : Fader 1 \xc3\xa0 -INF\n\n\"Il \xc3\xa9tait une fois\" > Top");
    CHECK (set.scenes[0].colour == 13);
    CHECK (set.scenes[1].name == "fo");
    CHECK (set.scenes[2].name.empty());
    CHECK (set.scenes[2].tempoEnabled);
    CHECK (set.scenes[2].tempo == doctest::Approx (96.0));

    /*  THE TRACKS AND THE RETURNS apart, each in Live's order - the returns'
        order being every track's sends' order. */
    REQUIRE (set.tracks.size() == 3u);
    REQUIRE (set.returns.size() == 2u);
    CHECK (set.returns[0].name == "A-Face");
    CHECK (set.returns[1].name == "B-Loin");
    CHECK (set.returns[1].output.kind == Routing::Kind::external);
    CHECK (set.returns[1].output.stereo);
    CHECK (set.returns[1].output.channel == 1);
    CHECK (set.returns[1].volume == doctest::Approx (0.945881784));

    REQUIRE (set.master.has_value());
    CHECK (set.master->output.kind == Routing::Kind::external);
    CHECK_FALSE (set.master->output.stereo);
    CHECK (set.master->output.channel == 17);

    REQUIRE (set.sendsPre.size() == 2u);
    CHECK_FALSE (set.sendsPre[0]);
    CHECK (set.sendsPre[1]);

    CHECK (set.arrangementClips == 1);
}

TEST_CASE ("als: a track's routing, monitoring, mixer and devices")
{
    const auto set = readFixture ("session.als.xml");

    const auto& one = trackNamed (set, "1");
    CHECK (one.id == "14");
    CHECK (one.kind == Track::Kind::audio);
    CHECK (one.monitoring == Track::Monitoring::off);
    CHECK (one.output.kind == Routing::Kind::none);
    CHECK (db (one.volume) == doctest::Approx (-70.0).epsilon (0.001));
    CHECK (one.pan == doctest::Approx (-0.25));
    REQUIRE (one.sends.size() == 2u);
    CHECK (one.sends[0] == doctest::Approx (0.5));

    /*  A SOURCE FEEDING A "TRACK IN", and the "track in" listening to it with
        its monitoring In - the pattern whose clips are silent carriers. */
    const auto& four = trackNamed (set, "4");
    CHECK (four.output.kind == Routing::Kind::track);
    CHECK (four.output.trackId == "28");

    const auto& carrier = trackNamed (set, "4 track in");
    CHECK (carrier.monitoring == Track::Monitoring::in);
    CHECK (carrier.input.kind == Routing::Kind::track);
    CHECK (carrier.input.trackId == "27");

    /*  THE DEVICES, in chain order, every automatable number by its path. */
    REQUIRE (one.devices.size() == 2u);

    const auto& eq = one.devices[0];
    CHECK (eq.kind == "Eq8");
    CHECK (eq.on);
    REQUIRE (eq.bands.size() == 8u);
    CHECK (eq.bands[0].on);
    CHECK (eq.bands[0].mode == 2);
    CHECK (eq.bands[0].frequency == doctest::Approx (120.0));
    CHECK (eq.bands[0].gain == doctest::Approx (-3.5));
    CHECK_FALSE (eq.bands[3].on);
    CHECK (eq.bands[3].mode == 5);
    CHECK (eq.bands[3].frequency == doctest::Approx (6388.70752));
    CHECK_FALSE (eq.bands[5].on);   // a band the file does not mention is off

    const auto& reverb = one.devices[1];
    CHECK (reverb.kind == "Reverb");
    CHECK_FALSE (reverb.on);
    CHECK (reverb.values.at ("MixDirect") == doctest::Approx (0.75));
    CHECK (reverb.values.at ("On") == doctest::Approx (0.0));
}

TEST_CASE ("als: a clip's file, markers, warp and slot")
{
    const auto set = readFixture ("session.als.xml");
    const auto& one = trackNamed (set, "1");

    REQUIRE (one.slots.size() == 3u);
    REQUIRE (one.slots[0].clip.has_value());
    CHECK_FALSE (one.slots[1].clip.has_value());
    CHECK (one.slots[1].hasStop);
    CHECK_FALSE (one.slots[2].hasStop);

    const auto& clip = *one.slots[0].clip;
    CHECK (clip.name == "Rain \xc3\xa9t\xc3\xa9");
    CHECK (clip.colour == 13);
    CHECK (clip.file.relativePath == "Samples/Imported/rain \xc3\xa9t\xc3\xa9.wav");
    CHECK (clip.file.absolutePath == "/Users/someone/Show Project/Samples/Imported/rain \xc3\xa9t\xc3\xa9.wav");
    CHECK (clip.file.name == "rain \xc3\xa9t\xc3\xa9.wav");
    CHECK (clip.file.size == 2880044);
    CHECK (clip.file.crc == 54990);
    CHECK (clip.file.sampleRate == doctest::Approx (48000.0));
    CHECK (clip.file.frames == 480000);

    CHECK (clip.loopStart == doctest::Approx (-2.0));
    CHECK (clip.loopEnd == doctest::Approx (16.0));
    CHECK_FALSE (clip.loopOn);
    CHECK (db (clip.gain) == doctest::Approx (-6.0).epsilon (0.001));
    CHECK_FALSE (clip.followAction);

    /*  THE WARP MAP: two markers a thirty-second of a beat apart at 120 BPM is
        a sample at its own rate, offset by a quarter of a second - so a beat
        before beat nought is half a second before that quarter. */
    REQUIRE (clip.warp.size() == 2u);
    CHECK (beatsPerSecond (clip) == doctest::Approx (2.0));
    CHECK (warpIsStraight (clip));
    CHECK (sampleSecondsAt (clip, 0.0, set.tempo) == doctest::Approx (0.25));
    CHECK (sampleSecondsAt (clip, -2.0, set.tempo) == doctest::Approx (-0.75));
    CHECK (sampleSecondsAt (clip, 16.0, set.tempo) == doctest::Approx (8.25));

    /*  A STRETCHED, LOOPING CLIP: a map that bends - two beats a second for
        ten seconds, then one - read straight between its markers and carried
        on past them by the nearest segment. */
    const auto& wind = *trackNamed (set, "4").slots[0].clip;
    CHECK (wind.loopOn);
    CHECK (wind.startRelative == doctest::Approx (-8.0));
    CHECK (wind.warpMode == 4);
    CHECK_FALSE (warpIsStraight (wind));
    CHECK (sampleSecondsAt (wind, 10.0, set.tempo) == doctest::Approx (5.0));
    CHECK (sampleSecondsAt (wind, 25.0, set.tempo) == doctest::Approx (15.0));
    CHECK (sampleSecondsAt (wind, 40.0, set.tempo) == doctest::Approx (30.0));
}

TEST_CASE ("als: an envelope names the parameter it moves, and one that names nothing is kept as such")
{
    const auto set = readFixture ("session.als.xml");
    const auto& clip = *trackNamed (set, "1").slots[0].clip;

    REQUIRE (clip.envelopes.size() == 4u);

    /*  THE TRACK'S VOLUME, in the clip's beats and Live's linear gain - the
        first point the one Live writes long before the clip. */
    const auto& volume = clip.envelopes[0];
    CHECK (volume.target.what == Parameter::volume);
    CHECK (volume.target.trackId == "14");
    CHECK_FALSE (volume.target.relative);
    CHECK_FALSE (volume.switches);
    REQUIRE (volume.points.size() == 3u);
    CHECK (volume.points[0].beats < -1.0e6);
    CHECK (volume.points[2].beats == doctest::Approx (8.5));
    CHECK (volume.points[2].value == doctest::Approx (1.0));

    /*  THE CLIP'S VOLUME MODULATION, relative, from -1 to 1. */
    const auto& modulation = clip.envelopes[1];
    CHECK (modulation.target.what == Parameter::modulation);
    CHECK (modulation.target.relative);
    CHECK (modulation.points.back().value == doctest::Approx (-1.0));

    /*  A DEVICE SWITCH: the EQ's On, as ones and noughts. */
    const auto& on = clip.envelopes[2];
    CHECK (on.target.what == Parameter::device);
    CHECK (on.target.deviceIndex == 0);
    CHECK (on.target.deviceKind == "Eq8");
    CHECK (on.target.path == "On");
    CHECK (on.switches);
    CHECK (on.points.back().value == doctest::Approx (1.0));

    /*  AND ONE THAT POINTS AT NOTHING THE SET HAS: kept, unknown, with the
        identifier it named, for the report to say. */
    const auto& stray = clip.envelopes[3];
    CHECK (stray.target.what == Parameter::unknown);
    CHECK (stray.pointeeId == "999");

    /*  A CARRIER'S ENVELOPE moves its OWN track's volume: the "track in". */
    const auto& carried = *trackNamed (set, "4 track in").slots[1].clip;
    REQUIRE (carried.envelopes.size() == 1u);
    CHECK (carried.envelopes[0].target.what == Parameter::volume);
    CHECK (carried.envelopes[0].target.trackId == "28");
}

TEST_CASE ("als: the hands - which control moved what, on which channel")
{
    const auto set = readFixture ("session.als.xml");

    const auto mapped = [&set] (int number) -> const Mapping*
    {
        for (const auto& mapping : set.mappings)
            if (mapping.number == number)
                return &mapping;

        return nullptr;
    };

    /*  Live counts channels from nought; a desk, and the report, from one. */
    const auto* fader = mapped (120);
    REQUIRE (fader != nullptr);
    CHECK (fader->channel == 7);
    CHECK_FALSE (fader->note);
    CHECK (fader->target.what == Parameter::volume);
    CHECK (fader->target.trackId == "14");

    const auto* rotary = mapped (111);
    REQUIRE (rotary != nullptr);
    CHECK (rotary->target.what == Parameter::send);
    CHECK (rotary->target.sendIndex == 1);

    const auto* wet = mapped (117);
    REQUIRE (wet != nullptr);
    CHECK (wet->target.what == Parameter::device);
    CHECK (wet->target.deviceKind == "Reverb");
    CHECK (wet->target.path == "MixDirect");

    REQUIRE (set.fireScene.has_value());
    CHECK (set.fireScene->channel == 7);
    CHECK (set.fireScene->number == 126);
    REQUIRE (set.sceneUp.has_value());
    CHECK (set.sceneUp->number == 124);
    CHECK_FALSE (set.sceneDown.has_value());   // a key mapped to nothing is no mapping
}

TEST_CASE ("als: a Live 10 set - its scene names, its folder list and its group")
{
    const auto set = readFixture ("live10.als.xml");

    CHECK (set.creator == "Ableton Live 10.0.5");
    CHECK (set.tempo == doctest::Approx (100.0));

    REQUIRE (set.scenes.size() == 1u);
    CHECK (set.scenes[0].name == "Beginning");
    CHECK (set.scenes[0].colour == 3);

    const auto& group = trackNamed (set, "Voices");
    CHECK (group.kind == Track::Kind::group);
    CHECK (group.output.kind == Routing::Kind::master);

    const auto& voice = trackNamed (set, "Voice");
    CHECK (voice.groupId == "65");
    CHECK (voice.output.kind == Routing::Kind::group);

    const auto& clip = *voice.slots[0].clip;
    CHECK (clip.file.relativePath == "Samples/Imported/voice papa.wav");
    CHECK (clip.file.name == "voice papa.wav");
    CHECK (clip.file.absolutePath.empty());
    CHECK (clip.colour == 5);
    CHECK_FALSE (clip.warped);

    /*  A FOLLOW ACTION in Live 10 is an action other than none. */
    CHECK (clip.followAction);

    /*  Unwarped, two beats a second at 120 - but this set runs at 100, and
        the map is still the markers', which say two. */
    CHECK (beatsPerSecond (clip) == doctest::Approx (2.0));
}

TEST_CASE ("als: the file as Live writes it, gzipped, reads the same as the XML")
{
    juce::TemporaryFile temporary (".als");

    {
        juce::MemoryBlock xml;
        REQUIRE (fixture ("session.als.xml").loadFileAsData (xml));

        juce::FileOutputStream out (temporary.getFile());
        REQUIRE (out.openedOk());

        juce::GZIPCompressorOutputStream gzip (out, 9, juce::GZIPCompressorOutputStream::windowBitsGZIP);
        gzip.write (xml.getData(), xml.getSize());
        gzip.flush();
    }

    const auto gzipped = readSet (temporary.getFile());
    REQUIRE (gzipped.set.has_value());

    const auto plain = readFixture ("session.als.xml");
    CHECK (gzipped.set->scenes.size() == plain.scenes.size());
    CHECK (gzipped.set->tracks.size() == plain.tracks.size());
    CHECK (gzipped.set->scenes[0].annotation == plain.scenes[0].annotation);
}

TEST_CASE ("als: the author's own sets read whole, when WFG_ALS_CORPUS names a folder of them")
{
    /*  REAL SHOWS, NOT COMMITTED: their media is gigabytes and their notes are
        a production's. Where the variable names a folder, every `.als` under
        it - backups aside - must read with every scene, every slot on every
        track, and every envelope's target known; anything less is printed,
        set by set. Off CI, a run at the desk says the reader meets the files
        it was written for. */
    const auto folder = juce::SystemStats::getEnvironmentVariable ("WFG_ALS_CORPUS", {});

    if (folder.isEmpty())
        return;

    int sets = 0;

    for (const auto& entry : juce::RangedDirectoryIterator (juce::File (folder), true, "*.als"))
    {
        const auto file = entry.getFile();

        if (file.getFullPathName().contains ("Backup") || file.getFileName().startsWith ("._"))
            continue;

        ++sets;
        INFO (file.getFullPathName().toStdString());

        const auto result = readSet (file);
        REQUIRE (result.set.has_value());

        const auto& set = *result.set;
        CHECK_FALSE (set.scenes.empty());

        int envelopes = 0, unknown = 0;

        for (const auto& track : set.tracks)
        {
            if (track.kind == Track::Kind::group)
                continue;

            CHECK (track.slots.size() == set.scenes.size());

            for (const auto& slot : track.slots)
                if (slot.clip.has_value())
                    for (const auto& envelope : slot.clip->envelopes)
                    {
                        ++envelopes;
                        unknown += envelope.target.what == Parameter::unknown ? 1 : 0;
                    }
        }

        for (const auto& mapping : set.mappings)
            CHECK (mapping.target.what != Parameter::unknown);

        MESSAGE (file.getFileName().toStdString() << ": " << set.creator << ", " << set.scenes.size()
                 << " scenes, " << set.tracks.size() << " tracks, " << set.returns.size() << " returns, "
                 << envelopes << " envelopes (" << unknown << " unknown), " << set.mappings.size()
                 << " mappings, " << set.problems.size() << " problems");

        CHECK (unknown == 0);

        for (const auto& problem : set.problems)
            MESSAGE ("  " << problem);
    }

    CHECK (sets > 0);
}

TEST_CASE ("als: what is not a Live set says so")
{
    CHECK (readSetXml ("this is not xml").error.find ("not XML") != std::string::npos);
    CHECK (readSetXml ("<Show/>").error.find ("not an Ableton Live set") != std::string::npos);

    const auto none = readSet (fixture ("absent.als"));
    CHECK_FALSE (none.set.has_value());
    CHECK (none.error.find ("could not be read") != std::string::npos);

    /*  A Live root with no set in it is a set with a problem, not a crash. */
    const auto empty = readSetXml ("<Ableton Creator=\"Ableton Live 12.0\"/>");
    REQUIRE (empty.set.has_value());
    CHECK_FALSE (empty.set->problems.empty());
}
