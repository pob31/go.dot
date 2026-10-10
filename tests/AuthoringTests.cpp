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
    Authoring from a processor (namespace draft §56): a processor declares
    itself and writes cues into the show, and is answered.

    THE PROCESSOR IS A REAL SOCKET. Its declare and its captures are submitted
    under a `udp:` origin naming that socket's port, exactly as serve stamps a
    datagram, and the answers are read off it after the flush - so a case about
    an answer tests the bytes a processor would get, not a queue.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/Engine.h>
#include <wfg/engine/command/Command.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/log/Replay.h>
#include <wfg/engine/osc/OscCodec.h>
#include <wfg/engine/osc/UdpEndpoint.h>
#include <wfg/engine/tree/AuthoringCommands.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/MountFetcher.h>
#include <wfg/engine/tree/RawSender.h>
#include <wfg/engine/tree/TreeCommands.h>

#include "TestSupport.h"

#include <juce_core/juce_core.h>

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace wfg;

namespace
{
    osc::Value text (const std::string& s) { return osc::Value::string (s); }
    osc::Value number (int n) { return osc::Value::int32 (n); }

    /*  A socket that keeps what it was sent. */
    struct Socket
    {
        Socket()
        {
            const auto started = endpoint.start (0, [this] (osc::Datagram datagram)
                                                    {
                                                        const std::lock_guard<std::mutex> lock { guard };
                                                        received.push_back (std::move (datagram));
                                                    });
            REQUIRE (started);
        }

        ~Socket() { endpoint.stop(); }

        int port() const { return endpoint.boundPort(); }

        std::size_t count() const
        {
            const std::lock_guard<std::mutex> lock { guard };
            return received.size();
        }

        bool waitFor (std::size_t howMany, int millisecondsAtMost = 4000)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds (millisecondsAtMost);

            while (std::chrono::steady_clock::now() < deadline)
            {
                if (count() >= howMany)
                    return true;

                std::this_thread::sleep_for (std::chrono::milliseconds (2));
            }

            return count() >= howMany;
        }

        /** The address and string arguments of the `at`-th datagram. */
        std::vector<std::string> words (std::size_t at) const
        {
            const std::lock_guard<std::mutex> lock { guard };
            REQUIRE (at < received.size());

            const auto& bytes = received[at].bytes;
            const auto decoded = osc::decode (bytes.data(), bytes.size());
            REQUIRE (decoded.ok);

            std::vector<std::string> out { decoded.packet.address };

            for (const auto& value : decoded.packet.args)
                out.push_back (value.isString() ? value.getString()
                                                : value.isInt32() ? std::to_string (value.getInt32()) : std::string {});
            return out;
        }

