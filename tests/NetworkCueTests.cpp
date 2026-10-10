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
    The moment a mount stops being a stub.

    Phase 1 read somebody else's namespace, published it as nodes, accepted
    writes to it and sent nothing - honestly, and with a comment saying where
    the socket would go. This is that socket, and the cue kind that uses it.

    WHAT THESE CASES ARE REALLY ABOUT. Not "does a datagram arrive": that is one
    line and it is the least interesting property here. They are about WHEN it
    leaves and WHAT THE SHOW IS TOLD ABOUT IT - the three-valued wait of PRD
    §3.11 is the whole reason a network cue is a cue rather than a message, and
    a `sent` that cannot report a failure is `none` with extra ceremony.

    Everything binds port 0 and reads the port back, which is the rule for this
    suite: a fixed number makes a test that cannot run twice at once, and ctest
    runs this in parallel with itself under two locales.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/cue/CurveCommands.h>
#include <wfg/engine/cue/CurveTable.h>
#include <wfg/engine/tree/PresetTable.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/LevelLane.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/log/Replay.h>
#include <wfg/engine/osc/OscCodec.h>
#include <wfg/engine/osc/UdpEndpoint.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/MountSender.h>
#include <wfg/engine/tree/TreeCommands.h>

#include "TestSupport.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace wfg;

namespace
{
    /*  A namespace with three nodes: one writable float, one writable integer
        and one read-only. Hand-written rather than captured, so a refusal has
        something to refuse. (2026-10-03, Doh! D3: and a fourth, an event - a
        write with nothing to read back, which a Doh cannot put back.) */
    constexpr const char* consoleJson = R"JSON({
      "FULL_PATH": "/",
      "CONTENTS": {
        "fader": { "FULL_PATH": "/fader", "TYPE": "f", "ACCESS": 3 },
        "scene": { "FULL_PATH": "/scene", "TYPE": "i", "ACCESS": 3 },
        "meter": { "FULL_PATH": "/meter", "TYPE": "f", "ACCESS": 1 },
        "go": { "FULL_PATH": "/go", "TYPE": "i", "ACCESS": 2 }
      }
    })JSON";

    tree::MountDeclaration consoleMount (int port)
    {
        tree::MountDeclaration mount;
        mount.id = "K3PV7WRB";
        mount.prefix = "/desk";
        mount.namespaceFile = "namespaces/desk.json";
        mount.host = "127.0.0.1";
        mount.port = port;
        return mount;
    }

    /*  A socket that keeps what it was sent, so a case can ask what arrived
        rather than only whether something did. */
    struct Listener
    {
        Listener()
        {
            const auto started = endpoint.start (0, [this] (osc::Datagram datagram)
                                                    {
                                                        const std::lock_guard<std::mutex> lock { guard };
                                                        received.push_back (std::move (datagram));
                                                    });
            REQUIRE (started);
            REQUIRE (endpoint.boundPort() > 0);
        }

        ~Listener() { endpoint.stop(); }

        int port() const { return endpoint.boundPort(); }

        std::size_t count() const
        {
            const std::lock_guard<std::mutex> lock { guard };
            return received.size();
        }

        /*  WAITS FOR A COUNT RATHER THAN SLEEPING FOR A DURATION. The datagram
            crosses a real socket and lands on the endpoint's own receive
            thread, so how long it takes is the operating system's business.
            A fixed sleep is either slow or flaky and usually both. */
        bool waitFor (std::size_t howMany, int millisecondsAtMost = 4000)
        {
            const auto deadline = std::chrono::steady_clock::now()
                                    + std::chrono::milliseconds (millisecondsAtMost);

            while (std::chrono::steady_clock::now() < deadline)
            {
                if (count() >= howMany)
                    return true;

                std::this_thread::sleep_for (std::chrono::milliseconds (2));
            }

            return count() >= howMany;
        }

        std::vector<osc::Datagram> all() const
        {
            const std::lock_guard<std::mutex> lock { guard };
            return received;
        }

        mutable std::mutex guard;
        std::vector<osc::Datagram> received;
        osc::UdpEndpoint endpoint;
    };
}

//==============================================================================
TEST_CASE ("mount sender: nothing leaves until the tick ends")
{
    /*  THE PROPERTY THE WHOLE DESIGN RESTS ON. A write does not reach a socket
        where it happens - it is queued, and the queue drains once, at the end
        of the tick, so every message belonging to one GO leaves together
        (PRD §3.4). A sender that sent at the point of the write would spread
        one cue's messages across whatever order its commands arrived in. */
    Listener listener;
    tree::MountSender sender { listener.endpoint };

    const tree::MountSender::Destination to { "127.0.0.1", listener.port() };

    const auto ticket = sender.queue ("K3PV7WRB", to, "/desk/fader", osc::Value::float32 (0.5f));

    CHECK (sender.pending() == 1u);
    CHECK (sender.outcomeOf (ticket) == tree::MountSender::Outcome::pending);
    CHECK (listener.count() == 0u);

    sender.flush();

    REQUIRE (listener.waitFor (1));
    CHECK (sender.pending() == 0u);
    CHECK (sender.outcomeOf (ticket) == tree::MountSender::Outcome::sent);
    CHECK (sender.sentFor ("K3PV7WRB") == 1u);
}

TEST_CASE ("mount sender: what arrives is the message, byte for byte")
{
    /*  Compared against what the codec would encode rather than against a hand
        written byte array, and that is the stronger check of the two: the codec
        has its own golden fixtures, so this asserts that the sender put THE
        MESSAGE on the wire and added nothing - no bundle wrapper, no padding of
        its own, no second copy. */
    Listener listener;
    tree::MountSender sender { listener.endpoint };

    sender.queue ("K3PV7WRB", { "127.0.0.1", listener.port() },
                  "/desk/fader", osc::Value::float32 (0.5f));
    sender.flush();

    REQUIRE (listener.waitFor (1));

    std::string error;
    const auto expected = osc::encode (osc::Packet::message ("/desk/fader",
                                                             { osc::Value::float32 (0.5f) }),
                                       error);

    REQUIRE (expected.has_value());

    const auto arrived = listener.all().front();
    CHECK (arrived.bytes == *expected);

    /*  And it decodes back to what was asked for, which is the property a
        console actually depends on. */
    const auto decoded = osc::decode (arrived.bytes.data(), arrived.bytes.size());
    REQUIRE (decoded.ok);
    CHECK (decoded.packet.address == "/desk/fader");
    REQUIRE (decoded.packet.args.size() == 1u);
    CHECK (decoded.packet.args.front() == osc::Value::float32 (0.5f));
}

TEST_CASE ("mount sender: forty writes in a tick are one message, and it is the last one")
{
    /*  COALESCING, which is not an optimisation but the thing that stops a fade
        at fifty a second and a client dragging a fader at four hundred from
        flooding a console. The same property the OSCQuery push side gets for
        free by reading a published snapshot; here it has to be arranged.

        The value is the NEWEST because sending a number somebody has already
        changed their mind about is sending a wrong number. */
    Listener listener;
    tree::MountSender sender { listener.endpoint };

    const tree::MountSender::Destination to { "127.0.0.1", listener.port() };

    for (int i = 0; i < 40; ++i)
        sender.queue ("K3PV7WRB", to, "/desk/fader",
                      osc::Value::float32 (static_cast<float> (i) / 100.0f));

    CHECK (sender.pending() == 1u);

    sender.flush();
    REQUIRE (listener.waitFor (1));

    /*  Given time to be wrong: if a second datagram were coming this is where
        it would arrive, and the count is checked after the wait rather than
        before it. */
    CHECK_FALSE (listener.waitFor (2, 150));
    CHECK (listener.count() == 1u);

    const auto decoded = osc::decode (listener.all().front().bytes.data(),
                                      listener.all().front().bytes.size());
    REQUIRE (decoded.ok);
    CHECK (decoded.packet.args.front() == osc::Value::float32 (0.39f));
}

TEST_CASE ("mount sender: a re-written address keeps its place in the order")
{
    /*  THE OTHER HALF OF COALESCING, and the half that is easy to get wrong. A
        cue that sets a mode and then a parameter OF that mode has to arrive in
        that order. Re-queueing a re-written address at the back of the queue -
        which is what a naive map-then-emit does - would silently reverse it,
        and the failure would be a device in the wrong mode on one show in ten.

        So: the value is the newest, the position is the oldest. */
    Listener listener;
    tree::MountSender sender { listener.endpoint };

    const tree::MountSender::Destination to { "127.0.0.1", listener.port() };

    sender.queue ("K3PV7WRB", to, "/desk/scene", osc::Value::int32 (1));
    sender.queue ("K3PV7WRB", to, "/desk/fader", osc::Value::float32 (0.1f));
    sender.queue ("K3PV7WRB", to, "/desk/scene", osc::Value::int32 (2));

    CHECK (sender.pending() == 2u);
    sender.flush();

    REQUIRE (listener.waitFor (2));

    const auto arrived = listener.all();
    REQUIRE (arrived.size() == 2u);

    const auto first = osc::decode (arrived[0].bytes.data(), arrived[0].bytes.size());
    const auto second = osc::decode (arrived[1].bytes.data(), arrived[1].bytes.size());

    REQUIRE (first.ok);
    REQUIRE (second.ok);

    CHECK (first.packet.address == "/desk/scene");
    CHECK (first.packet.args.front() == osc::Value::int32 (2));
    CHECK (second.packet.address == "/desk/fader");
}

TEST_CASE ("mount sender: a superseded message is answered rather than left waiting")
{
    /*  A ticket nothing will ever flush is a cue that hangs, and a cue that
        hangs is the exact failure a three-valued wait exists to make visible.
        It answers SENT rather than failed: what the caller asked for was that
        this address reach the target on this tick, and it will. */
    Listener listener;
    tree::MountSender sender { listener.endpoint };

    const tree::MountSender::Destination to { "127.0.0.1", listener.port() };

    const auto superseded = sender.queue ("K3PV7WRB", to, "/desk/fader", osc::Value::float32 (0.1f));
    const auto winner = sender.queue ("K3PV7WRB", to, "/desk/fader", osc::Value::float32 (0.2f));

    CHECK (superseded != winner);
    CHECK (sender.outcomeOf (superseded) == tree::MountSender::Outcome::sent);

    sender.flush();
    REQUIRE (listener.waitFor (1));
    CHECK (sender.outcomeOf (winner) == tree::MountSender::Outcome::sent);
}

TEST_CASE ("mount sender: with no socket it still queues, coalesces and answers")
{
    /*  A COMPLETE CONFIGURATION AND NOT A DEGRADED ONE, exactly as a null
        Player is on the audio side. `wfg replay` and `wfg tree` have no socket
        and must still behave the same everywhere the show can observe - only
        the datagram is missing. A sender that crashed or refused without one
        would make a replay a different program. */
    tree::MountSender sender;

    const auto ticket = sender.queue ("K3PV7WRB", { "127.0.0.1", 9000 },
                                      "/desk/fader", osc::Value::float32 (0.5f));

    CHECK (sender.pending() == 1u);
    sender.flush();

    CHECK (sender.pending() == 0u);

    /*  It reports FAILED, and that is the honest answer rather than an awkward
        one: nothing left this machine. A `sent` cue running with no socket has
        not had its guarantee met, and saying so is what stops "it works in
        replay" from meaning "it works". */
    CHECK (sender.outcomeOf (ticket) == tree::MountSender::Outcome::failed);
    CHECK (sender.sentFor ("K3PV7WRB") == 0u);
}

TEST_CASE ("mount sender: a port of zero is a destination that does not exist")
{
    /*  The document refuses this when the show loads, so it should be
        unreachable - but the check is here too, because "unreachable" is a
        claim about code somebody may change and this is the layer that would
        otherwise hand a zero to the operating system and find out. */
    Listener listener;
    tree::MountSender sender { listener.endpoint };

    const auto ticket = sender.queue ("K3PV7WRB", { "127.0.0.1", 0 },
                                      "/desk/fader", osc::Value::float32 (0.5f));
    sender.flush();

    CHECK (sender.outcomeOf (ticket) == tree::MountSender::Outcome::failed);
    CHECK_FALSE (listener.waitFor (1, 150));
}

//==============================================================================
namespace
{
    /*  A show with one mounted console and the cues that write to it. Everything
        below the command is real: a mount table with a parsed namespace, a
        sender on a live loopback socket, and the Runner's own dispatch. */
    struct NetworkRig
    {
        NetworkRig()
            : sender (listener.endpoint)
        {
            REQUIRE (mounts.load (consoleMount (listener.port()), consoleJson).ok);

            engine.log().openInMemory ({});
            doc::registerDocumentCommands (engine.commands(), document, foreignWrite());
            cue::registerCueCommands (engine.commands(), document, focus);
            cue::registerRunCommands (engine.commands(), runs);
            cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);

            runner.setMounts (&mounts, &sender);

            listId = document.createList ("Sound").id;
        }

        /*  The same routing `wfg serve` installs, written here rather than
            reached for, because the point of the callback is that the document
            layer does not know what a mount is - so the knowledge lives at the
            assembly site, and there are two of them. */
        doc::ForeignWrite foreignWrite()
        {
            /*  (2026-10-03, D3: through the door itself, `tree::writeToDevice`,
                rather than a copy of it - so a case about that door tests
                `serve`'s.) */
            return [this] (const std::string& address, const osc::Values& values)
            {
                const auto written = tree::writeToDevice (mounts, sender, address, values);

                if (! written.ok)
                    return Outcome::rejected (written.reason);

                std::vector<osc::Value> applied { osc::Value::string (address) };
                applied.insert (applied.end(), written.values.begin(), written.values.end());
                return Outcome::ok (std::move (applied));
            };
        }

        /** A network cue, written the way a show would write one. */
        std::string makeOsc (const std::string& address, const std::string& atom,
                             const std::string& wait)
        {
            const auto id = document.createCue (listId, index++, "osc", "Desk").id;
            const auto base = "/godot/cue/" + id + "/";

            document.setAttribute (base + "address", address);
            document.setAttribute (base + "value", atom);
            document.setAttribute (base + "wait", wait);
            return id;
        }

        /*  One tick of the real loop, in the order the tick thread runs it:
            the Runner observes and reports, the engine applies, and what the
            tick wrote leaves at the end of it. */
        void tickOnce()
        {
            runner.beforeTick (engine, tick);
            engine.processTick (tick++);
            sender.flush();
        }

        Engine::TickResult fire (const std::string& cueId)
        {
            engine.submit ("cli", "cue.fire", { osc::Value::string (cueId) });

            runner.beforeTick (engine, tick);
            const auto result = engine.processTick (tick++);
            sender.flush();
            return result;
        }

        const cue::Run* runOf (const std::string& cueId) const
        {
            for (const auto& run : runs.all())
                if (run.cue == cueId)
                    return &run;

            return nullptr;
        }

        Listener listener;
        tree::MountTable mounts;
        tree::MountSender sender;

        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::Focus focus;
        doc::IdRegistry runIds = doc::IdRegistry::withSeed (17);
        cue::Runner runner { document, runs, runIds, focus };

        std::string listId;
        int index = 0;
        std::int64_t tick = 0;
    };
}

TEST_CASE ("network cue: firing one writes the node and puts it on the wire")
{
    NetworkRig rig;

    const auto cueId = rig.makeOsc ("/desk/fader", "f:0.75", "none");
    const auto outcome = rig.fire (cueId);

    CHECK (outcome.applied == 1u);

    /*  IT REACHED THE TREE, which is what a client reads back and what a replay
        reproduces... */
    const auto* value = rig.mounts.valueOf ("/desk/fader");
    REQUIRE (value != nullptr);
    CHECK (*value == osc::Values { osc::Value::float32 (0.75f) });

    /*  ...AND IT REACHED THE WIRE, which is what the console hears. The two are
        separate on purpose: a cue that moved one and not the other would be a
        lie in whichever direction somebody happened to look. */
    REQUIRE (rig.listener.waitFor (1));

    const auto arrived = rig.listener.all().front();
    const auto decoded = osc::decode (arrived.bytes.data(), arrived.bytes.size());

    REQUIRE (decoded.ok);
    CHECK (decoded.packet.address == "/desk/fader");
    CHECK (decoded.packet.args.front() == osc::Value::float32 (0.75f));
}

//==============================================================================
/*  A DEVICE THAT IS NOT IN THE ROOM TONIGHT (2026-09-22).

    `tx` off is a rehearsal without the desk. The cue must still RUN - it is a
    legal request, the show is playing, and a designer working at home should
    get the show rather than a column of red - and nothing must leave the
    machine. The warning is what makes the difference visible without making it
    an error.
*/
TEST_CASE ("network cue: with tx off the cue runs and nothing leaves the machine")
{
    NetworkRig rig;

    auto quiet = *rig.mounts.declarationOf ("K3PV7WRB");
    quiet.tx = false;
    REQUIRE (rig.mounts.updateDeclaration (quiet));

    const auto cueId = rig.makeOsc ("/desk/fader", "f:0.75", "sent");
    const auto outcome = rig.fire (cueId);

    CHECK (outcome.applied == 1u);

    /*  IT REACHED THE TREE, exactly as it would have. The value a client reads
        back and the record a replay reproduces are both about what the show
        DECIDED, and turning a device off decides nothing about the cue. */
    const auto* value = rig.mounts.valueOf ("/desk/fader");
    REQUIRE (value != nullptr);
    CHECK (*value == osc::Values { osc::Value::float32 (0.75f) });

    //  AND IT DID NOT REACH THE WIRE.
    rig.tickOnce();
    CHECK (rig.listener.all().empty());

    /*  The run ENDED rather than failing, carrying the word. An error here
        would fill a running pane with red for a decision the operator took
        five minutes ago, and teach them to stop reading the colour that means
        something is actually wrong. */
    const auto* run = rig.runOf (cueId);
    REQUIRE (run != nullptr);
    CHECK (run->state == cue::runState::done);
    CHECK (run->error.empty());
    CHECK (run->warning == std::string (cue::runWarning::notSent));
}

TEST_CASE ("network cue: a verified cue on a silenced device does not sit waiting")
{
    /*  The trap this guards: `verified` asks the device and waits for an
        answer. With nothing sent there is nothing to answer, so without the
        early finish the cue would wait out its whole timeout and then fail -
        a failure produced by a setting rather than by anything in the rig. */
    NetworkRig rig;

    auto quiet = *rig.mounts.declarationOf ("K3PV7WRB");
    quiet.tx = false;
    REQUIRE (rig.mounts.updateDeclaration (quiet));

    const auto cueId = rig.makeOsc ("/desk/fader", "f:0.75", "verified");
    rig.fire (cueId);
    rig.tickOnce();

    const auto* run = rig.runOf (cueId);
    REQUIRE (run != nullptr);
    CHECK (run->state == cue::runState::done);
    CHECK (run->warning == std::string (cue::runWarning::notSent));
}

TEST_CASE ("network cue: turning a device back on sends again")
{
    NetworkRig rig;

    auto quiet = *rig.mounts.declarationOf ("K3PV7WRB");
    quiet.tx = false;
    REQUIRE (rig.mounts.updateDeclaration (quiet));

    rig.fire (rig.makeOsc ("/desk/fader", "f:0.25", "sent"));
    rig.tickOnce();
    REQUIRE (rig.listener.all().empty());

    quiet.tx = true;
    REQUIRE (rig.mounts.updateDeclaration (quiet));

    rig.fire (rig.makeOsc ("/desk/fader", "f:0.75", "sent"));

    /*  Nothing is remembered and nothing is caught up: what was not sent while
        the device was off is not sent when it comes back. A queue of messages
        from a rehearsal arriving at once is the opposite of what an operator
        switching a desk back on wants. */
    REQUIRE (rig.listener.waitFor (1));
    CHECK (rig.listener.all().size() == 1u);
}

TEST_CASE ("network cue: a run of its own, running from the tick it fires")
{
    /*  A network cue is a cue: pressing GO on it is a thing that happened, it
        needs an address while it is in flight, and a group will need it to
        finish (§3.6). And it is `playing` rather than `armed`, because there is
        nothing to arm - no voice to hold, no file to make ready. */
    NetworkRig rig;

    const auto cueId = rig.makeOsc ("/desk/fader", "f:0.5", "sent");
    rig.fire (cueId);

    const auto* run = rig.runOf (cueId);
    REQUIRE (run != nullptr);

    CHECK (run->kind == "osc");
    CHECK (run->state == cue::runState::playing);
    CHECK_FALSE (run->isFinished());
}

TEST_CASE ("network cue: none finishes without asking anything")
{
    /*  The right wait for a target that will never answer - a lighting desk, a
        projector, anything that takes a message and says nothing. It still
        finishes on the tick AFTER the cue fired, because that is when a report
        is allowed to leave, and not because it waited for anything. */
    NetworkRig rig;

    const auto cueId = rig.makeOsc ("/desk/fader", "f:0.5", "none");
    rig.fire (cueId);

    REQUIRE_FALSE (rig.runOf (cueId)->isFinished());

    rig.tickOnce();

    const auto* run = rig.runOf (cueId);
    CHECK (run->state == cue::runState::done);
    CHECK (run->error.empty());
}

TEST_CASE ("network cue: sent finishes when the datagram has left")
{
    NetworkRig rig;

    const auto cueId = rig.makeOsc ("/desk/scene", "i:4", "sent");
    rig.fire (cueId);

    REQUIRE_FALSE (rig.runOf (cueId)->isFinished());
    REQUIRE (rig.listener.waitFor (1));

    rig.tickOnce();

    CHECK (rig.runOf (cueId)->state == cue::runState::done);

    const auto decoded = osc::decode (rig.listener.all().front().bytes.data(),
                                      rig.listener.all().front().bytes.size());
    REQUIRE (decoded.ok);
    CHECK (decoded.packet.args.front() == osc::Value::int32 (4));
}

