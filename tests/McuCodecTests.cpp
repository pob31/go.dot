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

/*  A MACKIE SURFACE'S BYTES, BOTH WAYS.

    THE FIXTURES ARE THE CONTROL GUIDE'S, written out by hand from
    docs/D700_CONTROL_GUIDE.md and never from this codec's own output: a codec
    checked against itself proves that it is self-consistent and nothing else,
    and this one's whole job is to agree with a box on a desk. Expected bytes
    are spelled the way the guide prints them - "F0 00 00 66 14 1A ..." - so a
    line here can be checked against the line there by eye, and a failure
    prints two strings a person can compare.

    No engine, no JUCE and no port: the codec is a function of its arguments,
    so every case runs on a machine with no MIDI interface at all.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/surface/McuCodec.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

using namespace wfg::surface;

namespace
{
    /** Bytes as the control guide prints them: upper-case hex, one space apart. */
    std::string hexOf (const Bytes& bytes)
    {
        constexpr char digits[] = "0123456789ABCDEF";
        std::string out;

        for (const auto byte : bytes)
        {
            if (! out.empty())
                out += ' ';

            out += digits[byte >> 4];
            out += digits[byte & 0x0f];
        }

        return out;
    }

    /** A display field's text in the same form, for the middle of a SysEx. */
    std::string hexOfText (std::string_view text)
    {
        Bytes bytes;

        for (const auto c : text)
            bytes.push_back (static_cast<std::uint8_t> (c));

        return hexOf (bytes);
    }
}

//==============================================================================
TEST_CASE ("mcu codec: a fader arrives as fourteen bits, the low seven first")
{
    /*  E<n> <lsb> <msb>, value = (msb << 7) | lsb (control guide §3.1). The
        centre is the one everybody reads backwards once: 00 40, not 40 00. */
    const auto top = decodeMcu ({ 0xe3, 0x7f, 0x7f });
    REQUIRE (top.has_value());
    CHECK (top->kind == McuEvent::Kind::fader);
    CHECK (top->strip == 3);
    CHECK (top->value == 16383);

    const auto bottom = decodeMcu ({ 0xe0, 0x00, 0x00 });
    REQUIRE (bottom.has_value());
    CHECK (bottom->kind == McuEvent::Kind::fader);
    CHECK (bottom->strip == 0);
    CHECK (bottom->value == 0);

    const auto centre = decodeMcu ({ 0xe0, 0x00, 0x40 });
    REQUIRE (centre.has_value());
    CHECK (centre->value == 8192);

    /*  THE NINTH CHANNEL IS MCU'S MASTER, which on the D700 is the volume knob
        (control guide §3.4). It carries strip 8, the number faderPosition
        takes for the master, so an echo needs no special case. */
    const auto master = decodeMcu ({ 0xe8, 0x12, 0x34 });
    REQUIRE (master.has_value());
    CHECK (master->kind == McuEvent::Kind::masterFader);
    CHECK (master->strip == 8);
    CHECK (master->value == ((0x34 << 7) | 0x12));

    //  And no further: channels 10 to 16 carry no fader.
    CHECK_FALSE (decodeMcu ({ 0xe9, 0x00, 0x40 }).has_value());
    CHECK_FALSE (decodeMcu ({ 0xef, 0x7f, 0x7f }).has_value());
}

TEST_CASE ("mcu codec: a touch is note 0x68 plus the strip, and its release is either spelling")
{
    const auto touched = decodeMcu ({ 0x90, 0x6a, 0x7f });
    REQUIRE (touched.has_value());
    CHECK (touched->kind == McuEvent::Kind::touch);
    CHECK (touched->strip == 2);
    CHECK (touched->down);

    /*  VELOCITY NOUGHT AND A NOTE-OFF ARE BOTH A RELEASE, whatever velocity
        the note-off carries. The D700 sends the first; plenty of surfaces send
        the second. */
    for (const auto& release : { Bytes { 0x90, 0x6a, 0x00 },
                                 Bytes { 0x80, 0x6a, 0x00 },
                                 Bytes { 0x80, 0x6a, 0x40 } })
    {
        INFO (hexOf (release));
        const auto released = decodeMcu (release);
        REQUIRE (released.has_value());
        CHECK (released->kind == McuEvent::Kind::touch);
        CHECK (released->strip == 2);
        CHECK_FALSE (released->down);
    }

    //  Any velocity but nought is a touch: 127 is usual, not required.
    const auto light = decodeMcu ({ 0x90, 0x6f, 0x01 });
    REQUIRE (light.has_value());
    CHECK (light->kind == McuEvent::Kind::touch);
    CHECK (light->strip == 7);
    CHECK (light->down);

    /*  THE MASTER'S TOUCH, which the D700 never sends - its volume knob has
        no touch sense (control guide §3.4) - and a generic MCU does. */
    const auto master = decodeMcu ({ 0x90, 0x70, 0x7f });
    REQUIRE (master.has_value());
    CHECK (master->kind == McuEvent::Kind::masterTouch);
    CHECK (master->strip == 8);
    CHECK (master->down);
}

