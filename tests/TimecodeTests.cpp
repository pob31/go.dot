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
    Timecode rates and labels, on their own (namespace draft §59, AHF, AHG):
    what a label is, which frame it names at each rate, and the drop-frame rule
    the whole of 29.97 drop-frame turns on. Then TC.2's two pure pieces (AHJ,
    AHO): the 80 bits of an LTC frame, the bytes MTC sends, and the sample each
    is due on.

    The expected labels are written out by hand from the rule as SMPTE 12M
    states it - :00 and :01 left out at every minute not divisible by ten - and
    not computed by the code under test. The same goes for the LTC words and
    the MTC bytes: the golden vectors below were worked out by hand from the
    standard's table (SMPTE 12M as Wikipedia's "Linear timecode" and libltc's
    `LTCFrame` describe it, and the MIDI Time Code specification), and the
    sweeps read them back with readers of the tests' own, written from the
    table and not from the encoder, so a later decoder (TC.3) is checked
    against the standard and not against itself. The due samples were computed
    in exact integers outside the code. Nothing here reads a clock or a locale;
    it runs in both locales because a label is text a show stores.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/timecode/FrameClock.h>
#include <wfg/engine/timecode/LtcEncoder.h>
#include <wfg/engine/timecode/MtcSchedule.h>
#include <wfg/engine/timecode/Timecode.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace wfg::timecode;

namespace
{
    constexpr std::array<Rate, 6> everyRate { Rate::fps23976, Rate::fps24, Rate::fps25,
                                              Rate::fps2997df, Rate::fps2997, Rate::fps30 };

    constexpr std::array<std::int64_t, 4> everySampleRate { 44100, 48000, 96000, 192000 };

    Label label (int hours, int minutes, int seconds, int frames)
    {
        return Label { hours, minutes, seconds, frames };
    }

    std::string text (const Label& value)
    {
        return formatLabel (value, Rate::fps25);
    }

    //  Bytes as the MIDI specification writes them, `F0 7F ...`.
    template <std::size_t N>
    std::string hex (const std::array<std::uint8_t, N>& bytes, std::size_t size = N)
    {
        static constexpr char digits[] = "0123456789ABCDEF";
        std::string out;

        for (std::size_t n = 0; n < size && n < N; ++n)
        {
            if (! out.empty())
                out += ' ';

            out += digits[bytes[n] >> 4];
            out += digits[bytes[n] & 0x0F];
        }

        return out;
    }

    std::string hex (const MtcMessage& message)
    {
        return hex (message.bytes, static_cast<std::size_t> (message.size));
    }

    /*  THE TESTS' OWN LTC READER, from the table and nothing else: bit `n` is
        bit `n % 8` of byte `n / 8`, a field is read least significant bit
        first. */
    bool bitAt (const LtcBits& bits, std::size_t n)
    {
        return ((bits[n / 8] >> (n % 8)) & 1) != 0;
    }

    int field (const LtcBits& bits, std::size_t first, std::size_t width)
    {
        int value = 0;

        for (std::size_t b = 0; b < width; ++b)
            if (bitAt (bits, first + b))
                value |= 1 << b;

        return value;
    }

    //  The numbers of the bits set, in order: `0 3 9 ...`.
    std::string onesOf (const LtcBits& bits)
    {
        std::string out;

        for (std::size_t n = 0; n < 80; ++n)
            if (bitAt (bits, n))
                out += (out.empty() ? "" : " ") + std::to_string (n);

        return out;
    }

    const std::string syncOnes = "66 67 68 69 70 71 72 73 74 75 76 77 79";

    Label ltcLabelOf (const LtcBits& bits)
    {
        return Label { field (bits, 48, 4) + 10 * field (bits, 56, 2), field (bits, 32, 4) + 10 * field (bits, 40, 3),
                       field (bits, 16, 4) + 10 * field (bits, 24, 3), field (bits, 0, 4) + 10 * field (bits, 8, 2) };
    }

    /*  THE TESTS' OWN MTC READER: a label and a rate code out of eight quarter
        frames' data bytes, pieces 0 to 7 in order, from the specification's
        layout - frames, seconds, minutes, hours, low nibble then high, the
        rate in bits 1-2 of the last. -1 for a piece out of place or a bit set
        that the layout keeps clear. */
    struct Heard
    {
        Label label;
        int code = -1;
    };

    Heard readCycle (const std::array<std::uint8_t, 8>& data)
    {
        std::array<int, 8> nibble {};

        for (std::size_t piece = 0; piece < 8; ++piece)
        {
            if (static_cast<std::size_t> (data[piece] >> 4) != piece)
                return {};

            nibble[piece] = data[piece] & 0x0F;
        }

        if ((nibble[1] & 0x0E) != 0 || (nibble[3] & 0x0C) != 0 || (nibble[5] & 0x0C) != 0 || (nibble[7] & 0x08) != 0)
            return {};

        Heard heard;
        heard.label.frames = nibble[0] | (nibble[1] << 4);
        heard.label.seconds = nibble[2] | (nibble[3] << 4);
        heard.label.minutes = nibble[4] | (nibble[5] << 4);
        heard.label.hours = nibble[6] | ((nibble[7] & 1) << 4);
        heard.code = nibble[7] >> 1;
        return heard;
    }

