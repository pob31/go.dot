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
    The wires a message is rendered on (namespace draft §57, AFJ; DP.7): RCP's
    line from an address and atoms, bytes exact, and a console's reply taken
    apart. Pure functions; no socket, no table.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/tree/Wires.h>

#include <string>
#include <vector>

using namespace wfg;
using namespace wfg::tree::wire;

TEST_CASE ("wire: an RCP set is the verb, the parameter, X and Y from nought, and the atoms")
{
    //  The preset spells X and Y as the last segments, from one (AFH).
    CHECK (renderRcp ("/MIXER:Current/InCh/Fader/Level/1/1", { osc::Value::int32 (-32768) }, {})
             == "set MIXER:Current/InCh/Fader/Level 0 0 -32768");
    CHECK (renderRcp ("/MIXER:Current/InCh/ToMix/Level/12/3", { osc::Value::int32 (0) }, {})
             == "set MIXER:Current/InCh/ToMix/Level 11 2 0");

    //  One index spelled: Y is nought, as the console wants a column there.
    CHECK (renderRcp ("/MIXER:Current/St/Fader/Level/1", { osc::Value::int32 (1000) }, { "set", 1 })
             == "set MIXER:Current/St/Fader/Level 0 0 1000");

    //  Inferred where the node said nothing: the trailing whole numbers, two at most.
    CHECK (renderRcp ("/MIXER:Current/St/Fader/Level/2", { osc::Value::int32 (0) }, {})
             == "set MIXER:Current/St/Fader/Level 1 0 0");
    CHECK (renderRcp ("/MIXER:Current/DCA/Label/Name/3", { osc::Value::string ("Band") }, {})
             == "set MIXER:Current/DCA/Label/Name 2 0 \"Band\"");

    //  Said explicitly, a segment that looks like a number is kept as the parameter's.
    CHECK (renderRcp ("/MIXER:Current/Thing/7/2", { osc::Value::int32 (5) }, { "set", 1 })
             == "set MIXER:Current/Thing/7 1 0 5");
}

TEST_CASE ("wire: RCP atoms - a float rounded, a bool as 1 or 0, a string quoted and escaped, several in order")
{
    CHECK (renderRcp ("/MIXER:Current/InCh/Fader/Level/1/1", { osc::Value::float32 (-999.6f) }, {})
             == "set MIXER:Current/InCh/Fader/Level 0 0 -1000");
    CHECK (renderRcp ("/MIXER:Current/InCh/Fader/On/1/1", { osc::Value::boolean (true) }, {})
             == "set MIXER:Current/InCh/Fader/On 0 0 1");
    CHECK (renderRcp ("/MIXER:Current/InCh/Label/Name/1/1", { osc::Value::string ("Vox \"lead\" \\ 1") }, {})
             == "set MIXER:Current/InCh/Label/Name 0 0 \"Vox \\\"lead\\\" \\\\ 1\"");
    CHECK (renderRcp ("/MIXER:Current/InCh/Thing/1/1", { osc::Value::int32 (1), osc::Value::string ("a"), osc::Value::int64 (2) }, {})
             == "set MIXER:Current/InCh/Thing 0 0 1 \"a\" 2");

    //  An atom with no spelling - nil, an impulse, a blob - is left out.
    CHECK (renderRcp ("/MIXER:Current/InCh/Thing/1/1", { osc::Value::nil(), osc::Value::int32 (4) }, {})
             == "set MIXER:Current/InCh/Thing 0 0 4");
}

TEST_CASE ("wire: an RCP verb that is not a set has no X and Y - a scene recalled by number, or by list and name")
{
    CHECK (renderRcp ("/MIXER:Lib/Scene", { osc::Value::int32 (12) }, { "ssrecall_ex", 0 })
             == "ssrecall_ex MIXER:Lib/Scene 12");
    CHECK (renderRcp ("/scene_a", { osc::Value::string ("4.00") }, { "ssrecallt_ex", 0 })
             == "ssrecallt_ex scene_a \"4.00\"");

    //  And a get carries its indexes and no value.
    CHECK (renderRcp ("/MIXER:Current/InCh/Fader/Level/3/1", {}, { "get", -1 })
             == "get MIXER:Current/InCh/Fader/Level 2 0");
}