TEST_CASE ("network cue: sent is the only wait that can report a failure")
{
    /*  AND THAT IS THE WHOLE PRACTICAL DIFFERENCE between the two waits today.
        Both report on the same tick, because the flush happens at the end of
        the tick that queued the message - so `sent` costs nothing in latency.
        What it buys is that a message which did not leave says so, and a
        sequence of cues built on `sent` stops rather than carrying on into a
        scene whose console never heard the first instruction.

        The failure is arranged by taking the socket away, which is the one way
        a send can fail that a test can produce on demand. */
    NetworkRig rig;

    const auto cueId = rig.makeOsc ("/desk/fader", "f:0.5", "sent");

    rig.listener.endpoint.stop();
    rig.sender = tree::MountSender {};              // no socket at all

    rig.fire (cueId);
    rig.tickOnce();

    const auto* run = rig.runOf (cueId);
    REQUIRE (run != nullptr);

    INFO ("state " << run->state << ", error " << run->error);
    CHECK (run->state == cue::runState::failed);
    CHECK (run->error == cue::runError::sendFailed);
}

TEST_CASE ("network cue: the write refusals are the run's failure, and they say which")
{
    /*  A cue naming a node the target does not have, or one it will not take,
        or a value that is not what the node holds. Each is APPLIED - the
        request was legal and the show could not honour it - and each says which
        of the three it was, because "the cue failed" at half past seven is not
        a sentence anybody can act on. */
    NetworkRig rig;

    SUBCASE ("a node the target does not have")
    {
        const auto cueId = rig.makeOsc ("/desk/nosuch", "f:0.5", "none");
        rig.fire (cueId);
        rig.tickOnce();

        CHECK (rig.runOf (cueId)->state == cue::runState::failed);
        CHECK (rig.runOf (cueId)->error == reason::badAddress);
    }

    SUBCASE ("a node that refuses writes")
    {
        /*  A meter is something a console tells you, not something you tell it.
            Mounted access defaults to read, so this is also the ordinary case
            for a captured namespace that did not say. */
        const auto cueId = rig.makeOsc ("/desk/meter", "f:0.5", "none");
        rig.fire (cueId);
        rig.tickOnce();

        CHECK (rig.runOf (cueId)->state == cue::runState::failed);
        CHECK (rig.runOf (cueId)->error == reason::readOnly);
    }

    SUBCASE ("a value the node cannot hold")
    {
        const auto cueId = rig.makeOsc ("/desk/fader", "s:hello", "none");
        rig.fire (cueId);
        rig.tickOnce();

        CHECK (rig.runOf (cueId)->state == cue::runState::failed);
        CHECK (rig.runOf (cueId)->error == reason::typeMismatch);
    }

    SUBCASE ("a value that is not an atom at all")
    {
        const auto cueId = rig.makeOsc ("/desk/fader", "0.5", "none");
        rig.fire (cueId);
        rig.tickOnce();

        CHECK (rig.runOf (cueId)->state == cue::runState::failed);
        CHECK (rig.runOf (cueId)->error == reason::typeMismatch);
    }

    /*  IN EVERY CASE NOTHING WENT OUT. A cue that failed and sent anyway would
        be the worst of the two outcomes. */
    CHECK_FALSE (rig.listener.waitFor (1, 150));
}

TEST_CASE ("network cue: an integer written to a float node goes out as a float")
{
    /*  The mount coerces to what the node declared, and the WIRE gets the
        coerced value rather than the one the document wrote - so a console that
        would have rejected an integer never sees one. The log records the same
        coerced value, which is what makes a replay put identical bytes on the
        wire. */
    NetworkRig rig;

    const auto cueId = rig.makeOsc ("/desk/fader", "i:1", "none");
    rig.fire (cueId);

    REQUIRE (rig.listener.waitFor (1));

    const auto decoded = osc::decode (rig.listener.all().front().bytes.data(),
                                      rig.listener.all().front().bytes.size());
    REQUIRE (decoded.ok);
    CHECK (decoded.packet.args.front() == osc::Value::float32 (1.0f));
}

TEST_CASE ("node.set on a mounted address is the same command, and it sends too")
{
    /*  PRD §4.11: a client reaches the model through named commands and nothing
        else. There is one value-write command and from Phase 2 not every
        address it can be given belongs to the show - so a client writing a
        console's fader and a cue writing it take the same path, produce the
        same log record and are refused in the same words. */
    NetworkRig rig;

    rig.engine.submit ("udp:10.0.0.5:9000", "node.set",
                       { osc::Value::string ("/desk/fader"), osc::Value::float32 (0.25f) });

    rig.tickOnce();

    const auto* value = rig.mounts.valueOf ("/desk/fader");
    REQUIRE (value != nullptr);
    CHECK (*value == osc::Values { osc::Value::float32 (0.25f) });

    REQUIRE (rig.listener.waitFor (1));

    const auto decoded = osc::decode (rig.listener.all().front().bytes.data(),
                                      rig.listener.all().front().bytes.size());
    REQUIRE (decoded.ok);
    CHECK (decoded.packet.address == "/desk/fader");
}

TEST_CASE ("node.set with no mounts anywhere is refused as it always was")
{
    /*  `wfg tree`, `wfg canon` and every document test register these commands
        with no mounts at all, and a foreign address there is exactly what it
        was in Phase 1: an address that resolves to nothing. */
    Engine engine;
    doc::ShowDocument document;

    engine.log().openInMemory ({});
    doc::registerDocumentCommands (engine.commands(), document);

    engine.submit ("cli", "node.set",
                   { osc::Value::string ("/desk/fader"), osc::Value::float32 (0.5f) });

    const auto outcome = engine.processTick (0);
    CHECK (outcome.applied == 0u);
    CHECK (outcome.rejected == 1u);
}

//==============================================================================
/*  WHERE A MOUNT SENDS, CHECKED BEFORE ANYTHING IS SENT.

    This is the one refusal in the PR that costs a mount its whole namespace,
    and it is worth being explicit about why that is proportionate.

    UDP has no way of telling anybody that nobody was listening. A mount with a
    wrong port, or none, loads perfectly well and then every cue aimed at it
    does nothing at all, silently, for the whole of a show - and the operator's
    evidence is a device that is not moving, which looks identical to a device
    that is broken, a cable that is out, and a cue somebody forgot to write.
    There is no later moment when the engine could find out.

    So it is found out at the only moment it can be: when the file is read.

    TWO FENCES, and the outer one turns out to do most of the work. `port` is
    required with no default and ranged 1..65535, so the GRAMMAR refuses a show
    that omits it or writes a nonsense number - the same machinery that makes
    `audio/@tracks` required, reached by putting one row in a table. The check
    inside loadMountFromBundle is the inner fence: unreachable through a valid
    document, and kept because "unreachable" is a claim about code somebody may
    change, and this is the layer that would otherwise hand a zero to the
    operating system and find out.
*/
namespace
{
    /** A scratch copy of the minimal bundle whose show.xml a case can break. */
    juce::File scratchBundleWith (const std::string& find, const std::string& replace)
    {
        const juce::File fixture { juce::String (std::string (WFG_TEST_FIXTURES_DIR))
                                     + "/bundles/minimal" };
        REQUIRE (fixture.isDirectory());

        const auto scratch = juce::File::getSpecialLocation (juce::File::tempDirectory)
                               .getChildFile ("wfg-network-tests")
                               .getChildFile (juce::Uuid().toDashedString())
                               .getChildFile ("minimal");

        REQUIRE (fixture.copyDirectoryTo (scratch));

        const auto showFile = scratch.getChildFile ("show.xml");
        auto text = showFile.loadFileAsString().toStdString();

        REQUIRE (text.find (find) != std::string::npos);

        const auto at = text.find (find);
        text.replace (at, find.size(), replace);

        REQUIRE (showFile.replaceWithText (juce::String (text)));
        return scratch;
    }
}

TEST_CASE ("mount: the grammar refuses a target with nowhere to send")
{
    /*  Required and ranged, so it is the document layer that says no - before
        a mount table exists, before a socket exists, and in the same breath as
        every other thing wrong with the file. */
    SUBCASE ("no port at all")
    {
        const auto bundle = scratchBundleWith (" port=\"8000\"", "");

        doc::ShowDocument document;
        const auto opened = doc::Bundle::open (bundle, document);

        CHECK_FALSE (opened.ok);
        REQUIRE_FALSE (opened.problems.empty());

        INFO (opened.problems.front());
        CHECK (opened.problems.front().find ("port") != std::string::npos);
    }

    SUBCASE ("a port no machine has")
    {
        const auto bundle = scratchBundleWith ("port=\"8000\"", "port=\"70000\"");

        doc::ShowDocument document;
        const auto opened = doc::Bundle::open (bundle, document);

        CHECK_FALSE (opened.ok);
        REQUIRE_FALSE (opened.problems.empty());
        INFO (opened.problems.front());
    }
}

TEST_CASE ("mount: a transport Go.dot cannot speak is refused when the show opens")
{
    /*  `transport` was declared in Phase 1 and read by nobody. A document
        should be able to SAY what a device is before Go.dot can talk to it -
        that is why the enum has three values, and why this is a mount-load
        refusal rather than a grammar one. But a show that says `ws` and gets
        silence would be worse than one that will not open. */
    const auto bundle = scratchBundleWith ("<Mount id=\"G1JS4VWE\"",
                                           "<Mount id=\"G1JS4VWE\" transport=\"ws\"");

    doc::ShowDocument document;
    tree::MountTable mounts;

    REQUIRE (doc::Bundle::open (bundle, document).ok);

    const auto result = tree::loadMountFromBundle (document, mounts, bundle, "G1JS4VWE");

    CHECK_FALSE (result.ok);
    REQUIRE_FALSE (result.problems.empty());

    INFO (result.problems.front());
    CHECK (result.problems.front().find ("transport") != std::string::npos);

    /*  AND IT TOOK THE NAMESPACE WITH IT. A failed load erases what was there,
        deliberately: half a mount whose writes go nowhere is the situation this
        refusal exists to prevent, so leaving nodes behind would defeat it at
        the last step. */
    CHECK (mounts.nodeCount ("G1JS4VWE") == 0u);
    CHECK_FALSE (mounts.isLoaded ("G1JS4VWE"));
}

//==============================================================================
TEST_CASE ("rate cap: a node is not sent faster than its mount allows")
{
    /*  `mount/@rateCap` is a ceiling in hertz on ONE NODE, which is what the
        row says and the only reading that leaves PRD §3.4 intact: a cue that
        moves twelve parameters is twelve DIFFERENT addresses and they all leave
        in the same frame, as one gesture. What a cap limits is the same address
        being sent again - a fade running at fifty a second into a desk that
        wants ten.

        Fifty is the default and is exactly the tick rate, so the ordinary mount
        is already capped by the queue's own coalescing and this changes nothing
        for it. Below fifty the newest value waits, in its place in the order,
        until enough flushes have passed.

        Tested against the sender directly rather than through a cue, because
        what is being asserted is how many datagrams left - and that is the
        sender's own count. */
    osc::UdpEndpoint socket;
    REQUIRE (socket.start (0, [] (osc::Datagram) {}));

    Listener listener;
    tree::MountSender sender { socket };

    const tree::MountSender::Destination slow { "127.0.0.1", listener.port(), 10.0 };
    const tree::MountSender::Destination free { "127.0.0.1", listener.port(), 50.0 };

    /*  Ten hertz is one send every five flushes. Twenty flushes with a fresh
        value every time is four sends, not twenty. */
    for (int n = 0; n < 20; ++n)
    {
        sender.queue ("M1", slow, "/ext/console/fader",
                      osc::Value::float32 (static_cast<float> (n) / 20.0f));
        sender.flush();
    }

    CHECK (sender.sentFor ("M1") == 4u);

    /*  AND THE UNCAPPED ONE IS UNTOUCHED, which is what says the cap is doing
        something rather than the queue simply dropping things. */
    for (int n = 0; n < 20; ++n)
    {
        sender.queue ("M2", free, "/ext/console/level",
                      osc::Value::float32 (static_cast<float> (n) / 20.0f));
        sender.flush();
    }

    CHECK (sender.sentFor ("M2") == 20u);
}

TEST_CASE ("rate cap: a message that is waiting is not a message that failed")
{
    /*  A cue whose wait is `sent` asks the sender what happened to its ticket,
        and before the cap `pending` meant "the flush never ran", which is a
        wiring fault. Under a cap it means "queued, in order, holding the newest
        value, and it will go" - so the cue keeps waiting rather than reporting
        a failure about a message that is about to leave. */
    osc::UdpEndpoint socket;
    REQUIRE (socket.start (0, [] (osc::Datagram) {}));

    Listener listener;
    tree::MountSender sender { socket };

    const tree::MountSender::Destination slow { "127.0.0.1", listener.port(), 10.0 };

    const auto first = sender.queue ("M1", slow, "/ext/console/fader",
                                     osc::Value::float32 (0.1f));
    sender.flush();

    CHECK (sender.outcomeOf (first) == tree::MountSender::Outcome::sent);
    CHECK_FALSE (sender.stillQueued (first));

    /*  The next one for that address cannot go yet. */
    const auto held = sender.queue ("M1", slow, "/ext/console/fader",
                                    osc::Value::float32 (0.2f));
    sender.flush();

    CHECK (sender.outcomeOf (held) == tree::MountSender::Outcome::pending);
    CHECK (sender.stillQueued (held));

    /*  And it goes when its turn comes. */
    for (int n = 0; n < 5; ++n)
        sender.flush();

    CHECK (sender.outcomeOf (held) == tree::MountSender::Outcome::sent);
    CHECK_FALSE (sender.stillQueued (held));
    CHECK (sender.sentFor ("M1") == 2u);
}

//==============================================================================
/*  A DOUBLE ESC DROPS WHAT IS STILL WAITING TO LEAVE (2026-10-02, H4, namespace
    draft §23.10). PRD §4.4's immediate level "drops all actions", and a value a
    rate cap is holding back is an action still to come: until H4 it went out on
    its turn, seconds after the press. Every case here but the guards failed on
    the code before H4 - the drop did not exist, and the held value left. */
TEST_CASE ("mount sender: a double Esc drops what is still waiting, and answers each ticket failed")
{
    osc::UdpEndpoint socket;
    REQUIRE (socket.start (0, [] (osc::Datagram) {}));

    Listener listener;
    tree::MountSender sender { socket };

    /*  Two hertz: one send of an address every twenty-five flushes. */
    const tree::MountSender::Destination slow { "127.0.0.1", listener.port(), 2.0 };

    const auto first = sender.queue ("M1", slow, "/ext/console/fader", osc::Value::float32 (0.1f));
    sender.flush();
    REQUIRE (listener.waitFor (1));

    const auto held = sender.queue ("M1", slow, "/ext/console/fader", osc::Value::float32 (0.2f));
    sender.flush();
    REQUIRE (sender.stillQueued (held));

    /*  ANSWERED AT ONCE, so a `sent` wait on it ends rather than hangs - failed,
        and dropped rather than refused by the wire. */
    CHECK (sender.dropQueued() == 1u);
    CHECK (sender.pending() == 0u);
    CHECK_FALSE (sender.stillQueued (held));
    CHECK (sender.outcomeOf (held) == tree::MountSender::Outcome::failed);
    CHECK (sender.wasDropped (held));
    CHECK_FALSE (sender.wasDropped (first));

    SUBCASE ("nothing of it leaves, however long the cap would have held it")
    {
        for (int n = 0; n < 30; ++n)
            sender.flush();

        CHECK (sender.sentFor ("M1") == 1u);
        CHECK_FALSE (listener.waitFor (2, 150));
    }

    SUBCASE ("a value written after the press still waits its turn: the cap is the device's, not the press's")
    {
        const auto after = sender.queue ("M1", slow, "/ext/console/fader", osc::Value::float32 (0.3f));
        sender.flush();

        CHECK (sender.stillQueued (after));
        CHECK (sender.outcomeOf (after) == tree::MountSender::Outcome::pending);
        CHECK_FALSE (sender.wasDropped (after));
    }

    SUBCASE ("what the caller names is kept, in its order")
    {
        /*  The standby's pre-send, which the press leaves ready (§23.3): its
            owner is named and its message goes on its turn. */
        const auto kept = sender.queue ("M1", slow, "/ext/console/fader", osc::Value::float32 (0.4f), "READY001");
        const auto other = sender.queue ("M1", slow, "/ext/console/level", osc::Value::float32 (0.5f), "KILLED01");

        CHECK (sender.dropQueued ([] (const std::string& owner) { return owner == "READY001"; }) == 1u);
        CHECK (sender.stillQueued (kept));
        CHECK_FALSE (sender.wasDropped (kept));
        CHECK (sender.wasDropped (other));
        CHECK (sender.pending() == 1u);
    }
}

TEST_CASE ("double Esc: a value the rate cap is holding never leaves, and the cue waiting on it ends")
{
    NetworkRig rig;

    auto capped = *rig.mounts.declarationOf ("K3PV7WRB");
    capped.rateCap = 2.0;
    REQUIRE (rig.mounts.updateDeclaration (capped));

    rig.fire (rig.makeOsc ("/desk/fader", "f:0.25", "none"));
    REQUIRE (rig.listener.waitFor (1));

    /*  Fired by name, not by GO, so the GO debounce has no say. */
    const auto held = rig.makeOsc ("/desk/fader", "f:0.75", "sent");
    rig.fire (held);
    REQUIRE (rig.sender.pending() == 1u);

    REQUIRE (rig.engine.submit ("cli", "run.killAll", {}));
    rig.tickOnce();

    CHECK (rig.sender.pending() == 0u);

    for (int n = 0; n < 30; ++n)
        rig.tickOnce();

    CHECK_FALSE (rig.listener.waitFor (2, 150));
    CHECK (rig.listener.count() == 1u);
    CHECK (rig.sender.sentFor ("K3PV7WRB") == 1u);

    /*  ENDED, NOT FAILED: the kill is the account (§23.10). */
    const auto* run = rig.runOf (held);
    REQUIRE (run != nullptr);
    CHECK (run->state == cue::runState::done);
    CHECK (run->error.empty());
    CHECK (rig.runner.sends().empty());

    /*  The tree keeps what the cue wrote, and the desk never heard it: the
        world left in a state nobody declared, which §4.4 names as the price. */
    REQUIRE (rig.mounts.valueOf ("/desk/fader") != nullptr);
    CHECK (*rig.mounts.valueOf ("/desk/fader") == osc::Values { osc::Value::float32 (0.75f) });
}

TEST_CASE ("double Esc: a group member's held value is dropped, and the member ends rather than failing")
{
    /*  A MEMBER IS KILLED A TICK AFTER ITS GROUP, by the group's own job - and
        in that tick its job used to read the dropped ticket as a send that
        failed. Being killed from above is the account (§23.10). */
    NetworkRig rig;

    auto capped = *rig.mounts.declarationOf ("K3PV7WRB");
    capped.rateCap = 2.0;
    REQUIRE (rig.mounts.updateDeclaration (capped));

    const auto group = rig.document.createCue (rig.listId, rig.index++, "group", "Scene").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + group + "/advance", "auto").ok);

    const auto member = rig.document.createCue (group, 0, "osc", "Desk").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + member + "/address", "/desk/fader").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + member + "/value", "f:0.75").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + member + "/wait", "sent").ok);

    rig.fire (rig.makeOsc ("/desk/fader", "f:0.25", "none"));
    REQUIRE (rig.listener.waitFor (1));

    rig.fire (group);

    for (int n = 0; n < 20 && rig.sender.pending() == 0u; ++n)
        rig.tickOnce();

    REQUIRE (rig.sender.pending() == 1u);
    REQUIRE (rig.runOf (member) != nullptr);
    REQUIRE (rig.runOf (group) != nullptr);

    const auto memberRun = rig.runOf (member)->id;
    const auto groupRun = rig.runOf (group)->id;

    REQUIRE (rig.engine.submit ("cli", "run.killAll", {}));
    rig.tickOnce();

    CHECK (rig.sender.pending() == 0u);

    for (int n = 0; n < 10 && ! (rig.runs.find (memberRun)->isFinished()
                                  && rig.runs.find (groupRun)->isFinished()); ++n)
        rig.tickOnce();

    INFO ("member " << rig.runs.find (memberRun)->state << ", " << rig.runs.find (memberRun)->error);
    CHECK (rig.runs.find (memberRun)->state == cue::runState::done);
    CHECK (rig.runs.find (memberRun)->error.empty());
    CHECK (rig.runs.find (groupRun)->isFinished());

    for (int n = 0; n < 30; ++n)
        rig.tickOnce();

    CHECK_FALSE (rig.listener.waitFor (2, 150));
    CHECK (rig.listener.count() == 1u);
    CHECK (rig.runner.sends().empty());
}

TEST_CASE ("Esc: a value the rate cap is holding still leaves on its turn")
{
    /*  A GUARD, passing before H4 and after it: Esc is normal completion entered
        early (§4.4), so what was queued goes, and only the double press drops. */
    NetworkRig rig;

    auto capped = *rig.mounts.declarationOf ("K3PV7WRB");
    capped.rateCap = 2.0;
    REQUIRE (rig.mounts.updateDeclaration (capped));

    rig.fire (rig.makeOsc ("/desk/fader", "f:0.25", "none"));
    REQUIRE (rig.listener.waitFor (1));

    const auto held = rig.makeOsc ("/desk/fader", "f:0.75", "sent");
    rig.fire (held);
    REQUIRE (rig.sender.pending() == 1u);

    REQUIRE (rig.engine.submit ("cli", "run.stopAll", {}));
    rig.tickOnce();

    CHECK (rig.sender.pending() == 1u);

    for (int n = 0; n < 30 && rig.sender.pending() > 0u; ++n)
        rig.tickOnce();

    REQUIRE (rig.listener.waitFor (2));

    const auto second = rig.listener.all()[1];
    const auto decoded = osc::decode (second.bytes.data(), second.bytes.size());
    REQUIRE (decoded.ok);
    REQUIRE_FALSE (decoded.packet.args.empty());
    CHECK (decoded.packet.args.front() == osc::Value::float32 (0.75f));

    REQUIRE (rig.runOf (held) != nullptr);
    CHECK (rig.runOf (held)->state == cue::runState::done);
}

