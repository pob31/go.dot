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
    WHAT THIS COMPUTER IS IN ITS PAIR, read out of one snapshot (namespace draft
    §58, RD.11): the rows under `/godot/pair` a main and a backup publish,
    turned into the words the transport, its banner, the Show menu and the
    Redundancy tab of Show settings put in front of an operator - and which of
    the operator's five commands would be taken now, and why not when it would
    not.

    THE WORDS ARE IN ONE PLACE, HERE, as the timecode's are (model/Timecode.h).
    Some are the author's, chosen on 2026-10-12 from options the implementer
    wrote (§58.2, AGM-AGP): what a machine says it is beside the transport,
    "Take over" and "Hand over", "Two mains!" and "Make this the main", and the
    tab's name. Everything else is the implementer's, in the shape of the
    author's, listed in §58.7's RD.11 line for his review - so a word he changes
    is one edit here, and one in the page's views/transport.js, which says the
    same things to a browser and cannot read a C++ constant.

    A READING, NEVER A DECISION. Whether a command is refused is the engine's
    to say (pair/Leadership.h `refusal`, PairSettings.cpp); this asks the same
    function the engine asks, with what the rows say, so a button is greyed
    exactly where the engine would refuse it - and the engine still judges the
    press, its refusal a sentence on the transport's line like any other. The
    few things the rows cannot tell the engine's rule (whether the shows were
    compared, whether only files differ, whether a cue runs here) are read here
    from the rows that say them, and said in words.

    EVERY NUMBER IS KEPT AS THE TEXT THE ENGINE PUBLISHED, and read as a number
    only where a sentence is made: a reading is compared field by field on
    every pass, and two doubles compared with `==` is the comparison the strict
    build refuses - a text compares the same way the engine wrote it.

    std only, like the rest of model/.
*/

#include <wfg/client/model/Text.h>
#include <wfg/engine/command/Event.h>

