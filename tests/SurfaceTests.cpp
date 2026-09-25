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

/*  PHASE 6'S OBJECTS IN THE DOCUMENT: surfaces, their strips, and DCAs.

    What a show says about the boxes it talks to and the trims its cues follow,
    before anything drives a fader: that `surface.create` makes the strips its
    profile implies in one command and records every identifier it drew, that
    a strip is published as the fourth slot kind, that a DCA cannot be put
    inside itself at the door or opened from a file that tries, and that a row
    naming several ports is checked port by port.

    Driven through the engine rather than the document, because what a replay
    reproduces is the applied record, and that is half of what is being tested.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/client/model/Text.h>
#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/surface/SurfaceCommands.h>
#include <wfg/engine/surface/SurfaceTable.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>

#include <juce_data_structures/juce_data_structures.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace wfg;

namespace
{
    struct Rig
    {
        Rig()
        {
            engine.log().openInMemory ({});
            doc::registerDocumentCommands (engine.commands(), document);
            surface::registerSurfaceCommands (engine.commands(), document, surfaces);

            /*  The transaction hook serve installs, so an undo case can ask
                whether one command made one step. */
            engine.setBeforeApply ([this] (const Command& appliedCommand, const Event& submitted,
                                           const std::vector<osc::Value>& coerced,
                                           std::int64_t tickIndex)
                                   {
                                       document.beginTransaction (appliedCommand.name, tickIndex,
                                                                  submitted.origin, coerced);
                                   });

            parameters.setSurfaces (&surfaces);
        }

        Engine::TickResult apply (const std::string& command, std::vector<osc::Value> args = {})
        {
            REQUIRE (engine.submit (std::string (origin::cli), command, std::move (args)));
            return engine.processTick (tick++);
        }

        /** Every argument of the most recent record, as it was APPLIED. */
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

        std::string at (const std::string& address)
        {
            parameters.markStale();
            snapshot = parameters.publish (tick, state);
            return client::model::text (*snapshot, address);
        }

        bool exists (const std::string& address)
        {
            parameters.markStale();
            snapshot = parameters.publish (tick, state);
            return snapshot->find (address) != nullptr;
        }

        /** The surface a `surface.create` just made, off its applied record. */
        std::string madeSurface() { return lastApplied().at (2); }

        Engine engine;
        doc::ShowDocument document;
        tree::MountTable mounts;
        cue::RunTable runs;
        tree::ParameterTree parameters { document, engine.commands(), mounts, runs };
        tree::EngineState state;
        surface::SurfaceTable surfaces;
        std::shared_ptr<const tree::TreeSnapshot> snapshot;
        std::int64_t tick = 1;
    };

    std::vector<std::string> words (const std::string& text)
    {
        std::vector<std::string> out;
        std::size_t at = 0;

        while (at < text.size())
        {
            const auto start = text.find_first_not_of (' ', at);

            if (start == std::string::npos)
                break;

            const auto end = text.find (' ', start);
            out.push_back (text.substr (start, end == std::string::npos ? std::string::npos
                                                                         : end - start));
            at = end == std::string::npos ? text.size() : end;
        }

        return out;
    }
}

//==============================================================================
TEST_CASE ("surface: a D700 arrives with its sixteen strips, and the record names every one")
{
    Rig rig;

    const auto result = rig.apply ("surface.create", { osc::Value::string ("d700"),
                                                       osc::Value::string ("The D700") });
    REQUIRE (result.applied == 1);

    /*  THE RECORD IS THE SURFACE: profile, name, the surface's identifier and
        each strip's, in order - what a replay is handed instead of drawing. */
    const auto applied = rig.lastApplied();
    REQUIRE (applied.size() == 3u + 16u);
    CHECK (applied[0] == "d700");
    CHECK (applied[1] == "The D700");

    const auto surfaceId = applied[2];
    const auto base = "/godot/surface/" + surfaceId + "/";

    CHECK (rig.at (base + "profile") == "d700");
    CHECK (rig.at (base + "name") == "The D700");
    CHECK (rig.at (base + "strips") == "16");
    CHECK (rig.at ("/godot/surface/order") == surfaceId);

    /*  A HARDWARE SURFACE NOBODY HAS TALKED TO IS NOT CONNECTED, which is the
        truth rather than a placeholder - and says nothing about why until the
        bridge has looked. */
    CHECK (rig.at (base + "connected") == "false");

    for (std::size_t i = 0; i < 16; ++i)
    {
        const auto strip = "/godot/slot/" + applied[3 + i] + "/";
        INFO ("strip " << i);

        CHECK (rig.at (strip + "kind") == "strip");
        CHECK (rig.at (strip + "surface") == surfaceId);
        CHECK (rig.at (strip + "index") == std::to_string (i));
        CHECK (rig.at (strip + "role") == "sampler");
        CHECK (rig.at (strip + "endpoint") == "absolute");
    }

    /*  AND THEY ARE SLOTS: the table's roster names them, after any processor
        input or rack channel, so a reader of who holds what reads one list. */
    const auto order = words (rig.at ("/godot/slot/order"));
    REQUIRE (order.size() == 16u);
    CHECK (order.front() == applied[3]);
    CHECK (order.back() == applied[18]);
}

