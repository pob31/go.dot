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

#include <wfg/engine/mqtt/Packet.h>

#include <algorithm>

namespace wfg::mqtt
{
    namespace
    {
        constexpr std::size_t largestString = 65535;

        /*  A READER OVER THE VARIABLE HEADER AND PAYLOAD: every take checks
            what is left, and a short read is `truncated`, never a read past
            the end. */
        struct Reader
        {
            const std::uint8_t* at;
            const std::uint8_t* end;
            std::string& problem;

            bool fail (const char* why)
            {
                if (problem.empty())
                    problem = why;
                return false;
            }

            std::size_t left() const noexcept { return static_cast<std::size_t> (end - at); }

            bool byte (std::uint8_t& out)
            {
                if (left() < 1)
                    return fail (reason::truncated);
                out = *at++;
                return true;
            }

            bool twoBytes (std::uint16_t& out)
            {
                if (left() < 2)
                    return fail (reason::truncated);
                out = static_cast<std::uint16_t> ((static_cast<std::uint16_t> (at[0]) << 8) | at[1]);
                at += 2;
                return true;
            }

            /*  A length-prefixed string. The specification asks for UTF-8
                without NUL; a NUL is refused here and the rest is left to
                whoever reads the words, as an OSC string's are. */
            bool string (std::string& out)
            {
                std::uint16_t length = 0;
                if (! twoBytes (length))
                    return false;
                if (left() < length)
                    return fail (reason::truncated);
                out.assign (reinterpret_cast<const char*> (at), length);
                at += length;
                if (out.find ('\0') != std::string::npos)
                    return fail (reason::badString);
                return true;
            }

            bool packetId (std::uint16_t& out)
            {
                if (! twoBytes (out))
                    return false;
                if (out == 0)
                    return fail (reason::badPacketId);
                return true;
            }
        };

        void putTwo (std::vector<std::uint8_t>& out, std::uint16_t value)
        {
            out.push_back (static_cast<std::uint8_t> (value >> 8));
            out.push_back (static_cast<std::uint8_t> (value & 0xFFu));
        }

        bool putString (std::vector<std::uint8_t>& out, const std::string& text, std::string& problem)
        {
            if (text.size() > largestString)
            {
                problem = reason::tooLong;
                return false;
            }
            putTwo (out, static_cast<std::uint16_t> (text.size()));
            out.insert (out.end(), text.begin(), text.end());
            return true;
        }

        /*  The flags the fixed header must carry for every type but PUBLISH
            (§2.2.2): PUBREL, SUBSCRIBE and UNSUBSCRIBE 0010, the rest 0000. */
        std::uint8_t fixedFlagsOf (Type type) noexcept
        {
            switch (type)
            {
                case Type::pubrel:
                case Type::subscribe:
                case Type::unsubscribe: return 0x02;
                case Type::connect:
                case Type::connack:
                case Type::publish:
                case Type::puback:
                case Type::pubrec:
                case Type::pubcomp:
                case Type::suback:
                case Type::unsuback:
                case Type::pingreq:
                case Type::pingresp:
                case Type::disconnect: return 0x00;
            }
            return 0x00;
        }