TEST_CASE ("wire: a console's lines taken apart - OK and OKm echo the request, ERROR says why, NOTIFY reports a move")
{
    const auto ok = parseRcpLine ("OK set MIXER:Current/InCh/Fader/Level 0 0 -32768\n");
    REQUIRE (ok.has_value());
    CHECK (ok->word == "OK");
    CHECK (ok->verb == "set");
    CHECK (ok->parameter == "MIXER:Current/InCh/Fader/Level");
    CHECK (ok->indexed);
    CHECK (ok->x == 0);
    CHECK (ok->y == 0);
    REQUIRE (ok->values.size() == 1u);
    CHECK (ok->values[0].isInt32());
    CHECK (ok->values[0].getInt32() == -32768);
    CHECK (ok->text == "OK set MIXER:Current/InCh/Fader/Level 0 0 -32768");

    const auto notify = parseRcpLine ("NOTIFY set MIXER:Current/InCh/Label/Name 11 0 \"Vox \\\"lead\\\"\"");
    REQUIRE (notify.has_value());
    CHECK (notify->word == "NOTIFY");
    CHECK (notify->x == 11);
    CHECK (notify->y == 0);
    REQUIRE (notify->values.size() == 1u);
    CHECK (notify->values[0].isString());
    CHECK (notify->values[0].getString() == "Vox \"lead\"");

    //  A quoted number stays a string; a bare one is an integer.
    const auto quotedNumber = parseRcpLine ("NOTIFY set MIXER:Current/Thing 0 0 \"12\"");
    REQUIRE (quotedNumber.has_value());
    REQUIRE (quotedNumber->values.size() == 1u);
    CHECK (quotedNumber->values[0].isString());

    const auto error = parseRcpLine ("ERROR set InvalidArgument");
    REQUIRE (error.has_value());
    CHECK (error->word == "ERROR");
    CHECK (error->verb == "set");
    CHECK (error->parameter == "InvalidArgument");
    CHECK_FALSE (error->indexed);

    const auto okm = parseRcpLine ("OKm set MIXER:Current/InCh/Fader/Level 0 0 1000");
    REQUIRE (okm.has_value());
    CHECK (okm->word == "OKm");
    CHECK (okm->values.size() == 1u);

    //  A line the grammar does not name is kept whole, its first word the word.
    const auto other = parseRcpLine ("devinfo productname DM7");
    REQUIRE (other.has_value());
    CHECK (other->word == "devinfo");
    CHECK (other->values.size() == 2u);

    CHECK_FALSE (parseRcpLine ("").has_value());
    CHECK_FALSE (parseRcpLine ("   \r\n").has_value());
}

TEST_CASE ("wire: the addresses a reported parameter might be a node at, most specific first")
{
    const auto notify = parseRcpLine ("NOTIFY set MIXER:Current/InCh/ToMix/Level 11 2 -600");
    REQUIRE (notify.has_value());
    CHECK (rcpAddressesOf (*notify) == std::vector<std::string> {
        "/MIXER:Current/InCh/ToMix/Level/12/3", "/MIXER:Current/InCh/ToMix/Level/12", "/MIXER:Current/InCh/ToMix/Level" });

    const auto scene = parseRcpLine ("NOTIFY sscurrent_ex MIXER:Lib/Scene 12");
    REQUIRE (scene.has_value());
    CHECK (rcpAddressesOf (*scene) == std::vector<std::string> { "/MIXER:Lib/Scene" });

    CHECK (rcpAddressesOf (RcpLine {}).empty());
}

//==============================================================================
/*  DP.8: THE LINE WIRE - a console's own command line from the node's template. */

