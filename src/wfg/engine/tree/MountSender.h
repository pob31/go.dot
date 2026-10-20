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
    What leaves Go.dot for somebody else's box, and when.

    ONE FLUSH PER TICK, AT THE END OF IT, and that is the whole design. A write
    to a mounted node does not reach a socket where it happens: it is queued,
    and everything queued during a tick leaves together once the tick's commands
    have all been applied. Three things follow, and each is a reason rather than
    a consequence.

    EVERY MESSAGE BELONGING TO ONE GO STARTS LEAVING IN THE SAME FRAME (PRD
    §3.4). A cue that moves twelve parameters is twelve datagrams that a
    receiving box sees as one gesture, not a dribble spread across whatever the
    tick thread was doing. Sending at the point of the write would have made the
    spread depend on the order commands happened to arrive in.

    UP TO WHAT THE DEVICE TAKES IN A FRAME (namespace draft §88, AUT, the
    author's): `mount/perTick` datagrams a flush, 20 unless somebody changed it,
    and what is past it waits in its place in the order for the flushes after.
    A spatial processor's snapshot of seventy inputs was 5,300 datagrams in one
    frame, and the processor took 256 of them. A bundle is one datagram.

    A NODE WRITTEN FORTY TIMES IN A TICK SENDS ONCE. The queue is keyed by
    address and the last value wins, which is the same coalescing the OSCQuery
    push side already gets from reading a published snapshot. A fade running at
    fifty a second and a client dragging a fader at four hundred both come out
    at the tick rate, and neither can flood a console.

    AND A NODE IS NOT SENT FASTER THAN ITS MOUNT ALLOWS. `mount/@rateCap` is a
    per-node ceiling in hertz, and below fifty it means an address waits: the
    newest value for it stays queued, in its place in the order, until enough
    flushes have passed. A message that waits is not a message that failed - its
    ticket stays pending and a `sent` wait keeps waiting - because what the
    caller asked for is that the value reach the target, and it will.

    THE SYSCALL IS BOUNDED AND IN ONE PLACE. `sendto` blocks; PRD §4.2's
    lipogram is about the audio thread and says nothing about this one, but an
    unbounded loop of syscalls anywhere near the tick is still how a 50 Hz clock
    stops being 50 Hz. One flush, one pass over a queue whose length is the
    number of distinct addresses written, and nothing hidden inside a command
    handler.

    WHAT IT DOES NOT DO. It does not retry, because UDP has no notion of a
    delivery to retry and a resend of a stale value is worse than a gap. It
    bundles only for a device whose row says it reads bundles (namespace draft
    45, YQ): one message per datagram otherwise, because bundle support is
    uneven in the field.

    WHAT BECAME OF A RUN'S MESSAGES IS COUNTED BY RUN (namespace draft §88,
    AVC): waiting, sent, failed, dropped - so a cue of thousands of messages is
    answered by four numbers, where a ticket at a time from the last 512 answers
    outlived the cue it was asked for.

    AND A DOUBLE ESC EMPTIES IT (2026-10-02, namespace draft §23.10). PRD §4.4's
    immediate level "drops all actions", and a value a rate cap is holding back
    is an action still to come: left queued, it went out on its turn, seconds
    after the press that promised nothing more would. `dropQueued` is that
    drop, and every ticket it drops is answered at once, so nothing waiting on
    one waits for ever.
*/

#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/Mount.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace wfg::osc
{
    class UdpEndpoint;
}

namespace wfg::midi { struct MidiSink; }
namespace wfg::pair { class Hold; }

namespace wfg::tree
{
    class MountSender
    {
    public:
        /** Where one target is. Copied from the mount's declaration at queue
            time, so a reload cannot move a message that is already in flight. */
        struct Destination
        {
            std::string host = "127.0.0.1";
            int port = 0;

