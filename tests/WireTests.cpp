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

#include <wfg/engine/json/JsonValue.h>
#include <wfg/engine/tree/Wires.h>
#include <wfg/engine/tree/MountSender.h>

#include <chrono>
#include <memory>
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

//==============================================================================
/*  DP.10: WHAT COMES BACK - a console's echoes on the MIDI wire read by the
    shapes its nodes declared, and a subscription line taken apart. */

#include <wfg/engine/tree/MidiEchoes.h>

namespace
{
    tree::Node echoNode (const char* address, const char* kind, int a, int b = 0, int c = 0, int d = 0)
    {
        auto shape = std::make_shared<tree::NodeShape>();
        auto& midi = shape->midi;
        midi.kind = kind;

        if (std::string (kind) == "note")
        {
            midi.note = a;
            midi.offset = b;
            midi.on = 0x7F;
            midi.off = 0x3F;
            midi.hasOnOff = true;
        }
        else if (std::string (kind) == "nrpn")
        {
            midi.msb = a;
            midi.lsb = b;
            midi.bits = c == 0 ? 7 : c;
            midi.offset = d;
        }
        else if (std::string (kind) == "cc")
        {
            midi.cc = a;
        }

        tree::Node node;
        node.address = address;
        node.shape = std::move (shape);
        return node;
    }
}

TEST_CASE ("echo: a console's mute and fader come back as its nodes' values, through running status and any cut of the stream")
{
    const auto mute = echoNode ("/input/1/mute", "note", 0);
    const auto aux = echoNode ("/aux/3/mute", "note", 2, 2);
    const auto fader = echoNode ("/input/1/fader", "nrpn", 0, 0x17);
    const auto dca = echoNode ("/dca/1/fader", "nrpn", 0x36, 0x17, 7, 4);

    tree::MidiEchoes echoes;
    echoes.learn ("DESK0001", { &mute, &aux, &fader, &dca }, 12);

    //  The document's own strings: 9B 00 7F then its release, which says nothing.
    auto heard = echoes.feed ("DESK0001", { 0x9B, 0x00, 0x7F, 0x9B, 0x00, 0x00 });
    REQUIRE (heard.size() == 1u);
    CHECK (heard[0].address == "/input/1/mute");
    REQUIRE (heard[0].values.size() == 1u);
    CHECK (heard[0].values[0].isBool());
    CHECK (heard[0].values[0].getBool());

    //  Running status: the next note on the same channel without its status byte; 3F is off.
    heard = echoes.feed ("DESK0001", { 0x9B, 0x00, 0x3F, 0x00, 0x00 });
    REQUIRE (heard.size() == 1u);
    CHECK_FALSE (heard[0].values[0].getBool());

    //  Aux 3 is on N+2, note 02; input 3 on N, note 02, is nobody's.
    heard = echoes.feed ("DESK0001", { 0x9D, 0x02, 0x7F, 0x9B, 0x02, 0x7F });
    REQUIRE (heard.size() == 1u);
    CHECK (heard[0].address == "/aux/3/mute");

    //  A fader as three Control Changes, cut anywhere.
    heard = echoes.feed ("DESK0001", { 0xBB, 0x63, 0x00, 0xBB });
    CHECK (heard.empty());
    heard = echoes.feed ("DESK0001", { 0x62, 0x17, 0xBB, 0x06, 0x62 });
    REQUIRE (heard.size() == 1u);
    CHECK (heard[0].address == "/input/1/fader");
    CHECK (heard[0].values[0].getInt32() == 0x62);

    //  The DCA on N+4 at note 36; a System Exclusive in between is skipped whole.
    heard = echoes.feed ("DESK0001", { 0xF0, 0x00, 0x00, 0x1A, 0x50, 0x10, 0x01, 0x00, 0x0B, 0x03, 0x05, 0x56, 0xF7,
                                       0xBF, 0x63, 0x36, 0x62, 0x17, 0x06, 0x50 });
    REQUIRE (heard.size() == 1u);
    CHECK (heard[0].address == "/dca/1/fader");
    CHECK (heard[0].values[0].getInt32() == 0x50);

    //  Forgotten, it hears nothing; a mount never learned hears nothing.
    echoes.forget ("DESK0001");
    CHECK (echoes.feed ("DESK0001", { 0x9B, 0x00, 0x7F }).empty());
    CHECK (echoes.feed ("NOBODY01", { 0x9B, 0x00, 0x7F }).empty());
}

