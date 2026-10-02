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
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/Ids.h>
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
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace wfg;

namespace
{
    /*  A namespace with three nodes: one writable float, one writable integer
        and one read-only. Hand-written rather than captured, so a refusal has
        something to refuse. */
    constexpr const char* consoleJson = R"JSON({
      "FULL_PATH": "/",
      "CONTENTS": {
        "fader": { "FULL_PATH": "/fader", "TYPE": "f", "ACCESS": 3 },
        "scene": { "FULL_PATH": "/scene", "TYPE": "i", "ACCESS": 3 },
        "meter": { "FULL_PATH": "/meter", "TYPE": "f", "ACCESS": 1 }
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
            return [this] (const std::string& address, const osc::Value& value)
            {
                const auto written = mounts.write (address, value);

                if (! written.ok)
                    return Outcome::rejected (written.reason);

                if (const auto* declared = mounts.declarationOf (written.mountId))
                    sender.queue (written.mountId,
                                  { declared->host, declared->port, declared->rateCap },
                                  address, written.value);

                return Outcome::ok ({ osc::Value::string (address), written.value });
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
    CHECK (*value == osc::Value::float32 (0.75f));

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
    CHECK (*value == osc::Value::float32 (0.75f));

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
    CHECK (*value == osc::Value::float32 (0.25f));

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
    CHECK (*rig.mounts.valueOf ("/desk/fader") == osc::Value::float32 (0.75f));
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
