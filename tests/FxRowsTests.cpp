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
    THE FX DOOR (Phase 9a, PR 9a.8): the `p<n>` nodes under a cue's insert,
    written with node.set from any origin, rewriting the cue's one sparse row
    through the ordinary door - so the lock, the transaction and the
    coalescing are the document's. What it recognises, what it refuses, what
    it writes, and that a turn on one parameter is one undo step while two
    parameters are two.

    Driven through the engine rather than the door alone, because the
    coalescing is keyed on the applied record - the address the hand sent,
    not the row the door wrote - and that is the thing to prove.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/client/model/Text.h>
#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/FxRows.h>
#include <wfg/engine/cue/LiveEdits.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/plugin/Catalogue.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>

#include <juce_core/juce_core.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace wfg;

namespace
{
    struct Folder
    {
        Folder()
            : root (juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("wfg-fxrows-" + juce::Uuid().toDashedString()))
        {
            root.createDirectory();
        }

        ~Folder() { root.deleteRecursively(); }

        std::string path() const { return root.getFullPathName().toStdString(); }

        juce::File root;
    };

    struct Rig
    {
        Rig()
        {
            engine.log().openInMemory ({});
            doc::registerDocumentCommands (engine.commands(), document, {},
                                           cue::fxWriteFor (document, &store, &live));
            cue::registerLiveCommands (engine.commands(), document, live);
            engine.setBeforeApply ([this] (const Command& appliedCommand, const Event& submitted,
                                           const std::vector<osc::Value>& coerced,
                                           std::int64_t tickIndex)
                                   {
                                       if (cue::isLiveEdit (appliedCommand.name, coerced, document, live))
                                           return;

                                       document.beginTransaction (appliedCommand.name, tickIndex,
                                                                  submitted.origin, coerced);
                                   });
            parameters.setCatalogues (&store);
            parameters.setLiveEdits (&live);

            REQUIRE (apply ("list.create", { osc::Value::string ("Main") }).applied == 1);
            listId = lastApplied().back();

            REQUIRE (apply ("cue.create", { osc::Value::string (listId), osc::Value::int32 (0),
                                            osc::Value::string ("media"), osc::Value::string ("Tone") }).applied == 1);
            cueId = lastApplied().back();

            REQUIRE (apply ("plugin.create", { osc::Value::string ("Test gain"),
                                               osc::Value::string (plugin::Catalogue::testGainIdentifier()),
                                               osc::Value::string ("VST3"), osc::Value::string ("") }).applied == 1);
            pluginId = lastApplied().back();

            REQUIRE (apply ("fx.create", { osc::Value::string (cueId), osc::Value::string (pluginId) }).applied == 1);
            fxId = lastApplied().back();
        }

        Engine::TickResult apply (const std::string& command, std::vector<osc::Value> args = {},
                                  const char* origin = "cli")
        {
            REQUIRE (engine.submit (std::string (origin), command, std::move (args)));
            return engine.processTick (tick++);
        }

        /** A write through the door, at this tick. */
        Engine::TickResult set (const std::string& address, const std::string& text, const char* origin = "cli")
        {
            return apply ("node.set", { osc::Value::string (address), osc::Value::string (text) }, origin);
        }

        std::vector<std::string> lastApplied()
        {
            const auto parsed = LogFile::parse (engine.log().contents());
            REQUIRE (! parsed.records.empty());
            const auto& last = parsed.records.back();
            REQUIRE (last.kind == LogRecord::Kind::applied);
            std::vector<std::string> out;

            for (const auto& arg : last.args)
                out.push_back (arg.getString());

            return out;
        }

        std::string values() const
        {
            return document.findById (fxId).getProperty ("values").toString().toStdString();
        }

        std::string at (const std::string& address)
        {
            parameters.markStale();
            snapshot = parameters.publish (tick, state);
            return client::model::text (*snapshot, address);
        }

        std::string fxAddress (const std::string& leaf) const { return "/godot/fx/" + fxId + "/" + leaf; }

