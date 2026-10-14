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
    THE SAME SHOW, CHECKED (namespace draft §58, RD.10; the author's AFU, and
    AFY).

    A main and its backup each open their own copy of a show folder, and the
    link carries what the main applies, never the show. These cases hold the
    pieces that say whether the two copies are one show: the fingerprint, which
    hashes what someone decided and nothing of the machine; the units, which
    name what differs; the media list, which names a sound missing or changed;
    the comparison and its questions and answers between two machines; and
    `pair.adoptShow`, which takes the main's show on the backup, keeping the
    backup's own interface. Run in C and in fr_FR, as every suite is: a number
    the fingerprint spelled the French way would make two machines differ.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/command/Command.h>
#include <wfg/engine/command/Event.h>
#include <wfg/engine/cue/UndoStep.h>
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/DocumentSession.h>
#include <wfg/engine/document/DocumentWriter.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/document/ShowFingerprint.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/log/Replay.h>
#include <wfg/engine/osc/OscCodec.h>
#include <wfg/engine/pair/Leadership.h>
#include <wfg/engine/pair/MachineLocal.h>
#include <wfg/engine/pair/PairSettings.h>
#include <wfg/engine/pair/PeerLink.h>
#include <wfg/engine/pair/ShowCheck.h>
#include <wfg/engine/pair/Summary.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace wfg;

namespace
{
    using namespace std::chrono_literals;

    osc::Value s (std::string text) { return osc::Value::string (std::move (text)); }

    /*  THE MAIN'S SHOW: a list of three cues, a bus, a port - and the main's
        own interface and MIDI output, which are this machine's rows. Written
        canonically. */
    const std::string mainShow =
        "<Show>\n"
        "  <Lists goDebounce=\"0\">\n"
        "    <List id=\"7K2QM9X4\" name=\"Show\">\n"
        "      <Media id=\"B3N8R5TW\" file=\"tone.wav\" name=\"Thunder\" number=\"1\">\n"
        "        <Route id=\"Z04EH7PH\" bus=\"J3MT5XYA\" gains=\"1 0.5\"/>\n"
        "      </Media>\n"
        "      <Media id=\"P9XKC2WR\" file=\"rain.wav\" level=\"-6.5\" name=\"Rain\" number=\"4\">\n"
        "        <Route id=\"V3XY5Z7B\" bus=\"J3MT5XYA\" gains=\"0 1\"/>\n"
        "      </Media>\n"
        "      <Cue id=\"M5TQ7XVA\" name=\"And on\" number=\"5\"/>\n"
        "    </List>\n"
        "  </Lists>\n"
        "  <Mounts/>\n"
        "  <Audio outputDevice=\"Main Interface\" tracks=\"2\">\n"
        "    <Bus id=\"J3MT5XYA\" name=\"Main L/R\" width=\"2\"/>\n"
        "  </Audio>\n"
        "  <MidiPorts>\n"
        "    <Port id=\"C6P4M2HQ\" name=\"Lights\" outputDevice=\"Main MIDI\"/>\n"
        "  </MidiPorts>\n"
        "  <Network/>\n"
        "  <Surfaces/>\n"
        "  <Dcas/>\n"
        "</Show>\n";

    /*  THE SAME SHOW AS THE BACKUP HOLDS IT: its own interface and MIDI
        output, and written by hand - attributes in another order, the
        number a decimal any locale must read the same. */
    const std::string backupShow =
        "<Show>\n"
        "  <Lists goDebounce=\"0\">\n"
        "    <List name=\"Show\" id=\"7K2QM9X4\">\n"
        "      <Media number=\"1\" name=\"Thunder\" file=\"tone.wav\" id=\"B3N8R5TW\">\n"
        "        <Route gains=\"1 0.5\" bus=\"J3MT5XYA\" id=\"Z04EH7PH\"/>\n"
        "      </Media>\n"
        "      <Media name=\"Rain\" level=\"-6.50\" number=\"4\" file=\"rain.wav\" id=\"P9XKC2WR\">\n"
        "        <Route id=\"V3XY5Z7B\" gains=\"0 1\" bus=\"J3MT5XYA\"/>\n"
        "      </Media>\n"
        "      <Cue number=\"5\" name=\"And on\" id=\"M5TQ7XVA\"/>\n"
        "    </List>\n"
        "  </Lists>\n"
        "  <Mounts/>\n"
        "  <Audio tracks=\"2\" outputDevice=\"Backup Interface\">\n"
        "    <Bus width=\"2\" name=\"Main L/R\" id=\"J3MT5XYA\"/>\n"
        "  </Audio>\n"
        "  <MidiPorts>\n"
        "    <Port outputDevice=\"Backup MIDI\" name=\"Lights\" id=\"C6P4M2HQ\"/>\n"
        "  </MidiPorts>\n"
        "  <Network/>\n"
        "  <Surfaces/>\n"
        "  <Dcas/>\n"
        "</Show>\n";

    void read (const std::string& text, doc::ShowDocument& into)
    {
        const auto result = doc::CanonicalXml::read (text, into);
        INFO ((result.problems.empty() ? std::string() : result.problems.front()));
        REQUIRE (result.ok);
    }

    doc::ShowFingerprint printOf (const doc::ShowDocument& document, const juce::File& bundle = juce::File(),
                                  const std::string& mediaFolder = std::string())
    {
        return doc::fingerprintShow (document.root(), document.showRevision(), bundle, mediaFolder, nullptr);
    }

    void edit (doc::ShowDocument& document, const std::string& address, const std::string& value)
    {
        const auto done = document.setAttribute (address, value);
        INFO (address << " = " << value);
        REQUIRE (done.ok);
    }

    /*  A FOLDER OF THE TESTS' OWN, gone afterwards. */
    struct Scratch
    {
        Scratch()
        {
            folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("wfg-tests")
                       .getChildFile (juce::Uuid().toDashedString());
            folder.createDirectory();
        }