        mutable std::mutex guard;
        std::vector<osc::Datagram> received;
        osc::UdpEndpoint endpoint;
    };

    struct AuthoringRig
    {
        AuthoringRig()
            : answers (goDot.endpoint)
        {
            engine.log().openInMemory ({});
            doc::registerDocumentCommands (engine.commands(), document);
            tree::registerAuthoringCommands (engine.commands(), document, answers);

            /*  One undo step per applied command, as serve cuts them. */
            engine.setBeforeApply ([this] (const Command& command, const Event& submitted,
                                           const std::vector<osc::Value>& coerced, std::int64_t tickIndex)
                                   {
                                       document.beginTransaction (command.name, tickIndex, submitted.origin, coerced);
                                   });

            listId = document.createList ("Sound").id;
        }

        /** The processor's origin, as serve stamps its datagrams. */
        std::string fromProcessor() const { return "udp:127.0.0.1:" + std::to_string (processor.port()); }

        Engine::TickResult apply (const std::string& origin, const std::string& command, std::vector<osc::Value> args)
        {
            REQUIRE (engine.submit (origin, command, std::move (args)));
            const auto result = engine.processTick (tick++);
            answers.flush();
            return result;
        }

        Engine::TickResult declare (const std::string& prefix = "/proc")
        {
            return apply (fromProcessor(), "mount.declare",
                          { text (prefix), number (processor.port()), number (0), text ("Processor") });
        }

        /** A capture from the processor: the seven-string head, then the pairs. */
        Engine::TickResult capture (const std::string& where, const std::string& target, const std::string& id,
                                    const std::vector<std::pair<std::string, std::string>>& pairs,
                                    const std::string& name = "Snap", const std::string& origin = {})
        {
            std::vector<osc::Value> args { text (where), text (target), text (id), text (name),
                                           text (""), text (""), text ("") };

            for (const auto& [address, value] : pairs)
            {
                args.push_back (text (address));
                args.push_back (text (value));
            }

            return apply (origin.empty() ? fromProcessor() : origin, "cue.capture", std::move (args));
        }

        std::string attribute (const std::string& address) const
        {
            return document.getAttribute (address).value_or (std::string {});
        }

        std::vector<std::string> memberIds (const std::string& parentId) const
        {
            std::vector<std::string> ids;
            for (const auto& child : document.findById (parentId))
                if (child.hasProperty ("id") && ! child.hasType ("Message"))
                    ids.push_back (child.getProperty ("id").toString().toStdString());
            return ids;
        }

        /** The cue's further messages, in order: address and value of each. */
        std::vector<std::pair<std::string, std::string>> messagesOf (const std::string& cueId) const
        {
            std::vector<std::pair<std::string, std::string>> out;
            for (const auto& child : document.findById (cueId))
                if (child.hasType ("Message"))
                    out.emplace_back (child.getProperty ("address").toString().toStdString(),
                                      child.getProperty ("value").toString().toStdString());
            return out;
        }

        LogRecord lastRecord()
        {
            const auto parsed = LogFile::parse (engine.log().contents());
            REQUIRE (! parsed.records.empty());
            return parsed.records.back();
        }

        Socket processor;
        Socket goDot;
        tree::RawSender answers;
        Engine engine;
        doc::ShowDocument document;
        std::string listId;
        std::int64_t tick = 1;
    };

    std::string onlyMount (const doc::ShowDocument& document)
    {
        const auto ids = tree::declaredMountIds (document);
        REQUIRE (ids.size() == 1u);
        return ids.front();
    }
}

//==============================================================================
TEST_CASE ("authoring: a processor declares itself and becomes a device, answered created")
{
    AuthoringRig rig;

    CHECK (rig.declare().applied == 1u);

    const auto deviceId = onlyMount (rig.document);
    const auto declaration = tree::mountDeclarationFor (rig.document, deviceId);
    REQUIRE (declaration);
    CHECK (declaration->prefix == "/proc");
    CHECK (declaration->host == "127.0.0.1");
    CHECK (declaration->port == rig.processor.port());
    CHECK (declaration->name == "Processor");
    CHECK (declaration->rx);
    CHECK (declaration->tx);

    /*  THE HOST RIDES ON THE RECORD, so a replay - which has no datagram to
        read it from - makes the same device. */
    const auto record = rig.lastRecord();
    REQUIRE (record.args.size() == 6u);
    CHECK (record.args[4].getString() == deviceId);
    CHECK (record.args[5].getString() == "127.0.0.1");

    REQUIRE (rig.processor.waitFor (1));
    CHECK (rig.processor.words (0) == std::vector<std::string> { "/godot/declared", deviceId, "created" });
}

TEST_CASE ("authoring: declaring again moves where the device is and keeps what the operator decided")
{
    AuthoringRig rig;
    REQUIRE (rig.declare().applied == 1u);
    const auto deviceId = onlyMount (rig.document);

    REQUIRE (rig.document.setAttribute ("/godot/mount/" + deviceId + "/name", "Spatial desk").ok);
    REQUIRE (rig.document.setAttribute ("/godot/mount/" + deviceId + "/rx", "false").ok);

    Socket moved;
    REQUIRE (rig.apply (rig.fromProcessor(), "mount.declare",
                        { text ("/proc"), number (moved.port()), number (5005), text ("Processor") }).applied == 1u);

    CHECK (tree::declaredMountIds (rig.document).size() == 1u);
    const auto declaration = tree::mountDeclarationFor (rig.document, deviceId);
    REQUIRE (declaration);
    CHECK (declaration->port == moved.port());
    CHECK (declaration->queryPort == 5005);
    CHECK (declaration->name == "Spatial desk");
    CHECK_FALSE (declaration->rx);

    REQUIRE (moved.waitFor (1));
    CHECK (moved.words (0) == std::vector<std::string> { "/godot/declared", deviceId, "updated" });
}

