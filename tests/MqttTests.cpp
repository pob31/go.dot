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

/*
    MQTT (namespace draft §67; MQ.1): the 3.1.1 codec against bytes written
    by hand from the specification - as the OSC codec's fixtures are - the
    remaining length at its four boundaries, every type round-tripped, the
    malformed packets refused by name; topic filters; and the two rules a
    message crosses between a broker and the show: a topic as an address, a
    payload as values. No socket.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/mqtt/Packet.h>
#include <wfg/engine/mqtt/Words.h>

#include <cstdint>
#include <string>
#include <chrono>
#include <functional>
#include <optional>
#include <thread>
#include <vector>

using namespace wfg::mqtt;

namespace
{
    std::vector<std::uint8_t> bytesOf (std::initializer_list<int> values)
    {
        std::vector<std::uint8_t> out;
        for (const auto v : values)
            out.push_back (static_cast<std::uint8_t> (v));
        return out;
    }

    std::vector<std::uint8_t> encoded (const Packet& packet)
    {
        std::string problem;
        const auto bytes = encode (packet, problem);
        REQUIRE_MESSAGE (bytes.has_value(), problem);
        return *bytes;
    }

    Packet roundTrip (const Packet& packet)
    {
        const auto bytes = encoded (packet);
        const auto back = decode (bytes);
        REQUIRE_MESSAGE (back.ok, back.problem);
        CHECK (back.packet.type == packet.type);
        return back.packet;
    }
}

//==============================================================================
TEST_CASE ("mqtt codec: the specification's CONNECT, read off bytes written by hand")
{
    /*  3.1.1 §3.1.1 example: MQTT level 4, user name and password, clean
        session, keepalive 10, client "x", user "u", password "p". */
    const auto bytes = bytesOf ({ 0x10, 0x13,
                                  0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0xC2, 0x00, 0x0A,
                                  0x00, 0x01, 'x',
                                  0x00, 0x01, 'u',
                                  0x00, 0x01, 'p' });
    const auto got = decode (bytes);
    REQUIRE_MESSAGE (got.ok, got.problem);
    CHECK (got.packet.type == Type::connect);
    CHECK (got.packet.protocolLevel == 4);
    CHECK (got.packet.cleanSession);
    CHECK (got.packet.keepAlive == 10);
    CHECK (got.packet.clientId == "x");
    REQUIRE (got.packet.user.has_value());
    CHECK (*got.packet.user == "u");
    REQUIRE (got.packet.password.has_value());
    CHECK (*got.packet.password == "p");
    CHECK_FALSE (got.packet.will.has_value());

    /*  And written back, byte for byte. */
    CHECK (encoded (got.packet) == bytes);
}

TEST_CASE ("mqtt codec: a 3.1 CONNECT is read, any other protocol is bad-protocol")
{
    const auto v31 = bytesOf ({ 0x10, 0x10,
                                0x00, 0x06, 'M', 'Q', 'I', 's', 'd', 'p', 0x03, 0x02, 0x00, 0x3C,
                                0x00, 0x02, 'a', 'b' });
    const auto got = decode (v31);
    REQUIRE_MESSAGE (got.ok, got.problem);
    CHECK (got.packet.protocolLevel == 3);
    CHECK (got.packet.clientId == "ab");
    CHECK (got.packet.keepAlive == 60);

    /*  MQTT 5 is `MQTT` at level 5: refused as a protocol, so the broker
        can answer CONNACK 1 (ANG). */
    const auto v5 = bytesOf ({ 0x10, 0x0D,
                               0x00, 0x04, 'M', 'Q', 'T', 'T', 0x05, 0x02, 0x00, 0x3C,
                               0x00, 0x01, 'z' });
    const auto refused = decode (v5);
    CHECK_FALSE (refused.ok);
    CHECK (refused.problem == reason::badProtocol);
}

TEST_CASE ("mqtt codec: the remaining length at 127, 128, 16383 and 16384")
{
    /*  §2.2.3's table: one byte up to 127, two up to 16383, three up to
        2097151, four beyond. A PUBLISH with a one-byte topic "t" and a
        payload sized to land the remaining length on each boundary. */
    const auto publishWithRemaining = [] (std::size_t remaining)
    {
        // topic "t" takes 3 bytes; the payload fills the rest
        return Packet::publishOf ("t", std::string (remaining - 3, 'p'));
    };

    {
        const auto bytes = encoded (publishWithRemaining (127));
        CHECK (bytes.size() == 2 + 127);
        CHECK (bytes[1] == 0x7F);
        std::string problem;
        CHECK (frameLength (bytes.data(), bytes.size(), problem) == bytes.size());
    }
    {
        const auto bytes = encoded (publishWithRemaining (128));
        CHECK (bytes.size() == 3 + 128);
        CHECK (bytes[1] == 0x80);
        CHECK (bytes[2] == 0x01);
        std::string problem;
        CHECK (frameLength (bytes.data(), bytes.size(), problem) == bytes.size());
        CHECK (decode (bytes).ok);
    }
    {
        const auto bytes = encoded (publishWithRemaining (16383));
        CHECK (bytes.size() == 3 + 16383);
        CHECK (bytes[1] == 0xFF);
        CHECK (bytes[2] == 0x7F);
        CHECK (decode (bytes).ok);
    }
    {
        const auto bytes = encoded (publishWithRemaining (16384));
        CHECK (bytes.size() == 4 + 16384);
        CHECK (bytes[1] == 0x80);
        CHECK (bytes[2] == 0x80);
        CHECK (bytes[3] == 0x01);
        const auto back = decode (bytes);
        REQUIRE (back.ok);
        CHECK (back.packet.payload.size() == 16384 - 3);
    }
}

