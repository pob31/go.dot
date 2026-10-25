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
    A BACKUP THAT FOLLOWS ITS MAIN (namespace draft §58, RD.6; AFT, AGD, AGF,
    AGG).

    A machine whose role is `backup` and whose link to its main is up FOLLOWS:
    it applies what the main applied, a fixed number of ticks later; it holds
    every send meant for a device (RD.1's hold); it passes on to the main
    whatever its own clients ask of it; and it ignores what devices tell it,
    since the main heard them too and the main's copy arrives in the stream.
    From RD.7 who follows whom is pair/Leadership.h's to decide - a main
    started again follows the backup that took over, and two machines both
    sending each follow the other's operators - and a follower whose stream
    falls silent takes over: its hold let go, its gate opened, and what it
    passed on and never saw answered settled (`settle`, AGJ). And from RD.8
    a follower is seated at its leader's position when its follow begins or
    an operator asks again (`position`, pair/Position.h): held to its slot
    with the records, and submitted there as the engine's `pair.seat`.

    TWO PIECES, FOR TWO THREADS.

    `FollowGate` is the door in front of the engine's queue (`Engine::setGate`)
    and runs on whichever thread submits - the WebSocket's, the window's, a
    surface's, Pd's - deciding from the event alone (`gateFor`, AGD's table)
    and never waiting: a command passed on is handed to the dialler's queue.

    `Follower` is the tick thread's: the main's records as they arrive, held
    until their slot - the main's tick plus the offset fixed at link-up - and
    submitted at the head of the tick that slot names, all of one main tick on
    one local tick, in the main's order, exactly as `wfg replay` groups a log
    by tick. And each one compared, once applied, with what the main made of
    it: the first instrument of "in step" (RD.8 builds the rest).

    WHY A FIXED DELAY (AGF). The GO debounce, Doh!'s window and undo's merging
    all compare gaps between ticks. Applied as they arrive, two GOs the main
    took 0.52 s apart could reach the backup 0.48 s apart - the network's
    jitter, not the operator's - and the backup would refuse the second. Each
    record of main tick T goes in on local tick T + offset, so every gap is the
    main's to the tick. A record that misses its slot goes in at once, and is
    counted: `pair/late`.

    HOW THE OFFSET IS CHOSEN. Over the first beats after the link comes up,
    each beat's arrival is read as a fractional tick of this machine's clock -
    the tick it arrived in, and how far into it - and the main's tick is taken
    away: e = arrival - T. The smallest e is the two tick clocks' offset plus
    the fastest the main's work and the wire ever were; the largest, the
    slowest. A record of tick T arrives by T + max(e) at the latest seen, so
    its slot is put a whole tick after that - offset = ceil (max e) + 1 - which
    leaves a tick of margin for jitter nobody saw while measuring. Δ, the
    delay published (`pair/delay`) and the extra launch lead RD.9 will add, is
    how far behind the main that slot is in time: offset less the clocks' own
    offset, which is the smallest e less half the round trip, to the nearest
    tick and never under one.
*/

#include <wfg/engine/command/Event.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/pair/PeerLink.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace wfg
{
    class CommandRegistry;
    struct Command;
}

namespace wfg::pair
{
    //==========================================================================
    /*  AGD's TABLE: what a following backup does with an event submitted to it.

          apply    - the engine's own (`engine`), the main's records (`main:`),
                     a replay's, and this machine's own settings
                     (`isMachineLocal`, whoever asked): they belong here.
          passOn   - what this machine's own clients ask of it: the window,
                     a WebSocket client (`ws:`), a control surface
                     (`surface:`), the command line (`cli`), a patch saved in
                     the patch editor (`pd`). The main applies it and streams it
                     back (AFT).
          ignore   - what a device said: OSC over UDP (`udp:`, AGD - so the
                     remote app speaks WebSocket), a device's report and
                     answer (`mount:`), a serial port (`serial:`), a device
                     over a connection (`link:`), MIDI (`midi:`), a time-of-day
                     trigger and the wall clock as a code (`clock`), an
                     incoming code's turns (`ltc:`, `mtc:`; namespace draft
                     §60, TG.5) - a code reaching both machines fires once,
                     from the main's records - and, beyond §58.2's list, a
                     process cue's patch (`process:`), which each machine runs
                     on its own threads at its own moments, so its commands
                     are a device's: the main's arrive in the stream. And
                     anything this table does not know: a follower changes
                     nothing on its own word.

        Decided from the origin and, for `isMachineLocal`, the command and its
        arguments - never from anything a machine holds - so it reads alike on
        any thread. `command` may be null for one nobody registered. */
    enum class Gate { apply, passOn, ignore };

    Gate gateFor (std::string_view origin, const Command* command, const std::vector<osc::Value>& args);

    /*  `/godot/pair/relay s:line h:ticket`: the event as the line of an applied
        record, origin and all (PeerLink.h), and the ticket the leader carries
        back on the record that answers it (RD.7, AGJ); none when nought. */
    std::vector<std::uint8_t> relayOf (const Event& event, std::uint64_t ticket = 0);

    //==========================================================================
    /*  WHAT A FOLLOWER THAT TAKES OVER DOES WITH WHAT IT PASSED ON AND NEVER
        SAW ANSWERED (namespace draft §58, RD.7, decision AGJ): Esc and double
        Esc - the operator's stops, PRD §6.2's list (command/Recovery.h) - are
        applied here; everything else is dropped and counted: a GO, Doh!, a
        fader, a standby move. A command never seen to land cannot be known to
        have landed, and of all of them only a stop is safe to make twice. */
    struct Settled
    {
        std::vector<Event> applied;     // to submit here, in the order they were given
        std::int64_t dropped = 0;
    };

    Settled settle (std::vector<Event> unconfirmed);

    //==========================================================================
    /*  THE DOOR (`Engine::setGate`'s), any thread. `take` answers whether it
        took the event - passed on, or ignored - and so kept it from the queue. */
    class FollowGate
    {
    public:
        /*  Hands a relay to the link; false when it could not - no path open -
            and the command is then lost, and counted. Must never block. */
        using PassOn = std::function<bool (std::vector<std::uint8_t> bytes)>;

        /*  `firstTicket` is where this session's relay tickets start: serve
            draws it, so a ring the main replays to a backup started again
            never answers a ticket of the new session (RD.7). */
        FollowGate (const CommandRegistry& commandsToRead, PassOn passOnToUse, std::uint64_t firstTicket = 1);

        FollowGate (const FollowGate&) = delete;
        FollowGate& operator= (const FollowGate&) = delete;

        bool take (Event& event);

        /** Passed on, ignored, and lost for want of a path. Any thread. */
        std::int64_t passedOn() const noexcept { return passedOnCount.load (std::memory_order_relaxed); }
        std::int64_t ignored() const noexcept  { return ignoredCount.load (std::memory_order_relaxed); }
        std::int64_t lost() const noexcept     { return lostCount.load (std::memory_order_relaxed); }

        //======================================================================
        /*  THE LEDGER OF RELAYS NOT YET ANSWERED (RD.7, AGJ). Every command
            passed on is kept here with its ticket until the record that answers
            it - applied or refused on the leader - comes back down the stream
            (`confirm`, the tick thread's, as the receiver hands each record
            on). One that could not even be sent - no path open, which is the
            moment the leader has just gone - is kept too: an Esc pressed in
            the second before a takeover is the Esc most needed after it. */

        /** The record answering this ticket has arrived. Tick thread. */
        void confirm (std::uint64_t ticket);

        /*  Those not answered, oldest first, every one forgotten: what a
            follower that stops following settles (`settle`). One passed on more
            than `maxAgeSeconds` ago is not among them - with the link up all
            that time, the leader never had it to answer (a path that dropped
            and was dialled again) and it is not this takeover's to settle.
            Tick thread. */
        std::vector<Event> takeUnconfirmed (LinkClock::time_point now, double maxAgeSeconds);

        /*  Forgets those passed on more than `maxAgeSeconds` ago, counting each
            as unconfirmed: called once a tick while following, so the ledger
            holds a few seconds at most. Tick thread; one lock, and nothing to
            do on nearly every tick. */
        void expire (LinkClock::time_point now, double maxAgeSeconds);

        /** Relays waiting for their answer. Any thread. */
        std::size_t unanswered() const;

        /** Relays forgotten unanswered - expired, or dropped by a takeover. Any thread. */
        std::int64_t unconfirmed() const noexcept { return unconfirmedCount.load (std::memory_order_relaxed); }

        /** Counts relays a takeover dropped (`settle`'s). Tick thread. */
        void countDropped (std::int64_t count) noexcept { unconfirmedCount.fetch_add (count, std::memory_order_relaxed); }

    private:
        struct Pending
        {
            std::uint64_t ticket = 0;
            Event event;
            LinkClock::time_point at {};
        };

        const CommandRegistry& commands;
        PassOn passOn;

        std::atomic<std::uint64_t> nextTicket;
        mutable std::mutex ledgerLock;
        std::deque<Pending> pending;

        std::atomic<std::int64_t> passedOnCount { 0 }, ignoredCount { 0 }, lostCount { 0 }, unconfirmedCount { 0 };
    };

    //==========================================================================
    /*  THE TICK THREAD'S HALF: the main's records held to their slot, applied,
        and compared. Fed by the receiver's callbacks (`beat`, `record`) as it
        drains, driven from the head of the before-tick hook (`beforeTick`),
        told every record written (`applied`, the engine's after-record hook). */
    class Follower
    {
    public:
        /*  How a record goes in: `Engine::submit`, which the gate lets through
            for a `main:` origin. */
        using Submit = std::function<bool (Event event)>;

        explicit Follower (Submit submitToUse);

        Follower (const Follower&) = delete;
        Follower& operator= (const Follower&) = delete;

        /** The length of one tick, in seconds: the sample clock's, 0.02 at 50 Hz. */
        void setTickSeconds (double seconds) noexcept;

        /*  THIS MACHINE'S SAMPLES A SECOND, which a main's `code.heard` sample
            is moved onto this clock at (namespace draft §60, TG.5; AIP): by
            the offset the records are applied at, through
            `ClockMap::sampleOnThisClock`, before it is submitted - so the
            record this machine logs holds its own sample, a replay of it never
            maps, and what the main sent compares equal to what was applied. */
        void setSampleRate (int rate) noexcept { sampleRate = rate; }

        /*  THE MAP FROM THE LEADER'S SAMPLES TO THIS MACHINE'S (namespace
            draft §58, RD.9; pair/ClockMap.h), as serve composes it each tick:
            a main's `code.heard` and a position's code maps and scenes are
            moved through it - the leader's own moment, once it is trusted; the
            ticks' worth of samples until then. Untrusted until set. */
        void setClockMap (const ClockMap& map) noexcept { clockMap = map; }
        const ClockMap& clockMapInUse() const noexcept { return clockMap; }

        /*  WHAT THE LEADER SAID OF ITS CLOCKS (RD.9, `/godot/pair/clock`), in
            the stream's order: held to the slot of the tick it is true of, as
            a record of that tick is, so the lead a launch here is placed with
            is the lead the leader placed the same launch with. */
        void clockSaid (const wire::ClockSaid& said);

        /*  The leader's word on its clocks as of the last slot passed - what
            its launches of the tick this one applies were placed with; null
            before its first. */
        const wire::ClockSaid* leaderClock() const noexcept { return hasLeaderClock ? &leaderClockNow : nullptr; }

        /*  How many of the main's beats are measured before the offset is
            fixed: a fifth of a second of them at 50 Hz, enough to see the
            jitter of an ordinary network and short enough that a show started
            straight after the link misses nothing. The offset waits for a
            round trip too, or for three times as many beats without one. */
        static constexpr int measuredBeats = 10;

        //======================================================================
        /*  THE LINK CAME UP: a fresh offset is measured, and this link's counts
            start again. What was received before and not yet applied waits for
            the offset like the rest. */
        void start();

        /*  THE LINK WENT: what was received and not yet applied is applied at
            once - it is the main's history, and the backup is closest to the
            main with it - and nothing more is held for a slot. */
        void stop();

        bool isFollowing() const noexcept { return following; }

        //======================================================================
        /** One of the main's ticks, as its beat first arrived (Receiver::setOnBeat). */
        void beat (std::int64_t mainTick, LinkClock::time_point arrival);

        /** One of the main's records, in counter order (Receiver::setOnRecord). */
        void record (const LogRecord& mainRecord);

        /*  THE LEADER'S POSITION (RD.8; pair/Position.h), in the stream's order
            among its records (Receiver::setOnPosition): held to the slot of the
            tick it is true at, as a record of that tick is, and submitted there
            as the engine's `pair.seat` - its text moved onto this machine's
            clock first, by the offset, so the record logged speaks this
            machine's ticks and a replay needs nothing else. The records after
            it go in after it, on top. One still held when following stops is
            submitted with the rest, if the offset was fixed; with none fixed
            the leader's ticks cannot be read as this machine's, and it is
            dropped. A text that does not read is dropped too, and counted. */
        void position (std::int64_t leaderTick, std::string text);

        /** Whether a position waits for its slot. */
        bool seatHeld() const noexcept;

        /** Seats submitted, and positions dropped unread or with no clock to read them on. */
        std::int64_t seats() const noexcept         { return seatCount; }
        std::int64_t seatsDropped() const noexcept  { return droppedSeats; }

        /*  AT THE HEAD OF THE BEFORE-TICK HOOK of local tick `localTick`, which
            was due at `due` (now, less the tick's lateness): the beats since
            the last are measured, the offset fixed once there are enough, and
            every record whose slot has come is submitted, in order. `frozen`: a
            pass at a frozen tick (an outage, the show held, PRD §3.34), where
            no slot will come - nothing is measured, and every record that has
            arrived goes in at once, Play among them, or a follower held with
            its leader would wait for ever for a slot its own tick never
            reaches. */
        void beforeTick (std::int64_t localTick, LinkClock::time_point due, double roundTripSeconds,
                         bool frozen = false);

        /*  EVERY RECORD THIS MACHINE WRITES (Engine::setAfterRecord): one this
            follower submitted - origin `main:` - is compared with the main's,
            its kind, its reason and its arguments as applied. */
        void applied (const LogRecord& written);

        //======================================================================
        bool delayFixed() const noexcept { return fixed; }

        /** Δ in ticks, nought until fixed. */
        int delay() const noexcept { return fixed ? delta : 0; }

        /** Local tick = main tick + this, for a record on time; nought until fixed. */
        std::int64_t offset() const noexcept { return fixed ? slotOffset : 0; }

        /*  The two tick clocks' offset as measured - the local tick at the
            moment the main's tick 0 began - and the spread of the beats'
            arrivals, in ticks: what Δ was chosen from, for a test or a note. */
        double clockOffset() const noexcept { return clocks; }
        double spread() const noexcept { return latestSeen - earliestSeen; }

        /** Records applied after their slot, this link. */
        std::int64_t late() const noexcept { return lateCount; }

        /** Records applied otherwise than on the main, this link, and the first in a sentence. */
        std::int64_t differences() const noexcept { return differenceCount; }
        const std::string& differs() const noexcept { return differsText; }

        /** Records received and not yet submitted, a held position among them. */
        std::size_t waiting() const noexcept { return held.size(); }

    private:
        struct Arrival
        {
            std::int64_t mainTick = 0;
            LinkClock::time_point at {};
        };

        void submitDue (std::int64_t localTick, bool all, bool countLate = true);
        void compare (const LogRecord& mainRecord, const LogRecord& written);

        Submit submit;
        double tickSeconds = 0.02;
        int sampleRate = 0;
        ClockMap clockMap;

        //  RD.9's: the leader's words on its clocks waiting for their slot, and the one in force.
        std::deque<wire::ClockSaid> clocksHeld;
        wire::ClockSaid leaderClockNow;
        bool hasLeaderClock = false;

        bool following = false;
        bool fixed = false;
        int delta = 0;
        std::int64_t slotOffset = 0;

        //  The measurement, while it lasts.
        std::vector<Arrival> beats;          // since the last tick, reused
        int beatsMeasured = 0;
        double earliestSeen = 0.0, latestSeen = 0.0, clocks = 0.0;

        /*  The main's records waiting for their slot, a position among them
            (`seat`, its text the record's one argument and its tick the
            leader's), and those submitted and not yet seen applied. */
        struct Held
        {
            LogRecord record;
            bool seat = false;
        };

        std::deque<Held> held;
        std::deque<LogRecord> inFlight;
        std::int64_t seatCount = 0, droppedSeats = 0;

        std::int64_t lateCount = 0;
        std::int64_t differenceCount = 0;
        std::string differsText, firstDifference;
    };
}
