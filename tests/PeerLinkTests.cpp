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
    THE LINK BETWEEN A MAIN AND ITS BACKUP (namespace draft §58, RD.5; AFV).

    The main sends its backup every record it writes that the backup will one
    day apply, as its log writes it, and a heartbeat every tick; the backup
    dials, says what it holds, pings, and counts what it receives. RD.5 is the
    wire and what it says about itself - nothing received is applied yet.

    What is pinned here: what travels and what does not (the engine's own
    records, the machine's, the pairing's stay home; a `node.setMany` that
    mixes the two travels without the machine's pairs); that a log line
    crosses the wire and reads back exactly, the awkward atoms included and a
    French machine's numbers too; the ring a backup that reconnects is
    answered from - counters continuous, a resume inside it exact, one older
    answered `gap`; the backup's counter - two paths count a record once, a
    jump is a gap; when each side says it is linked; the engine's
    after-record hook the main's half hangs off; the two halves joined over
    a real WebSocket, the main going and coming back on its port; and what a
    tick of sixty-four records costs the main.

    Every case runs under fr_FR as well as C: a record's numbers are text.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/command/Command.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/osc/OscCodec.h>
#include <wfg/engine/oscquery/OscQueryServer.h>
#include <wfg/engine/pair/PeerLink.h>
#include <wfg/engine/pair/Presence.h>
#include <wfg/engine/tree/Node.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace wfg;

namespace
{
    using Clock = pair::LinkClock;
    using namespace std::chrono_literals;

    osc::Value s (std::string text) { return osc::Value::string (std::move (text)); }
    osc::Value h (std::int64_t value) { return osc::Value::int64 (value); }

    LogRecord applied (std::string from, std::string command, std::vector<osc::Value> args)
    {
        LogRecord r;
        r.kind = LogRecord::Kind::applied;
        r.tick = 412;
        r.seq = 9001;
        r.origin = std::move (from);
        r.command = std::move (command);
        r.args = std::move (args);
        return r;
    }

    LogRecord refused (std::string from, std::string why, std::string command, std::vector<osc::Value> args)
    {
        auto r = applied (std::move (from), std::move (command), std::move (args));
        r.kind = LogRecord::Kind::rejected;
        r.reason = std::move (why);
        return r;
    }

    Command named (std::string name, bool machine = false)
    {
        Command command;
        command.name = std::move (name);
        command.machineLocal = machine;
        return command;
    }

    /*  Two records are the same record when every field the line carries is
        the same - `operator==` on a value is exact, type and bits. */
    void checkSame (const LogRecord& got, const LogRecord& wanted)
    {
        CHECK (got.kind == wanted.kind);
        CHECK (got.tick == wanted.tick);
        CHECK (got.seq == wanted.seq);
        CHECK (got.origin == wanted.origin);
        CHECK (got.reason == wanted.reason);
        CHECK (got.command == wanted.command);
        REQUIRE (got.args.size() == wanted.args.size());

        for (std::size_t i = 0; i < got.args.size(); ++i)
            CHECK_MESSAGE (got.args[i] == wanted.args[i], "argument " << i << ": " << got.args[i].toAtom()
                                                             << " for " << wanted.args[i].toAtom());
    }

    /*  THE WIRE IN ONE PROCESS: what a stream sends, decoded, by connection,
        a tick's bundle opened into its messages in their order - and a
        connection the server no longer has, which refuses. */
    struct Wire
    {
        std::mutex mutex;
        std::map<std::string, std::vector<osc::Packet>> sent;
        std::map<std::string, int> frames;
        std::set<std::string> gone;

        void open (const std::string& origin, const osc::Packet& packet)
        {
            if (! packet.isBundle())
            {
                sent[origin].push_back (packet);
                return;
            }

            for (const auto& element : packet.elements)
                open (origin, element);
        }

        pair::Stream::Send send()
        {
            return [this] (const std::string& origin, const std::vector<std::uint8_t>& bytes)
            {
                const std::lock_guard<std::mutex> held { mutex };

                if (gone.count (origin) != 0)
                    return false;

                const auto decoded = osc::decode (bytes.data(), bytes.size());

                if (! decoded.ok)
                    return false;

                ++frames[origin];
                open (origin, decoded.packet);
                return true;
            };
        }

        std::vector<osc::Packet> take (const std::string& origin)
        {
            const std::lock_guard<std::mutex> held { mutex };
            auto out = std::move (sent[origin]);
            sent[origin].clear();
            return out;
        }
    };

    osc::Packet hello (std::uint64_t last, std::string stream, std::string role = "backup")
    {
        return osc::Packet::message (pair::wire::hello, { s ("backup-machine"), s (std::move (role)),
                                                          h (static_cast<std::int64_t> (last)), s (std::move (stream)) });
    }

    std::int64_t nanos (Clock::time_point at)
    {
        return static_cast<std::int64_t> (std::chrono::duration_cast<std::chrono::nanoseconds> (at.time_since_epoch()).count());
    }

    osc::Packet ping (Clock::time_point at, float roundTrip)
    {
        return osc::Packet::message (pair::wire::ping, { h (nanos (at)), osc::Value::float32 (roundTrip) });
    }

    /** The counters of the record messages among what was sent, in order. */
    std::vector<std::int64_t> countersIn (const std::vector<osc::Packet>& got)
    {
        std::vector<std::int64_t> out;

        for (const auto& packet : got)
            if (packet.address == pair::wire::record)
                out.push_back (packet.args.at (0).getInt64());

        return out;
    }

    std::vector<std::int64_t> run (std::int64_t from, std::int64_t to)
    {
        std::vector<std::int64_t> out;

        for (auto c = from; c <= to; ++c)
            out.push_back (c);

        return out;
    }

    const Clock::time_point t0 = Clock::time_point {} + std::chrono::hours { 1000 };

    const std::string followerA = "ws:10.0.0.2:50001";
    const std::string followerB = "ws:10.0.0.2:50002";
    const std::string followerC = "ws:10.0.0.2:50003";
}

//==============================================================================
TEST_CASE ("peer link: an operator's GO, a show edit and a refused GO travel; the engine's, the machine's and the pairing's do not")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    const auto go = named ("go");
    const auto nodeSet = named ("node.set");
    const auto nodeSetMany = named ("node.setMany");
    const auto configure = named ("audio.configure", true);
    const auto ended = named ("run.ended");

    //  An operator's GO, as applied: the runs it made are in it.
    const auto goPressed = applied ("udp:10.0.0.9:9000", "go", { s ("R8M2K4Q1"), s ("R8M2K4Q2") });
    REQUIRE (pair::keptForLink (goPressed, &go).has_value());
    checkSame (*pair::keptForLink (goPressed, &go), goPressed);

    //  A show edit.
    const auto renamed = applied ("ws:192.168.1.20:53211", "node.set", { s ("/godot/cue/B3N8R5TW/name"), s ("House") });
    REQUIRE (pair::keptForLink (renamed, &nodeSet).has_value());
    checkSame (*pair::keptForLink (renamed, &nodeSet), renamed);

    //  A refused GO: the backup must refuse it too, as a replay does.
    const auto tooSoon = refused ("udp:10.0.0.9:9000", "too-soon", "go", {});
    REQUIRE (pair::keptForLink (tooSoon, &go).has_value());
    checkSame (*pair::keptForLink (tooSoon, &go), tooSoon);

    //  And a command nobody registered, refused as it was refused here.
    const auto unknown = refused ("ws:192.168.1.20:53211", "unknown-command", "no.such", { s ("x") });
    CHECK (pair::travels (unknown, nullptr));

    //  The engine's own records are made on each machine (AGE); a replay's are a replay's.
    CHECK_FALSE (pair::travels (applied (std::string (origin::engine), "run.ended", { s ("R8M2K4Q1") }), &ended));
    CHECK_FALSE (pair::travels (applied (std::string (origin::replay), "go", {}), &go));

    /*  Nor what this machine applied from the other's stream (RD.7): a follower
        that took over, or two machines both sending, would send it round again. */
    CHECK_FALSE (pair::travels (applied ("main:udp:10.0.0.9:9000", "go", { s ("R8M2K4Q1") }), &go));
    CHECK_FALSE (pair::travels (applied ("main:peer:ws:192.168.1.20:53211", "go", {}), &go));
    CHECK (pair::travels (applied ("peer:ws:192.168.1.20:53211", "go", {}), &go));

    //  A command that acts on this machine alone (RD.3), whoever asked.
    CHECK_FALSE (pair::travels (applied ("ws:192.168.1.20:53211", "audio.configure", { s ("ASIO") }), &configure));

    //  A value set on one of this machine's rows.
    CHECK_FALSE (pair::travels (applied ("ws:192.168.1.20:53211", "node.set",
                                         { s ("/godot/audio/outputDevice"), s ("Fireface UCX") }), &nodeSet));

    //  The pairing's commands, by name, even one that were somehow not marked the machine's.
    const auto unmarked = named ("pair.setRole");
    CHECK_FALSE (pair::travels (applied ("cli", "pair.setRole", { s ("main") }), &unmarked));

    //  A packet that never became a command.
    LogRecord dropped;
    dropped.kind = LogRecord::Kind::dropped;
    dropped.origin = "udp:10.0.0.9:9000";
    dropped.reason = "not-osc";
    CHECK_FALSE (pair::travels (dropped, nullptr));

    /*  A `node.setMany` that mixes the two travels WITHOUT the machine's pairs,
        the show's as applied and in their order; one of nothing but machine
        rows stays home. */
    const auto mixed = applied ("ws:192.168.1.20:53211", "node.setMany",
                                { s ("/godot/cue/B3N8R5TW/name"), s ("A"),
                                  s ("/godot/audio/outputDevice"), s ("Fireface UCX"),
                                  s ("/godot/cue/B3N8R5TW/notes"), s ("B") });
    const auto sent = pair::keptForLink (mixed, &nodeSetMany);
    REQUIRE (sent.has_value());

    auto wanted = mixed;
    wanted.args = { s ("/godot/cue/B3N8R5TW/name"), s ("A"), s ("/godot/cue/B3N8R5TW/notes"), s ("B") };
    checkSame (*sent, wanted);

    CHECK_FALSE (pair::travels (applied ("ws:192.168.1.20:53211", "node.setMany",
                                         { s ("/godot/audio/outputDevice"), s ("Fireface UCX"),
                                           s ("/godot/audio/inputDevice"), s ("Fireface UCX") }),
                                &nodeSetMany));
}

