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
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/osc/OscCodec.h>
#include <wfg/engine/osc/UdpEndpoint.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/MountSender.h>
#include <wfg/engine/tree/TreeCommands.h>

#include "TestSupport.h"

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
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

                if (const auto* declaration = mounts.declarationOf (written.mountId))
                    sender.queue (written.mountId,
                                  { declaration->host, declaration->port },
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
