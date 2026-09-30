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
    WHICH PICTURE GOES WITH WHAT, so a cue can be recognised before it is read.

    The author, 2026-09-30: *"Could we add some glyphs/icons (not the standard
    emoticons please) in the inspector and foot panels, cue list (type of cue
    or group and their important settings) UI to make things more
    recognisable at a glance? We can also use small colour accents."*

    THE PICTURES ARE DRAWN, NOT TYPED. A symbol from a font is whatever that
    machine's font makes of it - on Windows a good third of the arrows and
    shapes this window would want fall through to the emoji font and arrive
    as coloured stickers, which is exactly what was asked against. So an icon
    here is only a NAME; `ui/Icons` draws each one as lines on a small grid,
    in the colour it is handed, the same on every machine.

    AN ICON IS NEVER THE ONLY TELLING (PRD §4.8, which is about colour and
    holds for shape too): the kind column still says the kind in a word, the
    inspector still names every row, and each mark here carries the words it
    stands for in `meaning` - for a tooltip, and for a test that asserts what
    a row says rather than how it is drawn.

    WHAT IS DECIDED HERE AND WHAT IS NOT. Which icon a kind, a mode, a verb or
    a panel has, and which marks a row wears, is the model's - std only, so it
    is asserted with no window. What the icon looks like and which colour the
    theme gives a kind's accent are the window's and the theme file's, so the
    author can change a colour with F5 rather than with a build.
*/

#include <string>
#include <vector>

namespace wfg::client::model
{
    struct Row;

    enum class Icon
    {
        none,

        //  What a cue is.
        memo, media, mic, fade, start, osc, midi, group, range, trigger,

        //  What a transport cue does, by its verb.
        stop, stopFade, stopAfter, advance, record, loop, overdub, clear,

        //  How a group runs its members, by its mode.
        sequence, timeline, sampler,

        //  What a row says about itself beside its name.
        forever, shuffle, follow, subset, preset, disabled, speed, stretch, dca, lane,

        /*  The panels at the foot - all but the EQ and the FX, which are
            their own two letters (author, 2026-09-30: "EQ toggle can show EQ
            rather than the Gaussian bump. Same for FX"): a word as short as a
            picture, and one everybody at a desk reads faster. */
        waveform, sends, curve, take,

        //  The inspector's drawers.
        identity, clock, list, info, sound
    };

    /*  THE ICON A ROW IS RECOGNISED BY: a group's mode (a sequence, a
        timeline, a sampler), a transport cue's verb (stop, fade out, record,
        advance...), else its kind. Every kind the engine names has one, and a
        kind added later draws `none` - nothing - rather than somebody else's. */
    Icon iconFor (const std::string& kind, const std::string& mode = {}, const std::string& verb = {});

    /*  THE THEME TOKEN A KIND'S ACCENT IS DRAWN IN - "kind-media" and so on -
        or the faint ink for a kind that has none. A token and not a colour, so
        the theme file is where it is changed. */
    std::string accentFor (const std::string& kind);

    /*  THE PANEL AT THE FOOT, by the word `Subject` uses: "waveform",
        "sends", "curve", "timeline", "take" - and `none` for "eq" and "fx",
        which are drawn as their words. */
    Icon iconForPanel (const std::string& subject);

    /*  A DRAWER OF THE INSPECTOR, by its heading. "what it does" wears the
        cue's own icon, because what a cue does IS what kind of cue it is. */
    Icon iconForDrawer (const std::string& heading, const std::string& kind, const std::string& mode = {});

    /*  ONE THING A ROW SAYS ABOUT ITSELF beside its name: a shape, a few
        characters when the shape needs a number (how many rounds, what
        speed), and the words it stands for. */
    struct Mark
    {
        Icon icon = Icon::none;
        std::string text;
        std::string meaning;

        bool operator== (const Mark&) const = default;
    };

    /*  THE IMPORTANT SETTINGS, AS MARKS, and only when they differ from the
        ordinary case - a group that plays its members once, in order, on GO,
        says nothing; a cue at its own speed says nothing. In the order an eye
        wants them: whether it plays at all, then how, then what else it
        answers to. */
    std::vector<Mark> marksFor (const Row& row);
}