            /*  HOW OFTEN ONE OF THIS MOUNT'S NODES MAY BE SENT, in hertz, from
                `mount/@rateCap`. Copied at queue time for the same reason the
                host and the port are: a reload must not change where a message
                already in flight goes, or how fast it was allowed to go.

                PER NODE AND NOT PER MOUNT, which is what the row says and is
                the only reading that leaves §3.4 intact: a cue that moves
                twelve parameters is twelve DIFFERENT addresses and they all
                leave in the same frame, as one gesture. What a cap limits is
                the same address being sent again - a fade running at fifty a
                second into a desk that wants twenty.

                Nought is no cap, not "never": the range starts at nought and a
                target that may never be written is a target nobody would mount.
                Fifty is the default and is exactly the tick rate, so the
                ordinary mount is capped by the queue's own coalescing and this
                changes nothing for it. */
            double rateCap = 0.0;

            /*  WHETHER WHAT A FLUSH SENDS THIS DEVICE LEAVES AS ONE BUNDLE, from
                `mount/@bundles` (namespace draft 45). Copied at queue time with
                the rest, for the same reason. */
            bool bundles = false;

            /*  HOW MANY DATAGRAMS A FLUSH GIVES THIS DEVICE, from `mount/perTick`
                (namespace draft §88, AUT): what is past it waits, in its place
                in the order, for the flushes after. Nought is no limit, as a
                rate cap's nought is no cap - a destination written by hand in a
                rig; every device's row says 20 unless somebody changed it. */
            int perTick = 0;

            /*  THE SERIAL PORT IT GOES DOWN instead of the network, when the
                device's transport is serial (namespace draft §51, PC.11): the
                datagram's bytes handed to `serialSink` to be framed in SLIP.
                Given its empty value here, as the members above are theirs, so a
                destination written as host and port alone leaves it out. */
            std::string serial {};

            /*  THE CONNECTION IT GOES DOWN instead of a datagram, when the
                device's transport is tcp (namespace draft §57, AFJ; DP.6) or
                mqtt (namespace draft §67, ANA; MQ.4): the
                mount's own identifier, which is what serve's links table is
                keyed by; the packet's bytes are handed to `linkSink` to be
                framed as the link's framing says. Empty, as `serial` is, for
                a destination that is a host and a port. */
            std::string link {};

            /*  WHAT THE BYTES ARE (namespace draft §57, AFJ; DP.7), from
                `mount/wire`: `osc`, the codec; `rcp`, a line of Yamaha's
                protocol, which never travels in a bundle. Copied at queue time
                with the rest, for the same reason. */
            std::string wire = "osc";

            /*  THE MIDI SIDE (DP.9), copied at queue time with the rest: the
                declared port a `midi` transport sends on - empty, the bytes
                go to the link or the datagram - the channel a node's shape
                counts from, and MSC's device and format. */
            std::string midiPort {};
            int midiChannel = 1;
            int mscDevice = 127;
            int mscFormat = 127;

            /*  THE PREFIX TAKEN OFF BEFORE A MESSAGE LEAVES (namespace draft 100,
                AYK): the device's own, when its `removePrefix` is on; empty
                otherwise. Copied at queue time with the rest. */
            std::string removedPrefix;
        };
        /*  THE DESTINATION A DECLARATION NAMES, made in one place (namespace
            draft §57, DP.1): host, port, rate cap, bundles, and the serial
            port when the transport is serial. Until it existed the Runner's
            cue and curve paths spelled theirs out by hand and left `serial`
            at its empty value, so an OSC cue aimed at a device on a serial
            port was sent to UDP port 0 while `node.set` to the same device
            went down the port - which is why every caller takes it from
            here now, and why a new member of Destination is filled here or
            nowhere. */
        static Destination destinationFor (const MountDeclaration& declaration);

        /*  A BUNDLE'S DATAGRAM IS CLOSED AT THIS MANY BYTES (namespace draft
            45, ZB): WFS-DIY's own ceiling for the bundles it sends, under the
            1500 of an Ethernet frame with room for the headers, so a bundle is
            never split by the network on its way - a fragment lost on Wi-Fi
            loses the whole datagram. */
        static constexpr std::size_t bundleBytes = 1200;