#include <optional>
#include <string>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    namespace pairWords
    {
        /*  THE AUTHOR'S (namespace draft §58.2, 2026-10-12), each as he chose it. */
        inline constexpr const char* tab = "Redundancy";                    // AGP
        inline constexpr const char* mainSending = "Main - sending";        // AGM
        inline constexpr const char* mainAlone = "Main - alone";            // AGM
        inline constexpr const char* backupFollowing = "Backup - following"; // AGM
        inline constexpr const char* takeOver = "Take over";                // AGN
        inline constexpr const char* handOver = "Hand over";                // AGN
        inline constexpr const char* twoMains = "Two mains!";               // AGO
        inline constexpr const char* makeThisTheMain = "Make this the main"; // AGO

        /*  THE IMPLEMENTER'S, in AGM's shape - role, then what it does - for the
            seats the author was not asked about: a backup waiting for its main,
            a backup that took over and sends, a main that follows a backup that
            took over. For his review (§58.7, RD.11). */
        inline constexpr const char* backupWaiting = "Backup - waiting";
        inline constexpr const char* backupSending = "Backup - sending";
        inline constexpr const char* mainFollowing = "Main - following";

        /*  The implementer's too: the two buttons the author was not asked
            about. Realign is the word the engine's own alarm already uses
            ("Realign to take the other machine's position again"); the show
            taken is named after whose it is - the main's on a backup, the
            backup's on a main that follows one. */
        inline constexpr const char* realign = "Realign";
        inline constexpr const char* takeTheShowOf = "Take the ";
        inline constexpr const char* takeTheShowEnd = "'s show";

        //  The role menu of the Redundancy tab, in the order a machine is set up.
        inline constexpr const char* notPaired = "Not paired";
        inline constexpr const char* mainRole = "Main";
        inline constexpr const char* backupRole = "Backup";
    }

    /*  THE OPERATOR'S FIVE (namespace draft §58.4): each one named command,
        `pair.takeOver`, `pair.handOver`, `pair.keep`, `pair.realign` and
        `pair.adoptShow`. */
    enum class PairAct { takeOver, handOver, keep, realign, adoptShow };

    /*  WHETHER A PRESS WOULD BE TAKEN NOW, and why not: `reason` is the
        engine's own word where the engine would refuse it (`not-holding`,
        `not-linked`, ...) or this client's where the rows say more than the
        engine's rule reads (`same-show`, `not-compared`, `files-only`), empty
        when it would be taken; `words` is what the button would do, or why it
        will not, in a sentence - beside a greyed button, never the grey alone
        (§4.8). */
    struct PairOffer
    {
        bool open = false;
        std::string reason = {};
        std::string words = {};
    };

    /*  ONE LINE OF THE REDUNDANCY TAB'S READOUT: what it is about, and what
        the rows say of it now. */
    struct PairLine
    {
        std::string label = {};
        std::string value = {};

        bool operator== (const PairLine& other) const noexcept { return label == other.label && value == other.value; }
    };

    struct PairReading
    {
        //  The machine's own settings (RD.4): the role, the other machine's addresses, the timeout.
        std::string role;               ///< `pair/role`: none, main or backup; empty unsaid
        std::string peers;              ///< `pair/peers`
        std::string timeout;            ///< `pair/timeout`, seconds, as published

        //  What it is now (RD.5-RD.10).
        std::string state;              ///< `pair/state`: alone, leading, following, waiting, bothSending
        Flag sending = Flag::unsaid;    ///< `pair/sending`
        Flag linked = Flag::unsaid;     ///< `pair/linked`
        std::string peer;               ///< `pair/peer`
        std::string alarm;              ///< `pair/alarm`, clipped
        Flag inStep = Flag::unsaid;     ///< `pair/inStep`
        std::string differs;            ///< `pair/differs`, clipped
        Flag showMatch = Flag::unsaid;  ///< `pair/showMatch`
        std::string showDiffers;        ///< `pair/showDiffers`, clipped
        Flag mapped = Flag::unsaid;     ///< `pair/mapped`

        //  The numbers, as the engine wrote them.
        std::string roundTrip;          ///< `pair/roundTrip`, seconds
        std::string delay;              ///< `pair/delay`, ticks
        std::string drift;              ///< `pair/drift`, ppm
        std::string clockResidual;      ///< `pair/clockResidual`, µs
        std::string outputLatency;      ///< `pair/outputLatency`, seconds
        std::string peerOutputLatency;  ///< `pair/peerOutputLatency`, seconds
        std::string received, gaps, late, relayed, dropped, unconfirmed;

        /*  A CUE RUNS HERE - one started and not ended, what the horizon only
            made ready left out, as `pair.adoptShow`'s `busy` counts (Console.cpp,
            `busyNow`). Read only where it decides something: on a machine that
            follows and whose show differs. */
        bool busy = false;

        //======================================================================
        /** Whether this machine has a role: main or backup. */
        bool paired() const;

        /*  WHAT THIS MACHINE IS, IN AGM'S WORDS: "Backup - following", "Main -
            sending", "Main - alone", and the implementer's for the other seats;
            empty when it is not paired or has not said. */
        std::string seatWords() const;

        /** The seat in a sentence: what this machine does now, and what the other does. */
        std::string seatSentence() const;

        /** "main" on a backup, "backup" on a main: who the other machine is. */
        std::string other() const;

        //  The banner (RD.7's alarm, said when there is one, with the one button that answers it).
        bool bannerShowing() const;
        std::string bannerHeading() const;      ///< "Two mains!", or the seat's words
        std::string bannerSentence() const;     ///< the engine's alarm, and why its button is greyed when it is
        std::optional<PairAct> bannerAct() const;
        bool bannerAlarming() const;            ///< two machines sending: the alarm's red rather than amber

        //  Each of the five.
        std::string buttonWords (PairAct act) const;
        PairOffer offer (PairAct act) const;

        /*  THE TAB'S READOUT, in order: this machine, the other, the timing,
            the output delays and the jump a changeover makes, in step, the
            same show, the counts. */
        std::vector<PairLine> lines() const;

        bool operator== (const PairReading& other) const noexcept;
        bool operator!= (const PairReading& other) const noexcept { return ! (*this == other); }
    };

    /** One pass over the snapshot's rows under `/godot/pair`. */
    PairReading readPair (const tree::TreeSnapshot& snapshot);

    /** The one named command each of the five is (§4.11). */
    Event eventFor (PairAct act);

    /*  WHETHER A RUN IS LIVE in the sense `pair.adoptShow` refuses for: one in
        `/godot/run/order` started and not ended. */
    bool anyRunLive (const tree::TreeSnapshot& snapshot);
}
