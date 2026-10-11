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
    Somebody else's namespace, mounted into ours.

    THE TWO FIXTURES ARE THE ARGUMENT. PRD §3.22 says the template format IS an
    OSCQuery description, so a capture from a running processor and a file
    somebody wrote by hand have to be the same kind of thing to the engine.
    `namespaces/wfs-diy.json` is shaped exactly as WFS-DIY's own server builds a
    reply - the key set, `CLIPMODE` alongside `RANGE`, the read-only
    `channelType` that is its only ACCESS 1 node, the EQ node with two range
    entries and no `VALUE`, and the float-widening artifact in `distanceRatio`'s
    minimum, which is what `0.1f` really serialises to through a double.
    `namespaces/console.json` is hand-written and uses the `GODOT` key to
    declare what a capture can only imply. Both go through one reader, and this
    file is what says so.

    A serialisation surface, so every case runs under fr_FR as well as C.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/json/JsonValue.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/MountSender.h>
#include <wfg/engine/osc/OscCodec.h>
#include <wfg/engine/tree/OscQueryJson.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <chrono>
#include <wfg/engine/tree/TreeCommands.h>
#include <wfg/engine/oscquery/OscQueryClient.h>
#include <wfg/engine/midi/MidiSink.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace wfg;
using namespace wfg::tree;

namespace
{
    juce::File fixtureBundle()
    {
        const juce::File folder { juce::String (std::string (WFG_TEST_FIXTURES_DIR))
                                    + "/bundles/minimal" };

        REQUIRE_MESSAGE (folder.isDirectory(), "missing fixture bundle: "
                                                 << folder.getFullPathName());
        return folder;
    }

    /** A scratch copy of the fixture, so a test can break a file inside it. */
    juce::File copyFixtureToScratch()
    {
        const auto scratch = juce::File::getSpecialLocation (juce::File::tempDirectory)
                               .getChildFile ("wfg-tests")
                               .getChildFile (juce::Uuid().toDashedString())
                               .getChildFile ("minimal");

        REQUIRE (fixtureBundle().copyDirectoryTo (scratch));
        return scratch;
    }

    MountDeclaration wfsDeclaration()
    {
        MountDeclaration declaration;
        declaration.id = "G1JS4VWE";
        declaration.prefix = "/wfs";
        declaration.namespaceFile = "namespaces/wfs-diy.json";
        return declaration;
    }

    /*  Engine, document, mounts and tree, wired the way `wfg tree` wires them
        and the way `serve` will. */
    struct Rig
    {
        Rig()
        {
            REQUIRE (doc::Bundle::open (folder, document).ok);
            doc::registerDocumentCommands (engine.commands(), document);
            registerMountCommands (engine.commands(), document, mounts, folder);

            const auto problems = loadAllMountsFromBundle (document, mounts, folder);

            for (const auto& problem : problems)
                INFO ("mount problem: " << problem);

            REQUIRE (problems.empty());
        }

        std::shared_ptr<const TreeSnapshot> publish (std::int64_t tick)
        {
            parameters.markStale();
            EngineState state;
            state.tick = tick;
            return parameters.publish (tick, state);
        }

        juce::File folder { fixtureBundle() };
        Engine engine;
        doc::ShowDocument document;
        MountTable mounts;
        cue::RunTable runs;
        ParameterTree parameters { document, engine.commands(), mounts, runs };
    };
}

//==============================================================================
//==============================================================================
/*  A DEVICE THAT ANSWERS AT SEVERAL ROOTS (2026-09-22).

    The author's own desk is the case. A DiGiCo S21 reached directly, rather
    than through its sidecar, speaks three vocabularies with nothing above
    them: `/channel/{n}/…` for every strip control, `/console/…` for ping,
    pong, resend and the channel counts, and `/digico/snapshots/fire` for
    snapshot recall. There is no root to mount it at, because its addresses
    already start at one.
*/
TEST_CASE ("mount: a device may answer at several roots, and they are one device")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    CHECK (prefixesOf ("/wfs") == std::vector<std::string> { "/wfs" });
    CHECK (prefixesOf ("/channel /console /digico")
             == std::vector<std::string> { "/channel", "/console", "/digico" });

    //  Written by a hand rather than by a writer: extra spaces mean nothing.
    CHECK (prefixesOf ("  /channel   /console  ")
             == std::vector<std::string> { "/channel", "/console" });
    CHECK (prefixesOf ("").empty());
    CHECK (prefixesOf ("   ").empty());

    SUBCASE ("an address under any of them belongs to the device")
    {
        const std::string s21 = "/channel /console /digico";

        CHECK (prefixMatchLength ("/channel/1/fader", s21) == 8u);
        CHECK (prefixMatchLength ("/console/ping", s21) == 8u);
        CHECK (prefixMatchLength ("/digico/snapshots/fire", s21) == 7u);

        //  And one it does not speak is not its.
        CHECK (prefixMatchLength ("/wfs/source/1/gain", s21) == 0u);

        /*  THE BOUNDARY IS A SEPARATOR, per root: `/channels` merely begins
            with the same letters as `/channel`. */
        CHECK (prefixMatchLength ("/channels/1/fader", s21) == 0u);

        //  The root itself is not under itself - there is no node there.
        CHECK (prefixMatchLength ("/console", s21) == 0u);
    }

    SUBCASE ("the longest root wins, which is what makes nesting mean anything")
    {
        CHECK (prefixMatchLength ("/desk/aux/1/level", "/desk") == 5u);
        CHECK (prefixMatchLength ("/desk/aux/1/level", "/desk/aux") == 9u);
    }
}

TEST_CASE ("mount: the engine sends to the device whose root covers most of the address")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    MountTable mounts;

    /*  IDENTIFIERS CHOSEN TO SORT THE WRONG WAY ROUND. Until 2026-09-22 this
        loop returned the first device whose root fitted, walking a map keyed by
        identifier - so `AAAA0001` would have taken a cue that belongs to
        `ZZZZ0001`, and which device got it depended on two random eight
        character strings. That was unreachable while prefixes were
        hand-written and became reachable the moment a window let somebody type
        one. */
    MountDeclaration desk;
    desk.id = "AAAA0001";
    desk.prefix = "/desk";
    desk.port = 9000;
    REQUIRE (mounts.declare (desk).ok);

    MountDeclaration aux;
    aux.id = "ZZZZ0001";
    aux.prefix = "/desk/aux";
    aux.port = 9001;
    REQUIRE (mounts.declare (aux).ok);

    CHECK (mounts.mountOf ("/desk/fader") == "AAAA0001");
    CHECK (mounts.mountOf ("/desk/aux/1/level") == "ZZZZ0001");
    CHECK (mounts.mountOf ("/elsewhere/x").empty());

    SUBCASE ("and a write reaches the same one the menu would have named")
    {
        const auto written = mounts.write ("/desk/aux/1/level", osc::Value::float32 (0.5f));

        CHECK (written.ok);
        CHECK (written.mountId == "ZZZZ0001");
    }
}

TEST_CASE ("mount: the S21's three vocabularies are one device on the wire")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    MountTable mounts;

    MountDeclaration s21;
    s21.id = "S2100001";
    s21.prefix = "/channel /console /digico";
    s21.host = "192.168.1.60";
    s21.port = 8000;
    REQUIRE (mounts.declare (s21).ok);

    /*  Three addresses out of the desk's own command set, each landing on the
        one device - which is the whole point: one row in the settings, one
        name, one `sent` count, and the address in the cue is the address in
        the manual it was copied from. */
    for (const auto* address : { "/channel/1/fader", "/channel/12/mute",
                                 "/console/ping", "/digico/snapshots/fire" })
    {
        INFO ("address: " << address);

        const auto written = mounts.write (address, osc::Value::int32 (1));

        CHECK (written.ok);
        CHECK (written.mountId == "S2100001");
    }
}

TEST_CASE ("mount: a node's role is read from its GODOT key, and only a word is one")
{
    /*  Namespace draft §57, AFM: the vocabulary every preset shares, one
        word per node that claims one; a node that claims none has none. */
    MountDeclaration desk;
    desk.id = "ROLE0001";
    desk.prefix = "/desk";
    desk.namespaceFile = "namespaces/desk.json";
    desk.port = 9000;

    const auto loaded = readNamespace (desk, R"JSON({"FULL_PATH": "/", "CONTENTS": {
        "fader":  {"FULL_PATH": "/fader",  "TYPE": "f", "ACCESS": 3, "VALUE": [0.0], "GODOT": {"ROLE": "strip.level"}},
        "recall": {"FULL_PATH": "/recall", "TYPE": "i", "ACCESS": 2, "GODOT": {"ROLE": "scene.recall"}},
        "mute":   {"FULL_PATH": "/mute",   "TYPE": "T", "ACCESS": 3, "VALUE": [false], "GODOT": {"ROLE": [1, 2]}},
        "name":   {"FULL_PATH": "/name",   "TYPE": "s", "ACCESS": 3, "VALUE": [""]} } })JSON");
    REQUIRE (loaded.ok);

    const auto roleOf = [&loaded] (const std::string& address)
    {
        for (const auto& node : loaded.nodes)
            if (node.address == address)
                return node.role;
        return std::string ("?");
    };

    CHECK (roleOf ("/desk/fader") == "strip.level");
    CHECK (roleOf ("/desk/recall") == "scene.recall");
    CHECK (roleOf ("/desk/mute").empty());
    CHECK (roleOf ("/desk/name").empty());
}

TEST_CASE ("mount: a device with several roots takes a description rooted at \"/\" whose entries are those roots")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    /*  Namespace draft §57, AFK. An X32 answers at /ch, /bus, /dca and more,
        with nothing above them; a preset for it is rooted at "/" and its
        entries are those roots, each mounted at its own name, so the address
        in a cue is the one in the manual. Until this round a described device
        kept one root and several were for a device nobody described. */
    MountDeclaration desk;
    desk.id = "X32A0001";
    desk.prefix = "/ch /bus";
    desk.namespaceFile = "namespaces/x32.json";
    desk.port = 10023;

    const char* twoRoots = R"({"FULL_PATH": "/", "CONTENTS": {
        "ch":  {"FULL_PATH": "/ch",  "CONTENTS": {"01": {"FULL_PATH": "/ch/01",  "CONTENTS": {"fader": {"FULL_PATH": "/ch/01/fader",  "TYPE": "f", "ACCESS": 3, "VALUE": [0.5]}}}}},
        "bus": {"FULL_PATH": "/bus", "CONTENTS": {"01": {"FULL_PATH": "/bus/01", "CONTENTS": {"fader": {"FULL_PATH": "/bus/01/fader", "TYPE": "f", "ACCESS": 3, "VALUE": [0.5]}}}}}}})";

    SUBCASE ("the roots are the file's: each entry mounts at its own name, and the file's root is no node")
    {
        const auto read = readNamespace (desk, twoRoots);
        REQUIRE (read.ok);

        std::vector<std::string> addresses;
        for (const auto& node : read.nodes)
            addresses.push_back (node.address);

        CHECK (addresses == std::vector<std::string> { "/bus", "/bus/01", "/bus/01/fader",
                                                       "/ch", "/ch/01", "/ch/01/fader" });

        MountTable mounts;
        REQUIRE (mounts.load (desk, twoRoots).ok);
        CHECK (mounts.mountOf ("/bus/01/fader") == "X32A0001");
        CHECK (mounts.mountOf ("/ch/01/fader") == "X32A0001");
        CHECK (mounts.write ("/ch/01/fader", osc::Value::float32 (0.75f)).ok);
        CHECK_FALSE (mounts.write ("/ch/02/fader", osc::Value::float32 (0.75f)).ok);
    }

    SUBCASE ("the roots the row names must be exactly the file's, and the sentence names both")
    {
        desk.prefix = "/ch /dca";
        const auto read = readNamespace (desk, twoRoots);
        CHECK_FALSE (read.ok);
        REQUIRE_FALSE (read.problems.empty());
        INFO ("said: " << read.problems.front());
        CHECK (read.problems.front().find ("/bus /ch") != std::string::npos);
        CHECK (read.problems.front().find ("/ch /dca") != std::string::npos);
    }

    SUBCASE ("a file rooted anywhere but \"/\" is one tree and mounts in one place, as before")
    {
        desk.prefix = "/wfs /other";
        const auto read = readNamespace (desk, R"({"FULL_PATH": "/wfs", "CONTENTS": {}})");
        CHECK_FALSE (read.ok);
        REQUIRE_FALSE (read.problems.empty());
        INFO ("said: " << read.problems.front());
        CHECK (read.problems.front().find ("root is \"/\"") != std::string::npos);
    }

    SUBCASE ("the roots a description answers at, for the row a preset gives a device")
    {
        auto roots = rootsOfNamespace (twoRoots);
        std::sort (roots.begin(), roots.end());
        CHECK (roots == std::vector<std::string> { "/bus", "/ch" });
        CHECK (rootsOfNamespace (R"({"FULL_PATH": "/wfs", "CONTENTS": {}})") == std::vector<std::string> { "/wfs" });
        CHECK (rootsOfNamespace ("not json").empty());
    }
}

TEST_CASE ("mount: a device has to say where it answers")
{
    MountTable mounts;

    MountDeclaration nowhere;
    nowhere.id = "NONE0001";
    nowhere.port = 9000;

    const auto result = mounts.declare (nowhere);

    CHECK_FALSE (result.ok);
    CHECK_FALSE (mounts.isLoaded ("NONE0001"));
}

//==============================================================================
/*  A DEVICE THAT DESCRIBES NOTHING (2026-09-22).

    Until this round a mount without a namespace file was refused outright, so
    the price of having a device at all was an OSCQuery description of it -
    which almost no desk ships and nobody wants to hand-write for a box they
    are about to send three messages to. These cases are the other half of PRD
    3.22: what an OPAQUE device can and cannot do, said as assertions rather
    than as a comment, because the temptation with a pass-through is to let it
    swallow everything and the whole value of a DESCRIBED device is that it
    does not.
*/
TEST_CASE ("mount: a device with no description is declared rather than refused")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    MountTable mounts;
    MountDeclaration desk;
    desk.id = "DESK0001";
    desk.prefix = "/desk";
    desk.host = "192.168.1.20";
    desk.port = 10023;

    REQUIRE (desk.opaque());

    const auto result = mounts.declare (desk);

    CHECK (result.ok);
    CHECK (result.problems.empty());
    CHECK (mounts.isLoaded ("DESK0001"));

    /*  NO NODES, which is the whole of what opaque means: there is nothing to
        publish under the prefix because nobody said what is there. */
    CHECK (mounts.nodeCount ("DESK0001") == 0);

    REQUIRE (mounts.declarationOf ("DESK0001") != nullptr);
    CHECK (mounts.declarationOf ("DESK0001")->host == "192.168.1.20");

    /*  AND IT CAN NEVER BE ASKED, whatever it declares. A verified cue aimed
        here would wait for an answer with nowhere to come from, so the refusal
        belongs at load and `canBeAsked` is what says so. */
    desk.readback = "oscquery";
    desk.queryPort = 5005;
    CHECK_FALSE (desk.canBeAsked());
}