//==============================================================================
TEST_CASE ("peer link: a record's line crosses the wire and reads back exactly, the awkward atoms included")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    /*  WHAT A LINE HAS TO SURVIVE: spaces, quotes and backslashes in a string,
        an empty one, every control character and a NUL, letters beyond ASCII,
        a string longer than one WebSocket frame's short length; blobs empty
        and holding every byte; the edges of every number type, subnormals and
        negative zero, which the locale must not touch; `h` kept `h` beside an
        `i` of the same value; the four bare atoms; time tags; a refusal's
        reason; an origin from an IPv6 peer; the largest sequence number. */
    std::string controls;

    for (int c = 0; c < 32; ++c)
        controls.push_back (static_cast<char> (c));

    controls.push_back (static_cast<char> (127));

    osc::Blob everyByte;

    for (int b = 0; b < 256; ++b)
        everyByte.bytes.push_back (static_cast<std::uint8_t> (b));

    std::vector<LogRecord> table;
    table.push_back (applied ("ws:192.168.1.20:53211", "node.set",
                              { s ("/godot/cue/B3N8R5TW/name"), s ("left wing, \"quoted\" and \\ back\\") }));
    table.push_back (applied ("udp:10.0.0.9:9000", "node.set", { s ("/godot/cue/B3N8R5TW/notes"), s ("") }));
    table.push_back (applied ("cli", "node.set", { s ("/godot/cue/B3N8R5TW/notes"), s ("one\ntwo\tthree\rfour") }));
    table.push_back (applied ("cli", "node.set", { s ("/godot/cue/B3N8R5TW/notes"), s (controls) }));
    table.push_back (applied ("cli", "node.set", { s ("/godot/cue/B3N8R5TW/notes"),
                                                   s ("Entr\xC3\xA9" "e des artistes \xE2\x80\x94 \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x8E\xAD") }));
    table.push_back (applied ("cli", "node.set", { s ("/godot/cue/B3N8R5TW/notes"), s (std::string (100000, 'x')) }));
    table.push_back (applied ("pd", "test.blobs", { osc::Value::blob ({}), osc::Value::blob (everyByte) }));
    table.push_back (applied ("cli", "test.ints",
                              { osc::Value::int32 (std::numeric_limits<std::int32_t>::min()),
                                osc::Value::int32 (std::numeric_limits<std::int32_t>::max()),
                                osc::Value::int32 (0), osc::Value::int32 (-1), osc::Value::int32 (5),
                                h (5), h (std::numeric_limits<std::int64_t>::min()),
                                h (std::numeric_limits<std::int64_t>::max()) }));
    table.push_back (applied ("cli", "test.floats",
                              { osc::Value::float32 (0.1f), osc::Value::float32 (-0.0f), osc::Value::float32 (1.5f),
                                osc::Value::float32 (std::numeric_limits<float>::denorm_min()),
                                osc::Value::float32 (std::numeric_limits<float>::min()),
                                osc::Value::float32 (std::numeric_limits<float>::max()),
                                osc::Value::float32 (-123456.789f) }));
    table.push_back (applied ("cli", "test.doubles",
                              { osc::Value::float64 (0.1), osc::Value::float64 (-0.0), osc::Value::float64 (1.5),
                                osc::Value::float64 (std::numeric_limits<double>::denorm_min()),
                                osc::Value::float64 (std::numeric_limits<double>::max()),
                                osc::Value::float64 (1234567.890123), osc::Value::float64 (-2.5e-7) }));
    table.push_back (applied ("cli", "test.bare",
                              { osc::Value::boolean (true), osc::Value::boolean (false), osc::Value::nil(),
                                osc::Value::impulse(), osc::Value::timeTag ({ 0 }), osc::Value::timeTag ({ 1 }),
                                osc::Value::timeTag ({ std::numeric_limits<std::uint64_t>::max() }) }));
    table.push_back (applied ("ws:::1:52011", "standby.next", {}));

    auto refusal = refused ("ws:fe80::1c2b:5010", "unknown-id", "cue.fire", { s ("ZZZZZZZZ") });
    refusal.seq = std::numeric_limits<std::uint64_t>::max();
    refusal.detail = "/godot/cue/ZZZZZZZZ/name";       // never on the line
    table.push_back (refusal);

    //  Through the real stream: kept, ringed, sent to a follower, decoded.
    Wire wire;
    pair::Stream stream { "S1" };
    stream.setSend (wire.send());
    stream.setLeading (true);

    for (const auto& record : table)
    {
        const auto command = named (record.command);
        stream.keep (record, &command);
    }

    REQUIRE (stream.lastCounter() == table.size());

    stream.received (followerA, hello (0, ""), t0);
    stream.afterTick (1, 1.0);

    const auto got = wire.take (followerA);
    std::vector<osc::Packet> records;

    for (const auto& packet : got)
        if (packet.address == pair::wire::record)
            records.push_back (packet);

    REQUIRE (records.size() == table.size());

    //  And through the backup's receiver, which reads the line on the path's thread.
    pair::Receiver receiver;
    std::vector<LogRecord> received;
    receiver.setOnRecord ([&received] (std::uint64_t, const LogRecord& record) { received.push_back (record); });

    for (const auto& packet : got)
        receiver.deliver ("10.0.0.1:5010", packet, t0);

    receiver.drain (t0, 1.0);
    REQUIRE (received.size() == table.size());

    for (std::size_t i = 0; i < table.size(); ++i)
    {
        INFO ("record " << i << ": " << table[i].toLine().substr (0, 200));

        const auto& packet = records[i];
        REQUIRE (packet.args.size() == 2);
        CHECK (packet.args[0] == h (static_cast<std::int64_t> (i + 1)));
        REQUIRE (packet.args[1].isString());
        CHECK (packet.args[1].getString() == table[i].toLine());

        const auto back = LogRecord::fromLine (packet.args[1].getString());
        REQUIRE (back.has_value());
        checkSame (*back, table[i]);
        CHECK (back->detail.empty());

        checkSame (received[i], table[i]);
    }
}

