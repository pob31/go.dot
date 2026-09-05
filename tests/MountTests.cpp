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
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/tree/TreeCommands.h>

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
        ParameterTree parameters { document, engine.commands(), mounts };
    };
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
        CHECK_FALSE (node->value.has_value());
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
    CHECK_FALSE (blackout->value.has_value());          // an event has no value at a given time

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
    REQUIRE (published->value.has_value());
    CHECK (*published->value == osc::Value::float32 (12.5f));
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

    for (const auto& prefix : { "", "/", "wfs", "/wfs/", "/a//b" })
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
        REQUIRE (node->value.has_value());
        return *node->value;
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
