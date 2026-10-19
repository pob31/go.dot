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

/*
    THE STREAM DECK (namespace draft §95): every model's row, its input reports
    read, its pictures cut into reports and its feature reports, against bytes.
    The decks themselves are the bench's.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/surface/StreamDeckReport.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

using namespace wfg;
using surface::DeckBytes;
using surface::DeckInput;
using surface::DeckModel;

namespace
{
    surface::DeckSpec specOf (std::string_view word)
    {
        const auto spec = surface::deckSpecFor (word);
        REQUIRE (spec.has_value());
        return *spec;
    }

    DeckBytes numbered (std::size_t count)
    {
        DeckBytes bytes (count);

        for (std::size_t at = 0; at < count; ++at)
            bytes[at] = static_cast<std::uint8_t> (at % 251);

        return bytes;
    }
}

TEST_CASE ("stream deck: every model has a row, found by its product and by its word")
{
    std::set<unsigned short> products;
    std::set<std::string> words;

    for (const auto& spec : surface::deckModels())
    {
        CHECK (products.insert (spec.product).second);
        CHECK (words.insert (std::string (spec.word)).second);
        CHECK (spec.keys == spec.columns * spec.rows);
        CHECK (spec.keys + spec.touchKeys <= 34);
        CHECK (surface::deckSpecFor (spec.product)->model == spec.model);
        CHECK (surface::deckSpecFor (spec.word)->model == spec.model);
    }

    CHECK (surface::deckModels().size() == 10);

    CHECK (specOf ("mk2").product == 0x0080);
    CHECK (specOf ("mk2").keys == 15);
    CHECK (specOf ("original").product == 0x0060);
    CHECK (specOf ("original").legacy);
    CHECK (specOf ("xl").keys == 32);
    CHECK (specOf ("xl").keyPixels == 96);
    CHECK (specOf ("mini").keys == 6);
    CHECK (specOf ("mini").image == surface::DeckImage::bmp);
    CHECK (specOf ("mini").quarterTurns == 1);
    CHECK (specOf ("neo").touchKeys == 2);
    CHECK (specOf ("neo").stripWidth == 248);
    CHECK (specOf ("pedal").image == surface::DeckImage::none);
    CHECK (specOf ("plus").dials == 4);
    CHECK (specOf ("plus").keyPixels == 120);
    CHECK (specOf ("plus").stripWidth == 800);
    CHECK_FALSE (specOf ("plus").mirrorX);

    CHECK_FALSE (surface::deckSpecFor (static_cast<unsigned short> (0x1234)).has_value());
    CHECK_FALSE (surface::deckSpecFor (std::string_view ("streamdeck")).has_value());
}

TEST_CASE ("stream deck: the keys are read from the fifth byte, and the first decks' from the second")
{
    const auto mk2 = specOf ("mk2");
    CHECK (surface::deckInputLength (mk2) == 19);

    DeckBytes report (19, 0);
    report[0] = 0x01;
    report[4 + 0] = 1;
    report[4 + 7] = 1;
    report[4 + 14] = 1;

    const auto read = surface::readDeckReport (mk2, report.data(), report.size());
    REQUIRE (read.kind == DeckInput::Kind::keys);
    CHECK (read.keys[0]);
    CHECK (read.keys[7]);
    CHECK (read.keys[14]);
    CHECK_FALSE (read.keys[1]);

    //  The Mini: from the second byte, in the hand's order.
    const auto mini = specOf ("mini");
    CHECK (surface::deckInputLength (mini) == 7);
    const std::uint8_t miniReport[] { 0x01, 0, 0, 1, 0, 0, 1 };
    const auto miniRead = surface::readDeckReport (mini, miniReport, sizeof (miniReport));
    REQUIRE (miniRead.kind == DeckInput::Kind::keys);
    CHECK (miniRead.keys[2]);
    CHECK (miniRead.keys[5]);
    CHECK_FALSE (miniRead.keys[0]);

    //  The Original v1 counts a row from the right: its key 4 is the hand's 0.
    const auto original = specOf ("original");
    DeckBytes v1 (16, 0);
    v1[0] = 0x01;
    v1[1 + 4] = 1;      // the device's 4: the top row's leftmost
    v1[1 + 5] = 1;      // the device's 5: the second row's rightmost
    const auto v1Read = surface::readDeckReport (original, v1.data(), v1.size());
    REQUIRE (v1Read.kind == DeckInput::Kind::keys);
    CHECK (v1Read.keys[0]);
    CHECK (v1Read.keys[9]);
    CHECK_FALSE (v1Read.keys[4]);
    CHECK_FALSE (v1Read.keys[5]);

    //  The Neo's two touch keys come after its eight.
    const auto neo = specOf ("neo");
    DeckBytes neoReport (surface::deckInputLength (neo), 0);
    CHECK (neoReport.size() == 14);
    neoReport[0] = 0x01;
    neoReport[4 + 8] = 1;
    const auto neoRead = surface::readDeckReport (neo, neoReport.data(), neoReport.size());
    REQUIRE (neoRead.kind == DeckInput::Kind::keys);
    CHECK (neoRead.keys[8]);
    CHECK_FALSE (neoRead.keys[9]);

    //  Nothing yet, and too short, are nothing.
    const std::uint8_t empty[] { 0x00, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    CHECK (surface::readDeckReport (mk2, empty, sizeof (empty)).kind == DeckInput::Kind::none);
    CHECK (surface::readDeckReport (mk2, report.data(), 10).kind == DeckInput::Kind::none);
    CHECK (surface::readDeckReport (mk2, nullptr, 19).kind == DeckInput::Kind::none);
}

TEST_CASE ("stream deck: the Stream Deck+ says its keys, its dials and its strip apart")
{
    const auto plus = specOf ("plus");
    CHECK (surface::deckInputLength (plus) == 14);

    const std::uint8_t keys[] { 0x01, 0x00, 0x08, 0x00, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0 };
    const auto keysRead = surface::readDeckReport (plus, keys, sizeof (keys));
    REQUIRE (keysRead.kind == DeckInput::Kind::keys);
    CHECK (keysRead.keys[1]);
    CHECK (keysRead.keys[7]);
    CHECK_FALSE (keysRead.keys[0]);

    const std::uint8_t turns[] { 0x01, 0x03, 0x05, 0x00, 0x01, 0x02, 0xfd, 0x00, 0x10, 0, 0, 0, 0, 0 };
    const auto turnsRead = surface::readDeckReport (plus, turns, sizeof (turns));
    REQUIRE (turnsRead.kind == DeckInput::Kind::dialTurns);
    CHECK (turnsRead.detents[0] == 2);
    CHECK (turnsRead.detents[1] == -3);
    CHECK (turnsRead.detents[2] == 0);
    CHECK (turnsRead.detents[3] == 16);

    const std::uint8_t presses[] { 0x01, 0x03, 0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0, 0, 0, 0, 0 };
    const auto pressesRead = surface::readDeckReport (plus, presses, sizeof (presses));
    REQUIRE (pressesRead.kind == DeckInput::Kind::dialPresses);
    CHECK_FALSE (pressesRead.dialsDown[0]);
    CHECK (pressesRead.dialsDown[1]);
    CHECK (pressesRead.dialsDown[3]);

    const std::uint8_t tap[] { 0x01, 0x02, 0x0e, 0x00, 0x01, 0x00, 0x2c, 0x01, 0x32, 0x00, 0, 0, 0, 0 };
    const auto tapRead = surface::readDeckReport (plus, tap, sizeof (tap));
    REQUIRE (tapRead.kind == DeckInput::Kind::tap);
    CHECK (tapRead.x == 300);
    CHECK (tapRead.y == 50);

    const std::uint8_t hold[] { 0x01, 0x02, 0x0e, 0x00, 0x02, 0x00, 0x64, 0x00, 0x0a, 0x00, 0, 0, 0, 0 };
    CHECK (surface::readDeckReport (plus, hold, sizeof (hold)).kind == DeckInput::Kind::longTap);

    const std::uint8_t swipe[] { 0x01, 0x02, 0x0e, 0x00, 0x03, 0x00, 0x58, 0x02, 0x32, 0x00, 0x64, 0x00, 0x30, 0x00 };
    const auto swipeRead = surface::readDeckReport (plus, swipe, sizeof (swipe));
    REQUIRE (swipeRead.kind == DeckInput::Kind::swipe);
    CHECK (swipeRead.x == 600);
    CHECK (swipeRead.toX == 100);
    CHECK (swipeRead.toY == 48);

    const std::uint8_t strange[] { 0x01, 0x07, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    CHECK (surface::readDeckReport (plus, strange, sizeof (strange)).kind == DeckInput::Kind::none);
}

TEST_CASE ("stream deck: a key's picture is cut into reports with the model's header")
{
    //  Every deck since the first: 1024 a report, an eight-byte header.
    const auto mk2 = specOf ("mk2");
    const auto picture = numbered (2000);
    const auto reports = surface::deckKeyPackets (mk2, 3, picture);
    REQUIRE (reports.size() == 2);

    for (const auto& report : reports)
        CHECK (report.size() == 1024);

    CHECK (DeckBytes (reports[0].begin(), reports[0].begin() + 8) == DeckBytes { 0x02, 0x07, 3, 0, 0xf8, 0x03, 0, 0 });
    CHECK (DeckBytes (reports[1].begin(), reports[1].begin() + 8) == DeckBytes { 0x02, 0x07, 3, 1, 0xd8, 0x03, 1, 0 });
    CHECK (reports[0][8] == picture[0]);
    CHECK (reports[1][8] == picture[1016]);
    CHECK (reports[1][8 + 983] == picture[1999]);
    CHECK (reports[1][8 + 984] == 0);

    //  The Original v1: two reports of 8191, pages from one, the key counted from the right.
    const auto original = specOf ("original");
    const auto bmp = numbered (15606);
    const auto v1 = surface::deckKeyPackets (original, 0, bmp);
    REQUIRE (v1.size() == 2);
    CHECK (v1[0].size() == 8191);
    CHECK (DeckBytes (v1[0].begin(), v1[0].begin() + 6) == DeckBytes { 0x02, 0x01, 1, 0, 0, 5 });
    CHECK (DeckBytes (v1[1].begin(), v1[1].begin() + 6) == DeckBytes { 0x02, 0x01, 2, 0, 1, 5 });
    CHECK (v1[1][16] == bmp[7803]);

    //  The Mini: 1024, a sixteen-byte header, pages from nought.
    const auto mini = specOf ("mini");
    const auto miniReports = surface::deckKeyPackets (mini, 5, numbered (19254));
    CHECK (miniReports.size() == 20);
    CHECK (DeckBytes (miniReports[0].begin(), miniReports[0].begin() + 6) == DeckBytes { 0x02, 0x01, 0, 0, 0, 6 });
    CHECK (DeckBytes (miniReports[19].begin(), miniReports[19].begin() + 6) == DeckBytes { 0x02, 0x01, 19, 0, 1, 6 });

    //  No screens, no such key, nothing to send: nothing.
    CHECK (surface::deckKeyPackets (specOf ("pedal"), 0, picture).empty());
    CHECK (surface::deckKeyPackets (mk2, 15, picture).empty());
    CHECK (surface::deckKeyPackets (mk2, -1, picture).empty());
    CHECK (surface::deckKeyPackets (mk2, 0, {}).empty());
}

TEST_CASE ("stream deck: the strip, the Neo's screen, brightness, reset and the firmware's version")
{
    const auto plus = specOf ("plus");
    const auto strip = surface::deckStripPackets (plus, 200, 0, 200, 100, numbered (1500));
    REQUIRE (strip.size() == 2);
    CHECK (DeckBytes (strip[0].begin(), strip[0].begin() + 16)
           == DeckBytes { 0x02, 0x0c, 0xc8, 0x00, 0, 0, 0xc8, 0x00, 0x64, 0x00, 0, 0, 0, 0xf0, 0x03, 0 });
    CHECK (DeckBytes (strip[1].begin(), strip[1].begin() + 16)
           == DeckBytes { 0x02, 0x0c, 0xc8, 0x00, 0, 0, 0xc8, 0x00, 0x64, 0x00, 1, 1, 0, 0xec, 0x01, 0 });
    CHECK (surface::deckStripPackets (specOf ("mk2"), 0, 0, 200, 100, numbered (10)).empty());

    const auto neo = specOf ("neo");
    const auto screen = surface::deckScreenPackets (neo, numbered (100));
    REQUIRE (screen.size() == 1);
    CHECK (DeckBytes (screen[0].begin(), screen[0].begin() + 8) == DeckBytes { 0x02, 0x0b, 0, 1, 100, 0, 0, 0 });
    CHECK (surface::deckScreenPackets (plus, numbered (100)).empty());

    const auto bright = surface::deckBrightnessReport (plus, 140);
    CHECK (bright.size() == 32);
    CHECK (DeckBytes (bright.begin(), bright.begin() + 4) == DeckBytes { 0x03, 0x08, 100, 0 });
    const auto dim = surface::deckBrightnessReport (specOf ("mini"), 30);
    CHECK (dim.size() == 17);
    CHECK (DeckBytes (dim.begin(), dim.begin() + 7) == DeckBytes { 0x05, 0x55, 0xaa, 0xd1, 0x01, 30, 0 });

    CHECK (surface::deckResetReport (plus) == [] { DeckBytes b (32, 0); b[0] = 0x03; b[1] = 0x02; return b; }());
    CHECK (surface::deckResetReport (specOf ("original")).size() == 17);
    CHECK (surface::deckResetReport (specOf ("original"))[1] == 0x63);

    const auto touch = surface::deckTouchKeyColourReport (neo, 1, 255, 300, -4);
    CHECK (DeckBytes (touch.begin(), touch.begin() + 6) == DeckBytes { 0x03, 0x06, 9, 255, 255, 0 });
    CHECK (surface::deckTouchKeyColourReport (neo, 2, 0, 0, 0).empty());
    CHECK (surface::deckTouchKeyColourReport (plus, 0, 0, 0, 0).empty());

    const auto ask = surface::deckFirmwareAsk (plus);
    CHECK (ask.report == 0x05);
    CHECK (ask.length == 32);
    DeckBytes answer (33, 0);
    answer[0] = 0x05;
    const std::string version = "1.01.000";
    std::copy (version.begin(), version.end(), answer.begin() + 6);
    CHECK (surface::deckAnswerText (ask, answer.data(), answer.size()) == "1.01.000");
    CHECK (surface::deckAnswerText (ask, answer.data(), 4).empty());
    CHECK (surface::deckFirmwareAsk (specOf ("mini")).textFrom == 5);
}

TEST_CASE ("stream deck: a picture is turned and mirrored the deck's way, and written as a bitmap")
{
    //  Two by one: red then blue.
    surface::DeckPixels wide;
    wide.width = 2;
    wide.height = 1;
    wide.rgb = { 255, 0, 0, 0, 0, 255 };

    const auto turned = surface::deckTransformed (wide, 1, false, false);
    CHECK (turned.width == 1);
    CHECK (turned.height == 2);
    CHECK (turned.rgb == std::vector<std::uint8_t> { 255, 0, 0, 0, 0, 255 });    // red on top

    const auto half = surface::deckTransformed (wide, 2, false, false);
    CHECK (half.rgb == std::vector<std::uint8_t> { 0, 0, 255, 255, 0, 0 });
    CHECK (surface::deckTransformed (wide, 0, true, true).rgb == half.rgb);       // both mirrors are a half turn
    CHECK (surface::deckTransformed (wide, 3, false, false).rgb == std::vector<std::uint8_t> { 0, 0, 255, 255, 0, 0 });

    //  Three by two, a quarter turn: column by column from the bottom up.
    surface::DeckPixels grid;
    grid.width = 3;
    grid.height = 2;

    for (std::uint8_t at = 0; at < 6; ++at)
        grid.rgb.insert (grid.rgb.end(), { at, 0, 0 });

    const auto quarter = surface::deckTransformed (grid, 1, false, false);
    REQUIRE (quarter.width == 2);
    REQUIRE (quarter.height == 3);
    std::vector<std::uint8_t> reds;

    for (std::size_t at = 0; at < quarter.rgb.size(); at += 3)
        reds.push_back (quarter.rgb[at]);

    CHECK (reds == std::vector<std::uint8_t> { 3, 0, 4, 1, 5, 2 });

    //  A key of the Original: the header Elgato's own blank key carries.
    surface::DeckPixels key;
    key.width = 72;
    key.height = 72;
    key.rgb.assign (72 * 72 * 3, 0);
    key.rgb[0] = 10;            // the top left pixel's red
    key.rgb[1] = 20;
    key.rgb[2] = 30;

    const auto bmp = surface::deckBmp (key);
    REQUIRE (bmp.size() == 15606);
    const DeckBytes header { 0x42, 0x4d, 0xf6, 0x3c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00,
                             0x28, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00, 0x00, 0x01, 0x00,
                             0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x3c, 0x00, 0x00, 0xc4, 0x0e, 0x00, 0x00,
                             0xc4, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    CHECK (DeckBytes (bmp.begin(), bmp.begin() + 54) == header);

    //  The top row is written last, blue first.
    const auto topLeft = 54 + 71 * 72 * 3;
    CHECK (bmp[topLeft] == 30);
    CHECK (bmp[topLeft + 1] == 20);
    CHECK (bmp[topLeft + 2] == 10);

    //  A row is padded to four bytes.
    surface::DeckPixels one;
    one.width = 1;
    one.height = 2;
    one.rgb = { 1, 2, 3, 4, 5, 6 };
    const auto small = surface::deckBmp (one);
    CHECK (small.size() == 54 + 8);
    CHECK (DeckBytes (small.begin() + 54, small.end()) == DeckBytes { 6, 5, 4, 0, 3, 2, 1, 0 });

    CHECK (surface::deckBmp ({}).empty());
}
