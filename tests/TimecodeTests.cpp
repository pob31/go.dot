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

    Last, TC.3's generator (AHJ, AHK, AHL): the sound the audio thread draws,
    rendered block by block as AudioHost renders it and read back by
    LtcReader.h - a reader written from the standard's description of the
    signal that shares no code with the encoder or the generator, so the line
    is judged by what a timecode reader would make of it.

    And TG.3's decoder (namespace draft §60; AIL, AIR): the reading side,
    fed with the generator's line - the reference signal TC.3 already holds
    to the standard - and cross-checked against LtcReader.h. Every frame read
    at every rate and in every block size, each where its bit 0 began to a
    tenth of a sample once the edge filter's constant delay is known; played
    backwards, at half and at twice its speed, quiet, off-centre, inverted and
    under noise; a dropout, a takeover, a stop; the rate told from the frames;
    and the outbox's sixty-four.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "LtcReader.h"

#include <wfg/engine/timecode/FrameClock.h>
#include <wfg/engine/timecode/LtcDecoder.h>
#include <wfg/engine/timecode/LtcEncoder.h>
#include <wfg/engine/timecode/LtcGenerator.h>
#include <wfg/engine/timecode/MtcSchedule.h>
#include <wfg/engine/timecode/Timecode.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
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

//==============================================================================
//  LTC drawn: the generator on the audio thread, read back (TC.3)

namespace
{
    //  -18 dBFS, the show's default level (AHH), as the line's peak.
    const float ltcGain = static_cast<float> (std::pow (10.0, -18.0 / 20.0));

    /*  The one-pole low-pass every edge goes through (AHJ): a 10-90 % rise of
        40 microseconds is a time constant of 40 / 2.197 of them. The step is
        how far towards its input the line moves in one sample. */
    double tauSamples (int sampleRate)
    {
        return static_cast<double> (sampleRate) * 40.0e-6 / 2.197;
    }

    double lowPassStep (int sampleRate)
    {
        return 1.0 - std::exp (-1.0 / tauSamples (sampleRate));
    }

    //  Eight time constants, in whole samples: the line is quiet by then.
    std::int64_t fewTaus (int sampleRate)
    {
        return static_cast<std::int64_t> (std::ceil (8.0 * tauSamples (sampleRate)));
    }

    double framesPerSecondOf (Rate rate)
    {
        return static_cast<double> (numOf (rate)) / static_cast<double> (denOf (rate));
    }

    //  Block sizes as an interface might hand them, a block of one among them.
    const std::vector<int> mixedBlocks { 64, 128, 480, 512, 1, 333, 7 };

    //  What a cue routed onto the LTC channel would have put there.
    constexpr float routedSound = 0.7f;

    /*  WHAT THE LINE CARRIED from `from` for `count` samples, rendered the way
        AudioHost renders it - a block at a time into one channel of a block
        `width` wide - with every channel holding sound before each block, so
        whatever the generator did not replace is seen. */
    struct Line
    {
        std::int64_t from = 0;
        std::vector<float> samples;

        //  Samples of the other channels that no longer hold what they held.
        std::int64_t othersTouched = 0;

        float at (std::int64_t sample) const
        {
            return samples.at (static_cast<std::size_t> (sample - from));
        }

        std::int64_t end() const
        {
            return from + static_cast<std::int64_t> (samples.size());
        }
    };

    Line render (LtcGenerator& generator, std::int64_t from, std::int64_t count,
                 const std::vector<int>& sizes = mixedBlocks, int channel = 1, int width = 2)
    {
        Line line;
        line.from = from;
        line.samples.assign (static_cast<std::size_t> (count), 0.0f);

        const auto largest = static_cast<std::size_t> (*std::max_element (sizes.begin(), sizes.end()));
        std::vector<std::vector<float>> block (static_cast<std::size_t> (width), std::vector<float> (largest));
        std::vector<float*> pointers;

        for (auto& one : block)
            pointers.push_back (one.data());

        std::int64_t at = from;
        std::size_t which = 0;

        while (at < from + count)
        {
            const auto size = std::min<std::int64_t> (sizes[which++ % sizes.size()], from + count - at);

            for (auto& one : block)
                std::fill (one.begin(), one.end(), routedSound);

            generator.render (pointers.data(), width, static_cast<int> (size), at);

            for (std::size_t c = 0; c < block.size(); ++c)
            {
                for (std::size_t n = 0; n < static_cast<std::size_t> (size); ++n)
                {
                    const auto value = block[c][n];

                    if (static_cast<int> (c) == channel)
                        line.samples[static_cast<std::size_t> (at - from) + n] = value;
                    else if (std::fabs (value - routedSound) > 0.0f)
                        ++line.othersTouched;
                }
            }

            at += size;
        }

        return line;
    }

    std::vector<ltcreader::Frame> read (const Line& line, int sampleRate, Rate rate)
    {
        return ltcreader::decode (line.samples.data(), line.samples.size(), static_cast<double> (sampleRate),
                                  framesPerSecondOf (rate), line.from);
    }

    Label heardLabel (const ltcreader::Frame& frame)
    {
        return Label { frame.hours, frame.minutes, frame.seconds, frame.frames };
    }

    //  The first sample that is not nought, or -1.
    std::int64_t firstNonZero (const Line& line)
    {
        for (std::size_t n = 0; n < line.samples.size(); ++n)
            if (std::fabs (line.samples[n]) > 0.0f)
                return line.from + static_cast<std::int64_t> (n);

        return -1;
    }

    //  How many samples from `from` on are not below `level`.
    std::int64_t aboveFrom (const Line& line, std::int64_t from, float level)
    {
        std::int64_t above = 0;

        for (auto sample = std::max (from, line.from); sample < line.end(); ++sample)
            if (! (std::fabs (line.at (sample)) < level))
                ++above;

        return above;
    }

    //  The longest run of samples in [from, to) whose magnitude is below `level`.
    std::int64_t longestBelow (const Line& line, std::int64_t from, std::int64_t to, float level)
    {
        std::int64_t longest = 0, run = 0;

        for (auto sample = from; sample < to; ++sample)
        {
            run = std::fabs (line.at (sample)) < level ? run + 1 : 0;
            longest = std::max (longest, run);
        }

        return longest;
    }

    float peakOf (const Line& line)
    {
        float peak = 0.0f;

        for (const auto value : line.samples)
            peak = std::max (peak, std::fabs (value));

        return peak;
    }

    /*  THAT A RUN READ BACK FRAME BY FRAME: frames `from` to `from + count` of
        what was read are frames `first`, `first + 1`... of a program whose frame
        nought began on `zero` and was called `startFrame` - each with the label
        the rate gives it (the day wrapped), its ones even, the drop-frame flag
        exactly at drop-frame, and its bit 0 within a sample before and four
        after where the one rule puts the frame: the low-pass delays a zero
        crossing by up to 2.4 samples at 192 kHz, and an edge placed early in a
        sample can tip that sample over. */
    struct RunCheck
    {
        int wrong = 0;
        std::string firstWrong;
    };

    RunCheck checkRun (const std::vector<ltcreader::Frame>& frames, std::size_t from, std::size_t count,
                       std::int64_t zero, std::int64_t startFrame, std::int64_t first, int sampleRate, Rate rate)
    {
        RunCheck result;

        for (std::size_t i = 0; i < count; ++i)
        {
            const auto& frame = frames.at (from + i);
            const auto k = first + static_cast<std::int64_t> (i);
            const auto wanted = labelOf (startFrame + k, rate);
            const auto due = zero + frameStartSample (k, sampleRate, rate);
            const auto off = frame.sample - due;

            const auto right = heardLabel (frame) == wanted && frame.evenParity
                                 && frame.dropFrame == dropsFrames (rate) && off >= -1 && off <= 4;

            if (! right && result.wrong++ == 0)
                result.firstWrong = "frame " + std::to_string (k) + ": heard " + formatLabel (heardLabel (frame), rate)
                                      + " at " + std::to_string (frame.sample) + (frame.evenParity ? "" : " (odd)")
                                      + (frame.dropFrame ? " (drop)" : "") + ", wanted " + formatLabel (wanted, rate)
                                      + " at " + std::to_string (due);
        }

        return result;
    }

    using Command = LtcGenerator::Command;

    struct RateAt
    {
        Rate rate;
        int sampleRate;
    };
}

TEST_CASE ("ltc generator: three seconds at every rate and 44.1, 48 and 96 kHz read back frame by frame from the start")
{
    /*  29.97 at 44.1 kHz is the fractional worst case: a half-bit is 9.197
        samples and no edge after the first lands on a sample boundary. */
    for (const auto rate : everyRate)
    {
        for (const int sampleRate : { 44100, 48000, 96000 })
        {
            INFO ("rate " << std::string (wordOf (rate)) << " at " << sampleRate << " Hz");

            LtcGenerator generator;
            generator.prepare (sampleRate);
            generator.setOutput (1, ltcGain);

            constexpr std::int64_t zero = 1000;
            const auto startFrame = frameOf (label (1, 0, 0, 0), rate);
            REQUIRE (generator.post (Command::start (7, zero, startFrame, rate)));

            const auto line = render (generator, 0, zero + 3 * sampleRate);
            const auto frames = read (line, sampleRate, rate);

            //  Every frame the three seconds hold whole; the one the render
            //  ends inside cannot be read.
            const auto whole = static_cast<std::size_t> (frameAtSample (3 * sampleRate, sampleRate, rate));
            REQUIRE (frames.size() + 1 >= whole);
            CHECK (frames.size() <= whole);

            CHECK (frames.front().sample == zero);
            const auto run = checkRun (frames, 0, frames.size(), zero, startFrame, 0, sampleRate, rate);
            INFO (run.firstWrong);
            CHECK (run.wrong == 0);

            CHECK (line.othersTouched == 0);
            CHECK (generator.currentProgram() == 7);
            CHECK (generator.lateCount() == 0);
        }
    }
}

