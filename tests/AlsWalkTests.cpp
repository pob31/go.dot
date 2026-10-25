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
#include <cmath>
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
    CHECK_FALSE (walked.mixes[1].aux);
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

TEST_CASE ("walk: an unwarped clip's loop points are seconds of the file, and it plays at its own speed (namespace draft §121, BDQ)")
{
    auto set = setOf (2);
    auto source = trackOf ("1", "1", 2, 2);
    source.sends[0] = 1.0;

    /*  SIX SECONDS OF THE FILE, Live spelling an unwarped clip's loop points as
        seconds; the markers' slope - the clip's own 120 BPM - makes them the
        twelve beats its envelopes would be drawn in. Read as beats they were
        three seconds, which is how the Sturbzep organ was cut at 2:57 of 6:42. */
    auto plain = clipOf ("plain", 0.0, 6.0);
    plain.warped = false;
    source.slots[0].clip = plain;

    /*  LOOPED, THE SAME: from the top, then round seconds two to six. */
    auto looped = clipOf ("looped", 2.0, 6.0);
    looped.warped = false;
    looped.loopOn = true;
    looped.startRelative = -2.0;
    source.slots[1].clip = looped;
    set.tracks.push_back (source);

    const auto walked = walk (set);
    REQUIRE (walked.steps.size() == 2u);

    const auto& first = walked.steps[0].sounds.at (0);
    CHECK (first.in == doctest::Approx (0.0));
    CHECK (first.out == doctest::Approx (6.0));
    CHECK (first.rate == doctest::Approx (1.0));
    CHECK (first.beatsPerFileSecond == doctest::Approx (0.0));

    const auto& second = walked.steps[1].sounds.at (0);
    CHECK (second.loops);
    CHECK (second.in == doctest::Approx (0.0));
    CHECK (second.loopIn == doctest::Approx (2.0));
    CHECK (second.loopOut == doctest::Approx (6.0));
}

TEST_CASE ("walk: a scene's tempo holds from there on, and re-stretches a warped sound still playing (namespace draft §121, BDR)")
{
    auto set = setOf (3);
    set.scenes[1].tempoEnabled = true;
    set.scenes[1].tempo = 60.0;

    auto one = trackOf ("1", "1", 3, 2);
    one.sends[0] = 1.0;
    one.slots[0].clip = clipOf ("held", 0.0, 16.0);      // launched at the set's 120: its own speed

    auto two = trackOf ("2", "2", 3, 2);
    two.sends[0] = 1.0;
    two.slots[1].clip = clipOf ("later", 0.0, 16.0);     // launched at 60: half its speed
    two.slots[2].clip = clipOf ("third", 0.0, 16.0);     // the third scene sets no tempo: still 60
    set.tracks.push_back (one);
    set.tracks.push_back (two);

    const auto walked = walk (set);
    REQUIRE (walked.steps.size() == 3u);
    CHECK (walked.steps[0].sounds.at (0).rate == doctest::Approx (1.0));

    /*  THE SECOND GO: its own sound at half speed, and "held", still playing,
        re-stretched to half by a speed fade (BDR). */
    const auto& second = walked.steps[1];
    CHECK (second.sounds.at (0).rate == doctest::Approx (0.5));
    REQUIRE (second.fades.size() == 1u);
    CHECK (second.fades[0].target == walked.steps[0].sounds[0].key);
    REQUIRE (second.fades[0].rate.has_value());
    CHECK (*second.fades[0].rate == doctest::Approx (0.5));
    CHECK (second.fades[0].duration > 0.0);

    CHECK (walked.steps[2].sounds.at (0).rate == doctest::Approx (0.5));
    CHECK (walked.steps[2].fades.empty());

    CHECK (std::any_of (walked.notes.begin(), walked.notes.end(), [] (const Note& note)
    {
        return note.text.find ("re-stretches it") != std::string::npos;
    }));
}