        Folder folder;
        plugin::CatalogueStore store { folder.path() };
        Engine engine;
        doc::ShowDocument document;
        cue::LiveEdits live;
        tree::MountTable mounts;
        cue::RunTable runs;
        tree::ParameterTree parameters { document, engine.commands(), mounts, runs };
        tree::EngineState state;
        std::shared_ptr<const tree::TreeSnapshot> snapshot;
        std::int64_t tick = 1;
        std::string listId, cueId, pluginId, fxId;
    };
}

//==============================================================================
TEST_CASE ("fx door: the address it answers to, and the ones it leaves to the document")
{
    CHECK (cue::isFxParameterAddress ("/godot/fx/FX000001/p0"));
    CHECK (cue::isFxParameterAddress ("/godot/fx/FX000001/p17"));
    CHECK_FALSE (cue::isFxParameterAddress ("/godot/fx/FX000001/p01"));
    CHECK_FALSE (cue::isFxParameterAddress ("/godot/fx/FX000001/p"));
    CHECK_FALSE (cue::isFxParameterAddress ("/godot/fx/FX000001/px"));
    CHECK_FALSE (cue::isFxParameterAddress ("/godot/fx/FX000001/values"));
    CHECK_FALSE (cue::isFxParameterAddress ("/godot/fx/FX000001/t0"));
    CHECK_FALSE (cue::isFxParameterAddress ("/godot/fx/FX000001/p0/x"));
    CHECK_FALSE (cue::isFxParameterAddress ("/godot/cue/FX000001/p0"));
    CHECK_FALSE (cue::isFxParameterAddress ("/godot/fx//p0"));
}

TEST_CASE ("fx door: the sparse row parses what is sound, drops what is not, and is spelled canonically under either locale")
{
    const auto parsed = cue::parseFxValues ("3:0.5 1:1 x:2 -1:0.2 2:abc 4:1.5 5:-0.5 :7 6:");
    REQUIRE (parsed.size() == 4u);
    CHECK (parsed.at (1) == doctest::Approx (1.0));
    CHECK (parsed.at (3) == doctest::Approx (0.5));
    CHECK (parsed.at (4) == doctest::Approx (1.0));      // clamped
    CHECK (parsed.at (5) == doctest::Approx (0.0));      // clamped
    CHECK (parsed.count (0) == 0);                        // "x:2" is not parameter nought
    CHECK (parsed.count (2) == 0);
    CHECK (parsed.count (6) == 0);

    INFO ("locale " << wfgtest::appliedLocaleName());
    CHECK (cue::formatFxValues (parsed) == "1:1 3:0.5 4:1 5:0");
    CHECK (cue::formatFxValues ({}) == "");
    CHECK (cue::parseFxValues ("").empty());

    /*  A round trip through the row's own spelling. */
    CHECK (cue::formatFxValues (cue::parseFxValues ("0:0.25 7:0.125")) == "0:0.25 7:0.125");
}

TEST_CASE ("fx door: a write to p<n> rewrites the cue's row, and the tree shows the value and the plugin's text for it")
{
    Rig rig;

    CHECK (rig.values() == "");
    CHECK (rig.at (rig.fxAddress ("p0")) == "0.5");          // the catalogue's default
    CHECK (rig.at (rig.fxAddress ("t0")) == "-6.0 dB");
    CHECK (rig.at (rig.fxAddress ("p1")) == "0");
    CHECK (rig.at (rig.fxAddress ("t1")) == "alive");

    CHECK (rig.set (rig.fxAddress ("p0"), "0.25").applied == 1);
    CHECK (rig.values() == "0:0.25");
    CHECK (rig.at (rig.fxAddress ("p0")) == "0.25");
    CHECK (rig.at (rig.fxAddress ("t0")) == "-12.0 dB");
    CHECK (rig.at (rig.fxAddress ("values")) == "0:0.25");

    CHECK (rig.set (rig.fxAddress ("p1"), "1").applied == 1);
    CHECK (rig.values() == "0:0.25 1:1");
    CHECK (rig.at (rig.fxAddress ("t1")) == "dead");

    /*  The record in the log is the node.set the hand sent, applied. */
    const auto applied = rig.lastApplied();
    REQUIRE (applied.size() == 2u);
    CHECK (applied[0] == rig.fxAddress ("p1"));
}