TEST_CASE ("mqtt codec: frameLength says where a packet ends, and when it cannot")
{
    std::string problem;

    /*  Not enough to know yet. */
    const auto one = bytesOf ({ 0x30 });
    CHECK (frameLength (one.data(), one.size(), problem) == 0);
    CHECK (problem.empty());

    const auto twoOfThree = bytesOf ({ 0x30, 0x80 });    // a continued length, its next byte not here
    CHECK (frameLength (twoOfThree.data(), twoOfThree.size(), problem) == 0);
    CHECK (problem.empty());

    /*  A whole small packet in a longer stream: its own length, not the stream's. */
    const auto pingAndMore = bytesOf ({ 0xC0, 0x00, 0xD0, 0x00 });
    CHECK (frameLength (pingAndMore.data(), pingAndMore.size(), problem) == 2);

    /*  A fifth length byte is not a length: said once, never read on. */
    const auto five = bytesOf ({ 0x30, 0x80, 0x80, 0x80, 0x80, 0x01 });
    CHECK (frameLength (five.data(), five.size(), problem) == 0);
    CHECK (problem == reason::badLength);
}

TEST_CASE ("mqtt codec: every type round-trips")
{
    {
        auto p = Packet::connectWith ("godot-ABC-laptop", 30);
        p.user = "u";
        p.password = "secret";
        p.will = Will { "status/godot", "gone", 1, true };
        const auto back = roundTrip (p);
        CHECK (back.clientId == "godot-ABC-laptop");
        CHECK (back.keepAlive == 30);
        REQUIRE (back.will.has_value());
        CHECK (back.will->topic == "status/godot");
        CHECK (back.will->payload == "gone");
        CHECK (back.will->qos == 1);
        CHECK (back.will->retain);
        CHECK (back.user == "u");
        CHECK (back.password == "secret");
    }
    {
        const auto back = roundTrip (Packet::connackWith (connack::badLogin, false));
        CHECK (back.returnCode == connack::badLogin);
        CHECK_FALSE (back.sessionPresent);
        CHECK (encoded (Packet::connackWith (0, true)) == bytesOf ({ 0x20, 0x02, 0x01, 0x00 }));
    }
    {
        auto p = Packet::publishOf ("room1/temp", "23.5", 0, false);
        const auto bytes = encoded (p);
        CHECK (bytes == bytesOf ({ 0x30, 0x10, 0x00, 0x0A, 'r', 'o', 'o', 'm', '1', '/', 't', 'e', 'm', 'p', '2', '3', '.', '5' }));
        const auto back = roundTrip (p);
        CHECK (back.topic == "room1/temp");
        CHECK (back.payload == "23.5");
        CHECK (back.qos == 0);

        auto q = Packet::publishOf ("a", "b", 2, true);
        q.packetId = 7;
        q.duplicate = true;
        const auto backQ = roundTrip (q);
        CHECK (backQ.qos == 2);
        CHECK (backQ.retain);
        CHECK (backQ.duplicate);
        CHECK (backQ.packetId == 7);
        CHECK (encoded (q)[0] == 0x3D);    // 0011 1101: dup, QoS 2, retain
    }
    for (const auto type : { Type::puback, Type::pubrec, Type::pubrel, Type::pubcomp, Type::unsuback })
    {
        const auto back = roundTrip (Packet::acknowledgement (type, 300));
        CHECK (back.packetId == 300);
    }
    CHECK (encoded (Packet::acknowledgement (Type::pubrel, 1)) == bytesOf ({ 0x62, 0x02, 0x00, 0x01 }));
    {
        const auto back = roundTrip (Packet::subscribeTo (5, { { "house/#", 0 }, { "+/temp", 1 } }));
        REQUIRE (back.filters.size() == 2);
        CHECK (back.filters[0].first == "house/#");
        CHECK (back.filters[1].second == 1);
        CHECK (encoded (Packet::subscribeTo (1, { { "a", 0 } }))
                 == bytesOf ({ 0x82, 0x06, 0x00, 0x01, 0x00, 0x01, 'a', 0x00 }));
    }
    {
        const auto back = roundTrip (Packet::subackWith (5, { 0, subscribeFailed }));
        REQUIRE (back.returnCodes.size() == 2);
        CHECK (back.returnCodes[1] == subscribeFailed);
    }
    {
        const auto back = roundTrip (Packet::unsubscribeFrom (9, { "house/#" }));
        REQUIRE (back.filters.size() == 1);
        CHECK (back.filters[0].first == "house/#");
        CHECK (encoded (Packet::unsubscribeFrom (1, { "a" })) == bytesOf ({ 0xA2, 0x05, 0x00, 0x01, 0x00, 0x01, 'a' }));
    }
    CHECK (encoded (Packet::plain (Type::pingreq)) == bytesOf ({ 0xC0, 0x00 }));
    CHECK (encoded (Packet::plain (Type::pingresp)) == bytesOf ({ 0xD0, 0x00 }));
    CHECK (encoded (Packet::plain (Type::disconnect)) == bytesOf ({ 0xE0, 0x00 }));
    CHECK (roundTrip (Packet::plain (Type::disconnect)).type == Type::disconnect);
}