//==============================================================================
TEST_CASE ("peer link: a record and a beat written straight into their bytes are the codec's bytes")
{
    const auto line = applied ("ws:192.168.1.20:53211", "node.set",
                               { s ("/godot/cue/B3N8R5TW/name"), s ("left wing") }).toLine();

    for (const auto& text : { line, std::string(), std::string ("abc"), std::string ("abcd") })
    {
        std::vector<std::uint8_t> written { 9, 9, 9 };      // a reused buffer, longer or shorter
        REQUIRE (pair::wire::writeRecord (written, 4242, text));
        CHECK (written == pair::wire::encode (pair::wire::record, { h (4242), s (text) }));
    }

    //  RD.7: a relay's ticket after the line, and the cues running in the beat.
    std::vector<std::uint8_t> ticketed;
    REQUIRE (pair::wire::writeRecord (ticketed, 4242, line, 0x2a00000007ull));
    CHECK (ticketed == pair::wire::encode (pair::wire::record, { h (4242), s (line), h (0x2a00000007ll) }));

    std::vector<std::uint8_t> beat;
    pair::wire::writeBeat (beat, -7, 123456789, 3);
    CHECK (beat == pair::wire::encode (pair::wire::beat, { h (-7), h (123456789), h (3) }));

    //  A NUL cannot be one OSC string, and the buffer is left as it was.
    std::vector<std::uint8_t> untouched { 1, 2, 3 };
    CHECK_FALSE (pair::wire::writeRecord (untouched, 1, std::string ("a\0b", 3)));
    CHECK (untouched == std::vector<std::uint8_t> { 1, 2, 3 });
}

//==============================================================================
TEST_CASE ("peer link: RD.7's wire - a hello says what a machine is, both ways, and a message knows which half it is for")
{
    pair::wire::Hello said;
    said.machine = "STAGE-LEFT";
    said.role = "backup";
    said.lastCounter = pair::wire::fromNow;
    said.stream = "S-main";
    said.state = "following";
    said.ownStream = "S-backup";
    said.fresh = true;
    said.liveRuns = 3;
    said.salt = 0xfedcba9876543210ull;
    said.described = true;

    const auto bytes = pair::wire::helloBytes (said);
    const auto decoded = osc::decode (bytes.data(), bytes.size());
    REQUIRE (decoded.ok);

    //  Eleven since RD.10: the show's fingerprint and its media's digest after RD.7's nine.
    CHECK (decoded.packet.args.size() == 11u);

    const auto read = pair::wire::readHello (decoded.packet);
    REQUIRE (read.has_value());
    CHECK (read->machine == said.machine);
    CHECK (read->role == said.role);
    CHECK (read->lastCounter == said.lastCounter);
    CHECK (read->stream == said.stream);
    CHECK (read->state == said.state);
    CHECK (read->ownStream == said.ownStream);
    CHECK (read->fresh);
    CHECK (read->liveRuns == 3);
    CHECK (read->salt == said.salt);
    CHECK (read->described);
    CHECK (read->wantsStream());
    CHECK (read->showFingerprint.empty());

    //  RD.7's nine still read, saying no show.
    {
        auto nine = decoded.packet;
        nine.args.resize (9);
        const auto older = pair::wire::readHello (nine);
        REQUIRE (older.has_value());
        CHECK (older->described);
        CHECK (older->salt == said.salt);
        CHECK (older->mediaDigest.empty());
    }

    //  What asks for the stream: following, both sending - and RD.5's backup.
    auto other = said;
    other.state = "bothSending";
    CHECK (other.wantsStream());

    for (const auto* state : { "leading", "alone", "waiting" })
    {
        other.state = state;
        CHECK_FALSE (other.wantsStream());
    }

    const auto legacy = pair::wire::readHello (osc::Packet::message (pair::wire::hello,
                                                                     { s ("b"), s ("backup"), h (4), s ("S1") }));
    REQUIRE (legacy.has_value());
    CHECK_FALSE (legacy->described);
    CHECK (legacy->wantsStream());
    CHECK_FALSE (pair::wire::readHello (osc::Packet::message (pair::wire::hello, { s ("b"), s ("backup"), h (4) })).has_value());

    //  Which half a message is for.
    using Bound = pair::wire::Bound;
    CHECK (pair::wire::boundFor (decoded.packet) == Bound::leader);
    CHECK (pair::wire::boundFor (osc::Packet::message (pair::wire::ping, { h (1) })) == Bound::leader);
    CHECK (pair::wire::boundFor (osc::Packet::message (pair::wire::relay, { s ("x") })) == Bound::leader);
    CHECK (pair::wire::boundFor (osc::Packet::message (pair::wire::pong, { h (1), h (2) })) == Bound::follower);
    CHECK (pair::wire::boundFor (osc::Packet::message (pair::wire::beat, { h (1), h (2) })) == Bound::follower);
    CHECK (pair::wire::boundFor (osc::Packet::bundle (osc::TimeTag {}, { osc::Packet::message (pair::wire::beat, { h (1), h (2) }) }))
           == Bound::follower);
    CHECK (pair::wire::boundFor (osc::Packet::message (pair::wire::order, { s ("hold"), s ("keep") })) == Bound::neither);
}

TEST_CASE ("peer link: a hello that follows asks from now; one that says it no longer follows is forgotten as a follower")
{
    Wire wire;
    pair::Stream stream { "S-lead" };
    stream.setSend (wire.send());
    stream.setLeading (true);

    const auto go = named ("go");

    for (int i = 0; i < 5; ++i)
        stream.keep (applied ("cli", "go", {}), &go);

    //  A machine that starts following now asks for what comes from now on, never the five before.
    pair::wire::Hello said;
    said.machine = "FOLLOWER";
    said.role = "main";
    said.lastCounter = pair::wire::fromNow;
    said.stream = "S-lead";
    said.state = "following";
    said.ownStream = "S-other";
    said.described = true;

    const auto hello = [&said]
    {
        const auto bytes = pair::wire::helloBytes (said);
        return osc::decode (bytes.data(), bytes.size()).packet;
    };

    stream.received ("10.0.0.1:5010", hello(), t0);
    stream.afterTick (1, 1.0);

    auto got = wire.take ("10.0.0.1:5010");
    REQUIRE (! got.empty());
    CHECK (got[0].address == pair::wire::welcome);
    CHECK (got[0].args[1] == h (6));
    CHECK (countersIn (got).empty());
    CHECK (stream.followerCount() == 1);

    stream.keep (applied ("cli", "go", {}), &go);
    stream.afterTick (2, 1.0);
    CHECK (countersIn (wire.take ("10.0.0.1:5010")) == std::vector<std::int64_t> { 6 });

    //  It took over: it says so, and is no follower any more. Not ignored - it said what it is.
    said.state = "leading";
    const auto ignoredBefore = stream.ignored();
    stream.received ("10.0.0.1:5010", hello(), t0);
    stream.afterTick (3, 1.0);
    CHECK (stream.followerCount() == 0);
    CHECK (stream.ignored() == ignoredBefore);

    //  A relay's ticket rides back on the record that answers it.
    LogRecord answered = applied ("peer:ws:10.0.0.7:5555", "go", {});
    answered.ticket = 77;
    said.state = "bothSending";
    stream.received ("10.0.0.1:5010", hello(), t0);
    stream.afterTick (4, 1.0);
    stream.keep (answered, &go);
    stream.afterTick (5, 1.0);

    got = wire.take ("10.0.0.1:5010");
    const auto record = std::find_if (got.begin(), got.end(), [] (const osc::Packet& p) { return p.address == pair::wire::record; });
    REQUIRE (record != got.end());
    REQUIRE (record->args.size() == 3u);
    CHECK (record->args[2] == h (77));

    pair::Receiver receiver;
    std::vector<std::uint64_t> tickets;
    receiver.setOnRecord ([&tickets] (std::uint64_t, const LogRecord& r) { tickets.push_back (r.ticket); });
    receiver.startAfresh ("S-lead");
    CHECK (receiver.resumePoint() == std::make_pair (pair::wire::fromNow, std::string ("S-lead")));

    for (const auto& packet : got)
        receiver.deliver ("10.0.0.1:5010", packet, t0);

    receiver.drain (t0, 1.0);

    /*  STARTED AFRESH: the welcome says where it starts, and what came before
        it is no gap; the beat says the cues running there. */
    CHECK (tickets == std::vector<std::uint64_t> { 77 });
    CHECK (receiver.reading (t0, 1.0).gaps == 0);
    CHECK (receiver.streamId() == "S-lead");
    CHECK (receiver.resumePoint() == std::make_pair (std::uint64_t { 7 }, std::string ("S-lead")));
}