TEST_CASE ("ltc generator: 29.97 drop-frame from 23:59:59;27 reads on through midnight")
{
    const auto rate = Rate::fps2997df;

    for (const int sampleRate : { 44100, 48000, 96000 })
    {
        INFO ("at " << sampleRate << " Hz");

        LtcGenerator generator;
        generator.prepare (sampleRate);
        generator.setOutput (1, ltcGain);

        constexpr std::int64_t zero = 4321;
        const auto startFrame = frameOf (label (23, 59, 59, 27), rate);
        REQUIRE (generator.post (Command::start (1, zero, startFrame, rate)));

        const auto line = render (generator, 0, zero + 3 * sampleRate);
        const auto frames = read (line, sampleRate, rate);

        REQUIRE (frames.size() > 80u);
        CHECK (heardLabel (frames[0]) == label (23, 59, 59, 27));
        CHECK (heardLabel (frames[2]) == label (23, 59, 59, 29));
        CHECK (heardLabel (frames[3]) == label (0, 0, 0, 0));
        CHECK (frames[3].dropFrame);

        const auto run = checkRun (frames, 0, frames.size(), zero, startFrame, 0, sampleRate, rate);
        INFO (run.firstWrong);
        CHECK (run.wrong == 0);
    }
}

TEST_CASE ("ltc generator: the first sample that is not nought is the start's own, mid-block and across a block edge")
{
    /*  Mid-block, a 128-sample block's first sample, its last, and the very
        first sample drawn; in blocks of 128, of one, and mixed. The line holds
        nought before, and its first edge goes up to the level. */
    for (const int sampleRate : { 44100, 48000, 96000, 192000 })
    {
        for (const std::int64_t zero : { std::int64_t { 300 }, std::int64_t { 384 }, std::int64_t { 383 }, std::int64_t { 0 } })
        {
            for (const auto& sizes : { std::vector<int> { 128 }, std::vector<int> { 1 }, mixedBlocks })
            {
                INFO ("at " << sampleRate << " Hz, zero " << zero << ", blocks of " << sizes.front());

                LtcGenerator generator;
                generator.prepare (sampleRate);
                generator.setOutput (1, ltcGain);
                REQUIRE (generator.post (Command::start (1, zero, frameOf (label (1, 0, 0, 0), Rate::fps25), Rate::fps25)));

                const auto line = render (generator, 0, zero + 4000, sizes);

                CHECK (firstNonZero (line) == zero);
                CHECK (line.at (zero) > 0.0f);
            }
        }
    }
}

TEST_CASE ("ltc generator: each edge sits at its exact fraction of a sample, rounded by the standard's rise time")
{
    /*  THE LINE BEFORE ITS LOW-PASS, recovered: the low-pass is one pole,
        `y[n] = y[n-1] + a * (x[n] - y[n-1])` with `a` from the 40 microsecond
        rise (AHJ), so what went in is `(y[n] - (1 - a) * y[n-1]) / a`. If the
        rise were wrong the levels below would not come out flat. Then every
        cell's starting edge - every even half-bit `k`, whose exact moment is
        `k * rate * den / (160 * num)` samples after zero, worked out here in
        whole numbers - is checked where it falls: on a sample boundary, the
        level simply changes there; inside a sample, that sample holds the two
        levels in proportion to the time each held it. 29.97 at 44.1 kHz puts
        almost every edge inside a sample. */
    for (const auto& at : { RateAt { Rate::fps2997, 44100 }, RateAt { Rate::fps2997df, 48000 },
                            RateAt { Rate::fps25, 48000 }, RateAt { Rate::fps23976, 192000 } })
    {
        const auto rate = at.rate;
        const auto sampleRate = at.sampleRate;

        INFO ("rate " << std::string (wordOf (rate)) << " at " << sampleRate << " Hz");

        const auto step = lowPassStep (sampleRate);
        constexpr std::int64_t zero = 700;

        LtcGenerator generator;
        generator.prepare (sampleRate);
        generator.setOutput (1, ltcGain);
        REQUIRE (generator.post (Command::start (1, zero, frameOf (label (1, 0, 0, 0), rate), rate)));

        const auto line = render (generator, 0, zero + sampleRate / 4);
        const auto input = [&] (std::int64_t sample)
        {
            return (static_cast<double> (line.at (sample)) - (1.0 - step) * static_cast<double> (line.at (sample - 1))) / step;
        };

        constexpr double tolerance = 1.0e-4;
        const auto level = static_cast<double> (ltcGain);

        //  Nought before zero, and the first edge from nought up to the level.
        CHECK (std::fabs (input (zero - 1)) < tolerance);
        CHECK (input (zero) == doctest::Approx (level).epsilon (tolerance));

        const auto multiplier = static_cast<std::int64_t> (sampleRate) * denOf (rate);
        const auto divisor = 160 * numOf (rate);

        int edges = 0, inside = 0, wrong = 0;
        std::string firstWrong;

        for (std::int64_t k = 2; k < 3 * 160; k += 2)
        {
            const auto whole = k * multiplier / divisor;
            const auto rest = k * multiplier % divisor;
            const auto holding = zero + whole;

            if (holding + 2 >= line.end())
                break;

            const auto before = input (holding - 1);
            const auto after = input (holding + 1);
            const auto here = input (holding);
            auto right = std::fabs (std::fabs (before) - level) < tolerance && std::fabs (before + after) < tolerance;

            if (rest == 0)
            {
                //  On the boundary: the sample `holding` is already the new level.
                right = right && std::fabs (here - after) < tolerance;
            }
            else
            {
                const auto share = static_cast<double> (rest) / static_cast<double> (divisor);
                right = right && std::fabs (here - (share * before + (1.0 - share) * after)) < tolerance;
                ++inside;
            }

            ++edges;

            if (! right && wrong++ == 0)
                firstWrong = "half-bit " + std::to_string (k) + " at sample " + std::to_string (holding) + ": "
                               + std::to_string (before) + " " + std::to_string (here) + " " + std::to_string (after);
        }

        INFO (firstWrong);
        CHECK (wrong == 0);
        CHECK (edges > 200);

        if (rate == Rate::fps2997)
            CHECK (inside > edges / 2);
    }
}

TEST_CASE ("ltc generator: a stop ends the run on the first frame boundary at or after its sample, its last frame whole")
{
    for (const auto& at : { RateAt { Rate::fps25, 48000 }, RateAt { Rate::fps2997, 44100 },
                            RateAt { Rate::fps2997df, 48000 }, RateAt { Rate::fps23976, 96000 } })
    {
        const auto rate = at.rate;
        const auto sampleRate = at.sampleRate;

        INFO ("rate " << std::string (wordOf (rate)) << " at " << sampleRate << " Hz");

        const auto step = lowPassStep (sampleRate);
        const auto startFrame = frameOf (label (1, 0, 0, 0), rate);
        constexpr std::int64_t zero = 500;

        //  Seven samples into frame 10: frame 10 is drawn to its end, and no more.
        {
            LtcGenerator generator;
            generator.prepare (sampleRate);
            generator.setOutput (1, ltcGain);

            const auto stopAt = zero + frameStartSample (10, sampleRate, rate) + 7;
            const auto end = zero + frameStartSample (11, sampleRate, rate);

            REQUIRE (generator.post (Command::start (3, zero, startFrame, rate)));
            REQUIRE (generator.post (Command::stop (3, stopAt)));

            const auto line = render (generator, 0, end + sampleRate / 2);
            const auto frames = read (line, sampleRate, rate);

            REQUIRE (frames.size() == 11u);
            const auto run = checkRun (frames, 0, frames.size(), zero, startFrame, 0, sampleRate, rate);
            INFO (run.firstWrong);
            CHECK (run.wrong == 0);

            /*  Through its last half-bit at the level, and from the boundary
                on nothing goes in: the low-pass empties, a step a sample. */
            CHECK (std::fabs (line.at (end - 2)) > 0.9f * ltcGain);
            CHECK (line.at (end) == doctest::Approx ((1.0 - step) * line.at (end - 1)).epsilon (1.0e-5));
            CHECK (line.at (end + 1) == doctest::Approx ((1.0 - step) * line.at (end)).epsilon (1.0e-5));
            CHECK (aboveFrom (line, end + fewTaus (sampleRate), 1.0e-4f) == 0);

            CHECK (generator.currentProgram() == 0);
            CHECK (generator.lateCount() == 0);
        }

        //  On a boundary itself: that boundary, so frame 9 is the last.
        {
            LtcGenerator generator;
            generator.prepare (sampleRate);
            generator.setOutput (1, ltcGain);

            const auto end = zero + frameStartSample (10, sampleRate, rate);

            REQUIRE (generator.post (Command::start (3, zero, startFrame, rate)));
            REQUIRE (generator.post (Command::stop (3, end)));

            const auto line = render (generator, 0, end + sampleRate / 2);
            const auto frames = read (line, sampleRate, rate);

            REQUIRE (frames.size() == 10u);
            CHECK (heardLabel (frames.back()) == labelOf (startFrame + 9, rate));
            CHECK (line.at (end) == doctest::Approx ((1.0 - step) * line.at (end - 1)).epsilon (1.0e-5));
            CHECK (aboveFrom (line, end + fewTaus (sampleRate), 1.0e-4f) == 0);
        }
    }

    /*  Where the frame boundaries are whole samples - 1920 of them a frame at
        25 and 48 kHz, each half-bit 12 - the line is still at its level on the
        sample before the boundary: it did not stop early. */
    {
        LtcGenerator generator;
        generator.prepare (48000);
        generator.setOutput (1, ltcGain);

        REQUIRE (generator.post (Command::start (3, 500, 0, Rate::fps25)));
        REQUIRE (generator.post (Command::stop (3, 500 + 1920 * 10 + 7)));

        const auto line = render (generator, 0, 500 + 1920 * 12);
        CHECK (std::fabs (line.at (500 + 1920 * 11 - 1)) > 0.9f * ltcGain);
        CHECK (std::fabs (line.at (500 + 1920 * 11)) < 0.5f * ltcGain);
    }
}