TEST_CASE ("walk: a live input is a mic cue fired in the first GO, and a clip launched on it later is a fade on it (namespace draft §121, BDU)")
{
    auto set = setOf (3);

    /*  "VOIX": monitoring In on the interface's fourth input, -6 dB, sent to
        Face at 0 dB; no clip in the first scene, a carrier clip in the second
        that brings its fader to the bottom over ten beats. */
    auto voice = trackOf ("7", "Voix", 3, 2);
    voice.monitoring = Track::Monitoring::in;
    voice.input.kind = Routing::Kind::external;
    voice.input.stereo = false;
    voice.input.channel = 3;
    voice.volume = 0.5011872336;
    voice.sends[0] = 1.0;

    auto down = clipOf ("down", 0.0, 10.0);
    down.envelopes.push_back (envelope ("7", Parameter::volume, { { 0.0, 0.5011872336 }, { 10.0, floorGain } }));
    voice.slots[1].clip = down;

    auto source = trackOf ("1", "1", 3, 2);
    source.sends[0] = 1.0;
    source.slots[0].clip = clipOf ("rain", 0.0, 16.0);
    source.slots[2].clip = clipOf ("wind", 0.0, 16.0);
    set.tracks = { voice, source };

    const auto walked = walk (set);
    REQUIRE (walked.steps.size() == 3u);

    /*  THE FIRST GO: the rain, and the voice fired beside it. */
    const auto& first = walked.steps[0];
    REQUIRE (first.sounds.size() == 2u);
    const auto live = std::find_if (first.sounds.begin(), first.sounds.end(), [] (const Sound& one) { return one.live; });
    REQUIRE (live != first.sounds.end());
    CHECK (live->key == "live:7");
    CHECK (live->name == "Voix");
    CHECK (live->inputChannel == 3);
    CHECK_FALSE (live->inputStereo);
    CHECK (live->levelDb == doctest::Approx (-6.0).epsilon (0.001));
    REQUIRE (sendTo (*live, "return:0") != nullptr);
    CHECK (sendTo (*live, "return:0")->levelDb == doctest::Approx (0.0));
    CHECK (live->file.name.empty());

    /*  THE SECOND: its carrier clip a fade on the mic cue, to silence. */
    const auto& second = walked.steps[1];
    CHECK (second.sounds.empty());
    REQUIRE (second.fades.size() == 1u);
    CHECK (second.fades[0].target == "live:7");
    CHECK (second.fades[0].stopWhenDone);
    CHECK (second.fades[0].duration == doctest::Approx (5.0));

    /*  THE THIRD: the wind alone - the voice is running already. */
    const auto& third = walked.steps[2];
    REQUIRE (third.sounds.size() == 1u);
    CHECK_FALSE (third.sounds[0].live);

    CHECK (std::any_of (walked.notes.begin(), walked.notes.end(), [] (const Note& note)
    {
        return note.text.find ("a mic cue, fired here") != std::string::npos;
    }));
}

namespace
{
    /*  A MIDI TRACK on the IAC bus, channel 7, `scenes` empty slots. */
    Track midiTrackOf (const std::string& id, std::size_t scenes)
    {
        Track track;
        track.id = id;
        track.name = "IAC";
        track.kind = Track::Kind::midi;
        track.midiOut = { "IAC MIDI (Bus 1)", 7 };
        track.slots.resize (scenes);

        for (auto& slot : track.slots)
            slot.hasStop = false;

        return track;
    }

    /*  A MIDI CLIP of `beats`, one controller curve on it. */
    Clip midiClipOf (const std::string& name, const std::string& trackId, int controller, std::vector<EnvelopePoint> points,
                     double beats = 8.0)
    {
        Clip clip;
        clip.name = name;
        clip.midi = true;
        clip.loopStart = 0.0;
        clip.loopEnd = beats;

        Envelope curve;
        curve.target.trackId = trackId;
        curve.target.what = Parameter::controller;
        curve.target.path = std::to_string (controller);
        curve.points.push_back ({ -63072000.0, points.front().value });
        curve.points.insert (curve.points.end(), points.begin(), points.end());
        clip.envelopes.push_back (curve);
        return clip;
    }