        bool decodeConnect (Reader& in, Packet& packet)
        {
            std::string protocolName;
            if (! in.string (protocolName))
                return false;

            if (! in.byte (packet.protocolLevel))
                return false;

            /*  3.1.1 is `MQTT` at level 4; 3.1 is `MQIsdp` at level 3. Any
                other pairing is a protocol this codec does not read, said as
                such so the broker can answer CONNACK 1 rather than guess. */
            const bool v311 = protocolName == "MQTT" && packet.protocolLevel == 4;
            const bool v31 = protocolName == "MQIsdp" && packet.protocolLevel == 3;
            if (! v311 && ! v31)
                return in.fail (reason::badProtocol);

            std::uint8_t flags = 0;
            if (! in.byte (flags))
                return false;
            if ((flags & 0x01u) != 0)
                return in.fail (reason::badFlags);

            const bool hasUser = (flags & 0x80u) != 0;
            const bool hasPassword = (flags & 0x40u) != 0;
            const bool willRetain = (flags & 0x20u) != 0;
            const auto willQos = static_cast<std::uint8_t> ((flags >> 3) & 0x03u);
            const bool hasWill = (flags & 0x04u) != 0;
            packet.cleanSession = (flags & 0x02u) != 0;

            /*  §3.1.2.5-6: no will, then its QoS and retain are nought; a
                password only with a user name. */
            if (! hasWill && (willQos != 0 || willRetain))
                return in.fail (reason::badFlags);
            if (willQos > 2)
                return in.fail (reason::badQos);
            if (hasPassword && ! hasUser)
                return in.fail (reason::badFlags);

            if (! in.twoBytes (packet.keepAlive))
                return false;
            if (! in.string (packet.clientId))
                return false;

            if (hasWill)
            {
                Will will;
                will.qos = willQos;
                will.retain = willRetain;
                if (! in.string (will.topic))
                    return false;
                if (! validTopic (will.topic))
                    return in.fail (reason::badTopic);
                std::uint16_t length = 0;
                if (! in.twoBytes (length))
                    return false;
                if (in.left() < length)
                    return in.fail (reason::truncated);
                will.payload.assign (reinterpret_cast<const char*> (in.at), length);
                in.at += length;
                packet.will = std::move (will);
            }

            if (hasUser)
            {
                std::string user;
                if (! in.string (user))
                    return false;
                packet.user = std::move (user);
            }

            if (hasPassword)
            {
                std::uint16_t length = 0;
                if (! in.twoBytes (length))
                    return false;
                if (in.left() < length)
                    return in.fail (reason::truncated);
                packet.password = std::string (reinterpret_cast<const char*> (in.at), length);
                in.at += length;
            }

            return true;
        }

        bool decodePublish (Reader& in, Packet& packet, std::uint8_t flags)
        {
            packet.duplicate = (flags & 0x08u) != 0;
            packet.qos = static_cast<std::uint8_t> ((flags >> 1) & 0x03u);
            packet.retain = (flags & 0x01u) != 0;

            if (packet.qos > 2)
                return in.fail (reason::badQos);
            if (packet.qos == 0 && packet.duplicate)
                return in.fail (reason::badFlags);

            if (! in.string (packet.topic))
                return false;
            if (! validTopic (packet.topic))
                return in.fail (reason::badTopic);

            if (packet.qos > 0 && ! in.packetId (packet.packetId))
                return false;

            /*  The payload is what is left, bytes of any kind. */
            packet.payload.assign (reinterpret_cast<const char*> (in.at), in.left());
            in.at = in.end;
            return true;
        }

        bool decodeSubscribe (Reader& in, Packet& packet, bool withQos)
        {
            if (! in.packetId (packet.packetId))
                return false;

            while (in.left() > 0)
            {
                std::string filter;
                if (! in.string (filter))
                    return false;
                if (! validFilter (filter))
                    return in.fail (reason::badTopic);

                std::uint8_t qos = 0;
                if (withQos)
                {
                    if (! in.byte (qos))
                        return false;
                    if (qos > 2)
                        return in.fail (reason::badQos);
                }
                packet.filters.emplace_back (std::move (filter), qos);
            }

            /*  §3.8.3-1 and §3.10.3-2: at least one filter. */
            if (packet.filters.empty())
                return in.fail (reason::truncated);
            return true;
        }
    }

    //==========================================================================
    Packet Packet::connectWith (std::string clientId, std::uint16_t keepAlive)
    {
        Packet packet;
        packet.type = Type::connect;
        packet.clientId = std::move (clientId);
        packet.keepAlive = keepAlive;
        return packet;
    }

    Packet Packet::connackWith (std::uint8_t code, bool sessionPresent)
    {
        Packet packet;
        packet.type = Type::connack;
        packet.returnCode = code;
        packet.sessionPresent = sessionPresent;
        return packet;
    }

    Packet Packet::publishOf (std::string topic, std::string payload, std::uint8_t qos, bool retain)
    {
        Packet packet;
        packet.type = Type::publish;
        packet.topic = std::move (topic);
        packet.payload = std::move (payload);
        packet.qos = qos;
        packet.retain = retain;
        return packet;
    }

    Packet Packet::acknowledgement (Type type, std::uint16_t packetId)
    {
        Packet packet;
        packet.type = type;
        packet.packetId = packetId;
        return packet;
    }

    Packet Packet::subscribeTo (std::uint16_t packetId, std::vector<std::pair<std::string, std::uint8_t>> filters)
    {
        Packet packet;
        packet.type = Type::subscribe;
        packet.packetId = packetId;
        packet.filters = std::move (filters);
        return packet;
    }

