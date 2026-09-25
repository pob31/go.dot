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
    THE PLUGIN'S OWN WINDOW, WITH NO WINDOW (the author's decision of
    2026-09-25). A helper process per open window, `wfg plugin-editor`, driven
    here through its parent's end with `--no-window` - the test binary is the
    helper, as it is the voice child in ProxyTests - and the built-in test
    gain, a real processor in the helper, as the plugin.

    What is pinned: the region's ring; the helper comes up and says so; it
    puts its plugin where a subject says and reports nothing for doing it; a
    hand on the window is one value event under the subject it was made on;
    greyed, a hand goes nowhere; a value moved elsewhere comes in but never
    over the hand's own; the close button is an event; leave ends it; a
    helper that dies is seen to have gone; a plugin it cannot make says why.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/plugin/Catalogue.h>
#include <wfg/engine/plugin/EditorHost.h>
#include <wfg/engine/plugin/EditorRegion.h>

#include <juce_core/juce_core.h>

#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

using namespace wfg;

namespace
{
    using Status = plugin::EditorHost::Status;
    using Kind = plugin::editor::EventKind;

    struct Folder
    {
        Folder()
            : path (juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("wfg-editor-test-" + juce::Uuid().toDashedString()))
        {
            path.createDirectory();
        }

        ~Folder() { path.deleteRecursively(); }

        std::string string() const { return path.getFullPathName().toStdString(); }

        juce::File path;
    };

    plugin::EditorSpec testGainEditor (const Folder& folder)
    {
        plugin::EditorSpec spec;
        spec.pluginId = "PG7N0001";
        spec.identifier = plugin::Catalogue::testGainIdentifier();
        spec.name = "Test gain";
        spec.workFolder = folder.string();
        spec.headless = true;
        spec.launch.executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                                     .getFullPathName().toStdString();
        spec.launch.leadingArgs = { "plugin-editor" };
        return spec;
    }

    /** Polls the host until `done`, or gives up; true when done. */
    template <typename Done>
    bool waitFor (plugin::EditorHost& host, Done done, int milliseconds = 10000)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds (milliseconds);

        while (std::chrono::steady_clock::now() < until)
        {
            host.poll();

            if (done())
                return true;

            std::this_thread::sleep_for (std::chrono::milliseconds (10));
        }

        host.poll();
        return done();
    }

    bool near (float a, float b)
    {
        return std::abs (a - b) < 1.0e-4f;
    }

    plugin::EditorHost::Subject subjectOf (const char* cueId, const char* fxId, std::vector<float> values)
    {
        plugin::EditorHost::Subject subject;
        subject.cueId = cueId;
        subject.fxId = fxId;
        subject.title = std::string ("Test gain - ") + cueId;
        subject.reason = "in this cue's signal";
        subject.greyed = false;
        subject.values = std::move (values);
        return subject;
    }
}

TEST_CASE ("editor helper: the region's ring carries events in order, and says when it is full")
{
    auto region = std::make_unique<plugin::editor::Region>();

    CHECK_FALSE (plugin::editor::looksValid (*region));
    region->magic.store (plugin::editor::magic);
    region->version.store (plugin::editor::version);
    region->layoutHash.store (plugin::editor::layoutHash());
    CHECK (plugin::editor::looksValid (*region));

    plugin::editor::Popped popped;
    CHECK_FALSE (plugin::editor::pop (*region, popped));

    for (std::uint32_t i = 0; i < plugin::editor::ringSize; ++i)
        REQUIRE (plugin::editor::push (*region, Kind::value, i, 0.5f, 7));

    //  Full: the helper drops rather than waits.
    CHECK_FALSE (plugin::editor::push (*region, Kind::value, 9999, 0.5f, 7));

    for (std::uint32_t i = 0; i < plugin::editor::ringSize; ++i)
    {
        REQUIRE (plugin::editor::pop (*region, popped));
        CHECK (popped.kind == Kind::value);
        CHECK (popped.index == i);
        CHECK (popped.subjectSeq == 7u);
    }

    //  And round again, past the wrap.
    REQUIRE (plugin::editor::push (*region, Kind::windowClosed, 0, 0.0f, 8));
    REQUIRE (plugin::editor::pop (*region, popped));
    CHECK (popped.kind == Kind::windowClosed);
    CHECK_FALSE (plugin::editor::pop (*region, popped));
}

