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
    The walk (namespace draft §29.2, QH-QJ): a Live set replayed scene by
    scene and each sound flattened onto one cue.

    THE SETS HERE ARE BUILT IN CODE, one rule each, so a case reads as the
    rule it pins: a scene is a GO, a new clip stops the old one, a start before
    the file is a pre-wait, a fader's envelope is a level lane, a carrier with
    its sound folds into it and one without is a fade, a fader a hand rode is
    a DCA. The laws are the defaults (QU) - straight in decibels with Live's
    -70 floor - which is what the numbers below are worked out with.

    One case reads the hand-written session fixture end to end, and one walks
    the author's real sets when WFG_ALS_CORPUS names them, printing every GO.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/import/AlsReader.h>
#include <wfg/engine/import/AlsWalk.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <string>
#include <tuple>
#include <vector>

using namespace wfg;
using namespace wfg::import::als;

namespace
{
    constexpr double floorGain = 0.0003162277571;   // Live's bottom of the fader, -70 dB

    /*  A clip of a ten-second file at its own rate, at 120 BPM: two beats a
        second, beat nought the file's start. */
    Clip clipOf (const std::string& name, double fromBeat, double toBeat)
    {
        Clip clip;
        clip.name = name;
        clip.file.relativePath = "Samples/Imported/" + name + ".wav";
        clip.file.name = name + ".wav";
        clip.file.sampleRate = 48000.0;
        clip.file.frames = 480000;
        clip.loopStart = fromBeat;
        clip.loopEnd = toBeat;
        clip.warp = { { 0.0, 0.0 }, { 0.015625, 0.03125 } };
        return clip;
    }

    Envelope envelope (const std::string& trackId, Parameter what, std::vector<EnvelopePoint> points,
                       int send = -1)
    {
        Envelope out;
        out.target.trackId = trackId;
        out.target.what = what;
        out.target.sendIndex = send;
        out.points.push_back ({ -63072000.0, points.front().value });
        out.points.insert (out.points.end(), points.begin(), points.end());
        return out;
    }

    Track trackOf (const std::string& id, const std::string& name, std::size_t scenes, std::size_t returns)
    {
        Track track;
        track.id = id;
        track.name = name;
        track.monitoring = Track::Monitoring::off;
        track.output.kind = Routing::Kind::none;
        track.sends.assign (returns, floorGain);
        track.slots.resize (scenes);

        for (auto& slot : track.slots)
            slot.hasStop = false;

        return track;
    }

    /*  A set with two returns on the first two pairs - "Face" and "Loin" - and
        `scenes` scenes, all named. */
    LiveSet setOf (std::size_t scenes)
    {
        LiveSet set;
        set.tempo = 120.0;

        for (std::size_t at = 0; at < scenes; ++at)
            set.scenes.push_back ({ std::to_string (100 + at), "Q" + std::to_string (at + 1), {}, -1, false, 0.0, false });

        for (const auto& [id, name, pair] : { std::tuple<const char*, const char*, int> { "90", "A-Face", 0 },
                                              std::tuple<const char*, const char*, int> { "91", "B-Loin", 1 } })
        {
            Track ret;
            ret.id = id;
            ret.name = name;
            ret.kind = Track::Kind::returnTrack;
            ret.output.kind = Routing::Kind::external;
            ret.output.stereo = true;
            ret.output.channel = pair;
            set.returns.push_back (ret);
        }

        set.sendsPre = { false, false };
        return set;
    }

    const SendOut* sendTo (const Sound& sound, const std::string& mix)
    {
        for (const auto& send : sound.sends)
            if (send.mix == mix)
                return &send;

        return nullptr;
    }
}

