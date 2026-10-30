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

#include <wfg/engine/json/JsonValue.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/Node.h>

#include <cstdint>

#include <map>

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

    /*  THE MIDI WIRE (DP.9): a node's shape rendered to the messages it
        takes, in order - a Bank Select before a Program Change, a Note On
        then its release, the NRPN's three or four Control Changes, one
        System Exclusive - from the message's atoms, the mount's base channel
        (the shape's CHANNEL fixing it, its OFFSET added), and MSC's device
        and format. A shape with no kind renders nothing. */
    std::vector<std::vector<std::uint8_t>> renderMidi (const MidiShape& shape, const osc::Values& values,
                                                        int baseChannel, int mscDevice, int mscFormat);

    /*  A SUBSCRIPTION AS THE FILE SPELLS IT (DP.10): the address and its
        atoms - "/eos/subscribe 1" - a whole number an int32, one with a dot a
        float32, anything else a string; nothing for an empty line. */
    std::optional<std::pair<std::string, osc::Values>> parseSubscribe (const std::string& line);

    //==========================================================================
    /*  THE SHURE WIRE (namespace draft §124, BER): a Shure receiver's command
        strings - Axient Digital, ULX-D, SLX-D, QLX-D - on TCP 2202, ASCII,
        each between `<` and `>` and nothing ending a line:

            < GET 1 AUDIO_GAIN >        < SET 1 AUDIO_GAIN 30 >
            < REP 1 AUDIO_GAIN 030 >    < SAMPLE 1 ALL 005 031 102 … >

        The channel comes first, nought meaning every channel; a slot or an
        antenna follows the parameter; a device's own property has no channel.
        THE ADDRESS UNDER THE DEVICE'S PREFIX SPELLS THE STRING IN ITS ORDER -
        `/1/AUDIO_GAIN`, `/1/SLOT_RF_OUTPUT/3`, `/FLASH` - and the prefix is
        never sent, as an MQTT topic's is not (§67, ANA); these functions take
        the address with the prefix already off. */

    /*  What a node says about its spelling (`GODOT.SHURE`): `offset` added to
        a number before it is sent and taken off one heard - the gain's 18,
        so a cue says decibels and the receiver hears 0 to 60 - and `braces`,
        a string sent in braces and heard with the receiver's padding trimmed:
        a channel's name, `{Lead Vox}`; `pad`, the digits a number is sent
        in, zeros before it - METER_RATE's five, `01000`. */
    struct ShureSpec
    {
        int offset = 0;
        bool braces = false;
        int pad = 0;
    };

    /*  The frame a message is sent as, `<` to `>`: SET with the values, or
        GET with none when `asking`. A bool is ON or OFF; a number has the
        offset added and is rounded (the receiver takes whole numbers, and a
        curve's move arrives as a float); a string goes in braces where the
        spec says, as it is otherwise - TOGGLE, RF_MUTE, INC 5. An address
        with no parameter renders nothing. */
    std::string renderShure (const std::string& relative, const osc::Values& values, const ShureSpec& spec, bool asking);

    /*  One frame a receiver said, taken apart: `word` REP or SAMPLE, the
        `channel` when one came first, the `parameter`, then every token
        after it - `braced` where it came in braces - and the whole frame as
        `text`. `< REP ERR >` is the word REP and the parameter ERR. Nothing
        for a frame with no word. */
    struct ShureToken
    {
        std::string text;
        bool braced = false;
    };

    struct ShureFrame
    {
        std::string word;
        std::string channel;
        std::string parameter;
        std::vector<ShureToken> rest;
        std::string text;
    };

    std::optional<ShureFrame> parseShureFrame (const std::string& frame);

    /*  Where a REP might be a node, most specific first, each with the
        tokens left for its value: with the first token after the parameter
        as an index when it is a whole number and more follow
        (`< REP 1 RSSI 1 083 >` is `/1/RSSI/1` at 83), then with none. The
        caller keeps the first the device has, as RCP's (DP.7). */
    std::vector<std::pair<std::string, std::vector<ShureToken>>> shureAddressesOf (const ShureFrame& frame);

    /*  A REP's tokens as the node's values: braced text trimmed of its
        padding; ON and OFF a bool on a node typed T; a whole number with the
        offset taken off, an int32; anything else a string. `typeTags` is the
        node's own. */
    osc::Values shureValuesOf (const std::vector<ShureToken>& tokens, const ShureSpec& spec, const std::string& typeTags);

    /*  A SAMPLE's fields by the file's layouts (`GODOT.SAMPLE` at the root):
        each layout a list of node names under the channel - `RSSI/1` - and the
        one whose length is the count of fields that came is used, since a
        receiver's channel says more in Quadversity or FD-C than plainly. Each
        field paired with `/<channel>/<name>`, a name left empty skipped (a
        field no node keeps); nothing when no layout fits. */
    std::vector<std::pair<std::string, ShureToken>> shureSampleOf (const ShureFrame& frame,
                                                                   const std::vector<std::vector<std::string>>& layouts);

    //==========================================================================
    /*  THE SSC WIRE (namespace draft §124, BES): Sennheiser's Sound Control
        Protocol, version 1 - a Digital 6000, an EW-DX below firmware 4, a
        SpeechLine receiver - one JSON object per UDP datagram, port 45. THE
        ADDRESS IS THE JSON'S PATH: `/rx1/mute` true is

            {"rx1":{"mute":true}}

        a question is the same with null, and the receiver answers every
        message with the same shape holding what the property now is - the
        value it took, which may be one it adapted. The prefix is the
        device's and never sent, as the shure wire's. */

    /*  The message: a value, an array for several, null when `asking` or
        with none. A number in the shortest spelling that reads back, with a
        dot whatever the locale; a bool true or false; a string quoted. An
        address with no segment renders nothing. */
    std::string renderSsc (const std::string& relative, const osc::Values& values, bool asking);

    /*  Every property a message holds, as its path and its value - the
        `osc` part (errors, subscriptions, the protocol's own) left out - in
        path order. A value is a JSON primitive or an array of them. */
    std::vector<std::pair<std::string, json::Value>> sscLeavesOf (const json::Value& message);

    /*  A value as the node takes it: a number an int32 on a node typed `i`
        - or, with no node to say (`typeTags` empty), a whole number - and a
        float32 otherwise, a bool T or F, a string as it is, each
        element of an array one atom - an array inside it flattened in order;
        null is nothing. */
    osc::Values sscValuesOf (const json::Value& value, const std::string& typeTags);

    /*  The error a message carries, `osc.error`, as compact JSON - what the
        device's last reply keeps - or empty where there is none. */
    std::string sscErrorOf (const json::Value& message);

    //==========================================================================
    /*  THE SSCV2 WIRE (namespace draft §124, BET): Sennheiser's version 2 - a
        Spectera, an EW-DX from firmware 4 - REST and JSON over HTTPS. The
        address under the prefix is the resource's path and then the field:
        `/api/mts/paired/all/1234/name` is the field `name` of the resource
        `/api/mts/paired/all/1234`. A write is a PUT of that resource with the
        field alone, `{"name":"Lead"}`; a question a GET of the resource, whose
        answer is heard whole. Rendered as the request table takes it: the
        method and the path, a new line, the body. */
    std::string renderSscV2 (const std::string& relative, const osc::Values& values, bool asking);

    /*  What a resource holds, as addresses under it and values: an object's
        fields by name, nested; a collection - an array the file names an
        identifier for (`ids`, the root's `GODOT.IDS`: `/api/mts/paired/all` is
        `mtUid`) - each element under its identifier's value; anything else a
        leaf. An event is an object of these keyed by path. */
    std::vector<std::pair<std::string, json::Value>> sscV2LeavesOf (const std::string& path, const json::Value& value,
                                                                    const std::map<std::string, std::string>& ids);
}