TEST_CASE ("mqtt codec: what is malformed is refused by name")
{
    const auto refused = [] (std::vector<std::uint8_t> bytes, const char* why)
    {
        const auto got = decode (bytes);
        CHECK_FALSE (got.ok);
        CHECK (got.problem == why);
    };

    refused (bytesOf ({ 0x00, 0x00 }), reason::badType);              // type 0
    refused (bytesOf ({ 0xF0, 0x00 }), reason::badType);              // type 15
    refused (bytesOf ({ 0xC1, 0x00 }), reason::badFlags);             // PINGREQ with a flag
    refused (bytesOf ({ 0x60, 0x02, 0x00, 0x01 }), reason::badFlags); // PUBREL without its 0010
    refused (bytesOf ({ 0x30, 0x05, 0x00, 0x0A, 't' }), reason::truncated);   // string longer than what follows
    refused (bytesOf ({ 0x30, 0x03, 0x00, 0x01, '#' }), reason::badTopic);    // a wildcard in a publish
    refused (bytesOf ({ 0x30, 0x03, 0x00, 0x01, 't', 0x00 }), reason::trailing); // a byte past the length
    refused (bytesOf ({ 0x3E, 0x05, 0x00, 0x01, 't', 0x00, 0x01 }), reason::badQos);   // QoS 3
    refused (bytesOf ({ 0x38, 0x03, 0x00, 0x01, 't' }), reason::badFlags);    // dup on QoS 0
    refused (bytesOf ({ 0x32, 0x05, 0x00, 0x01, 't', 0x00, 0x00 }), reason::badPacketId);  // QoS 1, id 0
    refused (bytesOf ({ 0x40, 0x02, 0x00, 0x00 }), reason::badPacketId);      // PUBACK id 0
    refused (bytesOf ({ 0x82, 0x02, 0x00, 0x01 }), reason::truncated);        // SUBSCRIBE with no filter
    refused (bytesOf ({ 0x82, 0x06, 0x00, 0x01, 0x00, 0x01, 'a', 0x03 }), reason::badQos);
    refused (bytesOf ({ 0x82, 0x07, 0x00, 0x01, 0x00, 0x02, 'a', '#', 0x00 }), reason::badTopic);  // `a#`
    refused (bytesOf ({ 0x90, 0x03, 0x00, 0x01, 0x05 }), reason::badQos);     // SUBACK code 5
    refused (bytesOf ({ 0x20, 0x02, 0x02, 0x00 }), reason::badFlags);         // CONNACK with a reserved bit
    refused (bytesOf ({ 0x30, 0x04, 0x00, 0x02, 't', 0x00 }), reason::badString);   // NUL in a topic

    /*  A CONNECT whose flags lie: the reserved bit, a will QoS with no will,
        a password with no user. */
    refused (bytesOf ({ 0x10, 0x0D, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0x03, 0x00, 0x0A, 0x00, 0x01, 'x' }), reason::badFlags);
    refused (bytesOf ({ 0x10, 0x0D, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0x0A, 0x00, 0x0A, 0x00, 0x01, 'x' }), reason::badFlags);
    refused (bytesOf ({ 0x10, 0x10, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0x42, 0x00, 0x0A, 0x00, 0x01, 'x', 0x00, 0x01, 'p' }), reason::badFlags);

    /*  And the encoder refuses what it cannot write. */
    std::string problem;
    CHECK_FALSE (encode (Packet::connectWith (std::string (65536, 'c'), 10), problem).has_value());
    CHECK (problem == reason::tooLong);
    problem.clear();
    CHECK_FALSE (encode (Packet::publishOf (std::string (65536, 't'), ""), problem).has_value());
    CHECK (problem == reason::badTopic);
    problem.clear();
    CHECK_FALSE (encode (Packet::publishOf ("a/+", ""), problem).has_value());
    CHECK (problem == reason::badTopic);
    problem.clear();
    CHECK_FALSE (encode (Packet::acknowledgement (Type::puback, 0), problem).has_value());
    CHECK (problem == reason::badPacketId);
}

//==============================================================================
TEST_CASE ("mqtt topics: what is a topic, what is a filter")
{
    CHECK (validTopic ("a"));
    CHECK (validTopic ("a/b/c"));
    CHECK (validTopic ("/leading"));
    CHECK (validTopic ("a//b"));
    CHECK (validTopic ("with space"));
    CHECK (validTopic ("$SYS/broker"));
    CHECK_FALSE (validTopic (""));
    CHECK_FALSE (validTopic ("a/+"));
    CHECK_FALSE (validTopic ("a/#"));
    CHECK_FALSE (validTopic (std::string ("a\0b", 3)));
    CHECK_FALSE (validTopic (std::string (65536, 'a')));

    CHECK (validFilter ("#"));
    CHECK (validFilter ("+"));
    CHECK (validFilter ("a/#"));
    CHECK (validFilter ("a/+/c"));
    CHECK (validFilter ("+/+/#"));
    CHECK (validFilter ("/#"));
    CHECK_FALSE (validFilter (""));
    CHECK_FALSE (validFilter ("a#"));
    CHECK_FALSE (validFilter ("#/a"));
    CHECK_FALSE (validFilter ("a/#/b"));
    CHECK_FALSE (validFilter ("a+"));
    CHECK_FALSE (validFilter ("+a/b"));
}

TEST_CASE ("mqtt topics: a filter matches by level, and $ topics hide from the root wildcards")
{
    CHECK (topicMatches ("#", "a"));
    CHECK (topicMatches ("#", "a/b/c"));
    CHECK (topicMatches ("#", "/"));
    CHECK (topicMatches ("a/#", "a"));            // §4.7.1.2: the parent too
    CHECK (topicMatches ("a/#", "a/b"));
    CHECK (topicMatches ("a/#", "a/b/c"));
    CHECK_FALSE (topicMatches ("a/#", "b"));
    CHECK_FALSE (topicMatches ("a/#", "ab"));
    CHECK (topicMatches ("+", "a"));
    CHECK_FALSE (topicMatches ("+", "a/b"));
    CHECK (topicMatches ("+/temp", "room1/temp"));
    CHECK_FALSE (topicMatches ("+/temp", "room1/x/temp"));
    CHECK (topicMatches ("a/+/c", "a/b/c"));
    CHECK (topicMatches ("a/+/c", "a//c"));       // an empty level is a level
    CHECK_FALSE (topicMatches ("a/+/c", "a/c"));
    CHECK (topicMatches ("a/b", "a/b"));
    CHECK_FALSE (topicMatches ("a/b", "a/b/c"));
    CHECK_FALSE (topicMatches ("a/b/c", "a/b"));
    CHECK (topicMatches ("/+", "/a"));
    CHECK_FALSE (topicMatches ("+", "/a"));

    CHECK_FALSE (topicMatches ("#", "$SYS/broker"));
    CHECK_FALSE (topicMatches ("+/broker", "$SYS/broker"));
    CHECK (topicMatches ("$SYS/#", "$SYS/broker"));
    CHECK (topicMatches ("$SYS/+", "$SYS/broker"));
}