TEST_CASE ("ltc generator: a cut ends the run on its own sample, mid-frame")
{
    for (const auto& at : { RateAt { Rate::fps25, 48000 }, RateAt { Rate::fps2997, 44100 } })
    {
        const auto rate = at.rate;
        const auto sampleRate = at.sampleRate;

        INFO ("rate " << std::string (wordOf (rate)) << " at " << sampleRate << " Hz");

        const auto step = lowPassStep (sampleRate);
        const auto startFrame = frameOf (label (1, 0, 0, 0), rate);
        constexpr std::int64_t zero = 500;
        const auto cutAt = zero + frameStartSample (10, sampleRate, rate) + 700;

        LtcGenerator generator;
        generator.prepare (sampleRate);
        generator.setOutput (1, ltcGain);

        REQUIRE (generator.post (Command::start (3, zero, startFrame, rate)));
        REQUIRE (generator.post (Command::cut (3, cutAt)));

        const auto line = render (generator, 0, cutAt + sampleRate / 2);
        const auto frames = read (line, sampleRate, rate);

        //  Frames 0 to 9 whole; frame 10 cut short, so never read.
        REQUIRE (frames.size() == 10u);
        const auto run = checkRun (frames, 0, frames.size(), zero, startFrame, 0, sampleRate, rate);
        INFO (run.firstWrong);
        CHECK (run.wrong == 0);

        //  From the cut's own sample on, nothing goes in.
        CHECK (line.at (cutAt) == doctest::Approx ((1.0 - step) * line.at (cutAt - 1)).epsilon (1.0e-5));
        CHECK (line.at (cutAt + 1) == doctest::Approx ((1.0 - step) * line.at (cutAt)).epsilon (1.0e-5));
        CHECK (aboveFrom (line, cutAt + fewTaus (sampleRate), 1.0e-4f) == 0);
        CHECK (generator.currentProgram() == 0);

        /*  And on the sample before it, the code was still going in. At 25 and
            48 kHz every edge is on a sample boundary, so what went in there
            was the whole level, one way or the other. */
        if (rate == Rate::fps25)
            CHECK (std::fabs (line.at (cutAt - 1) - (1.0 - step) * line.at (cutAt - 2)) > 0.9 * step * ltcGain);
    }
}

TEST_CASE ("ltc generator: a start takes the running program over on its sample, the line never quiet between")
{
    for (const auto& at : { RateAt { Rate::fps25, 48000 }, RateAt { Rate::fps2997, 44100 },
                            RateAt { Rate::fps2997df, 96000 } })
    {
        const auto rate = at.rate;
        const auto sampleRate = at.sampleRate;

        INFO ("rate " << std::string (wordOf (rate)) << " at " << sampleRate << " Hz");

        LtcGenerator generator;
        generator.prepare (sampleRate);
        generator.setOutput (1, ltcGain);

        constexpr std::int64_t firstZero = 200;
        const auto firstStart = frameOf (label (1, 0, 0, 0), rate);
        const auto takeover = firstZero + frameStartSample (20, sampleRate, rate) + 333;
        const auto secondStart = frameOf (label (10, 0, 0, 0), rate);

        REQUIRE (generator.post (Command::start (1, firstZero, firstStart, rate)));
        REQUIRE (generator.post (Command::start (2, takeover, secondStart, rate)));

        const auto line = render (generator, 0, takeover + 2 * sampleRate);
        const auto frames = read (line, sampleRate, rate);

        const auto second = std::find_if (frames.begin(), frames.end(),
                                          [] (const ltcreader::Frame& frame) { return frame.hours == 10; });
        REQUIRE (second != frames.end());

        //  The first run's twenty whole frames, then the second's from its start.
        const auto firstCount = static_cast<std::size_t> (second - frames.begin());
        CHECK (firstCount == 20u);

        const auto before = checkRun (frames, 0, firstCount, firstZero, firstStart, 0, sampleRate, rate);
        INFO ("before: " << before.firstWrong);
        CHECK (before.wrong == 0);

        const auto after = checkRun (frames, firstCount, frames.size() - firstCount, takeover, secondStart, 0,
                                     sampleRate, rate);
        INFO ("after: " << after.firstWrong);
        CHECK (after.wrong == 0);
        CHECK (frames.size() - firstCount + 2 >= static_cast<std::size_t> (2 * framesPerSecondOf (rate)));

        //  Never quiet: below half the level only where the line crosses nought.
        CHECK (longestBelow (line, firstZero + 1, line.end(), 0.5f * ltcGain) <= 3);

        CHECK (generator.currentProgram() == 2);
        CHECK (generator.lateCount() == 0);
    }
}

TEST_CASE ("ltc generator: a program started again from another frame - a jump - carries on from it on its sample, and the stop its old frames posted does not end it")
{
    /*  TC.7: `run.seek` places a code again under its OWN program, so an Esc
        while the jump waits stops both; the length its first placement posted
        is for frames the code no longer runs, and a stop posted before a start
        of its program is not that start's. */
    for (const auto& at : { RateAt { Rate::fps25, 48000 }, RateAt { Rate::fps2997, 44100 } })
    {
        const auto rate = at.rate;
        const auto sampleRate = at.sampleRate;

        INFO ("rate " << std::string (wordOf (rate)) << " at " << sampleRate << " Hz");

        LtcGenerator generator;
        generator.prepare (sampleRate);
        generator.setOutput (1, ltcGain);

        constexpr std::int64_t zero = 200;
        const auto startFrame = frameOf (label (1, 0, 0, 0), rate);
        const auto jumpedFrame = frameOf (label (1, 0, 20, 0), rate);

        //  Its length, twelve frames, posted with its start.
        REQUIRE (generator.post (Command::start (3, zero, startFrame, rate)));
        REQUIRE (generator.post (Command::stop (3, zero + frameStartSample (12, sampleRate, rate))));

        //  Drawn into frame 6; then the jump, placed ahead inside frame 8.
        const auto split = zero + frameStartSample (6, sampleRate, rate) + 100;
        const auto jumpAt = zero + frameStartSample (8, sampleRate, rate) + 333;

        auto line = render (generator, 0, split);
        REQUIRE (generator.post (Command::start (3, jumpAt, jumpedFrame, rate)));
        const auto rest = render (generator, split, jumpAt + 2 * sampleRate - split);
        line.samples.insert (line.samples.end(), rest.samples.begin(), rest.samples.end());

        const auto frames = read (line, sampleRate, rate);
        const auto jumped = std::find_if (frames.begin(), frames.end(),
                                          [] (const ltcreader::Frame& frame) { return frame.seconds == 20; });
        REQUIRE (jumped != frames.end());

        //  Eight whole frames of the first placement, then the jump's from its zero.
        const auto firstCount = static_cast<std::size_t> (jumped - frames.begin());
        CHECK (firstCount == 8u);

        const auto before = checkRun (frames, 0, firstCount, zero, startFrame, 0, sampleRate, rate);
        INFO ("before: " << before.firstWrong);
        CHECK (before.wrong == 0);

        const auto after = checkRun (frames, firstCount, frames.size() - firstCount, jumpAt, jumpedFrame, 0,
                                     sampleRate, rate);
        INFO ("after: " << after.firstWrong);
        CHECK (after.wrong == 0);

        //  Two seconds of it - far past where the old length would have ended it.
        CHECK (frames.size() - firstCount + 2 >= static_cast<std::size_t> (2 * framesPerSecondOf (rate)));

        //  Never quiet: below half the level only where the line crosses nought.
        CHECK (longestBelow (line, zero + 1, line.end(), 0.5f * ltcGain) <= 3);
        CHECK (generator.currentProgram() == 3);
        CHECK (generator.lateCount() == 0);

        //  A stop posted after it is its: the jumped code ends on its next frame boundary.
        REQUIRE (generator.post (Command::stop (3, 0)));
        const auto tail = render (generator, line.end(), sampleRate / 2);

        CHECK (generator.currentProgram() == 0);
        CHECK (aboveFrom (tail, tail.from + frameStartSample (1, sampleRate, rate) + fewTaus (sampleRate), 1.0e-4f) == 0);
    }
}