    Mapping mappingOf (int controller, const std::string& trackId, Parameter what)
    {
        Mapping mapping;
        mapping.channel = 7;
        mapping.number = controller;
        mapping.target.trackId = trackId;
        mapping.target.what = what;
        mapping.rangeMin = what == Parameter::active ? 0.0 : floorGain;
        mapping.rangeMax = what == Parameter::volume ? 1.99526238 : 1.0;
        return mapping;
    }
}

TEST_CASE ("walk: a MIDI clip's controller curve on a mapped fader is a fade on the sound, its value held, and the mapping no hand (namespace draft §121, BDV, BDW, BDY)")
{
    auto set = setOf (3);
    auto source = trackOf ("1", "1", 3, 2);
    source.sends[0] = 1.0;
    source.volume = 0.5011872336;       // saved at -6 dB
    source.slots[0].clip = clipOf ("rain", 0.0, 16.0);
    source.slots[2].clip = clipOf ("wind", 0.0, 16.0);

    /*  THE SECOND SCENE: controller 11 from nought to 109 over eight beats -
        the fader of track 1 from the bottom to 0 dB along the provisional law. */
    auto iac = midiTrackOf ("50", 3);
    iac.slots[1].clip = midiClipOf ("Fade", "50", 11, { { 0.0, 0.0 }, { 8.0, 109.0 } });

    set.tracks = { source, iac };
    set.mappings = { mappingOf (11, "1", Parameter::volume) };

    const auto walked = walk (set);
    REQUIRE (walked.steps.size() == 3u);

    const auto& rain = walked.steps[0].sounds.at (0);
    CHECK (rain.levelDb == doctest::Approx (-6.0).epsilon (0.001));

    REQUIRE (walked.steps[1].fades.size() == 1u);
    const auto& fade = walked.steps[1].fades[0];
    CHECK (fade.target == rain.key);
    CHECK (fade.duration == doctest::Approx (4.0));
    REQUIRE (fade.points.size() >= 2u);
    CHECK (fade.points.front().db <= -70.0);
    CHECK (fade.points.back().db == doctest::Approx (0.0).epsilon (0.01));
    CHECK (fade.levelDb == doctest::Approx (0.0).epsilon (0.01));
    CHECK_FALSE (fade.stopWhenDone);

    /*  HELD: the wind, launched in the third scene, plays at where the clip
        left the fader - 0 dB - and not at the set's saved value. */
    CHECK (walked.steps[2].sounds.at (0).levelDb == doctest::Approx (0.0).epsilon (0.01));

    /*  NO HAND, NO DCA: a row of its own instead. */
    CHECK (walked.dcas.empty());
    CHECK (walked.hands.empty());
    REQUIRE (walked.clipFed.size() == 1u);
    CHECK (walked.clipFed[0].control == "CC 11, channel 7");
    CHECK (walked.clipFed[0].goDot.find ("fade") != std::string::npos);

    CHECK (std::any_of (walked.notes.begin(), walked.notes.end(), [] (const Note& note)
    {
        return note.kind == Note::Kind::approximated && note.text.find ("controller law") != std::string::npos;
    }));
    CHECK (std::any_of (walked.notes.begin(), walked.notes.end(), [] (const Note& note)
    {
        return note.text.find ("loopback") != std::string::npos;
    }));
}

