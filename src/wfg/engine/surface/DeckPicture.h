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
    WHAT A STREAM DECK SHOWS, AS WORDS AND COLOURS (namespace draft §95).

    The bridge decides, on the tick thread, what each key, each part of the
    Stream Deck+'s strip and the Neo's screen should show; the deck's own
    thread draws it (DeckPainter) and sends it. Between them is this: a
    description small enough to compare, so a key is drawn and sent only when
    what it says changed, and a test reads what a deck would show with no deck
    in the room.

    A KEY ALWAYS SAYS IN WORDS what its colour says (PRD §4.8): a pad going is
    lit and says "playing"; GO is green and says GO.

    std only.
*/

#include <cstdint>
#include <string>

namespace wfg::surface
{
    /*  A colour a byte a component, as a picture is drawn. */
    struct DeckColour
    {
        std::uint8_t red = 0;
        std::uint8_t green = 0;
        std::uint8_t blue = 0;

        bool operator== (const DeckColour&) const = default;
    };

    /*  ONE KEY. A pad carries its cue's number over its name, and a word for
        what it is doing under them; a key of the show or of a page carries a
        glyph, its word and sometimes a name under it. */
    struct KeyPicture
    {
        enum class Glyph { none, go, esc, doh, standbyUp, standbyDown, pageNext,
                           aimPrevious, aimNext, controlsPrevious, controlsNext,
                           pageSend, pageEq, pageFx, pageLoop };

        Glyph glyph = Glyph::none;

        std::string top;            // a pad's cue number; a page key's page number
        std::string label;          // the name, or the key's word: "GO", "Esc", "EQ"
        std::string bottom;         // what it is doing: "playing", "next: 12 Rain"

        DeckColour background;      // the cue's colour, darkened; a show key's own
        bool lit = false;           // going, or the page up: a bright frame
        bool dim = false;           // nothing under it now: drawn faint
        int progress = -1;          // a going pad's run, 0..1000 of its length; -1 none

        bool operator== (const KeyPicture&) const = default;
    };

    /*  ONE QUARTER OF THE STREAM DECK+'S STRIP, over one dial: what it turns,
        its value, and where the value stands in its range. */
    struct ZonePicture
    {
        std::string title;          // the page and the aimed cue, over the first zone
        std::string label;          // "B2 gain", "Reverb", "Loop in"
        std::string value;          // "+3.5 dB", "off", "1.250 s"

        DeckColour colour;          // the band's or the cue's
        double fill = -1.0;         // 0..1 along the bar; below nought, no bar
        bool fromCentre = false;    // a gain's bar grows either way from the middle
        bool off = false;           // its band or its send is switched out: drawn faint

        bool operator== (const ZonePicture& other) const
        {
            const auto sameFill = (fill < 0.0 && other.fill < 0.0)
                                    || (fill >= 0.0 && other.fill >= 0.0
                                        && static_cast<int> (fill * 1000.0 + 0.5)
                                             == static_cast<int> (other.fill * 1000.0 + 0.5));

            return title == other.title && label == other.label && value == other.value
                && colour == other.colour && sameFill && fromCentre == other.fromCentre && off == other.off;
        }
    };

    /*  THE NEO'S SCREEN: two lines. */
    struct ScreenPicture
    {
        std::string first;
        std::string second;

        bool operator== (const ScreenPicture&) const = default;
    };
}
