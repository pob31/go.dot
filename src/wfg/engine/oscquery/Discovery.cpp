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

#include <wfg/engine/oscquery/Discovery.h>

#include <wfg/engine/oscquery/MulticastJoin.h>

#include <juce_core/juce_core.h>

#include <algorithm>

namespace wfg::oscquery
{
    namespace
    {
        /*  A name as a key: dotted and in lower case, since DNS compares
            names without regard to case. Two instances whose names differ
            only in where a dot falls inside a label would share one - which
            no responder names twice. */
        std::string keyOf (const mdns::Name& name)
        {
            auto key = mdns::dotted (name);

            for (auto& c : key)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char> (c - 'A' + 'a');

            return key;
        }

        std::chrono::seconds secondsOf (std::uint32_t ttl)
        {
            return std::chrono::seconds (static_cast<std::chrono::seconds::rep> (ttl));
        }

        /*  THIS MACHINE'S IPv4 ADDRESSES, one an interface, the loopback left
            out: the interfaces the group is joined on and asked from. A link
            without a DHCP server - a Dante network, often - has an address
            in 169.254 and is kept: devices are found there too. */
        std::vector<std::string> ipv4Interfaces()
        {
            std::vector<std::string> out;

            for (const auto& address : juce::IPAddress::getAllAddresses (false))
            {
                /*  Windows lists the multicast groups it has joined among a
                    machine's addresses (serve's own client lines say so); a
                    group is not an interface. */
                if (address.isNull() || address.address[0] >= 224)
                    continue;

                const auto text = address.toString().toStdString();

                if (text.empty() || text.rfind ("127.", 0) == 0)
                    continue;

                out.push_back (text);
            }

            std::sort (out.begin(), out.end());
            out.erase (std::unique (out.begin(), out.end()), out.end());
            return out;
        }

        std::string linesOf (const std::vector<Discovery::Found>& found)
        {
            std::string text;

            for (const auto& each : found)
                text += (text.empty() ? "" : "\n") + each.name + "\t" + each.host + "\t" + std::to_string (each.queryPort);

            return text;
        }
    }

    Discovery::Discovery() = default;

    Discovery::~Discovery()
    {
        stop();
    }

    bool Discovery::start (Options optionsToUse)
    {
        if (running.load (std::memory_order_relaxed))
            return false;

        options = std::move (optionsToUse);
        service = mdns::nameOf (options.service);
        onGroup = false;

        std::string problemNow;

        /*  SHARED WITH THE SYSTEM'S RESPONDER: the constructor's address reuse
            is what shares a port on Windows and Linux, and port reuse is what
            does on macOS - set here, before the bind. */
        socket = std::make_unique<juce::DatagramSocket> (false);
        socket->setEnablePortReuse (true);

        if (options.port == mdns::port)
        {
            if (socket->bindToPort (mdns::port))
            {
                onGroup = true;
            }
            else
            {
                problemNow = "port 5353 is another program's alone: devices are asked from a port of Go.dot's own,"
                             " and Go.dot is not announced";
                socket = std::make_unique<juce::DatagramSocket> (false);

                if (! socket->bindToPort (0))
                {
                    say ("off", "no port to ask the network from");
                    socket.reset();
                    return false;
                }
            }
        }
        else if (! socket->bindToPort (options.port))
        {
            say ("off", "port " + std::to_string (options.port) + " could not be bound");
            socket.reset();
            return false;
        }

        bound.store (socket->getBoundPort(), std::memory_order_relaxed);
        interfaces.clear();
        openInterfaces();

        say (onGroup ? "browsing" : "unicast", problemNow);

        nextQuery = Clock::now();
        interval = std::chrono::seconds (1);
        stopping.store (false, std::memory_order_relaxed);
        running.store (true, std::memory_order_relaxed);
        thread = std::thread ([this] { run(); });
        return true;
    }

    void Discovery::stop()
    {
        if (! running.load (std::memory_order_relaxed))
            return;

        stopping.store (true, std::memory_order_relaxed);

        if (thread.joinable())
            thread.join();

        if (socket != nullptr)
            socket->shutdown();

        socket.reset();
        running.store (false, std::memory_order_relaxed);
    }

    void Discovery::browse()
    {
        browseWanted.store (true, std::memory_order_relaxed);
    }

    std::string Discovery::mode() const
    {
        const std::lock_guard<std::mutex> lock { guard };
        return modeWord;
    }

