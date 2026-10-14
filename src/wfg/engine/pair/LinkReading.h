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
            main; ON A MAIN, a backup that said hello pinged within it (AFV) -
            as the stream and the receiver read it. Serve publishes the
            presence's instead from RD.7 (pair/Presence.h): the other machine's
            engine heard, whichever of the two leads. */
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
        /*  `alone`, `leading`, `following`, `waiting` or `bothSending`: the
            seat the log last said (RD.7, pair/Leadership.h). */
        std::string state = "alone";

        /*  THE SENTENCE IN FRONT OF THE OPERATOR (RD.7): both machines
            sending, waiting for the main, the other machine lost and this one
            sending in its place, a follower not yet in step. Empty otherwise. */
        std::string alarm;

        /*  Commands this machine passed on that its leader never answered, and
            that were dropped: at a takeover, all but Esc and double Esc (AGJ). */
        std::int64_t unconfirmed = 0;

        /*  Δ, IN TICKS (AGF): how far behind its main a following backup
            applies the main's records, as measured and fixed at link-up;
            nought before it is fixed, and on a machine that does not follow.
            The extra launch lead a main adds while linked is this (RD.9). */
        int delay = 0;

        /*  The main's records applied after their slot, this link: each one
            arrived too late for the tick Δ gave it and went in at once. */
        std::int64_t late = 0;

        /*  WHAT IS NOT IN STEP WITH THE LEADER, in one sentence, the most
            recent cause (RD.6, RD.8; pair/Summary.h): what its summaries showed
            different, or a record of its applied otherwise here; empty once a
            summary matches. */
        std::string differs;

        /*  WHETHER THE LEADER'S LAST SUMMARY MATCHED what this machine had at
            the same moment (RD.8); false on a machine that does not follow. */
        bool inStep = false;

        /*  Commands this backup's clients gave it that it passed on to the
            main (AFT), and commands its devices sent it that it ignored
            because the main's copy arrives in the stream (AGD). Counted since
            serve started, every link alike. */
        std::int64_t relayed = 0;
        std::int64_t dropped = 0;

        /*  WHETHER ITS SHOW IS ITS LEADER'S (RD.10, AFU; pair/ShowCheck.h):
            the fingerprints agree and the media is the same; false until the
            first comparison after a follow begins, while anything differs, and
            on a machine that does not follow. And what differs, in one
            sentence - the show's cues and settings, its sounds missing or
            changed - empty when nothing does or nothing is known yet. */
        bool showMatch = false;
        std::string showDiffers = {};

        /*  THE SAME SAMPLE (RD.9, AFS; pair/ClockMap.h): whether this machine,
            following, places its launches on its leader's samples, the drift
            between the two sound clocks in parts per million (AGI) and how
            well the map is known, in microseconds - nought and false on a
            machine that does not follow; and each side's interface output
            delay as its driver reports it, in seconds, shown and never
            compensated (AGQ). */
        bool mapped = false;
        double drift = 0.0;
        double clockResidual = 0.0;
        double outputLatency = 0.0;
        double peerOutputLatency = 0.0;
    };
}