//==============================================================================
TEST_CASE ("peer link: the ring - counters continuous, a resume inside it exact, one older answered gap")
{
    Wire wire;
    pair::Stream stream { "S1", 16 };
    stream.setSend (wire.send());
    stream.setLeading (true);

    const auto next = named ("standby.next");
    const auto ended = named ("run.ended");
    std::int64_t tick = 100;

    const auto keep = [&] (int count)
    {
        for (int i = 0; i < count; ++i)
        {
            stream.keep (applied ("udp:10.0.0.9:9000", "standby.next", {}), &next);

            //  The engine's own, between them, take no counter: the stream's has no gaps.
            stream.keep (applied (std::string (origin::engine), "run.ended", { s ("R1") }), &ended);
        }
    };

    const auto beat = [&] { stream.afterTick (++tick, 1.0); };

    keep (10);
    CHECK (stream.lastCounter() == 10);
    CHECK (stream.oldestCounter() == 1);

    //  A follower that holds nothing hears everything, from the first.
    stream.received (followerA, hello (0, ""), t0);
    beat();

    auto got = wire.take (followerA);
    REQUIRE (got.size() == 12);
    CHECK (got.front().address == pair::wire::welcome);
    CHECK (got.front().args == std::vector<osc::Value> { s ("S1"), h (1) });
    CHECK (countersIn (got) == run (1, 10));
    CHECK (got.back().address == pair::wire::beat);
    CHECK (got.back().args == std::vector<osc::Value> { h (tick), h (10), h (0) });

    //  The next tick, nothing new: a beat, and only a beat.
    beat();
    got = wire.take (followerA);
    REQUIRE (got.size() == 1);
    CHECK (got.front().args == std::vector<osc::Value> { h (tick), h (10), h (0) });

    //  One that held this stream up to 7 hears 8, 9 and 10, exactly.
    stream.received (followerB, hello (7, "S1"), t0 + 10ms);
    beat();
    got = wire.take (followerB);
    REQUIRE (! got.empty());
    CHECK (got.front().args == std::vector<osc::Value> { s ("S1"), h (8) });
    CHECK (countersIn (got) == run (8, 10));
    CHECK (wire.take (followerA).size() == 1);

    //  Four more, then sixteen: both hear every one in order, and the ring of sixteen holds 15 to 30.
    keep (4);
    beat();
    CHECK (countersIn (wire.take (followerA)) == run (11, 14));
    CHECK (countersIn (wire.take (followerB)) == run (11, 14));

    keep (16);
    CHECK (stream.lastCounter() == 30);
    CHECK (stream.oldestCounter() == 15);
    beat();
    CHECK (countersIn (wire.take (followerA)) == run (15, 30));
    CHECK (countersIn (wire.take (followerB)) == run (15, 30));
    CHECK (stream.reading (t0 + 20ms, 1.0).gaps == 0);

    //  One that held up to 5: what it asks for is gone. Told gap, then sent what the ring holds.
    stream.received (followerC, hello (5, "S1"), t0 + 20ms);
    beat();
    got = wire.take (followerC);
    REQUIRE (got.size() >= 3);
    CHECK (got[0].args == std::vector<osc::Value> { s ("S1"), h (15) });
    CHECK (got[1].address == pair::wire::gap);
    CHECK (got[1].args == std::vector<osc::Value> { h (15) });
    CHECK (countersIn (got) == run (15, 30));
    CHECK (stream.reading (t0 + 20ms, 1.0).gaps == 1);

    /*  ANOTHER STREAM'S COUNTER MEANS NOTHING HERE - a main that went away and
        came back is a new stream - and nothing is the same as nothing held:
        from the first, which the ring has let go of, so a gap too. */
    const std::string followerD = "ws:10.0.0.3:50004";
    stream.received (followerD, hello (100, "S0"), t0 + 20ms);
    beat();
    got = wire.take (followerD);
    REQUIRE (got.size() >= 2);
    CHECK (got[0].args == std::vector<osc::Value> { s ("S1"), h (15) });
    CHECK (got[1].args == std::vector<osc::Value> { h (15) });
    CHECK (stream.reading (t0 + 20ms, 1.0).gaps == 2);

    //  One that holds it all is welcomed past the end, and sent the beat.
    const std::string followerE = "ws:10.0.0.3:50005";
    stream.received (followerE, hello (30, "S1"), t0 + 20ms);
    beat();
    got = wire.take (followerE);
    REQUIRE (got.size() == 2);
    CHECK (got[0].args == std::vector<osc::Value> { s ("S1"), h (31) });
    CHECK (got[1].address == pair::wire::beat);

    /*  A FOLLOWER STILL OWED RECORDS THE RING HAS SINCE LET GO OF - twenty kept
        in one tick on a ring of sixteen - is told gap and jumps, rather than be
        sent a record that is no longer the one its counter names. */
    wire.take (followerA);
    keep (20);
    beat();
    got = wire.take (followerA);
    REQUIRE (! got.empty());
    CHECK (got[0].address == pair::wire::gap);
    CHECK (got[0].args == std::vector<osc::Value> { h (35) });
    CHECK (countersIn (got) == run (35, 50));

    //  A connection closed is a follower forgotten; one the server no longer has, the same.
    const auto before = stream.followerCount();
    stream.closed (followerA);
    beat();
    CHECK (wire.take (followerA).empty());
    CHECK (stream.followerCount() == before - 1);

    {
        const std::lock_guard<std::mutex> held { wire.mutex };
        wire.gone.insert (followerB);
    }

    beat();
    CHECK (stream.followerCount() == before - 2);
}

TEST_CASE ("peer link: a backlog goes a slice a tick, while the main goes on keeping")
{
    Wire wire;
    pair::Stream stream { "S1", 64, 4 };
    stream.setSend (wire.send());
    stream.setLeading (true);

    const auto next = named ("standby.next");

    for (int i = 0; i < 10; ++i)
        stream.keep (applied ("udp:10.0.0.9:9000", "standby.next", {}), &next);

    stream.received (followerA, hello (0, ""), t0);

    stream.afterTick (1, 1.0);
    CHECK (countersIn (wire.take (followerA)) == run (1, 4));

    stream.afterTick (2, 1.0);
    auto got = wire.take (followerA);
    CHECK (countersIn (got) == run (5, 8));

    //  A tick's messages to one follower are one frame, however many.
    CHECK (wire.frames[followerA] == 2);

    //  The beat says how far the main has got, not how far this follower has.
    CHECK (got.back().args == std::vector<osc::Value> { h (2), h (10), h (0) });

    stream.keep (applied ("udp:10.0.0.9:9000", "standby.next", {}), &next);
    stream.afterTick (3, 1.0);
    CHECK (countersIn (wire.take (followerA)) == run (9, 11));
}

TEST_CASE ("peer link: a ping is answered at once; what is not the link's grammar, or not for a main, is ignored and counted")
{
    Wire wire;
    pair::Stream stream { "S1" };
    stream.setSend (wire.send());
    stream.setLeading (true);
    stream.afterTick (77, 1.0);

    //  At once, on the server's thread, with this machine's tick: no tick in between.
    stream.received (followerA, ping (t0, 0.0f), t0);
    auto got = wire.take (followerA);
    REQUIRE (got.size() == 1);
    CHECK (got[0].address == pair::wire::pong);
    CHECK (got[0].args == std::vector<osc::Value> { h (nanos (t0)), h (77) });

    stream.received (followerA, osc::Packet::message ("/godot/pair/nonsense", {}), t0);
    stream.received (followerA, osc::Packet::message (pair::wire::hello, { s ("only a name") }), t0);
    stream.received (followerA, osc::Packet::message (pair::wire::ping, { s ("not a time") }), t0);
    stream.received (followerA, osc::Packet::message (pair::wire::hello, { s ("b"), s ("backup"), h (-1), s ("") }), t0);
    CHECK (stream.ignored() == 4);

    //  A hello from a machine that does not say it is a backup...
    stream.received (followerA, hello (0, "", "main"), t0);
    stream.afterTick (78, 1.0);
    CHECK (stream.ignored() == 5);
    CHECK (stream.followerCount() == 0);

    //  ...or to a machine that is not a main, which keeps nothing either.
    stream.setLeading (false);
    stream.received (followerA, hello (0, ""), t0);
    stream.afterTick (79, 1.0);
    CHECK (stream.ignored() == 6);
    CHECK (stream.followerCount() == 0);

    const auto go = named ("go");
    stream.keep (applied ("udp:10.0.0.9:9000", "go", {}), &go);
    CHECK (stream.lastCounter() == 0);
}

