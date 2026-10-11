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

#include <wfg/engine/pair/PeerLink.h>

#include <wfg/engine/command/Command.h>
#include <wfg/engine/command/Event.h>
#include <wfg/engine/document/Schema.h>
#include <wfg/engine/pair/MachineLocal.h>

#include <juce_simpleweb/juce_simpleweb.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <iterator>
#include <system_error>
#include <thread>

namespace wfg::pair
{
    namespace
    {
        osc::Value counterValue (std::uint64_t counter)
        {
            return osc::Value::int64 (static_cast<std::int64_t> (counter));
        }

        /** Whether `at` is no more than `timeoutSeconds` after `since`. */
        bool within (LinkClock::time_point since, LinkClock::time_point at, double timeoutSeconds)
        {
            return std::chrono::duration<double> (at - since).count() <= timeoutSeconds;
        }

        /*  A `node.setMany` that set one of this machine's rows beside the
            show's: every pair read as the command reads them, an address and a
            value, and an odd count is a message cut short that the command
            refused - it travels as it was refused, whole. */
        bool mixesMachineRows (const LogRecord& record)
        {
            if (record.command != "node.setMany" || record.args.size() % 2 != 0)
                return false;

            for (std::size_t at = 0; at < record.args.size(); at += 2)
                if (record.args[at].isString() && doc::Schema::isMachineAddress (record.args[at].getString()))
                    return true;

            return false;
        }

        LogRecord withoutMachineRows (const LogRecord& record)
        {
            LogRecord out;
            out.kind = record.kind;
            out.tick = record.tick;
            out.seq = record.seq;
            out.origin = record.origin;
            out.reason = record.reason;
            out.command = record.command;
            out.detail = record.detail;
            out.ticket = record.ticket;
            out.args.reserve (record.args.size());

            for (std::size_t at = 0; at + 1 < record.args.size(); at += 2)
            {
                if (record.args[at].isString() && doc::Schema::isMachineAddress (record.args[at].getString()))
                    continue;

                out.args.push_back (record.args[at]);
                out.args.push_back (record.args[at + 1]);
            }

            return out;
        }

        //======================================================================
        /*  THE TWO MESSAGES A TICK MAKES, WRITTEN STRAIGHT INTO THEIR BYTES.

            The codec's own encoder takes a packet of values and is the right
            thing everywhere a message is made now and then; for the two the
            tick thread makes on GO's path - a record per record kept, a beat a
            tick - it was measured at ten microseconds a message in the Debug
            build, four times the cost of making the line itself. Both have a
            fixed shape, int64s and a string, so they are written here in one
            pass into a buffer that is reused, a ring slot's or the beat's:
            no allocation once the ring has gone round. A test holds the bytes
            equal to the codec's. */
        std::size_t padded (std::size_t size) noexcept
        {
            return (size + 3) & ~std::size_t { 3 };
        }

        //  The buffer is zeroed first: a string's terminator and padding are its zeros.
        void putString (std::uint8_t*& at, std::string_view text) noexcept
        {
            std::memcpy (at, text.data(), text.size());
            at += padded (text.size() + 1);
        }

        void putInt64 (std::uint8_t*& at, std::int64_t value) noexcept
        {
            const auto bits = static_cast<std::uint64_t> (value);

            for (int i = 0; i < 8; ++i)
                at[i] = static_cast<std::uint8_t> ((bits >> (56 - 8 * i)) & 0xffu);

            at += 8;
        }

        void putUInt32 (std::uint8_t* at, std::uint32_t value) noexcept
        {
            for (int i = 0; i < 4; ++i)
                at[i] = static_cast<std::uint8_t> ((value >> (24 - 8 * i)) & 0xffu);
        }

        /*  A TICK'S MESSAGES TO ONE FOLLOWER GO AS ONE BUNDLE: one WebSocket
            frame, one send. Sent one frame a message, a tick of sixty-four
            records cost the main's tick two and a half milliseconds in the
            Debug build - every send is a queue, a lock, a timer and a socket
            write - and the beat alone was one of those every tick. The
            messages inside are the link's own, in the order they were made:
            the welcome, a gap, the records, the beat last. Time tag 1,
            "immediately", which is all a bundle has ever meant to Go.dot. */
        void startBundle (std::vector<std::uint8_t>& frame)
        {
            static constexpr std::uint8_t head[16] = { '#', 'b', 'u', 'n', 'd', 'l', 'e', 0,
                                                       0, 0, 0, 0, 0, 0, 0, 1 };
            frame.assign (std::begin (head), std::end (head));
        }

        void addToBundle (std::vector<std::uint8_t>& frame, const std::vector<std::uint8_t>& message)
        {
            if (message.empty())
                return;

            const auto at = frame.size();
            frame.resize (at + 4 + message.size());
            putUInt32 (frame.data() + at, static_cast<std::uint32_t> (message.size()));
            std::memcpy (frame.data() + at + 4, message.data(), message.size());
        }

        /** A counter as the wire carries it, an int64: nothing for a negative one. */
        std::optional<std::uint64_t> counterOf (const osc::Value& value)
        {
            if (! value.isInt64() || value.getInt64() < 0)
                return std::nullopt;

            return static_cast<std::uint64_t> (value.getInt64());
        }
    }

    //==========================================================================
    namespace wire
    {
        bool isPairAddress (std::string_view address) noexcept
        {
            return address.size() > prefix.size() && address.substr (0, prefix.size()) == prefix;
        }

        bool startsWithPairAddress (const std::uint8_t* data, std::size_t size) noexcept
        {
            return data != nullptr && size > prefix.size()
                     && std::memcmp (data, prefix.data(), prefix.size()) == 0;
        }

        std::vector<std::uint8_t> encode (const char* address, std::vector<osc::Value> args)
        {
            std::string error;
            auto bytes = osc::encode (osc::Packet::message (address, std::move (args)), error);
            return bytes.has_value() ? std::move (*bytes) : std::vector<std::uint8_t> {};
        }