TEST_CASE ("echo: the SQ's 14-bit levels and switches wait for their fine byte; a Control Change is its own node")
{
    auto level = echoNode ("/input/1/lr/level", "nrpn", 0x40, 0x00, 14);
    auto mute = echoNode ("/input/1/mute", "nrpn", 0x00, 0x00, 14);
    {
        //  A shape is shared and never changed once made: a switch is a new one.
        auto switched = std::make_shared<tree::NodeShape> (mute.about());
        switched->midi.hasOnOff = true;
        switched->midi.on = 1;
        switched->midi.off = 0;
        mute.shape = std::move (switched);
    }
    const auto cc = echoNode ("/cc/7", "cc", 7);

    tree::MidiEchoes echoes;
    echoes.learn ("SQ000001", { &level, &mute, &cc }, 1);

    auto heard = echoes.feed ("SQ000001", { 0xB0, 0x63, 0x40, 0xB0, 0x62, 0x00, 0xB0, 0x06, 0x76 });
    CHECK (heard.empty());
    heard = echoes.feed ("SQ000001", { 0xB0, 0x26, 0x5C });
    REQUIRE (heard.size() == 1u);
    CHECK (heard[0].address == "/input/1/lr/level");
    CHECK (heard[0].values[0].getInt32() == ((0x76 << 7) | 0x5C));

    heard = echoes.feed ("SQ000001", { 0xB0, 0x63, 0x00, 0xB0, 0x62, 0x00, 0xB0, 0x06, 0x00, 0xB0, 0x26, 0x01 });
    REQUIRE (heard.size() == 1u);
    CHECK (heard[0].address == "/input/1/mute");
    CHECK (heard[0].values[0].getBool());

    heard = echoes.feed ("SQ000001", { 0xB0, 0x07, 0x64 });
    REQUIRE (heard.size() == 1u);
    CHECK (heard[0].address == "/cc/7");
    CHECK (heard[0].values[0].getInt32() == 100);

    //  A Program Change names no node, and a note nobody declared is dropped.
    CHECK (echoes.feed ("SQ000001", { 0xC0, 0x05, 0x90, 0x40, 0x7F }).empty());
}

TEST_CASE ("wire: a subscription line is an address and its atoms, and nothing when it is not one")
{
    const auto eos = parseSubscribe ("/eos/subscribe 1");
    REQUIRE (eos.has_value());
    CHECK (eos->first == "/eos/subscribe");
    REQUIRE (eos->second.size() == 1u);
    CHECK (eos->second[0].isInt32());
    CHECK (eos->second[0].getInt32() == 1);

    const auto bare = parseSubscribe ("/*s");
    REQUIRE (bare.has_value());
    CHECK (bare->first == "/*s");
    CHECK (bare->second.empty());

    const auto mixed = parseSubscribe ("/sub 0.5 name");
    REQUIRE (mixed.has_value());
    CHECK (mixed->second[0].isFloat32());
    CHECK (mixed->second[1].getString() == "name");

    CHECK_FALSE (parseSubscribe ("").has_value());
    CHECK_FALSE (parseSubscribe ("subscribe 1").has_value());
}

//==============================================================================
/*  THE SHURE WIRE (namespace draft §124, BER): a receiver's command strings,
    against the Axient Digital, ULX-D and SLX-D documents' own examples. The
    address arrives with the device's prefix already off. */