    std::string Discovery::problem() const
    {
        const std::lock_guard<std::mutex> lock { guard };
        return problemWords;
    }

    std::vector<Discovery::Found> Discovery::found() const
    {
        const std::lock_guard<std::mutex> lock { guard };
        return list;
    }

    std::string Discovery::foundText() const
    {
        const std::lock_guard<std::mutex> lock { guard };
        return linesOf (list);
    }

    void Discovery::say (std::string modeNow, std::string problemNow)
    {
        {
            const std::lock_guard<std::mutex> lock { guard };
            modeWord = std::move (modeNow);
            problemWords = std::move (problemNow);
        }

        stamp.fetch_add (1, std::memory_order_acq_rel);
    }

    /*  THE GROUP JOINED ON EVERY INTERFACE NOT JOINED YET - at the start, and
        every thirty seconds after, so a network plugged in during the evening
        is asked too. Where no interface could be joined by address, the
        system's default is. */
    void Discovery::openInterfaces()
    {
        const auto now = ipv4Interfaces();
        ownAddresses = now;
        ownAddresses.push_back ("127.0.0.1");

        if (! onGroup)
        {
            interfaces = now;
            return;
        }

        const auto handle = socket->getRawSocketHandle();
        multicast::setHops (handle, 255);

        auto joinedAny = ! interfaces.empty();

        for (const auto& address : now)
        {
            if (std::find (interfaces.begin(), interfaces.end(), address) != interfaces.end())
                continue;

            if (multicast::join (handle, mdns::group, address))
            {
                interfaces.push_back (address);
                joinedAny = true;
            }
        }

        if (! joinedAny)
            socket->joinMulticast (mdns::group);
    }

    void Discovery::ask (const std::vector<mdns::Question>& questions, bool toGroup)
    {
        if (questions.empty() || socket == nullptr)
            return;

        auto asked = questions;

        //  From a port other than 5353, answers come back to it directly (RFC 6762 §5.4, §6.7).
        for (auto& question : asked)
            question.unicastResponse = ! onGroup;

        const auto bytes = mdns::encodeQuery (asked);
        const auto size = static_cast<int> (bytes.size());

        if (toGroup && options.port == mdns::port)
        {
            if (onGroup && ! interfaces.empty())
            {
                for (const auto& address : interfaces)
                {
                    multicast::sendFrom (socket->getRawSocketHandle(), address);
                    socket->write (mdns::group, mdns::port, bytes.data(), size);
                }
            }
            else
            {
                socket->write (mdns::group, mdns::port, bytes.data(), size);
            }
        }

        for (const auto& [host, peerPort] : options.peers)
            socket->write (juce::String (host), peerPort, bytes.data(), size);
    }

    void Discovery::run()
    {
        std::vector<std::uint8_t> buffer (9000);
        auto lastSweep = Clock::now();
        auto lastInterfaces = Clock::now();

        while (! stopping.load (std::memory_order_relaxed))
        {
            const auto now = Clock::now();

            if (browseWanted.exchange (false, std::memory_order_relaxed))
            {
                nextQuery = now;
                interval = std::chrono::seconds (1);
            }

            /*  THE QUESTION, again and again less often: one, two, four
                seconds, up to a minute - a responder announces itself when it
                arrives, so the late question is the one for a quiet one. From
                a port of its own the answers last ten seconds at most, so the
                question is put every five. */
            if (now >= nextQuery)
            {
                ask ({ { service, mdns::type::ptr, false } }, true);
                nextQuery = now + (onGroup ? interval : std::chrono::seconds (5));
                interval = std::min<std::chrono::seconds> (interval * 2, std::chrono::seconds (60));
            }

            followUp (now);

            if (now - lastSweep >= std::chrono::seconds (1))
            {
                sweep (now);
                lastSweep = now;
            }

            if (now - lastInterfaces >= std::chrono::seconds (30))
            {
                openInterfaces();
                lastInterfaces = now;
            }

            if (socket->waitUntilReady (true, 100) != 1)
                continue;

            juce::String fromIp;
            int fromPort = 0;
            const auto got = socket->read (buffer.data(), static_cast<int> (buffer.size()), false, fromIp, fromPort);

            if (got > 0)
                heard (std::vector<std::uint8_t> (buffer.begin(), buffer.begin() + got), fromIp.toStdString(), fromPort);
        }
    }

