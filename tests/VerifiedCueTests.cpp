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
    The other direction: asking somebody else's box what a value actually is.

    Everything Go.dot did before this asserts. PRD §3.11 wants the loop closed -
    read the value back and compare - because that is what turns a list of
    network cues into a chain that can be relied on, and what puts a failure in
    the cue list instead of leaving it to be discovered by ear.

    THE TARGET HERE IS GO.DOT'S OWN OSCQUERY SERVER, driven by a scripted
    namespace the case controls. That is not a shortcut, it is the strongest
    available shape: a real HTTP server, on a real socket, answering real status
    codes, with the four behaviours a device can have - it agrees, it disagrees,
    it has nothing to say, it is not there - each arranged deliberately rather
    than waited for.

    Everything binds port 0 and reads the port back.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/cue/OscJob.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/osc/UdpEndpoint.h>
#include <wfg/engine/oscquery/OscQueryClient.h>
#include <wfg/engine/oscquery/OscQueryServer.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/MountProbe.h>
#include <wfg/engine/tree/MountSender.h>
#include <wfg/engine/tree/TreeCommands.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace wfg;

namespace
{
    /*  A device, as a namespace Go.dot's own server can publish.

        Its one node is `/desk/fader`, and the case decides what it says it
        holds - which is the whole of the scripting this needs. A device that
        agrees is one that reports what was written to it; one that disagrees
        reports something else; one with nothing to say has no value at all and
        the server answers 204. */
    struct ScriptedTarget final : public oscquery::Namespace
    {
        ScriptedTarget()
        {
            rebuild ({});
        }

        /** What the device will say it holds. Empty for "nothing to say". */
        void says (std::vector<osc::Value> values)
        {
            rebuild (std::move (values));
        }

        void rebuild (std::vector<osc::Value> values)
        {
            auto nodes = std::make_shared<std::vector<tree::Node>>();

            tree::Node root;
            root.address = "/desk";
            root.kind = tree::Kind::container;
            root.access = tree::Access::none;
            nodes->push_back (root);

            tree::Node fader;
            fader.address = "/desk/fader";
            fader.kind = tree::Kind::state;
            fader.access = tree::Access::readWrite;
            fader.typeTags = "f";
            fader.values = std::move (values);
            nodes->push_back (fader);

            /*  Sorted, because TreeSnapshot::find is a lower_bound and says so
                as a precondition. Two entries here, but the rule does not have
                a size below which it stops applying. */
            std::sort (nodes->begin(), nodes->end(),
                       [] (const tree::Node& a, const tree::Node& b)
                       { return a.address < b.address; });

            const std::lock_guard<std::mutex> lock { guard };
            published = std::make_shared<const tree::TreeSnapshot> (
                1, nodes, std::make_shared<const std::vector<tree::Node>>(),
                std::vector<tree::Node> {});
        }

        std::shared_ptr<const tree::TreeSnapshot> snapshot() const override
        {
            /*  A SLOW DEVICE, when a case asks for one: the answer waits while
                `holding` is set - for a second at the most, so a case that
                fails while holding cannot leave the server's thread stuck. */
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds (1);

            while (holding.load() && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for (std::chrono::milliseconds (1));

            const std::lock_guard<std::mutex> lock { guard };
            return published;
        }

        void write (const std::string&, const osc::Packet&) override {}
        void forget (const std::string&) override {}
        bool shouldPush (const std::string&, const std::string&,
                         const std::string&) const override { return true; }
        int oscPort() const override { return 0; }

        mutable std::mutex guard;
        std::shared_ptr<const tree::TreeSnapshot> published;
        std::atomic<bool> holding { false };
    };

    /** The scripted target, served over HTTP on a port of its own. */
    struct FakeDevice
    {
        FakeDevice()
        {
            REQUIRE (server.start (0, target));
            REQUIRE (server.boundPort() > 0);
        }

        ~FakeDevice() { server.stop(); }

        int port() const { return server.boundPort(); }

        ScriptedTarget target;
        oscquery::OscQueryServer server;
    };
}

//==============================================================================
TEST_CASE ("oscquery client: it reads a value back off a real server")
{
    /*  The client against Go.dot's own server, which is the closest thing to a
        real device this suite can stand up - and a useful one to have, because
        Go.dot IS an OSCQuery target and a chain of Go.dots is a shape the PRD
        expects. */
    FakeDevice device;
    device.target.says ({ osc::Value::float32 (0.5f) });

    const auto value = oscquery::OscQueryClient::readValue ("127.0.0.1", device.port(),
                                                            "/desk/fader", "f", 4000);

    REQUIRE (value.has_value());
    CHECK (*value == osc::Value::float32 (0.5f));
}

TEST_CASE ("oscquery client: the query string arrives unmangled")
{
    /*  THE REASON THIS CLASS EXISTS AT ALL. OSCQuery asks with a BARE key -
        `?VALUE`, `?HOST_INFO` - and juce::URL re-encodes that as `?VALUE=`,
        which a conforming server does not match. WFS-DIY hit exactly this and
        abandoned juce::URL in its own client.

        A `?VALUE=` would be refused with 400 by this project's own server, so
        asserting a 200 here asserts that the bare key survived. */
    FakeDevice device;
    device.target.says ({ osc::Value::float32 (0.25f) });

    const auto bare = oscquery::OscQueryClient::get ("127.0.0.1", device.port(),
                                                     "/desk/fader", "VALUE", 4000);
    REQUIRE (bare.ok);
    CHECK (bare.status == 200);

    /*  And the shape juce::URL would have produced, so that what is being
        asserted above is a difference and not a coincidence. */
    const auto mangled = oscquery::OscQueryClient::get ("127.0.0.1", device.port(),
                                                        "/desk/fader", "VALUE=", 4000);
    REQUIRE (mangled.ok);
    CHECK (mangled.status == 400);
}

TEST_CASE ("oscquery client: the four answers are four different things")
{
    /*  Each of these is a value the client must NOT return, and each sends a
        different person to look at a different thing. Collapsing them - which a
        client returning "no value" for all four would do - is how a device that
        is not plugged in gets diagnosed as a device that disagrees. */
    FakeDevice device;

    SUBCASE ("200 with a value: an answer")
    {
        device.target.says ({ osc::Value::float32 (0.75f) });

        const auto reply = oscquery::OscQueryClient::get ("127.0.0.1", device.port(),
                                                          "/desk/fader", "VALUE", 4000);
        CHECK (reply.status == 200);
    }

    SUBCASE ("204: the node is real and has no value yet")
    {
        device.target.says ({});

        const auto reply = oscquery::OscQueryClient::get ("127.0.0.1", device.port(),
                                                          "/desk/fader", "VALUE", 4000);
        CHECK (reply.status == 204);
        CHECK_FALSE (oscquery::OscQueryClient::readValue ("127.0.0.1", device.port(),
                                                          "/desk/fader", "f", 4000)
                       .has_value());
    }

    SUBCASE ("404: the device does not have that node")
    {
        const auto reply = oscquery::OscQueryClient::get ("127.0.0.1", device.port(),
                                                          "/desk/nosuch", "VALUE", 4000);
        CHECK (reply.status == 404);
    }

    SUBCASE ("nothing at all: the device is not there")
    {
        /*  A port nobody is listening on. The exchange does not complete, which
            is different from every status above: there is no server to have an
            opinion. */
        const auto reply = oscquery::OscQueryClient::get ("127.0.0.1", 1, "/desk/fader",
                                                          "VALUE", 250);
        CHECK_FALSE (reply.ok);
        CHECK (reply.status == 0);
        CHECK_FALSE (reply.error.empty());
    }
}

TEST_CASE ("oscquery client: what comes back is coerced to what the node declared")
{
    /*  JSON HAS THREE SCALAR TYPES AND OSC HAS TEN. A reply of `1` is an
        integer to a JSON reader and `1.0` is a double, and neither compares
        equal to the float32 that was written - so without the node's declared
        tag a verified cue would time out against a device doing exactly what it
        was told, for ever, and the log would say the device disagreed.

        Checked against a string literal rather than a socket, because the
        parsing is the part worth pinning. */
    using Client = oscquery::OscQueryClient;

    const auto asFloat = Client::valueFromReply (R"({"VALUE": [1]})", "f");
    REQUIRE (asFloat.has_value());
    CHECK (*asFloat == osc::Value::float32 (1.0f));

    const auto asInt = Client::valueFromReply (R"({"VALUE": [1.0]})", "i");
    REQUIRE (asInt.has_value());
    CHECK (*asInt == osc::Value::int32 (1));

    const auto asString = Client::valueFromReply (R"({"VALUE": ["scene 4"]})", "s");
    REQUIRE (asString.has_value());
    CHECK (*asString == osc::Value::string ("scene 4"));

    /*  And the refusals, each of which is "no answer" rather than a zero. */
    CHECK_FALSE (Client::valueFromReply (R"({"VALUE": []})", "f").has_value());
    CHECK_FALSE (Client::valueFromReply (R"({"TYPE": "f"})", "f").has_value());
    CHECK_FALSE (Client::valueFromReply ("not json at all", "f").has_value());
}

//==============================================================================
namespace
{
    constexpr const char* deskJson = R"JSON({
      "FULL_PATH": "/desk",
      "CONTENTS": {
        "fader": { "FULL_PATH": "/desk/fader", "TYPE": "f", "ACCESS": 3 }
      }
    })JSON";

    /*  A show with one verified cue, a device that can be scripted, and the
        whole path between them: the sender, the probe thread, the readback
        command and the Runner's own comparison. */
    struct VerifiedRig
    {
        VerifiedRig()
            : probe (engine)
        {
            REQUIRE (socket.start (0, [] (osc::Datagram) {}));

            mountDeclaration.id = "K3PV7WRB";
            mountDeclaration.prefix = "/desk";
            mountDeclaration.namespaceFile = "namespaces/desk.json";
            mountDeclaration.host = "127.0.0.1";
            mountDeclaration.port = socket.boundPort();
            mountDeclaration.readback = "oscquery";
            mountDeclaration.queryPort = device.port();

            REQUIRE (mounts.load (mountDeclaration, deskJson).ok);

            sender.setSocket (socket);

            engine.log().openInMemory ({});
            /*  A WRITE TO SOMEBODY ELSE'S NODE, wired exactly as `wfg serve`
                wires it: the value lands in the mount table, which is what a
                client reads and what a replay reproduces, and it is queued for
                the end of the tick, which is what the desk hears. Without this
                the rig could pre-send but never put anything back, because a
                restore IS one of these writes. */
            doc::registerDocumentCommands (
                engine.commands(), document,
                [this] (const std::string& address, const osc::Value& value)
                {
                    const auto written = mounts.write (address, value);

                    if (! written.ok)
                        return Outcome::rejected (written.reason);

                    if (const auto* declaration = mounts.declarationOf (written.mountId))
                        sender.queue (written.mountId,
                                      { declaration->host, declaration->port,
                                        declaration->rateCap },
                                      address, written.value);

                    return Outcome::ok ({ osc::Value::string (address), written.value });
                });
            cue::registerCueCommands (engine.commands(), document, focus);
            cue::registerRunCommands (engine.commands(), runs);
            cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);
            tree::registerMountCommands (engine.commands(), document, mounts, nowhere);

            runner.setMounts (&mounts, &sender, &probe);
            probe.setTimeout (1500);
            REQUIRE (probe.start());

            listId = document.createList ("Cues").id;
        }

        ~VerifiedRig()
        {
            probe.stop();
            socket.stop();
        }

        /*  THE MOUNT SAYS AN EARLY WRITE IS SAFE. PRD §3.3 makes
            `anticipatable` false for a third party by default - we do not get
            to decide that somebody else's box does not mind being written to
            ahead of time - so a test about anticipation has to say so out loud,
            exactly as a designer would. */
        void anticipate()
        {
            mountDeclaration.anticipatable = true;
            REQUIRE (mounts.load (mountDeclaration, deskJson).ok);
        }

        /** Parks the pointer and lets the horizon see it, as a show does. */
        void setStandby (const std::string& cueId)
        {
            document.setAttribute (cue::standbyAddressOf (listId), cueId);
            tickOnce();
        }

        std::string makeVerified (const std::string& atom, const char* timeout = "5")
        {
            const auto id = document.createCue (listId, index++, "osc", "Desk").id;
            const auto base = "/godot/cue/" + id + "/";

            document.setAttribute (base + "address", "/desk/fader");
            document.setAttribute (base + "value", atom);
            document.setAttribute (base + "wait", "verified");
            document.setAttribute (base + "timeout", timeout);
            return id;
        }

        void tickOnce()
        {
            runner.beforeTick (engine, tick);
            engine.processTick (tick++);
            sender.flush();
        }

        void fire (const std::string& cueId)
        {
            engine.submit ("cli", "cue.fire", { osc::Value::string (cueId) });
            tickOnce();
        }

        /*  Ticks until the cue is over or the patience runs out. The answer
            crosses a socket and comes back on another thread, so how many ticks
            it takes is the operating system's business - a fixed count is
            either slow or flaky and usually both. */
        const cue::Run* runUntilFinished (const std::string& cueId, int atMost = 600)
        {
            for (int i = 0; i < atMost; ++i)
            {
                if (const auto* run = runOf (cueId); run != nullptr && run->isFinished())
                    return run;

                tickOnce();
                std::this_thread::sleep_for (std::chrono::milliseconds (2));
            }

            return runOf (cueId);
        }

        const cue::Run* runOf (const std::string& cueId) const
        {
            for (const auto& run : runs.all())
                if (run.cue == cueId)
                    return &run;

            return nullptr;
        }

        FakeDevice device;
        osc::UdpEndpoint socket;
        juce::File nowhere;
        tree::MountDeclaration mountDeclaration;

        tree::MountTable mounts;
        tree::MountSender sender;

        Engine engine;
        tree::MountProbe probe;

        doc::ShowDocument document;
        cue::RunTable runs;
        cue::Focus focus;
        doc::IdRegistry runIds = doc::IdRegistry::withSeed (23);
        cue::Runner runner { document, runs, runIds, focus };

        std::string listId;
        int index = 0;
        std::int64_t tick = 0;
    };
}