TEST_CASE ("mount: a prefix that would mount over the engine is refused, described or not")
{
    MountTable mounts;
    MountDeclaration bad;
    bad.id = "BAD00001";
    bad.prefix = "/godot/cue";
    bad.port = 9000;

    const auto result = mounts.declare (bad);

    /*  The same check a described mount goes through, and it has to be: an
        opaque device publishes no nodes, but its PREFIX is still what every
        outgoing address is matched against, so a bad one would silently claim
        cues meant for somebody else.

        `/godot` was added to the reserved list in the same round (2026-09-22).
        Nothing had refused it before - a mount there would shadow the engine's
        own addresses - and it had been harmless only because a prefix could
        only be hand-written by somebody who knew what it was. The settings
        window is a box a person types one into. */
    CHECK_FALSE (result.ok);
    CHECK_FALSE (mounts.isLoaded ("BAD00001"));
}

TEST_CASE ("mount: a write to an opaque device goes as it was typed, and to a described one it is checked")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    Rig rig;

    MountDeclaration desk;
    desk.id = "DESK0001";
    desk.prefix = "/desk";
    desk.port = 10023;
    REQUIRE (rig.mounts.declare (desk).ok);

    SUBCASE ("an address nobody described is a message, not a mistake")
    {
        const auto written = rig.mounts.write ("/desk/ch/01/mix/fader",
                                               osc::Value::float32 (0.5f));

        CHECK (written.ok);
        CHECK (written.mountId == "DESK0001");

        /*  AS TYPED. There is no declared type to coerce to, so the value that
            goes on the wire is the one the cue spells - which is why the cue's
            own atom carries its type. */
        REQUIRE (written.values.size() == 1u);
        CHECK (written.values.front().isFloat32());

        /*  AND NOTHING IS STORED. A value nobody can read back is not a fact
            about the device, only about what was sent; `sent` on the mount is
            what a rehearsal reads instead. */
        CHECK (rig.mounts.valueOf ("/desk/ch/01/mix/fader") == nullptr);
    }

    SUBCASE ("the same address under a DESCRIBED device is still refused")
    {
        /*  The fixture's own mount, which has a namespace file. This is the
            case the pass-through must not swallow: the show said what that box
            has, this is not among it, and saying so now beats a datagram that
            leaves and is ignored. */
        const auto written = rig.mounts.write ("/wfs/not/a/node", osc::Value::float32 (0.5f));

        CHECK_FALSE (written.ok);
        CHECK (written.reason == reason::badAddress);
    }

    SUBCASE ("and an address under no device at all is refused")
    {
        const auto written = rig.mounts.write ("/nowhere/at/all", osc::Value::float32 (0.5f));

        CHECK_FALSE (written.ok);
        CHECK (written.reason == reason::badAddress);
    }

    SUBCASE ("a prefix is a boundary, not a string start")
    {
        /*  "/desktop" is not under "/desk", and the pass-through must not
            claim it: a device whose prefix happens to begin another's would
            otherwise take its cues. */
        const auto written = rig.mounts.write ("/desktop/fader", osc::Value::float32 (0.5f));

        CHECK_FALSE (written.ok);
    }
}

TEST_CASE ("mount: a retyped port keeps the nodes and the values")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    Rig rig;

    const auto* before = rig.mounts.declarationOf ("G1JS4VWE");
    REQUIRE (before != nullptr);

    const auto nodes = rig.mounts.nodeCount ("G1JS4VWE");
    REQUIRE (nodes > 0);

    /*  Something the tree holds, so the test can prove it survives. */
    const auto address = rig.mounts.allNodes().front().address;
    rig.mounts.noteReadback (address, osc::Value::float32 (0.25f));

    auto moved = *before;
    moved.host = "10.0.0.7";
    moved.port = 9001;

    const auto version = rig.mounts.revision();

    CHECK (rig.mounts.updateDeclaration (moved));

    /*  THE DESTINATION MOVED AND THE DEVICE DID NOT. Re-reading the namespace
        for a changed host would throw away every value and every read-back in
        flight to arrive at the same nodes - which, during a tech rehearsal
        where somebody is retyping an address, is the whole session. */
    CHECK (rig.mounts.declarationOf ("G1JS4VWE")->host == "10.0.0.7");
    CHECK (rig.mounts.nodeCount ("G1JS4VWE") == nodes);
    REQUIRE (rig.mounts.readbackOf (address) != nullptr);

    /*  And the table says it moved, so the published tree is rebuilt. */
    CHECK (rig.mounts.revision() != version);

    CHECK_FALSE (rig.mounts.updateDeclaration (MountDeclaration {}));
}

TEST_CASE ("mount: why a device cannot be used is kept where a client can read it")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    MountTable mounts;

    CHECK (mounts.problemOf ("DESK0001").empty());

    /*  KEPT FOR A MOUNT THAT HAS NO ENTRY. A device refused for having no port
        never became one, and this sentence is the only thing anybody can act
        on - which is precisely the case that used to be a line on a terminal
        at startup and nothing else. */
    mounts.setProblem ("DESK0001", "no usable port");
    CHECK (mounts.problemOf ("DESK0001") == "no usable port");
    CHECK_FALSE (mounts.isLoaded ("DESK0001"));

    mounts.setProblem ("DESK0001", {});
    CHECK (mounts.problemOf ("DESK0001").empty());
}

TEST_CASE ("mount: the document is what the table follows, and a refresh carries an edit to it")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    Rig rig;

    /*  A device made the way the settings window makes one: a prefix, no
        namespace file, and everything else written afterwards. */
    const auto made = rig.document.createMount ("/desk", {}, {});
    REQUIRE (made.ok);

    const auto id = made.id;

    refreshMountDeclarations (rig.document, rig.mounts, rig.folder);

    /*  It has no port yet, so it is refused - and says why, rather than
        looking exactly like a device that works. */
    CHECK_FALSE (rig.mounts.isLoaded (id));
    CHECK_FALSE (rig.mounts.problemOf (id).empty());

    REQUIRE (rig.document.setAttribute ("/godot/mount/" + id + "/port", "10023").ok);
    refreshMountDeclarations (rig.document, rig.mounts, rig.folder);

    CHECK (rig.mounts.isLoaded (id));
    CHECK (rig.mounts.problemOf (id).empty());
    REQUIRE (rig.mounts.declarationOf (id) != nullptr);
    CHECK (rig.mounts.declarationOf (id)->port == 10023);

    /*  AND THE ROWS THE WINDOW WRITES REACH THE DECLARATION. This is what
        makes a retyped address reach the socket without reopening the show. */
    REQUIRE (rig.document.setAttribute ("/godot/mount/" + id + "/host", "192.168.1.20").ok);
    REQUIRE (rig.document.setAttribute ("/godot/mount/" + id + "/tx", "false").ok);
    REQUIRE (rig.document.setAttribute ("/godot/mount/" + id + "/rx", "true").ok);
    REQUIRE (rig.document.setAttribute ("/godot/mount/" + id + "/name", "Lighting desk").ok);
    refreshMountDeclarations (rig.document, rig.mounts, rig.folder);

    const auto* held = rig.mounts.declarationOf (id);
    REQUIRE (held != nullptr);
    CHECK (held->host == "192.168.1.20");
    CHECK_FALSE (held->tx);
    CHECK (held->rx);
    CHECK (held->name == "Lighting desk");

    /*  AND A DEVICE SOMEBODY DELETED STOPS BEING ONE. Without this its prefix
        would go on claiming addresses for the rest of the session, so a cue
        re-aimed at its replacement would still be matched to the ghost. */
    REQUIRE (rig.document.remove (id).ok);
    refreshMountDeclarations (rig.document, rig.mounts, rig.folder);
    CHECK_FALSE (rig.mounts.isLoaded (id));
}

//==============================================================================
TEST_CASE ("mount: a captured description and a hand-written one load the same way")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    Rig rig;

    CHECK (rig.mounts.isLoaded ("G1JS4VWE"));       // captured from WFS-DIY
    CHECK (rig.mounts.isLoaded ("H2KP7RTV"));       // written by hand
    CHECK (rig.mounts.size() == 2);

    CHECK (rig.mounts.nodeCount ("G1JS4VWE") > 0);
    CHECK (rig.mounts.nodeCount ("H2KP7RTV") > 0);

    /*  Both land at their own prefixes, not under /godot/mount, which holds the
        declaration rather than the namespace. */
    const auto snapshot = rig.publish (0);

    CHECK (snapshot->find ("/wfs/input/1/positionX") != nullptr);
    CHECK (snapshot->find ("/ext/console/masterLevel") != nullptr);
    CHECK (snapshot->find ("/godot/mount/G1JS4VWE/prefix") != nullptr);
}

TEST_CASE ("mount: a subtree capture mounts where it is put, not one level deeper")
{
    /*  WFS-DIY publishes everything under a `/wfs` container of its own, so a
        capture of GET / mounted at /wfs would give /wfs/wfs/input/1/positionX.
        The fixture captures GET /wfs instead, and the addresses come out the
        way anybody would expect. */
    Rig rig;
    const auto snapshot = rig.publish (0);

    CHECK (snapshot->find ("/wfs/input/1/positionX") != nullptr);
    CHECK (snapshot->find ("/wfs/wfs/input/1/positionX") == nullptr);
    CHECK (snapshot->find ("/wfs") != nullptr);
}

//==============================================================================
TEST_CASE ("mount: a captured VALUE is dropped, because nobody decided it")
{
    /*  PRD §4.10. A capture says what the target happened to be doing when
        somebody pointed a browser at it. The fixture is full of them -
        positionY is 2.5, attenuation is -5, the console's mode is "run" - and
        not one survives. A mounted node has no value until something writes
        one. */
    Rig rig;
    const auto snapshot = rig.publish (0);
    int checked = 0;

    for (const auto* node : snapshot->all())
    {
        if (node->address.rfind ("/wfs", 0) != 0 && node->address.rfind ("/ext", 0) != 0)
            continue;

        INFO ("address: " << node->address);
        CHECK_FALSE (node->soleValue().has_value());
        ++checked;
    }

    CHECK (checked > 10);       // it really did look at the mounted nodes
}

TEST_CASE ("mount: every mounted node carries the declaration's metadata")
{
    MountDeclaration declaration = wfsDeclaration();
    declaration.rateCap = 12.5;
    declaration.anticipatable = true;
    declaration.panic = "snap";

    const auto file = fixtureBundle().getChildFile ("namespaces/wfs-diy.json");
    const auto result = readNamespace (declaration, file.loadFileAsString().toStdString());

    REQUIRE (result.ok);
    REQUIRE (! result.nodes.empty());

    for (const auto& node : result.nodes)
    {
        INFO ("address: " << node.address);
        CHECK (node.rateCap == doctest::Approx (12.5));
        CHECK (node.anticipatable);
        CHECK (node.panic == "snap");
    }
}

TEST_CASE ("mount: the kind is inferred when the file does not say")
{
    Rig rig;
    const auto snapshot = rig.publish (0);

    const auto kindAt = [&snapshot] (const std::string& address)
    {
        const auto* node = snapshot->find (address);
        REQUIRE_MESSAGE (node != nullptr, "no node at " << address);
        return node->kind;
    };

    // Write-only with no VALUE: there is nothing to ask it at a given time.
    CHECK (kindAt ("/ext/console/go") == Kind::event);

    // Readable, so it has a value even though the capture's copy was dropped.
    CHECK (kindAt ("/wfs/input/1/positionX") == Kind::state);
    CHECK (kindAt ("/wfs/input/1/channelType") == Kind::state);

    // No type, and children.
    CHECK (kindAt ("/wfs/input/1") == Kind::container);
    CHECK (kindAt ("/wfs/input") == Kind::container);
}

TEST_CASE ("mount: a GODOT key overrides what inference would have said")
{
    /*  The point of PRD §3.22: a hand-written template declares what a captured
        one can only imply, and the engine cannot tell the two apart. `blackout`
        is readable and carries a VALUE, so inference would call it state; the
        file says event and the file wins. */
    Rig rig;
    const auto snapshot = rig.publish (0);

    const auto* blackout = snapshot->find ("/ext/console/blackout");
    REQUIRE (blackout != nullptr);

    CHECK (blackout->kind == Kind::event);
    CHECK (blackout->access == Access::readWrite);      // the access is still the file's
    CHECK_FALSE (blackout->soleValue().has_value());          // an event has no value at a given time

    // And the other three declarations override the mount's defaults.
    const auto* master = snapshot->find ("/ext/console/masterLevel");
    REQUIRE (master != nullptr);

    CHECK (master->rateCap == doctest::Approx (25.0));
    CHECK (master->anticipatable);
    CHECK (master->panic == "snap");
    CHECK (master->unit == "dB");

    // A node without the key keeps the mount's defaults.
    const auto* mode = snapshot->find ("/ext/console/mode");
    REQUIRE (mode != nullptr);

    CHECK (mode->rateCap == doctest::Approx (50.0));
    CHECK_FALSE (mode->anticipatable);
    CHECK (mode->panic == "park");
    CHECK (mode->enumValues == std::vector<std::string> { "blind", "run", "program" });
}

//==============================================================================
/*  A PANIC ARRAY IS THE NODE'S SAFE VALUE (2026-10-02, H5, namespace draft
    §23.11). §3 lets a template's GODOT.PANIC be "park", "snap" or a JSON array
    holding the declared safe VALUE - the resting state PRD §4.6 asks of every
    parameter. The reader took the key's text whatever it was, so an array read
    as "" and went back out as `"PANIC": ""`: a resting state that says nothing,
    from a file that had said exactly what it meant. Nothing applies the value
    yet (devplan Phase 10); what is held here is that it is read as the node's
    own type, refused when the node could never hold it, and published as the
    array it was. */
namespace
{
    MountDeclaration deskDeclaration()
    {
        MountDeclaration declaration;
        declaration.id = "K3PV7WRB";
        declaration.prefix = "/desk";
        declaration.namespaceFile = "namespaces/desk.json";
        return declaration;
    }

    /*  A caller keeps a copy of the node, not a reference: GCC 13's
        -Wdangling-reference takes the address string made for the call
        for what the returned node lives in. */
    const Node& mountedAt (const MountResult& result, const std::string& address)
    {
        const auto found = std::find_if (result.nodes.begin(), result.nodes.end(),
                                         [&address] (const Node& node) { return node.address == address; });

        REQUIRE_MESSAGE (found != result.nodes.end(), "no mounted node at " << address);
        return *found;
    }

    /*  A description holding one node, `/fader` - the shape every refusal
        below is tried on. */
    std::string oneNode (const std::string& node)
    {
        return R"({ "FULL_PATH": "/", "CONTENTS": { "fader": )" + node + " } }";
    }
}

