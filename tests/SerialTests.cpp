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
    Serial ports (namespace draft §51, ACR; PC.10): the table that opens the
    show's ports on threads of their own, against a fake device - lines in,
    lines out, a port that will not open or goes away tried again, the show's
    edits reconciled - and the lines as a patch hears them. No hardware.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/serial/Angles.h>
#include <wfg/engine/serial/SerialTable.h>

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace wfg::serial;
using namespace std::chrono_literals;

namespace
{
    /*  A device at the end of a fake cable: what it will say, what it was
        sent, and whether it is plugged in. */
    struct FakeDevice
    {
        std::mutex lock;
        std::deque<std::string> toSay;
        std::string heard;
        bool plugged = true;
        int opens = 0;
        int refusals = 0;      // opens to refuse before one succeeds

        void say (std::string bytes)
        {
            const std::lock_guard<std::mutex> held (lock);
            toSay.push_back (std::move (bytes));
        }

        std::string sent()
        {
            const std::lock_guard<std::mutex> held (lock);
            return heard;
        }
    };

    class FakeLink final : public Link
    {
    public:
        explicit FakeLink (std::shared_ptr<FakeDevice> d) : device (std::move (d)) {}

        std::optional<std::string> read (std::chrono::milliseconds wait) override
        {
            {
                const std::lock_guard<std::mutex> held (device->lock);
                if (! device->plugged)
                    return std::nullopt;
                if (! device->toSay.empty())
                {
                    auto bytes = std::move (device->toSay.front());
                    device->toSay.pop_front();
                    return bytes;
                }
            }
            std::this_thread::sleep_for (std::min (wait, std::chrono::milliseconds (2)));
            return std::string {};
        }

        bool write (const std::string& bytes) override
        {
            const std::lock_guard<std::mutex> held (device->lock);
            if (! device->plugged)
                return false;
            device->heard += bytes;
            return true;
        }

        std::string problem() const override { return "the port went away"; }

    private:
        std::shared_ptr<FakeDevice> device;
    };

    struct Bench
    {
        std::map<std::string, std::shared_ptr<FakeDevice>> devices;

        Opener opener()
        {
            return [this] (const std::string& path, int, std::string& problem) -> std::unique_ptr<Link>
            {
                const auto found = devices.find (path);
                if (found == devices.end())
                {
                    problem = path + ": no such port on this machine";
                    return nullptr;
                }
                auto& device = *found->second;
                const std::lock_guard<std::mutex> held (device.lock);
                if (! device.plugged)
                {
                    problem = path + ": no such port on this machine";
                    return nullptr;
                }
                if (device.refusals > 0)
                {
                    --device.refusals;
                    problem = path + ": the port is in use by another program";
                    return nullptr;
                }
                ++device.opens;
                return std::make_unique<FakeLink> (found->second);
            };
        }

        std::shared_ptr<FakeDevice> plug (const std::string& path)
        {
            auto device = std::make_shared<FakeDevice>();
            devices[path] = device;
            return device;
        }
    };

    bool within (std::chrono::milliseconds limit, const std::function<bool()>& done)
    {
        const auto until = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < until)
        {
            if (done())
                return true;
            std::this_thread::sleep_for (2ms);
        }
        return done();
    }

    Wanted port (const std::string& id, const std::string& path)
    {
        Wanted wanted;
        wanted.id = id;
        wanted.path = path;
        return wanted;
    }

    std::vector<std::string> collect (SerialTable& table, const std::string& id, std::size_t count)
    {
        std::vector<std::string> lines;
        within (3s, [&]
        {
            for (auto& [from, taken] : table.takeLines (64))
                if (from == id)
                    lines.insert (lines.end(), taken.begin(), taken.end());
            return lines.size() >= count;
        });
        return lines;
    }
}

