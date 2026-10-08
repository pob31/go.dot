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
    The QLab walk (namespace draft §46.2, ZS-ZX): one rule a case, each on a
    workspace written out as facts - a group's mode, a chain somebody set, a
    sound's matrix and ranges, a fade absolute or relative, a message and its
    device, a stop, a kind with no equivalent - and what the plan says of it,
    including what the report is told.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/import/QlabWalk.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace wfg::import;
using namespace wfg::import::qlab;

namespace
{
    int ids = 0;

    Cue cue (const std::string& type, const std::string& name = {})
    {
        Cue c;
        c.type = type;
        c.id = "Q-" + std::to_string (++ids);
        c.name = name;
        return c;
    }

    Cue group (const std::string& name, int mode, std::vector<Cue> children)
    {
        auto g = cue ("Group", name);
        g.groupMode = mode;
        g.children = std::move (children);
        return g;
    }

    Cue network (const std::string& message, const std::string& patch = "S21")
    {
        auto c = cue ("OSC");
        c.message = message;
        c.networkPatch = patch;
        return c;
    }

    double gain (double db) { return std::pow (10.0, db / 20.0); }

    Workspace workspaceOf (std::vector<Cue> top, int major = 5)
    {
        Workspace w;
        w.major = major;
        w.minVolume = -80.0;

        auto list = group ("Main", 0, std::move (top));
        w.lists.push_back (std::move (list));

        AudioPatch audio;
        audio.id = "A";
        audio.name = "Desk";
        audio.outputs = 4;
        audio.outputNames = { { 1, "FOH L" }, { 2, "FOH R" }, { 3, "Mon 1" } };
        w.audioPatches.push_back (audio);

        w.networkPatches.push_back ({ "S21", "S21", "osc", "192.168.1.221", 8024, false });
        w.networkPatches.push_back ({ "WFS", "WFS", "osc", "192.168.1.32", 8051, false });
        w.networkPatches.push_back ({ "SELF", "localhost", "osc", "localhost", 53000, false });
        w.networkPatches.push_back ({ "TEXT", "Text", "com.figure53.plaintext", "10.0.0.1", 9000, false });
        return w;
    }

    std::string attribute (const Item& item, const std::string& name)
    {
        for (const auto& [row, value] : item.attributes)
            if (row == name)
                return value;

        return {};
    }

    bool said (const Plan& plan, const std::string& words)
    {
        return std::any_of (plan.notes.begin(), plan.notes.end(),
                            [&words] (const Note& note) { return note.text.find (words) != std::string::npos; });
    }

    const std::vector<Item>& top (const Plan& plan)
    {
        REQUIRE (plan.lists.size() == 1);
        return plan.lists[0].items;
    }
}

TEST_CASE ("qlab walk: a group's mode is the decision (ZS)")
{
    auto enter = group ("1 - Doors", 1, { cue ("Memo", "a"), cue ("Memo", "b") });
    auto playlist = group ("Doors open", 6, { cue ("Memo", "c"), cue ("Memo", "d") });
    playlist.playlistLoop = true;
    playlist.playlistShuffle = true;
    playlist.playlistCrossfade = true;
    auto timeline = group ("Machinery", 3, { cue ("Memo", "e") });
    auto random = group ("Any", 4, { cue ("Memo", "f"), cue ("Memo", "g") });
    auto firstOfOne = group ("Reverb", 2, { cue ("Memo", "h") });
    auto firstOfTwo = group ("THE END", 2, { cue ("Memo", "/!\\ REALLY ? /!\\"), group ("Mute all", 6, {}) });

    const auto plan = walk (workspaceOf ({ enter, playlist, timeline, random, firstOfOne, firstOfTwo }));
    const auto& items = top (plan);
    REQUIRE (items.size() == 6);

    CHECK (attribute (items[0], "mode") == "sequence");
    CHECK (attribute (items[0], "advance") == "manual");

    CHECK (attribute (items[1], "advance") == "auto");
    CHECK (attribute (items[1], "loops") == "0");
    CHECK (attribute (items[1], "selection") == "shuffle");
    CHECK (said (plan, "the playlist's crossfade is not imported"));

    CHECK (attribute (items[2], "mode") == "timeline");

    CHECK (attribute (items[3], "selection") == "shuffle");
    CHECK (attribute (items[3], "play") == "1");

    CHECK (attribute (items[4], "advance") == "auto");

    //  The warning before the mutes stays a warning: a GO between them.
    CHECK (attribute (items[5], "advance") == "manual");
    CHECK (said (plan, "start first with several members"));

    CHECK (plan.cues == 6 + 2 + 2 + 1 + 2 + 1 + 2);
}

