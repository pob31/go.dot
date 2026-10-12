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
    WHAT BELONGS TO THE MACHINE (namespace draft §58, RD.3, decision AGC).

    A backup follows its main by applying the main's records, and two
    machines have two sets of hardware: the main's audio interface, MIDI
    cables, serial paths and displays are not the backup's. Decision D3 put
    those bindings in the show, so the show file now holds a few rows that are
    not decisions about the show but facts about the computer it runs on -
    marked `machine` in the parameter table's `scope` column - and a few
    commands act on the machine alone, marked `Command::machineLocal`. Neither
    may travel down the link, be compared between the two, or be a step of
    undo: a mirrored `undo` must take back the same step on both machines, so
    the two histories must have the same shape, and anything one machine does
    and the other never sees must leave no trace on its history - not a step,
    and not even the end of a coalescing window.

    What is pinned here: which rows and which commands are marked (both
    directions, so a mark added or lost is a decision somebody makes on
    purpose); that every machine row is reached at the address its own owner
    word spells, which is what lets the test from the address be a string
    test; the one predicate the link (RD.5) and both transaction hooks ask,
    `pair::isMachineLocal`; and the undo behaviour, through an engine wired
    with the hook `serve` and `replay` install.

    It saves a show, so every case runs under fr_FR as well as C.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/audio/AudioCommands.h>
#include <wfg/engine/audio/AudioSettings.h>
#include <wfg/engine/audio/MediaAnalyser.h>
#include <wfg/engine/command/Command.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/cue/CurveCommands.h>
#include <wfg/engine/cue/CurveTable.h>
#include <wfg/engine/cue/LiveRows.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/DocumentSession.h>
#include <wfg/engine/document/DocumentWriter.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/Schema.h>
#include <wfg/engine/document/SchemaTable.generated.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/pair/MachineLocal.h>
#include <wfg/engine/pair/PairSettings.h>
#include <wfg/engine/plugin/PluginCommands.h>
#include <wfg/engine/plugin/PluginTable.h>
#include <wfg/engine/process/PatchEditor.h>
#include <wfg/engine/video/Conversion.h>
#include <wfg/engine/video/FfmpegInstall.h>
#include <wfg/engine/video/VideoCommands.h>

#include <juce_core/juce_core.h>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace wfg;

namespace
{
    /*  THE ROWS THAT NAME THIS MACHINE'S HARDWARE, as RD.3 marked them after
        reading each one's description: the audio interface - which driver,
        which device each way, the block it is asked for, and the patch from
        the show's outputs and inputs onto its channels, with the two flags
        that say the patch has stopped following the list - a MIDI port's
        device each way, by name and by this machine's identifier, a serial
        port's path, and a video output's display, by name and identifier.
        And since RD.4 the machine's pairing - its role, the other machine's
        addresses, the seconds of silence that lose the link - runtime rows
        kept in pairing.xml and never in a show (AGC). */
    const std::set<std::string> machineRows {
        "audio/enabled", "audio/deviceType", "audio/outputDevice", "audio/inputDevice",
        "audio/bufferSize", "audio/inputPatch", "audio/outputPatch",
        "audio/patchSettled", "audio/inputPatchSettled",
        "port/outputDevice", "port/outputDeviceId", "port/inputDevice", "port/inputDeviceId",
        "serial/path",
        "videoOutput/display", "videoOutput/displayId",
        "pair/role", "pair/peers", "pair/timeout"
    };

    /*  THE COMMANDS THAT ACT ON THIS MACHINE ALONE, and the reports of what
        its own equipment did: choosing and testing its interface, its plugin
        host, its files and its folder, its windows and its puck - and its
        pairing, and the report of its file written (RD.4). */
    const std::set<std::string> machineCommands {
        "audio.configure", "audio.setup", "audio.defaults", "audio.defaultsReady",
        "audio.settingsReady", "audio.clockMoved", "audio.reconnect", "audio.connection",
        "audio.testSignal", "audio.testStop", "audio.editBuilt",
        "plugin.scan", "plugin.scanRetry", "plugin.scanned", "plugin.failed", "plugin.restart",
        "document.saveAs", "document.autosave", "document.recover", "document.discardRecovery",
        "media.cleanCache", "ffmpeg.install", "pd.install", "process.edit",
        "videoOutput.identify", "video.hideProjectors", "spacemouse.closeDriver",
        "pair.setRole", "pair.setPeers", "pair.setTimeout", "pair.saved",

        /*  And RD.7's: what the machine now is in its pair, and the operator's
            three - each acts on this machine, and tells the other its own way,
            never by being mirrored. */
        "pair.lead", "pair.hold", "pair.takeOver", "pair.handOver", "pair.keep",

        /*  And RD.8's: a follower seated at its leader's position, and an
            operator asking for it again - each this machine's alone. */
        "pair.seat", "pair.realign"
    };