        ~Scratch() { folder.deleteRecursively(); }

        Scratch (const Scratch&) = delete;
        Scratch& operator= (const Scratch&) = delete;

        juce::File folder;
    };

    void writeFile (const juce::File& file, const std::string& bytes)
    {
        file.getParentDirectory().createDirectory();
        REQUIRE (file.replaceWithData (bytes.data(), bytes.size()));
    }

    std::string bytesOf (const juce::File& file)
    {
        juce::MemoryBlock block;
        REQUIRE_MESSAGE (file.loadFileAsData (block), "cannot read " << file.getFullPathName());
        return std::string (static_cast<const char*> (block.getData()), block.getSize());
    }

    template <typename Predicate>
    bool waitFor (Predicate predicate, std::chrono::milliseconds most = 10s)
    {
        const auto until = std::chrono::steady_clock::now() + most;

        while (std::chrono::steady_clock::now() < until)
        {
            if (predicate())
                return true;

            std::this_thread::sleep_for (10ms);
        }

        return predicate();
    }
}

//==============================================================================
TEST_CASE ("same show: the fingerprint is what someone decided - machine rows, the standby, attribute order and the locale change nothing; any show edit moves it")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    doc::ShowDocument main, backup;
    read (mainShow, main);
    read (backupShow, backup);

    const auto mainPrint = printOf (main);
    const auto backupPrint = printOf (backup);

    //  ONE SHOW: different interfaces, different MIDI outputs, written in another order.
    CHECK (mainPrint.fingerprint.size() == 64u);
    CHECK (mainPrint.fingerprint == backupPrint.fingerprint);
    CHECK (mainPrint.decided == backupPrint.decided);
    CHECK (mainPrint.units == backupPrint.units);

    /*  WHAT IT HASHES: the show as the file spells it, its decimal written with
        a point whichever locale this runs in, and nothing of this machine. */
    CHECK (mainPrint.decided.find ("level=\"-6.5\"") != std::string::npos);
    CHECK (mainPrint.decided.find ("Main Interface") == std::string::npos);
    CHECK (mainPrint.decided.find ("Main MIDI") == std::string::npos);
    CHECK (mainPrint.decided.find ("tracks=\"2\"") != std::string::npos);
    CHECK (mainPrint.fingerprint == doc::fingerprintOf (mainPrint.decided, {}));

    //  show.xml as written still holds the machine's rows: only the fingerprint leaves them out.
    CHECK (doc::CanonicalXml::write (main).find ("Main Interface") != std::string::npos);

    //  THE STANDBY IS STATE, and state.xml is not the show: a GO moves nothing here.
    edit (main, "/godot/list/7K2QM9X4/standby", "P9XKC2WR");
    CHECK (printOf (main).fingerprint == mainPrint.fingerprint);

    //  A MACHINE ROW CHANGED moves nothing either.
    edit (main, "/godot/audio/outputDevice", "Another Interface");
    edit (main, "/godot/port/C6P4M2HQ/outputDevice", "Another MIDI");
    CHECK (printOf (main).fingerprint == mainPrint.fingerprint);

    //  ANY SHOW EDIT MOVES IT: a cue's level, a list's setting, a bus's name, each back again.
    const auto moves = [&main, &mainPrint] (const std::string& address, const std::string& changed, const std::string& back)
    {
        edit (main, address, changed);
        CHECK_MESSAGE (printOf (main).fingerprint != mainPrint.fingerprint, address);
        edit (main, address, back);
        CHECK_MESSAGE (printOf (main).fingerprint == mainPrint.fingerprint, address);
    };

    moves ("/godot/cue/P9XKC2WR/level", "-12", "-6.5");
    moves ("/godot/cue/B3N8R5TW/name", "Thunder clap", "Thunder");
    moves ("/godot/list/goDebounce", "0.5", "0");
    moves ("/godot/bus/J3MT5XYA/name", "Front", "Main L/R");

    //  A DEVICE DESCRIPTION is part of it, as the log header's hash has it: one more, or one changed, moves it.
    Scratch scratch;
    writeFile (scratch.folder.getChildFile ("namespaces").getChildFile ("desk.json"), "{ \"FULL_PATH\": \"/\" }");
    const auto withDesk = printOf (main, scratch.folder);
    CHECK (withDesk.fingerprint != mainPrint.fingerprint);
    REQUIRE (withDesk.descriptions.size() == 1u);
    CHECK (withDesk.descriptions[0].name == "desk.json");

    writeFile (scratch.folder.getChildFile ("namespaces").getChildFile ("desk.json"), "{ \"FULL_PATH\": \"/desk\" }");
    CHECK (printOf (main, scratch.folder).fingerprint != withDesk.fingerprint);
}

