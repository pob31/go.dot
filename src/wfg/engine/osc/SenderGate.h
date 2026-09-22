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

/*  WHO THIS SHOW IS WILLING TO HEAR FROM.

    A Go.dot engine answers its OSC port to anybody who can reach it, which is
    what makes a tablet work at all: nobody wants to declare a phone before it
    can press GO. But a show on a house network shares that network with
    everything else in the building, and the author's own desk (WFS-DIY) has had
    the other choice for years - an "OSC Filter" with two settings, Accept All
    and Registered Only. This is that switch.

    WITH IT ON, a datagram is taken only when its sender's address belongs to a
    device the show declares with `rx` set. Everything else is DROPPED AND
    LOGGED, never quietly ignored: the log record says who sent it and that it
    was refused, and `/godot/network/refused` counts them, because the failure
    this creates - a surface that does nothing and no way to find out why - is
    the one thing worse than the noise it prevents.

    IT GATES THE OSC PORT AND NOTHING ELSE. A WebSocket client is a CLIENT, not
    a device: the page, the tablet and the desktop window all write through it,
    and gating those would lock an operator out of their own engine over a
    setting they cannot reach without it. §3.2 says clients are equals; this is
    about the rest of the building.

    THE SHAPE IS THE TRIGGER INDEX'S, and deliberately: a socket thread must
    never take a lock the tick thread holds while it rebuilds the document.
    The tick thread builds an immutable set and publishes a `shared_ptr` to it;
    the socket thread takes one reference under a short mutex and reads it for
    as long as it likes. The set a datagram was judged against may be one tick
    old, which is correct - it was the rule when the datagram arrived.

    MATCHED ON THE ADDRESS ALONE, not the port. A device's source port is
    whatever the operating system gave it, changes when it reconnects, and is
    not what anybody typed into the show. Two boxes behind one NAT would share
    an entry here; that is a real limitation of the setting and not a bug in
    it - which is why the default is off.
*/

#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace wfg::osc
{
    /*  The rule, as one immutable value. Built on the tick thread, read on the
        socket thread, and never modified after it is published. */
    struct Allowed
    {
        /** False lets everybody in, which is what Go.dot has always done. */
        bool strict = false;

        /** The addresses of the declared devices whose `rx` is set. */
        std::set<std::string> hosts;
    };

    class SenderGate
    {
    public:
        /*  Publishes a new rule. Tick thread, once per tick, and only when the
            document moved - so the ordinary cost is one atomic compare. */
        void publish (std::shared_ptr<const Allowed> rule)
        {
            const std::lock_guard<std::mutex> lock { mutex };
            current = std::move (rule);
        }

        /*  Whether a datagram from this address is taken. Socket thread.

            No rule yet means yes: the gate is built before the first tick has
            published anything, and a show that refused everything for its first
            twenty milliseconds would be a race nobody could reproduce. */
        bool allows (const std::string& senderIp) const
        {
            std::shared_ptr<const Allowed> rule;

            {
                const std::lock_guard<std::mutex> lock { mutex };
                rule = current;
            }

            if (rule == nullptr || ! rule->strict)
                return true;

            return rule->hosts.find (senderIp) != rule->hosts.end();
        }

        /** Whether the gate is turned on at all, for a caller deciding whether
            to bother building a set. */
        bool isStrict() const
        {
            const std::lock_guard<std::mutex> lock { mutex };
            return current != nullptr && current->strict;
        }

    private:
        mutable std::mutex mutex;
        std::shared_ptr<const Allowed> current;
    };
}
