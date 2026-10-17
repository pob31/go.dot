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
    MULTICAST DNS MESSAGES, read and written (namespace draft §76, ARU, ASA).

    The OSCQuery proposal says how a server makes itself known: DNS service
    discovery over multicast DNS, service `_oscjson._tcp` - the same mechanism
    a printer or a speaker uses (RFC 6762, RFC 6763). This is the part of DNS's
    message format that needs: a question, and the four kinds of answer a
    service is found by - PTR (the instances of a service), SRV (an instance's
    host and port), TXT (its notes), A (the host's IPv4 address). Anything else
    in a message is stepped over by its length.

    OURS RATHER THAN A LIBRARY, as the OSC codec and the JSON reader are: the
    subset is small, the bytes are specified to the bit, and a codec with no
    socket in sight is one that byte fixtures test completely. No system
    responder is asked either (Bonjour, Avahi, Windows' own): none is on every
    machine, each has its own API, and what is needed is a question and its
    answers on one port.

    A NAME IS ITS LABELS, not a dotted string: an instance's label may hold a
    dot - "Go.dot on STAGE-PC" is one label - and only the length bytes say
    where a label ends. `nameOf` splits a dotted name whose labels have no dot
    in them (a service, a host), and `Name` compares as DNS does: ASCII letters
    alike whatever their case (RFC 6762 §16).

    PURE: no socket, no clock, no JUCE. A message that is not one is nothing
    (`decode` returns empty) rather than a guess: a truncated packet, a label
    longer than the message, a pointer that loops.
*/

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::mdns
{
    /** Where multicast DNS is: the IPv4 group and the port (RFC 6762 §3). */
    inline constexpr const char* group = "224.0.0.251";
    inline constexpr int port = 5353;

    /** The record types this codec reads; any other is stepped over. */
    namespace type
    {
        inline constexpr std::uint16_t a = 1;
        inline constexpr std::uint16_t ptr = 12;
        inline constexpr std::uint16_t txt = 16;
        inline constexpr std::uint16_t srv = 33;
        inline constexpr std::uint16_t any = 255;
    }

    using Name = std::vector<std::string>;

    /** "_oscjson._tcp.local" as its labels. For names whose labels hold no dot. */
    Name nameOf (std::string_view dotted);

    /** The labels joined by dots, for a person: an instance's own dot is not told apart. */
    std::string dotted (const Name& name);

    /** DNS's comparison: same labels, ASCII letters alike whatever their case. */
    bool sameName (const Name& a, const Name& b);

    struct Question
    {
        Name name {};
        std::uint16_t type = 0;
        bool unicastResponse = false;    ///< the QU bit: answer me directly (RFC 6762 §5.4)
    };

    struct Record
    {
        Name name {};
        std::uint16_t type = 0;
        bool cacheFlush = false;          ///< the top bit of the class: this is the whole set (RFC 6762 §10.2)
        std::uint32_t ttl = 0;            ///< seconds; nought says the record is gone
        Name target {};                   ///< PTR: the instance; SRV: the host
        std::uint16_t port = 0;           ///< SRV
        std::vector<std::string> txt {};  ///< TXT: its strings, empty ones left out
        std::array<std::uint8_t, 4> address {};   ///< A

        /** "10.0.0.7", for an A record. */
        std::string addressText() const;
    };

    struct Message
    {
        std::uint16_t id = 0;
        bool response = false;
        std::vector<Question> questions {};
        std::vector<Record> answers {};
        std::vector<Record> authorities {};
        std::vector<Record> additionals {};

        /** The answers, then the authorities, then the additionals: where a record is found, not why. */
        std::vector<Record> records() const;
    };

    /*  A query: the questions, nothing else, ID nought - which is what a
        multicast querier sends, and what a responder answering a query from a
        port other than 5353 must echo (RFC 6762 §6.7). */
    std::vector<std::uint8_t> encodeQuery (const std::vector<Question>& questions);

    /*  A response: flags "authoritative answer", the questions echoed when a
        querier from another port is answered (§6.7), the answers and the
        additional records. Names are written whole, never compressed: a
        response here is a few hundred bytes, and every reader reads both. */
    std::vector<std::uint8_t> encodeResponse (const std::vector<Record>& answers,
                                              const std::vector<Record>& additionals = {},
                                              std::uint16_t id = 0,
                                              const std::vector<Question>& questions = {});

    /** A message, or nothing when the bytes are not one. */
    std::optional<Message> decode (const std::uint8_t* data, std::size_t size);

    inline std::optional<Message> decode (const std::vector<std::uint8_t>& bytes)
    {
        return decode (bytes.data(), bytes.size());
    }
}