TEST_CASE ("authoring: a declare is refused for a reserved root, a bad port, no host and a locked show")
{
    AuthoringRig rig;

    SUBCASE ("a script with no host is bad-value, and nobody is answered")
    {
        CHECK (rig.apply ("cli", "mount.declare",
                          { text ("/proc"), number (rig.processor.port()), number (0), text ("P") }).rejected == 1u);
        CHECK (tree::declaredMountIds (rig.document).empty());
    }

    SUBCASE ("a root under Go.dot's own is bad-value, answered")
    {
        CHECK (rig.declare ("/godot/proc").rejected == 1u);
        CHECK (rig.declare ("/ui").rejected == 1u);
        CHECK (rig.declare ("/").rejected == 1u);
        CHECK (rig.declare ("/two roots").rejected == 1u);
        CHECK (tree::declaredMountIds (rig.document).empty());

        REQUIRE (rig.processor.waitFor (4));
        CHECK (rig.processor.words (0)[2] == "bad-value");
    }

    SUBCASE ("port nought is bad-value")
    {
        CHECK (rig.apply (rig.fromProcessor(), "mount.declare",
                          { text ("/proc"), number (0), number (0), text ("P") }).rejected == 1u);
    }

    SUBCASE ("a locked show refuses, and says so")
    {
        REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
        CHECK (rig.declare().rejected == 1u);
        REQUIRE (rig.processor.waitFor (1));
        CHECK (rig.processor.words (0)[2] == "locked");
    }
}

//==============================================================================
TEST_CASE ("authoring: a capture lands after the standby as one OSC cue, its messages in order, one undo step")
{
    AuthoringRig rig;
    REQUIRE (rig.declare().applied == 1u);
    REQUIRE (rig.processor.waitFor (1));

    const auto first = rig.document.createCue (rig.listId, 0, "memo", "First").id;
    const auto second = rig.document.createCue (rig.listId, 1, "memo", "Second").id;
    REQUIRE (rig.document.setAttribute ("/godot/list/" + rig.listId + "/standby", first).ok);

    const auto outcome = rig.capture ("standby", "", "", { { "/proc/a", "i:3" }, { "/proc/b", "f:0.5" },
                                                         { "/proc/c", "s:\"x y\"" } });
    REQUIRE (outcome.applied == 1u);

    const auto record = rig.lastRecord();
    const auto cueId = record.args[2].getString();
    REQUIRE (cueId.size() == 8u);

    CHECK (rig.memberIds (rig.listId) == std::vector<std::string> { first, cueId, second });
    CHECK (rig.document.findById (cueId).hasType ("Osc"));
    CHECK (rig.attribute ("/godot/cue/" + cueId + "/address") == "/proc/a");
    CHECK (rig.attribute ("/godot/cue/" + cueId + "/value") == "i:3");
    CHECK (rig.attribute ("/godot/cue/" + cueId + "/name") == "Snap");
    CHECK (rig.messagesOf (cueId) == std::vector<std::pair<std::string, std::string>> {
                                         { "/proc/b", "f:0.5" }, { "/proc/c", "s:\"x y\"" } });

    /*  THE MESSAGES' IDENTIFIERS ON THE RECORD, in order, so a replay draws none. */
    CHECK (record.args[6].getString().size() == 17u);

    REQUIRE (rig.processor.waitFor (2));
    CHECK (rig.processor.words (1) == std::vector<std::string> { "/godot/captured", cueId, "created", "", "Snap" });

    REQUIRE (rig.apply ("cli", "undo", {}).applied == 1u);
    CHECK_FALSE (rig.document.findById (cueId).isValid());
    CHECK (rig.memberIds (rig.listId) == std::vector<std::string> { first, second });
}