//==============================================================================
TEST_CASE ("same show: two shows a cue's level apart name that cue; cues added, removed and moved, and settings, named too")
{
    doc::ShowDocument main, backup;
    read (mainShow, main);
    read (backupShow, backup);

    //  The same: nothing to say.
    CHECK (pair::showDifferenceOf (printOf (main).units, printOf (backup).units).empty());

    //  ONE CUE'S LEVEL, edited on the main alone.
    edit (main, "/godot/cue/P9XKC2WR/level", "-12");
    CHECK (pair::showDifferenceOf (printOf (main).units, printOf (backup).units)
           == "the show differs: cue 4 Rain changed");

    //  A route of the cue is the cue's: its gains changed is the cue changed.
    edit (main, "/godot/cue/P9XKC2WR/level", "-6.5");
    edit (main, "/godot/route/Z04EH7PH/gains", "1 1");
    CHECK (pair::showDifferenceOf (printOf (main).units, printOf (backup).units)
           == "the show differs: cue 1 Thunder changed");
    edit (main, "/godot/route/Z04EH7PH/gains", "1 0.5");

    //  A cue the main has and the backup does not, and a setting.
    const auto made = main.createCue ("7K2QM9X4", 3, "memo", "Bells");
    REQUIRE (made.ok);
    edit (main, "/godot/cue/" + made.id + "/number", "6");
    edit (main, "/godot/bus/J3MT5XYA/name", "Front");
    edit (main, "/godot/cue/P9XKC2WR/level", "-12");

    CHECK (pair::showDifferenceOf (printOf (main).units, printOf (backup).units)
           == "the show differs: 1 cue changed (4 Rain), 1 added (6 Bells), bus Front changed");

    //  Seen from the other side: removed, and the setting by the name it has there.
    CHECK (pair::showDifferenceOf (printOf (backup).units, printOf (main).units)
           == "the show differs: 1 cue changed (4 Rain), 1 removed (6 Bells), bus Main L/R changed");

    //  CUES MOVED: the same cues in another order, nothing else.
    doc::ShowDocument moved;
    read (mainShow, moved);
    REQUIRE (moved.move ("M5TQ7XVA", "7K2QM9X4", 0).ok);
    CHECK (pair::showDifferenceOf (printOf (moved).units, printOf (backup).units)
           == "the show differs: cues moved in list Show");

    //  A DEVICE DESCRIPTION: its own words.
    Scratch scratch;
    writeFile (scratch.folder.getChildFile ("namespaces").getChildFile ("desk.json"), "{}");
    doc::ShowDocument plain;
    read (mainShow, plain);
    CHECK (pair::showDifferenceOf (printOf (plain, scratch.folder).units, printOf (plain).units)
           == "the show differs: the device description desk.json differs");
}

//==============================================================================
TEST_CASE ("same show: the media list names a file missing here, one missing on the main, and one that differs")
{
    Scratch mainSide, backupSide;
    const auto mainMedia = mainSide.folder.getChildFile ("media");
    const auto backupMedia = backupSide.folder.getChildFile ("media");

    //  tone.wav on both, the same; rain.wav on the main alone.
    writeFile (mainMedia.getChildFile ("tone.wav"), std::string (1000, 't'));
    writeFile (backupMedia.getChildFile ("tone.wav"), std::string (1000, 't'));
    writeFile (mainMedia.getChildFile ("rain.wav"), std::string (2000, 'r'));

    const auto stamp = juce::Time::getCurrentTime() - juce::RelativeTime::hours (1);
    REQUIRE (mainMedia.getChildFile ("tone.wav").setLastModificationTime (stamp));
    REQUIRE (backupMedia.getChildFile ("tone.wav").setLastModificationTime (stamp));

    doc::ShowDocument show;
    read (mainShow, show);

    auto theirs = doc::mediaListOf (show.root(), mainMedia.getFullPathName().toStdString(), nullptr);
    auto ours = doc::mediaListOf (show.root(), backupMedia.getFullPathName().toStdString(), nullptr);

    REQUIRE (theirs.size() == 2u);
    CHECK (theirs[0].path == "rain.wav");          // sorted by path
    CHECK (theirs[0].kind == "sound");
    CHECK (theirs[0].present);
    CHECK (theirs[0].size == 2000);
    CHECK_FALSE (ours[0].present);
    CHECK (theirs[1].path == "tone.wav");

    CHECK (doc::mediaDigestOf (theirs) != doc::mediaDigestOf (ours));
    CHECK (pair::mediaDifferenceOf (theirs, ours, "the main") == "1 sound missing here: rain.wav");
    CHECK (pair::mediaDifferenceOf (ours, theirs, "the main") == "1 sound missing on the main: rain.wav");

    //  THE SAME FILE, the same size, a second apart (a copy onto a disk that keeps two): the same.
    ours[1].modified = theirs[1].modified + 1000;
    CHECK (pair::mediaDifferenceOf (theirs, ours, "the main") == "1 sound missing here: rain.wav");

    //  ANOTHER SIZE: it differs.
    writeFile (backupMedia.getChildFile ("tone.wav"), std::string (1200, 't'));
    ours = doc::mediaListOf (show.root(), backupMedia.getFullPathName().toStdString(), nullptr);
    CHECK (pair::mediaDifferenceOf (theirs, ours, "the main")
           == "1 sound missing here: rain.wav; 1 sound differs: tone.wav");

    /*  THE ANALYSERS' HASHES DECIDE WHEN BOTH HAVE ONE: the same bytes written
        at other times are the same; other bytes the same size and date are not. */
    auto a = theirs[1], b = theirs[1];
    a.hash = "1111";
    b.hash = "1111";
    b.modified += 3600 * 1000;
    CHECK (pair::mediaDifferenceOf ({ a }, { b }, "the main").empty());

    b = theirs[1];
    a.hash = "1111";
    b.hash = "2222";
    CHECK (pair::mediaDifferenceOf ({ a }, { b }, "the main") == "1 sound differs: tone.wav");

    //  One hash only: by size and date, as when neither has one.
    b.hash.clear();
    CHECK (pair::mediaDifferenceOf ({ a }, { b }, "the main").empty());

    //  A FILE ONE SHOW NAMES AND THE OTHER DOES NOT is the show's difference, not the media's.
    doc::MediaEntry stray;
    stray.path = "bells.wav";
    stray.kind = "sound";
    stray.present = true;
    CHECK (pair::mediaDifferenceOf ({ stray }, {}, "the main").empty());

    //  And a picture, and two of them: the word follows.
    doc::MediaEntry still, still2;
    still.path = "slide.png";
    still.kind = "picture";
    still.present = true;
    still2 = still;
    still2.path = "slide2.png";
    auto stillHere = still, still2Here = still2;
    stillHere.present = false;
    still2Here.present = false;
    CHECK (pair::mediaDifferenceOf ({ still, still2 }, { stillHere, still2Here }, "the backup")
           == "2 pictures missing here: slide.png, slide2.png");
}

