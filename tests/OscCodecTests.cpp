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
    OSC on the wire.

    THE BYTE FIXTURES ARE WRITTEN OUT BY HAND, from the specification and not
    from the encoder in this repository. An encoder checked against its own
    output proves that it is self-consistent and nothing else, and a codec's
    whole job is to agree with somebody else's bytes. Where a float appears in a
    fixture it is one whose IEEE-754 encoding can be stated exactly - 440.0 is
    0x43DC0000 - so the fixture can be read and checked by eye rather than
    trusted.

    THE MALFORMED CASES ARE THE POINT of the file. A decoder reads bytes from a
    network, and six of these are not hypothetical: they are defects read out of
    spatcore's OSC parser, the implementation this one replaces. A truncated
    payload delivered as zeroes; a bundle element size checked by an addition
    that overflows; a nested element parsed against the whole datagram instead
    of its own extent; uncapped recursion; an unterminated address accepted; an
    absent type-tag string read as "no arguments". Each has a case below that
    names it, so that a future reader can tell which cases are there for a
    reason and which are there for symmetry.

    (JUCE's own juce::OSCReceiver is not among them. Its problem is different -
    it throws on four of the type tags Go.dot needs - and it is documented at
    the top of UdpEndpoint.h, where the decision not to use it was taken.)
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/osc/OscAddress.h>
#include <wfg/engine/osc/OscCodec.h>
#include <wfg/engine/osc/UdpEndpoint.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace wfg;
using namespace wfg::osc;

namespace
{
    using Bytes = std::vector<std::uint8_t>;

    /** An OSC string as it appears on the wire: NUL-terminated, padded to four. */
    Bytes str (const std::string& text)
    {
        Bytes out (text.begin(), text.end());
        out.push_back (0);

        while (out.size() % 4 != 0)
            out.push_back (0);

        return out;
    }

    Bytes u32 (std::uint32_t v)
    {
        return { static_cast<std::uint8_t> (v >> 24), static_cast<std::uint8_t> (v >> 16),
                 static_cast<std::uint8_t> (v >> 8),  static_cast<std::uint8_t> (v) };
    }

    Bytes u64 (std::uint64_t v)
    {
        auto out = u32 (static_cast<std::uint32_t> (v >> 32));
        const auto lo = u32 (static_cast<std::uint32_t> (v & 0xffffffffu));
        out.insert (out.end(), lo.begin(), lo.end());
        return out;
    }

    Bytes operator+ (Bytes a, const Bytes& b)
    {
        a.insert (a.end(), b.begin(), b.end());
        return a;
    }

    DecodeResult decodeBytes (const Bytes& bytes)
    {
        return decode (bytes.data(), bytes.size());
    }

    /*  Refused, by the named guard.

        The ATOM is asserted and not just the fact of refusal. "It was refused"
        passes for the wrong reason as easily as the right one - a decoder that
        rejected every packet would pass a whole table of those - and the atom
        is what the log's `X` column will carry, so a test that does not pin it
        is not testing what the operator will read. */
    void refuses (const Bytes& bytes, const char* expectedReason, const std::string& what)
    {
        const auto result = decodeBytes (bytes);

        INFO ("case: " << what);
        INFO ("reason given: " << result.reason << " - " << result.error);
        CHECK_FALSE (result.ok);
        CHECK (result.reason == expectedReason);
        CHECK_FALSE (result.error.empty());
    }

    std::string encodeFails (const Packet& packet)
    {
        std::string error;
        const auto encoded = encode (packet, error);

        CHECK_FALSE (encoded.has_value());
        return error;
    }

    template <typename Predicate>
    bool waitUntil (Predicate predicate,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds { 5000 })
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        while (std::chrono::steady_clock::now() < deadline)
        {
            if (predicate())
                return true;

            std::this_thread::sleep_for (std::chrono::milliseconds { 1 });
        }

        return predicate();
    }
}

