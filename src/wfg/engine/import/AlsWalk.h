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

#pragma once

/*
    THE WALK (namespace draft §29.2, QH-QJ): a Live set replayed scene by scene,
    as a night runs it, and each sound flattened onto one cue.

    The second of the importer's three stages. It reads `AlsReader`'s facts and
    answers in Go.dot's terms - a sound with a file range, a level, a level
    lane over the file, a send per mix with its own lane, a fade, a stop - but
    writes no document: `AlsTranslate` does that, so what the walk decided can
    be tested without one.

    WHAT IT DECIDES:

    - WHICH TRACK SOUNDS. A track whose monitoring is not In plays its clips. A
      track whose monitoring is In and whose input is another track is a
      "track in": its clips are SILENT CARRIERS whose envelopes move the
      track's volume, sends and devices - which shape the sound coming in.
    - WHERE A SOUND GOES. Every path from the sound's track to a speaker - its
      own sends, through a "track in" or a group, to the master - is multiplied
      out, and each return or master that reaches the interface is a mix. What
      every path shares (the clip, the track's own fader) goes on the cue's
      level; what differs goes on its sends.
    - WHAT A SCENE DOES. A clip on a sounding track is a new sound, and stops
      the one before it on that track; an empty slot with a stop button stops
      it; a carrier launched with its sound shapes that sound; a carrier
      launched alone is a fade on the sound its track is hearing.

    THE LAWS ARE NOT ASSUMED SILENTLY (QU). How Live joins two points of a
    volume envelope, what a modulation of -1..1 does to a level, and what a
    parameter holds once its clip has stopped are `Laws` - named, with the
    values used until the probe set measures them, and every one a single
    place to change.

    What the walk can see but not carry - an effect, a pan, a modulation
    across a loop, two paths summed - is a `Note` for the report, never a
    silent approximation (QV).
*/