    void Discovery::heard (const std::vector<std::uint8_t>& bytes, const std::string&, int)
    {
        const auto message = mdns::decode (bytes);

        if (! message || ! message->response)
            return;

        const auto now = Clock::now();
        const auto records = message->records();

        //  The instances first, so an SRV in the same message finds its own.
        for (const auto& record : records)
        {
            if (record.type != mdns::type::ptr || ! mdns::sameName (record.name, service) || record.target.empty())
                continue;

            const auto key = keyOf (record.target);

            if (record.ttl == 0)
            {
                instances.erase (key);
                continue;
            }

            auto& instance = instances[key];
            instance.name = record.target;
            instance.expires = now + secondsOf (record.ttl);
        }

        for (const auto& record : records)
        {
            if (record.type == mdns::type::srv || record.type == mdns::type::txt)
            {
                const auto match = instances.find (keyOf (record.name));

                if (match == instances.end())
                    continue;

                auto& instance = match->second;

                if (record.type == mdns::type::srv)
                {
                    instance.hasService = record.ttl > 0;
                    instance.host = record.target;
                    instance.port = record.port;
                    instance.serviceExpires = now + secondsOf (record.ttl);
                }
                else
                {
                    instance.txt = record.txt;
                }
            }
            else if (record.type == mdns::type::a)
            {
                const auto key = keyOf (record.name);

                if (record.ttl == 0)
                {
                    hosts.erase (key);
                    continue;
                }

                auto& host = hosts[key];
                host.address = record.addressText();
                host.expires = now + secondsOf (record.ttl);
            }
        }

        publish();
    }

    /*  WHAT IS STILL MISSING, ASKED FOR: an instance with no host or port yet,
        a host with no address - once a second each, together in one message. */
    void Discovery::followUp (Clock::time_point now)
    {
        std::vector<mdns::Question> questions;

        for (auto& [key, instance] : instances)
        {
            if (! instance.hasService)
            {
                if (now - instance.askedAt >= std::chrono::seconds (1))
                {
                    instance.askedAt = now;
                    questions.push_back ({ instance.name, mdns::type::srv, false });
                    questions.push_back ({ instance.name, mdns::type::txt, false });
                }

                continue;
            }

            const auto hostKey = keyOf (instance.host);

            if (const auto known = hosts.find (hostKey); known != hosts.end() && ! known->second.address.empty())
                continue;

            auto& host = hosts[hostKey];

            if (now - host.askedAt >= std::chrono::seconds (1))
            {
                host.askedAt = now;
                questions.push_back ({ instance.host, mdns::type::a, false });
            }
        }

        ask (questions, true);
    }

    void Discovery::sweep (Clock::time_point now)
    {
        for (auto at = instances.begin(); at != instances.end();)
        {
            if (at->second.expires <= now)
            {
                at = instances.erase (at);
                continue;
            }

            if (at->second.hasService && at->second.serviceExpires <= now)
                at->second.hasService = false;

            ++at;
        }

        for (auto at = hosts.begin(); at != hosts.end();)
        {
            if (! at->second.address.empty() && at->second.expires <= now)
                at = hosts.erase (at);
            else
                ++at;
        }

        publish();
    }

    /*  THE LIST, made again from what is known, and handed over only when it
        differs: complete instances, by name, this Go.dot left out. */
    void Discovery::publish()
    {
        std::vector<Found> now;

        for (const auto& [key, instance] : instances)
        {
            if (! instance.hasService || instance.port <= 0)
                continue;

            const auto host = hosts.find (keyOf (instance.host));

            if (host == hosts.end() || host->second.address.empty())
                continue;

            const auto own = options.ownPort > 0 && instance.port == options.ownPort
                          && std::find (ownAddresses.begin(), ownAddresses.end(), host->second.address) != ownAddresses.end();

            if (own)
                continue;

            Found entry;
            entry.name = instance.name.empty() ? key : instance.name.front();
            entry.host = host->second.address;
            entry.queryPort = instance.port;
            entry.txt = instance.txt;
            now.push_back (std::move (entry));
        }

        std::sort (now.begin(), now.end(), [] (const Found& a, const Found& b)
                   {
                       return a.name != b.name ? a.name < b.name : a.host < b.host;
                   });

        {
            const std::lock_guard<std::mutex> lock { guard };

            if (linesOf (now) == linesOf (list))
                return;

            list = std::move (now);
        }

        stamp.fetch_add (1, std::memory_order_acq_rel);
    }
}