TEST_CASE ("verified: the device agrees, and the cue says so")
{
    VerifiedRig rig;

    /*  The device will report back exactly what the cue writes, which is what a
        working processor does. */
    rig.device.target.says ({ osc::Value::float32 (0.75f) });

    const auto cueId = rig.makeVerified ("f:0.75");
    rig.fire (cueId);

    const auto* run = rig.runUntilFinished (cueId);
    REQUIRE (run != nullptr);

    INFO ("state " << run->state << ", error " << run->error);
    CHECK (run->state == cue::runState::done);
    CHECK (run->error.empty());
}

TEST_CASE ("verified: the device disagrees, and that is a different failure from silence")
{
    /*  THE ONE FAILURE THAT MEANS THE DEVICE IS THERE and is not doing what it
        was told - a clipped range, a mode that ignores the parameter, a channel
        somebody re-patched. It deserves its own word because it sends a
        different person to look than a timeout does. */
    VerifiedRig rig;

    rig.device.target.says ({ osc::Value::float32 (0.2f) });

    const auto cueId = rig.makeVerified ("f:0.75", "2");
    rig.fire (cueId);

    const auto* run = rig.runUntilFinished (cueId);
    REQUIRE (run != nullptr);

    INFO ("state " << run->state << ", error " << run->error);
    CHECK (run->state == cue::runState::failed);
    CHECK (run->error == cue::oscError::disagreed);
}

TEST_CASE ("verified: a device with nothing to say times out, and says which")
{
    /*  204 rather than 200: the node is real and has no value yet. The client
        returns nothing, the probe submits nothing - a silence is not a state
        transition and §3.15 keeps it out of the log - and the cue's own
        patience is what turns it into a failure. */
    VerifiedRig rig;

    rig.device.target.says ({});

    const auto cueId = rig.makeVerified ("f:0.75", "0.2");
    rig.fire (cueId);

    const auto* run = rig.runUntilFinished (cueId);
    REQUIRE (run != nullptr);

    INFO ("state " << run->state << ", error " << run->error);
    CHECK (run->state == cue::runState::failed);
    CHECK (run->error == cue::oscError::timeout);
}

TEST_CASE ("verified: an answer to somebody else's question does not count")
{
    /*  THE LINE THAT MAKES THIS VERIFICATION RATHER THAN THE APPEARANCE OF IT.

        A read-back is remembered, so a cue that wrote the same node a minute
        ago has left an answer lying about. Without forgetting it at the moment
        of writing, the next cue on that node would find a match on its first
        tick and report verified with nothing having been asked - and it would
        do that even against a device that had been unplugged in between. */
    VerifiedRig rig;

    rig.device.target.says ({ osc::Value::float32 (0.75f) });

    const auto first = rig.makeVerified ("f:0.75");
    rig.fire (first);
    REQUIRE (rig.runUntilFinished (first)->state == cue::runState::done);

    /*  The device is now unplugged, and still holds the answer it gave. */
    rig.device.server.stop();

    const auto second = rig.makeVerified ("f:0.75", "0.2");
    rig.fire (second);

    const auto* run = rig.runUntilFinished (second);
    REQUIRE (run != nullptr);

    INFO ("state " << run->state << ", error " << run->error);
    CHECK (run->state == cue::runState::failed);
    CHECK (run->error == cue::oscError::timeout);
}

TEST_CASE ("verified: every answer is in the log, so a replay can reach the same verdict")
{
    /*  §3.15: a read-back arriving is a state transition and goes in the log.
        That is what makes a verified cue replayable at all - the answer is a
        record, and `wfg replay` re-injects it and reaches the same verdict on
        the same tick with no network and no device in the room. */
    VerifiedRig rig;

    rig.device.target.says ({ osc::Value::float32 (0.5f) });

    const auto cueId = rig.makeVerified ("f:0.5");
    rig.fire (cueId);
    REQUIRE (rig.runUntilFinished (cueId)->state == cue::runState::done);

    const auto parsed = LogFile::parse (rig.engine.log().contents());

    const auto readback = std::find_if (parsed.records.begin(), parsed.records.end(),
                                        [] (const auto& r)
                                        { return r.command == "mount.readback"; });

    REQUIRE (readback != parsed.records.end());
    REQUIRE (readback->args.size() == 3u);

    CHECK (readback->origin == "mount:K3PV7WRB");
    CHECK (readback->args[0].getString() == "K3PV7WRB");
    CHECK (readback->args[1].getString() == "/desk/fader");
    CHECK (readback->args[2] == osc::Value::float32 (0.5f));
}

//==============================================================================
TEST_CASE ("question K: a cue that asks for verification a target cannot give is refused")
{
    /*  ANSWERED THE STRICT WAY (namespace draft §9, question K). `transport`
        says how to SEND and nothing said whether a box could be ASKED, so a
        verified cue aimed at a write-only device was a cue that could never
        succeed - and nothing would notice until somebody was standing in a
        theatre wondering why the list had stopped.

        The check is on the document alone: the cue names an address, the
        address falls under a mount's prefix, and the mount says whether it can
        answer. So it runs on a laptop with nothing plugged in, which is the
        machine somebody is sitting at when they have time to fix it.

        AND IT IS A LOAD REFUSAL, which decision K asked for and PR 2.6 did
        not build: the check sat in the mount loader, where serve, tree and
        replay printed it and opened the show anyway. Asking `validate()` is
        asking the thing that decides whether a show opens at all. */
    doc::ShowDocument document;

    const auto listId = document.createList ("Cues").id;

    /*  THROUGH THE DOCUMENT'S OWN DOOR rather than by appending a second
        <Mounts> container by hand. The check lives in `validate()` now, which
        walks the whole tree - so a hand-built duplicate container would be a
        second thing for it to find, and the test would be measuring the rig. */
    const auto mountEdit = document.createMount ("/desk", "namespaces/desk.json");
    REQUIRE (mountEdit.ok);

    const auto mountId = mountEdit.id;
    auto mount = document.findById (mountId);
    REQUIRE (mount.isValid());
    mount.setProperty (juce::Identifier ("port"), 9000, nullptr);

    const auto cueId = document.createCue (listId, 0, "osc", "Desk").id;
    document.setAttribute ("/godot/cue/" + cueId + "/address", "/desk/fader");
    document.setAttribute ("/godot/cue/" + cueId + "/value", "f:0.5");

    SUBCASE ("a mount that declares no readback")
    {
        document.setAttribute ("/godot/cue/" + cueId + "/wait", "verified");

        const auto problems = document.validate();

        REQUIRE (problems.size() == 1u);
        INFO (problems.front());
        CHECK (problems.front().find ("readback") != std::string::npos);
        CHECK (problems.front().find (cueId) != std::string::npos);
    }

    SUBCASE ("an address under no mount at all")
    {
        document.setAttribute ("/godot/cue/" + cueId + "/wait", "verified");
        document.setAttribute ("/godot/cue/" + cueId + "/address", "/nowhere/fader");

        const auto problems = document.validate();

        REQUIRE (problems.size() == 1u);
        INFO (problems.front());
        CHECK (problems.front().find ("no mounted namespace") != std::string::npos);
    }

    SUBCASE ("the same cue, not asking to be verified")
    {
        /*  `none` and `sent` need nothing of the target, so the same show with
            the same mount is perfectly sound. The refusal is about the promise
            the cue makes, not about the device. */
        document.setAttribute ("/godot/cue/" + cueId + "/wait", "sent");
        CHECK (document.validate().empty());
    }

    SUBCASE ("a mount that says it can be asked")
    {
        mount.setProperty (juce::Identifier ("readback"), "oscquery", nullptr);
        mount.setProperty (juce::Identifier ("queryPort"), 5005, nullptr);

        document.setAttribute ("/godot/cue/" + cueId + "/wait", "verified");
        CHECK (document.validate().empty());
    }

    SUBCASE ("readback declared but no port to ask on")
    {
        /*  Declaring the mechanism and not where to reach it is the same
            promise unkept, so it is the same refusal. */
        mount.setProperty (juce::Identifier ("readback"), "oscquery", nullptr);

        document.setAttribute ("/godot/cue/" + cueId + "/wait", "verified");
        CHECK (document.validate().size() == 1u);
    }
}