TEST_CASE ("authoring: with nothing standing by a capture lands at the end; with no list it is refused")
{
    SUBCASE ("the end of the focused list")
    {
        AuthoringRig rig;
        REQUIRE (rig.declare().applied == 1u);
        const auto only = rig.document.createCue (rig.listId, 0, "memo", "Only").id;

        REQUIRE (rig.capture ("standby", "", "", { { "/proc/a", "i:1" } }).applied == 1u);
        const auto cueId = rig.lastRecord().args[2].getString();
        CHECK (rig.memberIds (rig.listId) == std::vector<std::string> { only, cueId });
    }

    SUBCASE ("a named list, by name")
    {
        AuthoringRig rig;
        REQUIRE (rig.declare().applied == 1u);
        const auto other = rig.document.createList ("Video").id;

        REQUIRE (rig.capture ("list", "Video", "", { { "/proc/a", "i:1" } }).applied == 1u);
        const auto cueId = rig.lastRecord().args[2].getString();
        CHECK (rig.memberIds (other) == std::vector<std::string> { cueId });
    }

    SUBCASE ("no list at all")
    {
        AuthoringRig rig;
        REQUIRE (rig.declare().applied == 1u);
        REQUIRE (rig.document.remove (rig.listId).ok);
        CHECK (rig.capture ("standby", "", "", { { "/proc/a", "i:1" } }).rejected == 1u);
    }
}

TEST_CASE ("authoring: the identifier updates the cue where it stands, and a deleted one is made again")
{
    AuthoringRig rig;
    REQUIRE (rig.declare().applied == 1u);

    REQUIRE (rig.capture ("standby", "", "KEEP1234", { { "/proc/a", "i:1" }, { "/proc/b", "i:2" } }).applied == 1u);
    const auto end = rig.document.createCue (rig.listId, 5, "memo", "After").id;

    REQUIRE (rig.capture ("standby", "", "KEEP1234", { { "/proc/a", "i:9" }, { "/proc/c", "i:3" }, { "/proc/d", "i:4" } },
                          "Snap again").applied == 1u);

    CHECK (rig.memberIds (rig.listId) == std::vector<std::string> { "KEEP1234", end });
    CHECK (rig.attribute ("/godot/cue/KEEP1234/value") == "i:9");
    CHECK (rig.attribute ("/godot/cue/KEEP1234/name") == "Snap again");
    CHECK (rig.messagesOf ("KEEP1234") == std::vector<std::pair<std::string, std::string>> {
                                              { "/proc/c", "i:3" }, { "/proc/d", "i:4" } });

    REQUIRE (rig.processor.waitFor (3));
    CHECK (rig.processor.words (2)[2] == "updated");

    /*  ONE STEP: an undo puts the first capture back whole. */
    REQUIRE (rig.apply ("cli", "undo", {}).applied == 1u);
    CHECK (rig.attribute ("/godot/cue/KEEP1234/value") == "i:1");
    CHECK (rig.messagesOf ("KEEP1234") == std::vector<std::pair<std::string, std::string>> { { "/proc/b", "i:2" } });

    REQUIRE (rig.document.remove ("KEEP1234").ok);
    REQUIRE (rig.capture ("standby", "", "KEEP1234", { { "/proc/a", "i:5" } }).applied == 1u);
    CHECK (rig.document.findById ("KEEP1234").isValid());
}