TEST_CASE ("wire: a Shure SET is the channel, the parameter, a slot after it, and the value as the node spells it")
{
    //  The gain in decibels, the receiver's 0 to 60: "To set to 22 dB: < SET x AUDIO_GAIN 40 >".
    CHECK (renderShure ("/1/AUDIO_GAIN", { osc::Value::int32 (22) }, { 18, false, 0 }, false) == "< SET 1 AUDIO_GAIN 40 >");
    CHECK (renderShure ("/2/AUDIO_GAIN", { osc::Value::float32 (-17.6f) }, { 18, false, 0 }, false) == "< SET 2 AUDIO_GAIN 0 >");

    //  A bool ON or OFF; a word as it is.
    CHECK (renderShure ("/1/AUDIO_MUTE", { osc::Value::boolean (true) }, {}, false) == "< SET 1 AUDIO_MUTE ON >");
    CHECK (renderShure ("/1/AUDIO_MUTE", { osc::Value::string ("TOGGLE") }, {}, false) == "< SET 1 AUDIO_MUTE TOGGLE >");
    CHECK (renderShure ("/3/AUDIO_GAIN", { osc::Value::string ("INC 10") }, {}, false) == "< SET 3 AUDIO_GAIN INC 10 >");

    //  A name in braces, any brace in it taken out: "< SET x CHAN_NAME {Lead Vox} >".
    CHECK (renderShure ("/1/CHAN_NAME", { osc::Value::string ("Lead Vox") }, { 0, true, 0 }, false) == "< SET 1 CHAN_NAME {Lead Vox} >");
    CHECK (renderShure ("/1/CHAN_NAME", { osc::Value::string ("a{b}") }, { 0, true, 0 }, false) == "< SET 1 CHAN_NAME {ab} >");

    //  A frequency in kHz as it is; a meter rate in its five digits.
    CHECK (renderShure ("/1/FREQUENCY", { osc::Value::int32 (602125) }, {}, false) == "< SET 1 FREQUENCY 602125 >");
    CHECK (renderShure ("/1/METER_RATE", { osc::Value::int32 (1000) }, { 0, false, 5 }, false) == "< SET 1 METER_RATE 01000 >");

    //  A slot after the parameter: "< SET x SLOT_RF_OUTPUT z RF_MUTE >".
    CHECK (renderShure ("/1/SLOT_RF_OUTPUT/3", { osc::Value::string ("RF_MUTE") }, {}, false) == "< SET 1 SLOT_RF_OUTPUT 3 RF_MUTE >");

    //  A device's own property has no channel: "< SET FLASH ON >".
    CHECK (renderShure ("/FLASH", { osc::Value::boolean (true) }, {}, false) == "< SET FLASH ON >");

    //  A question is GET with no value, whatever the message held.
    CHECK (renderShure ("/1/AUDIO_GAIN", { osc::Value::int32 (3) }, { 18, false, 0 }, true) == "< GET 1 AUDIO_GAIN >");
    CHECK (renderShure ("/MODEL", {}, {}, true) == "< GET MODEL >");

    //  No parameter, nothing.
    CHECK (renderShure ("/1", { osc::Value::int32 (1) }, {}, false).empty());
    CHECK (renderShure ("", {}, {}, false).empty());
}

TEST_CASE ("wire: a Shure receiver's REP taken apart - the channel, the parameter, braces kept whole, ERR")
{
    const auto gain = parseShureFrame ("< REP 1 AUDIO_GAIN 030 >");
    REQUIRE (gain.has_value());
    CHECK (gain->word == "REP");
    CHECK (gain->channel == "1");
    CHECK (gain->parameter == "AUDIO_GAIN");
    REQUIRE (gain->rest.size() == 1u);
    CHECK (gain->rest[0].text == "030");
    CHECK (gain->text == "< REP 1 AUDIO_GAIN 030 >");

    //  A name padded to thirty-one, and a > inside it.
    const auto name = parseShureFrame ("< REP 2 CHAN_NAME {Lead > Vox                } >");
    REQUIRE (name.has_value());
    REQUIRE (name->rest.size() == 1u);
    CHECK (name->rest[0].braced);
    CHECK (name->rest[0].text == "Lead > Vox                ");

    //  A device's own: no channel.
    const auto model = parseShureFrame ("< REP MODEL {AD4Q-A                          } >");
    REQUIRE (model.has_value());
    CHECK (model->channel.empty());
    CHECK (model->parameter == "MODEL");

    //  A command refused.
    const auto error = parseShureFrame ("< REP ERR >");
    REQUIRE (error.has_value());
    CHECK (error->word == "REP");
    CHECK (error->parameter == "ERR");
    CHECK (error->rest.empty());

    CHECK_FALSE (parseShureFrame ("<  >").has_value());
    CHECK_FALSE (parseShureFrame ("").has_value());
}