    Heard readFullFrame (const std::array<std::uint8_t, mtcFullFrameSize>& bytes)
    {
        if (bytes[0] != 0xF0 || bytes[1] != 0x7F || bytes[2] != 0x7F || bytes[3] != 0x01 || bytes[4] != 0x01
              || bytes[9] != 0xF7)
            return {};

        Heard heard;
        heard.label = Label { bytes[5] & 0x1F, bytes[6], bytes[7], bytes[8] };
        heard.code = (bytes[5] >> 5) & 0x03;
        return heard;
    }

    std::vector<MtcMessage> take (MtcCursor& cursor, int count)
    {
        std::vector<MtcMessage> out;

        for (int n = 0; n < count; ++n)
        {
            out.push_back (cursor.next());
            cursor.advance();
        }

        return out;
    }

    //  Eight quarter frames from `first` on, read as one cycle.
    Heard cycleFrom (const std::vector<MtcMessage>& messages, std::size_t first)
    {
        std::array<std::uint8_t, 8> data {};

        for (std::size_t piece = 0; piece < 8; ++piece)
        {
            const auto& message = messages.at (first + piece);

            if (message.size != mtcQuarterFrameSize || message.bytes[0] != 0xF1)
                return {};

            data[piece] = message.bytes[1];
        }

        return readCycle (data);
    }
}

TEST_CASE ("timecode: the six rates, their fractions, their counts and their words")
{
    CHECK (numOf (Rate::fps23976) == 24000);
    CHECK (denOf (Rate::fps23976) == 1001);
    CHECK (numOf (Rate::fps24) == 24);
    CHECK (denOf (Rate::fps24) == 1);
    CHECK (numOf (Rate::fps25) == 25);
    CHECK (denOf (Rate::fps25) == 1);
    CHECK (numOf (Rate::fps2997df) == 30000);
    CHECK (denOf (Rate::fps2997df) == 1001);
    CHECK (numOf (Rate::fps2997) == 30000);
    CHECK (denOf (Rate::fps2997) == 1001);
    CHECK (numOf (Rate::fps30) == 30);
    CHECK (denOf (Rate::fps30) == 1);

    const std::array<int, 6> nominal { 24, 24, 25, 30, 30, 30 };
    const std::array<int, 6> mtc { 0, 0, 1, 2, 3, 3 };
    const std::array<const char*, 6> words { "23.976", "24", "25", "29.97df", "29.97", "30" };

    for (std::size_t n = 0; n < everyRate.size(); ++n)
    {
        INFO ("rate " << words[n]);
        CHECK (nominalOf (everyRate[n]) == nominal[n]);
        CHECK (mtcCodeOf (everyRate[n]) == mtc[n]);
        CHECK (dropsFrames (everyRate[n]) == (everyRate[n] == Rate::fps2997df));
        CHECK (wordOf (everyRate[n]) == words[n]);
        REQUIRE (rateFromWord (words[n]).has_value());
        CHECK (*rateFromWord (words[n]) == everyRate[n]);
    }

    CHECK_FALSE (rateFromWord ("29,97").has_value());
    CHECK_FALSE (rateFromWord ("").has_value());
    CHECK_FALSE (rateFromWord ("50").has_value());
}

TEST_CASE ("timecode: a label reads with colons, a semicolon or a full stop before its frames")
{
    REQUIRE (parseLabel ("01:00:00:00").has_value());
    CHECK (*parseLabel ("01:00:00:00") == label (1, 0, 0, 0));
    CHECK (*parseLabel ("23:59:59:29") == label (23, 59, 59, 29));
    CHECK (*parseLabel ("10:20:30;15") == label (10, 20, 30, 15));
    CHECK (*parseLabel ("10:20:30.15") == label (10, 20, 30, 15));

    //  A drop-frame display's own spelling, semicolons all through.
    REQUIRE (parseLabel ("01;00;00;00").has_value());
    CHECK (*parseLabel ("01;00;00;00") == label (1, 0, 0, 0));

    //  Written back with colons whatever it was read with (AHF).
    CHECK (formatLabel (*parseLabel ("01;02;03;04"), Rate::fps2997df) == "01:02:03:04");
    CHECK (formatLabel (*parseLabel ("01;02;03;04"), Rate::fps2997df, true) == "01:02:03;04");
    CHECK (formatLabel (*parseLabel ("01;02;03;04"), Rate::fps25, true) == "01:02:03:04");
}

TEST_CASE ("timecode: what is not a label is refused, never guessed at")
{
    for (const auto* bad : { "", "1:2:3", "1:2:3:4", "01:00:00", "01:00:00:00:00", "25:00:00:00",
                             "24:00:00:00", "01:60:00:00", "01:00:60:00", "01:00:00:30",
                             "01-00-00-00", "01:00.00:00", "0a:00:00:00", " 1:00:00:00",
                             "01:00:00:00 ", "-1:00:00:00", "01:00:00:5" })
    {
        INFO ("text: \"" << bad << "\"");
        CHECK_FALSE (parseLabel (bad).has_value());
    }
}

