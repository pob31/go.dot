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
    A media cue's EQ, as the window reads it and draws it (Phase 9a).

    THE NINETEEN ROWS READ BACK INTO THE ONE VALUE the engine uses - the same
    `audio::EqSettings` the voice is given - so the panel holds what the voice
    holds and nothing is translated on the way. And THE CURVE IS THE DSP'S OWN
    FUNCTION: `eqCurve` asks `audio/EqMath.h`, which is header-only and names
    no JUCE type, so the picture a designer drags is by construction the
    response the voice plays. CueEqTests pins the maths to the sound; this
    pins the picture to the maths.

    std only, one snapshot door as every model file: the window reads the tree
    once a pass and hands the reading down.
*/

#include <wfg/engine/audio/EqSettings.h>

#include <string>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    struct EqReading
    {
        /** Whether the cue publishes an EQ at all - only a media cue does. */
        bool present = false;

        audio::EqSettings settings;

        /** Why there is nothing to draw, when there is nothing to draw. */
        std::string notice;
    };

    /** The cue's nineteen rows, by exact address, as one value. */
    EqReading readEq (const tree::TreeSnapshot&, const std::string& cueId);

    /** One point of the drawn response. */
    struct EqPoint
    {
        double frequency = 0.0;
        double db = 0.0;
    };

    /*  The response over the audible range, `points` of them spaced evenly
        in octaves from 20 Hz to 20 kHz, at the sample rate the picture is
        drawn for. Nought everywhere for a flat EQ, by construction. */
    std::vector<EqPoint> eqCurve (const audio::EqSettings&, double sampleRate, int points);

    /** `/godot/cue/<id>/<row>`, the address a hand writes. */
    std::string eqAddress (const std::string& cueId, const std::string& row);

    /*  The row names in the table's own spelling, so the panel and the
        reader never disagree about a letter: `eqB2Gain`, `eqHpfFreq`. */
    std::string eqBandRow (int band, const char* suffix);

    /** The shape words the two shelving bands accept, as the row spells them. */
    const char* eqShapeWord (audio::EqSettings::Shape);
    audio::EqSettings::Shape eqShapeFor (const std::string& word);
}
