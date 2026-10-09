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
    THE KNOB ABOVE A DCA STRIP: WHAT IT REACHES AND WHAT IT SAYS (namespace
    draft §50). One rule for a desk's knob and the window's panel's, read off
    the tree as `DcaColour` is.

    A turn writes the marks of what is playing under the DCA now (ABP): the
    runs playing or stopping, and for each its own cue's mark and each run's
    above it - found through the runs' own parents, not the cues', so a mark
    is written only where it changes something heard or seen (ABY) - when
    that mark reaches the DCA, itself or through a DCA it sits inside (ABN).
    On the picture's curve, the marks with a picture under them; on the
    sound's offset, those with a sound. A mark reached twice is written once.

    WHAT IS SHOWN is the nearest mark of the run that started last; a turn sets
    every mark it writes to that value moved by its detents, clamped to the
    rows' ranges, as one `node.setMany`. With nothing playing nothing is
    written. Words and the ring's fraction are made here so a desk's screen and
    the panel say the same, digit by digit so no locale changes them.

    Pure functions of a snapshot; std only, for the engine's bridge and the
    window's model alike.
*/

#include <wfg/engine/surface/DcaColour.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::surface
{
    /** What the knob turns: the picture's curve, or the sound's offset (ABK). */
    enum class KnobMode { curve, offset };

    //  The rows' ranges and a detent's worth (namespace draft §50, ABT, ABX).
    inline constexpr double knobCurveMin = -100.0;
    inline constexpr double knobCurveMax = 100.0;
    inline constexpr double knobOffsetMinDb = -24.0;
    inline constexpr double knobOffsetMaxDb = 12.0;
    inline constexpr double knobCurveStep = 2.0;
    inline constexpr double knobOffsetStepDb = 0.5;

    /*  ONE MARK A KNOB REACHES: the cue carrying it - a sound, a picture or a
        group - what it carries, what plays under it, and how near it is to the
        run that started last, which decides the value shown. */
    struct DcaMark
    {
        std::string cue;
        double curve = 0.0;
        double offsetDb = 0.0;
        bool picture = false;
        bool sound = false;

        double started = -1.0;      ///< the latest `run/started` among the runs it reaches
        int depth = 0;              ///< 0 the run's own cue, 1 the run above it...
        int order = -1;             ///< where that run stands in `run/order`
    };

    /*  For each DCA, the marks of what plays under it now, the one shown first:
        the nearest mark of the run that started last. */
    std::map<std::string, std::vector<DcaMark>> dcaMarksPlaying (const tree::TreeSnapshot& snapshot);

    /*  Where a strip starts: on the curve when its DCA has pictures assigned,
        the "V" of §39's letters, on the offset otherwise (ABX). */
    KnobMode knobStartMode (const DcaContents& contents) noexcept;

    struct KnobReading
    {
        bool any = false;           ///< something under the DCA the mode can move
        double value = 0.0;         ///< the shown mark's
        bool disagree = false;      ///< another mark the mode moves says otherwise
    };

    KnobReading knobReading (const std::vector<DcaMark>& marks, KnobMode mode) noexcept;

    /** A value moved by `steps` detents, held to the row's range. */
    double knobTurned (KnobMode mode, double value, int steps) noexcept;

    /*  THE WRITES A TURN TO `target` MAKES, as address and text pairs for one
        `node.setMany`: every mark the mode moves, once, where it differs.
        Empty when there is nothing to change. */
    std::vector<std::pair<std::string, std::string>> knobWrites (const std::vector<DcaMark>& marks,
                                                                   KnobMode mode, double target);

    /*  WHAT A STRIP'S SCREEN SAYS (ABZ): "pic +20" or "snd -3.5" in eight
        characters, "p +20" or "s -3.5" in seven or fewer, a "*" after the word
        when the marks disagree, and "pic --" with nothing to move. */
    std::string knobWords (KnobMode mode, const KnobReading& reading, int width);

    /*  Where the ring stands, 0..1 with the middle at straight and at nought dB
        - the offset's two halves of unequal size, so nought is the middle. */
    double knobRingFraction (KnobMode mode, double value) noexcept;
}