TEST_CASE ("peer link: a main is linked while its backup pings, and counts each record sent once this link")
{
    Wire wire;
    pair::Stream stream { "S1" };
    stream.setSend (wire.send());
    stream.setLeading (true);

    const auto next = named ("standby.next");

    for (int i = 0; i < 3; ++i)
        stream.keep (applied ("udp:10.0.0.9:9000", "standby.next", {}), &next);

    stream.received (followerA, hello (0, ""), t0);
    stream.afterTick (1, 1.0);

    auto reading = stream.reading (t0, 1.0);
    CHECK_FALSE (reading.linked);           // said hello, never pinged
    CHECK (reading.received == 3);
    CHECK (reading.roundTrip == doctest::Approx (0.0));

    stream.received (followerA, ping (t0 + 100ms, 0.0025f), t0 + 100ms);
    stream.afterTick (2, 1.0);

    reading = stream.reading (t0 + 150ms, 1.0);
    CHECK (reading.linked);
    CHECK (reading.peer == followerA);
    CHECK (reading.roundTrip == doctest::Approx (0.0025).epsilon (1e-6));

    //  A second path joins the link that is up: what it is sent again is not counted again.
    stream.received (followerB, hello (0, ""), t0 + 120ms);
    stream.received (followerB, ping (t0 + 130ms, 0.004f), t0 + 130ms);
    stream.afterTick (3, 1.0);

    reading = stream.reading (t0 + 140ms, 1.0);
    CHECK (reading.received == 3);
    CHECK (reading.peer == followerB);
    CHECK (reading.roundTrip == doctest::Approx (0.004).epsilon (1e-6));

    for (int i = 0; i < 2; ++i)
        stream.keep (applied ("udp:10.0.0.9:9000", "standby.next", {}), &next);

    stream.afterTick (4, 1.0);
    CHECK (countersIn (wire.take (followerA)).back() == 5);
    CHECK (countersIn (wire.take (followerB)).back() == 5);
    CHECK (stream.reading (t0 + 140ms, 1.0).received == 5);

    //  Silent past the timeout: not linked, no round trip, the peer said and the counts kept.
    reading = stream.reading (t0 + 1200ms, 1.0);
    CHECK_FALSE (reading.linked);
    CHECK (reading.roundTrip == doctest::Approx (0.0));
    CHECK (reading.peer == followerB);
    CHECK (reading.received == 5);

    //  Gone, and back after the silence: a new link, counted from nought.
    stream.closed (followerA);
    stream.closed (followerB);
    stream.received (followerC, hello (0, ""), t0 + 3s);
    stream.afterTick (5, 1.0);
    CHECK (countersIn (wire.take (followerC)) == run (1, 5));
    CHECK (stream.reading (t0 + 3s, 1.0).received == 5);
    CHECK (stream.reading (t0 + 3s, 1.0).gaps == 0);
}

TEST_CASE ("peer link: a backup counts a record once whichever path brought it, and a jump is a gap")
{
    pair::Receiver receiver;
    std::vector<std::uint64_t> counters;
    receiver.setOnRecord ([&counters] (std::uint64_t counter, const LogRecord&) { counters.push_back (counter); });

    const std::string pathA = "10.0.0.1:5010";
    const std::string pathB = "10.1.0.1:5010";
    const auto line = applied ("udp:10.0.0.9:9000", "standby.next", {}).toLine();

    const auto record = [&line] (std::int64_t counter)
    {
        return osc::Packet::message (pair::wire::record, { h (counter), s (line) });
    };

    const auto welcome = [] (std::string stream, std::int64_t next)
    {
        return osc::Packet::message (pair::wire::welcome, { s (std::move (stream)), h (next) });
    };

    const auto beat = [] (std::int64_t tick, std::int64_t last)
    {
        return osc::Packet::message (pair::wire::beat, { h (tick), h (last) });
    };

    //  Two paths to one main, each bringing what the other did.
    receiver.deliver (pathA, welcome ("S1", 1), t0);

    for (std::int64_t c = 1; c <= 3; ++c)
        receiver.deliver (pathA, record (c), t0);

    receiver.deliver (pathB, welcome ("S1", 1), t0 + 1ms);

    for (std::int64_t c = 1; c <= 4; ++c)
        receiver.deliver (pathB, record (c), t0 + 1ms);

    receiver.deliver (pathA, record (4), t0 + 2ms);
    receiver.drain (t0 + 2ms, 1.0);

    auto reading = receiver.reading (t0 + 2ms, 1.0);
    CHECK (counters == std::vector<std::uint64_t> { 1, 2, 3, 4 });
    CHECK (reading.received == 4);
    CHECK (reading.gaps == 0);
    CHECK (receiver.duplicates() == 4);
    CHECK (reading.peer == pathB);         // the fourth came by B first
    CHECK (receiver.lastCounter() == 4);
    CHECK (receiver.streamId() == "S1");
    CHECK_FALSE (reading.linked);           // records, and no heartbeat yet

    //  A heartbeat on either path: linked for the timeout, then not.
    receiver.deliver (pathA, beat (50, 4), t0 + 20ms);
    receiver.deliver (pathB, beat (50, 4), t0 + 21ms);
    receiver.drain (t0 + 21ms, 1.0);

    reading = receiver.reading (t0 + 500ms, 1.0);
    CHECK (reading.linked);
    CHECK (reading.peer == pathA);          // tick 50's beat came first by A
    CHECK (receiver.mainTick() == 50);
    CHECK (receiver.mainLastCounter() == 4);
    CHECK_FALSE (receiver.reading (t0 + 1100ms, 1.0).linked);

    //  A jump is a gap; so is one the main says; a second path's word for one filled is nothing.
    receiver.deliver (pathA, record (6), t0 + 30ms);
    receiver.deliver (pathA, osc::Packet::message (pair::wire::gap, { h (10) }), t0 + 31ms);
    receiver.deliver (pathA, record (10), t0 + 32ms);
    receiver.deliver (pathB, osc::Packet::message (pair::wire::gap, { h (10) }), t0 + 33ms);
    receiver.drain (t0 + 33ms, 1.0);

    reading = receiver.reading (t0 + 33ms, 1.0);
    CHECK (counters == std::vector<std::uint64_t> { 1, 2, 3, 4, 6, 10 });
    CHECK (reading.gaps == 2);
    CHECK (reading.received == 6);

    //  A line that does not read is a record lost - never handed on.
    receiver.deliver (pathA, osc::Packet::message (pair::wire::record, { h (11), s ("not a line") }), t0 + 34ms);
    receiver.deliver (pathA, osc::Packet::message ("/godot/pair/nonsense", {}), t0 + 34ms);
    receiver.drain (t0 + 34ms, 1.0);
    CHECK (receiver.ignored() == 2);
    CHECK (counters.back() == 10);

    /*  THE ROUND TRIP, on this machine's clock: the fastest of the last
        second's, nought once the link is lost. */
    receiver.deliver (pathA, beat (60, 10), t0 + 2s);
    receiver.deliver (pathA, osc::Packet::message (pair::wire::pong, { h (nanos (t0 + 2s)), h (60) }), t0 + 2s + 3ms);
    receiver.deliver (pathB, osc::Packet::message (pair::wire::pong, { h (nanos (t0 + 2s)), h (60) }), t0 + 2s + 2ms);
    receiver.drain (t0 + 2s + 10ms, 1.0);

    reading = receiver.reading (t0 + 2s + 10ms, 1.0);
    CHECK (reading.linked);
    CHECK (reading.roundTrip == doctest::Approx (0.002).epsilon (1e-6));
    CHECK (receiver.roundTripToSay() == doctest::Approx (0.002).epsilon (1e-6));
    CHECK (receiver.reading (t0 + 3500ms, 1.0).roundTrip == doctest::Approx (0.0));

    //  A new stream - the main went and came back - holds nothing of the old: counted from nought.
    receiver.deliver (pathA, welcome ("S2", 1), t0 + 4s);
    receiver.deliver (pathA, record (1), t0 + 4s);
    receiver.drain (t0 + 4s, 1.0);

    reading = receiver.reading (t0 + 4s, 1.0);
    CHECK (receiver.streamId() == "S2");
    CHECK (receiver.lastCounter() == 1);
    CHECK (reading.received == 1);
    CHECK (reading.gaps == 0);
}