    Packet Packet::subackWith (std::uint16_t packetId, std::vector<std::uint8_t> codes)
    {
        Packet packet;
        packet.type = Type::suback;
        packet.packetId = packetId;
        packet.returnCodes = std::move (codes);
        return packet;
    }

    Packet Packet::unsubscribeFrom (std::uint16_t packetId, std::vector<std::string> filters)
    {
        Packet packet;
        packet.type = Type::unsubscribe;
        packet.packetId = packetId;
        for (auto& filter : filters)
            packet.filters.emplace_back (std::move (filter), std::uint8_t { 0 });
        return packet;
    }

    Packet Packet::plain (Type type)
    {
        Packet packet;
        packet.type = type;
        return packet;
    }

    //==========================================================================
    std::size_t frameLength (const std::uint8_t* bytes, std::size_t size, std::string& problem)
    {
        if (size < 2)
            return 0;

        std::size_t remaining = 0;
        std::size_t multiplier = 1;
        std::size_t at = 1;

        for (;;)
        {
            if (at >= size)
                return 0;                       // the length is not all here yet

            const auto byte = bytes[at++];
            remaining += static_cast<std::size_t> (byte & 0x7Fu) * multiplier;

            if ((byte & 0x80u) == 0)
                break;

            if (at > 4)
            {
                problem = reason::badLength;    // a fifth byte: not a length
                return 0;
            }
            multiplier *= 128;
        }

        return at + remaining;
    }

    Decoded decode (const std::vector<std::uint8_t>& bytes)
    {
        return decode (bytes.data(), bytes.size());
    }

    Decoded decode (const std::uint8_t* bytes, std::size_t size)
    {
        Decoded out;

        const auto whole = frameLength (bytes, size, out.problem);
        if (whole == 0)
        {
            if (out.problem.empty())
                out.problem = reason::truncated;
            return out;
        }
        if (whole > size)
        {
            out.problem = reason::truncated;
            return out;
        }
        if (whole < size)
        {
            out.problem = reason::trailing;
            return out;
        }

        const auto typeByte = static_cast<std::uint8_t> (bytes[0] >> 4);
        const auto flags = static_cast<std::uint8_t> (bytes[0] & 0x0Fu);

        if (typeByte < 1 || typeByte > 14)
        {
            out.problem = reason::badType;
            return out;
        }

        auto& packet = out.packet;
        packet.type = static_cast<Type> (typeByte);

        if (packet.type != Type::publish && flags != fixedFlagsOf (packet.type))
        {
            out.problem = reason::badFlags;
            return out;
        }

        /*  Past the fixed header. */
        std::size_t headerSize = 1;
        while ((bytes[headerSize++] & 0x80u) != 0) {}

        Reader in { bytes + headerSize, bytes + whole, out.problem };
        bool ok = false;

        switch (packet.type)
        {
            case Type::connect:
                ok = decodeConnect (in, packet);
                break;

            case Type::connack:
            {
                std::uint8_t ack = 0;
                ok = in.byte (ack) && in.byte (packet.returnCode);
                if (ok && (ack & 0xFEu) != 0)
                    ok = in.fail (reason::badFlags);
                packet.sessionPresent = (ack & 0x01u) != 0;
                break;
            }

            case Type::publish:
                ok = decodePublish (in, packet, flags);
                break;

            case Type::puback:
            case Type::pubrec:
            case Type::pubrel:
            case Type::pubcomp:
            case Type::unsuback:
                ok = in.packetId (packet.packetId);
                break;

            case Type::subscribe:
                ok = decodeSubscribe (in, packet, true);
                break;

            case Type::unsubscribe:
                ok = decodeSubscribe (in, packet, false);
                break;

            case Type::suback:
                ok = in.packetId (packet.packetId);
                while (ok && in.left() > 0)
                {
                    std::uint8_t code = 0;
                    ok = in.byte (code);
                    if (ok && code > 2 && code != subscribeFailed)
                        ok = in.fail (reason::badQos);
                    packet.returnCodes.push_back (code);
                }
                if (ok && packet.returnCodes.empty())
                    ok = in.fail (reason::truncated);
                break;

            case Type::pingreq:
            case Type::pingresp:
            case Type::disconnect:
                ok = true;
                break;
        }

        if (ok && in.left() > 0)
            ok = in.fail (reason::trailing);

        out.ok = ok;
        if (! ok && out.problem.empty())
            out.problem = reason::truncated;
        return out;
    }

