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