TEST_CASE ("serial: a port's lines arrive whole and in order, however the bytes were cut")
{
    Bench bench;
    auto arduino = bench.plug ("COM3");
    SerialTable table (bench.opener());
    table.reconcile ({ port ("SR000001", "COM3") });
    REQUIRE (within (2s, [&] { return table.stateOf ("SR000001").state == "open"; }));

    arduino->say ("51");
    arduino->say ("2\r\n13 7");
    arduino->say (",8\n\n");
    arduino->say ("last");
    const auto lines = collect (table, "SR000001", 3);
    CHECK (lines == std::vector<std::string> { "512", "13 7,8", "" });   // "last" waits for its new line

    arduino->say ("\n");
    CHECK (collect (table, "SR000001", 1) == std::vector<std::string> { "last" });
}

TEST_CASE ("serial: a line out reaches the port with its new line, and not when tx is off")
{
    Bench bench;
    auto arduino = bench.plug ("COM3");
    SerialTable table (bench.opener());
    table.reconcile ({ port ("SR000001", "COM3") });
    REQUIRE (within (2s, [&] { return table.stateOf ("SR000001").state == "open"; }));

    CHECK (table.send ("SR000001", "led 1"));
    CHECK (within (2s, [&] { return arduino->sent() == "led 1\n"; }));
    CHECK_FALSE (table.send ("NOSUCHID", "led 1"));

    auto quiet = port ("SR000001", "COM3");
    quiet.tx = false;
    table.reconcile ({ quiet });
    CHECK_FALSE (table.send ("SR000001", "led 0"));
    CHECK (arduino->opens == 1);                      // tx changed in place, not opened again
}

TEST_CASE ("serial: rx off drops what arrives, the port still open")
{
    Bench bench;
    auto arduino = bench.plug ("COM3");
    SerialTable table (bench.opener());
    auto deaf = port ("SR000001", "COM3");
    deaf.rx = false;
    table.reconcile ({ deaf });
    REQUIRE (within (2s, [&] { return table.stateOf ("SR000001").state == "open"; }));

    arduino->say ("512\n");
    std::this_thread::sleep_for (60ms);
    CHECK (table.takeLines (64).empty());
}

TEST_CASE ("serial: a port that will not open is tried again after a pause, and says why meanwhile")
{
    Bench bench;
    auto arduino = bench.plug ("COM3");
    arduino->refusals = 1;
    SerialTable table (bench.opener());
    table.reconcile ({ port ("SR000001", "COM3") });

    REQUIRE (within (2s, [&] { return table.stateOf ("SR000001").state == "retrying"; }));
    CHECK (table.stateOf ("SR000001").problem == "COM3: the port is in use by another program");

    //  Half a second later, it opens.
    REQUIRE (within (3s, [&] { return table.stateOf ("SR000001").state == "open"; }));
    CHECK (table.stateOf ("SR000001").problem.empty());
    CHECK (arduino->opens == 1);
}

TEST_CASE ("serial: a port that goes away is opened again when it comes back")
{
    Bench bench;
    auto arduino = bench.plug ("COM3");
    SerialTable table (bench.opener());
    table.reconcile ({ port ("SR000001", "COM3") });
    REQUIRE (within (2s, [&] { return table.stateOf ("SR000001").state == "open"; }));

    {
        const std::lock_guard<std::mutex> held (arduino->lock);
        arduino->plugged = false;
    }
    REQUIRE (within (2s, [&] { return table.stateOf ("SR000001").state == "retrying"; }));
    CHECK (table.stateOf ("SR000001").problem == "the port went away");
    CHECK_FALSE (table.send ("SR000001", "led 1"));    // nothing is open to take it

    {
        const std::lock_guard<std::mutex> held (arduino->lock);
        arduino->plugged = true;
    }
    REQUIRE (within (3s, [&] { return table.stateOf ("SR000001").state == "open"; }));
    CHECK (arduino->opens == 2);
    arduino->say ("back\n");
    CHECK (collect (table, "SR000001", 1) == std::vector<std::string> { "back" });
}