TEST_CASE ("walk: a MIDI clip on a group's fader reaches every sound under it, a switch mutes and unmutes, and a controller mapped nowhere is a MIDI cue (namespace draft §121, BDV)")
{
    auto set = setOf (4);

    Track master;
    master.id = "99";
    master.name = "Master";
    master.kind = Track::Kind::master;
    master.output.kind = Routing::Kind::external;
    master.output.stereo = true;
    master.output.channel = 2;
    set.master = master;

    /*  TWO TRACKS IN A GROUP that goes to the master. */
    auto band = trackOf ("G", "Band", 4, 2);
    band.kind = Track::Kind::group;
    band.output.kind = Routing::Kind::master;

    auto brass = trackOf ("1", "Brass", 4, 2);
    brass.groupId = "G";
    brass.output.kind = Routing::Kind::group;
    brass.slots[0].clip = clipOf ("brass", 0.0, 32.0);

    auto drums = trackOf ("2", "Drums", 4, 2);
    drums.groupId = "G";
    drums.output.kind = Routing::Kind::group;
    drums.slots[0].clip = clipOf ("drums", 0.0, 32.0);

    auto iac = midiTrackOf ("50", 4);
    iac.slots[1].clip = midiClipOf ("Band down", "50", 2, { { 0.0, 109.0 }, { 4.0, 0.0 } }, 4.0);
    iac.slots[2].clip = midiClipOf ("Brass off", "50", 16, { { 0.0, 0.0 } }, 1.0);
    auto last = midiClipOf ("Brass on", "50", 16, { { 0.0, 127.0 } }, 1.0);
    last.envelopes.push_back (midiClipOf ("x", "50", 99, { { 0.0, 64.0 } }, 1.0).envelopes.front());
    iac.slots[3].clip = last;

    set.tracks = { band, brass, drums, iac };
    set.mappings = { mappingOf (2, "G", Parameter::volume), mappingOf (16, "1", Parameter::active) };

    const auto walked = walk (set);
    REQUIRE (walked.steps.size() == 4u);
    REQUIRE (walked.steps[0].sounds.size() == 2u);
    const auto brassKey = walked.steps[0].sounds[0].key;
    const auto brassLevel = walked.steps[0].sounds[0].levelDb;

    /*  THE GROUP'S FADER TO THE BOTTOM: a fade on each, to silence and stopped. */
    const auto& down = walked.steps[1].fades;
    REQUIRE (down.size() == 2u);
    CHECK (down[0].key != down[1].key);
    CHECK (down[0].stopWhenDone);
    CHECK (down[1].stopWhenDone);
    CHECK (down[0].duration == doctest::Approx (2.0));

    /*  THE BRASS SWITCHED OFF: faded to silence and kept running; on again:
        faded back to its level. */
    REQUIRE (walked.steps[2].fades.size() == 1u);
    const auto& mute = walked.steps[2].fades[0];
    CHECK (mute.target == brassKey);
    CHECK (mute.levelDb == doctest::Approx (-120.0));
    CHECK (mute.points.empty());
    CHECK_FALSE (mute.stopWhenDone);
    CHECK (mute.name.rfind ("Mute", 0) == 0u);

    REQUIRE (walked.steps[3].fades.size() == 1u);
    const auto& unmute = walked.steps[3].fades[0];
    CHECK (unmute.target == brassKey);
    CHECK (unmute.levelDb == doctest::Approx (brassLevel));
    CHECK (unmute.name.rfind ("Unmute", 0) == 0u);

    /*  CONTROLLER 99 LANDS ON NOTHING: sent out as a MIDI cue, once, at 64. */
    REQUIRE (walked.steps[3].midi.size() == 1u);
    const auto& sent = walked.steps[3].midi[0];
    CHECK (sent.port == "IAC MIDI (Bus 1)");
    CHECK (sent.channel == 7);
    CHECK (sent.controller == 99);
    CHECK (sent.from == 64);
    CHECK (sent.to == 64);
    CHECK (sent.seconds == doctest::Approx (0.0));

    CHECK (walked.dcas.empty());
    CHECK (walked.clipFed.size() == 2u);
}