TEST_CASE ("peer link: a main and a backup joined by a function - a cable out and back resumes without a gap or a twice")
{
    pair::Stream stream { "S1" };
    pair::Receiver receiver;

    const std::string path = "10.0.0.1:5010";
    auto connected = true;
    auto now = t0;
    std::int64_t tick = 0;

    stream.setSend ([&] (const std::string& to, const std::vector<std::uint8_t>& bytes)
    {
        if (! connected || to != followerA)
            return false;

        const auto decoded = osc::decode (bytes.data(), bytes.size());

        if (! decoded.ok)
            return false;

        receiver.deliver (path, decoded.packet, now);
        return true;
    });

    stream.setLeading (true);

    std::vector<std::uint64_t> counters;
    receiver.setOnRecord ([&counters] (std::uint64_t counter, const LogRecord&) { counters.push_back (counter); });

    const auto dial = [&]
    {
        stream.received (followerA, osc::Packet::message (pair::wire::hello,
                                                          { s ("backup"), s ("backup"),
                                                            h (static_cast<std::int64_t> (receiver.lastCounter())),
                                                            s (receiver.streamId()) }), now);
    };

    const auto tickOnce = [&]
    {
        stream.afterTick (++tick, 1.0);
        receiver.drain (now, 1.0);

        if (connected)
            stream.received (followerA, ping (now, static_cast<float> (receiver.roundTripToSay())), now);

        now += 20ms;
    };

    const auto next = named ("standby.next");
    const auto keep = [&] (int count)
    {
        for (int i = 0; i < count; ++i)
            stream.keep (applied ("udp:10.0.0.9:9000", "standby.next", {}), &next);
    };

    keep (5);
    dial();

    for (int i = 0; i < 3; ++i)
        tickOnce();

    CHECK (counters == std::vector<std::uint64_t> { 1, 2, 3, 4, 5 });
    CHECK (receiver.reading (now, 1.0).linked);
    CHECK (stream.reading (now, 1.0).linked);

    //  The cable out: three records the backup does not hear, and the follower forgotten.
    connected = false;
    keep (3);
    tickOnce();
    CHECK (stream.followerCount() == 0);

    //  And back: the hello says 5, of S1, and 6 to 8 follow - none twice, none missing.
    connected = true;
    dial();
    tickOnce();
    tickOnce();

    CHECK (counters == std::vector<std::uint64_t> { 1, 2, 3, 4, 5, 6, 7, 8 });
    CHECK (receiver.duplicates() == 0);
    CHECK (receiver.reading (now, 1.0).gaps == 0);
    CHECK (stream.reading (now, 1.0).gaps == 0);
}

//==============================================================================
TEST_CASE ("after record: once per record, after the log write, with the arguments as applied")
{
    Engine engine;
    engine.log().openInMemory ({});

    engine.commands().add ({ "test.make", "Makes something and names it.",
                             { { "name", 's', false }, { "id", 's', true } }, true,
                             [] (CommandContext&, const std::vector<osc::Value>& args)
                             {
                                 return Outcome::ok ({ args[0], osc::Value::string ("MADE2345") });
                             } });

    engine.commands().add ({ "test.refuse", "Refuses.", {}, true,
                             [] (CommandContext&, const std::vector<osc::Value>&)
                             {
                                 return Outcome::rejected ("not-today");
                             } });

    struct Seen
    {
        LogRecord record;
        const Command* command = nullptr;
        bool alreadyLogged = false;
    };

    std::vector<Seen> seen;

    engine.setAfterRecord ([&engine, &seen] (const LogRecord& record, const Command* command)
    {
        const auto& text = engine.log().contents();
        const auto line = record.toLine() + "\n";
        const auto logged = text.size() >= line.size()
                              && text.compare (text.size() - line.size(), line.size(), line) == 0;
        seen.push_back ({ record, command, logged });
    });

    REQUIRE (engine.submit ("udp:10.0.0.9:9000", "test.make", { s ("lamp") }));
    REQUIRE (engine.submit ("ws:192.168.1.20:53211", "test.refuse"));
    REQUIRE (engine.submit ("cli", "no.such", { s ("x") }));

    Drop drop;
    drop.origin = "udp:10.0.0.9:9000";
    drop.reason = "not-osc";
    drop.payload = { 1, 2, 3 };
    REQUIRE (engine.submit (std::move (drop)));

    CHECK (engine.processTick (7).total() == 4);
    REQUIRE (seen.size() == 4);

    for (std::size_t i = 0; i < seen.size(); ++i)
    {
        CHECK (seen[i].alreadyLogged);
        CHECK (seen[i].record.seq == i);
        CHECK (seen[i].record.tick == 7);
    }

    CHECK (seen[0].record.kind == LogRecord::Kind::applied);
    CHECK (seen[0].record.args == std::vector<osc::Value> { s ("lamp"), s ("MADE2345") });
    CHECK (seen[0].command == engine.commands().find ("test.make"));

    CHECK (seen[1].record.kind == LogRecord::Kind::rejected);
    CHECK (seen[1].record.reason == "not-today");
    CHECK (seen[1].command == engine.commands().find ("test.refuse"));

    CHECK (seen[2].record.kind == LogRecord::Kind::rejected);
    CHECK (seen[2].record.reason == std::string (reason::unknownCommand));
    CHECK (seen[2].record.args == std::vector<osc::Value> { s ("x") });
    CHECK (seen[2].command == nullptr);

    CHECK (seen[3].record.kind == LogRecord::Kind::dropped);
    CHECK (seen[3].command == nullptr);

    //  None set, none called.
    engine.setAfterRecord (nullptr);
    REQUIRE (engine.submit ("cli", "noop"));
    engine.processTick (8);
    CHECK (seen.size() == 4);
}

//==============================================================================
namespace
{
    /*  A namespace with nothing in it, for a server whose only use here is its
        side door - and which counts what reaches it as a write, which a
        pairing frame never must. */
    struct QuietNamespace final : public oscquery::Namespace
    {
        std::shared_ptr<const tree::TreeSnapshot> tree;
        std::atomic<int> writes { 0 };

        QuietNamespace()
        {
            auto nodes = std::make_shared<std::vector<tree::Node>>();
            tree::Node root;
            root.address = "/";
            nodes->push_back (root);
            tree = std::make_shared<const tree::TreeSnapshot> (1, nodes, std::make_shared<const std::vector<tree::Node>>(),
                                                               std::vector<tree::Node> {});
        }

        std::shared_ptr<const tree::TreeSnapshot> snapshot() const override { return tree; }
        void write (const std::string&, const osc::Packet&) override { ++writes; }
        void forget (const std::string&) override {}
        bool shouldPush (const std::string&, const std::string&, const std::string&) const override { return true; }
        int oscPort() const override { return 0; }
    };

    struct StreamDoor final : public oscquery::PeerDoor
    {
        explicit StreamDoor (pair::Stream& streamToTell) : stream (streamToTell) {}

        void peerMessage (const std::string& origin, const osc::Packet& message) override
        {
            stream.received (origin, message, Clock::now());
        }

        void peerClosed (const std::string& origin) override { stream.closed (origin); }

        pair::Stream& stream;
    };
}