TEST_CASE ("mcu codec: an encoder step is sign-magnitude, so 65 is one step back and not sixty-three")
{
    /*  BIT 6 IS THE DIRECTION AND BITS 0-5 THE SIZE (control guide §3.2). A
        two's-complement reader takes 65 as -63, which is the bug this case
        exists for - so it is read both ways, at every size. */
    const auto forward = decodeMcu ({ 0xb0, 0x10, 0x01 });
    REQUIRE (forward.has_value());
    CHECK (forward->kind == McuEvent::Kind::encoder);
    CHECK (forward->strip == 0);
    CHECK (forward->value == 1);

    const auto back = decodeMcu ({ 0xb0, 0x13, 0x41 });
    REQUIRE (back.has_value());
    CHECK (back->kind == McuEvent::Kind::encoder);
    CHECK (back->strip == 3);
    CHECK (back->value == -1);
    CHECK (back->value != -63);

    const auto backThree = decodeMcu ({ 0xb0, 0x17, 0x43 });
    REQUIRE (backThree.has_value());
    CHECK (backThree->strip == 7);
    CHECK (backThree->value == -3);

    for (int size = 1; size <= 63; ++size)
    {
        INFO ("size " << size);
        const auto clockwise = decodeMcu ({ 0xb0, 0x15, static_cast<std::uint8_t> (size) });
        const auto anticlockwise = decodeMcu ({ 0xb0, 0x15, static_cast<std::uint8_t> (0x40 | size) });
        REQUIRE (clockwise.has_value());
        REQUIRE (anticlockwise.has_value());
        CHECK (clockwise->strip == 5);
        CHECK (clockwise->value == size);
        CHECK (anticlockwise->value == -size);
    }

    //  A step of nothing is nothing, and so is "minus nothing".
    const auto still = decodeMcu ({ 0xb0, 0x10, 0x00 });
    const auto stillBackwards = decodeMcu ({ 0xb0, 0x10, 0x40 });
    REQUIRE (still.has_value());
    REQUIRE (stillBackwards.has_value());
    CHECK (still->value == 0);
    CHECK (stillBackwards->value == 0);

    //  The jog wheel speaks the same way.
    const auto jog = decodeMcu ({ 0xb0, 0x3c, 0x42 });
    REQUIRE (jog.has_value());
    CHECK (jog->kind == McuEvent::Kind::jog);
    CHECK (jog->value == -2);
}

