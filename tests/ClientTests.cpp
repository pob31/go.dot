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

#include <initializer_list>
#include <optional>

#include <wfg/client/model/Curve.h>
#include <wfg/client/model/DirectOuts.h>
#include <wfg/client/model/Fader.h>
#include <wfg/client/model/Gestures.h>
#include <wfg/client/model/Devices.h>
#include <wfg/client/model/MidiPorts.h>
#include <wfg/client/model/Icons.h>
#include <wfg/client/model/Traffic.h>
#include <wfg/engine/osc/OscCodec.h>
#include <wfg/client/model/Inspector.h>
#include <wfg/client/model/LoadToTime.h>
#include <wfg/client/model/Media.h>
#include <wfg/client/model/NewCue.h>
#include <wfg/client/model/NewCueMenus.h>
#include <wfg/client/model/OutputList.h>
#include <wfg/client/model/InputList.h>
#include <wfg/client/model/Foot.h>
#include <wfg/client/model/Panic.h>
#include <wfg/client/model/Ranges.h>
#include <wfg/client/model/View.h>
#include <wfg/client/model/Reorder.h>
#include <wfg/client/model/RunModel.h>
#include <wfg/client/model/Eq.h>
#include <wfg/client/model/FadeMix.h>
#include <wfg/client/model/Fx.h>
#include <wfg/client/model/FxEditor.h>
#include <wfg/client/model/Rack.h>
#include <wfg/engine/plugin/PluginCommands.h>
#include <wfg/client/model/Sends.h>
#include <wfg/client/model/Timeline.h>
#include <wfg/client/model/Scrub.h>
#include <wfg/client/model/Selection.h>
#include <wfg/client/model/ShowModel.h>
#include <wfg/client/model/Surfaces.h>
#include <wfg/client/model/Take.h>
#include <wfg/client/model/Text.h>
#include <wfg/client/model/Theme.h>
#include <wfg/client/model/Transport.h>
#include <wfg/client/model/UndoHistory.h>
#include <wfg/client/model/Waveform.h>
#include <wfg/engine/audio/AudioCommands.h>
#include <wfg/engine/audio/AudioSettings.h>
#include <wfg/engine/audio/Peaks.h>
#include <wfg/engine/audio/Timbre.h>
#include <wfg/engine/Engine.h>
#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/command/Event.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/cue/DcaTable.h>
#include <wfg/engine/cue/DohSetting.h>
#include <wfg/engine/cue/LiveEdits.h>
#include <wfg/engine/cue/LaneCommands.h>
#include <wfg/engine/cue/LaneTable.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/TakeTable.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/DocumentSession.h>
#include <wfg/engine/document/FadePoints.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/LevelLane.h>
#include <wfg/engine/document/DocumentWriter.h>
#include <wfg/engine/surface/SurfaceCommands.h>
#include <wfg/engine/surface/SurfaceTable.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/tree/TreeCommands.h>

#include <juce_core/juce_core.h>

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <utility>
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

    /*  GO IS GREY WITHOUT AUDIO AND SAYS SO UNDER ITS WORD (author,
        2026-09-25; §4.8), and says nothing while the audio runs. */
    CHECK (reading.audioRunning());
    CHECK (reading.goLine().empty());

    reading.status = "stopped";
    CHECK_FALSE (reading.audioRunning());
    CHECK (reading.goLine() == "no audio");

    reading.status = "noClock";
    CHECK (reading.goLine() == "no clock");

    reading.status.clear();
    CHECK_FALSE (reading.audioRunning());
    CHECK (reading.goLine() == "audio —");
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

    row.id = "CUE";
    row.parent = "GROUP";
    CHECK (row.parksOn() == "CUE");

    for (const auto section : { model::Section::header, model::Section::footer,
                                model::Section::persistent })
    {
        row.section = section;
        CHECK_FALSE (row.mayPark());
    }

    /*  BUT A HEADER'S OR A FOOTER'S LINE SENDS ITS GROUP (author, 2026-09-26:
        "move the pointer to the group instead of showing an error"), and only
        a persistent bed, which has no group, is still told why not. */
    row.section = model::Section::header;
    CHECK (row.parksOn() == "GROUP");
    row.section = model::Section::footer;
    CHECK (row.parksOn() == "GROUP");
    row.section = model::Section::persistent;
    CHECK (row.parksOn().empty());

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

    /*  A BOUNCED GO IS SAID IN WORDS, and where to change it (2026-09-28). */
    reading.lastError = "5500 27 window too-soon go";
    CHECK (reading.errorLine() == "GO ignored: too soon after the last one (Show settings > Playback)");

    /*  SEVERAL VALUES REFUSED AT ONE (namespace draft §30.11): the sixth field
        is the address, and the sentence says none was written. */
    reading.lastError = "5501 28 window locked node.setMany /godot/cue/B3N8R5TW/level";
    CHECK (reading.errorLine() == "node.setMany refused: locked at /godot/cue/B3N8R5TW/level"
                                  " - none of its values was written");

    //  An address a client sent may hold a space: the rest of the line is the address, whole.
    reading.lastError = "5502 29 udp:10.0.0.5:9000 bad-address node.setMany /godot/cue/B3N8R5TW/my level";
    CHECK (reading.errorLine() == "node.setMany refused: bad-address at /godot/cue/B3N8R5TW/my level"
                                  " - none of its values was written");
    reading.lastError = "something else entirely";

    /*  THE CLOCK MOVED AND THE SHOW FOLLOWED IT (PRD §6.2, 2026-09-28): said
        until something is refused after it - the cues it stopped are what
        anybody at the desk asks about first - and a refusal from before it is
        older news. The outage itself outranks both. */
    reading.rateMoved = "The interface's clock moved from 48000 Hz to 96000 Hz; the show runs at 96000 Hz, "
                        "and the cues that were playing were stopped.";
    reading.rateMovedTick = "6000";
    CHECK (reading.errorLine() == "something else entirely");     // a record the model cannot date stays visible

    reading.lastError = "5411 26 window not-a-stop standby.set";
    CHECK (reading.errorLine() == reading.rateMoved);

    reading.lastError.clear();
    CHECK (reading.errorLine() == reading.rateMoved);

    reading.lastError = "6021 30 window not-a-stop standby.set";
    CHECK (reading.errorLine() == "standby.set refused: not-a-stop");

    reading.status = "noClock";
    CHECK (reading.errorLine() == "Audio disconnected - cues paused; waiting for the interface and clock.");
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

    /*  And the sandbox's two, so the Plugins tab's restart is a pin like the rest (Phase 9a). */
    plugin::PluginTable pluginTable;
    plugin::registerPluginCommands (rig.engine.commands(), pluginTable, {});

    /*  And the audio settings' commands, where Load now's plugin.load lives
        beside audio.apply (2026-09-26). */
    audio::AudioState audioState;
    audio::registerAudioSettingsCommands (rig.engine, rig.document, runner, rig.runs, audioState);

    /*  And the surfaces' aim and the live layer's two (2026-09-25): the running
        pane's name and the bar's buttons. */
    surface::SurfaceTable surfaces;
    surface::registerSurfaceCommands (rig.engine.commands(), rig.document, surfaces);
    cue::LiveEdits live;
    cue::registerLiveCommands (rig.engine.commands(), rig.document, live);

    /*  And the lanes recorded from the flipped faders (namespace draft §34):
        the waveform's Rec and ✕, and the virtual panel's REC on a lane. */
    cue::LaneTable lanes;
    cue::registerLaneCommands (rig.engine.commands(), rig.engine, runner, rig.document, lanes);

    const std::vector<Event> gestures
    {
        gesture::laneArm ("B3N8R5TW"), gesture::laneArm (""), gesture::laneRec ("level", true),
        gesture::laneFree(), gesture::laneRecord (0.0), gesture::laneRecord (12.5), gesture::laneStop(),

        gesture::go(), gesture::doh(), gesture::standbyNext(), gesture::standbyPrevious(),
        gesture::stopAll(), gesture::killAll(),
        gesture::park ("B3N8R5TW"), gesture::kill ("R4NID001"),
        gesture::seek ("R4NID001", 12.5),
        gesture::aim ("7K2QM9X4", "B3N8R5TW", 12.5), gesture::loadToTime ("7K2QM9X4"),
        gesture::recordStart(), gesture::recordStop(),
        gesture::setNode ("/godot/cue/B3N8R5TW/name", "Renamed"),
        gesture::setNodes ({ { "/godot/cue/B3N8R5TW/name", "Renamed" }, { "/godot/cue/F7HR8TVD/name", "Too" } }),
        gesture::setAll ({ "/godot/cue/B3N8R5TW/preWait", "/godot/cue/F7HR8TVD/preWait" }, "2"),
        gesture::createCue ("7K2QM9X4", 0, "media", "Thunder"),
        gesture::moveObject ("B3N8R5TW", "7K2QM9X4", 0),
        gesture::deleteObject ("B3N8R5TW"),
        gesture::groupRole ("B3N8R5TW", "footer"),
        gesture::listPersistent ("7K2QM9X4"),
        gesture::undo(), gesture::redo(), gesture::save(), gesture::revert(),
        gesture::saveAs ("C:/shows/copy"), gesture::saveAs ("C:/shows/copy", true),
        gesture::copyCues ({ "B3N8R5TW", "F7HR8TVD" }),
        gesture::pasteCues ("7K2QM9X4", 0, "<Fragment/>"),
        gesture::recover(), gesture::discardRecovery(),
        gesture::setLocked (true), gesture::setLocked (false),
        gesture::createBus ("direct", 1, -1), gesture::createBus ("mix", 2, 0),
        gesture::createDevice ("/desk"),
        gesture::deleteBus ("J3MT5XYA"), gesture::moveBus ("J3MT5XYA", 2),
        gesture::setBusWidth ("J3MT5XYA", 2),
        gesture::setPatchSettled (true), gesture::setPatchSettled (false),
        gesture::createRange ("B3N8R5TW", 1.0, 4.0),
        gesture::fireCue ("B3N8R5TW"),
        gesture::splitRange ("B3N8R5TW", 6.0),
        gesture::createSend ("B3N8R5TW", "J3MT5XYA"),
        gesture::createSend ("B3N8R5TW", "J3MT5XYA", -18.5),
        gesture::eqReset ("B3N8R5TW"),
        gesture::createFx ("B3N8R5TW", "PG7N0001"),
        gesture::captureFx ("FX7N0001", "state/PG7N0001-0123456789abcdef.state", "0:0.5 1:0"),
        gesture::createPlugin ("Verb", "VST3-0badf00d-verb", "VST3", "C:/plugins/verb.vst3"),
        gesture::restartPlugin ("PG7N0001"),

        /*  THE APP'S SCAN AND LOAD NOW (2026-09-26): the Plugins tab's Scan,
            its folder, a skipped file's Retry, and Load now. */
        gesture::scanPlugins(), gesture::scanPlugins ("lv2"), gesture::scanPlugins ("", "D:/lv2"),
        gesture::retryScan ("C:/plugins/hangs.vst3"), gesture::loadPlugins(),
        gesture::createPort ("Lights"),

        /*  PHASE 6: the Surfaces tab's three ADD buttons, and the virtual
            panel's pad and fader - the document's creates, the Runner's two
            hand gestures, and the touch table's two. */
        gesture::createSurface ("d700"), gesture::createSurface ("mcu", "The desk"),
        gesture::createStrip ("SVRF0001"),
        gesture::createDca ("Band"),
        gesture::pressStrip ("STRP0001", 100), gesture::pressStrip ("STRP0001", 0),
        gesture::releaseStrip ("STRP0001"),
        gesture::touchNode ("/godot/dca/DCA00001/trim"),
        gesture::releaseNode ("/godot/dca/DCA00001/trim"),

        /*  THE ROTARIES' AIM AND THE LIVE BAR (2026-09-25). */
        gesture::aimSurfaces ("B3N8R5TW"), gesture::aimSurfaces (""),
        gesture::keepLive(), gesture::dropLive(),

        /*  THE MASTER DIAL (2026-09-26): a number clicked, and letting go. */
        gesture::dial ("/godot/cue/B3N8R5TW/level"), gesture::dial (""),
        gesture::setNode ("/godot/cue/B3N8R5TW/eqB2On", "false"),

        /*  PHASE 9b: the Inputs tab's list and its patch rule, and the Rack
            tab's two creates. */
        gesture::createInput (1, -1), gesture::createInput (2, 0),
        gesture::deleteInput ("N1000001"), gesture::moveInput ("N1000001", 1),
        gesture::setInputPatchSettled (true), gesture::setInputPatchSettled (false),
        gesture::createRackChannel ("mono"),
        gesture::createChannelPlugin ("K1000001", "Verb", "VST3-0badf00d-verb", "VST3", "C:/plugins/verb.vst3"),

        /*  THE NEW-CUE LISTS (2026-09-27): a group made around the picked cues,
            and a mic cue born with its input and channel. Every line of the
            fixed lists joins below. */
        gesture::wrapGroup ({ "B3N8R5TW", "P9XKC2WR" }, { { "mode", "timeline" } }),
        gesture::wrapGroup ({ "B3N8R5TW" }, {}),
        gesture::createCue ("7K2QM9X4", 0, "mic", "", { { "input", "N1000001" }, { "channel", "K1000001" } }),
    };

    std::vector<Event> listed = gestures;

    for (const auto* choices : { &model::groupChoices(), &model::transportChoices(), &model::midiChoices() })
        for (const auto& choice : *choices)
        {
            auto settings = choice.settings;

            if (choice.aimed)
                settings.emplace_back ("target", "B3N8R5TW");

            listed.push_back (gesture::createCue ("7K2QM9X4", 0, choice.kind, "", settings));

            if (choice.kind == "group")
                listed.push_back (gesture::wrapGroup ({ "B3N8R5TW" }, settings));
        }

    for (const auto& event : listed)
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

    /*  A HAND WITH NO VELOCITY SENDS NONE, rather than a number nobody struck,
        and a surface nobody named sends no name: in both the engine decides
        what the absence means, and the record says it was absent. */
    const auto struck = gesture::pressStrip ("STRP0001", 100);
    REQUIRE (struck.args.size() == 2);
    CHECK (struck.args[1] == osc::Value::int32 (100));
    CHECK (gesture::pressStrip ("STRP0001", 0).args.size() == 1);
    CHECK (gesture::createSurface ("virtual").args.size() == 1);
    CHECK (gesture::createSurface ("mcu", "The desk").args[1] == osc::Value::string ("The desk"));
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
TEST_CASE ("client: a band with no section behind it makes one rather than taking nothing")
{
    /*  The author, 2026-09-22: "drag and drop to an empty group header or
        footer is not working". An empty section draws no band, so the list
        grows the two bands of whichever group is being dragged over - and
        letting go on one of them has to MAKE the section, because there is
        nothing to move into yet.

        The footer already had that path from a group's title row; the header
        had none at all, which is the half that was missing. */
    model::Row dragged;
    dragged.rowKind = model::RowKind::cue;
    dragged.id = "M3D7ACQE";

    model::Row band;
    band.rowKind = model::RowKind::band;
    band.parent = "GRP00001";
    band.name = "Header";

    SUBCASE ("a header band with no section names the group and the role")
    {
        band.section = model::Section::header;

        const auto drop = model::dropFor (band, dragged, 0.5);

        CHECK (drop.kind == model::DropKind::header);
        CHECK (drop.cueId == "GRP00001");

        //  And it is the same colour the preset mark uses, which is the header's.
        CHECK (model::dropTone (drop.kind) == "drop-header");
    }

    SUBCASE ("and a footer band the same, in the footer's own colour")
    {
        band.section = model::Section::footer;
        band.name = "Footer";

        const auto drop = model::dropFor (band, dragged, 0.5);

        CHECK (drop.kind == model::DropKind::footer);
        CHECK (drop.cueId == "GRP00001");
        CHECK (model::dropTone (drop.kind) == "drop-footer");
    }

    SUBCASE ("a band whose section exists still moves into it, as it always did")
    {
        band.section = model::Section::header;
        band.sectionId = "HDR00001";

        const auto drop = model::dropFor (band, dragged, 0.5);

        CHECK (drop.kind == model::DropKind::into);
        CHECK (drop.container == "HDR00001");
        CHECK (drop.index == -1);
    }

    SUBCASE ("and a group cannot be dropped into its own empty section")
    {
        band.section = model::Section::footer;
        dragged.id = "GRP00001";

        CHECK (model::dropFor (band, dragged, 0.5).kind == model::DropKind::none);
    }

    SUBCASE ("the sentence says which section it would make")
    {
        band.section = model::Section::header;

        model::Row group;
        group.name = "Preshow";

        const auto said = model::describe (model::dropFor (band, dragged, 0.5), group, false);
        CHECK (said == "into Preshow's header");
    }
}

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
TEST_CASE ("client: a list with no persistent section still draws its band, empty, and the band makes the section")
{
    /*  The author, 2026-10-05: "Permanent cues have disappeared." Nothing had
        removed them: a show made in the window has no `<Persistent>` element
        until somebody asks for one, the band was drawn only for a section with
        cues in it, and nothing the window offered ever asked (namespace draft
        §30, S5). `minimal` is such a show. */
    Rig rig;
    auto tick = std::int64_t { 1 };

    const auto listId = model::readTransport (*rig.publish (0)).listId;
    REQUIRE_FALSE (listId.empty());
    REQUIRE (model::text (*rig.publish (0), "/godot/list/" + listId + "/persistent").empty());

    const auto persistentBands = [] (const model::ShowModel& model)
    {
        std::vector<model::Row> found;

        for (const auto& row : model.rows())
            if (row.rowKind == model::RowKind::band && row.section == model::Section::persistent)
                found.push_back (row);

        return found;
    };

    model::ShowModel show;
    REQUIRE (show.refresh (*rig.publish (0), listId));

    //  ONE BAND, AT THE TOP, WHERE IT STANDS WHEN IT HAS CUES - and nothing under it.
    auto bands = persistentBands (show);
    REQUIRE (bands.size() == 1u);
    REQUIRE_FALSE (show.rows().empty());

    const auto& top = show.rows().front();
    CHECK (top.rowKind == model::RowKind::band);
    CHECK (top.section == model::Section::persistent);
    CHECK (top.depth == 0);
    CHECK (top.count == 0u);
    CHECK (top.parent == listId);
    CHECK (top.sectionId.empty());                       // no section yet, so nothing to name
    CHECK (top.bandKey == listId + "/persistent");
    CHECK_FALSE (top.mayPark());
    CHECK (top.parksOn().empty());

    //  It says what it is for, in place of a count of nothing.
    CHECK (model::emptyBandWords (top) == "drop media, mic, OSC or MIDI cues here to keep them running all show");

    //  The always-drawn band is the persistent one alone: the group's empty header and footer stay unframed.
    for (const auto& row : show.rows())
        if (row.rowKind == model::RowKind::band)
            CHECK (row.section == model::Section::persistent);

    //  Every cue row is still where the index says, under the band.
    for (std::size_t at = 0; at < show.rows().size(); ++at)
        if (show.rows()[at].rowKind == model::RowKind::cue)
            CHECK (show.indexOf (show.rows()[at].id) == static_cast<int> (at));

    /*  ITS FOLD WORKS AS ANY BAND'S, with nothing to hide: shut, it is still
        drawn and still the only extra row. */
    const auto drawn = show.rows().size();

    show.toggle (top.bandKey);
    REQUIRE (show.refresh (*rig.publish (0), listId));
    bands = persistentBands (show);
    REQUIRE (bands.size() == 1u);
    CHECK (bands.front().shut);
    CHECK (show.rows().size() == drawn);
    CHECK (show.foldAddress (bands.front().bandKey) == "/godot/list/" + listId + "/persistentFolded");

    show.toggle (bands.front().bandKey);
    REQUIRE (show.refresh (*rig.publish (0), listId));
    CHECK_FALSE (persistentBands (show).front().shut);

    //  And the flag a fold writes is taken with no section behind it, and seeds a fresh model shut.
    REQUIRE (rig.apply (tick++, "window", "node.set",
                        { osc::Value::string ("/godot/list/" + listId + "/persistentFolded"),
                          osc::Value::boolean (true) }).applied == 1);

    {
        model::ShowModel reopened;
        REQUIRE (reopened.refresh (*rig.publish (tick), listId));
        CHECK (reopened.isShut (listId + "/persistent"));
        REQUIRE (persistentBands (reopened).size() == 1u);
    }

    REQUIRE (rig.apply (tick++, "window", "node.set",
                        { osc::Value::string ("/godot/list/" + listId + "/persistentFolded"),
                          osc::Value::boolean (false) }).applied == 1);

    /*  A DROP ON IT ASKS FOR THE SECTION, and the engine makes it. An OSC cue,
        which the section keeps running; the memo beside it is refused. */
    REQUIRE (rig.apply (tick++, "window", "cue.create",
                        { osc::Value::string (listId), osc::Value::int32 (0),
                          osc::Value::string ("osc"), osc::Value::string ("Desk scene") }).applied == 1);
    const auto oscCue = model::createdAt (model::text (*rig.publish (tick), "/godot/list/" + listId + "/order"), 0);
    REQUIRE_FALSE (oscCue.empty());

    REQUIRE (show.refresh (*rig.publish (tick), listId));
    const auto oscAt = show.indexOf (oscCue);
    REQUIRE (oscAt >= 0);
    const auto oscRow = show.rows()[static_cast<std::size_t> (oscAt)];
    const auto memoAt = show.indexOf ("B3N8R5TW");
    REQUIRE (memoAt >= 0);
    const auto memoRow = show.rows()[static_cast<std::size_t> (memoAt)];

    CHECK (model::dropFor (show.rows().front(), memoRow, 0.5).kind == model::DropKind::none);

    const auto make = model::dropFor (show.rows().front(), oscRow, 0.5);
    REQUIRE (make.kind == model::DropKind::persistent);
    CHECK (make.container == listId);

    const auto ask = gesture::listPersistent (make.container);
    REQUIRE (rig.apply (tick++, ask.origin, ask.command, ask.args).applied == 1);

    const auto section = model::text (*rig.publish (tick), "/godot/list/" + listId + "/persistent");
    REQUIRE_FALSE (section.empty());

    //  The pass that sees the section: the band names it, still empty, and a drop is now a plain move into it.
    REQUIRE (show.refresh (*rig.publish (tick), listId));
    REQUIRE (persistentBands (show).size() == 1u);
    CHECK (show.rows().front().sectionId == section);
    CHECK (show.rows().front().count == 0u);
    CHECK_FALSE (model::emptyBandWords (show.rows().front()).empty());

    const auto into = model::dropFor (show.rows().front(), oscRow, 0.5);
    REQUIRE (into.kind == model::DropKind::into);
    CHECK (into.container == section);

    const auto move = gesture::moveObject (oscCue, into.container, 0);
    REQUIRE (rig.apply (tick++, move.origin, move.command, move.args).applied == 1);
    CHECK (model::text (*rig.publish (tick), "/godot/list/" + listId + "/persistentOrder") == oscCue);

    //  And the band is an ordinary one again: a count, no words, the cue one level in under it.
    REQUIRE (show.refresh (*rig.publish (tick), listId));
    CHECK (show.rows().front().count == 1u);
    CHECK (model::emptyBandWords (show.rows().front()).empty());
    REQUIRE (show.indexOf (oscCue) == 1);
    CHECK (show.rows()[1].section == model::Section::persistent);
    CHECK (show.rows()[1].depth == 1);

    //  Asking twice answers with the one it has, so a stale second drop makes nothing new.
    REQUIRE (rig.apply (tick++, ask.origin, ask.command, ask.args).applied == 1);
    CHECK (model::text (*rig.publish (tick), "/godot/list/" + listId + "/persistent") == section);
}

TEST_CASE ("client: the persistent band takes what the section keeps running, and refuses the rest in words")
{
    /*  Decision S (§3.29), and the engine's own rule since Phase 9b: media,
        mic, OSC and MIDI are what a persistent section re-asserts. The engine
        TAKES the others - a fade, a stop, a group - and ignores them with a
        validate warning; this is where the window says so before anybody
        lets go (namespace draft §30, S5). */
    const auto cueOf = [] (const char* id, const char* kind, const char* name)
    {
        model::Row row;
        row.rowKind = model::RowKind::cue;
        row.id = id;
        row.kind = kind;
        row.name = name;
        row.parent = "7K2QM9X4";
        row.isGroup = std::string (kind) == "group";
        return row;
    };

    model::Row band;
    band.rowKind = model::RowKind::band;
    band.section = model::Section::persistent;
    band.parent = "7K2QM9X4";
    band.name = "persistent";
    band.bandKey = "7K2QM9X4/persistent";

    SUBCASE ("with no section yet, a kept kind asks for one, named for the list")
    {
        for (const auto* kind : { "media", "mic", "osc", "midi" })
        {
            CAPTURE (kind);
            const auto drop = model::dropFor (band, cueOf ("MED1A001", kind, "Bed"), 0.5);

            CHECK (drop.kind == model::DropKind::persistent);
            CHECK (drop.container == "7K2QM9X4");
            CHECK (drop.refused.empty());
        }

        const auto drop = model::dropFor (band, cueOf ("MED1A001", "media", "Bed"), 0.5);

        //  It lights the band as a move into a section does, and says so.
        CHECK (model::dropTone (drop.kind) == "drop-into");
        CHECK (model::describe (drop, band, false) == "into the persistent section");

        //  Anywhere on the band: its top and its bottom are the band too.
        CHECK (model::dropFor (band, cueOf ("MED1A001", "media", "Bed"), 0.05).kind == model::DropKind::persistent);
        CHECK (model::dropFor (band, cueOf ("MED1A001", "media", "Bed"), 0.95).kind == model::DropKind::persistent);
    }

    SUBCASE ("anything else is refused, and the sentence says why and names the cue")
    {
        for (const auto* kind : { "memo", "fade", "transport", "group", "start" })
        {
            CAPTURE (kind);
            const auto drop = model::dropFor (band, cueOf ("F4DE0001", kind, "Lights down"), 0.5);

            CHECK (drop.kind == model::DropKind::none);
            CHECK (drop.refused == "only media, mic, OSC and MIDI cues can be persistent, so Lights down"
                                   " stays where it is");

            //  The sentence is what `describe` says for it, so the list has one place to ask.
            CHECK (model::describe (drop, band, false) == drop.refused);
        }

        //  A cue with no name is named by its identifier.
        CHECK (model::dropFor (band, cueOf ("F4DE0001", "fade", ""), 0.5).refused.find ("so F4DE0001 stays")
                 != std::string::npos);
    }

    SUBCASE ("with the section made, a kept kind moves into it and anything else is still refused")
    {
        band.sectionId = "PRS00001";

        const auto drop = model::dropFor (band, cueOf ("MED1A001", "media", "Bed"), 0.5);
        CHECK (drop.kind == model::DropKind::into);
        CHECK (drop.container == "PRS00001");
        CHECK (drop.index == -1);
        CHECK (model::describe (drop, band, false) == "into the persistent section");

        CHECK (model::dropFor (band, cueOf ("F4DE0001", "fade", "Lights down"), 0.5).kind
                 == model::DropKind::none);
    }

    SUBCASE ("after one of the section's rows, the same rule")
    {
        auto bed = cueOf ("MED1A002", "media", "Rain");
        bed.section = model::Section::persistent;
        bed.sectionId = "PRS00001";
        bed.depth = 1;
        bed.indexInParent = 0;

        const auto kept = model::dropFor (bed, cueOf ("MED1A001", "osc", "Desk"), 0.9);
        CHECK (kept.kind == model::DropKind::after);
        CHECK (kept.container == "PRS00001");
        CHECK (kept.index == 1);

        const auto refused = model::dropFor (bed, cueOf ("F4DE0001", "fade", "Lights down"), 0.9);
        CHECK (refused.kind == model::DropKind::none);
        CHECK_FALSE (refused.refused.empty());
    }

    SUBCASE ("what is already in the section can be reordered there, whatever it is")
    {
        band.sectionId = "PRS00001";

        auto stray = cueOf ("F4DE0001", "fade", "Lights down");
        stray.section = model::Section::persistent;
        stray.sectionId = "PRS00001";

        CHECK (model::dropFor (band, stray, 0.5).kind == model::DropKind::into);
    }

    SUBCASE ("a group's empty header and footer are untouched by the rule")
    {
        model::Row header;
        header.rowKind = model::RowKind::band;
        header.section = model::Section::header;
        header.parent = "GRP00001";

        const auto drop = model::dropFor (header, cueOf ("F4DE0001", "fade", "Lights down"), 0.5);
        CHECK (drop.kind == model::DropKind::header);
        CHECK (drop.refused.empty());
    }
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
TEST_CASE ("client: every kind wears an icon and an accent the theme declares")
{
    /*  The author, 2026-09-30: "Could we add some glyphs/icons (not the
        standard emoticons please) ... cue list (type of cue or group and their
        important settings) ... We can also use small colour accents." */
    const model::Theme theme;
    const auto& names = model::Theme::colourNames();

    for (const auto& kind : model::cueKinds())
    {
        INFO ("kind " << kind);
        CHECK (model::iconFor (kind) != model::Icon::none);

        const auto accent = model::accentFor (kind);
        CHECK (accent == "kind-" + kind);
        CHECK (std::find (names.begin(), names.end(), accent) != names.end());
    }

    //  A start cue acts on a cue, and wears the transport's accent; a kind nobody knows, the ink's.
    CHECK (model::iconFor ("start") == model::Icon::start);
    CHECK (model::accentFor ("start") == "kind-transport");
    CHECK (model::iconFor ("somethingNew") == model::Icon::none);
    CHECK (model::accentFor ("somethingNew") == "ink-faint");

    //  A group is known by its mode, a transport cue by its verb.
    CHECK (model::iconFor ("group", "timeline") == model::Icon::timeline);
    CHECK (model::iconFor ("group", "sampler") == model::Icon::sampler);
    CHECK (model::iconFor ("group", "sequence") == model::Icon::sequence);
    CHECK (model::iconFor ("transport", {}, "hard") == model::Icon::stop);
    CHECK (model::iconFor ("transport", {}, "fade") == model::Icon::stopFade);
    CHECK (model::iconFor ("transport", {}, "record") == model::Icon::record);
    CHECK (model::iconFor ("transport", {}, "advance") == model::Icon::advance);
    CHECK (model::iconFor ("transport", {}, "afterIteration") == model::Icon::stopAfter);

    //  Every panel at the foot, both ways between a subject and its word.
    for (const auto kind : { model::Subject::Kind::waveform, model::Subject::Kind::sends,
                             model::Subject::Kind::timeline, model::Subject::Kind::curve,
                             model::Subject::Kind::eq, model::Subject::Kind::fx, model::Subject::Kind::take,
                             model::Subject::Kind::fade })
    {
        const auto word = model::wordFor (kind);
        INFO ("panel " << word);
        CHECK (model::subjectKindFor (word) == kind);

        /*  A picture for each - but the EQ and the FX, which are their own
            letters (author, 2026-09-30: "EQ toggle can show EQ rather than
            the Gaussian bump. Same for FX"). */
        const auto lettered = kind == model::Subject::Kind::eq || kind == model::Subject::Kind::fx;
        CHECK ((model::iconForPanel (word) == model::Icon::none) == lettered);
    }

    CHECK (model::wordFor (model::Subject::Kind::none).empty());
    CHECK (model::subjectKindFor ("nonsense") == model::Subject::Kind::none);

    //  "What it does" wears the cue's own picture: what a cue does is what kind it is.
    CHECK (model::iconForDrawer ("what it does", "media") == model::Icon::media);
    CHECK (model::iconForDrawer ("what it does", "group", "sampler") == model::Icon::sampler);
    CHECK (model::iconForDrawer ("sampler", "media") == model::Icon::sampler);

    //  A cue's own colour is the theme's spelling, and nothing else.
    CHECK (model::colourFromHex ("#ff8800") == std::optional<std::uint32_t> (0xFFFF8800u));
    CHECK_FALSE (model::colourFromHex ("orange").has_value());
    CHECK_FALSE (model::colourFromHex ("").has_value());
}

TEST_CASE ("client: the network monitor reads OSC and MIDI as lines a person can follow")
{
    /*  The author, 2026-09-30: a network monitor "similar to the one in
        WFS-DIY", OSC and MIDI, in and out. The window decodes what the engine
        kept, with the engine's own codec. */
    const auto captured = [] (monitor::Direction direction, monitor::Medium medium, monitor::Road road,
                              std::string_view peer, const std::vector<std::uint8_t>& bytes,
                              std::size_t fullSize = 0)
    {
        monitor::Capture capture;
        capture.wallMicros = 1'000'000;
        capture.direction = direction;
        capture.medium = medium;
        capture.road = road;
        capture.size = static_cast<std::uint16_t> (std::min (bytes.size(), monitor::maxBytes));
        capture.fullSize = static_cast<std::uint32_t> (fullSize > 0 ? fullSize : bytes.size());
        capture.peerLength = static_cast<std::uint8_t> (peer.size());
        std::copy (peer.begin(), peer.end(), capture.peer);
        std::copy (bytes.begin(), bytes.begin() + capture.size, capture.bytes);
        return capture;
    };

    std::string error;
    const auto message = osc::encode (osc::Packet::message ("/desk/fader/1",
                                                            { osc::Value::float32 (0.75f),
                                                              osc::Value::string ("Voix"),
                                                              osc::Value::int32 (3) }), error);
    REQUIRE (message.has_value());

    auto rows = model::describe (captured (monitor::Direction::out, monitor::Medium::osc, monitor::Road::udp,
                                           "192.168.1.20:9000", *message));
    REQUIRE (rows.size() == 1u);
    CHECK_FALSE (rows[0].incoming);
    CHECK_FALSE (rows[0].midi);
    CHECK (rows[0].road == "udp");
    CHECK (rows[0].peer == "192.168.1.20:9000");
    CHECK (rows[0].address == "/desk/fader/1");
    CHECK (rows[0].types == "fsi");
    CHECK (rows[0].arguments == "0.75  \"Voix\"  3");
    CHECK (rows[0].problem.empty());

    //  A bundle is a line per message in it.
    const auto bundle = osc::encode (osc::Packet::bundle ({}, { osc::Packet::message ("/a", { osc::Value::int32 (1) }),
                                                                osc::Packet::message ("/b") }), error);
    REQUIRE (bundle.has_value());
    rows = model::describe (captured (monitor::Direction::in, monitor::Medium::osc, monitor::Road::page,
                                      "127.0.0.1:51000", *bundle));
    REQUIRE (rows.size() == 2u);
    CHECK (rows[0].address == "/a");
    CHECK (rows[1].address == "/b");
    CHECK (rows[1].road == "page");

    //  What the engine would refuse is a line saying why, with its address when it can be read.
    std::vector<std::uint8_t> broken { '/', 'x', 0, 0, ',', 'f', 0, 0 };    // promises a float, carries none
    rows = model::describe (captured (monitor::Direction::in, monitor::Medium::osc, monitor::Road::udp,
                                      "10.0.0.5:8000", broken));
    REQUIRE (rows.size() == 1u);
    CHECK (rows[0].address == "/x");
    CHECK_FALSE (rows[0].problem.empty());

    //  Cut by the monitor, not by the sender: said so, and not called the sender's fault.
    rows = model::describe (captured (monitor::Direction::out, monitor::Medium::osc, monitor::Road::udp,
                                      "10.0.0.5:8000", *message, 2000));
    REQUIRE (rows.size() == 1u);
    CHECK (rows[0].address == "/desk/fader/1");
    CHECK (rows[0].problem.find ("only the first") != std::string::npos);

    //  MIDI: the message's name, then its channel from one and its numbers.
    rows = model::describe (captured (monitor::Direction::in, monitor::Medium::midi, monitor::Road::midi,
                                      "D700", { 0x91, 60, 100 }));
    REQUIRE (rows.size() == 1u);
    CHECK (rows[0].midi);
    CHECK (rows[0].address == "note on");
    CHECK (rows[0].arguments == "ch 2  60  100");

    model::TrafficRow midiRow;
    model::describeMidi (std::vector<std::uint8_t> { 0x90, 60, 0 }.data(), 3, midiRow);
    CHECK (midiRow.address == "note off");      // a note on at nought velocity is a note off
    model::describeMidi (std::vector<std::uint8_t> { 0xB0, 7, 127 }.data(), 3, midiRow);
    CHECK (midiRow.address == "control change");
    CHECK (midiRow.arguments == "ch 1  7  127");
    model::describeMidi (std::vector<std::uint8_t> { 0xC5, 12 }.data(), 2, midiRow);
    CHECK (midiRow.address == "program change");
    CHECK (midiRow.arguments == "ch 6  12");
    model::describeMidi (std::vector<std::uint8_t> { 0xE0, 0, 64 }.data(), 3, midiRow);
    CHECK (midiRow.arguments == "ch 1  0");     // the middle of the wheel
    model::describeMidi (std::vector<std::uint8_t> { 0xF0, 0x00, 0x20, 0x32, 0xF7 }.data(), 5, midiRow);
    CHECK (midiRow.address == "sysex");
    CHECK (midiRow.arguments == "5 bytes  F0 00 20 32 F7");

    //  The filters: each switch, and a text found in the address, the values or the peer.
    model::TrafficRow in;
    in.incoming = true; in.road = "udp"; in.address = "/desk/fader"; in.peer = "10.0.0.5:8000";
    model::TrafficRow page = in;
    page.road = "page";
    model::TrafficRow note;
    note.incoming = false; note.midi = true; note.road = "midi"; note.address = "note on"; note.peer = "Pads";

    model::TrafficFilter all;
    CHECK (all.matches (in));
    CHECK (all.matches (page));
    CHECK (all.matches (note));

    auto onlyOut = all;
    onlyOut.in = false;
    CHECK_FALSE (onlyOut.matches (in));
    CHECK (onlyOut.matches (note));

    auto noPage = all;
    noPage.page = false;
    CHECK (noPage.matches (in));
    CHECK_FALSE (noPage.matches (page));

    auto noMidi = all;
    noMidi.midi = false;
    CHECK_FALSE (noMidi.matches (note));

    auto text = all;
    text.text = "FADER";
    CHECK (text.matches (in));
    CHECK_FALSE (text.matches (note));
    text.text = "pads";
    CHECK (text.matches (note));

    //  CSV: a header, and a field with a comma or a quote quoted.
    model::TrafficRow tricky = in;
    tricky.arguments = "\"a, b\"";
    const auto csv = model::csvOf ({ tricky }, [] (std::int64_t) { return std::string ("12:00:00.000"); });
    CHECK (csv.rfind ("time,direction,medium,road,peer,address,types,arguments,problem\n", 0) == 0);
    CHECK (csv.find ("12:00:00.000,in,osc,udp,10.0.0.5:8000,/desk/fader,,\"\"\"a, b\"\"\",\n") != std::string::npos);
}

TEST_CASE ("client: a switch for this run is a mark in words, dims the row, and a jump with and Go says so")
{
    /*  Namespace draft §27 (PW): words beside the shape, not colour alone. */
    model::Row row;
    row.kind = "memo";
    CHECK (model::marksFor (row).empty());
    CHECK (row.runsNow());

    row.overridden = "off";
    CHECK_FALSE (row.runsNow());
    REQUIRE (! model::marksFor (row).empty());
    CHECK (model::marksFor (row).front().icon == model::Icon::disabled);
    CHECK (model::marksFor (row).front().text == "off for this run");

    //  Off in the file, on for this run: runs, and says why.
    row.enabled = false;
    row.overridden = "on";
    CHECK (row.runsNow());
    CHECK (model::marksFor (row).front().text == "on for this run");

    row.overridden = "file";
    CHECK_FALSE (row.runsNow());
    CHECK (model::marksFor (row).front().text.empty());

    CHECK (model::verbWord ("jump") == "jump");
    CHECK (model::verbWord ("jump", true) == "jump+go");
    CHECK (model::iconFor ("transport", {}, "enable") == model::Icon::enable);
    CHECK (model::iconFor ("transport", {}, "disable") == model::Icon::disabled);
    CHECK (model::iconFor ("transport", {}, "jump") == model::Icon::jump);
}

TEST_CASE ("client: a row's important settings are marks, and the ordinary case says nothing")
{
    const auto iconsOf = [] (const model::Row& row)
    {
        std::vector<model::Icon> out;

        for (const auto& mark : model::marksFor (row))
            out.push_back (mark.icon);

        return out;
    };

    model::Row plain;
    plain.kind = "media";
    plain.rate = "1";
    CHECK (model::marksFor (plain).empty());

    model::Row group;
    group.kind = "group";
    group.isGroup = true;
    group.mode = "sequence";
    group.loops = "1";
    group.selection = "sequential";
    group.advance = "manual";
    group.play = "0";
    group.members = 5;
    CHECK (model::marksFor (group).empty());

    //  For ever, then shuffled, then two of five, then one after another with no GO.
    group.loops = "0";
    group.selection = "shuffle";
    group.play = "2";
    group.advance = "auto";
    CHECK (iconsOf (group) == std::vector<model::Icon> { model::Icon::forever, model::Icon::shuffle,
                                                         model::Icon::subset, model::Icon::follow });
    CHECK (model::marksFor (group)[2].text == "2 of 5");

    //  A count beside the loop; all five a round is the ordinary case.
    group.loops = "3";
    group.play = "5";
    CHECK (model::marksFor (group).front() == model::Mark { model::Icon::loop, "3", "plays 3 rounds" });

    const auto allFive = iconsOf (group);
    CHECK (std::find (allFive.begin(), allFive.end(), model::Icon::subset) == allFive.end());

    //  A timeline's members start together: `advance` is a sequence's question.
    group.mode = "timeline";
    CHECK (iconsOf (group).back() != model::Icon::follow);

    //  A speed, as the window writes one - and its mode says which picture.
    model::Row media;
    media.kind = "media";
    media.rate = "0.5";
    media.rateMode = "varispeed";
    REQUIRE (model::marksFor (media).size() == 1u);
    CHECK (model::marksFor (media)[0].icon == model::Icon::speed);
    CHECK (model::marksFor (media)[0].text == "\xc3\x97" "0.5");

    media.rateMode = "timestretch";
    CHECK (model::marksFor (media)[0].icon == model::Icon::stretch);

    media.rate = "0";
    CHECK (model::marksFor (media)[0].text == "\xc3\x97" "0");

    //  Disabled first, then how it plays, then what it answers to, then the preset's pin.
    media.enabled = false;
    media.lane = true;
    media.dca = "Band";
    media.preset = "GRP00001";
    CHECK (iconsOf (media) == std::vector<model::Icon> { model::Icon::disabled, model::Icon::stretch,
                                                         model::Icon::lane, model::Icon::dca,
                                                         model::Icon::preset });
    CHECK (model::marksFor (media)[3].text == "Band");

    //  The header's reading of a preset says "preset" in words already, and wears no pin.
    media.derived = true;
    CHECK (iconsOf (media).back() != model::Icon::preset);

    //  A fade: the speed it takes its target to, and that it stops what it faded.
    model::Row fade;
    fade.kind = "fade";
    fade.rateOn = true;
    fade.rate = "1";
    fade.stopWhenDone = true;
    CHECK (iconsOf (fade) == std::vector<model::Icon> { model::Icon::speed, model::Icon::stop });
    CHECK (model::marksFor (fade)[0].text == "\xc3\x97" "1");

    //  Every mark carries the words it stands for (§4.8).
    for (const auto& row : { group, media, fade })
        for (const auto& mark : model::marksFor (row))
            CHECK_FALSE (mark.meaning.empty());
}

TEST_CASE ("client: a row reads what its marks are drawn from")
{
    Rig rig;

    const auto media = rig.document.createCue ("7K2QM9X4", 0, "media", "Slow bed");
    const auto fade = rig.document.createCue ("7K2QM9X4", 1, "fade", "Speed up");
    REQUIRE (media.ok);
    REQUIRE (fade.ok);

    const auto set = [&rig] (const std::string& cueId, const char* row, const char* value)
    {
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + cueId + "/" + row, value).ok);
    };

    set (media.id, "rate", "0.5");
    set (media.id, "rateMode", "timestretch");
    set (media.id, "colour", "#ff8800");
    set (fade.id, "rateOn", "true");
    set (fade.id, "rate", "2");
    set (fade.id, "stopWhenDone", "true");

    rig.parameters.markStale();
    const auto snapshot = rig.publish (1);

    model::ShowModel show;
    REQUIRE (show.refresh (*snapshot, "7K2QM9X4"));

    const auto rowOf = [&show] (const std::string& id)
    {
        const auto at = show.indexOf (id);
        REQUIRE (at >= 0);
        return show.rows()[static_cast<std::size_t> (at)];
    };

    const auto bed = rowOf (media.id);
    CHECK (bed.colour == "#ff8800");
    CHECK (bed.rateMode == "timestretch");
    CHECK_FALSE (bed.lane);
    REQUIRE_FALSE (model::marksFor (bed).empty());
    CHECK (model::marksFor (bed)[0].icon == model::Icon::stretch);
    CHECK (model::marksFor (bed)[0].text == "\xc3\x97" "0.5");

    const auto faster = rowOf (fade.id);
    CHECK (faster.rateOn);
    CHECK (faster.stopWhenDone);
    REQUIRE (model::marksFor (faster).size() == 2u);
    CHECK (model::marksFor (faster)[0].text == "\xc3\x97" "2");
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
/*  THE BUG ROUND OF 2026-10-05, item 6 (namespace draft §30, S7): "I had issues
    importing some media files (wav) at first." A file whose name a session that
    was never saved had left in `media/` was refused as "could not copy", the
    copy ran on the window's thread, and a lagging engine ran an import's
    patience down without having looked at its create. These are the decisions;
    the reading and copying is ui/MediaCopier's, and wfg_audio_ui_tests holds it. */
namespace
{
    model::MediaWork workOf (model::Found found, const std::string& name = {}, const std::string& why = {})
    {
        model::MediaWork work;
        work.found = found;
        work.name = name;
        work.why = why;
        return work;
    }

    /*  The window's half of `follow`, for a test: the create it hands back is
        sent as the window sends it, through the real gesture. */
    struct ImportRig
    {
        Rig rig;
        std::int64_t tick = 1;
        std::shared_ptr<const TreeSnapshot> snapshot = rig.publish (tick);
        model::MediaImports imports;

        static constexpr const char* listId = "7K2QM9X4";

        std::string order() { return model::text (*snapshot, std::string ("/godot/list/") + listId + "/order"); }

        Engine::TickResult send (const Event& event)
        {
            return rig.apply (tick++, event.origin, event.command, event.args);
        }

        model::MediaImports::Steps pass()
        {
            snapshot = rig.publish (tick);
            return imports.follow (*snapshot, model::readTransport (*snapshot).revision);
        }
    };
}

TEST_CASE ("client: a name already in media/ is compared as a case-blind disk compares it, and the free one is the first number")
{
    //  The brief's ladder: "X.wav" -> "X 2.wav" -> "X 3.wav".
    CHECK (model::freeName ("X.wav", std::vector<std::string> {}) == "X.wav");
    CHECK (model::freeName ("X.wav", std::vector<std::string> { "X.wav" }) == "X 2.wav");
    CHECK (model::freeName ("X.wav", std::vector<std::string> { "X.wav", "X 2.wav" }) == "X 3.wav");
    CHECK (model::freeName ("X.wav", std::vector<std::string> { "X.wav", "X 3.wav" }) == "X 2.wav");   // the FIRST free

    //  The last dot is the extension's, as cueNameFor reads one; no dot, no extension.
    CHECK (model::freeName ("Take.01.wav", std::vector<std::string> { "Take.01.wav" }) == "Take.01 2.wav");
    CHECK (model::freeName ("Thunder", std::vector<std::string> { "Thunder" }) == "Thunder 2");
    CHECK (model::freeName (".hidden", std::vector<std::string> { ".hidden" }) == ".hidden 2");

    /*  CASE-BLIND, because Windows and macOS keep "x.WAV" and "X.wav" as one
        file and a show travels: a name free on Linux would not be on the Mac. */
    CHECK (model::sameFileName ("Thunder.WAV", "thunder.wav"));
    CHECK_FALSE (model::sameFileName ("Thunder.wav", "Thunder2.wav"));
    CHECK (model::freeName ("X.wav", std::vector<std::string> { "x.WAV" }) == "X 2.wav");
    CHECK (model::freeName ("X.wav", std::vector<std::string> { "x.wav", "X 2.WAV" }) == "X 3.wav");

    /*  AND PAST ASCII: the author's own "Danse en Chœur" is one file with its
        capitals, as are É and é, Ÿ and ÿ - and two different letters are not. */
    CHECK (model::sameFileName ("Danse en Ch\xc5\x93ur + C10.wav", "DANSE EN CH\xc5\x92UR + C10.WAV"));
    CHECK (model::sameFileName ("\xc3\x89t\xc3\xa9.wav", "\xc3\xa9T\xc3\x89.wav"));
    CHECK (model::sameFileName ("\xc5\xb8.wav", "\xc3\xbf.wav"));
    CHECK (model::sameFileName ("\xc5\xbd.wav", "\xc5\xbe.wav"));                     // Ž ž, a capital on an odd point
    CHECK_FALSE (model::sameFileName ("\xc3\xa9.wav", "e.wav"));
    CHECK_FALSE (model::sameFileName ("\xc5\x93.wav", "\xc5\x91.wav"));               // œ and ő

    //  The name met is the one the disk has, which is what a cue that uses it names.
    const std::vector<std::string> present { "Rain.wav", "THUNDER.wav" };
    CHECK (model::nameAmong (present, "Thunder.wav") == "THUNDER.wav");
    CHECK (model::nameAmong (present, "Wind.wav").empty());

    //  A taken test can be anything - the window adds what its own disk says.
    CHECK (model::freeName ("A.wav", [] (const std::string& name) { return name != "A 4.wav"; }) == "A 4.wav");
    CHECK (model::freeName ("A.wav", [] (const std::string&) { return true; }).empty());
}

TEST_CASE ("client: the same file is used silently, and its bytes are read only when its size agrees")
{
    auto reads = 0;
    const auto same = [&reads] { ++reads; return true; };
    const auto other = [&reads] { ++reads; return false; };

    model::Arrival arrival;
    arrival.exists = true;
    arrival.readable = true;
    arrival.size = 1000;

    CHECK (model::verdictFor (arrival, same) == model::Found::free);

    arrival.meets = "Thunder.wav";
    arrival.meetsSize = 999;
    CHECK (model::verdictFor (arrival, same) == model::Found::other);
    CHECK (reads == 0);                                // a size apart is another file, unread

    arrival.meetsSize = 1000;
    CHECK (model::verdictFor (arrival, same) == model::Found::same);
    CHECK (model::verdictFor (arrival, other) == model::Found::other);
    CHECK (reads == 2);

    //  A file picked out of the show's own folder meets itself and is that file.
    arrival.inShow = true;
    CHECK (model::verdictFor (arrival, other) == model::Found::inShow);

    arrival.readable = false;
    CHECK (model::verdictFor (arrival, same) == model::Found::unreadable);

    arrival.exists = false;
    CHECK (model::verdictFor (arrival, same) == model::Found::missing);
    CHECK (reads == 2);
}

TEST_CASE ("client: an import looks at every file before copying any, and makes its cues in the order picked, each after the one before")
{
    ImportRig rig;

    /*  Three files let go after House to half, the list's first member. One
        is already in the show byte for byte, which is the author's case: a
        session that was never saved left it there. */
    rig.imports.add (ImportRig::listId, 1, rig.order(),
                     { "C:/sounds/Rain.wav", "D:/elsewhere/Thunder.wav", "C:/sounds/Wind.wav" });
    CHECK_FALSE (rig.imports.idle());

    //  EVERY LOOK BEFORE ANY COPY, one file at a time (SI).
    auto job = rig.imports.nextJob();
    REQUIRE (job.has_value());
    CHECK_FALSE (job->copy);
    CHECK (job->source == "C:/sounds/Rain.wav");
    CHECK_FALSE (rig.imports.nextJob().has_value());   // the worker has one
    CHECK (rig.imports.progress().empty());            // a look is not a copy
    rig.imports.worked (workOf (model::Found::free, "Rain.wav"));

    job = rig.imports.nextJob();
    REQUIRE (job.has_value());
    CHECK_FALSE (job->copy);
    CHECK (job->source == "D:/elsewhere/Thunder.wav");
    rig.imports.worked (workOf (model::Found::same, "Thunder.wav"));

    job = rig.imports.nextJob();
    REQUIRE (job.has_value());
    CHECK_FALSE (job->copy);
    rig.imports.worked (workOf (model::Found::free, "Wind.wav"));

    job = rig.imports.nextJob();
    REQUIRE (job.has_value());
    CHECK (job->copy);
    CHECK (job->source == "C:/sounds/Rain.wav");
    CHECK (job->answer == model::Clash::ask);
    CHECK (rig.imports.progress() == "Copying 1 of 3: Rain.wav");

    //  Nothing is asked of the show until a file has landed.
    auto steps = rig.pass();
    CHECK_FALSE (steps.create.has_value());
    CHECK (steps.namings.empty());

    /*  A STRANGER ARRIVES WHILE THE BYTES COPY: a cue at the top of the list,
        from the page. A position fixed at the drop would now name the wrong
        place; the member the hand let go after has not moved. */
    REQUIRE (rig.send ({ "page", "cue.create", { osc::Value::string (ImportRig::listId), osc::Value::int32 (0),
                                                 osc::Value::string ("osc"), osc::Value::string ("Stranger") } })
                 .applied == 1);

    rig.imports.worked (workOf (model::Found::copied, "Rain.wav"));
    job = rig.imports.nextJob();
    REQUIRE (job.has_value());
    CHECK (job->source == "C:/sounds/Wind.wav");
    CHECK (rig.imports.progress() == "Copying 3 of 3: Wind.wav");

    steps = rig.pass();
    REQUIRE (steps.create.has_value());
    CHECK (steps.create->parent == ImportRig::listId);
    CHECK (steps.create->index == 2);                  // after House to half, now second
    CHECK (steps.create->cueName == "Rain");
    CHECK (steps.create->mediaName == "Rain.wav");
    REQUIRE (rig.send (gesture::createCue (steps.create->parent, steps.create->index, "media",
                                           steps.create->cueName)).applied == 1);

    /*  FOUND AND NAMED, and the next cue asked for from the same tree, after
        it - the one already in the show, which needed no copy. */
    steps = rig.pass();
    REQUIRE (steps.namings.size() == 1u);
    const auto rain = steps.namings[0].cueId;
    CHECK (steps.namings[0].mediaName == "Rain.wav");
    CHECK (model::text (*rig.snapshot, "/godot/cue/" + rain + "/name") == "Rain");
    REQUIRE (steps.create.has_value());
    CHECK (steps.create->index == 3);
    CHECK (steps.create->cueName == "Thunder");
    REQUIRE (rig.send (gesture::createCue (steps.create->parent, steps.create->index, "media",
                                           steps.create->cueName)).applied == 1);

    //  Wind is still copying, so nothing more is asked for.
    steps = rig.pass();
    REQUIRE (steps.namings.size() == 1u);
    const auto thunder = steps.namings[0].cueId;
    CHECK_FALSE (steps.create.has_value());
    CHECK (steps.ended.empty());

    rig.imports.worked (workOf (model::Found::copied, "Wind.wav"));
    CHECK (rig.imports.progress().empty());

    steps = rig.pass();
    REQUIRE (steps.create.has_value());
    CHECK (steps.create->index == 4);
    REQUIRE (rig.send (gesture::createCue (steps.create->parent, steps.create->index, "media",
                                           steps.create->cueName)).applied == 1);

    steps = rig.pass();
    REQUIRE (steps.namings.size() == 1u);
    const auto wind = steps.namings[0].cueId;

    //  THE LAST WORD: what it did, which stays on the foot.
    REQUIRE (steps.ended.size() == 1u);
    CHECK (steps.ended[0].sentence == "imported 3 files (1 already in the show)");
    CHECK (steps.ended[0].made == 3);
    CHECK (rig.imports.idle());

    //  The order the files were picked, after the member they were dropped after.
    const auto members = model::words (rig.order());
    REQUIRE (members.size() == 6u);
    CHECK (model::text (*rig.snapshot, "/godot/cue/" + members[0] + "/name") == "Stranger");
    CHECK (members[1] == "B3N8R5TW");
    CHECK (members[2] == rain);
    CHECK (members[3] == thunder);
    CHECK (members[4] == wind);
    CHECK (members[5] == "D9FH2JKA");
}

TEST_CASE ("client: an import waits for its cue in the engine's ticks, not the window's passes, and a second waits behind it")
{
    //  The rule itself: the engine's clock, and a clock gone backwards is no clock to wait for.
    model::Import asked;
    asked.askedTick = 100;
    CHECK_FALSE (model::outOfPatience (asked, 100));
    CHECK_FALSE (model::outOfPatience (asked, 100 + model::importPatienceTicks - 1));
    CHECK (model::outOfPatience (asked, 100 + model::importPatienceTicks));
    CHECK (model::outOfPatience (asked, 99));

    ImportRig rig;

    //  One file at the end, then a second import dropped at the top while it runs.
    rig.imports.add (ImportRig::listId, -1, rig.order(), { "a/One.wav" });
    rig.imports.add (ImportRig::listId, 0, rig.order(), { "b/Two.wav" });

    for (const auto* name : { "One.wav", "Two.wav" })
    {
        const auto look = rig.imports.nextJob();
        REQUIRE (look.has_value());
        CHECK_FALSE (look->copy);
        rig.imports.worked (workOf (model::Found::free, name));
    }

    for (const auto* name : { "One.wav", "Two.wav" })
    {
        const auto copy = rig.imports.nextJob();
        REQUIRE (copy.has_value());
        CHECK (copy->copy);
        rig.imports.worked (workOf (model::Found::copied, name));
    }

    auto steps = rig.pass();
    REQUIRE (steps.create.has_value());
    CHECK (steps.create->cueName == "One");
    CHECK (steps.create->index == 2);                  // the end: two members

    /*  THE CREATE NEVER ARRIVES - refused, or not yet looked at - and the
        window runs a thousand passes on a tree the engine has not moved past.
        Counted in passes, that was the end of the file; counted in the
        engine's ticks, nothing has happened yet. And Two, which landed too,
        is not asked for ahead of it. */
    const auto revision = model::readTransport (*rig.snapshot).revision;

    for (auto i = 0; i < 1000; ++i)
    {
        steps = rig.imports.follow (*rig.snapshot, revision);
        REQUIRE (steps.said.empty());
        REQUIRE (steps.ended.empty());
        REQUIRE_FALSE (steps.create.has_value());
    }

    //  The engine's clock runs on without the cue: given up, in words, and Two asked for.
    rig.snapshot = rig.rig.publish (rig.tick + model::importPatienceTicks);
    steps = rig.imports.follow (*rig.snapshot, revision);

    REQUIRE (steps.said.size() == 1u);
    CHECK (steps.said[0] == "One.wav is in the show, but the cue for it was refused");
    REQUIRE (steps.ended.size() == 1u);
    CHECK (steps.ended[0].sentence == "One.wav is in the show, but the cue for it was refused");
    CHECK (steps.ended[0].made == 0);
    REQUIRE (steps.create.has_value());
    CHECK (steps.create->cueName == "Two");
    CHECK (steps.create->index == 0);                  // the top, where it was dropped
}

TEST_CASE ("client: another file of the same name is asked about, one at a time, and an answer can stand for the rest")
{
    model::MediaImports imports;
    imports.add ("L1", -1, "", { "a/P.wav", "a/Q.wav", "a/R.wav" });

    imports.nextJob();
    imports.worked (workOf (model::Found::other, "P.wav"));
    imports.nextJob();
    imports.worked (workOf (model::Found::other, "q.WAV"));    // met under the disk's own spelling

    //  The first in the order picked, and the others may meet one too.
    auto asked = imports.asking();
    REQUIRE (asked.has_value());
    CHECK (asked->picked == "P.wav");
    CHECK (asked->met == "P.wav");
    CHECK (asked->more);

    //  The copies of what met nothing go on while the question is up.
    imports.nextJob();
    imports.worked (workOf (model::Found::free, "R.wav"));
    auto job = imports.nextJob();
    REQUIRE (job.has_value());
    CHECK (job->copy);
    CHECK (job->source == "a/R.wav");
    imports.worked (workOf (model::Found::copied, "R.wav"));

    //  Replace, for this one only: Q is asked in turn, and nothing else may meet one.
    imports.answer (model::Clash::replace, false);
    asked = imports.asking();
    REQUIRE (asked.has_value());
    CHECK (asked->picked == "Q.wav");
    CHECK (asked->met == "q.WAV");
    CHECK_FALSE (asked->more);

    job = imports.nextJob();
    REQUIRE (job.has_value());
    CHECK (job->copy);
    CHECK (job->source == "a/P.wav");
    CHECK (job->answer == model::Clash::replace);
    CHECK (job->met == "P.wav");
    imports.worked (workOf (model::Found::copied, "P.wav"));

    //  Use the one in the show: no copy, and the cue will play the show's own spelling.
    imports.answer (model::Clash::useTheShows, false);
    CHECK_FALSE (imports.asking().has_value());
    CHECK_FALSE (imports.nextJob().has_value());

    //  An answer for the rest stands for every file of that import still to meet a name.
    model::MediaImports several;
    several.add ("L1", -1, "", { "a/X.wav", "a/Y.wav", "a/Z.wav" });
    several.nextJob();
    several.worked (workOf (model::Found::other, "X.wav"));
    several.nextJob();
    several.worked (workOf (model::Found::other, "Y.wav"));

    REQUIRE (several.asking().has_value());
    several.answer (model::Clash::keepBoth, true);
    CHECK_FALSE (several.asking().has_value());       // Y answered too

    several.nextJob();
    several.worked (workOf (model::Found::other, "Z.wav"));   // and Z, met after the answer
    CHECK_FALSE (several.asking().has_value());

    for (const auto* source : { "a/X.wav", "a/Y.wav", "a/Z.wav" })
    {
        job = several.nextJob();
        REQUIRE (job.has_value());
        CHECK (job->source == source);
        CHECK (job->answer == model::Clash::keepBoth);
        several.worked (workOf (model::Found::copied, "copied"));
    }

    //  And the question, in words: who else plays the file, and what Keep both would call this one.
    auto words = model::clashWords ("X.wav", "X.wav", {}, "X 2.wav");
    CHECK (words.title == "Another X.wav is in the show");
    CHECK (words.message == "The show already has a file called X.wav, and it is not this one.\n\n"
                            "Keep both puts this one beside it as X 2.wav.");

    words = model::clashWords ("X.wav", "X.wav", { "Rain" }, "X 2.wav");
    CHECK (words.message.find ("\"Rain\" plays it, so replacing it changes that cue too.") != std::string::npos);

    words = model::clashWords ("X.wav", "X.wav", { "A", "B" }, "");
    CHECK (words.message == "The show already has a file called X.wav, and it is not this one.\n\n"
                            "\"A\" and \"B\" play it, so replacing it changes those cues too.");

    words = model::clashWords ("X.wav", "X.wav", { "A", "B", "C" }, "");
    CHECK (words.message.find ("\"A\", \"B\" and \"C\" play it") != std::string::npos);

    words = model::clashWords ("X.wav", "X.wav", { "A", "B", "C", "D", "E" }, "");
    CHECK (words.message.find ("\"A\", \"B\", \"C\" and 2 more play it") != std::string::npos);
}

TEST_CASE ("client: each way a file is not imported says its own cause, and the last word says them again")
{
    //  The four causes the one sentence "could not copy X into the show" used to cover.
    CHECK (model::notFoundWords ("A.wav") == "A.wav could not be found");
    CHECK (model::unreadableWords ("A.wav") == "A.wav could not be read");
    CHECK (model::copyFailedWords ("A.wav", "There is not enough space on the disk")
           == "A.wav could not be copied into the show: There is not enough space on the disk");
    CHECK (model::copyFailedWords ("A.wav", "") == "A.wav could not be copied into the show");
    CHECK (model::noFolderWords ("A.wav") == "A.wav has nowhere to go: the show has no folder yet");
    CHECK (model::refusedWords ("A.wav") == "A.wav is in the show, but the cue for it was refused");

    ImportRig rig;
    rig.imports.add (ImportRig::listId, -1, rig.order(), { "s/A.wav", "s/B.wav", "s/C.wav", "s/D.wav" });

    rig.imports.nextJob();
    rig.imports.worked (workOf (model::Found::free, "A.wav"));
    rig.imports.nextJob();
    rig.imports.worked (workOf (model::Found::missing));
    rig.imports.nextJob();
    rig.imports.worked (workOf (model::Found::free, "C.wav"));
    rig.imports.nextJob();
    rig.imports.worked (workOf (model::Found::unreadable));

    rig.imports.nextJob();
    rig.imports.worked (workOf (model::Found::copied, "A.wav"));
    rig.imports.nextJob();
    rig.imports.worked (workOf (model::Found::failed, {}, "There is not enough space on the disk"));

    //  Said as they happened, in order.
    auto steps = rig.pass();
    REQUIRE (steps.said.size() == 3u);
    CHECK (steps.said[0] == "B.wav could not be found");
    CHECK (steps.said[1] == "D.wav could not be read");
    CHECK (steps.said[2] == "C.wav could not be copied into the show: There is not enough space on the disk");

    REQUIRE (steps.create.has_value());
    REQUIRE (rig.send (gesture::createCue (steps.create->parent, steps.create->index, "media",
                                           steps.create->cueName)).applied == 1);

    //  And again at the end, where it stays: the first, and how many more.
    steps = rig.pass();
    REQUIRE (steps.namings.size() == 1u);
    REQUIRE (steps.ended.size() == 1u);
    CHECK (steps.ended[0].sentence == "imported 1 of 4 files - B.wav could not be found (and 2 more)");
    CHECK (steps.ended[0].made == 1);

    //  One file: what it was called, and what it is called now.
    rig.imports.add (ImportRig::listId, -1, rig.order(), { "s/X.wav" });
    rig.imports.nextJob();
    rig.imports.worked (workOf (model::Found::other, "X.wav"));
    rig.imports.answer (model::Clash::keepBoth, false);
    rig.imports.nextJob();
    rig.imports.worked (workOf (model::Found::copied, "X 2.wav"));

    steps = rig.pass();
    REQUIRE (steps.create.has_value());
    CHECK (steps.create->cueName == "X 2");            // named after the file it plays
    REQUIRE (rig.send (gesture::createCue (steps.create->parent, steps.create->index, "media",
                                           steps.create->cueName)).applied == 1);
    steps = rig.pass();
    REQUIRE (steps.ended.size() == 1u);
    CHECK (steps.ended[0].sentence == "imported X.wav as X 2.wav");

    //  A file dropped on a media cue names it, with no create, and says so.
    rig.imports.link ("M3D7ACQE", "s/Thunder.wav");
    rig.imports.nextJob();
    rig.imports.worked (workOf (model::Found::same, "Thunder.wav"));

    steps = rig.pass();
    CHECK_FALSE (steps.create.has_value());
    REQUIRE (steps.namings.size() == 1u);
    CHECK (steps.namings[0].cueId == "M3D7ACQE");
    CHECK (steps.namings[0].mediaName == "Thunder.wav");
    REQUIRE (steps.ended.size() == 1u);
    CHECK (steps.ended[0].sentence == "Thunder.wav is on the cue");
    CHECK (rig.imports.idle());
}

TEST_CASE ("client: the question names the cues that play the file a replace would change")
{
    Rig rig;
    std::int64_t tick = 1;

    REQUIRE (rig.apply (tick++, "window", "cue.create",
                        { osc::Value::string ("7K2QM9X4"), osc::Value::int32 (0),
                          osc::Value::string ("media"), osc::Value::string ("Rain bed") }).applied == 1);
    const auto made = model::createdAt (model::text (*rig.publish (tick), "/godot/list/7K2QM9X4/order"), 0);
    REQUIRE_FALSE (made.empty());
    REQUIRE (rig.apply (tick++, "window", "node.set",
                        { osc::Value::string ("/godot/cue/" + made + "/file"),
                          osc::Value::string ("Rain.wav") }).applied == 1);

    const auto snapshot = rig.publish (tick);
    CHECK (model::cuesPlaying (*snapshot, "Rain.wav") == std::vector<std::string> { "Rain bed" });
    CHECK (model::cuesPlaying (*snapshot, "RAIN.WAV") == std::vector<std::string> { "Rain bed" });
    CHECK (model::cuesPlaying (*snapshot, "Thunder.wav").empty());
}

//==============================================================================
TEST_CASE ("client: a file dropped on a group's row goes into the group")
{
    /*  The author, 2026-09-25: "Drag and dropping a media file on a group
        places the new media cue after the group and requires moving it into
        the group afterwards." Rows built by hand, as the row drag's are. */
    const auto cue = [] (const char* id, const char* kind, const char* parent, int index, int depth)
    {
        model::Row row;
        row.rowKind = model::RowKind::cue;
        row.id = id;
        row.name = id;
        row.kind = kind;
        row.parent = parent;
        row.indexInParent = index;
        row.depth = depth;
        row.isGroup = std::string (kind) == "group";
        return row;
    };

    //  A, then G open holding M1 and M2, then S, then F folded - all in the list L.
    auto folded = cue ("F", "group", "L", 3, 0);
    folded.shut = true;

    const std::vector<model::Row> rows {
        cue ("A", "memo", "L", 0, 0),
        cue ("G", "group", "L", 1, 0),
        cue ("M1", "media", "G", 0, 1),
        cue ("M2", "memo", "G", 1, 1),
        cue ("S", "memo", "L", 2, 0),
        folded,
    };

    //  The middle of a group's row: into it, at the end, the row lit - open or folded.
    for (const std::size_t group : { std::size_t { 1 }, std::size_t { 5 } })
    {
        const auto into = model::fileDropAt (rows, group, 0.5, 0);
        CHECK (into.parent == rows[group].id);
        CHECK (into.index == -1);
        CHECK (into.lit);
        CHECK (into.words == "into " + rows[group].id + ", at the end");
    }

    /*  THE REST OF AN OPEN GROUP'S ROW IS ITS FIRST PLACE, where the line
        under its name is drawn - the author's case: it was after the group. */
    for (const auto fraction : { 0.1, 0.9 })
    {
        const auto first = model::fileDropAt (rows, 1, fraction, 0);
        CHECK (first.parent == "G");
        CHECK (first.index == 0);
        CHECK_FALSE (first.lit);
        CHECK (first.depth == 1);
        CHECK (first.words == "first in G");
    }

    //  A folded group's row is still after it: its members are not on screen to go among.
    {
        const auto after = model::fileDropAt (rows, 5, 0.9, 0);
        CHECK (after.parent == "L");
        CHECK (after.index == 4);
        CHECK (after.depth == 0);
        CHECK (after.words == "after F");
    }

    //  After a member, in its group; and, with the hand left of it, after the group it ends.
    {
        const auto inG = model::fileDropAt (rows, 3, 0.9, 1);
        CHECK (inG.parent == "G");
        CHECK (inG.index == 2);
        CHECK (inG.depth == 1);

        const auto outOfG = model::fileDropAt (rows, 3, 0.9, 0);
        CHECK (outOfG.parent == "L");
        CHECK (outOfG.index == 2);
        CHECK (outOfG.depth == 0);
        CHECK (outOfG.words == "after G");

        //  A plain cue at the top of the list: after it.
        CHECK (model::fileDropAt (rows, 0, 0.5, 0).index == 1);
    }

    /*  A HEADER ROW AND A BAND: the end of their group's members, with no
        line, since a create cannot reach the section the line would be in. */
    {
        auto inHeader = cue ("H1", "memo", "G", 0, 1);
        inHeader.section = model::Section::header;
        inHeader.sectionId = "G/header";

        model::Row band;
        band.rowKind = model::RowKind::band;
        band.section = model::Section::header;
        band.parent = "G";
        band.sectionId = "G/header";
        band.depth = 1;

        const std::vector<model::Row> headed { cue ("G", "group", "L", 0, 0), band, inHeader,
                                               cue ("M1", "memo", "G", 0, 1) };

        for (const std::size_t at : { std::size_t { 1 }, std::size_t { 2 } })
        {
            const auto end = model::fileDropAt (headed, at, 0.9, 1);
            CHECK (end.parent == "G");
            CHECK (end.index == -1);
            CHECK_FALSE (end.lit);
            CHECK (end.depth == -1);
            CHECK (end.words == "at the end of G");
        }
    }

    //  Past the rows there is nothing to read.
    CHECK (model::fileDropAt (rows, rows.size(), 0.5, 0).parent.empty());
}

//==============================================================================
TEST_CASE ("client: a cue dragged to the end of a group leaves it when the hand moves left")
{
    /*  The author, 2026-09-25: "It's hard to move a cue out of group to place
        it right below it. It always gets moved back into the group at the last
        position." Under the last row of a group the pointer's height cannot
        tell "after this row" from "after the group"; its x can - `depth` is
        the level the hand is over. Rows built by hand, with their depths. */
    const auto cue = [] (const char* id, const char* kind, const char* parent, int index, int depth)
    {
        model::Row row;
        row.rowKind = model::RowKind::cue;
        row.id = id;
        row.name = id;
        row.kind = kind;
        row.parent = parent;
        row.indexInParent = index;
        row.depth = depth;
        row.isGroup = std::string (kind) == "group";
        return row;
    };

    //  A, then G holding M1 and M2, then S - all in the list L.
    const std::vector<model::Row> rows {
        cue ("A", "memo", "L", 0, 0),
        cue ("G", "group", "L", 1, 0),
        cue ("M1", "memo", "G", 0, 1),
        cue ("M2", "memo", "G", 1, 1),
        cue ("S", "memo", "L", 2, 0),
    };

    //  The last member ends the group; the first does not.
    CHECK (model::endingAt (rows, 3, 1)->id == "M2");
    CHECK (model::endingAt (rows, 3, 0)->id == "G");
    CHECK (model::endingAt (rows, 2, 0)->id == "M1");

    //  M1 dropped under M2 with the hand over the group's rail: after G, in the list.
    {
        int landed = -1;
        const auto out = model::dropAtDepth (rows, 3, rows[2], 0.9, 0, &landed);
        CHECK (out.kind == model::DropKind::after);
        CHECK (out.container == "L");
        CHECK (out.index == 2);
        CHECK (landed == 0);

        //  And over the row's own text: after M2, in the group, as before.
        const auto in = model::dropAtDepth (rows, 3, rows[2], 0.9, 1, &landed);
        CHECK (in.kind == model::DropKind::after);
        CHECK (in.container == "G");
        CHECK (landed == 1);
    }

    /*  THE LAST MEMBER TAKES ITSELF OUT: dragged over its own row, left, it
        lands directly below the group - the author's case exactly. Over its
        own text it is still nothing, as a row onto itself always was. */
    {
        const auto out = model::dropAtDepth (rows, 3, rows[3], 0.9, 0);
        CHECK (out.kind == model::DropKind::after);
        CHECK (out.container == "L");
        CHECK (out.index == 2);

        CHECK (model::dropAtDepth (rows, 3, rows[3], 0.9, 1).kind == model::DropKind::none);
    }

    //  The middle of a group's own row still means into it, wherever the hand is.
    CHECK (model::dropAtDepth (rows, 1, rows[0], 0.5, 0).kind == model::DropKind::into);

    //  Nested: G holding H holding N, then S. Each level left is one group further out.
    {
        const std::vector<model::Row> nested {
            cue ("G", "group", "L", 0, 0),
            cue ("H", "group", "G", 0, 1),
            cue ("N", "memo", "H", 0, 2),
            cue ("S", "memo", "L", 1, 0),
        };

        CHECK (model::endingAt (nested, 2, 2)->id == "N");
        CHECK (model::endingAt (nested, 2, 1)->id == "H");
        CHECK (model::endingAt (nested, 2, 0)->id == "G");

        const auto outOfH = model::dropAtDepth (nested, 2, nested[3], 0.9, 1);
        CHECK (outOfH.kind == model::DropKind::after);
        CHECK (outOfH.container == "G");
        CHECK (outOfH.index == 1);
    }

    //  A group at the very end of the list: the next row is nothing, so both levels are open.
    {
        const std::vector<model::Row> last {
            cue ("A", "memo", "L", 0, 0),
            cue ("G", "group", "L", 1, 0),
            cue ("M1", "memo", "G", 0, 1),
        };

        CHECK (model::endingAt (last, 2, 0)->id == "G");

        const auto out = model::dropAtDepth (last, 2, last[0], 0.9, 0);
        CHECK (out.kind == model::DropKind::after);
        CHECK (out.container == "L");
    }

    //  A group the next row is still inside is not left: the line is only as far out as it is.
    {
        const std::vector<model::Row> inside {
            cue ("G", "group", "L", 0, 0),
            cue ("M1", "memo", "G", 0, 1),
            cue ("M2", "memo", "G", 1, 1),
        };

        CHECK (model::endingAt (inside, 1, 0)->id == "M1");
    }
}

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

    /*  A band takes the cue INTO its section when the tree names one, and
        MAKES the section when it does not (2026-09-22). It used to take
        nothing there, which was right while such a band was drawn only for a
        section that had been emptied - there was nothing on screen to aim at
        either way. Now the list grows the missing bands of whichever group is
        being dragged over, so the band is the target and letting go has to
        make the thing being dropped into. */
    {
        model::Row band;
        band.rowKind = model::RowKind::band;
        band.section = model::Section::footer;
        band.parent = "G";

        const auto made = model::dropFor (band, a, 0.5);
        CHECK (made.kind == model::DropKind::footer);
        CHECK (made.cueId == "G");

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
    CHECK (model::editAttributeFor (model::EditCell::duration, "transport") == "duration");
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
TEST_CASE ("client: the foot copies the part it shows, and pastes it onto the picked cues that take it")
{
    /*  Copy and Paste on the foot (namespace draft §38): each panel's part,
        a copied part read back off the clipboard by its two landmarks, the
        picked cues of a kind that takes it - never the cue it came from - and
        the paste as the window sends it. */
    CHECK (model::partForPanel (model::Subject::Kind::waveform) == "time");
    CHECK (model::partForPanel (model::Subject::Kind::eq) == "eq");
    CHECK (model::partForPanel (model::Subject::Kind::sends) == "sends");
    CHECK (model::partForPanel (model::Subject::Kind::fx) == "fx");
    CHECK (model::partForPanel (model::Subject::Kind::timeline).empty());
    CHECK (model::partForPanel (model::Subject::Kind::fade).empty());

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

    const auto rain = create ("media", "Rain");
    const auto wind = create ("media", "Wind");
    const auto memo = create ("memo", "Note");
    const auto movie = create ("video", "Clip");

    REQUIRE (rig.apply (tick++, "window", "node.set",
                        { osc::Value::string ("/godot/cue/" + rain + "/eqB1Gain"),
                          osc::Value::string ("4.5") }).applied == 1);

    const auto copy = gesture::copyPart ("eq", rain);
    REQUIRE (rig.apply (tick++, copy.origin, copy.command, copy.args).applied == 1);

    const auto fragment = rig.document.partClipboardText();
    const auto clip = model::readPartClip (fragment);
    REQUIRE (clip.isPart());
    CHECK (clip.parts == "eq");
    CHECK (clip.element == "Media");
    CHECK (clip.sourceId == rain);

    CHECK (model::takesPart (clip, "media"));
    CHECK (model::takesPart (clip, "mic"));
    CHECK_FALSE (model::takesPart (clip, "video"));
    CHECK_FALSE (model::takesPart (clip, "memo"));

    /*  Cues are not part of one, and words are not either. */
    CHECK_FALSE (model::readPartClip ("<Fragment>\n  <Media id=\"X\"/>\n</Fragment>\n").isPart());
    CHECK_FALSE (model::readPartClip ("hello").isPart());

    const auto snapshot = rig.publish (tick);

    /*  The panel's cue among several picked: every one that takes it, the
        source passed over. Not among them: the panel's cue alone. */
    CHECK (model::pasteTargets (*snapshot, clip, wind, { rain, wind, memo, movie })
             == std::vector<std::string> { wind });
    CHECK (model::pasteTargets (*snapshot, clip, wind, { memo, movie })
             == std::vector<std::string> { wind });
    CHECK (model::pasteTargets (*snapshot, clip, rain, {}).empty());

    const auto paste = gesture::pastePart (fragment, { wind });
    REQUIRE (rig.apply (tick++, paste.origin, paste.command, paste.args).applied == 1);
    CHECK (model::text (*rig.publish (tick), "/godot/cue/" + wind + "/eqB1Gain") == "4.5");

    /*  A chain only between cues of one kind (WU). */
    model::PartClip chain;
    chain.parts = "fx";
    chain.element = "Media";
    CHECK (model::takesPart (chain, "media"));
    CHECK_FALSE (model::takesPart (chain, "mic"));
}

//==============================================================================
TEST_CASE ("client: an output's trim and DCA are read, and a DCA that counts twice is said in words")
{
    /*  Namespace draft §38: each output's trim and DCA on its row, the trim
        in no locale's spelling, and WQ's sentence when an output's DCA also
        marks a cue routed to it - by a direct out or by a send. */
    model::OutputRow row;
    row.trimDb = 0.0;
    CHECK (row.trimWord() == "0 dB");
    row.trimDb = -3.0;
    CHECK (row.trimWord() == "-3 dB");
    row.trimDb = 1.5;
    CHECK (row.trimWord() == "+1.5 dB");
    row.trimDb = -0.04;
    CHECK (row.trimWord() == "0 dB");

    Rig rig;
    auto tick = std::int64_t { 1 };

    const auto applied = [&] (const auto& event)
    {
        REQUIRE (rig.apply (tick++, event.origin, event.command, event.args).applied == 1);
    };

    const auto before = model::readOutputs (*rig.publish (tick));
    applied (gesture::createBus ("direct", 2, -1));
    applied (gesture::createBus ("mix", 2, -1));
    applied (gesture::createDca ("Music"));

    auto snapshot = rig.publish (tick);
    const auto outputs = model::readOutputs (*snapshot);
    REQUIRE (outputs.size() == before.size() + 2);

    const auto front = outputs[before.size()].id;
    const auto reverb = outputs[before.size() + 1].id;
    const auto dcas = model::readDcas (*snapshot);
    REQUIRE (! dcas.empty());
    const auto music = dcas.back().id;

    applied (gesture::setNode ("/godot/bus/" + front + "/trim", "-4.5"));
    applied (gesture::setNode ("/godot/bus/" + front + "/dca", music));

    snapshot = rig.publish (tick);
    const auto read = model::readOutputs (*snapshot);
    CHECK (read[before.size()].trimWord() == "-4.5 dB");
    CHECK (read[before.size()].dca == music);

    //  Nothing routed there is marked yet: nothing to say.
    CHECK (model::dcaTwiceSentences (*snapshot, read, model::readDcas (*snapshot)).empty());

    const auto listId = model::readTransport (*snapshot).listId;
    REQUIRE (rig.apply (tick++, "window", "cue.create",
                        { osc::Value::string (listId), osc::Value::int32 (0),
                          osc::Value::string ("media"), osc::Value::string ("Song") }).applied == 1);
    const auto song = model::createdAt (model::text (*rig.publish (tick), "/godot/list/" + listId + "/order"), 0);

    applied (gesture::setNode ("/godot/cue/" + song + "/dca", music));
    applied (gesture::setNode ("/godot/cue/" + song + "/directOut", front));

    snapshot = rig.publish (tick);
    const auto twice = model::dcaTwiceSentences (*snapshot, model::readOutputs (*snapshot), model::readDcas (*snapshot));
    REQUIRE (twice.size() == 1);
    CHECK (twice[0].find ("Music also trims 1 cue played through") == 0);
    CHECK (twice[0].find ("counts twice") != std::string::npos);

    /*  A send into a mix marked with the same DCA is the same story. */
    applied (gesture::setNode ("/godot/bus/" + reverb + "/dca", music));
    REQUIRE (rig.apply (tick++, "window", "send.create",
                        { osc::Value::string (song), osc::Value::string (reverb) }).applied == 1);

    snapshot = rig.publish (tick);
    CHECK (model::dcaTwiceSentences (*snapshot, model::readOutputs (*snapshot), model::readDcas (*snapshot)).size() == 2);
}

//==============================================================================
TEST_CASE ("client: cue templates are offered in the Add lists, and a cue is born from one")
{
    /*  Namespace draft §38: a template saved from a cue, read off the tree
        with what it carries in words; "+ media" opens a list of files or
        templates; a picture's templates join "+ video"; a picked cue is
        offered the templates of its own kind; and a cue born from one, its
        file given, plays it with the template's settings. */
    CHECK (model::opensList ("media"));

    Rig rig;
    const auto listId = model::readTransport (*rig.publish (0)).listId;
    auto tick = std::int64_t { 1 };

    REQUIRE (rig.apply (tick++, "window", "cue.create",
                        { osc::Value::string (listId), osc::Value::int32 (0),
                          osc::Value::string ("media"), osc::Value::string ("Voice") }).applied == 1);
    const auto voice = model::createdAt (model::text (*rig.publish (tick), "/godot/list/" + listId + "/order"), 0);

    REQUIRE (rig.apply (tick++, "window", "node.set",
                        { osc::Value::string ("/godot/cue/" + voice + "/level"), osc::Value::string ("-6") }).applied == 1);

    //  No template yet: the files, and a word on how one is made.
    auto choices = model::mediaChoices (*rig.publish (tick));
    REQUIRE (choices.size() == 1);
    CHECK (choices[0].cueTemplate.empty());
    CHECK (model::mediaMenu (choices, "at the end").back().text.find ("Save as template") != std::string::npos);

    const auto save = gesture::createCueTemplate ("Voice", voice);
    REQUIRE (rig.apply (tick++, save.origin, save.command, save.args).applied == 1);

    auto snapshot = rig.publish (tick);
    const auto templates = model::readCueTemplates (*snapshot);
    REQUIRE (templates.size() == 1);
    CHECK (templates[0].name == "Voice");
    CHECK (templates[0].kind == "media");
    CHECK (templates[0].kindWord() == "media cue");
    CHECK (templates[0].carriesWords().find ("level") != std::string::npos);

    choices = model::mediaChoices (*snapshot);
    REQUIRE (choices.size() == 2);
    CHECK (choices[1].cueTemplate == templates[0].id);
    CHECK (choices[1].section == "From a template");

    //  A picked media cue may take it; a picture may not.
    CHECK (model::templatesFor (templates, "media", "").size() == 1);
    CHECK (model::templatesFor (templates, "video", "movie").empty());

    //  Born from it, its file given, in one step.
    const auto make = gesture::createCueFrom (listId, 1, templates[0].id, "Second voice", { { "file", "two.wav" } });
    REQUIRE (rig.apply (tick++, make.origin, make.command, make.args).applied == 1);

    snapshot = rig.publish (tick);
    const auto second = model::createdAt (model::text (*snapshot, "/godot/list/" + listId + "/order"), 1);
    CHECK (model::text (*snapshot, "/godot/cue/" + second + "/name") == "Second voice");
    CHECK (model::text (*snapshot, "/godot/cue/" + second + "/file") == "two.wav");
    CHECK (model::text (*snapshot, "/godot/cue/" + second + "/level") == "-6");

    /*  AN IMPORT CARRIES IT: the cue it asks for is to be born from it. */
    model::MediaImports imports;
    imports.add (listId, -1, model::text (*snapshot, "/godot/list/" + listId + "/order"), { "C:/a.wav" },
                 templates[0].id);
    CHECK_FALSE (imports.idle());
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
TEST_CASE ("client: a member dropped on its own group's header band is marked and stays, and a cue from outside is moved in")
{
    /*  Namespace draft §30, decision QZ. The author dragged cues into a header
        expecting them to be got ready there, and they were MOVED: out of the
        body, and played audibly before the members at the GO. A member let go
        on its own group's header band, at any depth, is now marked instead -
        one write to its `preset` - and stays where it was; anything else on
        that band is moved into the header as before. Read on `descent`: Scene
        holds a header, a nested Inner, a member after it, and a footer. */
    Rig rig { "descent" };

    auto tick = std::int64_t { 1 };
    const auto listId = model::readTransport (*rig.publish (0)).listId;

    model::ShowModel show;
    REQUIRE (show.refresh (*rig.publish (0), listId));

    const auto rowAt = [&show] (const std::string& id) -> const model::Row&
    {
        const auto found = show.indexOf (id);
        REQUIRE (found >= 0);
        return show.rows()[static_cast<std::size_t> (found)];
    };

    const auto bandOf = [&show] (const std::string& group, model::Section which)
    {
        for (std::size_t row = 0; row < show.rows().size(); ++row)
            if (show.rows()[row].rowKind == model::RowKind::band && show.rows()[row].parent == group
                  && show.rows()[row].section == which)
                return row;

        return show.rows().size();
    };

    auto sceneBand = bandOf ("BBBB2222", model::Section::header);
    REQUIRE (sceneBand < show.rows().size());
    REQUIRE (show.rows()[sceneBand].sectionId == "HDR30000");

    //  A member two groups down is marked for Scene, wherever across the band the hand is.
    for (const auto x : { 0, 1, 2, 3 })
    {
        CAPTURE (x);
        const auto drop = model::dropAtDepth (show.rows(), sceneBand, rowAt ("DDDD6666"), 0.5, x);
        CHECK (drop.kind == model::DropKind::preset);
        CHECK (drop.cueId == "BBBB2222");
        CHECK (model::dropTone (drop.kind) == "drop-header");
    }

    //  And a member directly in Scene.
    CHECK (model::dropAtDepth (show.rows(), sceneBand, rowAt ("FFFF8888"), 0.5, 1).kind
             == model::DropKind::preset);

    //  From outside the group, and from its own footer, it is still a move into the header.
    for (const auto* id : { "AAAA1111", "FTR91000", "GGGG9999" })
    {
        CAPTURE (id);
        const auto drop = model::dropAtDepth (show.rows(), sceneBand, rowAt (id), 0.5, 1);
        CHECK (drop.kind == model::DropKind::into);
        CHECK (drop.container == "HDR30000");
        CHECK_FALSE (model::headerMarkFor (show.rows()[sceneBand], rowAt (id), show.rows()).has_value());
    }

    //  A cue of the header itself is moved within it, and after a header row is still a place in it.
    CHECK (model::dropAtDepth (show.rows(), sceneBand, rowAt ("HDR40000"), 0.5, 2).kind
             == model::DropKind::into);
    {
        const auto afterRow = model::dropAtDepth (show.rows(), static_cast<std::size_t> (show.indexOf ("HDR40000")),
                                                  rowAt ("DDDD6666"), 0.9, 2);
        CHECK (afterRow.kind == model::DropKind::after);
        CHECK (afterRow.container == "HDR30000");
    }

    //  The footer's band is not a mark, whoever is dropped on it.
    {
        const auto footer = bandOf ("BBBB2222", model::Section::footer);
        REQUIRE (footer < show.rows().size());
        CHECK_FALSE (model::headerMarkFor (show.rows()[footer], rowAt ("DDDD6666"), show.rows()).has_value());
    }

    //  Said in the air with the group's name, and that the cue stays where it is.
    const auto mark = model::dropAtDepth (show.rows(), sceneBand, rowAt ("DDDD6666"), 0.5, 2);
    REQUIRE (mark.kind == model::DropKind::preset);
    CHECK (model::describe (mark, model::groupNamedBy (mark, show.rows()[sceneBand], show.rows()), false)
             == "prepare it in Scene's header, keeping it in its place");

    /*  LETTING GO IS ONE `node.set` OF THE CUE'S `preset`, through the real
        command: the cue stays in Inner, nothing moves into the header, and
        the engine publishes the reading of the mark at once. */
    const auto write = gesture::setNode ("/godot/cue/DDDD6666/preset", mark.cueId);
    REQUIRE (rig.apply (tick++, write.origin, write.command, write.args).applied == 1);

    const auto marked = rig.publish (tick);
    CHECK (model::text (*marked, "/godot/cue/DDDD6666/preset") == "BBBB2222");
    CHECK (model::text (*marked, "/godot/cue/BBBB2222/headerDerived") == "DDDD6666");
    CHECK (model::text (*marked, "/godot/cue/CCCC5555/order") == "DDDD6666 EEEE7777");
    CHECK (model::text (*marked, "/godot/cue/BBBB2222/headerOrder") == "HDR40000");

    //  And the list draws it on the next pass: the reading under Scene's header, the cue in Inner.
    REQUIRE (show.refresh (*marked, listId));
    sceneBand = bandOf ("BBBB2222", model::Section::header);
    REQUIRE (sceneBand < show.rows().size());
    CHECK (show.rows()[sceneBand].count == 2u);

    auto readings = 0;

    for (const auto& row : show.rows())
        if (row.rowKind == model::RowKind::cue && row.id == "DDDD6666" && row.derived)
        {
            ++readings;
            CHECK (row.section == model::Section::header);
            CHECK (row.parent == "BBBB2222");
        }

    CHECK (readings == 1);
    CHECK_FALSE (rowAt ("DDDD6666").derived);
    CHECK (rowAt ("DDDD6666").parent == "CCCC5555");
    CHECK (rowAt ("DDDD6666").section == model::Section::member);
    CHECK (rowAt ("DDDD6666").preset == "BBBB2222");

    //  Dropped there again: nothing to write, and said.
    {
        const auto again = model::dropAtDepth (show.rows(), sceneBand, rowAt ("DDDD6666"), 0.5, 2);
        CHECK (again.kind == model::DropKind::none);
        CHECK (again.refused == "already prepared in Scene's header");
        CHECK (model::describe (again, show.rows()[sceneBand], false) == "already prepared in Scene's header");
    }

    /*  INNER HAS NO HEADER, so its band is the one a drag grows over its
        title, built here by hand: the mark moves inward onto it, while
        Scene's other member - not inside Inner - makes Inner's header and is
        moved into it, said with Inner's name and not the band's word. */
    model::Row innerBand;
    innerBand.rowKind = model::RowKind::band;
    innerBand.section = model::Section::header;
    innerBand.parent = "CCCC5555";
    innerBand.name = "header";
    innerBand.depth = 2;

    const auto inward = model::headerMarkFor (innerBand, rowAt ("DDDD6666"), show.rows());
    REQUIRE (inward.has_value());
    CHECK (inward->kind == model::DropKind::preset);
    CHECK (inward->cueId == "CCCC5555");

    CHECK_FALSE (model::headerMarkFor (innerBand, rowAt ("FFFF8888"), show.rows()).has_value());
    const auto made = model::dropFor (innerBand, rowAt ("FFFF8888"), 0.5);
    CHECK (made.kind == model::DropKind::header);
    CHECK (made.cueId == "CCCC5555");
    CHECK (model::describe (made, model::groupNamedBy (made, innerBand, show.rows()), false)
             == "into Inner's header");

    //  And the menu's first item takes the mark off: the reading goes, the cue never moved.
    const auto clear = gesture::setNode ("/godot/cue/DDDD6666/preset", "");
    REQUIRE (rig.apply (tick++, clear.origin, clear.command, clear.args).applied == 1);
    CHECK (model::text (*rig.publish (tick), "/godot/cue/BBBB2222/headerDerived").empty());
    CHECK (model::text (*rig.publish (tick), "/godot/cue/CCCC5555/order") == "DDDD6666 EEEE7777");
}

//==============================================================================
TEST_CASE ("client: a cue's preset is a menu of the groups around it, innermost first, and over several only theirs in common")
{
    /*  Namespace draft §30, decision QZ - the author: "I could not see a
        header/preset toggle in the cues." The row was a bare box wanting a
        group's identifier. It is a menu now: not prepared ahead, then the
        cue's own group, then each group outside it, read by number and name
        and written as the identifier. */
    Rig rig { "descent" };

    auto tick = std::int64_t { 1 };
    const auto snapshot = rig.publish (0);

    using Choices = std::vector<std::pair<std::string, std::string>>;

    const auto presetIn = [] (const model::Inspection& in) -> model::Field
    {
        for (const auto& block : in.blocks)
            for (const auto& field : block.fields)
                if (field.name == "preset")
                {
                    CHECK (block.heading == "in the list");
                    return field;
                }

        FAIL_CHECK ("no preset row");
        return model::Field {};
    };

    const auto none = std::pair<std::string, std::string> { "", "not prepared ahead" };
    const auto scene = std::pair<std::string, std::string> { "BBBB2222", "2 Scene" };
    const auto inner = std::pair<std::string, std::string> { "CCCC5555", "2.1 Inner" };

    //  Two groups down: its own group first, then the one outside it.
    {
        const auto field = presetIn (model::inspect (*snapshot, "DDDD6666"));
        CHECK (field.control == model::Control::groupRef);
        CHECK (field.writable);
        CHECK (field.address == "/godot/cue/DDDD6666/preset");
        CHECK (field.value.empty());
        CHECK (field.choices == Choices { none, inner, scene });
    }

    //  At the top of the list there is nothing around it but the list.
    CHECK (presetIn (model::inspect (*snapshot, "AAAA1111")).choices == Choices { none });

    //  A header's cue and a footer's are under their group as a member is; a group is offered the ones outside it.
    CHECK (presetIn (model::inspect (*snapshot, "HDR40000")).choices == Choices { none, scene });
    CHECK (presetIn (model::inspect (*snapshot, "FTR91000")).choices == Choices { none, scene });
    CHECK (presetIn (model::inspect (*snapshot, "CCCC5555")).choices == Choices { none, scene });

    //  Several: the groups around every one of them, each written to all.
    {
        const auto siblings = presetIn (model::inspectMany (*snapshot, { "DDDD6666", "EEEE7777" }));
        CHECK (siblings.control == model::Control::groupRef);
        CHECK (siblings.choices == Choices { none, inner, scene });
        CHECK (siblings.addresses == std::vector<std::string> { "/godot/cue/DDDD6666/preset",
                                                                "/godot/cue/EEEE7777/preset" });

        CHECK (presetIn (model::inspectMany (*snapshot, { "DDDD6666", "FFFF8888" })).choices
                 == Choices { none, scene });
        CHECK (presetIn (model::inspectMany (*snapshot, { "FFFF8888", "DDDD6666" })).choices
                 == Choices { none, scene });
        CHECK (presetIn (model::inspectMany (*snapshot, { "DDDD6666", "AAAA1111" })).choices
                 == Choices { none });
    }

    //  A pick is the cue's mark: the menu shows it picked.
    REQUIRE (rig.apply (tick++, "window", "node.set",
                        { osc::Value::string ("/godot/cue/DDDD6666/preset"),
                          osc::Value::string ("BBBB2222") }).applied == 1);
    CHECK (presetIn (model::inspect (*rig.publish (tick), "DDDD6666")).value == "BBBB2222");

    //  A mark naming no group around the cue is shown, and said to be one.
    REQUIRE (rig.apply (tick++, "window", "node.set",
                        { osc::Value::string ("/godot/cue/AAAA1111/preset"),
                          osc::Value::string ("BBBB2222") }).applied == 1);
    {
        const auto stale = presetIn (model::inspect (*rig.publish (tick), "AAAA1111"));
        CHECK (stale.value == "BBBB2222");
        CHECK (stale.choices == Choices { none, { "BBBB2222", "2 Scene (not a group it is in)" } });
    }
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
    CHECK_FALSE (model::madeByCreate (job, "transport", ""));       // another kind
    CHECK_FALSE (model::madeByCreate (job, "fade", "Lights"));  // somebody else's, named already
}

TEST_CASE ("client: a cue born with its settings is one record, carrying the id it drew")
{
    /*  The new-cue lists (2026-09-27) send `cue.create` and `group.wrap` with
        an EMPTY identifier and the settings after it, as `attribute value`
        pairs. What a replay needs is the record: the drawn identifier in the
        empty one's place and the pairs after it, so the second run makes the
        same cue with the same settings and draws nothing. */
    Rig rig;
    rig.engine.log().openInMemory ({});

    const auto listId = model::readTransport (*rig.publish (0)).listId;
    REQUIRE_FALSE (listId.empty());
    const auto membersBefore = model::words (model::text (*rig.publish (0), "/godot/list/" + listId + "/order")).size();
    auto tick = std::int64_t { 1 };

    const auto args = [] (std::initializer_list<const char*> texts)
    {
        std::vector<osc::Value> made;
        for (const auto* text : texts)
            made.push_back (osc::Value::string (text));
        return made;
    };

    auto create = args ({ "", "", "group", "", "", "mode", "timeline" });
    create[0] = osc::Value::string (listId);
    create[1] = osc::Value::int32 (0);
    rig.document.beginTransaction ("cue.create", tick, "window", {});
    REQUIRE (rig.apply (tick++, "window", "cue.create", create).applied == 1);

    auto snapshot = rig.publish (tick);
    const auto group = model::createdAt (model::text (*snapshot, "/godot/list/" + listId + "/order"), 0);
    REQUIRE (model::text (*snapshot, "/godot/cue/" + group + "/kind") == "group");
    CHECK (model::text (*snapshot, "/godot/cue/" + group + "/mode") == "timeline");

    auto aimed = args ({ "", "", "transport", "", "", "verb", "afterIteration", "target", "" });
    aimed[0] = osc::Value::string (listId);
    aimed[1] = osc::Value::int32 (1);
    aimed[8] = osc::Value::string (group);
    rig.document.beginTransaction ("cue.create", tick, "window", {});
    REQUIRE (rig.apply (tick++, "window", "cue.create", aimed).applied == 1);

    snapshot = rig.publish (tick);
    const auto stop = model::createdAt (model::text (*snapshot, "/godot/list/" + listId + "/order"), 1);
    REQUIRE (model::text (*snapshot, "/godot/cue/" + stop + "/kind") == "transport");
    CHECK (model::text (*snapshot, "/godot/cue/" + stop + "/verb") == "afterIteration");
    CHECK (model::text (*snapshot, "/godot/cue/" + stop + "/target") == group);

    // One Undo takes the cue and its settings together.
    REQUIRE (rig.apply (tick++, "window", "undo", {}).applied == 1);
    CHECK (model::words (model::text (*rig.publish (tick), "/godot/list/" + listId + "/order")).size()
             == membersBefore + 1);

    // A value missing its name is a message cut short, refused whole.
    auto cut = args ({ "", "", "memo", "", "", "notes" });
    cut[0] = osc::Value::string (listId);
    cut[1] = osc::Value::int32 (0);
    CHECK (rig.apply (tick++, "window", "cue.create", cut).rejected == 1);

    const auto parsed = LogFile::parse (rig.engine.log().contents());
    std::vector<LogRecord> creates;

    for (const auto& record : parsed.records)
        if (record.command == "cue.create" && record.kind == LogRecord::Kind::applied)
            creates.push_back (record);

    REQUIRE (creates.size() == 2);
    REQUIRE (creates[0].args.size() == 7);
    CHECK (creates[0].args[4].getString() == group);
    CHECK (creates[0].args[6].getString() == "timeline");
    REQUIRE (creates[1].args.size() == 9);
    CHECK (creates[1].args[4].getString() == stop);
    CHECK (creates[1].args[8].getString() == group);
}

TEST_CASE ("client: every line of the new-cue lists makes its cue, born with what the line says")
{
    /*  The four lists (the author, 2026-09-27) held against the engine, the
        way the row of buttons is above: each line is a `cue.create` with
        pairs the engine accepts, and the cue it makes reads back what the line
        promised. The mic fixture has named inputs and a rack, so the mic list
        is a real one. */
    Rig rig ("mic");
    const std::string listId = "MC000001";
    auto tick = std::int64_t { 1 };

    // A stereo channel, and a mono one made shared - a bus with a chain, never claimed.
    REQUIRE (rig.apply (tick++, "window", "channel.create",
                        { osc::Value::string ("stereo"), osc::Value::string ("CH000031") }).applied == 1);
    REQUIRE (rig.apply (tick++, "window", "channel.create",
                        { osc::Value::string ("mono"), osc::Value::string ("CH000032") }).applied == 1);
    REQUIRE (rig.apply (tick++, "window", "node.set",
                        { osc::Value::string ("/godot/slot/CH000032/access"),
                          osc::Value::string ("shared") }).applied == 1);

    auto snapshot = rig.publish (tick);
    const auto mics = model::micChoices (*snapshot);

    const auto through = [&mics] (const std::string& input)
    {
        std::vector<std::string> channels;
        for (const auto& choice : mics)
            for (const auto& [attribute, value] : choice.settings)
                if (attribute == "input" && value == input)
                    for (const auto& [other, channel] : choice.settings)
                        if (other == "channel")
                            channels.push_back (channel);
        return channels;
    };

    CHECK (through ("MC000021") == std::vector<std::string> { "MC000011" });   // mono into mono-to-stereo
    CHECK (through ("MC000022") == std::vector<std::string> { "CH000031" });   // stereo into stereo
    REQUIRE_FALSE (mics.empty());
    CHECK (mics.back().settings.empty());                                       // "No input yet"
    CHECK (mics.back().section.empty());

    const auto micLines = model::micMenu (*snapshot, mics, "at the end of the list");
    CHECK (std::count_if (micLines.begin(), micLines.end(), [] (const model::MenuLine& line)
                          { return line.kind == model::MenuLine::Kind::item; })
             == static_cast<std::ptrdiff_t> (mics.size()));
    CHECK (std::count_if (micLines.begin(), micLines.end(), [] (const model::MenuLine& line)
                          { return line.kind == model::MenuLine::Kind::header; }) == 2);

    //  --- every line, made -----------------------------------------------------
    std::vector<model::Choice> every;
    for (const auto* choices : { &model::groupChoices(), &model::transportChoices(), &model::midiChoices() })
        every.insert (every.end(), choices->begin(), choices->end());
    every.insert (every.end(), mics.begin(), mics.end());

    for (const auto& choice : every)
    {
        CAPTURE (choice.label);

        auto settings = choice.settings;
        if (choice.aimed)
            settings.emplace_back ("target", "MC000006");

        const auto event = gesture::createCue (listId, 0, choice.kind, "", settings);
        rig.document.beginTransaction (event.command, tick, event.origin, {});
        const auto outcome = rig.apply (tick++, event.origin, event.command, event.args);
        CHECK_MESSAGE (outcome.applied == 1, rig.engine.lastError());

        snapshot = rig.publish (tick);
        const auto made = model::createdAt (model::text (*snapshot, "/godot/list/" + listId + "/order"), 0);
        REQUIRE (model::text (*snapshot, "/godot/cue/" + made + "/kind") == choice.kind);

        for (const auto& [attribute, value] : settings)
            CHECK (model::text (*snapshot, "/godot/cue/" + made + "/" + attribute) == value);

        REQUIRE (rig.apply (tick++, "window", "undo", {}).applied == 1);
    }

    //  --- the buttons that open them ------------------------------------------
    const auto& kinds = model::cueKinds();
    CHECK (std::find (kinds.begin(), kinds.end(), "start") == kinds.end());
    CHECK (std::any_of (model::transportChoices().begin(), model::transportChoices().end(),
                        [] (const model::Choice& choice) { return choice.kind == "start"; }));
    CHECK_FALSE (std::any_of (model::transportChoices().begin(), model::transportChoices().end(),
                              [] (const model::Choice& choice)
                              { return choice.settings == model::Settings { { "verb", "fade" } }; }));

    for (const char* kind : { "group", "transport", "midi", "mic", "media" })
        CHECK (model::opensList (kind));
    for (const char* kind : { "memo", "fade", "osc" })
        CHECK_FALSE (model::opensList (kind));

    /*  A WORD FOR EVERY VERB THE ENGINE HAS, short enough for the kind
        column - read off the row's own range, so a verb the engine grows is a
        failing case here and not a row that shows its raw name. */
    const auto stop = gesture::createCue (listId, 0, "transport", "");
    REQUIRE (rig.apply (tick++, stop.origin, stop.command, stop.args).applied == 1);
    snapshot = rig.publish (tick);
    const auto stopId = model::createdAt (model::text (*snapshot, "/godot/list/" + listId + "/order"), 0);
    const auto* verbs = snapshot->find ("/godot/cue/" + stopId + "/verb");
    REQUIRE (verbs != nullptr);
    REQUIRE_FALSE (verbs->enumValues.empty());

    std::vector<std::string> shown;
    for (const auto& verb : verbs->enumValues)
    {
        CAPTURE (verb);
        const auto word = model::verbWord (verb);
        CHECK_FALSE (word.empty());
        CHECK (word.size() <= 8);
        CHECK (std::find (shown.begin(), shown.end(), word) == shown.end());
        shown.push_back (word);
    }

    CHECK (model::verbWord ("hard") == "stop");
    CHECK (model::verbWord ("afterIteration") == "round");
}

TEST_CASE ("client: the group list offers the picked cues when the engine would take them")
{
    Rig rig;
    const auto listId = model::readTransport (*rig.publish (0)).listId;
    REQUIRE_FALSE (listId.empty());
    auto tick = std::int64_t { 1 };

    const auto create = [&] (const std::string& parent, const char* kind, const char* id)
    {
        REQUIRE (rig.apply (tick++, "window", "cue.create",
                            { osc::Value::string (parent), osc::Value::int32 (0), osc::Value::string (kind),
                              osc::Value::string (""), osc::Value::string (id) }).applied == 1);
    };

    create (listId, "memo", "A0000001");
    create (listId, "media", "B0000001");
    create (listId, "media", "C0000001");
    create (listId, "group", "G0000001");
    create ("G0000001", "memo", "D0000001");
    REQUIRE (rig.apply (tick++, "window", "list.create",
                        { osc::Value::string ("Other"), osc::Value::string ("E0000002") }).applied == 1);
    create ("E0000002", "memo", "E0000001");

    const auto snapshot = rig.publish (tick);

    const auto nothing = model::wrapOf (*snapshot, {});
    CHECK_FALSE (nothing.possible());
    CHECK (nothing.why.empty());

    const auto media = model::wrapOf (*snapshot, { "C0000001", "B0000001" });
    CHECK (media.possible());
    CHECK (media.count == 2);
    CHECK (media.allMedia);

    const auto mixed = model::wrapOf (*snapshot, { "A0000001", "B0000001" });
    CHECK (mixed.count == 2);
    CHECK_FALSE (mixed.allMedia);

    // A group and its own member: the member rides in with the group.
    CHECK (model::wrapOf (*snapshot, { "G0000001", "D0000001" }).count == 1);

    const auto apart = model::wrapOf (*snapshot, { "A0000001", "E0000001" });
    CHECK_FALSE (apart.possible());
    CHECK_FALSE (apart.why.empty());

    const auto items = [] (const std::vector<model::MenuLine>& lines, bool wrapped)
    {
        return std::count_if (lines.begin(), lines.end(), [wrapped] (const model::MenuLine& line)
                              { return line.kind == model::MenuLine::Kind::item && line.wrap == wrapped; });
    };

    const auto groups = static_cast<std::ptrdiff_t> (model::groupChoices().size());

    // Nothing picked: the empty part only, and it says where the group lands.
    const auto plain = model::groupMenu (nothing, "at the end of the list");
    REQUIRE_FALSE (plain.empty());
    CHECK (plain.front().kind == model::MenuLine::Kind::header);
    CHECK (plain.front().text == "An empty new group, at the end of the list");
    CHECK (items (plain, true) == 0);
    CHECK (items (plain, false) == groups);

    // All media: every type around them, the sampler included.
    const auto around = model::groupMenu (media, "after Rain");
    CHECK (around.front().text == "Put the 2 picked cues in a new…");
    CHECK (items (around, true) == groups);
    CHECK (items (around, false) == groups);

    // A memo among them: no sampler around them, still one in the empty part.
    CHECK (items (model::groupMenu (mixed, "after Rain"), true) == groups - 1);

    CHECK (model::groupMenu (model::wrapOf (*snapshot, { "A0000001" }), "after Rain").front().text
             == "Put the picked cue in a new…");

    // Two lists: the sentence why, in place of the offer.
    const auto refused = model::groupMenu (apart, "after Rain");
    CHECK (refused.front().kind == model::MenuLine::Kind::note);
    CHECK (refused.front().text == apart.why);
    CHECK (items (refused, true) == 0);

    // And the engine takes what the list offered.
    const auto sampler = gesture::wrapGroup (media.cues, model::groupChoices().back().settings);
    rig.document.beginTransaction (sampler.command, tick, sampler.origin, {});
    REQUIRE (rig.apply (tick++, sampler.origin, sampler.command, sampler.args).applied == 1);
    const auto after = rig.publish (tick);
    const auto group = model::text (*after, "/godot/cue/B0000001/parent");
    CHECK (model::text (*after, "/godot/cue/" + group + "/mode") == "sampler");
    CHECK (model::text (*after, "/godot/cue/C0000001/parent") == group);

    // The transport list says where its cue lands, and aims it.
    const auto aimed = model::transportMenu ("Rain", "after Rain");
    CHECK (aimed.front().text.find ("Aimed at Rain") == 0);
    CHECK (items (aimed, false) == static_cast<std::ptrdiff_t> (model::transportChoices().size()));
    CHECK (model::transportMenu ("", "at the end of the list").front().text.find ("No target yet") == 0);
}

//==============================================================================
TEST_CASE ("client: a view zooms about the pointer and never leaves the file")
{
    /*  THE ONE PROPERTY WORTH ASSERTING about the foot panel's arithmetic: the
        second under the pointer stays under the pointer. That is what makes a
        wheel feel like magnifying the picture rather than scrolling it, and
        anchoring to an edge instead would send whatever somebody was looking
        at off the side every time they leaned on it. */
    model::View view;
    view.reset (30.0);

    CHECK (view.span() == doctest::Approx (30.0));
    CHECK (view.isWholeThing());

    const auto width = 600;
    const auto x = 450.0;                       // three quarters across
    const auto before = view.secondsForX (x, width);

    for (auto turn = 0; turn < 6; ++turn)
        view.zoomAbout (x, width, 0.85);

    CHECK (view.secondsForX (x, width) == doctest::Approx (before).epsilon (0.001));
    CHECK (view.span() < 30.0);

    /*  AND IT NEVER WANDERS OFF THE FILE: panned hard either way, the window
        comes back inside rather than showing empty time - there is no state in
        which the bar is blank because the view left. */
    view.panBy (-100000.0, width);
    CHECK (view.from >= 0.0);

    view.panBy (100000.0, width);
    CHECK (view.to <= 30.0 + 1.0e-9);

    //  Zoomed out past the file, it IS the file rather than something wider.
    for (auto turn = 0; turn < 40; ++turn)
        view.zoomAbout (x, width, 1.4);

    CHECK (view.isWholeThing());
    CHECK (view.span() == doctest::Approx (30.0));

    //  And it has a floor, because the pyramid has one.
    for (auto turn = 0; turn < 200; ++turn)
        view.zoomAbout (x, width, 0.7);

    CHECK (view.span() >= model::View::floorSeconds - 1.0e-9);
}

TEST_CASE ("client: a loop join is one handle and two writes")
{
    /*  Where one range ends and the next begins at the same instant, the two
        edges sit on the same pixel. Dragging either of them ALONE would part
        them and leave a silent gap nobody asked for, so the pair is found and
        moved together - which is the whole reason `Handle::slice` exists. */
    std::vector<model::RangeRow> ranges
    {
        { "R1", "verse",  1.0, 4.0, 2, 0 },
        { "R2", "chorus", 4.0, 8.0, 1, 1 },
        { "R3", "outro", 12.0, 16.0, 1, 2 },
    };

    const auto join = model::hitTest (ranges, 4.02, 0.1);
    CHECK (join.handle == model::Handle::slice);
    CHECK (join.rangeId == "R1");
    CHECK (join.nextId == "R2");

    const auto writes = model::dragTo (join, 5.5, ranges, 20.0);
    REQUIRE (writes.size() == 2u);
    CHECK (writes[0].rangeId == "R1");
    CHECK (std::string (writes[0].attribute) == "out");
    CHECK (writes[1].rangeId == "R2");
    CHECK (std::string (writes[1].attribute) == "in");
    CHECK (writes[0].seconds == doctest::Approx (5.5));
    CHECK (writes[1].seconds == doctest::Approx (5.5));

    /*  An edge that stands alone is one write, and a range is never dragged
        through its own far edge: a range of no length is not a shorter range,
        it is a mistake with no handle left to undo it by. */
    const auto out = model::hitTest (ranges, 16.0, 0.1);
    CHECK (out.handle == model::Handle::out);
    CHECK (out.rangeId == "R3");

    const auto collapsed = model::dragTo (out, 0.0, ranges, 20.0);
    REQUIRE (collapsed.size() == 1u);
    CHECK (collapsed[0].seconds > 12.0);

    /*  A DRAG PAST A LIMIT STOPS AT IT rather than being refused, and that is
        a deliberate difference from a drag that is impossible. A hand that
        overshoots wants the edge to wait at the end for it to come back; a
        gesture that answered nothing would feel like the handle had been
        dropped, and the operator would let go somewhere they did not mean. */
    const auto overshot = model::dragTo (out, 99.0, ranges, 20.0);
    REQUIRE (overshot.size() == 1u);
    CHECK (overshot[0].seconds == doctest::Approx (20.0));   // the end of the file

    /*  AND THE JOIN STOPS INSIDE BOTH NEIGHBOURS, so a gesture aimed at one of
        them can never turn the other inside out. */
    const auto tooEarly = model::dragTo (join, 0.0, ranges, 20.0);
    REQUIRE (tooEarly.size() == 2u);
    CHECK (tooEarly[0].seconds > 1.0);        // not through R1's in-point
    CHECK (tooEarly[0].seconds < 1.2);

    const auto tooLate = model::dragTo (join, 50.0, ranges, 20.0);
    REQUIRE (tooLate.size() == 2u);
    CHECK (tooLate[0].seconds < 8.0);         // not through R2's out-point
    CHECK (tooLate[0].seconds > 7.8);
}

TEST_CASE ("client: an edge snaps to what is already there, and never to itself")
{
    std::vector<model::RangeRow> ranges
    {
        { "R1", "", 1.0, 4.0, 1, 0 },
        { "R2", "", 9.0, 12.0, 1, 1 },
    };

    const auto targets = model::snapTargets (ranges, "R1", 30.0);

    //  Its own edges are not targets; everything else is, plus both ends.
    CHECK (std::find (targets.begin(), targets.end(), 9.0) != targets.end());
    CHECK (std::find (targets.begin(), targets.end(), 0.0) != targets.end());
    CHECK (std::find (targets.begin(), targets.end(), 30.0) != targets.end());
    CHECK (std::find (targets.begin(), targets.end(), 4.0) == targets.end());

    CHECK (model::snapTo (8.97, targets, 0.1) == doctest::Approx (9.0));
    CHECK (model::snapTo (8.5, targets, 0.1) == doctest::Approx (8.5));
}

TEST_CASE ("client: the foot panel says which subject it is on, and follows a pick")
{
    /*  The author's shape for it (2026-09-21): it opens on ONE thing, named by
        whatever opened it, rather than on a tab bar. A waveform follows the
        pick, because "show me that cue's file" is the same question asked of a
        different cue; subjects that are not about the picked cue will not. */
    CHECK (model::followsPick (model::Subject::Kind::waveform));

    model::Subject shut;
    CHECK_FALSE (shut.isOpen());
    CHECK_FALSE (model::followsPick (shut.kind));

    Rig rig ("phase4");
    const auto snapshot = rig.publish (0);

    const model::Subject onMedia { model::Subject::Kind::waveform, "P4MED001" };
    const auto reading = model::readFoot (*snapshot, onMedia);

    CHECK (reading.cueKind == "media");
    CHECK (reading.cueName == "The bed");
    CHECK (reading.file == "segments.wav");

    /*  AND IT SAYS WHY THERE IS NOTHING TO DRAW when there is nothing, rather
        than sitting blank: a silent file, a missing one and one still being
        analysed are three situations with three different answers. */
    const auto onGroup = model::readFoot (*snapshot,
                                          { model::Subject::Kind::waveform, "P4GRP001" });
    CHECK (onGroup.notice.find ("media cue") != std::string::npos);

    //  A shut panel reads nothing at all.
    CHECK (model::readFoot (*snapshot, shut).cueName.empty());
}


TEST_CASE ("client: a speed is said where it is not one - beside a run, and in the waveform's head row")
{
    /*  Namespace draft §22.7. To the thousandth, in no locale's spelling, and
        nothing at all at one. */
    CHECK (model::speedText (1.0).empty());
    CHECK (model::speedText (0.9996).empty());
    CHECK (model::speedText (0.5) == "×0.5");
    CHECK (model::speedText (2.0) == "×2");
    CHECK (model::speedText (0.0) == "×0");
    CHECK (model::speedText (1.0594630943592953) == "×1.059");

    Rig rig ("phase4");
    REQUIRE (rig.document.setAttribute ("/godot/cue/P4MED001/rate", "0.5").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/P4MED001/rateMode", "timestretch").ok);

    //  The run's readout is what a fade is moving, so the row reads the run.
    rig.runs.create ("SPEEDRUN", "P4MED001", "media");
    auto* run = rig.runs.find ("SPEEDRUN");
    REQUIRE (run != nullptr);
    run->state = cue::runState::playing;
    run->rateNow = 0.75;

    rig.parameters.markStale();
    const auto snapshot = rig.publish (1);

    auto found = false;

    for (const auto& row : model::readRuns (*snapshot))
        if (row.id == "SPEEDRUN")
        {
            found = true;
            CHECK (row.rate == doctest::Approx (0.75));
            CHECK (model::speedText (row.rate) == "×0.75");
        }

    CHECK (found);

    //  The head row reads the cue: what the document decided, speed and mode.
    const auto reading = model::readFoot (*snapshot, { model::Subject::Kind::waveform, "P4MED001" });
    CHECK (reading.rate == doctest::Approx (0.5));
    CHECK (reading.rateMode == "timestretch");
}

TEST_CASE ("client: a time reads and writes back as the same instant, in either locale")
{
    /*  The table types where the bar drags: a loop point is FOUND with a hand
        and FIXED with a number, and the two have to agree to the millisecond
        or the next range will not start where this one ends.

        BOTH LOCALES, which is why this is worth a case at all: the suite runs
        every serialisation test under fr-FR as well as C, where a hand types
        "4,25" and the document holds "4.25". `osc::formatDouble` and
        `osc::parseDouble` are the one pair in this project that knows the
        difference. */
    CHECK (model::timeText (0.0) == "0");
    CHECK (model::timeText (4.25) == "4.25");
    CHECK (model::timeText (59.999) == "59.999");

    //  Minutes once there are any, and the seconds padded so a column lines up.
    CHECK (model::timeText (60.0) == "1:00");
    CHECK (model::timeText (187.4) == "3:07.4");
    CHECK (model::timeText (612.25) == "10:12.25");

    //  And back again, from either form.
    CHECK (model::timeFrom ("4.25").value() == doctest::Approx (4.25));
    CHECK (model::timeFrom ("  4.25 ").value() == doctest::Approx (4.25));
    CHECK (model::timeFrom ("3:07.4").value() == doctest::Approx (187.4));
    CHECK (model::timeFrom (model::timeText (612.25)).value() == doctest::Approx (612.25));

    /*  WHAT IS NOT A TIME IS NOT NOUGHT. A cell that took "4.2z" as 4.2, or
        an empty one as the top of the file, would move a cue point on a slip
        of the keyboard - so these answer nothing at all and the table puts
        the old number back. */
    CHECK_FALSE (model::timeFrom ("").has_value());
    CHECK_FALSE (model::timeFrom ("  ").has_value());
    CHECK_FALSE (model::timeFrom ("4.2z").has_value());
    CHECK_FALSE (model::timeFrom ("-1").has_value());

    //  Sixty-one seconds into a minute is a typing slip, not a minute and a quarter.
    CHECK_FALSE (model::timeFrom ("1:75").has_value());
}

TEST_CASE ("client: a length copied to the next slice moves its out-point and leaves the join alone")
{
    /*  The author, 2026-09-21: "it would be great to be able copy a duration
        from one slice to the next so the next out point is at the same time
        from the previous". */
    std::vector<model::RangeRow> ranges
    {
        { "R1", "verse", 1.0, 4.0, 1, 0 },      // three seconds long
        { "R2", "chorus", 4.0, 12.0, 2, 1 },    // eight, and joined to R1
        { "R3", "tail", 12.0, 20.0, 1, 2 },
    };

    const auto writes = model::copyLengthToNext (ranges, 0, 30.0);

    /*  ONE WRITE, and it is the NEXT range's out-point: its in-point stays
        where it is, so the join with R1 survives the gesture. Two writes here
        would be a gesture that moved a loop point nobody aimed at. */
    REQUIRE (writes.size() == 1);
    CHECK (writes[0].rangeId == "R2");
    CHECK (std::string (writes[0].attribute) == "out");
    CHECK (writes[0].seconds == doctest::Approx (7.0));   // 4.0 + 3.0

    //  Pressed down the table, it builds a run of equal slices.
    ranges[1].out = 7.0;
    const auto next = model::copyLengthToNext (ranges, 1, 30.0);
    REQUIRE (next.size() == 1);
    CHECK (next[0].rangeId == "R3");
    CHECK (next[0].seconds == doctest::Approx (15.0));    // 12.0 + 3.0

    //  The last range has nothing to copy to, and nothing is written.
    CHECK (model::copyLengthToNext (ranges, 2, 30.0).empty());

    /*  AND IT DECLINES RATHER THAN CLAMPS when the answer would run past the
        end of the file. Clamping is right for a DRAG - the handle waits at
        the end for the hand to come back - but this button means "the same
        length again", and a shorter one silently is the button lying. */
    CHECK (model::copyLengthToNext (ranges, 1, 14.0).empty());

    //  A range with no length has none to give.
    const std::vector<model::RangeRow> flat { { "F1", "", 2.0, 2.0, 1, 0 },
                                              { "F2", "", 3.0, 5.0, 1, 1 } };
    CHECK (model::copyLengthToNext (flat, 0, 30.0).empty());
}


TEST_CASE ("client: the plus cuts where the playhead stands, and declines where there is nothing to cut")
{
    /*  The author, 2026-09-21: *"even if the ranges amount to the full file,
        pressing the [+] range button will split the range where the cursor is.
        No split if the cursor is already on a cut or either the start or end
        of the file."*

        So one button has three answers and the head decides which. */
    const std::vector<model::RangeRow> whole { { "R1", "all", 0.0, 30.0, 1, 0 } };

    //  Inside: a cut, which is how a bed covering the file becomes slices.
    const auto cut = model::addAt (whole, 12.0, 30.0);
    CHECK (cut.kind == model::RangeAdd::Kind::split);
    CHECK (cut.at == doctest::Approx (12.0));

    //  Either end of the file: nothing, and a sentence saying why.
    for (const auto seconds : { 0.0, 30.0 })
    {
        const auto edge = model::addAt (whole, seconds, 30.0);
        CHECK (edge.kind == model::RangeAdd::Kind::nothing);
        CHECK_FALSE (edge.why.empty());
    }

    //  On a cut already: nothing. Two ranges meeting at 12 leave nothing to divide there.
    const std::vector<model::RangeRow> sliced { { "R1", "", 0.0, 12.0, 1, 0 },
                                                { "R2", "", 12.0, 30.0, 1, 1 } };

    const auto onCut = model::addAt (sliced, 12.0, 30.0);
    CHECK (onCut.kind == model::RangeAdd::Kind::nothing);
    CHECK (onCut.why.find ("already") != std::string::npos);

    //  Within a millisecond of one is on it: a hand on a bar does not land on the sample.
    CHECK (model::addAt (sliced, 12.0004, 30.0).kind == model::RangeAdd::Kind::nothing);

    //  And a hair further off is an ordinary cut of the second slice.
    CHECK (model::addAt (sliced, 12.5, 30.0).kind == model::RangeAdd::Kind::split);

    /*  A CUE THAT HAS SAID NOTHING YET gets its one range over the whole file,
        which is the only way to make the FIRST one and is not a split. */
    const auto first = model::addAt ({}, 5.0, 30.0);
    CHECK (first.kind == model::RangeAdd::Kind::create);
    CHECK (first.in == doctest::Approx (0.0));
    CHECK (first.out == doctest::Approx (30.0));

    /*  WHEREVER THE HEAD IS, the edges included (2026-09-30): the head starts
        at nought, and the first range is not a cut. */
    for (const auto seconds : { 0.0, 30.0 })
    {
        const auto atEdge = model::addAt ({}, seconds, 30.0);
        CHECK (atEdge.kind == model::RangeAdd::Kind::create);
        CHECK (atEdge.out == doctest::Approx (30.0));
    }

    /*  IN A GAP, a range from the head to whatever comes next - §3.24 lets a
        cue's regions be neither contiguous nor in file order, so a gap is an
        ordinary place to be and not a fault. */
    const std::vector<model::RangeRow> gapped { { "R1", "", 0.0, 5.0, 1, 0 },
                                                { "R2", "", 20.0, 24.0, 1, 1 } };

    const auto inGap = model::addAt (gapped, 8.0, 30.0);
    CHECK (inGap.kind == model::RangeAdd::Kind::create);
    CHECK (inGap.in == doctest::Approx (8.0));
    CHECK (inGap.out == doctest::Approx (20.0));     // up to the next in-point, not the end

    //  Past the last range, up to the end of the file.
    const auto after = model::addAt (gapped, 26.0, 30.0);
    CHECK (after.kind == model::RangeAdd::Kind::create);
    CHECK (after.out == doctest::Approx (30.0));

    //  And a length nobody knows yet is a reason, not a guess.
    CHECK (model::addAt (whole, 12.0, 0.0).kind == model::RangeAdd::Kind::nothing);
}

TEST_CASE ("client: a new range lands after the material already spoken for")
{
    //  Nothing yet: the whole file.
    const auto empty = model::nextRange ({}, 30.0);
    REQUIRE (empty.has_value());
    CHECK (empty->first == doctest::Approx (0.0));
    CHECK (empty->second == doctest::Approx (30.0));

    /*  AFTER THE LAST ONE IN FILE ORDER and not the last in the list: §3.24
        lets a cue walk its file out of order, so "the end of the playlist" and
        "the end of what is used" are two different instants. */
    const std::vector<model::RangeRow> outOfOrder { { "R1", "", 10.0, 20.0, 1, 0 },
                                                    { "R2", "", 2.0, 5.0, 1, 1 } };

    const auto after = model::nextRange (outOfOrder, 30.0);
    REQUIRE (after.has_value());
    CHECK (after->first == doctest::Approx (20.0));
    CHECK (after->second == doctest::Approx (30.0));

    //  No room, and no length known: two reasons to decline rather than make a nothing.
    CHECK_FALSE (model::nextRange (outOfOrder, 20.0).has_value());
    CHECK_FALSE (model::nextRange ({}, 0.0).has_value());
}

TEST_CASE ("client: the inspector offers the panels a kind actually has, and no others")
{
    /*  The author, 2026-09-21: "the controls to show the waveform, the send
        levels, the EQ, the group timeline were in the inspector. No hunting in
        the menus." Four were asked for and one is built, and this case is what
        keeps the offer honest as the other three land: a button that opened
        nothing would teach somebody the feature is broken rather than absent. */
    const auto onMedia = model::openersFor ("media", "B3N8R5TW");

    /*  THREE SINCE PHASE 9a: the waveform, the EQ - the second of the four
        the author named, drawn at the foot as a response a hand can shape -
        and the signal chain, whose boxes open each plugin's own window. */
    REQUIRE (onMedia.size() >= 3);
    CHECK (onMedia[0].control == model::Control::opener);
    CHECK (onMedia[0].value == "waveform");
    CHECK (onMedia[0].address == "B3N8R5TW");
    CHECK_FALSE (onMedia[0].label.empty());
    CHECK (onMedia[1].control == model::Control::opener);
    CHECK (onMedia[1].value == "eq");
    CHECK (onMedia[1].address == "B3N8R5TW");
    CHECK (onMedia[1].label.find ("EQ") != std::string::npos);
    CHECK (onMedia[2].control == model::Control::opener);
    CHECK (onMedia[2].value == "fx");
    CHECK (onMedia[2].address == "B3N8R5TW");
    CHECK (onMedia[2].label.find ("FX") != std::string::npos);
    CHECK_FALSE (onMedia[2].writable);

    /*  AND THE SEND LEVELS, since the panels became a bar at the head of the
        inspector (2026-09-30): every panel a cue has, in the order its sound
        goes - where it is sent last. */
    REQUIRE (onMedia.size() == 4);
    CHECK (onMedia[3].control == model::Control::opener);
    CHECK (onMedia[3].value == "sends");
    CHECK (onMedia[3].label.find ("Sends") != std::string::npos);

    //  An opener is a door and not a decision: it writes nothing.
    CHECK_FALSE (onMedia[0].writable);
    CHECK_FALSE (onMedia[1].writable);

    /*  A GROUP HAS ONE TOO, since 2026-09-22: its members laid out in time,
        which is a thing only a container has. Offered for a sequence as well
        as a timeline - seeing the shape is worth the look even where nothing
        can be dragged - and the panel says which it is. */
    const auto onGroup = model::openersFor ("group", "B3N8R5TW");

    REQUIRE (onGroup.size() == 1);
    CHECK (onGroup[0].value == "timeline");
    CHECK_FALSE (onGroup[0].writable);

    /*  AND A FADE HAS ONE, since 2026-09-22: the shape it takes, which is the
        one thing about a fade that is not a row. Offered even though picking a
        fade opens it by itself, because the row is also how it is SHUT - a
        panel that opened on its own and could only be closed from somewhere
        else would be a trap. */
    const auto onFade = model::openersFor ("fade", "B3N8R5TW");

    /*  AND SINCE 2026-10-03 ITS MIXER FIRST (namespace draft §26, PG): what
        the fade moves, then the target's EQ on the fade, then the curve. */
    REQUIRE (onFade.size() == 3);
    CHECK (onFade[0].value == "fade");
    CHECK (onFade[1].value == "eq");
    CHECK (onFade[2].value == "curve");
    CHECK_FALSE (onFade[2].writable);

    //  And the kinds with nothing longer to look at still offer nothing.
    for (const auto* kind : { "wait", "message", "osc" })
        CHECK (model::openersFor (kind, "B3N8R5TW").empty());

    /*  AND THEY ARRIVE AS THE PANEL BAR, not among the rows (author,
        2026-09-30: "the toggles for the foot panels in the inspector should be
        at the top to make opening the panel really quick"): the inspection
        carries them apart, in the order `openersFor` gives, and no block holds
        a door. */
    Rig rig ("phase4");
    const auto snapshot = rig.publish (0);
    const auto inspection = model::inspect (*snapshot, "P4MED001");

    REQUIRE (inspection.panels.size() == onMedia.size());

    for (std::size_t at = 0; at < onMedia.size(); ++at)
        CHECK (inspection.panels[at].value == onMedia[at].value);

    for (const auto& block : inspection.blocks)
        for (const auto& field : block.fields)
            CHECK (field.control != model::Control::opener);

    /*  SEVERAL CUES AT ONCE HAVE THE PANELS THAT ACT ON ALL OF THEM (namespace
        draft §30.11) - the EQ and the sends - and no others: the case below
        has the rest. */
    const auto several = model::inspectMany (*snapshot, { "P4MED001", "P4MED002" });

    REQUIRE (several.panels.size() == 2u);
    CHECK (several.panels[0].value == "eq");
    CHECK (several.panels[1].value == "sends");
}

TEST_CASE ("client: a zoomed bar reads finer frames of a shorter span, not the same ones wider")
{
    /*  The pyramid exists so a bar of any width reads one level and stops
        (PRD 3.30). Zooming has to walk DOWN it - finer frames over a shorter
        window - or a zoomed-in bar would be the same handful of columns drawn
        fatter, which looks like a fault and is one. */
    audio::TimbrePyramid pyramid;
    pyramid.sampleRate = 48000;
    pyramid.samples = 48000ull * 60ull;   // a minute

    for (const auto count : { 2048, 1024, 512, 256, 128, 64 })
    {
        std::vector<audio::timbre::Frame> level;

        for (auto at = 0; at < count; ++at)
        {
            audio::timbre::Frame frame;
            //  One loud frame a tenth of the way in, and silence elsewhere.
            frame.peak = static_cast<std::uint8_t> (at == count / 10 ? 255 : 0);
            frame.saturation = 200;
            frame.lightness = 128;
            level.push_back (frame);
        }

        pyramid.levels.push_back (std::move (level));
    }

    CHECK (model::lengthOf (pyramid) == doctest::Approx (60.0));

    const auto whole = model::waveform (pyramid, 200, 0.0, 60.0);
    const auto window = model::waveform (pyramid, 200, 20.0, 24.0);

    REQUIRE (whole.size() == 200u);
    REQUIRE (window.size() == 200u);

    const auto loudest = [] (const std::vector<model::Column>& columns)
    {
        auto best = std::size_t { 0 };

        for (std::size_t at = 1; at < columns.size(); ++at)
            if (columns[at].peak > columns[best].peak)
                best = at;

        return best;
    };

    //  The loud frame is a tenth in, so the whole-file bar shows it near 20 of 200.
    CHECK (whole[loudest (whole)].peak > 0.9);
    CHECK (loudest (whole) > 10u);
    CHECK (loudest (whole) < 32u);

    //  Twenty seconds in is past it, so that window is quiet throughout.
    CHECK (window[loudest (window)].peak < 0.5);

    //  A window backwards, or no width at all, draws nothing rather than guessing.
    CHECK (model::waveform (pyramid, 200, 40.0, 30.0).empty());
    CHECK (model::waveform (pyramid, 0, 0.0, 60.0).empty());
}

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

TEST_CASE ("client: a drawn fade, and every string it writes is one the engine accepts")
{
    /*  THE ANTI-DRIFT CHECK, and the reason `model/Curve` may exist at all.
        `doc::readFadePoints` is the ONE judge of what a curve is - the write
        door, `validate` and the Runner all ask it - and the client boundary
        forbids naming `doc::`, so the rules are stated a second time in the
        client. A copy nobody compares is a copy that drifts; this is the
        comparison. */
    const auto accepted = [] (const std::vector<model::CurvePoint>& points)
    {
        const auto text = model::writePoints (points);
        const auto judged = doc::readFadePoints (text);

        INFO ("wrote: " << text);
        INFO ("engine said: " << judged.problem);

        return judged.problem.empty() && judged.points.size() == points.size();
    };

    SUBCASE ("a straight line, and a drawn shape, both round-trip")
    {
        CHECK (accepted ({ { 0.0, 0.0 }, { 1.0, -120.0 } }));
        CHECK (accepted ({ { 0.0, 0.0 }, { 0.5, -30.0 }, { 1.0, -10.0 } }));
        CHECK (accepted ({ { 0.0, -6.5 }, { 0.25, 3.25 }, { 0.75, -12.75 }, { 1.0, -120.0 } }));
    }

    SUBCASE ("and the numbers survive a French locale, which is where a comma would get in")
    {
        const auto text = model::writePoints ({ { 0.0, 0.0 }, { 0.5, -6.5 }, { 1.0, -120.0 } });

        CHECK (text.find (',') == std::string::npos);
        CHECK (doc::readFadePoints (text).problem.empty());
    }

    SUBCASE ("the window refuses what the door would refuse, rather than finding out after")
    {
        //  Each of these is a shape `readFadePoints` turns down, named here first.
        CHECK_FALSE (model::whyNotACurve ({ { 0.2, 0.0 }, { 1.0, -10.0 } }).empty());
        CHECK_FALSE (model::whyNotACurve ({ { 0.0, 0.0 }, { 0.8, -10.0 } }).empty());
        CHECK_FALSE (model::whyNotACurve ({ { 0.0, 0.0 }, { 0.5, -3.0 }, { 0.5, -9.0 },
                                            { 1.0, -10.0 } }).empty());
        CHECK_FALSE (model::whyNotACurve ({ { 0.0, 0.0 }, { 1.0, -400.0 } }).empty());

        //  An EMPTY list is not a bad curve, it is the absence of one.
        CHECK (model::whyNotACurve ({}).empty());
        CHECK (model::writePoints ({}).empty());
        CHECK (doc::readFadePoints ("").problem.empty());
    }
}

TEST_CASE ("client: a level lane the window writes is one the engine's judge accepts")
{
    /*  `doc::readLevelLane` is the ONE judge of what a lane is (namespace
        draft §20.3) - the door, `validate` and the Runner - and `model/Lane`
        restates its rules because the boundary forbids reaching for them. So
        every string the window can write is put to the real judge here. */
    const auto accepted = [] (const std::vector<model::LanePoint>& points)
    {
        const auto text = model::writeLane (points);
        const auto judged = doc::readLevelLane (text);

        INFO ("wrote: " << text);
        INFO ("engine said: " << judged.problem);

        return judged.problem.empty() && judged.points.size() == points.size();
    };

    SUBCASE ("one point, a dip, and a lane over the middle of a file all round-trip")
    {
        CHECK (accepted ({ { 12.5, -6.0 } }));
        CHECK (accepted ({ { 4.0, 0.0 }, { 5.0, -20.0 }, { 8.0, -20.0 }, { 9.0, 0.0 } }));
        CHECK (accepted ({ { 0.0, -120.0 }, { 0.25, 12.0 }, { 187.4, -3.25 } }));
    }

    SUBCASE ("and the numbers survive a French locale, which is where a comma would get in")
    {
        const auto text = model::writeLane ({ { 4.5, -6.5 }, { 10.25, -120.0 } });

        CHECK (text.find (',') == std::string::npos);
        CHECK (doc::readLevelLane (text).problem.empty());
    }

    SUBCASE ("the window refuses what the door would refuse, rather than finding out after")
    {
        CHECK_FALSE (model::whyNotALane ({ { -0.5, 0.0 } }).empty());
        CHECK_FALSE (model::whyNotALane ({ { 4.0, 0.0 }, { 4.0, -6.0 } }).empty());
        CHECK_FALSE (model::whyNotALane ({ { 5.0, 0.0 }, { 4.0, -6.0 } }).empty());
        CHECK_FALSE (model::whyNotALane ({ { 4.0, -400.0 } }).empty());

        //  An EMPTY list is not a bad lane, it is the absence of one.
        CHECK (model::whyNotALane ({}).empty());
        CHECK (model::writeLane ({}).empty());
    }

    SUBCASE ("and what the window draws is what the engine plays")
    {
        const std::vector<model::LanePoint> points { { 4.0, 0.0 }, { 5.0, -20.0 }, { 9.0, 0.0 } };
        const auto engine = doc::readLevelLane (model::writeLane (points));
        REQUIRE (engine.problem.empty());

        for (const auto seconds : { 0.0, 4.0, 4.3, 5.0, 6.75, 9.0, 12.0 })
        {
            INFO ("at " << seconds << " s");
            CHECK (model::laneLevelAt (points, seconds)
                     == doctest::Approx (doc::laneLevelDb (engine.points, seconds)));
        }
    }
}

TEST_CASE ("client: drawing a level lane over a waveform, point by point")
{
    const std::vector<model::LanePoint> dip { { 4.0, 0.0 }, { 5.0, -20.0 }, { 9.0, 0.0 } };

    SUBCASE ("the first point on a lane is a constant offset of nothing, so nothing moves")
    {
        const auto made = model::insertLanePoint ({}, 3.0, 30.0);

        REQUIRE (made.has_value());
        REQUIRE (made->size() == 1u);
        CHECK (made->front().seconds == doctest::Approx (3.0));
        CHECK (made->front().levelDb == doctest::Approx (0.0));
    }

    SUBCASE ("a point added lands on the line, in order, and changes nothing until it moves")
    {
        const auto added = model::insertLanePoint (dip, 7.0, 30.0);

        REQUIRE (added.has_value());
        REQUIRE (added->size() == 4u);
        CHECK ((*added)[2].seconds == doctest::Approx (7.0));
        CHECK ((*added)[2].levelDb == doctest::Approx (-10.0));

        for (const auto seconds : { 3.0, 4.5, 7.0, 8.5, 11.0 })
            CHECK (model::laneLevelAt (*added, seconds) == doctest::Approx (model::laneLevelAt (dip, seconds)));

        //  Beyond both ends it holds, so a point added out there is on the held line.
        const auto early = model::insertLanePoint (dip, 1.0, 30.0);
        REQUIRE (early.has_value());
        CHECK (early->front().levelDb == doctest::Approx (0.0));

        //  Not on top of another, and not outside the file.
        CHECK_FALSE (model::insertLanePoint (dip, 5.0, 30.0).has_value());
        CHECK_FALSE (model::insertLanePoint (dip, 31.0, 30.0).has_value());
        CHECK_FALSE (model::insertLanePoint (dip, -1.0, 30.0).has_value());
    }

    SUBCASE ("a dragged point stays between its neighbours, in the file, at a level a cue may take")
    {
        auto moved = model::dragLanePoint (dip, 1, 11.0, -200.0, 30.0);
        CHECK (moved.seconds < 9.0);
        CHECK (moved.seconds > 8.99);
        CHECK (moved.levelDb == doctest::Approx (-120.0));

        moved = model::dragLanePoint (dip, 1, 2.0, 40.0, 30.0);
        CHECK (moved.seconds > 4.0);
        CHECK (moved.seconds < 4.01);
        CHECK (moved.levelDb == doctest::Approx (12.0));

        //  THE ENDS MOVE IN TIME TOO - a lane has no ends it must keep - but
        //  not before the file starts, nor past its end.
        CHECK (model::dragLanePoint (dip, 0, -3.0, 0.0, 30.0).seconds == doctest::Approx (0.0));
        CHECK (model::dragLanePoint (dip, 2, 45.0, 0.0, 30.0).seconds == doctest::Approx (30.0));

        //  And the lane with it moved is still a lane.
        const auto lane = model::withLanePoint (dip, 1, 6.5, -9.0, 30.0);
        CHECK (model::whyNotALane (lane).empty());
        CHECK (lane[1].seconds == doctest::Approx (6.5));
    }

    SUBCASE ("any point may go, and the last one gone is no lane")
    {
        CHECK (model::removeLanePoint (dip, 0).size() == 2u);
        CHECK (model::removeLanePoint (dip, 2).size() == 2u);
        CHECK (model::removeLanePoint ({ { 3.0, -6.0 } }, 0).empty());
        CHECK (model::removeLanePoint (dip, 7).size() == 3u);
    }

    SUBCASE ("drawn on a fader's throw, and found by how close it looks")
    {
        //  The strips' own law: unity high up, silence at the bottom (DD).
        CHECK (model::laneHeightFor (0.0) == doctest::Approx (model::fractionForDb (0.0)));
        CHECK (model::laneHeightFor (-120.0) == doctest::Approx (0.0));
        CHECK (model::laneLevelForHeight (model::laneHeightFor (-9.5)) == doctest::Approx (-9.5));

        const auto atFive = model::laneHeightFor (-20.0);

        CHECK (model::nearestLanePoint (dip, 5.02, atFive + 0.01, 0.1, 0.05) == 1u);
        CHECK (model::nearestLanePoint (dip, 6.0, atFive, 0.1, 0.05) == static_cast<std::size_t> (-1));
        CHECK (model::onLaneLine (dip, 7.0, model::laneHeightFor (-10.0), 0.02));
        CHECK_FALSE (model::onLaneLine (dip, 7.0, model::laneHeightFor (0.0), 0.02));

        //  An empty lane's line is unity, where the cue is as written.
        CHECK (model::onLaneLine ({}, 12.0, model::laneHeightFor (0.0), 0.02));
    }

    SUBCASE ("a typed level reads the way the window writes one")
    {
        CHECK (model::levelFrom ("-6").value() == doctest::Approx (-6.0));
        CHECK (model::levelFrom (" -6.5 dB").value() == doctest::Approx (-6.5));
        CHECK (model::levelFrom ("+3").value() == doctest::Approx (3.0));
        CHECK (model::levelFrom ("silence").value() == doctest::Approx (-120.0));

        //  And whatever the box itself shows reads back as the same level.
        for (const auto level : { -120.0, -18.0, -6.5, 0.0, 3.0 })
            CHECK (model::levelFrom (model::faderText (level) + " dB").value() == doctest::Approx (level));
        CHECK_FALSE (model::levelFrom ("loud").has_value());
        CHECK_FALSE (model::levelFrom ("40").has_value());
        CHECK_FALSE (model::levelFrom ("").has_value());
    }
}

TEST_CASE ("client: the waveform's reading carries the cue's level lane, and the lock")
{
    Rig rig ("phase4");

    const auto lane = model::laneAddress ("P4MED001");

    REQUIRE (rig.apply (1, "cli", "node.set", { osc::Value::string (lane),
                                                osc::Value::string ("4 0 5 -20 9 0") }).applied == 1);

    const auto reading = model::readFoot (*rig.publish (1),
                                          { model::Subject::Kind::waveform, "P4MED001" });

    REQUIRE (reading.lane.size() == 3u);
    CHECK (reading.lane[1].seconds == doctest::Approx (5.0));
    CHECK (reading.lane[1].levelDb == doctest::Approx (-20.0));
    CHECK_FALSE (reading.locked);

    //  A cue that is not media has no lane to read, whatever its rows say.
    CHECK (model::readLane (*rig.publish (1), "P4GRP001").empty());
}

TEST_CASE ("client: the waveform's reading carries a lane for each send the cue has")
{
    /*  Namespace draft §28, QB: the picker's entries after Level are the
        cue's sends, by their mix's name, each with its own lane - and a mix
        the cue does not send into is not one of them. The fixture's cue sends
        into the foldback and not into the reverb. */
    Rig rig ("live");

    CHECK (model::sendLaneAddress ("RV000006") == "/godot/send/RV000006/levelLane");

    REQUIRE (rig.apply (1, "cli", "node.set", { osc::Value::string (model::sendLaneAddress ("RV000006")),
                                                osc::Value::string ("1 -30 3 0") }).applied == 1);

    const auto snapshot = rig.publish (1);
    const auto reading = model::readFoot (*snapshot, { model::Subject::Kind::waveform, "RV000002" });

    REQUIRE (reading.sendLanes.size() == 1u);
    CHECK (reading.sendLanes[0].sendId == "RV000006");
    CHECK (reading.sendLanes[0].busName == "Foldback");
    REQUIRE (reading.sendLanes[0].points.size() == 2u);
    CHECK (reading.sendLanes[0].points[0].levelDb == doctest::Approx (-30.0));
    CHECK (reading.sendLanes[0].points[1].seconds == doctest::Approx (3.0));

    //  The level's own lane is the cue's, and none is drawn on it.
    CHECK (reading.lane.empty());

    //  Read by its address, the same points.
    CHECK (model::readLaneAt (*snapshot, model::sendLaneAddress ("RV000006")).size() == 2u);
}

TEST_CASE ("client: the faders flipped to a cue read as the tree says, a lane to each fader by the name a person reads")
{
    /*  Namespace draft §34: the cue the faders show, whether a pass runs, and
        a fader for each lane in strip order - the level, then the show's mixes
        by name - armed or not, riding the number as heard. The waveform's
        reading carries it, whichever cue it is for, so the panel can say when
        it is another's. */
    Rig rig ("surfaces");
    cue::LaneTable lanes;
    rig.parameters.setLanes (&lanes);

    auto reading = model::readLaneRecord (*rig.publish (0));
    CHECK (reading.cue.empty());
    CHECK_FALSE (reading.flipped);
    REQUIRE_FALSE (reading.faders.empty());
    CHECK (reading.faders[0].key == "level");
    CHECK_FALSE (reading.faders[0].hasRide);

    lanes.flip ("SRF00005");
    lanes.rideOf ("level").rideDb = -12.0;
    lanes.setArmed ("level", true);
    rig.parameters.markStale();
    reading = model::readLaneRecord (*rig.publish (1));

    CHECK (reading.cue == "SRF00005");
    CHECK (reading.flipped);
    CHECK_FALSE (reading.recording);

    const auto* level = reading.faderOf ("level");
    REQUIRE (level != nullptr);
    CHECK (level->name == "Level");
    CHECK (level->armed);
    REQUIRE (level->hasRide);
    CHECK (level->rideDb == doctest::Approx (-12.0));
    CHECK (reading.armedNames() == std::vector<std::string> { "Level" });

    //  Every mix of the show is a fader after it, by its name.
    for (std::size_t k = 1; k < reading.faders.size(); ++k)
    {
        CHECK_FALSE (reading.faders[k].name.empty());
        CHECK_FALSE (reading.faders[k].armed);
    }

    lanes.startPass ("RN000001");
    rig.parameters.markStale();
    CHECK (model::readLaneRecord (*rig.publish (3)).recording);

    const auto foot = model::readFoot (*rig.publish (3), { model::Subject::Kind::waveform, "SRF00005" });
    CHECK (foot.laneRecord.recording);
    CHECK (foot.laneRecord.cue == "SRF00005");

    //  A ride's address says its lane; any other says none.
    CHECK (model::laneKeyOfRide ("/godot/surface/laneRide") == std::optional<std::string> ("level"));
    CHECK (model::laneKeyOfRide ("/godot/bus/SN000010/laneRide") == std::optional<std::string> ("SN000010"));
    CHECK_FALSE (model::laneKeyOfRide ("/godot/dca/D1/trim").has_value());
}

TEST_CASE ("client: what a lane's last pass ended in is read from the tree and said in words, a full stop in every locale")
{
    /*  Namespace draft §30.4, §34: `lanePass` says what the pass did - the
        points it wrote, the seconds they span and the lanes, or that nothing
        was written and why - under the recorder's own name, the button's. */
    Rig rig ("surfaces");
    cue::LaneTable lanes;
    rig.parameters.setLanes (&lanes);

    auto reading = model::readLaneRecord (*rig.publish (0));
    CHECK (reading.pass.tick < 0);
    CHECK (model::lanePassWords (reading.pass).empty());

    lanes.flip ("SRF00005");
    lanes.startPass ("RN000001");
    lanes.endPass ("4242 SRF00005 kept 7 12 41.5 level");
    rig.parameters.markStale();
    reading = model::readLaneRecord (*rig.publish (1));

    CHECK (reading.flipped);                    // UM: the faders stay on the cue
    CHECK (reading.pass.tick == 4242);
    CHECK (reading.pass.cue == "SRF00005");
    CHECK (reading.pass.how == "kept");
    REQUIRE (reading.pass.spans);
    CHECK (reading.pass.points == 7);
    CHECK (reading.pass.from == doctest::Approx (12.0));
    CHECK (reading.pass.to == doctest::Approx (41.5));
    CHECK (reading.pass.lanes == std::vector<std::string> { "Level" });
    CHECK (model::lanePassWords (reading.pass) == "Autom.: Level - 7 points, 12.0\xe2\x80\x93" "41.5 s");

    model::LanePass pass;
    pass.tick = 10;
    pass.cue = "SRF00005";

    //  Past a minute, as the ruler writes it; one point is a point; two lanes by name.
    pass.how = "kept";
    pass.spans = true;
    pass.points = 1;
    pass.from = 150.25;
    pass.to = 224.3;
    pass.lanes = { "Level", "Face" };
    CHECK (model::lanePassWords (pass) == "Autom.: Level, Face - 1 point, 2:30.3\xe2\x80\x93" "3:44.3");

    //  A record from before the numbers were carried.
    pass.spans = false;
    CHECK (model::lanePassWords (pass) == "Autom.: written");

    //  And every end that wrote nothing, saying why.
    pass.how = "untouched";
    CHECK (model::lanePassWords (pass) == "Autom.: nothing written - no armed fader was touched while the cue played");
    pass.how = "locked";
    CHECK (model::lanePassWords (pass) == "Autom.: nothing written - the show is locked");
    pass.how = "dropped";
    CHECK (model::lanePassWords (pass) == "Autom.: nothing written - the pass was dropped");
}

TEST_CASE ("client: the lanes' refusals read as sentences under the recorder's name")
{
    /*  Namespace draft §30.4, §34: a pass that does not start writes nothing,
        and that is said in words rather than left to a code. */
    model::TransportReading reading;

    reading.lastError = "5411 26 window not-flipped lane.record";
    CHECK (reading.errorLine() == "Autom. not recorded: the faders show no cue - press Autom. first");

    reading.lastError = "5411 26 window busy lane.record";
    CHECK (reading.errorLine() == "Autom.: a pass is running - stop it first");

    reading.lastError = "5411 26 window locked lane.record";
    CHECK (reading.errorLine() == "Autom. not recorded: the show is locked");

    reading.lastError = "5411 26 window locked lane.arm";
    CHECK (reading.errorLine() == "Autom. not armed: the show is locked");

    reading.lastError = "5411 26 window locked lane.rec";
    CHECK (reading.errorLine() == "Autom. not armed: the show is locked");

    reading.lastError = "5411 26 window bad-value lane.arm";
    CHECK (reading.errorLine() == "Autom. records a media cue's level and sends only");

    reading.lastError = "5411 26 window busy lane.free";
    CHECK (reading.errorLine() == "Autom.: a pass is running - stop it first");

    //  Anything else on the lanes is the engine's words, as before.
    reading.lastError = "5411 26 window bad-value lane.rec";
    CHECK (reading.errorLine() == "lane.rec refused: bad-value");
}

TEST_CASE ("client: a view follows a playhead at a readable scale, paging rather than scrolling, and frames a stretch")
{
    /*  Namespace draft §30.4: a 647-second file drawn whole is a pixel and a
        half a second, and a ride drawn at that scale is a smudge. While a pass
        records the window follows no wider than a minute (or as close as the
        hand had zoomed), and when it ends what was written is framed. */
    model::View view;
    view.reset (647.0);

    //  Whole, it narrows to a minute, the playhead in it.
    view.follow (10.0, 60.0);
    CHECK (view.span() == doctest::Approx (60.0));
    CHECK (view.from == doctest::Approx (0.0));
    CHECK (view.to == doctest::Approx (60.0));

    //  Inside the window it stays still - the picture does not move under the eye.
    view.follow (40.0, 60.0);
    CHECK (view.from == doctest::Approx (0.0));

    //  Into its last tenth it pages, the playhead a quarter of the way in.
    view.follow (55.0, 60.0);
    CHECK (view.from == doctest::Approx (40.0));
    CHECK (view.span() == doctest::Approx (60.0));

    //  A playhead outside the window - a seek - pages to it.
    view.follow (300.0, 60.0);
    CHECK (view.from == doctest::Approx (285.0));

    //  Near the end it stays inside the file.
    view.follow (640.0, 60.0);
    CHECK (view.to == doctest::Approx (647.0));
    CHECK (view.span() == doctest::Approx (60.0));

    //  Zoomed closer by hand, the hand's zoom is kept.
    view.from = 100.0;
    view.to = 110.0;
    view.follow (105.0, 60.0);
    CHECK (view.from == doctest::Approx (100.0));
    view.follow (120.0, 60.0);
    CHECK (view.span() == doctest::Approx (10.0));
    CHECK (view.from == doctest::Approx (117.5));

    //  Framing what a pass wrote: a tenth of its length either side, a second at the least.
    view.frame (12.0, 41.5);
    CHECK (view.from == doctest::Approx (9.05));
    CHECK (view.to == doctest::Approx (44.45));

    view.frame (0.0, 0.5);
    CHECK (view.from == doctest::Approx (0.0));
    CHECK (view.span() == doctest::Approx (2.5));

    //  Wider than the file, the file.
    view.frame (0.0, 640.0);
    CHECK (view.isWholeThing());
}

TEST_CASE ("client: drawing on a fade, point by point")
{
    const std::vector<model::CurvePoint> line { { 0.0, 0.0 }, { 1.0, -20.0 } };

    SUBCASE ("a worded fade becomes a drawn one shaped like the word, so nothing jumps")
    {
        /*  A fade with no points plays its `curve`. The first thing drawn has
            to make a curve the rules accept - two ends and the point asked
            for - and the ends take the levels the fade already has. */
        const auto made = model::insertAt ({}, 0.5, 0.0, -20.0);

        REQUIRE (made.has_value());
        REQUIRE (made->size() == 3);
        CHECK ((*made)[0].t == doctest::Approx (0.0));
        CHECK ((*made)[2].t == doctest::Approx (1.0));
        CHECK ((*made)[2].levelDb == doctest::Approx (-20.0));

        //  And the new one sits on the line the word already drew.
        CHECK ((*made)[1].levelDb == doctest::Approx (-10.0));
        CHECK (model::whyNotACurve (*made).empty());
    }

    SUBCASE ("a point added to a drawn curve changes the shape not at all")
    {
        /*  Which is what makes adding one safe to do while listening: it lands
            on the line that is already there, and only moving it does anything. */
        const auto more = model::insertAt (line, 0.25, 0.0, -20.0);

        REQUIRE (more.has_value());
        REQUIRE (more->size() == 3);
        CHECK ((*more)[1].t == doctest::Approx (0.25));
        CHECK ((*more)[1].levelDb == doctest::Approx (-5.0));
    }

    SUBCASE ("and one at a moment already taken is refused rather than doubled")
    {
        CHECK_FALSE (model::insertAt (line, 0.0, 0.0, -20.0).has_value());
        CHECK_FALSE (model::insertAt (line, 1.0, 0.0, -20.0).has_value());
    }

    SUBCASE ("the ends move in level and never in time")
    {
        /*  A curve that began after nought would leave a stretch of the fade
            it says nothing about, which the engine's door refuses. */
        const auto first = model::dragTo (line, 0, 0.4, -6.0);
        CHECK (first.t == doctest::Approx (0.0));
        CHECK (first.levelDb == doctest::Approx (-6.0));

        const auto last = model::dragTo (line, 1, 0.6, -3.0);
        CHECK (last.t == doctest::Approx (1.0));
        CHECK (last.levelDb == doctest::Approx (-3.0));
    }

    SUBCASE ("and a middle one stays strictly inside its neighbours")
    {
        const std::vector<model::CurvePoint> three { { 0.0, 0.0 }, { 0.5, -10.0 },
                                                     { 1.0, -20.0 } };

        CHECK (model::dragTo (three, 1, -1.0, 0.0).t > 0.0);
        CHECK (model::dragTo (three, 1, 2.0, 0.0).t < 1.0);

        //  A level outside what a fade may reach is clamped, not written.
        CHECK (model::dragTo (three, 1, 0.5, -400.0).levelDb
                 == doctest::Approx (model::quietestDb));
        CHECK (model::dragTo (three, 1, 0.5, 400.0).levelDb
                 == doctest::Approx (model::loudestFadeDb));
    }

    SUBCASE ("neither end can be taken away, and a middle one can")
    {
        const std::vector<model::CurvePoint> three { { 0.0, 0.0 }, { 0.5, -10.0 },
                                                     { 1.0, -20.0 } };

        CHECK_FALSE (model::removeAt (three, 0).has_value());
        CHECK_FALSE (model::removeAt (three, 2).has_value());

        const auto fewer = model::removeAt (three, 1);
        REQUIRE (fewer.has_value());
        CHECK (fewer->size() == 2);
        CHECK (model::whyNotACurve (*fewer).empty());
    }
}

TEST_CASE ("client: a timeline group's members are bars, and a sequence's are a consequence")
{
    /*  PRD 3.6: a timeline schedules every member at the group's entry and
        each one's pre-wait is its OFFSET from that moment - so a bar's left
        edge IS its pre-wait, and moving it writes one number. A sequence runs
        them one after another, where a member's position is arithmetic over
        everything above it. */
    Rig rig;

    const auto list = "7K2QM9X4";

    rig.apply (1, "window", "cue.create",
               { osc::Value::string (list), osc::Value::int32 (0),
                 osc::Value::string ("group"), osc::Value::string ("Scene"),
                 osc::Value::string ("GRPXXXX1") });

    const auto member = [&rig] (std::int64_t tick, const char* id, const char* name,
                                int at, const char* preWait)
    {
        rig.apply (tick, "window", "cue.create",
                   { osc::Value::string ("GRPXXXX1"), osc::Value::int32 (at),
                     osc::Value::string ("memo"), osc::Value::string (name),
                     osc::Value::string (id) });

        rig.apply (tick + 1, "window", "node.set",
                   { osc::Value::string (std::string ("/godot/cue/") + id + "/preWait"),
                     osc::Value::string (preWait) });
    };

    member (2, "MEMXXXX1", "First",  0, "1");
    member (4, "MEMXXXX2", "Second", 1, "4");

    SUBCASE ("a sequence says so and is not arranged by dragging")
    {
        const auto reading = model::readTimeline (*rig.publish (6), "GRPXXXX1");

        CHECK (reading.mode == "sequence");
        CHECK_FALSE (reading.draggable);
        REQUIRE (reading.bars.size() == 2);

        /*  A memo takes no time, so the second starts at its own pre-wait
            after the first - which is a sum and not a decision. */
        CHECK (reading.bars[0].at == doctest::Approx (1.0));
        CHECK (reading.bars[1].at == doctest::Approx (5.0));
    }

    SUBCASE ("a timeline puts every member at its own pre-wait, and can be dragged")
    {
        rig.apply (6, "window", "node.set",
                   { osc::Value::string ("/godot/cue/GRPXXXX1/mode"),
                     osc::Value::string ("timeline") });

        const auto reading = model::readTimeline (*rig.publish (7), "GRPXXXX1");

        CHECK (reading.mode == "timeline");
        CHECK (reading.draggable);
        REQUIRE (reading.bars.size() == 2);

        //  Both measured from the group's entry, not from each other.
        CHECK (reading.bars[0].at == doctest::Approx (1.0));
        CHECK (reading.bars[1].at == doctest::Approx (4.0));
        CHECK (reading.bars[0].name == "First");
    }

    SUBCASE ("a cue that is not a group has no members to arrange")
    {
        const auto reading = model::readTimeline (*rig.publish (6), "B3N8R5TW");

        CHECK (reading.bars.empty());
        CHECK_FALSE (reading.notice.empty());
    }
}

TEST_CASE ("client: a nested group's bar is as long as what is inside it")
{
    /*  The author, 2026-09-22: "can you resolve nested groups?" No group's
        length is published - the engine keeps it inside its own static walk -
        but everything the sum is MADE of is published a level at a time, so
        the client walks down and does the same arithmetic.

        Under the same guards, and each one is a reason the answer cannot be
        known rather than a formality. */
    Rig rig ("phase4");

    const auto reading = [&rig] (std::int64_t tick, const char* group)
    {
        return model::readTimeline (*rig.publish (tick), group);
    };

    /*  `phase4` has a scene group with a ramp and two beds in it. Made a
        timeline so its own members are placed by their pre-waits, and wrapped
        so there is a nested one to resolve. */
    rig.apply (1, "window", "cue.create",
               { osc::Value::string ("P4ACT001"), osc::Value::int32 (0),
                 osc::Value::string ("group"), osc::Value::string ("Outer"),
                 osc::Value::string ("GRPAXXX1") });

    rig.apply (2, "window", "node.set",
               { osc::Value::string ("/godot/cue/GRPAXXX1/mode"),
                 osc::Value::string ("timeline") });

    rig.apply (3, "window", "cue.create",
               { osc::Value::string ("GRPAXXX1"), osc::Value::int32 (0),
                 osc::Value::string ("group"), osc::Value::string ("Inner"),
                 osc::Value::string ("GRPBXXX1") });

    rig.apply (4, "window", "node.set",
               { osc::Value::string ("/godot/cue/GRPBXXX1/mode"),
                 osc::Value::string ("timeline") });

    /*  Asserted rather than assumed. Identifiers are Crockford base32 and the
        alphabet skips I, L, O and U, so a test id with one of those in it
        creates nothing at all and every check below fails somewhere else
        entirely. */
    REQUIRE (model::text (*rig.publish (4), "/godot/cue/GRPBXXX1/kind") == "group");

    SUBCASE ("an empty nested group reaches nowhere, which is a length and not an unknown")
    {
        const auto outer = reading (5, "GRPAXXX1");

        REQUIRE (outer.bars.size() == 1);
        CHECK (outer.bars[0].isGroup);
        CHECK (outer.bars[0].lengthKnown);
        CHECK (outer.bars[0].length == doctest::Approx (0.0));
    }

    SUBCASE ("and it is as long as the furthest thing inside it reaches")
    {
        /*  A memo takes no time, so the inner group's extent is its member's
            pre-wait: place one at four seconds and the inner group is four
            seconds long, measured from its OWN entry. */
        rig.apply (5, "window", "cue.create",
                   { osc::Value::string ("GRPBXXX1"), osc::Value::int32 (0),
                     osc::Value::string ("memo"), osc::Value::string ("Mark"),
                     osc::Value::string ("MRKXXXX1") });

        rig.apply (6, "window", "node.set",
                   { osc::Value::string ("/godot/cue/MRKXXXX1/preWait"),
                     osc::Value::string ("4") });

        const auto outer = reading (7, "GRPAXXX1");

        REQUIRE (outer.bars.size() == 1);
        CHECK (outer.bars[0].lengthKnown);
        CHECK (outer.bars[0].length == doctest::Approx (4.0));

        SUBCASE ("and the inner group's OWN pre-wait is not counted into its length")
        {
            /*  A bar's left edge is already its pre-wait; a length that
                contained it too would draw the group that much too long, and
                every bar after it in a sequence that much too late. */
            rig.apply (8, "window", "node.set",
                       { osc::Value::string ("/godot/cue/GRPBXXX1/preWait"),
                         osc::Value::string ("3") });

            const auto moved = reading (9, "GRPAXXX1");

            REQUIRE (moved.bars.size() == 1);
            CHECK (moved.bars[0].at == doctest::Approx (3.0));
            CHECK (moved.bars[0].length == doctest::Approx (4.0));
        }

        SUBCASE ("a manual sequence inside cannot be resolved, because a person is in it")
        {
            rig.apply (8, "window", "node.set",
                       { osc::Value::string ("/godot/cue/GRPBXXX1/mode"),
                         osc::Value::string ("sequence") });

            const auto manual = reading (9, "GRPAXXX1");

            REQUIRE (manual.bars.size() == 1);
            CHECK_FALSE (manual.bars[0].lengthKnown);
        }

        SUBCASE ("nor one that loops for ever, or plays some of its members")
        {
            rig.apply (8, "window", "node.set",
                       { osc::Value::string ("/godot/cue/GRPBXXX1/loops"),
                         osc::Value::string ("0") });

            CHECK_FALSE (reading (9, "GRPAXXX1").bars[0].lengthKnown);
        }

        SUBCASE ("and a round count multiplies it")
        {
            rig.apply (8, "window", "node.set",
                       { osc::Value::string ("/godot/cue/GRPBXXX1/loops"),
                         osc::Value::string ("3") });

            CHECK (reading (9, "GRPAXXX1").bars[0].length == doctest::Approx (12.0));
        }
    }

    //  And the reading says what it is inside of, so the panel can climb back up.
    CHECK (reading (5, "GRPBXXX1").parent == "GRPAXXX1");
}

TEST_CASE ("client: shift lines a bar up with another, by any of its edges")
{
    /*  The author, 2026-09-22: "you can snap starts together, start and end,
        end and end with drag+shift modifier". All three fall out of offering
        BOTH edges of the dragged bar to every target - which is why there is
        one comparison here and not three. */
    std::vector<model::Bar> bars;

    model::Bar bed;
    bed.id = "BED";
    bed.name = "The bed";
    bed.at = 4.0;
    bed.length = 6.0;
    bed.lengthKnown = true;
    bars.push_back (bed);

    model::Bar voice;
    voice.id = "VOICE";
    voice.name = "Voice";
    voice.at = 20.0;
    voice.length = 3.0;
    voice.lengthKnown = true;
    bars.push_back (voice);

    const auto targets = model::snapTargets (bars, "VOICE");

    /*  THE GROUP'S ENTRY AND BOTH EDGES OF THE OTHER BAR, and nothing of the
        dragged one: a bar cannot line up with itself. */
    REQUIRE (targets.size() == 3);
    CHECK (targets[0].seconds == doctest::Approx (0.0));
    CHECK (targets[1].seconds == doctest::Approx (4.0));
    CHECK (targets[2].seconds == doctest::Approx (10.0));

    SUBCASE ("start to start")
    {
        const auto found = model::snapTo (4.4, 3.0, true, targets, 1.0);

        REQUIRE (found.has_value());
        CHECK (found->at == doctest::Approx (4.0));
        CHECK (found->said.find ("start") != std::string::npos);
        CHECK (found->said.find ("The bed") != std::string::npos);
    }

    SUBCASE ("start to end")
    {
        const auto found = model::snapTo (9.7, 3.0, true, targets, 1.0);

        REQUIRE (found.has_value());
        CHECK (found->at == doctest::Approx (10.0));
    }

    SUBCASE ("end to end: the START is what gets written, however the match was made")
    {
        /*  The dragged bar is three seconds long, so lining its END up with
            the bed's end at ten puts its start at seven. */
        const auto found = model::snapTo (7.2, 3.0, true, targets, 1.0);

        REQUIRE (found.has_value());
        CHECK (found->at == doctest::Approx (7.0));
        CHECK (found->said.find ("its end") != std::string::npos);
    }

    SUBCASE ("and back to the group's entry, which is always there to line up with")
    {
        const auto found = model::snapTo (0.3, 3.0, true, targets, 1.0);

        REQUIRE (found.has_value());
        CHECK (found->at == doctest::Approx (0.0));
        CHECK (found->said.find ("group") != std::string::npos);
    }

    SUBCASE ("nothing near is no answer rather than a wrong one")
    {
        CHECK_FALSE (model::snapTo (14.0, 3.0, true, targets, 1.0).has_value());
    }

    SUBCASE ("a bar whose length nobody knows offers no end to line up with")
    {
        /*  A group's length never reaches a client at all, and a media file
            this build could not read has none either. Offering their "end"
            would be offering their start wearing a different word. */
        bars[0].lengthKnown = false;

        const auto fewer = model::snapTargets (bars, "VOICE");
        REQUIRE (fewer.size() == 2);

        //  And the dragged bar's own end is not offered either.
        CHECK_FALSE (model::snapTo (7.2, 3.0, false, fewer, 1.0).has_value());
    }

    //  A member cannot begin before the group it is inside of.
    CHECK (model::preWaitFor (-3.0) == doctest::Approx (0.0));
    CHECK (model::preWaitFor (2.5) == doctest::Approx (2.5));
}

TEST_CASE ("client: a fader's throw bends where a hand expects it to")
{
    /*  A straight -120..+12 scale puts unity nine tenths of the way up and
        spends its bottom half below audibility. The taper is four points and
        straight between them, and what has to be true of it is that it goes
        both ways exactly: a hand drags to a place, reads a number, types the
        number back, and lands on the same place. */
    CHECK (model::fractionForDb (model::silenceDb) == doctest::Approx (0.0));
    CHECK (model::fractionForDb (0.0) == doctest::Approx (0.85));
    CHECK (model::fractionForDb (model::loudestDb) == doctest::Approx (1.0));

    //  Unity sits high, which is the whole reason for the bend.
    CHECK (model::fractionForDb (0.0) > 0.8);

    for (const auto decibels : { -120.0, -90.0, -60.0, -24.0, -6.0, 0.0, 6.0, 12.0 })
    {
        INFO (decibels << " dB");
        CHECK (model::dbForFraction (model::fractionForDb (decibels))
                 == doctest::Approx (decibels));
    }

    //  And nothing outside the range can be asked for, from either end.
    CHECK (model::dbForFraction (-1.0) == doctest::Approx (model::silenceDb));
    CHECK (model::dbForFraction (2.0) == doctest::Approx (model::loudestDb));
    CHECK (model::stepDb (model::loudestDb, 5, false) == doctest::Approx (model::loudestDb));
    CHECK (model::stepDb (model::silenceDb, -5, false) == doctest::Approx (model::silenceDb));

    //  A click is a decibel, a shift-click a tenth.
    CHECK (model::stepDb (-6.0, 1, false) == doctest::Approx (-5.0));
    CHECK (model::stepDb (-6.0, 1, true) == doctest::Approx (-5.9));

    /*  THE WORD AND NOT THE NUMBER AT THE BOTTOM: "-120.0" reads as very quiet
        where what it means is nothing at all. */
    CHECK (model::faderText (model::silenceDb) == "-inf");
    CHECK (model::faderText (-125.0) == "-inf");
    CHECK (model::faderText (0.0) == "0");
    CHECK (model::faderText (-6.25) == "-6.3");
}

TEST_CASE ("client: the mixer draws a strip per mix channel, not per send")
{
    /*  The document holds a `Send` only where somebody set one, so a mixer
        built from the sends would be one you cannot raise a new send on. The
        strips come from the RIG; what is up is what somebody pushed. */
    Rig rig;

    //  `minimal` has two direct outs; make one of them a mix to send into.
    const auto rows = model::readOutputs (*rig.publish (0));
    REQUIRE (rows.size() == 2);

    rig.apply (1, "window", "node.set",
               { osc::Value::string ("/godot/bus/" + rows[1].id + "/kind"),
                 osc::Value::string ("mix") });

    /*  AND A CUE THAT PLAYS SOMETHING. `minimal`'s own cues are memos, which
        have no sends and no direct out - only a media cue has anywhere for a
        sound to go. */
    const std::string cue = "M3D7A5XZ";

    rig.apply (2, "window", "cue.create",
               { osc::Value::string ("7K2QM9X4"), osc::Value::int32 (0),
                 osc::Value::string ("media"), osc::Value::string ("Thunder"),
                 osc::Value::string (cue) });

    const auto snapshot = rig.publish (3);

    /*  Asserted rather than assumed: an identifier outside the alphabet, or a
        parent that is not a container, would leave every check below failing
        for a reason that has nothing to do with sends. */
    REQUIRE (model::text (*snapshot, "/godot/cue/" + cue + "/kind") == "media");

    auto strips = model::readSends (*snapshot, cue);
    REQUIRE (strips.size() == 1);
    CHECK (strips[0].busId == rows[1].id);
    CHECK (strips[0].name == "Foldback");

    /*  NO SEND IS DRAWN AT SILENCE, which is what it sounds like - and what
        marks it out is that there is no object behind it to delete. */
    CHECK (strips[0].present() == false);
    CHECK (strips[0].levelDb == doctest::Approx (model::silenceDb));

    SUBCASE ("and one the cue actually sends into carries its level")
    {
        rig.apply (4, "window", "send.create",
                   { osc::Value::string (cue), osc::Value::string (rows[1].id) });

        const auto after = rig.publish (5);
        auto raised = model::readSends (*after, cue);

        REQUIRE (raised.size() == 1);
        CHECK (raised[0].present());
        CHECK (raised[0].levelDb == doctest::Approx (0.0));

        //  And another cue's sends are not this one's.
        CHECK (model::readSends (*after, "F7HR8TVD").front().present() == false);
    }

    SUBCASE ("and one made at a level is born at it, in the same edit (2026-09-25)")
    {
        const auto made = gesture::createSend (cue, rows[1].id, -18.5);
        rig.apply (4, made.origin, made.command, made.args);

        const auto after = rig.publish (5);
        auto raised = model::readSends (*after, cue);

        REQUIRE (raised.size() == 1);
        CHECK (raised[0].present());
        CHECK (raised[0].levelDb == doctest::Approx (-18.5));
    }

    SUBCASE ("and a level the row would refuse makes nothing")
    {
        rig.apply (4, "window", "send.create",
                   { osc::Value::string (cue), osc::Value::string (rows[1].id),
                     osc::Value::string ({}), osc::Value::string ("loud") });

        CHECK (model::readSends (*rig.publish (5), cue).front().present() == false);
    }
}

//==============================================================================
/*  SEVERAL CUES AT ONCE (namespace draft §30.11): the author's report - "I
    managed to open the panel, select multiple media cues and see the send
    levels then, but it didn't spread to the complete selection" - and their
    rule for it, RA: a fader over several picked cues moves every one by the
    same number of decibels, a typed number sets them all, one gesture is one
    undo.
*/
namespace
{
    /*  `minimal` with both its outputs made mixes and three media cues: the
        first two send into the foldback at different levels, the third into
        nothing; the first two into the main at one level. */
    struct ManyRig : Rig
    {
        ManyRig()
        {
            const auto rows = model::readOutputs (*publish (0));
            REQUIRE (rows.size() == 2);
            mainBus = rows[0].id;
            foldback = rows[1].id;

            for (const auto& bus : { mainBus, foldback })
                REQUIRE (apply (1, "window", "node.set", { osc::Value::string ("/godot/bus/" + bus + "/kind"),
                                                           osc::Value::string ("mix") }).applied == 1);

            int index = 0;

            for (const auto& id : { first, second, third })
                REQUIRE (apply (2, "window", "cue.create",
                                { osc::Value::string ("7K2QM9X4"), osc::Value::int32 (index++),
                                  osc::Value::string ("media"), osc::Value::string ("Rain " + id),
                                  osc::Value::string (id) }).applied == 1);

            const auto send = [this] (const std::string& cue, const std::string& bus, const char* level, const char* id)
            {
                REQUIRE (apply (3, "window", "send.create", { osc::Value::string (cue), osc::Value::string (bus),
                                                              osc::Value::string (id), osc::Value::string (level) })
                             .applied == 1);
            };

            send (first, foldback, "-6", "SND0000A");
            send (second, foldback, "-12", "SND0000B");
            send (first, mainBus, "-3", "SND0000C");
            send (second, mainBus, "-3", "SND0000D");

            REQUIRE (apply (4, "window", "node.set", { osc::Value::string ("/godot/cue/" + first + "/level"),
                                                       osc::Value::string ("-4") }).applied == 1);
        }

        const std::string first = "M3D7A5XZ";
        const std::string second = "N4E8B6YZ";
        const std::string third = "P5F9C7ZZ";
        const std::string memo = "B3N8R5TW";
        std::string mainBus, foldback;
    };

    const model::SendStrip& stripInto (const std::vector<model::SendStrip>& strips, const std::string& bus)
    {
        const auto found = std::find_if (strips.begin(), strips.end(),
                                         [&bus] (const model::SendStrip& strip) { return strip.busId == bus; });
        REQUIRE (found != strips.end());
        return *found;
    }
}

TEST_CASE ("client: over several picked cues the send mixer reads every cue's send, draws the anchor's, and says how many")
{
    ManyRig rig;
    const auto snapshot = rig.publish (5);

    REQUIRE (model::text (*snapshot, "/godot/cue/" + rig.memo + "/kind") == "memo");

    /*  THE CUES A PANEL ACTS ON: the anchor first, then the rest in the order
        picked, the memo passed over - and nothing for a panel that is one
        cue's, or for one cue picked. */
    const std::vector<std::string> picked { rig.first, rig.memo, rig.second, rig.third };

    CHECK (model::footCues (*snapshot, model::Subject::Kind::sends, rig.second, picked)
             == std::vector<std::string> { rig.second, rig.first, rig.third });
    CHECK (model::footCues (*snapshot, model::Subject::Kind::eq, rig.second, picked).size() == 3u);
    CHECK (model::footCues (*snapshot, model::Subject::Kind::waveform, rig.second, picked).empty());
    CHECK (model::footCues (*snapshot, model::Subject::Kind::fx, rig.second, picked).empty());
    CHECK (model::footCues (*snapshot, model::Subject::Kind::sends, rig.second, { rig.second }).empty());

    //  An anchor the panel does not serve leads with the first that it does.
    CHECK (model::footCues (*snapshot, model::Subject::Kind::sends, rig.memo, picked).front() == rig.first);

    const auto reading = model::readFoot (*snapshot, { model::Subject::Kind::sends, rig.second }, picked);

    REQUIRE (reading.many());
    CHECK (reading.cues == std::vector<std::string> { rig.second, rig.first, rig.third });
    CHECK (reading.picked == 4u);
    CHECK (reading.subject.objectId == rig.second);
    CHECK (reading.cueName == "3 of 4 cues");
    CHECK (reading.notice.empty());

    //  Every cue's own level, in the reading's order: the master's band.
    REQUIRE (reading.cueLevels.size() == 3u);
    CHECK (reading.cueLevels[0] == doctest::Approx (0.0));
    CHECK (reading.cueLevels[1] == doctest::Approx (-4.0));

    /*  THE FOLDBACK: the anchor's send drawn, every cue's part of it, the
        spread, and one of three with none. */
    const auto& foldback = stripInto (reading.sends, rig.foldback);

    CHECK (foldback.sendId == "SND0000B");
    CHECK (foldback.levelDb == doctest::Approx (-12.0));
    REQUIRE (foldback.each.size() == 3u);
    CHECK (foldback.each[0].cueId == rig.second);
    CHECK (foldback.each[1].sendId == "SND0000A");
    CHECK_FALSE (foldback.each[2].present());
    CHECK (foldback.having() == 2u);
    CHECK (foldback.onCount() == 2u);
    CHECK (foldback.lowestDb == doctest::Approx (-12.0));
    CHECK (foldback.loudestDb == doctest::Approx (-6.0));
    CHECK (foldback.mixed());

    //  The main: two at one level is not mixed.
    const auto& toMain = stripInto (reading.sends, rig.mainBus);
    CHECK (toMain.having() == 2u);
    CHECK_FALSE (toMain.mixed());

    //  Picked whole, the head says how many and no more.
    CHECK (model::readFoot (*snapshot, { model::Subject::Kind::sends, rig.second },
                            { rig.first, rig.second, rig.third }).cueName == "3 cues");

    //  The EQ over the same: every cue's, the lead's drawn.
    const auto eq = model::readFoot (*snapshot, { model::Subject::Kind::eq, rig.second }, picked);
    CHECK (eq.eqs.size() == 3u);
    CHECK (eq.eq.present);

    //  A waveform stays one cue's, the selection notwithstanding.
    const auto wave = model::readFoot (*snapshot, { model::Subject::Kind::waveform, rig.second }, picked);
    CHECK_FALSE (wave.many());
    CHECK (wave.cueName == "Rain " + rig.second);

    //  And over one cue nothing about several is filled.
    const auto one = model::readFoot (*snapshot, { model::Subject::Kind::sends, rig.second });
    CHECK_FALSE (one.many());
    CHECK (stripInto (one.sends, rig.foldback).each.empty());

    SUBCASE ("a drag over the three is one write that the engine takes whole, each send moved by the same")
    {
        //  The foldback's two sends, held where they stood, moved up three decibels.
        std::vector<std::pair<std::string, double>> held;

        for (const auto& share : foldback.each)
            if (share.present())
                held.emplace_back ("/godot/send/" + share.sendId + "/level", share.levelDb);

        const auto event = gesture::setNodes (model::levelsMovedBy (held, 3.0));
        CHECK (event.command == "node.setMany");
        REQUIRE (rig.apply (6, event.origin, event.command, event.args).applied == 1);

        const auto after = model::readFoot (*rig.publish (7), { model::Subject::Kind::sends, rig.second }, picked);
        const auto& moved = stripInto (after.sends, rig.foldback);

        CHECK (moved.levelDb == doctest::Approx (-9.0));
        CHECK (moved.each[1].levelDb == doctest::Approx (-3.0));
        CHECK (moved.loudestDb - moved.lowestDb == doctest::Approx (6.0));
    }

    SUBCASE ("a field typed over the three is one write too")
    {
        const auto event = gesture::setAll ({ "/godot/cue/" + rig.first + "/level", "/godot/cue/" + rig.second + "/level",
                                              "/godot/cue/" + rig.third + "/level" }, "-6");
        CHECK (event.command == "node.setMany");
        REQUIRE (rig.apply (6, event.origin, event.command, event.args).applied == 1);

        const auto after = model::readFoot (*rig.publish (7), { model::Subject::Kind::sends, rig.second }, picked);

        for (const auto level : after.cueLevels)
            CHECK (level == doctest::Approx (-6.0));
    }
}

TEST_CASE ("client: a fader over several cues moves each by the same decibels, held in its own range, and a typed number sets them all")
{
    /*  RA's arithmetic, apart from any window: measured from the grab, so a
        level the bottom held comes back to its place when the hand does. */
    const std::vector<std::pair<std::string, double>> grabbed { { "a", -6.0 }, { "b", -12.0 }, { "c", -118.0 } };

    const auto texts = [] (const std::vector<std::pair<std::string, std::string>>& writes)
    {
        std::vector<std::string> out;

        for (const auto& write : writes)
            out.push_back (write.second);

        return out;
    };

    CHECK (texts (model::levelsMovedBy (grabbed, -5.0)) == std::vector<std::string> { "-11", "-17", "-120" });
    CHECK (texts (model::levelsMovedBy (grabbed, 20.0)) == std::vector<std::string> { "12", "8", "-98" });
    CHECK (texts (model::levelsMovedBy (grabbed, 0.0)) == std::vector<std::string> { "-6", "-12", "-118" });
    CHECK (texts (model::levelsMovedBy (grabbed, 1.5)) == std::vector<std::string> { "-4.5", "-10.5", "-116.5" });
    CHECK (model::levelsMovedBy (grabbed, 0.0).front().first == "a");

    //  Typed, or reset: every one to the one number, clamped and rounded alike.
    CHECK (texts (model::levelsSetTo ({ "a", "b" }, -3.04)) == std::vector<std::string> { "-3", "-3" });
    CHECK (texts (model::levelsSetTo ({ "a" }, 40.0)) == std::vector<std::string> { "12" });
    CHECK (texts (model::levelsSetTo ({ "a" }, -400.0)) == std::vector<std::string> { "-120" });

    //  In the locale-free spelling, French included.
    CHECK (model::levelText (-6.25) == "-6.3");

    /*  AN EQ BAND'S GAIN DRAGGED OVER SEVERAL (TL): the lead's move added to
        each one's own, within the field's range; its frequency and width the
        same for all, as any row typed. */
    CHECK (model::eqGainMoved (3.0, 0.0, 6.0, 24.0) == doctest::Approx (9.0));
    CHECK (model::eqGainMoved (20.0, 0.0, 10.0, 24.0) == doctest::Approx (24.0));
    CHECK (model::eqGainMoved (-20.0, 2.0, -8.0, 24.0) == doctest::Approx (-24.0));

    const auto rows = model::eqRowForAll ({ "M3D7A5XZ", "N4E8B6YZ" }, "eqB2Freq", "800");
    REQUIRE (rows.size() == 2u);
    CHECK (rows[1].first == "/godot/cue/N4E8B6YZ/eqB2Freq");
    CHECK (rows[1].second == "800");
}

TEST_CASE ("client: the panel bar over several cues is the EQ and the sends, opening on the anchor, and says how many")
{
    const auto all = model::openersForMany ({ "media", "mic", "media" }, "N4E8B6YZ");

    REQUIRE (all.size() == 2u);
    CHECK (all[0].value == "eq");
    CHECK (all[1].value == "sends");
    CHECK (all[0].control == model::Control::opener);
    CHECK (all[0].address == "N4E8B6YZ");
    CHECK (all[0].label == "EQ, four bands and two filters, on all 3 cues at once");
    CHECK (all[1].label == "Sends, levels into the show's mix channels, on all 3 cues at once");

    const auto some = model::openersForMany ({ "media", "memo", "fade", "media" }, "N4E8B6YZ");
    REQUIRE (some.size() == 2u);
    CHECK (some[1].label == "Sends, levels into the show's mix channels, on 2 of the 4 cues picked - the media and mic cues");

    const auto alone = model::openersForMany ({ "memo", "media" }, "N4E8B6YZ");
    REQUIRE (alone.size() == 2u);
    CHECK (alone[0].label == "EQ, four bands and two filters, on 1 of the 2 cues picked - the media or mic cue");

    //  Nothing to act on, nothing offered: no waveform, chain or take over several.
    CHECK (model::openersForMany ({ "memo", "fade", "group" }, "N4E8B6YZ").empty());

    //  On a real selection the bar opens on the anchor, or the first cue when the anchor is not picked.
    ManyRig rig;
    const auto snapshot = rig.publish (5);

    const auto anchored = model::inspectMany (*snapshot, { rig.first, rig.second, rig.memo }, rig.second);
    CHECK (anchored.panelCue == rig.second);
    REQUIRE (anchored.panels.size() == 2u);
    CHECK (anchored.panels[0].address == rig.second);
    CHECK (anchored.panels[1].label.find ("2 of the 3 cues picked") != std::string::npos);

    CHECK (model::inspectMany (*snapshot, { rig.first, rig.second }, "ZZZZZZZZ").panelCue == rig.first);
    CHECK (model::inspect (*snapshot, rig.third).panelCue == rig.third);

    //  And the words the head says in place of a name.
    CHECK (model::manyCuesWords (6, 6) == "6 cues");
    CHECK (model::manyCuesWords (6, 8) == "6 of 8 cues");
}

//==============================================================================
/*  THE SHOW'S DEVICES, AND AIMING A CUE AT ONE (2026-09-22).

    A network cue carries the whole address it writes, so which box it is aimed
    at is the front of that address rather than a field. These cases pin the
    two halves of that: reading the devices out of the tree, and the rewrite the
    inspector's target menu commits. The rewrite is pure and is tested as such -
    the menu is the only place in the window where picking an item writes a
    value the window computed rather than one the engine offered, and the thing
    that would make it dangerous is getting the arithmetic wrong.
*/
TEST_CASE ("client: the show's devices are read out of the tree")
{
    Rig rig;

    const auto devices = model::readDevices (*rig.publish (0));

    //  The fixture declares two, both described.
    REQUIRE (devices.size() == 2);

    const auto* wfs = &devices[0];

    for (const auto& row : devices)
        if (row.prefix == "/wfs")
            wfs = &row;

    CHECK (wfs->prefix == "/wfs");
    CHECK (wfs->port == 8000);
    CHECK_FALSE (wfs->opaque());

    /*  A device with no name reads as its prefix, so a list never has a blank
        row in it and a menu always has something to point at. */
    CHECK (wfs->name.empty());
    CHECK (wfs->label() == "/wfs");

    rig.apply (1, "window", "node.set",
               { osc::Value::string ("/godot/mount/" + wfs->id + "/name"),
                 osc::Value::string ("The WFS") });

    CHECK (model::readDevices (*rig.publish (2)).front().label() != "/wfs");
}

TEST_CASE ("client: which device an address is aimed at, and the rewrite that moves it")
{
    std::vector<model::DeviceRow> devices;

    model::DeviceRow desk;
    desk.id = "DESK0001";
    desk.prefix = "/desk";
    desk.name = "Lighting desk";
    devices.push_back (desk);

    model::DeviceRow wfs;
    wfs.id = "WFS00001";
    wfs.prefix = "/wfs";
    devices.push_back (wfs);

    /*  A NESTED PREFIX, because this is what makes "the longest wins" a rule
        rather than a detail: both cover the address and the cue belongs to the
        more specific one. */
    model::DeviceRow aux;
    aux.id = "AUX00001";
    aux.prefix = "/desk/aux";
    devices.push_back (aux);

    SUBCASE ("the longest prefix wins, and the boundary is a separator")
    {
        CHECK (model::deviceOf ("/desk/fader", devices) == "DESK0001");
        CHECK (model::deviceOf ("/desk/aux/1/level", devices) == "AUX00001");

        /*  "/desktop" is not under "/desk". The whole reason this is written
            out rather than a string-start test. */
        CHECK (model::deviceOf ("/desktop/fader", devices).empty());

        //  The prefix itself is not under itself: there is no node there.
        CHECK (model::deviceOf ("/desk", devices).empty());
        CHECK (model::deviceOf ("/nowhere", devices).empty());
    }

    SUBCASE ("aiming a cue at another device swaps the front of its address")
    {
        CHECK (model::retarget ("/desk/fader", devices, "WFS00001") == "/wfs/fader");
        CHECK (model::retarget ("/desk/aux/1/level", devices, "WFS00001") == "/wfs/1/level");

        //  Aiming it where it already points changes nothing, which is what
        //  lets the menu find the current device by matching on the address.
        CHECK (model::retarget ("/desk/fader", devices, "DESK0001") == "/desk/fader");
    }

    SUBCASE ("an address under no device gets the prefix put in front of it")
    {
        /*  So a cue somebody typed by hand is aimable without retyping, which
            is the case that makes the menu worth having at all. */
        CHECK (model::retarget ("/go", devices, "DESK0001") == "/desk/go");
    }

    SUBCASE ("and aiming it at nothing strips the prefix rather than keeping it")
    {
        /*  HONEST RATHER THAN HELPFUL. It leaves an address the engine refuses
            when the cue fires - which is exactly what "this cue is aimed at no
            device" means, and what validate says out loud afterwards. Silently
            keeping the old prefix would be a menu that lies about what it did. */
        CHECK (model::retarget ("/desk/fader", devices, {}) == "/fader");
        CHECK (model::retarget ("/fader", devices, {}) == "/fader");
    }

    SUBCASE ("a device that is not there changes nothing")
    {
        CHECK (model::retarget ("/desk/fader", devices, "GONE0001") == "/desk/fader");
    }

    SUBCASE ("the menu offers whole addresses, with none first")
    {
        const auto choices = model::targetChoices ("/desk/fader", devices);

        REQUIRE (choices.size() == 4);
        CHECK (choices[0].second == "(none)");
        CHECK (choices[0].first == "/fader");

        /*  THE KEY IS AN ADDRESS. That is what lets the inspector commit a
            choice with the `node.set` it already has, and what lets the menu
            find its own current value without a second field to compare. */
        CHECK (choices[1].first == "/desk/fader");
        CHECK (choices[1].second == "Lighting desk");

        auto found = false;

        for (const auto& choice : choices)
            if (choice.first == "/desk/fader" && choice.second == "Lighting desk")
                found = true;

        CHECK (found);
    }
}

TEST_CASE ("client: a MIDI cue's two numbers are named for the message they carry")
{
    /*  The author, looking at the panel: "There are two number fields in the
        MIDI cue." They are `number` and `data`, and what each one means
        depends entirely on the type above them. The row names stay - one row
        carries the payload whatever it is - and the PANEL says which is which.

        The table is `midi::build`'s own, which is what makes this test worth
        having: if the builder learns a type, this goes stale loudly. */
    Rig rig;

    const std::string cue = "M1D2N3P4";

    rig.apply (1, "window", "cue.create",
               { osc::Value::string ("7K2QM9X4"), osc::Value::int32 (0),
                 osc::Value::string ("midi"), osc::Value::string ("Stinger"),
                 osc::Value::string (cue) });

    struct Wanted
    {
        const char* type;
        const char* number;     ///< what the panel calls it
        bool numberApplies;
        const char* data;
        bool dataApplies;
    };

    const Wanted table[] {
        { "noteOn",          "note",       true,  "velocity", true  },
        { "noteOff",         "note",       true,  "velocity", true  },
        { "aftertouch",      "note",       true,  "pressure", true  },
        { "controlChange",   "controller", true,  "value",    true  },
        { "programChange",   "program",    true,  "data2",    false },
        { "channelPressure", "data1",      false, "pressure", true  },
        { "pitchBend",       "data1",      false, "bend",     true  },
        { "sysex",           "data1",      false, "data2",    false },
    };

    auto tick = 2;

    for (const auto& wanted : table)
    {
        INFO ("type: " << wanted.type);

        rig.apply (tick++, "window", "node.set",
                   { osc::Value::string ("/godot/cue/" + cue + "/type"),
                     osc::Value::string (wanted.type) });

        const auto inspection = model::inspect (*rig.publish (tick++), cue);

        const model::Field* number = nullptr;
        const model::Field* data = nullptr;
        const model::Field* sysex = nullptr;
        const model::Field* channel = nullptr;

        for (const auto& block : inspection.blocks)
            for (const auto& field : block.fields)
            {
                if (field.name == "data1")   number = &field;
                if (field.name == "data2")   data = &field;
                if (field.name == "sysex")   sysex = &field;
                if (field.name == "channel") channel = &field;
            }

        REQUIRE (number != nullptr);
        REQUIRE (data != nullptr);
        REQUIRE (sysex != nullptr);
        REQUIRE (channel != nullptr);

        CHECK (number->label == std::string (wanted.number));
        CHECK (data->label == std::string (wanted.data));

        /*  GREYED AND NOT HIDDEN: an absence reads as "this program cannot do
            that", which is the wrong thing to say about a field that would
            work if the type above it were different. */
        CHECK (number->applies == wanted.numberApplies);
        CHECK (data->applies == wanted.dataApplies);

        //  And the two that only one type uses at all.
        CHECK (sysex->applies == (std::string (wanted.type) == "sysex"));
        CHECK (channel->applies == (std::string (wanted.type) != "sysex"));
    }
}

TEST_CASE ("client: a device with several roots is one entry in the target menu")
{
    /*  The author's own desk. A DiGiCo S21 reached directly speaks three
        vocabularies with nothing above them, so the panel has to treat all
        three as one box - one menu entry, one name - and has to agree with the
        engine about which box an address belongs to. It agrees by calling the
        engine's own function rather than by restating it. */
    std::vector<model::DeviceRow> devices;

    model::DeviceRow s21;
    s21.id = "S2100001";
    s21.name = "S21";
    s21.prefix = "/channel /console /digico";
    devices.push_back (s21);

    model::DeviceRow wfs;
    wfs.id = "WFS00001";
    wfs.name = "The WFS";
    wfs.prefix = "/wfs";
    devices.push_back (wfs);

    CHECK (devices.front().prefixes().size() == 3u);
    CHECK (devices.front().firstPrefix() == "/channel");

    SUBCASE ("every one of its vocabularies names the same device")
    {
        for (const auto* address : { "/channel/1/fader", "/console/ping",
                                     "/digico/snapshots/fire" })
        {
            INFO ("address: " << address);
            CHECK (model::deviceOf (address, devices) == "S2100001");
        }

        CHECK (model::deviceOf ("/wfs/source/1/gain", devices) == "WFS00001");
        CHECK (model::deviceOf ("/channels/1/fader", devices).empty());
    }

    SUBCASE ("and the menu offers it once, whichever root the cue is on")
    {
        for (const auto* address : { "/channel/1/fader", "/digico/snapshots/fire" })
        {
            INFO ("address: " << address);

            const auto choices = model::targetChoices (address, devices);

            //  "(none)" plus one per device, not one per root.
            REQUIRE (choices.size() == 3u);

            auto named = 0;

            for (const auto& choice : choices)
                if (choice.second == "S21")
                    ++named;

            CHECK (named == 1);

            /*  And the entry for the device the cue is already on carries the
                address unchanged, so the menu selects it. */
            const auto here = model::retarget (address, devices, "S2100001");
            CHECK (here == std::string (address));
        }
    }

    SUBCASE ("aiming it elsewhere strips whichever root it matched")
    {
        CHECK (model::retarget ("/digico/snapshots/fire", devices, "WFS00001")
                 == "/wfs/snapshots/fire");
        CHECK (model::retarget ("/console/ping", devices, "WFS00001") == "/wfs/ping");

        /*  AND BACK THE OTHER WAY IT LANDS ON THE FIRST ROOT, which is a
            starting point rather than a translation: a fader on a WFS and a
            fader on a DiGiCo are not one address with a different beginning,
            and the panel does not pretend they are. */
        CHECK (model::retarget ("/wfs/source/1/gain", devices, "S2100001")
                 == "/channel/source/1/gain");
    }

    SUBCASE ("and taking it off the device strips the root it was on")
    {
        CHECK (model::retarget ("/console/ping", devices, {}) == "/ping");
    }
}

TEST_CASE ("client: a network cue's target is a menu, and picking one rewrites its address")
{
    Rig rig;

    const auto devices = model::readDevices (*rig.publish (0));
    REQUIRE (devices.size() == 2);

    const std::string cue = "N4T9B2QE";

    rig.apply (1, "window", "cue.create",
               { osc::Value::string ("7K2QM9X4"), osc::Value::int32 (0),
                 osc::Value::string ("osc"), osc::Value::string ("Desk go"),
                 osc::Value::string (cue) });

    rig.apply (2, "window", "node.set",
               { osc::Value::string ("/godot/cue/" + cue + "/address"),
                 osc::Value::string ("/wfs/source/1/gain") });

    const auto inspection = model::inspect (*rig.publish (3), cue);

    const model::Field* target = nullptr;
    const model::Field* address = nullptr;

    for (const auto& block : inspection.blocks)
        for (const auto& field : block.fields)
        {
            if (field.name == "device")  target = &field;
            if (field.name == "address") address = &field;
        }

    REQUIRE (target != nullptr);
    REQUIRE (address != nullptr);

    CHECK (target->control == model::Control::deviceRef);
    CHECK (target->label == "target");
    CHECK (target->writable);

    /*  IT WRITES THE ADDRESS ROW. There is no target attribute in the document
        and there must not be one: the address already says where the cue is
        going, and a second field naming the device would be a second truth to
        keep in step with the first. */
    CHECK (target->address == address->address);
    CHECK (target->address == "/godot/cue/" + cue + "/address");

    /*  And its current value is the address, so the menu selects the device
        the cue is already aimed at without anything else being consulted. */
    CHECK (target->value == "/wfs/source/1/gain");

    auto aimedHere = false;

    for (const auto& choice : target->choices)
        if (choice.first == target->value)
            aimedHere = true;

    CHECK (aimedHere);

    //  It is read before the address it is derived from, which is the order
    //  somebody fills a network cue in.
    for (const auto& block : inspection.blocks)
    {
        auto seenTarget = false;

        for (const auto& field : block.fields)
        {
            if (field.name == "device")  seenTarget = true;
            if (field.name == "address") CHECK (seenTarget);
        }
    }
}

TEST_CASE ("client: the target menu is not offered over a selection of cues")
{
    Rig rig;

    const std::string first = "N4T9B2QE", second = "N5T8B3QF";

    for (const auto& id : { first, second })
        rig.apply (1, "window", "cue.create",
                   { osc::Value::string ("7K2QM9X4"), osc::Value::int32 (0),
                     osc::Value::string ("osc"), osc::Value::string ("Desk go"),
                     osc::Value::string (id) });

    const auto inspection = model::inspectMany (*rig.publish (2), { first, second });

    /*  EVERY OTHER ROW WRITES ONE VALUE TO N ADDRESSES. This one would write a
        rewrite of each cue's own address, and every cue has a different one -
        so a menu that quietly aimed six cues at one address would be the worst
        kind of helpful. Aiming several cues at a device is worth having and is
        a command that does not exist yet. */
    for (const auto& block : inspection.blocks)
        for (const auto& field : block.fields)
            CHECK (field.name != "device");

    //  The ordinary rows still are, so this is a rule about one line.
    auto sawAddress = false;

    for (const auto& block : inspection.blocks)
        for (const auto& field : block.fields)
            if (field.name == "address")
                sawAddress = true;

    CHECK (sawAddress);
}

TEST_CASE ("client: the direct-out row is a menu of the show's own outputs")
{
    Rig rig;

    const auto rows = model::readOutputs (*rig.publish (0));
    REQUIRE (rows.size() == 2);

    //  One direct out and one mix, so the menu has something to leave out.
    rig.apply (1, "window", "node.set",
               { osc::Value::string ("/godot/bus/" + rows[1].id + "/kind"),
                 osc::Value::string ("mix") });

    const std::string cue = "M3D7A5XZ";

    rig.apply (2, "window", "cue.create",
               { osc::Value::string ("7K2QM9X4"), osc::Value::int32 (0),
                 osc::Value::string ("media"), osc::Value::string ("Thunder"),
                 osc::Value::string (cue) });

    const auto snapshot = rig.publish (3);
    const auto inspection = model::inspect (*snapshot, cue);

    const model::Field* directOut = nullptr;
    const model::Field* fold = nullptr;

    for (const auto& block : inspection.blocks)
        for (const auto& field : block.fields)
        {
            if (field.name == "directOut")    directOut = &field;
            if (field.name == "stereoToMono") fold = &field;
        }

    REQUIRE (directOut != nullptr);
    CHECK (directOut->control == model::Control::busRef);

    /*  "(none)" AND THE DIRECT OUTS, and the mix channel left out: a mix is
        reached through a send, at a level, which is the whole difference. */
    REQUIRE (directOut->choices.size() == 2);
    CHECK (directOut->choices[0].first.empty());
    CHECK (directOut->choices[0].second == "(none)");
    CHECK (directOut->choices[1].first == rows[0].id);

    /*  AND IT CARRIES THE MARK, in words rather than by colour (4.8). Nothing
        else in this show lands anywhere, so the one direct out is free. */
    CHECK (directOut->choices[1].second == "Main L/R · Stereo — free");

    /*  AND THE FOLD IS GREYED ON A CUE WHOSE FILE IS NOT STEREO. Drawn rather
        than hidden: an absence reads as "this program cannot do that". */
    REQUIRE (fold != nullptr);
    CHECK (fold->applies == false);

    SUBCASE ("and it applies once the cue says it has two channels")
    {
        rig.apply (4, "window", "node.set",
                   { osc::Value::string ("/godot/cue/" + cue + "/channels"),
                     osc::Value::string ("2") });

        const auto after = model::inspect (*rig.publish (5), cue);

        for (const auto& block : after.blocks)
            for (const auto& field : block.fields)
                if (field.name == "stereoToMono")
                    CHECK (field.applies);
    }
}

TEST_CASE ("client: the menu marks an output taken, undecided, or free - and never hides one")
{
    /*  The author, 2026-09-22: *"mark clearly the available direct outs and
        the ones taken... when uncertain, cue playing out until its end, then
        just don't mark it, the user will decide"* - and *"don't block
        assignation. In any case sum if there is an overlap."* So three states,
        and every output offered whatever its state. */
    Rig rig;

    const auto rows = model::readOutputs (*rig.publish (0));
    REQUIRE (rows.size() == 2);

    const auto out = rows[0].id;

    const auto make = [&rig, &out] (std::int64_t tick, int at, const char* id, const char* name)
    {
        rig.apply (tick, "window", "cue.create",
                   { osc::Value::string ("7K2QM9X4"), osc::Value::int32 (at),
                     osc::Value::string ("media"), osc::Value::string (name),
                     osc::Value::string (id) });

        rig.apply (tick + 1, "window", "node.set",
                   { osc::Value::string (std::string ("/godot/cue/") + id + "/directOut"),
                     osc::Value::string (out) });
    };

    /*  THE BED FIRST AND THE VOICE AFTER IT, which is the order the question
        is about: a cue that has not been fired yet is on no output, however
        long it would go on for once it had. */
    make (1, 0, "BEDXXXX1", "The bed");
    make (3, 1, "VCEXXXX1", "Voice");

    const auto marked = [] (const tree::TreeSnapshot& snapshot, const std::string& cue,
                            const std::string& bus)
    {
        for (const auto& mark : model::readOutMarks (snapshot, cue))
            if (mark.busId == bus)
                return mark;

        return model::OutMark {};
    };

    SUBCASE ("a finite cue in a manual list decides nothing, because a person is in between")
    {
        /*  Both cues sit at the top of a manual list with nothing saying when
            either ends. The first may well have finished long before the
            second's GO, and it may equally still be sounding - so the honest
            answer is neither taken nor free. */
        const auto snapshot = rig.publish (5);
        const auto mark = marked (*snapshot, "VCEXXXX1", out);

        CHECK (mark.state == model::OutMark::State::undecided);
        CHECK (mark.byCue == "The bed");

        //  And the menu shows the output all the same, with no word after it.
        CHECK (model::markedLabel ("Main L/R", "Stereo", mark) == "Main L/R · Stereo");
    }

    SUBCASE ("a bed that loops for ever is provably still on it")
    {
        /*  `Walk::unbounded`: a cue that ends on its own is over by the time a
            later manual step is reached, and one that does not is still going.
            A range looping for ever is the second. */
        rig.apply (5, "window", "range.create",
                   { osc::Value::string ("BEDXXXX1"), osc::Value::float64 (0.0),
                     osc::Value::float64 (4.0), osc::Value::string ("RNGXXXX1") });

        rig.apply (6, "window", "node.set",
                   { osc::Value::string ("/godot/range/RNGXXXX1/loops"),
                     osc::Value::string ("0") });

        const auto snapshot = rig.publish (7);
        const auto mark = marked (*snapshot, "VCEXXXX1", out);

        INFO ("busy: " << model::text (*snapshot, "/godot/cue/VCEXXXX1/outsBusy")
                << " / maybe: " << model::text (*snapshot, "/godot/cue/VCEXXXX1/outsMaybe"));

        CHECK (mark.state == model::OutMark::State::taken);
        CHECK (mark.byCue == "The bed");

        CHECK (model::markedLabel ("Main L/R", "Stereo", mark)
                 == "Main L/R · Stereo — taken by \"The bed\"");

        SUBCASE ("and a cue landing nowhere is in nobody's way")
        {
            rig.apply (8, "window", "node.set",
                       { osc::Value::string ("/godot/cue/BEDXXXX1/directOut"),
                         osc::Value::string ("") });

            const auto after = rig.publish (9);

            CHECK (marked (*after, "VCEXXXX1", out).state == model::OutMark::State::free);
        }
    }

    SUBCASE ("a move that creates the overlap says so, and one that changes nothing stays quiet")
    {
        rig.apply (5, "window", "range.create",
                   { osc::Value::string ("BEDXXXX1"), osc::Value::float64 (0.0),
                     osc::Value::float64 (4.0), osc::Value::string ("RNGXXXX1") });

        rig.apply (6, "window", "node.set",
                   { osc::Value::string ("/godot/range/RNGXXXX1/loops"),
                     osc::Value::string ("0") });

        const auto snapshot = rig.publish (7);

        const auto free = std::vector<model::OutMark> { { out, {}, model::OutMark::State::free } };
        const auto now = model::readOutMarks (*snapshot, "VCEXXXX1");

        const auto said = model::newClash (free, now, out, "Main L/R", "Voice");

        REQUIRE (said.has_value());
        CHECK (said->find ("Voice") != std::string::npos);
        CHECK (said->find ("The bed") != std::string::npos);
        CHECK (said->find ("Main L/R") != std::string::npos);

        /*  AND IT NEVER SAYS THE SAME THING TWICE. An overlap that was already
            there before the move is not news, and a notice that fires for a
            state somebody has already looked at is one they learn to ignore. */
        CHECK_FALSE (model::newClash (now, now, out, "Main L/R", "Voice").has_value());
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

TEST_CASE ("client: a waveform's height is the finer level's, where the analysis has one")
{
    /*  The author, 2026-09-25: "Can the waveform be more precise in level,
        not colour when zooming in." The colour stays the frame's; the height
        is the peak track's - 64 samples a pair and sixteen bits. */
    audio::TimbrePyramid pyramid;
    pyramid.sampleRate = 48000;
    pyramid.samples = 1024 * 4;

    //  Four frames, each loud by its byte: 0.5 all the way.
    std::vector<audio::timbre::Frame> frames (4);

    for (auto& frame : frames)
    {
        frame.peak = 128;
        frame.hue = 40;
        frame.saturation = 200;
        frame.lightness = 128;
    }

    pyramid.levels.push_back (frames);

    //  And the level under them: sixty-four pairs, each its own height, never 0.5.
    std::vector<audio::PeakPair> pairs (64);

    for (std::size_t at = 0; at < pairs.size(); ++at)
    {
        const auto height = static_cast<std::int16_t> (1000 + 100 * static_cast<int> (at));
        pairs[at] = { static_cast<std::int16_t> (-height / 2), height };
    }

    const auto track = audio::peaks::trackOf (pairs, 48000u, 1024u * 4u);

    //  ZOOMED INTO ONE FRAME: sixteen columns over its sixteen pairs, sixteen heights.
    const auto seconds = 1024.0 / 48000.0;
    const auto close = model::waveform (pyramid, &track, 16, seconds, 2.0 * seconds);
    REQUIRE (close.size() == 16u);

    for (std::size_t at = 0; at < close.size(); ++at)
    {
        const auto& pair = pairs[16 + at];
        CHECK (close[at].high == doctest::Approx (audio::peaks::toUnit (pair.high)));
        CHECK (close[at].low == doctest::Approx (audio::peaks::toUnit (pair.low)));
        CHECK (close[at].peak == doctest::Approx (audio::peaks::toUnit (pair.high)));
        CHECK (close[at].hue == doctest::Approx (audio::timbre::hueOf (frames[1])));   // the frame's colour
    }

    CHECK (close[0].high < close[15].high);

    //  Without the level, the frame's byte, mirrored: the old picture, unchanged.
    const auto coarse = model::waveform (pyramid, nullptr, 16, seconds, 2.0 * seconds);
    REQUIRE (coarse.size() == 16u);
    CHECK (coarse[0].peak == doctest::Approx (audio::timbre::peakOf (frames[1])));
    CHECK (coarse[0].low == doctest::Approx (-coarse[0].peak));

    //  The whole file with its level: one column per pixel, each the loudest pair of its span.
    const auto whole = model::waveform (pyramid, &track, 4);
    REQUIRE (whole.size() == 4u);
    CHECK (whole[3].high == doctest::Approx (audio::peaks::toUnit (pairs[63].high)));
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


TEST_CASE ("client: a running strip shows the stretch the cue plays, and a loop sends the head back")
{
    /*  The author, 2026-09-21: *"in the running cues, the part of the waveform
        between the first in point and last out point should be displayed. And
        for slices looping have cursor go back to the beginning of each slice
        as they play loops."*

        THE SECOND HALF IS THE ENGINE'S ALREADY. `run/@position` is a FILE
        position with the range wrap in it (`Runner::updatePositions`: the
        elapsed count is taken modulo the pass and added to the range's
        in-point), so a looping slice IS back at its in-point on every pass.
        What was missing was the picture: measured against the whole file the
        jump is a twitch, and measured against the stretch that plays it is the
        head returning to the start of the slice. */
    CHECK (model::playhead (0.0, 4.0, 12.0) == doctest::Approx (0.0));
    CHECK (model::playhead (8.0, 4.0, 12.0) == doctest::Approx (0.5));
    CHECK (model::playhead (12.0, 4.0, 12.0) == doctest::Approx (1.0));

    //  Outside the window is the nearest end, never a bar off the edge of the world.
    CHECK (model::playhead (1.0, 4.0, 12.0) == doctest::Approx (0.0));
    CHECK (model::playhead (99.0, 4.0, 12.0) == doctest::Approx (1.0));

    //  An empty or backwards window is nought, as an unknown length is.
    CHECK (model::playhead (5.0, 4.0, 4.0) == doctest::Approx (0.0));
    CHECK (model::playhead (5.0, 12.0, 4.0) == doctest::Approx (0.0));

    /*  AND WHAT A LOOP LOOKS LIKE ON IT. Three slices over a thirty-second
        file, the second of them repeating: the head walks 12 to 18 and is back
        at 12 on the next pass, which over the played stretch is a jump from
        three quarters of the way along back to half way - a movement somebody
        can see, and the reason the slice boundaries are drawn. */
    const auto from = 0.0, to = 24.0;

    CHECK (model::playhead (17.9, from, to) > model::playhead (12.0, from, to));
    CHECK (model::playhead (12.0, from, to) == doctest::Approx (0.5));
}

TEST_CASE ("client: every range in the show is gathered in one pass, owned by its cue")
{
    /*  ONE WALK FOR ALL OF THEM. The running pane wants the ranges of every
        row it is drawing, twenty-five times a second; asking per row would be
        the per-row habit the boundary check exists to stop. */
    Rig rig ("ambience");
    const auto snapshot = rig.publish (0);

    const auto all = model::rangesByCue (*snapshot);

    REQUIRE_FALSE (all.empty());

    //  The fixture's bed: three regions, the first of them looping for ever.
    const auto found = std::find_if (all.begin(), all.end(),
                                     [] (const auto& pair) { return pair.second.size() == 3; });

    REQUIRE (found != all.end());

    const auto& rows = found->second;

    CHECK (rows[0].name == "The bed");
    CHECK (rows[0].loops == 0);                       // nought is for ever
    CHECK (rows[0].in == doctest::Approx (0.0));
    CHECK (rows[1].in == doctest::Approx (12.0));
    CHECK (rows[2].out == doctest::Approx (24.0));

    //  And it is in playlist order, which `index` is what says.
    CHECK (rows[0].index <= rows[1].index);
    CHECK (rows[1].index <= rows[2].index);

    //  One cue picked out of it is what `readRanges` answers.
    const auto one = model::readRanges (*snapshot, found->first);
    REQUIRE (one.size() == 3);
    CHECK (one[0].id == rows[0].id);

    //  A cue with no ranges has none, rather than somebody else's.
    CHECK (model::readRanges (*snapshot, "NOSUCHID").empty());
}

TEST_CASE ("client: a media cue's time is its file's at its own speed - in the list's column and on a timeline")
{
    /*  Namespace draft §22.5, decision EE: `duration` stays the file's own
        length, and what counts time as heard divides it by the cue's speed -
        for ever at nought, which no bar can end. */
    Rig rig ("phase4");
    const std::map<std::string, double> lengths { { "segments.wav", 30.0 }, { "ramp.wav", 4.0 } };
    rig.parameters.setMediaDurations (&lengths);

    const auto columnAt = [&rig] (std::int64_t tick)
    {
        rig.parameters.markStale();
        model::ShowModel show;
        REQUIRE (show.refresh (*rig.publish (tick), "P4ACT001"));

        const auto at = show.indexOf ("P4MED003");
        REQUIRE (at >= 0);
        return show.rows()[static_cast<std::size_t> (at)].duration;
    };

    const auto besideAt = [&rig] (std::int64_t tick)
    {
        rig.parameters.markStale();
        const auto reading = model::readTimeline (*rig.publish (tick), "P4GRP002");
        REQUIRE (reading.bars.size() == 2u);
        return reading.bars[1];
    };

    CHECK (columnAt (1) == "30.0");
    CHECK (besideAt (1).lengthKnown);
    CHECK (besideAt (1).length == doctest::Approx (30.0));

    REQUIRE (rig.document.setAttribute ("/godot/cue/P4MED003/rate", "2").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/P4MED002/rate", "2").ok);
    CHECK (columnAt (2) == "15.0");
    CHECK (besideAt (2).length == doctest::Approx (15.0));

    //  The file's own length is the file's, whatever the speed.
    CHECK (model::text (*rig.publish (2), "/godot/cue/P4MED003/duration") == "30");

    REQUIRE (rig.document.setAttribute ("/godot/cue/P4MED003/rate", "0").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/P4MED002/rate", "0").ok);
    CHECK (columnAt (3) == "\xe2\x88\x9e");
    CHECK_FALSE (besideAt (3).lengthKnown);
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
TEST_CASE ("client: a run says whether it can be scrubbed as the engine says, never from its cue's mode")
{
    Rig rig;

    /*  K9 (2026-10-02, namespace draft §23.18). The row worked it out from the
        cue's mode and offered a scrub on every timeline - one that loops, before
        its first round, or in its header, where the engine moves nothing - so
        the head the hand dragged went back. It reads the engine's own
        `run/seekable` now: a timeline the engine says yes to, a timeline in its
        header it says no to, and a manual sequence, which it never says yes to. */
    const auto listId = rig.document.createList ("Sound").id;
    const auto timeline = rig.document.createCue (listId, 0, "group", "Scene").id;
    const auto looping = rig.document.createCue (listId, 1, "group", "Rain").id;
    const auto manual = rig.document.createCue (listId, 2, "group", "Act").id;

    for (const auto& id : { timeline, looping })
        rig.document.setAttribute ("/godot/cue/" + id + "/mode", "timeline");

    rig.document.setAttribute ("/godot/cue/" + looping + "/loops", "0");

    rig.runs.create ("RUNTIME1", timeline, "group");
    rig.runs.create ("RUNL00P1", looping, "group");
    rig.runs.create ("RUNMANU1", manual, "group");

    //  What the Runner's hook mirrors every tick; here, the engine's answer set by hand.
    rig.runs.find ("RUNTIME1")->seekable = true;

    const auto rows = model::readRuns (*rig.publish (1));

    auto seen = 0;

    for (const auto& row : rows)
    {
        if (row.id == "RUNTIME1") { ++seen; CHECK (row.seekable); }
        if (row.id == "RUNL00P1") { ++seen; CHECK_FALSE (row.seekable); }
        if (row.id == "RUNMANU1") { ++seen; CHECK_FALSE (row.seekable); }
    }

    CHECK (seen == 3);
}

//==============================================================================
/*  SCRUBBING A SCENE WITH LOOPS (2026-10-05, namespace draft §30.6, S8; item 10
    of the bug round). The model's half: a scene's scrub held to its round and
    sent once on release (the author's RB), a sound's held to the stretch its
    ranges play, the strip's span and its counts, and why a scene will not
    scrub - read off the tree. */
TEST_CASE ("client: a scene's scrub is held to its round and sent once, when the hand lets go")
{
    /*  A scene in a round from 20 s to 30 s of its own clock, taken at 23 s,
        a tenth of a second a pixel. Before S8 it had no floor, no end - a
        scene "has no file to run out of" - and sent a seek every fifth of a
        second the hand moved. */
    model::Scrub::Setup setup;
    setup.position = 23.0;
    setup.from = 20.0;
    setup.to = 30.0;
    setup.secondsPerPixel = 0.1;
    setup.unit = 20.0;
    setup.x = 100.0;
    setup.onRelease = true;

    model::Scrub scene;
    scene.begin (setup);
    REQUIRE (scene.sendsOnRelease());

    //  The ghost follows the hand, and nothing is due however long it moves.
    CHECK (scene.moveTo (130.0, 0.0) == doctest::Approx (26.0));
    CHECK_FALSE (scene.due (0.0));
    CHECK_FALSE (scene.due (100000.0));

    //  Held to the round at both ends.
    CHECK (scene.moveTo (1000.0, 0.0) == doctest::Approx (30.0));
    CHECK (scene.push (1, 1.0, 0.0) == doctest::Approx (30.0));
    CHECK (scene.moveTo (-1000.0, 0.0) == doctest::Approx (20.0));
    CHECK_FALSE (scene.due (200000.0));

    //  Let go: sent once.
    CHECK (scene.moveTo (-950.0, 0.0) == doctest::Approx (25.0));
    CHECK (scene.settle());
    CHECK_FALSE (scene.settle());

    //  A head a tick past the round's end, as the round turns, is taken at its end - and a grab is not a seek.
    setup.position = 30.04;
    model::Scrub turning;
    turning.begin (setup);
    CHECK (turning.target() == doctest::Approx (30.0));
    CHECK_FALSE (turning.settle());

    //  A sound keeps scrubbing live, held to the stretch its ranges play: 10 s to 20 s of a 95 s file.
    model::Scrub sound;
    sound.begin ({ 15.0, 20.0, 0.05, 40.0, 0.0, 10.0, false });
    CHECK_FALSE (sound.sendsOnRelease());
    CHECK (sound.moveTo (-1000.0, 0.0) == doctest::Approx (10.0));
    CHECK (sound.due (0.0));
    CHECK (sound.moveTo (100000.0, 0.0) == doctest::Approx (20.0));
}

TEST_CASE ("client: a strip spans what the engine honours, and says its range's pass and its scene's round")
{
    //  A sound: the stretch its ranges play, else the whole file the pane measured.
    model::RunRow sound;
    sound.kind = "media";
    sound.playFrom = 10.0;
    sound.playTo = 20.0;

    auto span = model::stripSpan (sound, 95.0);
    CHECK (span.known());
    CHECK (span.from == doctest::Approx (10.0));
    CHECK (span.to == doctest::Approx (20.0));

    sound.playFrom = 0.0;
    sound.playTo = 0.0;
    span = model::stripSpan (sound, 95.0);
    CHECK (span.from == doctest::Approx (0.0));
    CHECK (span.to == doctest::Approx (95.0));
    CHECK_FALSE (model::stripSpan (sound, 0.0).known());

    //  A scene: the round the engine solved, or nothing.
    model::RunRow scene;
    scene.kind = "group";
    scene.roundFrom = 20.0;
    scene.roundLength = 10.0;

    span = model::stripSpan (scene, 0.0);
    CHECK (span.from == doctest::Approx (20.0));
    CHECK (span.to == doctest::Approx (30.0));
    CHECK (span.length() == doctest::Approx (10.0));

    scene.roundLength = 0.0;
    CHECK_FALSE (model::stripSpan (scene, 0.0).known());

    /*  The counts (PRD §3.6, §3.24: "3/8"): range two, its second pass of two;
        a range that plays for ever; one range played once says nothing. */
    model::RangeRow first;
    first.in = 0.0;
    first.out = 10.0;

    model::RangeRow second = first;
    second.in = 10.0;
    second.out = 20.0;
    second.loops = 2;

    sound.ranges = { first, second };
    sound.rangeIndex = 1;
    sound.rangeIteration = 2;
    CHECK (model::countWords (sound) == "R2 2/2");

    sound.ranges[1].loops = 0;
    sound.rangeIteration = 3;
    CHECK (model::countWords (sound) == "R2 3/\xe2\x88\x9e");

    sound.ranges = { first };
    sound.rangeIndex = 0;
    sound.rangeIteration = 1;
    CHECK (model::countWords (sound).empty());

    sound.ranges[0].loops = 4;
    CHECK (model::countWords (sound) == "R1 1/4");

    sound.rangeIndex = -1;
    CHECK (model::countWords (sound).empty());

    scene.iteration = 2;
    scene.iterations = 3;
    CHECK (model::countWords (scene) == "round 2/3");

    scene.iterations = 0;
    CHECK (model::countWords (scene) == "round 2/\xe2\x88\x9e");

    scene.iteration = 1;
    scene.iterations = 1;
    CHECK (model::countWords (scene).empty());
}

TEST_CASE ("client: a scene that will not scrub says why, and the round it is held to is read")
{
    /*  Item 10: neither of the author's groups could be scrubbed - a manual
        sequence and a sampler bank - and a drag on either did nothing and said
        nothing. The row says why now, from the cue and the run's phase, while
        the offer stays the engine's (`seekable`). And the round the engine
        solved, and which of how many, are read beside it. */
    Rig rig;

    const auto listId = rig.document.createList ("Sound").id;
    const auto manual = rig.document.createCue (listId, 0, "group", "Act").id;
    const auto bank = rig.document.createCue (listId, 1, "group", "Pads").id;
    const auto opening = rig.document.createCue (listId, 2, "group", "Opening").id;
    const auto closing = rig.document.createCue (listId, 3, "group", "Closing").id;
    const auto rain = rig.document.createCue (listId, 4, "group", "Rain").id;

    rig.document.setAttribute ("/godot/cue/" + bank + "/mode", "sampler");

    for (const auto& id : { opening, closing, rain })
        rig.document.setAttribute ("/godot/cue/" + id + "/mode", "timeline");

    const auto play = [&rig] (const char* runId, const std::string& cueId, const char* phase)
    {
        rig.runs.create (runId, cueId, "group");
        auto* run = rig.runs.find (runId);
        REQUIRE (run != nullptr);
        run->state = cue::runState::playing;
        run->phase = phase;
        return run;
    };

    play ("RVNMANV1", manual, "members");
    play ("RVNBANK1", bank, "members");
    play ("RVNHEAD1", opening, "header");
    play ("RVNF00T1", closing, "footer");

    //  The engine's answer for the looping scene, as its hooks would mirror it.
    auto* looping = play ("RVNRA1N1", rain, "members");
    looping->seekable = true;
    looping->roundFrom = 20.0;
    looping->roundLength = 10.0;
    looping->iteration = 2;
    looping->iterations = 3;

    std::map<std::string, model::RunRow> read;

    for (const auto& row : model::readRuns (*rig.publish (1)))
        read[row.id] = row;

    REQUIRE (read.size() == 5u);

    CHECK (read["RVNMANV1"].scrubRefusal == "A manual sequence is played by GO \xe2\x80\x94 it cannot be scrubbed");
    CHECK (read["RVNBANK1"].scrubRefusal == "A sampler group is played by hand \xe2\x80\x94 it cannot be scrubbed");
    CHECK (read["RVNHEAD1"].scrubRefusal
             == "Its header is playing \xe2\x80\x94 it can be scrubbed once the header is over");
    CHECK (read["RVNF00T1"].scrubRefusal == "Its footer is playing \xe2\x80\x94 it can no longer be scrubbed");

    const auto& scene = read["RVNRA1N1"];
    CHECK (scene.scrubRefusal.empty());
    CHECK (scene.roundFrom == doctest::Approx (20.0));
    CHECK (scene.roundLength == doctest::Approx (10.0));
    CHECK (scene.iteration == 2);
    CHECK (scene.iterations == 3);
    CHECK (model::countWords (scene) == "round 2/3");

    //  A manual sequence that is not playing is not a strip anybody would drag: nothing said.
    rig.runs.find ("RVNMANV1")->state = cue::runState::armed;

    for (const auto& row : model::readRuns (*rig.publish (2)))
        if (row.id == "RVNMANV1")
            CHECK (row.scrubRefusal.empty());
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

//==============================================================================
namespace
{
    /*  A SHOW WITH A DESK IN IT, on the minimal bundle: two ports, a Mackie
        unit on both of them and the virtual panel, two DCAs one inside the
        other, and two of the panel's strips made dca strips - one riding
        "Band", one riding nothing yet. Written through the document and not
        the engine, because what is under test here is the READING: the
        commands have SurfaceTests.cpp.

        THE IDENTIFIERS ARE GIVEN, AND BACKWARDS ON PURPOSE. The Mackie unit is
        declared first and its identifier sorts after the panel's, and the same
        for the two DCAs, so a reader that answered in identifier order rather
        than in the show's would put them the wrong way round and fail. */
    struct Desk
    {
        std::string lights, bank2;
        std::string mcu = "SZZZ0001", panel = "SAAA0001";
        std::vector<std::string> mcuStrips, panelStrips;
        std::string everything = "DZZZ0001", band = "DAAA0001";
    };

    Desk declareADesk (Rig& rig)
    {
        Desk desk;

        const auto lights = rig.document.createPort ("Lights");
        const auto bank2 = rig.document.createPort ("Bank 2");
        REQUIRE (lights.ok);
        REQUIRE (bank2.ok);
        desk.lights = lights.id;
        desk.bank2 = bank2.id;

        REQUIRE (rig.document.createSurface ("mcu", "Desk", desk.mcu, {}, desk.mcuStrips).ok);
        REQUIRE (rig.document.createSurface ("virtual", "", desk.panel, {}, desk.panelStrips).ok);
        REQUIRE (desk.mcuStrips.size() == 8u);
        REQUIRE (desk.panelStrips.size() == 8u);

        REQUIRE (rig.document.setAttribute ("/godot/surface/" + desk.mcu + "/ports",
                                            desk.lights + " " + desk.bank2).ok);

        REQUIRE (rig.document.createDca ("Everything", desk.everything).ok);
        REQUIRE (rig.document.createDca ("Band", desk.band).ok);
        REQUIRE (rig.document.setAttribute ("/godot/dca/" + desk.band + "/dca", desk.everything).ok);
        REQUIRE (rig.document.setAttribute ("/godot/dca/" + desk.band + "/shortName", "BND").ok);

        const auto slot = [] (const std::string& strip) { return "/godot/slot/" + strip + "/"; };

        REQUIRE (rig.document.setAttribute (slot (desk.panelStrips[1]) + "role", "dca").ok);
        REQUIRE (rig.document.setAttribute (slot (desk.panelStrips[1]) + "dca", desk.band).ok);
        REQUIRE (rig.document.setAttribute (slot (desk.panelStrips[2]) + "role", "dca").ok);

        rig.parameters.markStale();
        return desk;
    }

    /*  Whether a row of the panel means anything for the cue it is about, and
        a failure rather than a crash when the row is not there at all. */
    bool appliesIn (const model::Inspection& inspection, const std::string& name)
    {
        for (const auto& block : inspection.blocks)
            for (const auto& field : block.fields)
                if (field.name == name)
                    return field.applies;

        FAIL ("no row named " << name);
        return false;
    }

    const model::Field* rowIn (const model::Inspection& inspection, const std::string& name)
    {
        for (const auto& block : inspection.blocks)
            for (const auto& field : block.fields)
                if (field.name == name)
                    return &field;

        return nullptr;
    }

    std::vector<std::string> namesUnder (const model::Inspection& inspection, const std::string& heading)
    {
        std::vector<std::string> names;

        for (const auto& block : inspection.blocks)
            if (block.heading == heading)
                for (const auto& field : block.fields)
                    names.push_back (field.name);

        return names;
    }

    /** Where a name sits in a list of them, or the list's length when it is not there. */
    std::size_t positionOf (const std::vector<std::string>& names, const std::string& name)
    {
        const auto found = std::find (names.begin(), names.end(), name);
        return static_cast<std::size_t> (found - names.begin());
    }
}

TEST_CASE ("client: the surfaces are read in the show's order, with their ports and their state in words")
{
    Rig rig;
    const auto desk = declareADesk (rig);

    auto surfaces = model::readSurfaces (*rig.publish (1));
    REQUIRE (surfaces.size() == 2u);

    //  The show's order, which is not the identifiers'.
    CHECK (surfaces[0].id == desk.mcu);
    CHECK (surfaces[1].id == desk.panel);

    /*  THE PORTS ARE A LIST, one per bank and in bank order: the first carries
        strips one to eight, the second nine to sixteen. */
    CHECK (surfaces[0].name == "Desk");
    CHECK (surfaces[0].label() == "Desk");
    CHECK (surfaces[0].profile == "mcu");
    CHECK (surfaces[0].ports == std::vector<std::string> { desk.lights, desk.bank2 });
    CHECK (surfaces[0].strips == 8);
    CHECK (surfaces[0].enabled);
    CHECK_FALSE (surfaces[0].connected);

    /*  A HARDWARE SURFACE NOBODY HAS TALKED TO IS NOT CONNECTED, in those
        words; the virtual panel is, being this client's own. And a surface
        nobody named is called what its profile is. */
    CHECK (surfaces[0].stateWord() == "not connected");
    CHECK (surfaces[1].name.empty());
    CHECK (surfaces[1].label() == "Virtual panel");
    CHECK (surfaces[1].ports.empty());
    CHECK (surfaces[1].stateWord() == "connected");

    //  The engine's own sentence when it has one, never rewritten here.
    surface::SurfaceTable table;
    rig.parameters.setSurfaces (&table);

    table.set (desk.mcu, { false, "the port \"Lights\" has no device behind it", {} });
    rig.parameters.markStale();
    surfaces = model::readSurfaces (*rig.publish (2));
    CHECK (surfaces[0].stateWord() == "the port \"Lights\" has no device behind it");

    table.set (desk.mcu, { true, {}, "D700RTB" });
    rig.parameters.markStale();
    surfaces = model::readSurfaces (*rig.publish (3));
    CHECK (surfaces[0].connected);
    CHECK (surfaces[0].serial == "D700RTB");
    CHECK (surfaces[0].stateWord() == "connected");

    /*  OFF IS SOMEBODY'S DECISION, and says so over whatever the cables are
        doing: a row reading "connected" over a surface the operator switched
        off would be the window hiding the switch. */
    REQUIRE (rig.document.setAttribute ("/godot/surface/" + desk.mcu + "/enabled", "false").ok);
    rig.parameters.markStale();
    surfaces = model::readSurfaces (*rig.publish (4));
    CHECK_FALSE (surfaces[0].enabled);
    CHECK (surfaces[0].stateWord() == "off");

    rig.parameters.setSurfaces (nullptr);

    //  The four profiles, the panel first: the one that works with nothing plugged in.
    const auto profiles = model::profileChoices();
    REQUIRE (profiles.size() == 4u);
    CHECK (profiles[0] == std::pair<std::string, std::string> { "virtual", "Virtual panel" });
    CHECK (profiles[1] == std::pair<std::string, std::string> { "mcu", "Mackie Control" });
    CHECK (profiles[2] == std::pair<std::string, std::string> { "d700", "Asparion D700" });
    CHECK (profiles[3] == std::pair<std::string, std::string> { "midiPads", "Pads" });
}

TEST_CASE ("client: every strip is read in the order a sampler group fills them, with what it rides")
{
    Rig rig;
    const auto desk = declareADesk (rig);

    cue::DcaTable trims;
    rig.parameters.setDcas (&trims);

    auto strips = model::readStrips (*rig.publish (1));
    REQUIRE (strips.size() == 16u);

    /*  SURFACE ORDER, THEN INDEX: the Mackie unit's eight, then the panel's -
        the order a sampler group fills them in, left to right. */
    for (std::size_t at = 0; at < 8; ++at)
    {
        INFO ("strip " << at);

        CHECK (strips[at].id == desk.mcuStrips[at]);
        CHECK (strips[at].surface == desk.mcu);
        CHECK (strips[at].index == static_cast<int> (at));

        CHECK (strips[8 + at].id == desk.panelStrips[at]);
        CHECK (strips[8 + at].surface == desk.panel);
        CHECK (strips[8 + at].index == static_cast<int> (at));
    }

    //  A strip with nothing on it: a fader riding nothing, and the dash.
    const auto idle = strips[0];
    CHECK (idle.role == "sampler");
    CHECK (idle.endpoint == "absolute");
    CHECK (idle.word == "free");
    CHECK (idle.target.empty());
    CHECK (idle.cue.empty());
    CHECK (idle.holder.empty());
    CHECK_FALSE (idle.hasLevel);
    CHECK (idle.label() == "—");

    /*  A DCA STRIP RIDES ITS DCA'S TRIM - the one node its fader writes - and
        is called by the DCA's short name, which is what a scribble strip is
        written for. */
    const auto riding = strips[9];
    CHECK (riding.role == "dca");
    CHECK (riding.dca == desk.band);
    CHECK (riding.word == "dca");
    CHECK (riding.target == "/godot/dca/" + desk.band + "/trim");
    CHECK (riding.dcaName == "BND");
    CHECK (riding.label() == "BND");
    CHECK (riding.hasLevel);
    CHECK (riding.levelDb == doctest::Approx (0.0));

    //  One naming no DCA says so in its word, and rides nothing.
    const auto unassigned = strips[10];
    CHECK (unassigned.role == "dca");
    CHECK (unassigned.word == "unassigned");
    CHECK (unassigned.target.empty());
    CHECK_FALSE (unassigned.hasLevel);
    CHECK (unassigned.label() == "—");

    //  The value under the fader is the engine's, not the last one a hand sent.
    trims.set (desk.band, -6.0);
    strips = model::readStrips (*rig.publish (2));
    CHECK (strips[9].levelDb == doctest::Approx (-6.0));

    rig.parameters.setDcas (nullptr);

    //  One surface's strips, in index order; a surface that is not there has none.
    const auto panel = model::stripsOf (strips, desk.panel);
    REQUIRE (panel.size() == 8u);
    CHECK (panel.front().id == desk.panelStrips.front());
    CHECK (panel.back().index == 7);
    CHECK (model::stripsOf (strips, "ZZZZZZZZ").empty());

    /*  WHAT A STRIP WITH A CUE ON IT SAYS, which needs a sampler group armed to
        happen in a show and nothing but the row to decide: the short name the
        cue's author wrote, then its name, never cut here. */
    model::StripRow holding;
    holding.cue = "C1C1C1C1";
    CHECK (holding.label() == "C1C1C1C1");
    holding.cueNumber = "4.1";
    CHECK (holding.label() == "4.1");
    holding.cueName = "Thunder and lightning";
    CHECK (holding.label() == "Thunder and lightning");
    holding.cueShortName = "THNDR";
    CHECK (holding.label() == "THNDR");
}

TEST_CASE ("client: the DCAs are read in the show's order, and a menu of them starts with none")
{
    Rig rig;
    const auto desk = declareADesk (rig);

    cue::DcaTable trims;
    rig.parameters.setDcas (&trims);
    trims.set (desk.band, -3.5);

    const auto dcas = model::readDcas (*rig.publish (1));
    REQUIRE (dcas.size() == 2u);

    //  Declared first, read first - although its identifier sorts last.
    CHECK (dcas[0].id == desk.everything);
    CHECK (dcas[0].name == "Everything");
    CHECK (dcas[0].parent.empty());
    CHECK (dcas[0].label() == "Everything");
    CHECK (dcas[0].trimDb == doctest::Approx (0.0));

    CHECK (dcas[1].id == desk.band);
    CHECK (dcas[1].name == "Band");
    CHECK (dcas[1].parent == desk.everything);
    CHECK (dcas[1].shortName == "BND");
    CHECK (dcas[1].label() == "BND");
    CHECK (dcas[1].trimDb == doctest::Approx (-3.5));

    rig.parameters.setDcas (nullptr);

    /*  "(NONE)" FIRST, with an empty key: no DCA is what every cue is until
        somebody assigns one. And the WHOLE name in a menu, which has the room
        a scribble strip does not; the key is the identifier either way. */
    const auto choices = model::dcaChoices (dcas);
    REQUIRE (choices.size() == 3u);
    CHECK (choices[0].first.empty());
    CHECK (choices[0].second == "(none)");
    CHECK (choices[1] == std::pair<std::string, std::string> { desk.everything, "Everything" });
    CHECK (choices[2] == std::pair<std::string, std::string> { desk.band, "Band" });

    //  A show with no DCA still has the one entry that says so.
    CHECK (model::dcaChoices ({}).size() == 1u);
}

namespace
{
    using Writes = std::vector<std::pair<std::string, std::string>>;

    /*  THE WRITES A CHOICE MAKES, SENT AS THE WINDOW SENDS THEM: one
        `node.set` each, in order, through the registry and the engine's own
        door, applied in one tick. */
    void sendWrites (Rig& rig, const Writes& writes, std::int64_t tick)
    {
        for (const auto& write : writes)
        {
            const auto event = gesture::setNode (write.first, write.second);
            const auto* command = rig.engine.commands().find (event.command);

            REQUIRE (command != nullptr);
            CHECK (CommandRegistry::checkArgs (*command, event.args).ok);
            REQUIRE (rig.engine.submit (event));
        }

        const auto outcome = rig.engine.processTick (tick);
        CHECK (outcome.applied == writes.size());
        CHECK (outcome.rejected == 0u);
        rig.parameters.markStale();
    }

    const model::StripRow* stripIn (const std::vector<model::StripRow>& strips, const std::string& id)
    {
        for (const auto& strip : strips)
            if (strip.id == id)
                return &strip;

        return nullptr;
    }
}

TEST_CASE ("client: a strip's Role is one menu, Sampler then each DCA, and a choice sends the role before the DCA")
{
    /*  Namespace draft §30, S4. The author, 2026-10-05: "adding some DCA in
        the surface parameters did not show the DCA anywhere". A DCA reached a
        fader only through a second cell that took no click until the first
        said DCA; now one menu says both. */
    Rig rig;
    const auto desk = declareADesk (rig);

    const auto strips = model::readStrips (*rig.publish (1));
    const auto dcas = model::readDcas (*rig.publish (1));
    REQUIRE (strips.size() == 16u);
    REQUIRE (dcas.size() == 2u);

    const auto slot = [] (const std::string& strip) { return "/godot/slot/" + strip + "/"; };

    /*  A SAMPLER STRIP: Sampler, and it is what the strip is now; then each
        DCA in the show's order, by its WHOLE name - "Band", not the "BND" a
        scribble strip shows. */
    const auto sampler = *stripIn (strips, desk.mcuStrips[0]);
    auto choices = model::roleChoices (sampler, dcas);

    REQUIRE (choices.size() == 3u);
    CHECK (choices[0].role == "sampler");
    CHECK (choices[0].dca.empty());
    CHECK (choices[0].label == "Sampler");
    CHECK (choices[0].now);
    CHECK (choices[1].role == "dca");
    CHECK (choices[1].dca == desk.everything);
    CHECK (choices[1].label == "DCA: Everything");
    CHECK_FALSE (choices[1].now);
    CHECK (choices[2].dca == desk.band);
    CHECK (choices[2].label == "DCA: Band");

    for (const auto& choice : choices)
        CHECK (choice.enabled);

    CHECK (model::roleWords (sampler, dcas) == "Sampler");

    //  A DCA strip riding Band: that item is now, and the cell says it.
    const auto riding = *stripIn (strips, desk.panelStrips[1]);
    choices = model::roleChoices (riding, dcas);

    REQUIRE (choices.size() == 3u);
    CHECK_FALSE (choices[0].now);
    CHECK (choices[2].now);
    CHECK (model::roleWords (riding, dcas) == "DCA: Band");

    /*  WHAT IT IS NOW, WHEN NO ITEM IS: a DCA strip riding none, and one
        naming a DCA the show no longer declares - at the end, so the menu
        never opens on nothing picked. */
    const auto unassigned = *stripIn (strips, desk.panelStrips[2]);
    choices = model::roleChoices (unassigned, dcas);

    REQUIRE (choices.size() == 4u);
    CHECK (choices[3].now);
    CHECK (choices[3].role == "dca");
    CHECK (choices[3].dca.empty());
    CHECK (choices[3].label == "DCA: none chosen");
    CHECK (model::roleWords (unassigned, dcas) == "DCA: none chosen");

    auto stale = riding;
    stale.dca = "DQQQ0001";
    choices = model::roleChoices (stale, dcas);

    REQUIRE (choices.size() == 4u);
    CHECK (choices.back().now);
    CHECK (choices.back().dca == "DQQQ0001");
    CHECK (choices.back().label == "DCA: DQQQ0001  (not declared)");

    /*  A SHOW WITH NO DCA: one greyed item, which says where a DCA comes from
        and writes nothing. */
    choices = model::roleChoices (sampler, {});

    REQUIRE (choices.size() == 2u);
    CHECK (choices[0].now);
    CHECK_FALSE (choices[1].enabled);
    CHECK (choices[1].role.empty());
    CHECK (choices[1].label == "No DCA yet: ADD DCA below makes one");
    CHECK (model::roleWrites (sampler, choices[1]).empty());

    /*  THE WRITES: the role first, then the DCA; Sampler clears the DCA; and
        what is already so is not written. */
    choices = model::roleChoices (sampler, dcas);
    CHECK (model::roleWrites (sampler, choices[2])
           == Writes { { slot (sampler.id) + "role", "dca" }, { slot (sampler.id) + "dca", desk.band } });
    CHECK (model::roleWrites (sampler, choices[0]).empty());

    choices = model::roleChoices (riding, dcas);
    CHECK (model::roleWrites (riding, choices[1]) == Writes { { slot (riding.id) + "dca", desk.everything } });
    CHECK (model::roleWrites (riding, choices[2]).empty());
    CHECK (model::roleWrites (riding, choices[0])
           == Writes { { slot (riding.id) + "role", "sampler" }, { slot (riding.id) + "dca", "" } });

    //  A sampler strip still carrying a DCA from before: Sampler takes it off.
    auto carrying = sampler;
    carrying.dca = desk.band;
    CHECK (model::roleWrites (carrying, model::roleChoices (carrying, dcas)[0])
           == Writes { { slot (sampler.id) + "dca", "" } });

    /*  AGAINST THE REAL REGISTRY AND THE REAL DOCUMENT: a sampler strip made
        to ride Band rides its trim, in the engine's word, from that tick. */
    sendWrites (rig, model::roleWrites (sampler, model::roleChoices (sampler, dcas)[2]), 2);

    auto now = model::readStrips (*rig.publish (2));
    REQUIRE (stripIn (now, sampler.id) != nullptr);
    CHECK (stripIn (now, sampler.id)->role == "dca");
    CHECK (stripIn (now, sampler.id)->dca == desk.band);
    CHECK (stripIn (now, sampler.id)->word == "dca");
    CHECK (stripIn (now, sampler.id)->target == "/godot/dca/" + desk.band + "/trim");
    CHECK (model::roleWords (*stripIn (now, sampler.id), dcas) == "DCA: Band");

    //  And the one that rode Band, made a sampler strip again, carries no DCA.
    sendWrites (rig, model::roleWrites (riding, model::roleChoices (riding, dcas)[0]), 3);

    now = model::readStrips (*rig.publish (3));
    REQUIRE (stripIn (now, riding.id) != nullptr);
    CHECK (stripIn (now, riding.id)->role == "sampler");
    CHECK (stripIn (now, riding.id)->dca.empty());
    CHECK (stripIn (now, riding.id)->target.empty());
    CHECK (model::roleWords (*stripIn (now, riding.id), dcas) == "Sampler");
}

TEST_CASE ("client: each DCA says which faders ride it, in words, and no fader when none does")
{
    /*  Namespace draft §30, S4: a DCA made with ADD DCA and never put on a
        strip was invisible, so the DCA list says it. Two surfaces here - the
        desk and the virtual panel - so each fader is named with its own. */
    Rig rig;
    const auto desk = declareADesk (rig);

    auto snapshot = rig.publish (1);
    auto surfaces = model::readSurfaces (*snapshot);
    REQUIRE (surfaces.size() == 2u);

    CHECK (model::fadersRiding (desk.band, model::readStrips (*snapshot), surfaces) == "Virtual panel · fader 2");
    CHECK (model::fadersRiding (desk.everything, model::readStrips (*snapshot), surfaces) == "no fader");

    //  A DCA strip riding none is not riding "nothing called nothing".
    CHECK (model::fadersRiding ("", model::readStrips (*snapshot), surfaces) == "no fader");

    /*  TWO MORE ON THE DESK, made in the wrong order: surface order, then
        index, and one surface after another. A sampler strip still carrying
        Band from before rides nothing, and is not counted. */
    const auto slot = [] (const std::string& strip) { return "/godot/slot/" + strip + "/"; };

    for (const auto& strip : { desk.mcuStrips[4], desk.mcuStrips[2] })
    {
        REQUIRE (rig.document.setAttribute (slot (strip) + "role", "dca").ok);
        REQUIRE (rig.document.setAttribute (slot (strip) + "dca", desk.band).ok);
    }

    REQUIRE (rig.document.setAttribute (slot (desk.mcuStrips[6]) + "dca", desk.band).ok);

    rig.parameters.markStale();
    snapshot = rig.publish (2);
    CHECK (model::fadersRiding (desk.band, model::readStrips (*snapshot), model::readSurfaces (*snapshot))
           == "Desk · faders 3 and 5; Virtual panel · fader 2");

    /*  ONE SURFACE: nothing to tell it from, so no name in front - and three
        or more are a list with "and" before the last. */
    REQUIRE (rig.document.remove (desk.mcu).ok);
    REQUIRE (rig.document.setAttribute (slot (desk.panelStrips[3]) + "role", "dca").ok);
    REQUIRE (rig.document.setAttribute (slot (desk.panelStrips[3]) + "dca", desk.band).ok);

    rig.parameters.markStale();
    snapshot = rig.publish (3);
    surfaces = model::readSurfaces (*snapshot);
    REQUIRE (surfaces.size() == 1u);
    CHECK (model::fadersRiding (desk.band, model::readStrips (*snapshot), surfaces) == "Faders 2 and 4");

    REQUIRE (rig.document.setAttribute (slot (desk.panelStrips[5]) + "role", "dca").ok);
    REQUIRE (rig.document.setAttribute (slot (desk.panelStrips[5]) + "dca", desk.band).ok);

    rig.parameters.markStale();
    snapshot = rig.publish (4);
    CHECK (model::fadersRiding (desk.band, model::readStrips (*snapshot), surfaces) == "Faders 2, 4 and 6");

    //  A pad is a pad, with one surface and with two.
    model::StripRow pad;
    pad.id = "PADS0001";
    pad.surface = "SPAD0001";
    pad.index = 2;
    pad.role = "dca";
    pad.dca = desk.band;
    pad.endpoint = "gate";

    CHECK (model::fadersRiding (desk.band, { pad }, {}) == "Pad 3");

    model::SurfaceRow pads;
    pads.id = "SPAD0001";
    pads.profile = "midiPads";

    CHECK (model::fadersRiding (desk.band, { pad }, { surfaces[0], pads }) == "Pads · pad 3");
}

TEST_CASE ("client: a strip dragged in the Surfaces tab lands where its line was, and the engine renumbers the surface")
{
    /*  Namespace draft §30, S4: "there is no way to rearrange the order". The
        position is `object.move`'s, in the list as it stands with the dragged
        strip still counted (model/Reorder.h's rule): four strips A B C D, the
        gaps 0 to 4 between them. */
    CHECK_FALSE (model::stripMovePosition (0, 0, 4).has_value());   // A above itself
    CHECK_FALSE (model::stripMovePosition (0, 1, 4).has_value());   // A below itself
    CHECK (model::stripMovePosition (0, 2, 4) == 1);                // A after B: B's place
    CHECK (model::stripMovePosition (0, 3, 4) == 2);                // A after C
    CHECK (model::stripMovePosition (0, 4, 4) == 3);                // A to the end
    CHECK (model::stripMovePosition (3, 0, 4) == 0);                // D to the top
    CHECK (model::stripMovePosition (3, 1, 4) == 1);                // D after A
    CHECK_FALSE (model::stripMovePosition (3, 3, 4).has_value());
    CHECK_FALSE (model::stripMovePosition (3, 4, 4).has_value());
    CHECK (model::stripMovePosition (1, 3, 4) == 2);                // B after C

    //  Off the list either way: nothing.
    CHECK_FALSE (model::stripMovePosition (-1, 2, 4).has_value());
    CHECK_FALSE (model::stripMovePosition (4, 2, 4).has_value());
    CHECK_FALSE (model::stripMovePosition (2, -1, 4).has_value());
    CHECK_FALSE (model::stripMovePosition (2, 5, 4).has_value());

    /*  AGAINST THE REAL ENGINE: the panel's first strip let go under its
        fourth is the fourth fader, and the DCA strip that was fader 2 is
        fader 1 - which the DCA list says. */
    Rig rig;
    const auto desk = declareADesk (rig);

    const auto to = model::stripMovePosition (0, 4, 8);
    REQUIRE (to.has_value());
    CHECK (*to == 3);

    const auto move = gesture::moveObject (desk.panelStrips[0], desk.panel, *to);
    const auto* command = rig.engine.commands().find (move.command);
    REQUIRE (command != nullptr);
    CHECK (CommandRegistry::checkArgs (*command, move.args).ok);

    REQUIRE (rig.engine.submit (move));
    CHECK (rig.engine.processTick (2).applied == 1u);
    rig.parameters.markStale();

    auto snapshot = rig.publish (2);
    auto panel = model::stripsOf (model::readStrips (*snapshot), desk.panel);
    REQUIRE (panel.size() == 8u);

    const std::vector<std::string> moved { desk.panelStrips[1], desk.panelStrips[2], desk.panelStrips[3],
                                           desk.panelStrips[0], desk.panelStrips[4], desk.panelStrips[5],
                                           desk.panelStrips[6], desk.panelStrips[7] };

    for (std::size_t at = 0; at < panel.size(); ++at)
    {
        INFO ("fader " << at + 1);
        CHECK (panel[at].id == moved[at]);
        CHECK (panel[at].index == static_cast<int> (at));
    }

    CHECK (model::fadersRiding (desk.band, model::readStrips (*snapshot), model::readSurfaces (*snapshot))
           == "Virtual panel · fader 1");

    //  And back to the top: the gap above the first.
    const auto back = model::stripMovePosition (3, 0, 8);
    REQUIRE (back.has_value());

    REQUIRE (rig.engine.submit (gesture::moveObject (desk.panelStrips[0], desk.panel, *back)));
    CHECK (rig.engine.processTick (3).applied == 1u);
    rig.parameters.markStale();

    panel = model::stripsOf (model::readStrips (*rig.publish (3)), desk.panel);
    REQUIRE (panel.size() == 8u);

    for (std::size_t at = 0; at < panel.size(); ++at)
        CHECK (panel[at].id == desk.panelStrips[at]);
}

TEST_CASE ("client: a fade's two switches, each before what it moves, and what a switch leaves alone is greyed")
{
    /*  Namespace draft §22.7: the Level switch before the level, the Speed
        switch before the speed, and the curve - the shape of both - after
        both. Greyed while its switch is off and never hidden, so turning one
        on finds the row where it already was; and a greyed number is not one
        the dial turns. */
    Rig rig;

    const auto fade = rig.document.createCue ("7K2QM9X4", 0, "fade", "Slower");
    REQUIRE (fade.ok);

    const auto panelNow = [&rig, &fade]
    {
        rig.parameters.markStale();
        return model::inspect (*rig.publish (1), fade.id);
    };

    auto panel = panelNow();
    const auto does = namesUnder (panel, "what it does");
    const auto levelOnAt = positionOf (does, "levelOn");
    REQUIRE (levelOnAt + 4 < does.size());
    CHECK (does[levelOnAt + 1] == "level");
    CHECK (does[levelOnAt + 2] == "rateOn");
    CHECK (does[levelOnAt + 3] == "rate");
    CHECK (does[levelOnAt + 4] == "curve");

    for (const auto& said : std::vector<std::pair<std::string, std::string>> {
             { "levelOn", "moves level" }, { "rateOn", "moves speed" }, { "rate", "speed" } })
    {
        INFO ("row " << said.first);
        REQUIRE (rowIn (panel, said.first) != nullptr);
        CHECK (rowIn (panel, said.first)->label == said.second);
    }

    //  As every fade was: the level moves, the speed does not.
    CHECK (rowIn (panel, "level")->applies);
    CHECK_FALSE (rowIn (panel, "rate")->applies);
    CHECK (rowIn (panel, "curve")->applies);
    CHECK_FALSE (model::mayDial (*rowIn (panel, "rate")));

    //  The speed switched on: its row lights, and the dial may turn it.
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + fade.id + "/rateOn", "true").ok);
    panel = panelNow();
    CHECK (rowIn (panel, "rate")->applies);
    CHECK (model::mayDial (*rowIn (panel, "rate")));

    //  The level switched off: the level greys, the curve still shapes the speed.
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + fade.id + "/levelOn", "false").ok);
    panel = panelNow();
    CHECK_FALSE (rowIn (panel, "level")->applies);
    CHECK (rowIn (panel, "curve")->applies);

    //  Neither: nothing to shape.
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + fade.id + "/rateOn", "false").ok);
    panel = panelNow();
    CHECK_FALSE (rowIn (panel, "curve")->applies);
}

TEST_CASE ("client: a transport cue's verb is a worded menu, and what the verb leaves alone is greyed")
{
    /*  Namespace draft §27: "and Go" after the verb, live for a jump only; the
        slice for advance; the curve for a fade. Greyed, never hidden. */
    Rig rig;

    const auto jump = rig.document.createCue ("7K2QM9X4", 0, "transport", "Back");
    REQUIRE (jump.ok);

    const auto panelNow = [&rig, &jump]
    {
        rig.parameters.markStale();
        return model::inspect (*rig.publish (1), jump.id);
    };

    auto panel = panelNow();
    const auto does = namesUnder (panel, "what it does");
    const auto verbAt = positionOf (does, "verb");
    REQUIRE (verbAt + 1 < does.size());
    CHECK (does[verbAt + 1] == "andGo");
    REQUIRE (rowIn (panel, "andGo") != nullptr);
    CHECK (rowIn (panel, "andGo")->label == "and Go");

    //  A stop: and Go and the slice greyed.
    CHECK_FALSE (rowIn (panel, "andGo")->applies);
    CHECK_FALSE (rowIn (panel, "range")->applies);

    const auto& verb = *rowIn (panel, "verb");
    const auto worded = std::find_if (verb.choices.begin(), verb.choices.end(),
                                      [] (const auto& choice) { return choice.first == "jump"; });
    REQUIRE (worded != verb.choices.end());
    CHECK (worded->second == "jump standby to the target");

    REQUIRE (rig.document.setAttribute ("/godot/cue/" + jump.id + "/verb", "jump").ok);
    panel = panelNow();
    CHECK (rowIn (panel, "andGo")->applies);
    CHECK_FALSE (rowIn (panel, "curve")->applies);
}

TEST_CASE ("client: a cue's DCA is a menu of the show's DCAs, and the sampler rows follow what a media cue has")
{
    Rig rig;
    const auto desk = declareADesk (rig);

    const auto media = rig.document.createCue ("7K2QM9X4", 0, "media", "Thunder");
    const auto fade = rig.document.createCue ("7K2QM9X4", 1, "fade", "Band down");
    REQUIRE (media.ok);
    REQUIRE (fade.ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + media.id + "/dca", desk.band).ok);

    rig.parameters.markStale();
    const auto snapshot = rig.publish (1);
    const auto panel = model::inspect (*snapshot, media.id);

    /*  THE SHORT NAME UNDER THE NAME IT SHORTENS, on every cue: it is what a
        seven-character display shows for this one. */
    const auto first = namesUnder (panel, "what it is");
    REQUIRE (first.size() >= 3u);
    CHECK (first[0] == "number");
    CHECK (first[1] == "name");
    CHECK (first[2] == "shortName");

    /*  A SAMPLER MEMBER'S ROWS AFTER EVERYTHING A MEDIA CUE HAS, in the order a
        press happens - in a drawer of their own since 2026-09-30 ("we can also
        make more drawers for things"), straight after what the cue does. The
        DCA stays behind with what the cue does: it trims any cue. */
    const std::vector<std::string> wanted { "level", "startOffset", "dca" };
    const auto does = namesUnder (panel, "what it does");

    std::vector<std::string> seen;

    for (const auto& name : does)
        if (std::find (wanted.begin(), wanted.end(), name) != wanted.end())
            seen.push_back (name);

    CHECK (seen == wanted);

    const std::vector<std::string> pressed { "strip", "initialLevel", "release", "secondPress", "velocity",
                                             "velocityFloor", "pressure", "releaseFade" };
    CHECK (namesUnder (panel, "sampler") == pressed);

    for (const auto& name : pressed)
        CHECK (positionOf (does, name) == does.size());

    //  The drawer comes straight after what the cue does, and before its place in the list.
    std::vector<std::string> headings;

    for (const auto& block : panel.blocks)
        headings.push_back (block.heading);

    const auto doesAt = positionOf (headings, "what it does");
    REQUIRE (doesAt + 1 < headings.size());
    CHECK (headings[doesAt + 1] == "sampler");

    /*  The speed and its mode straight after where the file starts, then the
        sampler's rows (namespace draft §22.7). */
    const auto startOffsetAt = positionOf (does, "startOffset");
    REQUIRE (startOffsetAt + 3 < does.size());
    CHECK (does[startOffsetAt + 1] == "rate");
    CHECK (does[startOffsetAt + 2] == "rateMode");
    CHECK (does[startOffsetAt + 3] == "dca");

    //  Two words the tree runs together, said as two words.
    const std::vector<std::pair<std::string, std::string>> spoken {
        { "secondPress", "second press" }, { "velocityFloor", "velocity floor" },
        { "releaseFade", "release fade" }, { "shortName", "short name" },
        { "initialLevel", "initial level" }, { "rate", "speed" },
        { "rateMode", "speed mode" } };

    for (const auto& said : spoken)
    {
        INFO ("row " << said.first);

        const auto* row = rowIn (panel, said.first);
        REQUIRE (row != nullptr);
        CHECK (row->label == said.second);
    }

    /*  THE DCA IS A MENU: the identifier it writes, the name a person reads,
        and "(none)" first. What the cue is marked with is its value. */
    const auto* dca = rowIn (panel, "dca");
    REQUIRE (dca != nullptr);
    CHECK (dca->control == model::Control::dcaRef);
    CHECK (dca->writable);
    CHECK (dca->applies);          // any media cue can be trimmed, pressed or fired
    CHECK (dca->value == desk.band);
    CHECK (dca->address == "/godot/cue/" + media.id + "/dca");
    REQUIRE (dca->choices.size() == 3u);
    CHECK (dca->choices[0] == std::pair<std::string, std::string> { "", "(none)" });
    CHECK (dca->choices[1].first == desk.everything);
    CHECK (dca->choices[2] == std::pair<std::string, std::string> { desk.band, "Band" });

    //  A fade's DCA beside its target - the other thing it can move - and a menu too.
    const auto fading = model::inspect (*snapshot, fade.id);
    const auto fadeRows = namesUnder (fading, "what it does");
    REQUIRE (fadeRows.size() >= 4u);
    CHECK (fadeRows[0] == "target");
    CHECK (fadeRows[1] == "dca");
    CHECK (fadeRows[2] == "levelOn");
    CHECK (fadeRows[3] == "level");
    REQUIRE (rowIn (fading, "dca") != nullptr);
    CHECK (rowIn (fading, "dca")->control == model::Control::dcaRef);

    //  A group's: `takeover` beside `mode`, and the DCA it answers to after its round.
    const auto group = model::inspect (*snapshot, "D9FH2JKA");
    const auto groupRows = namesUnder (group, "what it does");
    REQUIRE (groupRows.size() >= 2u);
    CHECK (groupRows[0] == "mode");
    CHECK (groupRows[1] == "takeover");

    const auto seedAt = positionOf (groupRows, "seed");
    REQUIRE (seedAt + 1 < groupRows.size());
    CHECK (groupRows[seedAt + 1] == "dca");
    REQUIRE (rowIn (group, "dca") != nullptr);
    CHECK (rowIn (group, "dca")->control == model::Control::dcaRef);

    /*  EIGHT CUES ON ONE DCA IS ONE MENU WRITING EIGHT ROWS - §3.28's
        multi-select assignment, with nothing new. Cues on different DCAs
        disagree, and say so rather than showing one of them. */
    const auto both = model::inspectMany (*snapshot, { media.id, fade.id });
    const auto* shared = rowIn (both, "dca");
    REQUIRE (shared != nullptr);
    CHECK (shared->control == model::Control::dcaRef);
    CHECK (shared->mixed);
    CHECK (shared->addresses == std::vector<std::string> { "/godot/cue/" + media.id + "/dca",
                                                           "/godot/cue/" + fade.id + "/dca" });
}

TEST_CASE ("client: a sampler row is greyed on a cue no hand can press, and drawn on one a hand can")
{
    Rig rig;

    const std::string list = "7K2QM9X4";

    const auto loose = rig.document.createCue (list, 0, "media", "Loose");
    const auto another = rig.document.createCue (list, 1, "media", "Another");
    const auto pads = rig.document.createCue (list, 2, "group", "Pads");
    REQUIRE (loose.ok);
    REQUIRE (another.ok);
    REQUIRE (pads.ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + pads.id + "/mode", "sampler").ok);

    const auto member = rig.document.createCue (pads.id, 0, "media", "Thunder");
    REQUIRE (member.ok);

    /*  A HEADER'S CUE HAS THE GROUP FOR ITS PARENT in the tree, and is still no
        member: a header is the group's preparation, and no strip holds one. */
    const auto header = rig.document.createRole (pads.id, "header");
    REQUIRE (header.ok);
    const auto preload = rig.document.createCue (header.id, 0, "media", "Preload");
    REQUIRE (preload.ok);

    rig.parameters.markStale();
    auto snapshot = rig.publish (1);

    const std::vector<std::string> samplerRows { "initialLevel", "release", "secondPress", "velocity",
                                                 "velocityFloor", "pressure", "releaseFade" };

    //  Outside a sampler group nothing presses a cue, so none of them means anything.
    const auto outside = model::inspect (*snapshot, loose.id);

    for (const auto& name : samplerRows)
    {
        INFO ("row " << name);
        CHECK_FALSE (appliesIn (outside, name));
    }

    //  The DCA is not one of them: a DCA trims any media cue.
    CHECK (appliesIn (outside, "dca"));

    const auto prepared = model::inspect (*snapshot, preload.id);
    CHECK_FALSE (appliesIn (prepared, "release"));

    /*  A MEMBER OF A SAMPLER GROUP: they apply - all but the floor, which is the
        bottom of a velocity or pressure scale neither of which is on. */
    auto inside = model::inspect (*snapshot, member.id);

    CHECK (appliesIn (inside, "release"));
    CHECK (appliesIn (inside, "secondPress"));
    CHECK (appliesIn (inside, "velocity"));
    CHECK (appliesIn (inside, "pressure"));
    CHECK (appliesIn (inside, "releaseFade"));
    CHECK_FALSE (appliesIn (inside, "velocityFloor"));

    /*  A HOLD CLIP CANNOT BE PRESSED AGAIN by the hand still holding it, so its
        second press means nothing; and velocity on gives the floor a scale. */
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + member.id + "/release", "hold").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + member.id + "/velocity", "true").ok);
    rig.parameters.markStale();
    snapshot = rig.publish (2);
    inside = model::inspect (*snapshot, member.id);

    CHECK (appliesIn (inside, "release"));
    CHECK_FALSE (appliesIn (inside, "secondPress"));
    CHECK (appliesIn (inside, "velocityFloor"));

    //  Pressure alone gives it one too.
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + member.id + "/velocity", "false").ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + member.id + "/pressure", "true").ok);
    rig.parameters.markStale();
    snapshot = rig.publish (3);
    CHECK (appliesIn (model::inspect (*snapshot, member.id), "velocityFloor"));

    //  A group's takeover is a question only a sampler group is asked.
    CHECK (appliesIn (model::inspect (*snapshot, pads.id), "takeover"));
    CHECK_FALSE (appliesIn (model::inspect (*snapshot, "D9FH2JKA"), "takeover"));

    /*  AND A SEQUENCE'S QUESTIONS ARE NEVER ASKED OF ONE: the hand launches a
        sampler group's members, so how it advances and how its rounds are
        drawn mean nothing there - and still everything to any other group. */
    for (const auto* name : { "advance", "selection", "play", "loops", "seed" })
    {
        INFO ("row " << name);
        CHECK_FALSE (appliesIn (model::inspect (*snapshot, pads.id), name));
        CHECK (appliesIn (model::inspect (*snapshot, "D9FH2JKA"), name));
    }

    /*  SEVERAL CUES: greyed only where it is greyed for every one of them, and
        whichever of them happens to be first. */
    CHECK (appliesIn (model::inspectMany (*snapshot, { loose.id, member.id }), "release"));
    CHECK (appliesIn (model::inspectMany (*snapshot, { member.id, loose.id }), "release"));
    CHECK_FALSE (appliesIn (model::inspectMany (*snapshot, { loose.id, another.id }), "release"));
}

//==============================================================================
/*  PHASE 9a: A MEDIA CUE'S EQ, read back as the one value the voice is given,
    and drawn from the maths the voice plays. The rows are the document's; the
    reader turns nineteen of them into `audio::EqSettings`; the curve asks
    `audio/EqMath.h`, which CueEqTests pins to the sound. */
TEST_CASE ("client: a cue's inserts are one strip per entry of the set, read and never worked out")
{
    /*  Decision AE, read from the client's side (PR 9a.9): the strips come
        from the set in chain order, whether or not the cue has an Fx for
        them; where it has, the value and the plugin's own text are the cue's
        nodes; where it has not, the value shown is the catalogue's default
        and the text is empty. The Plugins tab's two lists read beside it. */
    Rig rig ("first-sound");
    const std::string cue = "B3N8R5TW";

    rig.apply (1, "window", "plugin.create", { osc::Value::string ("Test gain"),
                                               osc::Value::string ("godot:test-gain"),
                                               osc::Value::string ("VST3"), osc::Value::string ("") });
    rig.apply (2, "window", "plugin.create", { osc::Value::string ("Verb"),
                                               osc::Value::string ("VST3-0badf00d-verb"),
                                               osc::Value::string ("VST3"), osc::Value::string ("C:/plugins/verb.vst3") });

    auto snapshot = rig.publish (3);
    const auto set = model::readPluginSet (*snapshot);
    REQUIRE (set.size() == 2u);
    CHECK (set[0].name == "Test gain");
    CHECK (set[0].identifier == "godot:test-gain");
    CHECK (set[0].state == "unloaded");
    CHECK (set[1].name == "Verb");
    CHECK (set[1].path == "C:/plugins/verb.vst3");
    const auto gainId = set[0].id;
    const auto verbId = set[1].id;

    //  Nothing switched in yet: two strips, neither present.
    auto reading = model::readFx (*snapshot, cue);
    REQUIRE (reading.present);
    CHECK (reading.notice.empty());
    REQUIRE (reading.strips.size() == 2u);
    CHECK (reading.strips[0].pluginId == gainId);
    CHECK (reading.strips[0].index == 0);
    CHECK_FALSE (reading.strips[0].present());
    CHECK_FALSE (reading.strips[0].enabled);
    CHECK (reading.strips[1].pluginId == verbId);
    CHECK (reading.strips[1].index == 1);

    //  The catalogue is the tree's business; a rig with no store sees no parameters, honestly.
    CHECK (reading.strips[1].params.empty());

    rig.apply (4, "window", "fx.create", { osc::Value::string (cue), osc::Value::string (gainId) });
    snapshot = rig.publish (5);
    reading = model::readFx (*snapshot, cue);
    REQUIRE (reading.strips.size() == 2u);
    CHECK (reading.strips[0].present());
    CHECK (reading.strips[0].enabled);
    CHECK_FALSE (reading.strips[0].fxId.empty());
    CHECK_FALSE (reading.strips[1].present());

    const auto fxId = reading.strips[0].fxId;
    CHECK (model::fxAddress (fxId, "enabled") == "/godot/fx/" + fxId + "/enabled");
    CHECK (model::fxParameterAddress (fxId, 3) == "/godot/fx/" + fxId + "/p3");

    rig.apply (6, "window", "node.set", { osc::Value::string ("/godot/fx/" + fxId + "/enabled"),
                                          osc::Value::boolean (false) });
    snapshot = rig.publish (7);
    reading = model::readFx (*snapshot, cue);
    CHECK (reading.strips[0].present());
    CHECK_FALSE (reading.strips[0].enabled);

    SUBCASE ("and the machine's known list reads beside the set, empty when no scan was run")
    {
        CHECK (model::readKnownPlugins (*snapshot).empty());
    }

    SUBCASE ("and the foot reads the chain, with the cue's own EQ as its first box")
    {
        /*  The author's chain (2026-09-25): file, EQ, the set, out. The FX
            subject fills the EQ's reading too, because the first box is the
            EQ's and its switch is `eqOn`; and it follows the pick, as the EQ
            does. An unloaded entry adds no latency - nothing is known yet. */
        const auto foot = model::readFoot (*snapshot, { model::Subject::Kind::fx, cue });
        CHECK (foot.fx.present);
        CHECK (foot.fx.strips.size() == 2u);
        CHECK (foot.eq.present);
        CHECK (foot.eq.settings.on);
        CHECK (foot.notice.empty());
        CHECK (foot.fx.strips[0].latencySamples == 0);
        CHECK (model::followsPick (model::Subject::Kind::fx));
    }

    SUBCASE ("and a cue that is not media has none, and says so")
    {
        Rig memo;
        const auto plain = memo.publish (1);
        const auto none = model::readFx (*plain, "B3N8R5TW");
        CHECK_FALSE (none.present);
        CHECK (none.notice.find ("media") != std::string::npos);
    }

    SUBCASE ("and a show with no set says so, with the strips empty")
    {
        Rig bare ("first-sound");
        const auto plain = bare.publish (1);
        const auto none = model::readFx (*plain, cue);
        CHECK (none.present);
        CHECK (none.strips.empty());
        CHECK (none.notice.find ("Plugins") != std::string::npos);
    }
}

TEST_CASE ("client: a box in the chain says what became of its plugin, and what that does to this cue")
{
    /*  §4.8 in words: the state word the engine publishes, its sentence, and
        "this cue is silent" only where this cue has the insert in - a plugin
        that is not there is not a problem for a cue that does not use it
        (and it is silence, never dry: the author's decision of 2026-09-26,
        CU). And a latency is said in samples, or not at all. */
    model::FxStrip strip;
    strip.state = "loaded";
    CHECK (model::stateSentence (strip) == "loaded");
    CHECK (model::latencyWords (strip).empty());

    strip.state = "loading";
    CHECK (model::stateSentence (strip) == "loading...");

    strip.state = "unloaded";
    strip.problem = "added since the show opened; reload to load it";
    CHECK (model::stateSentence (strip) == "not loaded: added since the show opened; reload to load it");

    strip.state = "missing";
    strip.problem = "not on this machine";
    CHECK (model::stateSentence (strip) == "missing: not on this machine");

    strip.fxId = "FX7N0001";
    strip.enabled = true;
    CHECK (model::stateSentence (strip) == "missing: not on this machine - this cue is silent while it has it switched in");

    strip.enabled = false;
    CHECK (model::stateSentence (strip) == "missing: not on this machine");

    strip.enabled = true;
    strip.state = "failed";
    strip.problem = "the plugin host process died; every voice using it is silent until it is back";
    CHECK (model::stateSentence (strip) == "failed: the plugin host process died; every voice using it is silent"
                                           " until it is back - this cue is silent until it is back");
    strip.enabled = false;

    strip.state.clear();
    strip.problem.clear();
    CHECK (model::stateSentence (strip) == "-");

    strip.latencySamples = 1;
    CHECK (model::latencyWords (strip) == "1 sample late while it is in");
    strip.latencySamples = 64;
    CHECK (model::latencyWords (strip) == "64 samples late while it is in");

    /*  AND WHY IT PLAYS THIS CUE DRY (2026-09-26), switched in and loaded. */
    strip.state = "loaded";
    strip.enabled = true;
    strip.dryWhy = "this cue is 2 channels wide here and the plugin takes 1: it plays dry";
    CHECK (model::stateSentence (strip) == "loaded - this cue is 2 channels wide here and the plugin takes 1: it plays dry");
}

TEST_CASE ("client: the chain says how wide the cue comes out through its inserts, and how late")
{
    model::FxReading reading;
    reading.fileChannels = 1;
    reading.chainChannels = 1;
    CHECK (model::chainWords (reading).empty());

    reading.chainChannels = 2;
    CHECK (model::chainWords (reading) == "Plays as stereo through its inserts.");

    reading.insertLatency = 1024;
    reading.sampleRate = 48000;
    CHECK (model::chainWords (reading) == "Plays as stereo through its inserts, 21 ms late through its inserts.");

    reading.chainChannels = 1;
    CHECK (model::chainWords (reading) == "Sounds 21 ms late through its inserts.");
}

TEST_CASE ("client: a media cue's EQ is read back as the value the voice gets, and drawn from the same maths")
{
    Rig rig ("first-sound");
    const std::string cue = "B3N8R5TW";

    rig.apply (1, "window", "node.set", { osc::Value::string ("/godot/cue/" + cue + "/eqB2Gain"),
                                          osc::Value::float64 (6.0) });
    rig.apply (2, "window", "node.set", { osc::Value::string ("/godot/cue/" + cue + "/eqHpf"),
                                          osc::Value::boolean (true) });
    rig.apply (3, "window", "node.set", { osc::Value::string ("/godot/cue/" + cue + "/eqB1Shape"),
                                          osc::Value::string ("lowShelf") });

    const auto snapshot = rig.publish (4);
    const auto eq = model::readEq (*snapshot, cue);

    REQUIRE (eq.present);
    CHECK (eq.notice.empty());
    CHECK (eq.settings.on);
    CHECK (eq.settings.hpf);
    CHECK_FALSE (eq.settings.lpf);
    CHECK (eq.settings.hpfFreq == doctest::Approx (80.0f));               // the table's default
    CHECK (eq.settings.band[1].gain == doctest::Approx (6.0f));
    CHECK (eq.settings.band[1].freq == doctest::Approx (500.0f));         // untouched, the default
    CHECK (eq.settings.band[0].shape == audio::EqSettings::Shape::lowShelf);
    CHECK (eq.settings.band[3].shape == audio::EqSettings::Shape::peak);
    CHECK_FALSE (eq.settings.isIdentity());

    /*  THE PICTURE IS THE MATHS: the drawn point nearest the band's centre
        reads the band's gain, the axis runs twenty to twenty thousand, and a
        flat EQ draws nought everywhere. */
    const auto curve = model::eqCurve (eq.settings, 48000.0, 241);
    REQUIRE (curve.size() == 241u);
    CHECK (curve.front().frequency == doctest::Approx (20.0));
    CHECK (curve.back().frequency == doctest::Approx (20000.0));

    auto nearest = curve.front();

    for (const auto& point : curve)
        if (std::abs (point.frequency - 500.0) < std::abs (nearest.frequency - 500.0))
            nearest = point;

    CHECK (nearest.db == doctest::Approx (6.0).epsilon (0.05));

    for (const auto& point : model::eqCurve (audio::EqSettings::flat(), 48000.0, 50))
        CHECK (point.db == doctest::Approx (0.0));

    //  The spellings the panel writes are the table's.
    CHECK (model::eqBandRow (1, "Gain") == "eqB2Gain");
    CHECK (model::eqAddress (cue, "eqHpf") == "/godot/cue/" + cue + "/eqHpf");
    CHECK (model::eqShapeFor ("highShelf") == audio::EqSettings::Shape::highShelf);
    CHECK (std::string (model::eqShapeWord (audio::EqSettings::Shape::lowShelf)) == "lowShelf");

    SUBCASE ("and a band switched off reads off, its gain kept, and draws flat")
    {
        /*  eqB<n>On (author, 2026-09-25): the press of a gain rotary. */
        CHECK (eq.settings.band[1].on);

        rig.apply (5, "window", "node.set", { osc::Value::string ("/godot/cue/" + cue + "/eqB2On"),
                                              osc::Value::boolean (false) });
        const auto off = model::readEq (*rig.publish (6), cue);

        CHECK_FALSE (off.settings.band[1].on);
        CHECK (off.settings.band[1].gain == doctest::Approx (6.0f));
        CHECK (off.settings.band[0].on);

        auto middle = model::eqCurve (off.settings, 48000.0, 241).front();

        for (const auto& point : model::eqCurve (off.settings, 48000.0, 241))
            if (std::abs (point.frequency - 500.0) < std::abs (middle.frequency - 500.0))
                middle = point;

        CHECK (middle.db == doctest::Approx (0.0).epsilon (0.05));
    }

    SUBCASE ("and a cue that is not media has none, and says so")
    {
        Rig memo;
        const auto plain = memo.publish (1);
        const auto none = model::readEq (*plain, "B3N8R5TW");

        CHECK_FALSE (none.present);
        CHECK (none.notice.find ("media") != std::string::npos);
    }
}

//==============================================================================
TEST_CASE ("client: closing two fingers narrows a band, on every road a pinch takes")
{
    /*  The author, 2026-09-25: "EQ peak gesture to narrow the band (higher
        Q) is inverted. Pinch widens and this feels reversed." A higher Q is
        a narrower band, as the row itself says. */

    //  Two fingers: half the distance, twice the Q; twice the distance, half.
    CHECK (model::pinchedQ (1.0, 60.0, 30.0) == doctest::Approx (2.0));
    CHECK (model::pinchedQ (1.0, 60.0, 120.0) == doctest::Approx (0.5));

    //  Within the rows' range, and a distance of nothing changes nothing.
    CHECK (model::pinchedQ (4.0, 100.0, 1.0) == doctest::Approx (model::eqQHighest));
    CHECK (model::pinchedQ (0.2, 1.0, 100.0) == doctest::Approx (model::eqQLowest));
    CHECK (model::pinchedQ (0.7, 0.0, 30.0) == doctest::Approx (0.7));

    //  A trackpad's magnify: fingers closing are a scale under one, and narrow it.
    CHECK (model::magnifiedQ (1.0, 0.5) == doctest::Approx (2.0));
    CHECK (model::magnifiedQ (1.0, 2.0) == doctest::Approx (0.5));

    /*  THE WHEEL: up narrows, as a knob turned up. A Windows touchpad's pinch
        comes as the wheel with ctrl, spreading as up - and so widens. */
    const auto click = 0.25;
    CHECK (model::turnedQ (1.0, click, false, false) > 1.2);
    CHECK (model::turnedQ (1.0, -click, false, false) < 1.0 / 1.2);
    CHECK (model::turnedQ (1.0, click, true, false) < 1.0);
    CHECK (model::turnedQ (1.0, -click, true, false) > 1.0);

    //  Shift is the fine step - 1.01 a tenth of a unit, not 1.1 - and a long turn stops at the top.
    CHECK (model::turnedQ (1.0, click, false, true) == doctest::Approx (1.025188).epsilon (1e-5));
    CHECK (model::turnedQ (9.9, 10.0, false, false) == doctest::Approx (model::eqQHighest));
}

TEST_CASE ("client: a surface's page holds the foot on the aimed cue from the press that puts it up")
{
    /*  The author, 2026-09-25: "When adjusting either EQ or send levels
        display the footer on screen." And 2026-10-05: "Pressing the Eq toggle
        on the controller does switch the rotaries to EQ, but it should open
        also the EQ footer for the selected channel for visualisation" - the
        page up is enough, before any turn (namespace draft §30.5). */
    Rig rig ("first-sound");
    surface::SurfaceTable surfaces;
    rig.parameters.setSurfaces (&surfaces);
    surface::registerSurfaceCommands (rig.engine.commands(), rig.document, surfaces);

    REQUIRE (rig.apply (1, "cli", "surface.create", { osc::Value::string ("d700") }).applied == 1);
    const auto surfaceId = model::text (*rig.publish (2), "/godot/surface/order");
    REQUIRE_FALSE (surfaceId.empty());

    const std::string cue = "B3N8R5TW";
    REQUIRE (rig.apply (3, "cli", "surface.aim", { osc::Value::string (cue) }).applied == 1);

    //  Nothing up: no page, and the foot is the window's own.
    auto page = model::readSurfacePage (*rig.publish (4));
    CHECK_FALSE (page.up);
    CHECK (page.aim == cue);
    CHECK_FALSE (model::footForSurface (page.up, page.word, page.aim).isOpen());

    //  An EQ page up, not yet turned: the aimed cue's EQ panel already.
    surfaces.setPage (surfaceId, { "eq", 0, 2, "" });
    page = model::readSurfacePage (*rig.publish (5));
    CHECK (page.up);
    CHECK (page.word == "eq");
    CHECK (page.count == 2);
    CHECK (page.edited.empty());
    CHECK (model::footForSurface (page.up, page.word, page.aim)
             == model::Subject { model::Subject::Kind::eq, cue });

    //  Turned: still the EQ panel, and the band the rotary is on.
    surfaces.setPage (surfaceId, { "eq", 0, 2, "/godot/cue/" + cue + "/eqB3Gain" });
    page = model::readSurfacePage (*rig.publish (6));
    CHECK (model::footForSurface (page.up, page.word, page.aim)
             == model::Subject { model::Subject::Kind::eq, cue });
    CHECK (model::eqHandleForAddress (cue, page.edited) == 2);
    CHECK (model::eqHandleForAddress (cue, "/godot/cue/" + cue + "/eqHpfFreq") == 4);
    CHECK (model::eqHandleForAddress (cue, "/godot/cue/" + cue + "/eqLpf") == 5);
    CHECK (model::eqHandleForAddress (cue, "/godot/cue/" + cue + "/eqOn") == -1);
    CHECK (model::eqHandleForAddress (cue, "/godot/cue/OTHERCUE/eqB1Gain") == -1);

    //  A Send page, the moment it is up: the send mixer.
    surfaces.setPage (surfaceId, { "send", 0, 1, "" });
    page = model::readSurfacePage (*rig.publish (7));
    CHECK (model::footForSurface (page.up, page.word, page.aim)
             == model::Subject { model::Subject::Kind::sends, cue });

    //  An FX page (2026-09-26), the moment it is up: the cue's chain.
    surfaces.setPage (surfaceId, { "fx", 1, 3, "" });
    page = model::readSurfacePage (*rig.publish (8));
    CHECK (page.word == "fx");
    CHECK (model::footForSurface (page.up, page.word, page.aim)
             == model::Subject { model::Subject::Kind::fx, cue });

    //  A Loop page (Phase 9c), the moment it is up: the take it rides.
    surfaces.setPage (surfaceId, { "loop", 0, 1, "" });
    page = model::readSurfacePage (*rig.publish (9));
    CHECK (page.word == "loop");
    CHECK (model::footForSurface (page.up, page.word, page.aim)
             == model::Subject { model::Subject::Kind::take, cue });

    /*  THE PAGE DOWN, the foot the window's own again (the window puts back
        what it showed, Client.cpp); and with the aim let go there is no cue
        for a page to be of. */
    surfaces.setPage (surfaceId, { "show", 0, 1, "" });
    page = model::readSurfacePage (*rig.publish (10));
    CHECK_FALSE (page.up);
    CHECK_FALSE (model::footForSurface (page.up, page.word, page.aim).isOpen());
    CHECK_FALSE (model::footForSurface (true, "eq", "").isOpen());
}

TEST_CASE ("client: a click on a number puts it on the master dial, and the window says what the dial turns")
{
    /*  The author, 2026-09-26: "Can selecting a parameter in the inspector or
        foot panel on-screen via mouse or touch assign it to the master rotary
        encoder on the D700?" - any click or touch, and it stays on that cue. */
    Rig rig ("first-sound");
    surface::SurfaceTable surfaces;
    rig.parameters.setSurfaces (&surfaces);
    surface::registerSurfaceCommands (rig.engine.commands(), rig.document, surfaces);

    const std::string cue = "B3N8R5TW";

    //  A show with no Mackie and no D700 has no dial, so a click sends nothing.
    CHECK_FALSE (model::hasMasterDial (*rig.publish (1)));

    REQUIRE (rig.apply (2, "cli", "surface.create", { osc::Value::string ("virtual") }).applied == 1);
    CHECK_FALSE (model::hasMasterDial (*rig.publish (3)));

    REQUIRE (rig.apply (4, "cli", "surface.create", { osc::Value::string ("d700") }).applied == 1);
    CHECK (model::hasMasterDial (*rig.publish (5)));

    /*  WHICH FIELDS A CLICK PUTS ON IT: a number a hand decides - not a name,
        a switch, a menu, or a reading. */
    const auto inspection = model::inspect (*rig.publish (6), cue);

    const auto fieldNamed = [&inspection] (const std::string& name) -> const model::Field*
    {
        for (const auto& block : inspection.blocks)
            for (const auto& field : block.fields)
                if (field.name == name)
                    return &field;

        for (const auto& field : inspection.details)
            if (field.name == name)
                return &field;

        return nullptr;
    };

    for (const auto* name : { "level", "preWait", "startOffset" })
    {
        INFO (std::string (name));
        const auto* field = fieldNamed (name);
        REQUIRE (field != nullptr);
        CHECK (model::mayDial (*field));
    }

    for (const auto* name : { "name", "enabled", "kind" })
    {
        INFO (std::string (name));
        const auto* field = fieldNamed (name);
        REQUIRE (field != nullptr);
        CHECK_FALSE (model::mayDial (*field));
    }

    //  Free, the window says nothing.
    CHECK (model::dialLine (*rig.publish (7)).empty());
    CHECK (model::readTransport (*rig.publish (7)).dial.empty());

    //  On the cue's level: its name, the row, the value and the unit.
    const auto level = fieldNamed ("level")->address;
    REQUIRE (rig.apply (8, "window", "surface.dial", { osc::Value::string (level) }).applied == 1);

    const auto snapshot = rig.publish (9);
    const auto name = model::text (*snapshot, "/godot/cue/" + cue + "/name");
    const auto line = model::dialLine (*snapshot);

    CHECK (line.rfind (name + ": level ", 0) == 0);
    CHECK (line.size() > 3);
    CHECK (line.substr (line.size() - 3) == " dB");
    CHECK (model::readTransport (*snapshot).dial == line);

    //  A row the inspector renames keeps the inspector's words.
    REQUIRE (rig.apply (10, "window", "surface.dial",
                        { osc::Value::string ("/godot/cue/" + cue + "/initialLevel") }).applied == 1);
    CHECK (model::dialLine (*rig.publish (11)).rfind (name + ": initial level ", 0) == 0);
}

TEST_CASE ("client: the input list reads the named inputs, names the patch rows, and says which regime it is in")
{
    /*  Phase 9b (namespace draft §18.2): the output list's twin, made through
        the same commands a window sends, so the rows are what a client would
        read off a real engine. */
    Rig rig;

    REQUIRE (rig.apply (1, "window", "input.create",
                        { osc::Value::int32 (1), osc::Value::int32 (-1), osc::Value::string ("N1000001") }).applied == 1);
    REQUIRE (rig.apply (2, "window", "input.create",
                        { osc::Value::int32 (2), osc::Value::int32 (-1), osc::Value::string ("N1000002") }).applied == 1);
    REQUIRE (rig.apply (3, "window", "node.set",
                        { osc::Value::string ("/godot/input/N1000001/name"), osc::Value::string ("Voix solo") }).applied == 1);

    const auto snapshot = rig.publish (3);
    const auto rows = model::readInputs (*snapshot);

    REQUIRE (rows.size() == 2);
    CHECK (rows[0].name == "Voix solo");
    CHECK (rows[0].widthWord() == "Mono");
    CHECK (rows[1].widthWord() == "Stereo");
    CHECK (rows[0].channelWord() == "1");
    CHECK (rows[1].channelWord() == "2-3");
    CHECK (model::inputChannelCount (rows) == 3);

    /*  No interface in this rig: every input says so in words, where the
        window would draw its meter. */
    CHECK (rows[0].problem == "no input interface is open");
    CHECK (rows[0].meterFill() == doctest::Approx (0.0));

    const auto labels = model::inputChannelLabels (rows, 0);
    REQUIRE (labels.size() == 3);
    CHECK (labels[0] == "Voix solo");
    CHECK (labels[1] == "Input 2 \xc2\xb7 L");
    CHECK (labels[2] == "Input 2 \xc2\xb7 R");
    CHECK (model::inputChannelLabels (rows, 5).back() == "Input 5");

    CHECK_FALSE (model::inputPatchHasSettled (*snapshot));
    CHECK (model::inputRegime (false).find ("follows this list") != std::string::npos);

    /*  A meter reads in fills from nought to one: -60 dB and below is dark. */
    model::InputRow loud;
    loud.meterDb = -6.0;
    CHECK (loud.meterFill() == doctest::Approx (0.9));
}

TEST_CASE ("client: a mic cue is inspected by its input and its channel, and its FX are its channel's")
{
    /*  Phase 9b (namespace draft 18.9): the `mic` fixture's cue MC000002,
        "Voix solo" through Vox 1 with the channel's test gain switched in.
        What the window reads of one before it makes a sound: two menus of
        what the show declares where a media cue has its file, a media cue's
        outputs, EQ and FX openers and no waveform, the FX panel's strips
        from the channel's chain, and an EQ like a media cue's. */
    Rig rig ("mic");
    const auto snapshot = rig.publish (1);

    const auto panel = model::inspect (*snapshot, "MC000002");
    REQUIRE_FALSE (panel.empty());
    CHECK (panel.kind == "mic");

    const auto fieldNamed = [&panel] (const std::string& name) -> const model::Field*
    {
        for (const auto& block : panel.blocks)
            for (const auto& field : block.fields)
                if (field.name == name)
                    return &field;

        return nullptr;
    };

    const auto* input = fieldNamed ("input");
    REQUIRE (input != nullptr);
    CHECK (input->control == model::Control::inputRef);
    CHECK (input->value == "MC000021");
    REQUIRE (input->choices.size() == 3u);
    CHECK (input->choices[0] == std::pair<std::string, std::string> { "", "(none)" });
    CHECK (input->choices[1].first == "MC000021");
    CHECK (input->choices[1].second == "Voix solo \xc2\xb7 Mono, input 1");
    CHECK (input->choices[2].second == "Keys \xc2\xb7 Stereo, inputs 2-3");

    const auto* channel = fieldNamed ("channel");
    REQUIRE (channel != nullptr);
    CHECK (channel->control == model::Control::channelRef);
    REQUIRE (channel->choices.size() == 2u);
    CHECK (channel->choices[1] == std::pair<std::string, std::string> { "MC000011", "Vox 1 \xc2\xb7 Mono to stereo" });

    //  A media cue's outputs, and its fade-in.
    REQUIRE (fieldNamed ("directOut") != nullptr);
    CHECK (fieldNamed ("directOut")->control == model::Control::busRef);
    CHECK (fieldNamed ("fadeIn") != nullptr);
    CHECK (fieldNamed ("level") != nullptr);

    /*  No file, and the EQ's rows are the panel's, behind its opener - which
        is in the panel bar since 2026-09-30, beside the FX, the sends and the
        take, and no waveform: a live input has nothing recorded to draw. */
    CHECK (fieldNamed ("file") == nullptr);
    CHECK (fieldNamed ("eqB1Freq") == nullptr);

    const auto panelNamed = [&panel] (const std::string& subject)
    {
        return std::any_of (panel.panels.begin(), panel.panels.end(),
                            [&subject] (const model::Field& field) { return field.value == subject; });
    };

    CHECK (panelNamed ("eq"));
    CHECK (panelNamed ("fx"));
    CHECK (panelNamed ("sends"));
    CHECK (panelNamed ("take"));
    CHECK_FALSE (panelNamed ("waveform"));

    /*  ITS FX ARE ITS CHANNEL'S: one strip, the channel's test gain, the
        fixture's Fx switched in - and the set, which is empty, is not asked. */
    const auto fx = model::readFx (*snapshot, "MC000002");
    REQUIRE (fx.present);
    CHECK (fx.notice.empty());
    REQUIRE (fx.strips.size() == 1u);
    CHECK (fx.strips[0].pluginId == "MC000012");
    CHECK (fx.strips[0].name == "Test gain");
    CHECK (fx.strips[0].fxId == "MC000005");
    CHECK (fx.strips[0].enabled);

    //  An EQ like a media cue's.
    CHECK (model::readEq (*snapshot, "MC000002").present);

    //  Its sends are a sound's too: the foot does not turn a mic cue away.
    model::Subject sends;
    sends.kind = model::Subject::Kind::sends;
    sends.objectId = "MC000002";
    CHECK (model::readFoot (*snapshot, sends).notice.find ("Only a media") == std::string::npos);

    /*  A MIC CUE THROUGH NO CHANNEL says so where its strips would be. */
    REQUIRE (rig.apply (2, "window", "node.set", { osc::Value::string ("/godot/cue/MC000002/channel"),
                                                  osc::Value::string ("") }).applied == 1);
    const auto none = model::readFx (*rig.publish (3), "MC000002");
    CHECK (none.present);
    CHECK (none.strips.empty());
    CHECK (none.notice == "This mic cue plays through no rack channel yet: pick one in the inspector.");
}

TEST_CASE ("client: a mic cue's chain starts at its input, its path is said against the budget, and its run by its channel")
{
    /*  Phase 9b, stage 9b.7 (namespace draft 18.7 and 18.9): what the window
        says of a mic cue that a media cue's words do not cover. */
    Rig rig ("mic");
    auto snapshot = rig.publish (1);

    /*  THE CHAIN BEGINS AT THE INPUT, by its name, as wide as it is; and the
        path is always said - here with no plugin loaded and an interface
        with no delay of its own. */
    const auto fx = model::readFx (*snapshot, "MC000002");
    REQUIRE (fx.present);
    CHECK (fx.live);
    CHECK (fx.source == "in \xc2\xb7 Voix solo");
    CHECK (fx.fileChannels == 1);
    CHECK (fx.budgetMs == doctest::Approx (5.0));
    CHECK (model::chainWords (fx) == "0 ms from the microphone to the output: 0 ms the interface's, 0 ms its"
                                     " plugins' - within the 5 ms budget.");

    /*  And as the words read with real delays: 120 samples of interface and
        230 of plugins at 48 kHz, then the plugins over the budget. */
    auto path = fx;
    path.sampleRate = 48000;
    path.inputLatency = 64;
    path.outputLatency = 56;
    path.insertLatency = 230;
    CHECK (model::chainWords (path) == "7.3 ms from the microphone to the output: 2.5 ms the interface's,"
                                       " 4.8 ms its plugins' - within the 5 ms budget.");

    path.insertLatency = 300;
    CHECK (model::chainWords (path).ends_with ("6.3 ms its plugins' - over the 5 ms budget."));

    //  A media cue's source is its file, as it always was.
    CHECK (model::FxReading {}.source == "file");

    /*  THE PLUGIN'S OWN WINDOW FOLLOWS THE PICK onto a mic cue whose channel
        carries the plugin. */
    const auto editor = model::readEditorSubject (*snapshot, "MC000002", "MC000012");
    CHECK_FALSE (editor.greyed);
    CHECK (editor.fxId == "MC000005");

    /*  A MIC RUN SAYS WHICH CHANNEL IT IS ON, OR WHY NOT: on it while it
        plays, waiting for it while another cue holds it, ringing out after its
        stop. */
    rig.runs.create ("MICRUN01", "MC000002", "mic");
    auto* run = rig.runs.find ("MICRUN01");
    REQUIRE (run != nullptr);

    const auto wordsOf = [&rig] (std::int64_t tick)
    {
        for (const auto& row : model::readRuns (*rig.publish (tick)))
            if (row.id == "MICRUN01")
                return row.liveWords;

        return std::string ("(no row)");
    };

    run->state = cue::runState::playing;
    CHECK (wordsOf (2) == "on Vox 1");

    run->pending = { "MC000011" };
    CHECK (wordsOf (3) == "waiting for Vox 1");

    run->pending.clear();
    run->state = cue::runState::stopping;
    CHECK (wordsOf (4) == "ringing out");

    /*  LOAD NOW SAYS WHAT KEEPS IT WAITING, by name - and a cue only got ready
        ahead is not a sound. */
    run->state = cue::runState::playing;
    CHECK (model::busyWords (*rig.publish (5)) == "Voix solo is sounding");

    run->state = cue::runState::armed;
    run->prepare = "armed";
    CHECK (model::busyWords (*rig.publish (6)).empty());
}

TEST_CASE ("client: the rack reads each channel with its own chain, and says the worst case against the budget")
{
    /*  Phase 9b (namespace draft §18.3): the Rack tab's reading, made through
        the commands the tab sends - the tab's own gestures for the creates, a
        rename, a class and a budget by `node.set`, a reorder by `object.move`
        within the channel, a preset named as the Plugins tab names one - so
        the rows are what a client would read off a real engine. */
    using V = osc::Value;
    Rig rig;

    const auto send = [&rig] (std::int64_t tick, const Event& event)
    {
        INFO (event.command);
        REQUIRE (rig.apply (tick, event.origin, event.command, event.args).applied == 1);
    };

    REQUIRE (rig.apply (1, "window", "channel.create", { V::string ("mono"), V::string ("K1000001") }).applied == 1);
    send (2, gesture::createRackChannel ("stereo"));
    send (3, gesture::setNode ("/godot/slot/K1000001/name", "Vox 1"));

    for (const auto* name : { "Gain A", "Gain B" })
        send (4, gesture::createChannelPlugin ("K1000001", name, "godot:test-gain", "VST3", ""));

    auto snapshot = rig.publish (5);
    auto rack = model::readRack (*snapshot);

    REQUIRE (rack.channels.size() == 2u);
    CHECK (rack.channels[0].id == "K1000001");
    CHECK (rack.channels[0].name == "Vox 1");
    CHECK (rack.channels[0].classWord() == "Mono");
    CHECK (rack.channels[0].chainWord() == "2 plugins");
    REQUIRE (rack.channels[0].chain.size() == 2u);
    CHECK (rack.channels[0].chain[0].name == "Gain A");
    CHECK (rack.channels[0].chain[1].name == "Gain B");
    CHECK (rack.channels[0].chain[0].identifier == "godot:test-gain");
    CHECK (rack.channels[0].chain[0].state == "unloaded");
    CHECK (rack.channels[0].latencySamples == 0);

    /*  A channel nobody named reads by its place among the rack's. */
    CHECK (rack.channels[1].name == "Channel 2");
    CHECK (rack.channels[1].classWord() == "Stereo");
    CHECK (rack.channels[1].chainWord() == "No plugins");

    CHECK (rack.budgetMs == doctest::Approx (5.0));
    CHECK (rack.sampleRate == 48000);

    /*  A RACK'S PLUGINS ARE NEVER THE SET'S: the set's order is the chain on
        every voice, and a channel's plugins are on its track alone. */
    CHECK (model::readPluginSet (*snapshot).empty());

    const auto gainA = rack.channels[0].chain[0].id;
    const auto gainB = rack.channels[0].chain[1].id;

    send (6, gesture::moveObject (gainB, "K1000001", 0));
    send (7, gesture::setNode ("/godot/plugin/" + gainA + "/preset", "room.vstpreset"));
    send (8, gesture::setNode ("/godot/slot/K1000001/class", "monoToStereo"));
    send (9, gesture::setNode ("/godot/audio/rackBudget", "2.5"));

    snapshot = rig.publish (10);
    rack = model::readRack (*snapshot);

    REQUIRE (rack.channels.size() == 2u);
    REQUIRE (rack.channels[0].chain.size() == 2u);
    CHECK (rack.channels[0].chain[0].id == gainB);
    CHECK (rack.channels[0].chain[1].id == gainA);
    CHECK (rack.channels[0].chain[1].preset == "room.vstpreset");
    CHECK (rack.channels[0].classWord() == "Mono to stereo");
    CHECK (rack.budgetMs == doctest::Approx (2.5));

    /*  THE WORDS. Nothing loads in a rig with no audio graph, and nothing
        loaded is nothing known - which is said, and not read as no delay. */
    CHECK (model::budgetWords (rack.channels[0], rack)
             == "No plugin has loaded yet, so none has said how late it makes the channel.");
    CHECK (model::budgetWords (rack.channels[1], rack) == "No plugins: the channel adds no delay.");

    auto closed = rack;
    closed.sampleRate = 0;
    CHECK (model::budgetWords (closed.channels[0], closed)
             == "The plugins load when the audio opens, and say then how late they make the channel.");

    /*  And as the engine gives them once the plugins have loaded: the sum it
        publishes, in milliseconds at the rate, against the budget. */
    auto vox = rack.channels[0];

    for (auto& entry : vox.chain)
        entry.state = "loaded";

    vox.latencySamples = 0;
    CHECK (model::budgetWords (vox, rack) == "Adds no delay, with every plugin in.");

    vox.latencySamples = 96;
    CHECK_FALSE (model::overBudget (vox, rack));
    CHECK (model::budgetWords (vox, rack) == "2 ms at worst, with every plugin in - within the 2.5 ms budget.");

    vox.latencySamples = 350;
    CHECK (model::overBudget (vox, rack));
    CHECK (model::budgetWords (vox, rack)
             == "7.3 ms at worst, with every plugin in - over the 2.5 ms budget. A mic cue that switches"
                " them all in says so, and plays.");

    /*  A plugin that failed has declared nothing, and the sentence names it. */
    vox.chain[1].state = "failed";
    CHECK (model::budgetWords (vox, rack).ends_with (" Not counted, not loaded: Gain A."));

    /*  With no audio open, samples - and nothing is over a budget in ms. */
    auto silent = rack;
    silent.sampleRate = 0;
    vox.chain[1].state = "loaded";
    CHECK_FALSE (model::overBudget (vox, silent));
    CHECK (model::budgetWords (vox, silent) == "350 samples at worst, with every plugin in.");

    CHECK (model::millisecondWords (5.0) == "5 ms");
    CHECK (model::millisecondWords (7.2916) == "7.3 ms");
    CHECK (model::millisecondWords (0.04) == "0 ms");

    /*  A CHANNEL TAKES ITS CHAIN WITH IT when it goes, and an undo brings both
        back. The rig cuts no transactions - serve's hook does - so the delete
        is given its own, as a window's command would be. */
    rig.document.beginTransaction ("object.delete", 11, "window", {});
    send (11, gesture::deleteObject ("K1000001"));
    snapshot = rig.publish (12);
    CHECK (model::readRack (*snapshot).channels.size() == 1u);
    CHECK (snapshot->find ("/godot/plugin/" + gainA + "/name") == nullptr);

    send (13, gesture::undo());
    snapshot = rig.publish (14);
    rack = model::readRack (*snapshot);
    REQUIRE (rack.channels.size() == 2u);
    CHECK (rack.channels[0].chain.size() == 2u);
}

TEST_CASE ("client: a sampling channel says its recorder, what it sets aside, and which side of it each plugin is on")
{
    /*  Phase 9c, stage 9c.2 (namespace draft §19.2): the recorder's rows
        written as the Rack tab writes them - the take's length and its layers
        on the channel, a plugin's side - and read back into the tab's words. */
    using V = osc::Value;
    Rig rig;

    const auto send = [&rig] (std::int64_t tick, const Event& event)
    {
        INFO (event.command);
        REQUIRE (rig.apply (tick, event.origin, event.command, event.args).applied == 1);
    };

    REQUIRE (rig.apply (1, "window", "channel.create", { V::string ("mono"), V::string ("K1000001") }).applied == 1);
    send (2, gesture::setNode ("/godot/slot/K1000001/name", "Loops"));
    send (3, gesture::createChannelPlugin ("K1000001", "Gain A", "godot:test-gain", "VST3", ""));

    auto rack = model::readRack (*rig.publish (4));
    REQUIRE (rack.channels.size() == 1u);
    CHECK_FALSE (rack.channels[0].samples());
    CHECK (rack.channels[0].takeWord() == "No recorder");
    CHECK (model::takeWords (rack.channels[0], rack).empty());
    REQUIRE (rack.channels[0].chain.size() == 1u);
    CHECK (rack.channels[0].chain[0].side == "after");

    const auto gain = rack.channels[0].chain[0].id;
    send (5, gesture::setNode ("/godot/slot/K1000001/takeSeconds", "60"));
    send (6, gesture::setNode ("/godot/slot/K1000001/layers", "4"));
    send (7, gesture::setNode ("/godot/plugin/" + gain + "/side", "before"));

    rack = model::readRack (*rig.publish (8));
    auto loops = rack.channels[0];
    CHECK (loops.samples());
    CHECK (loops.takeSeconds == doctest::Approx (60.0));
    CHECK (loops.layers == 4);
    CHECK (loops.takeWord() == "Take 1 min, 4 layers");
    CHECK (loops.chain[0].side == "before");

    /*  NOTHING SET ASIDE YET - a rig has no graph - so what Load now will set
        aside, at the engine's rate: five passes of a minute and a crossfade,
        two channels, four bytes a sample. */
    CHECK (model::takeWords (loops, rack)
             == "It records up to 1 min with 4 layers on top: 115.2 MB to set aside at Load now.");

    loops.takeMemoryMb = 115.2;
    CHECK (model::takeWords (loops, rack) == "It records up to 1 min with 4 layers on top: 115.2 MB set aside.");

    loops.takeProblem = "the take was cleared: the interface's rate changed";
    CHECK (model::takeWords (loops, rack).ends_with (" set aside. The take was cleared: the interface's rate changed."));

    auto closed = rack;
    closed.sampleRate = 0;
    CHECK (model::takeWords (closed.channels[0], closed)
             == "It records up to 1 min with 4 layers on top: about 115.2 MB at 48 kHz, set aside when the audio opens.");

    //  The schema's own words for what a row may hold.
    CHECK (rig.apply (9, "window", "node.set", { V::string ("/godot/slot/K1000001/takeSeconds"), V::string ("900") }).applied == 0);
    CHECK (rig.apply (10, "window", "node.set", { V::string ("/godot/plugin/" + gain + "/side"), V::string ("sideways") }).applied == 0);

    CHECK (model::secondsWords (10) == "10 s");
    CHECK (model::secondsWords (90) == "1 min 30 s");
    CHECK (model::secondsWords (600) == "10 min");
}

//==============================================================================
TEST_CASE ("client: a mic cue's take is read off its channel's rows, said in words, with why Rec is not offered")
{
    /*  Phase 9c, stage 9c.4 (namespace draft 19.7): the take panel's reading,
        from the `take` fixture with the engine's account of the take behind
        the tree - the same table the take verbs move. */
    Rig rig ("take");
    cue::TakeTable takes;
    rig.parameters.setTakes (&takes);

    /*  PRESSED HERE BY HAND, so the tree is told to look again as every
        applied command tells it in serve: the account only moves inside a
        take verb, `take.closed` or a release, and each of those applies. */
    const auto readNow = [&rig] (std::int64_t tick, const std::string& cueId)
    {
        rig.parameters.markStale();
        return model::readTake (*rig.publish (tick), cueId);
    };

    //  Only a mic cue has one: a transport cue says so.
    const auto transport = readNow (1, "TK000006");
    CHECK_FALSE (transport.present);
    CHECK (transport.notice == "Only a mic cue on a sampling channel has a take.");

    auto reading = readNow (2, "TK000002");
    REQUIRE (reading.present);
    CHECK (reading.channelId == "TK000011");
    CHECK (reading.channelName == "Looper");
    CHECK (reading.state == "empty");
    CHECK (reading.capacity == doctest::Approx (10.0));
    CHECK (reading.maxLayers == 2);
    CHECK (reading.onGo == "wait");
    CHECK_FALSE (reading.through);
    CHECK (reading.span() == doctest::Approx (10.0));
    CHECK (model::takePanelWords (reading) == "Empty: Rec records up to 10 s.");
    CHECK (model::takePressWhy (reading) == "Nothing sounds on Looper: GO a mic cue on it to record.");

    //  Scene 5's own rows: its GO loops what it finds, and it hears its input through.
    const auto scene = readNow (3, "TK000008");
    CHECK (scene.onGo == "loop");
    CHECK (scene.through);
    CHECK (scene.channelId == "TK000011");

    //  A mic cue sounding on the channel: Rec is offered, and the take records.
    rig.runs.create ("MICRUN01", "TK000002", "mic");
    auto* run = rig.runs.find ("MICRUN01");
    REQUIRE (run != nullptr);
    run->claims = { "TK000011" };
    run->state = cue::runState::playing;

    takes.press ("TK000011", cue::TakeVerb::record, 2);
    reading = readNow (4, "TK000002");
    CHECK (reading.state == "recording");
    CHECK (reading.channelSounds);
    CHECK (reading.holderName == "Loop voice");
    CHECK (model::takePressWhy (reading).empty());
    CHECK (model::takePanelWords (reading) == "Recording, up to 10 s: Rec again loops it.");

    //  Closed by the recorder at 4.25 s: looping between the take's two ends, the playhead going round.
    takes.press ("TK000011", cue::TakeVerb::record, 2);
    takes.closed ("TK000011", 4.25, "pressed", 10.0);
    takes.setPlayhead ("TK000011", 1.5);
    reading = readNow (5, "TK000002");
    CHECK (reading.state == "looping");
    CHECK (reading.length == doctest::Approx (4.25));
    CHECK (reading.loopIn == doctest::Approx (0.0));
    CHECK (reading.loopOut == doctest::Approx (4.25));
    CHECK (reading.playhead == doctest::Approx (1.5));
    CHECK (reading.span() == doctest::Approx (4.25));
    CHECK (model::takePanelWords (reading) == "Looping 4.3 s, 0 of 2 layers on it.");

    //  Both layers in use: section 19.3's sentence, which no refusal says where a hand would see it.
    for (int layer = 0; layer < 2; ++layer)
    {
        takes.press ("TK000011", cue::TakeVerb::overdub, 2);
        takes.press ("TK000011", cue::TakeVerb::loop, 2);
    }

    reading = readNow (6, "TK000002");
    CHECK (reading.layers == 2);
    CHECK (reading.layersFull());
    CHECK (model::takePanelWords (reading)
             == "Looping 4.3 s, 2 of 2 layers on it. Looper holds its 2 layers: Undo one or Clear.");

    //  The cue let go: held, and nothing sounding to press it through.
    takes.release ("TK000011");
    run->claims.clear();
    run->state = cue::runState::done;
    reading = readNow (7, "TK000002");
    CHECK (reading.state == "held");
    CHECK_FALSE (reading.channelSounds);
    CHECK (model::takePanelWords (reading).starts_with ("Held: 4.3 s, silent until Rec or Loop."));

    //  The door's address, and a tenth of a second spelled the same in either locale.
    CHECK (model::loopPointAddress ("TK000011", true) == "/godot/slot/TK000011/loopIn");
    CHECK (model::loopPointAddress ("TK000011", false) == "/godot/slot/TK000011/loopOut");
    CHECK (model::tenthsOfSeconds (4.25) == "4.3 s");
    CHECK (model::tenthsOfSeconds (0.04) == "0.0 s");

    //  The foot opens on it by its own word, and it follows the pick.
    const auto foot = model::readFoot (*rig.publish (8), { model::Subject::Kind::take, "TK000002" });
    CHECK (foot.take.present);
    CHECK (foot.notice.empty());
    CHECK (model::followsPick (model::Subject::Kind::take));

    //  And the gesture is the take verb of its name, from the window.
    const auto press = gesture::takePress ("record", "TK000011");
    CHECK (press.command == "take.record");
    CHECK (press.origin == "window");
    REQUIRE (press.args.size() == 1u);
    CHECK (press.args[0].getString() == "TK000011");

    /*  KEEP (9c.6): writing, then written, said in the sentence - and while
        it writes, another Keep is not offered. */
    takes.keep ({ "TK000011", "Looper", false, {}, 0.0, 4.25 });
    reading = readNow (30, "TK000002");
    CHECK (reading.keeping);
    CHECK_FALSE (reading.mayKeep());
    CHECK (model::takePanelWords (reading).ends_with ("Keeping it as a file..."));

    takes.kept ("TK000011", "takes/Looper take 1.wav", "");
    reading = readNow (31, "TK000002");
    CHECK_FALSE (reading.keeping);
    CHECK (reading.kept == "takes/Looper take 1.wav");
    CHECK (reading.mayKeep());
    CHECK (model::takePanelWords (reading).ends_with ("Kept as takes/Looper take 1.wav."));
    CHECK_FALSE (reading.locked);

    const auto keep = gesture::takeKeep ("TK000011", true, "TK000002");
    CHECK (keep.command == "take.keep");
    REQUIRE (keep.args.size() == 3u);
    CHECK (keep.args[0].getString() == "TK000011");
    CHECK (keep.args[1].getBool());
    CHECK (keep.args[2].getString() == "TK000002");
}

//==============================================================================
/*  DOH! IN THE DESKTOP CLIENT (PRD §3.32, D1, 2026-10-01): its refusals are
    sentences on the transport's line, its button names the GO it would take
    back while the window is open, its step reads as a word in the history, and
    its setting is read off the rows of the devices and the cues. Each failed
    before D1: no such words, no gesture, no rows. */
TEST_CASE ("client: Doh!'s refusals are sentences, its step is a word, and its button names the GO it would take back")
{
    model::TransportReading reading;
    reading.dohWindow = "10";

    reading.lastError = "5500 27 window too-late go.doh";
    CHECK (reading.errorLine() == "Doh! ignored: the last GO is too long ago to take back (Show settings > Playback)");

    reading.dohWindow = "0";
    CHECK (reading.errorLine() == "Doh! is off (Show settings > Playback)");
    reading.dohWindow = "10";

    reading.lastError = "5500 27 window nothing-to-take-back go.doh";
    CHECK (reading.errorLine() == "Doh! ignored: there is no GO it can take back");

    //  The author's own sentence.
    reading.lastError = "5500 27 window trigger-after-go go.doh";
    CHECK (reading.errorLine() == "Doh! ignored: a trigger fired after the last GO");

    reading.lastError = "5500 27 window too-soon go.doh";
    CHECK (reading.errorLine() == "Doh! ignored: pressed again too soon (Show settings > Playback)");

    //  The GO's own bounce keeps its words.
    reading.lastError = "5500 27 window too-soon go";
    CHECK (reading.errorLine() == "GO ignored: too soon after the last one (Show settings > Playback)");

    //  One named command, nothing else (§4.11).
    const auto doh = gesture::doh();
    CHECK (doh.command == "go.doh");
    CHECK (doh.args.empty());

    /*  THE BUTTON NAMES THE CUE while the GO is still inside the window, and is
        plain "Doh!" otherwise - read off the engine's tick, never a clock of
        this window's own. */
    reading.tick = "1100";
    reading.doh = "7K2QM9X4 B3N8R5TW 1000";
    reading.dohCue = "12";
    CHECK (reading.dohCaption() == "Doh! 12");

    reading.tick = "1500";
    CHECK (reading.dohCaption() == "Doh!");

    reading.tick = "1100";
    reading.doh.clear();
    CHECK (reading.dohCaption() == "Doh!");

    //  Its step in the history, and a press's.
    CHECK (model::originWord ('d') == "Doh!");
    CHECK (model::originWord ('p') == "press");

    /*  AND A DOH! IS NOT A FIRING: the aimed cue's clock is its latest GO, not
        the step that took a GO back. */
    const auto steps = model::readHistory ("900:A1:d 600:B2:g 100:A1:g");
    const auto lines = model::stepsUnder (steps, "A1", 600);
    REQUIRE (lines.size() == 1u);
    CHECK (lines[0].cue == "B2");
    CHECK (lines[0].offset == doctest::Approx (10.0));
}

/*  THE DOH! BUTTON'S OWN COLOUR, FADING AS ITS WINDOW RUNS OUT (the author,
    2026-10-02: "I would display the button in a distinctive colour and fade
    out when the Doh! timer is over"; K7). Open, in its colour, while the last
    GO can still be taken back; fading over the window's last two seconds - half
    the window when it is shorter than four - and over, the idle look and no
    click, from the tick a press would start being refused. All of it read off
    the engine's tick, the GO's tick and the show's window: never a clock of
    this window's own. Failed before K7: the reading had no such answer. */
TEST_CASE ("client: the Doh! button is open, fades as its window runs out, and is over on the engine's tick")
{
    model::TransportReading reading;
    reading.dohWindow = "10";
    reading.doh = "7K2QM9X4 B3N8R5TW 1000";
    reading.dohCue = "12";

    const auto at = [&reading] (const char* tick)
    {
        reading.tick = tick;
        return reading.dohLook();
    };

    //  Nothing to take back: over, and the button plain.
    {
        model::TransportReading none;
        none.tick = "1100";
        CHECK (none.dohLook().phase == model::DohPhase::over);
        CHECK (none.dohLook().strength == doctest::Approx (0.0));
        CHECK (none.dohTip().find ("left") == std::string::npos);
    }

    //  The GO's own tick: open, the colour whole, ten seconds left.
    auto look = at ("1000");
    CHECK (look.phase == model::DohPhase::open);
    CHECK (look.strength == doctest::Approx (1.0));
    CHECK (look.secondsLeft == 10);
    CHECK (reading.dohTip().find ("10 s left") != std::string::npos);

    //  Two seconds before the end, still whole; one tick later, fading.
    look = at ("1400");
    CHECK (look.phase == model::DohPhase::open);
    CHECK (look.strength == doctest::Approx (1.0));
    CHECK (look.secondsLeft == 2);

    look = at ("1401");
    CHECK (look.phase == model::DohPhase::fading);
    CHECK (look.strength == doctest::Approx (0.99));

    look = at ("1450");
    CHECK (look.phase == model::DohPhase::fading);
    CHECK (look.strength == doctest::Approx (0.5));
    CHECK (look.secondsLeft == 1);
    CHECK (reading.dohCaption() == "Doh! 12");

    //  The last tick inside the window: almost idle, still a Doh!.
    look = at ("1499");
    CHECK (look.phase == model::DohPhase::fading);
    CHECK (look.strength == doctest::Approx (0.01));
    CHECK (reading.dohCaption() == "Doh! 12");

    //  The tick a press is refused: over, the idle look, no seconds.
    look = at ("1500");
    CHECK (look.phase == model::DohPhase::over);
    CHECK (look.strength == doctest::Approx (0.0));
    CHECK (look.secondsLeft == 0);
    CHECK (reading.dohCaption() == "Doh!");
    CHECK (reading.dohTip().find ("left") == std::string::npos);

    //  A one-second window fades over its second half.
    reading.dohWindow = "1";
    CHECK (at ("1024").phase == model::DohPhase::open);
    CHECK (at ("1025").phase == model::DohPhase::open);
    CHECK (at ("1026").phase == model::DohPhase::fading);
    CHECK (at ("1026").strength == doctest::Approx (24.0 / 25.0));
    CHECK (at ("1049").phase == model::DohPhase::fading);
    CHECK (at ("1050").phase == model::DohPhase::over);

    //  Doh! switched off: always over.
    reading.dohWindow = "0";
    CHECK (at ("1000").phase == model::DohPhase::over);

    //  A tick older than the GO's - a reading behind the record - is not open.
    reading.dohWindow = "10";
    CHECK (at ("999").phase == model::DohPhase::over);

    /*  AND ITS COLOUR IS ITS OWN: a token of the theme, neither GO's yellow,
        PANIC's red, the standby's amber nor the grey of a GO with no audio. */
    const model::Theme theme;
    const auto& names = model::Theme::colourNames();
    CHECK (std::find (names.begin(), names.end(), "doh") != names.end());

    for (const auto* other : { "go", "go-idle", "failed", "standby" })
        CHECK_MESSAGE (theme.colour ("doh") != theme.colour (other), other);
}

TEST_CASE ("client: Doh!'s choice on an OSC or a MIDI cue says what its device says, and writes the cue's own row")
{
    Rig rig;

    const auto dohField = [&rig] (std::int64_t tick, const std::string& cue, const char* last)
    {
        const auto inspection = model::inspect (*rig.publish (tick), cue);
        model::Field found;
        auto seenLast = false;
        auto afterLast = false;

        for (const auto& block : inspection.blocks)
            for (const auto& field : block.fields)
            {
                if (field.name == last)
                    seenLast = true;

                if (field.name == "doh")
                {
                    found = field;
                    afterLast = seenLast;
                }
            }

        CHECK (afterLast);
        return found;
    };

    const std::string light = "N4T9B2QF";
    rig.apply (1, "window", "cue.create",
               { osc::Value::string ("7K2QM9X4"), osc::Value::int32 (0),
                 osc::Value::string ("osc"), osc::Value::string ("Desk go"),
                 osc::Value::string (light) });
    rig.apply (2, "window", "node.set",
               { osc::Value::string ("/godot/cue/" + light + "/address"),
                 osc::Value::string ("/wfs/source/1/gain") });

    auto field = dohField (3, light, "timeout");
    CHECK (field.label == "on Doh!");
    CHECK (field.address == "/godot/cue/" + light + "/doh");
    CHECK (field.writable);
    REQUIRE (field.choices.size() == 3u);
    CHECK (field.choices[0].first == "device");
    CHECK (field.choices[0].second == "as the device (Meh)");
    CHECK (field.choices[1].first == "takeBack");
    CHECK (field.choices[1].second == "Undo(h)");
    CHECK (field.choices[2].first == "leave");
    CHECK (field.choices[2].second == "Meh");

    rig.apply (4, "window", "node.set",
               { osc::Value::string ("/godot/mount/G1JS4VWE/doh"), osc::Value::string ("takeBack") });
    CHECK (dohField (5, light, "timeout").choices[0].second == "as the device (Undo(h))");

    //  A MIDI cue the same, through its port.
    rig.apply (6, "window", "port.create", { osc::Value::string ("Keys") });
    const auto ports = model::readPorts (*rig.publish (7));
    REQUIRE (ports.size() == 1u);
    CHECK (ports[0].audible == false);
    CHECK (ports[0].doh == "leave");

    const std::string note = "M1D1C0EE";
    rig.apply (8, "window", "cue.create",
               { osc::Value::string ("7K2QM9X4"), osc::Value::int32 (0),
                 osc::Value::string ("midi"), osc::Value::string ("Note"),
                 osc::Value::string (note) });
    rig.apply (9, "window", "node.set",
               { osc::Value::string ("/godot/cue/" + note + "/port"), osc::Value::string (ports[0].id) });

    CHECK (dohField (10, note, "wait").choices[0].second == "as the device (Meh)");

    rig.apply (11, "window", "node.set",
               { osc::Value::string ("/godot/port/" + ports[0].id + "/doh"), osc::Value::string ("takeBack") });
    rig.apply (12, "window", "node.set",
               { osc::Value::string ("/godot/port/" + ports[0].id + "/audible"), osc::Value::string ("true") });
    CHECK (dohField (13, note, "wait").choices[0].second == "as the device (Undo(h))");

    const auto after = model::readPorts (*rig.publish (14));
    REQUIRE (after.size() == 1u);
    CHECK (after[0].audible);
    CHECK (after[0].doh == "takeBack");
    CHECK (after[0].dohWord() == "Undo(h)");

    //  And a device's row reads its setting in words.
    const auto devices = model::readDevices (*rig.publish (15));
    REQUIRE (devices.size() == 2u);
    CHECK (devices[0].id == "G1JS4VWE");
    CHECK (devices[0].dohWord() == "Undo(h)");
    CHECK (devices[1].dohWord() == "Meh");
}

TEST_CASE ("client: Doh!'s rollback row shows what the Doh would send - the device's command, else the previous command - and the engine agrees")
{
    /*  The author, 2026-10-03 (OV-OX): "We could have a default for the device
        that is in the editable Doh rollback field. If the device default is
        left blank then the previous message is in the field and can be edited
        too." The previous command in PLAY order - a group's header, its
        members, its footer - whichever order the file keeps the sections in:
        the footer is made first here, so a walk in file order would differ. */
    Rig rig;
    const std::string list = "7K2QM9X4";

    const auto oscCue = [&rig] (const std::string& parent, int at, const char* atom)
    {
        const auto made = rig.document.createCue (parent, at, "osc", "Desk");
        REQUIRE (made.ok);
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + made.id + "/address", "/wfs/x").ok);
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + made.id + "/value", atom).ok);
        return made.id;
    };

    const auto scene = rig.document.createCue (list, 0, "group", "Scene");
    REQUIRE (scene.ok);
    const auto footer = rig.document.createRole (scene.id, "footer");
    REQUIRE (footer.ok);
    oscCue (footer.id, 0, "i:3");
    const auto member = oscCue (scene.id, 0, "i:2");
    const auto header = rig.document.createRole (scene.id, "header");
    REQUIRE (header.ok);
    oscCue (header.id, 0, "i:1");
    const auto target = oscCue (list, 1, "i:9");

    const auto rollbackRow = [&rig, &target] (std::int64_t tick)
    {
        rig.parameters.markStale();
        const auto inspection = model::inspect (*rig.publish (tick), target);
        model::Field found;
        auto afterDoh = false, seenDoh = false;

        for (const auto& block : inspection.blocks)
            for (const auto& field : block.fields)
            {
                if (field.name == "dohRollback")
                {
                    found = field;
                    afterDoh = seenDoh;
                }

                seenDoh = field.name == "doh";
            }

        CHECK (afterDoh);
        return found;
    };

    //  The client's walk is the engine's.
    rig.parameters.markStale();
    const auto snapshot = rig.publish (1);
    CHECK (model::previousCommand (*snapshot, "osc", "G1JS4VWE", member) == "/wfs/x i:1");
    CHECK (cue::rollbackOf (rig.document, member) == "/wfs/x i:1");
    CHECK (model::previousCommand (*snapshot, "osc", "G1JS4VWE", target) == "/wfs/x i:3");
    CHECK (cue::rollbackOf (rig.document, target) == "/wfs/x i:3");

    //  Under Meh, greyed - never read - and still saying what it would be.
    auto row = rollbackRow (2);
    CHECK (row.label == "rollback");
    CHECK (row.writable);
    CHECK_FALSE (row.applies);
    CHECK (row.value.empty());
    CHECK (row.placeholder == "/wfs/x i:3");

    //  Under Undo(h), live.
    REQUIRE (rig.document.setAttribute ("/godot/mount/G1JS4VWE/doh", "takeBack").ok);
    CHECK (rollbackRow (3).applies);

    //  The device's general command stands in for the previous command.
    REQUIRE (rig.document.setAttribute ("/godot/mount/G1JS4VWE/dohRollback", "/wfs/back I").ok);
    CHECK (rollbackRow (4).placeholder == "/wfs/back I");
    CHECK (model::readDevices (*rig.publish (5))[0].dohRollback == "/wfs/back I");

    //  And the cue's own, typed, is its value: nothing stands in for it.
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + target + "/dohRollback", "/wfs/x i:8").ok);
    row = rollbackRow (6);
    CHECK (row.value == "/wfs/x i:8");
    CHECK (row.placeholder.empty());

    //  A cue that overrides to Meh greys it again, whatever its device says.
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + target + "/doh", "leave").ok);
    CHECK_FALSE (rollbackRow (7).applies);
}

TEST_CASE ("client: the Doh! button names the GO the engine would take back, and stops naming it once a Doh, a fire by name or a jump has spent it")
{
    /*  END TO END (namespace draft §24.4): the engine publishes what Doh!
        would take back at `/godot/list/doh`, and the transport reads the cue's
        number off the same snapshot - "Doh! 1" after a GO on cue 1. Every road
        that spends that GO clears it, so the button never names a GO a press
        would be refused for. A net for the path the case above feeds by hand. */
    Engine engine;
    doc::ShowDocument document;
    REQUIRE (doc::Bundle::open (fixtureBundle(), document).ok);
    REQUIRE (document.setAttribute ("/godot/list/goDebounce", "0").ok);

    cue::RunTable runs;
    cue::Focus focus;
    auto runIds = doc::IdRegistry::withSeed (7);
    cue::Runner runner { document, runs, runIds, focus };

    doc::registerDocumentCommands (engine.commands(), document);
    cue::registerCueCommands (engine.commands(), document, focus);
    cue::registerRunCommands (engine.commands(), runs);
    cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);

    MountTable mounts;
    ParameterTree parameters { document, engine.commands(), mounts, runs };
    parameters.setListState (&runner.listState());

    std::int64_t tick = 0;

    const auto press = [&] (const std::string& command, std::vector<osc::Value> args)
    {
        REQUIRE (engine.submit ("cli", command, std::move (args)));
        runner.beforeTick (engine, tick);
        engine.processTick (tick++);
    };

    const auto caption = [&]
    {
        parameters.markStale();
        EngineState state;
        state.tick = tick;
        return model::readTransport (*parameters.publish (tick, state)).dohCaption();
    };

    CHECK (caption() == "Doh!");                             // no GO yet

    press ("go", {});
    CHECK (caption() == "Doh! 1");                           // "House to half", cue 1

    SUBCASE ("a Doh spends it")
    {
        press ("go.doh", {});
    }

    SUBCASE ("a fire by name on its list makes a press refused, so it is spent")
    {
        press ("cue.fire", { osc::Value::string ("E4GP6QSC") });
    }

    SUBCASE ("a jump on its list forgets it")
    {
        press ("list.aim", { osc::Value::string ("7K2QM9X4"), osc::Value::string ("B3N8R5TW"),
                             osc::Value::float64 (0.0) });
        press ("list.loadToTime", { osc::Value::string ("7K2QM9X4") });
    }

    CHECK (caption() == "Doh!");
}

/*  WHAT THE NEXT GO CARRIES ON (2026-10-02, Doh! D2, namespace draft §24.12):
    a cue Doh! paused is carried on by the next GO from where it was, and the
    standby says so beside its name - in words, at the second it carries on
    from, or plainly for a mic, which has no position - and the Doh! button's
    tooltip says that a second press starts it from the top instead. It could
    not be built before D2: the reading had no `resume`. */
TEST_CASE ("client: the standby says where the next GO carries a paused cue on, and Doh! says a second press forgets it")
{
    model::TransportReading reading;
    reading.standbyId = "B3N8R5TW";
    reading.standbyName = "Thunder";
    reading.standbyKind = "media";
    reading.tick = "2000";

    CHECK (reading.standbyLine() == "Thunder  media");

    reading.resume = "B3N8R5TW 8.42";
    CHECK (reading.standbyLine() == "Thunder  media  resumes at 0:08");

    reading.resume = "B3N8R5TW 125";
    CHECK (reading.standbyLine() == "Thunder  media  resumes at 2:05");

    reading.resume = "B3N8R5TW";
    CHECK (reading.standbyLine() == "Thunder  media  resumes");

    //  Another cue paused on the list is not this standby's.
    reading.resume = "Q7WD2M4K 3";
    CHECK (reading.standbyLine() == "Thunder  media");

    /*  The button, over - nothing to take back. A paused cue standing on the
        focused list is not enough (D2's review, MY): the press acts on the
        resume the engine names, on the list of the last Doh, whichever list
        has the focus. */
    reading.resume = "B3N8R5TW 8.42";
    CHECK (reading.dohLook().phase == model::DohPhase::over);
    CHECK_FALSE (reading.dohClickable());
    CHECK (reading.dohTip().find ("Nothing to take back") != std::string::npos);

    //  The engine names another list's cue: that is the one the tooltip names.
    reading.dohForget = "LQ4X8MZT Q7WD2M4K";
    reading.dohForgetCue = "7";
    CHECK (reading.dohClickable());
    CHECK (reading.dohTip().find ("carries 7 on") != std::string::npos);
    CHECK (reading.dohTip().find ("start it from the top") != std::string::npos);

    reading.dohForget.clear();
    reading.dohForgetCue.clear();
    CHECK_FALSE (reading.dohClickable());
}

/*  THE DOH NOTICE (2026-10-03, Doh! D4, namespace draft §24.14): what the last
    Doh put back and what it left, in front of the operator on the transport's
    line - the engine's one sentence, `/godot/list/dohReport`, opening with
    "Doh!". A refusal newer than the report takes the line back. Each failed
    before D4: the reading had no notice. (2026-10-03, D4's review, OJ: what an
    outage makes wait is the ENGINE's to say now, on the same readout, and only
    for a press it accepted - the client's own sentence, shown for refused
    presses too, is gone; the case keeps the sentence as the engine spells it.) */
TEST_CASE ("client: the Doh notice says the last Doh's report, a newer refusal takes the line, and an outage says what waits")
{
    model::TransportReading reading;
    reading.listId = "7K2QM9X4";
    reading.listName = "Show";
    reading.status = "running";

    CHECK (reading.dohNotice().empty());                     // no Doh yet

    reading.dohReport = "7K2QM9X4 1200 Note: MIDI to Keys could not be taken back - the next GO sends it again";
    reading.dohReportList = "Show";
    CHECK (reading.dohNotice() == "Doh!: Note: MIDI to Keys could not be taken back - the next GO sends it again");

    //  A refusal from before the report is older news; one after it takes the line.
    reading.lastError = "1150 27 window too-soon go";
    CHECK (reading.dohNotice() == "Doh!: Note: MIDI to Keys could not be taken back - the next GO sends it again");

    reading.lastError = "1300 31 window not-a-stop standby.set";
    CHECK (reading.dohNotice().empty());
    CHECK (reading.errorLine() == "standby.set refused: not-a-stop");

    //  A report after that refusal - a relaunch's, later - is the news again.
    reading.dohReport = "7K2QM9X4 1400 Scene: comes back once it has ended, unless a GO comes first";
    CHECK (reading.dohNotice() == "Doh!: Scene: comes back once it has ended, unless a GO comes first");

    //  An outage: the engine's pending sentence, read as any report.
    reading.status = "noClock";
    reading.dohReport = "7K2QM9X4 1500 the pointer is back; what it puts back comes when the audio returns";
    CHECK (reading.dohNotice() == "Doh!: the pointer is back; what it puts back comes when the audio returns");
}

TEST_CASE ("client: the Doh notice names the devices left to their operators, whole, in the engine's order")
{
    /*  The author, 2026-10-01: what reached the light board is the light
        operator's - so the notice says it whole, in words, the device and its
        cues. (2026-10-03, D4's review, OL: the left items FIRST is the engine's
        order now, composed so - the client's split on "; " cut a cue name that
        holds one, and is gone. The notice shows the sentence as it came.) */
    model::TransportReading reading;
    reading.listId = "7K2QM9X4";
    reading.listName = "Show";

    reading.dohReport = "7K2QM9X4 1200 Lighting desk: Q12, Q13 - left to its operator, not sent again";
    reading.dohReportList = "Show";
    CHECK (reading.dohNotice() == "Doh!: Lighting desk: Q12, Q13 - left to its operator, not sent again");

    //  A cue whose name holds "; " - and a "left to its operator" in another's.
    reading.dohReport = "7K2QM9X4 1200 Lighting desk: Q12; the flash, Q13 - left to its operator, not sent again; "
                        "Note: MIDI to Keys could not be taken back - the next GO sends it again";
    CHECK (reading.dohNotice() == "Doh!: Lighting desk: Q12; the flash, Q13 - left to its operator, not sent again; "
                                  "Note: MIDI to Keys could not be taken back - the next GO sends it again");
}

TEST_CASE ("client: the Doh notice is shown whatever list has the focus, opening with the list's name when it is another")
{
    /*  Red team C, minor 4: the Doh acts on the list of the last GO, not on the
        focused one, and a report shown only for the focused list told the light
        department nothing when the focus sat elsewhere. End to end: the engine's
        readout, read off a published tree, with the focus moved between the
        two lists. */
    Engine engine;
    doc::ShowDocument document;
    REQUIRE (doc::Bundle::open (fixtureBundle(), document).ok);

    cue::RunTable runs;
    cue::Focus focus;
    auto runIds = doc::IdRegistry::withSeed (7);
    cue::Runner runner { document, runs, runIds, focus };

    doc::registerDocumentCommands (engine.commands(), document);
    cue::registerCueCommands (engine.commands(), document, focus);
    cue::registerRunCommands (engine.commands(), runs);
    cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);

    MountTable mounts;
    ParameterTree parameters { document, engine.commands(), mounts, runs };
    parameters.setListState (&runner.listState());

    const auto other = document.createList ("Act 2").id;
    std::int64_t tick = 0;

    const auto press = [&] (const std::string& command, std::vector<osc::Value> args)
    {
        REQUIRE (engine.submit ("cli", command, std::move (args)));
        runner.beforeTick (engine, tick);
        REQUIRE (engine.processTick (tick++).rejected == 0);
    };

    const auto reading = [&]
    {
        parameters.markStale();
        EngineState state;
        state.tick = tick;
        return model::readTransport (*parameters.publish (tick, state));
    };

    const auto fixtureList = model::text (*parameters.publish (tick, EngineState {}), "/godot/list/7K2QM9X4/name");
    REQUIRE_FALSE (fixtureList.empty());

    press ("list.focus", { osc::Value::string (other) });
    REQUIRE (reading().listId == other);
    CHECK (reading().dohNotice().empty());

    //  The report is on the fixture's list; the focus on Act 2.
    press ("list.dohReport", { osc::Value::string ("7K2QM9X4"),
                               osc::Value::string ("Lighting desk: Q12 - left to its operator, not sent again") });
    const auto first = reading();
    CHECK (first.dohNotice() == "Doh! on " + fixtureList + ": Lighting desk: Q12 - left to its operator, not sent again");

    //  The focus on the report's own list: no list's name.
    press ("list.focus", { osc::Value::string ("7K2QM9X4") });
    CHECK (reading().dohNotice() == "Doh!: Lighting desk: Q12 - left to its operator, not sent again");

    /*  A later report - a relaunch's - is another reading. (2026-10-03, D4's
        review, OK: APPENDED to the Doh's, which it replaced until then - the
        light department's news must not vanish before anybody read it.) */
    press ("list.dohReport", { osc::Value::string ("7K2QM9X4"),
                               osc::Value::string ("Scene: put back at 0:12"), osc::Value::boolean (true) });
    const auto later = reading();
    CHECK (later.dohReport != first.dohReport);
    CHECK (later.dohNotice() == "Doh!: Lighting desk: Q12 - left to its operator, not sent again; then: Scene: put back at 0:12");

    //  A report that is not a relaunch's replaces; an empty one clears (OJ).
    press ("list.dohReport", { osc::Value::string ("7K2QM9X4"), osc::Value::string ("Sub: put back") });
    CHECK (reading().dohNotice() == "Doh!: Sub: put back");

    press ("list.dohReport", { osc::Value::string ("7K2QM9X4"), osc::Value::string ("") });
    CHECK (reading().dohNotice().empty());
    CHECK (reading().dohReport.empty());
}

TEST_CASE ("client: a fade's mixer has a strip per slider, lit where the fade moves it, and its EQ is the target's with the fade's rows over it")
{
    /*  Namespace draft §26, PG and PH: the level, the speed of a file, and a
        send into every mix; a strip the fade moves carries where it goes, one
        it leaves alone what the target holds now. */
    Rig rig;

    const auto media = rig.document.createCue ("7K2QM9X4", 0, "media", "Rain");
    REQUIRE (media.ok);
    const auto bus = rig.document.createBus ("mix", 2);
    REQUIRE (bus.ok);
    REQUIRE (rig.document.createSend (media.id, bus.id, {}, "-10").ok);

    const auto fade = rig.document.createCue ("7K2QM9X4", 1, "fade", "Rain away");
    REQUIRE (fade.ok);

    const auto set = [&rig, &fade] (const char* row, const std::string& value)
    {
        REQUIRE (rig.document.setAttribute ("/godot/cue/" + fade.id + "/" + row, value).ok);
    };

    set ("target", media.id);
    set ("sends", bus.id + ":-30");
    set ("eq", "eqB2Gain:-6");

    auto mix = model::readFadeMix (*rig.publish (1), fade.id);
    REQUIRE (mix.present);
    REQUIRE (mix.strips.size() >= 3u);

    CHECK (mix.strips[0].kind == "level");
    CHECK (mix.strips[0].moved);
    CHECK (mix.strips[0].switchAddress == "/godot/cue/" + fade.id + "/levelOn");

    CHECK (mix.strips[1].kind == "speed");
    CHECK_FALSE (mix.strips[1].moved);
    CHECK (mix.strips[1].value == doctest::Approx (1.0));

    const auto& send = mix.strips.back();
    CHECK (send.kind == "send");
    CHECK (send.busId == bus.id);
    CHECK (send.moved);
    CHECK (send.value == doctest::Approx (-30.0));
    CHECK (send.address == "/godot/cue/" + fade.id + "/moves/send/" + bus.id);

    REQUIRE (mix.moves.size() == 1u);
    CHECK (mix.moves[0].entry == "eq/eqB2Gain");
    CHECK (mix.moves[0].label == "EQ Band 2 gain");
    CHECK (mix.moves[0].valueText == "-6.0 dB");

    //  The send cleared from the list: the strip stays, unlit, at the cue's own -10.
    set ("sends", "");
    rig.parameters.markStale();
    mix = model::readFadeMix (*rig.publish (2), fade.id);
    CHECK_FALSE (mix.strips.back().moved);
    CHECK (mix.strips.back().value == doctest::Approx (-10.0));

    //  The EQ the panel draws on the fade: the target's, with band two at the fade's -6.
    const auto eq = model::readFadeEq (*rig.publish (3), fade.id);
    REQUIRE (eq.present);
    CHECK (eq.fadeId == fade.id);
    CHECK (eq.fadeMoves.count ("eqB2Gain") == 1u);
    CHECK (eq.settings.band[1].gain == doctest::Approx (-6.0f));

    //  And the lists are the mixer's, not the inspector's.
    const auto panel = model::inspect (*rig.publish (4), fade.id);
    CHECK (rowIn (panel, "sends") == nullptr);
    CHECK (rowIn (panel, "eq") == nullptr);
    CHECK (rowIn (panel, "fx") == nullptr);

    //  A plugin window on a fade is routed by an identifier no Fx can have.
    CHECK (model::fadeOfEditorId (model::fadeEditorId (fade.id)) == fade.id);
    CHECK (model::fadeOfEditorId ("FX000001").empty());

    //  The speed's throw: nought at the bottom, one in the middle, twenty at the top.
    CHECK (model::fractionForSpeed (0.0) <= 0.0);
    CHECK (model::fractionForSpeed (1.0) == doctest::Approx (0.5).epsilon (0.01));
    CHECK (model::speedForFraction (1.0) == doctest::Approx (20.0));
    CHECK (model::speedForFraction (model::fractionForSpeed (0.5)) == doctest::Approx (0.5));
}
