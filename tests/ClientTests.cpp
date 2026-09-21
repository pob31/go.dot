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
    The compiled client's model half: what a window would show, with no window.

    wfg_client_model names no JUCE type, so everything a label reads, every
    theme token and every gesture's Event can be asserted here with the same
    rig TreeTests.cpp uses - an engine, the minimal bundle and a ParameterTree
    - and under both locales, because the strings a cell shows are a
    serialisation surface like any other (a tick that read "1 234 567" under
    fr_FR would be the page and the window disagreeing about one number).

    AND EVERY GESTURE IS CHECKED AGAINST THE REAL REGISTRY, which is this
    file's version of what `tests/blackbox/client_page.py` does for the page's
    `gestures/commands.json`: every command a gesture names must exist and must
    accept the arguments the gesture sends. That is the check that catches a
    renamed command or a signature that grew an argument, and it is worth more
    than all the string assertions here put together - the strings only go
    wrong for the reader, a wrong command name goes wrong for the show.

    What is NOT here, and is written down as untestable in ui/Client.cpp: that
    the window opens, layout, colour on screen, hit-testing, focus, the
    dialogues, timing, the shutdown order. The first person to find a broken
    window is the author, on a build.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <optional>

#include <wfg/client/model/Gestures.h>
#include <wfg/client/model/Inspector.h>
#include <wfg/client/model/LoadToTime.h>
#include <wfg/client/model/Media.h>
#include <wfg/client/model/NewCue.h>
#include <wfg/client/model/OutputList.h>
#include <wfg/client/model/Panic.h>
#include <wfg/client/model/Reorder.h>
#include <wfg/client/model/RunModel.h>
#include <wfg/client/model/Scrub.h>
#include <wfg/client/model/Selection.h>
#include <wfg/client/model/ShowModel.h>
#include <wfg/client/model/Text.h>
#include <wfg/client/model/Theme.h>
#include <wfg/client/model/Transport.h>
#include <wfg/client/model/UndoHistory.h>
#include <wfg/client/model/Waveform.h>
#include <wfg/engine/audio/Timbre.h>
#include <wfg/engine/Engine.h>
#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/command/Event.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/DocumentSession.h>
#include <wfg/engine/document/DocumentWriter.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/tree/TreeCommands.h>

#include <juce_core/juce_core.h>

#include <cstdint>
#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace wfg;
using namespace wfg::client;
using namespace wfg::tree;

namespace
{
    juce::File fixtureBundle (const char* name = "minimal")
    {
        const juce::File folder { juce::String (std::string (WFG_TEST_FIXTURES_DIR))
                                    + "/bundles/" + name };

        REQUIRE_MESSAGE (folder.isDirectory(), "missing fixture bundle: "
                                                 << folder.getFullPathName());
        return folder;
    }

    /*  TreeTests.cpp's rig, with the readings serve fills in before the loop
        (Console.cpp:2102-2113, :2139-2141, :2699) so that the transport has a
        show name, a clock and a rate to read. */
    struct Rig
    {
        explicit Rig (const char* bundle = "minimal")
        {
            REQUIRE (doc::Bundle::open (fixtureBundle (bundle), document).ok);
            doc::registerDocumentCommands (engine.commands(), document);
            registerTreeCommands (engine.commands(), touches);
        }

        Engine::TickResult apply (std::int64_t tick, const std::string& origin,
                                  const std::string& command, std::vector<osc::Value> args = {})
        {
            REQUIRE (engine.submit (origin, command, std::move (args)));
            const auto result = engine.processTick (tick);
            parameters.markStale();
            return result;
        }

        std::shared_ptr<const TreeSnapshot> publish (std::int64_t tick)
        {
            EngineState state;
            state.version = "test";
            state.tick = tick;
            state.sampleRate = 48000;
            state.blockSize = 256;
            state.clock = "dummy";      // serve publishes this under --hosted too (Console.cpp:2699)
            state.audioStatus = "running";
            state.documentName = "minimal";
            state.documentRevision = document.showRevision();
            state.errorCount = engine.errorCount();
            state.lastError = engine.lastError();
            return parameters.publish (tick, state);
        }

        Engine engine;
        doc::ShowDocument document;
        TouchTable touches;
        MountTable mounts;
        cue::RunTable runs;
        ParameterTree parameters { document, engine.commands(), mounts, runs };
    };
}

//==============================================================================
TEST_CASE ("client: a value's text is the log's and the page's, under either locale")
{
    CHECK (model::text (osc::Value::float64 (0.5)) == "0.5");
    CHECK (model::text (osc::Value::float64 (1234567.25)) == "1234567.25");
    CHECK (model::text (osc::Value::float32 (0.1f)) == "0.1");
    CHECK (model::text (osc::Value::int64 (-12)) == "-12");
    CHECK (model::text (osc::Value::int32 (1234567)) == "1234567");     // no grouping, fr_FR included
    CHECK (model::text (osc::Value::boolean (true)) == "true");
    CHECK (model::text (osc::Value::boolean (false)) == "false");
    CHECK (model::text (osc::Value::string ("Announce")) == "Announce");
    CHECK (model::text (osc::Value::nil()).empty());
    CHECK (model::text (osc::Value::impulse()).empty());
    CHECK (model::text (static_cast<const Node*> (nullptr)).empty());

    CHECK (model::words ("A B  C") == std::vector<std::string> { "A", "B", "C" });
    CHECK (model::words (" A ") == std::vector<std::string> { "A" });
    CHECK (model::words ("").empty());
}

TEST_CASE ("client: a flag has three answers, and the third is not false")
{
    /*  A node the engine has not published is not a node reading false: a show
        with no answer yet about its lock is not an unlocked show. Every
        gesture this client offers reads `isYes`, so "unsaid" offers less
        rather than more. */
    Rig rig;
    const auto snapshot = rig.publish (0);

    CHECK (model::flag (*snapshot, "/godot/document/dirty") == model::Flag::no);
    CHECK (model::flag (*snapshot, "/godot/document/nothing-here") == model::Flag::unsaid);
    CHECK (model::flag (*snapshot, "/godot/document/name") == model::Flag::unsaid);   // a string, not a T

    CHECK_FALSE (model::isYes (model::Flag::unsaid));
    CHECK_FALSE (model::isYes (model::Flag::no));
    CHECK (model::isYes (model::Flag::yes));
}

TEST_CASE ("client: the transport is read out of one snapshot, at the addresses the page reads")
{
    Rig rig;

    auto reading = model::readTransport (*rig.publish (0));

    CHECK (reading.show == "minimal");
    CHECK (reading.tick == "0");
    CHECK (reading.clock == "dummy");
    CHECK (reading.rate == "48000 / 256");
    CHECK (reading.status == "running");
    CHECK (reading.listId == "7K2QM9X4");             // state.xml names no focus: the first list
    CHECK (reading.listName == "Main");
    CHECK (reading.standbyId == "B3N8R5TW");          // state.xml's standby
    CHECK (reading.standbyName == "House to half");
    CHECK (reading.lastError.empty());
    CHECK (reading.writeError.empty());
    CHECK (reading.revision >= 1);
    CHECK (reading.dirty == model::Flag::no);
    CHECK (reading.locked == model::Flag::no);
    CHECK (reading.recovery == model::Flag::no);
    CHECK (reading.canUndo == model::Flag::no);       // a freshly opened show has nothing to take back

    /*  THE TICK IS DIGITS, under fr_FR too: a number that went through a
        locale would read "1 234 567" here and disagree with the page. */
    reading = model::readTransport (*rig.publish (1234567));
    CHECK (reading.tick == "1234567");

    /*  A standby move, applied - a refused write would leave the old name in
        place and let the case pass for the wrong reason. */
    REQUIRE (rig.apply (2, "cli", "node.set",
                        { osc::Value::string ("/godot/list/7K2QM9X4/standby"),
                          osc::Value::string ("F7HR8TVD") }).applied == 1);

    reading = model::readTransport (*rig.publish (2));
    CHECK (reading.standbyId == "F7HR8TVD");
    CHECK (reading.standbyName == "Announce");

    // And a refusal reaches the strip as the engine's own sentence.
    CHECK (rig.apply (3, "cli", "node.set",
                      { osc::Value::string ("/godot/cue/ZZZZZZZZ/name"),
                        osc::Value::string ("x") }).applied == 0);

    reading = model::readTransport (*rig.publish (3));
    CHECK_FALSE (reading.lastError.empty());

    // Two readings of the same state are equal, which is what the window's redraw test relies on.
    CHECK (model::readTransport (*rig.publish (3)) == model::readTransport (*rig.publish (3)));
}

TEST_CASE ("client: an edit is a name to take back, and the window is told in the engine's words")
{
    Rig rig;

    REQUIRE (rig.apply (1, "cli", "node.set",
                        { osc::Value::string ("/godot/cue/B3N8R5TW/name"),
                          osc::Value::string ("Renamed") }).applied == 1);

    /*  The undo readouts are EngineState's, assigned in serve's after-tick from
        the history; this rig has no history, so what is pinned here is that the
        window reads whatever the engine published rather than deciding for
        itself - and the sentence it makes of it. */
    const auto reading = model::readTransport (*rig.publish (1));

    CHECK (reading.revision > 1);                     // an edit moved the show

    model::TransportReading made;
    made.canUndo = model::Flag::yes;
    made.undoName = "node.set";
    made.canRedo = model::Flag::no;

    /*  A TOOLTIP APIECE RATHER THAN A LINE, since the author asked for the
        headroom back (2026-09-18) - but still the engine's own name for what
        would be taken back, and still three answers to a flag. */
    CHECK (made.undoTip() == "undo node.set");
    CHECK (made.redoTip() == "nothing to redo");

    made.undoName.clear();
    CHECK (made.undoTip() == "undo the last edit");

    made.canRedo = model::Flag::yes;
    made.redoName = "object.delete";
    CHECK (made.redoTip() == "redo object.delete");

    made.canUndo = model::Flag::unsaid;
    made.canRedo = model::Flag::unsaid;
    CHECK (made.undoTip() == "undo: —");
    CHECK (made.redoTip() == "redo: —");
}

TEST_CASE ("client: the strip says in words what it also says in colour")
{
    model::TransportReading reading;

    // Nothing published yet.
    CHECK (reading.standbyLine() == "no standby");
    CHECK (reading.lockLine().empty());

    /*  AND A SAVE IS STILL OFFERED, because a dot the engine has not published
        is not a show with nothing in it: withholding the save is the expensive
        way to be wrong. */
    CHECK (reading.hasSomethingToSave());

    reading.standbyId = "X";
    reading.standbyName = "Thunder";
    reading.standbyKind = "media";
    CHECK (reading.standbyLine() == "Thunder  media");

    reading.standbyKind.clear();
    CHECK (reading.standbyLine() == "Thunder");

    /*  WHETHER THERE IS ANYTHING TO SAVE IS THE SAVE BUTTON'S OWN STATE
        now, and no longer a word on a line (author, 2026-09-18). */
    reading.dirty = model::Flag::no;
    CHECK_FALSE (reading.hasSomethingToSave());

    reading.dirty = model::Flag::yes;
    CHECK (reading.hasSomethingToSave());

    /*  AND THE LOCK IS STILL SAID IN A WORD and not only in a colour (§4.8),
        while the audio's own word has gone to a configuration panel nobody has
        built yet. */
    reading.status = "running";
    CHECK (reading.lockLine().empty());

    reading.locked = model::Flag::yes;
    CHECK (reading.lockLine() == "locked");

    // An unpublished lock says nothing here rather than saying "open".
    reading.locked = model::Flag::unsaid;
    CHECK (reading.lockLine().empty());
}