//==============================================================================
TEST_CASE ("same show: taking the main's show keeps this machine's rows and takes everything else, its own files set aside")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    //  THE MAIN'S SHOW, edited while the backup was away: a level, a name, a port added.
    doc::ShowDocument main;
    read (mainShow, main);
    edit (main, "/godot/cue/P9XKC2WR/level", "-12");
    edit (main, "/godot/cue/B3N8R5TW/name", "Thunder clap");
    const auto port = main.createPort ("Sound");
    REQUIRE (port.ok);
    edit (main, "/godot/port/" + port.id + "/outputDevice", "Main Sound MIDI");

    const auto mainPrint = printOf (main);

    //  THE MERGE, ON ITS OWN.
    {
        doc::ShowDocument backup, taken;
        read (backupShow, backup);
        read (mainPrint.decided, taken);

        doc::putMachineRowsBack (taken.root(), backup.root());

        CHECK (taken.getAttribute ("/godot/audio/outputDevice") == std::optional<std::string> ("Backup Interface"));
        CHECK (taken.getAttribute ("/godot/port/C6P4M2HQ/outputDevice") == std::optional<std::string> ("Backup MIDI"));

        //  A port the backup never had is bound to no device here: a machine row never travels.
        CHECK (taken.getAttribute ("/godot/port/" + port.id + "/outputDevice") == std::optional<std::string> (""));

        //  Everything else is the main's.
        CHECK (taken.getAttribute ("/godot/cue/P9XKC2WR/level") == std::optional<std::string> ("-12"));
        CHECK (taken.getAttribute ("/godot/cue/B3N8R5TW/name") == std::optional<std::string> ("Thunder clap"));
        CHECK (printOf (taken).fingerprint == mainPrint.fingerprint);
    }

    //  THE ACT, ON A BUNDLE: the backup's folder, its own description and one the main has not.
    Scratch scratch;
    const auto folder = scratch.folder.getChildFile ("Show");

    doc::ShowDocument backup;
    read (backupShow, backup);
    REQUIRE (doc::Bundle::save (folder, backup).ok);
    writeFile (doc::Bundle::namespacesFolder (folder).getChildFile ("desk.json"), "{ \"backup's\": 1 }");
    writeFile (doc::Bundle::namespacesFolder (folder).getChildFile ("old.json"), "{}");
    writeFile (folder.getChildFile ("media").getChildFile ("tone.wav"), "not a sound");

    const auto ownShowBytes = bytesOf (doc::Bundle::showFile (folder));

    doc::DocumentSession session { folder, backup.showRevision() };
    doc::DocumentWriter writer;

    //  An edit of the backup's own, which goes with the show it was about.
    REQUIRE (backup.setAttribute ("/godot/cue/M5TQ7XVA/name", "And so on").ok);

    const std::vector<doc::Description> descriptions { { "desk.json", "{ \"main's\": 1 }" }, { "wfs.json", "{ \"wfs\": 1 }" } };

    std::string aside;
    const auto taken = doc::Bundle::adoptShow (backup, session, writer, mainPrint.decided, descriptions,
                                               "set-aside.2026-10-14.21-30-05", aside);
    INFO ((taken.problems.empty() ? std::string() : taken.problems.front()));
    REQUIRE (taken.ok);
    CHECK (aside == "set-aside.2026-10-14.21-30-05");

    //  THE SHOW IS THE MAIN'S, with this machine's rows.
    CHECK (backup.getAttribute ("/godot/cue/P9XKC2WR/level") == std::optional<std::string> ("-12"));
    CHECK (backup.getAttribute ("/godot/cue/B3N8R5TW/name") == std::optional<std::string> ("Thunder clap"));
    CHECK (backup.getAttribute ("/godot/cue/M5TQ7XVA/name") == std::optional<std::string> ("And on"));
    CHECK (backup.getAttribute ("/godot/audio/outputDevice") == std::optional<std::string> ("Backup Interface"));
    CHECK (backup.getAttribute ("/godot/port/C6P4M2HQ/outputDevice") == std::optional<std::string> ("Backup MIDI"));
    CHECK (backup.findById (port.id).isValid());
    CHECK (printOf (backup, folder).fingerprint == doc::fingerprintOf (mainPrint.decided, descriptions));

    //  ON THE DISK: show.xml is the document, the descriptions are the main's alone, the sounds untouched.
    CHECK (bytesOf (doc::Bundle::showFile (folder)) == doc::CanonicalXml::write (backup));
    CHECK (bytesOf (doc::Bundle::namespacesFolder (folder).getChildFile ("desk.json")) == "{ \"main's\": 1 }");
    CHECK (bytesOf (doc::Bundle::namespacesFolder (folder).getChildFile ("wfs.json")) == "{ \"wfs\": 1 }");
    CHECK_FALSE (doc::Bundle::namespacesFolder (folder).getChildFile ("old.json").exists());
    CHECK (bytesOf (folder.getChildFile ("media").getChildFile ("tone.wav")) == "not a sound");

    //  ITS OWN FILES SET ASIDE, never deleted.
    const auto setAside = folder.getChildFile (aside);
    CHECK (bytesOf (setAside.getChildFile ("show.xml")) == ownShowBytes);
    CHECK (bytesOf (setAside.getChildFile ("namespaces").getChildFile ("desk.json")) == "{ \"backup's\": 1 }");
    CHECK (setAside.getChildFile ("namespaces").getChildFile ("old.json").existsAsFile());

    //  THE HISTORY WENT WITH THE SHOW (AFY), and the folder holds this show: the dot is out.
    CHECK_FALSE (backup.history (doc::UndoDomain::document).canUndo());
    CHECK_FALSE (doc::isDirty (backup, session));

    //  The log header's hash reads none of the set-aside folder: the bundle opens and hashes as one show.
    doc::ShowDocument reopened;
    REQUIRE (doc::Bundle::open (folder, reopened).ok);
    CHECK (doc::CanonicalXml::write (reopened) == doc::CanonicalXml::write (backup));

    //  TAKEN AGAIN IN THE SAME SECOND: a folder of its own.
    std::string again;
    REQUIRE (doc::Bundle::adoptShow (backup, session, writer, mainPrint.decided, descriptions,
                                     "set-aside.2026-10-14.21-30-05", again).ok);
    CHECK (again == "set-aside.2026-10-14.21-30-05.2");

    //  A TEXT THAT DOES NOT READ touches nothing.
    const auto before = bytesOf (doc::Bundle::showFile (folder));
    std::string none;
    const auto refused = doc::Bundle::adoptShow (backup, session, writer, "<Show><Nonsense/></Show>", {}, "set-aside.x",
                                                 none);
    CHECK_FALSE (refused.ok);
    CHECK (none.empty());
    CHECK_FALSE (folder.getChildFile ("set-aside.x").exists());
    CHECK (bytesOf (doc::Bundle::showFile (folder)) == before);

    //  NOR A DESCRIPTION NAMED TO LEAVE namespaces/.
    CHECK_FALSE (doc::Bundle::adoptShow (backup, session, writer, mainPrint.decided, { { "../show.xml", "x" } },
                                         "set-aside.y", none).ok);
    CHECK_FALSE (folder.getChildFile ("set-aside.y").exists());
}