TEST_CASE ("verified: a cue nobody gave a timeout waits five seconds rather than none")
{
    /*  THE OTHER HALF OF A BUG THE GROUP SCHEDULER FOUND. Every one of these
        attribute reads used to go straight to the ValueTree - and the canonical
        writer OMITS an attribute holding its default while the reader leaves it
        absent, so a cue nobody filled in has no such property and the read
        answers with the type's zero.

        For most rows in the table that IS the default and it looks like it
        works. `osc/@timeout` defaults to FIVE SECONDS, so a verified cue
        created and left alone read a timeout of zero and failed on the tick
        after it asked - reporting `timeout` about a device that had not been
        given a chance to answer, which sends somebody to look at a network that
        is working.

        The fix is that every read goes through `ShowDocument::getAttribute`,
        which resolves the row and supplies the default. What this asserts is
        the behaviour rather than the mechanism: a cue with no timeout set is
        still waiting long after one with a zero timeout would have given up. */
    VerifiedRig rig;

    const auto id = rig.document.createCue (rig.listId, 90, "osc", "Bare").id;
    const auto base = "/godot/cue/" + id + "/";

    rig.document.setAttribute (base + "address", "/desk/fader");
    rig.document.setAttribute (base + "value", "f:0.5");
    rig.document.setAttribute (base + "wait", "verified");

    // Nothing was written, so the property is absent and the row supplies five.
    REQUIRE (rig.document.findById (id).hasProperty (juce::Identifier ("timeout")) == false);
    CHECK (rig.document.getAttribute (base + "timeout").value_or ("?") == "5");

    /*  A device with nothing to say - the node is real and has no value yet,
        so it answers 204 and the probe submits nothing - and the only way the
        cue can end is by running out of patience. Ten ticks is a fifth of a
        second; a cue that had read its timeout as zero would have given up on
        the second one.

        It used to be a device that answered 0.2, on the reasoning that such a
        cue could never be satisfied. It cannot - but a verify decides on the
        first answer that comes back, and one that differs fails the run with
        `disagreed` at once (namespace draft §13.6). On a slow macOS runner
        (d71883a) the answer crossed the socket inside the ten ticks, and the
        run was over for the right reason, which is not this case's. */
    rig.device.target.says ({});
    rig.fire (id);

    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    const auto* run = rig.runOf (id);
    REQUIRE (run != nullptr);
    CHECK (run->error != std::string ("timeout"));
    CHECK_FALSE (run->isFinished());
}

//==============================================================================
/*  ANTICIPATION, AND THE READ THAT MAKES IT REVOCABLE.

    PRD §3.12 wants a value at the desk before the operator's hand comes down.
    §13.1 makes that conditional on being able to take it back, and §3.3 makes
    `anticipatable` false for a third party by default - we do not get to decide
    that somebody else's box does not mind being written to early.

    So the order is: ASK the target what it holds, keep that as the restore
    value, then write, then verify. The ordinary verified path does the exact
    opposite - it forgets the remembered answer at the moment it writes and asks
    afterwards - which is right for verification and precisely wrong for a
    restore. Same two operations, opposite order, different question.
*/
namespace
{
    /** A scene whose header holds one network cue on the desk. */
    struct PreparedScene
    {
        PreparedScene (VerifiedRig& rig, const char* atom, const char* wait)
        {
            group = rig.document.createCue (rig.listId, rig.index++, "group", "Scene").id;

            const auto header = rig.document.createRole (group, "header");
            REQUIRE (header.ok);

            cue = rig.document.createCue (header.id, 0, "osc", "Position the source").id;

            const auto base = "/godot/cue/" + cue + "/";
            rig.document.setAttribute (base + "address", "/desk/fader");
            rig.document.setAttribute (base + "value", atom);
            rig.document.setAttribute (base + "wait", wait);
            rig.document.setAttribute (base + "timeout", "5");

            /*  Somewhere for the pointer to go when it leaves the block. */
            after = rig.document.createCue (rig.listId, rig.index++, "memo", "After").id;
        }

        std::string group, cue, after;
    };
}

TEST_CASE ("prepare: the value that was there is read before the new one is written")
{
    VerifiedRig rig;
    rig.anticipate();

    /*  What the desk is holding before anybody touches it. */
    rig.device.target.says ({ osc::Value::float32 (0.2f) });

    const PreparedScene scene { rig, "f:0.8", "none" };

    rig.setStandby (scene.group);

    /*  The read crosses a socket and comes back on another thread, so how many
        ticks it takes is the operating system's business. */
    for (int n = 0; n < 600; ++n)
    {
        const auto* seen = rig.runOf (scene.cue);

        if (seen != nullptr && ! seen->restoreAtom.empty())
            break;

        rig.tickOnce();
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }

    /*  ASKED AGAIN AFTER THE LOOP, because a tick can create a run and the run
        table is a vector: a pointer taken before one is a pointer into memory
        the next push_back may have moved. */
    const auto* run = rig.runOf (scene.cue);
    REQUIRE (run != nullptr);

    /*  IT KEPT WHAT WAS THERE. Not the value it wrote - which is what the
        verify path remembers - but the one it found. */
    CHECK (run->restoreAddress == "/desk/fader");

    const auto held = osc::Value::fromAtom (run->restoreAtom);
    REQUIRE (held.has_value());
    CHECK (*held == osc::Value::float32 (0.2f));

    /*  AND THEN IT WROTE. The mounted tree is what a client reads and what a
        replay reproduces; the datagram is what the desk hears. */
    for (int n = 0; n < 600; ++n)
    {
        if (const auto* now = rig.mounts.valueOf ("/desk/fader");
            now != nullptr && *now == osc::Value::float32 (0.8f))
            break;

        rig.tickOnce();
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }

    const auto* written = rig.mounts.valueOf ("/desk/fader");
    REQUIRE (written != nullptr);
    CHECK (*written == osc::Value::float32 (0.8f));
}

TEST_CASE ("prepare: the pointer leaving puts the desk back, as an ordinary write")
{
    /*  §13.1's whole bargain, end to end. The restore goes out as `node.set` -
        the command any client uses to write a mounted node - so a replay
        reproduces it exactly as it reproduces every other write, with the value
        in the record and no network in the room. */
    VerifiedRig rig;
    rig.anticipate();
    rig.device.target.says ({ osc::Value::float32 (0.2f) });

    const PreparedScene scene { rig, "f:0.8", "none" };

    rig.setStandby (scene.group);

    for (int n = 0; n < 600; ++n)
    {
        if (const auto* now = rig.mounts.valueOf ("/desk/fader");
            now != nullptr && *now == osc::Value::float32 (0.8f))
            break;

        rig.tickOnce();
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }

    REQUIRE (rig.mounts.valueOf ("/desk/fader") != nullptr);
    REQUIRE (*rig.mounts.valueOf ("/desk/fader") == osc::Value::float32 (0.8f));

    /*  Away. */
    rig.setStandby (scene.after);
    rig.tickOnce();
    rig.tickOnce();

    const auto* restored = rig.mounts.valueOf ("/desk/fader");
    REQUIRE (restored != nullptr);
    CHECK (*restored == osc::Value::float32 (0.2f));

    /*  AND THE RESTORE IS IN THE LOG AS A WRITE, which is what makes a replay
        of a rehearsal put the desk back too. */
    const auto parsed = LogFile::parse (rig.engine.log().contents());

    const auto set = std::find_if (parsed.records.begin(), parsed.records.end(),
                                   [] (const auto& record)
                                   {
                                       return record.command == "node.set"
                                               && ! record.args.empty()
                                               && record.args[0].getString() == "/desk/fader";
                                   });

    REQUIRE (set != parsed.records.end());
    REQUIRE (set->args.size() == 2u);
    CHECK (set->args[1] == osc::Value::float32 (0.2f));
}

//==============================================================================
/*  A SCENE THAT NEVER STARTED, STOPPED (namespace draft §23, 2026-09-30): given
    back the way the pointer moving away gives one back - what it pre-sent put
    back on the desk first, then revoked - and never through its footer, which
    would release what it never took. Esc itself leaves the standby's scene
    alone: it is not running, and the pointer has not moved. */
namespace
{
    /*  Ticks until the desk holds `wanted`, or the patience runs out. The
        pre-send's read crosses a socket, so how long it takes is the operating
        system's business. */
    void tickUntilDeskHolds (VerifiedRig& rig, float wanted)
    {
        for (int n = 0; n < 600; ++n)
        {
            if (const auto* now = rig.mounts.valueOf ("/desk/fader");
                now != nullptr && *now == osc::Value::float32 (wanted))
                return;

            rig.tickOnce();
            std::this_thread::sleep_for (std::chrono::milliseconds (2));
        }
    }

    /*  Where a record stands in the session's log: the first one of that
        command whose first argument is `first`, or the end. */
    std::size_t recordIndex (VerifiedRig& rig, const char* command, const std::string& first)
    {
        const auto records = LogFile::parse (rig.engine.log().contents()).records;

        for (std::size_t n = 0; n < records.size(); ++n)
            if (records[n].command == command && ! records[n].args.empty()
                  && records[n].args[0].isString() && records[n].args[0].getString() == first)
                return n;

        return records.size();
    }
}