TEST_CASE ("mcu codec: every button note has its standard name, and every name its note")
{
    /*  THE FIVE PER-STRIP ROWS, eight wide, the index being the strip - and a
        decoded press carries that strip too. */
    const std::array<Button, 5> rows { { Button::rec, Button::solo, Button::mute,
                                         Button::select, Button::vpotPress } };

    for (std::size_t row = 0; row < rows.size(); ++row)
        for (int strip = 0; strip < 8; ++strip)
        {
            const auto note = static_cast<int> (row) * 8 + strip;
            INFO ("note " << note);

            const auto id = buttonForNote (note);
            CHECK (id.button == rows[row]);
            CHECK (id.index == strip);

            const auto pressed = decodeMcu ({ 0x90, static_cast<std::uint8_t> (note), 0x7f });
            REQUIRE (pressed.has_value());
            CHECK (pressed->kind == McuEvent::Kind::button);
            CHECK (pressed->id == id);
            CHECK (pressed->strip == strip);
            CHECK (pressed->down);
        }

    CHECK (buttonForNote (0x2e) == ButtonId { Button::bankLeft, -1 });
    CHECK (buttonForNote (0x2f) == ButtonId { Button::bankRight, -1 });

    for (int position = 0; position < 8; ++position)
        CHECK (buttonForNote (0x36 + position) == ButtonId { Button::function, position });

    CHECK (buttonForNote (0x5b) == ButtonId { Button::rewind, -1 });
    CHECK (buttonForNote (0x5c) == ButtonId { Button::forward, -1 });
    CHECK (buttonForNote (0x5d) == ButtonId { Button::stop, -1 });
    CHECK (buttonForNote (0x5e) == ButtonId { Button::play, -1 });
    CHECK (buttonForNote (0x5f) == ButtonId { Button::record, -1 });

    /*  A button that is one of a kind is no strip's, and its release is either
        spelling, exactly as a touch's is. */
    const auto play = decodeMcu ({ 0x90, 0x5e, 0x7f });
    REQUIRE (play.has_value());
    CHECK (play->id == ButtonId { Button::play, -1 });
    CHECK (play->strip == -1);
    CHECK (play->down);

    const auto playReleased = decodeMcu ({ 0x80, 0x5e, 0x7f });
    REQUIRE (playReleased.has_value());
    CHECK (playReleased->id == ButtonId { Button::play, -1 });
    CHECK_FALSE (playReleased->down);

    /*  STANDARD NAMES, NOT THE D700'S. Its master dial presses as 0x38, which
        is F3 to Mackie, and its `*` is F1 under the Mackie preset but global
        solo under Reaper's (control guide §1.2) - which is exactly why what a
        button means belongs to a profile and not here. */
    CHECK (buttonForNote (0x38) == ButtonId { Button::function, 2 });
    CHECK (buttonForNote (0x36) == ButtonId { Button::function, 0 });
    CHECK (buttonForNote (0x5a) == ButtonId { Button::globalSolo, -1 });

    //  THE WHOLE MAP ROUND TRIPS: every note from 0x00 to 0x67 is a button.
    for (int note = 0x00; note <= 0x67; ++note)
    {
        INFO ("note " << note);
        const auto id = buttonForNote (note);
        CHECK (id.button != Button::none);
        CHECK (noteForButton (id) == note);
    }

    //  A FADER'S TOUCH IS NOT A BUTTON, and nor is anything past it.
    for (int note = 0x68; note <= 0x7f; ++note)
        CHECK (buttonForNote (note).button == Button::none);

    CHECK (buttonForNote (-1).button == Button::none);
    CHECK (buttonForNote (0x80).button == Button::none);

    //  And a ButtonId buttonForNote would never make names no note.
    CHECK (noteForButton (ButtonId { Button::none, -1 }) == -1);
    CHECK (noteForButton (ButtonId { Button::rec, 8 }) == -1);
    CHECK (noteForButton (ButtonId { Button::rec, -1 }) == -1);
    CHECK (noteForButton (ButtonId { Button::play, 0 }) == -1);
}

TEST_CASE ("mcu codec: the connection handshake yields the unit's id and serial")
{
    /*  device -> host   F0 00 00 66 14 01 <7-byte serial> <4-byte challenge> F7
        (protocol §2.1). The serial is the prize: it survives USB renumbering. */
    const Bytes query { 0xf0, 0x00, 0x00, 0x66, 0x14, 0x01,
                        'D', '7', '0', '0', 'R', 'T', 'B',
                        0x01, 0x02, 0x03, 0x04,
                        0xf7 };

    const auto asked = decodeMcu (query);
    REQUIRE (asked.has_value());
    CHECK (asked->kind == McuEvent::Kind::hostConnectionQuery);
    CHECK (asked->deviceId == 0x14);
    CHECK (asked->serial == "D700RTB");
    CHECK (asked->challenge == std::array<std::uint8_t, 4> { { 0x01, 0x02, 0x03, 0x04 } });

    //  The D700's second bank reports itself as an Extender.
    auto fromBankTwo = query;
    fromBankTwo[4] = 0x15;
    const auto second = decodeMcu (fromBankTwo);
    REQUIRE (second.has_value());
    CHECK (second->kind == McuEvent::Kind::hostConnectionQuery);
    CHECK (second->deviceId == 0x15);

    //  device -> host   F0 00 00 66 14 03 <serial> F7        accepted
    const auto confirmed = decodeMcu ({ 0xf0, 0x00, 0x00, 0x66, 0x14, 0x03,
                                        'D', '7', '0', '0', 'R', 'T', 'B', 0xf7 });
    REQUIRE (confirmed.has_value());
    CHECK (confirmed->kind == McuEvent::Kind::hostConnectionConfirmation);
    CHECK (confirmed->deviceId == 0x14);
    CHECK (confirmed->serial == "D700RTB");

    //  ... and 04 where the reply was refused.
    const auto refused = decodeMcu ({ 0xf0, 0x00, 0x00, 0x66, 0x14, 0x04,
                                      'D', '7', '0', '0', 'R', 'T', 'B', 0xf7 });
    REQUIRE (refused.has_value());
    CHECK (refused->kind == McuEvent::Kind::hostConnectionError);
    CHECK (refused->serial == "D700RTB");

    /*  A TRUNCATED ONE IS NOTHING, not a query with a zero where the lost byte
        was: here one byte of the challenge has gone, and here the F7. */
    auto shortChallenge = query;
    shortChallenge.erase (shortChallenge.end() - 2);
    CHECK_FALSE (decodeMcu (shortChallenge).has_value());

    const Bytes unterminated (query.begin(), query.end() - 1);
    CHECK_FALSE (decodeMcu (unterminated).has_value());

    //  A status byte inside the serial ends a SysEx on the wire; it is refused.
    auto brokenSerial = query;
    brokenSerial[8] = 0xb0;
    CHECK_FALSE (decodeMcu (brokenSerial).has_value());

    /*  NOT MACKIE'S: a universal identity request, and somebody else's
        manufacturer id around an otherwise perfect query. */
    CHECK_FALSE (decodeMcu ({ 0xf0, 0x7e, 0x7f, 0x06, 0x01, 0xf7 }).has_value());

    auto otherMaker = query;
    otherMaker[2] = 0x20;
    otherMaker[3] = 0x32;
    CHECK_FALSE (decodeMcu (otherMaker).has_value());

    //  And a Mackie command a surface never sends - our own display write, echoed.
    CHECK_FALSE (decodeMcu ({ 0xf0, 0x00, 0x00, 0x66, 0x14, 0x12, 0x00, 0x41, 0xf7 }).has_value());
}