    //==========================================================================
    std::optional<std::vector<std::uint8_t>> encode (const Packet& packet, std::string& problem)
    {
        std::vector<std::uint8_t> body;
        std::uint8_t flags = fixedFlagsOf (packet.type);

        switch (packet.type)
        {
            case Type::connect:
            {
                const bool v31 = packet.protocolLevel == 3;
                if (! putString (body, v31 ? "MQIsdp" : "MQTT", problem))
                    return std::nullopt;
                body.push_back (v31 ? std::uint8_t { 3 } : std::uint8_t { 4 });

                std::uint8_t connectFlags = packet.cleanSession ? 0x02u : 0x00u;
                if (packet.will.has_value())
                {
                    if (packet.will->qos > 2)
                    {
                        problem = reason::badQos;
                        return std::nullopt;
                    }
                    if (! validTopic (packet.will->topic))
                    {
                        problem = reason::badTopic;
                        return std::nullopt;
                    }
                    connectFlags = static_cast<std::uint8_t> (connectFlags | 0x04u
                                                              | static_cast<std::uint8_t> (packet.will->qos << 3)
                                                              | (packet.will->retain ? 0x20u : 0x00u));
                }
                if (packet.user.has_value())
                    connectFlags = static_cast<std::uint8_t> (connectFlags | 0x80u);
                if (packet.password.has_value())
                {
                    if (! packet.user.has_value())
                    {
                        problem = reason::badFlags;
                        return std::nullopt;
                    }
                    connectFlags = static_cast<std::uint8_t> (connectFlags | 0x40u);
                }
                body.push_back (connectFlags);
                putTwo (body, packet.keepAlive);

                if (! putString (body, packet.clientId, problem))
                    return std::nullopt;
                if (packet.will.has_value())
                {
                    if (! putString (body, packet.will->topic, problem) || ! putString (body, packet.will->payload, problem))
                        return std::nullopt;
                }
                if (packet.user.has_value() && ! putString (body, *packet.user, problem))
                    return std::nullopt;
                if (packet.password.has_value() && ! putString (body, *packet.password, problem))
                    return std::nullopt;
                break;
            }

            case Type::connack:
                body.push_back (packet.sessionPresent ? 0x01u : 0x00u);
                body.push_back (packet.returnCode);
                break;

            case Type::publish:
                if (packet.qos > 2)
                {
                    problem = reason::badQos;
                    return std::nullopt;
                }
                if (! validTopic (packet.topic))
                {
                    problem = reason::badTopic;
                    return std::nullopt;
                }
                if (packet.qos > 0 && packet.packetId == 0)
                {
                    problem = reason::badPacketId;
                    return std::nullopt;
                }
                flags = static_cast<std::uint8_t> ((packet.duplicate && packet.qos > 0 ? 0x08u : 0x00u)
                                                   | static_cast<std::uint8_t> (packet.qos << 1)
                                                   | (packet.retain ? 0x01u : 0x00u));
                if (! putString (body, packet.topic, problem))
                    return std::nullopt;
                if (packet.qos > 0)
                    putTwo (body, packet.packetId);
                body.insert (body.end(), packet.payload.begin(), packet.payload.end());
                break;

            case Type::puback:
            case Type::pubrec:
            case Type::pubrel:
            case Type::pubcomp:
            case Type::unsuback:
                if (packet.packetId == 0)
                {
                    problem = reason::badPacketId;
                    return std::nullopt;
                }
                putTwo (body, packet.packetId);
                break;

            case Type::subscribe:
            case Type::unsubscribe:
                if (packet.packetId == 0)
                {
                    problem = reason::badPacketId;
                    return std::nullopt;
                }
                if (packet.filters.empty())
                {
                    problem = reason::truncated;
                    return std::nullopt;
                }
                putTwo (body, packet.packetId);
                for (const auto& [filter, qos] : packet.filters)
                {
                    if (! validFilter (filter))
                    {
                        problem = reason::badTopic;
                        return std::nullopt;
                    }
                    if (! putString (body, filter, problem))
                        return std::nullopt;
                    if (packet.type == Type::subscribe)
                    {
                        if (qos > 2)
                        {
                            problem = reason::badQos;
                            return std::nullopt;
                        }
                        body.push_back (qos);
                    }
                }
                break;

            case Type::suback:
                if (packet.packetId == 0)
                {
                    problem = reason::badPacketId;
                    return std::nullopt;
                }
                if (packet.returnCodes.empty())
                {
                    problem = reason::truncated;
                    return std::nullopt;
                }
                putTwo (body, packet.packetId);
                for (const auto code : packet.returnCodes)
                {
                    if (code > 2 && code != subscribeFailed)
                    {
                        problem = reason::badQos;
                        return std::nullopt;
                    }
                    body.push_back (code);
                }
                break;

            case Type::pingreq:
            case Type::pingresp:
            case Type::disconnect:
                break;
        }

        if (body.size() > largestRemaining)
        {
            problem = reason::tooLong;
            return std::nullopt;
        }

        std::vector<std::uint8_t> out;
        out.reserve (body.size() + 5);
        out.push_back (static_cast<std::uint8_t> ((static_cast<std::uint8_t> (packet.type) << 4) | flags));

        auto remaining = body.size();
        do
        {
            auto byte = static_cast<std::uint8_t> (remaining % 128);
            remaining /= 128;
            if (remaining > 0)
                byte = static_cast<std::uint8_t> (byte | 0x80u);
            out.push_back (byte);
        }
        while (remaining > 0);

        out.insert (out.end(), body.begin(), body.end());
        return out;
    }

