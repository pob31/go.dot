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
#include <wfg/engine/import/ImportCommon.h>

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

    /*  A NOTE FOR THE REPORT, shared with every importer (ImportCommon.h): here
        `scene` is the scene's index in the set, -1 for the set as a whole, and
        `track` the track's name. The hands table's rows are `info`. */
    using Note = import::Note;

    struct SendOut
    {
        std::string mix;            ///< the `Mix::key`
        double levelDb = 0.0;
        std::vector<CurvePoint> lane;   ///< over the FILE, an offset on `levelDb`; empty for none
        bool on = true;

        /*  A PLACE OF ITS OWN (namespace draft §121, BDT): a mono output that
            lands on one side of a stereo owner's pair is panned there, -1 the
            left and 1 the right. Empty, the sound's own pan rides it. */
        std::optional<double> pan;
    };

    /*  A MIX: a return, or the master, that reaches the interface - what a send
        is aimed at. `pair` is the interface's stereo pair from nought (or the
        mono channel with `mono`) and `effects` every device on it, by name.

        AN AUX (namespace draft §93, answering QE): a return that carries
        devices, or sends into another return, is a shared rack channel in
        Go.dot rather than a mix - its level its own, its EQ Eight its EQ when
        one fits, a third-party plugin carried onto its chain, Live's own
        devices noted, and its sends into the other returns its own sends. Its
        sends come over switched on. A return with none of that stays a mix, its
        fader folded into every send to it.

        A DIRECT OUT (namespace draft §121, BDS): a track wired straight to the
        interface has an output of its own, `direct:<track id>`, which the
        sound plays on beside its sends - a bus of kind direct in Go.dot, named
        on the cue rather than sent to.

        TWO ON ONE PAIR (BDT): the owner of a set of channels is the widest on
        them, then the first in Live's order; another whose channels all lie
        inside an owner's gives up its output - a return becomes an aux sending
        into the owner at 0 dB, a direct out sends its track `into` the owner
        instead - a mono one panned to its side. */
    struct Mix
    {
        std::string key;            ///< "return:<index>", "master" or "direct:<track id>"
        std::string name;           ///< the return's name, Live's letter taken off; a direct out's track's
        int pair = -1;
        bool mono = false;
        bool direct = false;        ///< a track's own output to the interface (BDS)
        std::string into;           ///< a direct out on channels an owner holds: that owner's key (BDT)
        std::optional<double> pan;  ///< with `into`: the side a mono one lands on
        std::vector<std::string> effects;
        bool aux = false;
        std::vector<const Device*> eqs;          ///< an aux's EQ Eights
        std::vector<const Device*> plugins;      ///< an aux's third-party plugins, in chain order
        std::vector<std::string> liveDevices;    ///< an aux's devices Go.dot has no equivalent for
        std::vector<SendOut> sends;              ///< an aux's own sends into the other returns
        double levelDb = 0.0;       ///< the return's own fader: an aux's level, or folded into every send to a mix
    };

    /*  ONE SOUND: what a media cue is made of. Seconds are the FILE's.

        OR A LIVE INPUT (namespace draft §121, BDU): a track whose monitoring is
        In on an input of the interface - a mic cue in Go.dot, with no file, on
        an input and a rack channel of its own, fired in the first GO where its
        track is on and kept running; its level, sends, EQ and devices read as
        a sound's are, from where its mixer stands. */
    struct Sound
    {
        std::string key;            ///< "sound:<scene id>:<track id>" - stable across venues (QQ); "live:<track id>"
        int scene = -1;
        std::string trackId, trackName;
        std::string name;
        FileReference file;

        bool live = false;          ///< a live input, a mic cue (BDU)
        int inputChannel = -1;      ///< with `live`: the interface's input, or pair, from nought
        bool inputStereo = false;

        double preWait = 0.0;       ///< silence before the file starts, in seconds of the room
        double in = 0.0, out = 0.0; ///< the file's seconds played first
        bool loops = false;         ///< then [loopIn, loopOut] for ever
        double loopIn = 0.0, loopOut = 0.0;

        double rate = 1.0;
        bool timestretch = false;
        double beatsPerFileSecond = 0.0;    ///< the warp's beats per second of the file; nought unwarped (BDR)

        double levelDb = 0.0;
        std::vector<CurvePoint> levelLane;
        std::vector<SendOut> sends;
        std::string directOut;      ///< the direct `Mix::key` it plays on as well (BDS); empty for none

        /*  THE TRACK'S OWN PAN, -1..1 (namespace draft §113): on every send of
            the cue as a pan when the file is mono and a balance when it is
            stereo - Live's stereo pan is a balance too. Nought at centre. */
        double pan = 0.0;

        /*  The EQ Eights on its path, as Live has them, for the translation to
            fit onto the cue's EQ, and the other devices by name for its notes. */
        std::vector<const Device*> eqs;
        std::vector<std::string> effects;

        std::string dcaTrack;       ///< the track whose fader a hand rode, when one did (QN)
        int colour = -1;

        /*  WHERE IT STARTS ON THE BEAT GRID (§121, BDZ; §122): Go.dot's word -
            `beat`, `bar`, `2bars`, `4bars`, `8bars` - or empty for at once. */
        std::string quantise;
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

        /*  A SPEED FADE (namespace draft §121, BDR): a warped sound still playing
            when a scene changes the tempo is re-stretched to its new rate, as
            Live does; the level is left alone. */
        std::optional<double> rate;
    };

    struct StopOut
    {
        std::string key;            ///< "stop:<scene id>:<track id>"
        int scene = -1;
        std::string target;         ///< the `Sound::key` it stops
        std::string name;
    };

    /*  A CONTROLLER SENT OUT (namespace draft §121, BDV): a MIDI clip's curve on
        a controller the set maps nowhere, which something outside may listen
        to - a MIDI cue on a Port named after Live's, from one value to another
        over its seconds. */
    struct MidiOut
    {
        std::string key;            ///< "midi:<scene id>:<track id>:<controller>"
        int scene = -1;
        std::string name;
        std::string port;           ///< Live's port, by name
        int channel = 1;            ///< one-based
        int controller = 0;
        int from = 0, to = 0;       ///< 0..127
        double seconds = 0.0;       ///< nought sends `to` at once
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
        std::vector<MidiOut> midi;

        /*  THE TEMPO THE SCENE SETS (BDR, §122): a transport cue's tempo verb
            first in the GO, when it differs from the tempo running. Nought
            for none. */
        double tempo = 0.0;

        bool empty() const noexcept { return sounds.empty() && fades.empty() && stops.empty() && midi.empty(); }
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

        /*  THE CONTROLS THE SET'S OWN MIDI CLIPS DRIVE (namespace draft §121,
            BDY): a mapping fed by a clip on the bus Live reads back is no hand
            - no DCA, no row in the hands table - but a row here, saying what
            the clips move and what Go.dot does for it. */
        std::vector<Hand> clipFed;

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