TEST_CASE ("mcu codec: a pad controller's messages are classified by the status byte")
{
    const auto hit = decodePads ({ 0x99, 0x24, 0x64 });
    REQUIRE (hit.has_value());
    CHECK (hit->kind == PadEvent::Kind::noteOn);
    CHECK (hit->channel == 10);
    CHECK (hit->note == 36);
    CHECK (hit->value == 100);

    /*  A NOTE-ON OF VELOCITY NOUGHT STAYS A NOTE-ON, as `midi::eventFrom`
        reports it: what the wire said is what is reported, and that it means
        a release is the bridge's to decide. */
    const auto zero = decodePads ({ 0x99, 0x24, 0x00 });
    REQUIRE (zero.has_value());
    CHECK (zero->kind == PadEvent::Kind::noteOn);
    CHECK (zero->note == 36);
    CHECK (zero->value == 0);

    const auto off = decodePads ({ 0x89, 0x24, 0x40 });
    REQUIRE (off.has_value());
    CHECK (off->kind == PadEvent::Kind::noteOff);
    CHECK (off->channel == 10);
    CHECK (off->note == 36);
    CHECK (off->value == 64);

    const auto pressed = decodePads ({ 0xa9, 0x24, 0x50 });
    REQUIRE (pressed.has_value());
    CHECK (pressed->kind == PadEvent::Kind::polyPressure);
    CHECK (pressed->note == 36);
    CHECK (pressed->value == 80);

    const auto aftertouch = decodePads ({ 0xd9, 0x30 });
    REQUIRE (aftertouch.has_value());
    CHECK (aftertouch->kind == PadEvent::Kind::channelPressure);
    CHECK (aftertouch->channel == 10);
    CHECK (aftertouch->note == -1);
    CHECK (aftertouch->value == 48);

    //  Channels are one-based, as every device prints them.
    const auto lowest = decodePads ({ 0x90, 0x3c, 0x7f });
    const auto highest = decodePads ({ 0x9f, 0x3c, 0x7f });
    REQUIRE (lowest.has_value());
    REQUIRE (highest.has_value());
    CHECK (lowest->channel == 1);
    CHECK (highest->channel == 16);

    //  Anything that is not a press or a pressure, and anything malformed.
    const Bytes refused[] = {
        { 0xb9, 0x07, 0x7f },       // a controller
        { 0xe9, 0x00, 0x40 },       // pitch bend
        { 0xc9, 0x01 },             // a program change
        {},
        { 0x99, 0x24 },             // a note with no velocity
        { 0x99, 0x24, 0x64, 0x00 }, // and one with a byte too many
        { 0xd9 },                   // pressure with no value
        { 0xd9, 0x30, 0x00 },
        { 0x99, 0xa4, 0x64 },       // a data byte with its top bit set
        { 0x24, 0x64 },             // running status, which JUCE never delivers
        { 0xf0, 0x00, 0x00, 0x66, 0x14, 0x00, 0xf7 },
    };

    for (const auto& message : refused)
    {
        INFO (hexOf (message));
        CHECK_FALSE (decodePads (message).has_value());
    }
}