    const std::string mainList  = "7K2QM9X4";
    const std::string firstCue  = "B3N8R5TW";
    const std::string thePort   = "C6P4M2HQ";
    const std::string theSerial = "D8Q3N7RS";
    const std::string theOutput = "E5R9T2VW";
    const std::string busA      = "F2G4H6J8";
    const std::string busB      = "G3H5J7K9";

    osc::Value text (const std::string& s) { return osc::Value::string (s); }

    std::string keyOf (const doc::AttributeRow& row)
    {
        return std::string (row.owner) + "/" + std::string (row.name);
    }

    /*  A scratch folder that cleans up after itself. */
    struct TempFolder
    {
        TempFolder()
        {
            parent = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("wfg-tests")
                       .getChildFile (juce::Uuid().toDashedString());
            parent.createDirectory();
            folder = parent.getChildFile ("machine");
        }

        ~TempFolder() { parent.deleteRecursively(); }

        TempFolder (const TempFolder&) = delete;
        TempFolder& operator= (const TempFolder&) = delete;

        juce::File parent, folder;
    };

    /*  An engine with every registrar that carries a machine-local command,
        and the document, cue and GO commands beside them, and the transaction
        hook wired as `serve` and `replay` wire it - the live-ride test, then
        the machine test, then a transaction. */
    struct Rig
    {
        Rig()
        {
            engine.log().openInMemory ({});

            doc::registerDocumentCommands (engine.commands(), document);
            cue::registerCueCommands (engine.commands(), document, focus);
            cue::registerRunCommands (engine.commands(), runs);
            cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);
            cue::registerCurveCommands (engine.commands(), engine, runner, document, curves);
            audio::registerAudioCommands (engine.commands(), audioState);
            audio::registerAudioSettingsCommands (engine, document, runner, runs, audioState);
            audio::registerAnalyserCommands (engine.commands(), nullptr);
            video::registerVideoCommands (engine.commands(), nullptr);
            video::registerConversionCommands (engine.commands(), document, nullptr, &runner.runNames());
            video::ffmpeg::registerInstallCommands (engine.commands(), nullptr);
            process::editor::registerCommands (engine.commands(), document, nullptr, nullptr);
            doc::registerBundleCommands (engine.commands(), document, session, writer);
            pair::registerPairCommands (engine.commands(), pairing);

            {
                plugin::PluginCommandHooks hooks;
                hooks.knows = plugin::pluginKnownBy (document);
                hooks.locked = plugin::showLockedBy (document);
                plugin::registerPluginCommands (engine.commands(), plugins, std::move (hooks));
            }

            runner.setSamplesPerTick (960);
            audioState.status = "running";

            engine.setBeforeApply ([this] (const Command& appliedCommand, const Event& submitted,
                                           const std::vector<osc::Value>& coerced, std::int64_t tickIndex)
                                   {
                                       if (cue::isLiveWrite (appliedCommand.name, coerced))
                                           return;

                                       if (pair::isMachineLocal (appliedCommand, coerced))
                                       {
                                           document.beginMachineEdit();
                                           return;
                                       }

                                       document.beginTransaction (appliedCommand.name, tickIndex,
                                                                  submitted.origin, coerced);
                                   });
        }

        Engine::TickResult apply (std::int64_t tick, const std::string& command,
                                  std::vector<osc::Value> args = {})
        {
            REQUIRE (engine.submit (std::string (origin::cli), command, std::move (args)));
            return engine.processTick (tick);
        }

        std::string undoName() const
        {
            return document.history (doc::UndoDomain::document).getUndoDescription().toStdString();
        }

        int drainUndoSteps()
        {
            int steps = 0;

            while (document.undo (doc::UndoDomain::document).has_value())
                ++steps;

            return steps;
        }

        std::string attribute (const std::string& address) const
        {
            return document.getAttribute (address).value_or (std::string ("<absent>"));
        }