TEST_CASE ("authoring: a cue named by number is replaced; a chunk appends; the wrong kind is refused")
{
    AuthoringRig rig;
    REQUIRE (rig.declare().applied == 1u);

    REQUIRE (rig.capture ("standby", "", "NVMB0001", { { "/proc/a", "i:1" } }).applied == 1u);
    REQUIRE (rig.document.setAttribute ("/godot/cue/NVMB0001/number", "12.5").ok);

    REQUIRE (rig.capture ("cue", "12.5", "", { { "/proc/z", "i:7" } }).applied == 1u);
    CHECK (rig.attribute ("/godot/cue/NVMB0001/address") == "/proc/z");

    REQUIRE (rig.capture ("more", "NVMB0001", "NVMB0001", { { "/proc/y", "i:8" } }, "").applied == 1u);
    CHECK (rig.messagesOf ("NVMB0001") == std::vector<std::pair<std::string, std::string>> { { "/proc/y", "i:8" } });

    const auto memo = rig.document.createCue (rig.listId, 0, "memo", "Words").id;
    CHECK (rig.capture ("standby", "", memo, { { "/proc/a", "i:1" } }).rejected == 1u);
    CHECK (rig.capture ("cue", "99", "", { { "/proc/a", "i:1" } }).rejected == 1u);
}

TEST_CASE ("authoring: a capture is checked whole before anything is written")
{
    AuthoringRig rig;
    REQUIRE (rig.declare().applied == 1u);
    REQUIRE (rig.document.createMount ("/other", {}).ok);
    const auto before = doc::CanonicalXml::write (rig.document);

    CHECK (rig.capture ("standby", "", "", {}).rejected == 1u);                                        // no pairs
    CHECK (rig.capture ("somewhere", "", "", { { "/proc/a", "i:1" } }).rejected == 1u);                  // where
    CHECK (rig.capture ("standby", "", "", { { "/proc/a", "i:1" }, { "/proc/b", "x:1" } }).rejected == 1u);  // atom
    CHECK (rig.capture ("standby", "", "", { { "/nowhere/a", "i:1" } }).rejected == 1u);                 // no device
    CHECK (rig.capture ("standby", "", "", { { "/proc/a", "i:1" }, { "/other/b", "i:1" } }).rejected == 1u);  // two

    CHECK (doc::CanonicalXml::write (rig.document) == before);

    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
    CHECK (rig.capture ("standby", "", "", { { "/proc/a", "i:1" } }).rejected == 1u);
}

TEST_CASE ("authoring: the answer goes to the device the sender is, else to the device the cue aims at")
{
    AuthoringRig rig;
    REQUIRE (rig.declare().applied == 1u);

    Socket desk;
    const auto deskId = rig.document.createMount ("/desk", {}).id;
    REQUIRE (rig.document.setAttribute ("/godot/mount/" + deskId + "/host", "127.0.0.1").ok);
    REQUIRE (rig.document.setAttribute ("/godot/mount/" + deskId + "/port", std::to_string (desk.port())).ok);

    /*  From the processor, aimed at the desk: the processor hears it. Both
        declared devices hold 127.0.0.1 here, so the first in document order -
        the processor's own - is the sender's. */
    REQUIRE (rig.capture ("standby", "", "", { { "/desk/fader", "f:0.5" } }).applied == 1u);
    REQUIRE (rig.processor.waitFor (2));

    /*  From a script: the device the cue aims at hears it. */
    REQUIRE (rig.engine.submit ("cli", "cue.capture",
                                { text ("standby"), text (""), text (""), text ("S"), text (""), text (""), text (""),
                                  text ("/desk/fader"), text ("f:0.25") }));
    rig.engine.processTick (rig.tick++);
    REQUIRE (rig.answers.waiting().size() == 1u);
    CHECK (rig.answers.waiting().front().port == desk.port());
    rig.answers.flush();
    REQUIRE (desk.waitFor (1));
}

