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
    What a cue is called while nobody has called it anything (namespace draft
    §53): the words each kind is given, the name that was only ever the default
    following its cue through an edit, and the row the tree publishes.

    Runs under fr_FR as well as C: a fade's level is read off the document, and
    a number read through text would be a locale question.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/AutoName.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>

#include <cstdint>
#include <memory>
#include <string>

using namespace wfg;

namespace
{
    struct Show
    {
        Show()
        {
            list = document.createList ("Main").id;
            REQUIRE (! list.empty());
        }

        std::string cue (const std::string& kind, const std::string& name = {})
        {
            const auto made = document.createCue (list, at++, kind, name);
            REQUIRE (made.ok);
            return made.id;
        }

        void set (const std::string& id, const std::string& row, const std::string& value)
        {
            const auto result = document.setAttribute ("/godot/cue/" + id + "/" + row, value);
            REQUIRE_MESSAGE (result.ok, row << " = " << value << ": " << result.reason);
        }

        std::string automatic (const std::string& id) const
        {
            return cue::AutoNames { document.root() }.of (document.findById (id));
        }

        std::string name (const std::string& id) const
        {
            return document.getAttribute ("/godot/cue/" + id + "/name").value_or ("?");
        }

        doc::ShowDocument document;
        std::string list;
        int at = 0;
    };
}

//==============================================================================
TEST_CASE ("auto name: a cue that does something to another is called by it")
{
    Show show;
    const auto music = show.cue ("media");
    show.set (music, "file", "music/Intro music.wav");

    //  A sound nobody named is called by its file, without the folder or the extension.
    CHECK (show.automatic (music) == "Intro music");

    const auto fade = show.cue ("fade");
    CHECK (show.automatic (fade).empty());         // no target, no name yet

    show.set (fade, "target", music);
    CHECK (show.automatic (fade) == "Fade out Intro music");    // a fresh fade goes to silence

    show.set (fade, "level", "-6");
    CHECK (show.automatic (fade) == "Fade Intro music");

    show.set (fade, "stopWhenDone", "true");
    CHECK (show.automatic (fade) == "Fade and stop Intro music");

    show.set (fade, "level", "-120");
    CHECK (show.automatic (fade) == "Fade out and stop Intro music");

    show.set (fade, "rateOn", "true");
    CHECK (show.automatic (fade) == "Fade and stop Intro music");   // it moves more than the level

    const auto stop = show.cue ("transport");
    show.set (stop, "target", music);
    CHECK (show.automatic (stop) == "Stop Intro music");

    show.set (stop, "verb", "afterIteration");
    CHECK (show.automatic (stop) == "Stop Intro music after this round");

    show.set (stop, "verb", "fade");
    CHECK (show.automatic (stop) == "Fade out and stop Intro music");

    show.set (stop, "verb", "disable");
    CHECK (show.automatic (stop) == "Disable Intro music");

    show.set (stop, "verb", "jump");
    show.set (stop, "andGo", "true");
    CHECK (show.automatic (stop) == "Jump to Intro music and Go");

    const auto start = show.cue ("start");
    show.set (start, "target", music);
    CHECK (show.automatic (start) == "Start Intro music");

    //  The target's own name, once it has one - and the fades follow it.
    show.set (music, "name", "Overture");
    CHECK (show.automatic (start) == "Start Overture");
    CHECK (show.automatic (fade) == "Fade and stop Overture");

    //  A target that is a fade of the sound reads as the fade does.
    const auto stopTheFade = show.cue ("transport");
    show.set (stopTheFade, "target", fade);
    CHECK (show.automatic (stopTheFade) == "Stop Fade and stop Overture");

    //  A target with neither name nor automatic name, by its number.
    const auto memo = show.cue ("memo");
    show.set (memo, "number", "12");
    show.set (stop, "target", memo);
    show.set (stop, "verb", "hard");
    CHECK (show.automatic (stop) == "Stop cue 12");

    //  Two fades of each other end, named by number.
    const auto one = show.cue ("fade");
    const auto two = show.cue ("fade");
    show.set (one, "number", "1");
    show.set (two, "number", "2");
    show.set (one, "target", two);
    show.set (two, "target", one);
    CHECK (! show.automatic (one).empty());
    CHECK (show.automatic (one).find ("cue") != std::string::npos);
}