TEST_CASE ("mcu codec: a malformed or foreign message is refused, never guessed")
{
    /*  Each of these is a byte away from something real, which is the point:
        a decoder that guessed would hand the engine a change nobody made. */
    const Bytes refused[] = {
        {},
        { 0xe0, 0x00 },                 // a fader missing its top half
        { 0xe0, 0x00, 0x40, 0x00 },     // and one with a byte too many
        { 0x6a, 0x7f },                 // running status, which JUCE never delivers
        { 0x6a, 0x7f, 0x00 },
        { 0xe0, 0x80, 0x00 },           // a data byte with its top bit set
        { 0x90, 0x6a, 0xff },
        { 0x91, 0x6a, 0x7f },           // channel 1 only: channel 2 is the D700's red, going out
        { 0xb1, 0x10, 0x01 },           // and so do controllers
        { 0x90, 0x71, 0x7f },           // a note past the touches, which nothing presses
        { 0xb0, 0x30, 0x01 },           // a ring's controller, which only ever goes out
        { 0xb0, 0x18, 0x01 },           // one past the last V-Pot
        { 0xd0, 0x3b },                 // a meter, likewise only ever outbound
        { 0xc0, 0x01 },                 // a program change: not MCU
        { 0xa0, 0x10, 0x40 },           // poly pressure: not MCU
        { 0xf8 },                       // clock
        { 0xf2, 0x00, 0x00 },           // song position
    };

    for (const auto& message : refused)
    {
        INFO (hexOf (message));
        CHECK_FALSE (decodeMcu (message).has_value());
    }
}

//==============================================================================
TEST_CASE ("mcu codec: what goes out to a Mackie surface, byte for byte")
{
    //  Control guide §4.1, §4.2, §4.3, §4.7.
    CHECK (hexOf (faderPosition (5, 16383)) == "E5 7F 7F");
    CHECK (hexOf (faderPosition (0, 8192)) == "E0 00 40");
    CHECK (hexOf (faderPosition (0, 0)) == "E0 00 00");
    CHECK (hexOf (faderPosition (8, 16383)) == "E8 7F 7F");      // the master

    CHECK (hexOf (led (0x5e, Led::on)) == "90 5E 7F");
    CHECK (hexOf (led (0x5e, Led::flash)) == "90 5E 01");
    CHECK (hexOf (led (0x5e, Led::off)) == "90 5E 00");

    CHECK (hexOf (ringMcu (2, 6, 1, false)) == "B0 32 16");
    CHECK (hexOf (ringMcu (7, 11, 3, true)) == "B0 37 7B");
    CHECK (hexOf (ringMcu (0, 0, 0, false)) == "B0 30 00");      // position 0 lights nothing

    /*  THE D700'S RING: the channel is the mode, the value is 0..127. */
    CHECK (hexOf (d700Ring (4, 127, 2)) == "B2 34 7F");         // fill from the left
    CHECK (hexOf (d700Ring (1, 64, 1)) == "B1 31 40");          // a pan at the centre
    CHECK (hexOf (d700Ring (0, 0, 0)) == "B0 30 00");

    CHECK (hexOf (meter (3, 11)) == "D0 3B");
    CHECK (hexOf (meterClearPeak (3)) == "D0 3F");
    CHECK (hexOf (meter (7, 0)) == "D0 70");

    CHECK (hexOf (deviceQuery (0x14)) == "F0 00 00 66 14 00 F7");
}

TEST_CASE ("mcu codec: a value out of range is clamped, an address out of range sends nothing")
{
    //  A value past the end still means "as far as it goes".
    CHECK (hexOf (faderPosition (0, -5)) == "E0 00 00");
    CHECK (hexOf (faderPosition (0, 20000)) == "E0 7F 7F");
    CHECK (hexOf (ringMcu (0, 12, 4, true)) == "B0 30 7B");
    CHECK (hexOf (ringMcu (0, -1, -1, false)) == "B0 30 00");
    CHECK (hexOf (d700Ring (0, 200, 7)) == "B2 30 7F");
    CHECK (hexOf (d700Ring (0, -1, -1)) == "B0 30 00");
    CHECK (hexOf (d700Colour (0x20, -1, 128, 64)) == "91 20 00 92 20 7F 93 20 40");

    /*  A LEVEL STOPS AT 0x0E, because 0x0F is not a level but the peak-hold
        reset: a meter pinned at the top must not clear its own peak. */
    CHECK (hexOf (meter (0, 15)) == "D0 0E");
    CHECK (hexOf (meter (0, -1)) == "D0 00");

    //  But a strip, a note or a row that does not exist is nothing at all.
    CHECK (faderPosition (9, 0).empty());
    CHECK (faderPosition (-1, 0).empty());
    CHECK (led (0x80, Led::on).empty());
    CHECK (led (-1, Led::on).empty());
    CHECK (ringMcu (8, 1, 0, false).empty());
    CHECK (ringMcu (-1, 1, 0, false).empty());
    CHECK (meter (8, 1).empty());
    CHECK (meterClearPeak (-1).empty());
    CHECK (d700Ring (8, 1, 2).empty());
    CHECK (lcd (0x14, 112, "x").empty());
    CHECK (lcd (0x14, -1, "x").empty());
    CHECK (lcd (0x80, 0, "x").empty());
    CHECK (lcdCell (0x14, 2, 0, "x").empty());
    CHECK (lcdCell (0x14, 0, 8, "x").empty());
    CHECK (deviceQuery (0x80).empty());
    CHECK (d700DisplayRow (8, 0, "x").empty());
    CHECK (d700DisplayRow (0, 2, "x").empty());
    CHECK (d700DisplayRow (0, -1, "x").empty());
    CHECK (d700DisplayRow3 (8, "x").empty());
    CHECK (d700DisplayRow3 (-1, "x").empty());
}

