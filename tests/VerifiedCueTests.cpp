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
#include <chrono>
#include <memory>
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
            published = std::make_shared<const tree::TreeSnapshot> (1, nodes,
                                                                    std::vector<tree::Node> {});
        }

        std::shared_ptr<const tree::TreeSnapshot> snapshot() const override
        {
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

            tree::MountDeclaration mount;
            mount.id = "K3PV7WRB";
            mount.prefix = "/desk";
            mount.namespaceFile = "namespaces/desk.json";
            mount.host = "127.0.0.1";
            mount.port = socket.boundPort();
            mount.readback = "oscquery";
            mount.queryPort = device.port();

            REQUIRE (mounts.load (mount, deskJson).ok);

            sender.setSocket (socket);

            engine.log().openInMemory ({});
            doc::registerDocumentCommands (engine.commands(), document);
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
        machine somebody is sitting at when they have time to fix it. */
    doc::ShowDocument document;

    const auto listId = document.createList ("Cues").id;

    juce::ValueTree mounts { "Mounts" };
    juce::ValueTree mount { "Mount" };
    mount.setProperty (juce::Identifier ("id"), "K3PV7WRB", nullptr);
    mount.setProperty (juce::Identifier ("prefix"), "/desk", nullptr);
    mount.setProperty (juce::Identifier ("namespace"), "namespaces/desk.json", nullptr);
    mount.setProperty (juce::Identifier ("port"), 9000, nullptr);
    mounts.appendChild (mount, nullptr);

    document.root().appendChild (mounts, nullptr);

    const auto cueId = document.createCue (listId, 0, "osc", "Desk").id;
    document.setAttribute ("/godot/cue/" + cueId + "/address", "/desk/fader");
    document.setAttribute ("/godot/cue/" + cueId + "/value", "f:0.5");

    SUBCASE ("a mount that declares no readback")
    {
        document.setAttribute ("/godot/cue/" + cueId + "/wait", "verified");

        const auto problems = tree::checkNetworkCues (document);

        REQUIRE (problems.size() == 1u);
        INFO (problems.front());
        CHECK (problems.front().find ("readback") != std::string::npos);
        CHECK (problems.front().find (cueId) != std::string::npos);
    }

    SUBCASE ("an address under no mount at all")
    {
        document.setAttribute ("/godot/cue/" + cueId + "/wait", "verified");
        document.setAttribute ("/godot/cue/" + cueId + "/address", "/nowhere/fader");

        const auto problems = tree::checkNetworkCues (document);

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
        CHECK (tree::checkNetworkCues (document).empty());
    }

    SUBCASE ("a mount that says it can be asked")
    {
        mount.setProperty (juce::Identifier ("readback"), "oscquery", nullptr);
        mount.setProperty (juce::Identifier ("queryPort"), 5005, nullptr);

        document.setAttribute ("/godot/cue/" + cueId + "/wait", "verified");
        CHECK (tree::checkNetworkCues (document).empty());
    }

    SUBCASE ("readback declared but no port to ask on")
    {
        /*  Declaring the mechanism and not where to reach it is the same
            promise unkept, so it is the same refusal. */
        mount.setProperty (juce::Identifier ("readback"), "oscquery", nullptr);

        document.setAttribute ("/godot/cue/" + cueId + "/wait", "verified");
        CHECK (tree::checkNetworkCues (document).size() == 1u);
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

    /*  A device that answers with something else, so the cue can never be
        satisfied and the only way it can end is by running out of patience.
        Ten ticks is a fifth of a second; a cue that had read its timeout as
        zero would have given up on the second one. */
    rig.device.target.says ({ osc::Value::float32 (0.2f) });
    rig.fire (id);

    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    const auto* run = rig.runOf (id);
    REQUIRE (run != nullptr);
    CHECK (run->error != std::string ("timeout"));
    CHECK_FALSE (run->isFinished());
}
