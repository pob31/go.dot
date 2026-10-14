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

#include <wfg/engine/pair/Leadership.h>

#include <wfg/engine/command/Command.h>

#include <utility>

namespace wfg::pair
{
    namespace
    {
        Move moveTo (Role role, Seat to, std::string reason, std::optional<Order> tell = std::nullopt)
        {
            Move move;
            move.role = role;
            move.to = to;
            move.reason = std::move (reason);
            move.tell = tell;
            return move;
        }

        Seat startingSeat (Role role) noexcept
        {
            /*  AGK: a backup that has never seen its main holds; a main that
                starts alone sends; a machine not paired is alone and sends. */
            return role == Role::backup ? Seat::waiting : Seat::alone;
        }

        /*  THE SEAT A MACHINE THAT SENDS TAKES: a backup leads - it sends only
            because it took over, or was handed the show, and its main follows
            it or will; a main leads while the other machine is there to follow
            it, and is alone otherwise. */
        Seat sendingSeat (Role role, bool linked) noexcept
        {
            if (role == Role::backup)
                return Seat::leading;

            return role == Role::main && linked ? Seat::leading : Seat::alone;
        }

        bool peerSends (const Situation& s) noexcept
        {
            return s.linked && s.peerSeat.has_value() && sends (*s.peerSeat);
        }

        bool peerFollows (const Situation& s) noexcept
        {
            return s.linked && s.peerSeat.has_value() && *s.peerSeat == Seat::following;
        }

        /*  WHETHER THIS MACHINE FOLLOWS A MACHINE IT FINDS SENDING (AFR, AGA):
            when it has run nothing since it started and the other has - it has
            nothing to choose between. Two that have both run nothing are a
            backup and its main, and the backup follows; or a pair set up wrong,
            two mains or two backups, and the one whose stream's identity sorts
            later follows, so the two never both wait for the other. One that
            has run something never follows unasked: two of those are AFQ's. */
        bool yields (const Situation& s) noexcept
        {
            if (! s.fresh)
                return false;

            if (! s.peerFresh)
                return true;

            if (s.role != s.peerRole)
                return s.role == Role::backup;

            return s.stream > s.peerStream;
        }

        std::string spelledRuns (std::int64_t count)
        {
            return std::to_string (count) + (count == 1 ? " cue was" : " cues were");
        }

        const char* otherOf (Role role) noexcept
        {
            return role == Role::backup ? "main" : "backup";
        }
    }

    //==========================================================================
    const char* wordOf (Seat seat) noexcept
    {
        switch (seat)
        {
            case Seat::alone:       return "alone";
            case Seat::leading:     return "leading";
            case Seat::following:   return "following";
            case Seat::waiting:     return "waiting";
            case Seat::bothSending: return "bothSending";
        }

        return "alone";
    }

    std::optional<Seat> seatNamed (std::string_view word) noexcept
    {
        for (const auto seat : { Seat::alone, Seat::leading, Seat::following, Seat::waiting, Seat::bothSending })
            if (word == wordOf (seat))
                return seat;

        return std::nullopt;
    }

    const char* wordOf (Role role) noexcept
    {
        switch (role)
        {
            case Role::none:   return "none";
            case Role::main:   return "main";
            case Role::backup: return "backup";
        }

        return "none";
    }

    Role roleNamed (std::string_view word) noexcept
    {
        return word == "main" ? Role::main : word == "backup" ? Role::backup : Role::none;
    }

    bool sends (Seat seat) noexcept
    {
        return seat == Seat::alone || seat == Seat::leading || seat == Seat::bothSending;
    }

    bool follows (Seat seat) noexcept
    {
        return seat == Seat::following || seat == Seat::bothSending;
    }

    bool gated (Seat seat) noexcept
    {
        return seat == Seat::following;
    }