TEST_CASE ("serial: the show's edits - a new path opens the new port, no path or no port closes it")
{
    Bench bench;
    auto first = bench.plug ("COM3");
    auto second = bench.plug ("COM4");
    SerialTable table (bench.opener());
    table.reconcile ({ port ("SR000001", "COM3") });
    REQUIRE (within (2s, [&] { return table.stateOf ("SR000001").state == "open"; }));

    table.reconcile ({ port ("SR000001", "COM4") });
    REQUIRE (within (2s, [&] { return second->opens == 1 && table.stateOf ("SR000001").state == "open"; }));
    second->say ("from four\n");
    CHECK (collect (table, "SR000001", 1) == std::vector<std::string> { "from four" });

    table.reconcile ({ port ("SR000001", "") });
    CHECK (table.stateOf ("SR000001").state == "closed");
    CHECK_FALSE (table.send ("SR000001", "x"));

    table.reconcile ({});
    CHECK (table.stateOf ("SR000001").state == "closed");
    CHECK (first->opens == 1);
}

TEST_CASE ("serial: a line as a patch hears it - words on spaces and commas, numbers as numbers")
{
    const auto words = wordsOfLine ("512 13,7.5\thello;-2e3");
    REQUIRE (words.size() == 5u);
    CHECK (words[0].isNumber);
    CHECK (words[0].number == doctest::Approx (512.0));
    CHECK (words[1].number == doctest::Approx (13.0));
    CHECK (words[2].number == doctest::Approx (7.5));
    CHECK_FALSE (words[3].isNumber);
    CHECK (words[3].text == "hello");
    CHECK (words[4].number == doctest::Approx (-2000.0));
    CHECK (wordsOfLine ("").empty());
    CHECK (wordsOfLine (" , ").empty());
}

TEST_CASE ("serial: the lines heard, by tick - what a patch reads after its last tick, and the last line")
{
    HeardLines heard;
    heard.note ("SR000001", "1", 10);
    heard.note ("SR000002", "a", 10);
    heard.note ("SR000001", "2", 11);

    CHECK (heard.after (10) == std::vector<std::pair<std::string, std::string>> { { "SR000001", "2" } });
    CHECK (heard.after (9).size() == 3u);
    CHECK (heard.lastLine ("SR000001") == "2");
    CHECK (heard.lastLine ("SR000002") == "a");
    CHECK (heard.lastLine ("NOSUCHID").empty());

    heard.forgetBefore (11);
    CHECK (heard.after (0).size() == 1u);
    CHECK (heard.lastLine ("SR000002") == "a");          // the last line outlives the lines
}

//==============================================================================
/*  PC.11: OSC OVER SLIP - the framing a device on a serial port speaks, and a
    port whose framing is slip reading and writing packets. */

TEST_CASE ("serial: SLIP frames a packet at both ends and escapes what would end it")
{
    const std::vector<std::uint8_t> packet { 0x2F, 0xC0, 0x41, 0xDB, 0x00 };
    const auto framed = slip::encode (packet);
    const std::string expected { '\xC0', '\x2F', '\xDB', '\xDC', '\x41', '\xDB', '\xDD', '\x00', '\xC0' };
    CHECK (framed == expected);

    slip::Decoder decoder;
    const auto back = decoder.feed (framed);
    REQUIRE (back.size() == 1u);
    CHECK (back[0] == packet);
}

TEST_CASE ("serial: SLIP read however the bytes come - split, run together, empty frames between")
{
    const std::vector<std::uint8_t> first { 1, 2, 3 };
    const std::vector<std::uint8_t> second { 0xC0, 9 };
    const auto both = slip::encode (first) + slip::encode (second);

    slip::Decoder decoder;
    std::vector<std::vector<std::uint8_t>> got;
    for (std::size_t at = 0; at < both.size(); at += 2)
        for (auto& packet : decoder.feed (both.substr (at, 2)))
            got.push_back (std::move (packet));
    REQUIRE (got.size() == 2u);
    CHECK (got[0] == first);
    CHECK (got[1] == second);
    CHECK (decoder.dropped() == 0u);

    //  An ESC followed by what may not follow it spoils its frame, and only that one.
    const std::string spoiled { '\xC0', '\x01', '\xDB', '\x05', '\x02', '\xC0' };
    CHECK (decoder.feed (spoiled + slip::encode (first)).size() == 1u);
    CHECK (decoder.dropped() == 1u);

    //  A frame past 64 kB is given up to its END.
    std::string huge (1, '\xC0');
    huge.append (slip::largestFrame + 10, 'x');
    huge.push_back ('\xC0');
    CHECK (decoder.feed (huge + slip::encode (second)).size() == 1u);
    CHECK (decoder.dropped() == 2u);
}

