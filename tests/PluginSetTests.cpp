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

/*  PHASE 9a'S PLUGIN SET IN THE DOCUMENT (decision AE): the processors a show
    carries on every voice, declared once as the tracks are. What a show says
    about them before any child process exists: that `plugin.create` makes the
    set on demand at a fixed place under <Audio>, so the canonical bytes do not
    depend on which container was asked for first; that every entry is
    published at /godot/plugin/<id> with the order beside it; that the four
    rows the machine fills read off a table, and read `unloaded` honestly
    until something fills them; and that a locked show refuses without gaining
    an empty container.

    Driven through the engine rather than the document, because what a replay
    reproduces is the applied record.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/client/model/Text.h>
#include <wfg/engine/Engine.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/plugin/PluginTable.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>

#include <juce_data_structures/juce_data_structures.h>

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

            engine.setBeforeApply ([this] (const Command& appliedCommand, const Event& submitted,
                                           const std::vector<osc::Value>& coerced,
                                           std::int64_t tickIndex)
                                   {
                                       document.beginTransaction (appliedCommand.name, tickIndex,
                                                                  submitted.origin, coerced);
                                   });
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

        /** The Audio element's children, by element name, in order. */
        std::vector<std::string> audioChildren() const
        {
            std::vector<std::string> out;

            for (const auto& child : document.root().getChildWithName ("Audio"))
                out.push_back (child.getType().toString().toStdString());

            return out;
        }

        Engine engine;
        doc::ShowDocument document;
        tree::MountTable mounts;
        cue::RunTable runs;
        tree::ParameterTree parameters { document, engine.commands(), mounts, runs };
        tree::EngineState state;
        std::shared_ptr<const tree::TreeSnapshot> snapshot;
        std::int64_t tick = 1;
    };

    std::vector<osc::Value> entry (const char* name, const char* identifier,
                                   const char* format, const char* path, const char* id = nullptr)
    {
        std::vector<osc::Value> out { osc::Value::string (name), osc::Value::string (identifier),
                                      osc::Value::string (format), osc::Value::string (path) };

        if (id != nullptr)
            out.push_back (osc::Value::string (id));

        return out;
    }
}

//==============================================================================
TEST_CASE ("plugin set: plugin.create makes the set on demand, after the buses and before the rack")
{
    Rig rig;

    //  A rack first, so the fixed place has something to be before.
    REQUIRE (rig.apply ("channel.create", { osc::Value::string ("mono") }).applied == 1);
    CHECK (rig.audioChildren() == std::vector<std::string> { "Rack" });

    const auto result = rig.apply ("plugin.create",
                                   entry ("Test gain", "godot:test-gain", "VST3", "C:/plugins/test.vst3"));
    REQUIRE (result.applied == 1);

    /*  THE RECORD IS THE ENTRY: the four words as given and the identifier
        drawn, so a replay on a machine that has never scanned needs no known
        list to make the same object. */
    const auto applied = rig.lastApplied();
    REQUIRE (applied.size() == 5u);
    CHECK (applied[0] == "Test gain");
    CHECK (applied[1] == "godot:test-gain");
    CHECK (applied[2] == "VST3");
    CHECK (applied[3] == "C:/plugins/test.vst3");

    const auto id = applied[4];
    REQUIRE (id.size() == 8u);

    //  Before the rack, whichever was asked for first (plan decision 20's neighbour).
    CHECK (rig.audioChildren() == std::vector<std::string> { "Plugins", "Rack" });

    const auto base = "/godot/plugin/" + id + "/";
    CHECK (rig.at ("/godot/plugin/order") == id);
    CHECK (rig.at (base + "name") == "Test gain");
    CHECK (rig.at (base + "identifier") == "godot:test-gain");
    CHECK (rig.at (base + "format") == "VST3");
    CHECK (rig.at (base + "path") == "C:/plugins/test.vst3");
    CHECK (rig.at (base + "preset") == "");

    /*  WHAT THE MACHINE FOUND, honestly: nothing has looked, so it is
        unloaded with no sentence, no latency and no parameters. */
    CHECK (rig.at (base + "state") == "unloaded");
    CHECK (rig.at (base + "problem") == "");
    CHECK (rig.at (base + "latencySamples") == "0");
    CHECK (rig.at (base + "paramCount") == "0");

    SUBCASE ("a second entry joins the same container, after the first")
    {
        REQUIRE (rig.apply ("plugin.create",
                            entry ("Verb", "VST3-abcd1234-verb", "VST3", "C:/plugins/verb.vst3")).applied == 1);

        const auto second = rig.lastApplied().at (4);

        CHECK (rig.audioChildren() == std::vector<std::string> { "Plugins", "Rack" });
        CHECK (rig.at ("/godot/plugin/order") == id + " " + second);
        CHECK (rig.at ("/godot/plugin/" + second + "/name") == "Verb");
    }

    SUBCASE ("and an entry is taken out with object.delete, the container staying")
    {
        REQUIRE (rig.apply ("object.delete", { osc::Value::string (id) }).applied == 1);

        CHECK (rig.at ("/godot/plugin/order") == "");
        CHECK_FALSE (rig.exists (base + "name"));
        CHECK (rig.audioChildren() == std::vector<std::string> { "Plugins", "Rack" });
    }

    SUBCASE ("and the name is the one row a hand edits")
    {
        REQUIRE (rig.apply ("node.set", { osc::Value::string (base + "name"),
                                          osc::Value::string ("Gain, test") }).applied == 1);
        CHECK (rig.at (base + "name") == "Gain, test");

        //  The identifier is fixed at creation: written only by plugin.create.
        CHECK (rig.apply ("node.set", { osc::Value::string (base + "identifier"),
                                        osc::Value::string ("other") }).rejected == 1);
        CHECK (rig.at (base + "identifier") == "godot:test-gain");
    }
}