TEST_CASE ("prepare: Esc on a running act takes back the scene prepared inside it, putting the desk back first")
{
    /*  A double Esc as well: a revocation is not a footer, so the killed act's
        prepared scene gives the desk back as the stopped one does. */
    for (const auto* level : { "run.stopAll", "run.killAll" })
    {
        INFO (std::string (level));
        VerifiedRig rig;
        rig.anticipate();
        rig.device.target.says ({ osc::Value::float32 (0.2f) });
        REQUIRE (rig.document.setAttribute ("/godot/list/goDebounce", "0").ok);

        //  An act (manual) opening with a memo, then a scene (manual) whose header pre-sends.
        const auto act = rig.document.createCue (rig.listId, rig.index++, "group", "Act").id;
        const auto opening = rig.document.createCue (act, 0, "memo", "Opening").id;
        const auto scene = rig.document.createCue (act, 1, "group", "Scene").id;
        const auto inside = rig.document.createCue (scene, 0, "memo", "Inside").id;

        const auto header = rig.document.createRole (scene, "header");
        REQUIRE (header.ok);

        const auto presend = rig.document.createCue (header.id, 0, "osc", "Position the source").id;
        rig.document.setAttribute ("/godot/cue/" + presend + "/address", "/desk/fader");
        rig.document.setAttribute ("/godot/cue/" + presend + "/value", "f:0.8");
        rig.document.setAttribute ("/godot/cue/" + presend + "/wait", "none");
        rig.document.setAttribute ("/godot/cue/" + presend + "/timeout", "5");

        const auto footer = rig.document.createRole (scene, "footer");
        REQUIRE (footer.ok);
        const auto release = rig.document.createCue (footer.id, 0, "memo", "Release").id;

        //  GO on the opening: the act runs, the pointer walks into the scene, and the horizon prepares it.
        rig.setStandby (opening);
        rig.engine.submit ("cli", "go", {});
        rig.tickOnce();

        REQUIRE (rig.document.findById (rig.listId)[juce::Identifier ("standby")].toString().toStdString()
                   == inside);

        tickUntilDeskHolds (rig, 0.8f);
        REQUIRE (rig.mounts.valueOf ("/desk/fader") != nullptr);
        REQUIRE (*rig.mounts.valueOf ("/desk/fader") == osc::Value::float32 (0.8f));

        const auto* ready = rig.runs.preparedRunOf (scene);
        REQUIRE (ready != nullptr);
        const auto sceneRun = ready->id;

        const auto* actRun = rig.runs.liveRunOf (act);
        REQUIRE (actRun != nullptr);
        CHECK (rig.runs.find (sceneRun)->parent == actRun->id);

        //  Esc: the act comes down, and the scene it was holding is given back.
        rig.engine.submit ("cli", level, {});

        for (int n = 0; n < 10; ++n)
            rig.tickOnce();

        REQUIRE (rig.mounts.valueOf ("/desk/fader") != nullptr);
        CHECK (*rig.mounts.valueOf ("/desk/fader") == osc::Value::float32 (0.2f));
        CHECK (rig.runs.find (sceneRun)->warning == cue::runWarning::revoked);
        CHECK (rig.runOf (release) == nullptr);

        //  PUT BACK FIRST, then revoked: a client watching sees the desk go back before the scene goes.
        const auto restored = recordIndex (rig, "node.set", "/desk/fader");
        const auto revoked = recordIndex (rig, "run.revoke", sceneRun);
        const auto end = LogFile::parse (rig.engine.log().contents()).records.size();

        CHECK (restored < end);
        CHECK (revoked < end);
        CHECK (restored < revoked);
    }
}

TEST_CASE ("prepare: a scene stopped while only prepared puts the desk back and runs no footer, and Esc leaves it prepared")
{
    for (const auto* how : { "run.stop", "run.kill", "run.stopAll" })
    {
        INFO (std::string (how));
        VerifiedRig rig;
        rig.anticipate();
        rig.device.target.says ({ osc::Value::float32 (0.2f) });
        REQUIRE (rig.document.setAttribute ("/godot/list/goDebounce", "0").ok);

        const PreparedScene scene { rig, "f:0.8", "none" };

        const auto footer = rig.document.createRole (scene.group, "footer");
        REQUIRE (footer.ok);
        const auto release = rig.document.createCue (footer.id, 0, "memo", "Release").id;

        rig.setStandby (scene.group);
        tickUntilDeskHolds (rig, 0.8f);
        REQUIRE (rig.mounts.valueOf ("/desk/fader") != nullptr);
        REQUIRE (*rig.mounts.valueOf ("/desk/fader") == osc::Value::float32 (0.8f));

        const auto* block = rig.runs.preparedRunOf (scene.group);
        REQUIRE (block != nullptr);
        const auto blockId = block->id;

        if (std::string (how) != "run.stopAll")
        {
            //  Stopped or killed by its run: the desk put back, the block revoked, no footer.
            rig.engine.submit ("cli", how, { osc::Value::string (blockId) });

            for (int n = 0; n < 5; ++n)
                rig.tickOnce();

            REQUIRE (rig.mounts.valueOf ("/desk/fader") != nullptr);
            CHECK (*rig.mounts.valueOf ("/desk/fader") == osc::Value::float32 (0.2f));
            CHECK (rig.runs.find (blockId)->warning == cue::runWarning::revoked);
            CHECK (rig.runOf (release) == nullptr);
            continue;
        }

        //  Esc: nothing running, so nothing stopped - the block and its value stay.
        rig.engine.submit ("cli", "run.stopAll", {});

        for (int n = 0; n < 10; ++n)
            rig.tickOnce();

        REQUIRE (rig.mounts.valueOf ("/desk/fader") != nullptr);
        CHECK (*rig.mounts.valueOf ("/desk/fader") == osc::Value::float32 (0.8f));
        REQUIRE (rig.runs.preparedRunOf (scene.group) != nullptr);
        CHECK (rig.runs.preparedRunOf (scene.group)->id == blockId);

        //  And GO adopts it: the pre-send was its execution, and does not run again.
        rig.engine.submit ("cli", "go", {});
        rig.tickOnce();

        const auto* live = rig.runs.liveRunOf (scene.group);
        REQUIRE (live != nullptr);
        CHECK (live->id == blockId);

        const auto presends = std::count_if (rig.runs.all().begin(), rig.runs.all().end(),
                                             [&scene] (const cue::Run& run) { return run.cue == scene.cue; });
        CHECK (presends == 1);
    }
}

TEST_CASE ("prepare: a read that comes back after its scene was given back writes nothing")
{
    /*  A PRE-SEND ASKS FIRST AND WRITES WHEN THE ANSWER COMES, and a scene can be
        given back in between. Its run was finished by the revocation, but the
        job asking for it went on: when the answer landed it wrote the new value
        to the desk - after the revocation, with nothing left to put it back.
        The test answers the read itself, so that it arrives when the test says.

        THE SAME TICK IS THE NARROW CASE: an answer taken in the drain that also
        stops the scene is there to be read in the very tick the scene is given
        back, before the revocation has been applied - the job read it, wrote,
        and the restore it would need was never asked for. */
    for (const auto* how : { "the pointer moving away", "a stop aimed at it",
                             "a stop aimed at it, answered in the same drain" })
    {
        INFO (std::string (how));
        VerifiedRig rig;
        rig.anticipate();
        rig.runner.setMounts (&rig.mounts, &rig.sender, nullptr);

        const PreparedScene scene { rig, "f:0.8", "none" };

        rig.setStandby (scene.group);

        //  The pre-send is out, asking what the desk holds, and nobody has answered.
        for (int n = 0; n < 5; ++n)
            rig.tickOnce();

        const auto* presend = rig.runOf (scene.cue);
        REQUIRE (presend != nullptr);
        REQUIRE (presend->restoreAtom.empty());
        REQUIRE (rig.runs.preparedRunOf (scene.group) != nullptr);

        const auto blockId = rig.runs.preparedRunOf (scene.group)->id;
        const auto together = std::string (how) == "a stop aimed at it, answered in the same drain";

        const auto answer = [&rig]
        {
            rig.engine.submit ("mount:K3PV7WRB", "mount.readback",
                               { osc::Value::string ("K3PV7WRB"), osc::Value::string ("/desk/fader"),
                                 osc::Value::float32 (0.2f) });
        };

        if (std::string (how) == "the pointer moving away")
            rig.document.setAttribute (cue::standbyAddressOf (rig.listId), scene.after);
        else
            rig.engine.submit ("cli", "run.stop", { osc::Value::string (blockId) });

        if (together)
            answer();

        for (int n = 0; n < 20 && ! rig.runs.find (blockId)->isFinished(); ++n)
            rig.tickOnce();

        REQUIRE (rig.runs.find (blockId)->isFinished());

        //  And now the answer arrives, when it has not already.
        if (! together)
            answer();

        for (int n = 0; n < 5; ++n)
            rig.tickOnce();

        CHECK (rig.mounts.valueOf ("/desk/fader") == nullptr);   // nothing written to the desk
    }
}

//==============================================================================
/*  A DOUBLE ESC AND THE PRE-SENDS (2026-10-02, H4, namespace draft §23.10). */
TEST_CASE ("double Esc: a pre-send of the GO's scene still asking when the press lands writes nothing when its answer comes")
{
    /*  A MEMBER IS KILLED A TICK AFTER ITS SCENE, by the scene's own job - and
        in that tick a pre-send still asking what the desk held read the answer
        and wrote its value: a datagram after the press that drops every
        action, and a desk value nothing would put back. Being killed from
        above now ends the job first. */
    VerifiedRig rig;
    rig.anticipate();
    rig.runner.setMounts (&rig.mounts, &rig.sender, nullptr);   // the test answers the read itself

    const PreparedScene scene { rig, "f:0.8", "none" };

    /*  A member that holds the scene in its members phase: a scene with
        nothing but its header ends at the GO, its pre-send left asking under
        a scene that is over (§23.10 names that road). */
    const auto hold = rig.document.createCue (scene.group, 0, "memo", "Hold").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + hold + "/preWait", "10").ok);

    rig.setStandby (scene.group);

    for (int n = 0; n < 5; ++n)
        rig.tickOnce();

    const auto* presend = rig.runOf (scene.cue);
    REQUIRE (presend != nullptr);
    REQUIRE (presend->restoreAtom.empty());                 // still asking
    const auto presendRun = presend->id;

    REQUIRE (rig.runs.preparedRunOf (scene.group) != nullptr);
    const auto blockId = rig.runs.preparedRunOf (scene.group)->id;

    /*  The GO adopts the scene, the pre-send still out: the press now kills it. */
    rig.engine.submit ("cli", "go", {});
    rig.tickOnce();
    rig.tickOnce();

    REQUIRE_FALSE (rig.runs.find (blockId)->onlyPrepared());
    REQUIRE_FALSE (rig.runs.find (blockId)->isFinished());
    REQUIRE_FALSE (rig.runs.find (presendRun)->isFinished());

    /*  The press, and the desk's answer in the same drain. */
    rig.engine.submit ("cli", "run.killAll", {});
    rig.engine.submit ("mount:K3PV7WRB", "mount.readback",
                       { osc::Value::string ("K3PV7WRB"), osc::Value::string ("/desk/fader"),
                         osc::Value::float32 (0.2f) });
    rig.tickOnce();

    for (int n = 0; n < 5; ++n)
        rig.tickOnce();

    CHECK (rig.runs.find (presendRun)->isFinished());
    CHECK (rig.mounts.valueOf ("/desk/fader") == nullptr);   // nothing written to the desk
    CHECK (rig.sender.sentFor ("K3PV7WRB") == 0u);
}