        /*  A list, a cue in it, and one of each object a machine row hangs
            on: three steps, the last of them the video output. */
        void buildShow()
        {
            REQUIRE (apply (0, "list.create", { text ("Main"), text (mainList) }).applied == 1);
            REQUIRE (apply (1, "cue.create", { text (mainList), osc::Value::int32 (0), text ("memo"),
                                               text ("House to half"), text (firstCue) }).applied == 1);
            REQUIRE (apply (2, "port.create", { text ("Lights"), text (thePort) }).applied == 1);
            REQUIRE (apply (3, "serial.create", { text ("The Arduino"), text (theSerial) }).applied == 1);
            REQUIRE (apply (4, "videoOutput.create", { text ("Face"), text (""), text (theOutput) }).applied == 1);
        }

        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::Focus focus;
        doc::IdRegistry runIds { doc::IdRegistry::withSeed (58) };
        cue::Runner runner { document, runs, runIds, focus };
        cue::CurveTable curves;
        audio::AudioState audioState;
        plugin::PluginTable plugins;
        pair::Pairing pairing;
        doc::DocumentSession session { juce::File::getCurrentWorkingDirectory(), document.showRevision() };
        doc::DocumentWriter writer;
    };

    const std::string cueName = "/godot/cue/" + firstCue + "/name";
    const std::string interfaceOut = "/godot/audio/outputDevice";
    const std::string portOut = "/godot/port/" + thePort + "/outputDevice";
}

//==============================================================================
TEST_CASE ("machine scope: the rows marked machine are this machine's bindings, and only those")
{
    std::set<std::string> marked;

    for (const auto& row : doc::generated::attributes)
        if (row.scope == doc::Scope::machine)
            marked.insert (keyOf (row));

    for (const auto& key : marked)
        CHECK_MESSAGE (machineRows.count (key) == 1, "marked machine and not expected: " << key);

    for (const auto& key : machineRows)
        CHECK_MESSAGE (marked.count (key) == 1, "expected machine and not marked: " << key);

    for (const auto& key : machineRows)
    {
        const auto slash = key.find ('/');
        CHECK_MESSAGE (doc::Schema::isMachine (key.substr (0, slash), key.substr (slash + 1)), key);
    }

    /*  AND THE SHOW'S OWN BESIDE THEM stay the show's: the port's name, its
        switches and Doh! setting are decisions; the serial port's speed is the
        sketch's; the output's name, canvas and mapping are the venue's; the
        track count is the shape of the show. */
    for (const auto& row : std::vector<std::pair<std::string, std::string>> {
             { "audio", "tracks" }, { "audio", "panicFade" }, { "port", "name" }, { "port", "tx" },
             { "serial", "baud" }, { "videoOutput", "name" }, { "videoOutput", "mesh" },
             { "cue", "name" }, { "plugin", "path" }, { "mount", "host" } })
        CHECK_MESSAGE (! doc::Schema::isMachine (row.first, row.second), row.first << "/" << row.second);

    //  The friendlier wrapper reads the same column.
    const auto& schema = doc::Schema::instance();
    REQUIRE (schema.attribute ("Audio", "outputDevice") != nullptr);
    CHECK (schema.attribute ("Audio", "outputDevice")->isMachine());
    CHECK (schema.attribute ("Audio", "outputDevice")->scope() == doc::Scope::machine);
    REQUIRE (schema.attribute ("Audio", "tracks") != nullptr);
    CHECK_FALSE (schema.attribute ("Audio", "tracks")->isMachine());
    CHECK (schema.attribute ("Audio", "tracks")->scope() == doc::Scope::show);
}

