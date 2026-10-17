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

#include <wfg/engine/oscquery/Discovery.h>
#include <wfg/engine/oscquery/Mdns.h>

#include "TestSupport.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
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

//==============================================================================
//  THE BROWSER (namespace draft §76, UD.7), against a responder of the test's
//  own on loopback, asked directly - no part of the network's 5353 is touched.
namespace
{
    /*  A responder: a socket on 127.0.0.1 that answers each question it is
        put with what `answer` says, to whoever asked. */
    struct FakeResponder
    {
        using Answer = std::function<std::vector<std::uint8_t> (const mdns::Message&, int queryNumber)>;

        explicit FakeResponder (Answer answerWith) : answer (std::move (answerWith))
        {
            REQUIRE (socket.bindToPort (0, "127.0.0.1"));
            thread = std::thread ([this] { run(); });
        }

        ~FakeResponder()
        {
            stopping.store (true);
            thread.join();
            socket.shutdown();
        }

        int port() const { return socket.getBoundPort(); }

        std::vector<mdns::Question> asked() const
        {
            const std::lock_guard<std::mutex> lock { guard };
            return questions;
        }

        void sendTo (int toPort, const std::vector<std::uint8_t>& bytes)
        {
            socket.write ("127.0.0.1", toPort, bytes.data(), static_cast<int> (bytes.size()));
        }

        void run()
        {
            std::vector<std::uint8_t> buffer (9000);

            while (! stopping.load())
            {
                if (socket.waitUntilReady (true, 20) != 1)
                    continue;

                juce::String fromIp;
                int fromPort = 0;
                const auto got = socket.read (buffer.data(), static_cast<int> (buffer.size()), false, fromIp, fromPort);
                const auto message = got > 0 ? mdns::decode (buffer.data(), static_cast<std::size_t> (got)) : std::nullopt;

                if (! message || message->response)
                    continue;

                const auto number = ++queries;

                {
                    const std::lock_guard<std::mutex> lock { guard };
                    questions.insert (questions.end(), message->questions.begin(), message->questions.end());
                }

                const auto bytes = answer (*message, number);

                if (! bytes.empty())
                    socket.write (fromIp, fromPort, bytes.data(), static_cast<int> (bytes.size()));
            }
        }

        Answer answer;
        juce::DatagramSocket socket { false };
        std::atomic<bool> stopping { false };
        std::atomic<int> queries { 0 };
        mutable std::mutex guard;
        std::vector<mdns::Question> questions;
        std::thread thread;
    };

    const mdns::Name service = mdns::nameOf ("_oscjson._tcp.local");
    const mdns::Name instance { "Chataigne", "_oscjson", "_tcp", "local" };
    const mdns::Name host { "chataigne-pc", "local" };

    mdns::Record ptrRecord (std::uint32_t ttl)
    {
        mdns::Record record;
        record.name = service;
        record.type = mdns::type::ptr;
        record.ttl = ttl;
        record.target = instance;
        return record;
    }

    mdns::Record srvRecord (std::uint32_t ttl, std::uint16_t port = 42000)
    {
        mdns::Record record;
        record.name = instance;
        record.type = mdns::type::srv;
        record.cacheFlush = true;
        record.ttl = ttl;
        record.port = port;
        record.target = host;
        return record;
    }

    mdns::Record txtRecord (std::uint32_t ttl)
    {
        mdns::Record record;
        record.name = instance;
        record.type = mdns::type::txt;
        record.cacheFlush = true;
        record.ttl = ttl;
        record.txt = { "txtvers=1" };
        return record;
    }

    mdns::Record aRecord (std::uint32_t ttl)
    {
        mdns::Record record;
        record.name = host;
        record.type = mdns::type::a;
        record.cacheFlush = true;
        record.ttl = ttl;
        record.address = { 127, 0, 0, 1 };
        return record;
    }

    /*  The whole answer to a question about the service, the way a responder
        sends it: the PTR, and the rest as additional records. */
    std::vector<std::uint8_t> wholeAnswer (std::uint32_t ttl)
    {
        return mdns::encodeResponse ({ ptrRecord (ttl) }, { srvRecord (ttl), txtRecord (ttl), aRecord (ttl) });
    }