//==============================================================================
/*  DOH! AND A DEVICE LEFT TO ITS OPERATOR (PRD §3.32, namespace draft §24;
    the author, 2026-10-01).

    His case: OSC cues to a light board that started sequences, moving heads
    repositioning before being lit - "fixing with Doh would really do more
    damage than the light operator handling the damage". So what reached a
    device is left to its operator unless the device, or the cue, says take
    back: the corrected GO runs such a cue with a first GO's timing and sends
    nothing. Every case here failed on the code before D1, where `go.doh` was
    an unknown command and the rows did not exist. */
namespace
{
    constexpr const char* lightingDesk = "QX7DESK0";

    /*  The network rig, plus the author's lighting desk: OPAQUE - no namespace,
        nothing to read back, never anticipatable - and declared in the show as
        well as mounted, because Doh!'s setting is read from the document. The
        rig's own described console is declared in the show too, at its id. */
    struct DohRig : NetworkRig
    {
        DohRig()
        {
            tree::MountDeclaration desk;
            desk.id = lightingDesk;
            desk.prefix = "/lx";
            desk.host = "127.0.0.1";
            desk.port = listener.port();
            REQUIRE (mounts.declare (desk).ok);

            REQUIRE (document.createMount ("/lx", "", lightingDesk).ok);
            REQUIRE (document.createMount ("/desk", "namespaces/desk.json", "K3PV7WRB").ok);

            /*  Where they listen, as a saved show says it: a show that names a
                device with no port does not read back, and a replay reads the
                show back first. */
            for (const auto* id : { lightingDesk, "K3PV7WRB" })
                REQUIRE (document.setAttribute ("/godot/mount/" + std::string (id) + "/port",
                                                std::to_string (listener.port())).ok);

            /*  A script presses GO, not a hand, as every rig here does. */
            REQUIRE (document.setAttribute ("/godot/list/goDebounce", "0").ok);
        }

        void park (const std::string& cueId)
        {
            REQUIRE (document.setAttribute (cue::standbyAddressOf (listId), cueId).ok);
            tickOnce();
        }

        Engine::TickResult press (const char* command, std::vector<osc::Value> args = {})
        {
            engine.submit ("cli", command, std::move (args));
            runner.beforeTick (engine, tick);
            const auto result = engine.processTick (tick++);
            sender.flush();
            return result;
        }

        std::string standby() const
        {
            return document.getAttribute (cue::standbyAddressOf (listId)).value_or ("");
        }

        /*  How many datagrams for one address have arrived - counted once
            EVERYTHING THE SENDER PUT ON THE WIRE HAS: the sender counts what it
            sends as it sends it, at the tick's flush, and both devices here
            send to this one socket. So a count waits for exactly what left,
            however long the socket's own thread takes, and is never a guess
            made after a fixed sleep - which is either slow or flaky, and here
            would have been both (the Listener's own rule, above). */
        std::size_t received (const std::string& address)
        {
            const auto sent = sender.sentFor (lightingDesk) + sender.sentFor ("K3PV7WRB");
            REQUIRE (listener.waitFor (sent));

            std::size_t count = 0;

            for (const auto& datagram : listener.all())
            {
                const auto decoded = osc::decode (datagram.bytes.data(), datagram.bytes.size());

                if (decoded.ok && decoded.packet.address == address)
                    ++count;
            }

            return count;
        }

        const cue::Run* newestRunOf (const std::string& cueId) const
        {
            const cue::Run* out = nullptr;

            for (const auto& run : runs.all())
                if (run.cue == cueId)
                    out = &run;

            return out;
        }

        void ticks (int count)
        {
            for (int n = 0; n < count; ++n)
                tickOnce();
        }
    };

    /*  THE SESSION AGAIN, FROM ITS LOG ALONE (namespace draft §24.1): what was
        left with a device's operator is decided from the document and the
        records - which device a send went to, which cues a corrected GO sends
        nothing of - never from the sender or the mount table, so a fresh
        engine reading the show and the log reaches the same runs. The warning
        is a hook's readout and is not asked: a replay runs no hook. */
    /*  `withSender` false is `wfg replay`'s own shape: no sender at all, so
        nothing a double Esc drops can have been dropped there (§23.10) - and
        the session must come out the same all the same. */
    void replaysTheSame (DohRig& session, bool withSender = true)
    {
        const auto show = doc::CanonicalXml::write (session.document);
        const auto original = LogFile::parse (session.engine.log().contents());
        REQUIRE (original.errors.empty());

        /*  The plain network rig with the desk on its mount table: the show
            brings its own devices, at the identifiers a DohRig would already
            have reserved. */
        NetworkRig fresh;
        tree::MountDeclaration desk;
        desk.id = lightingDesk;
        desk.prefix = "/lx";
        desk.host = "127.0.0.1";
        desk.port = fresh.listener.port();
        REQUIRE (fresh.mounts.declare (desk).ok);

        if (! withSender)
            fresh.runner.setMounts (&fresh.mounts, nullptr);

        const auto read = doc::CanonicalXml::read (show, fresh.document);

        for (const auto& problem : read.problems)
            MESSAGE (problem);

        REQUIRE (read.ok);

        const auto result = replay (fresh.engine, original);

        for (const auto& mismatch : result.mismatches)
            MESSAGE (mismatch);

        CHECK (result.ok);

        for (const auto& run : session.runs.all())
        {
            INFO ("run " << run.id << " of " << run.cue);
            const auto* again = fresh.runs.find (run.id);
            REQUIRE (again != nullptr);
            CHECK (again->state == run.state);
            CHECK (again->takenBack == run.takenBack);
            CHECK (again->sendsLeft == run.sendsLeft);
            CHECK (again->sentTo == run.sentTo);
            CHECK (again->sendDropped == run.sendDropped);
        }

        /*  And the report's readout, from its record (2026-10-03, D3); and the
            desk, put back by a record too. */
        CHECK (fresh.runner.listState().dohReport().text == session.runner.listState().dohReport().text);

        for (const auto* address : { "/desk/fader", "/desk/go" })
        {
            const auto* was = session.mounts.valueOf (address);
            const auto* now = fresh.mounts.valueOf (address);
            INFO (address);
            CHECK ((was == nullptr) == (now == nullptr));

            if (was != nullptr && now != nullptr)
                CHECK (*was == *now);
        }
    }
}

TEST_CASE ("go.doh: a lighting desk left to its operator - nothing sent again by the corrected GO")
{
    DohRig rig;
    const auto lx = rig.makeOsc ("/lx/go", "i:12", "none");
    const auto after = rig.document.createCue (rig.listId, rig.index++, "memo", "After").id;

    auto preWait = false;

    SUBCASE ("as it is")
    {
    }

    SUBCASE ("with a pre-wait: the corrected GO's run waits it, then sends nothing")
    {
        preWait = true;
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + lx + "/preWait", "1").ok);
    }

    //  Parked through the command, so the replay at the end is given the pointer too.
    REQUIRE (rig.press ("standby.set", { osc::Value::string (lx) }).rejected == 0);
    rig.tickOnce();                                         // and the horizon sees it, as a park does
    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (60);
    REQUIRE (rig.received ("/lx/go") == 1u);

    REQUIRE (rig.press ("go.doh").rejected == 0);
    REQUIRE (rig.standby() == lx);
    rig.ticks (2);
    CHECK (rig.received ("/lx/go") == 1u);

    const auto goTick = rig.tick;
    REQUIRE (rig.press ("go").rejected == 0);
    CHECK (rig.standby() == after);

    /*  A FIRST GO'S TIMING: the run waits its whole second, and only then ends
        having sent nothing. */
    if (preWait)
    {
        const auto* waiting = rig.newestRunOf (lx);
        REQUIRE (waiting != nullptr);
        CHECK (waiting->state == cue::runState::waiting);
        CHECK (waiting->dueTick == goTick + 50);
        CHECK (rig.received ("/lx/go") == 1u);
    }

    rig.ticks (60);

    CHECK (rig.received ("/lx/go") == 1u);

    const auto* again = rig.newestRunOf (lx);
    REQUIRE (again != nullptr);
    CHECK (again->state == cue::runState::done);
    CHECK (again->warning == std::string (cue::runWarning::leftToOperator));
    CHECK (again->error.empty());

    replaysTheSame (rig);
}

TEST_CASE ("go.doh: a verified cue left to its operator ends at once and asks nothing, and a second Doh keeps what was left")
{
    DohRig rig;
    const auto desk = rig.makeOsc ("/desk/fader", "f:0.75", "verified");
    rig.document.createCue (rig.listId, rig.index++, "memo", "After");

    rig.park (desk);
    REQUIRE (rig.press ("go").rejected == 0);
    REQUIRE (rig.listener.waitFor (1));

    REQUIRE (rig.press ("go.doh").rejected == 0);

    SUBCASE ("pressed once")
    {
    }

    SUBCASE ("pressed twice: there is no resume to forget, and nothing to take back")
    {
        rig.ticks (40);
        CHECK (rig.press ("go.doh").rejected == 1);
        CHECK (rig.engine.lastError().find ("nothing-to-take-back") != std::string::npos);
    }

    REQUIRE (rig.press ("go").rejected == 0);
    rig.tickOnce();

    const auto* again = rig.newestRunOf (desk);
    REQUIRE (again != nullptr);
    CHECK (again->state == cue::runState::done);
    CHECK (again->warning == std::string (cue::runWarning::leftToOperator));
    CHECK (rig.received ("/desk/fader") == 1u);
}

TEST_CASE ("go.doh: a device that takes back gets the cue again from the corrected GO")
{
    DohRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/mount/" + std::string (lightingDesk) + "/doh", "takeBack").ok);

    const auto lx = rig.makeOsc ("/lx/go", "i:12", "none");
    rig.document.createCue (rig.listId, rig.index++, "memo", "After");

    rig.park (lx);
    REQUIRE (rig.press ("go").rejected == 0);
    REQUIRE (rig.listener.waitFor (1));

    REQUIRE (rig.press ("go.doh").rejected == 0);
    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (2);

    CHECK (rig.received ("/lx/go") == 2u);

    const auto* again = rig.newestRunOf (lx);
    REQUIRE (again != nullptr);
    CHECK (again->state == cue::runState::done);
    CHECK (again->warning.empty());
}

TEST_CASE ("go.doh: a cue overrides its device either way")
{
    DohRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/mount/" + std::string (lightingDesk) + "/doh", "takeBack").ok);

    const auto scene = rig.document.createCue (rig.listId, rig.index++, "group", "Scene").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + scene + "/mode", "timeline").ok);

    const auto x = rig.document.createCue (scene, 0, "osc", "Leave it").id;
    const auto y = rig.document.createCue (scene, 1, "osc", "Take it back").id;

    for (const auto& [id, address, atom, own] : std::vector<std::tuple<std::string, std::string, std::string, std::string>> {
             { x, "/lx/go", "i:7", "leave" }, { y, "/desk/fader", "f:0.5", "takeBack" } })
    {
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + id + "/address", address).ok);
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + id + "/value", atom).ok);
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + id + "/doh", own).ok);
    }

    rig.document.createCue (rig.listId, rig.index++, "memo", "After");

    rig.park (scene);
    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (10);

    REQUIRE (rig.press ("go.doh").rejected == 0);
    rig.ticks (2);
    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (10);

    CHECK (rig.received ("/desk/fader") == 2u);
    CHECK (rig.received ("/lx/go") == 1u);

    const auto* xAgain = rig.newestRunOf (x);
    REQUIRE (xAgain != nullptr);
    CHECK (xAgain->warning == std::string (cue::runWarning::leftToOperator));
}

namespace
{
    /*  The author's own scene: lighting cues at nought, two and four seconds
        into a timeline, nothing in it heard. */
    struct LightingScene
    {
        explicit LightingScene (DohRig& rig)
        {
            group = rig.document.createCue (rig.listId, rig.index++, "group", "Scene").id;
            REQUIRE (rig.document.setAttribute ("/godot/cue/" + group + "/mode", "timeline").ok);

            const char* addresses[] { "/lx/a", "/lx/b", "/lx/c" };
            const char* offsets[] { "0", "2", "4" };

            for (int n = 0; n < 3; ++n)
            {
                const auto id = rig.document.createCue (group, n, "osc", "Light").id;
                REQUIRE (rig.document.setAttribute ("/godot/cue/" + id + "/address", addresses[n]).ok);
                REQUIRE (rig.document.setAttribute ("/godot/cue/" + id + "/value", "i:1").ok);
                REQUIRE (rig.document.setAttribute ("/godot/cue/" + id + "/preWait", offsets[n]).ok);
                members.push_back (id);
            }

            after = rig.document.createCue (rig.listId, rig.index++, "memo", "After").id;
        }

        std::string group, after;
        std::vector<std::string> members;
    };
}

TEST_CASE ("go.doh: a scene started over sends at its own time what the early GO had not sent, and nothing of what it had")
{
    DohRig rig;
    const LightingScene scene { rig };

    rig.park (scene.group);
    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (150);                                       // three seconds: two of the three have gone

    REQUIRE (rig.received ("/lx/a") == 1u);
    REQUIRE (rig.received ("/lx/b") == 1u);
    REQUIRE (rig.received ("/lx/c") == 0u);

    REQUIRE (rig.press ("go.doh").rejected == 0);
    rig.ticks (2);
    REQUIRE (rig.press ("go").rejected == 0);

    rig.ticks (190);                                       // just short of four seconds
    CHECK (rig.received ("/lx/a") == 1u);
    CHECK (rig.received ("/lx/b") == 1u);
    CHECK (rig.received ("/lx/c") == 0u);

    rig.ticks (20);
    CHECK (rig.received ("/lx/c") == 1u);

    for (const auto& id : { scene.members[0], scene.members[1] })
    {
        const auto* again = rig.newestRunOf (id);
        REQUIRE (again != nullptr);
        CHECK (again->warning == std::string (cue::runWarning::leftToOperator));
    }
}

TEST_CASE ("go.doh: an act's footer that went to a desk left to its operator is not sent again when the act ends again")
{
    DohRig rig;
    const auto act = rig.document.createCue (rig.listId, rig.index++, "group", "Act").id;
    const auto one = rig.document.createCue (act, 0, "memo", "One").id;
    rig.document.createCue (act, 1, "memo", "Two");

    std::string three;

    SUBCASE ("its last member a line")
    {
        three = rig.document.createCue (act, 2, "memo", "Three").id;
    }

    /*  THE CORRECTED GO'S OWN STOP ON THE ACT (§24, HB): the act brought back
        to life carries no stop of the early GO's, so the footer the corrected
        GO sets off is that GO's - and held back for the desk. Kept, the early
        GO's ask made it nobody's, and it went out again. */
    SUBCASE ("its last member a stop aimed at the act itself")
    {
        three = rig.document.createCue (act, 2, "transport", "End the act").id;
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + three + "/target", act).ok);
    }

    const auto footer = rig.document.createRole (act, "footer").id;
    const auto release = rig.document.createCue (footer, 0, "osc", "Houselights").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + release + "/address", "/lx/footer").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + release + "/value", "i:1").ok);
    rig.document.createCue (rig.listId, rig.index++, "memo", "After");

    rig.park (one);

    for (int press = 0; press < 3; ++press)
    {
        REQUIRE (rig.press ("go").rejected == 0);
        rig.ticks (8);
    }

    REQUIRE (rig.received ("/lx/footer") == 1u);

    REQUIRE (rig.press ("go.doh").rejected == 0);
    REQUIRE (rig.standby() == three);
    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (10);

    CHECK (rig.received ("/lx/footer") == 1u);

    const auto* again = rig.newestRunOf (release);
    REQUIRE (again != nullptr);
    CHECK (again->warning == std::string (cue::runWarning::leftToOperator));
}

TEST_CASE ("go.doh: a Doh of the corrected GO keeps what the first Doh left")
{
    DohRig rig;
    const LightingScene scene { rig };

    rig.park (scene.group);
    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (50);                                        // one second: the first light has gone
    REQUIRE (rig.received ("/lx/a") == 1u);

    REQUIRE (rig.press ("go.doh").rejected == 0);
    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (250);                                       // the corrected GO sends the other two

    REQUIRE (rig.received ("/lx/b") == 1u);
    REQUIRE (rig.received ("/lx/c") == 1u);

    REQUIRE (rig.press ("go.doh").rejected == 0);
    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (250);

    CHECK (rig.received ("/lx/a") == 1u);
    CHECK (rig.received ("/lx/b") == 1u);
    CHECK (rig.received ("/lx/c") == 1u);
}

TEST_CASE ("go.doh: the Doh's own persistent pass sends nothing to a device left to its operator")
{
    DohRig rig;

    const auto section = rig.document.createPersistent (rig.listId);
    REQUIRE (section.ok);
    const auto bed = rig.document.createCue (section.id, 0, "osc", "Sub one up").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + bed + "/address", "/lx/sub").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + bed + "/value", "f:1").ok);

    const auto eleven = rig.document.createCue (rig.listId, rig.index++, "memo", "Eleven").id;
    const auto twelve = rig.document.createCue (rig.listId, rig.index++, "group", "Twelve").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + twelve + "/mode", "timeline").ok);

    const auto stopper = rig.document.createCue (twelve, 0, "transport", "Sub one out").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + stopper + "/target", bed).ok);
    const auto blackout = rig.document.createCue (twelve, 1, "osc", "Sub one to nought").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + blackout + "/address", "/lx/sub").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + blackout + "/value", "f:0").ok);

    auto takeBack = false;

    SUBCASE ("left to its operator, the default") {}

    SUBCASE ("a device that takes back: the pass re-asserts, and the corrected GO sends the blackout again")
    {
        takeBack = true;
        REQUIRE (rig.document.setAttribute ("/godot/mount/" + std::string (lightingDesk) + "/doh", "takeBack").ok);
    }

    const auto values = [&rig]
    {
        //  Everything that left has arrived (the rig's `received`).
        REQUIRE (rig.listener.waitFor (rig.sender.sentFor (lightingDesk) + rig.sender.sentFor ("K3PV7WRB")));
        std::vector<float> out;

        for (const auto& datagram : rig.listener.all())
        {
            const auto decoded = osc::decode (datagram.bytes.data(), datagram.bytes.size());

            if (decoded.ok && decoded.packet.address == "/lx/sub" && ! decoded.packet.args.empty())
                out.push_back (decoded.packet.args.front().getFloat32());
        }

        return out;
    };

    rig.park (eleven);
    REQUIRE (rig.press ("go").rejected == 0);               // eleven: its step asserts the bed
    rig.ticks (40);
    REQUIRE (values() == std::vector<float> { 1.0f });

    REQUIRE (rig.press ("go").rejected == 0);               // twelve: the blackout, and the bed dropped
    rig.ticks (40);
    REQUIRE (values() == std::vector<float> { 1.0f, 0.0f });

    const auto assertsBefore = [&rig]
    {
        const auto parsed = LogFile::parse (rig.engine.log().contents());
        return std::count_if (parsed.records.begin(), parsed.records.end(),
                              [] (const auto& record) { return record.command == "run.assert"; });
    };

    const auto before = assertsBefore();

    REQUIRE (rig.press ("go.doh").rejected == 0);
    rig.ticks (40);

    if (takeBack)
    {
        CHECK (assertsBefore() > before);
        CHECK (values() == std::vector<float> { 1.0f, 0.0f, 1.0f });
    }
    else
    {
        CHECK (assertsBefore() == before);
        CHECK (values() == std::vector<float> { 1.0f, 0.0f });
    }

    REQUIRE (rig.press ("go").rejected == 0);               // twelve, corrected
    rig.ticks (40);

    if (takeBack)
        CHECK (values() == std::vector<float> { 1.0f, 0.0f, 1.0f, 0.0f });
    else
        CHECK (values() == std::vector<float> { 1.0f, 0.0f });
}

TEST_CASE ("go.doh: what was left outlives a GO on an earlier cue, and goes once the show has gone past it")
{
    DohRig rig;
    const auto halfway = rig.document.createCue (rig.listId, rig.index++, "memo", "Eleven and a half").id;
    const auto twelve = rig.makeOsc ("/lx/go", "i:12", "none");
    const auto thirteen = rig.document.createCue (rig.listId, rig.index++, "memo", "Thirteen").id;

    rig.park (twelve);                                     // eleven and a half skipped
    REQUIRE (rig.press ("go").rejected == 0);
    REQUIRE (rig.listener.waitFor (1));

    REQUIRE (rig.press ("go.doh").rejected == 0);
    REQUIRE (rig.standby() == twelve);

    SUBCASE ("the author's own: eleven and a half, then twelve - twelve sends nothing")
    {
        rig.park (halfway);
        REQUIRE (rig.press ("go").rejected == 0);
        REQUIRE (rig.standby() == twelve);
        REQUIRE (rig.press ("go").rejected == 0);
        rig.ticks (2);

        CHECK (rig.received ("/lx/go") == 1u);
        CHECK (rig.newestRunOf (twelve)->warning == std::string (cue::runWarning::leftToOperator));
    }

    SUBCASE ("a GO past twelve forgets it: coming back to twelve sends it")
    {
        rig.park (thirteen);
        REQUIRE (rig.press ("go").rejected == 0);
        rig.park (twelve);
        REQUIRE (rig.press ("go").rejected == 0);
        rig.ticks (2);

        CHECK (rig.received ("/lx/go") == 2u);
    }

    SUBCASE ("a start cue on the GO before it fires twelve: that GO's, and it sends nothing")
    {
        const auto starter = rig.document.createCue (rig.listId, 0, "start", "Start twelve").id;
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + starter + "/target", twelve).ok);

        rig.park (starter);
        REQUIRE (rig.press ("go").rejected == 0);
        rig.ticks (4);

        CHECK (rig.received ("/lx/go") == 1u);
        CHECK (rig.newestRunOf (twelve)->warning == std::string (cue::runWarning::leftToOperator));
    }
}