TEST_CASE ("client: a show with everything wrong with it is summarised, never carried whole")
{
    /*  THE CASE A HUNG WINDOW BOUGHT. `/godot/document/warnings` is one line
        per thing wrong with the show that did not stop it opening, and on a
        generated three-hundred-cue show it measured 233,471 characters over
        1,824 lines. The transport carried it whole and handed it to a label
        one row high; laying that much text into that little space is work
        without end, and the window spun - at a hundred percent of a core,
        before it was ever visible, with the tick stuck at zero because the
        clock starts after the client is built.

        So the reading carries a count and a first line, both bounded, and
        these two functions are where that happens. The numbers below are the
        real ones from that show. */
    std::string many;

    for (int i = 0; i < 1824; ++i)
        many += "/Show/.../Slot[SA00000" + std::to_string (i) + "]: cues MA1 and MA2 can both be "
                "holding it; mark either Feed or Insert shared if that is meant\n";

    /*  160 CHARACTERS AND AN ELLIPSIS, which is three bytes in UTF-8 - so the
        bound is 163, and it is written out rather than rounded up because a
        bound nobody can derive is a bound nobody will notice moving. */
    constexpr std::size_t clipped = 160 + 3;

    CHECK (many.size() > 200000);
    CHECK (model::countWarnings (many) == 1824);          // a trailing newline invents no last one

    /*  BOUNDED IS THE INVARIANT, and these warnings are the real shape: each
        line is about 120 characters, so the first comes back WHOLE and it is
        the count that does the work of not carrying 233 kB into a label. The
        per-line clip below is for the other shape - one enormous warning -
        which hangs a text layout just as well. */
    CHECK (model::firstWarning (many).size() <= clipped);
    CHECK (model::firstWarning (many).size() > 100);

    CHECK (model::countWarnings ("") == 0);
    CHECK (model::firstWarning ("").empty());
    CHECK (model::countWarnings ("one") == 1);
    CHECK (model::countWarnings ("one\ntwo") == 2);
    CHECK (model::countWarnings ("one\ntwo\n") == 2);
    CHECK (model::firstWarning ("one\ntwo") == "one");

    /*  ONE WARNING A QUARTER OF A MEGABYTE LONG is as able to hang a text
        layout as eighteen hundred short ones, so the clip is on length and not
        only on the line count. */
    CHECK (model::firstWarning (std::string (250000, 'x')).size() == clipped);

    model::TransportReading reading;
    CHECK (reading.warningLine().empty());

    reading.warningCount = 1;
    reading.warningFirst = "a slot is held twice";
    CHECK (reading.warningLine() == "1 warning · a slot is held twice");

    reading.warningCount = 1824;
    CHECK (reading.warningLine().rfind ("1824 warnings", 0) == 0);
    CHECK (reading.warningLine().size() < 250);
}

TEST_CASE ("client: a row the pointer cannot stand on is not offered, and a refusal is a sentence")
{
    /*  WHAT THE AUTHOR'S FIRST SESSION WITH THE CUE LIST FOUND. They clicked
        two rows and got `error: 5411 26 window not-a-stop standby.set` where
        an answer should have been. The engine was right - P4MSG002 is inside
        a Footer and P4MSG003 inside a Persistent section, and decision X lets
        the pointer stand on any cue of its list EXCEPT those and a header.
        What was wrong was the window offering a gesture it could have known
        would be refused. */
    model::Row row;

    row.section = model::Section::member;
    CHECK (row.mayPark());

    for (const auto section : { model::Section::header, model::Section::footer,
                                model::Section::persistent })
    {
        row.section = section;
        CHECK_FALSE (row.mayPark());
    }

    /*  AND THE REFUSAL READS AS A SENTENCE. The node is five fields written
        for grep at four in the morning; an operator who just pressed
        something needs what and why, and the tick is noise - they were
        there. The reason word stays the engine's own. */
    model::TransportReading reading;
    CHECK (reading.errorLine().empty());

    reading.lastError = "5411 26 window not-a-stop standby.set";
    CHECK (reading.errorLine() == "standby.set refused: not-a-stop");

    /*  Anything that is not five fields is shown whole, so a format change is
        visible rather than swallowed into a wrong-looking sentence. */
    reading.lastError = "something else entirely";
    CHECK (reading.errorLine() == "something else entirely");
}

TEST_CASE ("client: show mode does not offer a save, and nothing else is withdrawn")
{
    /*  §9, decision W, as the author reaffirmed it on 2026-09-17: the ENGINE
        keeps saving under the lock - lock first so nothing moves, then save,
        so the file on disk is the final show - and it is the CLIENT that stops
        asking, because usually nobody saves a show mid-performance. A script
        still can, which is why the rule lives in the client and not the door. */
    model::TransportReading reading;

    CHECK (reading.mayOfferSave());                   // unsaid is not locked
    reading.locked = model::Flag::no;
    CHECK (reading.mayOfferSave());
    reading.locked = model::Flag::yes;
    CHECK_FALSE (reading.mayOfferSave());
}