TEST_CASE ("plugin set: the set sits after the last bus, and the buses keep their place")
{
    Rig rig;

    REQUIRE (rig.apply ("bus.create", { osc::Value::string ("direct"), osc::Value::int32 (2), osc::Value::int32 (-1) }).applied == 1);
    REQUIRE (rig.apply ("plugin.create", entry ("Test gain", "godot:test-gain", "VST3", "")).applied == 1);
    REQUIRE (rig.apply ("bus.create", { osc::Value::string ("mix"), osc::Value::int32 (2), osc::Value::int32 (-1) }).applied == 1);

    /*  THE SET WAS PLACED AFTER THE LAST BUS THERE WAS when it was made. A
        bus made afterwards lands past the last child, after it - the
        document's own rule for every append (`rawIndexForPosition`: past the
        last member is past the last child, which is where a <Rack> has always
        ended up too), and the output list reads the buses in their own order
        wherever the containers sit. What this pins is the placement at
        creation and that nothing moved the first bus. */
    const auto children = rig.audioChildren();
    REQUIRE (children.size() == 3u);
    CHECK (children[0] == "Bus");
    CHECK (children[1] == "Plugins");
    CHECK (children[2] == "Bus");
}

TEST_CASE ("plugin set: the canonical bytes do not depend on which container was asked for first")
{
    Rig first, second;

    REQUIRE (first.apply ("channel.create", { osc::Value::string ("mono"), osc::Value::string ("RACK0001") }).applied == 1);
    REQUIRE (first.apply ("plugin.create", entry ("Test gain", "godot:test-gain", "VST3", "", "PG7N0001")).applied == 1);

    REQUIRE (second.apply ("plugin.create", entry ("Test gain", "godot:test-gain", "VST3", "", "PG7N0001")).applied == 1);
    REQUIRE (second.apply ("channel.create", { osc::Value::string ("mono"), osc::Value::string ("RACK0001") }).applied == 1);

    CHECK (doc::CanonicalXml::write (first.document) == doc::CanonicalXml::write (second.document));
    CHECK (first.audioChildren() == std::vector<std::string> { "Plugins", "Rack" });
    CHECK (second.audioChildren() == std::vector<std::string> { "Plugins", "Rack" });
}