TEST_CASE ("walk: the laws - Live's linear gain in decibels, its floor, and a curve thinned to its corners")
{
    Laws laws;
    CHECK (decibels (1.0, laws) == doctest::Approx (0.0));
    CHECK (decibels (0.5011872336, laws) == doctest::Approx (-6.0).epsilon (0.001));
    CHECK (decibels (floorGain, laws) == doctest::Approx (-70.0));
    CHECK (decibels (0.0, laws) == doctest::Approx (-70.0));

    /*  A ramp sampled every tenth of a second is two points; a corner stays. */
    std::vector<CurvePoint> ramp;

    for (int at = 0; at <= 40; ++at)
        ramp.push_back ({ at * 0.1, at <= 20 ? -at * 1.0 : -20.0 });

    const auto thinned = thin (ramp, 0.1);
    REQUIRE (thinned.size() == 3u);
    CHECK (thinned[1].seconds == doctest::Approx (2.0));
    CHECK (thinned[1].db == doctest::Approx (-20.0));
}

TEST_CASE ("walk: a scene that launches a clip is a GO, an empty one is not, and a stop button stops it")
{
    auto set = setOf (3);
    auto source = trackOf ("1", "1", 3, 2);
    source.sends[0] = 0.5011872336;                   // -6 dB to the face
    source.slots[0].clip = clipOf ("rain", 0.0, 16.0);
    source.slots[2].hasStop = true;
    set.tracks.push_back (source);

    const auto walked = walk (set);

    /*  The middle scene does nothing: no GO, and the numbers run on. */
    REQUIRE (walked.steps.size() == 2u);
    CHECK (walked.steps[0].number == 1);
    CHECK (walked.steps[0].name == "Q1");
    CHECK (walked.steps[1].number == 2);
    CHECK (walked.steps[1].scene == 2);

    REQUIRE (walked.steps[0].sounds.size() == 1u);
    const auto& sound = walked.steps[0].sounds[0];
    CHECK (sound.key == "sound:100:1");
    CHECK (sound.name == "rain");
    CHECK (sound.in == doctest::Approx (0.0));
    CHECK (sound.out == doctest::Approx (8.0));
    CHECK (sound.preWait == doctest::Approx (0.0));
    CHECK (sound.rate == doctest::Approx (1.0));
    CHECK (sound.levelDb == doctest::Approx (0.0));
    CHECK (sound.levelLane.empty());

    /*  ONE SEND, the face's; the distant speakers' sits at the bottom of its
        fader and carries nothing. */
    REQUIRE (sound.sends.size() == 1u);
    CHECK (sound.sends[0].mix == "return:0");
    CHECK (sound.sends[0].levelDb == doctest::Approx (-6.0).epsilon (0.001));
    CHECK (sound.sends[0].on);
    CHECK (sound.sends[0].lane.empty());

    REQUIRE (walked.steps[1].stops.size() == 1u);
    CHECK (walked.steps[1].stops[0].target == sound.key);

    /*  The mixes, named without Live's letters, on the pairs they were patched to. */
    REQUIRE (walked.mixes.size() >= 2u);
    CHECK (walked.mixes[0].name == "Face");
    CHECK (walked.mixes[0].pair == 0);
    CHECK (walked.mixes[1].name == "Loin");
    CHECK_FALSE (walked.mixes[1].parked);
}

TEST_CASE ("walk: a new clip on a track stops the one it replaces, in the same GO")
{
    auto set = setOf (2);
    auto source = trackOf ("1", "1", 2, 2);
    source.sends[0] = 1.0;
    source.slots[0].clip = clipOf ("first", 0.0, 8.0);
    source.slots[1].clip = clipOf ("second", 0.0, 8.0);
    set.tracks.push_back (source);

    const auto walked = walk (set);
    REQUIRE (walked.steps.size() == 2u);
    REQUIRE (walked.steps[1].stops.size() == 1u);
    CHECK (walked.steps[1].stops[0].target == walked.steps[0].sounds[0].key);
    REQUIRE (walked.steps[1].sounds.size() == 1u);
    CHECK (walked.steps[1].sounds[0].name == "second");
}

