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
    The import's last stage (namespace draft §29): the walk written into a show
    through the document's checked writes, its media found, its identifiers
    drawn from the set.

    THE SESSION FIXTURE, BUILT: what a reader of the show would check - a GO
    per scene, the sounds, the fade and the stop, the mixes on Live's pairs,
    the DCA, the EQ Eight on the cue's EQ - and that the same set builds the
    same show twice, identifier for identifier (QQ). A serialisation surface,
    so every case runs under fr_FR too.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/import/AlsImport.h>

#include <juce_core/juce_core.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>

using namespace wfg;
using namespace wfg::import::als;

namespace
{
    LiveSet sessionFixture()
    {
        const auto read = readSet (juce::File { juce::String (std::string (WFG_TEST_FIXTURES_DIR)) }
                                       .getChildFile ("als").getChildFile ("session.als.xml"));
        REQUIRE (read.set.has_value());
        return *read.set;
    }

    struct Built
    {
        doc::ShowDocument document;
        BuiltShow show;
        Walk walked;
    };

    /*  The whole fixture, every scene, with each sound named in media/ as the
        importer would and given two channels. */
    std::unique_ptr<Built> buildFixture()
    {
        auto built = std::make_unique<Built>();
        const auto set = sessionFixture();

        WalkOptions options;
        options.scenes = { 0, 1, 2 };
        built->walked = walk (set, options);

        std::map<std::string, std::pair<std::string, int>> media;

        for (const auto& step : built->walked.steps)
            for (const auto& sound : step.sounds)
                media[sound.key] = { sound.file.name, 2 };

        built->show = build (set, built->walked, "Session", media, built->document);
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

    std::string parentOf (const doc::ShowDocument& document, const std::string& id)
    {
        return document.findById (id).getParent().getProperty ("id").toString().toStdString();
    }
}

TEST_CASE ("als import: identifiers are drawn from the set, the same every time, and well formed")
{
    CHECK (doc::Id::isValid (idFor ("sound:37:14")));
    CHECK (idFor ("sound:37:14") == idFor ("sound:37:14"));
    CHECK (idFor ("sound:37:14") != idFor ("sound:37:27"));

    /*  Two builds of one set are one show, byte for byte. */
    const auto first = buildFixture();
    const auto second = buildFixture();
    CHECK (doc::CanonicalXml::write (first->document) == doc::CanonicalXml::write (second->document));
}

TEST_CASE ("als import: scene numbers as Live shows them, and the scenes imported unless somebody says")
{
    const auto parsed = parseScenes ("1-3, 5,16");
    REQUIRE (parsed.has_value());
    CHECK (*parsed == std::set<int> { 0, 1, 2, 4, 15 });

    CHECK_FALSE (parseScenes ("").has_value());
    CHECK_FALSE (parseScenes ("0").has_value());
    CHECK_FALSE (parseScenes ("3-1").has_value());
    CHECK_FALSE (parseScenes ("one").has_value());

    /*  QS: the named scenes that do something - the fixture's third scene has
        no name, and stays out. */
    CHECK (defaultScenes (sessionFixture()) == std::set<int> { 0, 1 });
}

TEST_CASE ("als import: the session fixture becomes a show - GOs, sounds, the fade, the stop, the mixes")
{
    const auto built = buildFixture();
    const auto& document = built->document;

    CHECK (document.validate().empty());
    CHECK (built->show.gos == 3);
    CHECK (built->show.sounds == 2);

    const auto rain = idFor ("sound:37:14");
    const auto wind = idFor ("sound:37:27");
    const auto first = idFor ("go:37");

    /*  THE FIRST GO: a timeline group numbered 1, the scene's name and its
        annotation, holding both sounds. */
    CHECK (elementOf (document, first) == "Group");
    CHECK (at (document, "/godot/cue/" + first + "/mode") == "timeline");
    CHECK (at (document, "/godot/cue/" + first + "/number") == "1");
    CHECK (at (document, "/godot/cue/" + first + "/name") == "top start");
    CHECK (at (document, "/godot/cue/" + first + "/notes").rfind ("MISE", 0) == 0u);
    CHECK (parentOf (document, rain) == first);

    /*  STANDBY ON IT, so the first GO fires it - as a show made by hand has. */
    CHECK (at (document, "/godot/list/" + idFor ("list") + "/standby") == first);

    /*  THE RAIN: its file, its gain, its pre-wait, its range, its lane, its DCA,
        and its EQ Eight's low shelf on band 1. */
    CHECK (at (document, "/godot/cue/" + rain + "/file") == "rain \xc3\xa9t\xc3\xa9.wav");
    CHECK (at (document, "/godot/cue/" + rain + "/channels") == "2");
    CHECK (at (document, "/godot/cue/" + rain + "/level") == "-6");
    CHECK (at (document, "/godot/cue/" + rain + "/preWait") == "0.75");
    CHECK (at (document, "/godot/cue/" + rain + "/levelLane") != "");
    CHECK (at (document, "/godot/cue/" + rain + "/dca") == idFor ("dca:14"));
    CHECK (at (document, "/godot/cue/" + rain + "/eqB1Shape") == "lowShelf");
    CHECK (at (document, "/godot/cue/" + rain + "/eqB1Freq") == "120");
    CHECK (at (document, "/godot/cue/" + rain + "/eqB1Gain") == "-3.5");

    /*  THE WIND: a speed, and a playlist of two ranges - from the top, then
        round the loop for ever. */
    CHECK (at (document, "/godot/cue/" + wind + "/rateMode") == "timestretch");
    const auto loop = idFor ("sound:37:27:loop");
    CHECK (at (document, "/godot/range/" + loop + "/loops") == "0");
    CHECK (at (document, "/godot/range/" + loop + "/in") == "4");

    /*  THE MIXES on Live's pairs, the DCA named after the track. */
    const auto face = idFor ("mix:return:0");
    const auto far = idFor ("mix:return:1");
    CHECK (at (document, "/godot/bus/" + face + "/name") == "Face");
    CHECK (at (document, "/godot/bus/" + face + "/kind") == "mix");
    CHECK (at (document, "/godot/bus/" + far + "/name") == "Loin");
    CHECK (at (document, "/godot/audio/outputPatch") == "0 1 2 3");
    CHECK (at (document, "/godot/dca/" + idFor ("dca:14") + "/name") == "1");

    /*  ITS PLACE (namespace draft §113): track 14 is panned -0.25 in Live and
        the rain is a stereo file, so every send of it leans by that balance;
        its pan row stays at rest. */
    auto rainSends = 0;

    for (const auto* mix : { "return:0", "return:1", "master" })
    {
        const auto sendId = idFor (std::string ("sound:37:14:send:") + mix);

        if (! document.findById (sendId).isValid())
            continue;

        ++rainSends;
        CHECK (at (document, "/godot/send/" + sendId + "/balance") == "-0.25");
        CHECK (at (document, "/godot/send/" + sendId + "/pan") == "0");
    }

    CHECK (rainSends > 0);

    /*  THE SECOND GO: the carrier's fade on the wind, and the rain stopped. */
    const auto fade = idFor ("fade:39:28");
    CHECK (elementOf (document, fade) == "Fade");
    CHECK (at (document, "/godot/cue/" + fade + "/target") == wind);
    CHECK (at (document, "/godot/cue/" + fade + "/stopWhenDone") == "true");
    CHECK (at (document, "/godot/cue/" + fade + "/duration") == "5");

    const auto stop = idFor ("stop:39:14");
    CHECK (elementOf (document, stop) == "Transport");
    CHECK (at (document, "/godot/cue/" + stop + "/target") == rain);
    CHECK (at (document, "/godot/cue/" + stop + "/verb") == "hard");

    /*  THE THIRD: a stop alone is still a group, since a lone cue that is not a
        sound would be a stop numbered as a GO - which it is - and named after
        the scene, which here has none. */
    const auto third = idFor ("go:44");
    CHECK (at (document, "/godot/cue/" + third + "/number") == "3");
    CHECK (at (document, "/godot/cue/" + third + "/name") == "Scene 3");
}

TEST_CASE ("als import: a sound is found on the disk by its path, or by its name and size with its accents folded")
{
    juce::TemporaryFile folder;
    const auto root = folder.getFile();
    REQUIRE (root.createDirectory());

    const auto samples = root.getChildFile ("Samples").getChildFile ("Imported");
    REQUIRE (samples.createDirectory());

    /*  ON THE DISK COMPOSED - one "é" - as a copy off the Mac leaves it. */
    const auto composed = samples.getChildFile (juce::String::fromUTF8 ("sifflements cal\xc3\xa9s.wav"));
    REQUIRE (composed.replaceWithText ("twelve bytes"));

    /*  IN THE SET DECOMPOSED - an "e" and a combining acute - as Live on macOS
        writes it. */
    FileReference decomposed;
    decomposed.relativePath = "Samples/Imported/sifflements cale\xcc\x81s.wav";
    decomposed.name = "sifflements cale\xcc\x81s.wav";
    decomposed.size = composed.getSize();

    /*  Found - by its path where the file system folds the two spellings
        itself, by its folded name where it does not - and it is that file. */
    const auto found = findMedia (decomposed, root);
    CHECK (found.existsAsFile());
    CHECK (found.getSize() == composed.getSize());

    /*  A take of the same name and another size is not it. */
    auto other = decomposed;
    other.size = 999;
    other.relativePath.clear();
    CHECK (findMedia (other, root) == juce::File());

    /*  A ':' the Mac allowed and the copy dropped. */
    const auto cargo = samples.getChildFile ("19_CARGO S.Berger.wav");
    REQUIRE (cargo.replaceWithText ("x"));

    FileReference colon;
    colon.relativePath = "Samples/Imported/19_CARGO: S.Berger.wav";
    colon.name = "19_CARGO: S.Berger.wav";
    CHECK (findMedia (colon, root) == cargo);

    root.deleteRecursively();
}

TEST_CASE ("als import: a performance is named after its date and what its set's name does not share")
{
    CHECK (sharedPrefix ({ "Lazzi r\xc3\xa9gie Pau", "Lazzi r\xc3\xa9gie Agen", "Lazzi r\xc3\xa9gie sophie antibes" })
             == "Lazzi r\xc3\xa9gie ");

    /*  Back to a whole word, so two venues sharing a first letter keep it. */
    CHECK (sharedPrefix ({ "Show Paris", "Show Pau" }) == "Show ");
    CHECK (sharedPrefix ({ "Alone" }).empty());

    juce::TemporaryFile temporary (".als");
    const auto file = temporary.getFile().getParentDirectory().getChildFile ("Lazzi r\xc3\xa9gie Pau.als");
    REQUIRE (file.replaceWithText ("x"));
    REQUIRE (file.setLastModificationTime (juce::Time (2022, 11, 13, 21, 57)));   // months count from nought

    CHECK (performanceName (file, "Lazzi r\xc3\xa9gie ") == "2022-12-13 Pau");
    file.deleteFile();
}

TEST_CASE ("als import: a tour is one show - a template, a performance per set, the media once, the notes borrowed")
{
    juce::TemporaryFile folder;
    const auto root = folder.getFile();
    REQUIRE (root.createDirectory());

    /*  TWO VENUES OF ONE SHOW: the fixture as it is, the newer; and a copy of it
        with its annotation taken off, the older - a set from before Live kept
        annotations. */
    const auto fixture = juce::File { juce::String (std::string (WFG_TEST_FIXTURES_DIR)) }
                             .getChildFile ("als").getChildFile ("session.als.xml");

    const auto newer = root.getChildFile ("Tour Pau.als");
    const auto older = root.getChildFile ("Tour Agen.als");
    REQUIRE (fixture.copyFileTo (newer));

    auto text = fixture.loadFileAsString();
    text = text.replace ("<Annotation Value=\"MISE : Fader 1 \xc3\xa0 -INF&#xA;&#xA;&quot;Il \xc3\xa9tait une fois&quot; &gt; Top\" />",
                         "<Annotation Value=\"\" />");
    REQUIRE (older.replaceWithText (text));
    REQUIRE (older.setLastModificationTime (juce::Time (2022, 9, 10, 20, 0)));
    REQUIRE (newer.setLastModificationTime (juce::Time (2022, 11, 13, 20, 0)));

    ImportOptions options;
    options.into = root.getChildFile ("Tour");
    options.copyMedia = false;

    const auto tour = importTour ({ older, newer }, {}, options);
    REQUIRE (tour.show.ok);
    REQUIRE (tour.performances.size() == 2u);
    CHECK (tour.performances[0].ok);
    CHECK (tour.performances[1].ok);

    /*  THE TEMPLATE IS THE NEWER SET, in the show folder; each performance a
        folder of its own beside the media they share. */
    CHECK (options.into.getChildFile ("Tour.wfg").existsAsFile());
    const auto agen = options.into.getChildFile ("2022-10-10 Agen");
    const auto pau = options.into.getChildFile ("2022-12-13 Pau");
    CHECK (agen.getChildFile ("2022-10-10 Agen.wfg").existsAsFile());
    CHECK (pau.getChildFile ("show.xml").existsAsFile());

    /*  ONE CUE, ONE IDENTIFIER, WHEREVER IT IS - the template's rain is the
        older venue's rain - and the older venue's notes are the template's. */
    doc::ShowDocument template_, performance;
    REQUIRE (doc::Bundle::open (options.into, template_).ok);
    REQUIRE (doc::Bundle::open (agen, performance).ok);

    const auto rain = idFor ("sound:37:14");
    CHECK (template_.findById (rain).isValid());
    CHECK (performance.findById (rain).isValid());

    const auto notes = performance.getAttribute ("/godot/cue/" + idFor ("go:37") + "/notes").value_or ("");
    CHECK (notes.rfind ("MISE", 0) == 0u);

    root.deleteRecursively();
}

TEST_CASE ("als import: into a folder that already holds a show, nothing is written")
{
    juce::TemporaryFile folder;
    const auto root = folder.getFile();
    REQUIRE (root.createDirectory());
    REQUIRE (root.getChildFile ("show.xml").replaceWithText ("<Show/>"));

    ImportOptions options;
    options.into = root;
    options.copyMedia = false;

    const auto outcome = importSet (juce::File { juce::String (std::string (WFG_TEST_FIXTURES_DIR)) }
                                        .getChildFile ("als").getChildFile ("session.als.xml"),
                                    options);
    CHECK_FALSE (outcome.ok);
    CHECK (outcome.error.find ("already holds a show") != std::string::npos);
    CHECK (root.getChildFile ("show.xml").loadFileAsString() == "<Show/>");

    root.deleteRecursively();
}

TEST_CASE ("als import: a return carrying devices is an aux - its own output, level, EQ and plugin, its sends heard")
{
    /*  Namespace draft §93 (answering QE): the fixture's second return, "Loin",
        given Live's Reverb, an EQ Eight with one bell, and a VST3 this machine's
        scan knows. */
    auto set = sessionFixture();
    REQUIRE (set.returns.size() >= 2u);

    Device reverb;
    reverb.kind = "Reverb";
    reverb.name = "Reverb";

    Device eq;
    eq.kind = "Eq8";
    eq.bands.resize (8);
    eq.bands[1].on = true;
    eq.bands[1].mode = 3;
    eq.bands[1].frequency = 2000.0;
    eq.bands[1].gain = 3.0;
    eq.bands[1].q = 1.0;

    Device verb;
    verb.kind = "PluginDevice";
    verb.name = "Verb";
    verb.plugin = PluginInfo { "VST3", "Verb", {}, "0123456789ABCDEF0123456789ABCDEF", { 1, 2, 3 }, { 4, 5 } };

    set.returns[1].devices = { reverb, eq, verb };

    WalkOptions options;
    options.scenes = { 0, 1, 2 };
    const auto walked = walk (set, options);

    std::map<std::string, std::pair<std::string, int>> media;

    for (const auto& step : walked.steps)
        for (const auto& sound : step.sounds)
            media[sound.key] = { sound.file.name, 2 };

    plugin::KnownPlugin known;
    known.name = "Verb";
    known.identifier = "VST3-Verb-1a2b3c4d-5e6f7a8b";
    known.format = "VST3";
    known.path = "C:/plugins/Verb.vst3";

    doc::ShowDocument document;
    const auto shown = build (set, walked, "Session", media, document, { known });

    INFO ("problems: " << (document.validate().empty() ? std::string ("none") : document.validate().front()));
    CHECK (document.validate().empty());

    const auto aux = idFor ("aux:return:1");
    const auto out = idFor ("out:return:1");

    REQUIRE (elementOf (document, aux) == "Channel");
    CHECK (at (document, "/godot/slot/" + aux + "/access") == "shared");
    CHECK (at (document, "/godot/slot/" + aux + "/name") == "Loin");
    CHECK (at (document, "/godot/slot/" + aux + "/directOut") == out);
    CHECK (at (document, "/godot/bus/" + out + "/kind") == "direct");

    //  Its EQ Eight's bell on band 2.
    CHECK (at (document, "/godot/slot/" + aux + "/eqB2Freq") == "2000");
    CHECK (at (document, "/godot/slot/" + aux + "/eqB2Gain") == "3");

    //  Its plugin, found in the scan, its state a file beside the show.
    const auto plugin = idFor ("plugin:return:1:0");
    REQUIRE (elementOf (document, plugin) == "Plugin");
    CHECK (parentOf (document, plugin) == aux);
    CHECK (at (document, "/godot/plugin/" + plugin + "/identifier") == known.identifier);
    CHECK (at (document, "/godot/plugin/" + plugin + "/preset") == "aux-" + plugin + ".state");
    REQUIRE (shown.files.size() == 1u);
    CHECK (shown.files[0].first == "plugins/aux-" + plugin + ".state");

    //  Every send into it is heard.
    auto sendsIntoAux = 0;

    for (const auto& step : walked.steps)
        for (const auto& sound : step.sounds)
        {
            const auto sendId = idFor (sound.key + ":send:return:1");

            if (! document.findById (sendId).isValid())
                continue;

            ++sendsIntoAux;
            CHECK (at (document, "/godot/send/" + sendId + "/bus") == aux);
            CHECK (at (document, "/godot/send/" + sendId + "/on") != "false");
        }

    CHECK (sendsIntoAux > 0);

    //  And Live's reverb said, for the author to replace.
    CHECK (std::any_of (walked.notes.begin(), walked.notes.end(), [] (const Note& note)
                        { return note.text.find ("Live's own") != std::string::npos; }));
}

TEST_CASE ("als import: a direct out is a direct bus on the cue, and a return on a held pair an aux panned into the mix (namespace draft §121, BDS, BDT)")
{
    /*  The fixture, its track 14 wired straight to the interface's sixth
        channel, and a mono return "C-Side" on channel 1 - the right of Face's
        pair - beside the two it has. */
    auto set = sessionFixture();
    REQUIRE (set.returns.size() == 2u);
    REQUIRE (! set.tracks.empty());

    Track side;
    side.id = "92";
    side.name = "C-Side";
    side.kind = Track::Kind::returnTrack;
    side.output.kind = Routing::Kind::external;
    side.output.stereo = false;
    side.output.channel = 1;
    side.sends.assign (3, 0.0003162277571);
    set.returns.push_back (side);
    set.sendsPre.push_back (false);

    for (auto& ret : set.returns)
        ret.sends.resize (3, 0.0003162277571);

    for (auto& track : set.tracks)
    {
        track.sends.resize (3, 0.0003162277571);

        if (track.id == "14")
        {
            track.output.kind = Routing::Kind::external;
            track.output.stereo = false;
            track.output.channel = 5;
            track.sends[2] = 1.0;
        }
    }

    WalkOptions options;
    options.scenes = { 0, 1, 2 };
    const auto walked = walk (set, options);

    std::map<std::string, std::pair<std::string, int>> media;

    for (const auto& step : walked.steps)
        for (const auto& sound : step.sounds)
            media[sound.key] = { sound.file.name, 2 };

    doc::ShowDocument document;
    build (set, walked, "Session", media, document);

    INFO ("problems: " << (document.validate().empty() ? std::string ("none") : document.validate().front()));
    CHECK (document.validate().empty());

    /*  THE DIRECT OUT: a bus of that kind, one channel wide, on channel 5 of the
        patch, named on the rain's directOut. */
    const auto direct = idFor ("direct:direct:14");
    const auto rain = idFor ("sound:37:14");
    CHECK (at (document, "/godot/bus/" + direct + "/kind") == "direct");
    CHECK (at (document, "/godot/bus/" + direct + "/width") == "1");
    CHECK (at (document, "/godot/bus/" + direct + "/name") == "1");
    CHECK (at (document, "/godot/cue/" + rain + "/directOut") == direct);
    CHECK (at (document, "/godot/audio/outputPatch") == "0 1 2 3 5");

    /*  THE SIDE: a mono aux with no output of its own, its one send into Face
        panned hard right; the rain's send to it heard. */
    const auto aux = idFor ("aux:return:2");
    const auto face = idFor ("mix:return:0");
    REQUIRE (elementOf (document, aux) == "Channel");
    CHECK (at (document, "/godot/slot/" + aux + "/class") == "mono");
    CHECK_FALSE (document.findById (idFor ("out:return:2")).isValid());

    const auto intoFace = idFor ("auxsend:return:2:return:0");
    CHECK (at (document, "/godot/send/" + intoFace + "/bus") == face);
    CHECK (at (document, "/godot/send/" + intoFace + "/level") == "0");
    CHECK (at (document, "/godot/send/" + intoFace + "/pan") == "1");
    CHECK (at (document, "/godot/send/" + idFor ("sound:37:14:send:return:2") + "/bus") == aux);
}

TEST_CASE ("als import: a live input is a mic cue on a named input and a rack channel of its own, patched where Live listened (namespace draft §121, BDU)")
{
    /*  The fixture's "4 track in" made a live input: its input the
        interface's fourth channel rather than track 4, which then feeds
        nothing. Its carrier clip in the second scene is a fade on the mic cue. */
    auto set = sessionFixture();

    for (auto& track : set.tracks)
    {
        if (track.id == "28")
        {
            track.input.kind = Routing::Kind::external;
            track.input.stereo = false;
            track.input.channel = 3;
        }

        if (track.id == "27")
            track.output.kind = Routing::Kind::none;
    }

    WalkOptions options;
    options.scenes = { 0, 1, 2 };
    const auto walked = walk (set, options);

    std::map<std::string, std::pair<std::string, int>> media;

    for (const auto& step : walked.steps)
        for (const auto& sound : step.sounds)
            if (! sound.live)
                media[sound.key] = { sound.file.name, 2 };

    doc::ShowDocument document;
    const auto shown = build (set, walked, "Session", media, document);

    INFO ("problems: " << (document.validate().empty() ? std::string ("none") : document.validate().front()));
    CHECK (document.validate().empty());
    CHECK (shown.lives == 1);

    const auto mic = idFor ("live:28");
    const auto input = idFor ("in:live:28");
    const auto channel = idFor ("chan:live:28");
    REQUIRE (elementOf (document, mic) == "Mic");
    CHECK (parentOf (document, mic) == idFor ("go:37"));
    CHECK (at (document, "/godot/cue/" + mic + "/name") == "4 track in");
    CHECK (at (document, "/godot/cue/" + mic + "/input") == input);
    CHECK (at (document, "/godot/cue/" + mic + "/channel") == channel);
    CHECK (at (document, "/godot/input/" + input + "/name") == "4 track in");
    CHECK (at (document, "/godot/input/" + input + "/width") == "1");
    CHECK (at (document, "/godot/slot/" + channel + "/class") == "mono");
    CHECK (at (document, "/godot/slot/" + channel + "/access") != "shared");
    CHECK (at (document, "/godot/audio/inputPatch") == "3");

    //  Its sends, as the track's stand: Face at 0 dB, Loin at -12.
    CHECK (at (document, "/godot/send/" + idFor ("live:28:send:return:0") + "/level") == "0");

    //  The fade in the second GO aims at it.
    const auto fade = idFor ("fade:39:28");
    REQUIRE (elementOf (document, fade) == "Fade");
    CHECK (at (document, "/godot/cue/" + fade + "/target") == mic);
}

TEST_CASE ("als import: a controller the set maps nowhere is a MIDI cue on a Port named after Live's, a ramp its fade (namespace draft §121, BDV)")
{
    Walk walked;
    Step step;
    step.scene = 0;
    step.number = 1;
    step.sceneId = "37";
    step.name = "top start";

    MidiOut sent;
    sent.key = "midi:37:50:99";
    sent.scene = 0;
    sent.name = "Fade CC 99";
    sent.port = "IAC MIDI (Bus 1)";
    sent.channel = 7;
    sent.controller = 99;
    sent.from = 10;
    sent.to = 100;
    sent.seconds = 2.0;
    step.midi.push_back (sent);
    walked.steps.push_back (step);

    doc::ShowDocument document;
    const auto shown = build (sessionFixture(), walked, "Session", {}, document);

    INFO ("problems: " << (document.validate().empty() ? std::string ("none") : document.validate().front()));
    CHECK (document.validate().empty());
    CHECK (shown.gos == 1);

    const auto cue = idFor ("midi:37:50:99");
    const auto port = idFor ("port:IAC MIDI (Bus 1)");
    REQUIRE (elementOf (document, cue) == "Midi");
    CHECK (parentOf (document, cue) == idFor ("go:37"));
    CHECK (at (document, "/godot/cue/" + cue + "/port") == port);
    CHECK (at (document, "/godot/port/" + port + "/name") == "IAC MIDI (Bus 1)");
    CHECK (at (document, "/godot/cue/" + cue + "/type") == "controlChange");
    CHECK (at (document, "/godot/cue/" + cue + "/channel") == "7");
    CHECK (at (document, "/godot/cue/" + cue + "/data1") == "99");
    CHECK (at (document, "/godot/cue/" + cue + "/data2") == "10");
    CHECK (at (document, "/godot/cue/" + cue + "/fadeTo") == "100");
    CHECK (at (document, "/godot/cue/" + cue + "/fadeTime") == "2");
}

TEST_CASE ("als import: a GO on the grid carries its sounds' quantise, a scene's tempo leads it as a tempo cue, and the show's tempo is the set's (namespace draft §121, BDZ; §122)")
{
    Walk walked;
    Step step;
    step.scene = 0;
    step.number = 1;
    step.sceneId = "37";
    step.name = "top start";
    step.tempo = 100.0;

    Sound sound;
    sound.key = "sound:37:14";
    sound.scene = 0;
    sound.trackId = "14";
    sound.trackName = "1";
    sound.name = "rain";
    sound.file.name = "rain.wav";
    sound.quantise = "bar";
    step.sounds.push_back (sound);
    walked.steps.push_back (step);

    doc::ShowDocument document;
    build (sessionFixture(), walked, "Session", { { "sound:37:14", { "rain.wav", 2 } } }, document);

    INFO ("problems: " << (document.validate().empty() ? std::string ("none") : document.validate().front()));
    CHECK (document.validate().empty());

    const auto group = idFor ("go:37");
    const auto tempo = idFor ("tempo:37");
    const auto rain = idFor ("sound:37:14");
    REQUIRE (elementOf (document, group) == "Group");
    CHECK (at (document, "/godot/cue/" + group + "/quantise") == "bar");
    CHECK (at (document, "/godot/cue/" + rain + "/quantise") != "bar");

    REQUIRE (elementOf (document, tempo) == "Transport");
    CHECK (at (document, "/godot/cue/" + tempo + "/verb") == "tempo");
    CHECK (at (document, "/godot/cue/" + tempo + "/bpm") == "100");
    CHECK (document.findById (group).getChild (0).getProperty ("id").toString().toStdString() == tempo);

    //  The show's tempo is the set's 120, in its 4/4.
    CHECK (at (document, "/godot/sync/bpm") == "120");
    CHECK (at (document, "/godot/sync/beatsPerBar") == "4");
}

TEST_CASE ("als import: a scene's bar rides its GO's tempo cue, alone or beside a tempo, and the set's bar is the show's (namespace draft §121, BEG)")
{
    auto set = sessionFixture();
    set.timeSignature = { 6, 8 };

    Walk walked;

    for (const auto& [sceneId, tempo, bar] : { std::tuple<const char*, double, int> { "37", 100.0, 3 },
                                               std::tuple<const char*, double, int> { "39", 0.0, 5 } })
    {
        Step step;
        step.scene = static_cast<int> (walked.steps.size());
        step.number = step.scene + 1;
        step.sceneId = sceneId;
        step.name = std::string ("Scene ") + sceneId;
        step.tempo = tempo;
        step.beatsPerBar = bar;
        walked.steps.push_back (step);
    }

    doc::ShowDocument document;
    build (set, walked, "Session", {}, document);

    INFO ("problems: " << (document.validate().empty() ? std::string ("none") : document.validate().front()));
    CHECK (document.validate().empty());
    CHECK (at (document, "/godot/sync/beatsPerBar") == "3");

    const auto both = idFor ("tempo:37");
    CHECK (at (document, "/godot/cue/" + both + "/bpm") == "100");
    CHECK (at (document, "/godot/cue/" + both + "/beatsPerBar") == "3");
    CHECK (at (document, "/godot/cue/" + both + "/name") == "Tempo 100, bar of 3");

    const auto barAlone = idFor ("tempo:39");
    CHECK (at (document, "/godot/cue/" + barAlone + "/bpm") == "0");
    CHECK (at (document, "/godot/cue/" + barAlone + "/beatsPerBar") == "5");
    CHECK (at (document, "/godot/cue/" + barAlone + "/name") == "Bar of 5");
}

TEST_CASE ("als import: a VST3's state is written as JUCE keeps one, an AU's as it is, a VST2's not at all")
{
    PluginInfo vst3 { "VST3", "Verb", {}, "0123456789ABCDEF0123456789ABCDEF", { 1, 2, 3 }, { 4, 5 } };
    const auto bytes = presetBytesOf (vst3);

    REQUIRE (bytes.size() > 8u);
    CHECK (bytes[0] == 0x56);
    CHECK (bytes[1] == 0x43);
    CHECK (bytes[2] == 0x32);
    CHECK (bytes[3] == 0x21);

    const std::string text (reinterpret_cast<const char*> (bytes.data() + 8), bytes.size() - 9);
    const auto xml = juce::parseXML (juce::String::fromUTF8 (text.c_str()));
    REQUIRE (xml != nullptr);
    CHECK (xml->hasTagName ("VST3PluginState"));

    juce::MemoryBlock component;
    REQUIRE (component.fromBase64Encoding (xml->getChildByName ("IComponent")->getAllSubText()));
    CHECK (component.getSize() == 3u);
    CHECK (static_cast<const std::uint8_t*> (component.getData())[2] == 3);

    PluginInfo au { "AudioUnit", "Verb", {}, "aufx/verb/Acme", { 9, 8, 7 }, {} };
    CHECK (presetBytesOf (au) == std::vector<std::uint8_t> { 9, 8, 7 });

    PluginInfo vst2 { "VST", "Verb", "C:/Verb.dll", "1234", { 1 }, {} };
    CHECK (presetBytesOf (vst2).empty());
}