TEST_CASE ("mount: a PANIC array is the node's safe value, read as the node's own type, and goes back out as one")
{
    constexpr const char* json = R"JSON({
      "FULL_PATH": "/",
      "CONTENTS": {
        "fader": { "TYPE": "f", "ACCESS": 3, "RANGE": [{ "MIN": -60, "MAX": 10 }], "GODOT": { "PANIC": [-60] } },
        "trim":  { "TYPE": "d", "ACCESS": 3, "GODOT": { "PANIC": [-0.5] } },
        "scene": { "TYPE": "i", "ACCESS": 3, "GODOT": { "PANIC": [0] } },
        "mode":  { "TYPE": "s", "ACCESS": 3, "RANGE": [{ "VALS": ["blind", "run"] }], "GODOT": { "PANIC": ["blind"] } },
        "mute":  { "TYPE": "T", "ACCESS": 3, "GODOT": { "PANIC": [true] } },
        "eq":    { "TYPE": "if", "ACCESS": 3, "GODOT": { "PANIC": [2, -6.5] } },
        "pan":   { "TYPE": "f", "ACCESS": 3, "GODOT": { "PANIC": "snap" } },
        "gain":  { "TYPE": "f", "ACCESS": 3 }
      }
    })JSON";

    const auto result = readNamespace (deskDeclaration(), json);

    std::string problems;

    for (const auto& problem : result.problems)
        problems += problem + "\n";

    INFO ("problems: " << problems);
    REQUIRE (result.ok);

    /*  EACH AS ITS OWN TYPE TAG READS IT: a JSON number is a float on an `f`
        node, an int on an `i` one, and one value per argument on a node that
        takes two. */
    const auto fader = mountedAt (result, "/desk/fader");
    CHECK (fader.panic == "value");
    CHECK (fader.panicValues == std::vector<osc::Value> { osc::Value::float32 (-60.0f) });

    CHECK (mountedAt (result, "/desk/trim").panicValues == std::vector<osc::Value> { osc::Value::float64 (-0.5) });
    CHECK (mountedAt (result, "/desk/scene").panicValues == std::vector<osc::Value> { osc::Value::int32 (0) });
    CHECK (mountedAt (result, "/desk/mode").panicValues == std::vector<osc::Value> { osc::Value::string ("blind") });
    CHECK (mountedAt (result, "/desk/mute").panicValues == std::vector<osc::Value> { osc::Value::boolean (true) });
    CHECK (mountedAt (result, "/desk/eq").panicValues
             == std::vector<osc::Value> { osc::Value::int32 (2), osc::Value::float32 (-6.5f) });

    //  A policy is still a word, and a node that says nothing has the declaration's.
    const auto pan = mountedAt (result, "/desk/pan");
    CHECK (pan.panic == "snap");
    CHECK (pan.panicValues.empty());

    const auto gain = mountedAt (result, "/desk/gain");
    CHECK (gain.panic == "park");
    CHECK (gain.panicValues.empty());

    /*  AND IT GOES BACK OUT AS THE ARRAY IT WAS, spelled as VALUE is - which is
        the one place a decimal could meet the French locale, so `-0.5` is
        checked under both. */
    const TreeSnapshot snapshot { 0, std::make_shared<const std::vector<Node>>(),
                                  std::make_shared<const std::vector<Node>> (result.nodes), {} };

    const auto publishes = [&snapshot] (const std::string& address, const std::string& panic)
    {
        const auto text = OscQueryJson::describe (snapshot, address);
        INFO (address << ": " << text);
        CHECK (text.find ("\"PANIC\": " + panic) != std::string::npos);
    };

    publishes ("/desk/fader", "[-60]");
    publishes ("/desk/trim", "[-0.5]");
    publishes ("/desk/mode", "[\"blind\"]");
    publishes ("/desk/mute", "[true]");
    publishes ("/desk/eq", "[2, -6.5]");
    publishes ("/desk/pan", "\"snap\"");
    publishes ("/desk/gain", "\"park\"");
}

TEST_CASE ("mount: a PANIC the node could never hold is a warning, and the device still loads")
{
    /*  A WARNING, NOT A REFUSAL (2026-10-02, K1, decision JT, the author's:
        "stay flexible"; it overrules JC, which refused the namespace). A GODOT
        key is written by whoever wrote the description, a template by hand or
        a device describing itself as Go.dot does, and a PANIC that is not a
        value of the node's own type is a mistake in that file - but one wrong
        word in somebody else's description must not unmount the whole device
        and fail every cue aimed at it. So the device loads, the PANIC is
        ignored and the node rests as the mount says, as on a container or an
        event, and the mistake is said: a warning naming the address and PANIC,
        never silence.

        "snap-to" is here because PRD §3.3 spelled the policy that way until
        2026-10-02; it says `snap` now, as the schema, show.rng and this reader
        do, so "snap-to" is just a word the reader does not know. */
    struct Ignored { std::string why, node; };

    const std::vector<Ignored> ignored
    {
        { "out of the node's range",   R"({ "TYPE": "f", "ACCESS": 3, "RANGE": [{ "MIN": -60, "MAX": 0 }], "GODOT": { "PANIC": [5] } })" },
        { "a word for a number",       R"({ "TYPE": "f", "ACCESS": 3, "GODOT": { "PANIC": ["loud"] } })" },
        { "an empty array",            R"({ "TYPE": "f", "ACCESS": 3, "GODOT": { "PANIC": [] } })" },
        { "two values for one tag",    R"({ "TYPE": "f", "ACCESS": 3, "GODOT": { "PANIC": [0, 1] } })" },
        { "an unknown word",           R"({ "TYPE": "f", "ACCESS": 3, "GODOT": { "PANIC": "freeze" } })" },
        { "the PRD's own spelling",    R"({ "TYPE": "f", "ACCESS": 3, "GODOT": { "PANIC": "snap-to" } })" },
        { "an object",                 R"({ "TYPE": "f", "ACCESS": 3, "GODOT": { "PANIC": { "x": 1 } } })" },
        { "a bare number",             R"({ "TYPE": "f", "ACCESS": 3, "GODOT": { "PANIC": 0 } })" },
        { "a null in the array",       R"({ "TYPE": "f", "ACCESS": 3, "GODOT": { "PANIC": [null] } })" },
        { "past what a float holds",   R"({ "TYPE": "f", "ACCESS": 3, "GODOT": { "PANIC": [1e300] } })" },
        { "not whole, on an int",      R"({ "TYPE": "i", "ACCESS": 3, "GODOT": { "PANIC": [2.5] } })" },
        { "past what an int holds",    R"({ "TYPE": "i", "ACCESS": 3, "GODOT": { "PANIC": [3000000000] } })" },
        { "not one of the values",     R"({ "TYPE": "s", "ACCESS": 3, "RANGE": [{ "VALS": ["slow", "medium"] }], "GODOT": { "PANIC": ["fast"] } })" },
        { "a number for a boolean",    R"({ "TYPE": "T", "ACCESS": 3, "GODOT": { "PANIC": [1] } })" },
    };

    for (const auto& entry : ignored)
    {
        INFO (entry.why << ": " << entry.node);

        const auto result = readNamespace (deskDeclaration(), oneNode (entry.node));

        INFO ("first problem: " << (result.problems.empty() ? std::string ("none") : result.problems.front()));
        CHECK (result.ok);
        CHECK (result.problems.empty());

        /*  THE MOUNT'S POLICY, AND NO VALUE: what the node would have had with
            no PANIC key at all. */
        if (result.nodes.empty())
            continue;

        const auto fader = mountedAt (result, "/desk/fader");
        CHECK (fader.panic == "park");
        CHECK (fader.panicValues.empty());

        //  And said, once, naming where and what.
        CHECK (result.warnings.size() == 1);

        if (result.warnings.empty())
            continue;

        INFO ("warning: " << result.warnings.front());
        CHECK (result.warnings.front().find ("/desk/fader") != std::string::npos);
        CHECK (result.warnings.front().find ("PANIC") != std::string::npos);
    }

    /*  AND THE SAME NODE, GIVEN A VALUE IT CAN HOLD, TAKES IT AND WARNS OF
        NOTHING - so the table above is ignoring what it names and not
        everything. A device whose own policy is `snap` rests there too. */
    const auto taken = readNamespace (deskDeclaration(),
                                      oneNode (R"({ "TYPE": "f", "ACCESS": 3, "RANGE": [{ "MIN": -60, "MAX": 0 }], "GODOT": { "PANIC": [-6] } })"));
    REQUIRE (taken.ok);
    CHECK (taken.warnings.empty());
    CHECK (mountedAt (taken, "/desk/fader").panic == "value");

    auto snapping = deskDeclaration();
    snapping.panic = "snap";

    const auto mistaken = readNamespace (snapping, oneNode (R"({ "TYPE": "f", "ACCESS": 3, "GODOT": { "PANIC": "snap-to" } })"));
    REQUIRE (mistaken.ok);
    CHECK (mistaken.warnings.size() == 1);
    CHECK (mountedAt (mistaken, "/desk/fader").panic == "snap");
}

TEST_CASE ("mount: a device's PANIC warning reaches the show's warnings, and the device is not a failed one")
{
    /*  WHERE THE WARNING GOES (K1). A mount's `problem` row is painted as a
        failure - it says why a device cannot be used - and this device can.
        The show already has a place for what is wrong but does not stop it
        opening, `/godot/document/warnings`, which every client counts and
        shows the first of; the warning goes there, and to the lines a show
        prints as it opens. The fixture's console says "snap" on its master;
        spelled "snap-to", as the PRD spelled it until 2026-10-02, it loads. */
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    const auto scratch = copyFixtureToScratch();
    const auto file = scratch.getChildFile ("namespaces/console.json");
    const auto text = file.loadFileAsString();

    REQUIRE (text.contains ("\"PANIC\": \"snap\""));
    REQUIRE (file.replaceWithText (text.replace ("\"PANIC\": \"snap\"", "\"PANIC\": \"snap-to\"")));

    Engine engine;
    doc::ShowDocument document;
    MountTable mounts;
    cue::RunTable runs;

    REQUIRE (doc::Bundle::open (scratch, document).ok);

    const auto lines = loadAllMountsFromBundle (document, mounts, scratch);

    CHECK (mounts.isLoaded ("H2KP7RTV"));
    CHECK (mounts.problemOf ("H2KP7RTV").empty());
    CHECK (std::any_of (lines.begin(), lines.end(), [] (const std::string& line)
                        { return line.find ("/ext/console/masterLevel") != std::string::npos
                                   && line.find ("PANIC") != std::string::npos; }));

    {
        ParameterTree parameters { document, engine.commands(), mounts, runs };
        parameters.markStale();

        EngineState state;
        const auto snapshot = parameters.publish (0, state);

        const auto* master = snapshot->find ("/ext/console/masterLevel");
        REQUIRE (master != nullptr);
        CHECK (master->panic == "park");

        const auto* warnings = snapshot->find ("/godot/document/warnings");
        REQUIRE (warnings != nullptr);
        REQUIRE (warnings->soleValue().has_value());

        const auto said = warnings->soleValue()->getString();
        INFO ("document warnings: " << said);
        CHECK (said.find ("/ext/console/masterLevel: PANIC") != std::string::npos);

        const auto* problem = snapshot->find ("/godot/mount/H2KP7RTV/problem");
        REQUIRE (problem != nullptr);
        CHECK ((! problem->soleValue().has_value() || problem->soleValue()->getString().empty()));
    }

    scratch.getParentDirectory().deleteRecursively();
}

TEST_CASE ("mount: a PANIC on an event or a container is ignored, and the namespace still loads")
{
    /*  NEITHER HAS A VALUE TO REST AT, AND THE KIND IS OFTEN ONLY INFERRED
        (decision JC). A node that is write-only with no VALUE reads as an
        event, so a hand-written `go` with a PANIC array loaded before
        2026-10-02 - and refusing it now would unmount the whole device, and
        every cue aimed at it would fail with `bad-address`. Anything but a
        policy word is ignored there, the mount's policy stays, and nothing is
        published: a container and an event declare their KIND and stop. */
    constexpr const char* json = R"JSON({
      "FULL_PATH": "/",
      "CONTENTS": {
        "go":   { "TYPE": "f", "ACCESS": 2, "GODOT": { "PANIC": [0] } },
        "cut":  { "TYPE": "f", "ACCESS": 2, "GODOT": { "PANIC": "loud" } },
        "bank": { "GODOT": { "PANIC": [1] },
                  "CONTENTS": { "level": { "TYPE": "f", "ACCESS": 3 } } }
      }
    })JSON";

    const auto result = readNamespace (deskDeclaration(), json);

    std::string problems;

    for (const auto& problem : result.problems)
        problems += problem + "\n";

    INFO ("problems: " << problems);
    REQUIRE (result.ok);

    for (const auto* address : { "/desk/go", "/desk/cut", "/desk/bank" })
    {
        INFO (address);
        const auto node = mountedAt (result, address);

        CHECK (node.kind != Kind::state);
        CHECK (node.panic == "park");
        CHECK (node.panicValues.empty());
    }

    CHECK (mountedAt (result, "/desk/go").kind == Kind::event);
    CHECK (mountedAt (result, "/desk/bank").kind == Kind::container);

    const TreeSnapshot snapshot { 0, std::make_shared<const std::vector<Node>>(),
                                  std::make_shared<const std::vector<Node>> (result.nodes), {} };

    CHECK (OscQueryJson::describe (snapshot, "/desk/go").find ("PANIC") == std::string::npos);
}

TEST_CASE ("mount: a multi-argument node keeps its types and its first argument's range")
{
    /*  WFS-DIY's EQ nodes take a band index and a value and carry a RANGE entry
        for each. RANGE is per argument, so entry zero really is the band
        index's - taking it is correct rather than a simplification. */
    Rig rig;
    const auto snapshot = rig.publish (0);

    const auto* eq = snapshot->find ("/wfs/output/1/EQgain");
    REQUIRE (eq != nullptr);

    CHECK (eq->typeTags == "if");
    CHECK (eq->hasMinimum);
    CHECK (eq->minimum == doctest::Approx (0.0));
    CHECK (eq->hasMaximum);
    CHECK (eq->maximum == doctest::Approx (5.0));       // bands 0-5, not the gain range
}

TEST_CASE ("mount: a range bound arrives exactly as the file spells it")
{
    /*  WFS-DIY widens a float to a double on the way out, so `0.1f` serialises
        as 0.100000001490116 - which is the number the target really enforces.
        Reading it as 0.1 would have Go.dot enforcing a bound nobody declared,
        and is precisely what JUCE's parser would have done to a longer one. */
    Rig rig;
    const auto snapshot = rig.publish (0);

    const auto* ratio = snapshot->find ("/wfs/input/1/distanceRatio");
    REQUIRE (ratio != nullptr);
    REQUIRE (ratio->hasMinimum);

    CHECK (osc::formatDouble (ratio->minimum) == "0.100000001490116");
}

