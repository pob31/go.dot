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

#include <wfg/client/model/Gestures.h>
#include <wfg/client/model/Inspector.h>
#include <wfg/client/model/Media.h>
#include <wfg/client/model/RunModel.h>
#include <wfg/client/model/ShowModel.h>
#include <wfg/client/model/Text.h>
#include <wfg/client/model/Theme.h>
#include <wfg/client/model/Transport.h>
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
        CHECK_MESSAGE ((theme.colour (name) & 0xFFFFFFu) != 0u, name << " is black");
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
        gesture::park ("B3N8R5TW"), gesture::kill ("R4NID001"),
        gesture::setNode ("/godot/cue/B3N8R5TW/name", "Renamed"),
        gesture::createCue ("7K2QM9X4", 0, "media", "Thunder"),
        gesture::undo(), gesture::redo(), gesture::save(), gesture::revert(),
        gesture::recover(), gesture::discardRecovery(),
        gesture::setLocked (true), gesture::setLocked (false),
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
