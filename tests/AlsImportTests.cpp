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