//==============================================================================
TEST_CASE ("mount: a write to a read-only node is refused")
{
    Rig rig;

    /*  channelType is the one read-only node WFS-DIY publishes, and it is
        read-only because it describes the channel rather than controlling it. */
    const auto refused = rig.mounts.write ("/wfs/input/1/channelType",
                                           osc::Value::string ("stereo"));

    CHECK_FALSE (refused.ok);
    CHECK (refused.reason == reason::readOnly);
    CHECK (rig.mounts.valueOf ("/wfs/input/1/channelType") == nullptr);
}

TEST_CASE ("mount: an accepted write lands, and goes no further")
{
    Rig rig;

    const auto accepted = rig.mounts.write ("/wfs/input/1/positionX",
                                            osc::Value::float32 (12.5f));

    CHECK (accepted.ok);
    CHECK (accepted.reason.empty());

    const auto* stored = rig.mounts.valueOf ("/wfs/input/1/positionX");
    REQUIRE (stored != nullptr);
    CHECK (*stored == osc::Values { osc::Value::float32 (12.5f) });

    // And it reaches the tree.
    const auto* published = rig.publish (1)->find ("/wfs/input/1/positionX");
    REQUIRE (published != nullptr);
    REQUIRE (published->soleValue().has_value());
    CHECK (*published->soleValue() == osc::Value::float32 (12.5f));
}

TEST_CASE ("mount: a word cannot get into a number, and an int into a float can")
{
    Rig rig;

    CHECK (rig.mounts.write ("/wfs/input/1/positionX", osc::Value::string ("left")).reason
             == reason::typeMismatch);

    /*  An int into a float is coerced, because that is what the rejection rules
        say and because a great many senders cannot tell the difference. The
        rules come from CommandRegistry rather than a second copy here, so the
        log's idea of a type mismatch and this one cannot drift apart. */
    const auto coerced = rig.mounts.write ("/wfs/input/1/positionX", osc::Value::int32 (3));

    CHECK (coerced.ok);
    CHECK (*rig.mounts.valueOf ("/wfs/input/1/positionX") == osc::Values { osc::Value::float32 (3.0f) });
}

TEST_CASE ("mount: a write to an address no mount holds is refused")
{
    /*  The three subjects are chosen so that no capture can accidentally contain
        them. An earlier version used input channel 9, which was absent from the
        hand-written placeholder this suite began with and is present in the real
        capture that replaced it - the assertion passed for a reason that had
        nothing to do with what it was testing. A leaf nobody named, a channel
        past the maximum WFS-DIY can be configured for, and an address under no
        prefix at all cannot go the same way. */
    Rig rig;

    CHECK (rig.mounts.write ("/wfs/input/1/noSuchParameter", osc::Value::float32 (0.0f)).reason
             == reason::badAddress);
    CHECK (rig.mounts.write ("/wfs/input/999/positionX", osc::Value::float32 (0.0f)).reason
             == reason::badAddress);
    CHECK (rig.mounts.write ("/nowhere", osc::Value::float32 (0.0f)).reason == reason::badAddress);
}

TEST_CASE ("mount: a reload forgets what was written to it")
{
    /*  The namespace may have changed shape underneath. Carrying a value across
        a reload would assert something nobody checked: that the node still
        exists, still means the same thing, and still holds that value on a box
        we have not spoken to. */
    Rig rig;

    REQUIRE (rig.mounts.write ("/wfs/input/1/positionX", osc::Value::float32 (7.0f)).ok);
    REQUIRE (rig.mounts.valueOf ("/wfs/input/1/positionX") != nullptr);

    REQUIRE (loadMountFromBundle (rig.document, rig.mounts, rig.folder, "G1JS4VWE").ok);

    CHECK (rig.mounts.valueOf ("/wfs/input/1/positionX") == nullptr);
}

//==============================================================================
TEST_CASE ("mount: a prefix that would swallow the tree, or is malformed, is refused")
{
    const std::string description =
        R"({"FULL_PATH": "/", "CONTENTS": {"x": {"FULL_PATH": "/x", "TYPE": "f", "ACCESS": 3}}})";

    /*  `/ui` and anything under it joins the list, because that is where the
        OSCQuery server answers with the client rather than with the tree. A
        mount there would be published and unreachable at once - visible in a
        tree dump, and answering HTML to anybody who asked for it over HTTP - so
        it is refused when the show is read rather than discovered during it. */
    for (const auto& prefix : { "", "/", "wfs", "/wfs/", "/a//b", "/ui", "/ui/desk" })
    {
        INFO ("prefix: \"" << prefix << "\"");

        MountDeclaration declaration;
        declaration.id = "TEST0000";
        declaration.prefix = prefix;

        CHECK_FALSE (readNamespace (declaration, description).ok);
    }

    MountDeclaration good;
    good.id = "TEST0000";
    good.prefix = "/ext/thing";

    CHECK (readNamespace (good, description).ok);
}

TEST_CASE ("mount: the addresses the engine serves over HTTP are reserved against mounts")
{
    /*  `/ui` answers with the client and `/media` answers with a timbre
        pyramid, both on the same port as the tree, so a mount at either would
        be published and unreachable at once. `/media` is reserved before the
        route that answers there exists, because a reservation is worth more
        before somebody's show file has used the prefix than after - and because
        a collision with a content-addressed route would read as a cache miss
        rather than as a collision. */
    const std::string description =
        R"({"FULL_PATH": "/", "CONTENTS": {"x": {"FULL_PATH": "/x", "TYPE": "f", "ACCESS": 3}}})";

    struct Refusal
    {
        const char* prefix;     ///< what the file asked for
        const char* reserved;   ///< the reservation it collides with, which the message names
    };

    for (const auto& refusal : { Refusal { "/ui",    "/ui" },
                                 Refusal { "/ui/desk", "/ui" },
                                 Refusal { "/media", "/media" },
                                 Refusal { "/media/timbre", "/media" } })
    {
        INFO ("prefix: \"" << refusal.prefix << "\"");

        MountDeclaration declaration;
        declaration.id = "TEST0000";
        declaration.prefix = refusal.prefix;

        const auto result = readNamespace (declaration, description);

        CHECK_FALSE (result.ok);

        /*  The message names the reserved address rather than only saying no,
            because an operator reading it at load has a file in front of them
            and needs to know which line of it to change. */
        REQUIRE (! result.problems.empty());
        INFO ("problem: " << result.problems.front());
        CHECK (result.problems.front().find (refusal.reserved) != std::string::npos);
    }

    /*  AND NOT A PREFIX THAT MERELY BEGINS WITH THE SAME LETTERS, which is the
        boundary bug this kind of check has whenever it is written as a
        starts-with. `/mediaserver` is somebody's perfectly ordinary box and
        collides with nothing; refusing it would be a bug of ours reported as a
        fault in their show file. */
    for (const auto& prefix : { "/mediaserver", "/uiserver", "/media2" })
    {
        INFO ("prefix: \"" << prefix << "\"");

        MountDeclaration declaration;
        declaration.id = "TEST0000";
        declaration.prefix = prefix;

        CHECK (readNamespace (declaration, description).ok);
    }
}

TEST_CASE ("mount: a description that is not JSON, or describes nothing, is refused")
{
    MountDeclaration declaration;
    declaration.id = "TEST0000";
    declaration.prefix = "/ext/thing";

    CHECK_FALSE (readNamespace (declaration, "not json at all").ok);
    CHECK_FALSE (readNamespace (declaration, "[1, 2, 3]").ok);
    CHECK_FALSE (readNamespace (declaration, "").ok);
}

TEST_CASE ("mount: a FULL_PATH that disagrees with the nesting is reported")
{
    /*  They agree in any well-formed description. When they do not, one is a
        lie and the nesting is the one that cannot be - so the file is refused
        and the message says both. */
    const std::string description =
        R"({"FULL_PATH": "/",
            "CONTENTS": {"x": {"FULL_PATH": "/somewhere/else", "TYPE": "f", "ACCESS": 3}}})";

    MountDeclaration declaration;
    declaration.id = "TEST0000";
    declaration.prefix = "/ext/thing";

    const auto result = readNamespace (declaration, description);

    CHECK_FALSE (result.ok);
    REQUIRE (! result.problems.empty());
    INFO ("problem: " << result.problems.front());
    CHECK (result.problems.front().find ("FULL_PATH") != std::string::npos);
}

//==============================================================================
TEST_CASE ("mount.load: it is a command, so a replay reproduces what was mounted")
{
    Rig rig;

    REQUIRE (rig.engine.submit ("cli", "mount.load", { osc::Value::string ("G1JS4VWE") }));
    CHECK (rig.engine.processTick (1).applied == 1);

    // A mount the show does not declare.
    REQUIRE (rig.engine.submit ("cli", "mount.load", { osc::Value::string ("ZZZZZZZZ") }));
    CHECK (rig.engine.processTick (2).rejected == 1);
    CHECK (rig.engine.lastError().find (reason::unknownId) != std::string::npos);
}

TEST_CASE ("mount.load: a declared mount whose file is broken says so specifically")
{
    /*  bad-namespace rather than unknown-id or bad-address: the mount was named
        correctly and what failed is the file it points at, which is somebody
        else's and is the thing to go and look at. */
    const auto scratch = copyFixtureToScratch();

    doc::ShowDocument document;
    REQUIRE (doc::Bundle::open (scratch, document).ok);

    MountTable mounts;
        cue::RunTable runs;
    Engine engine;
    registerMountCommands (engine.commands(), document, mounts, scratch);

    REQUIRE (scratch.getChildFile ("namespaces/console.json").replaceWithText ("{ broken"));

    REQUIRE (engine.submit ("cli", "mount.load", { osc::Value::string ("H2KP7RTV") }));
    CHECK (engine.processTick (1).rejected == 1);
    CHECK (engine.lastError().find (reason::badNamespace) != std::string::npos);

    // And a failed load leaves nothing mounted rather than half a namespace.
    CHECK_FALSE (mounts.isLoaded ("H2KP7RTV"));

    scratch.getParentDirectory().deleteRecursively();
}

//==============================================================================
TEST_CASE ("mount: /godot/mount says whether it loaded and how much it brought")
{
    Rig rig;

    const auto valueAt = [] (const std::shared_ptr<const TreeSnapshot>& tree,
                             const std::string& address)
    {
        const auto* node = tree->find (address);
        REQUIRE_MESSAGE (node != nullptr, "no node at " << address);
        REQUIRE (node->soleValue().has_value());
        return *node->soleValue();
    };

    const auto snapshot = rig.publish (0);

    CHECK (valueAt (snapshot, "/godot/mount/G1JS4VWE/loaded") == osc::Value::boolean (true));
    CHECK (valueAt (snapshot, "/godot/mount/G1JS4VWE/nodeCount")
             == osc::Value::int32 (static_cast<std::int32_t> (rig.mounts.nodeCount ("G1JS4VWE"))));

    /*  Those two come from the engine rather than the file: they answer "did it
        actually work", which is the question somebody asks when a target is not
        responding. */
    rig.mounts.unload ("G1JS4VWE");
    const auto after = rig.publish (1);

    CHECK (valueAt (after, "/godot/mount/G1JS4VWE/loaded") == osc::Value::boolean (false));
    CHECK (valueAt (after, "/godot/mount/G1JS4VWE/nodeCount") == osc::Value::int32 (0));
    CHECK (after->find ("/wfs/input/1/positionX") == nullptr);
}

//==============================================================================
TEST_CASE ("bundle: the log header hashes the namespace files too")
{
    /*  A session that read one description of somebody else's box behaves
        differently from one that read another, so a replay against a changed
        description is not a replay. The hash lets the log refuse instead of
        diverging and blaming the engine. */
    const auto scratch = copyFixtureToScratch();

    const auto before = doc::Bundle::contentHash (scratch);

    CHECK (before.length() == 64);                          // SHA-256, hex
    CHECK (before == doc::Bundle::contentHash (scratch));   // and it is stable

    // Change a namespace file and nothing else.
    const auto namespaceFile = scratch.getChildFile ("namespaces/console.json");
    const auto text = namespaceFile.loadFileAsString();
    REQUIRE (namespaceFile.replaceWithText (text.replace ("Grand master", "Master")));

    CHECK (doc::Bundle::contentHash (scratch) != before);

    // The header line names the bundle and carries the hash.
    const auto lines = doc::Bundle::logHeaderLines (scratch);

    REQUIRE (lines.size() == 1);
    INFO ("header: " << lines.front());
    CHECK (lines.front().rfind ("bundle minimal sha256:", 0) == 0);

    scratch.getParentDirectory().deleteRecursively();
}

TEST_CASE ("bundle: renaming a namespace file changes the hash")
{
    /*  The path is hashed as well as the bytes, so a mount pointed at a renamed
        file is a different session even when the contents match. */
    const auto scratch = copyFixtureToScratch();

    const auto before = doc::Bundle::contentHash (scratch);
    const auto original = scratch.getChildFile ("namespaces/console.json");

    REQUIRE (original.moveFileTo (scratch.getChildFile ("namespaces/desk.json")));

    CHECK (doc::Bundle::contentHash (scratch) != before);

    scratch.getParentDirectory().deleteRecursively();
}

//==============================================================================
TEST_CASE ("json: our reader is exact, and JUCE's is why we have one")
{
    /*  THE MEASUREMENT THAT PUT A JSON PARSER IN THIS PROJECT.

        JUCE accumulates a plain integer literal into an int64, one digit at a
        time, and only abandons that for the correctly-rounded floating path
        when it meets a "." or an "e" (juce_JSON.cpp, parseNumber). A literal
        with neither, and more digits than an int64 holds, therefore overflows
        in silence. Ours measures the token out by JSON's own grammar and hands
        it to strtod in a C locale, which is what the document, the log and the
        OSC atoms already use.

        It matters because a namespace file's numbers are somebody else's range
        bounds, and a bound that changes on the way in is a bound Go.dot would
        enforce against a target that never declared it.

        IF THE JUCE HALF EVER FAILS, JUCE has fixed parseNumber and whether to
        keep our own reader is worth reopening. A test should notice that, so
        this one is written to. */
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    std::mt19937_64 generator { 20260906 };
    int tried = 0;
    int ourFailures = 0;
    int juceFailures = 0;
    std::string firstJuceFailure;

    for (int i = 0; i < 20000; ++i)
    {
        const auto bits = generator();
        double original = 0.0;
        std::memcpy (&original, &bits, sizeof (double));

        if (! std::isfinite (original))
            continue;

        ++tried;

        const auto text = osc::formatDouble (original);
        const auto document = "{\"v\": " + text + "}";

        //----------------------------------------------------------------------
        const auto ours = json::parse (document);
        const auto* member = ours.ok() ? ours.value->find ("v") : nullptr;

        if (member == nullptr)
        {
            ++ourFailures;
        }
        else
        {
            const auto back = member->asNumber();

            if (std::memcmp (&original, &back, sizeof (double)) != 0)
                ++ourFailures;
        }

        //----------------------------------------------------------------------
        juce::var theirs;
        double theirBack = 0.0;

        if (juce::JSON::parse (juce::String (document), theirs).wasOk())
            theirBack = static_cast<double> (theirs.getProperty ("v", {}));

        if (std::memcmp (&original, &theirBack, sizeof (double)) != 0)
        {
            if (juceFailures == 0)
                firstJuceFailure = "wrote " + text + ", JUCE read back "
                                     + osc::formatDouble (theirBack);

            ++juceFailures;
        }
    }

    INFO ("tried " << tried << "; ours " << ourFailures << ", JUCE " << juceFailures
                   << "; " << firstJuceFailure);

    CHECK (ourFailures == 0);
    CHECK (juceFailures > 0);
}