TEST_CASE ("wire: a command line from the node's template - the page and executor from the address, the atoms by number")
{
    CHECK (renderLine ("/exec/1/2/go", {}, "Go+ Executor {x}.{y}") == "Go+ Executor 1.2");
    CHECK (renderLine ("/exec/3/14/fader", { osc::Value::int32 (50) }, "Fader {x}.{y} At {1}") == "Fader 3.14 At 50");
    CHECK (renderLine ("/cue/12/goto", {}, "Goto Cue {x}") == "Goto Cue 12");
    CHECK (renderLine ("/exec/1/1/cue", { osc::Value::int32 (5) }, "Go+ Executor {x}.{y} Cue {1}") == "Go+ Executor 1.1 Cue 5");

    //  A float with no fraction is a whole number; with one, as it is, trimmed.
    CHECK (renderLine ("/exec/1/1/fader", { osc::Value::float32 (50.0f) }, "Fader {x}.{y} At {1}") == "Fader 1.1 At 50");
    CHECK (renderLine ("/exec/1/1/fader", { osc::Value::float32 (12.5f) }, "Fader {x}.{y} At {1}") == "Fader 1.1 At 12.5");

    //  Several atoms, a bool, a string as it is; a placeholder with nothing for it is empty.
    CHECK (renderLine ("/thing", { osc::Value::string ("Main"), osc::Value::boolean (true), osc::Value::int32 (7) },
                       "Call {1} {2} then {3} and {4}.") == "Call Main 1 then 7 and .");
    CHECK (renderLine ("/cmd", { osc::Value::int32 (1) }, "Page {x}") == "Page ");

    //  Braces that are not a placeholder stay.
    CHECK (renderLine ("/exec/1/1/go", {}, "Go {q} {x}") == "Go {q} 1");
}

TEST_CASE ("wire: with no template the atoms are the line, and a console's chatter is kept printable")
{
    CHECK (renderLine ("/cmd", { osc::Value::string ("Goto Cue 12") }, "") == "Goto Cue 12");
    CHECK (renderLine ("/cmd", { osc::Value::string ("Fader"), osc::Value::int32 (1), osc::Value::float32 (0.5f) }, "")
             == "Fader 1 0.5");
    CHECK (renderLine ("/cmd", {}, "").empty());

    CHECK (printableLine ("\xFF\xFD\x18\xFF\xFD\x1FWelcome to grandMA2\r") == "Welcome to grandMA2");
    CHECK (printableLine ("admin@grandMA2>\tGo+ Executor 1.1") == "admin@grandMA2>\tGo+ Executor 1.1");
    CHECK (printableLine ("\x1B[2J").empty());
    CHECK (printableLine ("\x1B[32madmin@grandMA2>\x1B[0m ready") == "admin@grandMA2> ready");
}

//==============================================================================
/*  DP.9: THE MIDI WIRE - a node's shape rendered to the console's own bytes,
    held against the examples the consoles' documents print. */

namespace
{
    using Bytes = std::vector<std::uint8_t>;

    tree::MidiShape shapeOf (const char* kind)
    {
        tree::MidiShape shape;
        shape.kind = kind;
        return shape;
    }
}

TEST_CASE ("wire: a mute as Allen & Heath's dLive takes it - a Note On at 7F or 3F then its release, on the base channel plus the block's offset")
{
    auto mute = shapeOf ("note");
    mute.note = 0;
    mute.on = 0x7F;
    mute.off = 0x3F;
    mute.release = true;

    //  Input 1 on base channel 12: the document's own example, 9B 00 7F.
    CHECK (renderMidi (mute, { osc::Value::boolean (true) }, 12, 127, 127)
             == std::vector<Bytes> { { 0x9B, 0x00, 0x7F }, { 0x9B, 0x00, 0x00 } });
    CHECK (renderMidi (mute, { osc::Value::boolean (false) }, 12, 127, 127)
             == std::vector<Bytes> { { 0x9B, 0x00, 0x3F }, { 0x9B, 0x00, 0x00 } });

    //  Aux 3 is on N+2, note 02.
    mute.note = 2;
    mute.offset = 2;
    CHECK (renderMidi (mute, { osc::Value::int32 (1) }, 12, 127, 127)
             == std::vector<Bytes> { { 0x9D, 0x02, 0x7F }, { 0x9D, 0x02, 0x00 } });

    //  No atom is a press; no release where the shape says so; a channel never past 16.
    mute.release = false;
    CHECK (renderMidi (mute, {}, 15, 127, 127) == std::vector<Bytes> { { 0x9F, 0x02, 0x7F } });
}