//==============================================================================
namespace
{
    /*  AN ENGINE WITH THE PAIRING'S COMMANDS AND A SHOW, the undo rule serve's. */
    struct PairRig
    {
        PairRig()
        {
            engine.log().openInMemory ({});
            read (mainShow, document);
            doc::registerDocumentCommands (engine.commands(), document);
            pair::registerPairCommands (engine.commands(), pairing);

            engine.setBeforeApply ([this] (const Command& command, const Event& event,
                                           const std::vector<osc::Value>& args, std::int64_t tick)
                                   { cue::beginUndoStep (document, nullptr, command, event, args, tick); });
        }

        LogRecord apply (std::int64_t tick, const std::string& command, std::vector<osc::Value> args = {},
                         const std::string& from = std::string (origin::cli))
        {
            REQUIRE (engine.submit (from, command, std::move (args)));
            engine.processTick (tick);

            const auto parsed = LogFile::parse (engine.log().contents());
            REQUIRE (! parsed.records.empty());
            return parsed.records.back();
        }

        LogRecord seat (std::int64_t tick, const char* command, const char* seatWord, const char* why)
        {
            return apply (tick, command, { s ("backup"), s (seatWord), s (why) }, std::string (origin::engine));
        }

        Engine engine;
        doc::ShowDocument document;
        pair::Pairing pairing;
    };
}

TEST_CASE ("same show: pair.adoptShow is refused on a machine that sends and while anything runs here, and taken as one record when following")
{
    PairRig rig;

    const auto* command = rig.engine.commands().find ("pair.adoptShow");
    REQUIRE (command != nullptr);
    CHECK (pair::isMachineLocal (*command, {}));

    auto running = false;
    auto acts = 0;
    rig.pairing.busyNow = [&running] { return running; };

    //  NOT PAIRED, AND SENDING: refused, nothing done.
    CHECK (rig.apply (1, "pair.adoptShow").reason == "not-following");

    REQUIRE (rig.seat (2, "pair.lead", "leading", "follower-linked").kind == LogRecord::Kind::applied);
    CHECK (rig.apply (3, "pair.adoptShow").reason == "not-following");

    //  WAITING FOR ITS MAIN: no leader whose show to take.
    REQUIRE (rig.seat (4, "pair.hold", "waiting", "role").kind == LogRecord::Kind::applied);
    CHECK (rig.apply (5, "pair.adoptShow").reason == "not-following");

    //  FOLLOWING, A CUE RUNNING HERE: busy.
    REQUIRE (rig.seat (6, "pair.hold", "following", "main-found").kind == LogRecord::Kind::applied);
    running = true;
    CHECK (rig.apply (7, "pair.adoptShow").reason == "busy");
    running = false;

    //  NOTHING TO TAKE: not fetched.
    CHECK (rig.apply (8, "pair.adoptShow").reason == "not-fetched");
    CHECK_FALSE (rig.pairing.realignAsked);

    //  SERVE'S ACT: applied as one record, its arguments the fingerprint and the folder, and seated again after.
    rig.pairing.adoptShow = [&acts] (CommandContext&)
    {
        ++acts;
        return Outcome::ok ({ s ("ab12"), s ("set-aside.2026-10-14.21-30-05") });
    };

    const auto record = rig.apply (9, "pair.adoptShow");
    REQUIRE (record.kind == LogRecord::Kind::applied);
    CHECK (record.args == std::vector<osc::Value> { s ("ab12"), s ("set-aside.2026-10-14.21-30-05") });
    CHECK (acts == 1);
    CHECK (rig.pairing.realignAsked);

    //  The act refusing - nothing to take after all - is the record's refusal, and no seat is asked.
    rig.pairing.realignAsked = false;
    rig.pairing.adoptShow = [] (CommandContext&) { return Outcome::rejected (reason::notFetched); };
    CHECK (rig.apply (10, "pair.adoptShow").reason == "not-fetched");
    CHECK_FALSE (rig.pairing.realignAsked);

    //  AND A REPLAY STOPS AT THE ONE THAT TOOK A SHOW: what it ran from then on came down the link.
    const auto boundary = doc::Bundle::replayBoundary (LogFile::parse (rig.engine.log().contents()));
    const auto records = LogFile::parse (rig.engine.log().contents()).records;
    REQUIRE (boundary.replayable < records.size());
    CHECK (records[boundary.replayable].command == "pair.adoptShow");
    CHECK (records[boundary.replayable].kind == LogRecord::Kind::applied);
    CHECK (boundary.refusal.find ("took the other machine's show") != std::string::npos);
}