TEST_CASE ("timecode: every frame of a day round-trips through its label at every rate")
{
    for (const auto rate : everyRate)
    {
        INFO ("rate " << std::string (wordOf (rate)));
        const auto day = framesPerDay (rate);

        /*  Every frame of the first ten minutes and a half - two drop-frame
            blocks and the dropping minutes inside them - and then every 997th
            frame to the end of the day, which crosses every hour. */
        for (std::int64_t frame = 0; frame < 19000; ++frame)
        {
            const auto named = labelOf (frame, rate);
            REQUIRE (frameOf (named, rate) == frame);
            REQUIRE (normalise (named, rate) == named);
        }

        for (std::int64_t frame = 19000; frame < day; frame += 997)
            REQUIRE (frameOf (labelOf (frame, rate), rate) == frame);

        CHECK (frameOf (labelOf (day - 1, rate), rate) == day - 1);
    }
}

TEST_CASE ("timecode: the non-drop rates count every label")
{
    CHECK (framesPerDay (Rate::fps25) == 86400 * 25);
    CHECK (framesPerDay (Rate::fps24) == 86400 * 24);
    CHECK (framesPerDay (Rate::fps23976) == 86400 * 24);
    CHECK (framesPerDay (Rate::fps2997) == 86400 * 30);
    CHECK (framesPerDay (Rate::fps30) == 86400 * 30);

    CHECK (frameOf (label (1, 0, 0, 0), Rate::fps25) == 90000);
    CHECK (text (labelOf (90000 + 25 * 61 + 7, Rate::fps25)) == "01:01:01:07");
    CHECK (frameOf (label (0, 1, 0, 0), Rate::fps2997) == 1800);
    CHECK (formatLabel (labelOf (1800, Rate::fps2997), Rate::fps2997, true) == "00:01:00:00");
}

TEST_CASE ("timecode: drop-frame leaves out :00 and :01 at every minute not divisible by ten")
{
    constexpr auto df = Rate::fps2997df;

    //  00:00:59;29 + one frame = 00:01:00;02.
    const auto beforeMinute = frameOf (label (0, 0, 59, 29), df);
    CHECK (beforeMinute == 1799);
    CHECK (labelOf (beforeMinute + 1, df) == label (0, 1, 0, 2));
    CHECK (formatLabel (labelOf (beforeMinute + 1, df), df, true) == "00:01:00;02");

    //  00:09:59;29 + one frame = 00:10:00;00, which keeps its :00.
    const auto beforeTen = frameOf (label (0, 9, 59, 29), df);
    CHECK (beforeTen == 17981);
    CHECK (labelOf (beforeTen + 1, df) == label (0, 10, 0, 0));
    CHECK (labelOf (beforeTen + 2, df) == label (0, 10, 0, 1));
    CHECK (labelOf (beforeTen + 3, df) == label (0, 10, 0, 2));

    //  A ten-minute block holds 17982 frames, a dropping minute 1798.
    CHECK (frameOf (label (0, 10, 0, 0), df) == 17982);
    CHECK (frameOf (label (0, 2, 0, 2), df) - frameOf (label (0, 1, 0, 2), df) == 1798);
    CHECK (frameOf (label (1, 0, 0, 0), df) == 6 * 17982);
    CHECK (framesPerDay (df) == 24 * 6 * 17982);

    //  And the label the rate lacks reads as the next one (AHF).
    CHECK (normalise (label (0, 1, 0, 0), df) == label (0, 1, 0, 2));
    CHECK (normalise (label (0, 1, 0, 1), df) == label (0, 1, 0, 2));
    CHECK (normalise (label (0, 10, 0, 0), df) == label (0, 10, 0, 0));
    CHECK (normalise (label (0, 1, 1, 0), df) == label (0, 1, 1, 0));
    CHECK (frameOf (label (0, 1, 0, 0), df) == frameOf (label (0, 1, 0, 2), df));
}

TEST_CASE ("timecode: frames past a rate's count carry into the next second")
{
    CHECK (normalise (label (0, 0, 10, 27), Rate::fps25) == label (0, 0, 11, 0));
    CHECK (normalise (label (0, 0, 59, 24), Rate::fps24) == label (0, 1, 0, 0));
    CHECK (normalise (label (0, 0, 59, 24), Rate::fps25) == label (0, 0, 59, 24));
    CHECK (normalise (label (23, 59, 59, 27), Rate::fps25) == label (0, 0, 0, 0));

    //  Carried into a dropping minute, and then to its :02.
    CHECK (normalise (label (0, 0, 59, 29), Rate::fps2997df) == label (0, 0, 59, 29));
    CHECK (normalise (label (0, 0, 59, 29), Rate::fps24) == label (0, 1, 0, 0));
}