TEST_CASE ("walk: a live input saved switched off is fired by the MIDI clip that switches it on, and muted by the one that switches it off (namespace draft §121, BDU, BDV)")
{
    auto set = setOf (3);

    auto voice = trackOf ("7", "Voix", 3, 2);
    voice.monitoring = Track::Monitoring::in;
    voice.input.kind = Routing::Kind::external;
    voice.input.channel = 3;
    voice.sends[0] = 1.0;
    voice.active = false;

    auto source = trackOf ("1", "1", 3, 2);
    source.sends[0] = 1.0;
    source.slots[0].clip = clipOf ("rain", 0.0, 16.0);

    auto iac = midiTrackOf ("50", 3);
    iac.slots[1].clip = midiClipOf ("Voice on", "50", 16, { { 0.0, 127.0 } }, 1.0);
    iac.slots[2].clip = midiClipOf ("Voice off", "50", 16, { { 0.0, 0.0 } }, 1.0);

    set.tracks = { voice, source, iac };
    set.mappings = { mappingOf (16, "7", Parameter::active) };

    const auto walked = walk (set);
    REQUIRE (walked.steps.size() == 3u);

    //  Off at the first GO: not fired.
    CHECK (walked.steps[0].sounds.size() == 1u);
    CHECK_FALSE (walked.steps[0].sounds[0].live);

    //  Switched on: fired here, nothing to fade yet.
    REQUIRE (walked.steps[1].sounds.size() == 1u);
    CHECK (walked.steps[1].sounds[0].live);
    CHECK (walked.steps[1].sounds[0].key == "live:7");
    CHECK (walked.steps[1].fades.empty());

    //  Switched off: muted, kept running.
    REQUIRE (walked.steps[2].fades.size() == 1u);
    CHECK (walked.steps[2].fades[0].target == "live:7");
    CHECK (walked.steps[2].fades[0].levelDb == doctest::Approx (-120.0));
    CHECK_FALSE (walked.steps[2].fades[0].stopWhenDone);

    CHECK (std::none_of (walked.notes.begin(), walked.notes.end(), [] (const Note& note)
    {
        return note.text.find ("no mic cue is made") != std::string::npos;
    }));
}

TEST_CASE ("walk: the hands table says what fires a scene and what switches a track, and a Max for Live device is said by name (namespace draft §121, BEA)")
{
    auto xml = juce::File { juce::String (std::string (WFG_TEST_FIXTURES_DIR)) }
                   .getChildFile ("als").getChildFile ("session.als.xml").loadFileAsString().toStdString();

    const auto insert = [&xml] (const std::string& before, const std::string& text)
    {
        const auto at = xml.find (before);
        REQUIRE (at != std::string::npos);
        xml.insert (at + before.size(), text);
    };

    insert ("<Speaker>", R"(<KeyMidi><IsNote Value="false" /><Channel Value="6" /><NoteOrController Value="20" /></KeyMidi>)");
    insert ("<Scene Id=\"37\">", R"(<KeyMidi><IsNote Value="true" /><Channel Value="8" /><NoteOrController Value="4" /></KeyMidi>)");
    insert ("<Scene Id=\"44\">", R"(<KeyMidi><IsNote Value="true" /><Channel Value="8" /><NoteOrController Value="5" /></KeyMidi>)");

    const auto reverb = xml.find ("<Reverb Id=\"1\">");
    REQUIRE (reverb != std::string::npos);
    xml.insert (reverb, R"(<MxDeviceAudioEffect Id="5"><On><Manual Value="true" /></On><PatchSlot><Value><MxDPatchRef><FileRef>
        <RelativePath Value="Presets/Audio Effects/Max Audio Effect/Pause.amxd" /></FileRef></MxDPatchRef></Value></PatchSlot></MxDeviceAudioEffect>)");

    const auto read = readSetXml (xml);
    REQUIRE (read.set.has_value());

    WalkOptions options;
    options.scenes = { 0, 1 };      // the third scene left out
    const auto walked = walk (*read.set, options);

    const auto row = [&walked] (const std::string& moved) -> const Hand*
    {
        for (const auto& hand : walked.hands)
            if (hand.moved.find (moved) != std::string::npos)
                return &hand;

        return nullptr;
    };

    const auto* activator = row ("track activator of \"1\"");
    REQUIRE (activator != nullptr);
    CHECK (activator->control == "CC 20, channel 7");

    const auto* first = row ("launches scene 1 \"top start\"");
    REQUIRE (first != nullptr);
    CHECK (first->control == "note 4, channel 9");
    CHECK (first->goDot.find ("a GO from cue 1") != std::string::npos);

    const auto* third = row ("launches scene 3");
    REQUIRE (third != nullptr);
    CHECK (third->goDot.find ("not imported") != std::string::npos);

    CHECK (std::any_of (walked.notes.begin(), walked.notes.end(), [] (const Note& note)
    {
        return note.kind == Note::Kind::dropped && note.text.find ("Pause.amxd") != std::string::npos;
    }));
}