TEST_CASE ("double Esc: the standby's pre-send already on its way still leaves")
{
    /*  WHAT THE PRESS LEAVES READY KEEPS WHAT MAKES IT READY (§23.3, §23.10).
        The standby's prepared scene is spared by a double Esc, and the GO after
        it adopts the scene with its pre-send counted as done - so a pre-send
        written in the press's own tick, still in the sender's queue, has to
        leave: dropped, the desk would never get the value the scene believes
        it holds, and nothing would send it again. */
    VerifiedRig rig;
    rig.anticipate();
    rig.runner.setMounts (&rig.mounts, &rig.sender, nullptr);

    const PreparedScene scene { rig, "f:0.8", "none" };

    rig.setStandby (scene.group);

    for (int n = 0; n < 5; ++n)
        rig.tickOnce();

    REQUIRE (rig.runs.preparedRunOf (scene.group) != nullptr);
    const auto blockId = rig.runs.preparedRunOf (scene.group)->id;

    /*  The desk answers; the pre-send writes in the next tick's hook - the
        press's - and its value is queued when the press drains. */
    rig.engine.submit ("mount:K3PV7WRB", "mount.readback",
                       { osc::Value::string ("K3PV7WRB"), osc::Value::string ("/desk/fader"),
                         osc::Value::float32 (0.2f) });
    rig.tickOnce();
    REQUIRE (rig.sender.sentFor ("K3PV7WRB") == 0u);

    rig.engine.submit ("cli", "run.killAll", {});
    rig.tickOnce();

    CHECK (rig.sender.sentFor ("K3PV7WRB") == 1u);
    REQUIRE (rig.mounts.valueOf ("/desk/fader") != nullptr);
    CHECK (*rig.mounts.valueOf ("/desk/fader") == osc::Value::float32 (0.8f));

    /*  And the scene is still the standby's, made ready. */
    REQUIRE (rig.runs.find (blockId) != nullptr);
    CHECK_FALSE (rig.runs.find (blockId)->isFinished());
    CHECK (rig.runs.preparedRunOf (scene.group) != nullptr);
}

TEST_CASE ("prepare: a block whose pre-sent value came back equal says verified")
{
    /*  The one word of §13.6's six that means the DESK AGREES, rather than that
        Go.dot did what it meant to. */
    VerifiedRig rig;
    rig.anticipate();
    rig.device.target.says ({ osc::Value::float32 (0.2f) });

    const PreparedScene scene { rig, "f:0.8", "verified" };

    rig.setStandby (scene.group);

    /*  The desk takes the value once it has been read - which is what a desk
        that is plugged in does, and what this fake one has to be told to do. */
    for (int n = 0; n < 600; ++n)
    {
        const auto* run = rig.runOf (scene.cue);

        if (run != nullptr && ! run->restoreAtom.empty())
        {
            rig.device.target.says ({ osc::Value::float32 (0.8f) });
            break;
        }

        rig.tickOnce();
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }

    for (int n = 0; n < 600; ++n)
    {
        const auto* seen = rig.runs.preparedRunOf (scene.group);

        if (seen != nullptr && seen->prepare == std::string (cue::preparedness::verified))
            break;

        rig.tickOnce();
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }

    const auto* block = rig.runs.preparedRunOf (scene.group);
    REQUIRE (block != nullptr);
    CHECK (std::string (block->prepare) == cue::preparedness::verified);

    /*  And the cue does not run again at entry: its preparation WAS its
        execution, so the header's remainder leaves it out. */
    const auto* presend = rig.runOf (scene.cue);
    REQUIRE (presend != nullptr);
    INFO ("run error: " << presend->error << "; state " << presend->state);
    CHECK (presend->state == cue::runState::done);
}

TEST_CASE ("prepare: a mount that cannot be asked is never pre-sent")
{
    /*  §13.1 as a refusal rather than a warning. A value sent early to a target
        nobody can ask what it held is a value nobody can put back, so the cue
        is left for entry and the block says `partial` - which is §3.6's own
        word for a block that is not anticipatable all the way through. */
    VerifiedRig rig;

    rig.mountDeclaration.anticipatable = true;
    rig.mountDeclaration.readback = "none";
    REQUIRE (rig.mounts.load (rig.mountDeclaration, deskJson).ok);

    rig.device.target.says ({ osc::Value::float32 (0.2f) });

    const PreparedScene scene { rig, "f:0.8", "none" };

    rig.setStandby (scene.group);

    for (int n = 0; n < 20; ++n)
        rig.tickOnce();

    /*  Nothing ran, nothing was written, and nothing is holding a restore. */
    CHECK (rig.runOf (scene.cue) == nullptr);

    const auto* value = rig.mounts.valueOf ("/desk/fader");
    CHECK (value == nullptr);

    const auto* block = rig.runs.preparedRunOf (scene.group);
    REQUIRE (block != nullptr);
    CHECK (std::string (block->prepare) == cue::preparedness::partial);
}

TEST_CASE ("prepare: a mount that claims anticipation without read-back is a validate warning")
{
    /*  §13.1's condition, said where a designer will read it. The two
        attributes are set in different places, and a mount with one and not the
        other has told Go.dot that an early write is safe and given it no way to
        undo one - so nothing on it is ever pre-sent and every cue aimed at it
        runs at entry.

        A WARNING AND NOT A REFUSAL: the show is complete and correct, and what
        it loses is a saved moment rather than a sound. */
    doc::ShowDocument document;

    const auto mount = document.createMount ("/desk", "namespaces/desk.json");
    REQUIRE (mount.ok);

    auto node = document.findById (mount.id);
    node.setProperty (juce::Identifier ("port"), 9000, nullptr);
    node.setProperty (juce::Identifier ("anticipatable"), true, nullptr);

    const auto said = [&document] (const char* fragment)
    {
        for (const auto& problem : document.warnings())
            if (problem.find (fragment) != std::string::npos)
                return true;

        return false;
    };

    CHECK (said ("nothing will be pre-sent"));

    /*  And it goes when the mount can be asked. */
    node.setProperty (juce::Identifier ("readback"), "oscquery", nullptr);
    node.setProperty (juce::Identifier ("queryPort"), 9010, nullptr);

    CHECK_FALSE (said ("nothing will be pre-sent"));
}


//==============================================================================
/*  THE OBSERVATION SWEEP (§13.10): every applied trigger reads the world back.

    A step is a place the show was, and a place is only worth going back to if
    what was there is known. So at each step the Runner asks every target that
    can be asked about every address the show writes to it - once a second per
    mount, however many steps - and the answers are `mount.readback` records
    with the observation flag, which is what puts a fader somebody moved by
    hand into the log at the step that saw it.
*/
namespace
{
    struct ObservingRig : VerifiedRig
    {
        /** An ordinary network cue on the desk's one node. Nothing is verified. */
        std::string makeSent (const std::string& atom)
        {
            const auto id = document.createCue (listId, index++, "osc", "Desk").id;
            const auto base = "/godot/cue/" + id + "/";
            document.setAttribute (base + "address", "/desk/fader");
            document.setAttribute (base + "value", atom);
            document.setAttribute (base + "wait", "sent");
            return id;
        }

        /*  Ticks until an observation of the desk's node has landed, or the
            patience runs out. The answer crosses a socket on another thread. */
        bool observed (int atMost = 600)
        {
            for (int i = 0; i < atMost; ++i)
            {
                if (mounts.observedOf ("/desk/fader") != nullptr)
                    return true;

                tickOnce();
                std::this_thread::sleep_for (std::chrono::milliseconds (2));
            }

            return mounts.observedOf ("/desk/fader") != nullptr;
        }
    };
}

TEST_CASE ("observation: a step asks the desk what it holds, and the answer is not a read-back")
{
    ObservingRig rig;

    /*  The cue will write 0.75; the desk, asked afterwards, says 0.2 - a fader
        somebody moved, or a device that clips. Either way it is what is THERE,
        and that is what an observation is for. */
    rig.device.target.says ({ osc::Value::float32 (0.2f) });

    const auto cueId = rig.makeSent ("f:0.75");

    CHECK (rig.runner.observationsAsked() == 0u);

    rig.fire (cueId);                    // the step; the sweep is the next tick's
    REQUIRE (rig.observed());

    CHECK (rig.runner.observationsAsked() == 1u);
    CHECK (*rig.mounts.observedOf ("/desk/fader") == osc::Value::float32 (0.2f));

    /*  Not a read-back: no cue was waiting, and a verify that found this lying
        about would report a device agreeing with something it never said. */
    CHECK (rig.mounts.readbackOf ("/desk/fader") == nullptr);

    /*  And what Go.dot WROTE is still the write, untouched by what was seen. */
    REQUIRE (rig.mounts.valueOf ("/desk/fader") != nullptr);
    CHECK (*rig.mounts.valueOf ("/desk/fader") == osc::Value::float32 (0.75f));

    /*  The record says which it was, so a replay puts it in the same store. */
    const auto log = rig.engine.log().contents();
    CHECK (log.find ("mount.readback") != std::string::npos);
    CHECK (log.find ("/desk/fader") != std::string::npos);
}

TEST_CASE ("observation: one sweep a second per mount, however many steps")
{
    /*  Steps are the operator's business and can come in threes. What §13.10
        promised the desk was one sweep a second, and M21 priced exactly that:
        a second step inside the cap gets nothing, and the next step after the
        cap gets a fresh sweep rather than a queued old one. */
    ObservingRig rig;
    rig.device.target.says ({ osc::Value::float32 (0.5f) });

    const auto first = rig.makeSent ("f:0.1");
    const auto second = rig.makeSent ("f:0.2");
    const auto third = rig.makeSent ("f:0.3");

    rig.fire (first);

    /*  And let the first answer land before counting, because the probe's own
        rule - one outstanding question per address - would otherwise make this
        case about the network rather than the cap: fifty ticks with no sleep
        in them pass in microseconds, well inside one round trip. */
    REQUIRE (rig.observed());

    rig.fire (second);                    // the next tick: inside the cap
    rig.tickOnce();

    CHECK (rig.runner.observationsAsked() == 1u);

    for (int n = 0; n < 50; ++n)
        rig.tickOnce();

    rig.fire (third);
    rig.tickOnce();

    CHECK (rig.runner.observationsAsked() == 2u);
}

TEST_CASE ("observation: a verify and a sweep on one address are two questions, not one")
{
    /*  The probe drops a duplicate question so that a cue asking on every tick
        does not fill the queue - and an observation of the address a verified
        cue is waiting on must not be the duplicate that gets dropped, or a
        sweep of forty addresses could turn a cue into a timeout. Asked of a
        probe that is not running, so both sit in the queue to be counted. */
    Engine engine;
    tree::MountProbe idle { engine };

    tree::MountProbe::Question verify;
    verify.mountId = "K3PV7WRB";
    verify.host = "127.0.0.1";
    verify.queryPort = 5005;
    verify.address = "/desk/fader";
    verify.typeTag = "f";

    auto observation = verify;
    observation.observation = true;

    CHECK (idle.ask (verify));
    CHECK (idle.ask (observation));
    CHECK (idle.outstanding() == 2u);

    /*  And each is still one question: the same one twice is dropped. */
    CHECK_FALSE (idle.ask (verify));
    CHECK_FALSE (idle.ask (observation));
    CHECK (idle.outstanding() == 2u);
}

