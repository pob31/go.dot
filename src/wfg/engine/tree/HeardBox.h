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
    WHAT A DEVICE SAYS, HEARD (namespace draft 45, O.8, YZ).

    A device whose `rx` is on is heard: a datagram from its host, at an address
    under its own prefix, is the device reporting a value - a source dragged on
    WFS-DIY's map, a fader moved on a desk - and is kept as what the device
    said. Until this, such a datagram was taken as a WRITE: stored as the value
    Go.dot wrote and sent straight back to the device that had just said it.
    Every other sender - a tablet relaying `/wfs/...` through Go.dot - keeps
    that road, written and forwarded.

    TWO THREADS, THE TRIGGER INDEX'S SHAPE. The tick thread builds the rule -
    which host is which device, under which prefixes - and publishes it whole;
    the socket thread takes one reference to judge a datagram against. What it
    keeps is the newest value per address, and the tick thread takes them out
    once a tick as `mount.heard` records - so a device pushing thirty values a
    second for each of forty sources puts at most a tick's worth into the
    engine's queue, never a flood that pushes a GO out of it (ZK).
*/

#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/Mount.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace wfg::tree
{
    /*  Who is heard, as one immutable value: each host of a device with `rx`
        on, and that device's identifier and prefix row. */
    struct HeardRule
    {
        struct Device
        {
            std::string mountId;
            std::string prefixRow;
        };

        std::map<std::string, std::vector<Device>> byHost;
    };

    class HeardBox
    {
    public:
        /*  A new rule. Tick thread, when the show changed. Values heard under
            the old one stay until they are taken. */
        void publish (std::shared_ptr<const HeardRule> rule)
        {
            const std::lock_guard<std::mutex> lock { guard };
            current = std::move (rule);
        }

        /*  WHETHER THIS DATAGRAM IS A DEVICE'S OWN REPORT, and if it is, kept.
            Socket thread. The device whose prefix covers more of the address
            wins, as `MountTable::mountOf` decides it; a host that is two
            devices is heard as whichever the address belongs to. */
        bool take (const std::string& senderIp, const std::string& address, const osc::Values& values)
        {
            const std::lock_guard<std::mutex> lock { guard };

            if (current == nullptr)
                return false;

            const auto host = current->byHost.find (senderIp);

            if (host == current->byHost.end())
                return false;

            std::size_t best = 0;
            const HeardRule::Device* owner = nullptr;

            for (const auto& device : host->second)
                if (const auto length = prefixMatchLength (address, device.prefixRow); length > best)
                {
                    best = length;
                    owner = &device;
                }

            if (owner == nullptr)
                return false;

            auto& kept = waiting[address];
            kept.mountId = owner->mountId;
            kept.address = address;
            kept.values = values;
            kept.serial = ++serials;
            ++total;
            return true;
        }

        /*  A REPORT FROM A DEVICE ALREADY KNOWN (O.10): a value pushed down the
            socket Go.dot opened to that device's OSCQuery server, which says
            whose it is better than any host could. Kept as `take` keeps one.
            The listener's thread. */
        void takeFrom (const std::string& mountId, const std::string& address, const osc::Values& values)
        {
            const std::lock_guard<std::mutex> lock { guard };
            auto& kept = waiting[address];
            kept.mountId = mountId;
            kept.address = address;
            kept.values = values;
            kept.serial = ++serials;
            ++total;
        }

        struct Heard
        {
            std::string mountId;
            std::string address;
            osc::Values values;
            std::uint64_t serial = 0;
        };

        /*  WHAT WAS HEARD SINCE THE LAST TAKE, the newest per address, in the
            order it was heard, at most `most` - the rest wait for the next
            tick, newest still winning. Tick thread. */
        std::vector<Heard> drain (std::size_t most)
        {
            const std::lock_guard<std::mutex> lock { guard };

            std::vector<Heard> out;
            out.reserve (std::min (most, waiting.size()));

            for (const auto& [address, heard] : waiting)
                out.push_back (heard);

            std::sort (out.begin(), out.end(), [] (const Heard& a, const Heard& b) { return a.serial < b.serial; });

            if (out.size() > most)
                out.resize (most);

            for (const auto& heard : out)
                waiting.erase (heard.address);

            return out;
        }

        /** How many reports have been heard, for a readout and a test. */
        std::uint64_t heardSoFar() const
        {
            const std::lock_guard<std::mutex> lock { guard };
            return total;
        }

    private:
        mutable std::mutex guard;
        std::shared_ptr<const HeardRule> current;
        std::map<std::string, Heard> waiting;
        std::uint64_t serials = 0;
        std::uint64_t total = 0;
    };
}
