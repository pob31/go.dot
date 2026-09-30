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
    WHAT THE NETWORK MONITOR SAYS ABOUT ONE CAPTURE: the bytes the engine kept
    (monitor/TrafficTap.h) turned into lines a person reads - an OSC message's
    address, the kinds of its arguments and their values; a MIDI message's
    name, channel and numbers.

    DECODED HERE, ON THE WINDOW'S TIME, and with the codec the engine reads the
    same bytes with (osc/OscCodec.h), so the monitor shows what the engine
    understood rather than a second opinion. A packet the codec refuses is
    still a line - its refusal in words, and its address when the address can
    be read - because a malformed message from a desk is exactly what somebody
    opens a monitor to find. A bundle is a line per message inside it.

    std only, like the rest of model/: every rule here is asserted with no
    window.
*/

#include <wfg/engine/monitor/TrafficTap.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wfg::client::model
{
    struct TrafficRow
    {
        std::int64_t wallMicros = 0;
        bool incoming = true;
        bool midi = false;
        std::string road;       ///< "udp", "page" or "midi"
        std::string peer;       ///< the address and port, or the MIDI port's name

        /*  An OSC message's address, or what a MIDI message is ("note on",
            "control change"); its argument kinds ("ff") and its values. */
        std::string address;
        std::string types;
        std::string arguments;

        /*  WHY IT COULD NOT BE READ, in words, when it could not - the codec's
            own sentence, or that the monitor kept only the first part of a
            long packet. Empty for a message read whole. */
        std::string problem;
    };

    /** One capture as lines: one, or one per message of a bundle. */
    std::vector<TrafficRow> describe (const monitor::Capture& capture);

    /** A MIDI message as a name and its numbers, channels counted from one. */
    void describeMidi (const std::uint8_t* bytes, std::size_t size, TrafficRow& row);

    /*  WHAT THE MONITOR SHOWS: each direction, each medium and the web page's
        socket switched on or off, and a text that must appear - ignoring case
        - in the address, the values or the peer. */
    struct TrafficFilter
    {
        bool in = true, out = true, osc = true, midi = true, page = true;
        std::string text;

        bool matches (const TrafficRow& row) const;
        bool operator== (const TrafficFilter&) const = default;
    };

    /*  THE ROWS AS CSV, a header line first, every field quoted where it has
        to be. The time is written by `timeText`, which the window gives in the
        machine's own clock - this file names no time zone. */
    std::string csvOf (const std::vector<TrafficRow>& rows,
                       const std::function<std::string (std::int64_t wallMicros)>& timeText);
}