TEST_CASE ("wire: a fader as the dLive and the Qu take it - NRPN 17 with the level, the Qu's data LSB after")
{
    auto fader = shapeOf ("nrpn");
    fader.msb = 0;
    fader.lsb = 0x17;
    fader.bits = 7;
    CHECK (renderMidi (fader, { osc::Value::int32 (0x62) }, 12, 127, 127)
             == std::vector<Bytes> { { 0xBB, 0x63, 0x00 }, { 0xBB, 0x62, 0x17 }, { 0xBB, 0x06, 0x62 } });

    //  The Qu's input 1 is note 20 and wants 07 as the data LSB; a float is rounded.
    fader.msb = 0x20;
    fader.fine = 0x07;
    CHECK (renderMidi (fader, { osc::Value::float32 (98.4f) }, 1, 127, 127)
             == std::vector<Bytes> { { 0xB0, 0x63, 0x20 }, { 0xB0, 0x62, 0x17 }, { 0xB0, 0x06, 0x62 }, { 0xB0, 0x26, 0x07 } });

    //  A DCA assignment is two codes on a switch: Qu's DCA 2 on is 41, off 01.
    auto assign = shapeOf ("nrpn");
    assign.msb = 0x20;
    assign.lsb = 0x40;
    assign.fine = 0x07;
    assign.hasOnOff = true;
    assign.on = 0x41;
    assign.off = 0x01;
    CHECK (renderMidi (assign, { osc::Value::boolean (true) }, 1, 127, 127)
             == std::vector<Bytes> { { 0xB0, 0x63, 0x20 }, { 0xB0, 0x62, 0x40 }, { 0xB0, 0x06, 0x41 }, { 0xB0, 0x26, 0x07 } });
    CHECK (renderMidi (assign, { osc::Value::boolean (false) }, 1, 127, 127)[2] == Bytes { 0xB0, 0x06, 0x01 });
}

TEST_CASE ("wire: the SQ's 14-bit NRPNs - a level coarse and fine, a mute as 00 01, from the document's examples")
{
    //  Ip1 to LR at 0 dB on the linear taper: B0 63 40 B0 62 00 B0 06 76 B0 26 5C.
    auto level = shapeOf ("nrpn");
    level.msb = 0x40;
    level.lsb = 0x00;
    level.bits = 14;
    CHECK (renderMidi (level, { osc::Value::int32 ((0x76 << 7) | 0x5C) }, 1, 127, 127)
             == std::vector<Bytes> { { 0xB0, 0x63, 0x40 }, { 0xB0, 0x62, 0x00 }, { 0xB0, 0x06, 0x76 }, { 0xB0, 0x26, 0x5C } });

    //  Ip40 to Aux5 at -12 dB on channel 4: B3 63 44 B3 62 1C B3 06 6B B3 26 06.
    level.msb = 0x44;
    level.lsb = 0x1C;
    CHECK (renderMidi (level, { osc::Value::int32 ((0x6B << 7) | 0x06) }, 4, 127, 127)
             == std::vector<Bytes> { { 0xB3, 0x63, 0x44 }, { 0xB3, 0x62, 0x1C }, { 0xB3, 0x06, 0x6B }, { 0xB3, 0x26, 0x06 } });

    //  Ip1 mute on: B0 63 00 B0 62 00 B0 06 00 B0 26 01; Mute Grp 4 on channel 7: B6 63 04 B6 62 03 ...
    auto mute = shapeOf ("nrpn");
    mute.bits = 14;
    mute.hasOnOff = true;
    mute.on = 1;
    mute.off = 0;
    CHECK (renderMidi (mute, { osc::Value::boolean (true) }, 1, 127, 127)
             == std::vector<Bytes> { { 0xB0, 0x63, 0x00 }, { 0xB0, 0x62, 0x00 }, { 0xB0, 0x06, 0x00 }, { 0xB0, 0x26, 0x01 } });
    mute.msb = 0x04;
    mute.lsb = 0x03;
    CHECK (renderMidi (mute, { osc::Value::boolean (true) }, 7, 127, 127)[0] == Bytes { 0xB6, 0x63, 0x04 });
    CHECK (renderMidi (mute, { osc::Value::boolean (false) }, 7, 127, 127)[3] == Bytes { 0xB6, 0x26, 0x00 });
}