TEST_CASE ("serial: a port reading OSC hands packets, sends them framed, and takes no lines")
{
    Bench bench;
    auto arduino = bench.plug ("COM3");
    SerialTable table (bench.opener());
    auto osc = port ("SR000001", "COM3");
    osc.framing = "slip";
    table.reconcile ({ osc });
    REQUIRE (within (2s, [&] { return table.stateOf ("SR000001").state == "open"; }));

    const std::vector<std::uint8_t> packet { '/', 'a', 0, 0, ',', 0, 0, 0 };
    const auto framed = slip::encode (packet);
    arduino->say (framed.substr (0, 3));
    arduino->say (framed.substr (3));

    std::vector<std::vector<std::uint8_t>> got;
    within (2s, [&]
    {
        for (auto& [id, packets] : table.takePackets (64))
            for (auto& p : packets)
                got.push_back (std::move (p));
        return ! got.empty();
    });
    REQUIRE (got.size() == 1u);
    CHECK (got[0] == packet);
    CHECK (table.takeLines (64).empty());

    CHECK (table.sendPacket ("SR000001", packet));
    CHECK (within (2s, [&] { return arduino->sent() == framed; }));
    CHECK_FALSE (table.send ("SR000001", "a line"));

    //  And a port reading lines takes no packet.
    auto lines = port ("SR000002", "COM4");
    bench.plug ("COM4");
    table.reconcile ({ osc, lines });
    REQUIRE (within (2s, [&] { return table.stateOf ("SR000002").state == "open"; }));
    CHECK_FALSE (table.sendPacket ("SR000002", packet));
}

//==============================================================================
/*  DP.6: A STREAM CUT BY LENGTH - OSC 1.0 over TCP, what an Eos takes on 3032 -
    and a raw one, read and written by the table a serial port is read by, since
    a connection is a link on it (namespace draft §57, AFJ). The fake device is
    the same: a link is bytes in and bytes out, whatever is under it. */

TEST_CASE ("serial: a link framed by length hands packets however the bytes come, sends them with their size first, and starts afresh after a size that cannot be")
{
    Bench bench;
    auto eos = bench.plug ("127.0.0.1:3032");
    SerialTable table (bench.opener());
    auto link = port ("EOS00001", "127.0.0.1:3032");
    link.framing = "length";
    table.reconcile ({ link });
    REQUIRE (within (2s, [&] { return table.stateOf ("EOS00001").state == "open"; }));

    const std::vector<std::uint8_t> first { '/', 'a', 0, 0, ',', 0, 0, 0 };
    const std::vector<std::uint8_t> second { '/', 'b', 'c', 0, ',', 'i', 0, 0, 0, 0, 0, 7 };
    const auto sized = [] (const std::vector<std::uint8_t>& packet)
    {
        std::string out (4, '\0');
        out[3] = static_cast<char> (packet.size());
        out.append (packet.begin(), packet.end());
        return out;
    };
    const auto both = sized (first) + sized (second);

    //  Cut inside the first size, then inside the second packet, then the rest.
    eos->say (both.substr (0, 2));
    eos->say (both.substr (2, 13));
    eos->say (both.substr (15));

    std::vector<std::vector<std::uint8_t>> got;
    const auto take = [&]
    {
        for (auto& [id, packets] : table.takePackets (64))
            for (auto& packet : packets)
                got.push_back (std::move (packet));
        return got.size() >= 2u;
    };
    within (2s, take);
    REQUIRE (got.size() == 2u);
    CHECK (got[0] == first);
    CHECK (got[1] == second);
    CHECK (table.takeLines (64).empty());

    CHECK (table.sendPacket ("EOS00001", first));
    CHECK (within (2s, [&] { return eos->sent() == sized (first); }));
    CHECK_FALSE (table.send ("EOS00001", "a line"));

    //  A size that cannot be a packet drops what is held and counts once; the next read starts afresh.
    got.clear();
    eos->say (std::string ("\xFF\xFF\xFF\xFF", 4));
    eos->say (sized (second));
    within (2s, [&] { take(); return ! got.empty(); });
    REQUIRE (got.size() == 1u);
    CHECK (got[0] == second);
    CHECK (table.stateOf ("EOS00001").dropped == 1u);
}

