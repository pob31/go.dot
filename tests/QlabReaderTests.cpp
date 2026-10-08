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
    A QLab workspace read into plain facts (namespace draft §46, QL.4): QLab 5
    and QLab 4 into the same facts, the differences of §46.5 met one by one -
    the network destinations, the message's key, the relative path's folder,
    the fade's absolute or relative, the build number's type - what is stored
    read and nothing derived, and any other version refused in words.

    The workspaces are built by tests/QlabFixture.h until the author's probe
    workspaces arrive (ZQ); WFG_QLAB_CORPUS reads the author's real shows,
    never committed.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "QlabFixture.h"

#include <wfg/engine/import/QlabReader.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace wfg::import::qlab;
namespace fixture = wfg::test::qlab;
using fixture::Value;

namespace
{
    Workspace read (const std::vector<std::uint8_t>& bytes)
    {
        const auto result = readWorkspaceBytes (bytes.data(), bytes.size());
        INFO (result.error);
        REQUIRE (result.workspace.has_value());
        return *result.workspace;
    }

    /*  The name as a pointer, not a std::string: GCC 13 takes a reference
        returned beside a temporary string argument for one that may dangle
        (-Wdangling-reference), and the strict build makes that an error. */
    const Cue& named (const Cue& in, const char* name)
    {
        for (const auto& child : in.children)
        {
            if (child.name == name)
                return child;

            if (! child.children.empty())
                for (const auto& deeper : child.children)
                    if (deeper.name == name)
                        return deeper;
        }

        FAIL ("no cue named " << name);
        return in;
    }

    double db (double gain) { return gain <= 0.0 ? -200.0 : 20.0 * std::log10 (gain); }

    /*  ONE SCENE OF A SHOW, the same in either version: a start-first-and-enter
        group holding a playlist of two messages, a timeline of a sound and its
        fade, a relative fade on that timeline, an OSC fade, a Mic, a script,
        and a memo with notes. */
    std::vector<std::uint8_t> scene (int major)
    {
        fixture::Workspace w (major);
        const auto s21 = major >= 5 ? std::string ("PATCH-S21") : std::string ("3");
        const auto wfs = major >= 5 ? std::string ("PATCH-WFS") : std::string ("2");

        const auto unmute = w.network ("UnMute CG Monitors", "/channel/118/mute \\F", s21,
                                       { { "continueMode", Value::integer (major >= 5 ? 0 : 2) } });
        const auto place = w.network ("Set input 21", "/wfs/input/21/positionXYZ -3.0 1.0 3.0", wfs);
        const auto doors = w.group ("Doors open", major >= 5 ? 6 : 1, { unmute, place });

        const auto sound = w.audio ("", "audio/ambiances_19.WAV", { { 0, 0, -200.0 }, { 0, 1, 0.0 }, { 0, 2, -6.0 },
                                                                   { 1, 0, 0.0 }, { 1, 1, 0.0 }, { 2, 0, 0.0 }, { 2, 2, -3.0 } },
                                    { { "rate", Value::number (0.55) }, { "doPitchShift", Value::boolean (true) },
                                      { "startTime", Value::number (22.0) }, { "endTime", Value::number (595.25) },
                                      { "playCount", Value::integer (2) }, { "infiniteLoop", Value::boolean (true) } });
        const auto fadeIn = w.fade ("", sound, 1.0, true, { { 0, 0, -30.0 } });
        const auto machinery = w.group ("Machinery sounds", 3, { sound, fadeIn });
        const auto down = w.fade ("", machinery, 3.0, false, { { 0, 0, -9.0 } }, false, 2, 0.45);
        const auto fader = w.networkFade ("CG QLab Fader -18 > 0dB", "/channel/114/fader #v#", s21, -18.0, 0.0, 5.0);
        const auto mic = w.cue ("MicCue", "tails 1", { { "audioInputPatchID", Value::string ("PATCH-IN") },
                                                       { "channelOffset", Value::integer (4) }, { "channels", Value::integer (2) } });
        const auto script = w.cue ("ScriptCue", "a script", { { "source", Value::string ("tell application \"QLab\"") } });
        const auto memo = w.notes (w.memo ("TUNE"), "When house lights go down");

        const auto top = w.notes (w.group ("1 - Doors", 1, { doors, machinery, down, fader, mic, script, memo }),
                                  "Start before opening doors");
        const auto list = w.group ("Main", 0, { top });
        return w.bytes ({ list });
    }
}