    oscquery::Discovery::Options askingOnly (int responderPort)
    {
        oscquery::Discovery::Options options;
        options.port = 0;
        options.peers = { { "127.0.0.1", responderPort } };
        return options;
    }

    template <typename Predicate>
    bool within (int milliseconds, Predicate predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds (milliseconds);

        while (std::chrono::steady_clock::now() < deadline)
        {
            if (predicate())
                return true;

            std::this_thread::sleep_for (std::chrono::milliseconds (10));
        }

        return predicate();
    }
}

TEST_CASE ("mdns: the browser finds a server announced to it, and lets it go on a goodbye")
{
    FakeResponder responder ([] (const mdns::Message& asked, int)
                             {
                                 for (const auto& question : asked.questions)
                                     if (question.type == mdns::type::ptr && mdns::sameName (question.name, service))
                                         return wholeAnswer (120);

                                 return std::vector<std::uint8_t> {};
                             });

    oscquery::Discovery browser;
    CHECK (browser.mode() == "off");
    REQUIRE (browser.start (askingOnly (responder.port())));

    //  From a port of its own, asking directly: unicast, no problem.
    CHECK (browser.mode() == "unicast");
    CHECK (browser.problem().empty());
    CHECK (browser.boundPort() > 0);

    REQUIRE (within (3000, [&browser] { return ! browser.found().empty(); }));
    const auto found = browser.found();
    REQUIRE (found.size() == 1u);
    CHECK (found[0].name == "Chataigne");
    CHECK (found[0].host == "127.0.0.1");
    CHECK (found[0].queryPort == 42000);
    CHECK (found[0].txt == std::vector<std::string> { "txtvers=1" });
    CHECK (browser.foundText() == "Chataigne\t127.0.0.1\t42000");

    //  Its question was the service's PTR, "answer me directly".
    const auto asked = responder.asked();
    REQUIRE_FALSE (asked.empty());
    CHECK (mdns::sameName (asked.front().name, service));
    CHECK (asked.front().type == mdns::type::ptr);
    CHECK (asked.front().unicastResponse);

    //  A goodbye, unasked: gone at once, and the revision says so.
    const auto before = browser.revision();
    responder.sendTo (browser.boundPort(), mdns::encodeResponse ({ ptrRecord (0) }));
    CHECK (within (2000, [&browser] { return browser.found().empty(); }));
    CHECK (browser.revision() > before);
    CHECK (browser.foundText().empty());

    browser.stop();
}

TEST_CASE ("mdns: an instance named alone is asked for its host and port, then for the host's address")
{
    /*  A responder that answers each question with that answer alone, as
        one with a crowded packet or a quiet cache does: the browser has to
        ask for what is missing. */
    FakeResponder responder ([] (const mdns::Message& asked, int)
                             {
                                 std::vector<mdns::Record> answers;

                                 for (const auto& question : asked.questions)
                                 {
                                     if (question.type == mdns::type::ptr)
                                         answers.push_back (ptrRecord (120));
                                     else if (question.type == mdns::type::srv && mdns::sameName (question.name, instance))
                                         answers.push_back (srvRecord (120));
                                     else if (question.type == mdns::type::txt && mdns::sameName (question.name, instance))
                                         answers.push_back (txtRecord (120));
                                     else if (question.type == mdns::type::a && mdns::sameName (question.name, host))
                                         answers.push_back (aRecord (120));
                                 }

                                 return answers.empty() ? std::vector<std::uint8_t> {} : mdns::encodeResponse (answers);
                             });

    oscquery::Discovery browser;
    REQUIRE (browser.start (askingOnly (responder.port())));
    REQUIRE (within (4000, [&browser] { return ! browser.found().empty(); }));
    CHECK (browser.foundText() == "Chataigne\t127.0.0.1\t42000");

    bool askedService = false, askedAddress = false;

    for (const auto& question : responder.asked())
    {
        askedService = askedService || (question.type == mdns::type::srv && mdns::sameName (question.name, instance));
        askedAddress = askedAddress || (question.type == mdns::type::a && mdns::sameName (question.name, host));
    }

    CHECK (askedService);
    CHECK (askedAddress);
    browser.stop();
}