        bool writeRecord (std::vector<std::uint8_t>& out, std::uint64_t counter, std::string_view line,
                          std::uint64_t ticket)
        {
            /*  A string holding a NUL cannot be one OSC string - it would end
                there - and the codec refuses one rather than cut it. A log line
                never holds one: it escapes every control character. */
            if (line.find ('\0') != std::string_view::npos)
                return false;

            constexpr std::string_view address = record;

            //  The relay's ticket after the line, when there is one (RD.7); RD.5's message otherwise.
            const std::string_view tags = ticket != 0 ? std::string_view (",hsh") : std::string_view (",hs");

            out.assign (padded (address.size() + 1) + padded (tags.size() + 1) + 8 + padded (line.size() + 1)
                          + (ticket != 0 ? 8 : 0),
                        0);

            auto* at = out.data();
            putString (at, address);
            putString (at, tags);
            putInt64 (at, static_cast<std::int64_t> (counter));
            putString (at, line);

            if (ticket != 0)
                putInt64 (at, static_cast<std::int64_t> (ticket));

            return true;
        }

        void writeBeat (std::vector<std::uint8_t>& out, std::int64_t tick, std::uint64_t lastCounter,
                        std::int64_t liveRuns)
        {
            constexpr std::string_view address = beat;
            constexpr std::string_view tags = ",hhh";

            out.assign (padded (address.size() + 1) + padded (tags.size() + 1) + 24, 0);

            auto* at = out.data();
            putString (at, address);
            putString (at, tags);
            putInt64 (at, tick);
            putInt64 (at, static_cast<std::int64_t> (lastCounter));
            putInt64 (at, liveRuns);
        }

        Bound boundFor (const osc::Packet& message) noexcept
        {
            //  A tick's stream comes as one bundle; nothing else of the link does.
            if (message.isBundle())
                return Bound::follower;

            const auto& address = message.address;

            if (address == hello || address == ping || address == relay)
                return Bound::leader;

            if (address == welcome || address == gap || address == record || address == beat || address == pong)
                return Bound::follower;

            return Bound::neither;
        }

        bool Hello::wantsStream() const noexcept
        {
            if (! described)
                return role == "backup";

            return state == "following" || state == "bothSending";
        }

        std::vector<std::uint8_t> helloBytes (const Hello& said)
        {
            return encode (hello, { osc::Value::string (said.machine), osc::Value::string (said.role),
                                    osc::Value::int64 (static_cast<std::int64_t> (std::min (said.lastCounter, fromNow))),
                                    osc::Value::string (said.stream), osc::Value::string (said.state),
                                    osc::Value::string (said.ownStream), osc::Value::int32 (said.fresh ? 1 : 0),
                                    osc::Value::int64 (said.liveRuns),
                                    osc::Value::int64 (static_cast<std::int64_t> (said.salt)) });
        }

        std::optional<Hello> readHello (const osc::Packet& message)
        {
            const auto& a = message.args;

            if (message.isBundle() || message.address != hello || (a.size() != 4 && a.size() != 9))
                return std::nullopt;

            if (! a[0].isString() || ! a[1].isString() || ! a[2].isInt64() || a[2].getInt64() < 0 || ! a[3].isString())
                return std::nullopt;

            Hello out;
            out.machine = a[0].getString();
            out.role = a[1].getString();
            out.lastCounter = static_cast<std::uint64_t> (a[2].getInt64());
            out.stream = a[3].getString();

            if (a.size() == 4)
                return out;

            if (! a[4].isString() || ! a[5].isString() || ! a[6].isInt32() || ! a[7].isInt64() || ! a[8].isInt64())
                return std::nullopt;

            out.state = a[4].getString();
            out.ownStream = a[5].getString();
            out.fresh = a[6].getInt32() != 0;
            out.liveRuns = a[7].getInt64();
            out.salt = static_cast<std::uint64_t> (a[8].getInt64());
            out.described = true;
            return out;
        }
    }

    //==========================================================================
    bool travels (const LogRecord& record, const Command* command)
    {
        if (record.kind == LogRecord::Kind::dropped)
            return false;

        /*  THE ENGINE'S OWN RECORDS ARE MADE ON EACH MACHINE (AGE): a follow-on,
            a group's members, a sound reported ended - the backup makes them
            itself from the same show at the same moment, under the same derived
            names, and a copy from the main would make each of them twice. A
            replay's are a replay's. */
        if (record.origin == origin::engine || record.origin == origin::replay)
            return false;

        /*  NOR WHAT THIS MACHINE APPLIED FROM THE OTHER'S STREAM (RD.7): a
            follower that took over leads from its own records on, and two
            machines both sending each apply the other's - sent back, every one
            would come round again for ever. */
        if (record.origin.rfind (origin::fromMain, 0) == 0)
            return false;

        /*  The pairing's own commands are the machine's (RD.4) and would be
            refused here by the line below; said by name as well, because a
            pairing command that one day is not machine-local - `pair.takeOver`
            is RD.7's - must still never be mirrored onto the other machine. */
        if (record.command.rfind ("pair.", 0) == 0)
            return false;

        return command == nullptr || ! isMachineLocal (*command, record.args);
    }

    std::optional<LogRecord> keptForLink (const LogRecord& record, const Command* command)
    {
        if (! travels (record, command))
            return std::nullopt;

        if (mixesMachineRows (record))
            return withoutMachineRows (record);

        return record;
    }

    //==========================================================================
    Stream::Stream (std::string identity, std::size_t ringSize, std::size_t perTick)
        : streamId (std::move (identity)),
          capacity (std::max<std::size_t> (1, ringSize)),
          backlog (std::max<std::size_t> (1, perTick))
    {
        ring.resize (capacity);
    }

    void Stream::keep (const LogRecord& record, const Command* command)
    {
        if (! isLeading() || ! travels (record, command))
            return;

        /*  THE MESSAGE IS MADE NOW, once, into the ring slot it will be sent
            from as often as it is asked for: the line and its framing are the
            whole of what GO's path gains, and a backup that reconnects is
            answered from bytes already made. The slot's storage is reused
            once the ring has gone round. */
        const auto line = mixesMachineRows (record) ? withoutMachineRows (record).toLine() : record.toLine();
        auto& slot = ring[(last + 1) % capacity];

        /*  A record the wire cannot carry is given no counter, so it is no gap:
            it cannot happen (`wire::writeRecord`), and if it ever did, a
            counter promised and never sent would be a backup told it lost
            something the main never had to give. */
        if (! wire::writeRecord (slot, last + 1, line, record.ticket))
            return;     // refused before it wrote: the slot still holds the record it held

        ++last;

        if (last - oldest + 1 > capacity)
            oldest = last - capacity + 1;
    }

