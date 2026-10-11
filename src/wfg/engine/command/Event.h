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
        window             the compiled client, in the same process (namespace
                           draft 14.16) - what tells a click from a datagram
        main:<origin>      on a backup following its main (namespace draft
                           §58, RD.6): one of the main's records, applied
                           here, under the origin the main logged it with
        peer:<origin>      on a main: a command its backup's client gave the
                           backup, passed on (AFT) - so a backup's own client
                           reads `main:peer:<origin>` when it comes back
*/

#include <wfg/engine/osc/OscValue.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace wfg
{
    namespace origin
    {
        inline constexpr const char* cli    = "cli";
        inline constexpr const char* replay = "replay";
        inline constexpr const char* engine = "engine";

        /*  Neither `udp:` nor `ws:` in front of it, on purpose: EngineNamespace
            and OscQueryServer route echo suppression on those two prefixes,
            and a window reads the snapshot rather than a push. */
        inline constexpr const char* window = "window";

        /*  THE TWO PREFIXES A PAIR OF MACHINES ADDS (namespace draft §58, RD.6;
            AGD): a record a following backup applies from its main's stream,
            and a command a main applies that its backup passed on. */
        inline constexpr std::string_view fromMain = "main:";
        inline constexpr std::string_view fromPeer = "peer:";

        /*  THE CLIENT ON THIS MACHINE A RECORD CAME FROM, WHEN IT CAME THE
            LONG WAY ROUND. A command a backup's client gave is passed on to
            the main, applied there as `peer:<origin>`, and comes back down the
            stream to be applied here as `main:peer:<origin>` - and it is still
            that client's own: the push withheld from the client that caused a
            change (echo suppression) and a touch held by a hand on a fader are
            keyed on the client, and a backup's client dragging a fader would
            otherwise be sent its own values back a link's delay late, its
            fader pulled backwards under its finger. Anything else is itself. */
        inline std::string_view localClientOf (std::string_view origin) noexcept
        {
            if (origin.size() > fromMain.size() + fromPeer.size()
                  && origin.substr (0, fromMain.size()) == fromMain
                  && origin.substr (fromMain.size(), fromPeer.size()) == fromPeer)
                return origin.substr (fromMain.size() + fromPeer.size());

            return origin;
        }

        /*  THE ORIGIN AS THE MACHINE IT AROSE ON SPELLED IT: `main:` and then
            `peer:` taken off, so a record the main applied for a device or a
            client reads alike on both machines - where a processor sent from
            (AuthoringCommands' `originHost`) must be the same host on both. */
        inline std::string_view firstOrigin (std::string_view origin) noexcept
        {
            if (origin.size() > fromMain.size() && origin.substr (0, fromMain.size()) == fromMain)
                origin.remove_prefix (fromMain.size());

            if (origin.size() > fromPeer.size() && origin.substr (0, fromPeer.size()) == fromPeer)
                origin.remove_prefix (fromPeer.size());

            return origin;
        }
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
