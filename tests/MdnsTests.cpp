// This file is part of Go.dot — https://github.com/pob31/go.dot
//
// Copyright (C) 2026 Pierre-Olivier Boulant
//
// Go.dot is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. Go.dot is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
// (LICENSE, at the repository root) for more details.
//
// SPDX-License-Identifier: GPL-3.0-or-later

/*
    MULTICAST DNS, FROM BYTES ALONE (namespace draft §76, UD.6).

    The responses here are built byte by byte, compression pointers and all,
    by a writer of this file's own - not by the codec's encoder, which never
    compresses - so a decoder that agreed with its own encoder about a mistake
    would still be caught. The shapes are what a real responder sends: a PTR
    answer naming an instance, its SRV, TXT and the host's A as additional
    records, names pointing back into each other.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/oscquery/Mdns.h>

#include "TestSupport.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace wfg;

namespace
{
    /*  A message written by hand: labels, pointers, numbers, and lengths
        filled in once what they measure has been written. */
    struct Bytes
    {
        std::vector<std::uint8_t> b;

        std::size_t here() const { return b.size(); }
        void u8 (unsigned v) { b.push_back (static_cast<std::uint8_t> (v & 0xFFu)); }
        void u16 (unsigned v) { u8 (v >> 8); u8 (v); }
        void u32 (std::uint32_t v) { u16 (v >> 16); u16 (v & 0xFFFFu); }

        void label (std::string_view text)
        {
            u8 (static_cast<unsigned> (text.size()));
            b.insert (b.end(), text.begin(), text.end());
        }

        void pointer (std::size_t offset) { u8 (0xC0u | static_cast<unsigned> (offset >> 8)); u8 (static_cast<unsigned> (offset)); }
        void end() { u8 (0); }

        /*  A record's head up to its length, which `close` fills in. */
        std::size_t open (unsigned type, unsigned klass, std::uint32_t ttl)
        {
            u16 (type);
            u16 (klass);
            u32 (ttl);
            const auto at = here();
            u16 (0);
            return at;
        }

        void close (std::size_t lengthAt)
        {
            const auto length = here() - lengthAt - 2;
            b[lengthAt] = static_cast<std::uint8_t> (length >> 8);
            b[lengthAt + 1] = static_cast<std::uint8_t> (length & 0xFFu);
        }
    };

    /*  WHAT CHATAIGNE ON A LAPTOP ANSWERS, in shape: one PTR answer, then
        SRV, TXT, A and an AAAA (stepped over) as additional records, every
        repeated name a pointer back. */
    std::vector<std::uint8_t> chataigneAnswers()
    {
        Bytes m;
        m.u16 (0);          // id
        m.u16 (0x8400);     // response, authoritative
        m.u16 (0);          // questions
        m.u16 (1);          // answers
        m.u16 (0);          // authorities
        m.u16 (4);          // additionals

        //  The answer: _oscjson._tcp.local PTR Chataigne._oscjson._tcp.local
        const auto service = m.here();
        m.label ("_oscjson");
        m.label ("_tcp");
        const auto local = m.here();
        m.label ("local");
        m.end();
        auto length = m.open (12, 1, 4500);
        const auto instance = m.here();
        m.label ("Chataigne");
        m.pointer (service);
        m.close (length);

        //  SRV: port 42000 on chataigne-pc.local
        m.pointer (instance);
        length = m.open (33, 0x8001, 120);
        m.u16 (0);
        m.u16 (0);
        m.u16 (42000);
        const auto host = m.here();
        m.label ("chataigne-pc");
        m.pointer (local);
        m.close (length);

        //  TXT: two strings and an empty one
        m.pointer (instance);
        length = m.open (16, 0x8001, 4500);
        m.label ("txtvers=1");
        m.u8 (0);
        m.label ("oscport=12000");
        m.close (length);

        //  AAAA, which this codec steps over by its length
        m.pointer (host);
        length = m.open (28, 0x8001, 120);
        for (auto at = 0; at < 16; ++at)
            m.u8 (static_cast<unsigned> (at));
        m.close (length);

        //  A: 10.0.0.42
        m.pointer (host);
        length = m.open (1, 0x8001, 120);
        m.u8 (10); m.u8 (0); m.u8 (0); m.u8 (42);
        m.close (length);

        return m.b;
    }

    const mdns::Record* first (const std::vector<mdns::Record>& records, std::uint16_t type)
    {
        for (const auto& record : records)
            if (record.type == type)
                return &record;

        return nullptr;
    }
}

