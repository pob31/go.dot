// This file is part of Go.dot — https://github.com/pob31/go.dot
//
// Copyright (C) 2026 Pierre-Olivier Boulant
//
// Go.dot is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. Go.dot is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
// (LICENSE, at the repository root) for more details.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/*  THE WIRES A MESSAGE IS RENDERED ON (namespace draft §57, AFJ; DP.7).

    A cue holds an address and atoms, whatever the device at the far end
    reads; `MountSender` encodes OSC and delivers the bytes. A device whose
    wire is not OSC gets the same address and atoms rendered another way, at
    the same place, and these are the renderers: pure functions of the
    message and the node's own spelling, bytes out, nothing touched. The
    replies a console sends back are parsed here too, so the serve loop that
    reads a link's lines has one function to call and a test has one to hold.

    RCP (DP.7) is Yamaha's remote control protocol: a line of text per
    request, newline-ended, `<verb> <parameter> <X> <Y> <value>` for a set -
    `set MIXER:Current/InCh/Fader/Level 0 0 -32768` - with X and Y the
    console's own indexes from nought, Y nought where the parameter has none;
    an integer or a quoted string as the value. The console answers `OK` with
    the request echoed, `OKm` the same when the value was modified to fit,
    `ERROR` with the verb and a word, and says `NOTIFY set ...` on its own
    whenever a parameter moves - the free feedback of AFL (DP.10).

    THE PRESET SPELLS X AND Y AS THE LAST ADDRESS SEGMENTS, from one
    (`/MIXER:Current/InCh/Fader/Level/<x>/<y>`), because the mount table and
    the heard box key on the address and a channel carried as an atom would
    make every channel one node (AFH); the node's `GODOT.RCP` says the verb
    and how many trailing segments are indexes, and where it says nothing the
    verb is `set` and the indexes are the trailing segments that are whole
    numbers, two at most. */

#include <wfg/engine/osc/OscValue.h>

#include <optional>
#include <string>
#include <vector>

namespace wfg::tree::wire
{
    /*  What a node says about its RCP spelling (`GODOT.RCP`): the verb, and
        how many of the address's trailing segments are the console's X and Y
        - -1 to infer them from the address. */
    struct RcpSpec
    {
        std::string verb = "set";
        int indexes = -1;
    };

    /*  The line a message is sent as, without its newline: the parameter is
        the address between the root's slash and the index segments, X and Y
        each one less than the segment (the console counts from nought), 0
        where the address has none, and every atom after them - an integer as
        it is, a float rounded (RCP takes integers; a curve's move arrives as
        a float), a bool as 1 or 0, a string quoted with `"` and `\\` escaped.
        A verb other than `set` and `get` has no X and Y: `ssrecall_ex
        MIXER:Lib/Scene 12`. */
    std::string renderRcp (const std::string& address, const osc::Values& values, const RcpSpec& spec);

    /*  One line the console said, taken apart. `word` is its first token -
        OK, OKm, ERROR, NOTIFY - `verb` the next where the word carries one,
        `parameter` the console's address, `x` and `y` its indexes as it said
        them, `values` what followed (integers as int32, quoted text as
        strings, anything else as a string), `text` the whole line. An empty
        line is nothing. */
    struct RcpLine
    {
        std::string word;
        std::string verb;
        std::string parameter;
        int x = 0;
        int y = 0;
        bool indexed = false;
        osc::Values values;
        std::string text;
    };

    std::optional<RcpLine> parseRcpLine (const std::string& line);

    /*  The addresses a reported parameter might be a node at, most specific
        first: with both indexes as segments from one, with X alone, and
        bare. The caller keeps the first the mount has, since the console
        always says X and Y and only the preset knows which it spelled. */
    std::vector<std::string> rcpAddressesOf (const RcpLine& line);

    /*  THE LINE WIRE (DP.8): a console's own command line, one line per
        message - a grandMA2's telnet remote. The node's `GODOT.LINE` is a
        template: `{x}` and `{y}` are the address's first two whole-number
        segments (the page and the executor of `/exec/1/2/go`), `{1}` to
        `{9}` the message's atoms - an integer as it is, a float with its
        fraction only where it has one, a bool as 1 or 0, a string as it is.
        With no template the atoms are the line, joined by spaces: `/cmd s`
        carries a command line whole. What a placeholder has nothing for is
        left empty. */
    std::string renderLine (const std::string& address, const osc::Values& values, const std::string& templateText);

    /*  A line as a console said it, fit to be logged: printable ASCII and
        tabs, so the telnet negotiation bytes a server opens with and any
        control character are dropped rather than written into the show's
        record. */
    std::string printableLine (const std::string& line);
}
