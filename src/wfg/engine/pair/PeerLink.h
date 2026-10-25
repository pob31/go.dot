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

    FROM RD.7 THE LINK IS SYMMETRIC OVER ONE CONNECTION. The backup still
    dials and the main still listens - the socket's direction is the role's -
    but either end may lead: a backup that took over keeps dialling its main,
    and a main started again after a crash follows the backup that sends. So
    "backup -> main" above reads "follower -> leader" and "main -> backup"
    reads "leader -> follower": the leader's `Stream` sends down whichever
    connection its follower is at the end of - the server's or the
    dialler's - and the follower's `Receiver` reads from either. What RD.7
    adds to the wire:

      either way
        /godot/pair/hello   s:machine s:role h:lastCounter s:stream
                            s:state s:ownStream i:fresh h:liveRuns h:salt
                            what a machine is, said by both ends on a
                            connection's first message and whenever it
                            changes: the first four are RD.5's - what it
                            holds of the stream it follows - then its state
                            (`following` and `bothSending` ask for the
                            other's stream), its own stream's identity,
                            whether it has applied no GO and run no cue since
                            it started (AGA), how many cues it has running,
                            and its run-name salt (RD.2), which a machine
                            that starts following having run nothing adopts.
                            A hello of RD.5's four is a follower's when its
                            role is `backup`, and says nothing else.
        /godot/pair/order   s:order s:reason
                            an operator's choice on the other machine:
                            `hold` (it took over, or kept the show - hold and
                            follow it) or `lead` (it handed the show over).
      follower -> leader
        /godot/pair/ping    h:sent f:roundTrip h:tick     the pinging machine's
                            tick, so the other end knows its engine is alive
                            and not only its socket
        /godot/pair/relay   s:line h:ticket               the ticket the
                            leader carries back on the record (AGJ)
      leader -> follower
        /godot/pair/record  h:counter s:line h:ticket     the ticket of the
                            relay it answers; left off when there is none
        /godot/pair/beat    h:tick h:lastCounter h:liveRuns
                            how many cues run on the leader, for a follower
                            that joined while some did and is not in step

    AND WHAT RD.8 ADDS, so a follower is seated where its leader is and says
    whether it stays there (pair/Position.h, pair/Summary.h):

      follower -> leader
        /godot/pair/seat                                  send me where you
                            are: `pair.realign`, asked on the follower. A
                            follow that starts asks by its hello, from now
                            (RD.7), and a resume the ring no longer holds is
                            seated as well - neither needs saying.
      leader -> follower
        /godot/pair/position h:counter h:tick i:part i:parts s:text
                            the leader's position, true at the end of `tick`
                            after its stream's record `counter`, as text in
                            the log's own grammar; in parts of whole lines a
                            few kilobytes each, all in the one bundle of the
                            tick that sends it, after the records up to the
                            counter and before the beat - so a follower
                            seated at it applies what follows on top, in
                            order. Sent only once that follower has been sent
                            every record up to the counter.
        /godot/pair/summary h:tick h:counter i:lists [s:list s:cue]...
                            [s:run s:cue s:state h:playhead i:range]...
                            every fifth tick, what an operator would look at:
                            each list's standby and each live run.

    AND WHAT RD.10 ADDS, so the two machines are known to play the same show
    (pair/ShowCheck.h; the author's AFU):

      either way
        /godot/pair/hello   ... s:showFingerprint s:mediaDigest
                            the nine, and this machine's show fingerprint and
                            media digest as it last worked them out
      leader -> follower
        /godot/pair/summary ... s:showFingerprint s:mediaDigest
                            the same two after the runs, each empty while the
                            leader's are not of the show it has now
      follower -> leader, answered down the same connection
        /godot/pair/showAsk i:what      1: the show, 2: the media list, 3: both
        /godot/pair/showText s:fingerprint i:part i:parts s:text
                            the leader's show text with no machine row in it,
                            in parts of whole lines
        /godot/pair/showFile s:fingerprint s:name b:bytes
                            each of its device descriptions (namespaces/)
        /godot/pair/showUnits s:fingerprint i:units
                            [s:key s:element s:hash s:order s:label]...
                            its units, after its text and descriptions, so a
                            follower that has these has the rest
        /godot/pair/showMedia s:digest i:entries
                            [s:path s:kind i:present h:size h:modified s:hash]...
                            every file its show names

    AND WHAT RD.9 ADDS, so the two start a sound on the same sample (pair/
    ClockMap.h; the author's AFS):

      the machine that listens -> the one that pings
        /godot/pair/pong    h:sent h:tick h:received h:left
                            RD.5's two, and the answering machine's steady
                            clock when the ping arrived and when the pong left -
                            NTP's t1 and t2, so the pinger has all four. A pong
                            of two still reads, and gives a round trip and no
                            offset.
      either way
        /godot/pair/clock   h:tick i:samplesPerTick h:tickZero i:leadTicks
                            i:onTicks i:extraTicks i:delay d:outputLatency
                            i:lineValid i:rate h:sample h:nanos d:nanosPerSample
                            d:lineError d:lineSeconds
                            i:hostsValid h:at d:offset d:slope d:hostsError
                            d:bestTrip i:exchanges
                            what a machine says of its clocks (`ClockSaid`): the
                            leader's in its tick's bundle - its tick grid, the
                            lead its launches are placed at from that tick on,
                            its sound line, its output latency (AGQ), and, when
                            it dials, the follower's host clock against its own
                            - every half second and whenever the lead moves; the
                            follower's to the leader down the path relays take,
                            every half second and when Δ moves - Δ, which is
                            what the leader adds to its launch lead (AFS), and
                            its own output latency.

    AND WHAT RD.12 ADDS, so the machine that leads knows what only its
    follower can judge (pair/Leadership.h `FollowerVerdict`):

      follower -> leader, down the path relays take
        /godot/pair/verdict i:inStep s:differs i:showMatch s:showDiffers s:seat
                            whether it is in step and whether its show is the
                            leader's - 1 yes, 0 no, -1 not known - each with
                            its sentence, clipped, and its seat; every half
                            second and on the tick any of it changes. A
                            message of its own rather than five more arguments
                            on the clock's twenty-two: a build before RD.12
                            ignores an address it does not know, and would
                            refuse a clock of another length whole.
*/

#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/osc/OscCodec.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/pair/ClockMap.h>
#include <wfg/engine/pair/Leadership.h>
#include <wfg/engine/pair/LinkReading.h>
#include <wfg/engine/pair/Summary.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
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
        inline constexpr const char* order   = "/godot/pair/order";

        //  RD.8's.
        inline constexpr const char* seat     = "/godot/pair/seat";
        inline constexpr const char* position = "/godot/pair/position";
        inline constexpr const char* summary  = "/godot/pair/summary";

        //  RD.10's: the same show, checked (pair/ShowCheck.h).
        inline constexpr const char* showAsk   = "/godot/pair/showAsk";
        inline constexpr const char* showText  = "/godot/pair/showText";
        inline constexpr const char* showFile  = "/godot/pair/showFile";
        inline constexpr const char* showUnits = "/godot/pair/showUnits";
        inline constexpr const char* showMedia = "/godot/pair/showMedia";

        //  RD.9's: what a machine says of its clocks (pair/ClockMap.h).
        inline constexpr const char* clockLine = "/godot/pair/clock";

        //  RD.12's: what a follower says of itself (pair/Leadership.h `FollowerVerdict`).
        inline constexpr const char* verdict = "/godot/pair/verdict";

        /*  HOW LONG ONE PART OF A POSITION MAY BE, in bytes of text: a
            position is cut into parts of whole lines no longer than this -
            one line longer goes alone - so a show with hundreds of runs going
            is several messages rather than one string the size of all of
            them. The bundle that carries them is still one frame. */
        inline constexpr std::size_t positionPartBytes = 16 * 1024;

        /*  A POSITION'S TEXT CUT INTO ITS PARTS, and the messages that carry
            them: `/godot/pair/position h:counter h:tick i:part i:parts s:text`. */
        std::vector<std::vector<std::uint8_t>> positionMessages (std::uint64_t counter, std::int64_t tick,
                                                                 std::string_view text,
                                                                 std::size_t partBytes = positionPartBytes);

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
            `out` as it was; a ticket of nought is left off the message. */
        bool writeRecord (std::vector<std::uint8_t>& out, std::uint64_t counter, std::string_view line,
                          std::uint64_t ticket = 0);
        void writeBeat (std::vector<std::uint8_t>& out, std::int64_t tick, std::uint64_t lastCounter,
                        std::int64_t liveRuns = 0);

        /*  WHERE A MESSAGE OF THE LINK GOES ON THE MACHINE IT REACHES (RD.7):
            to the leader's half (`Stream`) - a hello, a ping, a relay - or to
            the follower's (`Receiver`) - a bundle of a tick's stream, a
            welcome, a gap, a record, a beat, a pong - whichever socket brought
            it. A hello goes to the leader's half AND says what the other
            machine is; an order is neither half's (pair/Presence.h). A
            question about the show or its answer (RD.10) is the show check's
            (pair/ShowCheck.h), whichever machine leads. */
        enum class Bound { leader, follower, neither, show };
        Bound boundFor (const osc::Packet& message) noexcept;

        //======================================================================
        /*  WHAT A MACHINE SAYS IT IS (RD.7): the hello's nine arguments, read
            and written in one place - eleven from RD.10, its show's
            fingerprint and its media's digest after them. The first four are
            RD.5's and a hello of only those is still read - `described` false,
            its role saying whether it follows - and so is one of nine. */
        struct Hello
        {
            std::string machine;
            std::string role;
            std::uint64_t lastCounter = 0;      // what it holds of the stream it follows...
            std::string stream;                 // ...and which stream that is
            std::string state;                  // `pair/state`'s word
            std::string ownStream;              // the identity of its own stream, which it leads with
            bool fresh = false;                 // no GO applied, no cue run, since it started (AGA)
            std::int64_t liveRuns = 0;          // cues running on it
            std::uint64_t salt = 0;             // its run-name salt (RD.2)
            bool described = false;             // all nine: an RD.7 machine's

            /*  ITS SHOW (RD.10, AFU): its fingerprint and its media list's
                digest as it last worked them out, each empty while it has not -
                carried at link-up, as RD.10 has it; what decides is the
                leader's summaries (pair/Summary.h), which carry the same two.
                Two more arguments after the nine; a hello of nine says none. */
            std::string showFingerprint;
            std::string mediaDigest;

            /*  Whether it asks for the stream of the machine it says hello to:
                following, or both sending (each applies the other's); RD.5's
                hello, when it says `backup`. */
            bool wantsStream() const noexcept;
        };

        std::vector<std::uint8_t> helloBytes (const Hello& hello);
        std::optional<Hello> readHello (const osc::Packet& message);

        //======================================================================
        /*  WHAT A MACHINE SAYS OF ITS CLOCKS (RD.9, `/godot/pair/clock`): its
            tick grid on its own count, the lead it places a launch at ahead
            of the tick that decided it, from `tick` on - and whether those
            launches sit on that grid, as a leader's do while it has a
            follower, rather than on its count as it stood - the extra lead it
            adds (AFS), Δ as it measured it following, its interface's
            reported output delay (AGQ), its sound line, and - from a machine
            that dials, which alone pings - the other machine's host clock
            against its own. */
        struct ClockSaid
        {
            std::int64_t tick = 0;
            int samplesPerTick = 0;
            std::int64_t tickZero = 0;          // where its tick nought sits on its audio side's count
            int leadTicks = 0;                  // the launch latency and the extra lead
            bool onTicks = false;
            int extraTicks = 0;
            int delay = 0;
            double outputLatency = 0.0;         // seconds
            SoundLine line;
            OffsetLine hosts;                   // the OTHER machine's host clock less this one's, in this one's time
        };

        std::vector<std::uint8_t> clockBytes (const ClockSaid& said);
        std::optional<ClockSaid> readClock (const osc::Packet& message);

        //======================================================================
        /*  WHAT A FOLLOWER SAYS OF ITSELF (RD.12, `/godot/pair/verdict`): its
            verdicts and their sentences, and its seat. A sentence names cues,
            and a cue's name is whatever somebody typed, so each is CLIPPED -
            written and read - to `verdictTextBytes`, cut on a character's first
            byte and ended "...": what reaches the leader's rows and its alarm
            is bounded whatever the follower's build sends. Strings and integers
            only, so a French machine writes and reads it as a C one does. */
        inline constexpr std::size_t verdictTextBytes = 512;

        /** A sentence no longer than `most` bytes: cut on a character's first byte and ended "..." when it was longer. */
        std::string clipped (std::string sentence, std::size_t most = verdictTextBytes);

        std::vector<std::uint8_t> verdictBytes (const FollowerVerdict& said);
        std::optional<FollowerVerdict> readVerdict (const osc::Packet& message);

        /*  HOW OFTEN A FOLLOWER SAYS IT, in ticks - half a second - besides the
            tick any of it changes; and HOW OLD A LEADER LETS IT BE before it
            reads as not known: the link's timeout, but never less than twice
            the interval, so a timeout set under a second does not read every
            verdict stale between two of them. */
        inline constexpr std::int64_t verdictEveryTicks = 25;
        inline constexpr double verdictLastsAtLeastSeconds = 1.0;

        /*  A follow that starts now asks for what the leader keeps from now
            on, never what it kept before (RD.7): this counter, on the leader's
            own stream, reads as "after your last". */
        inline constexpr std::uint64_t fromNow = static_cast<std::uint64_t> (std::numeric_limits<std::int64_t>::max());
    }

    /*  THE `pair/peer*` ROWS OF THE MACHINE FOLLOWED (RD.12), from what its
        follower last said - `Stream::followerVerdict`, null for nothing known:
        `unknown` and empty then. Each copied only when it moved, as serve's
        rows are. Serve's, and a test's. */
    void verdictRowsInto (FollowReading& rows, const FollowerVerdict* said);

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
            message is put in the ring - the one thing GO's path gains. A
            relay's record carries its ticket beside the line (RD.7, AGJ). */
        void keep (const LogRecord& record, const Command* command);

        /*  HOW MANY CUES RUN HERE, said in every beat (RD.7): a machine that
            starts following while some run is not in step with them until
            RD.8 seats it, and says so. The tick thread's, before `afterTick`. */
        void setLiveRuns (std::int64_t count) noexcept { liveRuns = count; }

        /*  WHERE THIS MACHINE IS, AS TEXT (RD.8, pair/Position.h): made on the
            tick thread, in `afterTick`, at most once a tick and only when a
            follower is owed one - a follow that has just started, a resume the
            ring could not answer, a `/godot/pair/seat` - and true at the end of
            this tick, after the stream's last counter. Set before `afterTick`
            first runs; none, and no follower is ever seated. */
        using PositionMaker = std::function<std::string (std::int64_t tick, std::uint64_t counter)>;
        void setPositionMaker (PositionMaker make) { makePosition = std::move (make); }

        /*  WHAT AN OPERATOR WOULD LOOK AT, every `summaryEveryTicks` ticks to
            every follower (pair/Summary.h): the message's bytes, made once a
            tick it is due. None: no summary. */
        using SummaryMaker = std::function<std::vector<std::uint8_t> (std::int64_t tick, std::uint64_t counter)>;
        void setSummaryMaker (SummaryMaker make) { makeSummary = std::move (make); }

        /** Positions sent, to any follower: what a test and a black box count. */
        std::int64_t positionsSent() const noexcept { return positionCount; }

        /*  WHAT THIS MACHINE SAYS OF ITS CLOCKS (RD.9, `wire::clockBytes`):
            carried in this tick's bundle to every follower, after the records
            and before the summary and the beat, and in the first bundle of
            every follower welcomed until the next is said - so a follower has
            the leader's lead in the stream's order, and from its first tick.
            The tick thread's, before `afterTick`. */
        void sayClock (std::vector<std::uint8_t> message);

        /*  WHAT A FOLLOWER LAST SAID OF ITS CLOCKS (RD.9): its Δ and its
            output latency, taken in `afterTick`; nothing before one has
            spoken. The tick thread's. */
        const std::optional<wire::ClockSaid>& followerClock() const noexcept { return heardClock; }

        /*  WHAT A FOLLOWER LAST SAID OF ITSELF (RD.12, `wire::readVerdict`),
            taken in `afterTick`: null - NOT KNOWN, never stale - unless this
            machine leads, the connection it came down still follows, its
            follower said it was following, and it is no older than the
            timeout (and `wire::verdictLastsAtLeastSeconds`). The tick thread's. */
        const FollowerVerdict* followerVerdict (LinkClock::time_point now, double timeoutSeconds) const;

        /*  THE HEARTBEAT, from serve's after-tick: what arrived since the last
            tick is read, then each follower is sent what it has not had - its
            welcome and any gap first, then the records, `backlogPerTick` at
            most - and the beat, which says this tick and the last counter
            kept. A hung engine sends no beat, which is what lets a backup
            tell a hung main from a slow one (AFV). `timeoutSeconds` decides
            whether a hello starts a new link (the counters start again) or
            joins the one that is up, by when the hello arrived. */
        void afterTick (std::int64_t tick, double timeoutSeconds);

        /*  THIS MACHINE'S PASS COUNT (TickThread::passCount), said in every
            pong so the other machine knows the engine behind the socket is
            alive - and still alive while the show is held, its tick standing
            (PRD §3.34). The tick thread's, every pass; a relaxed store. */
        void setPass (std::int64_t pass) noexcept { passNow.store (pass, std::memory_order_relaxed); }

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
            enum class Kind { hello, ping, closed, seat, clock, verdict };

            Kind kind = Kind::hello;
            std::string origin;
            std::string machine;
            std::string role;
            std::string stream;
            std::uint64_t lastCounter = 0;
            bool wants = false;                 // a hello asking for this stream (RD.7)
            bool described = false;             // an RD.7 hello, which says what the machine is
            double roundTrip = 0.0;
            LinkClock::time_point at {};
            wire::ClockSaid clock;              // a follower's word on its clocks (RD.9)
            FollowerVerdict verdict;            // and on itself (RD.12)
        };

        struct Follower
        {
            std::string machine;
            std::uint64_t sentUpTo = 0;         // the last counter sent down this connection
            std::uint64_t next = 0;             // the first counter its welcome promised
            bool welcomed = false;
            std::uint64_t gapOldest = 0;        // non-zero: a `gap` still to be said
            bool owesPosition = false;          // to be sent where this machine is (RD.8)
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
        PositionMaker makePosition;
        SummaryMaker makeSummary;
        std::int64_t positionCount = 0;

        //  RD.9's: this machine's word on its clocks, whether this tick says it, and a follower's last.
        std::vector<std::uint8_t> clockMessage;
        bool clockFresh = false;
        std::optional<wire::ClockSaid> heardClock;

        //  RD.12's: a follower's last word on itself, when it came and down which connection.
        std::optional<FollowerVerdict> heardVerdict;
        LinkClock::time_point verdictAt {};
        std::string verdictFrom;

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
        std::int64_t liveRuns = 0;

        //  Both.
        std::mutex inboxLock;
        std::vector<Arrival> inbox;
        std::atomic<std::int64_t> passNow { 0 };
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

        /*  WHERE A HELLO ASKS THE LEADER TO START (RD.7): after the last record
            drained, on the stream it was of - a path dialled again resumes
            exactly - or, once `startAfresh` has been called and until the
            leader's welcome arrives, "from now" on the leader's own stream. */
        std::pair<std::uint64_t, std::string> resumePoint() const;

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

        /** How many cues ran on the leader, as its last heartbeat said (RD.7). */
        std::int64_t leaderLiveRuns() const noexcept { return beatRuns; }

        /*  THE LEADER'S POSITION, once its parts are all in, in the stream's
            order - after the records up to its counter, before those after
            (RD.8): the text, the counter and the leader's tick it is true at.
            One that a second path brings again is not handed on. Tick thread. */
        using OnPosition = std::function<void (std::uint64_t counter, std::int64_t leaderTick, std::string text)>;
        void setOnPosition (OnPosition callback) { onPosition = std::move (callback); }

        /** Each summary the leader sends, once (RD.8). Tick thread. */
        using OnSummary = std::function<void (Summary summary)>;
        void setOnSummary (OnSummary callback) { onSummary = std::move (callback); }

        /*  WHAT THE LEADER SAYS OF ITS CLOCKS (RD.9), in the stream's order,
            each as it is drained - a second path's copy too, which says the
            same. Tick thread. */
        using OnClock = std::function<void (const wire::ClockSaid& said)>;
        void setOnClock (OnClock callback) { onClock = std::move (callback); }

        /*  EACH PONG'S FOUR STAMPS (RD.9; NTP's): the ping's departure and the
            pong's arrival on this machine's clock, the ping's arrival and the
            pong's departure on the other's - a pong of RD.5's two carries no
            offset and is not handed on. Whoever leads: only a machine that
            dials pings, and what it measures is the other's clock either way.
            Tick thread. */
        using OnHostSample = std::function<void (const HostSample& sample)>;
        void setOnHostSample (OnHostSample callback) { onHostSample = std::move (callback); }

        /*  A FOLLOW STARTS NOW, on the leader whose stream is `leaderStream`
            (RD.7): what this receiver held of any stream is forgotten, the
            next hello asks for what the leader keeps from now on
            (`wire::fromNow`), and the welcome that answers is where it starts -
            no gap for the records before it, which this machine never asked
            for. Tick thread. */
        void startAfresh (const std::string& leaderStream);

    private:
        struct Item
        {
            enum class Kind { welcome, gap, record, beat, pong, position, summary, clock };

            Kind kind = Kind::beat;
            std::string path;
            LinkClock::time_point at {};
            std::int64_t first = 0, second = 0, third = 0, fourth = 0;
            std::string stream;                 // a welcome's stream; a position part's text
            LogRecord record;
            Summary summary;
            wire::ClockSaid clock;              // the leader's word on its clocks (RD.9)
        };

        /*  A POSITION COMING IN PARTS, by the path bringing it: what has come,
            and which part is next. */
        struct Partial
        {
            std::uint64_t counter = 0;
            std::int64_t tick = 0;
            std::int64_t parts = 0, next = 0;
            std::string text;
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
        std::int64_t beatRuns = 0;
        bool afresh = false;                    // the next welcome is where this follow starts
        std::string peer;
        std::int64_t receivedThisLink = 0, gapsThisLink = 0, duplicateCount = 0;
        OnRecord onRecord;
        OnBeat onBeat;
        OnPosition onPosition;
        OnSummary onSummary;
        OnClock onClock;
        OnHostSample onHostSample;

        //  RD.8's: positions in parts by path, and the last handed on.
        std::map<std::string, Partial> partials;
        std::string positionStream;
        std::uint64_t positionCounter = 0;
        std::int64_t positionTick = 0;
        bool positionHanded = false;

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
        std::string afreshStream;               // the leader's stream a fresh follow asks of, under `lock`
        bool afreshShared = false;              // `afresh`, under `lock`
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

        //======================================================================
        /*  FROM RD.7 THE DIALLER CARRIES WHAT EITHER HALF SAYS. A backup that
            took over leads down the connections it dialled, so its stream is
            sent down a path (`sendTo`), what the main says back is handed to
            whoever routes it (`setRoute` - the receiver by default, as RD.5
            had it), and the hello a path opens with is what the machine is
            now (`setHello`), said again down every open path when that changes
            (`announce`). All set before `start`, except as said. */

        /** Who reads what a path brings: by default `Receiver::deliver`. Any thread (the path's). */
        using Route = std::function<void (const std::string& path, const osc::Packet& message,
                                          LinkClock::time_point arrival)>;
        void setRoute (Route routeToUse);

        /*  The hello a path opens with, made at that moment (it says what the
            receiver holds then); by default RD.5's, a backup's. Called on a
            path's thread. */
        using HelloMaker = std::function<std::vector<std::uint8_t>()>;
        void setHello (HelloMaker makeHello);

        /** The hello said again, made afresh, down every open path. Any thread. */
        void announce();

        /*  One message down one path - the stream of a backup that leads, to
            the main that follows it at the end of that path. False when that
            path is not open now. Any thread; queued, never waited on. */
        bool sendTo (const std::string& path, std::vector<std::uint8_t> bytes);

        /** One message down every open path: an order (RD.7). Any thread; queued. */
        void sendAll (std::vector<std::uint8_t> bytes);

        /*  THIS MACHINE'S TICK, carried in every ping so the main knows the
            engine behind the socket is alive (RD.7) - its PASS COUNT since PRD
            §3.34, which moves while a held show's tick stands. The tick
            thread's, every pass; a relaxed store. */
        void setTick (std::int64_t tick) noexcept;

        /** `host:port` read back into the two: an IPv6 host without its brackets. */
        static std::optional<std::pair<std::string, int>> hostAndPort (std::string_view peer);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