TEST_CASE ("serial: a raw link hands every read as one packet and sends the bytes as they are")
{
    Bench bench;
    auto desk = bench.plug ("127.0.0.1:51325");
    SerialTable table (bench.opener());
    auto link = port ("AHDL0001", "127.0.0.1:51325");
    link.framing = "raw";
    table.reconcile ({ link });
    REQUIRE (within (2s, [&] { return table.stateOf ("AHDL0001").state == "open"; }));

    const std::vector<std::uint8_t> sysex { 0xF0, 0x7F, 0x7F, 0x02, 0x01, 0x01, 0xF7 };
    desk->say (std::string (sysex.begin(), sysex.end()));

    std::vector<std::vector<std::uint8_t>> got;
    within (2s, [&]
    {
        for (auto& [id, packets] : table.takePackets (64))
            for (auto& packet : packets)
                got.push_back (std::move (packet));
        return ! got.empty();
    });
    REQUIRE (got.size() == 1u);
    CHECK (got[0] == sysex);

    CHECK (table.sendPacket ("AHDL0001", sysex));
    CHECK (within (2s, [&] { return desk->sent() == std::string (sysex.begin(), sysex.end()); }));
    CHECK_FALSE (table.send ("AHDL0001", "a line"));
}

TEST_CASE ("serial: a port's greeting is said as it opens and again after it comes back, and a changed one opens it again")
{
    //  DP.8: a console's login, the first line down a connection, every time.
    Bench bench;
    auto desk = bench.plug ("127.0.0.1:30000");
    SerialTable table (bench.opener());
    auto link = port ("MA200001", "127.0.0.1:30000");
    link.greeting = "login admin admin";
    table.reconcile ({ link });
    REQUIRE (within (2s, [&] { return table.stateOf ("MA200001").state == "open"; }));
    CHECK (within (2s, [&] { return desk->sent() == "login admin admin\n"; }));

    //  Then what the tick hands over, after it.
    CHECK (table.send ("MA200001", "Go+ Executor 1.1"));
    CHECK (within (2s, [&] { return desk->sent() == "login admin admin\nGo+ Executor 1.1\n"; }));

    //  Gone and back: the greeting again, before anything else.
    {
        const std::lock_guard<std::mutex> held (desk->lock);
        desk->plugged = false;
        desk->heard.clear();
    }
    REQUIRE (within (3s, [&] { return table.stateOf ("MA200001").state == "retrying"; }));
    {
        const std::lock_guard<std::mutex> held (desk->lock);
        desk->plugged = true;
    }
    REQUIRE (within (3s, [&] { return table.stateOf ("MA200001").state == "open"; }));
    CHECK (within (2s, [&] { return desk->sent() == "login admin admin\n"; }));

    //  A new greeting is a new opening.
    {
        const std::lock_guard<std::mutex> held (desk->lock);
        desk->heard.clear();
    }
    link.greeting = "login show show";
    table.reconcile ({ link });
    CHECK (within (3s, [&] { return desk->sent() == "login show show\n"; }));

    //  And none with tx off.
    {
        const std::lock_guard<std::mutex> held (desk->lock);
        desk->heard.clear();
    }
    link.tx = false;
    link.greeting = "login quiet quiet";
    table.reconcile ({ link });
    REQUIRE (within (3s, [&] { return table.stateOf ("MA200001").state == "open"; }));
    std::this_thread::sleep_for (100ms);
    CHECK (desk->sent().empty());
}