TEST_CASE ("same show: the backup's history starts at the link - a follow that begins forgets this machine's steps, and a replay forgets them alike (AFY)")
{
    PairRig rig;
    rig.pairing.followBegins = [&rig] { rig.document.clearHistory(); };

    //  An edit of the backup's own while it waited for its main.
    REQUIRE (rig.seat (1, "pair.hold", "waiting", "started").kind == LogRecord::Kind::applied);
    REQUIRE (rig.apply (2, "node.set", { s ("/godot/cue/M5TQ7XVA/name"), s ("Waiting edit") }).kind
             == LogRecord::Kind::applied);
    CHECK (rig.document.history (doc::UndoDomain::document).canUndo());

    //  THE FOLLOW BEGINS: that step is gone, the show is not.
    REQUIRE (rig.seat (3, "pair.hold", "following", "main-found").kind == LogRecord::Kind::applied);
    CHECK_FALSE (rig.document.history (doc::UndoDomain::document).canUndo());
    CHECK (rig.document.getAttribute ("/godot/cue/M5TQ7XVA/name") == std::optional<std::string> ("Waiting edit"));

    //  A MIRRORED UNDO reaching back before the link is refused here - the two shows then differ.
    CHECK (rig.apply (4, "undo", {}, "main:cli").reason == "nothing-to-undo");

    //  The main's edit after the link is a step like any other; a second word that it follows forgets nothing.
    REQUIRE (rig.apply (5, "node.set", { s ("/godot/cue/M5TQ7XVA/name"), s ("Main's edit") }, "main:cli").kind
             == LogRecord::Kind::applied);
    REQUIRE (rig.seat (6, "pair.hold", "following", "peer-keep").kind == LogRecord::Kind::applied);
    CHECK (rig.document.history (doc::UndoDomain::document).canUndo());
    CHECK (rig.apply (7, "undo", {}, "main:cli").kind == LogRecord::Kind::applied);

    //  A REPLAY, its own pairing told to forget as serve's is, comes out as the session did.
    Engine again;
    doc::ShowDocument replayedShow;
    read (mainShow, replayedShow);
    pair::Pairing replayed;
    replayed.followBegins = [&replayedShow] { replayedShow.clearHistory(); };
    doc::registerDocumentCommands (again.commands(), replayedShow);
    pair::registerPairCommands (again.commands(), replayed);
    again.setBeforeApply ([&replayedShow] (const Command& command, const Event& event,
                                           const std::vector<osc::Value>& args, std::int64_t tick)
                          { cue::beginUndoStep (replayedShow, nullptr, command, event, args, tick); });

    const auto result = wfg::replay (again, LogFile::parse (rig.engine.log().contents()));

    std::string mismatches;
    for (const auto& m : result.mismatches)
        mismatches += m + "\n";

    INFO (mismatches);
    CHECK (result.ok);
}

//==============================================================================
TEST_CASE ("same show: a summary and a hello carry the fingerprint and the digest, and an RD.8 summary still reads")
{
    pair::Summary leader;
    leader.tick = 300;
    leader.counter = 9;
    leader.standby = { { "7K2QM9X4", "B3N8R5TW" } };
    leader.runs = { { "R8M2K4Q1", "C1D2E3F4", "playing", 40, -1, 0 } };
    leader.showFingerprint = std::string (64, 'a');
    leader.mediaDigest = std::string (64, 'b');

    const auto bytes = pair::summaryBytes (leader);
    const auto decoded = osc::decode (bytes.data(), bytes.size());
    REQUIRE (decoded.ok);

    const auto read = pair::readSummary (decoded.packet);
    REQUIRE (read.has_value());
    CHECK (read->runs.size() == 1u);
    CHECK (read->showFingerprint == leader.showFingerprint);
    CHECK (read->mediaDigest == leader.mediaDigest);

    //  RD.8's, with nothing after its runs.
    auto older = decoded.packet;
    older.args.resize (older.args.size() - 2);
    const auto readOlder = pair::readSummary (older);
    REQUIRE (readOlder.has_value());
    CHECK (readOlder->runs.size() == 1u);
    CHECK (readOlder->showFingerprint.empty());

    //  None held yet: two empty strings, which never read as a run.
    pair::Summary bare;
    const auto bareBytes = pair::summaryBytes (bare);
    const auto bareRead = pair::readSummary (osc::decode (bareBytes.data(), bareBytes.size()).packet);
    REQUIRE (bareRead.has_value());
    CHECK (bareRead->runs.empty());

    //  The hello's two.
    pair::wire::Hello said;
    said.machine = "STAGE-RIGHT";
    said.role = "main";
    said.described = true;
    said.showFingerprint = leader.showFingerprint;
    said.mediaDigest = leader.mediaDigest;

    const auto hello = pair::wire::helloBytes (said);
    const auto readHello = pair::wire::readHello (osc::decode (hello.data(), hello.size()).packet);
    REQUIRE (readHello.has_value());
    CHECK (readHello->showFingerprint == said.showFingerprint);
    CHECK (readHello->mediaDigest == said.mediaDigest);

    //  And the show's own messages belong to the show check.
    using Bound = pair::wire::Bound;
    CHECK (pair::wire::boundFor (osc::Packet::message (pair::wire::showAsk, { osc::Value::int32 (1) })) == Bound::show);
    CHECK (pair::wire::boundFor (osc::Packet::message (pair::wire::showUnits, { s ("x"), osc::Value::int32 (0) }))
           == Bound::show);
}