//==============================================================================
TEST_CASE ("mqtt words: a topic is an address under the device's prefix, and back")
{
    CHECK (addressOfTopic ("/house", "room1/temp") == std::string ("/house/room1/temp"));
    CHECK (addressOfTopic ("/house /other", "a") == std::string ("/house/a"));   // the first root
    CHECK (addressOfTopic ("/house/", "a") == std::string ("/house/a"));

    /*  What an OSC address cannot carry is dropped (ANF). */
    CHECK_FALSE (addressOfTopic ("/house", "with space").has_value());
    CHECK_FALSE (addressOfTopic ("/house", "a,b").has_value());
    CHECK_FALSE (addressOfTopic ("/house", "a*").has_value());
    CHECK_FALSE (addressOfTopic ("/house", "q?").has_value());
    CHECK_FALSE (addressOfTopic ("/house", "a[1]").has_value());
    CHECK_FALSE (addressOfTopic ("/house", "{x}").has_value());
    CHECK_FALSE (addressOfTopic ("/house", "").has_value());
    CHECK_FALSE (addressOfTopic ("", "a").has_value());

    CHECK (topicOfAddress ("/house", "/house/room1/temp") == std::string ("room1/temp"));
    CHECK (topicOfAddress ("/house /other", "/other/lights/1") == std::string ("lights/1"));
    CHECK_FALSE (topicOfAddress ("/house", "/house").has_value());
    CHECK_FALSE (topicOfAddress ("/house", "/housekeeping/x").has_value());
    CHECK_FALSE (topicOfAddress ("/house", "/elsewhere/x").has_value());

    /*  Both ways round. */
    const auto address = addressOfTopic ("/house", "lights/1");
    REQUIRE (address.has_value());
    CHECK (topicOfAddress ("/house", *address) == std::string ("lights/1"));
}

TEST_CASE ("mqtt words: a payload is read as a serial line is, JSON whole, bytes as a blob")
{
    using wfg::osc::Value;

    {
        const auto values = valuesOfPayload ("23.5");
        REQUIRE (values.size() == 1);
        CHECK (values[0].isFloat64());
        CHECK (values[0].asDouble() == doctest::Approx (23.5));
    }
    {
        const auto values = valuesOfPayload ("on 1, 2.5 off");
        REQUIRE (values.size() == 4);
        CHECK (values[0].getString() == "on");
        CHECK (values[1].asDouble() == doctest::Approx (1.0));
        CHECK (values[2].asDouble() == doctest::Approx (2.5));
        CHECK (values[3].getString() == "off");
    }
    {
        const auto values = valuesOfPayload ("{\"t\": 23.5, \"h\": 40}");
        REQUIRE (values.size() == 1);
        CHECK (values[0].isString());
        CHECK (values[0].getString() == "{\"t\": 23.5, \"h\": 40}");
        CHECK (valuesOfPayload ("[1, 2]").size() == 1);
    }
    {
        const std::string bytes { '\x01', '\xFF', '\x00' };
        const auto values = valuesOfPayload (bytes);
        REQUIRE (values.size() == 1);
        CHECK (values[0].isBlob());
        CHECK (values[0].getBlob().bytes.size() == 3);
    }
    CHECK (valuesOfPayload ("").empty());
    CHECK (valuesOfPayload (std::string ("caf\xC3\xA9")).size() == 1);     // UTF-8 is text
    CHECK (isText ("a\tb\r\n"));
    CHECK_FALSE (isText (std::string ("a\x80")));       // a lone continuation byte
    CHECK_FALSE (isText (std::string ("\xC3")));        // a lead byte with nothing after

    /*  Out: numbers in no locale's way, one string as it is. */
    CHECK (payloadOfValues ({ Value::float64 (23.5) }) == "23.5");
    CHECK (payloadOfValues ({ Value::int32 (1) }) == "1");
    CHECK (payloadOfValues ({ Value::float32 (0.1f) }) == "0.1");
    CHECK (payloadOfValues ({ Value::string ("led"), Value::int32 (1) }) == "led 1");
    CHECK (payloadOfValues ({ Value::string ("{\"a\": 1, \"b\": 2}") }) == "{\"a\": 1, \"b\": 2}");
    CHECK (payloadOfValues ({ Value::boolean (true), Value::nil(), Value::string ("x") }) == "true x");
    CHECK (payloadOfValues ({}).empty());
}

//==============================================================================
/*  MQ.3: THE SESSION (namespace draft §67, ANE, ANI) - the client side of the
    protocol on the tick thread, driven here by hand: the link's state as the
    table would report it, the packets a broker would send, the ticks. */
#include <wfg/engine/mqtt/Session.h>

namespace
{
    wfg::serial::PortState openLink (std::uint64_t openings)
    {
        wfg::serial::PortState state;
        state.state = "open";
        state.openings = openings;
        return state;
    }

    SessionWanted house()
    {
        SessionWanted wanted;
        wanted.id = "MQ000001";
        wanted.prefix = "/house";
        wanted.topics = "house/# +/temp";
        wanted.login = "stage secret";
        wanted.clientId = "godot-MQ000001-laptop";
        return wanted;
    }
}