TEST_CASE ("qlab walk: a chain somebody set becomes a group of its own (ZS)")
{
    SUBCASE ("auto-follows: an automatic sequence, one GO for the chain")
    {
        auto a = cue ("Memo", "one"), b = cue ("Memo", "two"), c = cue ("Memo", "three"), d = cue ("Memo", "alone");
        a.continueMode = 2;
        b.continueMode = 2;
        const auto plan = walk (workspaceOf ({ group ("Preset", 1, { a, b, c, d }) }), {});

        const auto& scene = top (plan)[0];
        REQUIRE (scene.children.size() == 2);
        CHECK (scene.children[0].key == "chain:" + a.id);
        CHECK (scene.children[0].name == "Chain from one");
        CHECK (attribute (scene.children[0], "advance") == "auto");
        CHECK (scene.children[0].children.size() == 3);
        CHECK (scene.children[1].name == "alone");
        CHECK (said (plan, "a chain of auto-follows"));
    }

    SUBCASE ("auto-continues: a timeline whose offsets are the waits")
    {
        auto a = cue ("Memo", "one"), b = cue ("Memo", "two"), c = cue ("Memo", "three");
        a.continueMode = 1;
        a.postWait = 2.0;
        b.continueMode = 1;
        b.preWait = 0.5;
        b.postWait = 1.0;
        const auto plan = walk (workspaceOf ({ a, b, c }));

        const auto& chained = top (plan)[0];
        CHECK (attribute (chained, "mode") == "timeline");
        REQUIRE (chained.children.size() == 3);
        CHECK (attribute (chained.children[0], "preWait").empty());
        CHECK (attribute (chained.children[1], "preWait") == "2.5");
        CHECK (attribute (chained.children[2], "preWait") == "3.5");
    }

    SUBCASE ("in a playlist, QLab's own: no chain")
    {
        auto a = cue ("Memo", "one");
        a.continueMode = 1;
        const auto plan = walk (workspaceOf ({ group ("Doors", 6, { a, cue ("Memo", "two") }) }));
        CHECK (top (plan)[0].children.size() == 2);
    }
}

TEST_CASE ("qlab walk: a sound - its level, speed, ranges and routes (ZT, ZU)")
{
    auto sound = cue ("Audio");
    sound.file.relativePath = "audio/ambiances_19.WAV";
    sound.audioPatch = "A";
    sound.rate = 0.55;
    sound.pitchFollowsRate = false;
    sound.startTime = 22.0;
    sound.endTime = 595.25;
    sound.fileDuration = 600.0;
    sound.infiniteLoop = true;
    sound.levels = { { 0, 0, gain (-6.0) }, { 0, 1, 1.0 }, { 0, 2, gain (-6.0) }, { 0, 3, 1.0 },
                     { 1, 0, 1.0 }, { 1, 1, 1.0 }, { 2, 0, 1.0 }, { 2, 2, 1.0 },
                     { 3, 3, 1.0 } };    // a row QLab keeps for an input the file does not have

    WalkOptions options;
    options.channels[sound.id] = 2;
    const auto plan = walk (workspaceOf ({ sound }), options);
    const auto& item = top (plan)[0];

    CHECK (item.kind == "media");
    CHECK (item.name == "ambiances_19.WAV");
    CHECK (attribute (item, "level") == "-6");
    CHECK (attribute (item, "rate") == "0.55");
    CHECK (attribute (item, "rateMode") == "timestretch");

    REQUIRE (item.ranges.size() == 1);
    CHECK (item.ranges[0].in == doctest::Approx (22.0));
    CHECK (item.ranges[0].out == doctest::Approx (595.25));
    CHECK (item.ranges[0].loops == 0);

    //  Two routes - input 1 to output 1, input 2 to output 2 at -6 - and none
    //  from row 3, which is not one of this file's inputs.
    REQUIRE (item.routes.size() == 2);
    CHECK (item.routes[0].bus == "out:A:1");
    CHECK (item.routes[0].gains == std::vector<double> { 1.0, 0.0 });
    CHECK (item.routes[1].bus == "out:A:2");
    CHECK (item.routes[1].gains[1] == doctest::Approx (gain (-6.0)).epsilon (1e-5));

    REQUIRE (plan.buses.size() == 2);
    CHECK (plan.buses[0].name == "FOH L");
    CHECK (plan.buses[0].channel == 0);
    CHECK (plan.buses[1].name == "FOH R");
    CHECK (plan.buses[1].channel == 1);
}