TEST_CASE ("editor helper: the test gain comes up with no window, takes a subject, and says what a hand did")
{
    Folder folder;
    plugin::EditorHost host (testGainEditor (folder));

    std::string why;
    REQUIRE_MESSAGE (host.start (why), why);
    REQUIRE (waitFor (host, [&] { return host.status() != Status::starting; }));
    REQUIRE_MESSAGE (host.status() == Status::open, host.problem());
    CHECK (host.paramCount() == 2);
    CHECK (host.pid() >= 0);

    /*  THE CUE'S VALUES, AND ONE IT DOES NOT MENTION: Gain where the cue
        says, Die where the preset left it - off. */
    host.setSubject (subjectOf ("CUE00001", "FX000001", { 0.25f, plugin::editor::restsAtPreset }));
    REQUIRE (waitFor (host, [&] { return host.subjectTaken() == 1u; }));
    REQUIRE (waitFor (host, [&] { return near (host.currentValue (0), 0.25f); }));
    CHECK (near (host.currentValue (1), 0.0f));

    //  Putting the plugin where the cue says reports nothing: it was not a hand.
    CHECK (host.drain().empty());

    SUBCASE ("a hand on the window is one value event, tagged with the cue it was made on")
    {
        host.poke (0, 0.7f);

        std::vector<plugin::EditorHost::Event> seen;
        REQUIRE (waitFor (host, [&]
        {
            for (const auto& event : host.drain())
                seen.push_back (event);

            return ! seen.empty();
        }));

        REQUIRE (seen.size() == 1);
        CHECK (seen[0].kind == Kind::value);
        CHECK (seen[0].index == 0);
        CHECK (near (seen[0].value, 0.7f));
        CHECK (host.fxIdFor (seen[0].subjectSeq) == "FX000001");
    }

    SUBCASE ("greyed, a hand goes nowhere, and the subject names no insert")
    {
        auto greyed = subjectOf ("CUE00002", "", {});
        greyed.greyed = true;
        greyed.reason = "2 Memo plays no file: inserts belong to media cues.";
        host.setSubject (greyed);
        REQUIRE (waitFor (host, [&] { return host.subjectTaken() == 2u; }));

        host.poke (0, 0.1f);
        std::this_thread::sleep_for (std::chrono::milliseconds (150));
        host.poll();

        CHECK (host.drain().empty());
        CHECK (host.fxIdFor (2).empty());
    }

    SUBCASE ("a value moved elsewhere is brought in, but never over the hand's own")
    {
        host.poke (0, 0.6f);
        REQUIRE (waitFor (host, [&] { return near (host.currentValue (0), 0.6f); }));
        host.drain();

        //  The tree's older copy, arriving while the hand's value is fresh: not taken...
        host.setLive ({ 0.2f, plugin::editor::restsAtPreset });
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
        CHECK (near (host.currentValue (0), 0.6f));

        //  ...and taken once the hand's moment has passed - an undo does come through.
        REQUIRE (waitFor (host, [&] { return near (host.currentValue (0), 0.2f); }, 3000));
        CHECK (host.drain().empty());
    }

    SUBCASE ("a new cue's values replace the last one's, with no event for it")
    {
        host.setSubject (subjectOf ("CUE00003", "FX000003", { 0.9f, 1.0f }));
        REQUIRE (waitFor (host, [&] { return host.subjectTaken() == 2u; }));
        REQUIRE (waitFor (host, [&] { return near (host.currentValue (0), 0.9f) && near (host.currentValue (1), 1.0f); }));
        CHECK (host.drain().empty());

        host.poke (1, 0.0f);

        std::vector<plugin::EditorHost::Event> seen;
        REQUIRE (waitFor (host, [&]
        {
            for (const auto& event : host.drain())
                seen.push_back (event);

            return ! seen.empty();
        }));

        CHECK (host.fxIdFor (seen.front().subjectSeq) == "FX000003");
    }

    SUBCASE ("its close button is an event, and leave ends it")
    {
        host.poke (-2, 0.0f);

        std::vector<plugin::EditorHost::Event> seen;
        REQUIRE (waitFor (host, [&]
        {
            for (const auto& event : host.drain())
                seen.push_back (event);

            return ! seen.empty();
        }));

        CHECK (seen.front().kind == Kind::windowClosed);

        host.leave();
        REQUIRE (waitFor (host, [&] { return host.status() == Status::ended; }, 3000));
    }

    SUBCASE ("a helper that dies is seen to have gone")
    {
        host.kill();
        REQUIRE (waitFor (host, [&] { return host.status() == Status::ended; }, 3000));
    }
}