TEST_CASE ("timecode: the day wraps at 24 hours, both ways")
{
    for (const auto rate : everyRate)
    {
        INFO ("rate " << std::string (wordOf (rate)));
        const auto day = framesPerDay (rate);
        const auto last = nominalOf (rate) - 1;

        CHECK (labelOf (day, rate) == label (0, 0, 0, 0));
        CHECK (labelOf (day + 5, rate) == labelOf (5, rate));
        CHECK (labelOf (-1, rate) == label (23, 59, 59, last));
        CHECK (labelOf (day - 1, rate) == label (23, 59, 59, last));
        CHECK (frameOf (label (23, 59, 59, last), rate) == day - 1);

        //  The frame after the last of the day is the first.
        CHECK (labelOf (frameOf (label (23, 59, 59, last), rate) + 1, rate) == label (0, 0, 0, 0));
    }
}

//==============================================================================
//  LTC: the 80 bits of a frame (TC.2)

TEST_CASE ("timecode: LTC 00:00:00:00 at 25 is the sync word and the polarity bit at 59")
{
    const auto bits = ltcBits (label (0, 0, 0, 0), Rate::fps25);

    //  Thirteen ones in the sync word, so bit 59 makes them fourteen.
    CHECK (onesOf (bits) == "59 " + syncOnes);
    CHECK (hex (bits) == "00 00 00 00 00 00 00 08 FC BF");
    CHECK (onesIn (bits) == 14);
    CHECK (transitionsIn (bits) == 94);
}

TEST_CASE ("timecode: LTC 00:00:00:00 at 30 puts its polarity bit at 27")
{
    const auto bits = ltcBits (label (0, 0, 0, 0), Rate::fps30);

    CHECK (onesOf (bits) == "27 " + syncOnes);
    CHECK (hex (bits) == "00 00 00 08 00 00 00 00 FC BF");
}

TEST_CASE ("timecode: LTC 01:00:00:00 at 25 sets the hour's unit and needs no polarity bit")
{
    const auto bits = ltcBits (label (1, 0, 0, 0), Rate::fps25);

    CHECK (onesOf (bits) == "48 " + syncOnes);
    CHECK (hex (bits) == "00 00 00 00 00 00 01 00 FC BF");
    CHECK_FALSE (ltcBit (bits, 59));
    CHECK (ltcBit (bits, 48));
}

TEST_CASE ("timecode: LTC 23:59:59;29 at 29.97 drop-frame, every field full")
{
    const auto bits = ltcBits (label (23, 59, 59, 29), Rate::fps2997df);

    //  Fifteen ones in the fields and the drop flag, thirteen in the sync word.
    CHECK (onesOf (bits) == "0 3 9 10 16 19 24 26 32 35 40 42 48 49 57 " + syncOnes);
    CHECK (hex (bits) == "09 06 09 05 09 05 03 02 FC BF");
    CHECK_FALSE (ltcBit (bits, 27));
    CHECK (onesIn (bits) == 28);
    CHECK (transitionsIn (bits) == 108);

    //  The same label at 29.97 non-drop lacks only the flag, and so gains the
    //  polarity bit.
    CHECK (onesOf (ltcBits (label (23, 59, 59, 29), Rate::fps2997))
             == "0 3 9 16 19 24 26 27 32 35 40 42 48 49 57 " + syncOnes);
}

TEST_CASE ("timecode: every LTC frame is even, carries its sync word and reads back to its label")
{
    for (const auto rate : everyRate)
    {
        INFO ("rate " << std::string (wordOf (rate)));
        const bool twentyFive = rate == Rate::fps25;
        const std::size_t polarity = twentyFive ? 59u : 27u;
        const auto day = framesPerDay (rate);

        const auto check = [&] (std::int64_t frame)
        {
            const auto named = labelOf (frame, rate);
            const auto bits = ltcBits (named, rate);
            INFO ("label " << formatLabel (named, rate, true));

            //  Even, by the tests' own count and by the encoder's.
            int ones = 0;

            for (std::size_t n = 0; n < 80; ++n)
                ones += bitAt (bits, n) ? 1 : 0;

            REQUIRE (ones % 2 == 0);
            REQUIRE (onesIn (bits) == ones);
            REQUIRE (transitionsIn (bits) == 80 + ones);

            //  The sync word, then the fields, then every flag the table keeps
            //  clear: user bits, colour frame, the group flags.
            REQUIRE (bits[8] == 0xFC);
            REQUIRE (bits[9] == 0xBF);
            REQUIRE (ltcLabelOf (bits) == named);
            REQUIRE (bitAt (bits, 10) == dropsFrames (rate));
            REQUIRE_FALSE (bitAt (bits, 11));

            int userBits = 0;

            for (const std::size_t user : { 4u, 12u, 20u, 28u, 36u, 44u, 52u, 60u })
                userBits |= field (bits, user, 4);

            REQUIRE (userBits == 0);
            REQUIRE_FALSE (bitAt (bits, 43));
            REQUIRE_FALSE (bitAt (bits, 58));
            REQUIRE_FALSE (bitAt (bits, twentyFive ? 27u : 59u));

            //  The polarity bit is set exactly when the rest are odd.
            REQUIRE (bitAt (bits, polarity) == ((ones - (bitAt (bits, polarity) ? 1 : 0)) % 2 == 1));

            bool same = true;

            for (int n = 0; n < 80; ++n)
                same = same && ltcBit (bits, n) == bitAt (bits, static_cast<std::size_t> (n));

            REQUIRE (same);
        };

        /*  Every frame of the first minute and more - past the first minute
            that drops - and round the first ten-minute mark, then a stride
            through the day that crosses every hour, and the day's last frame. */
        for (std::int64_t frame = 0; frame < 2000; ++frame)
            check (frame);

        for (std::int64_t frame = 17950; frame < 18050; ++frame)
            check (frame);

        for (std::int64_t frame = 18050; frame < day; frame += 997)
            check (frame);

        check (day - 1);
    }

    CHECK_FALSE (ltcBit (ltcBits (label (0, 0, 0, 0), Rate::fps25), -1));
    CHECK_FALSE (ltcBit (ltcBits (label (0, 0, 0, 0), Rate::fps25), 80));
}