TEST_CASE ("ltc generator: a run begun a day before at 192 kHz reads its labels through midnight")
{
    /*  THE ARITHMETIC OVER A DAY. The program's zero is ten seconds' worth of
        frames short of a day before the block, at 192 kHz - 1.66e10 samples,
        3.3e8 half-bits: a half-bit's place and its fraction of a sample are
        computed exactly that far out, or the labels drift or the line breaks
        up. Its start reaches the generator a day late, so it is counted, and
        begins on the frame boundary that is the block's own first sample. */
    constexpr int sampleRate = 192000;

    for (const auto rate : { Rate::fps25, Rate::fps2997df, Rate::fps23976 })
    {
        INFO ("rate " << std::string (wordOf (rate)));

        const auto tenSeconds = 10 * static_cast<std::int64_t> (nominalOf (rate));
        const auto firstFrame = framesPerDay (rate) - tenSeconds;
        constexpr std::int64_t zero = 4800;
        const auto blockStart = zero + frameStartSample (firstFrame, sampleRate, rate);

        LtcGenerator generator;
        generator.prepare (sampleRate);
        generator.setOutput (1, ltcGain);
        REQUIRE (generator.post (Command::start (9, zero, 0, rate)));

        const auto line = render (generator, blockStart, 12 * sampleRate);
        const auto frames = read (line, sampleRate, rate);

        CHECK (generator.lateCount() == 1);
        CHECK (firstNonZero (line) == blockStart);

        REQUIRE (frames.size() > static_cast<std::size_t> (tenSeconds + nominalOf (rate)));
        CHECK (heardLabel (frames.front()) == label (23, 59, 50, 0));
        CHECK (heardLabel (frames[static_cast<std::size_t> (tenSeconds)]) == label (0, 0, 0, 0));

        const auto run = checkRun (frames, 0, frames.size(), zero, 0, firstFrame, sampleRate, rate);
        INFO (run.firstWrong);
        CHECK (run.wrong == 0);
    }
}

TEST_CASE ("ltc generator: a command for a sample already drawn is counted late, and a late start waits for a frame boundary")
{
    constexpr int sampleRate = 48000;
    const auto step = lowPassStep (sampleRate);
    const auto startFrame = frameOf (label (1, 0, 0, 0), Rate::fps25);

    LtcGenerator generator;
    generator.prepare (sampleRate);
    generator.setOutput (1, ltcGain);

    CHECK (firstNonZero (render (generator, 0, 10000)) == -1);

    /*  Its zero 5000 samples behind the block: begun on the next frame
        boundary of its own clock - frame 3, sample 5000 + 3 * 1920 - never on
        half a frame. */
    REQUIRE (generator.post (Command::start (1, 5000, startFrame, Rate::fps25)));
    const auto line = render (generator, 10000, 20000);
    const auto frames = read (line, sampleRate, Rate::fps25);

    CHECK (generator.lateCount() == 1);
    CHECK (firstNonZero (line) == 10760);
    REQUIRE_FALSE (frames.empty());
    CHECK (heardLabel (frames.front()) == label (1, 0, 0, 3));
    CHECK (frames.front().sample == 10760);

    const auto run = checkRun (frames, 0, frames.size(), 5000, startFrame, 3, sampleRate, Rate::fps25);
    INFO (run.firstWrong);
    CHECK (run.wrong == 0);

    /*  A stop placed for sample 20000, reaching a block that starts at 30000:
        late as well, and on the first boundary at or after that block -
        5000 + 14 * 1920. */
    REQUIRE (generator.post (Command::stop (1, 20000)));
    const auto after = render (generator, 30000, 4000);

    CHECK (generator.lateCount() == 2);
    CHECK (std::fabs (after.at (31878)) > 0.9f * ltcGain);
    CHECK (after.at (31880) == doctest::Approx ((1.0 - step) * after.at (31879)).epsilon (1.0e-5));
    CHECK (aboveFrom (after, 31880 + fewTaus (sampleRate), 1.0e-4f) == 0);
    CHECK (generator.currentProgram() == 0);
}

TEST_CASE ("ltc generator: an Esc's stop and a double Esc's cut, sent as soon as can be, are never late")
{
    constexpr int sampleRate = 48000;
    const auto step = lowPassStep (sampleRate);

    //  The stop: at the first frame boundary at or after the block that drains it.
    {
        LtcGenerator generator;
        generator.prepare (sampleRate);
        generator.setOutput (1, ltcGain);
        REQUIRE (generator.post (Command::start (1, 0, 0, Rate::fps25)));

        const auto before = render (generator, 0, 9000);
        REQUIRE (generator.post (Command::stop (1, 0)));
        const auto after = render (generator, 9000, 4000, { 128 });

        CHECK (generator.lateCount() == 0);
        CHECK (std::fabs (after.at (9598)) > 0.9f * ltcGain);
        CHECK (after.at (9600) == doctest::Approx ((1.0 - step) * after.at (9599)).epsilon (1.0e-5));
        CHECK (aboveFrom (after, 9600 + fewTaus (sampleRate), 1.0e-4f) == 0);
        CHECK (std::fabs (before.at (8999)) > 0.0f);
    }

    //  The cut: on the first sample of that block.
    {
        LtcGenerator generator;
        generator.prepare (sampleRate);
        generator.setOutput (1, ltcGain);
        REQUIRE (generator.post (Command::start (1, 0, 0, Rate::fps25)));

        const auto before = render (generator, 0, 9000);
        REQUIRE (generator.post (Command::cut (1, 0)));
        const auto after = render (generator, 9000, 4000, { 128 });

        CHECK (generator.lateCount() == 0);
        CHECK (after.at (9000) == doctest::Approx ((1.0 - step) * before.at (8999)).epsilon (1.0e-5));
        CHECK (aboveFrom (after, 9000 + fewTaus (sampleRate), 1.0e-4f) == 0);
        CHECK (generator.currentProgram() == 0);
    }
}

TEST_CASE ("ltc generator: the line peaks at the level it is given, and an output of -1 writes nothing")
{
    for (const auto& at : { RateAt { Rate::fps25, 48000 }, RateAt { Rate::fps2997, 44100 },
                            RateAt { Rate::fps30, 192000 } })
    {
        const auto rate = at.rate;
        const auto sampleRate = at.sampleRate;

        INFO ("rate " << std::string (wordOf (rate)) << " at " << sampleRate << " Hz");

        for (const auto gain : { ltcGain, 1.0f })
        {
            LtcGenerator generator;
            generator.prepare (sampleRate);
            generator.setOutput (1, gain);
            REQUIRE (generator.post (Command::start (1, 0, 0, rate)));

            const auto peak = peakOf (render (generator, 0, sampleRate));

            //  Within the low-pass's reach of the level, and never past it.
            CHECK (peak <= gain * (1.0f + 1.0e-6f));
            CHECK (peak >= gain * 0.999f);
        }
    }

    LtcGenerator generator;
    generator.prepare (48000);
    generator.setOutput (-1, ltcGain);
    REQUIRE (generator.post (Command::start (1, 0, 0, Rate::fps25)));

    //  No output: the channel keeps the sound it held, and the run goes on.
    const auto none = render (generator, 0, 4800);
    CHECK (aboveFrom (none, 0, routedSound - 1.0e-6f) == 4800);
    CHECK (none.othersTouched == 0);
    CHECK (generator.currentProgram() == 1);

    //  An output past the block's channels: nothing written anywhere.
    generator.setOutput (5, ltcGain);
    const auto past = render (generator, 4800, 4800);
    CHECK (aboveFrom (past, 4800, routedSound - 1.0e-6f) == 4800);
    CHECK (past.othersTouched == 0);

    /*  Back on the channel, mid-run - at frame 5's first sample - and the code
        is where the clock has got to: frame 5 or 6 the first read whole, and
        every frame on its own sample. */
    generator.setOutput (1, ltcGain);
    const auto back = render (generator, 9600, 48000);
    const auto frames = read (back, 48000, Rate::fps25);
    REQUIRE (frames.size() > 10u);

    const auto first = frameOf (heardLabel (frames.front()), Rate::fps25);
    CHECK (first >= 5);
    CHECK (first <= 6);

    const auto run = checkRun (frames, 0, frames.size(), 0, 0, first, 48000, Rate::fps25);
    INFO (run.firstWrong);
    CHECK (run.wrong == 0);
}

TEST_CASE ("ltc generator: the channel is replaced - by silence while nothing runs - and no other is touched")
{
    LtcGenerator generator;
    generator.prepare (48000);
    generator.setOutput (1, ltcGain);

    //  Nothing running: the sound routed there is gone, and nought is left.
    const auto idle = render (generator, 0, 4800);
    CHECK (firstNonZero (idle) == -1);
    CHECK (idle.othersTouched == 0);

    //  A run: only the code, which reads back.
    REQUIRE (generator.post (Command::start (1, 4800, frameOf (label (1, 0, 0, 0), Rate::fps25), Rate::fps25)));
    const auto running = render (generator, 4800, 48000);
    CHECK (read (running, 48000, Rate::fps25).size() >= 23u);
    CHECK (peakOf (running) <= ltcGain * (1.0f + 1.0e-6f));
    CHECK (running.othersTouched == 0);

    //  On a channel of an eight-wide block: that channel, and no other.
    const auto wide = render (generator, 52800, 4800, mixedBlocks, 1, 8);
    CHECK (wide.othersTouched == 0);
    CHECK (peakOf (wide) <= ltcGain * (1.0f + 1.0e-6f));
}

