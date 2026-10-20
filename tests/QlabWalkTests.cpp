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
#include <array>
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

        w.networkPatches.push_back ({ "S21", "S21", "osc", "192.168.1.221", 8024, false, 0, {} });
        w.networkPatches.push_back ({ "WFS", "WFS", "osc", "192.168.1.32", 8051, false, 0, {} });
        w.networkPatches.push_back ({ "SELF", "localhost", "osc", "localhost", 53000, false, 0, {} });
        w.networkPatches.push_back ({ "TEXT", "Text", "com.figure53.plaintext", "10.0.0.1", 9000, false, 0, {} });
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
    //  Its crossfade is the sequence's join (§69, AOI): no length in the workspace, so two seconds, said.
    CHECK (attribute (items[1], "join") == "crossfade");
    CHECK (attribute (items[1], "joinLength") == "2");
    CHECK (said (plan, "the workspace gave no length, so two seconds"));

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

TEST_CASE ("qlab walk: an unnamed fade aimed at itself, or two at each other, is named and walked (found by tests/fuzz)")
{
    //  Each took its name from its target's, for ever: a stack overflow.
    auto self = cue ("Fade");
    self.target = self.id;

    auto one = cue ("Stop");
    auto two = cue ("Fade");
    one.target = two.id;
    two.target = one.id;

    const auto plan = walk (workspaceOf ({ self, one, two }));
    const auto& items = top (plan);
    REQUIRE (items.size() == 3);

    for (const auto& item : items)
    {
        CHECK_FALSE (item.name.empty());
        CHECK (item.name.size() < 200);
    }
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

    /*  BOTH PATCHES SEND UNDER /wfs, so since §100 (AYK) each device is named
        under a prefix of its own, taken off before a message leaves. */
    CHECK (attribute (items[0], "address") == "/S21/channel/118/mute");
    CHECK (attribute (items[0], "value") == "F");
    CHECK (attribute (items[1], "address") == "/S21/wfs/input/21/positionXYZ");
    CHECK (attribute (items[3], "address") == "/WFS/wfs/input/21/lfo/xyz");
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

    //  Two devices, each under its own name.
    REQUIRE (plan.devices.size() == 2);
    CHECK (plan.devices[0].name == "S21");
    CHECK (plan.devices[0].host == "192.168.1.221");
    CHECK (plan.devices[0].port == 8024);
    CHECK (plan.devices[0].prefix == "/S21");
    CHECK (plan.devices[0].removePrefix);
    CHECK (plan.devices[1].name == "WFS");
    CHECK (plan.devices[1].prefix == "/WFS");
    CHECK_FALSE (said (plan, "two network patches send under /wfs"));
}