TEST_CASE ("walk: a start before the file is a pre-wait, a loop a second range, a stretch a speed")
{
    auto set = setOf (3);
    auto source = trackOf ("1", "1", 3, 2);
    source.sends[0] = 1.0;

    /*  THE FILE'S BEAT NOUGHT A QUARTER OF A SECOND IN, the clip starting two
        beats - a second - before it: three quarters of a second of silence. */
    auto early = clipOf ("early", -2.0, 16.0);
    early.warp = { { 0.25, 0.0 }, { 0.265625, 0.03125 } };
    source.slots[0].clip = early;

    /*  PLAYED FROM THE TOP, THEN ROUND BEATS 8 TO 24 FOR EVER. */
    auto looped = clipOf ("looped", 8.0, 24.0);
    looped.loopOn = true;
    looped.startRelative = -8.0;
    source.slots[1].clip = looped;

    /*  ONE BEAT A SECOND OF THE FILE, played at two beats a second: twice its
        speed, its pitch held. */
    auto stretched = clipOf ("stretched", 0.0, 16.0);
    stretched.warp = { { 0.0, 0.0 }, { 10.0, 10.0 } };
    source.slots[2].clip = stretched;
    set.tracks.push_back (source);

    const auto walked = walk (set);
    REQUIRE (walked.steps.size() == 3u);

    const auto& first = walked.steps[0].sounds.at (0);
    CHECK (first.preWait == doctest::Approx (0.75));
    CHECK (first.in == doctest::Approx (0.0));
    CHECK (first.out == doctest::Approx (8.25));

    const auto& second = walked.steps[1].sounds.at (0);
    CHECK (second.loops);
    CHECK (second.in == doctest::Approx (0.0));
    CHECK (second.loopIn == doctest::Approx (4.0));
    CHECK (second.loopOut == doctest::Approx (10.0));   // beat 24 is past the file's ten seconds

    const auto& third = walked.steps[2].sounds.at (0);
    CHECK (third.rate == doctest::Approx (2.0));
    CHECK (third.timestretch);
}

TEST_CASE ("walk: a fader's envelope is the level lane over the file, and a send's a send lane")
{
    auto set = setOf (1);
    auto source = trackOf ("1", "1", 1, 2);
    source.sends[0] = 1.0;
    source.sends[1] = 1.0;

    auto clip = clipOf ("rain", 0.0, 16.0);
    clip.gain = 0.5011872336;                                       // -6 dB on the clip itself

    /*  The fader up from -20 to unity over the first four seconds. */
    clip.envelopes.push_back (envelope ("1", Parameter::volume, { { 0.0, 0.1 }, { 8.0, 1.0 } }));

    /*  The distant speakers taken out over the first two. */
    clip.envelopes.push_back (envelope ("1", Parameter::send, { { 0.0, 1.0 }, { 4.0, floorGain } }, 1));
    source.slots[0].clip = clip;
    set.tracks.push_back (source);

    const auto walked = walk (set);
    const auto& sound = walked.steps.at (0).sounds.at (0);

    /*  THE CLIP'S GAIN IS THE CUE'S LEVEL, the fader the lane over it. */
    CHECK (sound.levelDb == doctest::Approx (-6.0).epsilon (0.001));
    REQUIRE (sound.levelLane.size() >= 2u);
    CHECK (sound.levelLane.front().seconds == doctest::Approx (0.0));
    CHECK (sound.levelLane.front().db == doctest::Approx (-20.0));

    const auto unity = std::find_if (sound.levelLane.begin(), sound.levelLane.end(),
                                     [] (const CurvePoint& point) { return point.db > -0.05; });
    REQUIRE (unity != sound.levelLane.end());
    CHECK (unity->seconds == doctest::Approx (4.0).epsilon (0.01));

    /*  The face's send untouched; the distant speakers' a lane down to the
        bottom of the fader in two seconds, and silence after. */
    const auto* face = sendTo (sound, "return:0");
    REQUIRE (face != nullptr);
    CHECK (face->lane.empty());
    CHECK (face->levelDb == doctest::Approx (0.0));

    const auto* far = sendTo (sound, "return:1");
    REQUIRE (far != nullptr);
    REQUIRE (far->lane.size() >= 2u);
    CHECK (far->lane.front().db == doctest::Approx (0.0));
    CHECK (far->lane.back().db == doctest::Approx (-120.0));

    const auto bottom = std::find_if (far->lane.begin(), far->lane.end(),
                                      [] (const CurvePoint& point) { return point.db <= -69.9; });
    REQUIRE (bottom != far->lane.end());
    CHECK (bottom->seconds == doctest::Approx (2.0).epsilon (0.01));
}