//==============================================================================
TEST_CASE ("osc address: what the spec reserves, and what it does not")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    CHECK (isValidAddress ("/"));                       // the root
    CHECK (isValidAddress ("/godot"));
    CHECK (isValidAddress ("/godot/cue/B3N8R5TW/name"));
    CHECK (isValidAddress ("/a.b-c_d~e:f@g+h"));        // none of these are reserved

    CHECK_FALSE (isValidAddress (""));
    CHECK_FALSE (isValidAddress ("godot"));             // not absolute
    CHECK_FALSE (isValidAddress ("/godot/"));           // a trailing empty part
    CHECK_FALSE (isValidAddress ("//godot"));           // an empty part
    CHECK_FALSE (isValidAddress ("/godot//cue"));
    CHECK_FALSE (isValidAddress ("/godot cue"));        // space is reserved
    CHECK_FALSE (isValidAddress ("/godot#cue"));        // starts a bundle
    CHECK_FALSE (isValidAddress ("/godot,cue"));        // separates type tags

    // The four wildcards are not an address, but they are a pattern.
    for (const auto* pattern : { "/godot/cue/*/name", "/godot/cue/?/name",
                                 "/godot/[abc]", "/godot/{a,b}" })
    {
        INFO ("pattern: " << pattern);
        CHECK_FALSE (isValidAddress (pattern));
    }

    CHECK (isValidPattern ("/godot/cue/*/name"));
    CHECK (isValidPattern ("/godot/[abc]"));
    CHECK (containsWildcard ("/godot/cue/*/name"));
    CHECK_FALSE (containsWildcard ("/godot/cue/B3N8R5TW/name"));

    // Non-ASCII is not an address. OSC strings are ASCII.
    CHECK_FALSE (isValidAddress ("/godot/caf\xc3\xa9"));

    CHECK (partsOf ("/godot/cue/B3N8R5TW")
             == std::vector<std::string> { "godot", "cue", "B3N8R5TW" });
    CHECK (partsOf ("/").empty());
}

//==============================================================================
TEST_CASE ("osc codec: the specification's own example decodes")
{
    /*  From the OSC 1.0 specification. 440.0f is 0x43DC0000: sign 0, exponent
        135, mantissa 0x5C0000 - which is a number this fixture can be checked
        against by hand rather than by running the encoder. */
    const auto bytes = str ("/oscillator/4/frequency") + str (",f") + u32 (0x43DC0000);

    REQUIRE (bytes.size() % 4 == 0);

    const auto result = decodeBytes (bytes);

    INFO (result.error);
    REQUIRE (result.ok);
    CHECK_FALSE (result.packet.isBundle());
    CHECK (result.packet.address == "/oscillator/4/frequency");
    REQUIRE (result.packet.args.size() == 1);
    CHECK (result.packet.args[0] == Value::float32 (440.0f));
}

TEST_CASE ("osc codec: a string pads to four, and an aligned one takes four more")
{
    /*  The rule that desyncs a reader when it is got wrong: the terminator is
        appended BEFORE the padding, so a string whose length is already a
        multiple of four occupies four more bytes, not zero more. */
    CHECK (str ("").size() == 4);           // just the terminator and its pad
    CHECK (str ("abc").size() == 4);        // 3 + terminator
    CHECK (str ("abcd").size() == 8);       // 4 + terminator, padded
    CHECK (str ("/foo").size() == 8);

    const auto bytes = str ("/x") + str (",ss") + str ("abcd") + str ("abc");
    const auto result = decodeBytes (bytes);

    INFO (result.error);
    REQUIRE (result.ok);
    REQUIRE (result.packet.args.size() == 2);
    CHECK (result.packet.args[0] == Value::string ("abcd"));
    CHECK (result.packet.args[1] == Value::string ("abc"));
}

TEST_CASE ("osc codec: every type tag, read off bytes written by hand")
{
    const Bytes blobContent { 0xde, 0xad, 0xbe };

    auto bytes = str ("/every") + str (",ihfdsbTFNIt");
    bytes = bytes + u32 (0x000003e8);                        // i  1000
    bytes = bytes + u64 (0xffffffffffffffffull);             // h  -1
    bytes = bytes + u32 (0x3f800000);                        // f  1.0
    bytes = bytes + u64 (0x3ff0000000000000ull);             // d  1.0
    bytes = bytes + str ("hello");                           // s
    bytes = bytes + u32 (3) + Bytes { 0xde, 0xad, 0xbe, 0x00 };   // b, 3 bytes then one pad
    //  T F N I carry no payload at all
    bytes = bytes + u64 (1);                                 // t  "immediately"

    REQUIRE (bytes.size() % 4 == 0);

    const auto result = decodeBytes (bytes);

    INFO (result.error);
    REQUIRE (result.ok);
    REQUIRE (result.packet.args.size() == 11);

    CHECK (result.packet.args[0] == Value::int32 (1000));
    CHECK (result.packet.args[1] == Value::int64 (-1));
    CHECK (result.packet.args[2] == Value::float32 (1.0f));
    CHECK (result.packet.args[3] == Value::float64 (1.0));
    CHECK (result.packet.args[4] == Value::string ("hello"));
    CHECK (result.packet.args[5] == Value::blob (Blob { blobContent }));
    CHECK (result.packet.args[6] == Value::boolean (true));
    CHECK (result.packet.args[7] == Value::boolean (false));
    CHECK (result.packet.args[8].isNil());
    CHECK (result.packet.args[9].isImpulse());
    CHECK (result.packet.args[10] == Value::timeTag (TimeTag { 1 }));
}