TEST_CASE ("serial: the angles framing - a Shure receiver's frames however the bytes come, a > inside braces kept, nothing after a frame sent")
{
    //  §124, BER: the cutter alone first.
    AngleCutter cutter;
    CHECK (cutter.feed ("< REP 1 AUDIO_GAIN 030 >< REP 1 CHAN").size() == 1u);
    const auto rest = cutter.feed (("_NAME {Lead > Vox} >"));
    REQUIRE (rest.size() == 1u);
    CHECK (rest[0] == "< REP 1 CHAN_NAME {Lead > Vox} >");

    //  Noise before a frame dropped; a frame cut short by the next one counted.
    AngleCutter noisy;
    const auto frames = noisy.feed ("junk< REP 1 AUD< REP 2 AUDIO_MUTE ON >");
    REQUIRE (frames.size() == 1u);
    CHECK (frames[0] == "< REP 2 AUDIO_MUTE ON >");
    CHECK (noisy.dropped == 1u);

    //  One too long dropped whole.
    AngleCutter longOne;
    CHECK (longOne.feed ("< " + std::string (40, 'x') + " >", 16).empty());
    CHECK (longOne.dropped == 1u);

    //  And on a link: frames as lines, the greeting and every line sent with nothing after.
    Bench bench;
    auto receiver = bench.plug ("127.0.0.1:2202");
    SerialTable table (bench.opener());
    auto link = port ("SHURE001", "127.0.0.1:2202");
    link.framing = "angles";
    link.greeting = "< GET 0 ALL >";
    table.reconcile ({ link });
    REQUIRE (within (2s, [&] { return table.stateOf ("SHURE001").state == "open"; }));
    CHECK (within (2s, [&] { return receiver->sent() == "< GET 0 ALL >"; }));

    CHECK (table.send ("SHURE001", "< SET 1 AUDIO_MUTE ON >"));
    CHECK (within (2s, [&] { return receiver->sent() == "< GET 0 ALL >< SET 1 AUDIO_MUTE ON >"; }));
    CHECK_FALSE (table.sendPacket ("SHURE001", { 0x01 }));

    receiver->say ("< REP 1 AUDIO_MUTE ON >< REP 1 CHAN_NAME {A>B} >< SAMP");
    std::vector<std::string> got;
    within (2s, [&]
    {
        for (auto& [id, lines] : table.takeLines (64))
            for (auto& line : lines)
                got.push_back (std::move (line));
        return got.size() >= 2u;
    });
    REQUIRE (got.size() == 2u);
    CHECK (got[0] == "< REP 1 AUDIO_MUTE ON >");
    CHECK (got[1] == "< REP 1 CHAN_NAME {A>B} >");
}

//==============================================================================
/*  RD.1: THE MACHINE'S HOLD (namespace draft §58, AGG): while it holds, a line
    or a packet is handed to no port and answered true - at the first line,
    before the port is looked up, because whether this machine has the port at
    all is the machine's and not the show's. The ports stay open and heard. */
#include <wfg/engine/pair/Hold.h>

TEST_CASE ("hold: a held table writes to no port and answers true - open, framed for packets, or not here at all")
{
    Bench bench;
    auto arduino = bench.plug ("COM3");
    SerialTable table (bench.opener());

    wfg::pair::Hold hold { true };
    table.setHold (&hold);

    table.reconcile ({ port ("SR000001", "COM3") });
    REQUIRE (within (2s, [&] { return table.stateOf ("SR000001").state == "open"; }));

    CHECK (table.send ("SR000001", "led 1"));                       // open: held
    CHECK (table.send ("NOSUCHID", "led 1"));                       // not declared here: held all the same
    CHECK (table.sendPacket ("SR000001", { 1, 2, 3 }));              // a lines port asked for a packet: held
    CHECK (table.sendPacket ("NOSUCHID", { 1, 2, 3 }));

    std::this_thread::sleep_for (60ms);
    CHECK (arduino->sent().empty());

    //  Still open, and still heard.
    arduino->say ("512\n");
    CHECK (collect (table, "SR000001", 1) == std::vector<std::string> { "512" });

    //  Let go, the line leaves; and a port not here refuses as ever.
    hold.set (false);
    CHECK (table.send ("SR000001", "led 0"));
    CHECK (within (2s, [&] { return arduino->sent() == "led 0\n"; }));
    CHECK_FALSE (table.send ("NOSUCHID", "led 0"));
}