TEST_CASE ("timecode: an LTC label the rate lacks is sent as the next one there is")
{
    CHECK (ltcBits (label (0, 1, 0, 0), Rate::fps2997df) == ltcBits (label (0, 1, 0, 2), Rate::fps2997df));
    CHECK (ltcBits (label (0, 0, 10, 27), Rate::fps25) == ltcBits (label (0, 0, 11, 0), Rate::fps25));
}

//==============================================================================
//  MTC: the bytes (TC.2)

TEST_CASE ("timecode: MTC full frame and quarter frames for 01:00:00:00 at 25")
{
    const auto at = label (1, 0, 0, 0);

    CHECK (hex (fullFrame (at, Rate::fps25)) == "F0 7F 7F 01 01 21 00 00 00 F7");

    const std::array<const char*, 8> pieces { "F1 00", "F1 10", "F1 20", "F1 30",
                                              "F1 40", "F1 50", "F1 61", "F1 72" };

    for (int piece = 0; piece < 8; ++piece)
    {
        INFO ("piece " << piece);
        CHECK (hex (quarterFrame (at, Rate::fps25, piece)) == pieces[static_cast<std::size_t> (piece)]);
    }

    CHECK (quarterNibble (at, Rate::fps25, 6) == 0x1);
    CHECK (quarterNibble (at, Rate::fps25, 7) == 0x2);
}

TEST_CASE ("timecode: MTC 23:59:59;29 at 29.97 drop-frame, every field full")
{
    const auto at = label (23, 59, 59, 29);

    //  The hours byte: code 2 in its top bits beside 23, 0x40 | 0x17.
    CHECK (hex (fullFrame (at, Rate::fps2997df)) == "F0 7F 7F 01 01 57 3B 3B 1D F7");

    const std::array<const char*, 8> pieces { "F1 0D", "F1 11", "F1 2B", "F1 33",
                                              "F1 4B", "F1 53", "F1 67", "F1 75" };

    for (int piece = 0; piece < 8; ++piece)
    {
        INFO ("piece " << piece);
        CHECK (hex (quarterFrame (at, Rate::fps2997df, piece)) == pieces[static_cast<std::size_t> (piece)]);
    }

    //  A piece number past 7 is taken modulo 8.
    CHECK (quarterFrame (at, Rate::fps2997df, 15) == quarterFrame (at, Rate::fps2997df, 7));
}

TEST_CASE ("timecode: MTC's rate codes, 23.976 sent as 24 and 29.97 non-drop as 30")
{
    const std::array<const char*, 6> hours { "00", "00", "20", "40", "60", "60" };
    const std::array<const char*, 6> lastPiece { "F1 70", "F1 70", "F1 72", "F1 74", "F1 76", "F1 76" };

    for (std::size_t n = 0; n < everyRate.size(); ++n)
    {
        INFO ("rate " << std::string (wordOf (everyRate[n])));
        CHECK (hex (fullFrame (label (0, 0, 0, 0), everyRate[n])) == "F0 7F 7F 01 01 " + std::string (hours[n]) + " 00 00 00 F7");
        CHECK (hex (quarterFrame (label (0, 0, 0, 0), everyRate[n], 7)) == lastPiece[n]);
    }
}

TEST_CASE ("timecode: every MTC label reads back from its full frame and from its eight pieces")
{
    for (const auto rate : everyRate)
    {
        INFO ("rate " << std::string (wordOf (rate)));
        const auto day = framesPerDay (rate);

        for (std::int64_t frame = 0; frame < day; frame += frame < 2000 ? 1 : 997)
        {
            const auto named = labelOf (frame, rate);
            INFO ("label " << formatLabel (named, rate, true));

            const auto whole = readFullFrame (fullFrame (named, rate));
            REQUIRE (whole.label == named);
            REQUIRE (whole.code == mtcCodeOf (rate));

            std::array<std::uint8_t, 8> data {};

            for (int piece = 0; piece < 8; ++piece)
            {
                const auto message = quarterFrame (named, rate, piece);
                REQUIRE (message[0] == 0xF1);
                REQUIRE ((message[1] & 0x0F) == quarterNibble (named, rate, piece));
                data[static_cast<std::size_t> (piece)] = message[1];
            }

            const auto pieces = readCycle (data);
            REQUIRE (pieces.label == named);
            REQUIRE (pieces.code == mtcCodeOf (rate));
        }
    }
}