TEST_CASE ("authoring: the applied records rebuild the same show with no socket and nothing drawn")
{
    AuthoringRig rig;
    REQUIRE (rig.declare().applied == 1u);
    REQUIRE (rig.capture ("standby", "", "", { { "/proc/a", "i:1" }, { "/proc/b", "i:2" }, { "/proc/c", "i:3" } }).applied == 1u);
    REQUIRE (rig.capture ("standby", "", "", { { "/proc/a", "i:4" } }).applied == 1u);

    const auto parsed = LogFile::parse (rig.engine.log().contents());

    /*  A second show, the same list, the records replayed under `cli` - no
        datagram, no socket - and the answers to nobody. */
    Engine engine;
    engine.log().openInMemory ({});
    doc::ShowDocument document;
    tree::RawSender nowhere;
    doc::registerDocumentCommands (engine.commands(), document);
    tree::registerAuthoringCommands (engine.commands(), document, nowhere);
    REQUIRE (document.createList ("Sound", rig.listId).ok);

    std::int64_t tick = 1;
    for (const auto& record : parsed.records)
    {
        if (record.kind != LogRecord::Kind::applied)
            continue;

        REQUIRE (engine.submit ("cli", record.command, record.args));
        CHECK (engine.processTick (tick++).applied == 1u);
    }

    nowhere.flush();
    CHECK (nowhere.sentCount() == 0u);
    CHECK (doc::CanonicalXml::write (document) == doc::CanonicalXml::write (rig.document));
}

TEST_CASE ("authoring: answers leave at the end of the tick, as raw datagrams")
{
    Socket to;
    Socket from;
    tree::RawSender answers { from.endpoint };

    answers.queue ("127.0.0.1", to.port(), osc::Packet::message ("/godot/captured", { text ("ABCDEFGH"), text ("created") }));
    answers.queue ("127.0.0.1", to.port(), osc::Packet::message ("/godot/captured", { text ("ABCDEFGH"), text ("appended") }));
    answers.queue ("127.0.0.1", 0, osc::Packet::message ("/godot/captured", {}));

    CHECK (answers.pending() == 2u);
    CHECK (answers.droppedCount() == 1u);
    CHECK (to.count() == 0u);

    answers.flush();

    REQUIRE (to.waitFor (2));
    CHECK (answers.sentCount() == 2u);
    CHECK (to.words (1)[2] == "appended");
    CHECK (to.received[0].senderPort == from.port());
}

TEST_CASE ("authoring: the origin's host")
{
    CHECK (tree::originHost ("udp:192.168.1.7:9000") == "192.168.1.7");
    CHECK (tree::originHost ("ws:10.0.0.2:51234") == "10.0.0.2");
    CHECK (tree::originHost ("udp:::1:9000") == "::1");
    CHECK (tree::originHost ("cli").empty());
    CHECK (tree::originHost ("mount:K3PV7WRB").empty());
}

//==============================================================================
TEST_CASE ("authoring: a node that takes no argument takes an empty value, and nothing else")
{
    tree::MountTable mounts;
    tree::MountDeclaration declaration;
    declaration.id = "DESK0001";
    declaration.prefix = "/desk";
    declaration.namespaceFile = "namespaces/desk.json";
    declaration.port = 9000;

    REQUIRE (mounts.load (declaration, R"JSON({ "FULL_PATH": "/", "CONTENTS": {
                 "go":    { "FULL_PATH": "/go", "ACCESS": 2 },
                 "stop":  { "FULL_PATH": "/stop", "TYPE": "N", "ACCESS": 2 },
                 "level": { "FULL_PATH": "/level", "TYPE": "f", "ACCESS": 3 } } })JSON").ok);

    CHECK (mounts.write ("/desk/go", osc::Values {}).ok);
    CHECK (mounts.write ("/desk/stop", osc::Values {}).ok);
    CHECK_FALSE (mounts.write ("/desk/go", osc::Value::int32 (1)).ok);
    CHECK_FALSE (mounts.write ("/desk/level", osc::Values {}).ok);
    CHECK (mounts.write ("/desk/level", osc::Value::float32 (0.5f)).ok);
}