    bool Stream::heardWithin (LinkClock::time_point at, double timeoutSeconds) const
    {
        return heardAny && within (lastHeard, at, timeoutSeconds);
    }

    const std::vector<std::uint8_t>* Stream::ringed (std::uint64_t counter) const
    {
        if (counter == 0 || counter < oldest || counter > last)
            return nullptr;

        return &ring[counter % capacity];
    }

    void Stream::received (const std::string& origin, const osc::Packet& message, LinkClock::time_point arrival)
    {
        const auto& a = message.args;

        //  RD.7's third argument, the pinging machine's tick, is the presence's (pair/Presence.h).
        if (! message.isBundle() && message.address == wire::ping
              && (a.size() >= 1 && a.size() <= 3) && a[0].isInt64()
              && (a.size() == 1 || a[1].isNumber()) && (a.size() < 3 || a[2].isInt64()))
        {
            /*  ANSWERED HERE AND AT ONCE, on the server's thread, with what this
                machine's tick is: the round trip is what a ping is for, and one
                that waited for the next tick would measure the tick. */
            if (send)
                send (origin, wire::encode (wire::pong, { a[0], osc::Value::int64 (tickNow.load (std::memory_order_relaxed)) }));

            Arrival ping;
            ping.kind = Arrival::Kind::ping;
            ping.origin = origin;
            ping.at = arrival;

            if (a.size() == 2 && std::isfinite (a[1].asDouble()) && a[1].asDouble() >= 0.0)
                ping.roundTrip = a[1].asDouble();

            const std::lock_guard<std::mutex> held { inboxLock };
            inbox.push_back (std::move (ping));
            return;
        }

        /*  A COMMAND THE BACKUP'S CLIENT GAVE IT, PASSED ON (RD.6, AFT): read
            back and handed on here, on the server's thread, so a GO passed on
            is applied on the main's next tick like one sent to it directly.
            Only by a machine that leads - a relay to one that does not is a
            pairing set up wrong somewhere, ignored and counted - and only a
            line that reads as an applied record: what a backup passes on is
            never a refusal or a drop. */
        if (! message.isBundle() && message.address == wire::relay && (a.size() == 1 || a.size() == 2)
              && a[0].isString() && (a.size() == 1 || counterOf (a[1]).has_value()))
        {
            auto parsed = LogRecord::fromLine (a[0].getString());

            if (isLeading() && relayed && parsed.has_value() && parsed->kind == LogRecord::Kind::applied
                  && ! parsed->command.empty())
            {
                /*  ITS TICKET, beside the line as the record that answers it
                    will carry it back (RD.7, AGJ): the backup counts a relay
                    confirmed when that record reaches it. */
                if (a.size() == 2)
                    parsed->ticket = *counterOf (a[1]);

                relayCount.fetch_add (1, std::memory_order_relaxed);
                relayed (*parsed);
                return;
            }

            ignoredCount.fetch_add (1, std::memory_order_relaxed);
            return;
        }

        if (const auto said = wire::readHello (message))
        {
            Arrival hello;
            hello.kind = Arrival::Kind::hello;
            hello.origin = origin;
            hello.machine = said->machine;
            hello.role = said->role;
            hello.lastCounter = said->lastCounter;
            hello.stream = said->stream;
            hello.wants = said->wantsStream();
            hello.described = said->described;
            hello.at = arrival;

            const std::lock_guard<std::mutex> held { inboxLock };
            inbox.push_back (std::move (hello));
            return;
        }

        ignoredCount.fetch_add (1, std::memory_order_relaxed);
    }

    void Stream::closed (const std::string& origin)
    {
        Arrival gone;
        gone.kind = Arrival::Kind::closed;
        gone.origin = origin;

        const std::lock_guard<std::mutex> held { inboxLock };
        inbox.push_back (std::move (gone));
    }

    void Stream::take (const Arrival& arrival, double timeoutSeconds)
    {
        switch (arrival.kind)
        {
            case Arrival::Kind::closed:
                followers.erase (arrival.origin);
                return;

            case Arrival::Kind::ping:
            {
                /*  A ping from a connection that never said hello - or no longer
                    follows - was answered all the same, and says nothing about
                    the link. */
                const auto found = followers.find (arrival.origin);

                if (found == followers.end())
                    return;

                found->second.pinged = true;
                found->second.lastPing = arrival.at;
                found->second.roundTrip = arrival.roundTrip;
                lastPinger = arrival.origin;

                if (! heardAny || arrival.at > lastHeard)
                    lastHeard = arrival.at;

                heardAny = true;
                return;
            }

            case Arrival::Kind::hello:
            {
                /*  ONLY A MACHINE THAT LEADS TAKES FOLLOWERS, and only one that
                    asks follows (RD.7): a machine that says it follows, or that
                    both send and so each applies the other's. One that says it
                    is anything else is telling this one what it is - pair/
                    Presence.h reads that - and follows nothing of it from here:
                    a follower on that connection that took over, say, is
                    forgotten. RD.5's hello, of four, follows when it says
                    `backup`; one that says anything else is a pairing set up
                    wrong, and like a follower's hello to a machine that does
                    not lead, it is ignored and counted. */
                if (! arrival.wants)
                {
                    if (arrival.described)
                        followers.erase (arrival.origin);
                    else
                        ignoredCount.fetch_add (1, std::memory_order_relaxed);

                    return;
                }

                if (! isLeading())
                {
                    ignoredCount.fetch_add (1, std::memory_order_relaxed);
                    return;
                }

                /*  WHERE IT RESUMES. A backup that held this stream's records up
                    to a counter is sent what follows; one that held another
                    stream's - a main that went away and came back is a new
                    stream - or none holds nothing of this one and starts at the
                    first. A start the ring no longer holds is a gap: it is told
                    so, and sent what the ring has (RD.8 seats it then). */
                const auto asked = arrival.stream == streamId ? std::min (arrival.lastCounter, last)
                                                              : std::uint64_t { 0 };
                Follower follower;
                follower.machine = arrival.machine;
                follower.next = asked + 1;

                if (follower.next < oldest)
                {
                    follower.gapOldest = oldest;
                    follower.next = oldest;
                }

                follower.sentUpTo = follower.next - 1;

                /*  A LINK COMES UP when nothing has been heard from a follower
                    for the timeout: its counts start again. A second path's
                    hello, or a backup that came back before it was missed, joins
                    the link that is up. */
                if (! heardWithin (arrival.at, timeoutSeconds))
                {
                    sentThisLink = 0;
                    gapsThisLink = 0;
                    highestSent = follower.sentUpTo;
                }

                if (follower.gapOldest != 0)
                    ++gapsThisLink;

                //  A second hello on one connection starts it again: it says what it holds now.
                followers[arrival.origin] = std::move (follower);

                if (! heardAny || arrival.at > lastHeard)
                    lastHeard = arrival.at;

                heardAny = true;
                return;
            }
        }
    }