TEST_CASE ("machine scope: every machine row is reached at the address its own owner word spells")
{
    /*  `Schema::isMachineAddress` reads an address as a string - the owner
        word and the row's name - and never asks the document, so that the
        main, the backup and a replay all answer alike from the record alone.
        That is only right while every machine row's owner word is the word
        its address is spelled with; this resolves each one through the
        document's own reader to prove it. A machine row added on an owner
        this map does not know fails here, and the remedy is to teach
        `isMachineAddress` its address before marking it. */
    Rig rig;
    rig.buildShow();

    const std::map<std::string, std::string> idOf {
        { "port", thePort }, { "serial", theSerial }, { "videoOutput", theOutput } };

    for (const auto& row : doc::generated::attributes)
    {
        if (row.scope != doc::Scope::machine)
            continue;

        const auto owner = std::string (row.owner);
        const auto name = std::string (row.name);

        std::string address;

        INFO ("row: " << keyOf (row));

        /*  THE PAIRING'S ROWS (RD.4) are the engine's kind: published by the
            tree at /godot/pair/<row>, held in pairing.xml and in no show, so
            there is no document to resolve them through - the address alone
            answers, and the schema puts the owner on no element. */
        if (owner == "pair")
        {
            CHECK (row.persist == doc::Persist::none);
            CHECK (doc::Schema::isMachineAddress ("/godot/pair/" + name));
            CHECK_FALSE (doc::Schema::isMachineAddress ("/godot/pair/X1/" + name));
            continue;
        }

        if (owner == "audio")
            address = "/godot/audio/" + name;
        else if (const auto found = idOf.find (owner); found != idOf.end())
            address = "/godot/" + owner + "/" + found->second + "/" + name;

        REQUIRE_MESSAGE (! address.empty(), "a machine row on an owner this test cannot address");

        const auto resolved = rig.document.resolve (address);
        REQUIRE (resolved.attribute != nullptr);
        CHECK (keyOf (*resolved.attribute->row) == keyOf (row));
        CHECK (resolved.attribute->isMachine());
        CHECK (doc::Schema::isMachineAddress (address));
    }

    //  And what is not one.
    for (const auto& address : { std::string ("/godot/audio/tracks"),
                                 cueName,
                                 "/godot/port/" + thePort + "/name",
                                 "/godot/serial/" + theSerial + "/baud",
                                 std::string ("audio/outputDevice"),
                                 std::string ("/godot/audio"),
                                 std::string ("/godot/audio/outputDevice/more"),
                                 "/godot/port/" + thePort + "/x/outputDevice",
                                 std::string ("/wfs/audio/outputDevice"),
                                 std::string ("/godot/pair/sending"),
                                 std::string ("/godot/engine/clock"),
                                 std::string ("") })
        CHECK_MESSAGE (! doc::Schema::isMachineAddress (address), address);
}

TEST_CASE ("machine scope: the commands that act on this machine alone are marked, and only those")
{
    Rig rig;

    for (const auto& name : machineCommands)
    {
        const auto* command = rig.engine.commands().find (name);
        REQUIRE_MESSAGE (command != nullptr, "not registered: " << name);
        CHECK_MESSAGE (command->machineLocal, "not marked machine-local: " << name);
    }

    for (const auto& command : rig.engine.commands().all())
        if (machineCommands.count (command.name) == 0)
            CHECK_MESSAGE (! command.machineLocal, "marked machine-local and not expected: " << command.name);

    /*  THE ONES LEFT ON PURPOSE, said here so a reader does not take them for
        forgotten: Apply and Load realise the show's own decisions - its track
        count, its plugin set - on each machine's own interface, and a backup
        that did not follow them would play a different graph; a save, a
        revert and a conversion are the show's, done on each machine's copy. */
    for (const std::string name : { "audio.apply", "plugin.load", "document.save", "document.revert",
                                    "media.convert", "go", "undo", "node.set", "node.setMany", "audio.armed" })
    {
        const auto* command = rig.engine.commands().find (name);
        REQUIRE_MESSAGE (command != nullptr, "not registered: " << name);
        CHECK_MESSAGE (! command->machineLocal, name);
    }
}

TEST_CASE ("machine scope: isMachineLocal answers never-mirror from the record alone")
{
    Rig rig;

    const auto local = [&rig] (const std::string& name, const std::vector<osc::Value>& args)
    {
        const auto* command = rig.engine.commands().find (name);
        REQUIRE_MESSAGE (command != nullptr, "not registered: " << name);
        return pair::isMachineLocal (*command, args);
    };

    //  Every marked command, whatever it is handed.
    for (const auto& name : machineCommands)
        CHECK_MESSAGE (local (name, {}), name);

    //  A value set on a machine row, alone or in a set of them.
    CHECK (local ("node.set", { text (interfaceOut), text ("Fireface UFX+") }));
    CHECK (local ("node.set", { text (portOut), text ("MIDIMATE II") }));
    CHECK (local ("node.set", { text ("/godot/serial/" + theSerial + "/path"), text ("COM3") }));
    CHECK (local ("node.set", { text ("/godot/videoOutput/" + theOutput + "/display"), text ("DELL U2720Q") }));
    CHECK (local ("node.setMany", { text (interfaceOut), text ("Fireface UFX+"),
                                    text (portOut), text ("MIDIMATE II") }));

    //  A show row, a device's own node, a set that mixes the two, and GO.
    CHECK_FALSE (local ("node.set", { text (cueName), text ("House to full") }));
    CHECK_FALSE (local ("node.set", { text ("/godot/audio/tracks"), osc::Value::int32 (16) }));
    CHECK_FALSE (local ("node.set", { text ("/wfs/input/3/x"), osc::Value::float32 (0.5f) }));
    CHECK_FALSE (local ("node.set", {}));
    CHECK_FALSE (local ("node.set", { osc::Value::int32 (3), text ("x") }));
    CHECK_FALSE (local ("node.setMany", { text (interfaceOut), text ("Fireface UFX+"),
                                          text (cueName), text ("House to full") }));
    CHECK_FALSE (local ("node.setMany", {}));
    CHECK_FALSE (local ("go", {}));
    CHECK_FALSE (local ("undo", { text ("document") }));
    CHECK_FALSE (local ("audio.apply", {}));
}