TEST_CASE ("go.doh: what left is decided at the send")
{
    DohRig rig;
    const auto lx = rig.makeOsc ("/lx/go", "i:12", "none");
    rig.document.createCue (rig.listId, rig.index++, "memo", "After");

    SUBCASE ("a value the described console's node refuses wrote nothing: the corrected GO sends again")
    {
        const auto refused = rig.makeOsc ("/desk/fader", "s:loud", "none");
        rig.park (refused);
        REQUIRE (rig.press ("go").rejected == 0);
        rig.ticks (2);
        REQUIRE (rig.newestRunOf (refused)->state == cue::runState::failed);

        REQUIRE (rig.press ("go.doh").rejected == 0);
        REQUIRE (rig.press ("go").rejected == 0);
        rig.ticks (2);

        CHECK (rig.newestRunOf (refused)->warning != std::string (cue::runWarning::leftToOperator));
    }

    SUBCASE ("the device's prefix edited between the GO and the Doh: still the device it was sent to, and its setting is read there")
    {
        /*  ON A DESK THAT TAKES BACK, so the two readings part: the device the
            send went to takes back, and the cue goes again; read off the
            address at the Doh, `/lx/go` would have named no device - `leave` -
            and nothing would have gone. */
        auto takeBack = false;

        SUBCASE ("left to its operator") {}

        SUBCASE ("taking back")
        {
            takeBack = true;
            REQUIRE (rig.document.setAttribute ("/godot/mount/" + std::string (lightingDesk) + "/doh", "takeBack").ok);
        }

        rig.park (lx);
        REQUIRE (rig.press ("go").rejected == 0);
        REQUIRE (rig.received ("/lx/go") == 1u);
        CHECK (rig.newestRunOf (lx)->sentTo == lightingDesk);

        REQUIRE (rig.document.setAttribute ("/godot/mount/" + std::string (lightingDesk) + "/prefix", "/lights").ok);

        REQUIRE (rig.press ("go.doh").rejected == 0);
        REQUIRE (rig.document.setAttribute ("/godot/mount/" + std::string (lightingDesk) + "/prefix", "/lx").ok);
        REQUIRE (rig.press ("go").rejected == 0);
        rig.ticks (2);

        CHECK (rig.received ("/lx/go") == (takeBack ? 2u : 1u));
        CHECK (rig.newestRunOf (lx)->warning
                 == std::string (takeBack ? "" : cue::runWarning::leftToOperator));
    }

    /*  THE DEVICE DELETED SINCE THE SEND (the design's test 50, the SUBCASE
        namespace draft §24.10 named owed; D5, 2026-10-03): a device gone reads
        `leave` whatever it said while it was there - the setting is the
        document's, and there is no row left to say take back - and the report
        names it by the identifier its run kept (`sentTo`). A net: D1 reads it
        so, and D3 names it. */
    SUBCASE ("the device deleted before the Doh, though it took back: left to its operator, named by its identifier")
    {
        REQUIRE (rig.document.setAttribute ("/godot/mount/" + std::string (lightingDesk) + "/doh", "takeBack").ok);

        rig.park (lx);
        REQUIRE (rig.press ("go").rejected == 0);
        REQUIRE (rig.received ("/lx/go") == 1u);
        REQUIRE (rig.newestRunOf (lx)->sentTo == lightingDesk);

        REQUIRE (rig.document.remove (lightingDesk).ok);

        REQUIRE (rig.press ("go.doh").rejected == 0);
        rig.ticks (2);

        const auto said = rig.runner.listState().dohReport().text;
        INFO (said);
        CHECK (said.find (std::string (lightingDesk) + ": ") != std::string::npos);
        CHECK (said.find ("left to its operator, not sent again") != std::string::npos);
        CHECK (said.find ("could not be taken back") == std::string::npos);

        REQUIRE (rig.press ("go").rejected == 0);
        rig.ticks (2);

        CHECK (rig.received ("/lx/go") == 1u);
        CHECK (rig.newestRunOf (lx)->warning == std::string (cue::runWarning::leftToOperator));
    }

    /*  A DOUBLE ESC IN THE GO'S OWN DRAIN DROPS WHAT THE GO QUEUED (2026-10-02,
        H4, namespace draft §23.10; §24, HQ, L31): the flush that ends the tick
        finds nothing, so the message never left - the run is stamped
        `sendDropped`, it does not count as sent, and the corrected GO sends the
        desk the cue it never had. The stamp is the handler's, from the runs
        launched in that drain, so a replay with no sender stamps the same run.
        Until H4 the message left, and this said so: it counted as sent, and the
        corrected GO sent nothing. */
    SUBCASE ("a double Esc in the GO's own drain: the message never left, so the corrected GO sends it")
    {
        //  Parked through the command, so the replay at the end is given the pointer too.
        REQUIRE (rig.press ("standby.set", { osc::Value::string (lx) }).rejected == 0);
        rig.tickOnce();
        rig.engine.submit ("cli", "go", {});
        rig.engine.submit ("cli", "run.killAll", {});
        rig.tickOnce();

        const auto* first = rig.newestRunOf (lx);
        REQUIRE (first != nullptr);
        CHECK (first->sendDropped);
        CHECK (rig.received ("/lx/go") == 0u);

        rig.ticks (3);
        REQUIRE (rig.press ("go.doh").rejected == 0);
        REQUIRE (rig.press ("go").rejected == 0);
        rig.ticks (2);

        CHECK (rig.received ("/lx/go") == 1u);
        CHECK (rig.newestRunOf (lx)->warning.empty());

        replaysTheSame (rig, false);
    }

    /*  A GUARD: the stamp is for the press's own drain. A message the flush of
        the GO's tick had already sent left, and counts. */
    SUBCASE ("a double Esc on the tick after the GO: it had left, so it is not sent again")
    {
        rig.park (lx);
        REQUIRE (rig.press ("go").rejected == 0);
        REQUIRE (rig.received ("/lx/go") == 1u);

        REQUIRE (rig.press ("run.killAll").rejected == 0);
        CHECK_FALSE (rig.newestRunOf (lx)->sendDropped);

        rig.ticks (3);
        REQUIRE (rig.press ("go.doh").rejected == 0);
        REQUIRE (rig.press ("go").rejected == 0);
        rig.ticks (2);

        CHECK (rig.received ("/lx/go") == 1u);
        CHECK (rig.newestRunOf (lx)->warning == std::string (cue::runWarning::leftToOperator));
    }

    SUBCASE ("a single Esc drops nothing: it left, and is not sent again")
    {
        rig.park (lx);
        rig.engine.submit ("cli", "go", {});
        rig.engine.submit ("cli", "run.stopAll", {});
        rig.tickOnce();
        REQUIRE (rig.listener.waitFor (1));

        rig.ticks (3);
        REQUIRE (rig.press ("go.doh").rejected == 0);
        REQUIRE (rig.press ("go").rejected == 0);
        rig.ticks (2);

        CHECK (rig.received ("/lx/go") == 1u);
        CHECK (rig.newestRunOf (lx)->warning == std::string (cue::runWarning::leftToOperator));
    }
}

TEST_CASE ("double Esc: a session whose outputs were dropped replays record for record with no sender")
{
    /*  THE DROP IS NO RECORD'S BUSINESS (namespace draft §23.10). `run.killAll`
        empties the sender inside its handler, and a replay runs that handler
        with no sender at all - so the handler must neither submit anything
        nor log what it dropped, or the replay's applied arguments part from
        the night's. A guard, passing before H4 and after it. */
    DohRig rig;

    auto capped = *rig.mounts.declarationOf ("K3PV7WRB");
    capped.rateCap = 2.0;
    REQUIRE (rig.mounts.updateDeclaration (capped));

    rig.fire (rig.makeOsc ("/desk/fader", "f:0.25", "none"));
    REQUIRE (rig.listener.waitFor (1));

    rig.fire (rig.makeOsc ("/desk/fader", "f:0.75", "sent"));
    REQUIRE (rig.sender.pending() == 1u);

    REQUIRE (rig.engine.submit ("cli", "run.killAll", {}));
    rig.ticks (5);

    replaysTheSame (rig, false);
}

TEST_CASE ("go.doh: two Dohs on one list - each cue left with the desk keeps its own entry")
{
    /*  WHAT WAS LEFT IS KEPT PER CUE (§24, HP): Doh! on 12, GO 11.5, Doh! on
        11.5 too - the list's mark holds both, each living by its own cue - and
        the corrected GOs on 11.5 and then on 12 send the desk neither again. A
        net for a road the design asked for and no case reached. */
    DohRig rig;
    const auto halfway = rig.makeOsc ("/lx/half", "i:1", "none");
    const auto twelve = rig.makeOsc ("/lx/go", "i:12", "none");
    rig.document.createCue (rig.listId, rig.index++, "memo", "Thirteen");

    rig.park (twelve);                                     // eleven and a half skipped
    REQUIRE (rig.press ("go").rejected == 0);
    REQUIRE (rig.received ("/lx/go") == 1u);
    REQUIRE (rig.press ("go.doh").rejected == 0);

    rig.park (halfway);
    REQUIRE (rig.press ("go").rejected == 0);              // eleven and a half - too early as well
    REQUIRE (rig.received ("/lx/half") == 1u);
    REQUIRE (rig.press ("go.doh").rejected == 0);
    REQUIRE (rig.standby() == halfway);

    REQUIRE (rig.press ("go").rejected == 0);              // eleven and a half, corrected
    REQUIRE (rig.standby() == twelve);
    REQUIRE (rig.press ("go").rejected == 0);              // twelve, corrected
    rig.ticks (2);

    CHECK (rig.received ("/lx/half") == 1u);
    CHECK (rig.received ("/lx/go") == 1u);
    CHECK (rig.newestRunOf (halfway)->warning == std::string (cue::runWarning::leftToOperator));
    CHECK (rig.newestRunOf (twelve)->warning == std::string (cue::runWarning::leftToOperator));
}

TEST_CASE ("go.doh: a jump forgets what was left with a device's operator - the next GO on the cue sends it")
{
    /*  A JUMP PUTS THE SHOW SOMEWHERE ELSE (§24, HP): what is due there goes
        out, and what a Doh left with a desk's operator on that list is
        forgotten with the GO it came from. A net for a road no case reached. */
    DohRig rig;
    const auto top = rig.document.createCue (rig.listId, rig.index++, "memo", "Top").id;
    const auto lx = rig.makeOsc ("/lx/go", "i:12", "none");
    rig.document.createCue (rig.listId, rig.index++, "memo", "After");

    rig.park (lx);
    REQUIRE (rig.press ("go").rejected == 0);
    REQUIRE (rig.received ("/lx/go") == 1u);
    REQUIRE (rig.press ("go.doh").rejected == 0);

    //  A jump to the top of the list, where nothing has fired yet.
    REQUIRE (rig.press ("list.aim", { osc::Value::string (rig.listId), osc::Value::string (top),
                                      osc::Value::float64 (0.0) }).rejected == 0);
    REQUIRE (rig.press ("list.loadToTime", { osc::Value::string (rig.listId) }).rejected == 0);

    rig.park (lx);
    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (2);

    CHECK (rig.received ("/lx/go") == 2u);
    CHECK (rig.newestRunOf (lx)->warning.empty());
}

//==============================================================================
/*  DOH! D2 (2026-10-02, namespace draft §24.12): a scene that was heard is
    carried on by the corrected GO - re-seated where it was - and what it had
    sent goes again only where it takes back, one tick apart per address; what
    reached a device left to its operator is not sent again. These rigs have no
    audio side, so the scene's sound is heard by `run.started` sent by hand, as
    the audio side would. Each case was run on the code before D2 first. */
namespace
{
    /*  A timeline scene with a bed at nought - what makes it heard - and the
        network cues a case names, each at its second. */
    struct HeardScene
    {
        HeardScene (DohRig& rig, std::vector<std::tuple<std::string, std::string, std::string>> sends)
        {
            group = rig.document.createCue (rig.listId, rig.index++, "group", "Scene").id;
            REQUIRE (rig.document.setAttribute ("/godot/cue/" + group + "/mode", "timeline").ok);

            bed = rig.document.createCue (group, 0, "media", "Bed").id;
            REQUIRE (rig.document.setAttribute ("/godot/cue/" + bed + "/file", "bed.wav").ok);

            auto at = 1;

            for (const auto& [address, atom, second] : sends)
            {
                const auto id = rig.document.createCue (group, at++, "osc", "Send").id;
                REQUIRE (rig.document.setAttribute ("/godot/cue/" + id + "/address", address).ok);
                REQUIRE (rig.document.setAttribute ("/godot/cue/" + id + "/value", atom).ok);
                REQUIRE (rig.document.setAttribute ("/godot/cue/" + id + "/preWait", second).ok);
                members.push_back (id);
            }

            after = rig.document.createCue (rig.listId, rig.index++, "memo", "After").id;
        }

        /*  GO, the bed heard, and `ticks` of the scene. */
        std::string goAndHear (DohRig& rig, int ticks)
        {
            //  Parked through the command, so a replay is given the pointer.
            REQUIRE (rig.press ("standby.set", { osc::Value::string (group) }).rejected == 0);
            rig.tickOnce();
            REQUIRE (rig.press ("go").rejected == 0);
            rig.tickOnce();

            const auto* sounding = rig.newestRunOf (bed);
            REQUIRE (sounding != nullptr);
            const auto id = sounding->id;
            REQUIRE (rig.engine.submit (origin::engine, "run.started", { osc::Value::string (id) }));
            rig.tickOnce();
            REQUIRE (rig.runs.find (id)->startedAtTick >= 0);

            rig.ticks (ticks);
            return id;
        }

        std::string group, bed, after;
        std::vector<std::string> members;
    };
}

TEST_CASE ("go.doh: a resumed scene sends again what it had sent, one tick apart per address")
{
    /*  The design's test 23 (red team B minor 1): the sender keeps one message
        per address a flush, the last winning, so two GOs the scene had sent to
        one address, both fired again at the corrected GO, would leave as one. */
    DohRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/mount/" + std::string (lightingDesk) + "/doh", "takeBack").ok);

    HeardScene scene { rig, { { "/lx/go", "i:1", "0" }, { "/lx/go", "i:2", "2" } } };
    scene.goAndHear (rig, 150);
    REQUIRE (rig.received ("/lx/go") == 2u);

    REQUIRE (rig.press ("go.doh").rejected == 0);
    rig.ticks (2);
    /*  (2026-10-03, the rollback: the scene's first send decides for the
        desk, and with nothing before it there is nothing to roll back with -
        never the scene's own first send, which the GO made.) */
    const auto said = rig.runner.listState().dohReport().text;
    INFO (said);
    CHECK (said.find ("rolled back") == std::string::npos);

    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (5);

    CHECK (rig.received ("/lx/go") == 4u);
}

TEST_CASE ("go.doh: a resumed scene sends again what takes back, and nothing of what was left")
{
    /*  The design's test 24 (the author, 2026-10-01): A went to the lighting
        desk, left to its operator by default; B to the console, which takes
        back; D, at six seconds, was never sent. The corrected GO carries the
        bed on at about three seconds, sends B again at once, A not at all, and
        D at its own time, three seconds on, whatever its device says. */
    DohRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/mount/K3PV7WRB/doh", "takeBack").ok);

    HeardScene scene { rig, { { "/lx/a", "i:1", "0.5" }, { "/desk/fader", "f:0.5", "1" }, { "/lx/d", "i:1", "6" } } };
    const auto a = scene.members[0];
    const auto b = scene.members[1];

    auto aAgain = 1u;
    auto bAgain = 2u;

    SUBCASE ("as the devices say") {}

    SUBCASE ("A says take back") { aAgain = 2u; REQUIRE (rig.document.setAttribute ("/godot/cue/" + a + "/doh", "takeBack").ok); }

    SUBCASE ("B says leave") { bAgain = 1u; REQUIRE (rig.document.setAttribute ("/godot/cue/" + b + "/doh", "leave").ok); }

    const auto bed = scene.goAndHear (rig, 148);
    REQUIRE (rig.received ("/lx/a") == 1u);
    REQUIRE (rig.received ("/desk/fader") == 1u);
    REQUIRE (rig.received ("/lx/d") == 0u);

    REQUIRE (rig.press ("go.doh").rejected == 0);
    rig.ticks (2);
    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (5);

    CHECK (rig.received ("/lx/a") == aAgain);
    CHECK (rig.received ("/desk/fader") == bAgain);
    CHECK (rig.received ("/lx/d") == 0u);

    const auto* carried = rig.newestRunOf (scene.bed);
    REQUIRE (carried != nullptr);
    CHECK (carried->id != bed);
    CHECK (carried->startOffset == doctest::Approx (3.0).epsilon (0.03));

    rig.ticks (155);
    CHECK (rig.received ("/lx/d") == 1u);
}

TEST_CASE ("go.doh: a second Doh! forgets the resume and keeps what was left")
{
    /*  The design's test 25 (the author, 2026-09-30, (b); L37): the second press
        means the next GO starts the scene from its top - the bed at its own
        offset - and still sends nothing a device's operator was left with; what
        takes back is sent again. */
    DohRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/mount/K3PV7WRB/doh", "takeBack").ok);

    HeardScene scene { rig, { { "/lx/a", "i:1", "0.5" }, { "/desk/fader", "f:0.5", "1" } } };
    const auto a = scene.members[0];
    const auto bed = scene.goAndHear (rig, 148);

    REQUIRE (rig.press ("go.doh").rejected == 0);
    rig.ticks (70);
    REQUIRE (rig.runs.find (bed)->isFinished());

    //  The second press: applied, and the resume is gone.
    REQUIRE (rig.press ("go.doh").rejected == 0);
    REQUIRE (rig.standby() == scene.group);

    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (80);

    const auto* fresh = rig.newestRunOf (scene.bed);
    REQUIRE (fresh != nullptr);
    CHECK (fresh->id != bed);
    CHECK (fresh->startOffset == doctest::Approx (0.0));

    CHECK (rig.received ("/lx/a") == 1u);
    CHECK (rig.received ("/desk/fader") == 2u);
    CHECK (rig.newestRunOf (a)->warning == std::string (cue::runWarning::leftToOperator));

    replaysTheSame (rig);
}

//==============================================================================
/*  DOH! D3 - THE DESK PUT BACK, AND THE REPORT (2026-10-03, PRD §3.32,
    namespace draft §24.13).

    What the GO wrote on a desk that takes back goes back to what the desk held
    before the GO - decided on the tick after the press, against what the GO
    wrote and the desk's first echo of it, and left alone where another writer
    has touched it; on a desk left to its operator - every device unless told
    otherwise - nothing goes back and the report names the device and the cue.
    Each case was run on the engine sources of 5fd0e76 (D2's review), built
    with these cases, and failed there unless it says it is a net. */
namespace
{
    /*  The applied `node.set` records on one address, in order. */
    std::vector<LogRecord> setsOn (NetworkRig& rig, const std::string& address)
    {
        std::vector<LogRecord> out;

        for (const auto& record : LogFile::parse (rig.engine.log().contents()).records)
            if (record.kind == LogRecord::Kind::applied && record.command == "node.set" && ! record.args.empty()
                  && record.args[0].isString() && record.args[0].getString() == address)
                out.push_back (record);

        return out;
    }

    /*  Those the engine sent - a put-back - rather than a client. */
    std::size_t engineSetsOn (NetworkRig& rig, const std::string& address)
    {
        const auto all = setsOn (rig, address);

        return static_cast<std::size_t> (std::count_if (all.begin(), all.end(),
                                                        [] (const LogRecord& record) { return record.origin == origin::engine; }));
    }

    std::string reportOf (NetworkRig& rig)
    {
        return rig.runner.listState().dohReport().text;
    }

    bool says (const std::string& text, const std::string& part)
    {
        return text.find (part) != std::string::npos;
    }
}

TEST_CASE ("node.set: a write to a device switched off changes the tree and puts nothing on the wire")
{
    /*  The design's test 2: `serve`'s door queued a client's write, a
        scene's restore - and now Doh!'s put-back - whatever the device's `tx`
        said, where a cue's own write never went out to a device switched off.
        Before D3 it went out. */
    NetworkRig rig;
    auto declared = consoleMount (rig.listener.port());
    declared.tx = false;
    REQUIRE (rig.mounts.updateDeclaration (declared));

    REQUIRE (rig.engine.submit ("cli", "node.set", { osc::Value::string ("/desk/fader"),
                                                     osc::Value::float32 (0.25f) }));
    rig.tickOnce();

    REQUIRE (rig.mounts.valueOf ("/desk/fader") != nullptr);
    CHECK (*rig.mounts.valueOf ("/desk/fader") == osc::Values { osc::Value::float32 (0.25f) });
    CHECK (rig.sender.sentFor ("K3PV7WRB") == 0u);

    std::this_thread::sleep_for (std::chrono::milliseconds (50));
    CHECK (rig.listener.count() == 0u);
}