TEST_CASE ("qlab walk: a sound's slices, an untouched matrix, a file not found")
{
    auto sliced = cue ("Audio", "Siren");
    sliced.startTime = 0.7;
    sliced.endTime = 22.7;
    sliced.slices = { { 20.2, 3, false }, { 2.7, 1, false } };
    sliced.lastSlice = { 0.0, 1, true };
    sliced.levels = { { 0, 0, 1.0 } };

    auto missing = cue ("Audio", "Gone");
    missing.levels = { { 1, 1, 1.0 } };

    WalkOptions options;
    options.channels[sliced.id] = 1;
    const auto plan = walk (workspaceOf ({ sliced, missing }), options);

    const auto& siren = top (plan)[0];
    REQUIRE (siren.ranges.size() == 3);
    CHECK (siren.ranges[0].in == doctest::Approx (0.7));
    CHECK (siren.ranges[0].out == doctest::Approx (2.7));
    CHECK (siren.ranges[1].out == doctest::Approx (20.2));
    CHECK (siren.ranges[1].loops == 3);
    CHECK (siren.ranges[2].out == doctest::Approx (22.7));
    CHECK (siren.ranges[2].loops == 0);

    CHECK (siren.defaultRoute);
    CHECK (said (plan, "Go.dot's default route"));

    CHECK (top (plan)[1].routes.empty());
    CHECK_FALSE (top (plan)[1].defaultRoute);
    CHECK (said (plan, "its file was not found"));
}

TEST_CASE ("qlab walk: fades - absolute, relative on a group as its trim, relative on a cue a memo (ZV)")
{
    auto sound = cue ("Audio", "Ambience");
    sound.levels = { { 0, 0, 0.0 } };

    auto in = cue ("Fade");
    in.target = sound.id;
    in.duration = 1.0;
    in.fadeLevels = { { 0, 0, 1.0e-4, gain (-30.0) } };

    auto linear = in;
    linear.id = "Q-linear";
    linear.shape.type = 3;

    auto machinery = group ("Machinery", 3, { sound, in, linear });

    const auto relative = [&machinery] (const std::string& id, double db, bool stop)
    {
        auto f = cue ("Fade");
        f.id = id;
        f.target = machinery.id;
        f.absolute = false;
        f.duration = 3.0;
        f.stopTargetWhenDone = stop;
        f.fadeLevels = { { 0, 0, 1.0, db <= -150.0 ? 0.0 : gain (db) } };
        f.shape.type = 2;
        f.shape.parameter = 0.45;
        return f;
    };

    auto onCue = cue ("Fade");
    onCue.target = sound.id;
    onCue.absolute = false;
    onCue.fadeLevels = { { 0, 0, 1.0, gain (-2.0) } };

    const auto plan = walk (workspaceOf ({ machinery, relative ("R1", -9.0, false), relative ("R2", 3.0, false),
                                           relative ("R3", -200.0, true), onCue }));
    const auto& items = top (plan);

    const auto& fadeIn = items[0].children[1];
    CHECK (fadeIn.kind == "fade");
    CHECK (fadeIn.target == "cue:" + sound.id);
    CHECK (fadeIn.name == "fade Ambience");
    CHECK (attribute (fadeIn, "level") == "-30");
    CHECK (attribute (fadeIn, "curve") == "sCurve");
    CHECK (attribute (items[0].children[2], "curve").empty());

    CHECK (attribute (items[1], "level") == "-9");
    CHECK (attribute (items[2], "level") == "-6");
    CHECK (attribute (items[3], "level") == "-120");
    CHECK (attribute (items[3], "stopWhenDone") == "true");
    CHECK (said (plan, "summed in show order"));
    CHECK (said (plan, "parametric shape"));

    CHECK (items[4].kind == "memo");
    CHECK (items[4].name.rfind ("[QLab] ", 0) == 0);
    CHECK (said (plan, "a relative fade on one cue"));
    CHECK (plan.placeholders == 1);
}

