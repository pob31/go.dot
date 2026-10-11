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
    THE MACHINE'S PAIRING (namespace draft §58, RD.4; AGC, AFV).

    Which machine is the main, where the other one is and how long a silence
    loses it are facts about the computer, kept in Go.dot's own folder as
    `pairing.xml` and never in a show. What is pinned here: the file - a round
    trip that a French machine reads back exactly, a missing file that is an
    unpaired machine, a broken one that is the defaults with a sentence rather
    than a refusal to start, a peer without a port given 5010 - and the three
    commands that write it: what they accept and refuse, that they ask for the
    file and never write it themselves, that the file keeps its own values
    under a command-line override, and that they are this machine's - never
    mirrored, no step of undo, never refused for the show's lock, nor by an
    audio outage.

    The file holds a number of seconds, so every case runs under fr_FR too.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/audio/AudioCommands.h>
#include <wfg/engine/audio/AudioSettings.h>
#include <wfg/engine/command/Command.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/pair/MachineLocal.h>
#include <wfg/engine/pair/PairSettings.h>

#include <juce_core/juce_core.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace wfg;

namespace
{
    osc::Value text (const std::string& s) { return osc::Value::string (s); }

    /*  A scratch folder that cleans up after itself, standing for Go.dot's own
        folder on a machine - never the developer's. */
    struct TempFolder
    {
        TempFolder()
        {
            folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("wfg-tests")
                       .getChildFile (juce::Uuid().toDashedString());
            folder.createDirectory();
        }

        ~TempFolder() { folder.deleteRecursively(); }

        TempFolder (const TempFolder&) = delete;
        TempFolder& operator= (const TempFolder&) = delete;

        std::string file (const std::string& name) const
        {
            return folder.getChildFile (juce::String (name)).getFullPathName().toStdString();
        }

        juce::File folder;
    };

    void write (const std::string& path, const std::string& contents)
    {
        REQUIRE (juce::File (juce::String (path)).replaceWithText (juce::String (contents)));
    }

    std::string read (const std::string& path)
    {
        return juce::File (juce::String (path)).loadFileAsString().toStdString();
    }

    /*  An engine with the pairing commands and what they sit beside - the
        document's and the cues' commands, the audio settings' with their
        admission check - and the transaction hook wired as serve wires it.
        A file write is asked through `requestSave`, which here only keeps
        what it was asked for. */
    struct Rig
    {
        Rig()
        {
            engine.log().openInMemory ({});

            doc::registerDocumentCommands (engine.commands(), document);
            cue::registerCueCommands (engine.commands(), document, focus);
            audio::registerAudioSettingsCommands (engine, document, runner, runs, audioState);
            pair::registerPairCommands (engine.commands(), pairing);

            pairing.requestSave = [this] (const pair::PairSettings& settings) { saves.push_back (settings); };
            audioState.status = "running";

            engine.setBeforeApply ([this] (const Command& appliedCommand, const Event& submitted,
                                           const std::vector<osc::Value>& coerced, std::int64_t tickIndex)
                                   {
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
                                  std::vector<osc::Value> args = {},
                                  const std::string& from = std::string (origin::cli))
        {
            REQUIRE (engine.submit (from, command, std::move (args)));
            return engine.processTick (tick);
        }

        LogRecord lastRecord()
        {
            const auto parsed = LogFile::parse (engine.log().contents());
            REQUIRE (! parsed.records.empty());
            return parsed.records.back();
        }

        std::string undoName() const
        {
            return document.history (doc::UndoDomain::document).getUndoDescription().toStdString();
        }

        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::Focus focus;
        doc::IdRegistry runIds { doc::IdRegistry::withSeed (58) };
        cue::Runner runner { document, runs, runIds, focus };
        audio::AudioState audioState;
        pair::Pairing pairing;
        std::vector<pair::PairSettings> saves;
    };

    const std::string mainList = "7K2QM9X4";
}

//==============================================================================
TEST_CASE ("pairing: the settings round-trip through pairing.xml, a fraction of a second exactly")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    TempFolder temp;
    const auto file = temp.file ("pairing.xml");

