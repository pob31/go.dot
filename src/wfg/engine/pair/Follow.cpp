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

#include <wfg/engine/pair/Follow.h>

#include <wfg/engine/command/Command.h>
#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/command/Recovery.h>
#include <wfg/engine/pair/MachineLocal.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace wfg::pair
{
    namespace
    {
        bool startsWith (std::string_view text, std::string_view prefix) noexcept
        {
            return text.size() >= prefix.size() && text.substr (0, prefix.size()) == prefix;
        }

        std::string spelledAtoms (const std::vector<osc::Value>& values)
        {
            std::string out;

            for (const auto& value : values)
            {
                if (! out.empty())
                    out += ' ';

                out += value.toAtom();
            }

            return out;
        }
    }

    //==========================================================================
    Gate gateFor (std::string_view from, const Command* command, const std::vector<osc::Value>& args)
    {
        /*  THE ENGINE'S OWN AND THE MAIN'S, FIRST: they are what following is
            made of, and the commonest - a `main:` record is every one of the
            main's, an `engine` one every decision this machine makes for
            itself (AGE). */
        if (from == origin::engine || from == origin::replay || startsWith (from, origin::fromMain))
            return Gate::apply;

        /*  THIS MACHINE'S OWN SETTINGS, whoever asked (RD.3): its interface,
            its plugin scan, its pairing - never mirrored, so never passed on
            either. An operator on the backup's page choosing its interface is
            choosing the backup's. */
        if (command != nullptr && isMachineLocal (*command, args))
            return Gate::apply;

        /*  THIS MACHINE'S CLIENTS (AFT): applied on the main, streamed back. */
        if (from == origin::window || from == origin::cli || from == "pd"
              || startsWith (from, "ws:") || startsWith (from, "surface:"))
            return Gate::passOn;

        /*  A DEVICE'S, AND ANYTHING UNKNOWN (AGD): `udp:`, `mount:`, `serial:`,
            `link:`, `midi:`, `clock`, a patch's `process:` - the main heard it
            too, and its copy arrives in the stream. */
        return Gate::ignore;
    }

    std::vector<std::uint8_t> relayOf (const Event& event, std::uint64_t ticket)
    {
        /*  THE LOG'S OWN LINE, so the main reads a relay with the reader that
            already reads its stream and its logs: every atom - a blob, a
            French machine's 0.5, an int64 against an int32 - comes back as it
            went. Tick and sequence say nothing here; the main stamps its own. */
        LogRecord line;
        line.kind = LogRecord::Kind::applied;
        line.origin = event.origin;
        line.command = event.command;
        line.args = event.args;

        std::vector<osc::Value> said { osc::Value::string (line.toLine()) };

        //  THE TICKET, beside the line and never in it (RD.7, AGJ).
        if (ticket != 0)
            said.push_back (osc::Value::int64 (static_cast<std::int64_t> (ticket)));

        return wire::encode (wire::relay, std::move (said));
    }

    Settled settle (std::vector<Event> unconfirmed)
    {
        Settled out;

        for (auto& event : unconfirmed)
        {
            if (recovery::isStop (event.command))
                out.applied.push_back (std::move (event));
            else
                ++out.dropped;
        }

        return out;
    }

    //==========================================================================
    FollowGate::FollowGate (const CommandRegistry& commandsToRead, PassOn passOnToUse, std::uint64_t firstTicket)
        : commands (commandsToRead), passOn (std::move (passOnToUse)),
          nextTicket (firstTicket == 0 ? 1 : firstTicket)
    {
    }

    void FollowGate::confirm (std::uint64_t ticket)
    {
        if (ticket == 0)
            return;

        const std::lock_guard<std::mutex> held { ledgerLock };

        //  Nearly always the oldest: the leader answers in the order it was asked.
        for (auto at = pending.begin(); at != pending.end(); ++at)
        {
            if (at->ticket == ticket)
            {
                pending.erase (at);
                return;
            }
        }
    }

    std::vector<Event> FollowGate::takeUnconfirmed (LinkClock::time_point now, double maxAgeSeconds)
    {
        std::deque<Pending> taken;

        {
            const std::lock_guard<std::mutex> held { ledgerLock };
            taken.swap (pending);
        }

        std::vector<Event> out;

        for (auto& entry : taken)
        {
            if (std::chrono::duration<double> (now - entry.at).count() > maxAgeSeconds)
            {
                unconfirmedCount.fetch_add (1, std::memory_order_relaxed);
                continue;
            }

            out.push_back (std::move (entry.event));
        }

        return out;
    }

    void FollowGate::expire (LinkClock::time_point now, double maxAgeSeconds)
    {
        const std::lock_guard<std::mutex> held { ledgerLock };

        while (! pending.empty() && std::chrono::duration<double> (now - pending.front().at).count() > maxAgeSeconds)
        {
            pending.pop_front();
            unconfirmedCount.fetch_add (1, std::memory_order_relaxed);
        }
    }

    std::size_t FollowGate::unanswered() const
    {
        const std::lock_guard<std::mutex> held { ledgerLock };
        return pending.size();
    }

    bool FollowGate::take (Event& event)
    {
        /*  The registry is read-only once serve has registered everything, and
            a backup follows only once it is linked - long after - so the look-up
            is safe from any thread. */
        const auto* command = commands.find (event.command);

        switch (gateFor (event.origin, command, event.args))
        {
            case Gate::apply:
                return false;

            case Gate::ignore:
                ignoredCount.fetch_add (1, std::memory_order_relaxed);
                return true;

            case Gate::passOn:
            {
                /*  A TICKET, AND A PLACE IN THE LEDGER BEFORE IT GOES (RD.7,
                    AGJ): the leader's answer may come back before this returns.
                    Kept whether or not it could be sent - see the ledger's
                    note in Follow.h. */
                const auto ticket = nextTicket.fetch_add (1, std::memory_order_relaxed);

                {
                    const std::lock_guard<std::mutex> held { ledgerLock };
                    pending.push_back ({ ticket, event, LinkClock::now() });
                }

                if (passOn && passOn (relayOf (event, ticket)))
                    passedOnCount.fetch_add (1, std::memory_order_relaxed);
                else
                    lostCount.fetch_add (1, std::memory_order_relaxed);

                return true;
            }
        }

        return false;
    }

    //==========================================================================
    Follower::Follower (Submit submitToUse)
        : submit (std::move (submitToUse))
    {
        beats.reserve (16);
    }

    void Follower::setTickSeconds (double seconds) noexcept
    {
        if (std::isfinite (seconds) && seconds > 0.0)
            tickSeconds = seconds;
    }

    void Follower::start()
    {
        following = true;
        fixed = false;
        delta = 0;
        slotOffset = 0;
        beatsMeasured = 0;
        earliestSeen = latestSeen = clocks = 0.0;

        lateCount = 0;
        differenceCount = 0;
        differsText.clear();
        firstDifference.clear();
    }

    void Follower::stop()
    {
        following = false;
        submitDue (0, true);
        fixed = false;
        beats.clear();
    }

    void Follower::beat (std::int64_t mainTick, LinkClock::time_point arrival)
    {
        //  Measured only until the offset is fixed; afterwards a beat says nothing new.
        if (! fixed)
            beats.push_back ({ mainTick, arrival });
    }

    void Follower::record (const LogRecord& mainRecord)
    {
        held.push_back (mainRecord);
    }

    void Follower::beforeTick (std::int64_t localTick, LinkClock::time_point due, double roundTripSeconds)
    {
        if (! following)
        {
            beats.clear();
            return;
        }

        if (! fixed)
        {
            /*  EACH BEAT AS A FRACTIONAL TICK OF THIS MACHINE'S CLOCK: the tick
                that was due at `due`, and how far before or after that moment
                it arrived, less the main's tick it beat for. */
            for (const auto& arrival : beats)
            {
                const auto into = std::chrono::duration<double> (arrival.at - due).count() / tickSeconds;
                const auto e = static_cast<double> (localTick) + into - static_cast<double> (arrival.mainTick);

                earliestSeen = beatsMeasured == 0 ? e : std::min (earliestSeen, e);
                latestSeen = beatsMeasured == 0 ? e : std::max (latestSeen, e);
                ++beatsMeasured;
            }

            beats.clear();

            if (beatsMeasured >= measuredBeats && (roundTripSeconds > 0.0 || beatsMeasured >= 3 * measuredBeats))
            {
                /*  A WHOLE TICK AFTER THE LATEST ARRIVAL SEEN, so a record a tick
                    of jitter later than any measured still makes its slot. */
                slotOffset = static_cast<std::int64_t> (std::ceil (latestSeen - 1.0e-9)) + 1;

                /*  THE CLOCKS' OWN OFFSET: the earliest arrival, less the wire's
                    half of the round trip - what is left is the local tick at
                    the moment a main's tick began, and the main's work before
                    its beat left, which is small and makes Δ read a little
                    short rather than long. */
                clocks = earliestSeen - (std::max (0.0, roundTripSeconds) / 2.0) / tickSeconds;
                delta = std::max (1, static_cast<int> (std::lround (static_cast<double> (slotOffset) - clocks)));
                fixed = true;
            }
        }

        if (fixed)
            submitDue (localTick, false);
    }

    void Follower::submitDue (std::int64_t localTick, bool all)
    {
        while (! held.empty())
        {
            auto& next = held.front();

            /*  ITS SLOT, or at once when it has passed: every record of one main
                tick shares a slot, so they go in together, in the main's order. */
            if (! all)
            {
                const auto slot = next.tick + slotOffset;

                if (slot > localTick)
                    break;

                if (slot < localTick)
                    ++lateCount;
            }
            else
            {
                ++lateCount;
            }

            Event event;
            event.origin.reserve (origin::fromMain.size() + next.origin.size());
            event.origin.append (origin::fromMain);
            event.origin.append (next.origin);
            event.command = next.command;
            event.args = next.args;

            inFlight.push_back (std::move (next));
            held.pop_front();

            if (submit)
                submit (std::move (event));
        }
    }

    void Follower::applied (const LogRecord& written)
    {
        if (! startsWith (written.origin, origin::fromMain) || inFlight.empty())
            return;

        const auto mainRecord = std::move (inFlight.front());
        inFlight.pop_front();

        compare (mainRecord, written);
    }

    void Follower::compare (const LogRecord& mainRecord, const LogRecord& written)
    {
        const auto sameKind = mainRecord.kind == written.kind;
        const auto sameReason = mainRecord.reason == written.reason;
        const auto sameArgs = mainRecord.args == written.args;

        if (sameKind && sameReason && sameArgs)
            return;

        ++differenceCount;

        if (firstDifference.empty())
        {
            std::string what;

            if (! sameKind)
                what = mainRecord.kind == LogRecord::Kind::applied
                         ? "applied there, refused here (" + written.reason + ")"
                         : "refused there (" + mainRecord.reason + "), applied here";
            else if (! sameReason)
                what = "refused there (" + mainRecord.reason + "), here (" + written.reason + ")";
            else
                what = "applied as [" + spelledAtoms (mainRecord.args) + "] there and [" + spelledAtoms (written.args) + "] here";

            firstDifference = mainRecord.command + " at the main's tick " + std::to_string (mainRecord.tick) + ": " + what;
        }

        differsText = std::to_string (differenceCount)
                    + (differenceCount == 1 ? " record" : " records")
                    + " applied otherwise than on the main; the first, " + firstDifference;
    }
}