//==============================================================================
TEST_CASE ("machine scope: an interface or a port's device written is no step of undo, and still dirties and saves")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    Rig rig;
    rig.buildShow();

    REQUIRE (rig.undoName() == "videoOutput.create");
    const auto unsaved = rig.document.showRevision();

    REQUIRE (rig.apply (100, "node.set", { text (interfaceOut), text ("Fireface UFX+") }).applied == 1);
    REQUIRE (rig.apply (200, "node.set", { text (portOut), text ("MIDIMATE II") }).applied == 1);
    REQUIRE (rig.apply (300, "node.set", { text ("/godot/serial/" + theSerial + "/path"), text ("COM3") }).applied == 1);
    REQUIRE (rig.apply (400, "node.set", { text ("/godot/audio/bufferSize"), osc::Value::int32 (128) }).applied == 1);

    //  THE HISTORY IS AS IT WAS: the last step is still the output's creation.
    CHECK (rig.undoName() == "videoOutput.create");

    //  STILL THE SHOW'S FILE (D3 stands): the dot lights, and a save keeps them.
    CHECK (rig.document.showRevision() > unsaved);

    TempFolder temp;
    REQUIRE (doc::Bundle::save (temp.folder, rig.document).ok);

    doc::ShowDocument reopened;
    REQUIRE (doc::Bundle::open (temp.folder, reopened).ok);
    CHECK (reopened.getAttribute (interfaceOut) == std::optional<std::string> ("Fireface UFX+"));
    CHECK (reopened.getAttribute (portOut) == std::optional<std::string> ("MIDIMATE II"));
    CHECK (reopened.getAttribute ("/godot/serial/" + theSerial + "/path") == std::optional<std::string> ("COM3"));
    CHECK (reopened.getAttribute ("/godot/audio/bufferSize") == std::optional<std::string> ("128"));

    /*  AND UNDO WALKS PAST THEM: the five steps the show was built in, and the
        interface stays the one this machine was given. */
    CHECK (rig.drainUndoSteps() == 5);
    CHECK (rig.attribute (interfaceOut) == "Fireface UFX+");
}