TEST_CASE ("qlab reader: a QLab 5 workspace, settings and cues, as QLab stores them")
{
    const auto workspace = read (scene (5));

    CHECK (workspace.major == 5);
    CHECK (workspace.version == "5.6.3");
    CHECK (workspace.build == "5603");
    CHECK (workspace.name == "Fixture");
    CHECK (workspace.minVolume == doctest::Approx (-80.0));

    REQUIRE (workspace.audioPatches.size() == 1);
    CHECK (workspace.audioPatches[0].id == "PATCH-AUDIO");
    CHECK (workspace.audioPatches[0].outputs == 4);
    CHECK (workspace.audioPatches[0].outputNames.at (3) == "Mon 1");

    REQUIRE (workspace.networkPatches.size() == 2);
    CHECK (workspace.networkPatches[1].name == "S21");
    CHECK (workspace.networkPatches[1].host == "192.168.1.221");
    CHECK (workspace.networkPatches[1].port == 8024);
    CHECK (workspace.networkPatches[1].kind == "osc");
    CHECK_FALSE (workspace.networkPatches[1].tcp);

    REQUIRE (workspace.lists.size() == 1);
    const auto& list = workspace.lists[0];
    CHECK (list.type == "Group");
    CHECK (list.groupMode == 0);
    CHECK (countCues (list) == 12);

    const auto& top = list.children.at (0);
    CHECK (top.name == "1 - Doors");
    CHECK (top.groupMode == 1);
    CHECK (top.notes == "Start before opening doors");

    const auto& doors = named (top, "Doors open");
    CHECK (doors.groupMode == 6);
    REQUIRE (doors.children.size() == 2);
    CHECK (doors.children[0].type == "OSC");
    CHECK (doors.children[0].message == "/channel/118/mute \\F");
    CHECK (doors.children[0].networkPatch == "PATCH-S21");
    CHECK (doors.children[0].continueMode == 0);        // what is stored, not what QLab derives

    const auto& machinery = named (top, "Machinery sounds");
    const auto& sound = machinery.children.at (0);
    CHECK (sound.type == "Audio");
    CHECK (sound.name.empty());
    CHECK (sound.file.relativePath == "audio/ambiances_19.WAV");
    CHECK_FALSE (sound.file.relativeToParent);
    CHECK (sound.file.name() == "ambiances_19.WAV");
    CHECK (sound.audioPatch == "PATCH-AUDIO");
    CHECK (sound.rate == doctest::Approx (0.55));
    CHECK (sound.pitchFollowsRate);
    CHECK (sound.startTime == doctest::Approx (22.0));
    CHECK (sound.endTime == doctest::Approx (595.25));
    CHECK (sound.playCount == 2);
    CHECK (sound.infiniteLoop);

    REQUIRE (sound.levels.size() == 7);
    CHECK (sound.levels[0].row == 0);
    CHECK (sound.levels[0].column == 0);
    CHECK (sound.levels[0].gain == doctest::Approx (0.0));
    CHECK (db (sound.levels[2].gain) == doctest::Approx (-6.0));
    CHECK (sound.levels[6].row == 2);
    CHECK (sound.levels[6].column == 2);

    const auto& fadeIn = machinery.children.at (1);
    CHECK (fadeIn.type == "Fade");
    CHECK (fadeIn.target == sound.id);
    CHECK (fadeIn.absolute);
    REQUIRE (fadeIn.fadeLevels.size() == 1);
    CHECK (db (fadeIn.fadeLevels[0].end) == doctest::Approx (-30.0));
    CHECK (fadeIn.shape.type == 1);

    const auto& down = top.children.at (2);
    CHECK (down.type == "Fade");
    CHECK (down.target == machinery.id);
    CHECK_FALSE (down.absolute);
    CHECK (down.shape.type == 2);
    CHECK (down.shape.parameter == doctest::Approx (0.45));

    const auto& fader = named (top, "CG QLab Fader -18 > 0dB");
    CHECK (fader.networkFadeType == 1);
    CHECK (fader.message == "/channel/114/fader #v#");
    CHECK (fader.fadeFrom == doctest::Approx (-18.0));
    CHECK (fader.fadeTo == doctest::Approx (0.0));
    CHECK (fader.duration == doctest::Approx (5.0));
    CHECK (fader.shape.type == 3);

    const auto& mic = named (top, "tails 1");
    CHECK (mic.type == "Mic");
    CHECK (mic.inputChannel == 4);
    CHECK (mic.inputChannels == 2);

    CHECK (named (top, "a script").source == "tell application \"QLab\"");
    CHECK (named (top, "TUNE").notes == "When house lights go down");
}