        /*  The socket is a reference and is not owned. In `wfg serve` it is the
            one endpoint the process has, already bound, so every outbound
            datagram carries the same source port and a receiving box sees one
            correspondent rather than a new one per message. */
        explicit MountSender (osc::UdpEndpoint& socket) noexcept : udp (&socket) {}

        /*  A sender with nowhere to send, which is a complete configuration and
            not a degraded one: `wfg replay` and `wfg tree` have no socket and
            must still queue, coalesce and report exactly as a live session did,
            because only the sound is missing. */
        MountSender() = default;

        /*  The socket, when it comes to exist after this object does. `wfg
            serve` builds its command set before it opens a port, because the
            OSCQuery namespace needs the port number and the port needs the
            handler that the namespace provides - so the endpoint is the LAST
            thing constructed, and a sender captured into a command handler
            before it cannot have been given one yet. */
        void setSocket (osc::UdpEndpoint& socket) noexcept { udp = &socket; }

        /*  WHERE A DEVICE ON A SERIAL PORT IS SENT (PC.11): the port's id and
            the packet's bytes; false when the port could not take it. Unset - a
            replay, a rig - and such a message fails as one to nowhere does. */
        using SerialSink = std::function<bool (const std::string& serialId, const std::vector<std::uint8_t>& packet)>;
        void setSerialSink (SerialSink sink) { serialSink = std::move (sink); }

        /*  WHERE A DEVICE OVER A CONNECTION IS SENT (DP.6): the mount's id and
            the packet's bytes; false when the link could not take it - not
            open, or its queue full. Unset, as the serial sink, such a message
            fails as one to nowhere does. */
        using LinkSink = std::function<bool (const std::string& mountId, const std::vector<std::uint8_t>& packet)>;
        void setLinkSink (LinkSink sink) { linkSink = std::move (sink); }

        /*  THE MOUNT TABLE, for a wire that renders by the node's own spelling
            (the rcp wire's verb and indexes, DP.7): read at the flush, on the
            tick thread that owns both. Unset - a rig with no table - and the
            wire infers what it can from the address. */
        void setMounts (const MountTable* table) noexcept { mounts = table; }

        /*  WHERE A DEVICE ON A MIDI PORT IS SENT (DP.9): the show's MIDI
            sender, each rendered message through `sendForRun` in the run's
            name, so a double Esc drops what has not left. Unset - a replay, a
            rig - and such a message fails as one to nowhere does. */
        void setMidiSink (midi::MidiSink* sink) noexcept { midiSink = sink; }

        /*  THE MACHINE'S HOLD (namespace draft §58, RD.1; AGG): while it holds,
            nothing queued here reaches a socket, a serial port, a connection or
            a MIDI port, and every message is answered `sent` all the same - in
            `deliver`, the one funnel, before a sink is even chosen, so a cue
            aimed at a closed link or a port of nought still ends as it would
            have where the bytes left. The queue, the coalescing, the rate cap,
            the bundles and the tickets all run as ever: a held flush is a
            flush whose last step did nothing. Null - every rig, a replay - is
            never held. */
        void setHold (const pair::Hold* holdToUse) noexcept { hold = holdToUse; }

        //======================================================================
        /*  Queues one message. Tick thread.

            Returns a ticket that names this message for the rest of its short
            life. `outcomeOf` answers with it after the flush, which is what
            lets a cue whose wait is `sent` report what actually happened rather
            than what was asked for.

            `owner` is the run that wrote it, empty for a write nobody's run
            made - a client's `node.set`, a restore. Only `dropQueued` reads it,
            and a re-written address takes its newest writer's. */
        std::uint64_t queue (const std::string& mountId, const Destination&,
                             const std::string& address, const osc::Values&,
                             const std::string& owner = {});