TEST_CASE ("ltc generator: a stop for a run that has not begun takes its start back, and one for another run is passed over")
{
    const auto startFrame = frameOf (label (1, 0, 0, 0), Rate::fps25);

    //  An Esc inside the launch horizon: the start placed ahead never sounds.
    {
        LtcGenerator generator;
        generator.prepare (48000);
        generator.setOutput (1, ltcGain);

        REQUIRE (generator.post (Command::start (4, 20000, startFrame, Rate::fps25)));
        CHECK (firstNonZero (render (generator, 0, 1000)) == -1);
        REQUIRE (generator.post (Command::stop (4, 0)));
        CHECK (firstNonZero (render (generator, 1000, 30000)) == -1);
        CHECK (generator.currentProgram() == 0);
        CHECK (generator.lateCount() == 0);
    }

    //  The same, both in the one block's inbox; and a cut does it too.
    for (const auto cut : { false, true })
    {
        LtcGenerator generator;
        generator.prepare (48000);
        generator.setOutput (1, ltcGain);

        REQUIRE (generator.post (Command::start (8, 100, startFrame, Rate::fps25)));
        REQUIRE (generator.post (cut ? Command::cut (8, 0) : Command::stop (8, 0)));
        CHECK (firstNonZero (render (generator, 0, 10000)) == -1);
    }

    //  A start posted after the stop is a new one, and plays.
    {
        LtcGenerator generator;
        generator.prepare (48000);
        generator.setOutput (1, ltcGain);

        REQUIRE (generator.post (Command::stop (8, 0)));
        REQUIRE (generator.post (Command::start (8, 100, startFrame, Rate::fps25)));
        CHECK (firstNonZero (render (generator, 0, 10000)) == 100);
        CHECK (generator.currentProgram() == 8);
    }

    //  A stop for a run that is not the one drawing changes nothing.
    {
        LtcGenerator generator;
        generator.prepare (48000);
        generator.setOutput (1, ltcGain);

        REQUIRE (generator.post (Command::start (5, 100, startFrame, Rate::fps25)));
        const auto first = render (generator, 0, 5000);
        REQUIRE (generator.post (Command::stop (6, 0)));
        REQUIRE (generator.post (Command::cut (7, 0)));
        const auto second = render (generator, 5000, 48000);
        const auto frames = read (second, 48000, Rate::fps25);

        CHECK (generator.currentProgram() == 5);
        REQUIRE (frames.size() > 20u);

        //  The first frame begun after sample 5000 is frame 3, at 100 + 3 * 1920.
        const auto firstHeard = frameOf (heardLabel (frames.front()), Rate::fps25) - startFrame;
        CHECK (firstHeard == 3);

        const auto run = checkRun (frames, 0, frames.size(), 100, startFrame, firstHeard, 48000, Rate::fps25);
        INFO (run.firstWrong);
        CHECK (run.wrong == 0);
        CHECK (std::fabs (first.at (4999)) > 0.0f);
    }
}

TEST_CASE ("ltc generator: prepare ends the run, forgets what was posted and counts an audio restart")
{
    LtcGenerator generator;
    generator.prepare (48000);
    generator.setOutput (1, ltcGain);
    const auto first = generator.generation();

    REQUIRE (generator.post (Command::start (1, 0, 0, Rate::fps25)));
    render (generator, 0, 5000);
    CHECK (generator.currentProgram() == 1);

    //  An audio restart (AHU): the run is over, and the line silent.
    generator.prepare (48000);
    CHECK (generator.generation() == first + 1);
    CHECK (generator.currentProgram() == 0);
    CHECK (firstNonZero (render (generator, 5000, 5000)) == -1);

    //  A start posted before a restart is not drawn after it.
    REQUIRE (generator.post (Command::start (2, 20000, 0, Rate::fps25)));
    generator.prepare (96000);
    CHECK (generator.generation() == first + 2);
    CHECK (firstNonZero (render (generator, 10000, 20000)) == -1);
    CHECK (generator.currentProgram() == 0);

    //  And one posted after it is.
    REQUIRE (generator.post (Command::start (3, 40000, 0, Rate::fps25)));
    CHECK (firstNonZero (render (generator, 30000, 20000)) == 40000);
    CHECK (generator.currentProgram() == 3);
}

TEST_CASE ("ltc generator: the inbox holds thirty-two commands, and a program must have a number")
{
    LtcGenerator generator;
    generator.prepare (48000);
    generator.setOutput (1, ltcGain);

    for (int n = 0; n < LtcGenerator::inboxSize; ++n)
        CHECK (generator.post (Command::start (static_cast<std::uint64_t> (n + 1), 1000000 + n, 0, Rate::fps25)));

    CHECK_FALSE (generator.post (Command::start (99, 2000000, 0, Rate::fps25)));

    //  A block drains it, and there is room again.
    render (generator, 0, 128, { 128 });
    CHECK (generator.post (Command::start (99, 2000000, 0, Rate::fps25)));

    //  Nought is no program: currentProgram's word for none.
    CHECK_FALSE (generator.post (Command::start (0, 3000000, 0, Rate::fps25)));
}

//==============================================================================
//  LTC read: the decoder on the audio thread, fed by the generator (TG.3)

namespace
{
    /*  THE CODE AS A LINE: program 7 of `rate` from `startLabel`, its frame
        nought beginning on `zero`, drawn for `count` samples from nought the
        way AudioHost draws it. */
    std::vector<float> codeLine (Rate rate, int sampleRate, std::int64_t zero, std::int64_t count,
                                 const Label& startLabel = Label { 1, 0, 0, 0 })
    {
        LtcGenerator generator;
        generator.prepare (sampleRate);
        generator.setOutput (1, ltcGain);
        REQUIRE (generator.post (Command::start (7, zero, frameOf (startLabel, rate), rate)));
        return render (generator, 0, count).samples;
    }

    /*  EVERYTHING A DECODER HEARS of a line, fed `block` samples at a time,
        the first of them sample `from` of Go.dot's count, and taken after
        every block as the tick would take it - into a vector reserved once. */
    std::vector<LtcHeard> hear (LtcDecoder& decoder, const std::vector<float>& line, std::int64_t from, int block)
    {
        std::vector<LtcHeard> all, some;
        some.reserve (static_cast<std::size_t> (LtcDecoder::outboxSize));

        for (std::size_t at = 0; at < line.size();)
        {
            const auto size = std::min (static_cast<std::size_t> (block), line.size() - at);
            decoder.process (line.data() + at, static_cast<int> (size), from + static_cast<std::int64_t> (at));
            decoder.take (some);
            all.insert (all.end(), some.begin(), some.end());
            at += size;
        }

        return all;
    }

    std::vector<LtcHeard> hear (const std::vector<float>& line, int sampleRate, int block = 64)
    {
        LtcDecoder decoder;
        decoder.prepare (sampleRate);
        return hear (decoder, line, 0, block);
    }

    //  Where a frame began, as one number.
    double startOf (const LtcHeard& heard)
    {
        return static_cast<double> (heard.startSample) + static_cast<double> (heard.startFraction);
    }

    /*  THE EXACT MOMENT frame `k` of a run begins: `k * rate * den / num`
        samples after its zero. Not FrameClock's sample, which is the first
        AT OR AFTER that moment: the generator places every edge at its
        fraction of a sample, and the decoder finds it there. */
    double exactStart (std::int64_t zero, std::int64_t k, int sampleRate, Rate rate)
    {
        return static_cast<double> (zero)
                 + static_cast<double> (k * sampleRate * denOf (rate)) / static_cast<double> (numOf (rate));
    }

    //  A bit, in samples.
    double bitLengthOf (int sampleRate, Rate rate)
    {
        return static_cast<double> (sampleRate) * static_cast<double> (denOf (rate))
                 / (80.0 * static_cast<double> (numOf (rate)));
    }

    /*  HOW MANY FRAMES of a run end two bits or more before `end`: each of
        them whole in the line, and read in time - a frame's last bit is
        closed by the next frame's first edge, which is placed once the edge
        after it is found. */
    std::size_t wholeBefore (std::int64_t end, std::int64_t zero, int sampleRate, Rate rate)
    {
        std::size_t count = 0;

        while (exactStart (zero, static_cast<std::int64_t> (count) + 1, sampleRate, rate)
                 + 2.0 * bitLengthOf (sampleRate, rate) <= static_cast<double> (end))
            ++count;

        return count;
    }

    /*  THAT FRAMES READ ARE A RUN: entry `i` is frame `first + i` of a program
        whose frame nought began on `zero` and was called `startFrame` - read
        forwards, its label, its drop flag, its parity even - and began where
        the frame's exact moment is plus `delay`, the edge filter's constant,
        to within `tolerance` of a sample. */
    struct ReadCheck
    {
        int wrong = 0;
        std::string firstWrong;
        double worst = 0.0;
    };

    ReadCheck checkRead (const std::vector<LtcHeard>& heard, std::size_t from, std::size_t count,
                         std::int64_t zero, std::int64_t startFrame, std::int64_t first, int sampleRate,
                         Rate rate, double delay, double tolerance)
    {
        ReadCheck result;

        for (std::size_t i = 0; i < count; ++i)
        {
            const auto& frame = heard.at (from + i);
            const auto k = first + static_cast<std::int64_t> (i);
            const auto wanted = labelOf (startFrame + k, rate);
            const auto off = startOf (frame) - exactStart (zero, k, sampleRate, rate) - delay;
            result.worst = std::max (result.worst, std::fabs (off));

            const auto right = ! frame.lost && ! frame.reverse && frame.parityOk && frame.label == wanted
                                 && frame.drop == dropsFrames (rate) && ! (std::fabs (off) > tolerance);

            if (! right && result.wrong++ == 0)
                result.firstWrong = "frame " + std::to_string (k) + ": heard " + formatLabel (frame.label, rate)
                                      + (frame.lost ? " (lost)" : "") + (frame.reverse ? " (reverse)" : "")
                                      + (frame.parityOk ? "" : " (odd)") + " at "
                                      + std::to_string (frame.startSample) + ", wanted " + formatLabel (wanted, rate)
                                      + " at " + std::to_string (static_cast<std::int64_t> (
                                                   std::floor (exactStart (zero, k, sampleRate, rate) + delay)));
        }

        return result;
    }