TEST_CASE ("mqtt session: CONNECT at each opening, SUBSCRIBE once the broker says yes, and what comes back is heard")
{
    Session session (house());
    wfg::serial::PortState closed;

    //  Nothing while the link is closed.
    CHECK (session.tick (closed, 1).empty());
    CHECK_FALSE (session.connected());

    //  The first opening: CONNECT with the login and the client id, clean session.
    auto out = session.tick (openLink (1), 2);
    REQUIRE (out.size() == 1u);
    CHECK (out[0].type == Type::connect);
    CHECK (out[0].clientId == "godot-MQ000001-laptop");
    CHECK (out[0].cleanSession);
    CHECK (out[0].keepAlive == Session::keepAliveSeconds);
    CHECK (out[0].user == std::string ("stage"));
    CHECK (out[0].password == std::string ("secret"));
    CHECK (session.tick (openLink (1), 3).empty());      // and waits

    //  CONNACK accepted: the SUBSCRIBE to every filter follows on the next tick.
    CHECK_FALSE (session.feed (Packet::connackWith (connack::accepted), 4).has_value());
    CHECK (session.connected());
    out = session.tick (openLink (1), 5);
    REQUIRE (out.size() == 1u);
    CHECK (out[0].type == Type::subscribe);
    REQUIRE (out[0].filters.size() == 2u);
    CHECK (out[0].filters[0].first == "house/#");
    CHECK (out[0].filters[1].first == "+/temp");
    CHECK (out[0].filters[0].second == 0);
    CHECK (out[0].packetId != 0);
    CHECK_FALSE (session.feed (Packet::subackWith (out[0].packetId, { 0, 0 }), 6).has_value());
    CHECK (session.problem().empty());

    //  A publish is heard at the address its topic spells, its payload as values.
    const auto heard = session.feed (Packet::publishOf ("room1/temp", "23.5"), 7);
    REQUIRE (heard.has_value());
    CHECK (heard->address == "/house/room1/temp");
    REQUIRE (heard->values.size() == 1u);
    CHECK (heard->values[0].asDouble() == doctest::Approx (23.5));

    //  A QoS 1 publish is acknowledged, a QoS 2 one walked through; both heard once.
    auto qos1 = Packet::publishOf ("lights/1", "on", 1);
    qos1.packetId = 40;
    CHECK (session.feed (qos1, 8).has_value());
    auto qos2 = Packet::publishOf ("lights/2", "off", 2);
    qos2.packetId = 41;
    CHECK (session.feed (qos2, 8).has_value());
    out = session.tick (openLink (1), 9);
    REQUIRE (out.size() == 2u);
    CHECK (out[0].type == Type::puback);
    CHECK (out[0].packetId == 40);
    CHECK (out[1].type == Type::pubrec);
    CHECK (out[1].packetId == 41);
    CHECK_FALSE (session.feed (Packet::acknowledgement (Type::pubrel, 41), 10).has_value());
    out = session.tick (openLink (1), 11);
    REQUIRE (out.size() == 1u);
    CHECK (out[0].type == Type::pubcomp);

    //  A topic that cannot be an address is dropped and counted (ANF).
    CHECK_FALSE (session.feed (Packet::publishOf ("with space", "1"), 12).has_value());
    CHECK (session.dropped() == 1u);

    //  A publish from the show: the topic under the prefix, the payload the values.
    const auto publish = session.publish ("/house/lights/1", { wfg::osc::Value::int32 (1) });
    REQUIRE (publish.has_value());
    CHECK (publish->topic == "lights/1");
    CHECK (publish->payload == "1");
    CHECK (publish->qos == 0);
    CHECK_FALSE (publish->retain);
    CHECK_FALSE (session.publish ("/elsewhere/x", {}).has_value());

    //  A second opening - the table counted it - starts again with CONNECT.
    out = session.tick (openLink (2), 13);
    REQUIRE (out.size() == 1u);
    CHECK (out[0].type == Type::connect);
    CHECK_FALSE (session.connected());

    //  The link closing in between resets too; opening 2 seen again is not a new one.
    CHECK (session.tick (closed, 14).empty());
    CHECK (session.tick (openLink (2), 15).empty());
}

TEST_CASE ("mqtt session: the keepalive by ticks, a silent broker given up on, a refusal said")
{
    Session session (house());
    session.tick (openLink (1), 0);
    session.feed (Packet::connackWith (connack::accepted), 1);
    auto out = session.tick (openLink (1), 2);             // the SUBSCRIBE
    REQUIRE (out.size() == 1u);
    session.feed (Packet::subackWith (out[0].packetId, { 0, 0 }), 3);

    //  Half the keepalive with nothing sent: a PINGREQ, and one only until answered.
    std::int64_t tick = 3;
    std::vector<Packet> pings;
    for (; tick < 2 + 15 * Session::ticksPerSecond + 5; ++tick)
        for (auto& packet : session.tick (openLink (1), tick))
            if (packet.type == Type::pingreq)
                pings.push_back (packet);
    REQUIRE (pings.size() == 1u);
    session.feed (Packet::plain (Type::pingresp), tick);
    CHECK (session.connected());

    //  The broker silent for the whole keepalive after that: given up on, the
    //  link asked to open again, and the sentence says so.
    for (; tick < 3 + 15 * Session::ticksPerSecond + 30 * Session::ticksPerSecond + 10; ++tick)
        session.tick (openLink (1), tick);
    CHECK_FALSE (session.connected());
    CHECK (session.problem() == "the broker has gone silent");
    CHECK (session.takeReopenWish());
    CHECK_FALSE (session.takeReopenWish());

    //  A CONNECT the broker refuses is said in words.
    Session refused (house());
    refused.tick (openLink (1), 0);
    refused.feed (Packet::connackWith (connack::badLogin), 1);
    CHECK_FALSE (refused.connected());
    CHECK (refused.problem() == "refused: bad user name or password");
    CHECK (refused.tick (openLink (1), 2).empty());

    //  And one the broker never answers.
    Session unanswered (house());
    unanswered.tick (openLink (1), 0);
    for (std::int64_t t = 1; t < 11 * Session::ticksPerSecond; ++t)
        unanswered.tick (openLink (1), t);
    CHECK (unanswered.problem() == "the broker answered nothing to CONNECT");
    CHECK (unanswered.takeReopenWish());

    //  A subscription refused is said, the rest held.
    Session partly (house());
    partly.tick (openLink (1), 0);
    partly.feed (Packet::connackWith (connack::accepted), 1);
    out = partly.tick (openLink (1), 2);
    REQUIRE (out.size() == 1u);
    partly.feed (Packet::subackWith (out[0].packetId, { 0, subscribeFailed }), 3);
    CHECK (partly.problem() == "the broker refused the subscription to +/temp");
}