TEST_CASE ("mdns: a query for the OSCQuery service is the bytes RFC 6762 says")
{
    const auto query = mdns::encodeQuery ({ { mdns::nameOf ("_oscjson._tcp.local"), mdns::type::ptr, true } });

    const std::vector<std::uint8_t> expected {
        0, 0,  0, 0,  0, 1,  0, 0,  0, 0,  0, 0,
        8, '_', 'o', 's', 'c', 'j', 's', 'o', 'n',
        4, '_', 't', 'c', 'p',
        5, 'l', 'o', 'c', 'a', 'l',
        0,
        0, 12,          // PTR
        0x80, 1 };      // IN, unicast response asked

    CHECK (query == expected);

    //  And read back: one question, the QU bit, not a response.
    const auto read = mdns::decode (query);
    REQUIRE (read.has_value());
    CHECK_FALSE (read->response);
    REQUIRE (read->questions.size() == 1u);
    CHECK (mdns::sameName (read->questions[0].name, mdns::nameOf ("_oscjson._tcp.local")));
    CHECK (read->questions[0].type == mdns::type::ptr);
    CHECK (read->questions[0].unicastResponse);
}

TEST_CASE ("mdns: a responder's answer is read through its pointers: instance, host, port, notes, address")
{
    const auto bytes = chataigneAnswers();
    const auto read = mdns::decode (bytes);
    REQUIRE (read.has_value());
    CHECK (read->response);
    REQUIRE (read->answers.size() == 1u);
    REQUIRE (read->additionals.size() == 4u);

    const auto& ptr = read->answers[0];
    CHECK (ptr.type == mdns::type::ptr);
    CHECK (ptr.ttl == 4500u);
    CHECK_FALSE (ptr.cacheFlush);
    CHECK (ptr.target == mdns::Name { "Chataigne", "_oscjson", "_tcp", "local" });

    const auto all = read->records();
    const auto* srv = first (all, mdns::type::srv);
    REQUIRE (srv != nullptr);
    CHECK (srv->cacheFlush);
    CHECK (srv->ttl == 120u);
    CHECK (srv->port == 42000);
    CHECK (mdns::sameName (srv->name, ptr.target));
    CHECK (srv->target == mdns::Name { "chataigne-pc", "local" });

    const auto* txt = first (all, mdns::type::txt);
    REQUIRE (txt != nullptr);
    CHECK (txt->txt == std::vector<std::string> { "txtvers=1", "oscport=12000" });

    const auto* a = first (all, mdns::type::a);
    REQUIRE (a != nullptr);
    CHECK (mdns::sameName (a->name, srv->target));
    CHECK (a->addressText() == "10.0.0.42");

    //  The AAAA is there, its data stepped over.
    CHECK (first (all, 28) != nullptr);
}

TEST_CASE ("mdns: a response written is read back the same, an instance's own dot kept inside its label")
{
    const auto instance = mdns::Name { "Go.dot on STAGE-PC", "_oscjson", "_tcp", "local" };
    const auto host = mdns::Name { "godot-stage-pc", "local" };

    mdns::Record ptr;
    ptr.name = mdns::nameOf ("_oscjson._tcp.local");
    ptr.type = mdns::type::ptr;
    ptr.ttl = 4500;
    ptr.target = instance;

    mdns::Record srv;
    srv.name = instance;
    srv.type = mdns::type::srv;
    srv.cacheFlush = true;
    srv.ttl = 120;
    srv.port = 5010;
    srv.target = host;

    mdns::Record txt;
    txt.name = instance;
    txt.type = mdns::type::txt;
    txt.cacheFlush = true;
    txt.ttl = 4500;

    mdns::Record a;
    a.name = host;
    a.type = mdns::type::a;
    a.cacheFlush = true;
    a.ttl = 120;
    a.address = { 192, 168, 1, 20 };

    const auto question = mdns::Question { mdns::nameOf ("_oscjson._tcp.local"), mdns::type::ptr, false };
    const auto bytes = mdns::encodeResponse ({ ptr }, { srv, txt, a }, 77, { question });
    const auto read = mdns::decode (bytes);
    REQUIRE (read.has_value());
    CHECK (read->id == 77);
    CHECK (read->response);
    REQUIRE (read->questions.size() == 1u);
    REQUIRE (read->answers.size() == 1u);
    REQUIRE (read->additionals.size() == 3u);

    CHECK (read->answers[0].target == instance);
    CHECK (read->answers[0].target.front() == "Go.dot on STAGE-PC");
    CHECK (read->additionals[0].port == 5010);
    CHECK (read->additionals[0].target == host);
    CHECK (read->additionals[0].cacheFlush);

    //  An empty TXT is one empty string on the wire, and nothing when read.
    CHECK (read->additionals[1].txt.empty());
    CHECK (read->additionals[2].addressText() == "192.168.1.20");

    //  And a goodbye is the same with nought for its time to live.
    ptr.ttl = 0;
    const auto goodbye = mdns::decode (mdns::encodeResponse ({ ptr }));
    REQUIRE (goodbye.has_value());
    CHECK (goodbye->answers[0].ttl == 0u);
}