TEST_CASE ("surface: a virtual panel is always connected, and a pad controller's strips are gates")
{
    Rig rig;

    REQUIRE (rig.apply ("surface.create", { osc::Value::string ("virtual") }).applied == 1);
    const auto panel = rig.madeSurface();
    CHECK (rig.at ("/godot/surface/" + panel + "/connected") == "true");
    CHECK (rig.at ("/godot/surface/" + panel + "/strips") == "8");

    REQUIRE (rig.apply ("surface.create", { osc::Value::string ("midiPads") }).applied == 1);
    const auto applied = rig.lastApplied();
    CHECK (applied.size() == 3u + 16u);
    CHECK (rig.at ("/godot/slot/" + applied[3] + "/endpoint") == "gate");

    /*  And a hardware one reads what the bridge found, once it has. */
    REQUIRE (rig.apply ("surface.create", { osc::Value::string ("mcu") }).applied == 1);
    const auto mcu = rig.madeSurface();
    rig.surfaces.set (mcu, { false, "the port \"Desk\" has no device behind it", {} });
    CHECK (rig.at ("/godot/surface/" + mcu + "/problem")
             == "the port \"Desk\" has no device behind it");
    rig.surfaces.set (mcu, { true, {}, "D700RTB" });
    CHECK (rig.at ("/godot/surface/" + mcu + "/connected") == "true");
    CHECK (rig.at ("/godot/surface/" + mcu + "/serial") == "D700RTB");
}

TEST_CASE ("surface: the rotaries' aim is one media cue, named by a command and published")
{
    /*  surface.aim (author, 2026-09-25): a SELECT on a sample strip, or a click
        on a running cue's name, says which cue the rotaries edit on the EQ and
        Send pages. One cue for every surface; not stored; a cue that is gone
        is no aim. */
    Rig rig;

    REQUIRE (rig.apply ("surface.create", { osc::Value::string ("d700") }).applied == 1);

    const auto list = rig.document.createList ("Main");
    REQUIRE (list.ok);
    const auto media = rig.document.createCue (list.id, 0, "media", "Kick");
    const auto memo = rig.document.createCue (list.id, 1, "memo", "Note");
    REQUIRE (media.ok);
    REQUIRE (memo.ok);

    CHECK (rig.exists ("/godot/surface/aim"));
    CHECK (rig.at ("/godot/surface/aim").empty());

    CHECK (rig.apply ("surface.aim", { osc::Value::string (media.id) }).applied == 1);
    CHECK (rig.surfaces.aim() == media.id);
    CHECK (rig.at ("/godot/surface/aim") == media.id);

    SUBCASE ("a cue that is not media, or no cue at all, is refused and moves nothing")
    {
        const auto memoAim = rig.apply ("surface.aim", { osc::Value::string (memo.id) });
        CHECK (memoAim.rejected == 1);

        const auto nowhere = rig.apply ("surface.aim", { osc::Value::string ("NQSCHQ00") });
        CHECK (nowhere.rejected == 1);

        CHECK (rig.at ("/godot/surface/aim") == media.id);
    }

    SUBCASE ("an empty argument lets go of it, and is applied")
    {
        CHECK (rig.apply ("surface.aim", { osc::Value::string ("") }).applied == 1);
        CHECK (rig.at ("/godot/surface/aim").empty());
    }

    SUBCASE ("a deleted cue is no aim, and an undo of the delete brings it back")
    {
        REQUIRE (rig.apply ("object.delete", { osc::Value::string (media.id) }).applied == 1);
        CHECK (rig.at ("/godot/surface/aim").empty());

        REQUIRE (rig.apply ("undo").applied == 1);
        CHECK (rig.at ("/godot/surface/aim") == media.id);
    }

    SUBCASE ("and it is no step of the show's history")
    {
        const auto& history = rig.document.history (doc::UndoDomain::document);
        const auto steps = history.getUndoDescriptions().size();

        REQUIRE (rig.apply ("surface.aim", { osc::Value::string ("") }).applied == 1);
        REQUIRE (rig.apply ("surface.aim", { osc::Value::string (media.id) }).applied == 1);
        CHECK (history.getUndoDescriptions().size() == steps);
    }
}