TEST_CASE ("mqtt session: a changed filter list is carried to the broker in place")
{
    Session session (house());
    session.tick (openLink (1), 0);
    session.feed (Packet::connackWith (connack::accepted), 1);
    auto out = session.tick (openLink (1), 2);
    REQUIRE (out.size() == 1u);

    auto wanted = house();
    wanted.topics = "house/# lights/+";
    session.want (wanted);
    out = session.tick (openLink (1), 3);
    REQUIRE (out.size() == 2u);
    CHECK (out[0].type == Type::unsubscribe);
    REQUIRE (out[0].filters.size() == 1u);
    CHECK (out[0].filters[0].first == "+/temp");
    CHECK (out[1].type == Type::subscribe);
    REQUIRE (out[1].filters.size() == 2u);
    CHECK (out[1].filters[1].first == "lights/+");

    //  The same list again is nothing.
    session.want (wanted);
    CHECK (session.tick (openLink (1), 4).empty());

    //  A new login is the link's to carry (a new opening): the session is not asked.
    wanted.login = "other word";
    session.want (wanted);
    CHECK (session.tick (openLink (1), 5).empty());
    CHECK (session.wanted().login == "other word");
}

//==============================================================================
/*  MQ.5: GO.DOT'S OWN BROKER (namespace draft §67, AND, ANG), against raw
    socket clients that speak the bytes themselves: the fan-out, retained
    messages, a will, the keepalive, QoS 1 and 2 answered, MQTT 5 and a bad
    login refused, and the link with no socket a device of the show takes. */
#include <wfg/engine/mqtt/Broker.h>

#include <juce_core/juce_core.h>

namespace
{
    /*  A client at the end of a socket: sends packets, waits for one back. */
    struct RawClient
    {
        juce::StreamingSocket socket;
        std::string partial;

        bool connectTo (int port)
        {
            return socket.connect ("127.0.0.1", port, 2000);
        }

        bool send (const Packet& packet)
        {
            std::string problem;
            const auto bytes = encode (packet, problem);
            REQUIRE_MESSAGE (bytes.has_value(), problem);
            return socket.write (bytes->data(), static_cast<int> (bytes->size())) == static_cast<int> (bytes->size());
        }

        std::optional<Packet> expect (int timeoutMs = 2000)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds (timeoutMs);

            for (;;)
            {
                std::string problem;
                const auto whole = frameLength (reinterpret_cast<const std::uint8_t*> (partial.data()), partial.size(), problem);
                if (whole > 0 && partial.size() >= whole)
                {
                    const auto decoded = decode (reinterpret_cast<const std::uint8_t*> (partial.data()), whole);
                    partial.erase (0, whole);
                    REQUIRE_MESSAGE (decoded.ok, decoded.problem);
                    return decoded.packet;
                }

                if (std::chrono::steady_clock::now() > deadline)
                    return std::nullopt;

                const auto ready = socket.waitUntilReady (true, 20);
                if (ready < 0)
                    return std::nullopt;
                if (ready == 0)
                    continue;

                char buffer[4096];
                const auto got = socket.read (buffer, static_cast<int> (sizeof (buffer)), false);
                if (got <= 0)
                    return std::nullopt;
                partial.append (buffer, static_cast<std::size_t> (got));
            }
        }

        bool closedByBroker (int timeoutMs = 2000)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds (timeoutMs);
            while (std::chrono::steady_clock::now() < deadline)
            {
                const auto ready = socket.waitUntilReady (true, 20);
                if (ready < 0)
                    return true;
                if (ready > 0)
                {
                    char buffer[64];
                    if (socket.read (buffer, static_cast<int> (sizeof (buffer)), false) <= 0)
                        return true;
                }
            }
            return false;
        }

        /*  CONNECT and the CONNACK back, accepted. */
        void join (int port, const std::string& id, std::uint16_t keepAlive = 60)
        {
            REQUIRE (connectTo (port));
            REQUIRE (send (Packet::connectWith (id, keepAlive)));
            const auto ack = expect();
            REQUIRE (ack.has_value());
            REQUIRE (ack->type == Type::connack);
            REQUIRE (ack->returnCode == connack::accepted);
            CHECK_FALSE (ack->sessionPresent);
        }

        void subscribe (const std::string& filter, std::uint16_t packetId = 1)
        {
            REQUIRE (send (Packet::subscribeTo (packetId, { { filter, 0 } })));
            const auto ack = expect();
            REQUIRE (ack.has_value());
            REQUIRE (ack->type == Type::suback);
            CHECK (ack->packetId == packetId);
            REQUIRE (ack->returnCodes.size() == 1u);
            CHECK (ack->returnCodes[0] == 0);
        }
    };

    bool within (std::chrono::milliseconds limit, const std::function<bool()>& done)
    {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (done())
                return true;
            std::this_thread::sleep_for (std::chrono::milliseconds (10));
        }
        return done();
    }
}