    void Stream::afterTick (std::int64_t tick, double timeoutSeconds)
    {
        tickNow.store (tick, std::memory_order_relaxed);

        {
            const std::lock_guard<std::mutex> held { inboxLock };
            arrivals.swap (inbox);
        }

        for (const auto& arrival : arrivals)
            take (arrival, timeoutSeconds);

        arrivals.clear();

        /*  A machine that no longer leads streams to nobody: its followers are
            forgotten, and if it leads again it takes them from their next hello
            (RD.7). */
        if (! isLeading())
            followers.clear();

        if (followers.empty())
            return;

        wire::writeBeat (beatBytes, tick, last, liveRuns);

        for (auto at = followers.begin(); at != followers.end();)
        {
            auto& follower = at->second;
            startBundle (frame);

            if (! follower.welcomed)
            {
                addToBundle (frame, wire::encode (wire::welcome, { osc::Value::string (streamId),
                                                                   counterValue (follower.next) }));

                if (follower.gapOldest != 0)
                    addToBundle (frame, wire::encode (wire::gap, { counterValue (follower.gapOldest) }));
            }

            /*  A FOLLOWER STILL CATCHING UP whose next record the ring has since
                let go of - only a main keeping more than a slice a tick for
                the length of the ring could do it - is told so and jumps,
                rather than being sent a record that is no longer the one its
                counter names. */
            const auto jumped = follower.sentUpTo + 1 < oldest;
            const auto from = jumped ? oldest - 1 : follower.sentUpTo;
            auto upTo = from;

            if (jumped)
                addToBundle (frame, wire::encode (wire::gap, { counterValue (oldest) }));

            for (std::size_t n = 0; n < backlog && upTo < last; ++n)
            {
                if (const auto* bytes = ringed (upTo + 1))
                    addToBundle (frame, *bytes);

                ++upTo;
            }

            addToBundle (frame, beatBytes);

            //  A connection the server no longer has is a follower gone, and what was not sent is not counted.
            if (send && ! send (at->first, frame))
            {
                at = followers.erase (at);
                continue;
            }

            follower.welcomed = true;

            if (jumped)
                ++gapsThisLink;

            //  Each record once, however many connections it went down.
            if (upTo > highestSent)
            {
                sentThisLink += static_cast<std::int64_t> (upTo - std::max (highestSent, from));
                highestSent = upTo;
            }

            follower.sentUpTo = upTo;
            ++at;
        }
    }

    LinkReading Stream::reading (LinkClock::time_point now, double timeoutSeconds) const
    {
        LinkReading out;
        readInto (out, now, timeoutSeconds);
        return out;
    }

    void Stream::readInto (LinkReading& out, LinkClock::time_point now, double timeoutSeconds) const
    {
        const Follower* freshest = nullptr;

        for (const auto& entry : followers)
        {
            const auto& follower = entry.second;

            if (follower.pinged && within (follower.lastPing, now, timeoutSeconds)
                  && (freshest == nullptr || follower.lastPing > freshest->lastPing))
                freshest = &follower;
        }

        out.linked = freshest != nullptr;
        out.roundTrip = freshest != nullptr ? freshest->roundTrip : 0.0;
        out.received = sentThisLink;
        out.gaps = gapsThisLink;

        //  Assigned, not rebuilt: the same text a tick reuses the string's storage.
        if (out.peer != lastPinger)
            out.peer = lastPinger;
    }

    //==========================================================================
    void Receiver::deliver (const std::string& path, const osc::Packet& message, LinkClock::time_point arrival)
    {
        //  A tick's messages come as one bundle (Stream::afterTick): each, in its order.
        if (message.isBundle())
        {
            for (const auto& element : message.elements)
                deliver (path, element, arrival);

            return;
        }

        const auto& a = message.args;

        Item item;
        item.path = path;
        item.at = arrival;

        const auto is = [&message] (const char* address) { return ! message.isBundle() && message.address == address; };

        if (is (wire::record) && (a.size() == 2 || a.size() == 3) && counterOf (a[0]).has_value() && a[1].isString()
              && (a.size() == 2 || counterOf (a[2]).has_value()))
        {
            /*  READ BACK HERE, on the path's own thread, so the tick thread is
                handed a record and never a line to parse. A line that does not
                read is a record lost: its counter is never seen, and the next
                one is a gap. */
            auto parsed = LogRecord::fromLine (a[1].getString());

            if (! parsed.has_value())
            {
                ignoredCount.fetch_add (1, std::memory_order_relaxed);
                return;
            }

            item.kind = Item::Kind::record;
            item.first = a[0].getInt64();
            item.record = std::move (*parsed);

            //  The relay it answers, beside the line (RD.7, AGJ).
            if (a.size() == 3)
                item.record.ticket = *counterOf (a[2]);
        }
        else if (is (wire::beat) && (a.size() == 2 || a.size() == 3) && a[0].isInt64() && counterOf (a[1]).has_value()
                   && (a.size() == 2 || a[2].isInt64()))
        {
            item.kind = Item::Kind::beat;
            item.first = a[0].getInt64();
            item.second = a[1].getInt64();
            item.third = a.size() == 3 ? a[2].getInt64() : 0;
        }
        else if (is (wire::welcome) && a.size() == 2 && a[0].isString() && counterOf (a[1]).has_value())
        {
            item.kind = Item::Kind::welcome;
            item.stream = a[0].getString();
            item.first = a[1].getInt64();
        }
        else if (is (wire::gap) && a.size() == 1 && counterOf (a[0]).has_value())
        {
            item.kind = Item::Kind::gap;
            item.first = a[0].getInt64();
        }
        else if (is (wire::pong) && a.size() == 2 && a[0].isInt64() && a[1].isInt64())
        {
            item.kind = Item::Kind::pong;
            item.first = a[0].getInt64();
            item.second = a[1].getInt64();
        }
        else
        {
            ignoredCount.fetch_add (1, std::memory_order_relaxed);
            return;
        }

        const std::lock_guard<std::mutex> held { lock };
        inbox.push_back (std::move (item));
    }