TEST_CASE ("peer link: a backup dials its main over a real WebSocket, is linked, and is linked again when the main comes back")
{
    QuietNamespace nameSpace;
    pair::Stream stream { "S-socket" };
    StreamDoor door { stream };

    oscquery::OscQueryServer server;
    server.setPeerDoor (std::string (pair::wire::prefix), &door);
    stream.setSend ([&server] (const std::string& origin, const std::vector<std::uint8_t>& bytes)
                    { return server.sendTo (origin, bytes.data(), bytes.size()); });
    stream.setLeading (true);
    REQUIRE (server.start (0, nameSpace));
    const auto port = server.boundPort();
    const auto address = "127.0.0.1:" + std::to_string (port);

    pair::Receiver receiver;
    std::vector<LogRecord> records;
    receiver.setOnRecord ([&records] (std::uint64_t, const LogRecord& record) { records.push_back (record); });

    pair::Dialler dialler { receiver, "backup-under-test" };
    REQUIRE (dialler.start());
    dialler.want ({ address });

    /*  THE TICK, played by this thread: the main's heartbeat and the backup's
        drain, every ten milliseconds. */
    std::int64_t tick = 0;

    const auto tickUntil = [&] (const std::function<bool()>& done, std::chrono::milliseconds limit)
    {
        const auto until = Clock::now() + limit;

        while (Clock::now() < until)
        {
            stream.afterTick (++tick, 1.0);
            receiver.drain (Clock::now(), 1.0);

            if (done())
                return true;

            std::this_thread::sleep_for (10ms);
        }

        return done();
    };

    const auto bothLinked = [&]
    {
        return receiver.reading (Clock::now(), 1.0).linked && stream.reading (Clock::now(), 1.0).linked;
    };

    REQUIRE (tickUntil (bothLinked, 10000ms));
    CHECK (dialler.openPaths() == 1);

    //  A record kept on the main arrives on the backup as the main's log wrote it - and is never a write.
    const auto go = named ("go");
    const auto pressed = applied ("udp:10.0.0.9:9000", "go", { s ("R8M2K4Q1") });
    stream.keep (pressed, &go);
    REQUIRE (tickUntil ([&records] { return records.size() == 1; }, 5000ms));
    checkSame (records[0], pressed);
    CHECK (nameSpace.writes.load() == 0);

    //  The round trip at both ends: there, and well under a tenth of a second on one machine.
    tickUntil ([&] { return stream.reading (Clock::now(), 1.0).roundTrip > 0.0; }, 3000ms);
    const auto backupSide = receiver.reading (Clock::now(), 1.0);
    const auto mainSide = stream.reading (Clock::now(), 1.0);
    CHECK (backupSide.roundTrip > 0.0);
    CHECK (backupSide.roundTrip < 0.1);
    CHECK (mainSide.roundTrip > 0.0);
    CHECK (mainSide.roundTrip < 0.1);
    CHECK (backupSide.peer == address);

    //  The backup's connection as the server names it: IPv4, or IPv4 mapped into IPv6 on a dual-stack socket.
    INFO ("the main's peer: " << mainSide.peer);
    CHECK (mainSide.peer.rfind ("ws:", 0) == 0);
    CHECK (mainSide.peer.find ("127.0.0.1:") != std::string::npos);
    CHECK (backupSide.received == 1);
    CHECK (mainSide.received == 1);
    CHECK (backupSide.gaps == 0);

    /*  WHAT A TICK COSTS TO SEND for real, into the server's queue: an idle
        one, its beat alone, and a busy one, sixty-four records and the beat. */
    std::vector<double> idle;

    for (int i = 0; i < 20; ++i)
    {
        const auto before = Clock::now();
        stream.afterTick (++tick, 1.0);
        idle.push_back (std::chrono::duration<double, std::micro> (Clock::now() - before).count());
    }

    std::sort (idle.begin(), idle.end());

    for (int i = 0; i < 64; ++i)
        stream.keep (pressed, &go);

    const auto before = Clock::now();
    stream.afterTick (++tick, 1.0);
    const auto sendMicros = std::chrono::duration<double, std::micro> (Clock::now() - before).count();
    MESSAGE ("handed to the server for one follower: a beat alone, median " << idle[idle.size() / 2]
             << " us; 64 records and a beat, " << sendMicros << " us");
    REQUIRE (tickUntil ([&records] { return records.size() == 65; }, 5000ms));

    //  The main goes: its heartbeat stops, and the backup is unlinked once the timeout has passed.
    server.stop();
    const auto stoppedAt = Clock::now();
    REQUIRE (tickUntil ([&] { return ! receiver.reading (Clock::now(), 1.0).linked; }, 5000ms));
    CHECK (Clock::now() - stoppedAt < 3s);

    /*  AND COMES BACK ON THE SAME PORT AT ONCE - the stopped server's
        connections still in TIME_WAIT on it - and is dialled again: the same
        stream, so the backup resumes after the last record it holds, and the
        next one arrives as the only record of the new link. */
    REQUIRE (server.start (port, nameSpace));
    REQUIRE (tickUntil (bothLinked, 10000ms));

    stream.keep (pressed, &go);
    REQUIRE (tickUntil ([&records] { return records.size() == 66; }, 5000ms));

    const auto again = receiver.reading (Clock::now(), 1.0);
    CHECK (again.received == 1);
    CHECK (again.gaps == 0);
    CHECK (receiver.duplicates() == 0);

    dialler.stop();
    server.stop();
}

//==============================================================================
/*  RD.7: THE LINK IS SYMMETRIC OVER ONE CONNECTION. The backup still dials and
    the main still listens, but a backup that took over leads the main that
    came back: its stream goes down the path it dialled, and the main's
    receiver reads it from behind the server. Both ends hear the other's
    engine - pings carry the dialler's tick, pongs the server's - whoever
    leads. */
namespace
{
    /*  WHAT THE SERVER'S END OF THE LINK HEARS, routed as serve routes it:
        presence hears all of it, the follower's half the leader's stream. */
    struct FollowerDoor final : public oscquery::PeerDoor
    {
        FollowerDoor (pair::Stream& leaderHalf, pair::Receiver& followerHalf, pair::Presence& presenceToTell)
            : stream (leaderHalf), receiver (followerHalf), presence (presenceToTell) {}

        void peerMessage (const std::string& origin, const osc::Packet& message) override
        {
            {
                const std::lock_guard<std::mutex> held { lock };

                /*  The connection the backup said hello on last: a path dialled
                    again is a new one, and serve says its hello down every new
                    one (presence's `opened`) - as the test does below. */
                if (message.address == pair::wire::hello)
                    first = origin;
            }

            presence.heard (origin, message, Clock::now());

            switch (pair::wire::boundFor (message))
            {
                case pair::wire::Bound::leader:   stream.received (origin, message, Clock::now()); break;
                case pair::wire::Bound::follower: receiver.deliver (origin, message, Clock::now()); break;
                case pair::wire::Bound::show:
                case pair::wire::Bound::neither:  break;
            }
        }

        void peerClosed (const std::string& origin) override
        {
            stream.closed (origin);
            presence.closed (origin);
        }

        std::string connection() const
        {
            const std::lock_guard<std::mutex> held { lock };
            return first;
        }

        pair::Stream& stream;
        pair::Receiver& receiver;
        pair::Presence& presence;
        mutable std::mutex lock;
        std::string first;
    };
}