//==============================================================================
//  The sample clock: when each quarter and each frame is due (TC.2)

TEST_CASE ("timecode: at 25 and 48 kHz a quarter frame every 480 samples, a frame every 1920")
{
    for (std::int64_t j = 0; j <= 4000; ++j)
        REQUIRE (quarterDueSample (j, 48000, Rate::fps25) == 480 * j);

    for (std::int64_t k = 0; k <= 1000; ++k)
        REQUIRE (frameStartSample (k, 48000, Rate::fps25) == 1920 * k);
}

TEST_CASE ("timecode: at 29.97 and 48 kHz each quarter is due on the first sample at or after its moment")
{
    //  400.4 samples a quarter: ceil (j * 48000 * 1001 / 120000).
    const std::array<std::int64_t, 13> quarters { 0, 401, 801, 1202, 1602, 2002, 2403, 2803, 3204, 3604, 4004, 4405, 4805 };
    const std::array<std::int64_t, 4> frames { 0, 1602, 3204, 4805 };

    for (const auto rate : { Rate::fps2997df, Rate::fps2997 })
    {
        INFO ("rate " << std::string (wordOf (rate)));

        for (std::size_t j = 0; j < quarters.size(); ++j)
            CHECK (quarterDueSample (static_cast<std::int64_t> (j), 48000, rate) == quarters[j]);

        for (std::size_t k = 0; k < frames.size(); ++k)
            CHECK (frameStartSample (static_cast<std::int64_t> (k), 48000, rate) == frames[k]);
    }
}

TEST_CASE ("timecode: at 24 and 44.1 kHz the quarters fall between samples, and are rounded up")
{
    //  459.375 samples a quarter, 1837.5 a frame.
    const std::array<std::int64_t, 13> quarters { 0, 460, 919, 1379, 1838, 2297, 2757, 3216, 3675, 4135, 4594, 5054, 5513 };
    const std::array<std::int64_t, 4> frames { 0, 1838, 3675, 5513 };

    for (std::size_t j = 0; j < quarters.size(); ++j)
        CHECK (quarterDueSample (static_cast<std::int64_t> (j), 44100, Rate::fps24) == quarters[j]);

    for (std::size_t k = 0; k < frames.size(); ++k)
        CHECK (frameStartSample (static_cast<std::int64_t> (k), 44100, Rate::fps24) == frames[k]);

    //  23.976 at 44.1 kHz: 459.834375 a quarter.
    const std::array<std::int64_t, 9> slower { 0, 460, 920, 1380, 1840, 2300, 2760, 3219, 3679 };

    for (std::size_t j = 0; j < slower.size(); ++j)
        CHECK (quarterDueSample (static_cast<std::int64_t> (j), 44100, Rate::fps23976) == slower[j]);
}

TEST_CASE ("timecode: a frame starts on its fourth quarter, its 160th half-bit, and the sample that reads as it")
{
    for (const auto rate : everyRate)
        for (const auto sampleRate : everySampleRate)
        {
            INFO ("rate " << std::string (wordOf (rate)) << " at " << sampleRate);
            const auto num = numOf (rate);
            const auto den = denOf (rate);
            std::int64_t before = -1;

            for (std::int64_t k = 0; k <= 1000; ++k)
            {
                const auto start = frameStartSample (k, sampleRate, rate);

                //  One rule for frames, quarters and LTC's half-bits.
                REQUIRE (quarterDueSample (4 * k, sampleRate, rate) == start);
                REQUIRE (partStartSample (160 * k, 160, sampleRate, rate) == start);
                REQUIRE (start > before);
                before = start;

                //  The first sample at or after the frame's exact moment,
                //  checked by the products themselves, which fit here.
                REQUIRE (start * num >= k * sampleRate * den);
                REQUIRE ((start - 1) * num < k * sampleRate * den);

                //  And the inverse: the start reads as the frame, the sample
                //  before as the one before it.
                REQUIRE (frameAtSample (start, sampleRate, rate) == k);
                REQUIRE (frameAtSample (start - 1, sampleRate, rate) == k - 1);
                REQUIRE (firstFrameAtOrAfter (start, sampleRate, rate) == k);
                REQUIRE (firstFrameAtOrAfter (start + 1, sampleRate, rate) == k + 1);
                REQUIRE (partAtSample (start, 160, sampleRate, rate) == 160 * k);
            }

            //  Every quarter in the first second and a half, by the same test.
            for (std::int64_t j = 0; j < 4 * 45; ++j)
            {
                const auto due = quarterDueSample (j, sampleRate, rate);
                REQUIRE (due * 4 * num >= j * sampleRate * den);
                REQUIRE ((due - 1) * 4 * num < j * sampleRate * den);
                REQUIRE (partAtSample (due, 4, sampleRate, rate) == j);
            }
        }

    //  Before the run's zero, a part below nought; nothing at all with no rate.
    CHECK (frameAtSample (-1, 48000, Rate::fps25) == -1);
    CHECK (frameAtSample (-1920, 48000, Rate::fps25) == -1);
    CHECK (frameAtSample (-1921, 48000, Rate::fps25) == -2);
    CHECK (firstFrameAtOrAfter (-5, 48000, Rate::fps25) == 0);
    CHECK (frameStartSample (10, 0, Rate::fps25) == 0);
    CHECK (frameAtSample (10, 0, Rate::fps25) == 0);
}

