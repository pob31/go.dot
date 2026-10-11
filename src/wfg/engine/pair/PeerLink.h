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
    THE LINK BETWEEN A MAIN AND ITS BACKUP: THE WIRE AND WHAT IT SAYS ABOUT
    ITSELF (namespace draft §58, RD.5; decisions AFV, AGB, AGD, AGF).

    The backup dials the main on the port every client of a serve already
    uses - the WebSocket, binary OSC - and the main sends down it every record
    it writes to its log that the backup will one day apply (RD.6), as its log
    writes it, and a heartbeat at the end of every tick. Both say whether they
    are linked. RD.5 is the wire, and the truthfulness of what it says; what
    a backup does with what it receives - holds it to a fixed delay and
    applies it, holds its own sends, passes its own clients' commands on to
    the main - is RD.6's (pair/Follow.h).

    THREE CLASSES, AND WHY THREE. `Stream` is the main's half: the filter, the
    ring a reconnecting backup asks from, the followers and what each has
    been sent, the heartbeat. `Receiver` is the backup's half of the protocol:
    the counter checked for gaps and duplicates, the heartbeat's arrival, the
    round trip. `Dialler` is the backup's sockets - one WebSocket client per
    address of the main, each on an io thread of its own, redialled quickly
    when it drops - and nothing else. The split is the one between a protocol
    and the transport it rides on: the first two take and give OSC packets and
    never see a socket, so a test runs a main and a backup in one process,
    joined by a function, and asserts on every counter without a port; the
    third is plumbing proved against the real server (PeerLinkTests, and
    `tests/blackbox/pair_link.py` between two processes).

    THE WIRE, every message binary OSC under `/godot/pair/`, which the server
    hands to the link and never to the namespace: a pairing frame never
    becomes an event, and never reaches the log (OscQueryServer::setPairDoor).
    Over the WebSocket only - a UDP datagram to the same address is a node
    write like any other, and refused as one (AGD: udp is a device's).

      backup -> main
        /godot/pair/hello   s:machine s:role h:lastCounter s:stream
        /godot/pair/ping    h:sent f:roundTrip
        /godot/pair/relay   s:line                          (RD.6)
      main -> backup
        /godot/pair/welcome s:stream h:next
        /godot/pair/gap     h:oldest
        /godot/pair/record  h:counter s:line
        /godot/pair/beat    h:tick h:lastCounter
        /godot/pair/pong    h:sent h:tick

    What a tick sends one follower - its welcome and a gap when it has just
    said hello, the records it has not had, the beat last - goes as ONE OSC
    bundle, one WebSocket frame: a frame a message cost the main's tick a
    send each, and a send is not cheap (PeerLink.cpp). A pong goes alone, at
    once.

    A RELAY IS A COMMAND THE BACKUP'S OWN CLIENT GAVE IT (RD.6, AFT), passed on
    rather than applied: the same line, its origin the client's on the backup
    - `ws:...`, `window`, `cli`, `surface:...`, `pd` - which the main applies
    as `peer:<origin>` and streams back like anything else it applies.

    A RECORD IS ITS LOG LINE (`LogRecord::toLine`), the grammar a replay has
    read back since Phase 1, carried as one OSC string - so what crosses the
    link is exactly what the main's log says it applied, arguments as applied,
    and is read with the reader that already reproduces whole sessions.

    THE STREAM'S OWN COUNTER, not the log's `seq`: once the engine's own
    records are filtered out, `seq` has gaps by construction, and a gap must
    mean a record lost. A stream has an identity too, drawn once per session
    of the main, which is what lets a backup tell a main that went away and
    came back from a main that went on without it: counters restart with a
    new main, and "the next after 500" means nothing to a stream that has
    sent three.
*/

#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/osc/OscCodec.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/pair/LinkReading.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wfg
{
    struct Command;
}

namespace wfg::pair
{
    /*  Every time the link measures is on the steady clock of the machine
        that measures it: a heartbeat's arrival, a ping's departure. Never the
        wall clock, which a time server may move under a show. */
    using LinkClock = std::chrono::steady_clock;

    namespace wire
    {
        inline constexpr std::string_view prefix = "/godot/pair/";

        inline constexpr const char* hello   = "/godot/pair/hello";
        inline constexpr const char* ping    = "/godot/pair/ping";
        inline constexpr const char* welcome = "/godot/pair/welcome";
        inline constexpr const char* gap     = "/godot/pair/gap";
        inline constexpr const char* record  = "/godot/pair/record";
        inline constexpr const char* beat    = "/godot/pair/beat";
        inline constexpr const char* pong    = "/godot/pair/pong";
        inline constexpr const char* relay   = "/godot/pair/relay";

        /** Whether an address is the link's: `/godot/pair/` and something after it. */
        bool isPairAddress (std::string_view address) noexcept;

        /** Whether raw OSC bytes are a message to one of the link's addresses, without decoding them. */
        bool startsWithPairAddress (const std::uint8_t* data, std::size_t size) noexcept;

        /** The bytes of one message; empty when the codec refuses it (it never does for these). */
        std::vector<std::uint8_t> encode (const char* address, std::vector<osc::Value> args);

        /*  THE TWO MESSAGES A TICK MAKES, written straight into a buffer that
            is reused - the bytes `encode` would make, without its cost on the
            tick thread (PeerLink.cpp says what it measured). `writeRecord`
            refuses a line holding a NUL, which no log line does, and leaves
            `out` as it was. */
        bool writeRecord (std::vector<std::uint8_t>& out, std::uint64_t counter, std::string_view line);
        void writeBeat (std::vector<std::uint8_t>& out, std::int64_t tick, std::uint64_t lastCounter);
    }

    /*  HOW MANY RECORDS A MAIN KEEPS FOR A BACKUP THAT RECONNECTS. A backup
        whose cable came out for a moment asks for what it missed by counter,
        and the main answers from here; one away for longer than the ring
        holds is told `gap` instead, and is seated afresh (RD.8). 8192 is
        minutes of a busy show - a fader ridden at the tick rate is fifty
        records a second, so nearly three minutes of nothing but that - and
        a megabyte or two of memory: a ring kept by count, since a record's
        size is its arguments', a few hundred bytes for nearly all of them. */
    inline constexpr std::size_t ringCapacity = 8192;

    /*  HOW MANY OF THEM ONE TICK SENDS ONE FOLLOWER. A backup that has just
        said hello may be owed the whole ring, and eight thousand sends on one
        tick of the main's clock would be the main's lateness, paid for a
        machine that is not playing anything yet. So it is sent a slice a
        tick and catches up over a few - sixteen ticks for a full ring, a
        third of a second - while the main's own records go on being kept. */
    inline constexpr std::size_t backlogPerTick = 512;

    /** How often a backup pings each path to its main, for the round trip and so the main knows it is there. */
    inline constexpr std::chrono::milliseconds pingEvery { 100 };

    //==========================================================================
    /*  WHAT TRAVELS (§58.1's table): a record the main applied (`A`) or
        refused (`R` - the backup must refuse it too, as a replay does), whose
        origin is not the engine's own (nor a replay's), that is not this
        machine's alone (RD.3, `isMachineLocal`), and that is not a pairing
        command. A packet that never became a command (`X`) does not travel:
        there is nothing in it to apply. `command` is the registry's entry for
        the record's command, null for one nobody registered - which travels,
        refused, as it was refused here. */
    bool travels (const LogRecord& record, const Command* command);

    /*  THE RECORD AS THE LINK SENDS IT, or nothing when it does not travel.
        The one change to what the log holds: a `node.setMany` that set some of
        this machine's rows beside the show's travels without its machine
        pairs, so the backup never writes the main's interface or port names -
        `doc::Schema::isMachineAddress`, from the address alone. Everything
        else travels exactly as the log wrote it. */
    std::optional<LogRecord> keptForLink (const LogRecord& record, const Command* command);

    //==========================================================================
    /*  THE MAIN'S HALF. Kept on the tick thread, sent from the tick thread,
        with one inbox from the server's thread: hellos, pings and closes are
        pushed there and read at the start of the next `afterTick`, so the
        ring and the followers are the tick thread's alone and no lock is ever
        held across a send. The one thing answered on the server's thread is
        a ping, at once, from the tick index alone - the round trip is the
        point of it, and the tick may be busy.

        A STREAM THAT DOES NOT LEAD keeps nothing and takes no follower: a
        machine that is not a main (`setLeading`, from the role in force) pays
        one test per record. A hello reaching it is ignored and counted. */
    class Stream
    {
    public:
        /*  Sends one message to one connection; false when that connection is
            gone, and the follower is then forgotten. Called from the tick
            thread and, for a pong, from the server's: it must take either,
            and must never wait on the network - serve's is
            `OscQueryServer::sendTo`, which queues. */
        using Send = std::function<bool (const std::string& origin, const std::vector<std::uint8_t>& bytes)>;

        explicit Stream (std::string identity, std::size_t ringSize = ringCapacity,
                         std::size_t perTick = backlogPerTick);

        Stream (const Stream&) = delete;
        Stream& operator= (const Stream&) = delete;

        /** Before anything can arrive or be sent. */
        void setSend (Send sendToUse) { send = std::move (sendToUse); }

        /** This session's identity, said in every welcome. Any thread. */
        const std::string& id() const noexcept { return streamId; }

        //======================================================================
        //  The tick thread's.

        /** Whether this machine is a main: keeps records and takes followers. */
        void setLeading (bool shouldLead) noexcept { leading.store (shouldLead, std::memory_order_relaxed); }
        bool isLeading() const noexcept { return leading.load (std::memory_order_relaxed); }

        /*  WHAT A MAIN DOES WITH A COMMAND ITS BACKUP PASSED ON (RD.6, AFT): the
            record read back from the relay's line, its origin the backup's
            client's. Serve's submits it as `peer:<origin>`. Called on the
            server's thread, at once - a GO passed on waits for no tick here -
            and only while this machine leads; set before anything arrives. */
        using Relayed = std::function<void (const LogRecord& record)>;
        void setRelayed (Relayed callback) { relayed = std::move (callback); }

        /** Commands passed on by a backup and handed to `setRelayed`'s callback. Any thread. */
        std::int64_t relaysTaken() const noexcept { return relayCount.load (std::memory_order_relaxed); }

        /*  THE ENGINE'S AFTER-RECORD HOOK (Engine::setAfterRecord): every record,
            as written. One that travels is given the next counter and its
            message is put in the ring - the one thing GO's path gains. */
        void keep (const LogRecord& record, const Command* command);

        /*  THE HEARTBEAT, from serve's after-tick: what arrived since the last
            tick is read, then each follower is sent what it has not had - its
            welcome and any gap first, then the records, `backlogPerTick` at
            most - and the beat, which says this tick and the last counter
            kept. A hung engine sends no beat, which is what lets a backup
            tell a hung main from a slow one (AFV). `timeoutSeconds` decides
            whether a hello starts a new link (the counters start again) or
            joins the one that is up, by when the hello arrived. */
        void afterTick (std::int64_t tick, double timeoutSeconds);

        /*  What the `pair` rows publish on a main. `readInto` fills a reading kept
            from tick to tick, so the peer's text is copied only when it moved. */
        LinkReading reading (LinkClock::time_point now, double timeoutSeconds) const;
        void readInto (LinkReading& out, LinkClock::time_point now, double timeoutSeconds) const;

        /** The last counter given, nought before any; the oldest the ring still holds. */
        std::uint64_t lastCounter() const noexcept { return last; }
        std::uint64_t oldestCounter() const noexcept { return oldest; }
        std::size_t followerCount() const noexcept { return followers.size(); }

        //======================================================================
        //  The server's thread.

        /** A `/godot/pair/` message from a WebSocket connection. */
        void received (const std::string& origin, const osc::Packet& message, LinkClock::time_point arrival);

        /** That connection closed: its follower is forgotten. */
        void closed (const std::string& origin);

        /** Pairing messages that were not the link's grammar, or not for a main. Any thread. */
        std::int64_t ignored() const noexcept { return ignoredCount.load (std::memory_order_relaxed); }

    private:
        struct Arrival
        {
            enum class Kind { hello, ping, closed };

            Kind kind = Kind::hello;
            std::string origin;
            std::string machine;
            std::string role;
            std::string stream;
            std::uint64_t lastCounter = 0;
            double roundTrip = 0.0;
            LinkClock::time_point at {};
        };

        struct Follower
        {
            std::string machine;
            std::uint64_t sentUpTo = 0;         // the last counter sent down this connection
            std::uint64_t next = 0;             // the first counter its welcome promised
            bool welcomed = false;
            std::uint64_t gapOldest = 0;        // non-zero: a `gap` still to be said
            bool pinged = false;
            LinkClock::time_point lastPing {};
            double roundTrip = 0.0;
        };

        void take (const Arrival& arrival, double timeoutSeconds);
        bool heardWithin (LinkClock::time_point at, double timeoutSeconds) const;
        const std::vector<std::uint8_t>* ringed (std::uint64_t counter) const;

        const std::string streamId;
        const std::size_t capacity;
        const std::size_t backlog;
        Send send;
        Relayed relayed;

        //  Written on the tick thread, read on the server's too (a relay).
        std::atomic<bool> leading { false };

        //  The tick thread's.
        std::vector<std::vector<std::uint8_t>> ring;
        std::vector<std::uint8_t> frame, beatBytes;    // a tick's bundle and its beat, reused
        std::uint64_t last = 0, oldest = 1;
        std::map<std::string, Follower> followers;
        std::vector<Arrival> arrivals;          // swapped with the inbox, reused
        bool heardAny = false;
        LinkClock::time_point lastHeard {};     // the latest hello or ping, any follower
        std::string lastPinger;
        std::uint64_t highestSent = 0;
        std::int64_t sentThisLink = 0, gapsThisLink = 0;

        //  Both.
        std::mutex inboxLock;
        std::vector<Arrival> inbox;
        std::atomic<std::int64_t> tickNow { 0 };
        std::atomic<std::int64_t> ignoredCount { 0 };
        std::atomic<std::int64_t> relayCount { 0 };
    };

    //==========================================================================
    /*  THE BACKUP'S HALF OF THE PROTOCOL. What the main sends is delivered from
        the dialler's io threads - one per path - into one inbox, parsed there
        (a record's line read back into a LogRecord off the tick thread), and
        the tick thread drains it once a tick, holding the lock only to swap
        the vector.

        ONE COUNTER, ANY NUMBER OF PATHS (AFV): a record already received -
        its counter at or below the last one - is a duplicate and dropped,
        whichever path brought it first; one that jumps past the next is a
        gap, counted. Linked is a heartbeat within the timeout on any path. */
    class Receiver
    {
    public:
        Receiver() = default;

        Receiver (const Receiver&) = delete;
        Receiver& operator= (const Receiver&) = delete;

        //======================================================================
        //  Any thread: the dialler's.

        /*  A `/godot/pair/` message from the main, by the path that brought
            it - the main's address as dialled - stamped with when it arrived. */
        void deliver (const std::string& path, const osc::Packet& message, LinkClock::time_point arrival);

        /** What a hello says it has: the last counter drained, and the stream it belongs to. */
        std::uint64_t lastCounter() const noexcept { return lastSeen.load (std::memory_order_relaxed); }
        std::string streamId() const;

        /** The round trip as it was last published, for a ping to tell the main. */
        double roundTripToSay() const noexcept { return roundTripSaid.load (std::memory_order_relaxed); }

        /** Pairing messages that were not the link's grammar. */
        std::int64_t ignored() const noexcept { return ignoredCount.load (std::memory_order_relaxed); }

        //======================================================================
        //  The tick thread's.

        /*  Reads what arrived. `timeoutSeconds` decides whether a welcome
            starts a new link - the counts start again - or joins the one up. */
        void drain (LinkClock::time_point now, double timeoutSeconds);

        /** What the `pair` rows publish on a backup; `readInto` as the stream's. */
        LinkReading reading (LinkClock::time_point now, double timeoutSeconds) const;
        void readInto (LinkReading& out, LinkClock::time_point now, double timeoutSeconds) const;

        /*  EACH RECORD RECEIVED, once, in counter order, as the main's log
            wrote it: RD.6 applies them here. Tick thread. */
        using OnRecord = std::function<void (std::uint64_t counter, const LogRecord& record)>;
        void setOnRecord (OnRecord callback) { onRecord = std::move (callback); }

        /*  EACH OF THE MAIN'S TICKS, once, as its heartbeat first arrived on any
            path, with when it arrived: what a following backup measures the
            fixed delay from (RD.6, AGF). A beat a second path brought later,
            or one older than the last, is not handed on. Tick thread. */
        using OnBeat = std::function<void (std::int64_t mainTick, LinkClock::time_point arrival)>;
        void setOnBeat (OnBeat callback) { onBeat = std::move (callback); }

        /*  `reading (now, timeout).linked`, without the reading: one comparison,
            for the tick that decides whether this machine follows. */
        bool isLinked (LinkClock::time_point now, double timeoutSeconds) const;

        /** Records a second path brought again, dropped. */
        std::int64_t duplicates() const noexcept { return duplicateCount; }

        /** The main's tick and last counter, as its last heartbeat said them. */
        std::int64_t mainTick() const noexcept { return beatTick; }
        std::uint64_t mainLastCounter() const noexcept { return beatLast; }

    private:
        struct Item
        {
            enum class Kind { welcome, gap, record, beat, pong };

            Kind kind = Kind::beat;
            std::string path;
            LinkClock::time_point at {};
            std::int64_t first = 0, second = 0;
            std::string stream;
            LogRecord record;
        };

        void take (Item& item, double timeoutSeconds);
        void heard (LinkClock::time_point at);
        double fastestTrip (LinkClock::time_point now) const;

        //  The tick thread's.
        std::vector<Item> items;                // swapped with the inbox, reused
        std::string stream;
        std::uint64_t last = 0;
        bool heardAny = false;
        LinkClock::time_point lastHeard {};     // the latest welcome, record or beat
        bool beaten = false;
        LinkClock::time_point lastBeat {};
        std::int64_t beatTick = 0;
        std::uint64_t beatLast = 0;
        std::string peer;
        std::int64_t receivedThisLink = 0, gapsThisLink = 0, duplicateCount = 0;
        OnRecord onRecord;
        OnBeat onBeat;

        /*  THE LAST SECOND'S ROUND TRIPS, a few a path: their arrival and their
            length. A fixed ring - a ping a tenth of a second on each of a few
            paths never needs more - so a pong costs no allocation. */
        struct Trip
        {
            LinkClock::time_point at {};
            double seconds = 0.0;
        };

        std::array<Trip, 64> trips {};
        std::size_t tripsKept = 0, tripAt = 0;

        //  Both.
        mutable std::mutex lock;
        std::vector<Item> inbox;
        std::string streamShared;
        std::atomic<std::uint64_t> lastSeen { 0 };
        std::atomic<double> roundTripSaid { 0.0 };
        std::atomic<std::int64_t> ignoredCount { 0 };
    };

    //==========================================================================
    /*  THE BACKUP'S SOCKETS: one WebSocket client per address of its main, on
        a thread of its own that never is the tick thread - MountListener's
        shape - each reading on an io thread of its own. On open it says hello
        with what the receiver last held; it pings every tenth of a second;
        what comes back goes to the receiver. A path that drops, or never
        opens, is dialled again after a quarter of a second, then a half, one,
        and two from then on: a takeover wants a quick redial, and a main that
        is not there costs nothing to try again.

        The tick thread only says which addresses: the peers in force when this
        machine is a backup, none otherwise (`want`). */
    class Dialler
    {
    public:
        Dialler (Receiver& receiverToFeed, std::string machineName);
        ~Dialler();

        Dialler (const Dialler&) = delete;
        Dialler& operator= (const Dialler&) = delete;

        bool start();
        void stop();
        bool isRunning() const noexcept;

        /*  THE MAIN'S ADDRESSES, each `host:port` as `pair::peerAddresses`
            writes them - an IPv6 host in brackets. Tick thread; the same list
            again costs a comparison, and an address left out is hung up on. */
        void want (std::vector<std::string> peers);

        /** Paths whose socket is open now. Any thread. */
        std::size_t openPaths() const;

        /*  A MESSAGE FOR THE MAIN THAT IS NOT THE DIALLER'S OWN - a command one
            of this machine's clients gave, passed on (RD.6, AFT). Queued, and
            sent by the dialler's thread down ONE open path, never all of them:
            the main has no counter to drop a second copy by, and would apply a
            GO twice. False, and nothing queued, when no path is open. Any
            thread; never waits on the network. */
        bool relay (std::vector<std::uint8_t> bytes);

        /** `host:port` read back into the two: an IPv6 host without its brackets. */
        static std::optional<std::pair<std::string, int>> hostAndPort (std::string_view peer);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