        /*  Asks the device what a node holds (DP.10): a question in the queue
            under its own key, so a write of the same address this tick is
            neither replaced nor delayed by it. The answer comes back as the
            device's own report, heard, and serve turns it into the read-back
            the asker waits on. */
        std::uint64_t queueQuery (const std::string& mountId, const Destination&, const std::string& address);

        /** One value: a node of one argument. */
        std::uint64_t queue (const std::string& mountId, const Destination& destination,
                             const std::string& address, const osc::Value& value,
                             const std::string& owner = {})
        {
            return queue (mountId, destination, address, osc::Values { value }, owner);
        }

        /*  Sends everything queued and empties the queue. Tick thread, once per
            tick, AFTER the tick's commands have been applied - anything else
            sends a tick's writes in the middle of the tick that made them. */
        void flush();

        /*  DROPS WHAT IS STILL WAITING: a double Esc's half of this class (PRD
            §4.4, "drops all actions"). Tick thread, in the press's own drain,
            so the flush that ends that tick has nothing of it to send - and
            that holds while the clock is down too, where the flush still runs
            and no hook does.

            Every message goes but the ones `keep` names by their owner - what
            the press leaves ready (the standby's pre-sends) - kept in their
            order. Each dropped ticket is answered `failed` at once, so a `sent`
            wait never hangs on it, and `wasDropped` says why. The rate cap's
            clock is left as it was: the cap is the device's tolerance, so a
            value written after the press still waits its turn, counted from
            the last value that really went. Returns how many were dropped. */
        std::size_t dropQueued (const std::function<bool (const std::string& owner)>& keep = {});

        //======================================================================
        enum class Outcome { pending, sent, failed };

        /** What became of one queued message. */
        Outcome outcomeOf (std::uint64_t ticket) const;

        /*  Whether that ticket was dropped by `dropQueued` - answered `failed`
            there, but never sent and never tried: nothing went wrong on the
            wire. False for a ticket this object no longer remembers. */
        bool wasDropped (std::uint64_t ticket) const;

        /** How many messages have left for a mount since the show opened. */
        std::size_t sentFor (const std::string& mountId) const;

        /*  Whether this ticket is still waiting for a flush that will take it.

            A rate cap makes `pending` a legitimate answer rather than a wiring
            fault: the message is queued, in order, holding the newest value,
            and it will go. A cue whose wait is `sent` asks this before deciding
            it never left. */
        bool stillQueued (std::uint64_t ticket) const;

        /** How many are waiting for the next flush. */
        std::size_t pending() const noexcept { return queued.size(); }

        /*  WHAT BECAME OF ONE RUN'S MESSAGES (namespace draft §88, AVC), by the
            owner `queue` was given: how many still wait in the queue, and how
            many have been sent, failed or been dropped by a double Esc. A
            message a later write superseded is counted sent, as its ticket is
            answered. A run that queued nothing reads all nought. */
        struct Tally
        {
            std::size_t waiting = 0;
            std::size_t sent = 0;
            std::size_t failed = 0;
            std::size_t dropped = 0;

            /*  OF THOSE WAITING, HOW MANY THE LAST FLUSH HELD BACK BECAUSE THEIR
                DEVICE HAD BEEN GIVEN ITS `perTick` (namespace draft §88, AUT):
                a cue's own messages queued behind each other, which keep the cue
                playing until they have gone. A value a rate cap holds is not one
                of these - it is a device's tolerance for the same address written
                again, and the next write supersedes it - and keeps nobody
                playing that did not wait for it before. */
            std::size_t behind = 0;

            std::size_t settled() const noexcept { return sent + failed + dropped; }
        };

        Tally tallyOf (const std::string& owner) const;

        /*  Forgets a run's count, when nothing of it waits: the Runner's, once
            the run is over. One still waiting is kept, so its count goes on. */
        void forgetTally (const std::string& owner);

    private:
        struct Message
        {
            std::uint64_t ticket = 0;
            std::string mountId;
            Destination destination;
            std::string address;
            osc::Values values;     // every argument of the message (§45)
            std::string owner;