    pair::PairSettings settings;
    settings.role = "backup";
    settings.peers = { "10.0.0.2:5010", "10.1.0.2:6010" };
    settings.timeout = 1.5;

    REQUIRE (pair::save (file, settings).empty());

    //  THE NUMBER AS THE LOG SPELLS IT, never with the French decimal comma.
    const auto written = read (file);
    CHECK (written.find ("timeout=\"1.5\"") != std::string::npos);
    CHECK (written.find ("1,5") == std::string::npos);

    const auto loaded = pair::load (file);
    CHECK (loaded.problem.empty());
    CHECK (loaded.settings.role == "backup");
    CHECK (loaded.settings.peers == std::vector<std::string> { "10.0.0.2:5010", "10.1.0.2:6010" });
    CHECK (osc::Value::float64 (loaded.settings.timeout) == osc::Value::float64 (1.5));

    //  And a tenth, the shortest a timeout may be: shortest round-trip both ways.
    settings.timeout = 0.1;
    settings.role = "main";
    settings.peers.clear();
    REQUIRE (pair::save (file, settings).empty());
    CHECK (read (file).find ("timeout=\"0.1\"") != std::string::npos);

    const auto again = pair::load (file);
    CHECK (again.problem.empty());
    CHECK (again.settings.role == "main");
    CHECK (again.settings.peers.empty());
    CHECK (osc::Value::float64 (again.settings.timeout) == osc::Value::float64 (0.1));
}

TEST_CASE ("pairing: no file is a machine that is not paired, and says nothing")
{
    TempFolder temp;
    const auto loaded = pair::load (temp.file ("pairing.xml"));

    CHECK (loaded.problem.empty());
    CHECK (loaded.settings.role == "none");
    CHECK (loaded.settings.peers.empty());
    CHECK (osc::Value::float64 (loaded.settings.timeout) == osc::Value::float64 (pair::defaultTimeout));
}

TEST_CASE ("pairing: a file that cannot be read is the defaults, with a sentence")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    TempFolder temp;
    const auto file = temp.file ("pairing.xml");

    for (const auto& contents : { std::string ("this is not XML <"),
                                  std::string ("<AudioDefaults enabled=\"true\"/>"),
                                  std::string ("") })
    {
        INFO ("file: " << contents);
        write (file, contents);

        const auto loaded = pair::load (file);
        CHECK_FALSE (loaded.problem.empty());
        CHECK (loaded.settings.role == "none");
        CHECK (loaded.settings.peers.empty());
        CHECK (osc::Value::float64 (loaded.settings.timeout) == osc::Value::float64 (pair::defaultTimeout));
    }
}