TEST_CASE ("fx door: what it refuses - a value outside nought and one, a word, a parameter the catalogue does not have, an Fx nobody made")
{
    Rig rig;
    REQUIRE (rig.set (rig.fxAddress ("p0"), "0.5").applied == 1);

    CHECK (rig.set (rig.fxAddress ("p0"), "1.5").applied == 0);
    CHECK (rig.set (rig.fxAddress ("p0"), "-0.1").applied == 0);
    CHECK (rig.set (rig.fxAddress ("p0"), "loud").applied == 0);
    CHECK (rig.set (rig.fxAddress ("p0"), "nan").applied == 0);
    CHECK (rig.values() == "0:0.5");

    /*  The test gain has two parameters; p2 is a node that does not exist. */
    CHECK (rig.set (rig.fxAddress ("p2"), "0.5").applied == 0);
    CHECK (rig.values() == "0:0.5");

    CHECK (rig.set ("/godot/fx/FX0NOPE0/p0", "0.5").applied == 0);

    /*  And the row written whole, by a script, is an ordinary node.set that
        the door leaves to the document. */
    CHECK (rig.set (rig.fxAddress ("values"), "1:1").applied == 1);
    CHECK (rig.values() == "1:1");
}

TEST_CASE ("fx door: with no catalogue for the plugin any index is accepted, which is what a replay needs")
{
    Rig rig;

    REQUIRE (rig.apply ("plugin.create", { osc::Value::string ("Verb"),
                                           osc::Value::string ("VST3-0badf00d-verb"),
                                           osc::Value::string ("VST3"), osc::Value::string ("") }).applied == 1);
    const auto verbId = rig.lastApplied().back();
    REQUIRE (rig.apply ("fx.create", { osc::Value::string (rig.cueId), osc::Value::string (verbId) }).applied == 1);
    const auto verbFx = rig.lastApplied().back();

    CHECK (rig.set ("/godot/fx/" + verbFx + "/p57", "0.75").applied == 1);
    CHECK (rig.document.findById (verbFx).getProperty ("values").toString() == "57:0.75");

    /*  No catalogue, no nodes to name it by - and the row still says it. */
    CHECK (rig.at ("/godot/fx/" + verbFx + "/values") == "57:0.75");
    CHECK (rig.at ("/godot/fx/" + verbFx + "/p57") == "");
}

TEST_CASE ("fx door: under the lock a write rides live - the tree shows it and the plugin's text for it, the row keeps the show's")
{
    /*  2026-09-26, the FX page: what a client reads at p<n> is what is
        heard, and so is the row - as an EQ row is - while the show still
        says what it said. `live` names what rides. */
    Rig rig;
    REQUIRE (rig.set (rig.fxAddress ("p0"), "0.25").applied == 1);
    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);

    CHECK (rig.at (rig.fxAddress ("live")) == "");
    CHECK (rig.set (rig.fxAddress ("p0"), "0.5", "surface:PORTBNK1").applied == 1);
    CHECK (rig.values() == "0:0.25");
    CHECK (rig.at (rig.fxAddress ("values")) == "0:0.5");
    CHECK (rig.at (rig.fxAddress ("p0")) == "0.5");
    CHECK (rig.at (rig.fxAddress ("t0")) == "-6.0 dB");
    CHECK (rig.at (rig.fxAddress ("live")) == "0");
    CHECK (rig.at ("/godot/document/live") == "1");

    /*  A parameter the catalogue says the plugin does not have is refused
        under the lock as well. */
    CHECK (rig.set (rig.fxAddress ("p2"), "0.5").applied == 0);

    /*  Kept once unlocked: the row says it, nothing rides. */
    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "false").ok);
    CHECK (rig.apply ("live.keep").applied == 1);
    CHECK (rig.values() == "0:0.5");
    CHECK (rig.at (rig.fxAddress ("p0")) == "0.5");
    CHECK (rig.at (rig.fxAddress ("live")) == "");
    CHECK (rig.at ("/godot/document/live") == "0");

    /*  And an insert deleted while its value rode is skipped by Keep. */
    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
    CHECK (rig.set (rig.fxAddress ("p1"), "1").applied == 1);
    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "false").ok);
    REQUIRE (rig.apply ("object.delete", { osc::Value::string (rig.fxId) }).applied == 1);
    CHECK (rig.apply ("live.keep").applied == 1);
    CHECK (rig.live.empty());
}