    std::string Receiver::streamId() const
    {
        const std::lock_guard<std::mutex> held { lock };
        return streamShared;
    }

    std::pair<std::uint64_t, std::string> Receiver::resumePoint() const
    {
        const std::lock_guard<std::mutex> held { lock };

        if (afreshShared)
            return { wire::fromNow, afreshStream };

        return { lastSeen.load (std::memory_order_relaxed), streamShared };
    }

    void Receiver::startAfresh (const std::string& leaderStream)
    {
        /*  NOTHING HELD OF ANY STREAM: the counter and the stream forgotten
            here, so a path dialled again before the welcome comes asks from
            now as the first hello did, and never resumes the stream this
            machine followed before - which, after a takeover and back, is a
            stream it stopped applying long ago. */
        afresh = true;
        stream.clear();
        last = 0;
        lastSeen.store (0, std::memory_order_relaxed);

        const std::lock_guard<std::mutex> held { lock };
        afreshShared = true;
        afreshStream = leaderStream;
        streamShared.clear();
    }

    void Receiver::drain (LinkClock::time_point now, double timeoutSeconds)
    {
        {
            const std::lock_guard<std::mutex> held { lock };
            items.swap (inbox);
        }

        for (auto& item : items)
            take (item, timeoutSeconds);

        items.clear();

        lastSeen.store (last, std::memory_order_relaxed);
        roundTripSaid.store (fastestTrip (now), std::memory_order_relaxed);
    }

    double Receiver::fastestTrip (LinkClock::time_point now) const
    {
        auto fastest = 0.0;

        for (std::size_t i = 0; i < tripsKept; ++i)
        {
            const auto& trip = trips[i];

            if (within (trip.at, now, 1.0) && (fastest <= 0.0 || trip.seconds < fastest))
                fastest = trip.seconds;
        }

        return fastest;
    }

    void Receiver::heard (LinkClock::time_point at)
    {
        if (! heardAny || at > lastHeard)
            lastHeard = at;

        heardAny = true;
    }

    void Receiver::take (Item& item, double timeoutSeconds)
    {
        switch (item.kind)
        {
            case Item::Kind::welcome:
            {
                /*  A NEW STREAM - a main that went away and came back, or the
                    first one - holds nothing this machine has: its counter
                    starts again. And a link comes up, its counts from nought,
                    when nothing was heard for the timeout or the stream is a
                    new one; a second path's welcome joins the link that is up. */
                const auto changed = item.stream != stream;

                if (changed || afresh || ! heardAny || ! within (lastHeard, item.at, timeoutSeconds))
                {
                    receivedThisLink = 0;
                    gapsThisLink = 0;
                }

                if (changed || afresh)
                {
                    stream = item.stream;

                    /*  A FOLLOW THAT STARTED NOW starts where the welcome says
                        (RD.7): the records before it were never asked for, and
                        are no gap. Any other new stream holds nothing yet. */
                    last = afresh && item.first > 0 ? static_cast<std::uint64_t> (item.first) - 1 : 0;
                    afresh = false;

                    /*  AND ITS TICKS ARE ITS OWN: a leader started again counts
                        from nought, and a beat is handed on only when its tick
                        passes the last one's - measured against the old
                        leader's, the new one's would wait an evening. */
                    beaten = false;
                    beatTick = 0;
                    beatLast = 0;
                    beatRuns = 0;

                    const std::lock_guard<std::mutex> held { lock };
                    streamShared = stream;
                    afreshShared = false;
                    afreshStream.clear();
                }

                heard (item.at);
                return;
            }

            case Item::Kind::gap:
            {
                //  Said by the main; a second path's word for a gap the first already filled is nothing.
                const auto oldest = static_cast<std::uint64_t> (item.first);

                if (oldest > last + 1)
                {
                    ++gapsThisLink;
                    last = oldest - 1;
                }

                heard (item.at);
                return;
            }

            case Item::Kind::record:
            {
                const auto counter = static_cast<std::uint64_t> (item.first);

                if (counter <= last)
                {
                    ++duplicateCount;
                    return;
                }

                if (counter > last + 1)
                    ++gapsThisLink;

                last = counter;
                ++receivedThisLink;
                peer = item.path;
                heard (item.at);

                if (onRecord)
                    onRecord (counter, item.record);

                return;
            }

            case Item::Kind::beat:
            {
                //  The path whose beat for a tick came first is the one delivering.
                const auto fresh = ! beaten || item.first > beatTick;

                if (fresh)
                    peer = item.path;

                if (! beaten || item.at > lastBeat)
                    lastBeat = item.at;

                beaten = true;
                beatTick = std::max (beatTick, item.first);
                beatLast = std::max (beatLast, static_cast<std::uint64_t> (item.second));

                if (fresh)
                    beatRuns = item.third;

                heard (item.at);

                //  Each of the main's ticks once, as it first arrived (RD.6).
                if (fresh && onBeat)
                    onBeat (item.first, item.at);

                return;
            }

            case Item::Kind::pong:
            {
                /*  THERE AND BACK ON THIS MACHINE'S OWN CLOCK: the ping carried
                    the moment it left, the pong carries it home. A trip that
                    reads negative or longer than a minute is not one of ours. */
                const auto left = LinkClock::time_point (
                    std::chrono::duration_cast<LinkClock::duration> (std::chrono::nanoseconds (item.first)));
                const auto seconds = std::chrono::duration<double> (item.at - left).count();

                if (! (seconds >= 0.0) || seconds > 60.0)
                    return;

                trips[tripAt] = { item.at, seconds };
                tripAt = (tripAt + 1) % trips.size();
                tripsKept = std::min (tripsKept + 1, trips.size());
                return;
            }
        }
    }