            /*  A QUESTION, NOT A WRITE (namespace draft §57, AFL; DP.10): the
                address asked about, rendered as the device's wire asks - the
                bare address or the file's GET template on the osc wire, a
                `get` line on the rcp wire - and kept apart from a write of the
                same address in the queue, the cap and the count. */
            bool query = false;
        };

        /*  One message as a datagram of its own, and one device's messages of
            a flush as bundles (namespace draft 45). Each answers its tickets.
            The bundles stop at `datagramsAllowed` (namespace draft §88): how
            many of `messages` went, from the front, and in how many datagrams. */
        void sendAlone (const Message& message);
        static std::string onTheWire (const Message& message);

        struct Bundled
        {
            std::size_t messages = 0;
            int datagrams = 0;
        };

        Bundled sendBundled (const std::vector<const Message*>& messages, int datagramsAllowed);

        /*  A ticket answered, and its run's count moved with it - the one place
            both happen, so the two cannot disagree. */
        void answer (const Message& message, Outcome outcome, bool dropped);

        osc::UdpEndpoint* udp = nullptr;
        SerialSink serialSink;
        LinkSink linkSink;
        const MountTable* mounts = nullptr;
        midi::MidiSink* midiSink = nullptr;
        const pair::Hold* hold = nullptr;

        /*  One datagram's bytes to where the destination says: its serial port,
            or its host and port - or nowhere, and true, while the machine
            holds. */
        bool deliver (const Destination& to, const std::vector<std::uint8_t>& bytes);

        std::vector<Message> queued;          // in first-queued order

        /*  Each run's count, by its owner; a message nobody's run wrote (an
            empty owner) is counted nowhere. */
        std::map<std::string, Tally> tallies;
        std::map<std::string, std::size_t> queuedAt;   // address -> index in queued
        std::map<std::string, std::size_t> sent;       // mount id -> count

        /*  One ticket's answer: what became of it, and whether a double Esc
            dropped it rather than a flush trying it. */
        struct Answer
        {
            std::uint64_t ticket = 0;
            Outcome outcome = Outcome::pending;
            bool dropped = false;
        };

        /*  What the last few flushes did, newest last. Bounded because it is a
            diagnostic and a handful of cues' worth of answers, never a log: the
            log is the log, and §3.15 keeps per-message readouts out of it. */
        std::deque<Answer> outcomes;

        /** The answers beyond what is kept, oldest first, let go. */
        void forgetOldAnswers();

        std::uint64_t nextTicket = 1;

        /*  Which flush each address last went out on, and how many flushes
            there have been. A flush is a tick, so the two are the same clock
            counted where it is used.

            It grows with the number of distinct addresses a show writes, which
            is bounded by the show; it is not bounded by anything here, and if
            that ever matters the fix is to forget an address the queue has not
            seen for a while rather than to cap the map. */
        std::map<std::string, std::uint64_t> lastSentAt;
        std::uint64_t flushes = 0;

        static constexpr std::size_t outcomesKept = 512;
    };

    /*  THE DOOR A `node.set` ON A DEVICE'S ADDRESS GOES THROUGH in `wfg serve`
        (PRD §4.11): the value lands in the mount table - what a client reads
        back and what a replay reproduces - and is queued for the end of the
        tick, which is what the other box hears. One function rather than a
        lambda written at each assembly site, so the rigs that stand for
        `serve` take the door `serve` has.

        NOTHING IS QUEUED FOR A DEVICE SWITCHED OFF (2026-10-03, Doh! D3,
        namespace draft §24.13), as a cue's own write was never queued for one
        (`writeOscNow`): its `tx` says it is not in the room tonight. The tree
        still takes the value. Before this a client's write, a scene's restore
        and Doh!'s put-back all went out on the wire to a device the operator
        had switched off. */
    MountTable::WriteResult writeToDevice (MountTable& mounts, MountSender& sender,
                                           const std::string& address, const osc::Values& values);
}