    /*  WHITE NOISE, its standard deviation one, the same on every platform:
        four of mt19937's own numbers summed - the standard fixes those, where
        std::normal_distribution is each library's own. */
    struct Noise
    {
        std::mt19937 numbers { 20261013u };

        double next()
        {
            double sum = 0.0;

            for (int n = 0; n < 4; ++n)
                sum += static_cast<double> (numbers()) / 4294967295.0 - 0.5;

            return sum * std::sqrt (3.0);
        }
    };

    /*  A LINE PLAYED AT `speed`: read every `speed` samples, straight between
        the two either side - as a tape machine run fast or slow would hand
        it on. */
    std::vector<float> playedAt (const std::vector<float>& line, double speed)
    {
        std::vector<float> out;

        for (std::size_t m = 0;; ++m)
        {
            const auto at = static_cast<double> (m) * speed;
            const auto index = static_cast<std::size_t> (at);

            if (index + 1 >= line.size())
                break;

            const auto fraction = at - static_cast<double> (index);
            out.push_back (static_cast<float> ((1.0 - fraction) * static_cast<double> (line[index])
                                               + fraction * static_cast<double> (line[index + 1])));
        }

        return out;
    }

    std::size_t lostIn (const std::vector<LtcHeard>& heard)
    {
        return static_cast<std::size_t> (std::count_if (heard.begin(), heard.end(),
                                                        [] (const LtcHeard& one) { return one.lost; }));
    }
}

TEST_CASE ("ltc decoder: every frame at every rate and 44.1, 48 and 96 kHz, in blocks of 1, 7, 64, 333 and 512, each where it began to a tenth of a sample")
{
    /*  Two seconds from 01:00:00:00, read from the code's first edge - out of
        silence - to the last frame whole in the line. The edge filter delays
        every edge alike, by under two samples (a few tenths at 44.1 and 48 kHz,
        more at 96 where the rise is more samples long): learned from the first
        frame, every later one is held to it. And the blocks change nothing,
        not by a bit of a fraction. */
    for (const auto rate : everyRate)
    {
        for (const int sampleRate : { 44100, 48000, 96000 })
        {
            INFO ("rate " << std::string (wordOf (rate)) << " at " << sampleRate << " Hz");

            constexpr std::int64_t zero = 1000;
            const auto startFrame = frameOf (label (1, 0, 0, 0), rate);
            const auto count = zero + 2 * sampleRate;
            const auto line = codeLine (rate, sampleRate, zero, count);
            const auto whole = wholeBefore (count, zero, sampleRate, rate);

            std::vector<LtcHeard> firstHeard;

            for (const int block : { 1, 7, 64, 333, 512 })
            {
                INFO ("in blocks of " << block);

                const auto heard = hear (line, sampleRate, block);
                REQUIRE (heard.size() >= whole);
                CHECK (heard.size() <= whole + 1);

                const auto delay = startOf (heard.front()) - exactStart (zero, 0, sampleRate, rate);
                CHECK (delay > 0.0);
                CHECK (delay < 2.0);

                const auto read = checkRead (heard, 0, heard.size(), zero, startFrame, 0, sampleRate, rate, delay, 0.1);
                INFO (read.firstWrong);
                CHECK (read.wrong == 0);
                CHECK (read.worst < 0.1);

                for (const auto& frame : heard)
                    CHECK (frame.bitPeriod == doctest::Approx (bitLengthOf (sampleRate, rate)).epsilon (0.001));

                if (firstHeard.empty())
                {
                    firstHeard = heard;
                    continue;
                }

                auto same = firstHeard.size() == heard.size();

                for (std::size_t i = 0; same && i < heard.size(); ++i)
                    same = heard[i].label == firstHeard[i].label && heard[i].startSample == firstHeard[i].startSample
                             && ! (std::fabs (heard[i].startFraction - firstHeard[i].startFraction) > 0.0f);

                CHECK (same);
            }

            /*  AND AS THE TESTS' OWN READER HEARS IT (LtcReader.h, written from
                the standard alone): the same labels, each on the first sample
                of the new sign its edge brought - within two samples of where
                the decoder placed it. */
            const auto reader = ltcreader::decode (line.data(), line.size(), static_cast<double> (sampleRate),
                                                   framesPerSecondOf (rate), 0);
            const auto heard = firstHeard;
            REQUIRE (reader.size() >= whole);

            int disagree = 0;

            for (std::size_t i = 0; i < std::min (reader.size(), heard.size()); ++i)
                if (heardLabel (reader[i]) != heard[i].label || std::fabs (static_cast<double> (reader[i].sample) - startOf (heard[i])) > 2.0)
                    ++disagree;

            CHECK (disagree == 0);
        }
    }
}

TEST_CASE ("ltc decoder: a code played backwards reads with reverse set and its labels descending, each start the same edge met from the other side")
{
    /*  Reversed, a frame arrives bit 79 first, the sync word mirrored at its
        head, and its bit 0 last: the edge that began bit 0 is the one that
        closes it as it arrives. A sample `n` played backwards is `count - 1 -
        n`, so a moment `t` of the line forwards is `count - t` backwards -
        and the filter's delay, which followed each edge forwards, precedes it
        now. The frame the line ended inside comes first and is not whole; the
        last read is frame nought, closed by the fall into the silence the code
        started from. */
    for (const auto rate : { Rate::fps25, Rate::fps2997df, Rate::fps23976 })
    {
        INFO ("rate " << std::string (wordOf (rate)));

        constexpr int sampleRate = 48000;
        constexpr std::int64_t zero = 1000;
        const auto startFrame = frameOf (label (1, 0, 0, 0), rate);
        const auto count = zero + 2 * sampleRate;
        auto line = codeLine (rate, sampleRate, zero, count);

        const auto forwards = hear (line, sampleRate);
        REQUIRE (! forwards.empty());
        const auto delay = startOf (forwards.front()) - exactStart (zero, 0, sampleRate, rate);

        std::reverse (line.begin(), line.end());
        const auto heard = hear (line, sampleRate);
        const auto whole = wholeBefore (count, zero, sampleRate, rate);

        REQUIRE (heard.size() >= whole);
        CHECK (heard.size() <= whole + 1);
        CHECK (lostIn (heard) == 0);

        int wrong = 0;
        double worst = 0.0;

        for (std::size_t i = 0; i < heard.size(); ++i)
        {
            const auto& frame = heard[i];
            const auto k = static_cast<std::int64_t> (heard.size() - 1 - i);
            const auto off = startOf (frame) - (static_cast<double> (count) - exactStart (zero, k, sampleRate, rate) - delay);
            worst = std::max (worst, std::fabs (off));

            if (! frame.reverse || ! frame.parityOk || frame.drop != dropsFrames (rate)
                  || frame.label != labelOf (startFrame + k, rate))
                ++wrong;
        }

        CHECK (wrong == 0);
        CHECK (heard.back().label == label (1, 0, 0, 0));
        CHECK (worst < 0.1);
    }
}

TEST_CASE ("ltc decoder: a code played at half and at twice its speed, or sped up as it plays, reads, and the bit period follows it")
{
    /*  Four seconds from 01:00:00:00, played as a tape run at half speed and
        at twice - 23.976 at half is the longest bit the decoder follows, 30 at
        twice the shortest. Every frame reads, in order, and each says how long
        its bits were where it was heard: the period the speed made. */
    for (const auto rate : { Rate::fps23976, Rate::fps25, Rate::fps2997df, Rate::fps30 })
    {
        for (const double speed : { 0.5, 2.0 })
        {
            INFO ("rate " << std::string (wordOf (rate)) << " at " << speed << " times");

            constexpr int sampleRate = 48000;
            constexpr std::int64_t zero = 1000;
            const auto startFrame = frameOf (label (1, 0, 0, 0), rate);
            const auto count = zero + 4 * sampleRate;
            const auto played = playedAt (codeLine (rate, sampleRate, zero, count), speed);
            const auto heard = hear (played, sampleRate);
            const auto whole = wholeBefore (count, zero, sampleRate, rate);

            REQUIRE (heard.size() + 1 >= whole);
            CHECK (lostIn (heard) == 0);

            const auto bit = bitLengthOf (sampleRate, rate) / speed;
            const auto frameLength = 80.0 * bit;
            int wrong = 0;

            for (std::size_t i = 0; i < heard.size(); ++i)
            {
                const auto& frame = heard[i];
                const auto k = static_cast<std::int64_t> (i);
                const auto spaced = i == 0 || std::fabs (startOf (frame) - startOf (heard[i - 1]) - frameLength) < 0.5;

                if (frame.reverse || ! frame.parityOk || frame.label != labelOf (startFrame + k, rate) || ! spaced
                      || std::fabs (frame.bitPeriod / bit - 1.0) > 0.005)
                    ++wrong;
            }

            CHECK (wrong == 0);
        }
    }

    /*  AND A VARISPEED: 25 frames a second sped up from once to one and a
        half times over two seconds. Past a third faster a whole bit is
        shorter than three quarters of the bit the code began with, so only a
        period that follows the code reads it to the end - and the last frames
        say the bit the speed made. */
    constexpr int sampleRate = 48000;
    constexpr std::int64_t zero = 1000;
    const auto rate = Rate::fps25;
    const auto line = codeLine (rate, sampleRate, zero, zero + 4 * sampleRate);

    std::vector<float> ramped;
    constexpr double rampSamples = 2.0 * static_cast<double> (sampleRate);

    for (double at = 0.0, speed = 1.0; at + 1.0 < static_cast<double> (line.size()); at += speed)
    {
        const auto index = static_cast<std::size_t> (at);
        const auto fraction = at - static_cast<double> (index);
        ramped.push_back (static_cast<float> ((1.0 - fraction) * static_cast<double> (line[index])
                                              + fraction * static_cast<double> (line[index + 1])));
        speed = std::min (1.5, 1.0 + 0.5 * static_cast<double> (ramped.size()) / rampSamples);
    }

    const auto heard = hear (ramped, sampleRate);
    REQUIRE (heard.size() > 90u);
    CHECK (lostIn (heard) == 0);

    int wrong = 0;

    for (std::size_t i = 0; i < heard.size(); ++i)
        if (heard[i].reverse || ! heard[i].parityOk
              || heard[i].label != labelOf (frameOf (label (1, 0, 0, 0), rate) + static_cast<std::int64_t> (i), rate))
            ++wrong;

    CHECK (wrong == 0);
    CHECK (heard.back().bitPeriod == doctest::Approx (24.0 / 1.5).epsilon (0.005));
}