TEST_CASE ("qlab reader: a QLab 4 workspace reads into the same facts (§46.5)")
{
    const auto workspace = read (scene (4));

    CHECK (workspace.major == 4);
    CHECK (workspace.build == "4405");

    //  Sixteen destinations by number, the empty ones left out.
    REQUIRE (workspace.networkPatches.size() == 3);
    CHECK (workspace.networkPatches[2].id == "3");
    CHECK (workspace.networkPatches[2].name == "S21");
    CHECK (workspace.networkPatches[2].port == 8024);

    REQUIRE (workspace.audioPatches.size() == 1);
    CHECK (workspace.audioPatches[0].id == "1");
    CHECK (workspace.audioPatches[0].outputNames.at (2) == "FOH R");

    const auto& top = workspace.lists.at (0).children.at (0);
    const auto& doors = named (top, "Doors open");
    CHECK (doors.groupMode == 1);
    CHECK (doors.children[0].message == "/channel/118/mute \\F");
    CHECK (doors.children[0].networkPatch == "3");
    CHECK (doors.children[0].messageType == 2);
    CHECK (doors.children[0].continueMode == 2);        // an auto-follow somebody set

    const auto& sound = named (top, "Machinery sounds").children.at (0);
    CHECK (sound.file.relativePath == "Show/audio/ambiances_19.WAV");
    CHECK (sound.file.relativeToParent);
    CHECK (sound.audioPatch == "1");

    const auto& down = top.children.at (2);
    CHECK_FALSE (down.absolute);
    CHECK (down.shape.type == 2);

    CHECK (named (top, "CG QLab Fader -18 > 0dB").networkFadeType == 1);
}

TEST_CASE ("qlab reader: what is not a QLab 4 or 5 workspace is refused in words")
{
    const std::vector<std::uint8_t> text { 'n', 'o', 't', ' ', 'a', ' ', 'w', 'o', 'r', 'k', 's', 'p', 'a', 'c', 'e' };
    auto result = readWorkspaceBytes (text.data(), text.size());
    CHECK_FALSE (result.workspace.has_value());
    CHECK (result.error == "not a QLab workspace: not a binary property list");

    //  A QLab 3 workspace, as far as its version says.
    {
        wfg::test::plist::Keyed outer;
        const auto top = outer.dictionary ({ { outer.add (Value::string ("QLabShortVersionString")),
                                               outer.add (Value::string ("3.2.14")) } });
        const auto bytes = outer.archive (top);
        result = readWorkspaceBytes (bytes.data(), bytes.size());
        CHECK (result.error == "a QLab 3.2.14 workspace: Go.dot reads QLab 4 and QLab 5 workspaces only");
    }

    //  A keyed archive that is not a workspace.
    {
        wfg::test::plist::Keyed outer;
        const auto bytes = outer.archive (outer.dictionary ({}));
        result = readWorkspaceBytes (bytes.data(), bytes.size());
        CHECK (result.error == "not a QLab workspace: it names no QLab version");
    }

    //  A workspace whose cue lists are missing.
    {
        wfg::test::plist::Keyed outer;
        const auto top = outer.dictionary ({ { outer.add (Value::string ("QLabShortVersionString")),
                                               outer.add (Value::string ("5.6.3")) } });
        const auto bytes = outer.archive (top);
        result = readWorkspaceBytes (bytes.data(), bytes.size());
        CHECK (result.error == "the workspace holds no cue lists");
    }

    CHECK (readWorkspace (juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("no such workspace.qlab5")).error.rfind ("the file could not be read", 0) == 0);
}

TEST_CASE ("qlab reader: the author's own workspaces read whole, when WFG_QLAB_CORPUS names a folder of them")
{
    /*  REAL SHOWS, NOT COMMITTED: a production's notes and patch maps. Every
        `.qlab4` and `.qlab5` under the folder - backups and AppleDouble files
        aside - must read, with a cue list, every cue a type, and every network
        cue's patch one the workspace names. */
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

        const auto result = readWorkspace (file);
        INFO (result.error);
        REQUIRE (result.workspace.has_value());

        const auto& workspace = *result.workspace;
        CHECK_FALSE (workspace.lists.empty());
        CHECK (workspace.problems.empty());

        std::vector<const Cue*> stack;

        for (const auto& list : workspace.lists)
            stack.push_back (&list);

        int cues = 0, unpatched = 0;

        while (! stack.empty())
        {
            const auto* cue = stack.back();
            stack.pop_back();
            ++cues;

            CHECK_FALSE (cue->type.empty());
            CHECK_FALSE (cue->id.empty());

            if (cue->type == "OSC")
            {
                const auto known = std::any_of (workspace.networkPatches.begin(), workspace.networkPatches.end(),
                                                [cue] (const NetworkPatch& patch) { return patch.id == cue->networkPatch; });
                unpatched += known ? 0 : 1;
            }

            for (const auto& child : cue->children)
                stack.push_back (&child);
        }

        MESSAGE (file.getFileName().toStdString() << ": QLab " << workspace.version << ", " << cues << " cues, "
                 << workspace.networkPatches.size() << " network patches, " << unpatched << " network cues unpatched");
    }

    CHECK (workspaces > 0);
}