TEST_CASE ("walk: a carrier launched with its sound folds into it, and one launched alone is a fade")
{
    /*  LAZZI'S PATTERN: "5" plays the sound and sends nowhere itself; it goes
        into "5 track in", which listens with its monitoring In and sends to the
        face. The carrier's clip is silent; its envelope moves "5 track in". */
    auto set = setOf (2);

    auto source = trackOf ("5", "5", 2, 2);
    source.output.kind = Routing::Kind::track;
    source.output.trackId = "6";
    source.slots[0].clip = clipOf ("song", 0.0, 16.0);

    auto carrier = trackOf ("6", "5 track in", 2, 2);
    carrier.monitoring = Track::Monitoring::in;
    carrier.input.kind = Routing::Kind::track;
    carrier.input.trackId = "5";
    carrier.sends[0] = 1.0;

    auto with = clipOf ("song", 0.0, 16.0);
    with.envelopes.push_back (envelope ("6", Parameter::volume, { { 0.0, 0.8912509 } }));   // -1 dB
    carrier.slots[0].clip = with;

    auto alone = clipOf ("song", 0.0, 20.0);
    alone.envelopes.push_back (envelope ("6", Parameter::volume, { { 0.0, 0.8912509 }, { 10.0, floorGain } }));
    carrier.slots[1].clip = alone;

    set.tracks.push_back (source);
    set.tracks.push_back (carrier);

    const auto walked = walk (set);
    REQUIRE (walked.steps.size() == 2u);

    /*  FOLDED: one cue, the carrier's -1 dB on its level - every path that is
        heard crosses "5 track in"'s fader - and a send to the face. */
    REQUIRE (walked.steps[0].sounds.size() == 1u);
    CHECK (walked.steps[0].fades.empty());
    const auto& sound = walked.steps[0].sounds[0];
    CHECK (sound.levelDb == doctest::Approx (-1.0).epsilon (0.01));
    REQUIRE (sound.sends.size() == 1u);
    CHECK (sound.sends[0].mix == "return:0");
    CHECK (sound.sends[0].levelDb == doctest::Approx (0.0));

    /*  ALONE: a fade on that cue, five seconds down to silence, stopping it. */
    CHECK (walked.steps[1].sounds.empty());
    REQUIRE (walked.steps[1].fades.size() == 1u);
    const auto& fade = walked.steps[1].fades[0];
    CHECK (fade.target == sound.key);
    CHECK (fade.duration == doctest::Approx (5.0));
    CHECK (fade.stopWhenDone);
    CHECK (fade.levelDb == doctest::Approx (-120.0));
    REQUIRE (fade.points.size() >= 2u);
    CHECK (fade.points.front().seconds == doctest::Approx (0.0));
    CHECK (fade.points.front().db == doctest::Approx (-1.0).epsilon (0.01));
    CHECK (fade.points.back().seconds == doctest::Approx (1.0));
}