TEST_CASE ("wire: a scene as a Program Change - banked for each 128 from where the preset counts, the node's own number or the atom's, a fixed channel")
{
    //  The SQ's examples: scene 7 on channel 1 is B0 00 00 C0 06; scene 156 is B0 00 01 C0 1B; on channel 3, B2 and C2.
    auto scene = shapeOf ("pc");
    scene.banked = true;
    scene.start = 1;
    CHECK (renderMidi (scene, { osc::Value::int32 (7) }, 1, 127, 127) == std::vector<Bytes> { { 0xB0, 0x00, 0x00 }, { 0xC0, 0x06 } });
    CHECK (renderMidi (scene, { osc::Value::int32 (156) }, 1, 127, 127) == std::vector<Bytes> { { 0xB0, 0x00, 0x01 }, { 0xC0, 0x1B } });
    CHECK (renderMidi (scene, { osc::Value::int32 (156) }, 3, 127, 127) == std::vector<Bytes> { { 0xB2, 0x00, 0x01 }, { 0xC2, 0x1B } });

    //  The X32's scene 5: program 5 itself, from nought, on channel 1 whatever the mount says.
    auto x32 = shapeOf ("pc");
    x32.program = 5;
    x32.start = 0;
    x32.channel = 1;
    CHECK (renderMidi (x32, {}, 12, 127, 127) == std::vector<Bytes> { { 0xC0, 0x05 } });

    //  A Yamaha recall by atom, from one: scene 12 is program 11; below the start is clamped.
    auto yamaha = shapeOf ("pc");
    yamaha.start = 1;
    CHECK (renderMidi (yamaha, { osc::Value::int32 (12) }, 1, 127, 127) == std::vector<Bytes> { { 0xC0, 0x0B } });
    CHECK (renderMidi (yamaha, { osc::Value::int32 (0) }, 1, 127, 127) == std::vector<Bytes> { { 0xC0, 0x00 } });

    //  And a Control Change with the atom.
    auto cc = shapeOf ("cc");
    cc.cc = 7;
    CHECK (renderMidi (cc, { osc::Value::int32 (100) }, 1, 127, 127) == std::vector<Bytes> { { 0xB0, 0x07, 0x64 } });
}

TEST_CASE ("wire: a System Exclusive from the shape's bytes - hex, the channel tokens, the value, a string - the dLive's send level and name")
{
    //  SysEx Header, 0N, 0E, CH, SndN, SndCH, V, F7: input 1 to aux 3 on base channel 12 at 100.
    auto send = shapeOf ("sysex");
    send.bytes = { "F0", "00", "00", "1A", "50", "10", "01", "00", "N", "0E", "00", "N+2", "02", "V", "F7" };
    CHECK (renderMidi (send, { osc::Value::int32 (100) }, 12, 127, 127)
             == std::vector<Bytes> { { 0xF0, 0x00, 0x00, 0x1A, 0x50, 0x10, 0x01, 0x00, 0x0B, 0x0E, 0x00, 0x0D, 0x02, 0x64, 0xF7 } });

    //  SysEx Header, 0N, 03, CH, Name, F7.
    auto name = shapeOf ("sysex");
    name.bytes = { "F0", "00", "00", "1A", "50", "10", "01", "00", "N", "03", "05", "S", "F7" };
    CHECK (renderMidi (name, { osc::Value::string ("Vox") }, 1, 127, 127)
             == std::vector<Bytes> { { 0xF0, 0x00, 0x00, 0x1A, 0x50, 0x10, 0x01, 0x00, 0x00, 0x03, 0x05, 0x56, 0x6F, 0x78, 0xF7 } });

    //  A 14-bit value as two bytes; a token that is nothing is skipped; no bytes, no message.
    auto wide = shapeOf ("sysex");
    wide.bytes = { "F0", "V14", "??", "F7" };
    CHECK (renderMidi (wide, { osc::Value::int32 (0x1234) }, 1, 127, 127) == std::vector<Bytes> { { 0xF0, 0x24, 0x34, 0xF7 } });
    CHECK (renderMidi (shapeOf ("sysex"), {}, 1, 127, 127).empty());
    CHECK (renderMidi (shapeOf (""), { osc::Value::int32 (1) }, 1, 127, 127).empty());
}