TEST_CASE ("osc codec: a blob's declared length is its content, and its padding must be there")
{
    // Three bytes of content, one byte of padding, and the length says three.
    const auto three = str ("/b") + str (",b") + u32 (3) + Bytes { 1, 2, 3, 0 };
    const auto result = decodeBytes (three);

    INFO (result.error);
    REQUIRE (result.ok);
    REQUIRE (result.packet.args.size() == 1);
    CHECK (result.packet.args[0].getBlob().bytes == std::vector<std::uint8_t> { 1, 2, 3 });

    // An empty blob is four bytes of length and nothing else.
    const auto empty = str ("/b") + str (",b") + u32 (0);
    const auto emptyResult = decodeBytes (empty);

    REQUIRE (emptyResult.ok);
    CHECK (emptyResult.packet.args[0].getBlob().bytes.empty());

    /*  Content present, padding missing. Caught by the alignment gate rather
        than by the blob code - which is the point of having both: the padding
        is what keeps every later argument where it says it is, so a blob whose
        pad was dropped can only ever arrive as a packet that is not a multiple
        of four. */
    const auto unpadded = decodeBytes (str ("/b") + str (",b") + u32 (3) + Bytes { 1, 2, 3 });

    CHECK_FALSE (unpadded.ok);
    CHECK (unpadded.error == "the packet is not a multiple of four bytes");
}

//==============================================================================
TEST_CASE ("osc codec: a bundle carries its time tag, and its elements keep their order")
{
    const auto first = str ("/one") + str (",i") + u32 (1);
    const auto second = str ("/two") + str (",i") + u32 (2);

    const auto bytes = str ("#bundle") + u64 (0x0102030405060708ull)
                     + u32 (static_cast<std::uint32_t> (first.size())) + first
                     + u32 (static_cast<std::uint32_t> (second.size())) + second;

    const auto result = decodeBytes (bytes);

    INFO (result.error);
    REQUIRE (result.ok);
    REQUIRE (result.packet.isBundle());

    /*  The time tag survives. spatcore reads these eight bytes and throws them
        away, so a bundle that asked to happen at a moment arrives as one that
        asked to happen now - which is the whole of scheduling, lost silently. */
    CHECK (result.packet.time == TimeTag { 0x0102030405060708ull });

    REQUIRE (result.packet.elements.size() == 2);
    CHECK (result.packet.elements[0].address == "/one");
    CHECK (result.packet.elements[1].address == "/two");
    CHECK (result.packet.elements[0].args[0] == Value::int32 (1));
    CHECK (result.packet.elements[1].args[0] == Value::int32 (2));
}

TEST_CASE ("osc codec: a bundle may contain bundles, up to a limit")
{
    auto inner = str ("#bundle") + u64 (1);
    const auto leaf = str ("/x") + str (",");
    inner = inner + u32 (static_cast<std::uint32_t> (leaf.size())) + leaf;

    const auto outer = str ("#bundle") + u64 (1)
                     + u32 (static_cast<std::uint32_t> (inner.size())) + inner;

    const auto result = decodeBytes (outer);

    INFO (result.error);
    REQUIRE (result.ok);
    REQUIRE (result.packet.isBundle());
    REQUIRE (result.packet.elements.size() == 1);
    REQUIRE (result.packet.elements[0].isBundle());
    CHECK (result.packet.elements[0].elements[0].address == "/x");

    /*  And it stops. A bundle costs about twenty bytes a level, so a 64 KB
        datagram buys several thousand stack frames - a remote crash for the
        price of one packet, which is what an unbounded recursive parser is. */
    auto deep = str ("/x") + str (",");

    for (int i = 0; i < maxBundleDepth + 4; ++i)
        deep = str ("#bundle") + u64 (1) + u32 (static_cast<std::uint32_t> (deep.size())) + deep;

    refuses (deep, refusal::tooDeep,
             "a bundle nested past the depth limit");
}