TEST_CASE ("walk: a track listening with its monitoring In is a carrier whatever its input says, fed by an output")
{
    /*  LAZZI'S "6 TRACK IN": its input chooser says None, and track 6's output
        is it. Live keeps a monitor-In track's clips silent, so its clip is a
        carrier - and a monitor-In track nothing feeds shapes nothing. */
    auto set = setOf (1);

    auto source = trackOf ("33", "6", 1, 2);
    source.output.kind = Routing::Kind::track;
    source.output.trackId = "34";
    source.slots[0].clip = clipOf ("meteor", 0.0, 16.0);

    auto carrier = trackOf ("34", "6 track in", 1, 2);
    carrier.monitoring = Track::Monitoring::in;
    carrier.input.kind = Routing::Kind::none;
    carrier.sends[0] = 1.0;

    auto with = clipOf ("voice", 0.0, 16.0);
    with.envelopes.push_back (envelope ("34", Parameter::volume, { { 0.0, 0.5011872336 } }));   // -6 dB
    carrier.slots[0].clip = with;

    auto lonely = trackOf ("40", "nobody's", 1, 2);
    lonely.monitoring = Track::Monitoring::in;
    lonely.slots[0].clip = clipOf ("ghost", 0.0, 8.0);

    set.tracks = { source, carrier, lonely };

    const auto walked = walk (set);
    REQUIRE (walked.steps.size() == 1u);
    REQUIRE (walked.steps[0].sounds.size() == 1u);
    CHECK (walked.steps[0].sounds[0].name == "meteor");
    CHECK (walked.steps[0].sounds[0].levelDb == doctest::Approx (-6.0).epsilon (0.001));
    CHECK (walked.steps[0].fades.empty());
}

TEST_CASE ("walk: a fader a hand rode is a DCA, a send it rode is the Send page, an effects return is parked")
{
    auto set = setOf (1);

    /*  The second return carries a reverb: parked (QE). */
    Device reverb;
    reverb.kind = "Reverb";
    set.returns[1].devices.push_back (reverb);

    auto source = trackOf ("1", "1", 1, 2);
    source.sends[0] = 1.0;
    source.sends[1] = 0.5011872336;
    source.slots[0].clip = clipOf ("rain", 0.0, 8.0);
    set.tracks.push_back (source);

    Mapping fader;
    fader.channel = 7;
    fader.number = 120;
    fader.target.trackId = "1";
    fader.target.what = Parameter::volume;

    Mapping rotary;
    rotary.channel = 7;
    rotary.number = 111;
    rotary.target.trackId = "1";
    rotary.target.what = Parameter::send;
    rotary.target.sendIndex = 0;

    set.mappings = { fader, rotary };

    const auto walked = walk (set);

    REQUIRE (walked.dcas.count ("1") == 1u);
    CHECK (walked.steps.at (0).sounds.at (0).dcaTrack == "1");

    REQUIRE (walked.hands.size() == 2u);
    CHECK (walked.hands[0].control == "CC 120, channel 7");
    CHECK (walked.hands[0].goDot.find ("DCA") != std::string::npos);
    CHECK (walked.hands[1].goDot.find ("Send page") != std::string::npos);
    CHECK (walked.hands[1].goDot.find ("Face") != std::string::npos);

    /*  THE PARKED RETURN: its send kept at its level and switched off. */
    CHECK (walked.mixes[1].parked);
    const auto* parked = sendTo (walked.steps[0].sounds[0], "return:1");
    REQUIRE (parked != nullptr);
    CHECK_FALSE (parked->on);
    CHECK (parked->levelDb == doctest::Approx (-6.0).epsilon (0.001));

    const auto said = std::any_of (walked.notes.begin(), walked.notes.end(), [] (const Note& note)
    {
        return note.kind == Note::Kind::dropped && note.text.find ("switched off") != std::string::npos;
    });
    CHECK (said);
}