TEST_CASE ("qlab walk: messages - atoms, devices, a #v# fade as a curve (ZW)")
{
    auto fader = network ("/channel/114/fader #v#");
    fader.networkFadeType = 1;
    fader.fadeFrom = -18.0;
    fader.fadeTo = 0.0;
    fader.duration = 5.0;
    fader.shape.type = 3;

    auto eased = fader;
    eased.id = "Q-eased";
    eased.shape.type = 1;

    const auto plan = walk (workspaceOf ({
        network ("/channel/118/mute \\F"),
        network ("/wfs/input/21/positionXYZ -3.0 1.0 3."),
        network ("/channel/114/fader 0."),
        network ("/wfs/input/21/lfo/xyz 0 0 90 1 8 1 5.0 1.0 3.0", "WFS"),
        fader,
        eased,
        network ("/cue/1/start", "SELF"),
        network ("hello", "TEXT"),
        network ("/no/patch", "NOPE") }));

    const auto& items = top (plan);
    CHECK (attribute (items[0], "address") == "/channel/118/mute");
    CHECK (attribute (items[0], "value") == "F");
    CHECK (attribute (items[1], "value") == "f:-3 f:1 f:3");
    CHECK (attribute (items[2], "value") == "f:0");
    CHECK (attribute (items[3], "value") == "i:0 i:0 i:90 i:1 i:8 i:1 f:5 f:1 f:3");

    CHECK (attribute (items[4], "value") == "f:-18");
    CHECK (attribute (items[4], "duration") == "5");
    REQUIRE (items[4].curves.size() == 1);
    CHECK (items[4].curves[0].arg == 0);
    REQUIRE (items[4].curves[0].points.size() == 2);
    CHECK (items[4].curves[0].points[1].first == doctest::Approx (5.0));
    CHECK (items[4].curves[0].points[1].second == doctest::Approx (0.0));

    CHECK (items[5].curves[0].points.size() == 9);
    CHECK (said (plan, "QLab's S-curve, sampled at nine points"));

    CHECK (items[6].kind == "memo");
    CHECK (said (plan, "it is sent to QLab itself"));
    CHECK (items[7].kind == "memo");
    CHECK (items[8].kind == "memo");
    CHECK (plan.placeholders == 3);

    //  Two devices, each with the first segments its cues send under.
    REQUIRE (plan.devices.size() == 2);
    CHECK (plan.devices[0].name == "S21");
    CHECK (plan.devices[0].host == "192.168.1.221");
    CHECK (plan.devices[0].port == 8024);
    CHECK (plan.devices[0].prefix == "/channel /wfs");
    CHECK (plan.devices[1].name == "WFS");
    CHECK (said (plan, "two network patches send under /wfs"));
}

TEST_CASE ("qlab walk: QLab 4's commands and UDP messages are memos, its OSC messages cues")
{
    auto command = network ("/cue/5/start", "S21");
    command.messageType = 1;
    auto udp = network ("hello", "S21");
    udp.messageType = 3;

    const auto plan = walk (workspaceOf ({ command, udp, network ("/channel/1/mute \\T") }, 4));
    CHECK (top (plan)[0].kind == "memo");
    CHECK (said (plan, "a QLab command"));
    CHECK (top (plan)[1].kind == "memo");
    CHECK (top (plan)[2].kind == "osc");
}