//==============================================================================
/*  M9 - WHAT ONE `node.set` COSTS WHEN A REAL PROCESSOR IS MOUNTED.

    THE QUESTION, from the Phase 3 plan: the document half of the tree is
    rebuilt whole on ANY applied mutation, and the mounted namespace is part of
    it. With WFS-DIY's own capture that is a megabyte of JSON's worth of nodes
    re-materialised and re-sorted every time somebody writes a cue's name.

    Phase 1 knew and said so - "when there is a show big enough to measure,
    measure it" - and Phase 3 is what makes it matter: a trigger firing forty
    times a minute is forty rebuilds a minute, on the thread that owns the model
    and has twenty milliseconds to do everything in.

    WHAT IS MEASURED. The wall clock of `markStale()` plus `publish()` with the
    capture mounted, against the same thing with nothing mounted. The difference
    is what the mounted half costs per mutation, which is the number that
    decides whether it has to be split out and cached separately.

    IT REPORTS, IT DOES NOT GATE, like M3 and M11: a wall-clock threshold
    asserted on a shared CI runner is a flaky test that teaches people to re-run
    the suite, and Debug is not the number a show runs at. The numbers go in the
    PR and the decision is taken from them.
*/
TEST_CASE ("M9: what a mounted processor costs on every applied mutation")
{
    /*  A hundred publishes, so one slow one does not become the answer, and the
        two configurations measured the same way in one process. */
    constexpr int publishes = 100;

    const auto timePublishes = [] (ParameterTree& parameters, int howMany)
    {
        /*  One outside the timing, so what is measured is a REBUILD rather than
            a first build - the caches Tracktion and the allocator warm on the
            way through are not what this is about. */
        EngineState state;
        parameters.markStale();
        parameters.publish (0, state);

        const auto start = std::chrono::steady_clock::now();

        for (int n = 0; n < howMany; ++n)
        {
            parameters.markStale();
            parameters.publish (n, state);
        }

        const auto elapsed = std::chrono::steady_clock::now() - start;

        return std::chrono::duration<double, std::milli> (elapsed).count()
                 / static_cast<double> (howMany);
    };

    Rig withMount;
    const auto mounted = withMount.publish (0);

    REQUIRE (mounted != nullptr);

    /*  The same document with the mount table emptied, so the difference is the
        mounted nodes and nothing else - same show, same commands, same sort. */
    MountTable empty;
    cue::RunTable runs;
    ParameterTree bare { withMount.document, withMount.engine.commands(), empty, runs };

    EngineState state;
    bare.markStale();
    const auto without = bare.publish (0, state);

    REQUIRE (without != nullptr);

    const auto mountedNodes = static_cast<int> (mounted->all().size());
    const auto bareNodes = static_cast<int> (without->all().size());

    const auto withCost = timePublishes (withMount.parameters, publishes);
    const auto withoutCost = timePublishes (bare, publishes);

    MESSAGE ("M9: " << mountedNodes << " nodes with WFS-DIY mounted, " << bareNodes
              << " without - so the mount is " << (mountedNodes - bareNodes) << " of them");

    MESSAGE ("M9: one applied mutation costs " << juce::String (withCost, 3)
              << " ms with the mount and " << juce::String (withoutCost, 3)
              << " ms without; the mounted half is " << juce::String (withCost - withoutCost, 3)
              << " ms of that");

    MESSAGE ("M9: a 20 ms tick is " << juce::String (100.0 * withCost / 20.0, 1)
              << "% spent on one rebuild");

    /*  THE MOUNT REALLY IS THE BULK OF IT, or this measured two versions of the
        same small thing and the comparison says nothing. WFS-DIY's capture is a
        megabyte; the minimal show is four cues. */
    CHECK (mountedNodes > bareNodes * 4);

    /*  And both configurations produced a tree, which is the part that can be
        asserted honestly. */
    CHECK (withCost > 0.0);
    CHECK (withoutCost > 0.0);

    /*  WHAT THE SPLIT GUARANTEES, COUNTED RATHER THAN TIMED.

        The numbers above are a wall clock and a wall-clock threshold on a
        shared runner is a flaky test that teaches people to re-run the suite.
        What the split actually promises is exact: editing the SHOW does not
        rebuild the mounted half at all. A hundred and one publishes have
        happened by now - the timing loop's, plus the ones before it - and the
        mounted half was built ONCE.

        Put back the way it was, this is a hundred and one. */
    INFO ("mounted rebuilds after " << (publishes + 2) << " publishes");
    CHECK (withMount.parameters.mountRebuilds() == 1u);

    /*  A WRITE DOES NOT REBUILD IT EITHER (namespace draft 45, ZC): an OSC
        cue's curve writes a node every tick, and the node written is laid over
        the cached half instead - its new value in the published tree, the half
        built once. Written through the table, so the invalidation is the
        production path and not a test reaching past it. */
    const auto address = std::string ("/wfs/input/1/positionX");

    if (withMount.mounts.nodeAt (address) != nullptr)
    {
        REQUIRE (withMount.mounts.write (address, osc::Value::float32 (0.25f)).ok);

        const auto after = withMount.publish (1);
        CHECK (withMount.parameters.mountRebuilds() == 1u);

        /*  And the new value is in the published tree, which is what the cache
            existed to keep true - by the lookup and by the walk alike. */
        const auto* node = after->find (address);

        REQUIRE (node != nullptr);
        REQUIRE (node->soleValue().has_value());
        CHECK (node->soleValue()->getFloat32() == doctest::Approx (0.25f));

        const auto walked = after->all();
        const auto atAddress = std::find_if (walked.begin(), walked.end(),
                                             [&address] (const Node* n) { return n->address == address; });
        REQUIRE (atAddress != walked.end());
        CHECK (*atAddress == node);
        CHECK (walked.size() == after->size());

        /*  AND IT DOES REBUILD WHEN THE MOUNT MOVES, which is the other half of
            a cache being right: a write after a change of shape is in the new
            half, nothing left to lay over it. */
        REQUIRE (withMount.mounts.write (address, osc::Value::float32 (0.5f)).ok);
        withMount.mounts.setProblem ("nobody", "a change of shape");

        const auto rebuilt = withMount.publish (2);
        CHECK (withMount.parameters.mountRebuilds() == 2u);
        REQUIRE (rebuilt->find (address) != nullptr);
        CHECK (rebuilt->find (address)->soleValue()->getFloat32() == doctest::Approx (0.5f));
    }
}


//==============================================================================
/*  OBSERVATIONS: what the target said when nobody was waiting for it (§13.10).
*/
TEST_CASE ("observation: kept apart from a read-back, and ended by a write")
{
    /*  Three stores for one address, and each is a different fact. What Go.dot
        WROTE is the decision; what the target SAID TO A WAITING CUE is that
        cue's evidence; what the target was SEEN to hold is the freshest thing
        known about the room. Letting any two share a slot would let one answer
        stand in for another - a periodic sweep making a verification pass by
        construction, or a verify's stale answer telling a jump the desk still
        holds what it held a minute ago. */
    Rig rig;

    const std::string address = "/wfs/input/1/positionX";

    CHECK (rig.mounts.observedOf (address) == nullptr);

    rig.mounts.noteObservation (address, osc::Value::float32 (0.25f));

    REQUIRE (rig.mounts.observedOf (address) != nullptr);
    CHECK (*rig.mounts.observedOf (address) == osc::Values { osc::Value::float32 (0.25f) });

    /*  Not a read-back: nobody asked on a cue's behalf. */
    CHECK (rig.mounts.readbackOf (address) == nullptr);

    /*  And a read-back is not an observation either. */
    rig.mounts.noteReadback (address, osc::Value::float32 (0.5f));
    CHECK (*rig.mounts.observedOf (address) == osc::Values { osc::Value::float32 (0.25f) });

    /*  A WRITE ENDS IT. The next sweep will say what the target holds after
        this write; until then the written value is the best account there is,
        and the observation from before it would be a lie about now. */
    REQUIRE (rig.mounts.write (address, osc::Value::float32 (0.75f)).ok);
    CHECK (rig.mounts.observedOf (address) == nullptr);

    /*  The read-back survives the write: forgetting THAT is the Runner's job,
        done at the moment it asks, so a verify cannot be satisfied by an answer
        to a question it did not put. */
    CHECK (rig.mounts.readbackOf (address) != nullptr);

    rig.mounts.forgetObservation (address);        // a no-op on nothing
    CHECK (rig.mounts.observedOf (address) == nullptr);
}

TEST_CASE ("M21: what an observation sweep costs, in the two shapes it could take")
{
    /*  THE QUESTION §13.10 LEFT OPEN: at every step, ask the target about
        everything the show writes to it - as one GET of the subtree the mount
        covers, or as one GET per written address?

        The two are measured where they differ, which is what has to be
        digested afterwards. A subtree reply is the whole capture: WFS-DIY
        describes itself in about a megabyte, and the parse that turns it into
        nodes is the one `MountTable::load` does. A per-address reply is a few
        dozen bytes with one VALUE in it. What the network costs is a round trip
        either way, times one or times N, and is stated rather than measured -
        it is the same round trip the verify already pays.

        THE NUMBER THAT DECIDES IT IS N. A 500-cue show writes about forty
        distinct addresses (M20 counted them), and a capture holds thousands of
        nodes; a sweep asks about what the show WRITES, not about the namespace.
        The measurement below says how far apart the two shapes are at that N,
        and at the N where they would meet. */
    const auto file = fixtureBundle().getChildFile ("namespaces/wfs-diy.json");
    const auto text = file.loadFileAsString().toStdString();
    REQUIRE (! text.empty());

    auto declaration = wfsDeclaration();
    declaration.readback = "oscquery";
    declaration.queryPort = 5005;

    constexpr int rounds = 10;

    const auto subtreeStart = std::chrono::steady_clock::now();
    int nodes = 0;

    for (int n = 0; n < rounds; ++n)
    {
        MountTable table;
        REQUIRE (table.load (declaration, text).ok);
        nodes = static_cast<int> (table.nodeCount (declaration.id));
    }

    const auto subtreeMs = std::chrono::duration<double, std::milli> (
                             std::chrono::steady_clock::now() - subtreeStart).count() / rounds;

    /*  One reply, as a target answers `GET /wfs/input/1/positionX?VALUE`. */
    constexpr const char* reply
        = R"({"FULL_PATH":"/wfs/input/1/positionX","TYPE":"f","ACCESS":3,"VALUE":[0.5]})";

    const auto perAddressMs = [&] (int howMany)
    {
        const auto start = std::chrono::steady_clock::now();

        for (int round = 0; round < rounds; ++round)
            for (int n = 0; n < howMany; ++n)
            {
                const auto value = oscquery::OscQueryClient::valueFromReply (reply, "f");
                REQUIRE (value.has_value());
            }

        return std::chrono::duration<double, std::milli> (
                 std::chrono::steady_clock::now() - start).count() / rounds;
    };

    constexpr int written = 40;                     // M20's figure for a 500-cue show
    const auto fortyMs = perAddressMs (written);
    const auto everythingMs = perAddressMs (nodes);

    MESSAGE ("M21: the subtree shape - one GET of " << text.size() << " bytes, "
              << nodes << " nodes - costs " << juce::String (subtreeMs, 3)
              << " ms to digest, in a DEBUG build");

    MESSAGE ("M21: the per-address shape costs " << juce::String (fortyMs, 3) << " ms for the "
              << written << " addresses a 500-cue show writes ("
              << (static_cast<std::size_t> (written) * std::strlen (reply))
              << " bytes over " << written
              << " round trips), and " << juce::String (everythingMs, 3)
              << " ms if it asked about every one of the " << nodes << " nodes");

    MESSAGE ("M21: at one sweep a second per mount, the per-address shape is "
              << written << " records a second and "
              << juce::String (100.0 * fortyMs / 20.0, 2) << "% of one tick to digest; "
              "the subtree shape would be one round trip and "
              << juce::String (100.0 * subtreeMs / 20.0, 1) << "% of a tick, every second");

    /*  THE SHAPE DECISION, as an assertion: for the addresses a show actually
        writes, asking about each is cheaper than digesting the whole capture,
        by an order of magnitude or it is not worth having two probes. */
    CHECK (fortyMs * 10.0 < subtreeMs);

    /*  And the sweep really is over what the show writes: a show that wrote
        every node would be better served by the subtree, which is the number
        that says when to revisit this. */
    INFO ("break-even at about " << static_cast<int> (subtreeMs / (fortyMs / written))
           << " written addresses");
    CHECK (nodes > written * 10);
}

//==============================================================================
/*  A NODE OF SEVERAL ARGUMENTS (namespace draft §45). ADM-OSC's
    `/adm/obj/<n>/xyz` takes three numbers, each with its own bounds, and the
    table takes exactly three - each coerced to its own tag - and keeps each
    argument's RANGE rather than the first alone. */
namespace
{
    constexpr const char* admJson = R"JSON({
      "FULL_PATH": "/adm",
      "CONTENTS": {
        "xyz": { "FULL_PATH": "/adm/xyz", "TYPE": "fff", "ACCESS": 3,
                 "RANGE": [ { "MIN": -1, "MAX": 1 }, { "MIN": 0, "MAX": 20 }, { "MIN": -5, "MAX": 5 } ] },
        "gain": { "FULL_PATH": "/adm/gain", "TYPE": "f", "ACCESS": 3, "RANGE": [ { "MIN": 0, "MAX": 2 } ] }
      }
    })JSON";

    MountDeclaration admMount()
    {
        MountDeclaration mount;
        mount.id = "ADM00001";
        mount.prefix = "/adm";
        mount.namespaceFile = "namespaces/adm.json";
        mount.port = 4001;
        return mount;
    }
}

