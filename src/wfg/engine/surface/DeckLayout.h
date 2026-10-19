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
    WHICH KEY DOES WHAT, ON EVERY MODEL (namespace draft §95, SD.4).

    Two jobs, by model (the author, 2026-10-19: "a key only Streamdeck could
    be for carts and show control when a Streamdeck+ is mostly for encoding
    and parameter interactions"):

      - A DECK OF KEYS - the Original, the MK.2, the XL, the Mini, the Neo, the
        Pedal - shows a cart: its pads from the top left in the cart's order,
        and when the cart is longer than the deck, its last free key turns the
        page (the Neo's two touch keys do instead). With the show's keys
        switched on (off by default, the author's), the end of the last row is
        GO, Esc, Doh! and the standby's arrows - as many as the deck has room
        for.

      - THE STREAM DECK+ shows pages on the aimed cue, the D700's: its top
        row picks Send, EQ, FX or Loop, and its dials turn what the page puts
        under them. Its bottom row aims the previous or the next cue and moves
        along the page's controls - or, with the show's keys on, Esc and GO
        where the controls' arrows were; a swipe on the strip still moves along.

    THE ARRANGEMENT IS MINE, the author's to change (§95): GO at the bottom
    right, where a right hand finds it without looking; Esc beside it; Doh!
    before them; the standby's arrows first.

    std only and pure.
*/

#include <wfg/engine/surface/StreamDeckReport.h>

#include <vector>

namespace wfg::surface
{
    enum class DeckRole { blank, pad, cartPage, go, esc, doh, standbyUp, standbyDown,
                          pageSend, pageEq, pageFx, pageLoop,
                          aimPrevious, aimNext, controlsPrevious, controlsNext };

    struct DeckKey
    {
        DeckRole role = DeckRole::blank;
        int pad = -1;           // pad: which of the cart's pads
    };

    /*  WHAT A DECK OF KEYS SHOWS NOW, key by key in the hand's order - the
        Neo's touch keys not counted - and how many pages the cart takes. */
    struct DeckLayout
    {
        std::vector<DeckKey> keys;
        int page = 0;           // the cart's page shown, kept in range
        int pages = 1;
    };

    /** The show's keys a model has room for, in the order they stand at the
        end of its last row: none for the Stream Deck+, whose keys are its pages'. */
    std::vector<DeckRole> deckShowKeys (const DeckSpec& spec);

    /** A deck of keys over a cart of `pads` pads, on its page `page` (taken
        round into range). */
    DeckLayout deckLayoutFor (const DeckSpec& spec, bool showKeys, int pads, int page);

    /** The Stream Deck+'s eight keys. */
    DeckLayout plusLayout (bool showKeys);

    /** Whether a model is the Stream Deck+'s kind - dials and pages - rather
        than a deck of keys. */
    bool hasDials (const DeckSpec& spec) noexcept;
}