//==============================================================================
TEST_CASE ("osc codec: a truncated payload is refused, never delivered as zeroes")
{
    /*  THE MOST DANGEROUS DEFECT of the parser this replaces, and it is worth
        naming: its readInt32 returns 0 when the buffer runs out, so a message
        whose type-tag string promises three floats and carries none arrives as
        three real parameter changes of 0.0 that nobody sent. On a fader that is
        a snap to zero in the middle of a show.

        The bytes come from WFS-DIY's own fuzz corpus, where the case is
        annotated "expect: ignored". */
    refuses (str ("/x") + str (",fff"), refusal::truncated,
             "a type-tag string promising three floats and carrying none");
    refuses (str ("/x") + str (",ff") + u32 (0), refusal::truncated,
             "two floats promised, one sent");
    refuses (str ("/x") + str (",h") + u32 (0), refusal::truncated,
             "an int64 with only four bytes");
    refuses (str ("/x") + str (",d") + u32 (0), refusal::truncated,
             "a double with only four bytes");
    refuses (str ("/x") + str (",t") + u32 (0), refusal::truncated,
             "a time tag with only four bytes");
    refuses (str ("/x") + str (",s"), refusal::truncated,
             "a string argument promised and not sent");
    refuses (str ("/x") + str (",b"), refusal::truncated,
             "a blob whose length word is not there");

    /*  Truncation FINER than a word never reaches those checks: everything OSC
        contains is padded to four, so a packet that lost part of a word is not
        a multiple of four and is refused before a single argument is read. That
        is the cheapest of the guards and it catches the most. */
    for (const auto& cut : { str ("/x") + str (",f") + Bytes { 0, 0 },
                             str ("/x") + str (",i") + Bytes { 0 },
                             str ("/x") + str (",d") + u32 (0) + Bytes { 0, 0, 0 } })
    {
        const auto result = decodeBytes (cut);

        CHECK_FALSE (result.ok);
        CHECK (result.error == "the packet is not a multiple of four bytes");
    }
}

TEST_CASE ("osc codec: the address must be one, and the type-tag string must be there")
{
    refuses ({}, refusal::notOsc,
             "an empty datagram");
    refuses (Bytes { '/', 'x', 0 }, refusal::notOsc,
             "a datagram that is not a multiple of four bytes");
    refuses (Bytes { '/', 'x', 'y', 'z' }, refusal::badAddress,
             "an address with no terminator");
    refuses (str ("x") + str (","), refusal::badAddress,
             "an address that is not absolute");
    refuses (str ("") + str (","), refusal::badAddress,
             "an empty address");
    refuses (str ("/godot//cue") + str (","), refusal::badAddress,
             "an address with an empty part");

    /*  A pattern is not a bad address, and the refusal says so - a client that
        sent one has asked for something Go.dot does not do, rather than named
        a node that is not there. */
    const auto pattern = decodeBytes (str ("/godot/cue/*/name") + str (","));

    CHECK_FALSE (pattern.ok);
    INFO (pattern.error);
    CHECK (pattern.reason == refusal::addressPattern);
    CHECK (pattern.reason != refusal::badAddress);
    CHECK (pattern.error.find ("pattern") != std::string::npos);

    // OSC 1.1 requires the type-tag string, and requires it to start with a comma.
    refuses (str ("/x"), refusal::noTypeTags,
             "a message with no type-tag string at all");
    refuses (str ("/x") + str ("f"), refusal::noTypeTags,
             "a type-tag string with no leading comma");
    refuses (str ("/x") + str (",") + u32 (0), refusal::trailingBytes,
             "bytes after the last argument");
}