TEST_CASE ("editor helper: a real window, when somebody asks for one")
{
    /*  NOT AN ASSERTION BUT AN EYE, as the panels' pictures are: with
        WFG_EDITOR_WINDOW set to a number of seconds, the helper opens its
        window for that long - the test gain's generic editor under Go.dot's
        line - moving Gain once a second so the window can be seen following
        the tree, then greys it for the last second. With WFG_REAL_VST3 set,
        that plugin instead (its description from WFG_REAL_VST3_XML). Skipped,
        silently and green, everywhere else. */
    const auto seconds = juce::SystemStats::getEnvironmentVariable ("WFG_EDITOR_WINDOW", {}).getIntValue();

    if (seconds <= 0)
        return;

    Folder folder;
    auto spec = testGainEditor (folder);
    spec.headless = false;

    const auto real = juce::SystemStats::getEnvironmentVariable ("WFG_REAL_VST3", {});

    if (real.isNotEmpty())
    {
        spec.identifier = real.toStdString();
        spec.name = real.toStdString();
        spec.descriptionXml = juce::File (juce::SystemStats::getEnvironmentVariable ("WFG_REAL_VST3_XML", {}))
                                  .loadFileAsString().toStdString();
    }

    plugin::EditorHost host (std::move (spec));

    std::string why;
    REQUIRE_MESSAGE (host.start (why), why);
    REQUIRE (waitFor (host, [&] { return host.status() != Status::starting; }, 30000));
    REQUIRE_MESSAGE (host.status() == Status::open, host.problem());
    MESSAGE ("the helper is pid " << host.pid() << ", up in " << host.loadMicroseconds() / 1000u << " ms");

    host.setSubject (subjectOf ("CUE00001", "FX000001", { 0.25f, plugin::editor::restsAtPreset }));

    for (int second = 0; second < seconds - 1; ++second)
    {
        waitFor (host, [] { return false; }, 1000);
        host.setLive ({ 0.2f + 0.1f * static_cast<float> (second % 7), plugin::editor::restsAtPreset });

        for (const auto& event : host.drain())
            MESSAGE ("the window said: kind " << static_cast<int> (event.kind) << ", parameter "
                                             << event.index << " = " << event.value);
    }

    auto greyed = subjectOf ("CUE00002", "", {});
    greyed.greyed = true;
    greyed.title = "Test gain - 2 Memo";
    greyed.reason = "2 Memo plays no file: inserts belong to media cues.";
    host.setSubject (greyed);
    waitFor (host, [] { return false; }, 1000);
}

TEST_CASE ("editor helper: a plugin it cannot make says why, in a sentence")
{
    Folder folder;
    auto spec = testGainEditor (folder);
    spec.identifier = "VST3-00000000-nowhere";

    plugin::EditorHost host (std::move (spec));

    std::string why;
    REQUIRE_MESSAGE (host.start (why), why);
    REQUIRE (waitFor (host, [&] { return host.status() != Status::starting; }));
    CHECK (host.status() == Status::failed);
    CHECK_FALSE (host.problem().empty());
}
