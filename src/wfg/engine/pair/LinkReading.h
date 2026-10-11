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
    WHAT THE LINK SAYS ABOUT ITSELF (namespace draft §58.3, RD.5): the five
    rows `pair/linked`, `pair/peer`, `pair/roundTrip`, `pair/received` and
    `pair/gaps`, as one machine reads them on the tick it publishes.

    Its own small header, apart from the link (PeerLink.h), because the
    parameter tree carries one in `EngineState` and is included nearly
    everywhere; a readout of five values should not bring sockets, threads and
    a ring of records with it.

    The same struct on both machines, and the words mean the same thing seen
    from either end: a main reads it off its stream, a backup off what it
    received, and a machine that is neither - or a replay, or a rig - reads it
    as it stands here, unlinked and empty.
*/

#include <cstdint>
#include <string>

namespace wfg::pair
{
    struct LinkReading
    {
        /*  ON A BACKUP, a heartbeat arrived within the timeout on any path to the
            main; ON A MAIN, a backup that said hello pinged within it (AFV). */
        bool linked = false;

        /*  WHO IS AT THE OTHER END: on a backup, the main's address that last
            delivered a record or a heartbeat, as `host:port`; on a main, the
            backup's connection that last pinged, as its origin `ws:<ip>:<port>`.
            Kept after the link is lost, so the readout says who was lost. */
        std::string peer;

        /*  SECONDS THERE AND BACK, the fastest of the last second's pings: a
            backup measures it on its own steady clock; a main reads what the
            backup measured, carried in its ping. Nought while unlinked. */
        double roundTrip = 0.0;

        /*  RECORDS THIS LINK: received on a backup, each once however many
            paths carried it; sent on a main, each once however many
            connections it went down. Counted from the moment the link came up. */
        std::int64_t received = 0;

        /*  RECORDS MISSED THIS LINK, as gaps: on a backup, a counter that
            jumped or a `gap` the main said; on a main, a hello it had to answer
            with one, its ring no longer holding what the backup asked for. */
        std::int64_t gaps = 0;
    };

    /*  WHAT FOLLOWING SAYS ABOUT ITSELF (namespace draft §58.3, RD.6): the rows
        `pair/state`, `pair/delay`, `pair/late`, `pair/differs`, `pair/relayed`
        and `pair/dropped`, as serve reads them on the tick it publishes. Here
        beside the link's, for the link's reason: the tree carries one. */
    struct FollowReading
    {
        /*  `alone`, `following` or `leading` (RD.6); `bothSending` and
            `waiting` are RD.7's. A main linked to its backup leads; a backup
            linked to its main follows; anything else is alone. */
        std::string state = "alone";

        /*  Δ, IN TICKS (AGF): how far behind its main a following backup
            applies the main's records, as measured and fixed at link-up;
            nought before it is fixed, and on a machine that does not follow.
            The extra launch lead a main adds while linked is this (RD.9). */
        int delay = 0;

        /*  The main's records applied after their slot, this link: each one
            arrived too late for the tick Δ gave it and went in at once. */
        std::int64_t late = 0;

        /*  THE FIRST RECORD THE BACKUP APPLIED OTHERWISE THAN ITS MAIN, in one
            sentence with how many have, this link; empty while every one has
            come out as the main's - applied with the same arguments, or
            refused for the same reason. */
        std::string differs;

        /*  Commands this backup's clients gave it that it passed on to the
            main (AFT), and commands its devices sent it that it ignored
            because the main's copy arrives in the stream (AGD). Counted since
            serve started, every link alike. */
        std::int64_t relayed = 0;
        std::int64_t dropped = 0;
    };
}