TEST_CASE ("go.doh: a desk value the GO wrote goes back, as one engine node.set after the standby's revocations")
{
    /*  The design's test 3, on a console that takes back. What the console
        held before the GO - what Go.dot last wrote there, or what it was last
        seen to hold - goes back on the tick after the press, as an engine
        `node.set`. An event has no value to put back: said, and sent again by
        the next GO. Before D3 the GO's value stayed. */
    DohRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/mount/K3PV7WRB/doh", "takeBack").ok);
    REQUIRE (rig.mounts.write ("/desk/fader", osc::Value::float32 (0.25f)).ok);       // what Go.dot last wrote there

    std::string address = "/desk/fader";
    std::string atom = "f:0.75";
    auto expected = osc::Value::float32 (0.25f);
    auto event = false;

    SUBCASE ("what Go.dot had written there") {}

    SUBCASE ("what the console was seen to hold")
    {
        rig.mounts.noteObservation ("/desk/fader", osc::Value::float32 (0.5f));
        expected = osc::Value::float32 (0.5f);
    }

    SUBCASE ("an event: nothing to put back - said, and sent again by the next GO")
    {
        event = true;
        address = "/desk/go";
        atom = "i:3";
    }

    const auto cue = rig.makeOsc (address, atom, "none");
    rig.document.createCue (rig.listId, rig.index++, "memo", "After");

    REQUIRE (rig.press ("standby.set", { osc::Value::string (cue) }).rejected == 0);
    rig.tickOnce();
    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (5);
    REQUIRE (rig.received (address) == 1u);

    REQUIRE (rig.press ("go.doh").rejected == 0);
    rig.ticks (2);

    if (event)
    {
        CHECK (engineSetsOn (rig, address) == 0u);
        CHECK (says (reportOf (rig), "could not be taken back - the next GO sends it again"));

        REQUIRE (rig.press ("go").rejected == 0);
        rig.ticks (2);
        CHECK (rig.received (address) == 2u);
        return;
    }

    REQUIRE (engineSetsOn (rig, address) == 1u);
    const auto last = setsOn (rig, address).back();
    CHECK (last.origin == origin::engine);
    CHECK (last.args[1] == expected);
    REQUIRE (rig.mounts.valueOf (address) != nullptr);
    CHECK (*rig.mounts.valueOf (address) == osc::Values { expected });
    CHECK_FALSE (says (reportOf (rig), "changed since the GO"));

    replaysTheSame (rig);
}

TEST_CASE ("go.doh: a desk value moved by a hand after the GO is left, and said")
{
    /*  The design's test 5: the console's first answer after the GO's write is
        its echo of it; a later answer that differs from both is a hand on the
        desk, and what a hand did is not undone. Before D3 nothing was said. */
    DohRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/mount/K3PV7WRB/doh", "takeBack").ok);
    REQUIRE (rig.mounts.write ("/desk/fader", osc::Value::float32 (0.25f)).ok);       // what Go.dot last wrote there

    const auto cue = rig.makeOsc ("/desk/fader", "f:0.75", "none");
    rig.document.createCue (rig.listId, rig.index++, "memo", "After");

    rig.park (cue);
    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (2);

    rig.mounts.noteObservation ("/desk/fader", osc::Value::float32 (0.75f));     // the desk's echo
    rig.mounts.noteObservation ("/desk/fader", osc::Value::float32 (0.5f));      // a hand on it

    REQUIRE (rig.press ("go.doh").rejected == 0);
    rig.ticks (2);

    CHECK (engineSetsOn (rig, "/desk/fader") == 0u);
    CHECK (says (reportOf (rig), "/desk/fader: changed since the GO - left as it is"));
}

TEST_CASE ("go.doh: a desk left to its operator is not put back, and the report names it")
{
    /*  The design's test 25 (the author, 2026-10-01: "always leave to its
        operator"). On the default nothing goes back, nothing is said changed,
        and the report names the device and the cue; a cue may say otherwise
        either way. Before D3 nothing was named. */
    DohRig rig;
    REQUIRE (rig.mounts.write ("/desk/fader", osc::Value::float32 (0.25f)).ok);       // what Go.dot last wrote there

    std::string address = "/desk/fader";
    std::string atom = "f:0.75";
    std::string cueSays;
    auto back = false;

    SUBCASE ("the default") {}

    SUBCASE ("an event: named the same, never 'could not be taken back'")
    {
        address = "/desk/go";
        atom = "i:3";
    }

    SUBCASE ("the cue says take back: what was there goes back, and the cue is sent again")
    {
        cueSays = "takeBack";
        back = true;
    }

    SUBCASE ("the device takes back and the cue says leave: left, and named")
    {
        REQUIRE (rig.document.setAttribute ("/godot/mount/K3PV7WRB/doh", "takeBack").ok);
        cueSays = "leave";
    }

    const auto cue = rig.makeOsc (address, atom, "none");
    rig.document.createCue (rig.listId, rig.index++, "memo", "After");

    if (! cueSays.empty())
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + cue + "/doh", cueSays).ok);

    rig.park (cue);
    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (2);
    REQUIRE (rig.received (address) == 1u);

    REQUIRE (rig.press ("go.doh").rejected == 0);
    rig.ticks (2);

    const auto said = reportOf (rig);
    INFO (said);

    if (back)
    {
        CHECK (engineSetsOn (rig, address) == 1u);
        CHECK (*rig.mounts.valueOf (address) == osc::Values { osc::Value::float32 (0.25f) });
        CHECK_FALSE (says (said, "left to its operator"));

        REQUIRE (rig.press ("go").rejected == 0);
        rig.ticks (2);
        CHECK (rig.received (address) == 3u);
        return;
    }

    CHECK (engineSetsOn (rig, address) == 0u);
    CHECK (says (said, "K3PV7WRB: "));
    CHECK (says (said, "Desk"));
    CHECK (says (said, " - left to its operator, not sent again"));
    CHECK_FALSE (says (said, "changed since the GO"));
    CHECK_FALSE (says (said, "could not be taken back"));

    if (address == "/desk/fader")
        CHECK (*rig.mounts.valueOf (address) == osc::Values { osc::Value::float32 (0.75f) });

    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (2);
    CHECK (rig.received (address) == 1u);
}

TEST_CASE ("go.doh: an address two of the GO's cues wrote follows the last writer")
{
    /*  The design's test 26 (L33): a scene writes the console's fader twice,
        one cue taking back and one left to its operator. The LAST write
        decides: left - the 0.75 the console holds is the GO's own, never
        "changed" - or put back to what was there before the GO, the left cue
        still named. Before D3 nothing went back or was said. (Its fold
        SUBCASE, a pre-send the GO committed, is `VerifiedCueTests`'.) */
    DohRig rig;
    REQUIRE (rig.mounts.write ("/desk/fader", osc::Value::float32 (0.25f)).ok);       // what Go.dot last wrote there

    const auto scene = rig.document.createCue (rig.listId, rig.index++, "group", "Scene").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + scene + "/advance", "auto").ok);

    const auto write = [&rig, &scene] (int index, const char* name, const char* atom)
    {
        const auto id = rig.document.createCue (scene, index, "osc", name).id;
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + id + "/address", "/desk/fader").ok);
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + id + "/value", atom).ok);
        return id;
    };

    const auto first = write (0, "First", "f:0.5");
    const auto last = write (1, "Last", "f:0.75");
    rig.document.createCue (rig.listId, rig.index++, "memo", "After");

    auto back = false;

    SUBCASE ("taken back, then left: left, and never changed")
    {
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + first + "/doh", "takeBack").ok);
    }

    SUBCASE ("left, then taken back: what was there before the GO goes back, the left cue named")
    {
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + last + "/doh", "takeBack").ok);
        back = true;
    }

    rig.park (scene);
    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (20);
    REQUIRE (*rig.mounts.valueOf ("/desk/fader") == osc::Values { osc::Value::float32 (0.75f) });

    REQUIRE (rig.press ("go.doh").rejected == 0);
    rig.ticks (2);

    const auto said = reportOf (rig);
    INFO (said);
    CHECK_FALSE (says (said, "changed since the GO"));

    if (back)
    {
        CHECK (engineSetsOn (rig, "/desk/fader") == 1u);
        CHECK (*rig.mounts.valueOf ("/desk/fader") == osc::Values { osc::Value::float32 (0.25f) });
        CHECK (says (said, "First - left to its operator, not sent again"));
    }
    else
    {
        CHECK (engineSetsOn (rig, "/desk/fader") == 0u);
        CHECK (*rig.mounts.valueOf ("/desk/fader") == osc::Values { osc::Value::float32 (0.75f) });
        CHECK (says (said, "Last - left to its operator, not sent again"));
    }
}

TEST_CASE ("go.doh: an OSC cue the GO stopped in its pre-wait is sent at its own time, or - its time passed - only where it takes back")
{
    /*  The design's test 29 (red team C minor 3, HR). Nothing of a cue in its
        pre-wait had left, so put back with the rest of its wait it sends at
        its time whatever its device says; its time passed while the GO had it
        stopped, it is sent late only to a device that takes back, and to one
        left to its operator it is named, never sent. Before D3 it was never
        put back. (Its MIDI half is `MidiTests`'.) */
    DohRig rig;
    const auto lx = rig.makeOsc ("/lx/go", "i:12", "none");
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + lx + "/preWait", "4").ok);

    const auto stop = rig.document.createCue (rig.listId, rig.index++, "transport", "Hold the lights").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + stop + "/target", lx).ok);
    rig.document.createCue (rig.listId, rig.index++, "memo", "After");

    REQUIRE (rig.press ("cue.fire", { osc::Value::string (lx) }).rejected == 0);
    const auto old = rig.newestRunOf (lx)->id;
    const auto due = rig.newestRunOf (lx)->dueTick;

    rig.ticks (50);
    rig.park (stop);
    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (2);
    REQUIRE (rig.runs.find (old)->isFinished());

    SUBCASE ("still to fire: put back with the rest of its wait, sent at its time")
    {
        rig.ticks (40);
        REQUIRE (rig.press ("go.doh").rejected == 0);

        const auto* again = rig.newestRunOf (lx);
        REQUIRE (again->id != old);
        CHECK (again->goSerial == 0u);
        CHECK (again->state == cue::runState::waiting);
        CHECK (again->dueTick == due);

        while (rig.tick < due + 5)
            rig.tickOnce();

        CHECK (rig.received ("/lx/go") == 1u);
    }

    SUBCASE ("its time passed, left to its operator: not sent, and named")
    {
        while (rig.tick < due + 10)
            rig.tickOnce();

        REQUIRE (rig.press ("go.doh").rejected == 0);
        CHECK (rig.newestRunOf (lx)->id == old);

        rig.ticks (5);
        CHECK (rig.received ("/lx/go") == 0u);
        CHECK (says (reportOf (rig), "QX7DESK0: Desk - left to its operator, not sent (the GO stopped it)"));
    }

    SUBCASE ("its time passed, a desk that takes back: sent at the Doh, where it would be")
    {
        REQUIRE (rig.document.setAttribute ("/godot/mount/" + std::string (lightingDesk) + "/doh", "takeBack").ok);

        while (rig.tick < due + 10)
            rig.tickOnce();

        REQUIRE (rig.press ("go.doh").rejected == 0);
        rig.ticks (5);
        CHECK (rig.received ("/lx/go") == 1u);
    }
}

TEST_CASE ("go.doh: after an Esc the desk and the pointer go back, nothing is relaunched and nothing waits")
{
    /*  The design's test 19 (GV): Esc stopped everything with its footers, and
        a Doh never undoes an Esc - but the desk still goes back, on a console
        that takes back. A scene the GO stopped is not relaunched. Before D3
        the desk stayed. */
    DohRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/mount/K3PV7WRB/doh", "takeBack").ok);
    REQUIRE (rig.mounts.write ("/desk/fader", osc::Value::float32 (0.25f)).ok);       // what Go.dot last wrote there

    //  A scene playing on its own, which the GO stops.
    const auto playing = rig.document.createCue (rig.listId, rig.index++, "group", "Playing").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + playing + "/mode", "timeline").ok);
    const auto hold = rig.document.createCue (playing, 0, "memo", "Hold").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + hold + "/preWait", "30").ok);

    const auto go = rig.document.createCue (rig.listId, rig.index++, "group", "Change").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + go + "/mode", "timeline").ok);
    const auto fader = rig.document.createCue (go, 0, "osc", "Fader").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + fader + "/address", "/desk/fader").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + fader + "/value", "f:0.75").ok);
    const auto out = rig.document.createCue (go, 1, "transport", "Out").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + out + "/target", playing).ok);
    rig.document.createCue (rig.listId, rig.index++, "memo", "After");

    REQUIRE (rig.press ("cue.fire", { osc::Value::string (playing) }).rejected == 0);
    rig.ticks (2);
    const auto scene = rig.newestRunOf (playing)->id;

    rig.park (go);
    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (10);
    REQUIRE (rig.runs.find (scene)->isFinished());

    REQUIRE (rig.press ("run.stopAll").rejected == 0);
    rig.ticks (60);

    REQUIRE (rig.press ("go.doh").rejected == 0);
    CHECK (rig.standby() == go);
    rig.ticks (5);

    CHECK (engineSetsOn (rig, "/desk/fader") == 1u);
    CHECK (*rig.mounts.valueOf ("/desk/fader") == osc::Values { osc::Value::float32 (0.25f) });
    CHECK (rig.newestRunOf (playing)->id == scene);
    CHECK (says (reportOf (rig), "Esc since the GO"));

    const auto parsed = LogFile::parse (rig.engine.log().contents());
    CHECK (std::none_of (parsed.records.begin(), parsed.records.end(),
                         [] (const LogRecord& record) { return record.command == "go.dohRelaunch"; }));
}

TEST_CASE ("go.doh: what an act's footer wrote because the GO ended it goes back")
{
    /*  The design's test 23 (red team B blocker): an act whose last member the
        GO fired ends, and its footer - the GO's, caused by it - writes the
        console. Doh! brings the act back to life (D1) and what the footer
        wrote back on the console, from the flush; the corrected GO's member
        ends the act, and the footer writes once more. Before D3 the footer's
        write stood. (Its bed SUBCASE, and the looping act's, are `GoTests`'.) */
    DohRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/mount/K3PV7WRB/doh", "takeBack").ok);
    REQUIRE (rig.mounts.write ("/desk/fader", osc::Value::float32 (0.25f)).ok);       // what Go.dot last wrote there

    const auto act = rig.document.createCue (rig.listId, rig.index++, "group", "Act").id;
    const auto one = rig.document.createCue (act, 0, "memo", "One").id;
    rig.document.createCue (act, 1, "memo", "Two");
    const auto three = rig.document.createCue (act, 2, "memo", "Three").id;
    const auto footer = rig.document.createRole (act, "footer").id;
    const auto release = rig.document.createCue (footer, 0, "osc", "Fader down").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + release + "/address", "/desk/fader").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + release + "/value", "f:0").ok);
    rig.document.createCue (rig.listId, rig.index++, "memo", "After");

    rig.park (one);

    for (int press = 0; press < 3; ++press)
    {
        REQUIRE (rig.press ("go").rejected == 0);
        rig.ticks (8);
    }

    REQUIRE (*rig.mounts.valueOf ("/desk/fader") == osc::Values { osc::Value::float32 (0.0f) });
    rig.ticks (10);

    REQUIRE (rig.press ("go.doh").rejected == 0);
    REQUIRE (rig.standby() == three);
    CHECK (rig.newestRunOf (act)->state == cue::runState::playing);
    rig.ticks (2);

    CHECK (engineSetsOn (rig, "/desk/fader") == 1u);
    CHECK (*rig.mounts.valueOf ("/desk/fader") == osc::Values { osc::Value::float32 (0.25f) });

    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (10);

    CHECK (*rig.mounts.valueOf ("/desk/fader") == osc::Values { osc::Value::float32 (0.0f) });
    CHECK (rig.received ("/desk/fader") == 3u);                 // 0, put back 0.25, 0
}

TEST_CASE ("go.doh: an act under an automatic sequence is not brought back - said, and its footer's send to a desk left to its operator is not sent again")
{
    /*  The design's test 23, its last SUBCASE (red team C minor 4, L27): the act
        ended by the GO sits in an automatic sequence, whose next member that
        end set off, so it cannot come back; the report says so, and names its
        footer's cue left. The corrected GO enters the act again, and when it
        ends its footer sends nothing: once in all. Before D3 nothing was said. */
    DohRig rig;

    const auto outer = rig.document.createCue (rig.listId, rig.index++, "group", "Sequence").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + outer + "/advance", "auto").ok);
    const auto act = rig.document.createCue (outer, 0, "group", "Act").id;
    const auto one = rig.document.createCue (act, 0, "memo", "One").id;
    rig.document.createCue (act, 1, "memo", "Two");
    rig.document.createCue (act, 2, "memo", "Three");
    const auto hold = rig.document.createCue (outer, 1, "memo", "Hold").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + hold + "/preWait", "30").ok);

    const auto footer = rig.document.createRole (act, "footer").id;
    const auto release = rig.document.createCue (footer, 0, "osc", "Houselights").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + release + "/address", "/lx/go").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + release + "/value", "i:99").ok);
    rig.document.createCue (rig.listId, rig.index++, "memo", "After");

    //  The sequence running - started by name - and the act inside it waiting for its GOs.
    REQUIRE (rig.press ("cue.fire", { osc::Value::string (outer) }).rejected == 0);
    rig.ticks (5);
    REQUIRE (rig.newestRunOf (act) != nullptr);
    REQUIRE (rig.newestRunOf (act)->parent == rig.newestRunOf (outer)->id);

    rig.park (one);

    for (int press = 0; press < 3; ++press)
    {
        REQUIRE (rig.press ("go").rejected == 0);
        rig.ticks (8);
    }

    REQUIRE (rig.received ("/lx/go") == 1u);
    REQUIRE (rig.newestRunOf (act)->isFinished());

    REQUIRE (rig.press ("go.doh").rejected == 0);
    rig.ticks (2);

    const auto said = reportOf (rig);
    INFO (said);
    CHECK (says (said, "Act ended when "));
    CHECK (says (said, "it was not brought back - the next GO enters it again from its header"));
    CHECK (says (said, "Houselights - left to its operator, not sent again"));

    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (20);

    CHECK (rig.received ("/lx/go") == 1u);
}

TEST_CASE ("go.doh: the report names what the setting left alone, whatever list has the focus")
{
    /*  The design's test 30 (red team C minors 1 and 4): D1's persistent rig.
        The report names the persistent cue the restored pointer brought back
        into the plan that the Doh's own pass did not re-assert, and the cue
        the GO had sent to the lighting desk; a desk deleted since the GO is
        named by its identifier; and the report is the runner's, naming the
        GO's list, whichever list has the focus. Before D3 there was none. */
    DohRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/mount/" + std::string (lightingDesk) + "/name", "Lights").ok);

    const auto section = rig.document.createPersistent (rig.listId);
    REQUIRE (section.ok);
    const auto bed = rig.document.createCue (section.id, 0, "osc", "Sub one up").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + bed + "/address", "/lx/sub").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + bed + "/value", "f:1").ok);

    const auto eleven = rig.document.createCue (rig.listId, rig.index++, "memo", "Eleven").id;
    const auto twelve = rig.document.createCue (rig.listId, rig.index++, "group", "Twelve").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + twelve + "/mode", "timeline").ok);

    const auto stopper = rig.document.createCue (twelve, 0, "transport", "Sub one out").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + stopper + "/target", bed).ok);
    const auto blackout = rig.document.createCue (twelve, 1, "osc", "Sub one to nought").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + blackout + "/address", "/lx/sub").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + blackout + "/value", "f:0").ok);

    rig.park (eleven);
    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (40);
    REQUIRE (rig.press ("go").rejected == 0);               // twelve
    rig.ticks (40);

    std::string device = "Lights";

    SUBCASE ("as it is") {}

    SUBCASE ("the desk deleted since the GO: named by the identifier its runs kept")
    {
        REQUIRE (rig.document.remove (lightingDesk).ok);
        device = lightingDesk;
    }

    SUBCASE ("the focus on another list")
    {
        const auto other = rig.document.createList ("Effects").id;
        REQUIRE (rig.press ("list.focus", { osc::Value::string (other) }).rejected == 0);
    }

    REQUIRE (rig.press ("go.doh").rejected == 0);
    rig.ticks (2);

    const auto& report = rig.runner.listState().dohReport();
    INFO (report.text);
    CHECK (report.list == rig.listId);
    CHECK (says (report.text, device + ": Sub one to nought - left to its operator, not sent again"));

    if (device == "Lights")
        CHECK (says (report.text, "persistent Sub one up on Lights: not re-asserted - left to its operator"));
}