    //==========================================================================
    std::optional<Move> decide (const Seated& now, const Situation& s)
    {
        /*  A ROLE THAT CHANGED - the first tick of a paired machine, or a
            `pair.setRole` - seats the machine afresh, as it starts. */
        if (now.role != s.role)
            return moveTo (s.role, startingSeat (s.role),
                           now.role == Role::none && now.reason.empty() ? "started" : "role");

        if (s.role == Role::none)
            return std::nullopt;

        switch (now.seat)
        {
            case Seat::alone:
                if (peerFollows (s))
                    return moveTo (s.role, Seat::leading, "follower-linked");

                if (peerSends (s))
                {
                    if (yields (s))
                        return moveTo (s.role, Seat::following, "leader-found");

                    //  A fresh machine that sends will follow this one at once (AFR): it is waited for.
                    if (! s.peerFresh)
                        return moveTo (s.role, Seat::bothSending, "both-sending");
                }

                return std::nullopt;

            case Seat::leading:
                if (! s.linked)
                {
                    //  A main whose backup went is alone again; a backup that leads goes on leading.
                    if (s.role == Role::main)
                        return moveTo (s.role, Seat::alone, "follower-lost");

                    return std::nullopt;
                }

                if (peerSends (s))
                {
                    if (yields (s))
                        return moveTo (s.role, Seat::following, "leader-found");

                    if (! s.peerFresh)
                        return moveTo (s.role, Seat::bothSending, "both-sending");
                }

                return std::nullopt;

            case Seat::following:
                /*  THE LINK LOST (AFV): the stream silent for the timeout, the
                    follower takes over. A leader that says another stream is
                    one started again since - a crash, and a quick restart - and
                    holds nothing of the show: this machine does, and takes
                    over, and the leader, fresh, follows it (AFR). */
                if (! s.streamLive)
                    return moveTo (s.role, sendingSeat (s.role, false), "link-lost");

                if (s.leaderChanged)
                    return moveTo (s.role, sendingSeat (s.role, s.linked), "leader-restarted");

                return std::nullopt;

            case Seat::waiting:
                //  AGK: the main found, it is followed, whatever was done here meanwhile.
                if (peerSends (s))
                    return moveTo (s.role, Seat::following, "main-found");

                return std::nullopt;

            case Seat::bothSending:
                //  Neither changes until an operator picks one (AFQ) - unless the other went, or follows.
                if (! s.linked)
                    return moveTo (s.role, sendingSeat (s.role, false), "peer-lost");

                if (peerFollows (s))
                    return moveTo (s.role, Seat::leading, "follower-linked");

                return std::nullopt;
        }

        return std::nullopt;
    }

    std::optional<Move> asked (const Seated& now, Ask ask, const Situation& s)
    {
        if (now.role == Role::none)
            return std::nullopt;

        switch (ask)
        {
            case Ask::takeOver:
                /*  A FOLLOWER, OR A WAITING BACKUP, SENDS NOW; the leader it
                    followed, if it is there, is told to hold and follow. */
                if (now.seat == Seat::following || now.seat == Seat::waiting)
                    return moveTo (now.role, sendingSeat (now.role, s.linked), "take-over",
                                   s.linked ? std::optional<Order> (Order::hold) : std::nullopt);

                return std::nullopt;

            case Ask::handOver:
                //  THE MACHINE SENDING HOLDS AND FOLLOWS; the other is told to send.
                if ((now.seat == Seat::leading || now.seat == Seat::bothSending) && s.linked)
                    return moveTo (now.role, Seat::following, "hand-over", Order::lead);

                return std::nullopt;

            case Ask::keep:
                //  AFQ: this machine keeps the show; the other holds and follows.
                if (now.seat == Seat::bothSending)
                    return moveTo (now.role, sendingSeat (now.role, s.linked), "keep", Order::hold);

                return std::nullopt;
        }

        return std::nullopt;
    }