TEST_CASE ("osc codec: an unknown type tag takes the message with it")
{
    /*  It cannot be skipped: how many bytes a tag consumes is a property of the
        tag, so a reader that stepped over an unknown one would desync and every
        argument after it would be invented. */
    const auto result = decodeBytes (str ("/x") + str (",z") + u32 (0));

    CHECK_FALSE (result.ok);
    INFO (result.error);
    CHECK (result.reason == refusal::unknownTypeTag);

    /*  The offending tag is named in the SENTENCE and not in the atom. An atom
        that varied with the input would be an unbounded column - a log nothing
        could group by, and a cardinality an attacker chooses. */
    CHECK (result.error.find ("'z'") != std::string::npos);
    CHECK (result.reason.find ('z') == std::string::npos);

    // From the fuzz corpus: four undefined tags at once.
    refuses (str ("/x") + str (",zzzz") + u64 (0) + u64 (0), refusal::unknownTypeTag,
             "four undefined type tags");
}

TEST_CASE ("osc codec: a blob cannot declare a length the datagram does not have")
{
    refuses (str ("/b") + str (",b") + u32 (0xffffffffu), refusal::badBlob,
             "a blob declaring a negative length");
    refuses (str ("/b") + str (",b") + u32 (0x7fffffffu), refusal::truncated,
             "a blob declaring two gigabytes inside a twelve-byte packet");
    refuses (str ("/b") + str (",b") + u32 (64) + u32 (0), refusal::truncated,
             "a blob longer than the datagram");
}

TEST_CASE ("osc codec: a bundle element's size is checked without overflowing")
{
    const auto header = str ("#bundle") + u64 (1);

    /*  The bound is written as a subtraction. Written the other way round -
        position plus declared size against the total - a declared size near the
        top of the signed range overflows, the guard passes, and the reader
        indexes far outside the buffer. That is a live defect in the parser this
        replaces, reachable from one datagram. */
    refuses (header + u32 (0x7ffffffcu) + u32 (0), refusal::badBundle,
             "an element declaring two gigabytes");
    refuses (header + u32 (0x80000000u) + u32 (0), refusal::badBundle,
             "an element declaring a negative size");
    refuses (header + u32 (0) + u32 (0), refusal::badBundle,
             "an element declaring no bytes");
    refuses (header + u32 (5) + u32 (0) + u32 (0), refusal::badBundle,
             "an element size that is not a multiple of four");
    refuses (header + u32 (64) + u32 (0), refusal::badBundle,
             "an element longer than the bundle");
    refuses (header + u32 (4), refusal::badBundle,
             "an element that declares four bytes and sends none");
    refuses (str ("#bundle"), refusal::badBundle,
             "a bundle with no time tag");
}

TEST_CASE ("osc codec: an element is read against its own extent, not the whole datagram")
{
    /*  The framing hides this one. An element whose arguments run past its
        declared size will, in a parser that hands the whole datagram down,
        quietly read its NEIGHBOUR's bytes as its own - and then skip to the
        declared end as though nothing happened. The values are wrong and
        nothing reports it. */
    const auto liar = str ("/a") + str (",i");          // says one int, carries none
    const auto victim = str ("/b") + str (",i") + u32 (0x41414141);

    const auto bundle = str ("#bundle") + u64 (1)
                      + u32 (static_cast<std::uint32_t> (liar.size())) + liar
                      + u32 (static_cast<std::uint32_t> (victim.size())) + victim;

    const auto result = decodeBytes (bundle);

    INFO (result.error);
    CHECK_FALSE (result.ok);
}

TEST_CASE ("osc codec: only an exact \"#bundle\" marker is a bundle")
{
    /*  Compared in full, terminator included. A seven-byte compare accepts
        "#bundleX" as a bundle and then reads the next eight bytes as a time
        tag, which is a message turned into something else entirely. */
    const auto imposter = Bytes { '#', 'b', 'u', 'n', 'd', 'l', 'e', 'X' } + u64 (1);
    const auto result = decodeBytes (imposter);

    CHECK_FALSE (result.ok);
    INFO (result.error);

    //  Refused as a bad ADDRESS - it never entered the bundle path at all.
    CHECK (result.reason == refusal::badAddress);
}

//==============================================================================
TEST_CASE ("osc codec: what it writes, it reads back exactly")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    const auto packet = Packet::message ("/godot/cue/B3N8R5TW/preWait",
                                         { Value::int32 (-1),
                                           Value::int64 (1234567890123ll),
                                           Value::float32 (0.5f),
                                           Value::float64 (2.5),
                                           Value::string ("House to half"),
                                           Value::blob (Blob { { 0, 1, 2, 3, 4 } }),
                                           Value::boolean (true),
                                           Value::boolean (false),
                                           Value::nil(),
                                           Value::impulse(),
                                           Value::timeTag (TimeTag { 42 }) });

    std::string error;
    const auto encoded = encode (packet, error);

    INFO (error);
    REQUIRE (encoded.has_value());
    CHECK (encoded->size() % 4 == 0);

    const auto result = decode (encoded->data(), encoded->size());

    INFO (result.error);
    REQUIRE (result.ok);
    CHECK (result.packet == packet);
}