TEST_CASE ("go.doh: an OSC event and a write to an opaque device that take back are named, and sent again by the next GO")
{
    /*  Doh! D4 (2026-10-03, namespace draft §24.14; the design's test 2). Nothing
        to read back means nothing to put back: an event has no value, and an
        opaque device - the author's lighting desk - describes nothing. Where
        the device takes back, the report names the cue and the next GO sends it
        again, as a first GO would (the author, 2026-09-30, (a)); its run carries
        no warning. A net: D3 already named both, from the desk's capture (GW,
        §24.13), and this pins the opaque half, which no case drove. */
    DohRig rig;

    std::string address = "/lx/go";
    std::string atom = "i:12";

    SUBCASE ("a write to an opaque device")
    {
        REQUIRE (rig.document.setAttribute ("/godot/mount/" + std::string (lightingDesk) + "/doh", "takeBack").ok);
    }

    SUBCASE ("an event on a described console")
    {
        REQUIRE (rig.document.setAttribute ("/godot/mount/K3PV7WRB/doh", "takeBack").ok);
        address = "/desk/go";
        atom = "i:3";
    }

    const auto cue = rig.makeOsc (address, atom, "none");
    rig.document.createCue (rig.listId, rig.index++, "memo", "After");

    //  Parked by a record, so the replay below stands where the session stood.
    REQUIRE (rig.press ("standby.set", { osc::Value::string (cue) }).rejected == 0);
    rig.tickOnce();
    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (2);
    REQUIRE (rig.received (address) == 1u);

    REQUIRE (rig.press ("go.doh").rejected == 0);
    rig.ticks (2);

    const auto said = reportOf (rig);
    INFO (said);
    CHECK (says (said, "Desk: could not be taken back - the next GO sends it again"));
    CHECK_FALSE (says (said, "left to its operator"));
    CHECK (engineSetsOn (rig, address) == 0u);

    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (2);

    CHECK (rig.received (address) == 2u);
    REQUIRE (rig.newestRunOf (cue) != nullptr);
    CHECK (rig.newestRunOf (cue)->warning.empty());

    replaysTheSame (rig);
}

TEST_CASE ("go.doh: what could not be taken back is rolled back - the previous command, the device's, or the cue's own")
{
    /*  The rollback (2026-10-03, PRD §3.32, namespace draft §24.5, OV-OX). The
        author's case: "we might send a go back command or a different cue or
        scene number if for instance the light board has autofollow cues after
        the last one sent". On a desk that takes back, what nothing can read
        back is taken back at the Doh by a message written ahead, sent once,
        on the tick after the press, as an engine `node.set`; the corrected
        GO still sends the cue again. Before it, the report said "could not be
        taken back" and nothing was sent. */
    DohRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/mount/" + std::string (lightingDesk) + "/doh", "takeBack").ok);
    REQUIRE (rig.document.setAttribute ("/godot/mount/" + std::string (lightingDesk) + "/name", "Lights").ok);

    const auto before = rig.makeOsc ("/lx/go", "i:11", "none");
    const auto cue = rig.makeOsc ("/lx/go", "i:12", "none");
    rig.document.createCue (rig.listId, rig.index++, "memo", "After");
    juce::ignoreUnused (before);

    std::string address = "/lx/go";
    auto expected = osc::Value::int32 (11);
    std::string words = "rolled back with /lx/go i:11";

    SUBCASE ("the previous command") {}

    SUBCASE ("the device's general go-back command")
    {
        REQUIRE (rig.document.setAttribute ("/godot/mount/" + std::string (lightingDesk) + "/dohRollback",
                                            "/lx/key/go_back I").ok);
        address = "/lx/key/go_back";
        expected = osc::Value::impulse();
        words = "rolled back with /lx/key/go_back I";
    }

    SUBCASE ("the cue's own: an intermediate cue number")
    {
        REQUIRE (rig.document.setAttribute ("/godot/mount/" + std::string (lightingDesk) + "/dohRollback",
                                            "/lx/key/go_back I").ok);
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + cue + "/dohRollback", "/lx/go f:11.5").ok);
        expected = osc::Value::float32 (11.5f);
        words = "rolled back with /lx/go f:11.5";
    }

    const auto valuesOn = [&rig] (const std::string& at)
    {
        rig.received (at);
        std::vector<osc::Value> values;

        for (const auto& datagram : rig.listener.all())
        {
            const auto decoded = osc::decode (datagram.bytes.data(), datagram.bytes.size());

            if (decoded.ok && decoded.packet.address == at && ! decoded.packet.args.empty())
                values.push_back (decoded.packet.args.front());
        }

        return values;
    };

    REQUIRE (rig.press ("standby.set", { osc::Value::string (cue) }).rejected == 0);
    rig.tickOnce();
    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (2);
    REQUIRE (valuesOn ("/lx/go") == std::vector<osc::Value> { osc::Value::int32 (12) });

    REQUIRE (rig.press ("go.doh").rejected == 0);
    rig.ticks (2);

    const auto said = reportOf (rig);
    INFO (said);
    CHECK (says (said, "Lights: " + words + " (Desk) - the next GO sends it again"));
    CHECK_FALSE (says (said, "could not be taken back"));
    CHECK (engineSetsOn (rig, address) == 1u);
    REQUIRE_FALSE (valuesOn (address).empty());
    CHECK (valuesOn (address).back() == expected);

    //  A second press sends nothing more - here refused, nothing heard was
    //  left to resume; and a press that forgets a resume returns before it.
    const auto sentBefore = rig.sender.sentFor (lightingDesk);
    rig.ticks (30);
    rig.press ("go.doh");
    rig.ticks (2);
    CHECK (rig.sender.sentFor (lightingDesk) == sentBefore);

    //  The corrected GO sends the cue again, as Undo(h) always did.
    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (2);
    CHECK (valuesOn ("/lx/go").back() == osc::Value::int32 (12));

    replaysTheSame (rig);
}

TEST_CASE ("go.doh: no rollback to send leaves 'could not be taken back'; one that is not a message is said, and nothing goes")
{
    DohRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/mount/" + std::string (lightingDesk) + "/doh", "takeBack").ok);

    const auto cue = rig.makeOsc ("/lx/go", "i:12", "none");
    rig.document.createCue (rig.listId, rig.index++, "memo", "After");

    auto bad = false;

    SUBCASE ("nothing before it on the desk, and no general command") {}

    SUBCASE ("a rollback typed wrong")
    {
        bad = true;
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + cue + "/dohRollback", "/lx/go 11").ok);
    }

    REQUIRE (rig.press ("standby.set", { osc::Value::string (cue) }).rejected == 0);
    rig.tickOnce();
    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (2);
    const auto sent = rig.sender.sentFor (lightingDesk);

    REQUIRE (rig.press ("go.doh").rejected == 0);
    rig.ticks (2);

    const auto said = reportOf (rig);
    INFO (said);
    CHECK (says (said, "Desk: could not be taken back - the next GO sends it again"));
    CHECK (says (said, "is not a message") == bad);
    CHECK (rig.sender.sentFor (lightingDesk) == sent);
}

TEST_CASE ("go.doh: a rollback is sent once per device - the first sent cue's - and never to a device left to its operator")
{
    /*  A scene sent the desk its cue 12, then an effect: rolled back in turn,
        the effect's previous command - cue 12 itself - would leave the desk
        at 12, so only the first cue's - where the desk stood before the GO -
        goes. And Meh never reads the field. */
    DohRig rig;

    const auto previous = rig.makeOsc ("/lx/go", "i:11", "none");
    juce::ignoreUnused (previous);

    const auto scene = rig.document.createCue (rig.listId, rig.index++, "group", "Scene").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + scene + "/mode", "timeline").ok);

    int at = 0;

    for (const auto& [address, atom] : std::vector<std::pair<std::string, std::string>> {
             { "/lx/go", "i:12" }, { "/lx/fx", "i:3" } })
    {
        const auto member = rig.document.createCue (scene, at++, "osc", "Member").id;
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + member + "/address", address).ok);
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + member + "/value", atom).ok);
    }

    rig.document.createCue (rig.listId, rig.index++, "memo", "After");

    auto takesBack = true;

    SUBCASE ("Undo(h)")
    {
        REQUIRE (rig.document.setAttribute ("/godot/mount/" + std::string (lightingDesk) + "/doh", "takeBack").ok);
    }

    SUBCASE ("Meh")
    {
        takesBack = false;
    }

    REQUIRE (rig.press ("standby.set", { osc::Value::string (scene) }).rejected == 0);
    rig.tickOnce();
    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (3);
    REQUIRE (rig.received ("/lx/go") == 1u);
    REQUIRE (rig.received ("/lx/fx") == 1u);

    REQUIRE (rig.press ("go.doh").rejected == 0);
    rig.ticks (5);

    const auto said = reportOf (rig);
    INFO (said);
    CHECK (engineSetsOn (rig, "/lx/go") == (takesBack ? 1u : 0u));
    CHECK (rig.received ("/lx/go") == (takesBack ? 2u : 1u));
    CHECK (rig.received ("/lx/fx") == 1u);
    CHECK (says (said, "rolled back with /lx/go i:11") == takesBack);
    CHECK_FALSE (says (said, "rolled back with /lx/go i:12"));
}

//==============================================================================
/*  D3'S REVIEW (2026-10-03, namespace draft §24.13): each case was written
    first and run on bd25dd5 with D4 on top (204ac69), and failed there. */
TEST_CASE ("jump: a value the jump puts on the desk lands before a member its seat fires on its first tick")
{
    /*  The review's second finding, and a regression for every jump, in a show
        that never presses Doh!. A cue writes the fader 0.2; the scene after it
        writes 0.8 a hundredth of a second in. A jump to the scene's start wants
        0.2 on the desk now and seats the scene's member due a tick later. D3
        moved the jump's values to a hook on the next tick, which runs after
        the waits that fire that member - so the member wrote 0.8 and the jump's
        0.2 landed on top of it, the desk left at the earlier cue's value. */
    NetworkRig rig;

    const auto first = rig.makeOsc ("/desk/fader", "f:0.2", "none");
    juce::ignoreUnused (first);

    const auto scene = rig.document.createCue (rig.listId, rig.index++, "group", "Scene").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + scene + "/mode", "timeline").ok);
    const auto member = rig.document.createCue (scene, 0, "osc", "Fader").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + member + "/address", "/desk/fader").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + member + "/value", "f:0.8").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + member + "/preWait", "0.01").ok);
    rig.document.createCue (rig.listId, rig.index++, "memo", "After");

    REQUIRE (rig.engine.submit ("cli", "list.aim", { osc::Value::string (rig.listId), osc::Value::string (scene),
                                                     osc::Value::float64 (0.0) }));
    rig.tickOnce();
    REQUIRE (rig.engine.submit ("cli", "list.loadToTime", { osc::Value::string (rig.listId) }));
    rig.tickOnce();

    for (int n = 0; n < 5; ++n)
        rig.tickOnce();

    INFO (rig.engine.log().contents());
    REQUIRE (rig.mounts.valueOf ("/desk/fader") != nullptr);
    CHECK (*rig.mounts.valueOf ("/desk/fader") == osc::Values { osc::Value::float32 (0.8f) });
}

TEST_CASE ("go.doh: a jump in the Doh's own drain wins the desk over the Doh's put-back")
{
    /*  The review's seventh finding: the flush sent the values a jump wants
        before the Doh's desk entries, so a Doh and a load-to-time in one drain
        put the value from before the GO over the jump's. The desk entries go
        first now, and none is sent for an address the jump sets. On bd25dd5
        the desk ended at the value from before the GO. */
    DohRig rig;
    REQUIRE (rig.document.setAttribute ("/godot/mount/K3PV7WRB/doh", "takeBack").ok);
    REQUIRE (rig.mounts.write ("/desk/fader", osc::Value::float32 (0.25f)).ok);

    const auto cue = rig.makeOsc ("/desk/fader", "f:0.75", "none");
    rig.document.createCue (rig.listId, rig.index++, "memo", "After");

    rig.park (cue);
    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (2);
    REQUIRE (*rig.mounts.valueOf ("/desk/fader") == osc::Values { osc::Value::float32 (0.75f) });

    REQUIRE (rig.press ("list.aim", { osc::Value::string (rig.listId), osc::Value::string (cue),
                                      osc::Value::float64 (0.0) }).rejected == 0);

    rig.engine.submit ("cli", "go.doh", {});
    rig.engine.submit ("cli", "list.loadToTime", { osc::Value::string (rig.listId) });
    rig.tickOnce();
    rig.ticks (3);

    CHECK (*rig.mounts.valueOf ("/desk/fader") == osc::Values { osc::Value::float32 (0.75f) });
}

TEST_CASE ("go.doh: what was left with a desk's operator survives a GO into its act cut short by Esc before the cue was spawned")
{
    /*  The review's eleventh point (NN's hole). The corrected GO enters an act
        afresh inside an automatic sequence, which spawns the marked cue a tick
        later; an Esc in the GO's own drain stops the act before it does. The
        cue's entry was consumed by that GO all the same, and the next GO that
        reached the cue sent it to the lighting desk a second time. It lives
        now until a run of the cue claims it. On bd25dd5 the desk got it twice. */
    DohRig rig;

    const auto outer = rig.document.createCue (rig.listId, rig.index++, "group", "Sequence").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + outer + "/advance", "auto").ok);
    const auto act = rig.document.createCue (outer, 0, "group", "Act").id;
    const auto one = rig.document.createCue (act, 0, "memo", "One").id;
    const auto lx = rig.document.createCue (act, 1, "osc", "Lights twelve").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + lx + "/address", "/lx/go").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + lx + "/value", "i:12").ok);
    const auto hold = rig.document.createCue (outer, 1, "memo", "Hold").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + hold + "/preWait", "30").ok);
    rig.document.createCue (rig.listId, rig.index++, "memo", "After");

    REQUIRE (rig.press ("cue.fire", { osc::Value::string (outer) }).rejected == 0);
    rig.ticks (5);

    rig.park (one);
    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (8);
    REQUIRE (rig.press ("go").rejected == 0);               // the lights, early
    rig.ticks (8);
    REQUIRE (rig.received ("/lx/go") == 1u);

    REQUIRE (rig.press ("go.doh").rejected == 0);
    REQUIRE (rig.standby() == lx);

    //  The corrected GO, and an Esc in its own drain, before the act spawns the cue.
    rig.engine.submit ("cli", "go", {});
    rig.engine.submit ("cli", "run.stopAll", {});
    rig.tickOnce();
    rig.ticks (60);

    rig.park (lx);
    REQUIRE (rig.press ("go").rejected == 0);
    rig.ticks (20);

    CHECK (rig.received ("/lx/go") == 1u);
}

//==============================================================================
/*  SEVERAL VALUES IN ONE MESSAGE (namespace draft §45). An OSC cue's value is
    a list of atoms - every argument of the message - and what arrives is one
    message carrying all of them: ADM-OSC moves an object with
    `/adm/obj/<n>/xyz x y z`, and three messages would be three moves. */
namespace
{
    constexpr const char* objectJson = R"JSON({
      "FULL_PATH": "/obj",
      "CONTENTS": {
        "xyz": { "FULL_PATH": "/obj/xyz", "TYPE": "fff", "ACCESS": 3 }
      }
    })JSON";

    tree::MountDeclaration objectMount (int port)
    {
        tree::MountDeclaration mount;
        mount.id = "0BJ4CT00";
        mount.prefix = "/obj";
        mount.namespaceFile = "namespaces/obj.json";
        mount.host = "127.0.0.1";
        mount.port = port;
        return mount;
    }

    tree::MountDeclaration opaqueMount (int port)
    {
        tree::MountDeclaration mount;
        mount.id = "LX000001";
        mount.prefix = "/lx";
        mount.host = "127.0.0.1";
        mount.port = port;
        return mount;
    }

    osc::Packet decodedFrom (const osc::Datagram& datagram)
    {
        const auto decoded = osc::decode (datagram.bytes.data(), datagram.bytes.size());
        REQUIRE (decoded.ok);
        return decoded.packet;
    }
}

TEST_CASE ("network cue: several values in one message leave as that message's arguments")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    NetworkRig rig;
    REQUIRE (rig.mounts.load (objectMount (rig.listener.port()), objectJson).ok);

    const auto cueId = rig.makeOsc ("/obj/xyz", "i:1 f:2.5 d:-3", "none");
    rig.fire (cueId);
    rig.tickOnce();

    CHECK (rig.runOf (cueId)->state == cue::runState::done);

    //  Coerced each to its own tag, in the tree and on the wire.
    const auto three = osc::Values { osc::Value::float32 (1.0f), osc::Value::float32 (2.5f),
                                     osc::Value::float32 (-3.0f) };
    REQUIRE (rig.mounts.valueOf ("/obj/xyz") != nullptr);
    CHECK (*rig.mounts.valueOf ("/obj/xyz") == three);

    REQUIRE (rig.listener.waitFor (1));
    const auto packet = decodedFrom (rig.listener.all().front());
    CHECK (packet.address == "/obj/xyz");
    CHECK (packet.args == three);
}

TEST_CASE ("network cue: a described node refuses a message missing a value")
{
    NetworkRig rig;
    REQUIRE (rig.mounts.load (objectMount (rig.listener.port()), objectJson).ok);

    const auto cueId = rig.makeOsc ("/obj/xyz", "f:0.5 f:1", "none");
    rig.fire (cueId);
    rig.tickOnce();

    CHECK (rig.runOf (cueId)->state == cue::runState::failed);
    CHECK (rig.runOf (cueId)->error == reason::typeMismatch);
    CHECK_FALSE (rig.listener.waitFor (1, 150));
}

TEST_CASE ("network cue: an opaque device is sent the list as spelled, nothing at all included")
{
    NetworkRig rig;
    REQUIRE (rig.mounts.declare (opaqueMount (rig.listener.port())).ok);

    SUBCASE ("an empty value is a message that carries nothing")
    {
        const auto cueId = rig.makeOsc ("/lx/go", "", "none");
        rig.fire (cueId);
        rig.tickOnce();

        CHECK (rig.runOf (cueId)->state == cue::runState::done);
        REQUIRE (rig.listener.waitFor (1));

        const auto packet = decodedFrom (rig.listener.all().front());
        CHECK (packet.address == "/lx/go");
        CHECK (packet.args.empty());
    }

    SUBCASE ("a string with spaces and a number are two arguments")
    {
        const auto cueId = rig.makeOsc ("/lx/cmd", "s:\"Go To Cue\" i:11", "none");
        rig.fire (cueId);
        REQUIRE (rig.listener.waitFor (1));

        const auto packet = decodedFrom (rig.listener.all().front());
        CHECK (packet.args == osc::Values { osc::Value::string ("Go To Cue"), osc::Value::int32 (11) });
    }

    SUBCASE ("a value that does not spell is the run's failure, as one atom always was")
    {
        const auto cueId = rig.makeOsc ("/lx/cmd", "i:11 nonsense", "none");
        rig.fire (cueId);
        rig.tickOnce();

        CHECK (rig.runOf (cueId)->state == cue::runState::failed);
        CHECK (rig.runOf (cueId)->error == reason::typeMismatch);
    }
}

TEST_CASE ("node.set: a device's node of several takes them all, and a row of the show takes one")
{
    NetworkRig rig;
    REQUIRE (rig.mounts.load (objectMount (rig.listener.port()), objectJson).ok);

    rig.engine.submit ("udp:10.0.0.5:9000", "node.set",
                       { osc::Value::string ("/obj/xyz"), osc::Value::float32 (0.5f),
                         osc::Value::float32 (1.5f), osc::Value::float32 (2.5f) });
    rig.tickOnce();

    REQUIRE (rig.listener.waitFor (1));
    CHECK (decodedFrom (rig.listener.all().front()).args
             == osc::Values { osc::Value::float32 (0.5f), osc::Value::float32 (1.5f), osc::Value::float32 (2.5f) });

    /*  A ROW OF THE SHOW holds one value, and a write that tacks a second on
        is refused rather than read for its first. */
    const auto cueId = rig.makeOsc ("/desk/fader", "f:0.5", "none");
    const auto result = rig.engine.submit ("cli", "node.set",
                                           { osc::Value::string ("/godot/cue/" + cueId + "/timeout"),
                                             osc::Value::float64 (2.0), osc::Value::float64 (3.0) });
    CHECK (result);
    const auto tick = rig.engine.processTick (rig.tick++);
    CHECK (tick.rejected == 1u);
    CHECK (rig.document.getAttribute ("/godot/cue/" + cueId + "/timeout").value_or ("") != "2");
}

//==============================================================================
/*  SEVERAL MESSAGES IN ONE CUE (namespace draft 45, YP): the cue's own address
    and value first, then each <Message> in its order, all in the tick it
    fires, all to one device (YV). */
TEST_CASE ("network cue: several messages leave in their order, in the tick it fires")
{
    NetworkRig rig;

    const auto cueId = rig.makeOsc ("/desk/fader", "f:0.25", "sent");
    REQUIRE (rig.document.createMessage (cueId, "/desk/scene", "i:3").ok);

    rig.fire (cueId);
    REQUIRE (rig.listener.waitFor (2));

    const auto arrived = rig.listener.all();
    CHECK (decodedFrom (arrived[0]).address == "/desk/fader");
    CHECK (decodedFrom (arrived[1]).address == "/desk/scene");
    CHECK (decodedFrom (arrived[1]).args == osc::Values { osc::Value::int32 (3) });

    //  A `sent` wait is done when every message has left.
    rig.tickOnce();
    CHECK (rig.runOf (cueId)->state == cue::runState::done);
}

TEST_CASE ("network cue: a further message under another device fails the run before anything is written")
{
    NetworkRig rig;
    REQUIRE (rig.mounts.load (objectMount (rig.listener.port()), objectJson).ok);

    const auto cueId = rig.makeOsc ("/desk/fader", "f:0.25", "none");
    REQUIRE (rig.document.createMessage (cueId, "/obj/xyz", "f:1 f:2 f:3").ok);

    //  And the show says so before anybody presses GO, from the devices it declares.
    REQUIRE (rig.document.createMount ("/desk", "namespaces/desk.json", "K3PV7WRB").ok);
    REQUIRE (rig.document.createMount ("/obj", "namespaces/obj.json", "0BJ4CT00").ok);
    const auto warnings = rig.document.warnings();
    CHECK (std::any_of (warnings.begin(), warnings.end(),
                        [] (const std::string& w) { return w.find ("under another device") != std::string::npos; }));

    rig.fire (cueId);
    rig.tickOnce();

    CHECK (rig.runOf (cueId)->state == cue::runState::failed);
    CHECK (rig.runOf (cueId)->error == cue::oscError::severalDevices);
    CHECK (rig.mounts.valueOf ("/desk/fader") == nullptr);
    CHECK_FALSE (rig.listener.waitFor (1, 150));
}