TEST_CASE ("surface: each surface's page is published every tick, from the runtime half")
{
    /*  The page a surface's rotaries show moves with no command - the
        surface's own EQ, Send and star buttons - so it is read off the table
        at every publish; the document half, a cache, must not carry it too. */
    Rig rig;

    REQUIRE (rig.apply ("surface.create", { osc::Value::string ("d700") }).applied == 1);
    const auto d700 = rig.madeSurface();
    const auto base = "/godot/surface/" + d700 + "/";

    CHECK (rig.at (base + "page") == "show");
    CHECK (rig.at (base + "pageIndex") == "0");
    CHECK (rig.at (base + "pageCount") == "1");
    CHECK (rig.at (base + "edited").empty());

    rig.surfaces.setPage (d700, { "eq", 1, 2, "/godot/cue/K1CKK1CK/eqB2Gain" });

    /*  NO REBUILD ASKED FOR: the runtime half reads it at the next publish. */
    const auto snapshot = rig.parameters.publish (rig.tick, rig.state);
    CHECK (client::model::text (*snapshot, base + "page") == "eq");
    CHECK (client::model::text (*snapshot, base + "pageIndex") == "1");
    CHECK (client::model::text (*snapshot, base + "pageCount") == "2");
    CHECK (client::model::text (*snapshot, base + "edited") == "/godot/cue/K1CKK1CK/eqB2Gain");

    /*  ONE ADDRESS, ONE HALF: every node the walk finds is found once. */
    std::vector<std::string> addresses;

    for (const auto* node : snapshot->all())
        addresses.push_back (node->address);

    std::sort (addresses.begin(), addresses.end());
    CHECK (std::adjacent_find (addresses.begin(), addresses.end()) == addresses.end());
}

TEST_CASE ("surface: a profile nobody knows is refused, and makes nothing")
{
    Rig rig;

    const auto result = rig.apply ("surface.create", { osc::Value::string ("hui") });
    CHECK (result.rejected == 1);
    CHECK (rig.engine.lastError().find ("bad-value") != std::string::npos);
    CHECK (rig.at ("/godot/surface/order").empty());
}

TEST_CASE ("surface: a replay hands back the identifiers, and a taken one refuses the whole surface")
{
    Rig rig;

    /*  THE IDENTIFIERS THE LIVE SESSION DREW, which is what a replayed record
        carries: used in order, the strips beyond them drawn. */
    const std::string surfaceId = "SVRF0001";
    const std::vector<std::string> strips { "STRP0001", "STRP0002", "STRP0003" };

    std::vector<osc::Value> args { osc::Value::string ("virtual"), osc::Value::string (""),
                                   osc::Value::string (surfaceId) };

    for (const auto& strip : strips)
        args.push_back (osc::Value::string (strip));

    REQUIRE (rig.apply ("surface.create", args).applied == 1);

    const auto applied = rig.lastApplied();
    REQUIRE (applied.size() == 3u + 8u);
    CHECK (applied[2] == surfaceId);
    CHECK (applied[3] == strips[0]);
    CHECK (applied[5] == strips[2]);
    CHECK (rig.at ("/godot/slot/STRP0002/index") == "1");

    /*  ONE OF THEM TAKEN: the surface is refused whole. A refusal that left a
        surface short of strips would be a document the refusal had changed. */
    const auto before = rig.at ("/godot/surface/order");

    std::vector<osc::Value> again { osc::Value::string ("virtual"), osc::Value::string (""),
                                    osc::Value::string ("SVRF0002"),
                                    osc::Value::string ("STRP0009"),
                                    osc::Value::string ("STRP0001") };   // taken above

    CHECK (rig.apply ("surface.create", again).rejected == 1);
    CHECK (rig.at ("/godot/surface/order") == before);
    CHECK_FALSE (rig.document.findById ("SVRF0002").isValid());
    CHECK_FALSE (rig.document.findById ("STRP0009").isValid());
}