TEST_CASE ("wire: where a Shure REP is a node - an antenna or a slot after the parameter first - and its values")
{
    //  "< REP x RSSI 1 083 >": the antenna, then the level less 120.
    const auto rssi = parseShureFrame ("< REP 1 RSSI 1 083 >");
    REQUIRE (rssi.has_value());
    const auto places = shureAddressesOf (*rssi);
    REQUIRE (places.size() == 2u);
    CHECK (places[0].first == "/1/RSSI/1");
    CHECK (places[1].first == "/1/RSSI");
    const auto level = shureValuesOf (places[0].second, { 120, false, 0 }, "i");
    REQUIRE (level.size() == 1u);
    CHECK (level[0].getInt32() == -37);

    //  "< REP x SLOT_STATUS 2 LINKED.ACTIVE >".
    const auto slot = parseShureFrame ("< REP 3 SLOT_STATUS 2 LINKED.ACTIVE >");
    REQUIRE (slot.has_value());
    CHECK (shureAddressesOf (*slot)[0].first == "/3/SLOT_STATUS/2");
    CHECK (shureValuesOf (shureAddressesOf (*slot)[0].second, {}, "s")[0].getString() == "LINKED.ACTIVE");

    //  One value: only the plain address. The gain back in decibels.
    const auto gain = parseShureFrame ("< REP 1 AUDIO_GAIN 040 >");
    REQUIRE (gain.has_value());
    REQUIRE (shureAddressesOf (*gain).size() == 1u);
    CHECK (shureValuesOf (gain->rest, { 18, false, 0 }, "i")[0].getInt32() == 22);

    //  ON and OFF on a T node a bool; on an s node the word.
    const auto mute = parseShureFrame ("< REP 1 AUDIO_MUTE ON >");
    REQUIRE (mute.has_value());
    CHECK (shureValuesOf (mute->rest, {}, "T")[0].isBool());
    CHECK (shureValuesOf (mute->rest, {}, "T")[0].getBool());
    CHECK (shureValuesOf (mute->rest, {}, "s")[0].getString() == "ON");

    //  A name trimmed of its padding; a frequency's seven digits a number.
    const auto name = parseShureFrame ("< REP 1 CHAN_NAME {Lead Vox                       } >");
    REQUIRE (name.has_value());
    CHECK (shureValuesOf (name->rest, { 0, true, 0 }, "s")[0].getString() == "Lead Vox");
    const auto frequency = parseShureFrame ("< REP 1 FREQUENCY 0578350 >");
    REQUIRE (frequency.has_value());
    CHECK (shureValuesOf (frequency->rest, {}, "i")[0].getInt32() == 578350);

    //  ERR is nowhere.
    const auto error = parseShureFrame ("< REP ERR >");
    REQUIRE (error.has_value());
    CHECK (shureAddressesOf (*error).size() == 1u);
    CHECK (shureAddressesOf (*error)[0].first == "/ERR");
}