TEST_CASE ("pairing: a value that cannot be read is its default, the rest kept, with a sentence")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    TempFolder temp;
    const auto file = temp.file ("pairing.xml");

    SUBCASE ("a role nobody knows")
    {
        write (file, "<Pairing role=\"boss\" peers=\"10.0.0.2\" timeout=\"2\"/>");
        const auto loaded = pair::load (file);

        CHECK (loaded.problem.find ("boss") != std::string::npos);
        CHECK (loaded.settings.role == "none");
        CHECK (loaded.settings.peers == std::vector<std::string> { "10.0.0.2:5010" });
        CHECK (osc::Value::float64 (loaded.settings.timeout) == osc::Value::float64 (2.0));
    }

    SUBCASE ("an address that is not one")
    {
        write (file, "<Pairing role=\"backup\" peers=\"10.0.0.2 http://x/\" timeout=\"2\"/>");
        const auto loaded = pair::load (file);

        CHECK_FALSE (loaded.problem.empty());
        CHECK (loaded.settings.role == "backup");
        CHECK (loaded.settings.peers.empty());
    }

    SUBCASE ("a timeout with the French decimal comma, which no number in a Go.dot file has")
    {
        write (file, "<Pairing role=\"main\" timeout=\"1,5\"/>");
        const auto loaded = pair::load (file);

        CHECK_FALSE (loaded.problem.empty());
        CHECK (loaded.settings.role == "main");
        CHECK (osc::Value::float64 (loaded.settings.timeout) == osc::Value::float64 (pair::defaultTimeout));
    }

    SUBCASE ("a timeout out of range is brought into it")
    {
        write (file, "<Pairing role=\"main\" timeout=\"600\"/>");
        const auto loaded = pair::load (file);

        CHECK_FALSE (loaded.problem.empty());
        CHECK (osc::Value::float64 (loaded.settings.timeout) == osc::Value::float64 (pair::longestTimeout));

        write (file, "<Pairing role=\"main\" timeout=\"0\"/>");
        CHECK (osc::Value::float64 (pair::load (file).settings.timeout) == osc::Value::float64 (pair::shortestTimeout));
    }

    SUBCASE ("a value left out is its default, and says nothing")
    {
        write (file, "<Pairing role=\"backup\"/>");
        const auto loaded = pair::load (file);

        CHECK (loaded.problem.empty());
        CHECK (loaded.settings.role == "backup");
        CHECK (loaded.settings.peers.empty());
        CHECK (osc::Value::float64 (loaded.settings.timeout) == osc::Value::float64 (pair::defaultTimeout));
    }
}

TEST_CASE ("pairing: a peer is host:port, the port 5010 when it is left off")
{
    CHECK (pair::peerAddress ("10.0.0.2") == std::optional<std::string> ("10.0.0.2:5010"));
    CHECK (pair::peerAddress ("10.0.0.2:6010") == std::optional<std::string> ("10.0.0.2:6010"));
    CHECK (pair::peerAddress ("backup-mac.local") == std::optional<std::string> ("backup-mac.local:5010"));
    CHECK (pair::peerAddress ("[fe80::1]") == std::optional<std::string> ("[fe80::1]:5010"));
    CHECK (pair::peerAddress ("[fe80::1]:6010") == std::optional<std::string> ("[fe80::1]:6010"));
    CHECK (pair::peerAddress ("127.0.0.1:65535") == std::optional<std::string> ("127.0.0.1:65535"));

    for (const auto* bad : { "", ":5010", "10.0.0.2:", "10.0.0.2:0", "10.0.0.2:65536", "10.0.0.2:50a",
                             "10.0.0.2:+50", "fe80::1", "[fe80::1", "[]:5010", "[fe80::1]5010",
                             "http://10.0.0.2/", "a b", "caf\xc3\xa9.local" })
        CHECK_MESSAGE (! pair::peerAddress (bad).has_value(), "taken: \"" << bad << "\"");

    //  A list: spaces or commas between, each made host:port, a repeat kept once.
    CHECK (pair::peerAddresses ("10.0.0.2  10.1.0.2:6010")
             == std::optional<std::vector<std::string>> ({ "10.0.0.2:5010", "10.1.0.2:6010" }));
    CHECK (pair::peerAddresses ("10.0.0.2,10.1.0.2")
             == std::optional<std::vector<std::string>> ({ "10.0.0.2:5010", "10.1.0.2:5010" }));
    CHECK (pair::peerAddresses ("10.0.0.2 10.0.0.2:5010")
             == std::optional<std::vector<std::string>> ({ "10.0.0.2:5010" }));
    CHECK (pair::peerAddresses ("") == std::optional<std::vector<std::string>> (std::vector<std::string> {}));
    CHECK (pair::peerAddresses ("  ") == std::optional<std::vector<std::string>> (std::vector<std::string> {}));
    CHECK_FALSE (pair::peerAddresses ("10.0.0.2 nonsense:port").has_value());

    CHECK (pair::peersText ({ "10.0.0.2:5010", "10.1.0.2:6010" }) == "10.0.0.2:5010 10.1.0.2:6010");
    CHECK (pair::peersText ({}).empty());

    CHECK (pair::isRole ("main"));
    CHECK (pair::isRole ("backup"));
    CHECK (pair::isRole ("none"));
    CHECK_FALSE (pair::isRole ("Main"));
    CHECK_FALSE (pair::isRole (""));

    CHECK (pair::isTimeout (0.1));
    CHECK (pair::isTimeout (30.0));
    CHECK_FALSE (pair::isTimeout (0.05));
    CHECK_FALSE (pair::isTimeout (30.5));
    CHECK_FALSE (pair::isTimeout (std::numeric_limits<double>::quiet_NaN()));
    CHECK_FALSE (pair::isTimeout (std::numeric_limits<double>::infinity()));
}