TEST_CASE ("auto name: a DCA's fade and a ranged advance say what they move")
{
    Show show;
    const auto dca = show.document.createDca ("Band");
    REQUIRE (dca.ok);

    const auto fade = show.cue ("fade");
    show.set (fade, "dca", dca.id);
    CHECK (show.automatic (fade) == "Fade DCA Band");

    const auto bed = show.cue ("media");
    show.set (bed, "file", "Bed.wav");
    const auto verse = show.document.createRange (bed, 0.0, 4.0);
    REQUIRE (verse.ok);
    REQUIRE (show.document.setAttribute ("/godot/range/" + verse.id + "/name", "Verse").ok);

    const auto advance = show.cue ("transport");
    show.set (advance, "target", bed);
    show.set (advance, "verb", "advance");
    CHECK (show.automatic (advance) == "Advance Bed");

    show.set (advance, "range", verse.id);
    CHECK (show.automatic (advance) == "Advance Bed to Verse");
}

TEST_CASE ("auto name: an OSC or a MIDI cue is called by what it sends")
{
    Show show;

    const auto osc = show.cue ("osc");
    CHECK (show.automatic (osc).empty());

    show.set (osc, "address", "/mixer/ch/1/fader");
    CHECK (show.automatic (osc) == "/mixer/ch/1/fader");

    show.set (osc, "value", "f:0.5");
    CHECK (show.automatic (osc) == "/mixer/ch/1/fader 0.5");

    REQUIRE (show.document.createMessage (osc, "/mixer/ch/2/fader", "f:0.25").ok);
    CHECK (show.automatic (osc) == "/mixer/ch/1/fader 0.5 (+1)");

    const auto port = show.document.createPort ("Desk");
    REQUIRE (port.ok);

    const auto midi = show.cue ("midi");
    show.set (midi, "type", "programChange");
    show.set (midi, "data1", "5");
    CHECK (show.automatic (midi) == "Program change 5, ch 1");

    show.set (midi, "port", port.id);
    show.set (midi, "channel", "3");
    CHECK (show.automatic (midi) == "Program change 5, ch 3 on Desk");

    show.set (midi, "type", "controlChange");
    show.set (midi, "data1", "7");
    show.set (midi, "data2", "100");
    CHECK (show.automatic (midi) == "CC 7 = 100, ch 3 on Desk");

    show.set (midi, "type", "noteOn");
    show.set (midi, "data1", "60");
    CHECK (show.automatic (midi) == "Note on 60 vel 100, ch 3 on Desk");

    show.set (midi, "type", "sysex");
    show.set (midi, "sysex", "F0 7E 7F 06 01 02 03 F7");
    CHECK (show.automatic (midi) == "SysEx F0 7E 7F 06 01 02 ... on Desk");
}

TEST_CASE ("auto name: a picture, a movie, a capture and a mic are called by their source")
{
    Show show;

    const auto still = show.cue ("video");
    CHECK (show.automatic (still) == "Fill");                  // what a new video cue is

    show.set (still, "source", "picture");
    show.set (still, "file", "Backdrop.png");
    CHECK (show.automatic (still) == "Backdrop");

    show.set (still, "source", "mask");
    CHECK (show.automatic (still) == "Mask");

    const auto input = show.document.createVideoInput ("Stage camera", "ndi", "");
    REQUIRE (input.ok);
    show.set (still, "source", "capture");
    CHECK (show.automatic (still) == "Capture");
    show.set (still, "videoInput", input.id);
    CHECK (show.automatic (still) == "Stage camera");

    const auto voice = show.document.createInput (1);
    REQUIRE (voice.ok);
    REQUIRE (show.document.setAttribute ("/godot/input/" + voice.id + "/name", "Voix solo").ok);

    const auto mic = show.cue ("mic");
    CHECK (show.automatic (mic).empty());
    show.set (mic, "input", voice.id);
    CHECK (show.automatic (mic) == "Voix solo");

    //  A group and a memo have none: what they do is what somebody writes.
    CHECK (show.automatic (show.cue ("group")).empty());
    CHECK (show.automatic (show.cue ("memo")).empty());
}