TEST_CASE ("authoring: a declare with a query port asks for the description; one that changes nothing passes the lock")
{
    AuthoringRig rig;

    std::vector<std::string> asked;
    tree::registerAuthoringCommands (rig.engine.commands(), rig.document, rig.answers,
                                     [&asked] (const std::string& mountId, const std::string& host, int queryPort,
                                               const std::string& prefix)
                                     { asked.push_back (mountId + " " + host + " " + std::to_string (queryPort) + " " + prefix); });

    REQUIRE (rig.apply (rig.fromProcessor(), "mount.declare",
                        { text ("/proc"), number (rig.processor.port()), number (5005), text ("Processor") }).applied == 1u);
    const auto deviceId = onlyMount (rig.document);
    REQUIRE (asked.size() == 1u);
    CHECK (asked[0] == deviceId + " 127.0.0.1 5005 /proc");

    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);

    //  The same declare again: nothing to edit, so answered and described again.
    CHECK (rig.apply (rig.fromProcessor(), "mount.declare",
                      { text ("/proc"), number (rig.processor.port()), number (5005), text ("Processor") }).applied == 1u);
    CHECK (asked.size() == 2u);

    //  A moved port is an edit: refused under the lock, and nothing asked.
    CHECK (rig.apply (rig.fromProcessor(), "mount.declare",
                      { text ("/proc"), number (rig.processor.port()), number (6006), text ("Processor") }).rejected == 1u);
    CHECK (asked.size() == 2u);

    REQUIRE (rig.processor.waitFor (3));
    CHECK (rig.processor.words (1)[2] == "updated");
    CHECK (rig.processor.words (2)[2] == "locked");
}

TEST_CASE ("authoring: a fetched description is adopted, loaded and answered; a failed one keeps what was there")
{
    AuthoringRig rig;
    REQUIRE (rig.declare().applied == 1u);
    const auto deviceId = onlyMount (rig.document);
    const auto file = tree::MountFetcher::fileFor (deviceId);

    //  The bundle as the fetcher leaves it: the description beside the show.
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getNonexistentChildFile ("wfg-authoring", {}, false);
    REQUIRE (folder.getChildFile ("namespaces").createDirectory());
    REQUIRE (folder.getChildFile (file).replaceWithText (R"JSON({ "FULL_PATH": "/proc", "CONTENTS": {
                 "go": { "FULL_PATH": "/proc/go", "ACCESS": 2 },
                 "x":  { "FULL_PATH": "/proc/x", "TYPE": "i", "ACCESS": 3 } } })JSON"));

    tree::MountTable mounts;
    tree::registerDescriptionCommands (rig.engine.commands(), rig.document, mounts, folder, rig.answers);

    REQUIRE (rig.apply ("mount:" + deviceId, "mount.described",
                        { text (deviceId), text (file), number (2), text ("") }).applied == 1u);

    CHECK (rig.attribute ("/godot/mount/" + deviceId + "/namespace") == file);
    const auto loaded = mounts.nodeCount (deviceId);
    CHECK (loaded >= 2u);
    CHECK (mounts.write ("/proc/go", osc::Values {}).ok);

    REQUIRE (rig.processor.waitFor (2));
    CHECK (rig.processor.words (1) == std::vector<std::string> { "/godot/described", deviceId, std::to_string (loaded), "" });

    //  A fetch that failed: what was loaded stays, and the device says why.
    REQUIRE (rig.apply ("mount:" + deviceId, "mount.described",
                        { text (deviceId), text (file), number (0), text ("could not read http://127.0.0.1:1/proc") }).applied == 1u);
    CHECK (mounts.nodeCount (deviceId) == loaded);
    CHECK (mounts.problemOf (deviceId).find ("could not read") != std::string::npos);

    REQUIRE (rig.processor.waitFor (3));
    CHECK (rig.processor.words (2)[3] == "could not read http://127.0.0.1:1/proc");

    //  And the fetch itself, against nothing listening: a problem, no file.
    const auto outcome = tree::MountFetcher::fetchNow ({ "NWHERE01", "127.0.0.1", 1, "/proc", folder }, 1000);
    CHECK_FALSE (outcome.problem.empty());
    CHECK_FALSE (folder.getChildFile (tree::MountFetcher::fileFor ("NWHERE01")).exists());

    folder.deleteRecursively();
}