TEST_CASE ("pairing: a file that cannot be written says why")
{
    TempFolder temp;

    //  Its folder would have to be made inside a file.
    const auto blocker = temp.file ("not-a-folder");
    write (blocker, "x");

    const auto inside = juce::File (juce::String (blocker)).getChildFile ("pairing.xml");
    const auto error = pair::save (inside.getFullPathName().toStdString(), pair::PairSettings {});
    CHECK_FALSE (error.empty());

    //  And settings no machine could hold are not written at all.
    pair::PairSettings nonsense;
    nonsense.role = "boss";
    CHECK_FALSE (pair::save (temp.file ("pairing.xml"), nonsense).empty());
    CHECK_FALSE (juce::File (juce::String (temp.file ("pairing.xml"))).exists());
}

//==============================================================================
TEST_CASE ("pairing: the three commands change the settings in force and ask for the file")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    Rig rig;
    const auto revision = rig.pairing.revision;

    REQUIRE (rig.apply (1, "pair.setRole", { text ("backup") }).applied == 1);
    CHECK (rig.pairing.inForce.role == "backup");
    CHECK (rig.pairing.stored.role == "backup");
    CHECK (rig.pairing.revision > revision);
    REQUIRE (rig.saves.size() == 1);
    CHECK (rig.saves.back().role == "backup");

    /*  THE ADDRESSES AS APPLIED, each made host:port: what the log holds, so a
        replay re-applies exactly what the session did. */
    REQUIRE (rig.apply (2, "pair.setPeers", { text ("10.0.0.2 10.1.0.2:6010") }).applied == 1);
    CHECK (rig.pairing.inForce.peers == std::vector<std::string> { "10.0.0.2:5010", "10.1.0.2:6010" });
    CHECK (rig.lastRecord().args == std::vector<osc::Value> { text ("10.0.0.2:5010 10.1.0.2:6010") });
    REQUIRE (rig.saves.size() == 2);
    CHECK (rig.saves.back().peers == rig.pairing.inForce.peers);

    /*  A FLOAT ON THE WIRE, A LEGIBLE NUMBER IN THE FILE: 0.1f promoted is
        0.10000000149011612, which nobody typed. */
    REQUIRE (rig.apply (3, "pair.setTimeout", { osc::Value::float32 (0.1f) }).applied == 1);
    CHECK (osc::Value::float64 (rig.pairing.inForce.timeout) == osc::Value::float64 (0.1));
    REQUIRE (rig.apply (4, "pair.setTimeout", { osc::Value::float32 (2.5f) }).applied == 1);
    CHECK (osc::Value::float64 (rig.pairing.inForce.timeout) == osc::Value::float64 (2.5));
    REQUIRE (rig.saves.size() == 4);
    CHECK (osc::Value::float64 (rig.saves.back().timeout) == osc::Value::float64 (2.5));

    //  An integer is coerced, as for every f parameter.
    REQUIRE (rig.apply (5, "pair.setTimeout", { osc::Value::int32 (3) }).applied == 1);
    CHECK (osc::Value::float64 (rig.pairing.inForce.timeout) == osc::Value::float64 (3.0));

    //  And no peer at all is a setting too.
    REQUIRE (rig.apply (6, "pair.setPeers", { text ("") }).applied == 1);
    CHECK (rig.pairing.inForce.peers.empty());

    /*  WITH NOBODY TO WRITE THE FILE - a replay - a command still applies. */
    rig.pairing.requestSave = nullptr;
    REQUIRE (rig.apply (7, "pair.setRole", { text ("main") }).applied == 1);
    CHECK (rig.pairing.inForce.role == "main");
}

