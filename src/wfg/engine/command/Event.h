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
    An event is a command invocation on its way to the tick thread.

    Everything that mutates engine state - a node write, a standby move, a
    document load - is one of these, submitted from whatever thread received it
    and applied, in arrival order, by the tick thread (PRD 3.15: "all engine
    state mutation flows through one ordered, tick-indexed path"). There is no
    second path.

    The origin travels ON the event rather than in a thread-local, because the
    event changes threads between submission and application; echo suppression
    (PRD 3.16) reads it when the resulting change is pushed back out. WFS-DIY's
    OSCQuery server learned the same thing the hard way and captures its origin
    at queue time for exactly this reason.

    Origin spellings, fixed so that logs are greppable:

        ws:<ip>:<port>     an OSCQuery WebSocket client (the connection id)
        udp:<ip>:<port>    an OSC/UDP sender
        cli                the command line
        replay             the replay tool
        engine             the engine itself (startup, internal)
*/

#include <wfg/engine/osc/OscValue.h>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace wfg
{
    namespace origin
    {
        inline constexpr const char* cli    = "cli";
        inline constexpr const char* replay = "replay";
        inline constexpr const char* engine = "engine";
    }

    struct Event
    {
        std::string origin;
        std::string command;
        std::vector<osc::Value> args;
    };

    /*  Something that arrived and never became an event: a malformed datagram,
        an oversized frame, a JSON message that was not a command. It rides the
        same queue so that its log record lands in the right place among the
        events it arrived between, and it is never replayed - replaying a packet
        that was rejected by the parser would only re-test the parser. */
    struct Drop
    {
        std::string origin;
        std::string reason;
        std::vector<std::uint8_t> payload;   // what arrived, for the post-mortem
    };

    using Entry = std::variant<Event, Drop>;
}