    std::optional<Move> ordered (const Seated& now, const Ordered& order, const Situation& s)
    {
        if (now.role == Role::none)
            return std::nullopt;

        const auto reason = "peer-" + order.reason;

        if (order.order == Order::hold)
        {
            //  The other machine took over, or kept the show: follow it.
            if (now.seat != Seat::following)
                return moveTo (now.role, Seat::following, reason);

            return std::nullopt;
        }

        //  It handed the show over: send.
        if (now.seat == Seat::following || now.seat == Seat::waiting || now.seat == Seat::bothSending)
            return moveTo (now.role, sendingSeat (now.role, s.linked), reason);

        return std::nullopt;
    }

    std::string refusal (const Seated& now, Ask ask, std::string_view peerSeat)
    {
        if (now.role == Role::none)
            return reason::notPaired;

        const auto holding = now.seat == Seat::following || now.seat == Seat::waiting;

        switch (ask)
        {
            case Ask::takeOver:
                return holding ? std::string {} : std::string (reason::notHolding);

            case Ask::handOver:
                if (holding)
                    return reason::notSending;

                return (now.seat == Seat::leading || now.seat == Seat::bothSending) && ! peerSeat.empty()
                         ? std::string {} : std::string (reason::notLinked);

            case Ask::keep:
                return now.seat == Seat::bothSending ? std::string {} : std::string (reason::notBothSending);
        }

        return reason::notPaired;
    }

    //==========================================================================
    std::string alarm (const Seated& now, const Situation& s, const NotInStep& behind)
    {
        if (now.role == Role::none)
            return {};

        switch (now.seat)
        {
            case Seat::waiting:
                return "Waiting for the main: this backup holds and sends nothing until it finds its main and"
                       " follows it, or is told to take over.";

            case Seat::bothSending:
                return "Both machines are sending: every command reaches the devices from both. Keep the show"
                       " on one of them; the other will hold and follow.";

            case Seat::following:
            {
                /*  ON ITS WAY TO THE LEADER'S POSITION (RD.8): said with why it
                    needs one, when there is something to say. */
                const std::string seating = behind.seating ? " Taking the other machine's position." : "";

                if (behind.leaderRuns > 0)
                    return "Following, but not yet in step: " + spelledRuns (behind.leaderRuns)
                         + " running on the other machine when this one began to follow, and "
                         + (behind.leaderRuns == 1 ? "it does" : "they do") + " not run here." + seating;

                if (behind.changedWhileWaiting)
                    return "Following, but not yet in step: this machine was changed while it waited for its main."
                         + seating;

                if (behind.seating)
                    return "Following, and taking the other machine's position.";

                /*  NOT THE SAME SHOW (RD.10, AFU), before anything the runs say:
                    two shows that differ explain runs that do. What puts it
                    right - the other machine's show taken here, or its files
                    copied here by somebody, since sounds never travel. */
                if (! behind.showDiffers.empty())
                    return "Following, but " + behind.showDiffers + "."
                         + (behind.showTextDiffers ? " Take the other machine's show to make the two the same."
                                                   : " Copy the files so both machines have the same.");

                /*  SEATED, AND APART SINCE (RD.8): the first thing the leader's
                    summaries say differs, and what puts it right. */
                if (! behind.differs.empty())
                    return "Following, but not in step: " + behind.differs + ". Realign to take the other machine's"
                           " position again.";

                return {};
            }

            case Seat::alone:
            case Seat::leading:
            {
                //  Taken over and sending, with nothing following it yet.
                if (peerFollows (s))
                    return {};

                const std::string other = otherOf (now.role);

                if (now.reason == "link-lost")
                    return "The " + other + " was lost and this machine took over: it sends now.";

                if (now.reason == "leader-restarted")
                    return "The " + other + " started again and this machine took over: it sends now.";

                if (now.reason == "take-over")
                    return "This machine took over: it sends now, and the " + other + " does not follow it.";

                return {};
            }
        }

        return {};
    }
}
