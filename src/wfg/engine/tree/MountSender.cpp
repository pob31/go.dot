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

#include <wfg/engine/tree/MountSender.h>

#include <wfg/engine/mqtt/Packet.h>
#include <wfg/engine/mqtt/Words.h>

#include <wfg/engine/osc/OscCodec.h>
#include <wfg/engine/osc/UdpEndpoint.h>
#include <wfg/engine/pair/Hold.h>
#include <wfg/engine/tree/Wires.h>
#include <wfg/engine/midi/MidiSink.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace wfg::tree
{
    std::uint64_t MountSender::queue (const std::string& mountId, const Destination& destination,
                                      const std::string& address, const osc::Values& values,
                                      const std::string& owner)
    {
        const auto ticket = nextTicket++;

        /*  LAST WRITE WINS, IN THE FIRST WRITE'S PLACE. Two things are being
            decided here and they pull in opposite directions, so both are
            stated: the VALUE is the newest, because sending a value somebody
            has already changed their mind about is sending a wrong number; the
            ORDER is the oldest, because a cue that sets a mode and then a
            parameter of that mode has to arrive in that order, and re-queueing
            a re-written address at the back would silently reverse it. */
        const auto existing = queuedAt.find (address);

        if (existing != queuedAt.end())
        {
            auto& message = queued[existing->second];

            /*  The superseded message is answered now rather than left pending.
                Nothing will ever flush it, and a cue waiting on a ticket that
                cannot arrive is a cue that hangs - the exact failure a
                three-valued wait exists to make visible. It reports SENT and
                not failed: what the caller asked for was that this address
                reach the target this tick, and it will. */
            answer (message, Outcome::sent, false);

            message.ticket = ticket;
            message.mountId = mountId;
            message.destination = destination;
            message.values = values;
            message.owner = owner;

            if (! owner.empty())
                ++tallies[owner].waiting;

            return ticket;
        }

        queuedAt[address] = queued.size();
        queued.push_back (Message { ticket, mountId, destination, address, values, owner, false });

        if (! owner.empty())
            ++tallies[owner].waiting;

        return ticket;
    }

    std::uint64_t MountSender::queueQuery (const std::string& mountId, const Destination& destination,
                                           const std::string& address)
    {
        const auto ticket = nextTicket++;
        const auto key = "?" + address;
        const auto existing = queuedAt.find (key);

        if (existing != queuedAt.end())
        {
            auto& message = queued[existing->second];
            answer (message, Outcome::sent, false);
            message.ticket = ticket;
            message.mountId = mountId;
            message.destination = destination;
            return ticket;
        }

        queuedAt[key] = queued.size();
        queued.push_back (Message { ticket, mountId, destination, address, {}, {}, true });
        return ticket;
    }

    //==============================================================================
    namespace
    {
        /*  How many flushes must pass between two sends of one address.

            A flush is a tick and a tick is a fiftieth of a second, so a cap of
            fifty hertz is one flush - which every address already obeys, since
            the queue holds one message per address. Below fifty it waits; above
            it, and at nought, it does not. */
        std::uint64_t intervalFor (double rateCap) noexcept
        {
            if (! (rateCap > 0.0) || rateCap >= 50.0)
                return 1;

            return static_cast<std::uint64_t> (std::llround (50.0 / rateCap));
        }
    }

    /*  THE ADDRESS AS IT LEAVES (namespace draft 100, AYK): without the
        device's own prefix when it is one Go.dot named it under - `/go` for
        a cue aimed at `/MacMini_Justice1/go` - and as written otherwise. */
    std::string MountSender::onTheWire (const Message& message)
    {
        const auto& prefix = message.destination.removedPrefix;

        if (prefix.empty() || message.address.rfind (prefix, 0) != 0)
            return message.address;

        const auto rest = message.address.substr (prefix.size());

        if (rest.empty())
            return "/";

        return rest.front() == '/' ? rest : message.address;
    }

    void MountSender::flush()
    {
        ++flushes;

        /*  WHAT COULD NOT GO YET, kept in its place in the order.

            A capped address holds the newest value it was given and waits; the
            order it waits in is the order it was first queued in, which is the
            same rule the coalescing above follows and for the same reason - a
            cue that sets a mode and then a parameter of that mode has to arrive
            in that order. And a message past its device's limit for this flush
            (namespace draft §88, AUT) waits the same way, in the same place. */
        std::vector<bool> stays (queued.size(), false);

        /*  HOW MANY DATAGRAMS EACH DEVICE HAS BEEN GIVEN THIS FLUSH, against its
            `perTick` - copied onto every message at queue time, so each is
            judged by the limit its device had when it was written. */
        std::map<std::string, int> given;

        const auto roomFor = [&given] (const Message& message)
        {
            const auto limit = message.destination.perTick;
            return limit <= 0 || given[message.mountId] < limit;
        };

        const auto keyOf = [] (const Message& message)
        {
            return message.query ? "?" + message.address : message.address;
        };

        /*  WHO IS BEHIND, counted again by this flush. */
        for (auto& entry : tallies)
            entry.second.behind = 0;

        const auto holdBack = [this] (const Message& message)
        {
            if (const auto tally = tallies.find (message.owner); ! message.owner.empty() && tally != tallies.end())
                ++tally->second.behind;
        };

        /*  WHAT GOES TO A DEVICE THAT TAKES BUNDLES (namespace draft 45, YQ),
            gathered by device in the order its first message was queued, and
            sent once the rest have gone - their indexes in `queued`. */
        std::vector<std::pair<std::string, std::vector<std::size_t>>> bundled;

        for (std::size_t index = 0; index < queued.size(); ++index)
        {
            const auto& message = queued[index];
            const auto interval = intervalFor (message.destination.rateCap);
            const auto key = keyOf (message);
            const auto last = lastSentAt.find (key);

            /*  A MESSAGE THE RATE CAP HOLDS STAYS OUT OF THE BUNDLE, in its
                place in the queue: it goes in a later flush's. */
            if (interval > 1 && last != lastSentAt.end()
                 && flushes - last->second < interval)
            {
                stays[index] = true;
                continue;
            }

            /*  AND ONLY THE OSC WIRE BUNDLES (AFJ): a line of text has no
                bundle to travel in, whatever the device's row says. */
            if (message.destination.bundles && message.destination.wire == "osc" && ! message.query)
            {
                auto device = std::find_if (bundled.begin(), bundled.end(),
                                            [&message] (const auto& entry) { return entry.first == message.mountId; });

                if (device == bundled.end())
                {
                    bundled.push_back ({ message.mountId, {} });
                    device = std::prev (bundled.end());
                }

                device->second.push_back (index);
                continue;
            }

            if (! roomFor (message))
            {
                stays[index] = true;
                holdBack (message);
                continue;
            }

            lastSentAt[key] = flushes;
            ++given[message.mountId];
            sendAlone (message);
        }

        for (const auto& [mountId, indexes] : bundled)
        {
            std::vector<const Message*> messages;
            messages.reserve (indexes.size());

            for (const auto index : indexes)
                messages.push_back (&queued[index]);

            const auto limit = messages.front()->destination.perTick;
            const auto allowed = limit <= 0 ? std::numeric_limits<int>::max()
                                            : std::max (0, limit - given[mountId]);
            const auto went = sendBundled (messages, allowed);

            given[mountId] += went.datagrams;

            for (std::size_t at = 0; at < indexes.size(); ++at)
            {
                if (at < went.messages)
                {
                    lastSentAt[keyOf (queued[indexes[at]])] = flushes;
                }
                else
                {
                    stays[indexes[at]] = true;
                    holdBack (queued[indexes[at]]);
                }
            }
        }

        std::vector<Message> waiting;

        for (std::size_t index = 0; index < queued.size(); ++index)
            if (stays[index])
                waiting.push_back (std::move (queued[index]));

        queued = std::move (waiting);
        queuedAt.clear();

        for (std::size_t index = 0; index < queued.size(); ++index)
            queuedAt[keyOf (queued[index])] = index;

        forgetOldAnswers();
    }

    bool MountSender::deliver (const Destination& to, const std::vector<std::uint8_t>& bytes)
    {
        /*  HELD, AND SENT (namespace draft §58, RD.1; AGG): first, before a
            sink is chosen, so a device whose serial port this machine has not
            got, whose connection is closed - a backup's always are (AGW) - or
            whose port is nought still answers its ticket sent, and a `sent`
            wait ends `done` here as it does on the machine that sent. Every
            write and every question of every wire passes this line but the
            MIDI wire's own port, which `sendAlone` holds itself. */
        if (pair::isHeld (hold))
            return true;

        if (! to.serial.empty())
            return serialSink ? serialSink (to.serial, bytes) : false;

        if (! to.link.empty())
            return linkSink ? linkSink (to.link, bytes) : false;

        return udp != nullptr && to.port > 0 && udp->send (to.host, to.port, bytes);
    }

    namespace
    {
        /*  A TEMPLATE'S ATOMS (DP.10): after the address, each word an atom -
            `{address}` the node asked about, a whole number an int32, a number
            with a dot a float32, anything else a string. */
        osc::Values atomsOf (const std::vector<std::string>& words, const std::string& asked)
        {
            osc::Values out;

            for (std::size_t at = 1; at < words.size(); ++at)
            {
                const auto& word = words[at];

                if (word == "{address}")
                    out.push_back (osc::Value::string (asked));
                else if (! word.empty() && std::all_of (word.begin(), word.end(), [] (unsigned char c) { return std::isdigit (c) != 0; }))
                    out.push_back (osc::Value::int32 (std::atoi (word.c_str())));
                else if (word.find ('.') != std::string::npos && osc::parseDouble (word).has_value())
                    out.push_back (osc::Value::float32 (static_cast<float> (*osc::parseDouble (word))));
                else
                    out.push_back (osc::Value::string (word));
            }

            return out;
        }

        /*  A SHURE RECEIVER'S FRAME (namespace draft §124, BER): the address
            with the device's prefix taken off, spelled as the node says. An
            address under none of its prefixes renders nothing. */
        std::string shureFrameOf (const MountTable* mounts, const std::string& mountId, const std::string& address,
                                  const osc::Values& values, bool asking)
        {
            const auto* declaration = mounts != nullptr ? mounts->declarationOf (mountId) : nullptr;
            const auto covered = declaration != nullptr ? prefixMatchLength (address, declaration->prefix) : 0;

            if (covered == 0)
                return {};

            wire::ShureSpec spec;

            if (const auto* node = mounts->nodeAt (address); node != nullptr)
            {
                spec.offset = node->shureOffset();
                spec.braces = node->shureBraces();
                spec.pad = node->shurePad();
            }

            return wire::renderShure (address.substr (covered), values, spec, asking);
        }

        /*  AND A SENNHEISER RECEIVER'S MESSAGE (§124, BES): the address under
            the prefix as the JSON's path. Under none of its prefixes, nothing. */
        std::string sscMessageOf (const MountTable* mounts, const std::string& mountId, const std::string& address,
                                  const osc::Values& values, bool asking)
        {
            const auto* declaration = mounts != nullptr ? mounts->declarationOf (mountId) : nullptr;
            const auto covered = declaration != nullptr ? prefixMatchLength (address, declaration->prefix) : 0;
            return covered == 0 ? std::string {} : wire::renderSsc (address.substr (covered), values, asking);
        }

        /*  And a base station's request (§124, BET), the prefix off. A PUT of
            an element of a collection carries its identifier, read from the
            address - a pack's `mtUid` is the segment after the collection -
            and the file's CARRY fields as the device last said them: a pack's
            `type`. One the device has not said yet is left out, and the base
            station's refusal says why. */
        std::string sscV2RequestOf (const MountTable* mounts, const std::string& mountId, const std::string& address,
                                    const osc::Values& values, bool asking)
        {
            const auto* declaration = mounts != nullptr ? mounts->declarationOf (mountId) : nullptr;
            const auto covered = declaration != nullptr ? prefixMatchLength (address, declaration->prefix) : 0;

            if (covered == 0)
                return {};

            auto relative = address.substr (covered);

            /*  A FIELD PUT TO ANOTHER RESOURCE than the one it is read in - an
                EW-DX channel's frequency - by the node's own word, before what
                the PUT carries is worked out for it. A question still asks the
                resource it is read in. */
            if (const auto* node = mounts->nodeAt (address); ! asking && node != nullptr && ! node->sscResource().empty())
                if (const auto field = relative.rfind ('/'); field != std::string::npos)
                    relative = node->sscResource() + relative.substr (field);

            const auto cut = relative.rfind ('/');
            const auto resource = cut == std::string::npos ? std::string {} : relative.substr (0, cut);
            std::vector<std::pair<std::string, osc::Value>> carry;

            if (! asking && ! resource.empty())
            {
                const auto carried = mounts->carryFieldsOf (mountId);

                for (const auto& [collection, idField] : mounts->collectionIdsOf (mountId))
                {
                    if (resource.rfind (collection + "/", 0) != 0)
                        continue;

                    const auto id = resource.substr (collection.size() + 1);

                    if (id.empty() || id.find ('/') != std::string::npos)
                        continue;

                    const auto whole = std::all_of (id.begin(), id.end(), [] (unsigned char c) { return std::isdigit (c) != 0; });
                    carry.emplace_back (idField, whole && id.size() < 19 ? osc::Value::int64 (std::stoll (id))
                                                                        : osc::Value::string (id));

                    if (const auto fields = carried.find (collection); fields != carried.end())
                        for (const auto& field : fields->second)
                            if (const auto* said = mounts->observedOf (address.substr (0, covered) + resource + "/" + field);
                                said != nullptr && ! said->empty())
                                carry.emplace_back (field, said->front());
                }
            }

            return wire::renderSscV2 (relative, values, asking, carry);
        }

        std::vector<std::string> wordsOf (const std::string& text)
        {
            std::vector<std::string> out;
            std::string current;

            for (const char c : text)
            {
                if (c == ' ' || c == '\t')
                {
                    if (! current.empty())
                        out.push_back (current);
                    current.clear();
                }
                else
                {
                    current.push_back (c);
                }
            }

            if (! current.empty())
                out.push_back (current);

            return out;
        }
    }

    void MountSender::sendAlone (const Message& message)
    {
        auto ok = false;
        std::string error;

        /*  A QUESTION (DP.10), by the wire: the rcp wire's `get` with the
            node's indexes and no value; the osc wire's bare address, or the
            file's GET template with the address as its atom. Other wires have
            no question to ask. */
        if (message.query)
        {
            if (message.destination.wire == "rcp")
            {
                wire::RcpSpec spec;
                spec.verb = "get";

                if (const auto* node = mounts != nullptr ? mounts->nodeAt (message.address) : nullptr; node != nullptr)
                    spec.indexes = node->rcpIndexes();

                const auto line = wire::renderRcp (message.address, {}, spec);
                ok = deliver (message.destination, std::vector<std::uint8_t> (line.begin(), line.end()));
            }
            else if (message.destination.wire == "shure")
            {
                //  A Shure receiver is asked with GET (§124, BER).
                const auto frame = shureFrameOf (mounts, message.mountId, message.address, {}, true);
                ok = ! frame.empty() && deliver (message.destination, std::vector<std::uint8_t> (frame.begin(), frame.end()));
            }
            else if (message.destination.wire == "sscv2")
            {
                //  A base station is asked with a GET of the resource (§124, BET).
                const auto text = sscV2RequestOf (mounts, message.mountId, message.address, {}, true);
                ok = ! text.empty() && deliver (message.destination, std::vector<std::uint8_t> (text.begin(), text.end()));
            }
            else if (message.destination.wire == "ssc")
            {
                //  A Sennheiser receiver is asked with null (§124, BES).
                const auto text = sscMessageOf (mounts, message.mountId, message.address, {}, true);
                ok = ! text.empty() && deliver (message.destination, std::vector<std::uint8_t> (text.begin(), text.end()));
            }
            else if (message.destination.wire == "osc")
            {
                const auto words = wordsOf (mounts != nullptr ? mounts->getTemplateOf (message.mountId) : std::string {});
                const auto address = words.empty() ? message.address : words[0];
                const auto values = words.empty() ? osc::Values {} : atomsOf (words, message.address);

                if (const auto bytes = osc::encode (osc::Packet::message (address, values), error))
                    ok = deliver (message.destination, *bytes);
            }

            answer (message, ok ? Outcome::sent : Outcome::failed, false);
            return;
        }

        /*  Encoded here rather than at queue time, because a coalesced address
            is encoded once however many times it was written - which is the
            point of coalescing, and would be lost if the bytes were built where
            the value arrived. */
        if (message.destination.wire == "rcp")
        {
            /*  THE RCP WIRE (namespace draft §57, AFJ; DP.7): the line of
                Yamaha's protocol, spelled as the node says where the table
                holds one - the verb, how many trailing segments are X and Y
                - and inferred from the address otherwise. The bytes go to the
                link, which reads lines and adds the newline. */
            wire::RcpSpec spec;

            if (const auto* node = mounts != nullptr ? mounts->nodeAt (message.address) : nullptr; node != nullptr)
            {
                if (! node->rcpVerb().empty())
                    spec.verb = node->rcpVerb();

                spec.indexes = node->rcpIndexes();
            }

            const auto line = wire::renderRcp (message.address, message.values, spec);
            ok = deliver (message.destination, std::vector<std::uint8_t> (line.begin(), line.end()));
        }
        else if (message.destination.wire == "shure")
        {
            /*  THE SHURE WIRE (namespace draft §124, BER): a SET frame, the
                prefix off, the value spelled as the node says; to the link,
                whose framing adds nothing after it. */
            const auto frame = shureFrameOf (mounts, message.mountId, message.address, message.values, false);
            ok = ! frame.empty() && deliver (message.destination, std::vector<std::uint8_t> (frame.begin(), frame.end()));
        }
        else if (message.destination.wire == "sscv2")
        {
            /*  THE SSCV2 WIRE (namespace draft §124, BET): a PUT of the
                resource with the one field, handed to the base station's
                worker as its request. */
            const auto text = sscV2RequestOf (mounts, message.mountId, message.address, message.values, false);
            ok = ! text.empty() && deliver (message.destination, std::vector<std::uint8_t> (text.begin(), text.end()));
        }
        else if (message.destination.wire == "ssc")
        {
            /*  THE SSC WIRE (namespace draft §124, BES): the address under the
                prefix as the JSON's path, the value at its end; a datagram of
                its own down the device's socket. */
            const auto text = sscMessageOf (mounts, message.mountId, message.address, message.values, false);
            ok = ! text.empty() && deliver (message.destination, std::vector<std::uint8_t> (text.begin(), text.end()));
        }
        else if (message.destination.wire == "midi")
        {
            /*  THE MIDI WIRE (DP.9): the node's shape rendered to as many
                messages as it takes, in order, each to the declared port in
                the run's name, or down the link or the datagram as bytes. A
                node the table lacks has no shape and nothing leaves. */
            const auto* node = mounts != nullptr ? mounts->nodeAt (message.address) : nullptr;

            if (node != nullptr && ! node->midi().kind.empty())
            {
                const auto messages = wire::renderMidi (node->midi(), message.values, message.destination.midiChannel,
                                                        message.destination.mscDevice, message.destination.mscFormat);
                ok = ! messages.empty();

                /*  A DECLARED MIDI PORT skips `deliver`, so the hold is asked
                    here too (RD.1): a held machine hands the port nothing and
                    counts it sent, whether or not a sink or a cable is there -
                    the MIDI sender would hold it as well, and this does not
                    lean on it. */
                for (const auto& bytes : messages)
                {
                    if (! message.destination.midiPort.empty())
                        ok = ok && (pair::isHeld (hold)
                                    || (midiSink != nullptr
                                        && midiSink->sendForRun (message.owner, message.destination.midiPort, bytes).empty()));
                    else
                        ok = ok && deliver (message.destination, bytes);
                }
            }
        }
        else if (message.destination.wire == "line")
        {
            /*  THE LINE WIRE (DP.8): the node's command line from its
                template, or the atoms as they are, and a carriage return -
                the link adds the newline, so the console reads CR LF as a
                telnet line ends. */
            std::string templateText;

            if (const auto* node = mounts != nullptr ? mounts->nodeAt (message.address) : nullptr; node != nullptr)
                templateText = node->lineTemplate();

            const auto line = wire::renderLine (message.address, message.values, templateText) + "\r";
            ok = deliver (message.destination, std::vector<std::uint8_t> (line.begin(), line.end()));
        }
        else if (message.destination.wire == "mqtt")
        {
            /*  THE MQTT WIRE (namespace draft §67, ANA, ANC, ANF; MQ.4): a
                publish on the topic the address spells under the device's
                prefix, its payload the values joined by spaces, QoS 0, not
                retained; down the link to the broker, whose session the
                sink asks first. An address not under the prefix, or a
                device the table does not hold, sends nothing and fails. */
            const auto* declaration = mounts != nullptr ? mounts->declarationOf (message.mountId) : nullptr;
            const auto topic = declaration != nullptr ? mqtt::topicOfAddress (declaration->prefix, message.address)
                                                      : std::nullopt;

            if (topic.has_value())
            {
                std::string problem;
                if (const auto bytes = mqtt::encode (mqtt::Packet::publishOf (*topic, mqtt::payloadOfValues (message.values)), problem))
                    ok = deliver (message.destination, *bytes);
            }
        }
        else if (const auto bytes = osc::encode (osc::Packet::message (onTheWire (message), message.values), error))
        {
            ok = deliver (message.destination, *bytes);
        }

        if (ok)
            ++sent[message.mountId];

        answer (message, ok ? Outcome::sent : Outcome::failed, false);
    }

    MountSender::Bundled MountSender::sendBundled (const std::vector<const Message*>& messages, int datagramsAllowed)
    {
        /*  ONE DEVICE'S MESSAGES OF ONE FLUSH, in their order, as few bundles as
            fit (namespace draft 45, ZB): each closed before it would pass
            `bundleBytes`, time-tagged immediately - Go.dot's own processors
            would be the ones to read a later time, and spatcore's parser drops
            it (PRD 3.12). A message too big to share a datagram goes alone, as
            a plain message, and a bundle of one is still a bundle: the device
            said it reads them. Each message is answered and counted as itself.

            AND NO MORE DATAGRAMS THAN THE DEVICE IS GIVEN THIS FLUSH (namespace
            draft §88, AUT): the messages from the first that would need one
            more wait, in their order, for the next. */
        constexpr std::size_t header = 16;      // "#bundle\0" and the time tag
        constexpr std::size_t lengthField = 4;  // each element's size, before it

        Bundled went;
        std::vector<const Message*> batch;
        std::vector<osc::Packet> elements;
        std::size_t size = header;

        const auto close = [this, &batch, &elements, &size, &went]
        {
            if (batch.empty())
                return;

            auto ok = false;
            std::string error;
            const auto& to = batch.front()->destination;

            if (const auto bytes = osc::encode (osc::Packet::bundle (osc::TimeTag {}, elements), error))
                ok = deliver (to, *bytes);

            for (const auto* message : batch)
            {
                if (ok)
                    ++sent[message->mountId];

                answer (*message, ok ? Outcome::sent : Outcome::failed, false);
            }

            ++went.datagrams;
            batch.clear();
            elements.clear();
            size = header;
        };

        for (const auto* message : messages)
        {
            auto element = osc::Packet::message (onTheWire (*message), message->values);
            std::string error;
            const auto bytes = osc::encode (element, error);
            const auto length = bytes.has_value() ? bytes->size() : 0u;

            if (! bytes.has_value() || header + lengthField + length > bundleBytes)
            {
                close();

                if (went.datagrams >= datagramsAllowed)
                    break;

                sendAlone (*message);
                ++went.datagrams;
                ++went.messages;
                continue;
            }

            if (size + lengthField + length > bundleBytes)
                close();

            //  A new datagram, and none left to give.
            if (batch.empty() && went.datagrams >= datagramsAllowed)
                break;

            elements.push_back (std::move (element));
            batch.push_back (message);
            size += lengthField + length;
            ++went.messages;
        }

        close();
        return went;
    }

    std::size_t MountSender::dropQueued (const std::function<bool (const std::string& owner)>& keep)
    {
        /*  WHAT IS KEPT KEEPS ITS PLACE, for the reason the coalescing keeps
            the first write's: a pre-send that sets a mode and then a parameter
            of that mode still arrives in that order. */
        std::vector<Message> kept;
        std::size_t dropped = 0;

        for (auto& message : queued)
        {
            if (keep && keep (message.owner))
            {
                kept.push_back (std::move (message));
                continue;
            }

            /*  ANSWERED NOW, as a superseded message is: nothing will ever
                flush it, and a cue waiting on a ticket that cannot arrive is a
                cue that hangs. `failed`, because it did not reach the target;
                marked dropped, because nothing went wrong on the wire. */
            answer (message, Outcome::failed, true);
            ++dropped;
        }

        queued = std::move (kept);
        queuedAt.clear();

        for (std::size_t index = 0; index < queued.size(); ++index)
            queuedAt[queued[index].query ? "?" + queued[index].address : queued[index].address] = index;

        //  What is kept is judged again by the next flush (§88).
        for (auto& entry : tallies)
            entry.second.behind = 0;

        forgetOldAnswers();
        return dropped;
    }

    void MountSender::answer (const Message& message, Outcome outcome, bool dropped)
    {
        outcomes.push_back ({ message.ticket, outcome, dropped });

        if (message.owner.empty())
            return;

        /*  A run whose count was forgotten while something of it still waited
            - the Runner forgets only when nothing does - is not counted again
            from nought here: it would read a message settled that it never saw
            queued. */
        const auto tally = tallies.find (message.owner);

        if (tally == tallies.end())
            return;

        if (tally->second.waiting > 0)
            --tally->second.waiting;

        if (dropped)
            ++tally->second.dropped;
        else if (outcome == Outcome::sent)
            ++tally->second.sent;
        else
            ++tally->second.failed;
    }

    MountSender::Tally MountSender::tallyOf (const std::string& owner) const
    {
        const auto found = tallies.find (owner);
        return found == tallies.end() ? Tally {} : found->second;
    }

    void MountSender::forgetTally (const std::string& owner)
    {
        if (const auto found = tallies.find (owner); found != tallies.end() && found->second.waiting == 0)
            tallies.erase (found);
    }

    void MountSender::forgetOldAnswers()
    {
        while (outcomes.size() > outcomesKept)
            outcomes.pop_front();
    }

    bool MountSender::stillQueued (std::uint64_t ticket) const
    {
        return std::any_of (queued.begin(), queued.end(),
                            [ticket] (const Message& message)
                            {
                                return message.ticket == ticket;
                            });
    }

    //==============================================================================
    MountSender::Outcome MountSender::outcomeOf (std::uint64_t ticket) const
    {
        /*  From the back, because the answer a caller wants is almost always
            the most recent flush's - a cue asks one tick after it queued. */
        for (auto entry = outcomes.rbegin(); entry != outcomes.rend(); ++entry)
            if (entry->ticket == ticket)
                return entry->outcome;

        /*  Not flushed yet, or flushed so long ago that the answer has been
            dropped. `pending` is the honest word for both: this object no
            longer knows, and a caller still asking about a ticket five hundred
            messages old has a bug of its own. */
        return Outcome::pending;
    }

    bool MountSender::wasDropped (std::uint64_t ticket) const
    {
        for (auto entry = outcomes.rbegin(); entry != outcomes.rend(); ++entry)
            if (entry->ticket == ticket)
                return entry->dropped;

        return false;
    }

    std::size_t MountSender::sentFor (const std::string& mountId) const
    {
        const auto found = sent.find (mountId);
        return found == sent.end() ? 0u : found->second;
    }

    //==============================================================================
    MountSender::Destination MountSender::destinationFor (const MountDeclaration& declaration)
    {
        Destination destination;
        destination.host = declaration.host;
        destination.port = declaration.port;
        destination.rateCap = declaration.rateCap;
        destination.bundles = declaration.bundles;
        destination.perTick = declaration.perTick;
        destination.serial = declaration.transport == "serial" ? declaration.serial : std::string {};
        /*  A device on the ssc wire has a socket of its own (§124, BES), so
            it is reached as a link is, by its identifier. */
        destination.link = declaration.transport == "tcp" || declaration.transport == "mqtt" || declaration.wire == "ssc"
                                || declaration.wire == "sscv2"
                               ? declaration.id : std::string {};
        destination.wire = declaration.wire.empty() ? std::string ("osc") : declaration.wire;
        destination.midiPort = declaration.transport == "midi" ? declaration.midiPort : std::string {};
        destination.midiChannel = declaration.midiChannel;
        destination.mscDevice = declaration.mscDevice;
        destination.mscFormat = declaration.mscFormat;

        if (declaration.removePrefix)
        {
            const auto prefixes = prefixesOf (declaration.prefix);
            destination.removedPrefix = prefixes.empty() ? std::string {} : prefixes.front();
        }

        return destination;
    }

    MountTable::WriteResult writeToDevice (MountTable& mounts, MountSender& sender,
                                           const std::string& address, const osc::Values& values)
    {
        const auto written = mounts.write (address, values);

        if (! written.ok)
            return written;

        if (const auto* declaration = mounts.declarationOf (written.mountId); declaration != nullptr && declaration->tx)
            sender.queue (written.mountId, MountSender::destinationFor (*declaration), address, written.values);

        return written;
    }
}