TEST_CASE ("ltc decoder: a code at -40 dBFS, off-centre, inverted or under white noise reads every frame, and noise alone reads nothing")
{
    /*  29.97 drop-frame at 44.1 kHz, the fractional worst case, three
        seconds: at -40 dBFS; on a DC offset of a quarter of its peak; upside
        down; under white noise 20 dB below it, from the code's first sample;
        and all four at once. Every frame is read and nothing else is - no
        frame that is not the code's, no `lost`. The followers make a quiet,
        an off-centre and an inverted code read exactly as the code itself,
        to a tenth of a sample. Noise moves each edge, and the first edge out
        of silence most - a sample at -20 dB, the plateau it reached read from
        a peak the noise rides on - so under noise the reading is what is held
        to, and the starts only to two samples. Ten seconds of the noise alone
        read as nothing at all. */
    constexpr int sampleRate = 44100;
    constexpr std::int64_t zero = 1000;
    const auto rate = Rate::fps2997df;
    const auto startFrame = frameOf (label (1, 0, 0, 0), rate);
    const auto count = zero + 3 * sampleRate;
    const auto code = codeLine (rate, sampleRate, zero, count);
    const auto whole = wholeBefore (count, zero, sampleRate, rate);

    const auto delay = startOf (hear (code, sampleRate).front()) - exactStart (zero, 0, sampleRate, rate);
    const auto peak = static_cast<double> (ltcGain);

    struct Condition
    {
        const char* name;
        double gain;
        double offset;
        double noise;
        double tolerance;
    };

    const Condition conditions[] {
        { "at -40 dBFS", 0.01 / peak, 0.0, 0.0, 0.1 },
        { "on a DC offset of a quarter of its peak", 1.0, 0.25, 0.0, 0.1 },
        { "inverted", -1.0, 0.0, 0.0, 0.1 },
        { "under white noise 20 dB down", 1.0, 0.0, 0.1, 2.0 },
        { "at -40 dBFS, off-centre, inverted and under noise", -0.01 / peak, 0.25, 0.1, 2.0 }
    };

    for (const auto& condition : conditions)
    {
        INFO (std::string (condition.name));

        Noise noise;
        std::vector<float> line (code.size());
        const auto level = peak * std::fabs (condition.gain);

        for (std::size_t n = 0; n < line.size(); ++n)
        {
            auto value = static_cast<double> (code[n]) * condition.gain + condition.offset * level;

            if (condition.noise > 0.0 && static_cast<std::int64_t> (n) >= zero)
                value += condition.noise * level * noise.next();

            line[n] = static_cast<float> (value);
        }

        const auto heard = hear (line, sampleRate);
        REQUIRE (heard.size() >= whole);
        CHECK (heard.size() <= whole + 1);

        const auto read = checkRead (heard, 0, heard.size(), zero, startFrame, 0, sampleRate, rate, delay,
                                     condition.tolerance);
        INFO (read.firstWrong << "; the worst start " << read.worst << " samples out");
        CHECK (read.wrong == 0);
    }

    Noise noise;
    std::vector<float> alone (static_cast<std::size_t> (10 * sampleRate));

    for (auto& value : alone)
        value = static_cast<float> (0.1 * peak * noise.next());

    CHECK (hear (alone, sampleRate).empty());
}

TEST_CASE ("ltc decoder: half a second of silence says lost once, two frames after the last frame heard, and the code is read again after it")
{
    /*  25 at 48 kHz, a frame every 1920 samples from 1000; the line cut to
        silence from 48000 to 72000 - mid-frame both ways. The frames whole
        before the cut are read; two of their lengths after the last one
        ended, one `lost` (AIR's freewheel is TG.5's: the decoder says only
        that the code went quiet); and from the first frame whole after the
        silence, the code again, every frame where it began. */
    constexpr int sampleRate = 48000;
    constexpr std::int64_t zero = 1000;
    const auto rate = Rate::fps25;
    const auto startFrame = frameOf (label (1, 0, 0, 0), rate);
    const auto count = zero + 3 * sampleRate;

    auto line = codeLine (rate, sampleRate, zero, count);
    const auto delay = startOf (hear (line, sampleRate).front()) - exactStart (zero, 0, sampleRate, rate);

    std::fill (line.begin() + 48000, line.begin() + 72000, 0.0f);
    const auto heard = hear (line, sampleRate);

    REQUIRE (lostIn (heard) == 1);
    const auto lost = static_cast<std::size_t> (std::find_if (heard.begin(), heard.end(),
                                                              [] (const LtcHeard& one) { return one.lost; })
                                                - heard.begin());

    //  Frames 0 to 23 end by 47080; frame 24 runs into the silence.
    REQUIRE (lost == 24);
    const auto before = checkRead (heard, 0, lost, zero, startFrame, 0, sampleRate, rate, delay, 0.1);
    INFO (before.firstWrong);
    CHECK (before.wrong == 0);

    //  Frame 23 ended where frame 24 would have begun: two frames after that.
    CHECK (startOf (heard[lost]) == doctest::Approx (exactStart (zero, 24, sampleRate, rate) + delay + 2.0 * 1920.0).epsilon (1.0e-6));

    //  Frame 37 is the first to begin after the silence, at 72040.
    const auto after = heard.size() - lost - 1;
    REQUIRE (after >= 30u);
    const auto again = checkRead (heard, lost + 1, after, zero, startFrame, 37, sampleRate, rate, delay, 0.1);
    INFO (again.firstWrong);
    CHECK (again.wrong == 0);
}

TEST_CASE ("ltc decoder: a takeover splices a new code in, and its frames are read from within a frame of it")
{
    /*  Program 1 from 01:00:00:00 on 1000; program 2 from 10:00:00:00 takes
        the line over 777 samples into the old code's twenty-sixth frame - the
        generator's takeover, one more edge with the line never quiet (AHL).
        The old code's frames whole before it are read; then the new code's,
        the first of them within a frame of the splice, each where it began. */
    constexpr int sampleRate = 48000;
    constexpr std::int64_t zero = 1000;
    const auto rate = Rate::fps25;
    constexpr std::int64_t splice = zero + 25 * 1920 + 777;

    LtcGenerator generator;
    generator.prepare (sampleRate);
    generator.setOutput (1, ltcGain);
    REQUIRE (generator.post (Command::start (1, zero, frameOf (label (1, 0, 0, 0), rate), rate)));
    REQUIRE (generator.post (Command::start (2, splice, frameOf (label (10, 0, 0, 0), rate), rate)));

    const auto line = render (generator, 0, 3 * sampleRate).samples;
    const auto heard = hear (line, sampleRate);
    REQUIRE (heard.size() > 25u);
    CHECK (lostIn (heard) == 0);

    const auto delay = startOf (heard.front()) - exactStart (zero, 0, sampleRate, rate);
    const auto old = checkRead (heard, 0, 25, zero, frameOf (label (1, 0, 0, 0), rate), 0, sampleRate, rate, delay, 0.1);
    INFO (old.firstWrong);
    CHECK (old.wrong == 0);

    //  The first of the new code's: frame nought or frame one of it.
    const auto& first = heard[25];
    const auto newFrame = frameOf (first.label, rate) - frameOf (label (10, 0, 0, 0), rate);
    CHECK (newFrame >= 0);
    CHECK (newFrame <= 1);

    const auto taken = checkRead (heard, 25, heard.size() - 25, splice, frameOf (label (10, 0, 0, 0), rate),
                                  newFrame, sampleRate, rate, delay, 0.1);
    INFO (taken.firstWrong);
    CHECK (taken.wrong == 0);
}