TEST_CASE ("mdns: what is not a message is nothing - cut short, a pointer forward or round, a reserved label")
{
    const auto bytes = chataigneAnswers();

    //  Cut anywhere, it is not a message - and never read past its end.
    for (std::size_t cut = 0; cut < bytes.size(); ++cut)
    {
        INFO ("cut at: " << cut);
        CHECK_FALSE (mdns::decode (bytes.data(), cut).has_value());
    }

    CHECK_FALSE (mdns::decode (nullptr, 0).has_value());

    //  A name that points at itself.
    {
        Bytes m;
        m.u16 (0); m.u16 (0x8400); m.u16 (0); m.u16 (1); m.u16 (0); m.u16 (0);
        m.pointer (12);
        const auto length = m.open (12, 1, 10);
        m.end();
        m.close (length);
        CHECK_FALSE (mdns::decode (m.b).has_value());
    }

    //  A name that points forward.
    {
        Bytes m;
        m.u16 (0); m.u16 (0x8400); m.u16 (0); m.u16 (1); m.u16 (0); m.u16 (0);
        m.pointer (40);
        const auto length = m.open (12, 1, 10);
        m.end();
        m.close (length);
        while (m.here() < 60)
            m.u8 (0);
        CHECK_FALSE (mdns::decode (m.b).has_value());
    }

    //  Two pointers that lead to each other.
    {
        Bytes m;
        m.u16 (0); m.u16 (0x8400); m.u16 (0); m.u16 (1); m.u16 (0); m.u16 (0);
        m.label ("a");          // 12: a, then a pointer at 14 back to 12
        m.pointer (12);
        const auto length = m.open (12, 1, 10);
        m.end();
        m.close (length);
        CHECK_FALSE (mdns::decode (m.b).has_value());
    }

    //  A label of the reserved kinds (0x40, 0x80).
    for (const auto reserved : { 0x40u, 0x80u })
    {
        Bytes m;
        m.u16 (0); m.u16 (0x8400); m.u16 (0); m.u16 (1); m.u16 (0); m.u16 (0);
        m.u8 (reserved | 1u);
        m.u8 ('x');
        m.end();
        const auto length = m.open (12, 1, 10);
        m.end();
        m.close (length);
        CHECK_FALSE (mdns::decode (m.b).has_value());
    }

    //  An A record whose data is not four bytes.
    {
        Bytes m;
        m.u16 (0); m.u16 (0x8400); m.u16 (0); m.u16 (1); m.u16 (0); m.u16 (0);
        m.label ("host");
        m.label ("local");
        m.end();
        const auto length = m.open (1, 1, 10);
        m.u8 (10); m.u8 (0); m.u8 (0);
        m.close (length);
        CHECK_FALSE (mdns::decode (m.b).has_value());
    }
}

TEST_CASE ("mdns: names compare as DNS compares them, and are split and joined at their dots")
{
    CHECK (mdns::nameOf ("_oscjson._tcp.local") == mdns::Name { "_oscjson", "_tcp", "local" });
    CHECK (mdns::nameOf ("_oscjson._tcp.local.") == mdns::Name { "_oscjson", "_tcp", "local" });
    CHECK (mdns::nameOf ("").empty());
    CHECK (mdns::dotted (mdns::Name { "a", "b" }) == "a.b");
    CHECK (mdns::sameName (mdns::nameOf ("_OSCJSON._TCP.Local"), mdns::nameOf ("_oscjson._tcp.local")));
    CHECK_FALSE (mdns::sameName (mdns::nameOf ("_osc._udp.local"), mdns::nameOf ("_oscjson._tcp.local")));
    CHECK_FALSE (mdns::sameName (mdns::nameOf ("a.local"), mdns::nameOf ("local")));
}