    LinkReading Receiver::reading (LinkClock::time_point now, double timeoutSeconds) const
    {
        LinkReading out;
        readInto (out, now, timeoutSeconds);
        return out;
    }

    bool Receiver::isLinked (LinkClock::time_point now, double timeoutSeconds) const
    {
        return beaten && within (lastBeat, now, timeoutSeconds);
    }

    void Receiver::readInto (LinkReading& out, LinkClock::time_point now, double timeoutSeconds) const
    {
        out.linked = beaten && within (lastBeat, now, timeoutSeconds);
        out.roundTrip = out.linked ? fastestTrip (now) : 0.0;
        out.received = receivedThisLink;
        out.gaps = gapsThisLink;

        if (out.peer != peer)
            out.peer = peer;
    }

    //==========================================================================
    namespace
    {
        using WsClient = SimpleWeb::SocketClient<SimpleWeb::WS>;

        /*  A CLIENT TOLD ITS HOST AND PORT, never left to read them: SimpleWeb
            splits `host:port` at its first colon, which an IPv6 address has
            several of. Built on a placeholder and then given the two - both are
            the base's, protected, and read only when it connects. */
        class PeerSocket final : public WsClient
        {
        public:
            PeerSocket (const std::string& hostToDial, unsigned short portToDial)
                : WsClient ("localhost:" + std::to_string (portToDial) + "/")
            {
                host = hostToDial;
                port = portToDial;
            }
        };

        /*  THE WAITS BEFORE A PATH IS DIALLED AGAIN, after the first failure
            and each one after: a quarter of a second, a half, one, then two
            from then on. A takeover wants the main found again quickly, and a
            main that is not there costs nothing to try. */
        constexpr std::array<std::chrono::milliseconds, 4> redials {
            std::chrono::milliseconds { 250 }, std::chrono::milliseconds { 500 },
            std::chrono::milliseconds { 1000 }, std::chrono::milliseconds { 2000 } };

        /*  A socket not open this long after it was dialled is given up and
            dialled again: a main that is there answers in milliseconds. */
        constexpr auto openTimeout = std::chrono::seconds { 2 };

        /*  How often the manager thread looks at its paths. A ping is due a
            tenth of a second apart; this is the slack on that, and on a redial. */
        constexpr auto tendEvery = std::chrono::milliseconds { 10 };

        std::int64_t nanosecondsOf (LinkClock::time_point at)
        {
            return static_cast<std::int64_t> (
                std::chrono::duration_cast<std::chrono::nanoseconds> (at.time_since_epoch()).count());
        }
    }

    struct Dialler::Impl
    {
        struct Path
        {
            std::string peer;
            std::unique_ptr<PeerSocket> client;
            std::shared_ptr<SimpleWeb::io_context> io;
            std::thread reader;

            std::mutex lock;
            std::shared_ptr<WsClient::Connection> connection;
            std::atomic<bool> open { false };
            std::atomic<bool> ended { false };

            int failures = 0;
            LinkClock::time_point notBefore {};
            LinkClock::time_point openedAt {};
            LinkClock::time_point pingedAt {};

            bool active() const noexcept { return client != nullptr; }

            /*  Binary, opcode 130: the link speaks OSC, as every client of
                the port does. Any thread; queued by the connection, never
                waited on. */
            void send (const std::vector<std::uint8_t>& bytes)
            {
                std::shared_ptr<WsClient::Connection> to;

                {
                    const std::lock_guard<std::mutex> held { lock };
                    to = connection;
                }

                if (to == nullptr || bytes.empty())
                    return;

                to->send (std::string_view (reinterpret_cast<const char*> (bytes.data()), bytes.size()),
                          [this] (const SimpleWeb::error_code& error)
                          {
                              if (error)
                                  ended.store (true);
                          },
                          130);
            }

            void shut()
            {
                if (client == nullptr)
                    return;

                client->stop();

                if (io != nullptr)
                    io->stop();

                if (reader.joinable())
                    reader.join();

                {
                    const std::lock_guard<std::mutex> held { lock };
                    connection.reset();
                }

                client.reset();
                io.reset();
                open.store (false);
                ended.store (false);
            }

            ~Path() { shut(); }
        };

        Impl (Receiver& receiverToFeed, std::string machineName)
            : receiver (receiverToFeed), machine (std::move (machineName)) {}

        void run();
        void tend (Path& path, LinkClock::time_point now);
        void dial (Path& path, LinkClock::time_point now);
        void failed (Path& path, LinkClock::time_point now);

        Receiver& receiver;
        const std::string machine;

        /*  Who reads what a path brings, and what a path opens with (RD.7):
            set before `start`, read on the paths' threads. */
        Route route;
        HelloMaker makeHello;

        //  This machine's tick, for the pings (RD.7); -1 until the tick thread says.
        std::atomic<std::int64_t> tick { -1 };

        mutable std::mutex guard;
        std::condition_variable wake;
        std::vector<std::string> wanted;
        bool changed = false;
        bool stopping = false;

        /*  WHAT WAITS TO BE SENT that is not the dialler's own: relays (RD.6),
            down one open path; a leading backup's stream to the main at the end
            of one path, and orders down every one (RD.7); and whether the hello
            is to be said again. Sent by the manager thread, woken for at once.
            Under `guard`. */
        struct Outgoing
        {
            enum class To { onePath, thatPath, everyPath };

            To to = To::onePath;
            std::string path;
            std::vector<std::uint8_t> bytes;
        };

        std::vector<Outgoing> outbox;
        std::vector<Outgoing> sending;          // the manager thread's, swapped with it
        bool announcing = false;
        std::vector<std::string> openNames;     // the paths open at the last look, under `guard`

        std::atomic<bool> running { false };
        std::atomic<std::size_t> opened { 0 };
        std::thread thread;

        //  The manager thread's own.
        std::map<std::string, std::unique_ptr<Path>> paths;