TEST_CASE ("walk: a track wired straight to the interface is a direct out, and a return on channels another holds is an aux into it (namespace draft §121, BDS, BDT)")
{
    auto set = setOf (1);   // Face on pair 0 (channels 0/1), Loin on pair 1 (2/3)

    /*  TWO MORE RETURNS ON CHANNELS FACE HOLDS: "Side", mono on channel 1 - the
        right of Face's pair - and "Twin", a second stereo on pair 0. The owner
        is the widest, then the first in Live's order: Face. */
    Track side;
    side.id = "92";
    side.name = "C-Side";
    side.kind = Track::Kind::returnTrack;
    side.output.kind = Routing::Kind::external;
    side.output.stereo = false;
    side.output.channel = 1;
    set.returns.push_back (side);

    Track twin;
    twin.id = "93";
    twin.name = "D-Twin";
    twin.kind = Track::Kind::returnTrack;
    twin.output.kind = Routing::Kind::external;
    twin.output.stereo = true;
    twin.output.channel = 0;
    set.returns.push_back (twin);
    set.sendsPre = { false, false, false, false };

    for (auto& ret : set.returns)
        ret.sends.assign (4, floorGain);

    /*  THE SOURCE: sent to Face, and wired straight to the interface's sixth
        channel as well. */
    auto source = trackOf ("1", "1", 1, 4);
    source.sends[0] = 1.0;
    source.output.kind = Routing::Kind::external;
    source.output.stereo = false;
    source.output.channel = 5;
    source.slots[0].clip = clipOf ("one", 0.0, 16.0);
    set.tracks.push_back (source);

    const auto walked = walk (set);

    const auto mixNamed = [&walked] (const std::string& key) -> const Mix*
    {
        for (const auto& mix : walked.mixes)
            if (mix.key == key)
                return &mix;

        return nullptr;
    };

    const auto* direct = mixNamed ("direct:1");
    REQUIRE (direct != nullptr);
    CHECK (direct->direct);
    CHECK (direct->pair == 5);
    CHECK (direct->mono);
    CHECK (direct->name == "1");

    const auto* face = mixNamed ("return:0");
    REQUIRE (face != nullptr);
    CHECK_FALSE (face->aux);
    CHECK (face->pair == 0);

    const auto* sideMix = mixNamed ("return:2");
    REQUIRE (sideMix != nullptr);
    CHECK (sideMix->aux);
    CHECK (sideMix->pair < 0);
    CHECK (sideMix->mono);
    REQUIRE (sideMix->sends.size() == 1u);
    CHECK (sideMix->sends[0].mix == "return:0");
    CHECK (sideMix->sends[0].levelDb == doctest::Approx (0.0));
    REQUIRE (sideMix->sends[0].pan.has_value());
    CHECK (*sideMix->sends[0].pan == doctest::Approx (1.0));

    const auto* twinMix = mixNamed ("return:3");
    REQUIRE (twinMix != nullptr);
    CHECK (twinMix->aux);
    CHECK (twinMix->pair < 0);
    REQUIRE (twinMix->sends.size() == 1u);
    CHECK (twinMix->sends[0].mix == "return:0");
    CHECK_FALSE (twinMix->sends[0].pan.has_value());

    /*  THE SOUND: its send to Face, and its direct out - not a send. */
    REQUIRE (walked.steps.size() == 1u);
    const auto& sound = walked.steps[0].sounds.at (0);
    CHECK (sound.directOut == "direct:1");
    CHECK (sendTo (sound, "return:0") != nullptr);
    CHECK (sendTo (sound, "direct:1") == nullptr);

    CHECK (std::none_of (walked.notes.begin(), walked.notes.end(), [] (const Note& note)
    {
        return note.text.find ("straight to the interface") != std::string::npos;
    }));
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

TEST_CASE ("walk: a fader a hand rode is a DCA, a send it rode is the Send page, an effects return is an aux")
{
    auto set = setOf (1);

    /*  The second return carries a reverb: an aux (namespace draft §93, answering QE). */
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

    /*  THE EFFECTS RETURN IS AN AUX: its send heard, at its level, and Live's
        reverb said - the aux's chain is left for a plugin of the author's. */
    CHECK (walked.mixes[1].aux);
    REQUIRE (walked.mixes[1].liveDevices.size() == 1u);
    const auto* intoAux = sendTo (walked.steps[0].sounds[0], "return:1");
    REQUIRE (intoAux != nullptr);
    CHECK (intoAux->on);
    CHECK (intoAux->levelDb == doctest::Approx (-6.0).epsilon (0.001));

    const auto said = std::any_of (walked.notes.begin(), walked.notes.end(), [] (const Note& note)
    {
        return note.kind == Note::Kind::approximated && note.text.find ("Live's own") != std::string::npos;
    });
    CHECK (said);
}

TEST_CASE ("walk: a track's pan rides into its sound, said as approximated, and a hand on it is the Pan page (§113)")
{
    auto set = setOf (1);
    auto source = trackOf ("1", "1", 1, 2);
    source.sends[0] = 1.0;
    source.pan = -0.25;
    source.slots[0].clip = clipOf ("rain", 0.0, 8.0);
    set.tracks.push_back (source);

    Mapping rotary;
    rotary.channel = 7;
    rotary.number = 111;
    rotary.target.trackId = "1";
    rotary.target.what = Parameter::pan;
    set.mappings = { rotary };

    const auto walked = walk (set);

    REQUIRE (walked.steps.size() == 1u);
    REQUIRE (walked.steps[0].sounds.size() == 1u);
    CHECK (walked.steps[0].sounds[0].pan == doctest::Approx (-0.25));

    //  Said, and as an approximation now - Live's centre is 3 dB down, Go.dot's is full.
    const auto said = std::any_of (walked.notes.begin(), walked.notes.end(), [] (const Note& note)
    {
        return note.kind == Note::Kind::approximated && note.text.find ("panned") != std::string::npos;
    });
    CHECK (said);

    const auto dropped = std::any_of (walked.notes.begin(), walked.notes.end(), [] (const Note& note)
    {
        return note.kind == Note::Kind::dropped && note.text.find ("panned") != std::string::npos;
    });
    CHECK_FALSE (dropped);

    REQUIRE (walked.hands.size() == 1u);
    CHECK (walked.hands[0].goDot.find ("Pan page") != std::string::npos);

    //  A track at the centre carries nothing.
    set.tracks[0].pan = 0.0;
    CHECK (walk (set).steps[0].sounds[0].pan == doctest::Approx (0.0));
}

TEST_CASE ("walk: a return sending into another is an aux, its send its own, a mix's fader folded into it")
{
    auto set = setOf (1);

    /*  The first return sends half into the second, a plain mix whose fader is
        at -3 dB; nothing on either. */
    set.returns[0].sends.assign (set.returns.size(), 0.0);
    set.returns[0].sends[1] = 0.5011872336;
    set.returns[1].volume = 0.7079457844;

    auto source = trackOf ("1", "1", 1, 2);
    source.sends[0] = 1.0;
    source.slots[0].clip = clipOf ("rain", 0.0, 8.0);
    set.tracks.push_back (source);

    const auto walked = walk (set);

    REQUIRE (walked.mixes.size() >= 2u);
    CHECK (walked.mixes[0].aux);
    CHECK_FALSE (walked.mixes[1].aux);

    REQUIRE (walked.mixes[0].sends.size() == 1u);
    CHECK (walked.mixes[0].sends[0].mix == "return:1");
    CHECK (walked.mixes[0].sends[0].levelDb == doctest::Approx (-9.0).epsilon (0.01));

    /*  And the sound's send into the aux carries no fader of the aux's: the
        aux's own level does (it is nought here). */
    const auto* intoAux = sendTo (walked.steps.at (0).sounds.at (0), "return:0");
    REQUIRE (intoAux != nullptr);
    CHECK (intoAux->on);
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
        MESSAGE ("mix " << mix.key << " \"" << mix.name << "\" pair " << mix.pair << std::string (mix.aux ? " AUX" : ""));

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

TEST_CASE ("walk: a warp marker at the end of time is no marker, and every lane is at a second and a level that are numbers (found by tests/fuzz)")
{
    /*  One marker's seconds at 1e308 made the warp's slope an infinity, and a
        send's lane came out through a level that was not a number. The session
        fixture with that one number changed is the input the fuzzer found. */
    auto text = juce::File { juce::String (std::string (WFG_TEST_FIXTURES_DIR)) }
                    .getChildFile ("als").getChildFile ("session.als.xml").loadFileAsString().toStdString();
    const std::string marker = "SecTime=\"0.265625\"";
    const auto at = text.find (marker);
    REQUIRE (at != std::string::npos);
    text.replace (at, marker.size(), "SecTime=\"1e308\"");

    const auto read = readSetXml (text);
    REQUIRE (read.set.has_value());

    for (const auto& track : read.set->tracks)
        for (const auto& slot : track.slots)
            if (slot.clip)
            {
                for (const auto& point : slot.clip->warp)
                    CHECK (std::abs (point.seconds) < 1.0e9);

                for (const double beats : { -1.0e9, 0.0, 1.0e9 })
                    CHECK (std::isfinite (sampleSecondsAt (*slot.clip, beats, read.set->tempo)));
            }

    const auto walked = walk (*read.set);

    for (const auto& step : walked.steps)
        for (const auto& sound : step.sounds)
        {
            for (const auto& point : sound.levelLane)
                CHECK ((std::isfinite (point.seconds) && std::isfinite (point.db)));

            for (const auto& send : sound.sends)
                for (const auto& point : send.lane)
                    CHECK ((std::isfinite (point.seconds) && std::isfinite (point.db)));
        }
}

TEST_CASE ("walk: a loop that ends at the end of time still returns, and a return with a track's Id is walked (found by tests/fuzz)")
{
    //  A loop end of 1e308 beats made the curve's sample grid run until memory
    //  did; the grid is bounded now. The session fixture with that one number
    //  changed is the input the fuzzer found.
    auto text = juce::File { juce::String (std::string (WFG_TEST_FIXTURES_DIR)) }
                    .getChildFile ("als").getChildFile ("session.als.xml").loadFileAsString().toStdString();
    const std::string loopEnd = "<LoopEnd Value=\"16\" />";
    const auto at = text.find (loopEnd);
    REQUIRE (at != std::string::npos);
    text.replace (at, loopEnd.size(), "<LoopEnd Value=\"1e308\" />");

    const auto read = readSetXml (text);
    REQUIRE (read.set.has_value());
    (void) walk (*read.set);

    //  A return that carries a track's Id, as a set damaged or edited by hand
    //  can: the hand's fader finds the return, which has no slot for the
    //  scene. That is no envelope, said in a note, not a read past its slots.
    auto set = setOf (1);
    auto played = trackOf ("1", "played", 1, 2);
    played.slots[0].clip = clipOf ("rain", 0.0, 8.0);
    set.tracks = { played };
    set.returns[0].id = "1";

    Mapping fader;
    fader.channel = 7;
    fader.number = 120;
    fader.target.trackId = "1";
    fader.target.what = Parameter::volume;
    set.mappings = { fader };

    const auto walked = walk (set);

    const auto said = std::any_of (walked.notes.begin(), walked.notes.end(), [] (const Note& note)
    {
        return note.kind == Note::Kind::approximated && note.text.find ("no envelope on it") != std::string::npos;
    });
    CHECK (said);
}