TEST_CASE ("wire: a Shure SAMPLE heard field by field through the layout of its length")
{
    const std::vector<std::string> plain { "CHAN_QUALITY", "AUDIO_LED_BITMAP", "AUDIO_LEVEL_PEAK", "AUDIO_LEVEL_RMS",
                                           "ANTENNA_STATUS", "RSSI_LED_BITMAP/1", "RSSI/1", "RSSI_LED_BITMAP/2", "RSSI/2" };
    auto quad = plain;
    for (const auto* more : { "RSSI_LED_BITMAP/3", "RSSI/3", "RSSI_LED_BITMAP/4", "RSSI/4" })
        quad.push_back (more);
    auto fdc = plain;
    fdc.resize (plain.size() + 5);

    const std::vector<std::vector<std::string>> layouts { plain, quad, fdc };

    //  The document's own: "< SAMPLE 1 ALL 005 031 102 102 BB 31 086 31 065 >".
    const auto standard = parseShureFrame ("< SAMPLE 1 ALL 005 031 102 102 BB 31 086 31 065 >");
    REQUIRE (standard.has_value());
    CHECK (standard->parameter == "ALL");
    const auto fields = shureSampleOf (*standard, layouts);
    REQUIRE (fields.size() == 9u);
    CHECK (fields[0].first == "/1/CHAN_QUALITY");
    CHECK (fields[0].second.text == "005");
    CHECK (fields[4].first == "/1/ANTENNA_STATUS");
    CHECK (fields[4].second.text == "BB");
    CHECK (fields[8].first == "/1/RSSI/2");
    CHECK (fields[8].second.text == "065");

    //  Quadversity: thirteen.
    const auto four = parseShureFrame ("< SAMPLE 2 ALL 005 031 102 102 BBBB 31 083 31 068 31 069 31 072 >");
    REQUIRE (four.has_value());
    const auto quadFields = shureSampleOf (*four, layouts);
    REQUIRE (quadFields.size() == 13u);
    CHECK (quadFields[12].first == "/2/RSSI/4");

    //  FD-C: fourteen, the second frequency's five not kept.
    const auto both = parseShureFrame ("< SAMPLE 1 ALL 005 031 102 102 BB 31 082 31 060 BB 31 082 31 060 >");
    REQUIRE (both.has_value());
    CHECK (shureSampleOf (*both, layouts).size() == 9u);

    //  A count no layout has: nothing.
    const auto odd = parseShureFrame ("< SAMPLE 1 ALL 005 031 >");
    REQUIRE (odd.has_value());
    CHECK (shureSampleOf (*odd, layouts).empty());

    //  ULX-D's three.
    const auto ulxd = parseShureFrame ("< SAMPLE 4 ALL AX 095 120 >");
    REQUIRE (ulxd.has_value());
    const auto three = shureSampleOf (*ulxd, { { "RF_ANTENNA", "RX_RF_LVL", "AUDIO_LVL" } });
    REQUIRE (three.size() == 3u);
    CHECK (three[1].first == "/4/RX_RF_LVL");
    CHECK (shureValuesOf ({ three[1].second }, { 128, false, 0 }, "i")[0].getInt32() == -33);
}

//==============================================================================
/*  THE SSC WIRE (namespace draft §124, BES): Sennheiser's Sound Control
    Protocol, version 1, against the EW-DX and Digital 6000 guides' own
    examples. The address arrives with the device's prefix already off. */
TEST_CASE ("wire: an SSC message is the address as the JSON's path, the value at its end, null when asked")
{
    //  "TX: { "audio": { "out1": { "level_db": 1 }}}".
    CHECK (renderSsc ("/audio/out1/level_db", { osc::Value::int32 (1) }, false) == R"({"audio":{"out1":{"level_db":1}}})");
    CHECK (renderSsc ("/rx1/mute", { osc::Value::boolean (true) }, false) == R"({"rx1":{"mute":true}})");
    CHECK (renderSsc ("/rx1/name", { osc::Value::string ("Lead \"Vox\"") }, false) == R"({"rx1":{"name":"Lead \"Vox\""}})");
    CHECK (renderSsc ("/rx1/frequency", { osc::Value::int32 (520000) }, false) == R"({"rx1":{"frequency":520000}})");

    //  A float with a dot whatever the locale.
    CHECK (renderSsc ("/m/rx1/af", { osc::Value::float32 (-12.5f) }, false) == R"({"m":{"rx1":{"af":-12.5}}})");

    //  Several atoms an array: "{"rx1":{"active_bank_channel":["u2",63]}}".
    CHECK (renderSsc ("/rx1/active_bank_channel", { osc::Value::string ("u2"), osc::Value::int32 (63) }, false)
             == R"({"rx1":{"active_bank_channel":["u2",63]}})");

    //  A question: null, whatever the message held.
    CHECK (renderSsc ("/rx1/mute", { osc::Value::boolean (true) }, true) == R"({"rx1":{"mute":null}})");
    CHECK (renderSsc ("/rx1/mute", {}, false) == R"({"rx1":{"mute":null}})");

    CHECK (renderSsc ("", { osc::Value::int32 (1) }, false).empty());
}