TEST_CASE ("mdns: what is not refreshed goes when its time to live runs out, and this Go.dot is left out")
{
    SUBCASE ("one second to live, answered once")
    {
        FakeResponder responder ([] (const mdns::Message&, int number)
                                 {
                                     return number == 1 ? wholeAnswer (1) : std::vector<std::uint8_t> {};
                                 });

        oscquery::Discovery browser;
        REQUIRE (browser.start (askingOnly (responder.port())));
        REQUIRE (within (3000, [&browser] { return ! browser.found().empty(); }));
        CHECK (within (4000, [&browser] { return browser.found().empty(); }));
        browser.stop();
    }

    SUBCASE ("a server at this machine's address on this Go.dot's own port is not a device")
    {
        FakeResponder responder ([] (const mdns::Message&, int) { return wholeAnswer (120); });

        auto options = askingOnly (responder.port());
        options.ownPort = 42000;

        oscquery::Discovery browser;
        REQUIRE (browser.start (options));
        REQUIRE (within (3000, [&responder] { return responder.queries.load() > 0; }));
        std::this_thread::sleep_for (std::chrono::milliseconds (400));
        CHECK (browser.found().empty());
        browser.stop();
    }
}

//==============================================================================
//  GO.DOT ANNOUNCED (namespace draft §76, UD.11), asked directly on loopback.
namespace
{
    oscquery::Discovery::Options announcing (std::vector<std::pair<std::string, int>> peers = {})
    {
        oscquery::Discovery::Options options;
        options.port = 0;
        options.peers = std::move (peers);
        options.ownPort = 5099;
        options.oscPort = 8099;
        options.announceAs = "Go.dot on TEST-BOX";
        return options;
    }

    /*  A querier of the test's own: a socket on 127.0.0.1 that puts a
        question to `port` and keeps every answer it gets. */
    struct Querier
    {
        Querier()
        {
            REQUIRE (socket.bindToPort (0, "127.0.0.1"));
        }

        int port() const { return socket.getBoundPort(); }

        void ask (int toPort, const std::vector<mdns::Question>& questions)
        {
            const auto bytes = mdns::encodeQuery (questions);
            socket.write ("127.0.0.1", toPort, bytes.data(), static_cast<int> (bytes.size()));
        }

        /*  The next answer within the time, questions passed over: the
            announcer browses too, and puts its own to whoever it asks. */
        std::optional<mdns::Message> next (int milliseconds = 2000)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds (milliseconds);

            while (std::chrono::steady_clock::now() < deadline)
            {
                if (socket.waitUntilReady (true, 50) != 1)
                    continue;

                std::vector<std::uint8_t> buffer (9000);
                juce::String fromIp;
                int fromPort = 0;
                const auto got = socket.read (buffer.data(), static_cast<int> (buffer.size()), false, fromIp, fromPort);

                if (got <= 0)
                    continue;

                if (auto message = mdns::decode (buffer.data(), static_cast<std::size_t> (got)); message && message->response)
                    return message;
            }

            return std::nullopt;
        }

        juce::DatagramSocket socket { false };
    };

    const mdns::Name ownInstance { "Go.dot on TEST-BOX", "_oscjson", "_tcp", "local" };
}

TEST_CASE ("mdns: another browser finds Go.dot announced, under its name, at its HTTP port")
{
    oscquery::Discovery announcer;
    REQUIRE (announcer.start (announcing()));

    oscquery::Discovery browser;
    REQUIRE (browser.start (askingOnly (announcer.boundPort())));

    REQUIRE (within (4000, [&browser] { return ! browser.found().empty(); }));
    const auto found = browser.found();
    REQUIRE (found.size() == 1u);
    CHECK (found[0].name == "Go.dot on TEST-BOX");
    CHECK (found[0].queryPort == 5099);
    CHECK (found[0].txt == std::vector<std::string> { "txtvers=1" });
    CHECK_FALSE (found[0].host.empty());

    //  The announcer, a browser too, does not list itself.
    CHECK (announcer.found().empty());

    browser.stop();
    announcer.stop();
}