TEST_CASE ("mqtt broker: clients connect, subscribe and hear each other, with the retained message a late one is handed")
{
    Broker broker;
    REQUIRE (broker.start (0, {}));
    const auto port = broker.stateNow().port;
    REQUIRE (port > 0);
    CHECK (broker.stateNow().state == "listening");

    RawClient sensor, lights, late;
    sensor.join (port, "sensor");
    lights.join (port, "lights");
    lights.subscribe ("house/#");
    CHECK (within (std::chrono::seconds (2), [&] { return broker.stateNow().clients == 2u; }));

    //  A publish reaches the subscriber, not the publisher, at QoS 0.
    REQUIRE (sensor.send (Packet::publishOf ("house/room1/temp", "23.5")));
    const auto heard = lights.expect();
    REQUIRE (heard.has_value());
    CHECK (heard->type == Type::publish);
    CHECK (heard->topic == "house/room1/temp");
    CHECK (heard->payload == "23.5");
    CHECK (heard->qos == 0);
    CHECK_FALSE (heard->retain);
    CHECK_FALSE (sensor.expect (200).has_value());

    //  One not under the filter is not heard.
    REQUIRE (sensor.send (Packet::publishOf ("garden/temp", "12")));
    CHECK_FALSE (lights.expect (200).has_value());

    //  QoS 1 is acknowledged, QoS 2 walked through; the subscriber still gets QoS 0.
    auto qos1 = Packet::publishOf ("house/a", "1", 1);
    qos1.packetId = 7;
    REQUIRE (sensor.send (qos1));
    auto ack = sensor.expect();
    REQUIRE (ack.has_value());
    CHECK (ack->type == Type::puback);
    CHECK (ack->packetId == 7);
    CHECK (lights.expect()->qos == 0);

    auto qos2 = Packet::publishOf ("house/b", "2", 2);
    qos2.packetId = 8;
    REQUIRE (sensor.send (qos2));
    ack = sensor.expect();
    REQUIRE (ack.has_value());
    CHECK (ack->type == Type::pubrec);
    REQUIRE (sensor.send (Packet::acknowledgement (Type::pubrel, 8)));
    ack = sensor.expect();
    REQUIRE (ack.has_value());
    CHECK (ack->type == Type::pubcomp);
    CHECK (lights.expect()->topic == "house/b");

    //  A retained message is handed to a late subscriber, its retain flag set.
    REQUIRE (sensor.send (Packet::publishOf ("house/room1/setpoint", "21", 0, true)));
    CHECK (lights.expect()->retain == false);
    late.join (port, "late");
    late.subscribe ("house/+/setpoint");
    const auto kept = late.expect();
    REQUIRE (kept.has_value());
    CHECK (kept->topic == "house/room1/setpoint");
    CHECK (kept->payload == "21");
    CHECK (kept->retain);

    //  An empty retained payload forgets it (the live copy reaching `lights`
    //  is the moment the broker has taken it).
    REQUIRE (sensor.send (Packet::publishOf ("house/room1/setpoint", "", 0, true)));
    CHECK (lights.expect()->payload.empty());
    RawClient later;
    later.join (port, "later");
    later.subscribe ("house/#");
    CHECK_FALSE (later.expect (200).has_value());

    //  PINGREQ is answered; UNSUBSCRIBE stops the hearing.
    REQUIRE (lights.send (Packet::plain (Type::pingreq)));
    while (const auto p = lights.expect (300))
        if (p->type == Type::pingresp)
            break;
    REQUIRE (lights.send (Packet::unsubscribeFrom (3, { "house/#" })));
    while (const auto p = lights.expect (300))
        if (p->type == Type::unsuback)
            break;
    REQUIRE (sensor.send (Packet::publishOf ("house/c", "3")));
    CHECK_FALSE (lights.expect (200).has_value());
    CHECK (later.expect()->topic == "house/c");     // the one still subscribed hears it

    //  A $ topic is kept from the root wildcards.
    REQUIRE (sensor.send (Packet::publishOf ("$SYS/x", "y")));
    CHECK_FALSE (later.expect (200).has_value());

    //  DISCONNECT lets go; the count follows.
    REQUIRE (sensor.send (Packet::plain (Type::disconnect)));
    CHECK (within (std::chrono::seconds (2), [&] { return broker.stateNow().clients == 3u; }));

    broker.stop();
    CHECK (broker.stateNow().state == "off");
    CHECK (lights.closedByBroker());
}