TEST_CASE ("network cue: a refused further message fails the run, and what was written before it still leaves")
{
    NetworkRig rig;

    const auto cueId = rig.makeOsc ("/desk/fader", "f:0.25", "none");
    REQUIRE (rig.document.createMessage (cueId, "/desk/meter", "f:0.5").ok);

    rig.fire (cueId);
    rig.tickOnce();

    CHECK (rig.runOf (cueId)->state == cue::runState::failed);
    CHECK (rig.runOf (cueId)->error == reason::readOnly);

    //  The tree and the wire agree on the one that landed.
    REQUIRE (rig.mounts.valueOf ("/desk/fader") != nullptr);
    REQUIRE (rig.listener.waitFor (1));
    CHECK (decodedFrom (rig.listener.all().front()).address == "/desk/fader");
}

TEST_CASE ("network cue: with tx off every message lands in the tree and none leaves")
{
    NetworkRig rig;

    auto declaration = consoleMount (rig.listener.port());
    declaration.tx = false;
    REQUIRE (rig.mounts.updateDeclaration (declaration));

    const auto cueId = rig.makeOsc ("/desk/fader", "f:0.25", "none");
    REQUIRE (rig.document.createMessage (cueId, "/desk/scene", "i:3").ok);

    rig.fire (cueId);
    rig.tickOnce();

    CHECK (rig.runOf (cueId)->state == cue::runState::done);
    CHECK (rig.runOf (cueId)->warning == std::string (cue::runWarning::notSent));
    CHECK (rig.mounts.valueOf ("/desk/fader") != nullptr);
    CHECK (rig.mounts.valueOf ("/desk/scene") != nullptr);
    CHECK_FALSE (rig.listener.waitFor (1, 150));
}

//==============================================================================
/*  BUNDLES, THE DEVICE'S SETTING (namespace draft 45, YQ). What one flush sends
    a device that takes them leaves as one bundle, in the order it was queued,
    time-tagged immediately; a datagram is closed at 1200 bytes and a message
    too big to share one goes alone; a message the rate cap holds stays out. */
namespace
{
    tree::MountSender::Destination bundling (int port, double rateCap = 0.0)
    {
        return { "127.0.0.1", port, rateCap, true };
    }

    std::vector<osc::Packet> elementsOf (const osc::Datagram& datagram)
    {
        const auto packet = decodedFrom (datagram);
        REQUIRE (packet.isBundle());
        CHECK (packet.time.raw == 1u);
        return packet.elements;
    }
}

TEST_CASE ("mount sender: a device that takes bundles gets one tick's messages as one, in their order")
{
    Listener listener;
    tree::MountSender sender { listener.endpoint };

    const auto x = sender.queue ("W0RKS000", bundling (listener.port()), "/wfs/input/1/positionX", osc::Value::float32 (1.0f));
    const auto y = sender.queue ("W0RKS000", bundling (listener.port()), "/wfs/input/1/positionY", osc::Value::float32 (2.0f));
    const auto z = sender.queue ("W0RKS000", bundling (listener.port()), "/wfs/input/1/positionZ", osc::Value::float32 (3.0f));
    sender.flush();

    REQUIRE (listener.waitFor (1));
    CHECK_FALSE (listener.waitFor (2, 150));

    const auto elements = elementsOf (listener.all().front());
    REQUIRE (elements.size() == 3u);
    CHECK (elements[0].address == "/wfs/input/1/positionX");
    CHECK (elements[1].address == "/wfs/input/1/positionY");
    CHECK (elements[2].address == "/wfs/input/1/positionZ");
    CHECK (elements[2].args == osc::Values { osc::Value::float32 (3.0f) });

    //  Each message is answered, and counted, as itself.
    for (const auto ticket : { x, y, z })
        CHECK (sender.outcomeOf (ticket) == tree::MountSender::Outcome::sent);

    CHECK (sender.sentFor ("W0RKS000") == 3u);
}

TEST_CASE ("mount sender: a bundle is closed at 1200 bytes, and a message too big for one goes alone")
{
    Listener listener;
    tree::MountSender sender { listener.endpoint };

    //  Forty messages of about sixty bytes: more than one datagram's worth.
    for (int n = 0; n < 40; ++n)
        sender.queue ("W0RKS000", bundling (listener.port()),
                      "/wfs/input/" + std::to_string (n + 1) + "/positionX/and/some/more/words",
                      osc::Value::float32 (static_cast<float> (n)));

    //  And one that no bundle could hold.
    sender.queue ("W0RKS000", bundling (listener.port()), "/wfs/big", osc::Value::string (std::string (1300, 'x')));
    sender.flush();

    REQUIRE (listener.waitFor (3));
    std::this_thread::sleep_for (std::chrono::milliseconds (100));

    std::vector<std::string> addresses;
    auto plain = 0;

    for (const auto& datagram : listener.all())
    {
        const auto packet = decodedFrom (datagram);

        if (! packet.isBundle())
        {
            ++plain;
            CHECK (packet.address == "/wfs/big");
            continue;
        }

        CHECK (datagram.bytes.size() <= tree::MountSender::bundleBytes);

        for (const auto& element : packet.elements)
            addresses.push_back (element.address);
    }

    CHECK (plain == 1);
    REQUIRE (addresses.size() == 40u);
    CHECK (addresses.front() == "/wfs/input/1/positionX/and/some/more/words");
    CHECK (addresses.back() == "/wfs/input/40/positionX/and/some/more/words");
    CHECK (sender.sentFor ("W0RKS000") == 41u);
}

TEST_CASE ("mount sender: a message the rate cap holds stays out of the bundle and goes in a later one")
{
    Listener listener;
    tree::MountSender sender { listener.endpoint };

    //  Ten a second is a send every fifth flush.
    sender.queue ("W0RKS000", bundling (listener.port(), 10.0), "/wfs/slow", osc::Value::float32 (1.0f));
    sender.flush();
    REQUIRE (listener.waitFor (1));

    sender.queue ("W0RKS000", bundling (listener.port(), 10.0), "/wfs/slow", osc::Value::float32 (2.0f));
    sender.queue ("W0RKS000", bundling (listener.port(), 10.0), "/wfs/other", osc::Value::float32 (3.0f));
    sender.flush();

    REQUIRE (listener.waitFor (2));
    const auto second = elementsOf (listener.all()[1]);
    REQUIRE (second.size() == 1u);
    CHECK (second.front().address == "/wfs/other");
    CHECK (sender.pending() == 1u);
}

TEST_CASE ("mount sender: a device that does not take bundles is sent each message alone, beside one that does")
{
    Listener listener;
    tree::MountSender sender { listener.endpoint };

    sender.queue ("W0RKS000", bundling (listener.port()), "/wfs/a", osc::Value::float32 (1.0f));
    sender.queue ("DESK0001", { "127.0.0.1", listener.port(), 0.0, false }, "/desk/a", osc::Value::float32 (1.0f));
    sender.queue ("W0RKS000", bundling (listener.port()), "/wfs/b", osc::Value::float32 (1.0f));
    sender.queue ("DESK0001", { "127.0.0.1", listener.port(), 0.0, false }, "/desk/b", osc::Value::float32 (1.0f));
    sender.flush();

    REQUIRE (listener.waitFor (3));
    CHECK_FALSE (listener.waitFor (4, 150));

    auto bundles = 0;
    auto plain = 0;

    for (const auto& datagram : listener.all())
        (decodedFrom (datagram).isBundle() ? bundles : plain)++;

    CHECK (bundles == 1);
    CHECK (plain == 2);
}

//==============================================================================
/*  CURVES, PLAYED (namespace draft 45, O.4). A cue's curves run on its own clock
    - seconds from the tick it began sending, one for every fifty ticks - and a
    message a curve moves is written each tick its values change, through the
    device's door and queue; at the end the last values hold and the run is done
    by its wait. */
namespace
{
    std::string curveOn (NetworkRig& rig, const std::string& parent, int arg, const std::string& points)
    {
        const auto made = rig.document.createCurve (parent, arg);
        REQUIRE (made.ok);
        REQUIRE (rig.document.setAttribute ("/godot/curve/" + made.id + "/points", points).ok);
        return made.id;
    }

    float faderOf (NetworkRig& rig, const std::string& address = "/desk/fader")
    {
        const auto* values = rig.mounts.valueOf (address);
        REQUIRE (values != nullptr);
        REQUIRE (values->size() == 1u);
        return values->front().getFloat32();
    }

    std::size_t sentTo (NetworkRig& rig, const std::string& address)
    {
        std::size_t count = 0;

        for (const auto& datagram : rig.listener.all())
            if (decodedFrom (datagram).address == address)
                ++count;

        return count;
    }
}

TEST_CASE ("network cue: a curve plays on the cue's own clock, sent as it changes, and holds its end")
{
    NetworkRig rig;

    const auto cueId = rig.makeOsc ("/desk/fader", "f:0", "none");
    curveOn (rig, cueId, 0, "0 0 1 1");

    rig.fire (cueId);

    //  Twenty-five ticks is half a second: half way up the ramp.
    for (int n = 0; n < 25; ++n)
        rig.tickOnce();

    CHECK (faderOf (rig) == doctest::Approx (0.5f));
    CHECK (rig.runOf (cueId)->state == cue::runState::playing);
    CHECK (rig.runOf (cueId)->position == doctest::Approx (0.5));

    for (int n = 0; n < 40; ++n)
        rig.tickOnce();

    //  Done at its end by its wait, the last value held.
    CHECK (rig.runOf (cueId)->state == cue::runState::done);
    CHECK (faderOf (rig) == doctest::Approx (1.0f));

    //  One datagram at GO and one a tick up the ramp - and none once it holds.
    REQUIRE (rig.listener.waitFor (51));
    const auto sent = rig.listener.count();

    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    CHECK_FALSE (rig.listener.waitFor (sent + 1, 150));
}

TEST_CASE ("network cue: a further message's curve moves its integer by whole numbers, and the rest is sent once")
{
    NetworkRig rig;

    const auto cueId = rig.makeOsc ("/desk/fader", "f:0.5", "none");
    const auto message = rig.document.createMessage (cueId, "/desk/scene", "i:0");
    REQUIRE (message.ok);
    curveOn (rig, message.id, 0, "0 0 1 10");

    rig.fire (cueId);

    for (int n = 0; n < 25; ++n)
        rig.tickOnce();

    const auto* scene = rig.mounts.valueOf ("/desk/scene");
    REQUIRE (scene != nullptr);
    CHECK (*scene == osc::Values { osc::Value::int32 (5) });

    for (int n = 0; n < 40; ++n)
        rig.tickOnce();

    CHECK (rig.runOf (cueId)->state == cue::runState::done);
    REQUIRE (rig.listener.waitFor (11));
    std::this_thread::sleep_for (std::chrono::milliseconds (100));

    //  The fader has no curve: GO's datagram and nothing more. The scene
    //  changes ten times, so ten more beside GO's.
    CHECK (sentTo (rig, "/desk/fader") == 1u);
    CHECK (sentTo (rig, "/desk/scene") == 11u);
}

TEST_CASE ("network cue: a looping curve goes round until Esc, which leaves it where it is")
{
    NetworkRig rig;

    const auto cueId = rig.makeOsc ("/desk/fader", "f:0", "none");
    curveOn (rig, cueId, 0, "0 0 1 1");
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + cueId + "/loop", "true").ok);

    rig.fire (cueId);

    //  A second and a half: the second round, half way.
    for (int n = 0; n < 75; ++n)
        rig.tickOnce();

    CHECK (rig.runOf (cueId)->state == cue::runState::playing);
    CHECK (rig.runOf (cueId)->iteration == 2);
    CHECK (faderOf (rig) == doctest::Approx (0.5f));

    REQUIRE (rig.engine.submit ("cli", "run.stopAll", {}));
    rig.tickOnce();
    rig.tickOnce();

    CHECK (rig.runOf (cueId)->isFinished());
    const auto held = faderOf (rig);
    const auto revision = rig.mounts.valueRevision();

    for (int n = 0; n < 20; ++n)
        rig.tickOnce();

    CHECK (rig.mounts.valueRevision() == revision);
    CHECK (faderOf (rig) == doctest::Approx (held));
}

TEST_CASE ("network cue: a duration shorter than the curve cuts it there, and a seek moves its clock")
{
    NetworkRig rig;

    SUBCASE ("cut short")
    {
        const auto cueId = rig.makeOsc ("/desk/fader", "f:0", "none");
        curveOn (rig, cueId, 0, "0 0 1 1");
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + cueId + "/duration", "0.5").ok);

        rig.fire (cueId);

        for (int n = 0; n < 30; ++n)
            rig.tickOnce();

        CHECK (rig.runOf (cueId)->state == cue::runState::done);
        CHECK (faderOf (rig) == doctest::Approx (0.5f));
    }

    SUBCASE ("sought")
    {
        const auto cueId = rig.makeOsc ("/desk/fader", "f:0", "none");
        curveOn (rig, cueId, 0, "0 0 2 2");

        rig.fire (cueId);
        rig.tickOnce();

        const auto* run = rig.runOf (cueId);
        REQUIRE (run != nullptr);
        CHECK (run->seekable);

        REQUIRE (rig.engine.submit ("cli", "run.seek", { osc::Value::string (run->id), osc::Value::float64 (1.5) }));
        rig.tickOnce();
        rig.tickOnce();

        CHECK (faderOf (rig) == doctest::Approx (1.52f));
    }
}

TEST_CASE ("network cue: with tx off a curve moves the tree and sends nothing, and a double Esc ends it")
{
    NetworkRig rig;

    SUBCASE ("tx off")
    {
        auto declaration = consoleMount (rig.listener.port());
        declaration.tx = false;
        REQUIRE (rig.mounts.updateDeclaration (declaration));

        const auto cueId = rig.makeOsc ("/desk/fader", "f:0", "none");
        curveOn (rig, cueId, 0, "0 0 0.5 1");

        rig.fire (cueId);

        for (int n = 0; n < 40; ++n)
            rig.tickOnce();

        CHECK (rig.runOf (cueId)->state == cue::runState::done);
        CHECK (rig.runOf (cueId)->warning == std::string (cue::runWarning::notSent));
        CHECK (faderOf (rig) == doctest::Approx (1.0f));
        CHECK_FALSE (rig.listener.waitFor (1, 150));
    }

    SUBCASE ("double Esc")
    {
        const auto cueId = rig.makeOsc ("/desk/fader", "f:0", "none");
        curveOn (rig, cueId, 0, "0 0 2 1");

        rig.fire (cueId);

        for (int n = 0; n < 10; ++n)
            rig.tickOnce();

        REQUIRE (rig.engine.submit ("cli", "run.killAll", {}));
        rig.tickOnce();
        rig.tickOnce();

        CHECK (rig.runOf (cueId)->isFinished());
        const auto revision = rig.mounts.valueRevision();

        for (int n = 0; n < 20; ++n)
            rig.tickOnce();

        CHECK (rig.mounts.valueRevision() == revision);
    }
}

//==============================================================================
/*  CURVES, RECORDED (namespace draft 45, O.9). An OSC cue armed, a curve of it
    armed, a pass: the cue plays on its clock and each armed curve is written
    from the first value the device reports for it - heard (O.8), never a
    read-back - or a hand rides it, until the pass stops; then every curve is
    written in one `node.setMany`, one step of undo. Go.dot sends nothing of a
    message the device is reporting, and sends what a hand rides. */
namespace
{
    struct CurveRig : NetworkRig
    {
        CurveRig()
        {
            tree::registerMountCommands (engine.commands(), document, mounts, nowhere);
            cue::registerCurveCommands (engine.commands(), engine, runner, document, curves);
            runner.setCurves (&curves);
        }

        bool submit (const std::string& name, std::vector<osc::Value> args = {})
        {
            return engine.submit ("cli", name, std::move (args));
        }

        /** What the console reports its fader holds, as a device with rx on does. */
        void hear (float value, const std::string& address = "/desk/fader")
        {
            REQUIRE (engine.submit ("mount:K3PV7WRB", "mount.heard",
                                    { osc::Value::string ("K3PV7WRB"), osc::Value::string (address),
                                      osc::Value::float32 (value) }));
        }

        void ticks (int count)
        {
            for (int n = 0; n < count; ++n)
                tickOnce();
        }

        std::vector<doc::LanePoint> pointsOf (const std::string& curveId)
        {
            const auto text = document.getAttribute ("/godot/curve/" + curveId + "/points").value_or (std::string {});
            const auto read = doc::readLane (text, std::nullopt);
            REQUIRE (read.problem.empty());
            return read.points;
        }

        /** The datagrams that have arrived, once the wire has gone quiet. */
        std::size_t settled()
        {
            auto count = listener.count();

            while (listener.waitFor (count + 1, 120))
                count = listener.count();

            return count;
        }

        juce::File nowhere;
        cue::CurveTable curves;
    };

    std::string armedPass (CurveRig& rig, const std::string& points, std::string& curveId)
    {
        const auto cueId = rig.makeOsc ("/desk/fader", "f:0", "none");
        curveId = curveOn (rig, cueId, 0, points);

        REQUIRE (rig.submit ("curve.arm", { osc::Value::string (cueId) }));
        REQUIRE (rig.submit ("curve.rec", { osc::Value::string (curveId), osc::Value::boolean (true) }));
        REQUIRE (rig.submit ("curve.record"));
        rig.tickOnce();

        REQUIRE (rig.curves.recording);
        REQUIRE (! rig.curves.run.empty());
        return cueId;
    }
}

TEST_CASE ("curve record: a pass writes what the device reports from its first report, sending none of it meanwhile")
{
    CurveRig rig;
    std::string curveId;
    const auto cueId = armedPass (rig, "0 0 4 4", curveId);

    //  Before a report the curve plays: the ramp goes out a tick at a time.
    rig.ticks (10);
    CHECK (faderOf (rig) > 0.0f);

    rig.hear (0.5f);
    rig.ticks (2);

    //  Latched: from here nothing of the fader's message is sent.
    const auto before = rig.settled();

    rig.ticks (10);
    rig.hear (0.8f);
    rig.ticks (10);

    CHECK_FALSE (rig.listener.waitFor (before + 1, 150));
    CHECK (rig.curves.rides[curveId].latched);
    CHECK (rig.curves.rides[curveId].value == doctest::Approx (0.8));

    //  (This rig cuts no transactions, as serve's hook does: one is opened
    //  here, so the undo below takes the pass and nothing before it.)
    rig.document.beginTransaction ("curve.stop", rig.tick, "window", {});
    REQUIRE (rig.submit ("curve.stop"));
    rig.ticks (3);

    CHECK_FALSE (rig.curves.recording);
    CHECK (rig.curves.lastPass.find (" kept ") != std::string::npos);
    CHECK (rig.curves.lastPass.find (curveId) != std::string::npos);

    //  A hand that ended the pass stopped the cue too.
    CHECK (rig.runOf (cueId)->isFinished());

    /*  THE CURVE IS WHAT WAS HEARD where the pass rode, and the ramp before
        the first report and after the stop. */
    const auto points = rig.pointsOf (curveId);
    CHECK (doc::laneValueAt (points, 0.1) == doctest::Approx (0.1).epsilon (0.02));
    CHECK (doc::laneValueAt (points, 0.4) == doctest::Approx (0.5));
    CHECK (doc::laneValueAt (points, 0.8) == doctest::Approx (0.8));
    CHECK (doc::laneValueAt (points, 3.0) == doctest::Approx (3.0).epsilon (0.02));

    //  ONE STEP OF UNDO takes the pass back whole.
    REQUIRE (rig.document.undo (doc::UndoDomain::document).has_value());
    CHECK (rig.document.getAttribute ("/godot/curve/" + curveId + "/points").value_or ("") == "0 0 4 4");
}

TEST_CASE ("curve record: a hand's ride is sent, so the device follows, and written from its first move")
{
    CurveRig rig;
    std::string curveId;
    armedPass (rig, "0 0 4 0", curveId);

    rig.ticks (10);
    const auto before = rig.settled();

    REQUIRE (rig.submit ("curve.ride", { osc::Value::string (curveId), osc::Value::float64 (0.7) }));
    rig.ticks (3);

    CHECK (faderOf (rig) == doctest::Approx (0.7f));
    CHECK (rig.listener.waitFor (before + 1));

    rig.ticks (10);
    REQUIRE (rig.submit ("curve.stop"));
    rig.ticks (3);

    const auto points = rig.pointsOf (curveId);
    CHECK (doc::laneValueAt (points, 0.1) == doctest::Approx (0.0));
    CHECK (doc::laneValueAt (points, 0.4) == doctest::Approx (0.7));
}

TEST_CASE ("curve record: Esc keeps the pass, a double Esc and the lock leave the curve as it was")
{
    CurveRig rig;
    std::string curveId;
    armedPass (rig, "0 0 4 0", curveId);

    rig.ticks (5);
    rig.hear (0.5f);
    rig.ticks (10);

    SUBCASE ("Esc")
    {
        REQUIRE (rig.submit ("run.stopAll"));
        rig.ticks (4);

        CHECK_FALSE (rig.curves.recording);
        CHECK (rig.curves.lastPass.find (" kept ") != std::string::npos);
        CHECK (doc::laneValueAt (rig.pointsOf (curveId), 0.25) == doctest::Approx (0.5));
    }

    SUBCASE ("double Esc")
    {
        REQUIRE (rig.submit ("run.killAll"));
        rig.ticks (4);

        CHECK_FALSE (rig.curves.recording);
        CHECK (rig.curves.lastPass.find (" dropped") != std::string::npos);
        CHECK (rig.document.getAttribute ("/godot/curve/" + curveId + "/points").value_or ("") == "0 0 4 0");
    }

    SUBCASE ("the lock")
    {
        REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
        REQUIRE (rig.submit ("curve.stop"));
        rig.ticks (4);

        CHECK (rig.curves.lastPass.find (" locked") != std::string::npos);
        CHECK (rig.document.getAttribute ("/godot/curve/" + curveId + "/points").value_or ("") == "0 0 4 0");

        //  And the arming is let go of, since the lock keeps every curve.
        rig.ticks (2);
        CHECK_FALSE (rig.curves.armed());
    }
}