    //==========================================================================
    bool validTopic (std::string_view topic) noexcept
    {
        if (topic.empty() || topic.size() > largestString)
            return false;

        return std::none_of (topic.begin(), topic.end(),
                             [] (char c) { return c == '+' || c == '#' || c == '\0'; });
    }

    bool validFilter (std::string_view filter) noexcept
    {
        if (filter.empty() || filter.size() > largestString)
            return false;

        if (filter.find ('\0') != std::string_view::npos)
            return false;

        std::size_t start = 0;

        for (;;)
        {
            const auto slash = filter.find ('/', start);
            const auto level = filter.substr (start, slash == std::string_view::npos ? std::string_view::npos : slash - start);

            /*  A wildcard fills its level; `#` is the last level. */
            if (level.find ('+') != std::string_view::npos && level != "+")
                return false;
            if (level.find ('#') != std::string_view::npos && (level != "#" || slash != std::string_view::npos))
                return false;

            if (slash == std::string_view::npos)
                return true;
            start = slash + 1;
        }
    }

    bool topicMatches (std::string_view filter, std::string_view topic) noexcept
    {
        /*  §4.7.2: a topic beginning with `$` is kept from the wildcards at
            the root, so `#` does not hand out the broker's own `$SYS`. */
        if (! topic.empty() && topic.front() == '$' && ! filter.empty() && (filter.front() == '+' || filter.front() == '#'))
            return false;

        std::size_t f = 0;
        std::size_t t = 0;

        for (;;)
        {
            const auto fSlash = filter.find ('/', f);
            const auto tSlash = topic.find ('/', t);
            const auto fLevel = filter.substr (f, fSlash == std::string_view::npos ? std::string_view::npos : fSlash - f);
            const auto tLevel = topic.substr (t, tSlash == std::string_view::npos ? std::string_view::npos : tSlash - t);

            if (fLevel == "#")
                return true;                    // the rest, this level included, even when empty

            if (fLevel != "+" && fLevel != tLevel)
                return false;

            const bool fLast = fSlash == std::string_view::npos;
            const bool tLast = tSlash == std::string_view::npos;

            if (fLast && tLast)
                return true;

            /*  `a/#` matches `a` too (§4.7.1.2): the filter has one level
                left and it is `#`. */
            if (tLast)
                return ! fLast && filter.substr (fSlash + 1) == "#";

            if (fLast)
                return false;

            f = fSlash + 1;
            t = tSlash + 1;
        }
    }

    const char* nameOf (Type type) noexcept
    {
        switch (type)
        {
            case Type::connect:     return "CONNECT";
            case Type::connack:     return "CONNACK";
            case Type::publish:     return "PUBLISH";
            case Type::puback:      return "PUBACK";
            case Type::pubrec:      return "PUBREC";
            case Type::pubrel:      return "PUBREL";
            case Type::pubcomp:     return "PUBCOMP";
            case Type::subscribe:   return "SUBSCRIBE";
            case Type::suback:      return "SUBACK";
            case Type::unsubscribe: return "UNSUBSCRIBE";
            case Type::unsuback:    return "UNSUBACK";
            case Type::pingreq:     return "PINGREQ";
            case Type::pingresp:    return "PINGRESP";
            case Type::disconnect:  return "DISCONNECT";
        }
        return "?";
    }
}