TEST_CASE ("mdns: the announcer answers about its instance, its host and the services it offers, and withdraws at the stop")
{
    Querier querier;
    oscquery::Discovery announcer;
    REQUIRE (announcer.start (announcing ({ { "127.0.0.1", querier.port() } })));

    SUBCASE ("its instance's SRV and TXT, with the host's address beside them, the question echoed")
    {
        querier.ask (announcer.boundPort(), { { ownInstance, mdns::type::srv, false } });
        const auto heard = querier.next();
        REQUIRE (heard.has_value());
        CHECK (heard->response);
        REQUIRE (heard->questions.size() == 1u);

        const auto all = heard->records();
        const auto* srv = first (all, mdns::type::srv);
        REQUIRE (srv != nullptr);
        CHECK (mdns::sameName (srv->name, ownInstance));
        CHECK (srv->port == 5099);
        CHECK (srv->target.front().rfind ("godot-", 0) == 0);

        //  Asked from this machine, the loopback is the first address.
        const auto* a = first (all, mdns::type::a);
        REQUIRE (a != nullptr);
        CHECK (a->addressText() == "127.0.0.1");
        CHECK (mdns::sameName (a->name, srv->target));
    }

    SUBCASE ("the OSC service on the OSC port")
    {
        querier.ask (announcer.boundPort(), { { mdns::nameOf ("_osc._udp.local"), mdns::type::ptr, false } });
        const auto heard = querier.next();
        REQUIRE (heard.has_value());
        const auto all = heard->records();
        const auto* ptr = first (all, mdns::type::ptr);
        REQUIRE (ptr != nullptr);
        CHECK (ptr->target.front() == "Go.dot on TEST-BOX");
        const auto* srv = first (all, mdns::type::srv);
        REQUIRE (srv != nullptr);
        CHECK (srv->port == 8099);
    }

    SUBCASE ("what services there are")
    {
        querier.ask (announcer.boundPort(), { { mdns::nameOf ("_services._dns-sd._udp.local"), mdns::type::ptr, false } });
        const auto heard = querier.next();
        REQUIRE (heard.has_value());
        std::vector<std::string> offered;

        for (const auto& record : heard->answers)
            offered.push_back (mdns::dotted (record.target));

        CHECK (offered == std::vector<std::string> { "_oscjson._tcp.local", "_osc._udp.local" });
    }

    SUBCASE ("a question about somebody else is not answered")
    {
        querier.ask (announcer.boundPort(), { { mdns::nameOf ("_http._tcp.local"), mdns::type::ptr, false } });
        CHECK_FALSE (querier.next (300).has_value());
    }

    //  At the stop, the goodbye: every record with nought to live.
    announcer.stop();
    std::optional<mdns::Message> goodbye;

    for (auto tries = 0; tries < 8 && ! goodbye; ++tries)
        if (auto heard = querier.next (500); heard && ! heard->answers.empty() && heard->answers.front().ttl == 0)
            goodbye = std::move (heard);

    REQUIRE (goodbye.has_value());
    const auto* withdrawn = first (goodbye->answers, mdns::type::ptr);
    REQUIRE (withdrawn != nullptr);
    CHECK (mdns::sameName (withdrawn->target, ownInstance));
    CHECK (withdrawn->ttl == 0u);
}

TEST_CASE ("mdns: a browser leaves out the Go.dot on its own HTTP port at its own address, and names itself as the author's machine does")
{
    oscquery::Discovery announcer;
    REQUIRE (announcer.start (announcing()));

    auto options = askingOnly (announcer.boundPort());
    options.ownPort = 5099;

    oscquery::Discovery itself;
    REQUIRE (itself.start (options));
    std::this_thread::sleep_for (std::chrono::milliseconds (600));
    CHECK (itself.found().empty());

    itself.stop();
    announcer.stop();

    CHECK (oscquery::Discovery::instanceName ("STAGE-PC", 5010) == "Go.dot on STAGE-PC");
    CHECK (oscquery::Discovery::instanceName ("STAGE-PC", 5011) == "Go.dot on STAGE-PC, port 5011");
}
