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
    A STREAM DECK'S PICTURES DRAWN (namespace draft §95, SD.5).

    A key, a part of the Stream Deck+'s strip or the Neo's screen, drawn from
    its description (DeckPicture.h) with JUCE's software renderer, then
    turned the deck's way and encoded as it wants: a bitmap for the first
    decks, a JPEG for every one since. On the deck's own thread, never the
    tick's; a picture is a few thousand pixels and a millisecond or two.

    THE LOOK, mine and the author's to change (§95): the cue's colour as the
    ground, darkened so white words read on it; the number small at the top,
    the name in the middle on up to three lines, what it is doing at the foot,
    and a bar along the bottom edge for how far a going pad has got. A lit key
    has a white frame. The show's keys are their own colours - GO green, Esc
    red, Doh! amber - and always say their word.
*/

#include <wfg/engine/surface/DeckPicture.h>
#include <wfg/engine/surface/StreamDeckReport.h>

#include <string>

namespace wfg::surface::painter
{
    DeckPixels key (const KeyPicture& picture, int pixels);
    DeckPixels zone (const ZonePicture& picture, int width, int height);
    DeckPixels screen (const ScreenPicture& picture, int width, int height);

    /** A key's picture as `spec`'s keys take it: turned, then a bitmap or a JPEG. */
    DeckBytes keyBytes (const DeckSpec& spec, const DeckPixels& picture);

    /** A part of the Stream Deck+'s strip, or the Neo's whole screen, as it takes it. */
    DeckBytes stripBytes (const DeckSpec& spec, const DeckPixels& picture);

    /** A JPEG of a picture, at the quality the decks are sent. */
    DeckBytes jpeg (const DeckPixels& picture);

    /** A picture written as a PNG, for a test's snapshot: false when it could not be. */
    bool writePng (const DeckPixels& picture, const std::string& path);
}