//==============================================================================
TEST_CASE ("auto name: a name that was only ever the default follows the cue, a typed one stays")
{
    Show show;

    //  A drop names a new cue after its file before the file is there; once it
    //  is, the name is the default and is given back to it.
    const auto dropped = show.cue ("media", "Thunder");
    show.set (dropped, "file", "Thunder.wav");
    CHECK (show.name (dropped).empty());
    CHECK (show.automatic (dropped) == "Thunder");

    //  And another file is another default.
    show.set (dropped, "file", "Rain.wav");
    CHECK (show.automatic (dropped) == "Rain");

    //  A show made before §53: the sound carries its file's name as written.
    //  Changing its file lets the name go, since it was the default.
    const auto older = show.cue ("media", "Wind");
    show.set (older, "file", "Wind.wav");          // empty: it is Wind.wav's
    show.set (older, "name", "Wind");               // as the old drop wrote it
    show.set (older, "file", "Gale.wav");
    CHECK (show.name (older).empty());
    CHECK (show.automatic (older) == "Gale");

    //  A name somebody typed stays, whatever the file becomes...
    show.set (older, "name", "Storm, act 2");
    show.set (older, "file", "Breeze.wav");
    CHECK (show.name (older) == "Storm, act 2");

    //  ...until it is cleared, and then the default comes back, and follows again.
    show.set (older, "name", "");
    CHECK (show.automatic (older) == "Breeze");
    show.set (older, "file", "Squall.wav");
    CHECK (show.automatic (older) == "Squall");

    //  The same for a fade: a name written as the default goes with its target.
    const auto fade = show.cue ("fade");
    show.set (fade, "target", dropped);
    show.set (fade, "name", "Fade out Rain");
    show.set (fade, "target", older);
    CHECK (show.name (fade).empty());
    CHECK (show.automatic (fade) == "Fade out Squall");

    //  An edit that does not feed the name leaves a typed name alone.
    show.set (fade, "name", "Fade out Rain");
    show.set (fade, "duration", "3");
    CHECK (show.name (fade) == "Fade out Rain");
}

TEST_CASE ("auto name: letting the default name go is one step of Undo with the edit")
{
    Show show;
    const auto sound = show.cue ("media", "Wind");
    show.set (sound, "file", "Wind.wav");
    show.set (sound, "name", "Wind");

    show.document.beginTransaction ("node.set", 1000, "window", {});
    show.set (sound, "file", "Gale.wav");
    CHECK (show.name (sound).empty());

    REQUIRE (show.document.undo (doc::UndoDomain::document).has_value());
    CHECK (show.name (sound) == "Wind");
    CHECK (show.document.getAttribute ("/godot/cue/" + sound + "/file").value_or ("") == "Wind.wav");
}

//==============================================================================
TEST_CASE ("auto name: the tree publishes it beside the name, and a rename of the target moves it")
{
    Engine engine;
    doc::ShowDocument document;
    tree::MountTable mounts;
    cue::RunTable runs;
    tree::ParameterTree parameters { document, engine.commands(), mounts, runs };

    const auto list = document.createList ("Main").id;
    const auto sound = document.createCue (list, 0, "media", "").id;
    const auto fade = document.createCue (list, 1, "fade", "").id;
    REQUIRE (document.setAttribute ("/godot/cue/" + sound + "/file", "Thunder.wav").ok);
    REQUIRE (document.setAttribute ("/godot/cue/" + fade + "/target", sound).ok);

    const auto read = [&] (const std::string& address)
    {
        tree::EngineState state;
        state.version = "test";
        state.documentRevision = document.showRevision();
        parameters.markStale();

        const auto snapshot = parameters.publish (0, state);
        const auto* node = snapshot->find (address);
        REQUIRE (node != nullptr);
        REQUIRE (node->soleValue().has_value());
        return *node->soleValue();
    };

    CHECK (read ("/godot/cue/" + fade + "/name") == osc::Value::string (""));
    CHECK (read ("/godot/cue/" + fade + "/autoName") == osc::Value::string ("Fade out Thunder"));
    CHECK (read ("/godot/cue/" + sound + "/autoName") == osc::Value::string ("Thunder"));

    REQUIRE (document.setAttribute ("/godot/cue/" + sound + "/name", "Storm").ok);
    CHECK (read ("/godot/cue/" + fade + "/autoName") == osc::Value::string ("Fade out Storm"));
}