TEST_CASE ("peer link: a backup that leads streams down the path it dialled, to a main that follows behind its server")
{
    //  THE MAIN, which came back after a crash and follows: its server, its receiver, its presence.
    QuietNamespace nameSpace;
    pair::Stream mainStream { "S-main" };
    pair::Receiver mainReceiver;
    pair::Presence mainPresence;
    FollowerDoor door { mainStream, mainReceiver, mainPresence };

    oscquery::OscQueryServer server;
    server.setPeerDoor (std::string (pair::wire::prefix), &door);
    mainStream.setSend ([&server] (const std::string& origin, const std::vector<std::uint8_t>& bytes)
                        { return server.sendTo (origin, bytes.data(), bytes.size()); });
    REQUIRE (server.start (0, nameSpace));
    const auto address = "127.0.0.1:" + std::to_string (server.boundPort());

    std::vector<LogRecord> arrived;
    mainReceiver.setOnRecord ([&arrived] (std::uint64_t, const LogRecord& record) { arrived.push_back (record); });

    //  THE BACKUP, which took over and leads: its stream goes down its dialler's path.
    pair::Stream backupStream { "S-backup" };
    pair::Receiver backupReceiver;
    pair::Presence backupPresence;
    backupStream.setLeading (true);

    pair::Dialler dialler { backupReceiver, "backup-that-leads" };
    dialler.setRoute ([&] (const std::string& path, const osc::Packet& message, Clock::time_point arrival)
                      {
                          backupPresence.heard (path, message, arrival);

                          switch (pair::wire::boundFor (message))
                          {
                              case pair::wire::Bound::leader:   backupStream.received (path, message, arrival); break;
                              case pair::wire::Bound::follower: backupReceiver.deliver (path, message, arrival); break;
                              case pair::wire::Bound::show:
                              case pair::wire::Bound::neither:  break;
                          }
                      });
    backupStream.setSend ([&dialler] (const std::string& path, const std::vector<std::uint8_t>& bytes)
                          { return dialler.sendTo (path, bytes); });

    pair::wire::Hello leads;
    leads.machine = "backup-that-leads";
    leads.role = "backup";
    leads.state = "leading";
    leads.ownStream = "S-backup";
    leads.described = true;
    dialler.setHello ([&leads] { return pair::wire::helloBytes (leads); });

    REQUIRE (dialler.start());
    dialler.want ({ address });

    std::int64_t tick = 0;

    const auto tickUntil = [&] (const std::function<bool()>& done, std::chrono::milliseconds limit)
    {
        const auto until = Clock::now() + limit;

        while (Clock::now() < until)
        {
            ++tick;
            dialler.setTick (tick);
            mainStream.afterTick (tick, 1.0);
            backupStream.afterTick (tick, 1.0);
            mainReceiver.drain (Clock::now(), 1.0);
            backupReceiver.drain (Clock::now(), 1.0);
            mainPresence.drain();
            backupPresence.drain();

            if (done())
                return true;

            std::this_thread::sleep_for (10ms);
        }

        return done();
    };

    //  The main hears the backup say what it is, as the dialler opens.
    REQUIRE (tickUntil ([&] { return mainPresence.peer().described && ! door.connection().empty(); }, 10000ms));
    CHECK (mainPresence.peer().state == "leading");
    CHECK (mainPresence.peer().ownStream == "S-backup");

    //  And follows it: its hello down the connection the backup dialled, asking from now.
    pair::wire::Hello follows;
    follows.machine = "main-come-back";
    follows.role = "main";
    follows.lastCounter = pair::wire::fromNow;
    follows.stream = "S-backup";
    follows.state = "following";
    follows.ownStream = "S-main";
    follows.fresh = true;
    follows.described = true;
    mainReceiver.startAfresh ("S-backup");

    const auto said = pair::wire::helloBytes (follows);
    std::string saidOn;

    const auto followIt = [&]
    {
        if (const auto latest = door.connection(); latest != saidOn)
        {
            saidOn = latest;
            server.sendTo (latest, said.data(), said.size());
        }

        return backupStream.followerCount() == 1 && mainReceiver.isLinked (Clock::now(), 1.0);
    };

    const auto followed = tickUntil (followIt, 10000ms);
    INFO ("followers " << backupStream.followerCount() << ", main linked " << mainReceiver.isLinked (Clock::now(), 1.0)
          << ", open paths " << dialler.openPaths() << ", said on " << saidOn);
    REQUIRE (followed);

    //  BOTH ENDS HEAR THE OTHER, whoever leads: the main by the backup's pings, the backup by the main's pongs.
    CHECK (mainPresence.linked (Clock::now(), 1.0));
    CHECK (backupPresence.linked (Clock::now(), 1.0));
    CHECK (backupPresence.peer().state == "following");
    CHECK (backupPresence.latest() == address);
    CHECK (mainPresence.latest().rfind ("ws:", 0) == 0);

    //  What the backup applies reaches the main, as the backup's log wrote it, its ticket beside it.
    const auto go = named ("go");
    auto pressed = applied ("peer:ws:10.0.0.7:5555", "go", { s ("R8M2K4Q1") });
    pressed.ticket = 41;
    backupStream.keep (pressed, &go);
    backupStream.keep (applied ("ws:10.0.0.8:6000", "standby.next", {}), &go);

    REQUIRE (tickUntil ([&arrived] { return arrived.size() == 2; }, 5000ms));
    checkSame (arrived[0], pressed);
    CHECK (arrived[0].ticket == 41);
    CHECK (arrived[1].command == "standby.next");
    CHECK (mainReceiver.reading (Clock::now(), 1.0).gaps == 0);
    CHECK (mainReceiver.streamId() == "S-backup");
    CHECK (nameSpace.writes.load() == 0);

    //  AND A COMMAND THE MAIN'S CLIENT GAVE IT, passed on up the same connection to the leader.
    std::vector<LogRecord> relayed;
    backupStream.setRelayed ([&relayed] (const LogRecord& record) { relayed.push_back (record); });

    LogRecord line;
    line.kind = LogRecord::Kind::applied;
    line.origin = "ws:10.0.0.9:7000";
    line.command = "go";
    const auto relay = pair::wire::encode (pair::wire::relay, { s (line.toLine()), h (9) });
    REQUIRE (server.sendTo (door.connection(), relay.data(), relay.size()));

    REQUIRE (tickUntil ([&relayed] { return relayed.size() == 1; }, 5000ms));
    CHECK (relayed[0].origin == "ws:10.0.0.9:7000");
    CHECK (relayed[0].ticket == 9);

    //  An order down every path: the presence at the other end hears it.
    dialler.sendAll (pair::orderBytes (pair::Order::hold, "keep"));
    std::vector<pair::Ordered> orders;
    REQUIRE (tickUntil ([&] { for (auto& o : mainPresence.takeOrders()) orders.push_back (o); return ! orders.empty(); }, 5000ms));
    CHECK (orders[0].order == pair::Order::hold);
    CHECK (orders[0].reason == "keep");

    dialler.stop();
    server.stop();
}

//==============================================================================
TEST_CASE ("peer link: sixty-four records in a tick cost the main well under a millisecond")
{
    /*  GO'S PATH GAINS ONE THING: each record that travels is made into its
        message and put in the ring, on the tick thread, as it is written. What
        that costs a tick of sixty-four - GOs with their runs, faders, renames,
        fires - is measured here, and what handing them and a beat to one
        follower costs beside it; the brief's bound is a millisecond in Debug.
        The check is loose, for a runner busy with other suites; the figure is
        the measurement. */
    pair::Stream stream { "S1" };
    std::size_t bytesOut = 0;
    stream.setSend ([&bytesOut] (const std::string&, const std::vector<std::uint8_t>& bytes)
                    {
                        bytesOut += bytes.size();
                        return true;
                    });
    stream.setLeading (true);
    stream.received (followerA, hello (0, ""), t0);
    stream.afterTick (1, 1.0);

    const auto go = named ("go");
    const auto set = named ("node.set");
    const auto fire = named ("cue.fire");

    std::vector<std::pair<LogRecord, const Command*>> busyTick;

    for (int i = 0; i < 64; ++i)
    {
        switch (i % 4)
        {
            case 0:  busyTick.emplace_back (applied ("udp:10.0.0.9:9000", "go", { s ("R8M2K4Q1"), s ("R8M2K4Q2") }), &go); break;
            case 1:  busyTick.emplace_back (applied ("surface:d700", "node.set",
                                                     { s ("/godot/cue/B3N8R5TW/level"), osc::Value::float32 (-6.25f) }), &set); break;
            case 2:  busyTick.emplace_back (applied ("ws:192.168.1.20:53211", "node.set",
                                                     { s ("/godot/cue/B3N8R5TW/name"), s ("Walk-in, house to half") }), &set); break;
            default: busyTick.emplace_back (applied ("midi:pad", "cue.fire", { s ("E4GP6QSC"), s ("R8M2K4Q3") }), &fire); break;
        }
    }

    std::vector<double> keeps, sends;

    for (int round = 0; round < 30; ++round)
    {
        const auto a = std::chrono::steady_clock::now();

        for (const auto& [record, command] : busyTick)
            stream.keep (record, command);

        const auto b = std::chrono::steady_clock::now();
        stream.afterTick (2 + round, 1.0);
        const auto c = std::chrono::steady_clock::now();

        keeps.push_back (std::chrono::duration<double, std::micro> (b - a).count());
        sends.push_back (std::chrono::duration<double, std::micro> (c - b).count());
    }

    std::sort (keeps.begin(), keeps.end());
    std::sort (sends.begin(), sends.end());

    MESSAGE ("64 records kept: median " << keeps[keeps.size() / 2] << " us, best " << keeps.front()
             << " us; handed to one follower with the beat: median " << sends[sends.size() / 2]
             << " us, best " << sends.front() << " us");

    CHECK (stream.lastCounter() == 64 * 30);
    CHECK (bytesOut > 0);
    CHECK (keeps[keeps.size() / 2] < 5000.0);
}