TEST_CASE ("walk: the session fixture, end to end")
{
    const auto read = readSet (juce::File { juce::String (std::string (WFG_TEST_FIXTURES_DIR)) }
                                   .getChildFile ("als").getChildFile ("session.als.xml"));
    REQUIRE (read.set.has_value());

    const auto walked = walk (*read.set);

    /*  Three GOs: the rain and the wind; the carrier's fade on the wind with the
        rain stopped; the wind stopped. */
    REQUIRE (walked.steps.size() == 3u);
    CHECK (walked.steps[0].sounds.size() == 2u);
    REQUIRE (walked.steps[1].fades.size() == 1u);
    CHECK (walked.steps[1].fades[0].target == "sound:37:27");
    CHECK (walked.steps[1].fades[0].stopWhenDone);
    REQUIRE (walked.steps[1].stops.size() == 1u);
    CHECK (walked.steps[1].stops[0].target == "sound:37:14");
    REQUIRE (walked.steps[2].stops.size() == 1u);
    CHECK (walked.steps[2].stops[0].target == "sound:37:27");

    /*  The annotation is the GO's notes, as written. */
    CHECK (walked.steps[0].notes.find ("MISE") == 0u);

    /*  The rain's Reverb is off and never switched on: it is not one of its
        effects; the arrangement clip and the stray envelope are said. */
    CHECK (walked.steps[0].sounds[0].effects.empty());
    CHECK (walked.dcas.count ("14") == 1u);
}

TEST_CASE ("walk: the author's own sets, GO by GO, when WFG_ALS_CORPUS names a set")
{
    /*  AT THE DESK ONLY: WFG_ALS_CORPUS naming one `.als` prints its walk, for a
        person to read against the conduite. Nothing is judged but that it walks. */
    const auto named = juce::SystemStats::getEnvironmentVariable ("WFG_ALS_CORPUS", {});
    const juce::File file { named };

    if (named.isEmpty() || ! file.existsAsFile())
        return;

    const auto read = readSet (file);
    REQUIRE (read.set.has_value());

    const auto walked = walk (*read.set);
    CHECK_FALSE (walked.steps.empty());

    for (const auto& mix : walked.mixes)
        MESSAGE ("mix " << mix.key << " \"" << mix.name << "\" pair " << mix.pair << std::string (mix.parked ? " PARKED" : ""));

    for (const auto& step : walked.steps)
    {
        MESSAGE ("GO " << step.number << " \"" << step.name << "\": " << step.sounds.size() << " sound(s), "
                 << step.fades.size() << " fade(s), " << step.stops.size() << " stop(s)");

        for (const auto& sound : step.sounds)
        {
            std::string sends;

            for (const auto& send : sound.sends)
                sends += " " + send.mix + (send.lane.empty() ? "@" + std::to_string (send.levelDb)
                                                             : "~" + std::to_string (send.lane.size()) + "pts")
                         + std::string (send.on ? "" : "(off)");

            MESSAGE ("   sound \"" << sound.name << "\" on " << sound.trackName << " in " << sound.in << " out "
                     << sound.out << std::string (sound.loops ? " loops" : "") << " pre " << sound.preWait << " level "
                     << sound.levelDb << " lane " << sound.levelLane.size() << "pts sends" << sends
                     << " eqs " << sound.eqs.size() << " dca " << sound.dcaTrack);
        }

        for (const auto& fade : step.fades)
            MESSAGE ("   fade on " << fade.target << " " << fade.duration << " s to " << fade.levelDb << " dB, "
                     << fade.points.size() << " pts, " << fade.sends.size() << " send(s), " << fade.eq.size()
                     << " eq, stop " << fade.stopWhenDone);

        for (const auto& stop : step.stops)
            MESSAGE ("   stop " << stop.target);
    }

    for (const auto& note : walked.notes)
        MESSAGE (std::string (note.kind == Note::Kind::info ? "info " : note.kind == Note::Kind::approximated ? "approx " : "dropped ")
                 << note.scene << " " << note.track << ": " << note.text);
}