TEST_CASE ("pairing: a bad role, address or timeout is refused bad-value and changes nothing")
{
    Rig rig;
    REQUIRE (rig.apply (1, "pair.setRole", { text ("backup") }).applied == 1);
    REQUIRE (rig.apply (2, "pair.setPeers", { text ("10.0.0.2") }).applied == 1);
    REQUIRE (rig.saves.size() == 2);
    const auto revision = rig.pairing.revision;

    const auto refused = [&rig] (std::int64_t tick, const std::string& command, osc::Value value)
    {
        const auto result = rig.apply (tick, command, { std::move (value) });
        CHECK (result.applied == 0);
        CHECK (rig.lastRecord().reason == std::string (reason::badValue));
    };

    refused (3, "pair.setRole", text ("boss"));
    refused (4, "pair.setRole", text (""));
    refused (5, "pair.setPeers", text ("10.0.0.3 10.0.0.4:99999"));
    refused (6, "pair.setPeers", text ("fe80::1"));
    refused (7, "pair.setTimeout", osc::Value::float32 (0.0f));
    refused (8, "pair.setTimeout", osc::Value::float32 (31.0f));
    refused (9, "pair.setTimeout", osc::Value::float32 (-1.0f));

    CHECK (rig.pairing.inForce.role == "backup");
    CHECK (rig.pairing.inForce.peers == std::vector<std::string> { "10.0.0.2:5010" });
    CHECK (osc::Value::float64 (rig.pairing.inForce.timeout) == osc::Value::float64 (pair::defaultTimeout));
    CHECK (rig.pairing.revision == revision);
    CHECK (rig.saves.size() == 2);
}

TEST_CASE ("pairing: a command-line override is never written into the file by a command")
{
    /*  As serve starts a machine `--role=backup --link-timeout=2.5` whose file
        says it is not paired: the overrides are in force, and the file is the
        file's. */
    Rig rig;
    rig.pairing.inForce.role = "backup";
    rig.pairing.inForce.timeout = 2.5;

    //  Setting something else writes the file's own role and timeout.
    REQUIRE (rig.apply (1, "pair.setPeers", { text ("10.0.0.2") }).applied == 1);
    REQUIRE (rig.saves.size() == 1);
    CHECK (rig.saves.back().role == "none");
    CHECK (osc::Value::float64 (rig.saves.back().timeout) == osc::Value::float64 (pair::defaultTimeout));
    CHECK (rig.saves.back().peers == std::vector<std::string> { "10.0.0.2:5010" });

    //  And the overrides stay in force for this run.
    CHECK (rig.pairing.inForce.role == "backup");
    CHECK (osc::Value::float64 (rig.pairing.inForce.timeout) == osc::Value::float64 (2.5));
    CHECK (rig.pairing.inForce.peers == std::vector<std::string> { "10.0.0.2:5010" });

    //  Setting the overridden one itself is the later word, in force and in the file.
    REQUIRE (rig.apply (2, "pair.setRole", { text ("main") }).applied == 1);
    CHECK (rig.pairing.inForce.role == "main");
    REQUIRE (rig.saves.size() == 2);
    CHECK (rig.saves.back().role == "main");
}