TEST_CASE ("timecode: a day at 192 kHz is exact, and so is a count far past where a plain product overflows")
{
    //  A day's quarters at the 30 family's count: 4 * 30 * 86400.
    constexpr std::int64_t quartersInADay = 4 * 30 * 86400;

    CHECK (quarterDueSample (quartersInADay, 192000, Rate::fps30) == 16588800000);
    CHECK (quarterDueSample (quartersInADay - 1, 192000, Rate::fps30) == 16588798400);
    CHECK (quarterDueSample (quartersInADay, 192000, Rate::fps2997) == 16605388800);
    CHECK (quarterDueSample (quartersInADay - 1, 192000, Rate::fps2997) == 16605387199);
    CHECK (quarterDueSample (quartersInADay, 192000, Rate::fps23976) == 20756736000);
    CHECK (quarterDueSample (quartersInADay - 1, 192000, Rate::fps23976) == 20756733998);
    CHECK (quarterDueSample (quartersInADay, 192000, Rate::fps25) == 19906560000);
    CHECK (quarterDueSample (quartersInADay - 1, 192000, Rate::fps25) == 19906558080);
    CHECK (frameStartSample (30 * 86400 - 1, 192000, Rate::fps2997) == 16605382394);

    //  The frame a drop-frame day wraps on.
    CHECK (frameStartSample (framesPerDay (Rate::fps2997df), 192000, Rate::fps2997df) == 16588783412);
    CHECK (frameAtSample (16588783412, 192000, Rate::fps2997df) == framesPerDay (Rate::fps2997df));
    CHECK (frameAtSample (16588783411, 192000, Rate::fps2997df) == framesPerDay (Rate::fps2997df) - 1);

    /*  A count whose plain product, j * 192000 * 1001, is 1.9e20 - far past
        what 63 bits hold - placed exactly; and the clock's period holds a
        million periods out: 4 * num quarters are sampleRate * den samples. */
    CHECK (quarterDueSample (1000000000007, 192000, Rate::fps2997df) == 1601600000011212);

    for (const auto rate : everyRate)
    {
        INFO ("rate " << std::string (wordOf (rate)));
        const auto period = 4 * numOf (rate);
        const auto samples = 192000 * denOf (rate);

        for (std::int64_t j = 0; j < 400; j += 7)
            REQUIRE (quarterDueSample (j + period * 1000000, 192000, rate)
                       == quarterDueSample (j, 192000, rate) + samples * 1000000);
    }
}

//==============================================================================
//  MTC: the cursor - what is due next, in order (TC.2)

TEST_CASE ("timecode: an MTC run sends a full frame, then cycles of eight on their quarters' samples")
{
    constexpr std::int64_t zero = 1000;
    const auto start = frameOf (label (1, 0, 0, 0), Rate::fps25);
    MtcCursor cursor (Rate::fps25, 48000, start, zero);

    CHECK (cursor.fullFrameFirst());
    const auto messages = take (cursor, 1 + 24);

    //  The full frame first, on the run's first sample.
    REQUIRE (messages[0].isFullFrame());
    CHECK (messages[0].dueSample == zero);
    CHECK (hex (messages[0]) == "F0 7F 7F 01 01 21 00 00 00 F7");

    //  Then every quarter on its sample, piece 0 included on the same one.
    for (std::size_t j = 0; j < 24; ++j)
    {
        INFO ("quarter " << j);
        CHECK (messages[1 + j].size == mtcQuarterFrameSize);
        CHECK (messages[1 + j].dueSample == zero + 480 * static_cast<std::int64_t> (j));
        const int piece = messages[1 + j].bytes[1] >> 4;
        CHECK (piece == static_cast<int> (j % 8));
    }

    CHECK (hex (messages[1]) == "F1 00");
    CHECK (hex (messages[8]) == "F1 72");

    //  Each cycle encodes the frame its piece 0 starts: two frames apart.
    CHECK (cycleFrom (messages, 1).label == label (1, 0, 0, 0));
    CHECK (cycleFrom (messages, 9).label == label (1, 0, 0, 2));
    CHECK (cycleFrom (messages, 17).label == label (1, 0, 0, 4));
    CHECK (cycleFrom (messages, 9).code == 1);
    CHECK (messages[9].dueSample == zero + frameStartSample (2, 48000, Rate::fps25));
    CHECK (messages[17].dueSample == zero + frameStartSample (4, 48000, Rate::fps25));

    CHECK_FALSE (cursor.fullFrameFirst());
    CHECK (cursor.quarter() == 24);
    CHECK (cursor.cycleFrame() == start + 6);
    CHECK (cursor.cycleLabel() == label (1, 0, 0, 6));
}