TEST_CASE ("mount: a node of several arguments takes as many values as it has tags, each coerced to its own")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    MountTable mounts;
    REQUIRE (mounts.load (admMount(), admJson).ok);

    const auto* node = mounts.nodeAt ("/adm/xyz");
    REQUIRE (node != nullptr);
    CHECK (node->typeTags == "fff");

    //  Every argument's bounds, the first where every reader of one value reads it.
    REQUIRE (node->laterRanges.size() == 2u);
    CHECK (node->rangeOf (0).minimum == doctest::Approx (-1.0));
    CHECK (node->rangeOf (1).maximum == doctest::Approx (20.0));
    CHECK (node->rangeOf (2).minimum == doctest::Approx (-5.0));
    CHECK_FALSE (node->rangeOf (3).hasMinimum);

    SUBCASE ("three values land as three floats, whatever numbers they were")
    {
        const auto written = mounts.write ("/adm/xyz", osc::Values { osc::Value::int32 (1),
                                                                     osc::Value::float32 (2.5f),
                                                                     osc::Value::float64 (-3.0) });
        REQUIRE (written.ok);
        CHECK (written.values == osc::Values { osc::Value::float32 (1.0f), osc::Value::float32 (2.5f),
                                               osc::Value::float32 (-3.0f) });
        REQUIRE (mounts.valueOf ("/adm/xyz") != nullptr);
        CHECK (*mounts.valueOf ("/adm/xyz") == written.values);
    }

    SUBCASE ("a message missing a value is another message, and is refused")
    {
        CHECK (mounts.write ("/adm/xyz", osc::Value::float32 (0.5f)).reason == reason::typeMismatch);
        CHECK (mounts.write ("/adm/xyz", osc::Values {}).reason == reason::typeMismatch);
        CHECK (mounts.write ("/adm/xyz", osc::Values (4, osc::Value::float32 (0.0f))).reason
                 == reason::typeMismatch);
        CHECK (mounts.write ("/adm/xyz", osc::Values { osc::Value::float32 (0.0f), osc::Value::string ("x"),
                                                       osc::Value::float32 (0.0f) }).reason
                 == reason::typeMismatch);
        CHECK (mounts.valueOf ("/adm/xyz") == nullptr);
    }

    SUBCASE ("and it goes back out with each argument's RANGE")
    {
        const TreeSnapshot snapshot { 0, std::make_shared<const std::vector<Node>>(),
                                      std::make_shared<const std::vector<Node>> (mounts.allNodes()), {} };

        const auto text = OscQueryJson::describe (snapshot, "/adm/xyz");
        INFO (text);
        CHECK (text.find ("\"RANGE\": [{\"MIN\": -1, \"MAX\": 1}, {\"MIN\": 0, \"MAX\": 20}, {\"MIN\": -5, \"MAX\": 5}]")
                 != std::string::npos);

        //  A node of one is published exactly as before.
        CHECK (OscQueryJson::describe (snapshot, "/adm/gain").find ("\"RANGE\": [{\"MIN\": 0, \"MAX\": 2}]")
                 != std::string::npos);
    }
}

TEST_CASE ("mount: an opaque device takes the list as it was spelled, nothing at all included")
{
    MountTable mounts;

    MountDeclaration desk;
    desk.id = "DESK0001";
    desk.prefix = "/lx";
    desk.port = 10023;
    REQUIRE (mounts.declare (desk).ok);

    const auto bare = mounts.write ("/lx/go", osc::Values {});
    REQUIRE (bare.ok);
    CHECK (bare.values.empty());

    const auto two = mounts.write ("/lx/cmd", osc::Values { osc::Value::string ("Go To Cue"), osc::Value::int32 (11) });
    REQUIRE (two.ok);
    CHECK (two.values.size() == 2u);
}

TEST_CASE ("mount: mount.heard keeps what a device said as an observation, counts it, and writes nothing")
{
    Engine engine;
    engine.log().openInMemory ({});
    doc::ShowDocument document;
    MountTable mounts;
    juce::File nowhere;
    registerMountCommands (engine.commands(), document, mounts, nowhere);

    REQUIRE (mounts.load (admMount(), admJson).ok);

    engine.submit ("mount:ADM00001", "mount.heard",
                   { osc::Value::string ("ADM00001"), osc::Value::string ("/adm/xyz"),
                     osc::Value::float32 (0.5f), osc::Value::float32 (2.0f), osc::Value::float32 (-1.0f) });
    const auto result = engine.processTick (7);
    CHECK (result.applied == 1u);

    REQUIRE (mounts.observedOf ("/adm/xyz") != nullptr);
    CHECK (*mounts.observedOf ("/adm/xyz") == osc::Values { osc::Value::float32 (0.5f), osc::Value::float32 (2.0f),
                                                             osc::Value::float32 (-1.0f) });
    CHECK (mounts.observedAtTick ("/adm/xyz") == 7);
    CHECK (mounts.heardOf ("ADM00001") == 1u);

    //  Nothing written: the node holds no value of Go.dot's.
    CHECK (mounts.valueOf ("/adm/xyz") == nullptr);
}

TEST_CASE ("mount: a device on a serial port needs a port of the show that reads OSC, and no network port")
{
    /*  PC.11: transport serial - OSC over SLIP on the serial port the device
        names. Refused, in words, with no port named, a port the show lacks, or
        one reading lines; declared with no host or network port once the port
        reads OSC. */
    Rig rig;
    const auto made = rig.document.createMount ("/sensor", {}, {});
    REQUIRE (made.ok);
    const auto id = made.id;
    const auto base = "/godot/mount/" + id + "/";
    REQUIRE (rig.document.setAttribute (base + "transport", "serial").ok);

    refreshMountDeclarations (rig.document, rig.mounts, rig.folder);
    CHECK_FALSE (rig.mounts.isLoaded (id));
    CHECK (rig.mounts.problemOf (id) == "it is reached over a serial line and names no serial port");

    REQUIRE (rig.document.setAttribute (base + "serial", "SR000009").ok);
    refreshMountDeclarations (rig.document, rig.mounts, rig.folder);
    CHECK (rig.mounts.problemOf (id) == "it names serial port SR000009, which this show does not have");

    const auto port = rig.document.createSerial ("Arduino");
    REQUIRE (port.ok);
    REQUIRE (rig.document.setAttribute (base + "serial", port.id).ok);
    refreshMountDeclarations (rig.document, rig.mounts, rig.folder);
    CHECK (rig.mounts.problemOf (id).find ("reads lines") != std::string::npos);

    REQUIRE (rig.document.setAttribute ("/godot/serial/" + port.id + "/framing", "slip").ok);
    REQUIRE (loadMountFromBundle (rig.document, rig.mounts, rig.folder, id).ok);
    CHECK (rig.mounts.isLoaded (id));
    CHECK (rig.mounts.problemOf (id).empty());
    REQUIRE (rig.mounts.declarationOf (id) != nullptr);
    CHECK (rig.mounts.declarationOf (id)->serial == port.id);

    //  BACK ON THE NETWORK, it needs its port again: the transport changing reloads it.
    REQUIRE (rig.document.setAttribute (base + "transport", "udp").ok);
    refreshMountDeclarations (rig.document, rig.mounts, rig.folder);
    CHECK_FALSE (rig.mounts.problemOf (id).empty());
}

TEST_CASE ("mount sender: a device on a serial port is handed its packet's bytes, not sent a datagram")
{
    /*  PC.11: the destination names the serial port; the sink takes the
        bytes and says whether the port took them. */
    tree::MountSender sender;
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> handed;
    bool takes = true;
    sender.setSerialSink ([&] (const std::string& port, const std::vector<std::uint8_t>& packet)
    {
        handed.emplace_back (port, packet);
        return takes;
    });

    tree::MountSender::Destination onSerial;
    onSerial.serial = "SR000001";
    const auto ticket = sender.queue ("ARDU0001", onSerial, "/led", osc::Value::int32 (1));
    sender.flush();

    REQUIRE (handed.size() == 1u);
    CHECK (handed[0].first == "SR000001");
    const auto decoded = osc::decode (handed[0].second.data(), handed[0].second.size());
    REQUIRE (decoded.ok);
    CHECK (decoded.packet.address == "/led");
    CHECK (sender.outcomeOf (ticket) == tree::MountSender::Outcome::sent);

    //  A port that cannot take it fails the message, as nowhere to send does.
    takes = false;
    const auto refused = sender.queue ("ARDU0001", onSerial, "/led", osc::Value::int32 (0));
    sender.flush();
    CHECK (sender.outcomeOf (refused) == tree::MountSender::Outcome::failed);
}

//==============================================================================
/*  DP.6: A DEVICE OVER A CONNECTION (namespace draft §57, AFJ): transport tcp,
    the stream cut by length or SLIP, sent down a link serve keeps open. */

TEST_CASE ("mount: a device over a connection needs a host, a port and a framing the link can cut by; the connection itself is not the load's")
{
    Rig rig;
    const auto made = rig.document.createMount ("/eos", {}, {});
    REQUIRE (made.ok);
    const auto id = made.id;
    const auto base = "/godot/mount/" + id + "/";
    REQUIRE (rig.document.setAttribute (base + "transport", "tcp").ok);

    //  A port, as a datagram's: nothing can be inferred.
    refreshMountDeclarations (rig.document, rig.mounts, rig.folder);
    CHECK_FALSE (rig.mounts.isLoaded (id));
    CHECK (rig.mounts.problemOf (id).find ("no usable port") != std::string::npos);

    REQUIRE (rig.document.setAttribute (base + "port", "3032").ok);
    refreshMountDeclarations (rig.document, rig.mounts, rig.folder);
    CHECK (rig.mounts.isLoaded (id));
    CHECK (rig.mounts.problemOf (id).empty());
    REQUIRE (rig.mounts.declarationOf (id) != nullptr);
    CHECK (rig.mounts.declarationOf (id)->transport == "tcp");
    CHECK (rig.mounts.declarationOf (id)->framing == "length");

    //  SLIP on a connection - OSC 1.1, an Eos on 3037 - is the other framing.
    REQUIRE (rig.document.setAttribute (base + "framing", "slip").ok);
    refreshMountDeclarations (rig.document, rig.mounts, rig.folder);
    CHECK (rig.mounts.isLoaded (id));
    REQUIRE (rig.mounts.declarationOf (id) != nullptr);
    CHECK (rig.mounts.declarationOf (id)->framing == "slip");

    /*  THE ROW REFUSES ANY OTHER WORD; a file that carries one anyway is
        refused when the show opens, in words that name both it could take.
        Written under the schema's guard, as a file edited by hand would be,
        and read as the open reads it: a refresh after an edit keeps a loaded
        device's nodes and takes the new rows as they are, which is the
        row's guard doing the refusing in the live case. */
    CHECK_FALSE (rig.document.setAttribute (base + "framing", "lines").ok);
    const auto writeRaw = [&rig, &id] (const char* name, const char* value)
    {
        for (auto mount : rig.document.root().getChildWithName ("Mounts"))
            if (mount[juce::Identifier ("id")].toString() == juce::String (id))
                mount.setProperty (juce::Identifier (name), juce::String (value), nullptr);
    };
    writeRaw ("framing", "lines");
    CHECK_FALSE (loadMountFromBundle (rig.document, rig.mounts, rig.folder, id).ok);
    CHECK (rig.mounts.problemOf (id).find ("length") != std::string::npos);
    CHECK (rig.mounts.problemOf (id).find ("slip") != std::string::npos);

    //  And a host it has to have.
    writeRaw ("framing", "length");
    writeRaw ("host", "");
    CHECK_FALSE (loadMountFromBundle (rig.document, rig.mounts, rig.folder, id).ok);
    CHECK (rig.mounts.problemOf (id).find ("names no host") != std::string::npos);

    //  A wire Go.dot does not render - every word the schema has is rendered since DP.9, so one it lacks,
    //  written as a hand-edited file would carry it - is refused the same way, naming it (AFJ).
    writeRaw ("host", "127.0.0.1");
    writeRaw ("wire", "ws");
    CHECK_FALSE (loadMountFromBundle (rig.document, rig.mounts, rig.folder, id).ok);
    CHECK (rig.mounts.problemOf (id).find ("wire \"ws\"") != std::string::npos);
    writeRaw ("wire", "osc");

    //  By datagram again, as it was: loaded, with its port, the framing kept and not read.
    REQUIRE (rig.document.setAttribute (base + "transport", "udp").ok);
    refreshMountDeclarations (rig.document, rig.mounts, rig.folder);
    CHECK (rig.mounts.isLoaded (id));
    CHECK (rig.mounts.problemOf (id).empty());
}

TEST_CASE ("mount sender: a device over a connection is handed its packet to the link, not sent a datagram, and its destination names the link")
{
    //  The destination a tcp declaration names is its own identifier: the links table's key.
    tree::MountDeclaration eos;
    eos.id = "EOS00001";
    eos.transport = "tcp";
    eos.host = "10.0.0.5";
    eos.port = 3032;
    const auto to = tree::MountSender::destinationFor (eos);
    CHECK (to.link == "EOS00001");
    CHECK (to.serial.empty());
    CHECK (to.host == "10.0.0.5");
    CHECK (to.port == 3032);
    eos.transport = "udp";
    CHECK (tree::MountSender::destinationFor (eos).link.empty());

    tree::MountSender sender;        // no socket at all: the link is the whole way out
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> handed;
    bool takes = true;
    sender.setLinkSink ([&] (const std::string& mountId, const std::vector<std::uint8_t>& packet)
    {
        handed.emplace_back (mountId, packet);
        return takes;
    });

    tree::MountSender::Destination onLink;
    onLink.link = "EOS00001";
    onLink.host = "10.0.0.5";
    onLink.port = 3032;
    const auto ticket = sender.queue ("EOS00001", onLink, "/eos/sub/1", osc::Value::float32 (0.5f));
    sender.flush();

    REQUIRE (handed.size() == 1u);
    CHECK (handed[0].first == "EOS00001");
    const auto decoded = osc::decode (handed[0].second.data(), handed[0].second.size());
    REQUIRE (decoded.ok);
    CHECK (decoded.packet.address == "/eos/sub/1");
    CHECK (sender.outcomeOf (ticket) == tree::MountSender::Outcome::sent);
    CHECK (sender.sentFor ("EOS00001") == 1u);

    //  A link that cannot take it - not open, its queue full - fails the message, as a port that cannot does.
    takes = false;
    const auto refused = sender.queue ("EOS00001", onLink, "/eos/sub/1", osc::Value::float32 (0.0f));
    sender.flush();
    CHECK (sender.outcomeOf (refused) == tree::MountSender::Outcome::failed);
}

//==============================================================================
/*  DP.7: THE RCP WIRE (namespace draft §57, AFJ, AFH): a device whose bytes are
    lines of Yamaha's protocol, rendered at the flush by the node's own spelling
    and never bundled; the load taking it on a connection and nowhere else. */