TEST_CASE ("osc codec: a bundle round-trips, time tag and nesting included")
{
    const auto inner = Packet::bundle (TimeTag { 7 },
                                       { Packet::message ("/deep", { Value::int32 (3) }) });

    const auto packet = Packet::bundle (TimeTag { 0xdeadbeefcafebabeull },
                                        { Packet::message ("/a"),
                                          Packet::message ("/b", { Value::string ("x") }),
                                          inner });

    std::string error;
    const auto encoded = encode (packet, error);

    INFO (error);
    REQUIRE (encoded.has_value());

    const auto result = decode (encoded->data(), encoded->size());

    INFO (result.error);
    REQUIRE (result.ok);
    CHECK (result.packet == packet);
    CHECK (result.packet.time == TimeTag { 0xdeadbeefcafebabeull });
}

TEST_CASE ("osc codec: encoding refuses what the wire cannot say")
{
    // A NUL inside a string terminates it, so writing one would truncate.
    CHECK (encodeFails (Packet::message ("/x", { Value::string (std::string ("a\0b", 3)) }))
             .find ("NUL") != std::string::npos);

    // Non-finite floats, for the same reason the log refuses them.
    CHECK_FALSE (encodeFails (Packet::message (
                   "/x", { Value::float32 (std::numeric_limits<float>::quiet_NaN()) })).empty());
    CHECK_FALSE (encodeFails (Packet::message (
                   "/x", { Value::float64 (std::numeric_limits<double>::infinity()) })).empty());

    // And an address that is not one.
    CHECK_FALSE (encodeFails (Packet::message ("not-an-address")).empty());
    CHECK_FALSE (encodeFails (Packet::message ("/godot/cue/*/name")).empty());
}

//==============================================================================
TEST_CASE ("udp: a datagram arrives with the sender's address and port on it")
{
    /*  Port 0 on both ends. A fixed port makes a suite that cannot run twice at
        once, and this project runs ctest in parallel. */
    UdpEndpoint receiver;

    std::mutex mutex;
    std::vector<Datagram> arrived;

    REQUIRE (receiver.start (0, [&mutex, &arrived] (Datagram datagram)
                             {
                                 const std::lock_guard<std::mutex> lock { mutex };
                                 arrived.push_back (std::move (datagram));
                             }));

    CHECK (receiver.isRunning());
    REQUIRE (receiver.boundPort() > 0);

    UdpEndpoint sender;
    REQUIRE (sender.start (0, [] (Datagram) {}));
    REQUIRE (sender.boundPort() > 0);

    std::string error;
    const auto packet = Packet::message ("/godot/cmd/standby/next");
    const auto encoded = encode (packet, error);

    INFO (error);
    REQUIRE (encoded.has_value());
    REQUIRE (sender.send ("127.0.0.1", receiver.boundPort(), *encoded));

    REQUIRE (waitUntil ([&mutex, &arrived]
                        {
                            const std::lock_guard<std::mutex> lock { mutex };
                            return ! arrived.empty();
                        }));

    receiver.stop();
    sender.stop();

    REQUIRE (arrived.size() == 1);

    const auto& datagram = arrived.front();

    CHECK (datagram.bytes == *encoded);
    CHECK (datagram.senderIp == "127.0.0.1");
    CHECK (datagram.senderPort == sender.boundPort());

    /*  The origin the log records. The PORT is in it as well as the address:
        two clients behind one NAT share an address, and echo suppression keyed
        on the address alone would silence a message for a surface that never
        sent it. */
    CHECK (datagram.origin() == "udp:127.0.0.1:" + std::to_string (sender.boundPort()));

    // And what arrived decodes to what was sent.
    const auto result = decode (datagram.bytes.data(), datagram.bytes.size());

    REQUIRE (result.ok);
    CHECK (result.packet == packet);
}

