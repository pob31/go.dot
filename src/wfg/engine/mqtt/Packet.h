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
    MQTT 3.1.1 ON THE WIRE (namespace draft §67, ANB; MQ.1): the fourteen
    control packets, read from bytes and written to bytes, one codec that the
    client (a device of the show on a broker) and Go.dot's own broker both
    speak. OASIS MQTT Version 3.1.1, 29 October 2014; the 3.1 CONNECT (its
    protocol name `MQIsdp`, level 3) is read too, since the small boards that
    publish to a show still send it.

    A packet is a fixed header - the type and its flags in one byte, then the
    remaining length as one to four bytes of seven bits each - and what the
    type says follows: a packet identifier for the acknowledged types, strings
    as two bytes of length and the UTF-8 after. Nothing here owns a socket:
    `frameLength` tells a stream reader where a packet ends, `decode` reads a
    whole one, `encode` writes one. What is malformed is said in a word, as
    the OSC codec says it, and never guessed at.

    Topic filters are matched here too (`+` a level, `#` the rest, a `$`
    topic never matched by a filter that starts with a wildcard), since the
    client checks a filter before it subscribes and the broker matches every
    publish against every subscription.
*/

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wfg::mqtt
{
    enum class Type : std::uint8_t
    {
        connect     = 1,
        connack     = 2,
        publish     = 3,
        puback      = 4,
        pubrec      = 5,
        pubrel      = 6,
        pubcomp     = 7,
        subscribe   = 8,
        suback      = 9,
        unsubscribe = 10,
        unsuback    = 11,
        pingreq     = 12,
        pingresp    = 13,
        disconnect  = 14
    };

    /*  The CONNACK return codes the specification names. */
    namespace connack
    {
        inline constexpr std::uint8_t accepted            = 0;
        inline constexpr std::uint8_t badProtocol         = 1;
        inline constexpr std::uint8_t badIdentifier       = 2;
        inline constexpr std::uint8_t unavailable         = 3;
        inline constexpr std::uint8_t badLogin            = 4;
        inline constexpr std::uint8_t notAuthorised       = 5;
    }

    /*  The SUBACK code for a subscription the broker refused. */
    inline constexpr std::uint8_t subscribeFailed = 0x80;

    /*  A packet's largest remaining length: four bytes of seven bits. */
    inline constexpr std::size_t largestRemaining = 268435455;

    /*  What the client says when it connects. */
    struct Will
    {
        std::string topic;
        std::string payload;
        std::uint8_t qos = 0;
        bool retain = false;
    };

    /*  ONE CONTROL PACKET, every type's fields on one struct: the type says
        which are read. Kept flat rather than a variant because the broker and
        the session switch on the type and read two or three fields, and a
        variant would make each of those a visit. */
    struct Packet
    {
        Type type = Type::pingreq;

        // CONNECT
        std::uint8_t protocolLevel = 4;       // 4 is 3.1.1, 3 is 3.1
        std::string clientId;
        std::uint16_t keepAlive = 0;          // seconds; nought is none
        bool cleanSession = true;
        std::optional<std::string> user;
        std::optional<std::string> password;
        std::optional<Will> will;

        // CONNACK
        bool sessionPresent = false;
        std::uint8_t returnCode = 0;

        // PUBLISH
        std::string topic;
        std::string payload;
        std::uint8_t qos = 0;
        bool retain = false;
        bool duplicate = false;

        // PUBLISH (QoS 1, 2), PUBACK, PUBREC, PUBREL, PUBCOMP, SUBSCRIBE,
        // SUBACK, UNSUBSCRIBE, UNSUBACK
        std::uint16_t packetId = 0;

        // SUBSCRIBE (filter, QoS asked), UNSUBSCRIBE (filter, nought)
        std::vector<std::pair<std::string, std::uint8_t>> filters;

        // SUBACK: one code per filter, the QoS granted or subscribeFailed
        std::vector<std::uint8_t> returnCodes;

        static Packet connectWith (std::string clientId, std::uint16_t keepAlive);
        static Packet connackWith (std::uint8_t code, bool sessionPresent = false);
        static Packet publishOf (std::string topic, std::string payload, std::uint8_t qos = 0, bool retain = false);
        static Packet acknowledgement (Type type, std::uint16_t packetId);
        static Packet subscribeTo (std::uint16_t packetId, std::vector<std::pair<std::string, std::uint8_t>> filters);
        static Packet subackWith (std::uint16_t packetId, std::vector<std::uint8_t> codes);
        static Packet unsubscribeFrom (std::uint16_t packetId, std::vector<std::string> filters);
        static Packet plain (Type type);     // pingreq, pingresp, disconnect, unsuback with id 0
    };

    /*  WHERE THE FIRST PACKET IN A STREAM ENDS. `size` bytes have arrived:
        the packet's whole length (fixed header and all) when its fixed
        header is complete, nought when more bytes are needed to know, and
        `problem` set with nought when the remaining length cannot be one -
        a fifth byte of it - which no further reading mends. */
    std::size_t frameLength (const std::uint8_t* bytes, std::size_t size, std::string& problem);

    struct Decoded
    {
        bool ok = false;
        std::string problem;     // one of reason::* below
        Packet packet;
    };

    /*  One whole packet from its bytes - what `frameLength` measured. */
    Decoded decode (const std::uint8_t* bytes, std::size_t size);
    Decoded decode (const std::vector<std::uint8_t>& bytes);

    /*  One packet as its bytes; nullopt with `problem` for one that cannot be
        written - a string past 65535 bytes, a payload past the largest
        remaining length, a QoS above two. */
    std::optional<std::vector<std::uint8_t>> encode (const Packet& packet, std::string& problem);

    namespace reason
    {
        inline constexpr const char* truncated      = "truncated";
        inline constexpr const char* badLength      = "bad-length";
        inline constexpr const char* badType        = "bad-type";
        inline constexpr const char* badFlags       = "bad-flags";
        inline constexpr const char* badString      = "bad-string";
        inline constexpr const char* badProtocol    = "bad-protocol";
        inline constexpr const char* badQos         = "bad-qos";
        inline constexpr const char* badPacketId    = "bad-packet-id";
        inline constexpr const char* badTopic       = "bad-topic";
        inline constexpr const char* trailing       = "trailing";
        inline constexpr const char* tooLong        = "too-long";
    }

    //==========================================================================
    /*  TOPICS AND FILTERS (3.1.1 §4.7). A topic names one thing: at least one
        byte, no `+` or `#`, no NUL, at most 65535 bytes. A filter may have `+`
        for one level and `#` for every level after, each alone in its level
        and `#` last. */
    bool validTopic (std::string_view topic) noexcept;
    bool validFilter (std::string_view filter) noexcept;

    /*  Whether a publish on `topic` reaches a subscription to `filter`. A
        topic beginning with `$` is reached only by a filter whose first level
        is spelt out, never by one starting with `+` or `#`. */
    bool topicMatches (std::string_view filter, std::string_view topic) noexcept;

    const char* nameOf (Type type) noexcept;
}