#include <wfg/engine/import/AlsReader.h>

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace wfg::import::als
{
    /*  HOW LIVE'S NUMBERS BECOME DECIBELS AND CURVES. Until the probe set
        (AL.1) says otherwise:

        - a volume or a send envelope is STRAIGHT IN DECIBELS between two
          points - the closest Go.dot's own lanes come to a fader's throw -
          with Live's bottom of the fader, -70 dB, joined as -70 and held as
          silence;
        - a clip's volume modulation scales the CLIP'S gain from silence at -1
          to unity at 1, straight in gain;
        - a parameter holds the value its clip's envelope last reached when the
          clip stops. */
    struct Laws
    {
        enum class Volume { straightInDb, straightInGain } volume = Volume::straightInDb;
        double floorDb = -70.0;             ///< Live's bottom, joined as itself
        double silenceDb = -120.0;          ///< Go.dot's silence, which a held bottom becomes
        double thinningDb = 0.1;            ///< how far a thinned curve may stray from the sampled one
        double sampleSeconds = 0.05;        ///< the grid a curve is sampled on where it bends
    };

    /** One point of a curve: a second, and a level in dB. */
    struct CurvePoint
    {
        double seconds = 0.0;
        double db = 0.0;
    };

    /*  A NOTE FOR THE REPORT: what was seen and what was done about it, where.
        `approximated` is something that came over changed; `dropped` is
        something that did not come over; `info` is a fact a designer should
        know (the hands table's rows are `info`). */
    struct Note
    {
        enum class Kind { info, approximated, dropped } kind = Kind::info;
        int scene = -1;             ///< the scene's index in the set, -1 for the set as a whole
        std::string track;          ///< the track's name, empty for none
        std::string text;
    };

    /*  A MIX: a return, or the master, that reaches the interface - what a send
        is aimed at. `pair` is the interface's stereo pair from nought (or the
        mono channel with `mono`), `effects` the devices on it, and `parked`
        whether those effects make it a return Go.dot cannot hear (QE). */
    struct Mix
    {
        std::string key;            ///< "return:<index>" or "master"
        std::string name;           ///< the return's name, Live's letter taken off
        int pair = -1;
        bool mono = false;
        std::vector<std::string> effects;
        bool parked = false;
        double levelDb = 0.0;       ///< the return's own fader, folded into every send to it
    };

    struct SendOut
    {
        std::string mix;            ///< the `Mix::key`
        double levelDb = 0.0;
        std::vector<CurvePoint> lane;   ///< over the FILE, an offset on `levelDb`; empty for none
        bool on = true;
    };

    /*  ONE SOUND: what a media cue is made of. Seconds are the FILE's. */
    struct Sound
    {
        std::string key;            ///< "sound:<scene id>:<track id>" - stable across venues (QQ)
        int scene = -1;
        std::string trackId, trackName;
        std::string name;
        FileReference file;

        double preWait = 0.0;       ///< silence before the file starts, in seconds of the room
        double in = 0.0, out = 0.0; ///< the file's seconds played first
        bool loops = false;         ///< then [loopIn, loopOut] for ever
        double loopIn = 0.0, loopOut = 0.0;

        double rate = 1.0;
        bool timestretch = false;

        double levelDb = 0.0;
        std::vector<CurvePoint> levelLane;
        std::vector<SendOut> sends;

        /*  The EQ Eights on its path, as Live has them, for the translation to
            fit onto the cue's EQ, and the other devices by name for its notes. */
        std::vector<const Device*> eqs;
        std::vector<std::string> effects;

        std::string dcaTrack;       ///< the track whose fader a hand rode, when one did (QN)
        int colour = -1;
    };

    /*  A FADE: a carrier launched without its sound, on the sound its track is
        hearing (QI). `points` are t from 0 to 1 and an absolute level in dB; the
        sends and EQ rows are moved once, to where the carrier left them. */
    struct FadeOut
    {
        std::string key;            ///< "fade:<scene id>:<carrier track id>"
        int scene = -1;
        std::string name;
        std::string target;         ///< the `Sound::key` it moves
        double duration = 0.0;
        double levelDb = 0.0;
        std::vector<CurvePoint> points;   ///< seconds as fractions of `duration`
        std::map<std::string, double> sends;     ///< mix key to dB

        /*  An EQ Eight band the carrier moved, where the carrier left it: the
            translation knows which of the cue's EQ rows that band became. */
        struct EqMove
        {
            const Device* device = nullptr;
            int band = 0;
            std::string what;       ///< "Freq" or "Gain"
            double value = 0.0;
        };

        std::vector<EqMove> eq;
        bool stopWhenDone = false;
    };

    struct StopOut
    {
        std::string key;            ///< "stop:<scene id>:<track id>"
        int scene = -1;
        std::string target;         ///< the `Sound::key` it stops
        std::string name;
    };

    /*  ONE GO: a scene that does something, numbered in the order it is
        played, and what it does. */
    struct Step
    {
        int scene = -1;
        int number = 0;
        std::string sceneId, name, notes;
        int colour = -1;
        std::vector<Sound> sounds;
        std::vector<FadeOut> fades;
        std::vector<StopOut> stops;

        bool empty() const noexcept { return sounds.empty() && fades.empty() && stops.empty(); }
    };

    /*  A HAND: a control, what it moved in Live, and what in Go.dot does the
        same (QO). */
    struct Hand
    {
        std::string control;        ///< "CC 120, channel 7"
        std::string moved;          ///< "the volume of track 1"
        std::string goDot;          ///< "the DCA strip \"1\""
    };

    struct WalkOptions
    {
        /*  The scenes to play, by index; empty plays every scene that does
            something. A scene left out is not walked at all - its stops and
            its carriers do not happen. */
        std::set<int> scenes;
        Laws laws;
    };

    struct Walk
    {
        std::vector<Mix> mixes;
        std::vector<Step> steps;
        std::vector<Hand> hands;

        /*  The tracks a hand rode the fader of, by Live Id to name: one DCA
            each (QN). */
        std::map<std::string, std::string> dcas;

        std::vector<Note> notes;
    };

    Walk walk (const LiveSet&, const WalkOptions& = {});

    /*  Whether a scene does anything at all - launches a clip or stops a track
        - which is what the window's list ticks by default when it is named. */
    bool sceneDoesSomething (const LiveSet&, int scene);

    /*  THE CURVE ARITHMETIC, public because it is the part a test can pin:
        Live's linear gain in decibels, with its floor; and a curve thinned to
        the fewest points that stay within `tolerance` dB of it. */
    double decibels (double gain, const Laws&);
    std::vector<CurvePoint> thin (const std::vector<CurvePoint>&, double tolerance);
}
