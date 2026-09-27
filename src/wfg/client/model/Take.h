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
    A SAMPLING CHANNEL'S TAKE, AS THE TAKE PANEL READS IT (Phase 9c, stage
    9c.4, namespace draft §19.7).

    A MIC CUE'S, NOT A CHANNEL'S. The panel opens on the cue somebody picked,
    as every editor at the foot does, and reads the take of the channel that
    cue plays through: the take belongs to the channel for the session (§19.3),
    so a later mic cue on the same channel opens on the same take - scene 5
    finding the loop of scene 2 is what the picture shows.

    EVERYTHING IS READ, NEVER WORKED OUT: the state word, the length, the
    layers, the points and the playhead are the channel's rows, which the
    engine's account of the take publishes; the recorder's size and layer
    count are the channel's own. The peaks are not here: they come through the
    third door (audio/TakePictures.h), and the panel lays them under this.

    WHETHER THE CUE CAN PRESS. Rec, Loop and a layer record a cue's input, so
    the engine refuses them `not-running` unless a mic cue is sounding on the
    channel; the panel says which cue holds it, so a greyed Rec is explained
    rather than a mystery. Undo and Clear need no cue.

    std only, and a pure reading of the snapshot the window already holds.
*/

#include <string>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    struct TakeReading
    {
        /** Whether this is a mic cue on a channel with a recorder; a sentence when it is not. */
        bool present = false;
        std::string notice;

        std::string cueId;
        std::string channelId;
        std::string channelName;

        /** empty, recording, looping, overdubbing or held: the channel's own word. */
        std::string state = "empty";

        /** The take's length, the longest the channel records, and where the loop is: seconds. */
        double length = 0.0;
        double capacity = 0.0;
        double loopIn = 0.0;
        double loopOut = 0.0;
        double playhead = 0.0;

        /** The layers on the take, and how many the channel keeps. */
        int layers = 0;
        int maxLayers = 0;

        /** The last thing the take refused or did by itself, in a sentence. */
        std::string problem;

        /*  WHO HOLDS THE CHANNEL: a run, the name of its cue, and whether it
            is a mic cue sounding - the engine's own test for Rec, Loop and a
            layer, which press the channel's take whichever mic cue holds it. */
        std::string holderRun;
        std::string holderName;
        bool channelSounds = false;

        /** The cue's own rows: what its GO does with a take, and whether its input is heard too. */
        std::string onGo = "wait";
        bool through = false;

        bool hasTake() const noexcept   { return state != "empty"; }
        bool isRecording() const noexcept { return state == "recording"; }
        bool layersFull() const noexcept { return maxLayers > 0 && layers >= maxLayers; }

        /*  HOW MUCH TIME THE PICTURE SPANS: the longest take while nothing is
            closed, so a recording grows across it from the left; the take
            itself once it is. */
        double span() const noexcept
        {
            return state == "empty" || state == "recording" ? capacity : length;
        }
    };

    TakeReading readTake (const tree::TreeSnapshot&, const std::string& cueId);

    /*  WHAT THE TAKE IS DOING, IN A SENTENCE - never a colour alone (PRD
        §4.8): "Recording, 3.2 s of 10 s." "Looping 4.2 s, 1 of 4 layers."
        "Held: 4.2 s, silent until Rec or Loop." And, when every layer is in
        use, §19.3's own sentence: "Looper holds its 4 layers: Undo one or
        Clear." */
    std::string takePanelWords (const TakeReading&);

    /*  WHY REC IS NOT OFFERED, or nothing when it is: nothing sounding on the
        channel, or another cue holding it. */
    std::string takePressWhy (const TakeReading&);

    /** The take's door for a point, which a drag or the master dial writes. */
    std::string loopPointAddress (const std::string& channelId, bool inPoint);

    /** Seconds to a tenth, locale-free: "4.2 s". */
    std::string tenthsOfSeconds (double seconds);
}