TEST_CASE ("plugin set: the four rows the machine fills read off the table, and a change is a rebuild")
{
    Rig rig;

    REQUIRE (rig.apply ("plugin.create", entry ("Test gain", "godot:test-gain", "VST3", "")).applied == 1);
    const auto id = rig.lastApplied().at (4);
    const auto base = "/godot/plugin/" + id + "/";

    plugin::PluginTable table;
    rig.parameters.setPlugins (&table);

    CHECK (rig.at (base + "state") == "unloaded");

    /*  THE HOST'S REPORT: loaded, sixty-four samples late, twelve parameters.
        `set` answers whether a reader could tell, which is what the host asks
        before it marks the tree stale. */
    CHECK (table.set (id, { "loaded", "", 64, 12, 0.0, {} }));
    CHECK_FALSE (table.set (id, { "loaded", "", 64, 12, 0.0, {} }));

    CHECK (rig.at (base + "state") == "loaded");
    CHECK (rig.at (base + "problem") == "");
    CHECK (rig.at (base + "latencySamples") == "64");
    CHECK (rig.at (base + "paramCount") == "12");

    CHECK (table.set (id, { "failed", "the child died after 8 misses", 64, 12, 0.0, {} }));
    CHECK (rig.at (base + "state") == "failed");
    CHECK (rig.at (base + "problem") == "the child died after 8 misses");

    //  An entry nobody has looked at reads the default, whatever the table holds for others.
    CHECK (table.statusOf ("NOPE0001").state == "unloaded");
}

TEST_CASE ("plugin set: an entry the graph was built without says what brings it in, and the set reads changed")
{
    Rig rig;
    plugin::PluginTable table;
    rig.parameters.setPlugins (&table);

    REQUIRE (rig.apply ("plugin.create", entry ("Test gain", "godot:test-gain", "VST3", "")).applied == 1);
    const auto first = rig.lastApplied().at (4);

    //  No graph: nothing to differ from, and nothing said.
    CHECK (rig.at ("/godot/plugin/changed") == "false");
    CHECK (rig.at ("/godot/plugin/" + first + "/problem") == "");

    //  The graph built with the one entry.
    table.setBuilt ({ first });
    CHECK (rig.at ("/godot/plugin/changed") == "false");

    REQUIRE (rig.apply ("plugin.create", entry ("Verb", "VST3-abcd1234-verb", "VST3", "")).applied == 1);
    const auto second = rig.lastApplied().at (4);

    CHECK (rig.at ("/godot/plugin/changed") == "true");
    CHECK (rig.at ("/godot/plugin/" + second + "/state") == "unloaded");
    CHECK (rig.at ("/godot/plugin/" + second + "/problem").find ("Load now") != std::string::npos);
    CHECK (rig.at ("/godot/plugin/" + first + "/problem") == "");

    //  Rebuilt with both: the same again.
    table.setBuilt ({ first, second });
    CHECK (rig.at ("/godot/plugin/changed") == "false");
    CHECK (rig.at ("/godot/plugin/" + second + "/problem") == "");

    //  And no graph at all again - the host stopped - is no change either.
    table.clearBuilt();
    CHECK (rig.at ("/godot/plugin/changed") == "false");
}

TEST_CASE ("plugin set: a format word the show cannot store is refused bad-value, and makes nothing")
{
    Rig rig;

    /*  JUCE calls an AU `AudioUnit`; the schema says `AU` (2026-09-26). A
        client passing the scan's own name through would write a show that
        no longer validates, so the door refuses it. */
    const auto result = rig.apply ("plugin.create", entry ("Delay", "AudioUnit:Effects/aufx,dely,appl",
                                                           "AudioUnit", ""));
    CHECK (result.rejected == 1);
    CHECK (rig.audioChildren().empty());

    const auto parsed = LogFile::parse (rig.engine.log().contents());
    REQUIRE (! parsed.records.empty());
    CHECK (parsed.records.back().kind == LogRecord::Kind::rejected);
    CHECK (parsed.records.back().reason == reason::badValue);

    //  The three words, and none at all, are the show's.
    CHECK (rig.apply ("plugin.create", entry ("Delay", "AudioUnit:Effects/aufx,dely,appl", "AU", "")).applied == 1);
    CHECK (rig.apply ("plugin.create", entry ("Amp", "LV2-amp-00000000-00000000", "LV2", "")).applied == 1);
    CHECK (rig.apply ("plugin.create", entry ("Test gain", "godot:test-gain", "", "")).applied == 1);
}

TEST_CASE ("plugin set: a locked show refuses the create, and gains no empty container")
{
    Rig rig;

    REQUIRE (rig.apply ("node.set", { osc::Value::string ("/godot/document/locked"),
                                      osc::Value::string ("true") }).applied == 1);

    const auto result = rig.apply ("plugin.create", entry ("Test gain", "godot:test-gain", "VST3", ""));
    CHECK (result.rejected == 1);
    CHECK (rig.audioChildren().empty());
    CHECK (rig.at ("/godot/plugin/order") == "");        // the container's row is there, and empty
}

