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
#include <wfg/engine/tree/ParameterTree.h>

#include <chrono>
#include <wfg/engine/tree/TreeCommands.h>
#include <wfg/engine/oscquery/OscQueryClient.h>

#include <juce_core/juce_core.h>

#include <cmath>
#include <cstring>
#include <random>
#include <string>

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

TEST_CASE ("mount: a device that describes itself answers at one root")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    MountDeclaration several;
    several.id = "G1JS4VWE";
    several.prefix = "/wfs /other";
    several.namespaceFile = "namespaces/wfs-diy.json";
    several.port = 8000;

    /*  The description itself is beside the point - it is refused before a
        byte of it is read - so the simplest valid one will do. */
    const auto reloaded = readNamespace (several, R"({"FULL_PATH": "/", "CONTENTS": {}})");

    /*  Refused, and the sentence says why rather than leaving somebody to
        guess: a namespace file is ONE tree and mounts in ONE place, so a second
        root would route messages to a box whose nodes are published somewhere
        else - and every write under it would come back as an address the device
        does not have. Several roots are for a device nobody described. */
    CHECK_FALSE (reloaded.ok);
    REQUIRE_FALSE (reloaded.problems.empty());
    INFO ("said: " << reloaded.problems.front());
    CHECK (reloaded.problems.front().find ("one address") != std::string::npos);
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
        CHECK (written.value.isFloat32());

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
    CHECK (*stored == osc::Value::float32 (12.5f));

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
    CHECK (*rig.mounts.valueOf ("/wfs/input/1/positionX") == osc::Value::float32 (3.0f));
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

    /*  AND IT DOES REBUILD WHEN THE MOUNT MOVES, which is the other half of a
        cache being right. Written through the table, so the invalidation is the
        production path and not a test reaching past it. */
    const auto address = std::string ("/wfs/input/1/positionX");

    if (withMount.mounts.nodeAt (address) != nullptr)
    {
        REQUIRE (withMount.mounts.write (address, osc::Value::float32 (0.25f)).ok);

        withMount.publish (1);
        CHECK (withMount.parameters.mountRebuilds() == 2u);

        /*  And the new value is in the published tree, which is what the cache
            existed to keep true. */
        const auto after = withMount.publish (2);
        const auto* node = after->find (address);

        REQUIRE (node != nullptr);
        REQUIRE (node->soleValue().has_value());
        CHECK (node->soleValue()->getFloat32() == doctest::Approx (0.25f));
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
    CHECK (*rig.mounts.observedOf (address) == osc::Value::float32 (0.25f));

    /*  Not a read-back: nobody asked on a cue's behalf. */
    CHECK (rig.mounts.readbackOf (address) == nullptr);

    /*  And a read-back is not an observation either. */
    rig.mounts.noteReadback (address, osc::Value::float32 (0.5f));
    CHECK (*rig.mounts.observedOf (address) == osc::Value::float32 (0.25f));

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