TEST_CASE ("mqtt broker: a will when a client goes without saying, a silent client dropped, and what is refused")
{
    Broker broker;
    REQUIRE (broker.start (0, "stage secret"));
    const auto port = broker.stateNow().port;

    //  The login: wrong or missing is CONNACK 4 and the socket closed.
    RawClient stranger;
    REQUIRE (stranger.connectTo (port));
    REQUIRE (stranger.send (Packet::connectWith ("stranger", 10)));
    auto ack = stranger.expect();
    REQUIRE (ack.has_value());
    CHECK (ack->returnCode == connack::badLogin);
    CHECK (stranger.closedByBroker());

    //  MQTT 5 is CONNACK 1.
    RawClient modern;
    REQUIRE (modern.connectTo (port));
    {
        const std::vector<std::uint8_t> v5 { 0x10, 0x0D, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x05, 0x02, 0x00, 0x3C, 0x00, 0x01, 'z' };
        REQUIRE (modern.socket.write (v5.data(), static_cast<int> (v5.size())) == static_cast<int> (v5.size()));
    }
    ack = modern.expect();
    REQUIRE (ack.has_value());
    CHECK (ack->returnCode == connack::badProtocol);
    CHECK (modern.closedByBroker());

    //  The right login is taken; a subscriber waits for the will.
    const auto login = [] (RawClient& client, int at, const std::string& id, std::uint16_t keepAlive, std::optional<Will> will)
    {
        REQUIRE (client.connectTo (at));
        auto connect = Packet::connectWith (id, keepAlive);
        connect.user = "stage";
        connect.password = "secret";
        connect.will = std::move (will);
        REQUIRE (client.send (connect));
        const auto answer = client.expect();
        REQUIRE (answer.has_value());
        REQUIRE (answer->returnCode == connack::accepted);
    };

    RawClient watcher, fragile, silent;
    login (watcher, port, "watcher", 60, std::nullopt);
    watcher.subscribe ("status/#");
    login (fragile, port, "fragile", 60, Will { "status/fragile", "gone", 0, false });
    login (silent, port, "silent", 1, Will { "status/silent", "quiet", 0, true });
    CHECK (within (std::chrono::seconds (2), [&] { return broker.stateNow().clients == 3u; }));

    //  Gone without saying: the socket dropped, the will published.
    fragile.socket.close();
    auto will = watcher.expect (3000);
    REQUIRE (will.has_value());
    CHECK (will->topic == "status/fragile");
    CHECK (will->payload == "gone");

    //  Silent past one and a half times a one-second keepalive: dropped, its will out and retained.
    will = watcher.expect (4000);
    REQUIRE (will.has_value());
    CHECK (will->topic == "status/silent");
    CHECK (will->payload == "quiet");
    CHECK (silent.closedByBroker (3000));

    RawClient afterwards;
    login (afterwards, port, "afterwards", 60, std::nullopt);
    afterwards.subscribe ("status/silent");
    const auto kept = afterwards.expect();
    REQUIRE (kept.has_value());
    CHECK (kept->retain);
    CHECK (kept->payload == "quiet");

    //  A second client of one id evicts the first.
    RawClient twin;
    login (twin, port, "watcher", 60, std::nullopt);
    CHECK (watcher.closedByBroker());

    //  A port already taken is said.
    Broker second;
    CHECK_FALSE (second.start (port, {}));
    CHECK (second.stateNow().state == "problem");
    CHECK (second.stateNow().problem.find ("cannot be bound") != std::string::npos);
}

TEST_CASE ("mqtt broker: a link with no socket speaks to the broker in this process")
{
    Broker broker;
    std::string problem;
    CHECK (broker.localLink (problem) == nullptr);
    CHECK (problem == "Go.dot's broker is off");

    REQUIRE (broker.start (0, {}));
    const auto port = broker.stateNow().port;

    auto link = broker.localLink (problem);
    REQUIRE (link != nullptr);

    const auto bytesOf = [] (const Packet& packet)
    {
        std::string why;
        const auto bytes = encode (packet, why);
        REQUIRE_MESSAGE (bytes.has_value(), why);
        return std::string (bytes->begin(), bytes->end());
    };

    //  CONNECT in, CONNACK out, on the writer's own thread.
    REQUIRE (link->write (bytesOf (Packet::connectWith ("godot-MQ000001-laptop", 30))));
    auto got = link->read (std::chrono::milliseconds (500));
    REQUIRE (got.has_value());
    auto decoded = decode (reinterpret_cast<const std::uint8_t*> (got->data()), got->size());
    REQUIRE (decoded.ok);
    CHECK (decoded.packet.type == Type::connack);
    CHECK (decoded.packet.returnCode == connack::accepted);
    CHECK (within (std::chrono::seconds (2), [&] { return broker.stateNow().clients == 1u; }));

    REQUIRE (link->write (bytesOf (Packet::subscribeTo (1, { { "house/#", 0 } }))));
    got = link->read (std::chrono::milliseconds (500));
    REQUIRE (got.has_value());
    CHECK (decode (reinterpret_cast<const std::uint8_t*> (got->data()), got->size()).packet.type == Type::suback);

    //  A socket client's publish reaches the link; the link's publish reaches the socket.
    RawClient sensor;
    sensor.join (port, "sensor");
    sensor.subscribe ("house/lights/#");
    REQUIRE (sensor.send (Packet::publishOf ("house/room1/temp", "23.5")));
    got = link->read (std::chrono::milliseconds (1000));
    REQUIRE (got.has_value());
    decoded = decode (reinterpret_cast<const std::uint8_t*> (got->data()), got->size());
    REQUIRE (decoded.ok);
    CHECK (decoded.packet.topic == "house/room1/temp");
    CHECK (decoded.packet.payload == "23.5");

    REQUIRE (link->write (bytesOf (Packet::publishOf ("house/lights/1", "1"))));
    const auto heard = sensor.expect();
    REQUIRE (heard.has_value());
    CHECK (heard->topic == "house/lights/1");

    //  The link subscribed to house/#, so it hears its own publish back, as any client does.
    got = link->read (std::chrono::milliseconds (500));
    REQUIRE (got.has_value());
    CHECK (decode (reinterpret_cast<const std::uint8_t*> (got->data()), got->size()).packet.topic == "house/lights/1");

    //  Nothing to read is an empty string, as a quiet port's; the broker stopping closes the link.
    CHECK (link->read (std::chrono::milliseconds (50)) == std::string {});
    broker.stop();
    CHECK_FALSE (link->read (std::chrono::milliseconds (500)).has_value());
    CHECK_FALSE (link->write (bytesOf (Packet::plain (Type::pingreq))));
    link.reset();
}