TEST_CASE ("mount: the rcp wire rides a connection and nothing else, and a wire not built is still refused")
{
    Rig rig;
    const auto made = rig.document.createMount ("/MIXER:Current", {}, {});
    REQUIRE (made.ok);
    const auto id = made.id;
    const auto base = "/godot/mount/" + id + "/";
    REQUIRE (rig.document.setAttribute (base + "port", "49280").ok);
    REQUIRE (rig.document.setAttribute (base + "wire", "rcp").ok);

    refreshMountDeclarations (rig.document, rig.mounts, rig.folder);
    CHECK_FALSE (rig.mounts.isLoaded (id));
    CHECK (rig.mounts.problemOf (id).find ("set the transport to tcp") != std::string::npos);

    REQUIRE (rig.document.setAttribute (base + "transport", "tcp").ok);
    refreshMountDeclarations (rig.document, rig.mounts, rig.folder);
    CHECK (rig.mounts.isLoaded (id));
    CHECK (rig.mounts.problemOf (id).empty());
    REQUIRE (rig.mounts.declarationOf (id) != nullptr);
    CHECK (rig.mounts.declarationOf (id)->wire == "rcp");
    CHECK (tree::MountSender::destinationFor (*rig.mounts.declarationOf (id)).wire == "rcp");

    //  The row refuses a word the schema lacks; a file carrying one anyway is refused by the load, naming it.
    CHECK_FALSE (rig.document.setAttribute (base + "wire", "ws").ok);
    for (auto mount : rig.document.root().getChildWithName ("Mounts"))
        if (mount[juce::Identifier ("id")].toString() == juce::String (id))
            mount.setProperty (juce::Identifier ("wire"), juce::String ("ws"), nullptr);
    CHECK_FALSE (loadMountFromBundle (rig.document, rig.mounts, rig.folder, id).ok);
    CHECK (rig.mounts.problemOf (id).find ("wire \"ws\"") != std::string::npos);
}

TEST_CASE ("mount sender: a device on the rcp wire is sent lines spelled as its nodes say, one each whatever the bundles row says")
{
    tree::MountTable mounts;
    tree::MountDeclaration yamaha;
    yamaha.id = "YAMA0001";
    yamaha.prefix = "/MIXER:Current /MIXER:Lib";
    yamaha.namespaceFile = "namespaces/yamaha.json";
    yamaha.transport = "tcp";
    yamaha.wire = "rcp";
    yamaha.host = "10.0.0.9";
    yamaha.port = 49280;
    yamaha.bundles = true;
    REQUIRE (mounts.load (yamaha, R"JSON({"FULL_PATH": "/", "CONTENTS": {"MIXER:Current": {"FULL_PATH": "/MIXER:Current", "CONTENTS": {"InCh": {"FULL_PATH": "/MIXER:Current/InCh", "CONTENTS": {"Fader": {"FULL_PATH": "/MIXER:Current/InCh/Fader", "CONTENTS": {"Level": {"FULL_PATH": "/MIXER:Current/InCh/Fader/Level", "CONTENTS": {"1": {"FULL_PATH": "/MIXER:Current/InCh/Fader/Level/1", "CONTENTS": {"1": {"FULL_PATH": "/MIXER:Current/InCh/Fader/Level/1/1", "TYPE": "i", "ACCESS": 3, "VALUE": [0], "GODOT": {"RCP": {"VERB": "set", "XY": 2}}}}}}}}}}}}}, "MIXER:Lib": {"FULL_PATH": "/MIXER:Lib", "CONTENTS": {"Scene": {"FULL_PATH": "/MIXER:Lib/Scene", "TYPE": "i", "ACCESS": 2, "GODOT": {"RCP": {"VERB": "ssrecall_ex", "XY": 0}}}}}}})JSON").ok);

    tree::MountSender sender;
    sender.setMounts (&mounts);
    std::vector<std::string> lines;
    sender.setLinkSink ([&lines] (const std::string&, const std::vector<std::uint8_t>& bytes)
    {
        lines.emplace_back (bytes.begin(), bytes.end());
        return true;
    });

    const auto to = tree::MountSender::destinationFor (yamaha);
    REQUIRE (to.wire == "rcp");
    REQUIRE (to.bundles);
    const auto level = sender.queue ("YAMA0001", to, "/MIXER:Current/InCh/Fader/Level/1/1", osc::Value::int32 (-32768));
    const auto scene = sender.queue ("YAMA0001", to, "/MIXER:Lib/Scene", osc::Value::int32 (12));
    sender.flush();

    REQUIRE (lines.size() == 2u);
    CHECK (lines[0] == "set MIXER:Current/InCh/Fader/Level 0 0 -32768");
    CHECK (lines[1] == "ssrecall_ex MIXER:Lib/Scene 12");
    CHECK (sender.outcomeOf (level) == tree::MountSender::Outcome::sent);
    CHECK (sender.outcomeOf (scene) == tree::MountSender::Outcome::sent);
    CHECK (sender.sentFor ("YAMA0001") == 2u);

    //  A node the table does not hold is spelled by its address alone.
    sender.queue ("YAMA0001", to, "/MIXER:Current/St/Fader/Level/2", osc::Value::float32 (999.6f));
    sender.flush();
    REQUIRE (lines.size() == 3u);
    CHECK (lines[2] == "set MIXER:Current/St/Fader/Level 1 0 1000");

    //  And the console's answer is kept, the latest, by device.
    CHECK (mounts.lastReplyOf ("YAMA0001").empty());
    mounts.noteReply ("YAMA0001", "OK", "OK set MIXER:Current/InCh/Fader/Level 0 0 -32768");
    mounts.noteReply ("YAMA0001", "ERROR", "ERROR ssrecall_ex InvalidArgument");
    CHECK (mounts.lastReplyOf ("YAMA0001") == "ERROR ssrecall_ex InvalidArgument");
    CHECK (mounts.lastReplyOf ("NOBODY01").empty());
}

TEST_CASE ("mount: what a console answered arrives as mount.replied, from the link's origin, and is the device's last reply")
{
    Rig rig;
    rig.engine.submit ("link:YAMA0001", "mount.replied",
                       { osc::Value::string ("YAMA0001"), osc::Value::string ("ERROR"),
                         osc::Value::string ("ERROR set InvalidArgument") });
    const auto result = rig.engine.processTick (9);
    CHECK (result.applied == 1u);
    CHECK (rig.mounts.lastReplyOf ("YAMA0001") == "ERROR set InvalidArgument");
    CHECK (rig.mounts.lastReplyOf ("OTHER001").empty());
}

TEST_CASE ("mount: the line wire rides a connection with its login row, and the sender spells each node's command line")
{
    //  DP.8: a grandMA2 over telnet.
    Rig rig;
    const auto made = rig.document.createMount ("/exec /cmd", {}, {});
    REQUIRE (made.ok);
    const auto id = made.id;
    const auto base = "/godot/mount/" + id + "/";
    REQUIRE (rig.document.setAttribute (base + "port", "30000").ok);
    REQUIRE (rig.document.setAttribute (base + "wire", "line").ok);
    REQUIRE (rig.document.setAttribute (base + "login", "login admin admin").ok);

    refreshMountDeclarations (rig.document, rig.mounts, rig.folder);
    CHECK_FALSE (rig.mounts.isLoaded (id));
    CHECK (rig.mounts.problemOf (id).find ("the line wire is lines of text on a connection") != std::string::npos);

    REQUIRE (rig.document.setAttribute (base + "transport", "tcp").ok);
    refreshMountDeclarations (rig.document, rig.mounts, rig.folder);
    CHECK (rig.mounts.isLoaded (id));
    REQUIRE (rig.mounts.declarationOf (id) != nullptr);
    CHECK (rig.mounts.declarationOf (id)->wire == "line");
    CHECK (rig.mounts.declarationOf (id)->login == "login admin admin");

    //  The sender, over a described console: the template, and the atoms where there is none.
    tree::MountTable mounts;
    tree::MountDeclaration ma;
    ma.id = "MA200001";
    ma.prefix = "/exec /cmd";
    ma.namespaceFile = "namespaces/ma.json";
    ma.transport = "tcp";
    ma.wire = "line";
    ma.host = "10.0.0.2";
    ma.port = 30000;
    REQUIRE (mounts.load (ma, R"JSON({"FULL_PATH": "/", "CONTENTS": {"exec": {"FULL_PATH": "/exec", "CONTENTS": {"1": {"FULL_PATH": "/exec/1", "CONTENTS": {"2": {"FULL_PATH": "/exec/1/2", "CONTENTS": {"go": {"FULL_PATH": "/exec/1/2/go", "ACCESS": 2, "GODOT": {"LINE": "Go+ Executor {x}.{y}"}}, "fader": {"FULL_PATH": "/exec/1/2/fader", "TYPE": "i", "ACCESS": 3, "VALUE": [0], "GODOT": {"LINE": "Fader {x}.{y} At {1}"}}}}}}}}, "cmd": {"FULL_PATH": "/cmd", "TYPE": "s", "ACCESS": 2}}})JSON").ok);

    tree::MountSender sender;
    sender.setMounts (&mounts);
    std::vector<std::string> lines;
    sender.setLinkSink ([&lines] (const std::string&, const std::vector<std::uint8_t>& bytes)
    {
        lines.emplace_back (bytes.begin(), bytes.end());
        return true;
    });

    const auto to = tree::MountSender::destinationFor (ma);
    REQUIRE (to.wire == "line");
    sender.queue ("MA200001", to, "/exec/1/2/go", osc::Values {});
    sender.queue ("MA200001", to, "/exec/1/2/fader", osc::Value::int32 (50));
    sender.queue ("MA200001", to, "/cmd", osc::Value::string ("Goto Cue 12"));
    sender.flush();

    REQUIRE (lines.size() == 3u);
    CHECK (lines[0] == "Go+ Executor 1.2\r");
    CHECK (lines[1] == "Fader 1.2 At 50\r");
    CHECK (lines[2] == "Goto Cue 12\r");
}

//==============================================================================
/*  DP.9: THE MIDI WIRE - a device on a declared MIDI port, over a connection
    or in a datagram, its nodes' shapes rendered at the flush. */

namespace
{
    struct RecordingMidi final : midi::MidiSink
    {
        std::vector<std::pair<std::string, std::vector<std::uint8_t>>> taken;   // the port, the bytes
        std::vector<std::string> runs;
        bool takes = true;

        std::string send (const std::string& port, const midi::Bytes& bytes) override
        {
            if (! takes)
                return "no-port";
            taken.emplace_back (port, bytes);
            return {};
        }

        std::string sendForRun (const std::string& runId, const std::string& port, const midi::Bytes& bytes) override
        {
            runs.push_back (runId);
            return send (port, bytes);
        }
    };
}

TEST_CASE ("mount: a device on a MIDI port needs one of the show's and the midi wire; the midi wire rides a port, a connection or a datagram, never a serial line")
{
    Rig rig;
    const auto made = rig.document.createMount ("/msc", {}, {});
    REQUIRE (made.ok);
    const auto id = made.id;
    const auto base = "/godot/mount/" + id + "/";
    REQUIRE (rig.document.setAttribute (base + "transport", "midi").ok);

    refreshMountDeclarations (rig.document, rig.mounts, rig.folder);
    CHECK_FALSE (rig.mounts.isLoaded (id));
    CHECK (rig.mounts.problemOf (id).find ("the midi wire only") != std::string::npos);

    REQUIRE (rig.document.setAttribute (base + "wire", "midi").ok);
    refreshMountDeclarations (rig.document, rig.mounts, rig.folder);
    CHECK (rig.mounts.problemOf (id).find ("names none") != std::string::npos);

    REQUIRE (rig.document.setAttribute (base + "midiPort", "NOPORT01").ok);
    refreshMountDeclarations (rig.document, rig.mounts, rig.folder);
    CHECK (rig.mounts.problemOf (id).find ("which this show does not have") != std::string::npos);

    const auto port = rig.document.createPort ("Desk");
    REQUIRE (port.ok);
    REQUIRE (rig.document.setAttribute (base + "midiPort", port.id).ok);
    REQUIRE (rig.document.setAttribute (base + "midiChannel", "12").ok);
    REQUIRE (rig.document.setAttribute (base + "mscDevice", "5").ok);
    refreshMountDeclarations (rig.document, rig.mounts, rig.folder);
    CHECK (rig.mounts.isLoaded (id));
    CHECK (rig.mounts.problemOf (id).empty());
    REQUIRE (rig.mounts.declarationOf (id) != nullptr);
    CHECK (rig.mounts.declarationOf (id)->midiPort == port.id);
    CHECK (rig.mounts.declarationOf (id)->midiChannel == 12);
    CHECK (rig.mounts.declarationOf (id)->mscDevice == 5);
    CHECK (rig.mounts.declarationOf (id)->mscFormat == 127);

    const auto to = tree::MountSender::destinationFor (*rig.mounts.declarationOf (id));
    CHECK (to.wire == "midi");
    CHECK (to.midiPort == port.id);
    CHECK (to.midiChannel == 12);
    CHECK (to.mscDevice == 5);
    CHECK (to.link.empty());

    //  In a datagram - MSC to an MA desk - it needs a port number, as any datagram does.
    REQUIRE (rig.document.setAttribute (base + "transport", "udp").ok);
    refreshMountDeclarations (rig.document, rig.mounts, rig.folder);
    CHECK (rig.mounts.problemOf (id).find ("no usable port") != std::string::npos);
    REQUIRE (rig.document.setAttribute (base + "port", "6004").ok);
    refreshMountDeclarations (rig.document, rig.mounts, rig.folder);
    CHECK (rig.mounts.isLoaded (id));
    CHECK (tree::MountSender::destinationFor (*rig.mounts.declarationOf (id)).midiPort.empty());

    //  Over a connection, raw bytes down the link; never on a serial line.
    REQUIRE (rig.document.setAttribute (base + "transport", "tcp").ok);
    refreshMountDeclarations (rig.document, rig.mounts, rig.folder);
    CHECK (rig.mounts.isLoaded (id));
    CHECK (tree::MountSender::destinationFor (*rig.mounts.declarationOf (id)).link == id);

    REQUIRE (rig.document.setAttribute (base + "transport", "serial").ok);
    refreshMountDeclarations (rig.document, rig.mounts, rig.folder);
    CHECK (rig.mounts.problemOf (id).find ("does not ride a serial port") != std::string::npos);
}