TEST_CASE ("mcu codec: an MCU display cell is always exactly seven characters")
{
    /*  THE BUFFER IS FLAT AND A WRITE REPLACES ONLY THE BYTES SENT (control
        guide §4.6): "PORT-1" written over "S21 HIJACK" rendered as "PORT-1J".
        So a cell is padded to seven, always. */
    CHECK (hexOf (lcdCell (0x14, 0, 0, "PORT-1"))
           == "F0 00 00 66 14 12 00 " + hexOfText ("PORT-1 ") + " F7");

    //  0x38 is the lower row, and a strip is seven along: 0x38 + 7 * 7 = 0x69.
    CHECK (hexOf (lcdCell (0x14, 1, 7, "x"))
           == "F0 00 00 66 14 12 69 " + hexOfText ("x      ") + " F7");

    CHECK (hexOf (lcdCell (0x14, 0, 3, "Kick"))
           == "F0 00 00 66 14 12 15 " + hexOfText ("Kick   ") + " F7");

    //  A long name is cut, never spilled into the next strip's cell.
    CHECK (hexOf (lcdCell (0x14, 0, 0, "Twenty characters!!!"))
           == "F0 00 00 66 14 12 00 " + hexOfText ("Twenty ") + " F7");

    //  And folded: a display takes ASCII.
    CHECK (hexOf (lcdCell (0x15, 1, 0, "Écho"))
           == "F0 00 00 66 15 12 38 " + hexOfText ("Echo   ") + " F7");

    /*  THE RAW WRITE writes what it is given - folded, not padded - and stops
        where the buffer does: at 0x6E there is room for two characters. */
    CHECK (hexOf (lcd (0x14, 0x00, "Kick"))
           == "F0 00 00 66 14 12 00 " + hexOfText ("Kick") + " F7");
    CHECK (hexOf (lcd (0x14, 0x6e, "Écho"))
           == "F0 00 00 66 14 12 6E " + hexOfText ("Ec") + " F7");
}

TEST_CASE ("mcu codec: the D700's own display rows, with the row byte one-based")
{
    /*  Control guide §5.1, for strip n:
            F0 00 00 66 14 1A <n*12> 01 "Kick In     " F7      name
            F0 00 00 66 14 1A <n*12> 02 "-6.2 dB     " F7      value
            F0 00 00 66 14 19 <n*8>     "GATE    "     F7      tag
        THE ROW BYTE IS ONE-BASED ON THE WIRE: 01 is the top row. */
    CHECK (hexOf (d700DisplayRow (0, 0, "Kick In"))
           == "F0 00 00 66 14 1A 00 01 " + hexOfText ("Kick In     ") + " F7");

    //  Strip 2 starts at 2 * 12 = 0x18, and the middle row is 02.
    CHECK (hexOf (d700DisplayRow (2, 1, "-6.2 dB"))
           == "F0 00 00 66 14 1A 18 02 " + hexOfText ("-6.2 dB     ") + " F7");

    //  The third row is eight wide: strip 3 starts at 3 * 8 = 0x18 too.
    CHECK (hexOf (d700DisplayRow3 (3, "GATE"))
           == "F0 00 00 66 14 19 18 " + hexOfText ("GATE    ") + " F7");

    //  Folded and cut to the field: strip 7 is at 7 * 12 = 0x54.
    CHECK (hexOf (d700DisplayRow (7, 0, "Entrée à jardin"))
           == "F0 00 00 66 14 1A 54 01 " + hexOfText ("Entree a jar") + " F7");
    CHECK (hexOf (d700DisplayRow3 (7, "Réverbération"))
           == "F0 00 00 66 14 19 38 " + hexOfText ("Reverber") + " F7");

    //  The track numbers, one per strip and clamped into seven bits.
    CHECK (hexOf (d700TrackNumbers ({ { 1, 2, 3, 4, 5, 6, 7, 200 } }))
           == "F0 00 00 66 14 17 00 01 02 03 04 05 06 07 7F F7");
    CHECK (hexOf (d700TrackNumbers ({ { -3, 9, 10, 11, 12, 13, 14, 16 } }))
           == "F0 00 00 66 14 17 00 00 09 0A 0B 0C 0D 0E 10 F7");
}