TEST_CASE ("timecode: an MTC cycle at 29.97 drop-frame steps over the names a minute drops")
{
    const auto rate = Rate::fps2997df;
    const auto start = frameOf (label (0, 0, 59, 28), rate);
    MtcCursor cursor (rate, 48000, start, 0);
    const auto messages = take (cursor, 1 + 24);

    CHECK (hex (messages[0]) == "F0 7F 7F 01 01 40 00 3B 1C F7");
    CHECK (cycleFrom (messages, 1).label == label (0, 0, 59, 28));
    CHECK (cycleFrom (messages, 9).label == label (0, 1, 0, 2));
    CHECK (cycleFrom (messages, 17).label == label (0, 1, 0, 4));
    CHECK (cycleFrom (messages, 9).code == 2);

    for (std::size_t j = 0; j < 24; ++j)
        CHECK (messages[1 + j].dueSample == quarterDueSample (static_cast<std::int64_t> (j), 48000, rate));
}

TEST_CASE ("timecode: an MTC run carries on through midnight")
{
    {
        const auto start = frameOf (label (23, 59, 59, 20), Rate::fps25);
        MtcCursor cursor (Rate::fps25, 48000, start, 0);
        const auto messages = take (cursor, 1 + 40);

        CHECK (cycleFrom (messages, 1).label == label (23, 59, 59, 20));
        CHECK (cycleFrom (messages, 9).label == label (23, 59, 59, 22));
        CHECK (cycleFrom (messages, 17).label == label (23, 59, 59, 24));
        CHECK (cycleFrom (messages, 25).label == label (0, 0, 0, 1));
        CHECK (cycleFrom (messages, 33).label == label (0, 0, 0, 3));

        //  The count goes on past the day; only the label wraps.
        CHECK (cursor.cycleFrame() == start + 10);
        CHECK (cursor.cycleLabel() == label (0, 0, 0, 5));
    }

    {
        const auto rate = Rate::fps2997df;
        const auto start = frameOf (label (23, 59, 59, 28), rate);
        MtcCursor cursor (rate, 48000, start, 0);
        const auto messages = take (cursor, 1 + 24);

        //  Midnight is a tenth minute: it keeps its :00.
        CHECK (cycleFrom (messages, 1).label == label (23, 59, 59, 28));
        CHECK (cycleFrom (messages, 9).label == label (0, 0, 0, 0));
        CHECK (cycleFrom (messages, 17).label == label (0, 0, 0, 2));
    }
}

TEST_CASE ("timecode: an MTC cursor re-seated mid-cycle starts at the next piece 0, a full frame first")
{
    const auto rate = Rate::fps2997df;
    const auto start = frameOf (label (1, 0, 0, 0), rate);
    MtcCursor cursor (rate, 48000, start, 5000);

    take (cursor, 1 + 3);
    REQUIRE_FALSE (cursor.fullFrameFirst());
    REQUIRE (cursor.quarter() == 3);

    //  A sample one past the second cycle's start: the third cycle, frame 4.
    cursor.seek (5000 + frameStartSample (2, 48000, rate) + 1, true);

    CHECK (cursor.fullFrameFirst());
    CHECK (cursor.quarter() == 16);
    CHECK (cursor.cycleFrame() == start + 4);

    const auto messages = take (cursor, 1 + 8);
    REQUIRE (messages[0].isFullFrame());
    CHECK (messages[0].dueSample == 5000 + frameStartSample (4, 48000, rate));
    CHECK (readFullFrame (messages[0].bytes).label == label (1, 0, 0, 4));
    CHECK (messages[1].dueSample == messages[0].dueSample);
    CHECK (hex (messages[1]) == "F1 04");
    CHECK (cycleFrom (messages, 1).label == label (1, 0, 0, 4));

    //  On a cycle's own first sample, that cycle; on an odd frame's, the next.
    cursor.seek (5000 + frameStartSample (2, 48000, rate), true);
    CHECK (cursor.quarter() == 8);
    cursor.seek (5000 + frameStartSample (5, 48000, rate), true);
    CHECK (cursor.quarter() == 24);

    //  Without a full frame: the code just picks up at the cycle (AHQ).
    cursor.seek (5000 + frameStartSample (7, 48000, rate) - 1, false);
    CHECK_FALSE (cursor.fullFrameFirst());
    CHECK (cursor.quarter() == 32);
    CHECK (cursor.next().dueSample == 5000 + frameStartSample (8, 48000, rate));
    CHECK (hex (cursor.next()) == "F1 08");

    //  Never before the run's first cycle.
    cursor.seek (0, true);
    CHECK (cursor.quarter() == 0);
    CHECK (cursor.fullFrameFirst());
    CHECK (cursor.next().dueSample == 5000);
}