TEST_CASE ("mount sender: a device on the midi wire is sent its nodes' shapes as messages, to the port in the run's name, down the link or in a datagram")
{
    tree::MountTable mounts;
    tree::MountDeclaration desk;
    desk.id = "DESK0001";
    desk.prefix = "/scene /input";
    desk.namespaceFile = "namespaces/desk.json";
    desk.transport = "midi";
    desk.wire = "midi";
    desk.midiPort = "P0RT0001";
    desk.midiChannel = 12;
    REQUIRE (mounts.load (desk, R"JSON({"FULL_PATH": "/", "CONTENTS": {"scene": {"FULL_PATH": "/scene", "CONTENTS": {"recall": {"FULL_PATH": "/scene/recall", "TYPE": "i", "ACCESS": 2, "GODOT": {"MIDI": {"KIND": "pc", "BANKED": true, "START": 1}}}}}, "input": {"FULL_PATH": "/input", "CONTENTS": {"1": {"FULL_PATH": "/input/1", "CONTENTS": {"mute": {"FULL_PATH": "/input/1/mute", "TYPE": "T", "ACCESS": 3, "VALUE": [false], "GODOT": {"MIDI": {"KIND": "note", "NOTE": 0, "ON": 127, "OFF": 63, "RELEASE": true}}}, "plain": {"FULL_PATH": "/input/1/plain", "TYPE": "i", "ACCESS": 3, "VALUE": [0]}}}}}}})JSON").ok);

    const auto* mute = mounts.nodeAt ("/input/1/mute");
    REQUIRE (mute != nullptr);
    CHECK (mute->midi.kind == "note");
    CHECK (mute->midi.on == 127);
    CHECK (mute->midi.off == 63);
    CHECK (mute->midi.hasOnOff);
    CHECK (mute->midi.release);
    const auto* recall = mounts.nodeAt ("/scene/recall");
    REQUIRE (recall != nullptr);
    CHECK (recall->midi.kind == "pc");
    CHECK (recall->midi.banked);
    CHECK (recall->midi.start == 1);

    tree::MountSender sender;
    sender.setMounts (&mounts);
    RecordingMidi sink;
    sender.setMidiSink (&sink);

    const auto to = tree::MountSender::destinationFor (desk);
    REQUIRE (to.wire == "midi");
    REQUIRE (to.midiPort == "P0RT0001");
    const auto scene = sender.queue ("DESK0001", to, "/scene/recall", osc::Value::int32 (156), "RUN00001");
    const auto muted = sender.queue ("DESK0001", to, "/input/1/mute", osc::Value::boolean (true), "RUN00001");
    sender.flush();

    //  Bank Select, Program Change, Note On, its release: four messages to the port, in order, in the run's name.
    REQUIRE (sink.taken.size() == 4u);
    CHECK (sink.taken[0].first == "P0RT0001");
    CHECK (sink.taken[0].second == std::vector<std::uint8_t> { 0xBB, 0x00, 0x01 });
    CHECK (sink.taken[1].second == std::vector<std::uint8_t> { 0xCB, 0x1B });
    CHECK (sink.taken[2].second == std::vector<std::uint8_t> { 0x9B, 0x00, 0x7F });
    CHECK (sink.taken[3].second == std::vector<std::uint8_t> { 0x9B, 0x00, 0x00 });
    CHECK (sink.runs == std::vector<std::string> (4, "RUN00001"));
    CHECK (sender.outcomeOf (scene) == tree::MountSender::Outcome::sent);
    CHECK (sender.outcomeOf (muted) == tree::MountSender::Outcome::sent);
    CHECK (sender.sentFor ("DESK0001") == 2u);

    //  A node with no shape sends nothing and fails; a port that refuses fails.
    const auto plain = sender.queue ("DESK0001", to, "/input/1/plain", osc::Value::int32 (1));
    sender.flush();
    CHECK (sender.outcomeOf (plain) == tree::MountSender::Outcome::failed);
    CHECK (sink.taken.size() == 4u);
    sink.takes = false;
    const auto refused = sender.queue ("DESK0001", to, "/scene/recall", osc::Value::int32 (2));
    sender.flush();
    CHECK (sender.outcomeOf (refused) == tree::MountSender::Outcome::failed);

    //  Over a connection the same bytes go down the link, each message its own packet; bundles never.
    std::vector<std::vector<std::uint8_t>> linked;
    sender.setLinkSink ([&linked] (const std::string&, const std::vector<std::uint8_t>& bytes)
    {
        linked.push_back (bytes);
        return true;
    });
    desk.transport = "tcp";
    desk.host = "10.0.0.3";
    desk.port = 51325;
    desk.bundles = true;
    auto overLink = tree::MountSender::destinationFor (desk);
    CHECK (overLink.midiPort.empty());
    CHECK (overLink.link == "DESK0001");
    sender.queue ("DESK0001", overLink, "/input/1/mute", osc::Value::boolean (false));
    sender.flush();
    REQUIRE (linked.size() == 2u);
    CHECK (linked[0] == std::vector<std::uint8_t> { 0x9B, 0x00, 0x3F });
    CHECK (linked[1] == std::vector<std::uint8_t> { 0x9B, 0x00, 0x00 });
}

//==============================================================================
/*  DP.10: WHAT COMES BACK (namespace draft §57, AFL) - the words of
    mount/readback, each a wire's; a question on the wire; the file's own lines
    for asking and subscribing. */

TEST_CASE ("mount: the readback words - three ask, three tell, each on its own wire, and the file says how")
{
    tree::MountDeclaration device;
    device.namespaceFile = "namespaces/x.json";

    device.readback = "oscquery";
    CHECK_FALSE (device.canBeAsked());
    device.queryPort = 5005;
    CHECK (device.canBeAsked());
    CHECK_FALSE (device.isTelling());

    for (const auto* word : { "get", "notify" })
    {
        device.readback = word;
        device.queryPort = 0;
        INFO (word);
        CHECK (device.canBeAsked());
    }

    for (const auto* word : { "xremote", "subscribe", "midi" })
    {
        device.readback = word;
        INFO (word);
        CHECK_FALSE (device.canBeAsked());
        CHECK (device.isTelling());
    }

    device.readback = "none";
    CHECK_FALSE (device.canBeAsked());
    CHECK_FALSE (device.isTelling());

    //  An opaque device is never asked, whatever it says.
    device.readback = "get";
    device.namespaceFile.clear();
    CHECK_FALSE (device.canBeAsked());

    //  The load refuses a word on the wrong wire, in words.
    Rig rig;
    const auto made = rig.document.createMount ("/x32", {}, {});
    REQUIRE (made.ok);
    const auto id = made.id;
    const auto base = "/godot/mount/" + id + "/";
    REQUIRE (rig.document.setAttribute (base + "port", "10023").ok);
    REQUIRE (rig.document.setAttribute (base + "readback", "xremote").ok);
    refreshMountDeclarations (rig.document, rig.mounts, rig.folder);
    CHECK (rig.mounts.isLoaded (id));

    REQUIRE (rig.document.setAttribute (base + "readback", "notify").ok);
    CHECK_FALSE (loadMountFromBundle (rig.document, rig.mounts, rig.folder, id).ok);
    CHECK (rig.mounts.problemOf (id).find ("set the wire to rcp") != std::string::npos);

    REQUIRE (rig.document.setAttribute (base + "readback", "midi").ok);
    CHECK_FALSE (loadMountFromBundle (rig.document, rig.mounts, rig.folder, id).ok);
    CHECK (rig.mounts.problemOf (id).find ("set the wire to midi") != std::string::npos);

    REQUIRE (rig.document.setAttribute (base + "readback", "get").ok);
    REQUIRE (rig.document.setAttribute (base + "wire", "midi").ok);
    REQUIRE (rig.document.setAttribute (base + "transport", "tcp").ok);
    CHECK_FALSE (loadMountFromBundle (rig.document, rig.mounts, rig.folder, id).ok);
    CHECK (rig.mounts.problemOf (id).find ("set the wire to osc") != std::string::npos);

    //  The file's lines for asking and subscribing, read at the root.
    tree::MountTable mounts;
    tree::MountDeclaration holo;
    holo.id = "HOLO0001";
    holo.prefix = "/track";
    holo.namespaceFile = "namespaces/holo.json";
    holo.port = 4003;
    holo.readback = "get";
    REQUIRE (mounts.load (holo, R"JSON({"FULL_PATH": "/track", "GODOT": {"GET": "/get {address}", "SUBSCRIBE": "/sub 1"}, "CONTENTS": {"1": {"FULL_PATH": "/track/1", "CONTENTS": {"gain": {"FULL_PATH": "/track/1/gain", "TYPE": "f", "ACCESS": 3, "VALUE": [0.0]}}}}})JSON").ok);
    CHECK (mounts.getTemplateOf ("HOLO0001") == "/get {address}");
    CHECK (mounts.subscribeTemplateOf ("HOLO0001") == "/sub 1");
    CHECK (mounts.getTemplateOf ("NOBODY01").empty());
    REQUIRE (mounts.nodesOf ("HOLO0001").size() >= 1u);
    CHECK (mounts.nodesOf ("HOLO0001").back()->address == "/track/1/gain");
    CHECK (mounts.nodesOf ("NOBODY01").empty());
}

TEST_CASE ("mount sender: a question on the wire - the bare address, the file's GET line, a Yamaha get - apart from a write of the same address")
{
    tree::MountTable mounts;
    tree::MountDeclaration holo;
    holo.id = "HOLO0001";
    holo.prefix = "/track";
    holo.namespaceFile = "namespaces/holo.json";
    holo.host = "10.0.0.7";
    holo.port = 4003;
    holo.readback = "get";
    REQUIRE (mounts.load (holo, R"JSON({"FULL_PATH": "/track", "GODOT": {"GET": "/get {address}"}, "CONTENTS": {"1": {"FULL_PATH": "/track/1", "CONTENTS": {"gain": {"FULL_PATH": "/track/1/gain", "TYPE": "f", "ACCESS": 3, "VALUE": [0.0]}}}}})JSON").ok);

    tree::MountDeclaration ds;
    ds.id = "DS100001";
    ds.prefix = "/dbaudio1";
    ds.namespaceFile = "namespaces/ds.json";
    ds.host = "10.0.0.8";
    ds.port = 50010;
    ds.readback = "get";
    REQUIRE (mounts.load (ds, R"JSON({"FULL_PATH": "/dbaudio1", "CONTENTS": {"gain": {"FULL_PATH": "/dbaudio1/gain", "TYPE": "f", "ACCESS": 3, "VALUE": [0.0]}}})JSON").ok);

    tree::MountSender sender;
    sender.setMounts (&mounts);
    std::vector<std::pair<std::string, osc::Values>> linked;   // the address, the atoms, through the link sink for want of a socket
    sender.setLinkSink ([&linked] (const std::string&, const std::vector<std::uint8_t>& bytes)
    {
        const auto decoded = osc::decode (bytes.data(), bytes.size());
        linked.emplace_back (decoded.ok ? decoded.packet.address : "?", decoded.ok ? decoded.packet.args : osc::Values {});
        return true;
    });

    //  With the file's line: /get and the node as its one atom.
    auto to = tree::MountSender::destinationFor (holo);
    to.link = "HOLO0001";
    const auto write = sender.queue ("HOLO0001", to, "/track/1/gain", osc::Value::float32 (-6.0f));
    const auto ask = sender.queueQuery ("HOLO0001", to, "/track/1/gain");
    sender.flush();

    REQUIRE (linked.size() == 2u);
    CHECK (linked[0].first == "/track/1/gain");
    CHECK (linked[1].first == "/get");
    REQUIRE (linked[1].second.size() == 1u);
    CHECK (linked[1].second[0].getString() == "/track/1/gain");
    CHECK (sender.outcomeOf (write) == tree::MountSender::Outcome::sent);
    CHECK (sender.outcomeOf (ask) == tree::MountSender::Outcome::sent);

    //  Without one: the bare address, no atoms.
    auto bare = tree::MountSender::destinationFor (ds);
    bare.link = "DS100001";
    sender.queueQuery ("DS100001", bare, "/dbaudio1/gain");
    sender.flush();
    REQUIRE (linked.size() == 3u);
    CHECK (linked[2].first == "/dbaudio1/gain");
    CHECK (linked[2].second.empty());

    //  A Yamaha console is asked with a get line, the node's indexes and no value.
    tree::MountDeclaration yamaha;
    yamaha.id = "YAMA0001";
    yamaha.prefix = "/MIXER:Current";
    yamaha.namespaceFile = "namespaces/y.json";
    yamaha.transport = "tcp";
    yamaha.wire = "rcp";
    yamaha.host = "10.0.0.9";
    yamaha.port = 49280;
    yamaha.readback = "notify";
    REQUIRE (mounts.load (yamaha, R"JSON({"FULL_PATH": "/MIXER:Current", "CONTENTS": {"InCh": {"FULL_PATH": "/MIXER:Current/InCh", "CONTENTS": {"Fader": {"FULL_PATH": "/MIXER:Current/InCh/Fader", "CONTENTS": {"Level": {"FULL_PATH": "/MIXER:Current/InCh/Fader/Level", "CONTENTS": {"1": {"FULL_PATH": "/MIXER:Current/InCh/Fader/Level/1", "TYPE": "i", "ACCESS": 3, "VALUE": [0], "GODOT": {"RCP": {"VERB": "set", "XY": 1}}}}}}}}}}})JSON").ok);
    std::vector<std::string> lines;
    sender.setLinkSink ([&lines] (const std::string&, const std::vector<std::uint8_t>& bytes)
    {
        lines.emplace_back (bytes.begin(), bytes.end());
        return true;
    });
    sender.queueQuery ("YAMA0001", tree::MountSender::destinationFor (yamaha), "/MIXER:Current/InCh/Fader/Level/1");
    sender.flush();
    REQUIRE (lines.size() == 1u);
    CHECK (lines[0] == "get MIXER:Current/InCh/Fader/Level 0 0");

    //  Two questions of one address in one tick are one question.
    sender.queueQuery ("YAMA0001", tree::MountSender::destinationFor (yamaha), "/MIXER:Current/InCh/Fader/Level/1");
    sender.queueQuery ("YAMA0001", tree::MountSender::destinationFor (yamaha), "/MIXER:Current/InCh/Fader/Level/1");
    sender.flush();
    CHECK (lines.size() == 2u);
}

TEST_CASE ("mount: a description rooted at / with one entry mounts that entry at its own name, once, and the prefix row has to be it")
{
    /*  Found by the wires driver (DP.11): the Holophonix preset, rooted at "/"
        with "track" its one entry, was read as one tree under /track and
        published /track/track/1/gain. A file rooted at "/" mounts its entries
        at their own names whatever their number (AFK). */
    tree::MountTable mounts;
    tree::MountDeclaration holo;
    holo.id = "HOLO0001";
    holo.prefix = "/track";
    holo.namespaceFile = "namespaces/holo.json";
    holo.port = 4003;
    const auto text = R"JSON({"FULL_PATH": "/", "CONTENTS": {"track": {"FULL_PATH": "/track", "CONTENTS": {"1": {"FULL_PATH": "/track/1", "CONTENTS": {"gain": {"FULL_PATH": "/track/1/gain", "TYPE": "f", "ACCESS": 3, "VALUE": [0.0]}}}}}}})JSON";
    REQUIRE (mounts.load (holo, text).ok);
    CHECK (mounts.nodeAt ("/track/1/gain") != nullptr);
    CHECK (mounts.nodeAt ("/track/track/1/gain") == nullptr);
    CHECK (mounts.mountOf ("/track/1/gain") == "HOLO0001");

    //  Under another name the file is a capture of a whole namespace, and nests as it always did.
    holo.prefix = "/holo";
    REQUIRE (mounts.load (holo, text).ok);
    CHECK (mounts.nodeAt ("/holo/track/1/gain") != nullptr);
    CHECK (mounts.nodeAt ("/track/1/gain") == nullptr);
}