TEST_CASE ("wire: an SSC reply's properties at their paths, the osc part left out, typed by the node")
{
    //  Several properties in one reply, one nested two deep.
    const auto reply = json::parse (R"({"rx1":{"mute":false,"name":"Lead"},"mates":{"tx1":{"battery":{"gauge":70}}}})");
    REQUIRE (reply.ok());
    const auto leaves = sscLeavesOf (*reply.value);
    REQUIRE (leaves.size() == 3u);
    CHECK (leaves[0].first == "/mates/tx1/battery/gauge");
    CHECK (leaves[1].first == "/rx1/mute");
    CHECK (leaves[2].first == "/rx1/name");

    CHECK (sscValuesOf (leaves[0].second, "i")[0].getInt32() == 70);
    CHECK (sscValuesOf (leaves[0].second, "f")[0].getFloat32() == doctest::Approx (70.0));
    CHECK_FALSE (sscValuesOf (leaves[1].second, "T")[0].getBool());
    CHECK (sscValuesOf (leaves[2].second, "s")[0].getString() == "Lead");

    //  An array of words, each an atom: "["Aes256Error","AfPeak"]".
    const auto warnings = json::parse (R"(["Aes256Error","AfPeak"])");
    REQUIRE (warnings.ok());
    const auto words = sscValuesOf (*warnings.value, "s");
    REQUIRE (words.size() == 2u);
    CHECK (words[1].getString() == "AfPeak");

    //  The Digital 6000's meters, two channels of nine, flattened in order.
    const auto meters = json::parse (R"({"mm":[[0,0,0,0,0,0,0,0,0],[83,0,53,0,1,1,128,165,0]]})");
    REQUIRE (meters.ok());
    const auto mm = sscLeavesOf (*meters.value);
    REQUIRE (mm.size() == 1u);
    const auto flat = sscValuesOf (mm[0].second, std::string (18, 'i'));
    REQUIRE (flat.size() == 18u);
    CHECK (flat[9].getInt32() == 83);
    CHECK (flat[16].getInt32() == 165);

    //  Null is nothing; the protocol's own part is never a property.
    const auto mixed = json::parse (R"({"osc":{"error":[{"rx1":{"bogus":[404]}}]},"rx1":{"name":null}})");
    REQUIRE (mixed.ok());
    const auto kept = sscLeavesOf (*mixed.value);
    REQUIRE (kept.size() == 1u);
    CHECK (sscValuesOf (kept[0].second, "s").empty());
    CHECK (sscErrorOf (*mixed.value) == R"([{"rx1":{"bogus":[404]}}])");
    CHECK (sscErrorOf (*reply.value).empty());
}

//==============================================================================
/*  M60 (namespace draft §57.6): the cost of a flush that renders thirty-two
    NRPN faders and thirty-two RCP lines, on the tick thread - the wires'
    rendering is the only new work a flush does since DP.7, and a tick is
    twenty milliseconds. Reported, not asserted against a figure: the Debug
    binary this runs in says what the ceiling is, not what Release costs. */