TEST_CASE ("qlab walk: start, stop, goto, arm and the kinds with no equivalent (ZX, ZP)")
{
    auto memo = cue ("Memo", "Target");
    auto start = cue ("Start");
    start.target = memo.id;
    auto stop = cue ("Stop");
    stop.target = memo.id;
    auto go = cue ("Goto");
    go.target = memo.id;
    auto arm = cue ("Arm");
    arm.target = memo.id;
    auto lost = cue ("Stop", "aimed at nothing");
    lost.target = "nowhere";
    auto script = cue ("Script", "a script");
    script.source = "tell application \"QLab\"";
    auto video = cue ("Video", "Projection");
    video.notes = "Act 2";
    video.hotkeyTrigger = true;
    video.midiTrigger = true;
    video.armed = false;
    auto wait = cue ("Wait");
    wait.duration = 4.0;

    const auto plan = walk (workspaceOf ({ memo, start, stop, go, arm, lost, script, video, wait }));
    const auto& items = top (plan);

    CHECK (items[1].kind == "start");
    CHECK (items[1].target == "cue:" + memo.id);
    CHECK (items[1].name == "start Target");
    CHECK (attribute (items[2], "verb") == "hard");
    CHECK (attribute (items[3], "verb") == "jump");
    CHECK (attribute (items[4], "verb") == "enable");
    CHECK (said (plan, "lasts for this run of the show"));

    CHECK (items[5].kind == "memo");
    CHECK (said (plan, "its target is not imported"));

    CHECK (items[6].kind == "memo");
    CHECK (attribute (items[6], "notes").find ("tell application") != std::string::npos);
    CHECK (said (plan, "a script is never run"));

    CHECK (items[7].name == "[QLab] Projection");
    CHECK (attribute (items[7], "notes").find ("Go.dot has no Video cue") != std::string::npos);
    CHECK (attribute (items[7], "notes").find ("Act 2") != std::string::npos);
    CHECK (attribute (items[7], "enabled") == "false");
    CHECK (said (plan, "QLab fired it by a hotkey and a MIDI trigger"));

    CHECK (items[8].kind == "memo");
    CHECK (attribute (items[8], "preWait") == "4");

    CHECK (plan.placeholders == 3);
}

TEST_CASE ("qlab walk: a message split and typed as QLab spells it")
{
    CHECK (splitMessage ("/a/b 1 \"two words\" 3.") == std::vector<std::string> { "/a/b", "1", "\"two words\"", "3." });
    CHECK (splitMessage ("  /a   b ") == std::vector<std::string> { "/a", "b" });

    CHECK (atomFor ("\\T") == "T");
    CHECK (atomFor ("\\F") == "F");
    CHECK (atomFor ("\\I") == "I");
    CHECK (atomFor ("\\N") == "N");
    CHECK (atomFor ("21") == "i:21");
    CHECK (atomFor ("-3") == "i:-3");
    CHECK (atomFor ("8000000000") == "h:8000000000");
    CHECK (atomFor ("0.") == "f:0");
    CHECK (atomFor ("-.5") == "f:-0.5");
    CHECK (atomFor ("-18.") == "f:-18");
    CHECK (atomFor ("left") == "s:\"left\"");
    CHECK (atomFor ("\"left wing\"") == "s:\"left wing\"");
    CHECK (atomFor ("1.2.3") == "s:\"1.2.3\"");

    CHECK (decibels (0.0, -80.0) == doctest::Approx (-120.0));
    CHECK (decibels (gain (-81.0), -80.0) == doctest::Approx (-120.0));
    CHECK (decibels (gain (-30.0), -80.0) == doctest::Approx (-30.0));
    CHECK (decibels (gain (20.0), -80.0) == doctest::Approx (12.0));
}
