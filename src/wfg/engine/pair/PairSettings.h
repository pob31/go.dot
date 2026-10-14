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
    WHICH MACHINE THIS IS, WHERE THE OTHER ONE IS, AND HOW LONG A SILENCE LOSES
    IT (namespace draft §58, RD.4; decisions AGC, AFV, AGK).

    A main and a backup play one show on two computers, and three facts decide
    how they find each other: this machine's role - `main`, `backup`, or `none`
    for a machine that is not paired - the other machine's addresses, and the
    seconds of silence after which the link counts as lost. None of the three
    is a decision about the show. A show folder is copied from one machine to
    the other before a performance, and a role that travelled with it would
    make both machines mains, or both backups, the first time anybody copied
    the folder the wrong way (PRD §4.10: the document holds what someone
    decided about the show, never what the machine is).

    SO THEY LIVE IN GO.DOT'S OWN FOLDER ON EACH COMPUTER, `pairing.xml`, beside
    `last-show.txt` and `audio-defaults.xml`, and follow `--engine-folder` as
    those do. `wfg serve --role=`, `--peer=` and `--link-timeout=` override the
    file for one run without writing it - for a launcher, and for a test, which
    must never touch the developer's real folder. Both machines publish what is
    in force as `pair/role`, `pair/peers` and `pair/timeout`, ordinary read-only
    nodes, so the window, the page and the remote app to come read them as they
    read everything else.

    WRITTEN BY THREE COMMANDS, each this machine's alone (`Command::machineLocal`,
    RD.3): `pair.setRole`, `pair.setPeers` and `pair.setTimeout`. They change the
    settings on the tick thread, where everything that is published changes, and
    ask for the file to be written somewhere else - a disk can take its time,
    and the tick never waits for one - which ends with the engine's own
    `pair.saved` record saying whether it was.

    READ LENIENTLY, WRITTEN ATOMICALLY. A machine must start whatever its file
    says: no file is a machine that is not paired, and a value that cannot be
    read is that value's default, said in a sentence - never a refusal to
    start, which on the night would be a backup that did not come up. Written
    through a temporary file and a rename, as the audio defaults are, so a
    crash mid-write leaves the old file whole. Its number is written and read
    by the same locale-proof pair the event log uses (`osc::formatDouble`,
    `osc::parseDouble`): 1.5 seconds is `1.5` on a French machine too.

    THE LINK READS THEM (RD.5, pair/PeerLink.h): a `main` listens, a `backup`
    dials its peers, and both say whether they are linked within the timeout.
    What each then IS - alone, leading, following, waiting, both sending - is
    pair/Leadership.h's (RD.7): a backup started without its main waits,
    holding (AGK), follows its main once it finds it (RD.6), and takes over
    when the link is lost; `--hold` holds on its own, whatever the seat.
*/

#include <wfg/engine/command/Command.h>
#include <wfg/engine/pair/Leadership.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wfg
{
    class CommandRegistry;
}

namespace wfg::pair
{
    /*  THE PORT A PEER IS DIALLED ON when its address leaves one off: the
        HTTP and WebSocket port every client of a serve already uses (§58.1,
        "the backup dials the main on the port every client uses"). */
    inline constexpr int defaultPeerPort = 5010;

    /*  ONE SECOND OF SILENCE LOSES THE LINK (AFV), and nothing outside a tenth
        of a second to thirty is a setting anybody means: shorter than a few
        heartbeats is a link lost to every hiccup of a switch, longer than half
        a minute is a backup that waits through the first cue of the
        interruption it exists for. */
    inline constexpr double defaultTimeout = 1.0;
    inline constexpr double shortestTimeout = 0.1;
    inline constexpr double longestTimeout = 30.0;

    struct PairSettings
    {
        /** `main`, `backup`, or `none` for a machine that is not paired. */
        std::string role = "none";

        /*  The other machine's addresses, each `host:port`, in the order given:
            two when there are two network paths between them (AFV - the link
            is lost only when every one is silent). Empty: no peer. */
        std::vector<std::string> peers;

        /** Seconds of silence that lose the link (AFV). */
        double timeout = defaultTimeout;
    };

    /** Whether a word is a role: `main`, `backup` or `none`, as spelled. */
    bool isRole (std::string_view word);

    /*  ONE PEER'S ADDRESS, as typed, made `host:port`: a host name or an IPv4
        address, or an IPv6 address in brackets - `[fe80::1]:5010` - and a port
        from 1 to 65535, `defaultPeerPort` when it is left off. Nothing for
        anything else: an empty host, a port that is not a number, a URL, an
        IPv6 address without its brackets (whose last group would read as a
        port). Read character by character, never through the locale. */
    std::optional<std::string> peerAddress (std::string_view text);

    /*  A LIST OF THEM, separated by spaces - or commas, so that one
        `--peer=a,b` reads as two - each made `host:port`, an address given
        twice kept once. Nothing when any one of them is not an address, since
        a link dialled to half the paths somebody wrote is a link with a path
        missing that nobody was told about. Empty text is no peer. */
    std::optional<std::vector<std::string>> peerAddresses (std::string_view text);

    /** The peers as `pair/peers` publishes them and `pairing.xml` holds them: space-separated. */
    std::string peersText (const std::vector<std::string>& peers);