TEST_CASE ("fx door: a turn on one parameter is one undo step; two parameters are two; two hands are two")
{
    Rig rig;

    SUBCASE ("the same parameter twice from one origin, close together, is one step")
    {
        REQUIRE (rig.set (rig.fxAddress ("p0"), "0.1").applied == 1);
        REQUIRE (rig.set (rig.fxAddress ("p0"), "0.2").applied == 1);
        CHECK (rig.values() == "0:0.2");

        REQUIRE (rig.apply ("undo").applied == 1);
        CHECK (rig.values() == "");
    }

    SUBCASE ("two parameters are two steps, whatever row they share")
    {
        REQUIRE (rig.set (rig.fxAddress ("p0"), "0.3").applied == 1);
        REQUIRE (rig.set (rig.fxAddress ("p1"), "1").applied == 1);
        CHECK (rig.values() == "0:0.3 1:1");

        REQUIRE (rig.apply ("undo").applied == 1);
        CHECK (rig.values() == "0:0.3");

        REQUIRE (rig.apply ("undo").applied == 1);
        CHECK (rig.values() == "");
    }

    SUBCASE ("the same parameter from two origins is two steps")
    {
        REQUIRE (rig.set (rig.fxAddress ("p0"), "0.3", "udp:1").applied == 1);
        REQUIRE (rig.set (rig.fxAddress ("p0"), "0.4", "udp:2").applied == 1);

        REQUIRE (rig.apply ("undo").applied == 1);
        CHECK (rig.values() == "0:0.3");
    }
}