        std::vector<std::uint8_t> helloNow() const
        {
            if (makeHello)
                return makeHello();

            //  RD.5's: a backup's, with what the receiver holds.
            return wire::encode (wire::hello, { osc::Value::string (machine), osc::Value::string ("backup"),
                                                counterValue (receiver.lastCounter()),
                                                osc::Value::string (receiver.streamId()) });
        }
    };

    void Dialler::Impl::run()
    {
        for (;;)
        {
            std::vector<std::string> now;
            auto sayHello = false;

            {
                std::unique_lock<std::mutex> held { guard };
                wake.wait_for (held, tendEvery, [this] { return stopping || changed || announcing || ! outbox.empty(); });

                if (stopping)
                    break;

                changed = false;
                now = wanted;
                sending.swap (outbox);
                sayHello = announcing;
                announcing = false;
            }

            //  An address no longer wanted is hung up on.
            for (auto at = paths.begin(); at != paths.end();)
            {
                if (std::find (now.begin(), now.end(), at->first) != now.end())
                {
                    ++at;
                    continue;
                }

                at->second->shut();
                at = paths.erase (at);
            }

            const auto moment = LinkClock::now();
            std::size_t openNow = 0;

            for (const auto& peer : now)
            {
                auto& path = paths[peer];

                if (path == nullptr)
                {
                    path = std::make_unique<Path>();
                    path->peer = peer;
                }

                tend (*path, moment);

                if (path->open.load())
                    ++openNow;
            }

            opened.store (openNow);

            {
                /*  READ AND KEPT UNDER ONE HOLD OF THE LOCK, which a path that
                    opens takes too (on_open, below): read outside it, a path
                    that opened between the read and the write was left out
                    for a turn of this loop, and a leader's frame sent down it
                    then (`sendTo`) was refused - its follower forgotten, the
                    stream lost for the follower's timeout. */
                const std::lock_guard<std::mutex> held { guard };
                openNames.clear();

                for (const auto& peer : now)
                    if (const auto found = paths.find (peer);
                        found != paths.end() && found->second != nullptr && found->second->open.load())
                        openNames.push_back (peer);
            }

            //  WHAT THIS MACHINE IS, said again down every open path (RD.7).
            if (sayHello)
            {
                const auto bytes = helloNow();

                for (auto& entry : paths)
                    if (entry.second != nullptr && entry.second->open.load())
                        entry.second->send (bytes);
            }

            /*  PASSED ON DOWN ONE PATH, the first open one in the order they
                were given (RD.6): a relay sent down two would be two GOs on
                the main, which has no counter to drop the second by. One that
                finds no path open - the link fell between the gate's decision
                and here - is lost, as a datagram to a machine switched off is;
                a backup that takes over settles what it passed on and never
                saw come back (RD.7, AGJ). A stream's frame goes down the path
                its follower is at the end of, and an order down every one, in
                the order they were queued. */
            if (! sending.empty())
            {
                Path* first = nullptr;

                for (const auto& peer : now)
                    if (const auto found = paths.find (peer);
                        found != paths.end() && found->second != nullptr && found->second->open.load())
                    {
                        first = found->second.get();
                        break;
                    }

                for (const auto& out : sending)
                {
                    switch (out.to)
                    {
                        case Outgoing::To::onePath:
                            if (first != nullptr)
                                first->send (out.bytes);
                            break;

                        case Outgoing::To::thatPath:
                            if (const auto found = paths.find (out.path);
                                found != paths.end() && found->second != nullptr && found->second->open.load())
                                found->second->send (out.bytes);
                            break;

                        case Outgoing::To::everyPath:
                            for (auto& entry : paths)
                                if (entry.second != nullptr && entry.second->open.load())
                                    entry.second->send (out.bytes);
                            break;
                    }
                }

                sending.clear();
            }
        }

        for (auto& entry : paths)
            entry.second->shut();

        paths.clear();
        opened.store (0);

        const std::lock_guard<std::mutex> held { guard };
        openNames.clear();
    }

    void Dialler::Impl::failed (Path& path, LinkClock::time_point now)
    {
        path.shut();
        ++path.failures;
        path.notBefore = now + redials[static_cast<std::size_t> (std::min (path.failures, 4) - 1)];
    }

    void Dialler::Impl::tend (Path& path, LinkClock::time_point now)
    {
        //  Gone under it: the main closed, went away, or a send failed.
        if (path.active() && path.ended.load())
        {
            failed (path, now);
            return;
        }

        if (! path.active())
        {
            if (now >= path.notBefore)
                dial (path, now);

            return;
        }

        if (! path.open.load())
        {
            if (now - path.openedAt > openTimeout)
                failed (path, now);

            return;
        }

        //  OPEN: the next redial after this one drops is the quickest again.
        path.failures = 0;

        if (now - path.pingedAt >= pingEvery)
        {
            path.pingedAt = now;
            std::vector<osc::Value> said { osc::Value::int64 (nanosecondsOf (LinkClock::now())),
                                           osc::Value::float32 (static_cast<float> (receiver.roundTripToSay())) };

            //  AND THIS MACHINE'S TICK (RD.7): the main knows the engine is alive, not only the socket.
            if (const auto ticking = tick.load (std::memory_order_relaxed); ticking >= 0)
                said.push_back (osc::Value::int64 (ticking));

            path.send (wire::encode (wire::ping, std::move (said)));
        }
    }