TEST_CASE ("surface: one command is one undo step, strips and all")
{
    Rig rig;

    REQUIRE (rig.apply ("surface.create", { osc::Value::string ("d700") }).applied == 1);
    const auto surfaceId = rig.madeSurface();
    const auto firstStrip = rig.lastApplied().at (3);

    REQUIRE (rig.document.undo (doc::UndoDomain::document).has_value());

    CHECK_FALSE (rig.document.findById (surfaceId).isValid());
    CHECK_FALSE (rig.document.findById (firstStrip).isValid());
    CHECK_FALSE (rig.document.history (doc::UndoDomain::document).canUndo());
}

TEST_CASE ("surface: a strip can be added to any surface, and only to a surface")
{
    Rig rig;

    REQUIRE (rig.apply ("surface.create", { osc::Value::string ("mcu") }).applied == 1);
    const auto surfaceId = rig.madeSurface();

    /*  A MACKIE UNIT WITH AN EXTENDER is sixteen strips on two ports, so the
        profile's eight is where a surface starts and not a ceiling. */
    REQUIRE (rig.apply ("strip.create", { osc::Value::string (surfaceId) }).applied == 1);
    const auto added = rig.lastApplied().at (1);

    CHECK (rig.at ("/godot/surface/" + surfaceId + "/strips") == "9");
    CHECK (rig.at ("/godot/slot/" + added + "/index") == "8");

    REQUIRE (rig.apply ("dca.create", { osc::Value::string ("Band") }).applied == 1);
    const auto dcaId = rig.lastApplied().at (1);

    CHECK (rig.apply ("strip.create", { osc::Value::string (dcaId) }).rejected == 1);
    CHECK (rig.engine.lastError().find ("type-mismatch") != std::string::npos);
}

TEST_CASE ("surface: a strip's role and its DCA are the show's to decide")
{
    Rig rig;

    REQUIRE (rig.apply ("surface.create", { osc::Value::string ("virtual") }).applied == 1);
    const auto strip = rig.lastApplied().at (3);

    REQUIRE (rig.apply ("dca.create", { osc::Value::string ("Band") }).applied == 1);
    const auto band = rig.lastApplied().at (1);

    REQUIRE (rig.apply ("node.set", { osc::Value::string ("/godot/slot/" + strip + "/role"),
                                      osc::Value::string ("dca") }).applied == 1);
    REQUIRE (rig.apply ("node.set", { osc::Value::string ("/godot/slot/" + strip + "/dca"),
                                      osc::Value::string (band) }).applied == 1);

    CHECK (rig.at ("/godot/slot/" + strip + "/role") == "dca");
    CHECK (rig.at ("/godot/slot/" + strip + "/dca") == band);

    /*  WHERE IT SITS AND WHAT IT IS are derived, so a client cannot move a
        strip or turn a fader into a pad by writing a word. */
    CHECK (rig.apply ("node.set", { osc::Value::string ("/godot/slot/" + strip + "/index"),
                                    osc::Value::int32 (3) }).rejected == 1);
    CHECK (rig.apply ("node.set", { osc::Value::string ("/godot/slot/" + strip + "/endpoint"),
                                    osc::Value::string ("gate") }).rejected == 1);
}

//==============================================================================
TEST_CASE ("dca: DCAs nest, and one cannot be put inside itself")
{
    Rig rig;

    REQUIRE (rig.apply ("dca.create", { osc::Value::string ("Everything") }).applied == 1);
    const auto everything = rig.lastApplied().at (1);
    REQUIRE (rig.apply ("dca.create", { osc::Value::string ("Band") }).applied == 1);
    const auto band = rig.lastApplied().at (1);
    REQUIRE (rig.apply ("dca.create", { osc::Value::string ("Guitars") }).applied == 1);
    const auto guitars = rig.lastApplied().at (1);

    CHECK (words (rig.at ("/godot/dca/order")) == std::vector<std::string> { everything, band, guitars });
    CHECK (rig.at ("/godot/dca/" + band + "/name") == "Band");

    // Guitars inside Band inside Everything.
    REQUIRE (rig.apply ("node.set", { osc::Value::string ("/godot/dca/" + band + "/dca"),
                                      osc::Value::string (everything) }).applied == 1);
    REQUIRE (rig.apply ("node.set", { osc::Value::string ("/godot/dca/" + guitars + "/dca"),
                                      osc::Value::string (band) }).applied == 1);

    /*  EVERYTHING INSIDE GUITARS would close the circle, so the door refuses
        it - as it refuses a DCA inside itself, the circle of one. */
    CHECK (rig.apply ("node.set", { osc::Value::string ("/godot/dca/" + everything + "/dca"),
                                    osc::Value::string (guitars) }).rejected == 1);
    CHECK (rig.engine.lastError().find ("bad-value") != std::string::npos);
    CHECK (rig.apply ("node.set", { osc::Value::string ("/godot/dca/" + band + "/dca"),
                                    osc::Value::string (band) }).rejected == 1);

    CHECK (rig.at ("/godot/dca/" + everything + "/dca").empty());
    CHECK (rig.document.validate().empty());
}