TEST_CASE ("curve record: a read-back answer never latches a curve, and a pass nothing moved says so")
{
    CurveRig rig;
    std::string curveId;
    armedPass (rig, "0 0 4 0", curveId);

    rig.ticks (5);

    //  A sweep's answer, as `mount.readback` keeps it - not the device volunteering.
    rig.mounts.noteObservation ("/desk/fader", osc::Value::float32 (0.9f), rig.tick, -1);
    rig.ticks (5);

    CHECK_FALSE (rig.curves.rides[curveId].latched);

    REQUIRE (rig.submit ("curve.stop"));
    rig.ticks (3);

    CHECK (rig.curves.lastPass.find (" untouched") != std::string::npos);
    CHECK (rig.document.getAttribute ("/godot/curve/" + curveId + "/points").value_or ("") == "0 0 4 0");
}

TEST_CASE ("curve record: a curve with no point yet records from nothing, the cue's clock open until the pass stops")
{
    CurveRig rig;
    std::string curveId;
    const auto cueId = armedPass (rig, "", curveId);

    rig.ticks (5);
    rig.hear (0.25f);
    rig.ticks (100);

    //  Two seconds and more, and the cue still plays: nothing ends a pass on an empty curve but a stop.
    CHECK (rig.runOf (cueId)->state == cue::runState::playing);

    rig.hear (0.75f);
    rig.ticks (10);
    REQUIRE (rig.submit ("curve.stop"));
    rig.ticks (3);

    /*  THE RIDE IS THE CURVE, held at both ends rather than joined back to
        nought: the move ends where the device left it. */
    const auto points = rig.pointsOf (curveId);
    REQUIRE (! points.empty());
    CHECK (doc::laneValueAt (points, 0.0) == doctest::Approx (0.25));
    CHECK (doc::laneValueAt (points, 1.0) == doctest::Approx (0.25));
    CHECK (points.back().levelDb == doctest::Approx (0.75));
}

TEST_CASE ("curve record: the commands refuse what is not theirs")
{
    CurveRig rig;
    const auto cueId = rig.makeOsc ("/desk/fader", "f:0", "none");
    const auto curveId = curveOn (rig, cueId, 0, "0 0 1 1");
    const auto other = rig.makeOsc ("/desk/scene", "i:0", "none");
    const auto otherCurve = curveOn (rig, other, 0, "0 0 1 1");

    /*  The reason a command was refused with, read where a client reads it -
        or empty when it was applied. */
    const auto refusal = [&rig] (const std::string& name, std::vector<osc::Value> args = {}) -> std::string
    {
        rig.submit (name, std::move (args));
        const auto result = rig.engine.processTick (rig.tick++);

        if (result.rejected == 0)
            return {};

        const auto said = rig.engine.lastError();
        const auto end = said.rfind (" " + name);
        const auto start = said.rfind (' ', end - 1);
        return said.substr (start + 1, end - start - 1);
    };

    CHECK (refusal ("curve.rec", { osc::Value::string (curveId), osc::Value::boolean (true) }) == reason::notArmed);
    CHECK (refusal ("curve.record") == reason::notArmed);
    CHECK (refusal ("curve.arm", { osc::Value::string (rig.listId) }) == reason::badValue);

    CHECK (refusal ("curve.arm", { osc::Value::string (cueId) }).empty());
    CHECK (refusal ("curve.rec", { osc::Value::string (otherCurve), osc::Value::boolean (true) }) == reason::unknownId);
    CHECK (refusal ("curve.stop") == reason::notRunning);

    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
    CHECK (refusal ("curve.record") == reason::locked);
}

TEST_CASE ("curve record: a report of exactly what Go.dot sent is its own value coming back, and latches nothing")
{
    CurveRig rig;
    std::string curveId;
    armedPass (rig, "0 0 4 4", curveId);

    rig.ticks (10);

    //  The device answering with the value it was just sent - a motor fader, a server pushing to every listener.
    rig.hear (faderOf (rig));
    rig.tickOnce();
    CHECK_FALSE (rig.curves.rides[curveId].latched);

    //  A value of its own is a hand.
    rig.hear (3.5f);
    rig.ticks (2);
    CHECK (rig.curves.rides[curveId].latched);
    CHECK (rig.curves.rides[curveId].value == doctest::Approx (3.5));
}

TEST_CASE ("curve record: what to listen to is the armed curves' addresses, on a device that can be asked and is heard")
{
    CurveRig rig;

    const auto cueId = rig.makeOsc ("/desk/fader", "f:0", "none");
    const auto curveId = curveOn (rig, cueId, 0, "0 0 1 1");

    auto declaration = consoleMount (rig.listener.port());
    declaration.readback = "oscquery";
    declaration.queryPort = 5005;
    declaration.rx = true;
    REQUIRE (rig.mounts.updateDeclaration (declaration));

    //  Nothing armed, nothing wanted.
    CHECK (rig.runner.listenWanted().empty());

    REQUIRE (rig.submit ("curve.arm", { osc::Value::string (cueId) }));
    REQUIRE (rig.submit ("curve.rec", { osc::Value::string (curveId), osc::Value::boolean (true) }));
    rig.tickOnce();

    const auto wanted = rig.runner.listenWanted();
    REQUIRE (wanted.size() == 1u);
    REQUIRE (wanted.count ("K3PV7WRB") == 1u);
    CHECK (wanted.at ("K3PV7WRB").queryPort == 5005);
    CHECK (wanted.at ("K3PV7WRB").addresses == std::set<std::string> { "/desk/fader" });

    //  A device that is not heard is not listened to.
    declaration.rx = false;
    REQUIRE (rig.mounts.updateDeclaration (declaration));
    CHECK (rig.runner.listenWanted().empty());
}

TEST_CASE ("curve record: a hand's stop ends the cue's run once")
{
    CurveRig rig;
    std::string curveId;
    const auto cueId = armedPass (rig, "0 0 4 0", curveId);

    rig.ticks (5);
    rig.hear (0.5f);
    rig.ticks (10);

    REQUIRE (rig.submit ("curve.stop"));
    rig.ticks (6);

    CHECK (rig.runOf (cueId)->isFinished());

    const auto& said = rig.engine.log().contents();
    std::size_t ended = 0;

    for (auto at = said.find (" run.ended "); at != std::string::npos; at = said.find (" run.ended ", at + 1))
        ++ended;

    INFO (said);
    CHECK (ended == 1u);
}

TEST_CASE ("network cue: Esc ends a playing curve's run once")
{
    NetworkRig rig;

    const auto cueId = rig.makeOsc ("/desk/fader", "f:0", "none");
    curveOn (rig, cueId, 0, "0 0 4 1");

    rig.fire (cueId);

    for (int n = 0; n < 10; ++n)
        rig.tickOnce();

    REQUIRE (rig.engine.submit ("cli", "run.stopAll", {}));

    for (int n = 0; n < 6; ++n)
        rig.tickOnce();

    CHECK (rig.runOf (cueId)->isFinished());

    const auto& said = rig.engine.log().contents();
    std::size_t ended = 0;

    for (auto at = said.find (" run.ended "); at != std::string::npos; at = said.find (" run.ended ", at + 1))
        ++ended;

    INFO (said);
    CHECK (ended == 1u);
}

//==============================================================================
/*  A DEVICE ON A SERIAL PORT, reached by a cue (namespace draft §57, DP.1).
    `node.set` to such a device went down the port from the day it existed, and
    a cue aimed at it did not: the Runner spelled the destination out by hand
    and left the port off, so the datagram went to UDP port 0 and the run
    failed as one to nowhere. The destination is made in one place now,
    `MountSender::destinationFor`, and this is the case that fails if a caller
    stops taking it from there. */
TEST_CASE ("network cue: a cue, its further message and its curve aimed at a device on a serial port reach the port, and the network hears nothing")
{
    NetworkRig rig;

    tree::MountDeclaration onSerial;
    onSerial.id = "ARDU0001";
    onSerial.prefix = "/ardu";
    onSerial.namespaceFile = "namespaces/ardu.json";
    onSerial.transport = "serial";
    onSerial.serial = "SR000001";
    REQUIRE (rig.mounts.load (onSerial, R"JSON({ "FULL_PATH": "/ardu", "CONTENTS": {
        "led":  { "FULL_PATH": "/ardu/led",  "TYPE": "i", "ACCESS": 3, "VALUE": [0] },
        "dial": { "FULL_PATH": "/ardu/dial", "TYPE": "f", "ACCESS": 3, "VALUE": [0.0] } } })JSON").ok);

    std::vector<std::pair<std::string, std::string>> handed;   // the port, the address
    rig.sender.setSerialSink ([&handed] (const std::string& serialId, const std::vector<std::uint8_t>& packet)
    {
        const auto decoded = osc::decode (packet.data(), packet.size());
        handed.emplace_back (serialId, decoded.ok ? decoded.packet.address : std::string ("?"));
        return true;
    });

    const auto cueId = rig.makeOsc ("/ardu/dial", "f:0", "none");
    REQUIRE (rig.document.createMessage (cueId, "/ardu/led", "i:1").ok);
    curveOn (rig, cueId, 0, "0 0 1 1");

    rig.fire (cueId);

    //  The cue's two messages leave at GO; the curve's first move a tick later.
    for (int n = 0; n < 5; ++n)
        rig.tickOnce();

    REQUIRE (handed.size() >= 3u);
    CHECK (handed[0].first == "SR000001");
    CHECK (handed[0].second == "/ardu/dial");
    CHECK (handed[1].first == "SR000001");
    CHECK (handed[1].second == "/ardu/led");

    for (const auto& [serialId, address] : handed)
    {
        CHECK (serialId == "SR000001");
        CHECK (address.rfind ("/ardu/", 0) == 0);
    }

    REQUIRE (rig.runOf (cueId) != nullptr);
    CHECK (rig.runOf (cueId)->state == cue::runState::playing);

    //  And the network heard none of it: the port is where the device is.
    CHECK_FALSE (rig.listener.waitFor (1, 150));
}

//==============================================================================
/*  A PRESET'S NODES TAKE CURVES (namespace draft §57, AFI, the author's
    confirmation of 2026-10-10: "make sure once we have the device definitions
    that the values can be automated with automation curves as we have added
    to regular OSC cues"). A device made from a preset is a described device
    and a cue aimed at it is the OSC cue of §45, so a curve on its node plays
    through the device's door as on any other: this is the case that says so,
    against the shipped ADM-OSC preset - a one-argument gain and one argument
    of a three-argument position. */
TEST_CASE ("network cue: a curve on a preset's node plays through the described device, one argument of three included")
{
    NetworkRig rig;

    tree::PresetTable presets;
    presets.scan (std::string (WFG_REPO_ROOT) + "/presets/devices");
    const auto* adm = presets.find ("adm-osc");
    REQUIRE (adm != nullptr);
    REQUIRE (adm->usable());

    tree::MountDeclaration processor;
    processor.id = "ADMOSC01";
    processor.prefix = adm->rootRow();
    processor.namespaceFile = "namespaces/adm-osc-v1.json";
    processor.host = "127.0.0.1";
    processor.port = rig.listener.port();
    REQUIRE (rig.mounts.load (processor, adm->text).ok);
    CHECK (rig.mounts.nodeCount ("ADMOSC01") == static_cast<std::size_t> (adm->nodeCount));

    //  The gain, one argument, ramped over a second.
    const auto gain = rig.makeOsc ("/adm/obj/1/gain", "f:0", "none");
    curveOn (rig, gain, 0, "0 0 1 1");

    //  The position, three arguments, its Y alone ramped from behind to in front.
    const auto position = rig.makeOsc ("/adm/obj/1/xyz", "f:0 f:-1 f:0", "none");
    curveOn (rig, position, 1, "0 -1 1 1");

    rig.fire (gain);
    rig.fire (position);

    //  Twenty-five ticks into the gain's second, twenty-four into the position's.
    for (int n = 0; n < 24; ++n)
        rig.tickOnce();

    CHECK (faderOf (rig, "/adm/obj/1/gain") == doctest::Approx (0.5f).epsilon (0.05));

    const auto* xyz = rig.mounts.valueOf ("/adm/obj/1/xyz");
    REQUIRE (xyz != nullptr);
    REQUIRE (xyz->size() == 3u);
    CHECK ((*xyz)[0].getFloat32() == doctest::Approx (0.0f));
    CHECK ((*xyz)[1].getFloat32() == doctest::Approx (0.0f).epsilon (0.05));
    CHECK ((*xyz)[2].getFloat32() == doctest::Approx (0.0f));
    CHECK (rig.runOf (gain)->state == cue::runState::playing);
    CHECK (rig.runOf (position)->state == cue::runState::playing);

    for (int n = 0; n < 40; ++n)
        rig.tickOnce();

    CHECK (rig.runOf (gain)->state == cue::runState::done);
    CHECK (faderOf (rig, "/adm/obj/1/gain") == doctest::Approx (1.0f));
    CHECK ((*rig.mounts.valueOf ("/adm/obj/1/xyz"))[1].getFloat32() == doctest::Approx (1.0f));

    //  And every move left for the processor: the datagrams carry the preset's addresses.
    REQUIRE (rig.listener.waitFor (50));
    CHECK (sentTo (rig, "/adm/obj/1/gain") >= 25u);
    CHECK (sentTo (rig, "/adm/obj/1/xyz") >= 25u);

    //  A node the preset does not have is refused at GO, as any described device refuses it.
    const auto nowhere = rig.makeOsc ("/adm/obj/999/gain", "f:1", "none");
    rig.fire (nowhere);
    rig.tickOnce();
    REQUIRE (rig.runOf (nowhere) != nullptr);
    CHECK (rig.runOf (nowhere)->state == cue::runState::failed);
    CHECK (rig.runOf (nowhere)->error == reason::badAddress);
}

//==============================================================================
/*  DP.6: A DEVICE OVER A CONNECTION (namespace draft §57, AFJ) is sent to as
    one on a serial port is - its packets handed to a sink, serve's links table
    - and a link that cannot take them fails the run, as a closed port does. */
TEST_CASE ("network cue: a cue, its further message and its curve aimed at a device over a connection reach the link in order, and a link that cannot take it fails the run")
{
    NetworkRig rig;

    tree::MountDeclaration eos;
    eos.id = "EOS00001";
    eos.prefix = "/eos";
    eos.namespaceFile = "namespaces/eos.json";
    eos.transport = "tcp";
    eos.host = "127.0.0.1";
    eos.port = 3032;
    REQUIRE (rig.mounts.load (eos, R"JSON({ "FULL_PATH": "/eos", "CONTENTS": {
        "sub": { "FULL_PATH": "/eos/sub", "CONTENTS": {
            "1": { "FULL_PATH": "/eos/sub/1", "TYPE": "f", "ACCESS": 3, "VALUE": [0.0] } } },
        "key": { "FULL_PATH": "/eos/key", "CONTENTS": {
            "go_0": { "FULL_PATH": "/eos/key/go_0", "TYPE": "f", "ACCESS": 3, "VALUE": [0.0] } } } } })JSON").ok);

    std::vector<std::pair<std::string, std::string>> handed;   // the link, the address
    bool takes = true;
    rig.sender.setLinkSink ([&handed, &takes] (const std::string& mountId, const std::vector<std::uint8_t>& packet)
    {
        const auto decoded = osc::decode (packet.data(), packet.size());
        handed.emplace_back (mountId, decoded.ok ? decoded.packet.address : std::string ("?"));
        return takes;
    });

    const auto cueId = rig.makeOsc ("/eos/sub/1", "f:0", "none");
    REQUIRE (rig.document.createMessage (cueId, "/eos/key/go_0", "f:1").ok);
    curveOn (rig, cueId, 0, "0 0 1 1");

    rig.fire (cueId);

    //  The cue's two messages leave at GO; the curve's first move a tick later.
    for (int n = 0; n < 5; ++n)
        rig.tickOnce();

    REQUIRE (handed.size() >= 3u);
    CHECK (handed[0].first == "EOS00001");
    CHECK (handed[0].second == "/eos/sub/1");
    CHECK (handed[1].first == "EOS00001");
    CHECK (handed[1].second == "/eos/key/go_0");

    for (const auto& [mountId, address] : handed)
    {
        CHECK (mountId == "EOS00001");
        CHECK (address.rfind ("/eos/", 0) == 0);
    }

    REQUIRE (rig.runOf (cueId) != nullptr);
    CHECK (rig.runOf (cueId)->state == cue::runState::playing);

    //  And the network heard none of it: the link is where the device is.
    CHECK_FALSE (rig.listener.waitFor (1, 150));

    /*  A LINK THAT CANNOT TAKE IT - closed, retrying after the console went
        away - fails the run as a closed serial port does (AFJ): the Runner
        cannot see the link, and `sent` is the wait that says so. */
    takes = false;
    const auto refused = rig.makeOsc ("/eos/key/go_0", "f:1", "sent");
    rig.fire (refused);
    rig.tickOnce();

    REQUIRE (rig.runOf (refused) != nullptr);
    CHECK (rig.runOf (refused)->state == cue::runState::failed);
    CHECK (rig.runOf (refused)->error == cue::runError::sendFailed);
}

//==============================================================================
/*  DP.7: A CUE AIMED AT A CONSOLE ON THE RCP WIRE leaves as lines, its curve's
    moves as lines after it, in order; and what the console answers lands on
    the device through mount.replied (namespace draft §57, AFJ). */
TEST_CASE ("network cue: a cue and its curve aimed at a console on the rcp wire reach the link as lines in order, and its answer is kept")
{
    NetworkRig rig;

    tree::MountDeclaration yamaha;
    yamaha.id = "YAMA0001";
    yamaha.prefix = "/MIXER:Current";
    yamaha.namespaceFile = "namespaces/yamaha.json";
    yamaha.transport = "tcp";
    yamaha.wire = "rcp";
    yamaha.host = "127.0.0.1";
    yamaha.port = 49280;
    REQUIRE (rig.mounts.load (yamaha, R"JSON({ "FULL_PATH": "/MIXER:Current", "CONTENTS": {
        "InCh": { "FULL_PATH": "/MIXER:Current/InCh", "CONTENTS": {
            "Fader": { "FULL_PATH": "/MIXER:Current/InCh/Fader", "CONTENTS": {
                "Level": { "FULL_PATH": "/MIXER:Current/InCh/Fader/Level", "CONTENTS": {
                    "1": { "FULL_PATH": "/MIXER:Current/InCh/Fader/Level/1", "CONTENTS": {
                        "1": { "FULL_PATH": "/MIXER:Current/InCh/Fader/Level/1/1", "TYPE": "i", "ACCESS": 3, "VALUE": [0],
                               "RANGE": [{ "MIN": -32768, "MAX": 1000 }], "GODOT": { "RCP": { "VERB": "set", "XY": 2 } } } } } } },
                "On": { "FULL_PATH": "/MIXER:Current/InCh/Fader/On", "CONTENTS": {
                    "1": { "FULL_PATH": "/MIXER:Current/InCh/Fader/On/1", "CONTENTS": {
                        "1": { "FULL_PATH": "/MIXER:Current/InCh/Fader/On/1/1", "TYPE": "i", "ACCESS": 3, "VALUE": [0],
                               "GODOT": { "RCP": { "VERB": "set", "XY": 2 } } } } } } } } } } } } })JSON").ok);
    rig.sender.setMounts (&rig.mounts);

    std::vector<std::string> lines;
    rig.sender.setLinkSink ([&lines] (const std::string& mountId, const std::vector<std::uint8_t>& bytes)
    {
        CHECK (mountId == "YAMA0001");
        lines.emplace_back (bytes.begin(), bytes.end());
        return true;
    });

    const auto cueId = rig.makeOsc ("/MIXER:Current/InCh/Fader/Level/1/1", "i:-32768", "none");
    REQUIRE (rig.document.createMessage (cueId, "/MIXER:Current/InCh/Fader/On/1/1", "i:1").ok);
    curveOn (rig, cueId, 0, "0 -32768 1 0");

    rig.fire (cueId);

    for (int n = 0; n < 5; ++n)
        rig.tickOnce();

    REQUIRE (lines.size() >= 3u);
    CHECK (lines[0] == "set MIXER:Current/InCh/Fader/Level 0 0 -32768");
    CHECK (lines[1] == "set MIXER:Current/InCh/Fader/On 0 0 1");

    //  The curve's moves are lines of the same parameter, integers, climbing.
    for (std::size_t at = 2; at < lines.size(); ++at)
        CHECK (lines[at].rfind ("set MIXER:Current/InCh/Fader/Level 0 0 ", 0) == 0);

    REQUIRE (rig.runOf (cueId) != nullptr);
    CHECK (rig.runOf (cueId)->state == cue::runState::playing);
    CHECK_FALSE (rig.listener.waitFor (1, 150));

    //  What the console answered is the table's to keep (MountTests holds the command).
    rig.mounts.noteReply ("YAMA0001", "OK", "OK set MIXER:Current/InCh/Fader/Level 0 0 -32768");
    CHECK (rig.mounts.lastReplyOf ("YAMA0001") == "OK set MIXER:Current/InCh/Fader/Level 0 0 -32768");
}
