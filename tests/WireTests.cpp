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