TEST_CASE ("jump: a value the desk was seen to hold is what the diff is against")
{
    /*  §3.13's full sentence, at last. PR 4.8's jump compared the plan with
        what Go.dot last WROTE, so a fader somebody moved by hand agreed with
        the tree and was left where it was. With an observation in the table
        the comparison is against what the desk was SEEN to hold - and where
        there is none, what was written is still the best account there is. */
    ObservingRig rig;
    rig.device.target.says ({ osc::Value::float32 (0.75f) });

    const auto cueId = rig.makeSent ("f:0.75");
    const auto after = rig.document.createCue (rig.listId, rig.index++, "memo", "After").id;
    juce::ignoreUnused (after);

    const auto jump = [&rig, &cueId]
    {
        REQUIRE (rig.engine.submit ("cli", "list.aim",
                                    { osc::Value::string (rig.listId),
                                      osc::Value::string (cueId),
                                      osc::Value::float64 (0.0) }));
        rig.tickOnce();
        REQUIRE (rig.engine.submit ("cli", "list.loadToTime",
                                    { osc::Value::string (rig.listId) }));
        rig.tickOnce();
        rig.tickOnce();             // the write the jump queued lands on the next tick
    };

    /*  THE DESK AGREES: nothing is sent. Nothing has been written yet, so the
        only account of the node is the observation, and it matches the plan. */
    rig.mounts.noteObservation ("/desk/fader", osc::Value::float32 (0.75f));
    jump();
    CHECK (rig.mounts.valueOf ("/desk/fader") == nullptr);

    /*  THE DESK DISAGREES: the plan's value is sent, through the ordinary write
        - which lands it in the table and ends the observation it corrected. */
    rig.mounts.noteObservation ("/desk/fader", osc::Value::float32 (0.2f));
    jump();

    INFO ("log: " << rig.engine.log().contents());
    REQUIRE (rig.mounts.valueOf ("/desk/fader") != nullptr);
    CHECK (*rig.mounts.valueOf ("/desk/fader") == osc::Value::float32 (0.75f));
    CHECK (rig.mounts.observedOf ("/desk/fader") == nullptr);
}

//==============================================================================
/*  GO DOH! AND THE HORIZON'S PRE-SENDS (PRD §3.32, namespace draft §24; D1,
    2026-10-01). A pre-send the GO committed - a block it adopted - is the GO's:
    on a device left to its operator it is neither put back by the Doh nor sent
    again by the horizon or the corrected GO. And the horizon's preparation of
    the NEXT scene, made after the GO, is nobody's GO: the Doh gives it back the
    way the pointer moving away does - its pre-send put back - and never takes
    it down. Each case failed before D1, where `go.doh` was an unknown command. */
namespace
{
    std::size_t recordsOf (VerifiedRig& rig, const char* command, const std::string& first)
    {
        const auto records = LogFile::parse (rig.engine.log().contents()).records;

        return static_cast<std::size_t> (std::count_if (records.begin(), records.end(),
                                                        [&] (const auto& record)
                                                        {
                                                            return record.command == command && ! record.args.empty()
                                                                     && record.args[0].isString()
                                                                     && record.args[0].getString() == first;
                                                        }));
    }

    void ticksOf (VerifiedRig& rig, int count)
    {
        for (int n = 0; n < count; ++n)
        {
            rig.tickOnce();
            std::this_thread::sleep_for (std::chrono::milliseconds (2));
        }
    }
}

TEST_CASE ("go.doh: what the GO committed to a device left to its operator is neither put back nor pre-sent again")
{
    VerifiedRig rig;
    rig.anticipate();
    rig.device.target.says ({ osc::Value::float32 (0.2f) });
    REQUIRE (rig.document.setAttribute ("/godot/list/goDebounce", "0").ok);

    /*  The console, declared in the show as well as mounted: Go Doh!'s setting
        is the document's. Said nothing about, so left to its operator. */
    REQUIRE (rig.document.createMount ("/desk", "namespaces/desk.json", "K3PV7WRB").ok);

    const PreparedScene scene { rig, "f:0.8", "none" };

    //  What holds the scene open, and is never heard: a line two seconds in.
    const auto hold = rig.document.createCue (scene.group, 0, "memo", "Hold").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + hold + "/preWait", "2").ok);

    auto takeBack = false;

    SUBCASE ("pre-sent by the horizon, committed by the GO")
    {
        rig.setStandby (scene.group);
        tickUntilDeskHolds (rig, 0.8f);
        REQUIRE (rig.sender.sentFor ("K3PV7WRB") == 1u);

        rig.engine.submit ("cli", "go", {});
        rig.tickOnce();
    }

    SUBCASE ("entered cold: the GO wrote it at entry")
    {
        rig.engine.submit ("cli", "standby.set", { osc::Value::string (scene.group) });
        rig.engine.submit ("cli", "go", {});
        rig.tickOnce();
        tickUntilDeskHolds (rig, 0.8f);
    }

    SUBCASE ("entered cold, on a console that takes back: pre-sent again by the horizon")
    {
        takeBack = true;
        REQUIRE (rig.document.setAttribute ("/godot/mount/K3PV7WRB/doh", "takeBack").ok);

        rig.engine.submit ("cli", "standby.set", { osc::Value::string (scene.group) });
        rig.engine.submit ("cli", "go", {});
        rig.tickOnce();
        tickUntilDeskHolds (rig, 0.8f);
    }

    ticksOf (rig, 25);                                     // half a second, the hold still waiting
    REQUIRE (rig.sender.sentFor ("K3PV7WRB") == 1u);

    rig.engine.submit ("cli", "go.doh", {});
    rig.tickOnce();
    REQUIRE (rig.document.getAttribute (cue::standbyAddressOf (rig.listId)) == std::optional<std::string> (scene.group));

    /*  The horizon prepares the scene again at D+1 - and, on a console that
        takes back, pre-sends again, a read-back round trip later. */
    for (int n = 0; n < 600; ++n)
    {
        if (rig.runs.preparedRunOf (scene.group) != nullptr
              && (! takeBack || rig.sender.sentFor ("K3PV7WRB") >= 2u))
            break;

        rig.tickOnce();
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }

    ticksOf (rig, 10);

    //  Nothing put back on the console: the GO's own write stays.
    CHECK (recordsOf (rig, "node.set", "/desk/fader") == 0u);

    //  The horizon prepared the scene again; the cue left to the console's operator is not in it.
    const auto* again = rig.runs.preparedRunOf (scene.group);
    REQUIRE (again != nullptr);
    CHECK (rig.runs.hasChildFor (again->id, scene.cue) == takeBack);

    rig.engine.submit ("cli", "go", {});
    ticksOf (rig, 10);

    CHECK (rig.sender.sentFor ("K3PV7WRB") == (takeBack ? 2u : 1u));
}

TEST_CASE ("go.doh: the next scene the horizon prepared inside the GO's own act is not the GO's - given back with its pre-send put back")
{
    VerifiedRig rig;
    rig.anticipate();
    rig.device.target.says ({ osc::Value::float32 (0.2f) });
    REQUIRE (rig.document.setAttribute ("/godot/list/goDebounce", "0").ok);

    /*  AN ACT ENTERED BY AN EARLIER GO (the design's red team B, major 3): GO 1
        enters the act on scene one's opening line; the GO a Doh! takes back is
        GO 2, on scene one's second line - so the act and scene one are GO 1's,
        and the next scene's block the horizon makes under the act after GO 2
        is a sibling of GO 2's root, not under it. */
    const auto act = rig.document.createCue (rig.listId, rig.index++, "group", "Act").id;
    const auto first = rig.document.createCue (act, 0, "group", "Scene one").id;
    const auto note = rig.document.createCue (first, 0, "memo", "Opening").id;
    const auto line = rig.document.createCue (first, 1, "memo", "Line").id;

    const auto second = rig.document.createCue (act, 1, "group", "Scene two").id;
    const auto header = rig.document.createRole (second, "header");
    REQUIRE (header.ok);
    const auto position = rig.document.createCue (header.id, 0, "osc", "Position the source").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + position + "/address", "/desk/fader").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + position + "/value", "f:0.8").ok);
    rig.document.createCue (second, 0, "memo", "Next line");
    rig.document.createCue (rig.listId, rig.index++, "memo", "After");

    rig.setStandby (note);
    rig.engine.submit ("cli", "go", {});
    rig.tickOnce();
    REQUIRE (rig.document.getAttribute (cue::standbyAddressOf (rig.listId)) == std::optional<std::string> (line));

    rig.engine.submit ("cli", "go", {});                    // the GO a Doh! will take back: scene one's line
    rig.tickOnce();

    //  The horizon prepares scene two under the act, its header pre-sent.
    tickUntilDeskHolds (rig, 0.8f);
    const auto* block = rig.runs.preparedRunOf (second);
    REQUIRE (block != nullptr);
    const auto blockId = block->id;

    CHECK (block->goSerial == 0u);
    CHECK (block->preparedAfterGo == 2);
    REQUIRE (rig.runs.find (blockId)->parent == rig.runs.liveRunOf (act)->id);

    rig.engine.submit ("cli", "go.doh", {});
    rig.tickOnce();

    //  At once: asked to stop, never taken back, nothing of it ended by the Doh itself.
    CHECK (rig.runs.find (blockId)->state == cue::runState::stopping);
    CHECK_FALSE (rig.runs.find (blockId)->takenBack);

    ticksOf (rig, 5);

    //  At D+1, its own job: the pre-send put back - once - then the block revoked.
    CHECK (recordsOf (rig, "node.set", "/desk/fader") == 1u);
    CHECK (recordsOf (rig, "run.revoke", blockId) == 1u);
    CHECK (rig.runs.find (blockId)->warning == cue::runWarning::revoked);
    REQUIRE (rig.mounts.valueOf ("/desk/fader") != nullptr);
    CHECK (*rig.mounts.valueOf ("/desk/fader") == osc::Value::float32 (0.2f));
}