//==============================================================================
TEST_CASE ("same show: a follower compares at the slot, asks the leader twice-differing, and names the cue; the leader's show arrives whole")
{
    //  THE TWO SHOWS, a cue's level apart, and a sound missing on the follower.
    Scratch mainSide, backupSide;
    writeFile (mainSide.folder.getChildFile ("media").getChildFile ("rain.wav"), std::string (100, 'r'));
    writeFile (mainSide.folder.getChildFile ("namespaces").getChildFile ("desk.json"), "{}");
    writeFile (backupSide.folder.getChildFile ("namespaces").getChildFile ("desk.json"), "{}");

    doc::ShowDocument main, backup;
    read (mainShow, main);
    read (backupShow, backup);
    edit (main, "/godot/cue/P9XKC2WR/level", "-12");

    const pair::Fingerprinter mainPrinter { mainSide.folder, mainSide.folder.getChildFile ("media").getFullPathName().toStdString(), {} };
    const pair::Fingerprinter backupPrinter { backupSide.folder, backupSide.folder.getChildFile ("media").getFullPathName().toStdString(), {} };

    const auto leaderState = mainPrinter.make (main.root(), main.showRevision());
    const auto ownState = backupPrinter.make (backup.root(), backup.showRevision());

    pair::ShowCheck leader, follower;
    const auto own = pair::ShowCheck::Own { ownState.get(), true };
    const auto now = std::chrono::steady_clock::now();
    const std::int64_t offset = 2;

    //  ONE SUMMARY THAT DIFFERS, compared at its slot: not said yet.
    follower.heard (100, leaderState->print->fingerprint, leaderState->print->mediaDigest);
    follower.afterTick (101, offset, true, own, "the main", now);
    CHECK (follower.differs().empty());
    follower.afterTick (102, offset, true, own, "the main", now);
    CHECK_FALSE (follower.matches());
    CHECK (follower.differs().empty());
    CHECK (follower.question (now).empty());

    //  A SUMMARY STILL WORKING ITS SHOW OUT says nothing, and breaks no run.
    follower.heard (103, "", "");
    follower.afterTick (105, offset, true, own, "the main", now);
    CHECK (follower.differs().empty());

    //  THE SECOND THAT DIFFERS: the show differs; the leader is asked for its show and its media.
    follower.heard (105, leaderState->print->fingerprint, leaderState->print->mediaDigest);
    follower.afterTick (107, offset, true, own, "the main", now);
    CHECK_FALSE (follower.matches());
    CHECK (follower.showDiffers());

    const auto ask = follower.question (now);
    REQUIRE_FALSE (ask.empty());
    CHECK (follower.question (now).empty());           // asked once, not every tick
    CHECK (follower.package() == nullptr);

    //  THE LEADER: the question waits for a show that is its own now, then is answered down its channel.
    const auto askPacket = osc::decode (ask.data(), ask.size());
    REQUIRE (askPacket.ok);
    leader.received ("ws:10.0.0.9:5001", askPacket.packet);
    leader.drain();
    REQUIRE (leader.asked() == 1u);

    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> sent;
    const auto send = [&sent] (const std::string& channel, const std::vector<std::uint8_t>& bytes)
    {
        sent.emplace_back (channel, bytes);
        return true;
    };

    leader.answer (leaderState.get(), false, send);
    CHECK (sent.empty());
    leader.answer (leaderState.get(), true, send);
    REQUIRE (sent.size() >= 3u);
    CHECK (leader.asked() == 0u);
    CHECK (std::all_of (sent.begin(), sent.end(), [] (const auto& one) { return one.first == "ws:10.0.0.9:5001"; }));

    for (const auto& [channel, bytes] : sent)
    {
        const auto packet = osc::decode (bytes.data(), bytes.size());
        REQUIRE (packet.ok);
        CHECK (pair::wire::boundFor (packet.packet) == pair::wire::Bound::show);
        follower.received ("127.0.0.1:5010", packet.packet);
    }

    follower.drain();
    follower.afterTick (108, offset, true, own, "the main", now);

    //  SAID, in one sentence: the cue, and the sound.
    CHECK (follower.differs() == "the show differs: cue 4 Rain changed; 1 sound missing here: rain.wav");
    CHECK (follower.unitsHeld() == leaderState->print->units.size());

    //  THE LEADER'S SHOW, WHOLE: its text and its descriptions, the fingerprint it last said.
    const auto* package = follower.package();
    REQUIRE (package != nullptr);
    CHECK (package->fingerprint == leaderState->print->fingerprint);
    CHECK (package->text == leaderState->print->decided);
    REQUIRE (package->descriptions.size() == 1u);
    CHECK (package->descriptions[0].name == "desk.json");

    //  THE LEADER'S SHOW MOVES ON: the package is no longer the one to take, and the show is asked again.
    edit (main, "/godot/cue/B3N8R5TW/name", "Thunder clap");
    const auto moved = mainPrinter.make (main.root(), main.showRevision());
    follower.heard (110, moved->print->fingerprint, moved->print->mediaDigest);
    follower.afterTick (112, offset, true, own, "the main", now);
    CHECK (follower.package() == nullptr);
    CHECK_FALSE (follower.question (now).empty());

    //  TAKEN, AND THE SAME: one summary that agrees says so.
    doc::ShowDocument same;
    read (mainShow, same);
    edit (same, "/godot/cue/P9XKC2WR/level", "-12");
    edit (same, "/godot/cue/B3N8R5TW/name", "Thunder clap");
    writeFile (backupSide.folder.getChildFile ("media").getChildFile ("rain.wav"), std::string (100, 'r'));
    REQUIRE (backupSide.folder.getChildFile ("media").getChildFile ("rain.wav")
                .setLastModificationTime (mainSide.folder.getChildFile ("media").getChildFile ("rain.wav").getLastModificationTime()));

    const auto sameState = backupPrinter.make (same.root(), same.showRevision());
    REQUIRE (sameState->print->fingerprint == moved->print->fingerprint);
    REQUIRE (sameState->print->mediaDigest == moved->print->mediaDigest);

    follower.adopted();
    follower.heard (115, moved->print->fingerprint, moved->print->mediaDigest);
    follower.afterTick (117, offset, true, pair::ShowCheck::Own { sameState.get(), true }, "the main", now);
    CHECK (follower.matches());
    CHECK (follower.differs().empty());

    //  A LATE SUMMARY - its slot passed - and one compared against a show not yet worked out: neither counts.
    follower.reset();
    follower.heard (120, leaderState->print->fingerprint, leaderState->print->mediaDigest);
    follower.afterTick (130, offset, true, own, "the main", now);
    follower.heard (131, leaderState->print->fingerprint, leaderState->print->mediaDigest);
    follower.afterTick (133, offset, true, pair::ShowCheck::Own { ownState.get(), false }, "the main", now);
    follower.heard (134, leaderState->print->fingerprint, leaderState->print->mediaDigest);
    follower.afterTick (136, offset, true, own, "the main", now);
    CHECK_FALSE (follower.showDiffers());
}