TEST_CASE ("machine scope: a show edit beside a machine edit is still one step, as the other machine sees it")
{
    Rig rig;
    rig.buildShow();

    SUBCASE ("a machine row written in the middle of a drag does not end it")
    {
        /*  What the backup is sent is the two names, never the interface, and
            the two names within the window are one step there - so they have
            to be one step here too. */
        REQUIRE (rig.apply (100, "node.set", { text (cueName), text ("House to full") }).applied == 1);
        REQUIRE (rig.apply (102, "node.set", { text (interfaceOut), text ("Fireface UFX+") }).applied == 1);
        REQUIRE (rig.apply (104, "node.set", { text (cueName), text ("Houselights out") }).applied == 1);

        CHECK (rig.undoName() == "node.set");
        REQUIRE (rig.document.undo (doc::UndoDomain::document) == std::string ("node.set"));

        CHECK (rig.attribute (cueName) == "House to half");
        CHECK (rig.attribute (interfaceOut) == "Fireface UFX+");
        CHECK (rig.undoName() == "videoOutput.create");
    }

    SUBCASE ("a set mixing the two is one step, and its machine half is no part of what it coalesces on")
    {
        /*  RD.5 sends a mixed set down the link without its machine pairs, so
            the backup's key is the cue's name alone; the main's key leaves the
            interface out too, or a name written next would join on one machine
            and not on the other. */
        REQUIRE (rig.apply (100, "node.setMany", { text (cueName), text ("House to full"),
                                                   text (interfaceOut), text ("Fireface UFX+") }).applied == 1);
        REQUIRE (rig.apply (102, "node.set", { text (cueName), text ("Houselights out") }).applied == 1);

        CHECK (rig.undoName() == "node.setMany");
        REQUIRE (rig.document.undo (doc::UndoDomain::document) == std::string ("node.setMany"));

        /*  The step is the show's; the interface rode on it, and goes back with
            it, on this machine - the backup's never moved. */
        CHECK (rig.attribute (cueName) == "House to half");
        CHECK (rig.attribute (interfaceOut) != "Fireface UFX+");
        CHECK (rig.undoName() == "videoOutput.create");
    }

    SUBCASE ("the patch re-flowed by a show edit goes back with that edit")
    {
        /*  WHY A MACHINE ROW IS NOT KEPT OFF EVERY HISTORY: a settled patch
            follows the outputs when one moves, so each keeps the interface
            channels it had, and undoing the move has to put the patch back
            with it or the outputs come out of the wrong speakers. Both machines
            make that move, each on its own patch. */
        REQUIRE (rig.apply (100, "bus.create", { text ("mix"), osc::Value::int32 (1), osc::Value::int32 (-1), text (busA) }).applied == 1);
        REQUIRE (rig.apply (200, "bus.create", { text ("mix"), osc::Value::int32 (1), osc::Value::int32 (-1), text (busB) }).applied == 1);
        REQUIRE (rig.apply (300, "node.setMany", { text ("/godot/audio/outputPatch"), text ("3 7"),
                                                   text ("/godot/audio/patchSettled"), osc::Value::boolean (true) }).applied == 1);
        REQUIRE (rig.undoName() == "bus.create");

        REQUIRE (rig.apply (400, "bus.move", { text (busB), osc::Value::int32 (0) }).applied == 1);
        const auto moved = rig.attribute ("/godot/audio/outputPatch");
        CHECK (moved != "3 7");

        REQUIRE (rig.document.undo (doc::UndoDomain::document) == std::string ("bus.move"));
        CHECK (rig.attribute ("/godot/audio/outputPatch") == "3 7");
        CHECK (rig.undoName() == "bus.create");
    }
}

TEST_CASE ("machine scope: a machine-local command opens no transaction, and what it writes goes on no step")
{
    Rig rig;
    rig.buildShow();

    SUBCASE ("one between two writes of a drag does not split it")
    {
        REQUIRE (rig.apply (100, "node.set", { text (cueName), text ("House to full") }).applied == 1);
        REQUIRE (rig.apply (102, "audio.testStop").applied == 1);
        REQUIRE (rig.apply (104, "node.set", { text (cueName), text ("Houselights out") }).applied == 1);

        REQUIRE (rig.document.undo (doc::UndoDomain::document) == std::string ("node.set"));
        CHECK (rig.attribute (cueName) == "House to half");
        CHECK (rig.undoName() == "videoOutput.create");
    }

    SUBCASE ("the interface chosen right after an edit is not added to that edit's step")
    {
        /*  The edit's transaction is still the open one when `audio.configure`
            runs, and no new one is begun for it - so what it writes has to go
            on no history at all, or undoing the name would also take this
            machine's interface back. */
        REQUIRE (rig.apply (100, "node.set", { text (cueName), text ("House to full") }).applied == 1);
        REQUIRE (rig.apply (101, "audio.configure", { osc::Value::boolean (true), text ("ASIO"),
                                                      text ("Fireface UFX+"), text ("Fireface UFX+"),
                                                      osc::Value::int32 (256), text (""), text ("0 1") }).applied == 1);

        CHECK (rig.undoName() == "node.set");
        REQUIRE (rig.document.undo (doc::UndoDomain::document) == std::string ("node.set"));

        CHECK (rig.attribute (cueName) == "House to half");
        CHECK (rig.attribute (interfaceOut) == "Fireface UFX+");
        CHECK (rig.attribute ("/godot/audio/deviceType") == "ASIO");
        CHECK (rig.attribute ("/godot/audio/bufferSize") == "256");
        CHECK (rig.attribute ("/godot/audio/outputPatch") == "0 1");
        CHECK (rig.undoName() == "videoOutput.create");
    }

    SUBCASE ("alone, it leaves the history exactly as it was")
    {
        REQUIRE (rig.apply (100, "audio.configure", { osc::Value::boolean (true), text ("ASIO"),
                                                      text ("Fireface UFX+"), text (""),
                                                      osc::Value::int32 (0), text (""), text ("") }).applied == 1);

        CHECK (rig.undoName() == "videoOutput.create");
        CHECK (rig.drainUndoSteps() == 5);
        CHECK (rig.attribute (interfaceOut) == "Fireface UFX+");
    }
}