TEST_CASE ("go.doh: an act the GO entered, nobody heard, the next scene's block under it - given back, its pre-send put back once")
{
    /*  THE DESIGN'S MAIN CASE OF ITS TEST 29: the act is the GO's own root, and
        the next scene's block, which the horizon prepared under it after the
        GO, is not the GO's. The Doh gives the act back the way a preparation is
        given back - nobody heard it - and the block with it; the block's
        pre-send is put back, once, though the act and the block both give it
        back. The first build cleared that restore along with the act's own
        leave pre-sends, as if the GO had committed it: no restore at all, and
        the desk left at the next scene's value. */
    VerifiedRig rig;
    rig.anticipate();
    rig.device.target.says ({ osc::Value::float32 (0.2f) });
    REQUIRE (rig.document.setAttribute ("/godot/list/goDebounce", "0").ok);

    /*  The console declared in the show and left at its default: the horizon's
        preparation of the next scene follows §23.3's rule whatever the setting
        (§24, L34) - given back, its pre-send put back. */
    REQUIRE (rig.document.createMount ("/desk", "namespaces/desk.json", "K3PV7WRB").ok);

    const auto act = rig.document.createCue (rig.listId, rig.index++, "group", "Act").id;
    const auto first = rig.document.createCue (act, 0, "group", "Scene one").id;
    const auto line = rig.document.createCue (first, 0, "memo", "Line").id;

    const auto second = rig.document.createCue (act, 1, "group", "Scene two").id;
    const auto header = rig.document.createRole (second, "header");
    REQUIRE (header.ok);
    const auto position = rig.document.createCue (header.id, 0, "osc", "Position the source").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + position + "/address", "/desk/fader").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + position + "/value", "f:0.8").ok);
    rig.document.createCue (second, 0, "memo", "Next line");
    rig.document.createCue (rig.listId, rig.index++, "memo", "After");

    rig.setStandby (line);
    rig.engine.submit ("cli", "go", {});                    // into the act: this GO's own, nothing in it heard
    rig.tickOnce();

    const auto* actRun = rig.runs.liveRunOf (act);
    REQUIRE (actRun != nullptr);
    const auto actId = actRun->id;
    CHECK (actRun->goSerial == 1u);
    CHECK (actRun->parent.empty());

    //  The horizon prepares scene two under the act, its header pre-sent.
    tickUntilDeskHolds (rig, 0.8f);
    const auto* block = rig.runs.preparedRunOf (second);
    REQUIRE (block != nullptr);
    const auto blockId = block->id;
    CHECK (block->goSerial == 0u);
    CHECK (block->preparedAfterGo == 1);
    REQUIRE (block->parent == actId);

    rig.engine.submit ("cli", "go.doh", {});
    rig.tickOnce();

    CHECK (rig.runs.find (actId)->takenBack);
    CHECK_FALSE (rig.runs.find (blockId)->takenBack);

    ticksOf (rig, 10);

    CHECK (recordsOf (rig, "node.set", "/desk/fader") == 1u);
    CHECK (rig.runs.find (blockId)->warning == cue::runWarning::revoked);
    CHECK (rig.runs.find (actId)->warning == cue::runWarning::revoked);
    REQUIRE (rig.mounts.valueOf ("/desk/fader") != nullptr);
    CHECK (*rig.mounts.valueOf ("/desk/fader") == osc::Value::float32 (0.2f));
}

TEST_CASE ("go.doh: an unheard scene's pre-send that takes back is put back before the horizon prepares the scene again, however long the scene's own fade runs")
{
    /*  THE GIVE-BACK'S RESTORE GOES OUT AT ONCE (§24.2): the scene nobody heard
        is given back the way a preparation is - but the first build put its
        pre-sends back only once everything still moving under it had finished,
        a fade it had fired on a cue outside it among them. The horizon had
        meanwhile prepared the scene again for the restored pointer and
        pre-sent it, and the late restore then wrote the value from before the
        GO over the corrected GO's own: the desk ended where the scene was not. */
    VerifiedRig rig;
    rig.anticipate();
    rig.device.target.says ({ osc::Value::float32 (0.2f) });
    REQUIRE (rig.document.setAttribute ("/godot/list/goDebounce", "0").ok);
    REQUIRE (rig.document.createMount ("/desk", "namespaces/desk.json", "K3PV7WRB").ok);
    REQUIRE (rig.document.setAttribute ("/godot/mount/K3PV7WRB/doh", "takeBack").ok);

    //  Something outside the scene for its fade to move: a timeline that holds for a minute.
    const auto bed = rig.document.createCue (rig.listId, rig.index++, "group", "Bed").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + bed + "/mode", "timeline").ok);
    const auto hum = rig.document.createCue (bed, 0, "memo", "Hum").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + hum + "/preWait", "60").ok);

    const PreparedScene scene { rig, "f:0.8", "none" };
    const auto dip = rig.document.createCue (scene.group, 0, "fade", "Dip the bed").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + dip + "/target", bed).ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + dip + "/level", "-20").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + dip + "/duration", "3").ok);
    rig.document.createCue (scene.group, 1, "memo", "Hold");

    rig.fire (bed);
    rig.setStandby (scene.group);
    tickUntilDeskHolds (rig, 0.8f);
    REQUIRE (rig.runs.preparedRunOf (scene.group) != nullptr);
    const auto block = rig.runs.preparedRunOf (scene.group)->id;

    rig.engine.submit ("cli", "go", {});                    // too early: the block adopted, the dip begun
    rig.tickOnce();
    ticksOf (rig, 25);

    rig.engine.submit ("cli", "go.doh", {});
    rig.tickOnce();
    REQUIRE (rig.runs.find (block)->takenBack);

    //  The scene prepared again for the restored pointer, its header pre-sent afresh.
    const auto freshPreSent = [&rig, &scene, &block]
    {
        const auto* again = rig.runs.preparedRunOf (scene.group);

        if (again == nullptr || again->id == block)
            return false;

        for (const auto* child : rig.runs.childrenOf (again->id))
            if (child->cue == scene.cue && child->isFinished())
                return true;

        return false;
    };

    for (int n = 0; n < 600 && ! freshPreSent(); ++n)
    {
        rig.tickOnce();
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }

    REQUIRE (freshPreSent());

    //  The corrected GO while the old dip is still moving; the old scene goes after it.
    rig.engine.submit ("cli", "go", {});
    rig.tickOnce();
    ticksOf (rig, 200);

    REQUIRE (rig.runs.find (block)->isFinished());
    REQUIRE (rig.mounts.valueOf ("/desk/fader") != nullptr);
    CHECK (*rig.mounts.valueOf ("/desk/fader") == osc::Value::float32 (0.8f));
}

TEST_CASE ("go.doh: a pre-send whose read-back had not come back by the Doh is not counted as left, and is pre-sent again")
{
    /*  WHAT LEFT IS DECIDED AT THE SEND (§24, HQ), and a pre-send asks before
        it writes: one still waiting for its answer at the Doh may never have
        written, so it is not counted as left with the desk's operator - the
        Doh ends it, its late answer writes nothing, and the horizon pre-sends
        the cue again for the restored pointer. Counted, the cue would have been
        left out of the new preparation and sent nothing at the corrected GO:
        the desk would never have had it. A net: the rule was right, and no case
        reached it. */
    VerifiedRig rig;
    rig.anticipate();
    rig.device.target.says ({ osc::Value::float32 (0.2f) });
    REQUIRE (rig.document.setAttribute ("/godot/list/goDebounce", "0").ok);
    REQUIRE (rig.document.createMount ("/desk", "namespaces/desk.json", "K3PV7WRB").ok);

    const PreparedScene scene { rig, "f:0.8", "none" };
    const auto hold = rig.document.createCue (scene.group, 0, "memo", "Hold").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + hold + "/preWait", "2").ok);

    //  A slow device: the pre-send's question is still out at the GO and at the Doh.
    rig.device.target.holding = true;

    rig.setStandby (scene.group);
    ticksOf (rig, 10);
    REQUIRE (rig.sender.sentFor ("K3PV7WRB") == 0u);

    rig.engine.submit ("cli", "go", {});
    rig.tickOnce();
    rig.engine.submit ("cli", "go.doh", {});
    rig.tickOnce();

    rig.device.target.holding = false;

    //  The horizon prepares the scene again, and its pre-send asks and writes.
    for (int n = 0; n < 600 && rig.sender.sentFor ("K3PV7WRB") < 1u; ++n)
    {
        rig.tickOnce();
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }

    REQUIRE (rig.sender.sentFor ("K3PV7WRB") == 1u);
    ticksOf (rig, 10);

    rig.engine.submit ("cli", "go", {});
    ticksOf (rig, 10);

    CHECK (rig.sender.sentFor ("K3PV7WRB") == 1u);
    REQUIRE (rig.mounts.valueOf ("/desk/fader") != nullptr);
    CHECK (*rig.mounts.valueOf ("/desk/fader") == osc::Value::float32 (0.8f));
}

//==============================================================================
/*  A SCENE MADE READY UNDER AN ACT THAT IS RUNNING (2026-10-01, namespace draft
    §23.9, J2). The pointer on the row of a scene that plays itself, inside an
    act already running: the horizon prepares the scene under the act, its
    header pre-sent to the desk. The GO on that row now adopts that block, as a
    GO on a scene at the top of a list always has; until J2 it spawned a second
    run of the scene beside it, which entered cold and sent its header a second
    time, while the block kept its pre-send. And the pointer moving away gave
    back only a block at the top of a list, so a scene made ready under an act
    held its desk value for as long as the act ran, and after. */
namespace
{
    /*  An act - a manual group - of two scenes that play themselves and a line
        after them, the second scene's header pre-sending the desk's fader and
        a line two seconds in holding it open; and a line after the act. */
    struct SceneInAnAct
    {
        explicit SceneInAnAct (VerifiedRig& rig)
        {
            act = rig.document.createCue (rig.listId, rig.index++, "group", "Act").id;

            first = rig.document.createCue (act, 0, "group", "Scene one").id;
            REQUIRE (rig.document.setAttribute ("/godot/cue/" + first + "/advance", "auto").ok);
            rig.document.createCue (first, 0, "memo", "Opening");

            scene = rig.document.createCue (act, 1, "group", "Scene two").id;
            REQUIRE (rig.document.setAttribute ("/godot/cue/" + scene + "/advance", "auto").ok);

            const auto header = rig.document.createRole (scene, "header");
            REQUIRE (header.ok);
            presend = rig.document.createCue (header.id, 0, "osc", "Position the source").id;

            const auto base = "/godot/cue/" + presend + "/";
            REQUIRE (rig.document.setAttribute (base + "address", "/desk/fader").ok);
            REQUIRE (rig.document.setAttribute (base + "value", "f:0.8").ok);
            REQUIRE (rig.document.setAttribute (base + "wait", "none").ok);
            REQUIRE (rig.document.setAttribute (base + "timeout", "5").ok);

            const auto line = rig.document.createCue (scene, 0, "memo", "Line").id;
            REQUIRE (rig.document.setAttribute ("/godot/cue/" + line + "/preWait", "2").ok);

            later = rig.document.createCue (act, 2, "memo", "Later").id;
            after = rig.document.createCue (rig.listId, rig.index++, "memo", "After").id;
        }

        /*  GO on scene one's row enters the act, and the walk stands on scene
            two's row - a scene that plays itself is entered whole - where the
            horizon makes it ready under the act. */
        void enter (VerifiedRig& rig) const
        {
            rig.setStandby (first);
            rig.engine.submit ("cli", "go", {});
            rig.tickOnce();

            REQUIRE (rig.document.getAttribute (cue::standbyAddressOf (rig.listId))
                       == std::optional<std::string> (scene));
            REQUIRE (rig.runs.liveRunOf (act) != nullptr);
        }

        std::string act, first, scene, presend, later, after;
    };

    std::size_t runsOf (VerifiedRig& rig, const std::string& cueId)
    {
        return static_cast<std::size_t> (std::count_if (rig.runs.all().begin(), rig.runs.all().end(),
                                                        [&cueId] (const cue::Run& run) { return run.cue == cueId; }));
    }
}