TEST_CASE ("ltc decoder: a code stopped on a frame boundary is read to its last frame, then said lost")
{
    /*  The generator ends a stopped code on a frame boundary, its last edge
        drawn down to silence (AHK). That fall is the edge closing the last
        bit of the last frame - read, as a reader on the end of the line reads
        it - and two frames later the code is lost. */
    constexpr int sampleRate = 48000;
    constexpr std::int64_t zero = 1000;
    const auto rate = Rate::fps25;

    LtcGenerator generator;
    generator.prepare (sampleRate);
    generator.setOutput (1, ltcGain);
    REQUIRE (generator.post (Command::start (1, zero, frameOf (label (1, 0, 0, 0), rate), rate)));
    REQUIRE (generator.post (Command::stop (1, zero + 10 * 1920 - 5)));

    const auto heard = hear (render (generator, 0, sampleRate).samples, sampleRate);
    REQUIRE (heard.size() == 11u);

    const auto delay = startOf (heard.front()) - exactStart (zero, 0, sampleRate, rate);
    const auto read = checkRead (heard, 0, 10, zero, frameOf (label (1, 0, 0, 0), rate), 0, sampleRate, rate, delay, 0.1);
    INFO (read.firstWrong);
    CHECK (read.wrong == 0);

    CHECK (heard.back().lost);
    CHECK (startOf (heard.back()) == doctest::Approx (exactStart (zero, 10, sampleRate, rate) + delay + 2.0 * 1920.0).epsilon (1.0e-5));
}

TEST_CASE ("ltc decoder: detectRate tells the six rates apart from a second of frames, and says nothing from three")
{
    /*  AIL: what the labels count to comes from the largest frame number, the
        drop flag from the frames, and 23.976 from 24 and 29.97 from 30 from
        how long the frames are - here, measured from the decoder's own starts
        over a second and a fifth. */
    constexpr int sampleRate = 48000;
    constexpr std::int64_t zero = 1000;

    for (const auto rate : everyRate)
    {
        INFO ("rate " << std::string (wordOf (rate)));

        const auto heard = hear (codeLine (rate, sampleRate, zero, zero + sampleRate + sampleRate / 5), sampleRate);
        REQUIRE (heard.size() > 24u);

        int largest = 0;
        auto drop = true;

        for (const auto& frame : heard)
        {
            largest = std::max (largest, frame.label.frames);
            drop = drop && frame.drop;
        }

        const auto period = (startOf (heard.back()) - startOf (heard.front())) / static_cast<double> (heard.size() - 1);
        const auto detected = detectRate (largest, drop, period, sampleRate);
        REQUIRE (detected.has_value());
        CHECK (*detected == rate);

        //  Three frames show what the labels count to no more than a label does.
        int fewLargest = 0;

        for (std::size_t i = 0; i < 3; ++i)
            fewLargest = std::max (fewLargest, heard[i].label.frames);

        const auto fewPeriod = (startOf (heard[2]) - startOf (heard[0])) / 2.0;
        CHECK_FALSE (detectRate (fewLargest, heard[0].drop, fewPeriod, sampleRate).has_value());
    }

    //  By hand, at 48 kHz: a frame of 2000 samples is 24 a second, 2002 is 23.976, 1600 is 30, 1601.6 is 29.97.
    CHECK (detectRate (23, false, 2000.0, sampleRate) == Rate::fps24);
    CHECK (detectRate (23, false, 2002.0, sampleRate) == Rate::fps23976);
    CHECK (detectRate (24, false, 1920.0, sampleRate) == Rate::fps25);
    CHECK (detectRate (29, false, 1600.0, sampleRate) == Rate::fps30);
    CHECK (detectRate (29, false, 1601.6, sampleRate) == Rate::fps2997);
    CHECK (detectRate (29, true, 1601.6, sampleRate) == Rate::fps2997df);

    //  Nothing for what no rate is, or what the frames do not yet show.
    CHECK_FALSE (detectRate (29, true, 1600.0, sampleRate).has_value());
    CHECK_FALSE (detectRate (24, true, 1920.0, sampleRate).has_value());
    CHECK_FALSE (detectRate (23, true, 2000.0, sampleRate).has_value());
    CHECK_FALSE (detectRate (23, false, 1920.0, sampleRate).has_value());
    CHECK_FALSE (detectRate (24, false, 1920.0 * 1.0006, sampleRate).has_value());
    CHECK_FALSE (detectRate (29, false, 1600.8, sampleRate).has_value());
    CHECK_FALSE (detectRate (22, false, 2000.0, sampleRate).has_value());
    CHECK_FALSE (detectRate (24, false, 1920.0, 0).has_value());
    CHECK_FALSE (detectRate (24, false, 0.0, sampleRate).has_value());
}

TEST_CASE ("ltc decoder: a frame whose ones are odd is read, and says so")
{
    /*  The generator draws only even frames - that is its polarity bit's
        work - so this line is drawn by hand: five frames from 01:00:00:00 at
        25 and 48 kHz, square, a half-bit twelve samples exactly, the second
        with its polarity bit (59, at 25) turned over. Every frame reads with
        its label; the second says its ones are odd and is not refused for it
        - what an odd frame means is TG.5's. The silence after the last one
        closes its last bit, and two frames on, the code is lost. */
    constexpr int sampleRate = 48000;
    constexpr std::size_t halfBit = 12;
    const auto rate = Rate::fps25;
    const auto first = frameOf (label (1, 0, 0, 0), rate);

    std::vector<float> line (1000, 0.0f);
    auto level = -ltcGain;

    for (std::int64_t f = 0; f < 5; ++f)
    {
        auto bits = ltcBits (labelOf (first + f, rate), rate);

        if (f == 1)
            bits[std::size_t { 59 / 8 }] = static_cast<std::uint8_t> (bits[std::size_t { 59 / 8 }] ^ (1u << (59 % 8)));

        for (int b = 0; b < ltcBitsPerFrame; ++b)
        {
            //  Every cell begins with a change, and a one changes again in its middle.
            level = -level;
            line.insert (line.end(), halfBit, level);

            if (ltcBit (bits, b))
                level = -level;

            line.insert (line.end(), halfBit, level);
        }
    }

    line.insert (line.end(), std::size_t { 8000 }, 0.0f);

    const auto heard = hear (line, sampleRate);
    REQUIRE (heard.size() == 6u);

    for (std::size_t f = 0; f < 5; ++f)
    {
        INFO ("frame " << f);
        CHECK_FALSE (heard[f].lost);
        CHECK (heard[f].label == labelOf (first + static_cast<std::int64_t> (f), rate));
        CHECK (heard[f].parityOk == (f != 1));
        CHECK (heard[f].startSample == 1000 + static_cast<std::int64_t> (f) * 1920);
    }

    CHECK (heard[5].lost);
}

TEST_CASE ("ltc decoder: the outbox holds sixty-four frames undrained, the sixty-fifth is dropped and counted")
{
    /*  Sixty-five frames whole and nobody taking: the first sixty-four wait,
        the newest is dropped and counted - what is waiting is older, and the
        tick is told it lost one. Each carries the generation it was read
        under. */
    constexpr int sampleRate = 48000;
    constexpr std::int64_t zero = 1000;
    const auto rate = Rate::fps25;
    const auto line = codeLine (rate, sampleRate, zero, zero + 65 * 1920 + 300);

    LtcDecoder decoder;
    decoder.prepare (sampleRate);

    for (std::size_t at = 0; at < line.size(); at += 512)
        decoder.process (line.data() + at, static_cast<int> (std::min<std::size_t> (512, line.size() - at)),
                         static_cast<std::int64_t> (at));

    std::vector<LtcHeard> heard;
    heard.reserve (static_cast<std::size_t> (LtcDecoder::outboxSize));
    const auto reserved = heard.capacity();

    //  Into the caller's room: nothing allocated by the take.
    CHECK (decoder.take (heard) == 64u);
    CHECK (decoder.droppedCount() == 1);
    CHECK (heard.capacity() == reserved);
    CHECK (heard.front().label == label (1, 0, 0, 0));
    CHECK (heard.back().label == label (1, 0, 2, 13));

    auto stamped = true;

    for (const auto& frame : heard)
        stamped = stamped && frame.generation == decoder.generation();

    CHECK (stamped);

    //  Taken, it is empty.
    CHECK (decoder.take (heard) == 0u);
    CHECK (heard.empty());
}

TEST_CASE ("ltc decoder: prepare forgets the line, moves the generation on, and the next whole frame reads")
{
    /*  An audio restart (as LtcGenerator's prepare is, AHU): half a frame
        heard before it is not finished after it, the generation moves on and
        every frame after carries the new one, read from where its bit 0 began
        on the count that carries on. */
    constexpr int sampleRate = 48000;
    constexpr std::int64_t zero = 1000;
    const auto rate = Rate::fps25;
    const auto startFrame = frameOf (label (1, 0, 0, 0), rate);
    const auto line = codeLine (rate, sampleRate, zero, zero + 2 * sampleRate);

    LtcDecoder decoder;
    decoder.prepare (sampleRate);

    //  Ten frames and half of the eleventh.
    const auto cut = static_cast<std::size_t> (zero + 10 * 1920 + 960);
    const std::vector<float> early (line.begin(), line.begin() + static_cast<std::ptrdiff_t> (cut));
    const std::vector<float> late (line.begin() + static_cast<std::ptrdiff_t> (cut), line.end());

    const auto before = hear (decoder, early, 0, 64);
    REQUIRE (before.size() == 10u);
    const auto generation = decoder.generation();
    CHECK (before.back().generation == generation);

    const auto delay = startOf (before.front()) - exactStart (zero, 0, sampleRate, rate);

    decoder.prepare (sampleRate);
    CHECK (decoder.generation() == generation + 1);

    const auto after = hear (decoder, late, static_cast<std::int64_t> (cut), 64);
    REQUIRE (after.size() > 20u);
    CHECK (after.front().generation == generation + 1);

    //  Frame 10 was half heard: the first read after is frame 11.
    const auto read = checkRead (after, 0, after.size(), zero, startFrame, 11, sampleRate, rate, delay, 0.1);
    INFO (read.firstWrong);
    CHECK (read.wrong == 0);
}