TEST_CASE ("qlab walk: devices that share no address keep their roots as prefixes (ZW)")
{
    const auto plan = walk (workspaceOf ({ network ("/channel/118/mute \\F"),
                                           network ("/wfs/input/21/attenuation 0.", "WFS") }));
    const auto& items = top (plan);

    CHECK (attribute (items[0], "address") == "/channel/118/mute");
    REQUIRE (plan.devices.size() == 2);
    CHECK (plan.devices[0].prefix == "/channel");
    CHECK_FALSE (plan.devices[0].removePrefix);
    CHECK (plan.devices[1].prefix == "/wfs");
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
    auto video = cue ("Camera", "Projection");
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

    //  A camera a capture cue since §100, on a video input with no camera chosen; its triggers its own.
    CHECK (items[7].kind == "video");
    CHECK (items[7].name == "Projection");
    CHECK (attribute (items[7], "source") == "capture");
    CHECK (items[7].videoInput == "camera:default");
    CHECK (attribute (items[7], "notes") == "Act 2");
    CHECK (attribute (items[7], "enabled") == "false");
    CHECK (said (plan, "QLab fired it by a key Go.dot has no name for"));
    REQUIRE (items[7].triggers.size() == 1);
    CHECK (items[7].triggers[0].kind == "midi");

    CHECK (items[8].kind == "memo");
    CHECK (attribute (items[8], "preWait") == "4");

    CHECK (plan.placeholders == 2);
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

TEST_CASE ("qlab walk: a playlist's crossfade keeps its length as the sequence's join (§69, AOI)")
{
    auto playlist = group ("Walk-in", 6, { cue ("Memo", "a"), cue ("Memo", "b") });
    playlist.playlistCrossfade = true;
    playlist.playlistCrossfadeDuration = 3.5;

    auto plain = group ("Plain", 6, { cue ("Memo", "c") });

    const auto plan = walk (workspaceOf ({ playlist, plain }));
    const auto& items = top (plan);
    REQUIRE (items.size() == 2);

    CHECK (attribute (items[0], "join") == "crossfade");
    CHECK (attribute (items[0], "joinLength") == "3.5");
    CHECK (said (plan, "at its length"));

    //  A playlist with no crossfade lets its members play, as before.
    CHECK (attribute (items[1], "join").empty());
}

//==============================================================================
/*  QF.8: QLAB'S PICTURES AND WORDS WALKED (namespace draft §62, ALF-ALK):
    stages to canvases, video cues to pictures and movies, text cues to text
    cues, their fades to fades of the picture. Units, signs and numbers the
    probe workspace (§62.6) is to settle are as §62 writes them, provisional. */

namespace
{
    Cue picture (const std::string& file, const std::string& stage)
    {
        auto c = cue ("Video");
        c.file.relativePath = file;
        c.file.absolutePath = "/Volumes/Show/" + file;
        c.stage = stage;
        return c;
    }

    Workspace withStages (std::vector<Cue> top)
    {
        auto w = workspaceOf (std::move (top));
        w.stages = { { "VIEW-1", "Stage 1", 1920, 1080, { "Output 1" }, false, false },
                     { "VIEW-T", "triptyque", 5760, 1080, { "Gauche", "Centre", "Droite" }, true, false } };
        return w;
    }

    std::array<double, 4> aboutTheScreen (double degrees)
    {
        const auto half = degrees * 3.14159265358979323846 / 360.0;
        return { 0.0, 0.0, std::sin (half), std::cos (half) };
    }

    std::string n (double value, int decimals) { return wfg::import::number (value, decimals); }
}

TEST_CASE ("qlab walk: every stage a canvas of its size with its outputs; a picture on its stage, or on a screen's canvas (§62, ALF)")
{
    auto still = picture ("video/black.png", "VIEW-T");
    still.placement.fillStyle = 2;
    auto onScreen = picture ("video/a.png", "");
    onScreen.lastSeenScreen = "Built-in Retina Display";
    onScreen.lastSeenWidth = 3024;
    onScreen.lastSeenHeight = 1964;
    auto lost = picture ("video/b.png", "NOWHERE");

    const auto plan = walk (withStages ({ still, onScreen, lost }));

    REQUIRE (plan.canvases.size() == 4);
    CHECK (plan.canvases[0].key == "stage:VIEW-1");
    CHECK (plan.canvases[0].name == "Stage 1");
    CHECK (plan.canvases[0].width == 1920);
    CHECK (plan.canvases[0].outputs == std::vector<std::string> { "Output 1" });
    CHECK (plan.canvases[1].name == "triptyque");
    CHECK (plan.canvases[1].width == 5760);
    CHECK (plan.canvases[1].outputs.size() == 3u);
    CHECK (plan.canvases[2].key == "screen:Built-in Retina Display");
    CHECK (plan.canvases[2].width == 3024);
    CHECK (plan.canvases[2].height == 1964);
    CHECK (plan.canvases[3].key == "screen:Screen");
    CHECK (plan.canvases[3].width == 1920);

    const auto& items = top (plan);
    CHECK (items[0].kind == "video");
    CHECK (attribute (items[0], "source") == "picture");
    CHECK (items[0].canvas == "stage:VIEW-T");
    CHECK (attribute (items[0], "fit") == "stretch");
    CHECK (items[0].file.relativePath == "video/black.png");
    CHECK (items[1].canvas == "screen:Built-in Retina Display");
    CHECK (items[2].canvas == "screen:Screen");

    CHECK (said (plan, "a canvas of 1920 by 1080 called Screen"));
    CHECK (said (plan, "triptyque's edge blends are not imported"));
    CHECK (said (plan, "each QLab stage is a canvas"));

    //  Each projector once: each shows the stage drawn to it alone, else the first drawn to it.
    REQUIRE (plan.outputs.size() == 6u);
    CHECK (plan.outputs[0].name == "Output 1");
    CHECK (plan.outputs[0].canvas == "stage:VIEW-1");
    CHECK (plan.outputs[1].canvas == "screen:Built-in Retina Display");
    CHECK (plan.outputs[2].canvas == "screen:Screen");
    CHECK (plan.outputs[3].name == "Gauche");
    CHECK (plan.outputs[3].canvas == "stage:VIEW-T");

    SUBCASE ("QLab 4: a cue on no surface the workspace has is on its default surface")
    {
        auto w = workspaceOf ({ picture ("video/c.png", "0") }, 4);
        w.stages = { { "77", "Surface 1", 1680, 1050, { "Color LCD" }, false, false } };
        w.defaultStage = "77";
        const auto four = walk (w);
        REQUIRE (four.canvases.size() == 1);
        CHECK (top (four)[0].canvas == "stage:77");
    }
}

TEST_CASE ("qlab walk: a projector QLab drew several stages to is one output, showing the stage drawn to it alone; the stage spanning it said (§62, ALF)")
{
    auto w = workspaceOf ({ picture ("video/a.png", "VIEW-L") });
    w.stages = { { "VIEW-T", "triptyque", 5760, 1080, { "Gauche", "Centre", "Droite" }, false, false },
                 { "VIEW-L", "gauche", 1920, 1080, { "Gauche" }, false, false },
                 { "VIEW-C", "centre", 1920, 1080, { "Centre" }, false, false } };
    const auto plan = walk (w);

    REQUIRE (plan.outputs.size() == 3u);
    CHECK (plan.outputs[0].name == "Gauche");
    CHECK (plan.outputs[0].canvas == "stage:VIEW-L");
    CHECK (plan.outputs[1].name == "Centre");
    CHECK (plan.outputs[1].canvas == "stage:VIEW-C");
    CHECK (plan.outputs[2].name == "Droite");
    CHECK (plan.outputs[2].canvas == "stage:VIEW-T");
    CHECK (said (plan, "the stage triptyque was drawn to Gauche, Centre, which show another canvas here"));
}

TEST_CASE ("qlab walk: a picture placed - offsets a % of its canvas, a picture at its own size scaled from it, a turn, opacity, layer; what Go.dot has not got said (§62, ALG)")
{
    auto native = picture ("video/tableau.mov", "VIEW-1");
    native.placement.fullScreen = false;
    native.placement.scaleX = native.placement.scaleY = 0.81;
    native.placement.translationX = 10.0;
    native.placement.translationY = 30.0;
    native.placement.opacity = 0.0;
    native.placement.layer = 0.0;

    auto half = native;
    half.id = "Q-half";

    auto unread = native;
    unread.id = "Q-unread";

    auto turned = picture ("video/still.png", "VIEW-1");
    turned.placement.quaternion = aboutTheScreen (30.0);
    turned.placement.layer = 1000.0;
    turned.placement.opacity = 0.5;
    turned.placement.blendMode = 3;
    turned.placement.originX = 12.0;
    turned.placement.cropLeft = 4.0;
    turned.hasEffects = true;

    auto tilted = picture ("video/still.png", "VIEW-1");
    tilted.placement.quaternion = std::array<double, 4> { 0.2, 0.0, 0.0, 0.98 };
    tilted.placement.scaleX = 1.0;
    tilted.placement.scaleY = 0.5;

    WalkOptions options;
    options.pictureSizes[native.id] = { 1920, 1080 };
    options.pictureSizes[half.id] = { 960, 540 };

    const auto plan = walk (withStages ({ native, half, unread, turned, tilted }), options);
    const auto& items = top (plan);

    CHECK (attribute (items[0], "source") == "movie");
    CHECK (attribute (items[0], "offsetX") == n (10.0 / 1920.0 * 100.0, 4));
    CHECK (attribute (items[0], "offsetY") == n (30.0 / 1080.0 * 100.0, 4));
    CHECK (attribute (items[0], "scale") == n (81.0, 4));             // its own size is the canvas's: 0.81 of it
    CHECK (attribute (items[0], "opacity") == "0");
    CHECK (attribute (items[0], "layer").empty());
    CHECK (attribute (items[0], "fit").empty());
    CHECK (attribute (items[1], "scale") == n (40.5, 4));             // half the canvas, fitted to it twice over
    CHECK (attribute (items[2], "scale") == n (81.0, 4));
    CHECK (said (plan, "its own size, which is not known here"));

    CHECK (attribute (items[3], "rotation") == n (-30.0, 3));         // QLab counter-clockwise, Go.dot clockwise
    CHECK (attribute (items[3], "layer") == "999");
    CHECK (attribute (items[3], "opacity") == "50");
    CHECK (said (plan, "QLab's blend mode 3 is drawn normally"));
    CHECK (said (plan, "QLab moved its origin"));
    CHECK (said (plan, "QLab cropped it"));
    CHECK (said (plan, "its video effects are not imported"));

    CHECK (said (plan, "QLab tilted it in depth"));
    CHECK (said (plan, "% across and 50 % up and down"));
}

TEST_CASE ("qlab walk: a movie's timing its Ranges, its held last frame and its sound said (§62, ALH)")
{
    auto movie = picture ("video/CH1.mov", "VIEW-1");
    movie.startTime = 2.5;
    movie.endTime = 30.0;
    movie.playCount = 3;
    movie.holdLastFrame = true;
    movie.rate = 0.5;
    movie.levels = { { 0, 0, gain (-6.0) } };

    const auto plan = walk (withStages ({ movie }));
    const auto& item = top (plan)[0];
    CHECK (attribute (item, "source") == "movie");
    CHECK (attribute (item, "rate") == "0.5");
    REQUIRE (item.ranges.size() == 1u);
    CHECK (item.ranges[0].in == doctest::Approx (2.5));
    CHECK (item.ranges[0].out == doctest::Approx (30.0));
    CHECK (item.ranges[0].loops == 3);
    CHECK (said (plan, "QLab held its last frame"));
    CHECK (said (plan, "its sound played in QLab, at -6 dB"));
}

TEST_CASE ("qlab walk: a text cue - its words, braces doubled; its first run's font found here or read from its name, size, colour, alignment, spacing, width, outline (§62, ALI, ALK)")
{
    auto title = cue ("Titles", "Score");
    title.stage = "VIEW-1";
    title.words = "Score {home}";
    TextRun run;
    run.length = 12;
    run.font = "HelveticaNeue-Bold";
    run.size = 96.0;
    run.colour = { 1.0, 0.5, 0.0, 0.5, true };
    run.alignment = 0;
    run.lineHeightMultiple = 1.5;
    run.strokeWidth = -3.0;
    run.strokeColour = { 0.0, 0.0, 0.0, 1.0, true };
    title.runs = { run };
    title.fixedWidth = 960.0;
    title.lastSeenFontNames = { "Helvetica Neue Bold" };

    auto glow = cue ("Titles", "Glow");
    glow.stage = "VIEW-1";
    glow.words = "Act One, Act Two";
    TextRun arial;
    arial.length = 7;
    arial.font = "Arial-BoldMT";
    arial.size = 48.0;
    arial.colour = { 1.0, 1.0, 1.0, 1.0, true };
    arial.alignment = 2;
    arial.shadow = true;
    arial.shadowBlur = 6.0;
    arial.shadowX = 3.0;
    arial.shadowY = -3.0;
    arial.background = { 0.0, 0.0, 1.0, 1.0, true };
    auto red = arial;
    red.start = 7;
    red.length = 9;
    red.colour = { 1.0, 0.0, 0.0, 1.0, true };
    glow.runs = { arial, red };

    auto four = cue ("Titles", "Four");
    four.stage = "VIEW-1";
    four.words = "Fin";
    TextRun avenir;
    avenir.length = 3;
    avenir.font = "Avenir-HeavyOblique";
    avenir.size = 54.0;
    four.runs = { avenir };

    WalkOptions options;
    options.fontOf = [] (const std::string& postScript) -> std::optional<FontFace>
    {
        if (postScript == "Arial-BoldMT")
            return FontFace { "Arial", true, false };
        return std::nullopt;
    };

    const auto plan = walk (withStages ({ title, glow, four }), options);
    const auto& items = top (plan);

    const auto& t = items[0];
    CHECK (t.kind == "video");
    CHECK (t.canvas == "stage:VIEW-1");
    CHECK (attribute (t, "source") == "text");
    CHECK (attribute (t, "words") == "Score {{home}}");
    CHECK (attribute (t, "font") == "Helvetica Neue");
    CHECK (attribute (t, "bold") == "true");
    CHECK (attribute (t, "italic").empty());
    CHECK (attribute (t, "fontSize") == n (96.0 / 1080.0 * 100.0, 4));
    CHECK (attribute (t, "paint") == "#FF8000");
    CHECK (attribute (t, "opacity") == "50");                         // its colour's alpha
    CHECK (attribute (t, "align") == "left");
    CHECK (attribute (t, "lineSpacing") == "150");
    CHECK (attribute (t, "wrap") == "50");
    CHECK (attribute (t, "outline") == n (3.0 / 100.0 * 96.0 / 2.0, 2));
    CHECK (attribute (t, "outlineColour") == "#000000");
    CHECK (said (plan, "its font HelveticaNeue-Bold is not installed on this machine"));

    const auto& g = items[1];
    CHECK (attribute (g, "font") == "Arial");
    CHECK (attribute (g, "bold") == "true");
    CHECK (attribute (g, "align").empty());                           // centre, Go.dot's own
    CHECK (attribute (g, "wrap") == "0");                             // no fixed width: no box
    CHECK (attribute (g, "outline") == "1");
    CHECK (attribute (g, "outlineBlur") == "6");
    CHECK (said (plan, "its offset of 3, -3 points is not imported"));
    CHECK (said (plan, "its background colour is not imported"));
    CHECK (said (plan, "the first run's look is used for all of them"));

    const auto& f = items[2];
    CHECK (attribute (f, "font") == "Avenir");
    CHECK (attribute (f, "bold") == "true");
    CHECK (attribute (f, "italic") == "true");
    CHECK (attribute (f, "paint") == "#000000");

    REQUIRE (plan.fonts.size() == 3u);
    CHECK (plan.fonts[0].postScript == "HelveticaNeue-Bold");
    CHECK (plan.fonts[0].family == "Helvetica Neue");
    CHECK (plan.fonts[0].where == "missing");
    CHECK (plan.fonts[1].family == "Arial");
    CHECK (plan.fonts[1].where == "installed");
    CHECK (plan.fonts[2].family == "Avenir");
}

TEST_CASE ("qlab walk: a fade of a picture - its opacity, a move, a scale from its own size, a turn; a relative move added; one that moves nothing a memo (§62, ALJ)")
{
    auto movie = picture ("video/CH2.mov", "VIEW-1");
    movie.placement.fullScreen = false;
    movie.placement.scaleX = movie.placement.scaleY = 0.81;
    movie.placement.translationX = 10.0;
    movie.placement.translationY = 30.0;
    movie.placement.opacity = 0.0;

    auto in = cue ("Fade");
    in.target = movie.id;
    in.duration = 4.0;
    in.fadesOpacity = true;
    in.fadesTranslation = true;
    in.fadesScale = true;
    in.fadesRotation = true;
    in.placement.opacity = 1.0;
    in.placement.translationX = 96.0;
    in.placement.translationY = -54.0;
    in.placement.scaleX = 0.5;
    in.placement.quaternion = aboutTheScreen (90.0);
    in.shape.type = 3;
    in.stopTargetWhenDone = true;

    auto nudge = cue ("Fade");
    nudge.target = movie.id;
    nudge.fadesTranslation = true;
    nudge.geoMode = 1;
    nudge.placement.translationX = 86.0;
    nudge.placement.translationY = 24.0;
    nudge.fadeLevels = { { 0, 0, 1.0, gain (-20.0) } };

    auto effects = cue ("Fade");
    effects.target = movie.id;
    effects.fadesEffects = true;

    auto soundOnly = cue ("Fade");
    soundOnly.target = movie.id;
    soundOnly.fadeLevels = { { 0, 0, 1.0, gain (-60.0) } };

    WalkOptions options;
    options.pictureSizes[movie.id] = { 1920, 1080 };

    const auto plan = walk (withStages ({ movie, in, nudge, effects, soundOnly }), options);
    const auto& items = top (plan);

    CHECK (items[1].kind == "fade");
    CHECK (items[1].target == "cue:" + movie.id);
    CHECK (attribute (items[1], "video") == "offsetX:" + n (5.0, 4) + " offsetY:" + n (-5.0, 4) + " opacity:100 rotation:"
                                             + n (-90.0, 3) + " scale:50");
    CHECK (attribute (items[1], "stopWhenDone") == "true");
    CHECK (attribute (items[1], "curve").empty());                     // linear
    CHECK (attribute (items[1], "duration") == "4");

    //  Relative: added to where the movie was put, 10 and 30.
    CHECK (attribute (items[2], "video") == "offsetX:" + n (5.0, 4) + " offsetY:" + n (5.0, 4));
    CHECK (attribute (items[2], "curve") == "sCurve");
    CHECK (said (plan, "a relative move in QLab"));
    CHECK (said (plan, "it fades the movie's sound too"));

    CHECK (items[3].kind == "memo");
    CHECK (said (plan, "it moves nothing a Go.dot fade of a picture moves"));
    CHECK (items[4].kind == "memo");
    CHECK (said (plan, "it fades only the movie's sound"));
}

namespace
{
    Cue midiCue (const std::string& name, int status, int byte1, int byte2)
    {
        auto c = cue ("MIDI", name);
        c.midi.patch = "PATCH-MIDI";
        c.midi.status = status;
        c.midi.channel = 2;
        c.midi.byte1 = byte1;
        c.midi.byte2 = byte2;
        return c;
    }

    Cue rawMidi (const std::string& name, const std::string& raw)
    {
        auto c = cue ("MIDI", name);
        c.midi.messageType = 3;
        c.midi.raw = raw;
        return c;
    }

    bool has (const Trigger& trigger, const std::string& row, const std::string& value)
    {
        return std::find (trigger.attributes.begin(), trigger.attributes.end(),
                          std::pair<std::string, std::string> { row, value }) != trigger.attributes.end();
    }
}

TEST_CASE ("qlab walk: MIDI cues - each voice type, a fade, MIDI Show Control, raw bytes; a port per patch (§100)")
{
    auto fader = midiCue ("Elsa fader", 3, 14, 0);
    fader.midi.fade = true;
    fader.midi.endValue = 102;
    fader.duration = 0.9;
    fader.shape.type = 0;           // as Hécube's fades are stored

    auto linear = fader;
    linear.id = "Q-linear";
    linear.name = "linear";
    linear.shape.type = 3;

    auto bend = midiCue ("Bend", 6, 0, 0);
    bend.midi.byteCombo = 12000;

    auto msc = cue ("MIDI", "Lights GO");
    msc.midi.messageType = 2;
    msc.midi.deviceId = 1;
    msc.midi.commandFormat = 1;
    msc.midi.command = 1;
    msc.midi.qNumber = "12.5";
    msc.midi.qList = "2";

    auto w = workspaceOf ({ midiCue ("Elsa off", 3, 1, 0), fader, linear, midiCue ("Note", 1, 60, 100),
                            midiCue ("Program", 4, 12, 0), midiCue ("Pressure", 5, 90, 0), bend, msc,
                            rawMidi ("Mute", "90 20 7F"), rawMidi ("Two", "B0 07 64 0A 40"),
                            rawMidi ("Sysex", "F0 7E 7F 06 01 F7"), rawMidi ("Broken", "90 2X") });
    w.midiPatches.push_back ({ "PATCH-MIDI", "Desk MIDI", "USB MIDI Interface" });

    const auto plan = walk (w);
    const auto& items = top (plan);

    CHECK (items[0].kind == "midi");
    CHECK (attribute (items[0], "type") == "controlChange");
    CHECK (attribute (items[0], "channel") == "2");
    CHECK (attribute (items[0], "data1") == "1");
    CHECK (attribute (items[0], "data2").empty());
    CHECK (attribute (items[0], "fadeTo").empty());
    CHECK (items[0].port == "midi:PATCH-MIDI");

    CHECK (attribute (items[1], "fadeTo") == "102");
    CHECK (attribute (items[1], "fadeTime") == "0.9");
    CHECK (attribute (items[1], "fadeCurve") == "sCurve");
    CHECK (said (plan, "fade shape taken as Go.dot's S-curve"));
    CHECK (attribute (items[2], "fadeCurve").empty());

    CHECK (attribute (items[3], "type") == "noteOn");
    CHECK (attribute (items[3], "data1") == "60");
    CHECK (attribute (items[3], "data2") == "100");
    CHECK (attribute (items[4], "type") == "programChange");
    CHECK (attribute (items[4], "data1") == "12");
    CHECK (attribute (items[5], "type") == "channelPressure");
    CHECK (attribute (items[5], "data2") == "90");
    CHECK (attribute (items[6], "type") == "pitchBend");
    CHECK (attribute (items[6], "data2") == "12000");

    CHECK (attribute (items[7], "type") == "sysex");
    CHECK (attribute (items[7], "sysex") == "F0 7F 01 02 01 01 31 32 2E 35 00 32 F7");

    CHECK (attribute (items[8], "type") == "noteOn");
    CHECK (attribute (items[8], "channel") == "1");
    CHECK (attribute (items[8], "data1") == "32");
    CHECK (attribute (items[8], "data2") == "127");

    //  Two messages, running status: a timeline of two MIDI cues.
    CHECK (items[9].kind == "group");
    CHECK (attribute (items[9], "mode") == "timeline");
    REQUIRE (items[9].children.size() == 2);
    CHECK (attribute (items[9].children[1], "data1") == "10");
    CHECK (attribute (items[9].children[1], "data2") == "64");

    CHECK (attribute (items[10], "type") == "sysex");
    CHECK (attribute (items[10], "sysex") == "F0 7E 7F 06 01 F7");

    CHECK (items[11].kind == "memo");
    CHECK (said (plan, "its bytes are not hex Go.dot can read"));

    //  One port for the patch the voice cues name, one for the cues that name none.
    REQUIRE (plan.ports.size() == 2);
    CHECK (plan.ports[0].name == "Desk MIDI");
    CHECK (plan.ports[0].device == "USB MIDI Interface");
    CHECK (plan.ports[1].key == "midi:default");
}

TEST_CASE ("qlab walk: a hotkey, a MIDI trigger and a wall clock become Go.dot's triggers; what cannot is said (§100)")
{
    auto keyed = cue ("Memo", "Keyed");
    keyed.hotkeyTrigger = true;
    keyed.hotkey.character = 'e';
    keyed.hotkey.keyCode = 14;
    keyed.hotkey.control = true;

    auto kept = cue ("Memo", "Kept");
    kept.hotkeyTrigger = true;
    kept.hotkey.character = 'z';
    kept.hotkey.command = true;

    auto function = cue ("Memo", "F6");
    function.hotkeyTrigger = true;
    function.hotkey.character = 0xF709;
    function.hotkey.keyCode = 97;
    function.hotkey.function = true;

    auto heard = cue ("Memo", "Heard");
    heard.midiTrigger = true;
    heard.midiTriggerSpec = { 144, 0, 112, 127, 0, 0 };

    auto anyVelocity = cue ("Memo", "Any velocity");
    anyVelocity.midiTrigger = true;
    anyVelocity.midiTriggerSpec = { 0x91, 2, 60, 0, 0, 0 };

    //  P.U.N.K's pad: a hit above 20, a release below it; and a comparison not read.
    auto hit = cue ("Memo", "Hit");
    hit.midiTrigger = true;
    hit.midiTriggerSpec = { 144, 0, 32, 20, 0, 3 };
    auto release = cue ("Memo", "Release");
    release.midiTrigger = true;
    release.midiTriggerSpec = { 144, 0, 32, 20, 0, 2 };
    auto unread = cue ("Memo", "Unread");
    unread.midiTrigger = true;
    unread.midiTriggerSpec = { 144, 0, 33, 20, 0, 1 };

    auto clock = cue ("Memo", "Clock");
    clock.wallClockTrigger = true;
    clock.wallHours = 19;
    clock.wallMinutes = 30;
    clock.wallSeconds = 5;
    clock.wallDays = 0x7E;

    auto w = workspaceOf ({ keyed, kept, function, heard, anyVelocity, clock, hit, release, unread });
    w.lists[0].midiTrigger = true;

    const auto plan = walk (w);
    const auto& items = top (plan);

    REQUIRE (items[0].triggers.size() == 1);
    CHECK (items[0].triggers[0].kind == "key");
    CHECK (has (items[0].triggers[0], "key", "Mod+E"));
    CHECK (said (plan, "Control is Go.dot's Mod"));

    CHECK (items[1].triggers.empty());
    CHECK (said (plan, "QLab fired it by Mod+Z, a key Go.dot keeps for itself"));

    REQUIRE (items[2].triggers.size() == 1);
    CHECK (has (items[2].triggers[0], "key", "F6"));

    REQUIRE (items[3].triggers.size() == 1);
    CHECK (items[3].triggers[0].kind == "midi");
    CHECK (has (items[3].triggers[0], "type", "noteOn"));
    CHECK (has (items[3].triggers[0], "number", "112"));
    CHECK (has (items[3].triggers[0], "data", "127"));

    //  A note on of velocity nought is any velocity; the channel kept.
    REQUIRE (items[4].triggers.size() == 1);
    CHECK (has (items[4].triggers[0], "data", "-1"));
    CHECK (has (items[4].triggers[0], "channel", "2"));

    REQUIRE (items[5].triggers.size() == 1);
    CHECK (items[5].triggers[0].kind == "clock");
    CHECK (has (items[5].triggers[0], "at", "19:30:05"));
    CHECK (said (plan, "on some days of the week only"));

    CHECK (said (plan, "QLab fired the cue list itself by a trigger"));

    REQUIRE (items[6].triggers.size() == 1);
    CHECK (has (items[6].triggers[0], "data", "21"));
    CHECK (has (items[6].triggers[0], "compare", "atLeast"));
    REQUIRE (items[7].triggers.size() == 1);
    CHECK (has (items[7].triggers[0], "data", "19"));
    CHECK (has (items[7].triggers[0], "compare", "atMost"));
    REQUIRE (items[8].triggers.size() == 1);
    CHECK (has (items[8].triggers[0], "data", "-1"));
    CHECK (said (plan, "compared its value in a way Go.dot does not read (1)"));
}

TEST_CASE ("qlab walk: an Eos and QLab machines are OSC devices; each QLab named under its own prefix (§100, AYK)")
{
    auto w = workspaceOf ({ network ("/eos/cue/11/fire", "EOS"), network ("/go", "MAC1"), network ("/go", "MAC2"),
                            network ("/select/1", "MAC1"), network ("/channel/1/fader 0.") });
    w.networkPatches.push_back ({ "EOS", "EOS 5 CF", "com.etc.eos.2", "192.168.6.101", 5000, false, 1, {} });
    w.networkPatches.push_back ({ "MAC1", "MacMini_Justice1", "com.figure53.qlab.5", "192.168.1.211", 53000, false, 0, "2001" });
    w.networkPatches.push_back ({ "MAC2", "MacMini Justice 2", "com.figure53.qlab.5", "192.168.1.212", 53000, false, 0, {} });

    const auto plan = walk (w);
    const auto& items = top (plan);

    CHECK (items[0].kind == "osc");
    CHECK (attribute (items[0], "address") == "/eos/cue/11/fire");
    CHECK (attribute (items[1], "address") == "/MacMini_Justice1/go");
    CHECK (attribute (items[2], "address") == "/MacMini_Justice_2/go");
    CHECK (attribute (items[3], "address") == "/MacMini_Justice1/select/1");
    CHECK (attribute (items[4], "address") == "/channel/1/fader");

    REQUIRE (plan.devices.size() == 4);
    CHECK (plan.devices[0].prefix == "/eos");
    CHECK_FALSE (plan.devices[0].removePrefix);
    CHECK (plan.devices[1].prefix == "/MacMini_Justice1");
    CHECK (plan.devices[1].removePrefix);
    CHECK (plan.devices[1].port == 53000);
    CHECK (plan.devices[2].prefix == "/MacMini_Justice_2");

    CHECK (said (plan, "QLab sent the patch EOS 5 CF to 2 destinations"));
    CHECK (said (plan, "with its passcode"));
    CHECK (plan.placeholders == 0);
}

TEST_CASE ("qlab walk: a cart is a Go.dot cart (§100, AYM)")
{
    auto w = workspaceOf ({ cue ("Memo", "show") });
    w.lists.push_back (group ("Pads", 5, { cue ("Memo", "pad") }));

    const auto plan = walk (w);
    REQUIRE (plan.lists.size() == 2);
    CHECK_FALSE (plan.lists[0].cart);
    CHECK (plan.lists[1].cart);
    CHECK (said (plan, "a cart: a Go.dot cart"));
}

TEST_CASE ("qlab walk: a hotkey spelled, MSC's bytes, raw hex read (§100)")
{
    HotKey key;
    key.character = 'a';
    key.option = true;
    key.shift = true;
    CHECK (keyFor (key) == "Alt+Shift+A");

    key = {};
    key.character = '7';
    key.numericPad = true;
    CHECK (keyFor (key) == "Num7");

    key = {};
    key.keyCode = 123;
    CHECK (keyFor (key) == "ArrowLeft");

    key = {};
    key.character = '#';
    CHECK (keyFor (key).empty());

    MidiMessage m;
    m.command = 8;                  // ALL_OFF: no data
    m.deviceId = 127;
    m.commandFormat = 16;
    CHECK (mscBytes (m) == "F0 7F 7F 02 10 08 F7");

    m.command = 6;                  // SET control 300 to 1000
    m.controlNumber = 300;
    m.controlValue = 1000;
    CHECK (mscBytes (m) == "F0 7F 7F 02 10 06 2C 02 68 07 F7");

    CHECK (hexBytes ("90 20 7f") == std::vector<int> { 0x90, 0x20, 0x7F });
    CHECK (hexBytes ("f0,7e") == std::vector<int> { 0xF0, 0x7E });
    CHECK_FALSE (hexBytes ("123").has_value());
    CHECK_FALSE (hexBytes ("G0").has_value());
}

TEST_CASE ("qlab walk: a devamp that fires the next cue as its loop ends is an advance with thenNext (§100, AYN)")
{
    auto bed = cue ("Audio", "Bed");
    auto devamp = cue ("Devamp");
    devamp.target = bed.id;
    devamp.fireNextAtLoopEnd = true;
    devamp.stopTargetWhenDone = true;
    auto plain = cue ("Devamp");
    plain.target = bed.id;

    const auto plan = walk (workspaceOf ({ bed, devamp, plain }));
    const auto& items = top (plan);

    CHECK (attribute (items[1], "verb") == "advance");
    CHECK (attribute (items[1], "thenNext") == "true");
    CHECK (attribute (items[2], "thenNext").empty());
    CHECK (said (plan, "QLab stopped its target as the loop ended"));
}