//==============================================================================
TEST_CASE ("plugin set: an Fx is one entry switched in on one media cue, made once, and refused where it makes no sense")
{
    Rig rig;

    REQUIRE (rig.apply ("list.create", { osc::Value::string ("Main") }).applied == 1);
    const auto listId = rig.lastApplied().back();
    REQUIRE (rig.apply ("cue.create", { osc::Value::string (listId), osc::Value::int32 (0),
                                        osc::Value::string ("media"), osc::Value::string ("Tone") }).applied == 1);
    const auto mediaId = rig.lastApplied().back();
    REQUIRE (rig.apply ("cue.create", { osc::Value::string (listId), osc::Value::int32 (1),
                                        osc::Value::string ("memo"), osc::Value::string ("Note") }).applied == 1);
    const auto memoId = rig.lastApplied().back();
    REQUIRE (rig.apply ("plugin.create", { osc::Value::string ("Test gain"),
                                           osc::Value::string ("godot:test-gain"),
                                           osc::Value::string ("VST3"), osc::Value::string ("") }).applied == 1);
    const auto gainId = rig.lastApplied().back();

    /*  Refused: a cue that plays nothing, an entry that is not there, a cue
        that is not there. */
    CHECK (rig.apply ("fx.create", { osc::Value::string (memoId), osc::Value::string (gainId) }).applied == 0);
    CHECK (rig.apply ("fx.create", { osc::Value::string (mediaId), osc::Value::string ("PG7N0999") }).applied == 0);
    CHECK (rig.apply ("fx.create", { osc::Value::string ("XX000000"), osc::Value::string (gainId) }).applied == 0);

    REQUIRE (rig.apply ("fx.create", { osc::Value::string (mediaId), osc::Value::string (gainId) }).applied == 1);
    const auto fxId = rig.lastApplied().back();

    /*  Once per entry per cue. */
    CHECK (rig.apply ("fx.create", { osc::Value::string (mediaId), osc::Value::string (gainId) }).applied == 0);

    /*  Under the cue, with the entry named and nothing else stored: the
        switch and the values are defaults the canonical writer omits. */
    const auto fx = rig.document.findById (fxId);
    REQUIRE (fx.isValid());
    CHECK (fx.hasType ("Fx"));
    CHECK (fx.getParent().getProperty ("id").toString().toStdString() == mediaId);
    CHECK (fx.getProperty ("plugin").toString().toStdString() == gainId);
    CHECK_FALSE (fx.hasProperty ("values"));
    CHECK_FALSE (fx.hasProperty ("enabled"));

    CHECK (rig.at ("/godot/fx/" + fxId + "/plugin") == gainId);
    CHECK (rig.at ("/godot/fx/" + fxId + "/enabled") == "true");
    CHECK (rig.at ("/godot/fx/" + fxId + "/cue") == mediaId);
    CHECK (rig.at ("/godot/fx/" + fxId + "/name") == "Test gain");
    CHECK (rig.at ("/godot/fx/" + fxId + "/index") == "0");
    CHECK (rig.at ("/godot/cue/" + mediaId + "/fx") == fxId);

    /*  With an explicit id, as a replay makes it. */
    REQUIRE (rig.apply ("plugin.create", { osc::Value::string ("Verb"),
                                           osc::Value::string ("VST3-0badf00d-verb"),
                                           osc::Value::string ("VST3"), osc::Value::string ("") }).applied == 1);
    const auto verbId = rig.lastApplied().back();
    REQUIRE (rig.apply ("fx.create", { osc::Value::string (mediaId), osc::Value::string (verbId),
                                       osc::Value::string ("FX7N0002") }).applied == 1);
    CHECK (rig.lastApplied().back() == "FX7N0002");
    CHECK (rig.at ("/godot/fx/FX7N0002/index") == "1");

    /*  Deleted like any object, and undone whole. */
    REQUIRE (rig.apply ("object.delete", { osc::Value::string (fxId) }).applied == 1);
    CHECK_FALSE (rig.exists ("/godot/fx/" + fxId + "/plugin"));
    CHECK (rig.at ("/godot/cue/" + mediaId + "/fx") == "FX7N0002");
    REQUIRE (rig.apply ("undo").applied == 1);
    CHECK (rig.at ("/godot/fx/" + fxId + "/plugin") == gainId);
}