TEST_CASE ("prepare: a GO on a scene's row inside a running act adopts the scene made ready there, its header sent once")
{
    VerifiedRig rig;
    rig.anticipate();
    rig.device.target.says ({ osc::Value::float32 (0.2f) });
    REQUIRE (rig.document.setAttribute ("/godot/list/goDebounce", "0").ok);

    const SceneInAnAct shape { rig };
    shape.enter (rig);

    //  The horizon makes scene two ready under the act, its header pre-sent.
    tickUntilDeskHolds (rig, 0.8f);
    REQUIRE (rig.mounts.valueOf ("/desk/fader") != nullptr);
    REQUIRE (*rig.mounts.valueOf ("/desk/fader") == osc::Value::float32 (0.8f));

    const auto* ready = rig.runs.preparedRunOf (shape.scene);
    REQUIRE (ready != nullptr);
    const auto block = ready->id;
    const auto actRun = rig.runs.liveRunOf (shape.act)->id;
    REQUIRE (rig.runs.find (block)->parent == actRun);
    REQUIRE (rig.sender.sentFor ("K3PV7WRB") == 1u);

    rig.engine.submit ("cli", "go", {});                    // scene two's row
    rig.tickOnce();
    ticksOf (rig, 10);

    //  Adopted: the same run, playing under the act, and no second run of the scene beside it.
    const auto* live = rig.runs.liveRunOf (shape.scene);
    REQUIRE (live != nullptr);
    CHECK (live->id == block);
    CHECK (live->parent == actRun);
    CHECK (runsOf (rig, shape.scene) == 1u);

    //  Its header's pre-send was its execution: not run again, not sent again, never put back.
    CHECK (runsOf (rig, shape.presend) == 1u);
    CHECK (rig.sender.sentFor ("K3PV7WRB") == 1u);
    CHECK (recordsOf (rig, "node.set", "/desk/fader") == 0u);
    REQUIRE (rig.mounts.valueOf ("/desk/fader") != nullptr);
    CHECK (*rig.mounts.valueOf ("/desk/fader") == osc::Value::float32 (0.8f));
}

TEST_CASE ("prepare: the pointer leaving a scene made ready inside a running act gives it back, the desk put back first")
{
    for (const auto* where : { "a later line of the act", "a line after the act" })
    {
        INFO (std::string (where));
        VerifiedRig rig;
        rig.anticipate();
        rig.device.target.says ({ osc::Value::float32 (0.2f) });
        REQUIRE (rig.document.setAttribute ("/godot/list/goDebounce", "0").ok);

        const SceneInAnAct shape { rig };
        shape.enter (rig);

        tickUntilDeskHolds (rig, 0.8f);
        REQUIRE (rig.mounts.valueOf ("/desk/fader") != nullptr);
        REQUIRE (*rig.mounts.valueOf ("/desk/fader") == osc::Value::float32 (0.8f));

        const auto* ready = rig.runs.preparedRunOf (shape.scene);
        REQUIRE (ready != nullptr);
        const auto block = ready->id;

        //  Away, without a GO.
        rig.setStandby (std::string (where) == "a line after the act" ? shape.after : shape.later);

        for (int n = 0; n < 5; ++n)
            rig.tickOnce();

        REQUIRE (rig.mounts.valueOf ("/desk/fader") != nullptr);
        CHECK (*rig.mounts.valueOf ("/desk/fader") == osc::Value::float32 (0.2f));
        CHECK (rig.runs.find (block)->isFinished());
        CHECK (rig.runs.find (block)->warning == cue::runWarning::revoked);

        //  PUT BACK FIRST, then revoked; and the act, which is running, left alone.
        const auto restored = recordIndex (rig, "node.set", "/desk/fader");
        const auto revoked = recordIndex (rig, "run.revoke", block);
        const auto end = LogFile::parse (rig.engine.log().contents()).records.size();

        CHECK (restored < end);
        CHECK (revoked < end);
        CHECK (restored < revoked);
        CHECK (rig.runs.liveRunOf (shape.act) != nullptr);
    }
}

TEST_CASE ("prepare: a scene adopted under a running act whose header settles in the tick after the GO stays adopted")
{
    /*  THE MARK IS WHAT THE GIVE-BACK READS, so an adopted block must never get
        it back. A block the GO adopts under a running act keeps `preparing`
        until the act's job launches it, a tick later - and its own job, finding
        its header settled in that same tick, marked it prepared again: the
        pointer had moved on, so the give-back took the GO's own scene back and
        put its desk value back with it. The desk's answer is given by hand
        here, so the header settles in exactly that tick. */
    VerifiedRig rig;
    rig.anticipate();
    rig.runner.setMounts (&rig.mounts, &rig.sender, nullptr);
    REQUIRE (rig.document.setAttribute ("/godot/list/goDebounce", "0").ok);

    const SceneInAnAct shape { rig };
    shape.enter (rig);

    //  The horizon has made scene two ready under the act; its pre-send is asking what the desk holds.
    for (int n = 0; n < 5; ++n)
        rig.tickOnce();

    const auto* ready = rig.runs.preparedRunOf (shape.scene);
    REQUIRE (ready != nullptr);
    const auto block = ready->id;
    REQUIRE (rig.runOf (shape.presend) != nullptr);
    REQUIRE (rig.runOf (shape.presend)->restoreAtom.empty());

    //  The answer lands; the pre-send writes, and ends in the drain that applies the GO.
    rig.engine.submit ("mount:K3PV7WRB", "mount.readback",
                       { osc::Value::string ("K3PV7WRB"), osc::Value::string ("/desk/fader"),
                         osc::Value::float32 (0.2f) });
    rig.tickOnce();

    /*  THE TIMING THIS CASE IS ABOUT, pinned: the pre-send still out when the
        GO is pressed, and ended in the GO's own drain - the block adopted and
        waiting for the act's job, its mark cleared - so its job settles it a
        tick after the adoption. A pre-send ending a tick earlier would settle
        the block before the GO, and the case would pass without reaching IA. */
    REQUIRE_FALSE (rig.runOf (shape.presend)->isFinished());

    rig.engine.submit ("cli", "go", {});                    // scene two's row
    rig.tickOnce();
    REQUIRE (rig.runOf (shape.presend)->isFinished());
    REQUIRE (rig.runs.find (block)->state == cue::runState::preparing);
    REQUIRE (rig.runs.find (block)->prepare.empty());

    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    const auto* live = rig.runs.liveRunOf (shape.scene);
    REQUIRE (live != nullptr);
    CHECK (live->id == block);
    CHECK (rig.runs.find (block)->warning != cue::runWarning::revoked);
    CHECK (recordsOf (rig, "node.set", "/desk/fader") == 0u);
    REQUIRE (rig.mounts.valueOf ("/desk/fader") != nullptr);
    CHECK (*rig.mounts.valueOf ("/desk/fader") == osc::Value::float32 (0.8f));
}

/*  THE SAME SCENE AS THE ACT'S FIRST, THE GO ENTERING THE ACT ON ITS ROW
    (2026-10-01, namespace draft §23.9, IC). The act has not been entered: the
    horizon makes the act ready and the scene inside it, the scene's header
    pre-sent; the GO enters the act by adopting its block, and the act's header
    - a line the horizon could not take ahead - runs before its members. The
    scene's own block waited, marked, for those members to ask for it, and the
    pointer the GO had moved on gave it back in the meantime: the desk put back
    under the GO that had sent it, and the scene, revoked, never played. */
TEST_CASE ("prepare: a GO entering an act on its first scene's row plays the scene made ready there, the desk left as it was sent")
{
    VerifiedRig rig;
    rig.anticipate();
    rig.device.target.says ({ osc::Value::float32 (0.2f) });
    REQUIRE (rig.document.setAttribute ("/godot/list/goDebounce", "0").ok);

    const auto act = rig.document.createCue (rig.listId, rig.index++, "group", "Act").id;
    const auto actHeader = rig.document.createRole (act, "header");
    REQUIRE (actHeader.ok);
    const auto opening = rig.document.createCue (actHeader.id, 0, "memo", "House to half").id;

    const auto scene = rig.document.createCue (act, 0, "group", "Scene").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + scene + "/advance", "auto").ok);

    const auto header = rig.document.createRole (scene, "header");
    REQUIRE (header.ok);
    const auto presend = rig.document.createCue (header.id, 0, "osc", "Position the source").id;

    const auto base = "/godot/cue/" + presend + "/";
    REQUIRE (rig.document.setAttribute (base + "address", "/desk/fader").ok);
    REQUIRE (rig.document.setAttribute (base + "value", "f:0.8").ok);
    REQUIRE (rig.document.setAttribute (base + "wait", "none").ok);
    REQUIRE (rig.document.setAttribute (base + "timeout", "5").ok);

    const auto line = rig.document.createCue (scene, 0, "memo", "Line").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + line + "/preWait", "2").ok);

    const auto later = rig.document.createCue (act, 1, "memo", "Later").id;
    rig.document.createCue (rig.listId, rig.index++, "memo", "After");

    rig.setStandby (scene);
    tickUntilDeskHolds (rig, 0.8f);
    REQUIRE (rig.mounts.valueOf ("/desk/fader") != nullptr);
    REQUIRE (*rig.mounts.valueOf ("/desk/fader") == osc::Value::float32 (0.8f));

    const auto* actReady = rig.runs.preparedRunOf (act);
    const auto* ready = rig.runs.preparedRunOf (scene);
    REQUIRE (actReady != nullptr);
    REQUIRE (ready != nullptr);
    const auto actBlock = actReady->id;
    const auto block = ready->id;
    REQUIRE (rig.runs.find (block)->parent == actBlock);
    REQUIRE (rig.sender.sentFor ("K3PV7WRB") == 1u);

    rig.engine.submit ("cli", "go", {});                    // the scene's row, entering the act
    rig.tickOnce();
    REQUIRE (rig.document.getAttribute (cue::standbyAddressOf (rig.listId))
               == std::optional<std::string> (later));
    ticksOf (rig, 10);

    //  The act's header ran first; the scene is the block, under the act's, and nothing was put back.
    CHECK (runsOf (rig, opening) == 1u);

    const auto* live = rig.runs.liveRunOf (scene);
    REQUIRE (live != nullptr);
    CHECK (live->id == block);
    CHECK (live->parent == actBlock);
    CHECK (rig.runs.find (block)->warning != cue::runWarning::revoked);
    CHECK (recordsOf (rig, "run.revoke", block) == 0u);
    CHECK (runsOf (rig, scene) == 1u);
    CHECK (runsOf (rig, presend) == 1u);
    CHECK (rig.sender.sentFor ("K3PV7WRB") == 1u);
    CHECK (recordsOf (rig, "node.set", "/desk/fader") == 0u);
    REQUIRE (rig.mounts.valueOf ("/desk/fader") != nullptr);
    CHECK (*rig.mounts.valueOf ("/desk/fader") == osc::Value::float32 (0.8f));

    //  And it plays: its line comes, under the block.
    for (int n = 0; n < 200 && rig.runOf (line) == nullptr; ++n)
        rig.tickOnce();

    REQUIRE (rig.runOf (line) != nullptr);
    CHECK (rig.runOf (line)->parent == block);
}