TEST_CASE ("mcu codec: D700 colour is three note-ons, blue last, on seventeen elements only")
{
    /*  Control guide §4.4: 91 <note> <r>, 92 <note> <g>, 93 <note> <b> - and
        255, which is past seven bits, clamps. BLUE LAST: the element refreshes
        when blue arrives. */
    CHECK (hexOf (d700Colour (0x22, 255, 64, 0)) == "91 22 7F 92 22 40 93 22 00");

    //  The master dial takes colour at its own press note.
    CHECK (d700Colour (0x38, 10, 20, 30).size() == 9);
    CHECK (hexOf (d700Colour (0x38, 10, 20, 30)) == "91 38 0A 92 38 14 93 38 1E");

    //  A note with no RGB behind it answers nothing, rather than painting nothing.
    CHECK (d700Colour (0x30, 1, 2, 3).empty());
    CHECK (d700Colour (0x18, 1, 2, 3).empty());      // a select button: one colour
    CHECK (d700Colour (-1, 1, 2, 3).empty());

    /*  SEVENTEEN ELEMENTS ARE NINE NOTES: a bank's eight V-Pots and the master
        dial, the other bank's eight being the same notes on the other port. */
    int coloured = 0;

    for (int note = 0; note < 0x80; ++note)
        if (! d700Colour (note, 1, 2, 3).empty())
            ++coloured;

    CHECK (coloured == 9);
}

//==============================================================================
TEST_CASE ("mcu codec: a show's names fold to what a seven-bit display can show")
{
    /*  The literals here are UTF-8 because the source is, and MSVC is told so
        with /utf-8. This proves the premise first, so a mangled literal fails
        here and not as a mystery further down. */
    REQUIRE (std::string ("É") == "\xc3\x89");

    CHECK (asciiFold ("Écho à l'été") == "Echo a l'ete");
    CHECK (asciiFold ("Œuvre") == "OEuvre");
    CHECK (asciiFold ("straße") == "strasse");
    CHECK (asciiFold ("Ça, c'est Noël : déjà vu, garçon") == "Ca, c'est Noel : deja vu, garcon");
    CHECK (asciiFold ("Ægir Øresund Ångström") == "AEgir Oresund Angstrom");
    CHECK (asciiFold ("Škoda Žižek Łódź Dvořák") == "Skoda Zizek Lodz Dvorak");

    //  Printable ASCII is left exactly as it is, every character of it.
    std::string printable;

    for (char c = 0x20; c < 0x7f; ++c)
        printable += c;

    CHECK (asciiFold (printable) == printable);

    //  Curly quotes, guillemets and dashes have ASCII twins, and so has "…".
    CHECK (asciiFold ("l’été") == "l'ete");
    CHECK (asciiFold ("“Go” – “Stop” — fin…") == "\"Go\" - \"Stop\" - fin...");
    CHECK (asciiFold ("N° 3") == "No 3");

    /*  FRENCH PUTS A SPACE INSIDE ITS QUOTES, and it is a no-break one - or a
        narrow no-break one - and either is a space. */
    CHECK (asciiFold ("«\xc2\xa0" "bis\xe2\x80\xaf»") == "\" bis \"");

    /*  A LETTER TYPED DECOMPOSED - an E and then a combining acute, which is
        how a name copied from a macOS file name arrives - keeps its letter. */
    CHECK (asciiFold ("E\xcc\x81" "cho") == "Echo");

    //  Control characters are gaps; invisible characters stay invisible.
    CHECK (asciiFold ("a\tb\nc\x7f" "d") == "a b c d");
    CHECK (asciiFold ("soft\xc2\xad" "hyphen") == "softhyphen");
    CHECK (asciiFold ("\xef\xbb\xbf" "BOM") == "BOM");

    //  Anything else is one '?' for each CHARACTER, never one for each byte.
    CHECK (asciiFold ("日本") == "??");
    CHECK (asciiFold ("🎛 desk") == "? desk");
    CHECK (asciiFold ("Ωmega") == "?mega");

    /*  MALFORMED UTF-8 NEVER CRASHES, each bad byte is one '?', and reading
        resumes at the next byte - so what follows a bad byte survives it. */
    const auto garbage = asciiFold ("\xff\xfe");
    CHECK_FALSE (garbage.empty());
    CHECK (garbage.find_first_not_of ('?') == std::string::npos);
    CHECK (garbage == "??");

    CHECK (asciiFold ("caf\xc3") == "caf?");                // cut off in the middle
    CHECK (asciiFold ("\xc3" "A") == "?A");                 // a lead with no continuation
    CHECK (asciiFold ("\x80" "x") == "?x");                 // a continuation with no lead
    CHECK (asciiFold ("\xc0\xaf") == "??");                 // an overlong '/'
    CHECK (asciiFold ("\xed\xa0\x80") == "???");            // a surrogate, which UTF-8 may not carry
    CHECK (asciiFold ("\xf4\x90\x80\x80") == "????");       // past U+10FFFF
    CHECK (asciiFold ("") == "");

    //  fitText: folded first, then cut or padded to exactly the width.
    CHECK (fitText ("Entrée à jardin", 12) == "Entree a jar");
    CHECK (fitText ("ab", 5) == "ab   ");
    CHECK (fitText ("abc", 3) == "abc");
    CHECK (fitText ("abc", 0) == "");
    CHECK (fitText ("straße", 5) == "stras");
    CHECK (fitText ("", 3) == "   ");
}

