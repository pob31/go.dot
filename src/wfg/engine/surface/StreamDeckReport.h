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
    WHAT A STREAM DECK IS AND SAYS, READ AND WRITTEN (namespace draft §95, SD.1).

    Elgato's decks speak HID, and every model is a row of one table: how many
    keys in how many columns, how big a key's picture is, whether it takes a
    BMP or a JPEG and which way up, what its input report looks like, and
    whether it has dials, a touch strip, touch keys or a screen. Two families
    share the protocol: the first decks - the Original and the Mini - with
    sixteen-byte picture headers, key states from the second byte and feature
    reports of seventeen; and every deck since, with eight-byte headers, key
    states from the fifth byte and feature reports of thirty-two.

    THE FACTS ARE THE PROTOCOL'S, NOT A LIBRARY'S. They were read off the
    Rust crate `elgato-streamdeck` 0.9.2 (MPL-2.0), which S21_HiJack drives
    every model with, and checked against spatcore's Stream Deck+ driver
    (controllers/streamdeck/StreamDeckDevice.h) for the one model it has. No
    code is taken from either: these are byte layouts, written out here.

    A KEY IS NUMBERED AS THE HAND SEES IT: nought top left, along the row,
    then the next row. The Original v1 numbers its keys from the right, for
    pictures and presses alike, and is turned round here; nothing else is.

    std only and pure, so a report, a packet and a picture's bytes are checked
    against bytes with no deck in the room.
*/

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace wfg::surface
{
    /*  Elgato's vendor identifier: every Stream Deck. */
    inline constexpr unsigned short deckVendor = 0x0fd9;

    enum class DeckModel { original, originalV2, mini, miniMk2, mk2, xl, xlV2, neo, pedal, plus };

    enum class DeckImage { none, bmp, jpeg };

    /*  WHAT ONE MODEL HAS. A rotation is clockwise, in quarter turns, done
        before the mirrors - the order the decks were found to want. */
    struct DeckSpec
    {
        DeckModel model = DeckModel::mk2;
        unsigned short product = 0;
        std::string_view word;              // the show's word: "mk2", "plus", ...
        std::string_view name;              // the deck's name in a sentence

        int keys = 0;
        int columns = 0;
        int rows = 0;

        DeckImage image = DeckImage::none;  // none: the Pedal, which has no screens
        int keyPixels = 0;                  // a key's picture is square
        int quarterTurns = 0;               // 0..3, clockwise
        bool mirrorX = false;
        bool mirrorY = false;

        int dials = 0;                      // the Stream Deck+: four, with a click
        int touchKeys = 0;                  // the Neo: two touch keys below the screen
        int stripWidth = 0;                 // the Stream Deck+'s touch strip, the Neo's screen
        int stripHeight = 0;

        /*  The first decks' protocol: sixteen-byte picture headers, keys from
            the second byte, seventeen-byte feature reports. */
        bool legacy = false;
    };

    /** Every model this table knows, in the order a sentence lists them. */
    const std::vector<DeckSpec>& deckModels();

    /** The model behind a product identifier, or nothing for one this table
        does not know. */
    std::optional<DeckSpec> deckSpecFor (unsigned short product);

    /** The model a show's word names ("original", "mk2", "xl", "plus", ...),
        or nothing for any other word. */
    std::optional<DeckSpec> deckSpecFor (std::string_view word);

    //==========================================================================
    /*  WHAT ONE INPUT REPORT SAID. A deck sends the state of all its keys at
        once, of all its dials at once, or one touch on its strip. */
    struct DeckInput
    {
        enum class Kind { none, keys, dialPresses, dialTurns, tap, longTap, swipe };

        Kind kind = Kind::none;

        /*  keys: every key, in the hand's order; then the Neo's two touch
            keys after the last. */
        std::array<bool, 34> keys {};

        std::array<bool, 4> dialsDown {};   // dialPresses
        std::array<int, 4> detents {};      // dialTurns: clockwise positive

        int x = 0;                          // tap, longTap, swipe: where on the strip
        int y = 0;
        int toX = 0;                        // swipe: where it ended
        int toY = 0;
    };

    /** How many bytes to ask hidapi for, the report's number included. */
    std::size_t deckInputLength (const DeckSpec& spec) noexcept;

    /** One report read. A report of nought - hidapi's "nothing yet" - or one
        too short for what it claims, or of a kind this table does not know,
        is `none`. */
    DeckInput readDeckReport (const DeckSpec& spec, const std::uint8_t* data, std::size_t length) noexcept;

    //==========================================================================
    using DeckBytes = std::vector<std::uint8_t>;

    /** A key's picture, already encoded as the model wants it, cut into the
        output reports that carry it: each one full length, ready for
        `hid_write`. Nothing for a key past the deck's last, or a deck with no
        screens. */
    std::vector<DeckBytes> deckKeyPackets (const DeckSpec& spec, int key, const DeckBytes& encoded);

    /** A JPEG for a part of the Stream Deck+'s strip, `width` by `height` at
        `x`, `y`. Nothing for any other model. */
    std::vector<DeckBytes> deckStripPackets (const DeckSpec& spec, int x, int y, int width, int height,
                                             const DeckBytes& encoded);

    /** A JPEG for the Neo's whole screen. Nothing for any other model. */
    std::vector<DeckBytes> deckScreenPackets (const DeckSpec& spec, const DeckBytes& encoded);

    /** The feature report that sets the screens' brightness, 0 to 100. */
    DeckBytes deckBrightnessReport (const DeckSpec& spec, int percent);

    /** The feature report that resets the deck: its pictures cleared to its
        own logo, as it was when it was plugged in. */
    DeckBytes deckResetReport (const DeckSpec& spec);

    /** The feature report that lights one of the Neo's two touch keys. */
    DeckBytes deckTouchKeyColourReport (const DeckSpec& spec, int touchKey, int red, int green, int blue);

    /*  WHERE THE FIRMWARE'S VERSION IS: the feature report to ask for, how
        long it is, and where its text begins in the answer (the report's
        number counted). */
    struct DeckAsk
    {
        std::uint8_t report = 0;
        std::size_t length = 0;
        std::size_t textFrom = 0;
    };

    DeckAsk deckFirmwareAsk (const DeckSpec& spec) noexcept;

    /** The text in an answer to `deckFirmwareAsk`, its noughts dropped. */
    std::string_view deckAnswerText (const DeckAsk& ask, const std::uint8_t* answer, std::size_t length) noexcept;

    //==========================================================================
    /*  A PICTURE AS PIXELS: red, green and blue a byte each, rows from the
        top. What is drawn for a key is this, the hand's way up; what a deck is
        sent is this turned and mirrored its way. */
    struct DeckPixels
    {
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> rgb;      // width * height * 3
    };

    /** Turned `quarterTurns` clockwise, then mirrored left to right and top to
        bottom as asked. */
    DeckPixels deckTransformed (const DeckPixels& picture, int quarterTurns, bool mirrorX, bool mirrorY);

    /** A picture turned the way `spec`'s keys want it. */
    DeckPixels deckOriented (const DeckSpec& spec, const DeckPixels& picture);

    /** A picture as a twenty-four-bit Windows bitmap, rows from the bottom,
        blue first: what the first decks and the Minis take. */
    DeckBytes deckBmp (const DeckPixels& picture);
}
