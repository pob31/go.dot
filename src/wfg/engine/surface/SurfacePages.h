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
    A SURFACE'S EQ AND SEND PAGES, AS NUMBERS AND WORDS (author, 2026-09-25).

    Pick a sample with SELECT, press EQ, and the rotaries are that cue's EQ;
    press Send and they are its send levels. This file is what the rotaries
    mean on those pages - which control each one turns, how far a detent moves
    it, what its screen says, how its ring stands and which colour it wears -
    and nothing about bytes or the tree, which are the bridge's. So it is
    pure, and a test pins every law without a surface in the room.

    THE EQ MAP IS THE AUTHOR'S, sixteen controls in the order a hand meets
    them left to right: the high-pass, band one's shape, frequency, gain and
    width, band two's frequency, gain and width - then band three's, band
    four's shape and the rest of it, and the low-pass. Sixteen rotaries show
    them at once; eight show the first row, and a second press of EQ the
    second ("If there is only one set of rotaries the second press open the
    higher bands, if there are 2 banks of 8 then assign all of the bands").

    A PRESS SWITCHES: the high-pass and the low-pass in and out on their
    frequency's rotary, a band in and out on its gain's (eqB<n>On - off keeps
    the numbers), and a shape between its peak and its shelf. A frequency's
    or a width's press does nothing.

    std only.
*/

#include <wfg/engine/surface/FaderCurve.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace wfg::surface
{
    /*  What a surface's rotaries show. `fx` since 2026-09-26: the aimed cue's
        inserts, their parameters in each plugin's own order (the author's
        decision), the inserts walked in chain order. */
    enum class Page { show, eq, send, fx };

    /** The word `surface/page` publishes: show, eq, send or fx. */
    std::string_view pageWord (Page page) noexcept;

    /*  HOW A CONTROL MOVES AND READS. A frequency and a width are turned in
        ratios and drawn on a logarithmic ring; a gain in decibels, drawn from
        the centre; a shape is a choice of two; a send's level moves along the
        fader's own law, so its ring is where a fader would be. */
    enum class Law { frequency, gain, width, shape, level };

    /** What a press on a control does. */
    enum class Press { none, toggleSwitch, toggleShape };

    struct EqControl
    {
        std::string_view row;         // the media row it turns: eqB2Gain
        std::string_view label;       // at most twelve characters, a D700's first row
        std::string_view shortLabel;  // at most seven, an MCU scribble strip
        Law law;
        double minimum;
        double maximum;
        int colour;                   // 0 the high-pass, 1..4 the bands, 5 the low-pass (audio/EqColours.h)
        std::string_view switchRow;   // what takes this control's band out when it is false
        Press press;
        std::string_view shelf;       // a shape: the shelf its band may be
    };

    inline constexpr int eqControlCount = 16;

    /*  THE AUTHOR'S MAP. The ranges are the parameter table's; a test holds
        them to it. */
    inline constexpr std::array<EqControl, eqControlCount> eqControls { {
        { "eqHpfFreq", "HP freq",  "HP Frq",  Law::frequency, 20.0,  2000.0,  0, "eqHpf",  Press::toggleSwitch, {} },
        { "eqB1Shape", "B1 shape", "B1 Shp",  Law::shape,     0.0,   0.0,     1, "eqB1On", Press::toggleShape,  "lowShelf" },
        { "eqB1Freq",  "B1 freq",  "B1 Frq",  Law::frequency, 20.0,  20000.0, 1, "eqB1On", Press::none,         {} },
        { "eqB1Gain",  "B1 gain",  "B1 Gain", Law::gain,      -24.0, 24.0,    1, "eqB1On", Press::toggleSwitch, {} },
        { "eqB1Q",     "B1 Q",     "B1 Q",    Law::width,     0.1,   10.0,    1, "eqB1On", Press::none,         {} },
        { "eqB2Freq",  "B2 freq",  "B2 Frq",  Law::frequency, 20.0,  20000.0, 2, "eqB2On", Press::none,         {} },
        { "eqB2Gain",  "B2 gain",  "B2 Gain", Law::gain,      -24.0, 24.0,    2, "eqB2On", Press::toggleSwitch, {} },
        { "eqB2Q",     "B2 Q",     "B2 Q",    Law::width,     0.1,   10.0,    2, "eqB2On", Press::none,         {} },
        { "eqB3Freq",  "B3 freq",  "B3 Frq",  Law::frequency, 20.0,  20000.0, 3, "eqB3On", Press::none,         {} },
        { "eqB3Gain",  "B3 gain",  "B3 Gain", Law::gain,      -24.0, 24.0,    3, "eqB3On", Press::toggleSwitch, {} },
        { "eqB3Q",     "B3 Q",     "B3 Q",    Law::width,     0.1,   10.0,    3, "eqB3On", Press::none,         {} },
        { "eqB4Shape", "B4 shape", "B4 Shp",  Law::shape,     0.0,   0.0,     4, "eqB4On", Press::toggleShape,  "highShelf" },
        { "eqB4Freq",  "B4 freq",  "B4 Frq",  Law::frequency, 20.0,  20000.0, 4, "eqB4On", Press::none,         {} },
        { "eqB4Gain",  "B4 gain",  "B4 Gain", Law::gain,      -24.0, 24.0,    4, "eqB4On", Press::toggleSwitch, {} },
        { "eqB4Q",     "B4 Q",     "B4 Q",    Law::width,     0.1,   10.0,    4, "eqB4On", Press::none,         {} },
        { "eqLpfFreq", "LP freq",  "LP Frq",  Law::frequency, 1000.0, 20000.0, 5, "eqLpf", Press::toggleSwitch, {} },
    } };

    /** How many pages `controls` need on `rotaries` rotaries: one at least. */
    int pageCount (int controls, int rotaries) noexcept;

    /*  A NUMBER TURNED `steps` DETENTS by its law (SurfaceProfile.h's `page`
        numbers), kept within the range and rounded to what a hand can tell
        apart - a tenth of a hertz under a kilohertz and a hertz above, a
        tenth of a decibel, a hundredth of a width. A level turned up from
        silence starts where the fader's travel does. `fader` is the law a
        level moves along. */
    double turned (Law law, double value, int steps, double minimum, double maximum,
                   FaderLaw fader) noexcept;

    /** A shape turned: clockwise is the shelf, anticlockwise the peak. */
    std::string_view turnedShape (std::string_view current, int steps, std::string_view shelf) noexcept;

    /** A shape pressed: the peak becomes the shelf and anything else the peak. */
    std::string_view pressedShape (std::string_view current, std::string_view shelf) noexcept;

    /*  WHERE A CONTROL'S RING STANDS: a value and a fill. On a D700 the value
        is 0..127 and the fill is the MIDI channel it is sent on - 1 from the
        centre, 2 from the left (control guide §4.3); on an MCU it is one of
        eleven positions and Mackie's own mode - 1 boost/cut, 2 wrap. A gain
        fills from the centre and everything else from the left. A shape is
        empty as a peak and full as a shelf. */
    struct Ring
    {
        int value = 0;
        int mode = 2;

        bool operator== (const Ring&) const = default;
    };

    Ring d700RingFor (Law law, double value, double minimum, double maximum, FaderLaw fader) noexcept;
    Ring mcuRingFor (Law law, double value, double minimum, double maximum, FaderLaw fader) noexcept;

    /*  A PLUGIN'S PARAMETER ON THE FX PAGE (2026-09-26), normalised 0..1 as
        the plugin takes it. A stepped one (`parameterSteps` two or more) moves
        one step a detent, a continuous one `pageParameterTravelPerDetent` of
        its travel; within 0..1, unrounded - the plugin's own text says what
        it is, and a 128th is exact in binary. Its ring fills
        from the centre when its middle is its rest (`bipolar`, the catalogue's
        guess) and from the left otherwise. */
    double turnedParameter (double value, int steps, int parameterSteps) noexcept;
    Ring d700ParameterRing (double value, bool bipolar) noexcept;
    Ring mcuParameterRing (double value, bool bipolar) noexcept;

    /** A shape's ring, which reads a word rather than a number. */
    Ring d700ShapeRing (std::string_view shape) noexcept;
    Ring mcuShapeRing (std::string_view shape) noexcept;

    /*  WHAT A CONTROL'S SCREEN SAYS ITS VALUE IS, written digit by digit so no
        locale can move the point: "80.0 Hz", "125 Hz", "2.00 kHz", "12.5 kHz";
        "+3.5 dB", "-inf dB"; "Q 0.70". `compact` drops the space before a unit
        for an MCU's seven characters. */
    void valueText (Law law, double value, bool compact, std::string& out);

    /** A shape as a screen says it: "Peak", "Lo shelf", "Hi shelf". */
    std::string_view shapeText (std::string_view shape, bool compact) noexcept;

    /*  WHETHER A PAGE BUTTON IS LIT THIS TICK: steadily with one page of its
        kind, and with more, `index + 1` blinks at the start of every cycle
        (SurfaceProfile.h's `pageBlink` numbers). */
    bool pageButtonLit (int index, int count, std::int64_t tick) noexcept;
}