TEST_CASE ("mcu codec: only the six established command bytes are safe to send")
{
    /*  EVERY SYSEX THIS CODEC WRITES PASSES, which is what lets the bridge
        assert it before anything leaves. */
    for (const auto& written : { lcd (0x14, 0x00, "Kick"),
                                 lcd (0x15, 0x6e, "Écho"),
                                 lcdCell (0x14, 1, 7, "Snare"),
                                 deviceQuery (0x14),
                                 d700DisplayRow (0, 0, "Kick In"),
                                 d700DisplayRow (7, 1, "-6.2 dB"),
                                 d700DisplayRow3 (3, "GATE"),
                                 d700TrackNumbers ({ { 1, 2, 3, 4, 5, 6, 7, 8 } }) })
    {
        INFO (hexOf (written));
        CHECK (isSafeSysEx (written));
    }

    /*  THE SWEEP THAT NEEDED A RESTART (control guide §6, gotcha 2): of every
        command byte there is, exactly six are allowed. */
    int allowed = 0;

    for (int command = 0x00; command <= 0x7f; ++command)
        if (isSafeSysEx ({ 0xf0, 0x00, 0x00, 0x66, 0x14, static_cast<std::uint8_t> (command), 0xf7 }))
            ++allowed;

    CHECK (allowed == 6);

    //  And nothing that is not one of the six, or not framed as Mackie's.
    const Bytes refused[] = {
        { 0xf0, 0x00, 0x00, 0x66, 0x14, 0x0a, 0xf7 },                 // documented destructive
        { 0xf0, 0x00, 0x00, 0x66, 0x14, 0x72, 0x01, 0x02, 0xf7 },     // safe, and not ours to send
        { 0xf0, 0x00, 0x00, 0x66, 0x14, 0x01, 0xf7 },                 // the surface's own message
        { 0x00, 0x00, 0x66, 0x14, 0x12, 0x00, 0xf7 },                 // no F0
        { 0xf0, 0x00, 0x00, 0x66, 0x14, 0x12, 0x00, 0x41 },           // no F7
        {},
        { 0xf0, 0x00, 0x20, 0x32, 0x14, 0x12, 0x00, 0xf7 },           // another maker's
        { 0xf0, 0x00, 0x00, 0x66, 0x14, 0x12, 0x00, 0xc1, 0xf7 },     // a status byte inside
        { 0xf0, 0x00, 0x00, 0x66, 0x14, 0xf7 },                       // no command at all
        { 0xe0, 0x00, 0x00 },                                         // not a SysEx
    };

    for (const auto& message : refused)
    {
        INFO (hexOf (message));
        CHECK_FALSE (isSafeSysEx (message));
    }
}

TEST_CASE ("mcu codec: a fader position decodes back to itself")
{
    for (int strip = 0; strip < 8; ++strip)
        for (const auto value : { 0, 1, 127, 128, 8191, 8192, 16382, 16383 })
        {
            INFO ("strip " << strip << ", value " << value);
            const auto event = decodeMcu (faderPosition (strip, value));
            REQUIRE (event.has_value());
            CHECK (event->kind == McuEvent::Kind::fader);
            CHECK (event->strip == strip);
            CHECK (event->value == value);
        }

    const auto master = decodeMcu (faderPosition (8, 12345));
    REQUIRE (master.has_value());
    CHECK (master->kind == McuEvent::Kind::masterFader);
    CHECK (master->strip == 8);
    CHECK (master->value == 12345);
}