TEST_CASE ("pairing: the commands are this machine's - never mirrored, no step of undo, not refused locked")
{
    Rig rig;

    for (const auto* name : { "pair.setRole", "pair.setPeers", "pair.setTimeout", "pair.saved" })
    {
        const auto* command = rig.engine.commands().find (name);
        REQUIRE_MESSAGE (command != nullptr, "not registered: " << name);
        CHECK_MESSAGE (command->machineLocal, name);
        CHECK_MESSAGE (pair::isMachineLocal (*command, {}), name);
    }

    REQUIRE (rig.apply (1, "list.create", { text ("Main"), text (mainList) }).applied == 1);
    REQUIRE (rig.undoName() == "list.create");

    //  NO STEP: the history is the list's alone, before and after.
    REQUIRE (rig.apply (2, "pair.setRole", { text ("main") }).applied == 1);
    REQUIRE (rig.apply (3, "pair.setPeers", { text ("10.0.0.2") }).applied == 1);
    REQUIRE (rig.apply (4, "pair.setTimeout", { osc::Value::float32 (2.0f) }).applied == 1);
    REQUIRE (rig.apply (5, "pair.saved", { text ("") }, std::string (origin::engine)).applied == 1);
    CHECK (rig.undoName() == "list.create");

    REQUIRE (rig.document.undo (doc::UndoDomain::document) == std::string ("list.create"));
    CHECK_FALSE (rig.document.undo (doc::UndoDomain::document).has_value());
    CHECK (rig.pairing.inForce.role == "main");

    /*  THE LOCK IS THE SHOW'S, and these are the machine's: the show refuses
        an edit, and the role is still set. */
    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
    CHECK (rig.apply (6, "list.create", { text ("Second") }).applied == 0);
    CHECK (rig.lastRecord().reason == std::string (reason::locked));

    REQUIRE (rig.apply (7, "pair.setRole", { text ("backup") }).applied == 1);
    REQUIRE (rig.apply (8, "pair.setPeers", { text ("10.0.0.3") }).applied == 1);
    REQUIRE (rig.apply (9, "pair.setTimeout", { osc::Value::float32 (1.5f) }).applied == 1);
    CHECK (rig.pairing.inForce.role == "backup");
    CHECK (rig.pairing.inForce.peers == std::vector<std::string> { "10.0.0.3:5010" });
}

TEST_CASE ("pairing: neither an audio outage nor an audio restart refuses them")
{
    /*  A main whose interface went is the moment somebody reaches for the
        pairing, not a reason to refuse it; and a file written during an
        outage must still be heard to have been written. */
    Rig rig;

    rig.audioState.status = "noClock";
    CHECK (rig.apply (1, "list.create", { text ("Main"), text (mainList) }).applied == 0);
    CHECK (rig.lastRecord().reason == std::string ("audio-reconnecting"));

    CHECK (rig.apply (2, "pair.setRole", { text ("main") }).applied == 1);
    CHECK (rig.apply (3, "pair.setPeers", { text ("10.0.0.2") }).applied == 1);
    CHECK (rig.apply (4, "pair.setTimeout", { osc::Value::float32 (2.0f) }).applied == 1);
    CHECK (rig.apply (5, "pair.saved", { text ("") }, std::string (origin::engine)).applied == 1);

    rig.audioState.status = "running";
    rig.audioState.settingsStatus = "applying";
    CHECK (rig.apply (6, "pair.setRole", { text ("backup") }).applied == 1);
    CHECK (rig.apply (7, "pair.saved", { text ("") }, std::string (origin::engine)).applied == 1);
    CHECK (rig.pairing.inForce.role == "backup");
}

TEST_CASE ("pairing: a write that failed is said at lastError, write-failed, with its reason")
{
    Rig rig;

    REQUIRE (rig.apply (1, "pair.saved", { text ("") }, std::string (origin::engine)).applied == 1);
    CHECK (rig.engine.lastError().empty());

    const auto failed = rig.apply (2, "pair.saved", { text ("Could not write pairing.xml in D:/nowhere") },
                                   std::string (origin::engine));
    CHECK (failed.applied == 0);

    const auto record = rig.lastRecord();
    CHECK (record.reason == std::string (reason::writeFailed));
    CHECK (record.args == std::vector<osc::Value> { text ("Could not write pairing.xml in D:/nowhere") });

    const auto said = rig.engine.lastError();
    CHECK (said.find ("write-failed pair.saved Could not write pairing.xml") != std::string::npos);
}