TEST_CASE ("wire: MIDI Show Control - the frame, the cue number as text with its list and path, Timed Go, Set, Fire and the bare commands")
{
    auto go = shapeOf ("msc");
    go.command = 0x01;
    CHECK (renderMidi (go, { osc::Value::string ("12.5"), osc::Value::string ("1") }, 1, 127, 127)
             == std::vector<Bytes> { { 0xF0, 0x7F, 0x7F, 0x02, 0x7F, 0x01, '1', '2', '.', '5', 0x00, '1', 0xF7 } });
    CHECK (renderMidi (go, { osc::Value::int32 (3) }, 1, 5, 1)
             == std::vector<Bytes> { { 0xF0, 0x7F, 0x05, 0x02, 0x01, 0x01, '3', 0xF7 } });

    //  Stop with no cue stops everything; letters in a cue number are not sent.
    auto stop = shapeOf ("msc");
    stop.command = 0x02;
    CHECK (renderMidi (stop, {}, 1, 127, 127) == std::vector<Bytes> { { 0xF0, 0x7F, 0x7F, 0x02, 0x7F, 0x02, 0xF7 } });
    CHECK (renderMidi (go, { osc::Value::string ("Q12a") }, 1, 127, 127)
             == std::vector<Bytes> { { 0xF0, 0x7F, 0x7F, 0x02, 0x7F, 0x01, '1', '2', 0xF7 } });

    auto timed = shapeOf ("msc");
    timed.command = 0x04;
    CHECK (renderMidi (timed, { osc::Value::int32 (0), osc::Value::int32 (1), osc::Value::int32 (30), osc::Value::int32 (0),
                               osc::Value::int32 (0), osc::Value::string ("7") }, 1, 127, 127)
             == std::vector<Bytes> { { 0xF0, 0x7F, 0x7F, 0x02, 0x7F, 0x04, 0x00, 0x01, 0x1E, 0x00, 0x00, '7', 0xF7 } });

    auto set = shapeOf ("msc");
    set.command = 0x06;
    CHECK (renderMidi (set, { osc::Value::int32 (300), osc::Value::int32 (5) }, 1, 127, 127)
             == std::vector<Bytes> { { 0xF0, 0x7F, 0x7F, 0x02, 0x7F, 0x06, 0x2C, 0x02, 0x05, 0x00, 0xF7 } });

    auto fire = shapeOf ("msc");
    fire.command = 0x07;
    CHECK (renderMidi (fire, { osc::Value::int32 (3) }, 1, 127, 127) == std::vector<Bytes> { { 0xF0, 0x7F, 0x7F, 0x02, 0x7F, 0x07, 0x03, 0xF7 } });

    auto allOff = shapeOf ("msc");
    allOff.command = 0x08;
    CHECK (renderMidi (allOff, { osc::Value::string ("ignored") }, 1, 127, 127) == std::vector<Bytes> { { 0xF0, 0x7F, 0x7F, 0x02, 0x7F, 0x08, 0xF7 } });
}