TEST_CASE ("M60: a flush of thirty-two NRPN faders and thirty-two RCP lines, timed")
{
    tree::MountTable mounts;

    tree::MountDeclaration desk;
    desk.id = "DESK0001";
    desk.prefix = "/input";
    desk.namespaceFile = "namespaces/desk.json";
    desk.transport = "tcp";
    desk.wire = "midi";
    desk.host = "10.0.0.3";
    desk.port = 51325;
    desk.midiChannel = 12;

    std::string inputs = R"JSON({"FULL_PATH": "/input", "CONTENTS": {)JSON";
    for (int n = 1; n <= 32; ++n)
        inputs += (n > 1 ? "," : "") + std::string ("\"") + std::to_string (n) + R"JSON(": {"FULL_PATH": "/input/)JSON" + std::to_string (n)
                  + R"JSON(", "CONTENTS": {"fader": {"FULL_PATH": "/input/)JSON" + std::to_string (n)
                  + R"JSON(/fader", "TYPE": "i", "ACCESS": 3, "VALUE": [0], "GODOT": {"MIDI": {"KIND": "nrpn", "MSB": )JSON"
                  + std::to_string (n - 1) + R"JSON(, "LSB": 23, "BITS": 7}}}}})JSON";
    inputs += "}}";
    REQUIRE (mounts.load (desk, inputs).ok);

    tree::MountDeclaration yamaha;
    yamaha.id = "YAMA0001";
    yamaha.prefix = "/MIXER:Current";
    yamaha.namespaceFile = "namespaces/yamaha.json";
    yamaha.transport = "tcp";
    yamaha.wire = "rcp";
    yamaha.host = "10.0.0.9";
    yamaha.port = 49280;

    std::string levels = R"JSON({"FULL_PATH": "/MIXER:Current", "CONTENTS": {"InCh": {"FULL_PATH": "/MIXER:Current/InCh", "CONTENTS": {"Fader": {"FULL_PATH": "/MIXER:Current/InCh/Fader", "CONTENTS": {"Level": {"FULL_PATH": "/MIXER:Current/InCh/Fader/Level", "CONTENTS": {)JSON";
    for (int n = 1; n <= 32; ++n)
        levels += (n > 1 ? "," : "") + std::string ("\"") + std::to_string (n) + R"JSON(": {"FULL_PATH": "/MIXER:Current/InCh/Fader/Level/)JSON"
                  + std::to_string (n) + R"JSON(", "TYPE": "i", "ACCESS": 3, "VALUE": [0], "GODOT": {"RCP": {"VERB": "set", "XY": 1}}})JSON";
    levels += "}}}}}}}}";
    REQUIRE (mounts.load (yamaha, levels).ok);

    tree::MountSender sender;
    sender.setMounts (&mounts);
    std::size_t delivered = 0;
    sender.setLinkSink ([&delivered] (const std::string&, const std::vector<std::uint8_t>&) { ++delivered; return true; });

    /*  ONE FLUSH OF ALL SIXTY-FOUR is what this measures, so the devices'
        per-tick limit (namespace draft §88, AUT) is set aside: nought is no
        limit, as a rig may write it. */
    desk.perTick = 0;
    yamaha.perTick = 0;

    const auto toDesk = tree::MountSender::destinationFor (desk);
    const auto toYamaha = tree::MountSender::destinationFor (yamaha);

    //  Warm once, then time the flush of sixty-four messages.
    for (int pass = 0; pass < 2; ++pass)
    {
        for (int n = 1; n <= 32; ++n)
        {
            sender.queue ("DESK0001", toDesk, "/input/" + std::to_string (n) + "/fader", osc::Value::int32 (64 + n));
            sender.queue ("YAMA0001", toYamaha, "/MIXER:Current/InCh/Fader/Level/" + std::to_string (n), osc::Value::int32 (-1000 + n));
        }

        delivered = 0;
        const auto started = std::chrono::steady_clock::now();
        sender.flush();
        const auto took = std::chrono::duration_cast<std::chrono::microseconds> (std::chrono::steady_clock::now() - started);

        //  Three messages an NRPN fader, one line a level: 96 + 32.
        CHECK (delivered == 128u);

        if (pass == 1)
        {
            MESSAGE ("M60: a flush of 32 NRPN faders and 32 RCP lines took " << took.count() << " us (Debug; a tick is 20000)");
            CHECK (took < std::chrono::milliseconds (20));
        }
    }
}