    void Dialler::Impl::dial (Path& path, LinkClock::time_point now)
    {
        const auto target = Dialler::hostAndPort (path.peer);

        //  Never for an address `pair::peerAddresses` wrote; a bad one waits as a failure would.
        if (! target.has_value())
        {
            path.notBefore = now + redials.back();
            return;
        }

        path.io = std::make_shared<SimpleWeb::io_context>();
        path.client = std::make_unique<PeerSocket> (target->first, static_cast<unsigned short> (target->second));
        path.client->io_service = path.io;
        path.client->config.timeout_request = 2;

        auto* const self = &path;

        /*  ON OPEN, AT ONCE: hello, with what this machine holds - the last
            counter it drained and the stream it was of - so the main's first
            answer is what follows it. On the path's own thread; a record still
            waiting in the receiver's inbox is asked for again and dropped as a
            duplicate when it comes. */
        path.client->on_open = [this, self] (std::shared_ptr<WsClient::Connection> connection)
        {
            {
                const std::lock_guard<std::mutex> held { self->lock };
                self->connection = std::move (connection);
            }

            self->open.store (true);

            //  Open to `sendTo` at once, not at the manager's next look (RD.7).
            {
                const std::lock_guard<std::mutex> held { guard };

                if (std::find (openNames.begin(), openNames.end(), self->peer) == openNames.end())
                    openNames.push_back (self->peer);
            }

            self->send (helloNow());
        };

        path.client->on_message = [this, self] (std::shared_ptr<WsClient::Connection>,
                                                std::shared_ptr<WsClient::InMessage> message)
        {
            /*  A BINARY FRAME IS OSC; a text frame is the server's news about
                its namespace, which every connection to the port is sent and
                the link has no use for. Stamped as it is read: the heartbeat's
                arrival is what decides whether the main is there. */
            if ((message->fin_rsv_opcode & 0x0f) != 2)
                return;

            const auto bytes = message->string();
            const auto decoded = osc::decode (bytes);

            /*  A bundle is a tick's messages to this follower; a bare message,
                a pong - and from RD.7 whatever the main says as the other half:
                its hello, an order, a relay when it follows this machine. */
            if (! decoded.ok || (! decoded.packet.isBundle() && ! wire::isPairAddress (decoded.packet.address)))
                return;

            if (route)
                route (self->peer, decoded.packet, LinkClock::now());
            else
                receiver.deliver (self->peer, decoded.packet, LinkClock::now());
        };

        path.client->on_close = [self] (std::shared_ptr<WsClient::Connection>, int, const std::string&)
        {
            self->ended.store (true);
        };

        path.client->on_error = [self] (std::shared_ptr<WsClient::Connection>, const SimpleWeb::error_code&)
        {
            self->ended.store (true);
        };

        path.open.store (false);
        path.ended.store (false);
        path.openedAt = now;
        path.pingedAt = now;

        //  `start` only begins the connection on an io context of ours; the reader runs it.
        path.client->start();
        path.reader = std::thread ([io = path.io] { io->run(); });
    }

    //==========================================================================
    Dialler::Dialler (Receiver& receiverToFeed, std::string machineName)
        : impl (std::make_unique<Impl> (receiverToFeed, std::move (machineName)))
    {
    }

    Dialler::~Dialler()
    {
        stop();
    }

    bool Dialler::start()
    {
        if (impl->running.exchange (true))
            return false;

        {
            const std::lock_guard<std::mutex> held { impl->guard };
            impl->stopping = false;
        }

        impl->thread = std::thread ([this] { impl->run(); });
        return true;
    }

    void Dialler::stop()
    {
        if (! impl->running.load())
            return;

        {
            //  Under the lock, or a wake between the thread's test and its wait is lost.
            const std::lock_guard<std::mutex> held { impl->guard };
            impl->stopping = true;
        }

        impl->wake.notify_all();

        if (impl->thread.joinable())
            impl->thread.join();

        impl->running.store (false);
    }

    bool Dialler::isRunning() const noexcept
    {
        return impl->running.load();
    }

    void Dialler::want (std::vector<std::string> peers)
    {
        {
            const std::lock_guard<std::mutex> held { impl->guard };

            if (peers == impl->wanted)
                return;

            impl->wanted = std::move (peers);
            impl->changed = true;
        }

        impl->wake.notify_all();
    }

    std::size_t Dialler::openPaths() const
    {
        return impl->opened.load();
    }

    bool Dialler::relay (std::vector<std::uint8_t> bytes)
    {
        if (bytes.empty() || impl->opened.load() == 0)
            return false;

        {
            const std::lock_guard<std::mutex> held { impl->guard };

            if (impl->stopping || ! impl->running.load())
                return false;

            impl->outbox.push_back ({ Impl::Outgoing::To::onePath, {}, std::move (bytes) });
        }

        impl->wake.notify_all();
        return true;
    }

    void Dialler::setRoute (Route routeToUse)
    {
        impl->route = std::move (routeToUse);
    }

    void Dialler::setHello (HelloMaker makeHello)
    {
        impl->makeHello = std::move (makeHello);
    }

    void Dialler::announce()
    {
        {
            const std::lock_guard<std::mutex> held { impl->guard };

            if (impl->stopping || ! impl->running.load())
                return;

            impl->announcing = true;
        }

        impl->wake.notify_all();
    }

    bool Dialler::sendTo (const std::string& path, std::vector<std::uint8_t> bytes)
    {
        if (bytes.empty())
            return false;

        {
            const std::lock_guard<std::mutex> held { impl->guard };

            if (impl->stopping || ! impl->running.load()
                  || std::find (impl->openNames.begin(), impl->openNames.end(), path) == impl->openNames.end())
                return false;

            impl->outbox.push_back ({ Impl::Outgoing::To::thatPath, path, std::move (bytes) });
        }

        impl->wake.notify_all();
        return true;
    }

    void Dialler::sendAll (std::vector<std::uint8_t> bytes)
    {
        if (bytes.empty())
            return;

        {
            const std::lock_guard<std::mutex> held { impl->guard };

            if (impl->stopping || ! impl->running.load())
                return;

            impl->outbox.push_back ({ Impl::Outgoing::To::everyPath, {}, std::move (bytes) });
        }

        impl->wake.notify_all();
    }

    void Dialler::setTick (std::int64_t ticking) noexcept
    {
        impl->tick.store (ticking, std::memory_order_relaxed);
    }

    std::optional<std::pair<std::string, int>> Dialler::hostAndPort (std::string_view peer)
    {
        const auto colon = peer.rfind (':');

        if (colon == std::string_view::npos || colon == 0 || colon + 1 >= peer.size())
            return std::nullopt;

        auto host = peer.substr (0, colon);
        const auto digits = peer.substr (colon + 1);

        if (host.front() == '[')
        {
            if (host.size() < 3 || host.back() != ']')
                return std::nullopt;

            host = host.substr (1, host.size() - 2);
        }

        int port = 0;
        const auto parsed = std::from_chars (digits.data(), digits.data() + digits.size(), port);

        if (parsed.ec != std::errc {} || parsed.ptr != digits.data() + digits.size() || port < 1 || port > 65535)
            return std::nullopt;

        return std::make_pair (std::string (host), port);
    }
}