//==============================================================================
TEST_CASE ("client: a theme is complete from construction, and a file changes only what it names")
{
    model::Theme theme;

    for (const auto& name : model::Theme::colourNames())
    {
        CHECK_MESSAGE (theme.colour (name) != 0xFFFF00FFu, name << " is undeclared");

        /*  OPAQUE, not "not black": black is a colour somebody chose - the
            sections and the running pane are black since 2026-09-18, at the
            author's word - while a token nobody set would be nought, which has
            no alpha at all. That is the forgotten default this check exists to
            catch, and it catches it still. */
        CHECK_MESSAGE ((theme.colour (name) & 0xFF000000u) == 0xFF000000u, name << " is transparent");
    }

    CHECK (theme.colour ("nobody-declared-this") == 0xFFFF00FFu);
    CHECK (theme.colour ("standby") == 0xFFE8B04Bu);

    CHECK (theme.apply (R"({ "standby": "#ff0000", "type": 1.5, "about": "a note" })").empty());
    CHECK (theme.colour ("standby") == 0xFFFF0000u);
    CHECK (theme.colour ("ink") == 0xFFE8E6E1u);                 // untouched
    CHECK (theme.type == doctest::Approx (1.5));

    const auto before = theme;

    const auto broken = theme.apply ("{ \"ink\": \"#ffffff\", ");
    CHECK_FALSE (broken.empty());
    CHECK (theme == before);

    /*  REFUSED WHOLE: the ink that came first did not land either, because a
        half-applied look hides the sentence about the half that failed. */
    const auto misnamed = theme.apply (R"({ "ink": "#ffffff", "inc": "#000000" })");
    CHECK (misnamed.find ("inc") != std::string::npos);
    CHECK (theme == before);

    const auto notAColour = theme.apply (R"({ "standby": "red" })");
    CHECK (notAColour.find ("standby") != std::string::npos);
    CHECK (theme == before);

    CHECK_FALSE (theme.apply (R"({ "type": -1 })").empty());
    CHECK_FALSE (theme.apply (R"({ "refreshHz": "fast" })").empty());
    CHECK_FALSE (theme.apply ("[1, 2, 3]").empty());
    CHECK (theme == before);

    CHECK (theme.apply (R"({ "picked": "#9a95e480" })").empty());   // rrggbbaa
    CHECK (theme.colour ("picked") == 0x809A95E4u);
}

TEST_CASE ("client: the committed theme file is the defaults, transcribed")
{
    /*  clients/desktop/theme.json exists so the author can change it; until
        they do, it must say exactly what the defaults say, or the window
        opens looking different from the page for no reason anybody chose. */
    const juce::File file { juce::File (juce::String (std::string (WFG_REPO_ROOT)))
                              .getChildFile ("clients/desktop/theme.json") };

    REQUIRE_MESSAGE (file.existsAsFile(), "missing " << file.getFullPathName());

    model::Theme theme;
    CHECK (theme.apply (file.loadFileAsString().toStdString()).empty());
    CHECK (theme == model::Theme {});
}

//==============================================================================
TEST_CASE ("client: every gesture is a real command, with arguments it will accept")
{
    /*  THE DESKTOP'S VERSION OF `client_page.py`'s commands.json CHECK. Every
        gesture names a command `wfg commands` lists, and sends arguments that
        command's signature accepts - asserted against the registry the serve
        verb builds, not against a copy of it. A renamed command or a signature
        that grew an argument fails here rather than at 04:12.

        Registered the way serve registers them, so the names are the real
        ones: the document's, the cue list's, the bundle's (`document.save` and
        the rest) and the Runner's, which is where `go` itself lives. */
    Rig rig;

    cue::Focus focus;
    doc::DocumentSession session;
    doc::DocumentWriter writer;
    doc::IdRegistry runIds { doc::IdRegistry::withSeed (11) };
    cue::Runner runner { rig.document, rig.runs, runIds, focus };

    cue::registerCueCommands (rig.engine.commands(), rig.document, focus);
    cue::registerRunCommands (rig.engine.commands(), rig.runs);
    doc::registerBundleCommands (rig.engine.commands(), rig.document, session, writer);
    cue::registerGoCommands (rig.engine.commands(), rig.engine, runner, rig.document, focus, runIds);

    const std::vector<Event> gestures
    {
        gesture::go(), gesture::standbyNext(), gesture::standbyPrevious(),
        gesture::stopAll(), gesture::killAll(),
        gesture::park ("B3N8R5TW"), gesture::kill ("R4NID001"),
        gesture::seek ("R4NID001", 12.5),
        gesture::aim ("7K2QM9X4", "B3N8R5TW", 12.5), gesture::loadToTime ("7K2QM9X4"),
        gesture::recordStart(), gesture::recordStop(),
        gesture::setNode ("/godot/cue/B3N8R5TW/name", "Renamed"),
        gesture::createCue ("7K2QM9X4", 0, "media", "Thunder"),
        gesture::moveObject ("B3N8R5TW", "7K2QM9X4", 0),
        gesture::deleteObject ("B3N8R5TW"),
        gesture::groupRole ("B3N8R5TW", "footer"),
        gesture::undo(), gesture::redo(), gesture::save(), gesture::revert(),
        gesture::saveAs ("C:/shows/copy"),
        gesture::copyCues ({ "B3N8R5TW", "F7HR8TVD" }),
        gesture::pasteCues ("7K2QM9X4", 0, "<Fragment/>"),
        gesture::recover(), gesture::discardRecovery(),
        gesture::setLocked (true), gesture::setLocked (false),
        gesture::createBus ("direct", 1, -1), gesture::createBus ("mix", 2, 0),
        gesture::deleteBus ("J3MT5XYA"), gesture::moveBus ("J3MT5XYA", 2),
        gesture::setBusWidth ("J3MT5XYA", 2),
        gesture::setPatchSettled (true), gesture::setPatchSettled (false),
    };

    for (const auto& event : gestures)
    {
        INFO ("gesture sends " << event.command);

        /*  EVERY ONE CARRIES THE WINDOW'S ORIGIN, which is what makes §14.16's
            first rule checkable: a change that reached the show any other way
            would write no record, and a replay would not reproduce it. */
        CHECK (event.origin == std::string (origin::window));

        const auto* command = rig.engine.commands().find (event.command);
        REQUIRE_MESSAGE (command != nullptr, "no such command: " << event.command);

        const auto check = CommandRegistry::checkArgs (*command, event.args);
        CHECK_MESSAGE (check.ok, event.command << " refused the gesture's arguments: "
                                               << check.reason);
    }

    // And the one that carries arguments says what it means by them.
    const auto lock = gesture::setLocked (true);
    REQUIRE (lock.args.size() == 2);
    CHECK (lock.args[0] == osc::Value::string ("/godot/document/locked"));
    CHECK (lock.args[1] == osc::Value::boolean (true));
    CHECK (gesture::setLocked (false).args[1] == osc::Value::boolean (false));
}

TEST_CASE ("client: a gesture that reaches the engine is applied, and says the window sent it")
{
    /*  The registry check above proves the names; this proves the round trip -
        the same door OSC arrives through, the origin on the record, and the
        show actually moving. `wfg.replay.window` does the rest with a real
        session's log. */
    Rig rig;

    const auto lock = gesture::setLocked (true);

    REQUIRE (rig.engine.submit (lock));
    const auto outcome = rig.engine.processTick (1);

    CHECK (outcome.applied == 1);
    CHECK (outcome.soleOrigin == std::string (origin::window));

    rig.parameters.markStale();
    CHECK (model::readTransport (*rig.publish (1)).locked == model::Flag::yes);

    // And the window would stop offering a save from that reading alone.
    CHECK_FALSE (model::readTransport (*rig.publish (1)).mayOfferSave());
}

//==============================================================================
TEST_CASE ("client: the cue list is the show's own order, with groups nested inside it")
{
    /*  The rows a window draws, from a bundle that has nesting in it. What is
        asserted is the WALK - order, depth, which rows hold others - rather
        than any one cue's name, because the walk is what a view cannot fix. */
    Rig rig { "groups" };
    const auto snapshot = rig.publish (0);

    const auto listId = model::readTransport (*snapshot).listId;
    REQUIRE_FALSE (listId.empty());

    model::ShowModel show;
    CHECK (show.refresh (*snapshot, listId));
    CHECK (show.rebuilds() == 1);

    const auto& rows = show.rows();
    REQUIRE_FALSE (rows.empty());

    /*  Every CUE row is findable by id, and the index agrees with its
        position. A band is not a cue and is in no index: it names a section,
        not a thing the show can act on. */
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (rows[i].rowKind == model::RowKind::cue)
            CHECK (show.indexOf (rows[i].id) == static_cast<int> (i));

    CHECK (show.indexOf ("ZZZZZZZZ") == -1);

    //  A group's members follow it immediately and are one deeper: that is the
    //  whole of what "nested" means to a list drawn as rows.
    bool sawGroup = false;

    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        if (! rows[i].isGroup)
            continue;

        sawGroup = true;
        CHECK_FALSE (rows[i].mode.empty());           // a group says how it runs

        if (i + 1 < rows.size() && rows[i + 1].depth > rows[i].depth)
        {
            CHECK (rows[i + 1].depth == rows[i].depth + 1);
            CHECK (rows[i + 1].parent == rows[i].id);
        }
    }

    CHECK_MESSAGE (sawGroup, "the groups fixture should have a group in it");

    //  Nothing is drawn twice, and no row claims a depth the cap forbids.
    std::set<std::string> seen;

    for (const auto& row : rows)
    {
        CHECK (row.depth >= 0);
        CHECK (row.depth <= 64);

        if (row.rowKind != model::RowKind::cue)
            continue;

        CHECK (seen.insert (row.id).second);
        CHECK_FALSE (row.kind.empty());
    }
}

TEST_CASE ("client: the cue list is rebuilt when the show moves, and not when the operator does")
{
    /*  THE CASE M0 EXISTS FOR, in the shape M18 uses: count the work rather
        than time it. The document half of a snapshot is rebuilt whenever ANY
        command applies - `markStale` fires on `outcome.applied > 0`, GO
        included - so a model keyed on it would rebuild every row several times
        a second during a chain of runs, for a show nobody edited.
        `/godot/document/revision` counts what `show.xml` would record and
        nothing else, which is what makes the two halves of this case differ. */
    Rig rig;

    const auto listId = model::readTransport (*rig.publish (0)).listId;
    REQUIRE_FALSE (listId.empty());

    model::ShowModel show;

    REQUIRE (show.refresh (*rig.publish (0), listId));
    CHECK (show.rebuilds() == 1);

    //  A hundred publishes with nothing applied: one walk, still.
    for (std::int64_t tick = 1; tick <= 100; ++tick)
        CHECK_FALSE (show.refresh (*rig.publish (tick), listId));

    CHECK (show.rebuilds() == 1);

    /*  A STANDBY MOVE IS NOT AN EDIT. It is the write a GO makes, it applies,
        and `markStale` mints a fresh document half for it - so a model keyed
        on the snapshot would rebuild here and this is the assertion that says
        it does not. */
    REQUIRE (rig.apply (101, "cli", "node.set",
                        { osc::Value::string ("/godot/list/" + listId + "/standby"),
                          osc::Value::string ("F7HR8TVD") }).applied == 1);

    CHECK_FALSE (show.refresh (*rig.publish (101), listId));
    CHECK (show.rebuilds() == 1);

    //  An edit is.
    REQUIRE (rig.apply (102, "cli", "node.set",
                        { osc::Value::string ("/godot/cue/B3N8R5TW/name"),
                          osc::Value::string ("Renamed") }).applied == 1);

    CHECK (show.refresh (*rig.publish (102), listId));
    CHECK (show.rebuilds() == 2);
    const auto renamedAt = show.indexOf ("B3N8R5TW");
    REQUIRE (renamedAt >= 0);
    CHECK (show.rows()[static_cast<std::size_t> (renamedAt)].name == "Renamed");

    //  And so is looking at another list, which the revision cannot say.
    CHECK (show.refresh (*rig.publish (103), "SOMEOTHERLIST"));
    CHECK (show.rebuilds() == 3);
    CHECK (show.rows().empty());                      // a list that is not there draws nothing
    CHECK (show.list() == "SOMEOTHERLIST");
}

//==============================================================================
TEST_CASE ("client: a section is a band that folds, and the fold is the client's alone")
{
    /*  The author, 2026-09-18, having asked whether the header and footer
        sections were present at all: "I think we need a special container for
        the persistent cues that can be folded or expanded". They were present
        as rows and not as frames, which is why they did not read as sections.

        A BAND IS A ROW, in the same flat list as the cues - the page's answer,
        and for the page's reason: there is nothing around a section to put a
        border on, so the frame is drawn by the rows themselves. */
    Rig rig { "phase4" };
    const auto snapshot = rig.publish (0);

    const auto listId = model::readTransport (*snapshot).listId;
    REQUIRE_FALSE (listId.empty());

    model::ShowModel show;
    REQUIRE (show.refresh (*snapshot, listId));

    const auto bandsIn = [] (const model::ShowModel& model)
    {
        std::vector<model::Row> found;

        for (const auto& row : model.rows())
            if (row.rowKind == model::RowKind::band)
                found.push_back (row);

        return found;
    };

    const auto bands = bandsIn (show);
    REQUIRE_FALSE (bands.empty());

    //  phase4 has a footer inside its group and a persistent section on the list.
    bool sawPersistent = false, sawFooter = false;

    for (const auto& band : bands)
    {
        CHECK (band.count > 0);                       // an empty section is not framed at all
        CHECK_FALSE (band.bandKey.empty());
        CHECK_FALSE (band.mayPark());                 // a band is not a cue and takes no pointer

        if (band.section == model::Section::persistent) sawPersistent = true;
        if (band.section == model::Section::footer)     sawFooter = true;
    }

    CHECK (sawPersistent);
    CHECK (sawFooter);

    /*  SHUTTING ONE HIDES ITS ROWS AND KEEPS ITS HEAD, so the count is still
        there to say how much is hidden. */
    const auto persistent = std::find_if (bands.begin(), bands.end(),
                                          [] (const model::Row& b)
                                          { return b.section == model::Section::persistent; });
    REQUIRE (persistent != bands.end());

    const auto before = show.rows().size();

    show.toggle (persistent->bandKey);
    CHECK (show.isShut (persistent->bandKey));

    //  A fold is a reason to rebuild that the revision cannot express.
    CHECK (show.refresh (*snapshot, listId));
    CHECK (show.rows().size() == before - persistent->count);

    const auto shutBands = bandsIn (show);
    const auto stillThere = std::find_if (shutBands.begin(), shutBands.end(),
                                          [&] (const model::Row& b)
                                          { return b.bandKey == persistent->bandKey; });

    REQUIRE (stillThere != shutBands.end());
    CHECK (stillThere->shut);
    CHECK (stillThere->count == persistent->count);   // it still says how much is hidden

    //  And opening it again puts them back.
    show.toggle (persistent->bandKey);
    CHECK_FALSE (show.isShut (persistent->bandKey));
    CHECK (show.refresh (*snapshot, listId));
    CHECK (show.rows().size() == before);
}

//==============================================================================
TEST_CASE ("client: the inspector is built from the tree, in the order somebody works in")
{
    /*  §14.2's generic inspector, which is what lets a row added to the
        parameter table appear in this window with no line written here. The
        order is the page's, settled by the author with the page open
        (2026-09-16), because a window that re-argued where `preWait` goes
        would be two clients disagreeing about one panel. */
    Rig rig { "groups" };
    const auto snapshot = rig.publish (0);

    const auto panel = model::inspect (*snapshot, "B3N8R5TW");   // "House to half"

    REQUIRE_FALSE (panel.empty());
    CHECK (panel.cueId == "B3N8R5TW");
    CHECK (panel.cueName == "House to half");
    CHECK_FALSE (panel.kind.empty());

    /*  THE BLOCKS, in the order somebody fills them in - and never the tree's
        alphabet, which puts `postWait` above `preWait` and `duration` under
        another owner word entirely. */
    std::vector<std::string> headings;

    for (const auto& block : panel.blocks)
        headings.push_back (block.heading);

    const auto placeOf = [&headings] (const std::string& heading)
    {
        const auto found = std::find (headings.begin(), headings.end(), heading);
        return found == headings.end() ? headings.size()
                                       : static_cast<std::size_t> (found - headings.begin());
    };

    CHECK (placeOf ("what it is") < placeOf ("when"));
    CHECK (placeOf ("when") < placeOf ("in the list"));

    //  And within the timing block: before, how long, after.
    for (const auto& block : panel.blocks)
    {
        if (block.heading != "when")
            continue;

        std::vector<std::string> names;

        for (const auto& field : block.fields)
            names.push_back (field.name);

        const auto at = [&names] (const std::string& name)
        {
            const auto found = std::find (names.begin(), names.end(), name);
            return found == names.end() ? names.size()
                                        : static_cast<std::size_t> (found - names.begin());
        };

        CHECK (at ("preWait") < at ("postWait"));
    }

    /*  WHAT IS A DECISION AND WHAT IS THE ENGINE ANSWERING BACK is decided by
        the node's own ACCESS and never by a list of names here, so a row that
        becomes writable leaves the fold by itself. */
    for (const auto& block : panel.blocks)
        for (const auto& field : block.fields)
        {
            CHECK (field.writable);
            CHECK (field.address.rfind ("/godot/cue/B3N8R5TW/", 0) == 0);
            CHECK (field.address == "/godot/cue/B3N8R5TW/" + field.name);
        }

    CHECK_FALSE (panel.details.empty());          // kind, parent, index, role, prepare…

    for (const auto& field : panel.details)
        CHECK_FALSE (field.writable);

    //  Only this cue's own rows: nothing from a cue that merely shares a prefix.
    for (const auto& field : panel.details)
        CHECK (field.name.find ('/') == std::string::npos);

    //  A cue nobody named inspects to nothing rather than to a panel of blanks.
    CHECK (model::inspect (*snapshot, "").empty());
    CHECK (model::inspect (*snapshot, "ZZZZZZZZ").empty());
}

//==============================================================================
TEST_CASE ("client: which control asks a field is the node's answer, twice by name")
{
    /*  The two exceptions are the whole of the special-casing in this panel,
        and both are here so that adding a third has to be argued for. */
    Rig rig { "phase4" };
    const auto snapshot = rig.publish (0);

    const auto panel = model::inspect (*snapshot, "P4MED001");   // "The bed", a media cue

    REQUIRE_FALSE (panel.empty());
    CHECK (panel.kind == "media");

    const auto controlOf = [&panel] (const std::string& name)
    {
        for (const auto& block : panel.blocks)
            for (const auto& field : block.fields)
                if (field.name == name)
                    return field.control;

        for (const auto& field : panel.details)
            if (field.name == name)
                return field.control;

        return model::Control::text;
    };

    /*  A NAME ON A DISK IS SOMETHING A MACHINE CAN BE ASKED TO FIND, which is
        decision Y's control and the one thing the page cannot offer. It stays
        a text field underneath and sends the same `node.set`. */
    CHECK (controlOf ("file") == model::Control::file);

    //  And everything else is still decided by the node itself.
    CHECK (controlOf ("level") == model::Control::text);       // a number, with a range
    CHECK (controlOf ("enabled") == model::Control::toggle);   // a `T` row is a switch
    CHECK (controlOf ("kind") == model::Control::text);        // read-only: nothing to ask

    for (const auto& field : panel.blocks.front().fields)
        if (field.name == "file")
            CHECK (field.writable);
}

//==============================================================================
TEST_CASE ("client: a group folds like a section does, and a fold is a reason to rebuild")
{
    /*  THE CASE THE AUTHOR'S SESSION BOUGHT (2026-09-18: "the containers for
        the groups, headers and footers don't collapse. They're always
        expanded"). The model was folding correctly and the VIEW never noticed,
        because it keyed its rows on the show's revision - and a fold does not
        move that, nor should it: collapsing a section is not a change to the
        show. `rebuilds()` is what moves for every reason the rows can change,
        which is why the view keys on it now and why this case counts walks. */
    Rig rig { "groups" };
    const auto snapshot = rig.publish (0);

    const auto listId = model::readTransport (*snapshot).listId;
    model::ShowModel show;

    REQUIRE (show.refresh (*snapshot, listId));

    const auto groupOf = [] (const model::ShowModel& model) -> model::Row
    {
        for (const auto& row : model.rows())
            if (row.isGroup)
                return row;

        return {};
    };

    const auto group = groupOf (show);
    REQUIRE (group.isGroup);
    CHECK (group.bandKey == group.id);          // a group folds by its own identifier
    CHECK_FALSE (group.shut);

    const auto openRows = show.rows().size();
    const auto walksBefore = show.rebuilds();

    show.toggle (group.id);
    CHECK (show.isShut (group.id));

    //  The fold is a reason to rebuild that the revision cannot express.
    CHECK (show.refresh (*snapshot, listId));
    CHECK (show.rebuilds() == walksBefore + 1);
    CHECK (show.rows().size() < openRows);      // its members and their sections are gone

    //  The group itself stays, and says it is shut.
    const auto stillThere = groupOf (show);
    CHECK (stillThere.id == group.id);
    CHECK (stillThere.shut);

    //  Nothing inside it is drawn while it is shut.
    for (const auto& row : show.rows())
        CHECK (row.parent != group.id);

    //  And opening it puts them all back.
    show.toggle (group.id);
    CHECK (show.refresh (*snapshot, listId));
    CHECK (show.rows().size() == openRows);
}

//==============================================================================
TEST_CASE ("client: an import names its cue after the file, and finds what the create made")
{
    /*  DECISION Y's OWN CASE (§14.16): a browser is never told a dropped
        file's path, so importing media is the one thing the page cannot be
        given later - and the reason this client is compiled rather than
        served. What is testable without a window is the naming and the
        finding; the copying is the window's and is written down as untestable. */
    CHECK (model::cueNameFor ("Thunder.wav") == "Thunder");
    CHECK (model::cueNameFor ("03 Distant road.aiff") == "03 Distant road");
    CHECK (model::cueNameFor ("no-extension") == "no-extension");
    CHECK (model::cueNameFor (".hidden") == ".hidden");        // nothing before the dot
    CHECK (model::cueNameFor ("trailing.") == "trailing.");    // nothing after it
    CHECK (model::cueNameFor ("two.dots.wav") == "two.dots");  // the LAST dot

    /*  THE NAME, NEVER THE PATH: `media/@file` is relative to the bundle's
        media folder, because a show travels and an absolute path is a fact
        about the machine it was authored on. */
    CHECK (model::mediaNameFor ("C:/sounds/Thunder.wav") == "Thunder.wav");
    CHECK (model::mediaNameFor ("/home/po/sounds/Thunder.wav") == "Thunder.wav");
    CHECK (model::mediaNameFor ("Thunder.wav") == "Thunder.wav");

    /*  AND WHICH CUE A CREATE MADE, found by the member position it was asked
        for rather than by diffing before against after. */
    CHECK (model::createdAt ("A B C", 0) == "A");
    CHECK (model::createdAt ("A B C", 2) == "C");
    CHECK (model::createdAt ("A B C", 9) == "C");      // past the end means the end
    CHECK (model::createdAt ("A B C", -1).empty());
    CHECK (model::createdAt ("", 0).empty());          // nothing to answer with

    /*  AND THE GUARD THAT STOPS A FILE LANDING ON A STRANGER. A member
        position alone does not say the cue standing there is the one this
        import made: the show has other clients, and somebody inserting from
        the page in the same two hundred milliseconds would put a stranger
        exactly where the import is looking. Three things must agree. */
    const model::Import job { "L1", 3, "Thunder", "Thunder.wav", 7, 0 };

    CHECK (model::madeByImport (job, "media", "Thunder", ""));
    CHECK_FALSE (model::madeByImport (job, "group", "Thunder", ""));     // not a media cue
    CHECK_FALSE (model::madeByImport (job, "media", "Rain", ""));        // somebody else's
    CHECK_FALSE (model::madeByImport (job, "media", "Thunder", "Rain.wav"));  // already named
}

//==============================================================================
TEST_CASE ("client: a dragged row lands after, into or on, and a cue can be named by number or name")
{
    /*  model/Reorder.h: the one rule the drawing and the dropping share.
        Rows are built by hand, because what is under test is the arithmetic
        and not the walk. */
    const auto cue = [] (const char* id, const char* kind, const char* parent, int index,
                         const char* number = "", const char* name = "")
    {
        model::Row row;
        row.rowKind = model::RowKind::cue;
        row.id = id;
        row.kind = kind;
        row.parent = parent;
        row.indexInParent = index;
        row.number = number;
        row.name = name;
        row.isGroup = std::string (kind) == "group";
        return row;
    };

    const auto a = cue ("A", "memo", "L", 0, "1", "Thunder");
    const auto b = cue ("B", "fade", "L", 1, "2", "Fade it");
    const auto c = cue ("C", "memo", "L", 2, "3", "Thunder");     // a second Thunder
    const auto g = cue ("G", "group", "L", 3, "4", "Scene");
    const auto inner = cue ("I", "memo", "G", 0, "4.1", "Inside");

    //  AFTER, within one container: the position object.move wants.
    {
        const auto later = model::dropFor (c, a, 0.9);       // A dropped after C: moving later
        CHECK (later.kind == model::DropKind::after);
        CHECK (later.container == "L");
        CHECK (later.index == 2);                             // C's own position: A ends up after C

        const auto earlier = model::dropFor (a, c, 0.9);     // C dropped after A: moving earlier
        CHECK (earlier.kind == model::DropKind::after);
        CHECK (earlier.index == 1);                           // the position after A

        CHECK (model::dropFor (a, b, 0.9).kind == model::DropKind::none);   // B is already after A
        CHECK (model::dropFor (a, a, 0.5).kind == model::DropKind::none);   // onto itself
    }

    //  AFTER, from another container: the position after the row.
    {
        const auto out = model::dropFor (a, inner, 0.9);
        CHECK (out.kind == model::DropKind::after);
        CHECK (out.container == "L");
        CHECK (out.index == 1);
    }

    //  ON a fade aims it; on a group goes inside; the edges of both mean after.
    {
        const auto aim = model::dropFor (b, a, 0.5);
        CHECK (aim.kind == model::DropKind::target);
        CHECK (aim.cueId == "B");

        const auto into = model::dropFor (g, a, 0.5);
        CHECK (into.kind == model::DropKind::into);
        CHECK (into.container == "G");
        CHECK (into.index == -1);

        CHECK (model::dropFor (b, a, 0.1).kind == model::DropKind::after);
        CHECK (model::dropFor (g, a, 0.95).kind == model::DropKind::after);
        CHECK (model::dropFor (a, c, 0.5).kind == model::DropKind::after);   // a memo has no "on"
    }

    //  A band takes the cue INTO its section when the tree names one, and nothing otherwise.
    {
        model::Row band;
        band.rowKind = model::RowKind::band;
        band.section = model::Section::footer;
        band.parent = "G";
        CHECK (model::dropFor (band, a, 0.5).kind == model::DropKind::none);

        band.sectionId = "FOOT";
        const auto into = model::dropFor (band, a, 0.5);
        CHECK (into.kind == model::DropKind::into);
        CHECK (into.container == "FOOT");
        CHECK (into.index == -1);
    }

    //  A row inside a footer is after-able within the footer, and a derived line is not a place.
    {
        auto inFooter = cue ("F1", "memo", "G", 0);
        inFooter.section = model::Section::footer;
        inFooter.sectionId = "FOOT";

        const auto after = model::dropFor (inFooter, a, 0.9);
        CHECK (after.kind == model::DropKind::after);
        CHECK (after.container == "FOOT");
        CHECK (after.index == 1);
        CHECK (model::containerOf (inFooter) == "FOOT");
        CHECK (model::containerOf (a) == "L");

        auto reading = cue ("A", "memo", "G", 0);
        reading.derived = true;
        CHECK (model::dropFor (reading, c, 0.9).kind == model::DropKind::none);

        //  Shift+alt on a group title: into its footer; on a memo: nothing.
        CHECK (model::footerDropFor (g, a).kind == model::DropKind::footer);
        CHECK (model::footerDropFor (g, a).cueId == "G");
        CHECK (model::footerDropFor (a, c).kind == model::DropKind::none);
    }

    //  Said while the hand is in the air, and a timeline group is named as one.
    CHECK (model::describe (model::dropFor (c, a, 0.9), c, false) == "after Thunder");
    CHECK (model::describe (model::dropFor (g, a, 0.5), g, true).rfind ("into Scene - a timeline", 0) == 0);
    CHECK (model::describe (model::dropFor (b, a, 0.5), b, true) == "aim Fade it at it");
    CHECK (model::describe (model::dropFor (a, a, 0.5), a, false).empty());

    /*  AND THE ENGINE LANDS THE CUE WHERE THE LINE WAS DRAWN. The index the
        model computes is handed to the real `object.move`, in a group with a
        header so that raw child indices and member positions differ, and the
        published order is read back: this is the contract between the
        arithmetic above and ShowDocument::move, pinned from the client's side. */
    {
        Rig rig;

        const auto listId = model::readTransport (*rig.publish (0)).listId;
        auto tick = std::int64_t { 1 };

        const auto create = [&] (const std::string& parent, int index, const char* kind, const char* name)
        {
            REQUIRE (rig.apply (tick++, "window", "cue.create",
                                { osc::Value::string (parent), osc::Value::int32 (index),
                                  osc::Value::string (kind), osc::Value::string (name) }).applied == 1);
            return model::createdAt (model::text (*rig.publish (tick),
                                                  (parent == listId ? "/godot/list/" : "/godot/cue/")
                                                    + parent + "/order"), index);
        };

        const auto group = create (listId, 0, "group", "Scene");
        REQUIRE_FALSE (group.empty());
        REQUIRE (rig.apply (tick++, "window", "group.role",
                            { osc::Value::string (group), osc::Value::string ("header") }).applied == 1);

        const auto one = create (group, 0, "memo", "One");
        const auto two = create (group, 1, "memo", "Two");
        const auto three = create (group, 2, "memo", "Three");
        const auto four = create (group, 3, "memo", "Four");
        REQUIRE (model::text (*rig.publish (tick), "/godot/cue/" + group + "/order")
                   == one + " " + two + " " + three + " " + four);

        // Dropping on the group title appends through the actual UI gesture.
        const auto outside = create (listId, 1, "memo", "Outside");
        const auto into = model::dropFor (cue (group.c_str(), "group", listId.c_str(), 0),
                                         cue (outside.c_str(), "memo", listId.c_str(), 1), 0.5);
        REQUIRE (into.kind == model::DropKind::into);
        const auto event = gesture::moveObject (outside, into.container, into.index);
        REQUIRE (rig.apply (tick++, "window", event.command, event.args).applied == 1);
        CHECK (model::text (*rig.publish (tick), "/godot/cue/" + group + "/order")
               == one + " " + two + " " + three + " " + four + " " + outside);
        const auto back = gesture::moveObject (outside, listId, -1);
        REQUIRE (rig.apply (tick++, "window", back.command, back.args).applied == 1);
        rig.document.beginTransaction ("group.wrap", tick, "window", {});
        REQUIRE (rig.apply (tick++, "window", "group.wrap",
                            { osc::Value::string (outside + " " + group),
                              osc::Value::string ("WRAP0001") }).applied == 1);
        CHECK (model::text (*rig.publish (tick), "/godot/cue/WRAP0001/order") == group + " " + outside);
        REQUIRE (rig.apply (tick++, "window", "undo", {}).applied == 1);
        CHECK (model::text (*rig.publish (tick), "/godot/cue/" + group + "/order")
               == one + " " + two + " " + three + " " + four);

        //  Rows as the list would hold them, from the published order.
        const auto rowsOf = [&]
        {
            std::vector<model::Row> built;
            const auto members = model::words (model::text (*rig.publish (tick), "/godot/cue/" + group + "/order"));

            for (std::size_t at = 0; at < members.size(); ++at)
                built.push_back (cue (members[at].c_str(), "memo", group.c_str(), static_cast<int> (at)));

            return built;
        };

        const auto moveBy = [&] (const model::Drop& drop, const std::string& id)
        {
            REQUIRE (drop.kind == model::DropKind::after);
            REQUIRE (rig.apply (tick++, "window", "object.move",
                                { osc::Value::string (id), osc::Value::string (drop.container),
                                  osc::Value::int32 (drop.index) }).applied == 1);
            return model::text (*rig.publish (tick), "/godot/cue/" + group + "/order");
        };

        //  One dropped after Three: moving later lands directly after Three.
        {
            const auto built = rowsOf();
            CHECK (moveBy (model::dropFor (built[2], built[0], 0.9), one) == two + " " + three + " " + one + " " + four);
        }

        //  Four dropped after Two: moving earlier lands directly after Two.
        {
            const auto built = rowsOf();     // two three one four
            CHECK (moveBy (model::dropFor (built[0], built[3], 0.9), four) == two + " " + four + " " + three + " " + one);
        }

        //  One dropped after the last: the end.
        {
            const auto built = rowsOf();     // two four three one -> one is already last: after three is none
            CHECK (model::dropFor (built[2], built[3], 0.9).kind == model::DropKind::none);
            CHECK (moveBy (model::dropFor (built[3], built[0], 0.9), two) == four + " " + three + " " + one + " " + two);
        }
    }

    /*  THE PRESET: alt-drop on a group the cue is inside marks it; a group it
        is not inside is refused here; and the arrows step the mark outward
        and inward through the ancestors, innermost first. */
    {
        const auto outer = cue ("O", "group", "L", 0);
        const auto innerGroup = cue ("I", "group", "O", 0);
        const auto leaf = cue ("X", "memo", "I", 0);
        const auto other = cue ("P", "group", "L", 1);
        const std::vector<model::Row> nested { outer, innerGroup, leaf, other };

        CHECK (model::ancestorsOf ("X", nested) == std::vector<std::string> { "I", "O" });
        CHECK (model::ancestorsOf ("O", nested).empty());

        CHECK (model::presetDropFor (innerGroup, leaf, nested).kind == model::DropKind::preset);
        CHECK (model::presetDropFor (outer, leaf, nested).cueId == "O");
        CHECK (model::presetDropFor (other, leaf, nested).kind == model::DropKind::none);   // not inside it
        CHECK (model::presetDropFor (leaf, leaf, nested).kind == model::DropKind::none);

        //  Up is outward: none -> I -> O -> stays. Down is inward: O -> I -> none -> stays.
        CHECK (model::presetStep ("X", "", +1, nested) == std::optional<std::string> { "I" });
        CHECK (model::presetStep ("X", "I", +1, nested) == std::optional<std::string> { "O" });
        CHECK_FALSE (model::presetStep ("X", "O", +1, nested).has_value());
        CHECK (model::presetStep ("X", "O", -1, nested) == std::optional<std::string> { "I" });
        CHECK (model::presetStep ("X", "I", -1, nested) == std::optional<std::string> { "" });
        CHECK_FALSE (model::presetStep ("X", "", -1, nested).has_value());
        CHECK_FALSE (model::presetStep ("O", "", +1, nested).has_value());    // nothing above it

        /*  A DERIVED LINE DRAGGED: onto another group the cue is inside, or
            its header band, moves the mark; onto the group it names, nothing;
            anywhere else - a member, a stranger group, no row - clears it. */
        model::Row outerHeader;
        outerHeader.rowKind = model::RowKind::band;
        outerHeader.section = model::Section::header;
        outerHeader.parent = "O";

        CHECK (model::presetLineDropFor (&outer, "X", "I", nested).kind == model::DropKind::preset);
        CHECK (model::presetLineDropFor (&outer, "X", "I", nested).cueId == "O");
        CHECK (model::presetLineDropFor (&outerHeader, "X", "I", nested).cueId == "O");
        CHECK (model::presetLineDropFor (&innerGroup, "X", "I", nested).kind == model::DropKind::none);
        CHECK (model::presetLineDropFor (&other, "X", "I", nested).kind == model::DropKind::clearPreset);
        CHECK (model::presetLineDropFor (&leaf, "X", "I", nested).kind == model::DropKind::clearPreset);
        CHECK (model::presetLineDropFor (nullptr, "X", "I", nested).kind == model::DropKind::clearPreset);
        CHECK (model::presetLineDropFor (nullptr, "X", "I", nested).cueId == "X");
    }

    /*  EDITING IN THE LIST: which attribute a column writes, and which column
        is not this cue's to write - a media cue's duration is its file's. */
    CHECK (model::editAttributeFor (model::EditCell::number, "memo") == "number");
    CHECK (model::editAttributeFor (model::EditCell::name, "media") == "name");
    CHECK (model::editAttributeFor (model::EditCell::preWait, "osc") == "preWait");
    CHECK (model::editAttributeFor (model::EditCell::postWait, "group") == "postWait");
    CHECK (model::editAttributeFor (model::EditCell::duration, "fade") == "duration");
    CHECK (model::editAttributeFor (model::EditCell::duration, "stop") == "duration");
    CHECK (model::editAttributeFor (model::EditCell::duration, "media").empty());
    CHECK (model::editAttributeFor (model::EditCell::duration, "memo").empty());
    CHECK (model::editAttributeFor (model::EditCell::none, "memo").empty());

    /*  NAMING A CUE: identifier first, then number, then name - and two cues
        with one name answer nothing rather than one of them. */
    const std::vector<model::Row> rows { a, b, c, g, inner };

    CHECK (model::resolveCueRef ("B", rows) == "B");
    CHECK (model::resolveCueRef ("2", rows) == "B");
    CHECK (model::resolveCueRef ("4.1", rows) == "I");
    CHECK (model::resolveCueRef ("Fade it", rows) == "B");
    CHECK (model::resolveCueRef ("Thunder", rows).empty());    // two of them
    CHECK (model::resolveCueRef ("Nobody", rows).empty());
    CHECK (model::resolveCueRef ("", rows).empty());
}

//==============================================================================
TEST_CASE ("client: a click picks one, shift extends from the anchor, ctrl toggles, and a band is skipped")
{
    /*  model/Selection.h: the client's own state, so every rule of it can be
        pinned with no engine. Rows as the list draws them: four cues with a
        band between the second and third. */
    const auto cue = [] (const char* id)
    {
        model::Row row;
        row.rowKind = model::RowKind::cue;
        row.id = id;
        return row;
    };

    model::Row band;
    band.rowKind = model::RowKind::band;
    band.bandKey = "G:header";

    const std::vector<model::Row> rows { cue ("A"), cue ("B"), band, cue ("C"), cue ("D") };

    model::Selection picked;
    CHECK (picked.empty());

    picked.click ("B", false, false, rows);
    CHECK (picked.ids() == std::vector<std::string> { "B" });
    CHECK (picked.anchor() == "B");

    //  Shift: everything from the anchor to here, the band not among them.
    picked.click ("D", true, false, rows);
    CHECK (picked.ids() == std::vector<std::string> { "B", "C", "D" });
    CHECK (picked.anchor() == "B");                       // the anchor stays

    //  Ctrl on a picked cue takes it out; the anchor follows what is left.
    picked.click ("B", false, true, rows);
    CHECK (picked.ids() == std::vector<std::string> { "C", "D" });
    CHECK (picked.anchor() == "D");

    //  Ctrl on an unpicked cue adds it and makes it the anchor.
    picked.click ("A", false, true, rows);
    CHECK (picked.ids() == std::vector<std::string> { "C", "D", "A" });
    CHECK (picked.anchor() == "A");
    CHECK (picked.contains ("C"));
    CHECK_FALSE (picked.contains ("B"));

    //  A plain click picks that one alone.
    picked.click ("C", false, false, rows);
    CHECK (picked.ids() == std::vector<std::string> { "C" });

    //  All, then the show loses one: it is not picked any more.
    picked.all (rows);
    CHECK (picked.size() == 4u);
    picked.retain ({ cue ("A"), cue ("B"), cue ("D") });
    CHECK (picked.ids() == std::vector<std::string> { "A", "B", "D" });

    picked.set ({});
    CHECK (picked.empty());
    CHECK (picked.anchor().empty());
}

//==============================================================================
TEST_CASE ("client: several cues inspected together show what they share, and say where they differ")
{
    /*  The page's batch-edit rule (§14.3, 5.12), transcribed: the rows every
        picked cue has and may write, the value they agree on or `mixed`, and
        an address per cue so a commit is N writes. */
    Rig rig;

    const auto listId = model::readTransport (*rig.publish (0)).listId;
    auto tick = std::int64_t { 1 };

    const auto create = [&] (const char* kind, const char* name)
    {
        REQUIRE (rig.apply (tick++, "window", "cue.create",
                            { osc::Value::string (listId), osc::Value::int32 (0),
                              osc::Value::string (kind), osc::Value::string (name) }).applied == 1);
        return model::createdAt (model::text (*rig.publish (tick), "/godot/list/" + listId + "/order"), 0);
    };

    const auto one = create ("memo", "One");
    const auto two = create ("memo", "Two");
    const auto group = create ("group", "Scene");
    const auto snapshot = rig.publish (tick);

    const auto fieldNamed = [] (const model::Inspection& in, const std::string& name) -> const model::Field*
    {
        for (const auto& block : in.blocks)
            for (const auto& field : block.fields)
                if (field.name == name)
                    return &field;

        return nullptr;
    };

    //  One cue is the ordinary inspection.
    CHECK (model::inspectMany (*snapshot, { one }).count == 1);
    CHECK (model::inspectMany (*snapshot, {}).empty());

    //  Two memos: names differ, enabled agrees, and every field writes to both.
    const auto twoMemos = model::inspectMany (*snapshot, { one, two });
    CHECK (twoMemos.count == 2);
    CHECK (twoMemos.cueName == "2 cues");
    CHECK (twoMemos.kind == "memo");
    CHECK (twoMemos.details.empty());                     // a report is one cue's

    const auto* name = fieldNamed (twoMemos, "name");
    REQUIRE (name != nullptr);
    CHECK (name->mixed);
    CHECK (name->value.empty());
    REQUIRE (name->addresses.size() == 2u);
    CHECK (name->addresses[0] == "/godot/cue/" + one + "/name");
    CHECK (name->addresses[1] == "/godot/cue/" + two + "/name");

    const auto* enabled = fieldNamed (twoMemos, "enabled");
    REQUIRE (enabled != nullptr);
    CHECK_FALSE (enabled->mixed);
    CHECK (enabled->value == "true");

    //  And notes are the one field written at length, so they get the tall box.
    const auto* notes = fieldNamed (twoMemos, "notes");
    REQUIRE (notes != nullptr);
    CHECK (notes->control == model::Control::longText);

    //  A memo and a group share the cue rows and not the group's own.
    const auto mixedKinds = model::inspectMany (*snapshot, { one, group });
    CHECK (mixedKinds.kind == "memo + group");
    CHECK (fieldNamed (mixedKinds, "name") != nullptr);
    CHECK (fieldNamed (mixedKinds, "mode") == nullptr);
}

//==============================================================================
TEST_CASE ("client: copied cues come back as a fragment, and paste under new names with their references re-pointed")
{
    /*  Copy and paste, end to end through the two commands (author,
        2026-09-18). A memo and a fade aimed at it are copied; the fragment is
        canonical XML; pasting it makes two NEW cues, the fade aimed at the new
        memo and not the old, and the record carries the names drawn. */
    Rig rig;

    const auto listId = model::readTransport (*rig.publish (0)).listId;
    auto tick = std::int64_t { 1 };

    const auto create = [&] (const char* kind, const char* name)
    {
        REQUIRE (rig.apply (tick++, "window", "cue.create",
                            { osc::Value::string (listId), osc::Value::int32 (0),
                              osc::Value::string (kind), osc::Value::string (name) }).applied == 1);
        return model::createdAt (model::text (*rig.publish (tick), "/godot/list/" + listId + "/order"), 0);
    };

    const auto memo = create ("memo", "Thunder");
    const auto fade = create ("fade", "Fade it");
    REQUIRE (rig.apply (tick++, "window", "node.set",
                        { osc::Value::string ("/godot/cue/" + fade + "/target"),
                          osc::Value::string (memo) }).applied == 1);

    //  Copy: the fragment is where the tree would publish it.
    REQUIRE (rig.apply (tick++, "window", "document.copy",
                        { osc::Value::string (memo + " " + fade) }).applied == 1);

    const auto fragment = rig.document.clipboardText();
    CHECK (fragment.rfind ("<Fragment>", 0) == 0);
    CHECK (fragment.find ("name=\"Thunder\"") != std::string::npos);
    CHECK (fragment.find ("target=\"" + memo + "\"") != std::string::npos);

    const auto before = model::words (model::text (*rig.publish (tick), "/godot/list/" + listId + "/order"));

    //  Paste at the end: two new cues, new names, the fade aimed at the new memo.
    const auto outcome = rig.apply (tick++, "window", "document.paste",
                                    { osc::Value::string (listId),
                                      osc::Value::int32 (static_cast<int> (before.size())),
                                      osc::Value::string (fragment) });
    REQUIRE (outcome.applied == 1);

    const auto snapshot = rig.publish (tick);
    const auto after = model::words (model::text (*snapshot, "/godot/list/" + listId + "/order"));
    REQUIRE (after.size() == before.size() + 2);

    const auto newMemo = after[before.size()];
    const auto newFade = after[before.size() + 1];
    CHECK (newMemo != memo);
    CHECK (newFade != fade);
    CHECK (model::text (*snapshot, "/godot/cue/" + newMemo + "/name") == "Thunder");
    CHECK (model::text (*snapshot, "/godot/cue/" + newFade + "/kind") == "fade");
    CHECK (model::text (*snapshot, "/godot/cue/" + newFade + "/target") == newMemo);

    //  The originals are untouched.
    CHECK (model::text (*snapshot, "/godot/cue/" + fade + "/target") == memo);

    //  Text that is not a fragment is refused, and the show does not move.
    CHECK (rig.apply (tick++, "window", "document.paste",
                      { osc::Value::string (listId), osc::Value::int32 (0),
                        osc::Value::string ("hello") }).applied == 0);
    CHECK (model::words (model::text (*rig.publish (tick), "/godot/list/" + listId + "/order")).size()
             == after.size());
}

//==============================================================================
TEST_CASE ("client: a fold is recorded with the show, and a rebuilt list opens folded the way it was left")
{
    /*  The author (2026-09-18): "fold state should be recorded in project
        file." The flag is a state row on the group; the model seeds its fold
        set from it when it rebuilds, so a show opens as it was left. */
    Rig rig { "groups" };

    auto tick = std::int64_t { 1 };
    const auto listId = model::readTransport (*rig.publish (0)).listId;

    model::ShowModel show;
    REQUIRE (show.refresh (*rig.publish (0), listId));

    std::string group;

    for (const auto& row : show.rows())
        if (row.rowKind == model::RowKind::cue && row.isGroup)
        {
            group = row.id;
            break;
        }

    REQUIRE_FALSE (group.empty());
    CHECK (show.foldAddress (group) == "/godot/cue/" + group + "/folded");
    CHECK (show.foldAddress (listId + "/persistent") == "/godot/list/" + listId + "/persistentFolded");
    CHECK (show.foldAddress (group + "/header") == "/godot/cue/" + group + "/headerFolded");

    const auto drawnOpen = show.rows().size();

    //  The flag is a state row, so it is written like the standby is.
    REQUIRE (rig.apply (tick++, "window", "node.set",
                        { osc::Value::string ("/godot/cue/" + group + "/folded"),
                          osc::Value::boolean (true) }).applied == 1);

    //  A state write moves no revision; an edit does, and the rebuild reads the flag.
    REQUIRE (rig.apply (tick++, "window", "node.set",
                        { osc::Value::string ("/godot/cue/" + group + "/name"),
                          osc::Value::string ("Folded away") }).applied == 1);

    model::ShowModel reopened;
    REQUIRE (reopened.refresh (*rig.publish (tick), listId));
    CHECK (reopened.isShut (group));
    CHECK (reopened.rows().size() < drawnOpen);           // its members are not drawn
}

//==============================================================================
TEST_CASE ("client: a cue marked as a group's preset appears in that group's header band, as a reading")
{
    /*  The author (2026-09-18): "the headers are not updated when adding an
        element to them for preloading." The engine publishes the cues whose
        `preset` names a group as `headerDerived`; the model draws them after
        the header's own lines, marked derived, while the cue's own row keeps
        its place and the index. */
    Rig rig { "groups" };

    auto tick = std::int64_t { 1 };
    const auto listId = model::readTransport (*rig.publish (0)).listId;

    model::ShowModel show;
    REQUIRE (show.refresh (*rig.publish (0), listId));

    //  A group, and a member inside it.
    std::string group, member;

    for (const auto& row : show.rows())
    {
        if (row.rowKind != model::RowKind::cue)
            continue;

        if (group.empty() && row.isGroup)
            group = row.id;
        else if (! group.empty() && row.parent == group && row.section == model::Section::member)
        {
            member = row.id;
            break;
        }
    }

    REQUIRE_FALSE (group.empty());
    REQUIRE_FALSE (member.empty());

    REQUIRE (rig.apply (tick++, "window", "node.set",
                        { osc::Value::string ("/godot/cue/" + member + "/preset"),
                          osc::Value::string (group) }).applied == 1);

    REQUIRE (show.refresh (*rig.publish (tick), listId));

    auto derivedRows = 0, ownRows = 0;
    auto bandSeen = false;

    for (const auto& row : show.rows())
    {
        if (row.rowKind == model::RowKind::band && row.parent == group && row.section == model::Section::header)
            bandSeen = true;

        if (row.rowKind == model::RowKind::cue && row.id == member)
        {
            if (row.derived) ++derivedRows;
            else             ++ownRows;
        }
    }

    CHECK (bandSeen);
    CHECK (derivedRows == 1);
    CHECK (ownRows == 1);

    //  The index points at the cue's own row, not the reading of it.
    const auto at = show.indexOf (member);
    REQUIRE (at >= 0);
    CHECK_FALSE (show.rows()[static_cast<std::size_t> (at)].derived);
}

//==============================================================================
TEST_CASE ("client: a group names its header and footer in the tree, and a cue moved into the footer says so")
{
    /*  The footer as a PLACE (author, 2026-09-18): the tree now publishes a
        group's `header` and `footer` identities, so a window can hand
        `object.move` a container it can see. Made with group.role, filled with
        a move, read back through the model as a footer row with its section's
        own identifier. */
    Rig rig;

    const auto listId = model::readTransport (*rig.publish (0)).listId;
    auto tick = std::int64_t { 1 };

    const auto create = [&] (const std::string& parent, int index, const char* kind, const char* name)
    {
        REQUIRE (rig.apply (tick++, "window", "cue.create",
                            { osc::Value::string (parent), osc::Value::int32 (index),
                              osc::Value::string (kind), osc::Value::string (name) }).applied == 1);
        return model::createdAt (model::text (*rig.publish (tick),
                                              (parent == listId ? "/godot/list/" : "/godot/cue/")
                                                + parent + "/order"), index);
    };

    const auto group = create (listId, 0, "group", "Scene");
    const auto cue = create (group, 0, "memo", "Release");

    CHECK (model::text (*rig.publish (tick), "/godot/cue/" + group + "/footer").empty());

    REQUIRE (rig.apply (tick++, "window", "group.role",
                        { osc::Value::string (group), osc::Value::string ("footer") }).applied == 1);

    const auto footer = model::text (*rig.publish (tick), "/godot/cue/" + group + "/footer");
    REQUIRE_FALSE (footer.empty());

    REQUIRE (rig.apply (tick++, "window", "object.move",
                        { osc::Value::string (cue), osc::Value::string (footer), osc::Value::int32 (0) }).applied == 1);

    const auto snapshot = rig.publish (tick);
    CHECK (model::text (*snapshot, "/godot/cue/" + group + "/footerOrder") == cue);

    //  `parent` names the GROUP that holds the footer, as the table says; the section is `footer`'s.
    CHECK (model::text (*snapshot, "/godot/cue/" + cue + "/parent") == group);

    model::ShowModel show;
    REQUIRE (show.refresh (*snapshot, listId));

    const auto at = show.indexOf (cue);
    REQUIRE (at >= 0);
    const auto& row = show.rows()[static_cast<std::size_t> (at)];
    CHECK (row.section == model::Section::footer);
    CHECK (row.sectionId == footer);
    CHECK (row.parent == group);
    CHECK (model::containerOf (row) == footer);
}

//==============================================================================
TEST_CASE ("client: Esc is a stop, Esc again within the window is a kill, counted from the first")
{
    /*  §4.4 gives Esc two readings and only a hand can be read for which one
        was meant, so the reading is made here and each reading is one named
        command. Counted from the FIRST press: three presses inside the window
        are a stop and two kills, which is what a hammered key means. */
    model::Panic panic;

    CHECK_FALSE (panic.press (1000));                                   // the first: stop
    CHECK (panic.press (1000 + model::Panic::doublePressMs));           // just inside: kill
    CHECK (panic.press (1000 + model::Panic::doublePressMs + 100));     // hammered: kill again

    CHECK_FALSE (panic.press (10000));                                  // long after: a stop again
    CHECK_FALSE (panic.press (10000 + model::Panic::doublePressMs + 1)); // just outside: a stop
    CHECK (panic.press (10000 + model::Panic::doublePressMs + 2));       // and now inside the last
}

//==============================================================================
TEST_CASE ("client: the new-cue row offers every kind the engine makes, and lands after the pick")
{
    /*  The author's row of buttons (2026-09-18): "a stable UI for this. Lock
        makes them disappear. It also helps getting started." What can be
        asserted without a window is that every button is a word `cue.create`
        accepts - the list and the engine held together here, so a kind the
        engine grows or drops shows up as a failing case and not as a button
        that is always refused - and where the cue goes. */
    Rig rig;

    const auto listId = model::readTransport (*rig.publish (0)).listId;
    REQUIRE_FALSE (listId.empty());

    auto tick = std::int64_t { 1 };

    for (const auto& kind : model::cueKinds())
    {
        const auto outcome = rig.apply (tick++, "window", "cue.create",
                                        { osc::Value::string (listId), osc::Value::int32 (0),
                                          osc::Value::string (kind), osc::Value::string ("") });
        CHECK_MESSAGE (outcome.applied == 1, kind);
        CHECK_MESSAGE (rig.engine.lastError().empty(), kind << ": " << rig.engine.lastError());
    }

    /*  AFTER THE PICKED CUE, in member positions - which is index + 1, and
        the end when the pick is not among those members: a cue deleted from
        the page a moment ago, or a picture a pass behind. */
    CHECK (model::positionAfter ("A B C", "A") == 1);
    CHECK (model::positionAfter ("A B C", "C") == 3);
    CHECK (model::positionAfter ("A B C", "Z") == -1);
    CHECK (model::positionAfter ("", "A") == -1);
    CHECK (model::positionAfter ("A B C", "") == -1);

    /*  And the guard on finding what the create made: the kind and no name
        yet, because a new cue is made unnamed so that naming it is the next
        thing typed. */
    const model::Creation job { "L1", 2, "fade", 7, 0 };

    CHECK (model::madeByCreate (job, "fade", ""));
    CHECK_FALSE (model::madeByCreate (job, "stop", ""));       // another kind
    CHECK_FALSE (model::madeByCreate (job, "fade", "Lights"));  // somebody else's, named already
}

//==============================================================================
TEST_CASE ("client: each thing a drop would do wears its own colour")
{
    /*  Four gestures land ON a row rather than between two, and every one of
        them lit it the same green - so the hand had to remember which modifier
        it was holding to know which of four quite different things was about
        to happen (author, 2026-09-21: "so the drag and drop has a clear colour
        coding for the user to be sure what they're doing"). */
    CHECK (model::dropTone (model::DropKind::into) == "drop-into");
    CHECK (model::dropTone (model::DropKind::target) == "drop-aim");
    CHECK (model::dropTone (model::DropKind::preset) == "drop-header");
    CHECK (model::dropTone (model::DropKind::footer) == "drop-footer");

    //  All four are told apart, which is the whole point of having them.
    const std::set<std::string> tones
    {
        model::dropTone (model::DropKind::into), model::dropTone (model::DropKind::target),
        model::dropTone (model::DropKind::preset), model::dropTone (model::DropKind::footer)
    };

    CHECK (tones.size() == 4u);

    /*  AND EVERY ONE IS A COLOUR THE THEME REALLY DECLARES: a token nobody
        declared draws magenta, which is the theme's way of shouting, and a
        drop target is not where anybody wants to meet it. */
    model::Theme theme;

    for (const auto& tone : tones)
    {
        INFO (tone);
        CHECK (std::find (model::Theme::colourNames().begin(),
                          model::Theme::colourNames().end(), tone)
                 != model::Theme::colourNames().end());
    }
}

TEST_CASE ("client: the output list reads up the interface, and says which regime the patch is in")
{
    /*  `minimal` declares Main L/R at 0 and Foldback at 2, both stereo and
        both written by hand - so it is packed, and a fresh show following its
        list. */
    Rig rig;
    const auto snapshot = rig.publish (0);

    auto rows = model::readOutputs (*snapshot);
    REQUIRE (rows.size() == 2);
    CHECK (rows[0].name == "Main L/R");
    CHECK (rows[1].name == "Foldback");
    CHECK (rows[0].firstChannel == 0);
    CHECK (rows[1].firstChannel == 2);

    /*  A bus whose `kind` nobody has written is a direct out, which is that
        row's own default and what every show written before the word existed
        means. */
    CHECK (rows[0].kind == "direct");
    CHECK (rows[0].kindWord() == "Direct out");
    CHECK (rows[0].widthWord() == "Stereo");

    //  Counted from one, because an interface and a patch panel are.
    CHECK (rows[0].channelWord() == "1-2");
    CHECK (rows[1].channelWord() == "3-4");
    CHECK (model::outputChannelCount (rows) == 4);

    /*  AND THE ROWS OF THE PATCH MATRIX ARE NAMED AFTER THEM. A matrix whose
        rows read "Output 3" cannot be patched without counting. */
    const auto labels = model::channelLabels (rows, 0);
    REQUIRE (labels.size() == 4);
    CHECK (labels[0] == "Main L/R \xc2\xb7 L");
    CHECK (labels[1] == "Main L/R \xc2\xb7 R");
    CHECK (labels[2] == "Foldback \xc2\xb7 L");

    //  A channel no output claims still says what it is.
    CHECK (model::channelLabels (rows, 6).back() == "Output 6");

    /*  NOTHING HAS PLAYED AND NOBODY HAS PATCHED, so the interface follows the
        list - and the sentence says so, because the two regimes look identical
        and behave completely differently. */
    CHECK_FALSE (model::patchHasSettled (*snapshot));
    CHECK (model::outputRegime (false).find ("follows this list") != std::string::npos);
    CHECK (model::outputRegime (true).find ("keeps the channels") != std::string::npos);
}

TEST_CASE ("client: a patch that is written, or a layout with a hole, has already settled")
{
    Rig rig;

    /*  A written patch is the same fact as the flag, arrived at without it -
        which matters for a show saved before the flag existed. */
    rig.apply (0, "test", "node.set",
               { osc::Value::string ("/godot/audio/outputPatch"), osc::Value::string ("0 1 2 3") });
    CHECK (model::patchHasSettled (*rig.publish (1)));

    /*  And a layout written by hand with a hole in it is a rig: the channels
        are wired, not derived, so telling the designer the patch will follow
        their edits would be a promise the engine does not keep. */
    Rig unpacked ("slots");
    CHECK (model::patchHasSettled (*unpacked.publish (0)));
}

TEST_CASE ("client: a waveform is the engine's analysis bucketed, and never a second analysis")
{
    /*  §3.30's pyramid is what a file SOUNDS like, computed once on the
        analyser thread and halved level by level so a bar of any width can
        read one of them and stop. This file's whole job is picking that level
        and bucketing it; a window that decided for itself what a file looks
        like would be a second answer to a question already answered, and the
        two would drift the first time the ramp moved. */
    audio::TimbrePyramid pyramid;
    pyramid.sampleRate = 48000;
    pyramid.samples = 48000 * 4;

    //  Four levels: 512, 256, 128, 64 frames, as `pyramidOf` builds them.
    for (const auto count : { 512, 256, 128, 64 })
    {
        std::vector<audio::timbre::Frame> level;

        for (auto at = 0; at < count; ++at)
        {
            audio::timbre::Frame frame;
            frame.peak = static_cast<std::uint8_t> (at % 256);
            frame.hue = static_cast<std::uint8_t> ((at * 7) % 256);
            frame.saturation = 200;
            frame.lightness = 128;
            level.push_back (frame);
        }

        pyramid.levels.push_back (std::move (level));
    }

    /*  THE COARSEST LEVEL THAT STILL HAS A FRAME PER COLUMN. A bar that
        silently read level 0 of a three-hour show would still LOOK right and
        would walk a million frames per repaint, which is why this is asserted
        rather than left to the bucketing. */
    CHECK (model::levelFor (pyramid, 64) == 3u);     // exactly the coarsest
    CHECK (model::levelFor (pyramid, 100) == 2u);    // 128 frames is the first that fits
    CHECK (model::levelFor (pyramid, 256) == 1u);   // exactly a frame per column
    CHECK (model::levelFor (pyramid, 512) == 0u);
    CHECK (model::levelFor (pyramid, 5000) == 0u);   // wider than the file: the finest there is

    //  One column per pixel, whatever the level underneath it turns out to be.
    CHECK (model::waveform (pyramid, 200).size() == 200u);
    CHECK (model::waveform (pyramid, 1).size() == 1u);

    /*  NOTHING TO DRAW IS AN EMPTY BAR AND NEVER A FLAT ONE: a file being
        analysed is not a file with no sound in it, and a window that invented
        a line for one would be saying something false about a cue somebody is
        about to fire. */
    CHECK (model::waveform (pyramid, 0).empty());
    CHECK (model::waveform (audio::TimbrePyramid {}, 200).empty());

    //  The loudest frame of a span wins, so a transient survives to the screen.
    const auto wide = model::waveform (pyramid, 2);
    REQUIRE (wide.size() == 2u);
    CHECK (wide[0].peak > 0.0);

    for (const auto& column : model::waveform (pyramid, 128))
    {
        CHECK (column.hue >= 0.0);
        CHECK (column.hue < 360.0);
        CHECK (column.saturation >= 0.0);
        CHECK (column.saturation <= 1.0);
        CHECK (column.peak >= 0.0);
        CHECK (column.peak <= 1.0);
    }
}

TEST_CASE ("client: a playhead needs a length, and a countdown empties")
{
    CHECK (model::playhead (0.0, 10.0) == 0.0);
    CHECK (model::playhead (5.0, 10.0) == 0.5);
    CHECK (model::playhead (20.0, 10.0) == 1.0);     // never past the end
    CHECK (model::playhead (-1.0, 10.0) == 0.0);

    /*  A LENGTH OF NOUGHT IS A CUE IMPORTED IN THIS SESSION - the duration map
        is frozen at load - so the head stays at the left rather than sliding
        across a bar nobody has measured. */
    CHECK (model::playhead (5.0, 0.0) == 0.0);

    /*  AND A COUNTDOWN IS WHAT IS LEFT, not what is done: full is a wait that
        has not started, empty is a cue about to go (author, 2026-09-18). */
    CHECK (model::countdown (2.0, 2.0) == 1.0);
    CHECK (model::countdown (1.0, 2.0) == 0.5);
    CHECK (model::countdown (0.0, 2.0) == 0.0);

    //  An unmeasurable wait reads as waiting rather than as finished.
    CHECK (model::countdown (0.0, 0.0) == 1.0);
}

//==============================================================================
//==============================================================================
TEST_CASE ("client: a scrub is 1:1 in the strip, finer above it, and keeps going at an edge")
{
    /*  The author's design (2026-09-18): 1:1 inside the strip, finer the
        further the pointer goes above or below it, and a pointer pushed
        against the window edge keeps the head sliding. A ninety-minute track
        four hundred pixels wide: 13.5 seconds a pixel. */
    model::Scrub scrub;
    scrub.begin ({ 600.0, 5400.0, 13.5, 40.0, 100.0 });
    REQUIRE (scrub.active());

    //  Ten pixels right inside the strip is ten pixels of the bar.
    CHECK (scrub.moveTo (110.0, 0.0) == doctest::Approx (735.0));
    CHECK (scrub.rate() == doctest::Approx (1.0));

    //  Forty pixels above the strip halves the gearing; eighty quarters it.
    CHECK (scrub.moveTo (120.0, 40.0) == doctest::Approx (735.0 + 67.5));
    CHECK (scrub.rate() == doctest::Approx (0.5));
    CHECK (scrub.moveTo (130.0, 80.0) == doctest::Approx (735.0 + 67.5 + 33.75));
    CHECK (model::rateText (scrub.rate()) == "1/4");

    //  Coming back down is coarse again, and nothing jumped on the way.
    CHECK (scrub.moveTo (120.0, 0.0) == doctest::Approx (735.0 + 67.5 + 33.75 - 135.0));

    //  The floor: a hand at the top of a tall window still moves the head.
    CHECK (model::Scrub::rateFor (10000.0, 40.0) == doctest::Approx (1.0 / 256.0));
    CHECK (model::rateText (1.0 / 256.0) == "1/256");
    CHECK (model::rateText (1.0).empty());

    //  Pushed against the right edge for a second at 1/4: ninety pixels' worth.
    const auto before = scrub.target();
    CHECK (scrub.push (1, 1.0, 80.0) == doctest::Approx (before + 90.0 * 13.5 * 0.25));

    //  Never past the end, never before the start.
    scrub.moveTo (100000.0, 0.0);
    CHECK (scrub.target() == doctest::Approx (5400.0));
    scrub.moveTo (-100000.0, 0.0);
    CHECK (scrub.target() == doctest::Approx (0.0));

    //  A group has no file to run out of.
    model::Scrub open;
    open.begin ({ 10.0, 0.0, 0.1, 26.0, 0.0 });
    CHECK (open.moveTo (100000.0, 0.0) > 5400.0);
}

TEST_CASE ("client: a scrub sends one record per position it settles on, and one on release")
{
    model::Scrub scrub;
    scrub.begin ({ 0.0, 100.0, 1.0, 40.0, 0.0 });

    //  Nothing moved: nothing to send.
    CHECK_FALSE (scrub.due (0.0));

    scrub.moveTo (10.0, 0.0);
    CHECK (scrub.due (0.0));                 // the first move sends at once
    CHECK_FALSE (scrub.due (50.0));          // the same position is not sent twice

    scrub.moveTo (20.0, 0.0);
    CHECK_FALSE (scrub.due (100.0));         // moved, but the interval has not passed
    CHECK (scrub.due (250.0));

    //  Letting go sends what is unsent, and only that.
    CHECK_FALSE (scrub.settle());
    scrub.moveTo (30.0, 0.0);
    CHECK (scrub.settle());

    scrub.end();
    CHECK_FALSE (scrub.active());
    CHECK_FALSE (scrub.due (10000.0));

    //  The clock beside the head.
    CHECK (model::clockText (0.0) == "0:00.0");
    CHECK (model::clockText (75.25) == "1:15.2");
    CHECK (model::clockText (3725.0) == "1:02:05.0");
}

//==============================================================================
TEST_CASE ("client: a group run says whether it can be scrubbed, from its cue's mode")
{
    Rig rig;

    /*  A timeline and a manual sequence side by side, and a run of each read
        back through the rows: the pane offers a scrub on the first alone. */
    const auto listId = rig.document.createList ("Sound").id;
    const auto timeline = rig.document.createCue (listId, 0, "group", "Scene").id;
    const auto manual = rig.document.createCue (listId, 1, "group", "Act").id;
    rig.document.setAttribute ("/godot/cue/" + timeline + "/mode", "timeline");

    rig.runs.create ("RUNTIME1", timeline, "group");
    rig.runs.create ("RUNMANU1", manual, "group");

    const auto rows = model::readRuns (*rig.publish (1));

    auto sawTimed = false, sawManual = false;

    for (const auto& row : rows)
    {
        if (row.id == "RUNTIME1") { sawTimed = true; CHECK (row.timedGroup); }
        if (row.id == "RUNMANU1") { sawManual = true; CHECK_FALSE (row.timedGroup); }
    }

    CHECK (sawTimed);
    CHECK (sawManual);
}

//==============================================================================
TEST_CASE ("client: the list's history is read newest first, and the steps after the aimed cue sit under it")
{
    /*  As `list/history` spells it: newest first, tick:cue:origin. */
    const auto steps = model::readHistory ("900:C3:t 600:B2:g 100:A1:g junk 50:A1");
    REQUIRE (steps.size() == 3u);
    CHECK (steps[0].cue == "C3");
    CHECK (steps[0].tick == 900);
    CHECK (steps[0].origin == 't');
    CHECK (steps[2].cue == "A1");
    CHECK (model::originWord ('g') == "GO");
    CHECK (model::originWord ('f') == "by name");

    /*  Aimed at A1, ten seconds in: the instant is tick 600, so B2 (at +10 s)
        is in force and C3 (at +16 s) is one a load would undo. */
    const auto lines = model::stepsUnder (steps, "A1", 600);
    REQUIRE (lines.size() == 2u);
    CHECK (lines[0].cue == "B2");
    CHECK (lines[0].offset == doctest::Approx (10.0));
    CHECK_FALSE (lines[0].undone);
    CHECK (lines[1].cue == "C3");
    CHECK (lines[1].offset == doctest::Approx (16.0));
    CHECK (lines[1].undone);

    //  A cue never fired has no clock to place the others on.
    CHECK (model::stepsUnder (steps, "Z9", 600).empty());

    /*  The rows under the cue: the pointer among the steps where its offset
        falls, first when the aim is "before". */
    const std::map<std::string, std::string> names { { "B2", "Rain" }, { "C3", "Thunder" } };
    const auto rows = model::stepRows (lines, 12.0, names, 1);
    REQUIRE (rows.size() == 3u);
    CHECK (rows[0].rowKind == model::RowKind::step);
    CHECK (rows[0].id == "B2");
    CHECK (rows[0].name == "Rain");
    CHECK (rows[0].number == "GO");
    CHECK (rows[1].pointer);
    CHECK (rows[1].name == "+0:12.0");
    CHECK (rows[2].id == "C3");
    CHECK (rows[2].undone);
    CHECK_FALSE (rows[2].enabled);

    const auto before = model::stepRows (lines, -1.0, names, 0);
    REQUIRE (before.size() == 3u);
    CHECK (before[0].pointer);
    CHECK (before[0].name == "before");

    CHECK (model::agoText (100, 600) == "10 s ago");
    CHECK (model::agoText (0, 50 * 130) == "2 min ago");
}

TEST_CASE ("client: the load-to-time reading is the engine's answer, names and all")
{
    Rig rig;
    cue::Focus focus;
    doc::IdRegistry runIds { doc::IdRegistry::withSeed (5) };
    cue::Runner runner { rig.document, rig.runs, runIds, focus };
    cue::registerCueCommands (rig.engine.commands(), rig.document, focus);
    cue::registerRunCommands (rig.engine.commands(), rig.runs);
    cue::registerGoCommands (rig.engine.commands(), rig.engine, runner, rig.document, focus, runIds);
    rig.parameters.setListState (&runner.listState());

    const auto listId = rig.document.createList ("Sound").id;
    const auto thunder = rig.document.createCue (listId, 0, "media", "Thunder").id;
    const auto rain = rig.document.createCue (listId, 1, "media", "Rain").id;
    rig.document.setAttribute (cue::standbyAddressOf (listId), thunder);

    //  Two GOs two seconds apart, then an aim one second into the rain. The
    //  bundle has a list of its own, so the new one is focused first.
    rig.apply (5, "window", "list.focus", { osc::Value::string (listId) });
    rig.apply (10, "window", "go");
    rig.apply (110, "window", "go");
    rig.apply (120, "window", "list.aim",
               { osc::Value::string (listId), osc::Value::string (rain), osc::Value::float64 (1.0) });

    const auto reading = model::readLoadToTime (*rig.publish (130), listId);
    CHECK (reading.listName == "Sound");
    CHECK (reading.tick == 130);
    REQUIRE (reading.steps.size() == 2u);
    CHECK (reading.steps[0].cue == rain);
    CHECK (reading.steps[0].tick == 110);
    CHECK (reading.aimed);
    CHECK (reading.aimCue == rain);
    CHECK (reading.aimOffset == doctest::Approx (1.0));
    CHECK (reading.ok);
    CHECK (reading.how == "history");
    CHECK (reading.instant == 160);
    CHECK (reading.nameOf (thunder) == "Thunder");
    CHECK (reading.nameOf (rain) == "Rain");

    //  The thunder is three seconds in, by the clock the steps kept.
    auto sawThunder = false;

    for (const auto& line : reading.runs)
        if (line.cue == thunder)
        {
            sawThunder = true;
            CHECK (line.when == "sounding");
            CHECK (line.offset == doctest::Approx (3.0));
        }

    CHECK (sawThunder);

    //  Nothing aimed: nothing answered, and the steps still read.
    rig.apply (140, "window", "list.aim", { osc::Value::string (listId), osc::Value::string (""),
                                            osc::Value::float64 (-1.0) });
    const auto cleared = model::readLoadToTime (*rig.publish (150), listId);
    CHECK_FALSE (cleared.aimed);
    CHECK_FALSE (cleared.ok);
    CHECK (cleared.steps.size() == 2u);
}

//==============================================================================
TEST_CASE ("client: the undo history is a place to stand in, and the diff is what standing elsewhere changed")
{
    /*  As the engine publishes it: Undo's list newest first, Redo's nearest
        first. Three applied, two undone. */
    model::UndoReading reading;
    reading.undo = { "object.move", "cue.create", "node.set" };
    reading.redo = { "object.delete", "node.set" };
    CHECK (reading.position() == 3);

    /*  Newest at the top: the two Redo would put back, furthest first, then
        the three Undo would unmake, then the show as opened. Each says where
        standing after it is. */
    const auto stack = model::standings (reading);
    REQUIRE (stack.size() == 6u);
    CHECK (stack[0].name == "node.set");      CHECK (stack[0].index == 5); CHECK_FALSE (stack[0].applied);
    CHECK (stack[1].name == "object.delete"); CHECK (stack[1].index == 4); CHECK_FALSE (stack[1].applied);
    CHECK (stack[2].name == "object.move");   CHECK (stack[2].index == 3); CHECK (stack[2].applied);
    CHECK (stack[3].name == "cue.create");    CHECK (stack[3].index == 2);
    CHECK (stack[4].name == "node.set");      CHECK (stack[4].index == 1);
    CHECK (stack[5].opening);                 CHECK (stack[5].index == 0);

    /*  The picture and the diff: a rename is a change, a cue that is not in
        the picture is new, one that is gone is named. Bands and step rows
        are not cues and are not in it. */
    model::Row a; a.id = "A1"; a.name = "Thunder"; a.kind = "media"; a.number = "1";
    model::Row b; b.id = "B2"; b.name = "Rain"; b.kind = "media"; b.number = "2";
    model::Row band; band.rowKind = model::RowKind::band; band.bandKey = "x";
    model::Row step; step.rowKind = model::RowKind::step; step.id = "A1";

    const auto before = model::pictureOf ({ band, a, b, step });
    CHECK (before.saying.size() == 2u);

    auto a2 = a; a2.name = "Thunder, louder";
    model::Row c; c.id = "C3"; c.name = "Wind"; c.kind = "media";

    const auto now = model::pictureOf ({ a2, c });
    const auto changes = model::diff (before, now);
    CHECK (changes.changed == std::vector<std::string> { "A1" });
    CHECK (changes.added == std::vector<std::string> { "C3" });
    CHECK (changes.removed == std::vector<std::string> { "Rain" });
    CHECK_FALSE (changes.empty());
    CHECK (model::diff (before, before).empty());

    //  A move is a change too: the picture holds where a cue stands.
    auto moved = b; moved.indexInParent = 4;
    CHECK (model::diff (before, model::pictureOf ({ a, moved })).changed
             == std::vector<std::string> { "B2" });
}

TEST_CASE ("client: the undo history is read from the two nodes the engine publishes")
{
    Rig rig;

    EngineState state;
    state.version = "test";
    state.tick = 7;
    state.sampleRate = 48000;
    state.blockSize = 256;
    state.clock = "dummy";
    state.audioStatus = "running";
    state.documentName = "minimal";
    state.documentRevision = rig.document.showRevision();
    state.documentUndoHistory = "object.move cue.create";
    state.documentRedoHistory = "node.set";

    const auto reading = model::readUndoHistory (*rig.parameters.publish (7, state));
    CHECK (reading.undo == std::vector<std::string> { "object.move", "cue.create" });
    CHECK (reading.redo == std::vector<std::string> { "node.set" });
    CHECK (reading.position() == 2);

    //  Nothing published reads as nothing applied.
    const auto none = model::readUndoHistory (*rig.publish (8));
    CHECK (none.undo.empty());
    CHECK (none.redo.empty());
}

TEST_CASE ("client: the running pane reads the way the show happened, not the way runs were made")
{
    /*  THE ENGINE'S ORDER IS THE ORDER RUNS WERE CREATED, and the two differ
        exactly where it matters: a cue the anticipation window prepared is
        created BEFORE the things already sounding, so the run table puts the
        next cue above them - upside down from where an operator looks for it
        (author, 2026-09-18: "the yellow ring marked next cue should always sit
        at the bottom to reflect the structure of the cuelist... order items in
        the active cues by start time"). */
    const auto row = [] (const char* id, const char* parent, std::int64_t started)
    {
        model::RunRow made;
        made.id = id;
        made.parentRun = parent;
        made.started = started;
        return made;
    };

    /*  As the engine holds them: the armed group first, because preparing it
        is what created it, and the thing that is actually sounding after. */
    const std::vector<model::RunRow> asMade
    {
        row ("ARMED", "", 0),          // prepared, not let go: the next cue
        row ("GROUP", "", 10),
        row ("LATE",  "GROUP", 30),
        row ("EARLY", "GROUP", 12),
        row ("FIRST", "", 5),
    };

    std::vector<std::string> order;

    for (const auto& one : model::inShowOrder (asMade))
        order.push_back (one.id);

    /*  What started first is at the top, each parent keeps its children
        directly under it, and the one nobody has let go yet is last. */
    CHECK (order == std::vector<std::string> { "FIRST", "GROUP", "EARLY", "LATE", "ARMED" });

    //  A tie keeps the order the engine made them in, which is document order.
    const std::vector<model::RunRow> together { row ("A", "", 7), row ("B", "", 7) };
    const auto tied = model::inShowOrder (together);

    REQUIRE (tied.size() == 2u);
    CHECK (tied[0].id == "A");

    /*  AND A CHAIN THAT DOES NOT ADD UP IS DRAWN ANYWAY. A client reading a
        tree it did not build does not get to assume the parents resolve; a row
        dropped from this pane is a run somebody cannot kill. */
    const std::vector<model::RunRow> orphaned { row ("X", "NOBODY", 1) };
    CHECK (model::inShowOrder (orphaned).size() == 1u);

    CHECK (model::inShowOrder ({}).empty());
}

//==============================================================================
TEST_CASE ("client: a section is a band and its rows sit one level inside it")
{
    /*  The rule the bracket is drawn from (author, 2026-09-18: "I think the
        header and footer need to have more definition visually"). A section's
        rows used to share their band's depth, so nothing drew them as
        contained and the band had no level to open a rail on. */
    Rig rig { "phase4" };
    const auto snapshot = rig.publish (0);

    model::ShowModel show;
    REQUIRE (show.refresh (*snapshot, "P4ACT001"));

    const model::Row* footerBand = nullptr;
    const model::Row* persistentBand = nullptr;

    for (const auto& row : show.rows())
        if (row.rowKind == model::RowKind::band)
        {
            if (row.section == model::Section::footer)     footerBand = &row;
            if (row.section == model::Section::persistent) persistentBand = &row;
        }

    //  Both sections of this show are drawn, and drawn as bands.
    REQUIRE_MESSAGE (persistentBand != nullptr, "no persistent band in " << show.rows().size()
                                                  << " rows");
    REQUIRE_MESSAGE (footerBand != nullptr, "no footer band in " << show.rows().size() << " rows");

    CHECK (persistentBand->count == 1u);
    CHECK (footerBand->count == 1u);

    //  The persistent section heads the list itself, so its band is outermost.
    CHECK (persistentBand->depth == 0);

    //  The footer belongs to a group one level in, so its band is there too.
    CHECK (footerBand->depth == 1);

    /*  AND EVERY ROW OF A SECTION IS ONE DEEPER THAN ITS BAND, which is what
        gives the band a rail to open and the rows a bracket to hang from. */
    const auto rowsOf = [&show] (model::Section which)
    {
        std::vector<const model::Row*> found;

        for (const auto& row : show.rows())
            if (row.section == which && row.rowKind == model::RowKind::cue)
                found.push_back (&row);

        return found;
    };

    for (const auto* row : rowsOf (model::Section::persistent))
        CHECK (row->depth == persistentBand->depth + 1);

    for (const auto* row : rowsOf (model::Section::footer))
        CHECK (row->depth == footerBand->depth + 1);

    CHECK_FALSE (rowsOf (model::Section::persistent).empty());
    CHECK_FALSE (rowsOf (model::Section::footer).empty());
}

TEST_CASE ("client: cue error history survives retirement and Clear dismisses observed failures")
{
    model::CueErrorLog history;
    model::RunRow failed;
    failed.id = "run-1"; failed.cueId = "cue-1"; failed.cueName = "Thunder";
    failed.state = "failed"; failed.error = "missing-media";
    CHECK (history.observe ({ failed }));
    CHECK_FALSE (history.observe ({ failed }));
    REQUIRE (history.errors().size() == 1);
    CHECK (history.errors()[0].cueName == "Thunder");
    history.clear();
    CHECK_FALSE (history.observe ({ failed }));
    CHECK (history.errors().empty());
    failed.id = "run-2";
    CHECK (history.observe ({ failed }));
    REQUIRE (history.errors().size() == 1);
    CHECK_FALSE (history.observe ({}));
    CHECK (history.errors().size() == 1);
    failed.id = "run-3"; failed.error = "no-track";
    CHECK (history.observe ({ failed }));
    CHECK (history.errors().size() == 2);
    failed.error = "bad-route";
    CHECK (history.observe ({ failed }));
    CHECK (history.errors().size() == 3);
    CHECK (history.errors().back().error == "bad-route");
}