TEST_CASE ("fx door: a captured state is one step, and it joins the turn of the same insert that left it")
{
    /*  The author's decision of 2026-09-25: the whole state of a plugin kept
        per cue, saved a moment after the hand stops, and the turn and its
        state ONE Undo. `fx.capture` writes the file's name and every value
        in one transaction; after a turn of that insert's parameters, from the
        same origin, within the window, it joins the turn's step. */
    Rig rig;
    const auto file = "state/" + rig.pluginId + "-0123456789abcdef.state";
    const auto other = "state/" + rig.pluginId + "-fedcba9876543210.state";

    const auto capture = [&rig] (const std::string& name, const std::string& values, const char* origin = "cli")
    {
        return rig.apply ("fx.capture", { osc::Value::string (rig.fxId), osc::Value::string (name),
                                          osc::Value::string (values) }, origin);
    };

    const auto stateFile = [&rig]
    {
        return rig.document.findById (rig.fxId).getProperty ("stateFile").toString().toStdString();
    };

    SUBCASE ("alone, it writes the file and every value, canonically, as one step")
    {
        REQUIRE (capture (file, "1:0 0:0.25").applied == 1);
        CHECK (stateFile() == file);
        CHECK (rig.values() == "0:0.25 1:0");
        CHECK (rig.at (rig.fxAddress ("stateFile")) == file);

        REQUIRE (rig.apply ("undo").applied == 1);
        CHECK (stateFile().empty());
        CHECK (rig.values().empty());
    }

    SUBCASE ("after a turn of the same insert from the same hand, the turn and its state are one step")
    {
        REQUIRE (rig.set (rig.fxAddress ("p0"), "0.3").applied == 1);
        REQUIRE (rig.set (rig.fxAddress ("p0"), "0.4").applied == 1);
        rig.tick += 75;     // the helper's quiet moment, and the round trip

        REQUIRE (capture (file, "0:0.4 1:0").applied == 1);
        CHECK (stateFile() == file);

        REQUIRE (rig.apply ("undo").applied == 1);
        CHECK (stateFile().empty());
        CHECK (rig.values().empty());
    }

    SUBCASE ("from another hand it is a step of its own")
    {
        REQUIRE (rig.set (rig.fxAddress ("p0"), "0.3").applied == 1);
        REQUIRE (capture (file, "0:0.3 1:0", "udp:1").applied == 1);

        REQUIRE (rig.apply ("undo").applied == 1);
        CHECK (stateFile().empty());
        CHECK (rig.values() == "0:0.3");
    }

    SUBCASE ("too long after the turn it is a step of its own")
    {
        REQUIRE (rig.set (rig.fxAddress ("p0"), "0.3").applied == 1);
        rig.tick += doc::ShowDocument::captureJoinWindowTicks + 10;
        REQUIRE (capture (file, "0:0.3 1:0").applied == 1);

        REQUIRE (rig.apply ("undo").applied == 1);
        CHECK (stateFile().empty());
        CHECK (rig.values() == "0:0.3");
    }

    SUBCASE ("a second capture never joins, and the turn after one is a new step")
    {
        REQUIRE (rig.set (rig.fxAddress ("p0"), "0.3").applied == 1);
        REQUIRE (capture (file, "0:0.3 1:0").applied == 1);
        REQUIRE (capture (other, "0:0.3 1:1").applied == 1);
        REQUIRE (rig.set (rig.fxAddress ("p0"), "0.9").applied == 1);

        REQUIRE (rig.apply ("undo").applied == 1);
        CHECK (rig.values() == "0:0.3 1:1");
        CHECK (stateFile() == other);

        REQUIRE (rig.apply ("undo").applied == 1);
        CHECK (stateFile() == file);

        REQUIRE (rig.apply ("undo").applied == 1);
        CHECK (stateFile().empty());
        CHECK (rig.values().empty());
    }

    SUBCASE ("what it refuses: a name that is not plugins/state's, and an Fx nobody made")
    {
        CHECK (capture ("plugin.state", "0:0.5").applied == 0);
        CHECK (capture ("state/../show.state", "0:0.5").applied == 0);
        CHECK (capture ("state/a b.state", "0:0.5").applied == 0);
        CHECK (capture ("state/.state", "0:0.5").applied == 0);
        CHECK (capture ("/tmp/x.state", "0:0.5").applied == 0);
        CHECK (rig.apply ("fx.capture", { osc::Value::string ("FX0NOPE0"), osc::Value::string (file),
                                          osc::Value::string ("0:0.5") }).applied == 0);
        CHECK (stateFile().empty());
    }
}

TEST_CASE ("fx door: the cue lists its enabled inserts in chain order, and a switched-off one drops out")
{
    Rig rig;

    REQUIRE (rig.apply ("plugin.create", { osc::Value::string ("Verb"),
                                           osc::Value::string ("VST3-0badf00d-verb"),
                                           osc::Value::string ("VST3"), osc::Value::string ("") }).applied == 1);
    const auto verbId = rig.lastApplied().back();
    REQUIRE (rig.apply ("fx.create", { osc::Value::string (rig.cueId), osc::Value::string (verbId) }).applied == 1);
    const auto verbFx = rig.lastApplied().back();

    CHECK (rig.at ("/godot/cue/" + rig.cueId + "/fx") == rig.fxId + " " + verbFx);
    CHECK (rig.at (rig.fxAddress ("index")) == "0");
    CHECK (rig.at ("/godot/fx/" + verbFx + "/index") == "1");
    CHECK (rig.at (rig.fxAddress ("name")) == "Test gain");
    CHECK (rig.at (rig.fxAddress ("cue")) == rig.cueId);
    CHECK (rig.at (rig.fxAddress ("enabled")) == "true");

    REQUIRE (rig.set (rig.fxAddress ("enabled"), "false").applied == 1);
    CHECK (rig.at ("/godot/cue/" + rig.cueId + "/fx") == verbFx);

    /*  The chain's order is the set's, not the children's: the verb was
        created second and sits second whatever the cue did first. */
    REQUIRE (rig.set (rig.fxAddress ("enabled"), "true").applied == 1);
    CHECK (rig.at ("/godot/cue/" + rig.cueId + "/fx") == rig.fxId + " " + verbFx);
}