TEST_CASE ("dca: a file carrying a circle of DCAs is refused when it is read")
{
    Rig rig;

    REQUIRE (rig.apply ("dca.create", { osc::Value::string ("A") }).applied == 1);
    const auto a = rig.lastApplied().at (1);
    REQUIRE (rig.apply ("dca.create", { osc::Value::string ("B") }).applied == 1);
    const auto b = rig.lastApplied().at (1);

    /*  Written around the door, the way a hand-edited file arrives. */
    auto dcas = rig.document.root().getChildWithName ("Dcas");
    dcas.getChildWithProperty ("id", juce::String (a)).setProperty ("dca", juce::String (b), nullptr);
    dcas.getChildWithProperty ("id", juce::String (b)).setProperty ("dca", juce::String (a), nullptr);

    const auto problems = rig.document.validate();
    REQUIRE (problems.size() == 2u);
    CHECK (problems[0].find ("sits inside itself") != std::string::npos);
}

TEST_CASE ("dca: a trim is not the show's, so the document will not store one")
{
    Rig rig;

    REQUIRE (rig.apply ("dca.create", { osc::Value::string ("Band") }).applied == 1);
    const auto band = rig.lastApplied().at (1);

    /*  `persist=none` IS DERIVED BY CONSTRUCTION: the document has nowhere to
        put what a fader is doing tonight. Phase 6's live door is what takes
        this write, in front of the document; the document itself refuses it. */
    CHECK_FALSE (rig.document.setAttribute ("/godot/dca/" + band + "/trim", "-6").ok);
}

//==============================================================================
TEST_CASE ("refers: a row naming several identifiers is checked one by one")
{
    Rig rig;

    REQUIRE (rig.document.createPort ("Bank 1").ok);
    REQUIRE (rig.document.createPort ("Bank 2").ok);

    const auto ports = rig.document.root().getChildWithName ("MidiPorts");
    const auto p1 = ports.getChild (0)["id"].toString().toStdString();
    const auto p2 = ports.getChild (1)["id"].toString().toStdString();

    REQUIRE (rig.apply ("surface.create", { osc::Value::string ("d700") }).applied == 1);
    const auto surfaceId = rig.madeSurface();
    const auto address = "/godot/surface/" + surfaceId + "/ports";

    const auto warningsAbout = [&rig, &surfaceId]
    {
        std::vector<std::string> out;

        for (const auto& warning : rig.document.warnings())
            if (warning.find (surfaceId) != std::string::npos)
                out.push_back (warning);

        return out;
    };

    /*  TWO PORTS, BOTH DECLARED: nothing to say. A single-valued reading of the
        row would have looked for one identifier called "P1 P2". */
    REQUIRE (rig.apply ("node.set", { osc::Value::string (address),
                                      osc::Value::string (p1 + " " + p2) }).applied == 1);
    CHECK (warningsAbout().empty());

    /*  ONE OF THEM NOT A PORT: one warning, naming that one only. */
    REQUIRE (rig.apply ("node.set", { osc::Value::string (address),
                                      osc::Value::string (p1 + " " + surfaceId) }).applied == 1);
    const auto warned = warningsAbout();
    REQUIRE (warned.size() == 1u);
    CHECK (warned[0].find ("is a surface and not a port") != std::string::npos);
    CHECK (warned[0].find (p1) == std::string::npos);
}

TEST_CASE ("surface: a show that arrives without the containers is given them")
{
    doc::ShowDocument fresh;

    CHECK (fresh.root().getChildWithName ("Surfaces").isValid());
    CHECK (fresh.root().getChildWithName ("Dcas").isValid());

    /*  And a surface can be declared in it at once - the hole `ensureContainers`
        exists to close, which `createMount` fell into before it existed. */
    std::vector<std::string> strips;
    CHECK (fresh.createSurface ("virtual", "Panel", {}, {}, strips).ok);
    CHECK (strips.size() == 8u);
    CHECK (fresh.createDca ("Band").ok);
}
