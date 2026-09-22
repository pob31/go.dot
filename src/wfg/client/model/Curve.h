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
    The shape a fade takes, as breakpoints somebody can drag.

    THE SAME RULES THE ENGINE JUDGES BY, restated because the boundary forbids
    reaching for them: `doc::readFadePoints` is the one judge - the write door,
    `validate` and the Runner all ask it - and `doc::` is a token this half of
    the program may not name. So the rules are here a second time, and the test
    that keeps the two honest asserts every string this writes against the real
    `readFadePoints`. A copy checked against its original is a different thing
    from a copy nobody compares.

    WHAT MAKES A RUN OF DOUBLES A CURVE:

      - they pair up as (t, level), t a fraction of the duration;
      - the times climb strictly, and the first is 0 and the last is 1, so the
        drawing covers the whole of the fade and leaves no stretch it says
        nothing about;
      - every level is one a fade may reach, which is the range `fade/level`
        declares rather than a second copy of those numbers.

    AN EMPTY LIST IS NOT A BAD CURVE, it is the absence of one: `curve` applies,
    linear or sCurve, which is how every fade written before there was anything
    to draw with goes on meaning what it meant. Drawing the first point is what
    turns one into the other, and removing the last turns it back.

    NOTHING HERE WRITES. Each verb answers what the text WOULD become, and the
    window sends it - so a drag that would make a curve the engine's door refuses
    is refused here instead, before it becomes a refusal somebody has to read.
*/

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    /** One breakpoint: when, as a fraction of the fade, and at what level. */
    struct CurvePoint
    {
        double t = 0.0;
        double levelDb = 0.0;
    };

    /** The quietest and loudest a fade may be asked for, as `fade/level` says. */
    inline constexpr double quietestDb = -120.0;
    inline constexpr double loudestFadeDb = 12.0;

    struct CurveReading
    {
        std::string cueId;
        std::string cueName;

        /** The breakpoints, in order. Empty when the fade uses its `curve` word. */
        std::vector<CurvePoint> points;

        /** `linear` or `sCurve`, which is what applies while there are no points. */
        std::string curve;

        /** Where the fade ends and how long it takes, for the axes. */
        double levelDb = 0.0;
        double seconds = 0.0;

        /** Empty when there is something to draw; a sentence when there is not. */
        std::string notice;
    };

    CurveReading readCurve (const tree::TreeSnapshot&, const std::string& cueId);

    /** Which breakpoint a point on the field is over, or npos. */
    std::size_t nearestPoint (const std::vector<CurvePoint>&, double t, double levelDb,
                              double tTolerance, double levelTolerance);

    /*  WHERE A DRAGGED BREAKPOINT WOULD LAND. The two ends may move in level
        but never in time - a curve that did not start at 0 or end at 1 would
        leave a stretch of the fade undrawn, which the engine's door refuses -
        and the ones between are held strictly inside their neighbours, because
        two breakpoints at one instant are two answers to one question.
    */
    CurvePoint dragTo (const std::vector<CurvePoint>&, std::size_t at,
                       double t, double levelDb);

    /*  THE CURVE WITH ONE MORE POINT IN IT, at the time given, on the line it
        already draws there - so adding a point changes the shape not at all
        until somebody moves it, which is what makes adding one safe to do while
        listening. `nullopt` where there is already a point at that instant.

        On a curve that has none, this makes the first THREE: the two ends the
        rules require, and the one asked for. A fade with a `curve` word becomes
        a drawn one shaped exactly like the word it replaces would have been at
        its ends, so nothing jumps.
    */
    std::optional<std::vector<CurvePoint>> insertAt (const std::vector<CurvePoint>&,
                                                     double t, double fromDb, double toDb);

    /*  THE CURVE WITHOUT ONE. Removing either END is refused - the drawing has
        to cover the whole fade - and removing the last of the middle ones
        leaves two, which is a straight line and still a curve. `nullopt` when
        the answer would not be one.
    */
    std::optional<std::vector<CurvePoint>> removeAt (const std::vector<CurvePoint>&,
                                                     std::size_t at);

    /*  The text `node.set` is given, in the spelling `osc::formatDouble` writes
        so a French locale reads it back. Empty for an empty curve, which is how
        a drawn fade goes back to being a worded one.
    */
    std::string writePoints (const std::vector<CurvePoint>&);

    /** Why this is not a curve, in words; empty when it is one. */
    std::string whyNotACurve (const std::vector<CurvePoint>&);
}