//==============================================================================
/*  MQ.3: A STREAM CUT BY MQTT'S OWN LENGTH (namespace draft §67, ANE) - a
    device on a broker is a link on the table, its packets cut by the fixed
    header the codec reads - and what a session on the tick thread needs of
    the table: the openings counted, a sentence noted on an open link, and a
    link asked to open again. */
TEST_CASE ("serial: a link cut by mqtt packets hands whole packets, counts its openings, and opens again when asked")
{
    Bench bench;
    auto broker = bench.plug ("broker:1883");
    SerialTable table (bench.opener());
    auto link = port ("MQ000001", "broker:1883");
    link.framing = "mqtt";
    table.reconcile ({ link });
    REQUIRE (within (2s, [&] { return table.stateOf ("MQ000001").state == "open"; }));
    CHECK (table.stateOf ("MQ000001").openings == 1u);

    //  Two packets in one read, the second cut in two: a CONNACK and a PINGRESP.
    const std::string connack { '\x20', '\x02', '\x00', '\x00' };
    const std::string pingresp { '\xD0', '\x00' };
    broker->say (connack + pingresp.substr (0, 1));
    broker->say (pingresp.substr (1));

    std::vector<std::vector<std::uint8_t>> got;
    within (2s, [&]
    {
        for (auto& [id, packets] : table.takePackets (64))
            for (auto& p : packets)
                got.push_back (std::move (p));
        return got.size() >= 2;
    });
    REQUIRE (got.size() == 2u);
    CHECK (got[0] == std::vector<std::uint8_t> { 0x20, 0x02, 0x00, 0x00 });
    CHECK (got[1] == std::vector<std::uint8_t> { 0xD0, 0x00 });

    //  Sent as they are: no size, no frame.
    CHECK (table.sendPacket ("MQ000001", { 0xC0, 0x00 }));
    CHECK (within (2s, [&] { return broker->sent() == std::string ("\xC0\x00", 2); }));

    //  A sentence noted on the open link shows on its problem row.
    table.note ("MQ000001", "refused: bad user name or password");
    CHECK (table.stateOf ("MQ000001").problem == "refused: bad user name or password");

    //  Asked to open again: closed, opened, the count moved on.
    table.reopen ("MQ000001");
    REQUIRE (within (4s, [&] { return table.stateOf ("MQ000001").openings == 2u; }));
    CHECK (within (2s, [&] { return table.stateOf ("MQ000001").state == "open"; }));
    CHECK (table.stateOf ("MQ000001").problem.empty());

    //  A stream out of step - a fifth length byte - is dropped and counted, and read on.
    broker->say (std::string ("\x30\x80\x80\x80\x80\x01", 6));
    CHECK (within (2s, [&] { return table.stateOf ("MQ000001").dropped == 1u; }));
    broker->say (pingresp);
    got.clear();
    within (2s, [&]
    {
        for (auto& [id, packets] : table.takePackets (64))
            for (auto& p : packets)
                got.push_back (std::move (p));
        return ! got.empty();
    });
    CHECK (got.size() == 1u);

    //  A changed session word closes the link and opens it again.
    link.session = "another login";
    table.reconcile ({ link });
    CHECK (within (4s, [&] { return table.stateOf ("MQ000001").openings == 1u
                                      && table.stateOf ("MQ000001").state == "open"; }));
}