TEST_CASE ("udp: start and stop are safe to repeat, and a bad handler is refused")
{
    UdpEndpoint endpoint;

    endpoint.stop();                                            // never started
    CHECK_FALSE (endpoint.start (0, nullptr));                  // no handler
    CHECK_FALSE (endpoint.isRunning());

    REQUIRE (endpoint.start (0, [] (Datagram) {}));
    CHECK_FALSE (endpoint.start (0, [] (Datagram) {}));         // already running

    const auto port = endpoint.boundPort();
    CHECK (port > 0);

    endpoint.stop();
    endpoint.stop();
    CHECK_FALSE (endpoint.isRunning());

    // And it can be started again afterwards, on a new port.
    REQUIRE (endpoint.start (0, [] (Datagram) {}));
    CHECK (endpoint.boundPort() > 0);
    endpoint.stop();
}

TEST_CASE ("osc codec: a datagram the codec refuses becomes an X record naming its sender")
{
    /*  The plan's requirement for this PR, end to end and with no server in it:
        bytes arrive, the codec refuses them, and what the log keeps is a `X`
        record carrying WHO sent it, WHY it was refused, and the bytes
        themselves.

        All three matter. A drop with no origin is a drop nobody can act on -
        the operator cannot unplug the offender. A drop with no reason cannot be
        told from a drop for a different reason. And the payload is the only
        copy: the datagram is gone, and a post-mortem with no packet in it is a
        guess. It is a `X` and not a `R` because nothing was ever rejected -
        there was no command to reject. Replay skips it for the same reason. */
    Engine engine;
    engine.log().openInMemory ({});

    Datagram datagram;
    datagram.bytes = str ("/godot/cmd/standby/next") + str (",fff");   // three floats, none sent
    datagram.senderIp = "192.168.1.7";
    datagram.senderPort = 9000;

    const auto refused = decode (datagram.bytes.data(), datagram.bytes.size());

    REQUIRE_FALSE (refused.ok);

    Drop drop;
    drop.origin = datagram.origin();
    drop.reason = refused.reason;
    drop.payload = datagram.bytes;

    engine.submit (std::move (drop));

    const auto result = engine.processTick (11);

    CHECK (result.applied == 0);
    CHECK (result.rejected == 0);
    CHECK (result.dropped == 1);

    const auto parsed = LogFile::parse (engine.log().contents());

    CHECK (parsed.errors.empty());
    REQUIRE (parsed.records.size() == 1);

    const auto& record = parsed.records.front();

    CHECK (record.kind == LogRecord::Kind::dropped);
    CHECK (record.tick == 11);
    CHECK (record.origin == "udp:192.168.1.7:9000");
    CHECK (record.reason == refusal::truncated);
    CHECK (record.command.empty());

    // The bytes ride along as a blob, and they survive the round trip through
    // the log's base64 exactly - a post-mortem reads the packet, not a summary.
    REQUIRE (record.args.size() == 1);
    REQUIRE (record.args.front().isBlob());
    CHECK (record.args.front().getBlob().bytes == datagram.bytes);
}

TEST_CASE ("udp: a malformed datagram reaches the handler and is refused by the codec")
{
    /*  The two halves stay apart. The endpoint moves bytes and names the
        sender; whether those bytes are OSC is the codec's question. That split
        is what lets every case above be tested against a byte fixture with no
        socket in sight - and it is what makes a malformed packet a logged `X`
        record rather than a silence. */
    UdpEndpoint receiver;

    std::mutex mutex;
    std::vector<Datagram> arrived;

    REQUIRE (receiver.start (0, [&mutex, &arrived] (Datagram datagram)
                             {
                                 const std::lock_guard<std::mutex> lock { mutex };
                                 arrived.push_back (std::move (datagram));
                             }));

    UdpEndpoint sender;
    const Bytes rubbish { '/', 'x', 0, 0, ',', 'f', 'f', 'f' };   // three floats, no payload

    REQUIRE (sender.send ("127.0.0.1", receiver.boundPort(), rubbish));

    REQUIRE (waitUntil ([&mutex, &arrived]
                        {
                            const std::lock_guard<std::mutex> lock { mutex };
                            return ! arrived.empty();
                        }));

    receiver.stop();

    REQUIRE (arrived.size() == 1);
    CHECK (arrived.front().bytes == rubbish);
    CHECK (receiver.datagramsReceived() == 1);

    const auto result = decode (arrived.front().bytes.data(), arrived.front().bytes.size());

    CHECK_FALSE (result.ok);
    INFO (result.error);
    CHECK_FALSE (result.error.empty());
}