    /** Whether a number of seconds is a timeout this machine takes: finite, and within the range above. */
    bool isTimeout (double seconds);

    //==========================================================================
    /*  WHAT A READ OF `pairing.xml` FOUND: the settings - every value the file
        held that could be read, the default for every other - and, when the
        file was not everything it should have been, one sentence saying what
        was set aside, for serve to say as it starts. No file at all is the
        ordinary case of a machine never paired, and says nothing. */
    struct Loaded
    {
        PairSettings settings;
        std::string problem;
    };

    Loaded load (const std::string& file);

    /** Writes the settings, atomically; empty on success, the reason otherwise. */
    std::string save (const std::string& file, const PairSettings& settings);

    //==========================================================================
    /*  WHAT A SESSION HOLDS OF IT, owned by whoever runs the session - serve,
        and every verb that replays or lists a log - and handed to the commands
        by reference. The tick thread's alone: the commands change it, and
        serve copies what is in force into what it publishes.

        TWO COPIES, AND WHY. `stored` is what `pairing.xml` holds; `inForce` is
        that with this run's command-line overrides on top. A command changes
        its one setting in both, and the file is written from `stored` - so an
        override is never written into the file by a command that changed
        something else: a machine started `--role=backup` whose peers are set
        from the window keeps the role its file had, for the next launch
        without the option. A command that changes the very setting the
        command line overrode wins, in force and in the file: it is the later
        and the more deliberate of the two, made by somebody looking at this
        machine. */
    struct Pairing
    {
        PairSettings inForce;
        PairSettings stored;

        /** Moved by every change, so that serve copies what is in force only on a tick it changed. */
        std::uint64_t revision = 0;

        /*  ASKS FOR `stored` TO BE WRITTEN, from the tick thread and without
            waiting: serve posts it to a pump that writes on the message thread
            and submits `pair.saved` with what came of it. Empty wherever there
            is no file to write - a replay, a listing, a test that only reads -
            and then a command changes the settings and nothing else. */
        std::function<void (const PairSettings&)> requestSave;

        //======================================================================
        /*  WHAT THIS MACHINE IS IN ITS PAIR, AS THE LOG LAST SAID IT (RD.7;
            pair/Leadership.h): set by the engine's `pair.lead` and `pair.hold`
            as they are applied - in serve and in a replay alike - and moved
            with `seatCount`, so serve acts on a seat on the tick it changed. */
        Seated seated;
        std::uint64_t seatCount = 0;

        /*  AN OPERATOR'S `pair.takeOver`, `pair.handOver` or `pair.keep`,
            accepted and waiting for serve's next tick to make the move -
            which is logged as `pair.lead` or `pair.hold` like any other. A
            replay sets it and nothing reads it: its moves are in the log. */
        std::optional<Ask> asked;

        /*  AN OPERATOR'S `pair.realign`, accepted on a machine that follows and
            waiting for serve's next tick (RD.8): the leader is asked where it
            is, and this machine is seated there again. A replay sets it and
            nothing reads it: the seat it led to is its own record. */
        bool realignAsked = false;

        /*  THE OTHER MACHINE'S SEAT AS THIS ONE SEES IT NOW, `pair/state`'s
            word, empty when it is not linked: what an operator's command is
            applied with (its argument as applied), so a replay refuses alike.
            Serve's; empty everywhere else. */
        std::function<std::string()> peerNow;

        //======================================================================
        /*  RD.10's (namespace draft §58, AFU, AFY), each serve's and empty
            elsewhere unless said.

            A FOLLOW BEGINS: called by `pair.hold` as it is applied, when the
            seat it takes is `following` and the one before was not - the
            backup's history starts at the link (AFY), so serve and a replay
            both hand it the document's `clearHistory`, and a replay forgets
            the steps where the session forgot them. */
        std::function<void()> followBegins;

        /*  Whether anything runs on this machine now: a cue started and not
            ended - what the horizon only made ready does not count.
            `pair.adoptShow` is refused `busy` while it does. */
        std::function<bool()> busyNow;

        /*  `pair.adoptShow`'S ACT, once the command has found the seat
            following and nothing running: the leader's show taken from what
            the follower holds of it (pair/ShowCheck.h) and written and reloaded
            here (`doc::Bundle::adoptShow`) - refused `not-fetched` with nothing
            to take. Empty, as in a replay: refused `not-fetched`, and a replay
            stops before the record anyway (`Bundle::replayBoundary`). */
        std::function<Outcome (CommandContext& context)> adoptShow;
    };

    /*  `pair.setRole`, `pair.setPeers`, `pair.setTimeout`, and the engine's
        `pair.saved` (namespace draft §58.4): all four this machine's alone,
        never mirrored and no step of undo (RD.3), and none refused for the
        show's lock, which is about the show and not the machine it runs on.
        And RD.7's: the engine's `pair.lead` and `pair.hold`, which say what
        this machine now is and why, and the operator's `pair.takeOver`,
        `pair.handOver` and `pair.keep` - machine-local too, since each acts on
        this machine, though each also tells the other machine when it can.
        And RD.8's `pair.realign`, the operator's on a machine that follows.
        And RD.10's `pair.adoptShow`, the operator's too: the follower takes
        its leader's show (the hooks above). */
    void registerPairCommands (CommandRegistry& registry, Pairing& pairing);
}