//==============================================================================
TEST_CASE ("same show: the fingerprinter works off the tick thread from a copy, and looks at the media again by itself")
{
    Scratch scratch;
    const auto media = scratch.folder.getChildFile ("media");
    writeFile (media.getChildFile ("tone.wav"), std::string (100, 't'));
    writeFile (media.getChildFile ("rain.wav"), std::string (100, 'r'));

    doc::ShowDocument show;
    read (mainShow, show);

    pair::Fingerprinter printer { scratch.folder, media.getFullPathName().toStdString(), {}, 50ms };
    REQUIRE (printer.start());
    CHECK (printer.idle());
    CHECK (printer.latest() == nullptr);

    //  A COPY, handed over: the worker's, whatever the tick does to the show after.
    printer.offer (show.root().createCopy(), show.showRevision());
    edit (show, "/godot/cue/P9XKC2WR/level", "-3");

    REQUIRE (waitFor ([&printer] { return printer.published() > 0 && printer.idle(); }));
    const auto first = printer.latest();
    REQUIRE (first != nullptr);
    CHECK (first->print->revision + 1 <= show.showRevision());
    CHECK (first->print->decided.find ("level=\"-6.5\"") != std::string::npos);
    CHECK (first->print->media.size() == 2u);
    CHECK (std::all_of (first->print->media.begin(), first->print->media.end(),
                        [] (const doc::MediaEntry& entry) { return entry.present; }));

    //  A SOUND REMOVED, nothing offered: the worker sees it by itself, and only the digest moves.
    const auto seen = printer.published();
    REQUIRE (media.getChildFile ("rain.wav").deleteFile());
    REQUIRE (waitFor ([&printer, seen] { return printer.published() > seen; }));

    const auto second = printer.latest();
    CHECK (second->print->fingerprint == first->print->fingerprint);
    CHECK (second->print->mediaDigest != first->print->mediaDigest);
    CHECK (second->print->revision == first->print->revision);

    printer.stop();
}

//==============================================================================
TEST_CASE ("same show: what a fingerprint costs - the tick's copy and the worker's work on the 500-cue show")
{
    /*  WHERE THE WORK IS, AND WHY THERE (pair/ShowCheck.h): the tick thread's
        whole share is a deep copy of the tree, handed to a worker that writes
        the decided text, hashes it, cuts it into units and makes the answers.
        M23's shape - five hundred Media cues, a Feed each - so the figures
        stand beside the autosave's. Printed to be quoted, gating nothing but
        counts: a clock on a shared runner is a flaky test. */
    doc::ShowDocument document;
    read (mainShow, document);

    auto list = document.findById ("7K2QM9X4");
    REQUIRE (list.isValid());

    constexpr int cueCount = 500;

    for (int n = 0; n < cueCount; ++n)
    {
        juce::ValueTree media { "Media" };
        media.setProperty (juce::Identifier ("id"), juce::String ("MA") + juce::String (100000 + n), nullptr);
        media.setProperty (juce::Identifier ("name"), juce::String ("Cue ") + juce::String (n), nullptr);
        media.setProperty (juce::Identifier ("file"), "tone.wav", nullptr);

        juce::ValueTree feed { "Feed" };
        feed.setProperty (juce::Identifier ("id"), juce::String ("FA") + juce::String (100000 + n), nullptr);
        feed.setProperty (juce::Identifier ("slot"), juce::String ("SA") + juce::String (100000 + n % 20), nullptr);
        feed.setProperty (juce::Identifier ("gains"), "1", nullptr);
        media.addChild (feed, -1, nullptr);

        list.addChild (media, -1, nullptr);
    }

    const pair::Fingerprinter printer { juce::File(), std::string(), {} };

    constexpr int rounds = 10;
    std::vector<double> copies, works, texts;

    for (int n = 0; n < rounds; ++n)
    {
        const auto start = juce::Time::getMillisecondCounterHiRes();
        auto copy = document.root().createCopy();
        const auto copied = juce::Time::getMillisecondCounterHiRes();
        const auto state = printer.make (copy, document.showRevision());
        const auto made = juce::Time::getMillisecondCounterHiRes();
        const auto text = doc::CanonicalXml::write (document);
        const auto written = juce::Time::getMillisecondCounterHiRes();

        copies.push_back (copied - start);
        works.push_back (made - copied);
        texts.push_back (written - made);

        CHECK (state->print->units.size() > static_cast<std::size_t> (cueCount));
        CHECK (! text.empty());
    }

    const auto median = [] (std::vector<double> values)
    {
        std::sort (values.begin(), values.end());
        return values[values.size() / 2];
    };

    MESSAGE ("500 cues: the tick's copy " << median (copies) << " ms; the worker's fingerprint, units and answers "
             << median (works) << " ms; the canonical text alone, for comparison, " << median (texts) << " ms");
}
