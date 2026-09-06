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

/*  WHAT FIRES A CUE WHEN NOBODY PRESSES ANYTHING.

    The three matchers are pure functions of an index and an input - no socket,
    no MIDI port, no clock - which is what lets them be tested exhaustively on a
    machine that has none of those. Every CI runner is such a machine, and the
    wiring that hands them their input is three lines each.

    The index itself is read out of a document on the tick thread and published
    immutable, exactly as the parameter tree is, because the matching happens on
    whichever thread the input arrived on and none of those may read a document.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/cue/TriggerIndex.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/ShowDocument.h>

using namespace wfg;
using namespace wfg::cue;

namespace
{
    /*  A show with a cue and however many triggers a case wants. The index is
        rebuilt on demand, because that is what the engine does: a document
        change republishes it whole, like the tree. */
    struct TriggerRig
    {
        TriggerRig()
        {
            listId = document.createList ("Main").id;
            cueId = document.createCue (listId, 0, "memo", "House to half").id;
            other = document.createCue (listId, 1, "memo", "Doors").id;
        }

        std::string add (const std::string& kind, const std::string& onCue = {})
        {
            const auto edit = document.createTrigger (onCue.empty() ? cueId : onCue, kind);
            REQUIRE (edit.ok);
            return edit.id;
        }

        void set (const std::string& triggerId, const char* name, const std::string& value)
        {
            REQUIRE (document.setAttribute ("/godot/trigger/" + triggerId + "/" + name,
                                            value).ok);
        }

        std::shared_ptr<const TriggerIndex> index() const
        {
            return TriggerIndex::build (document);
        }

        doc::ShowDocument document;
        std::string listId, cueId, other;
    };

    std::vector<osc::Value> noArgs() { return {}; }
}

//==============================================================================
TEST_CASE ("trigger index: it finds triggers wherever the cue they belong to sits")
{
    /*  §3.7 gives the list to "a cue or a group", and a cue can be three levels
        inside a group, inside a header, inside another group. The index walks
        the whole show rather than the top level, and each trigger carries the
        identifier of the cue it will fire - read from where it SITS rather than
        stored, so the containment cannot come to disagree with a copy of
        itself. */
    TriggerRig rig;

    const auto group = rig.document.createCue (rig.listId, 2, "group", "Scene").id;
    const auto member = rig.document.createCue (group, 0, "memo", "Deep").id;

    const auto top = rig.add (triggerKind::osc);
    const auto deep = rig.add (triggerKind::osc, member);
    const auto onTheGroup = rig.add (triggerKind::clock, group);

    const auto index = rig.index();
    REQUIRE (index->triggers.size() == 3u);

    const auto cueFor = [&index] (const std::string& id)
    {
        for (const auto& trigger : index->triggers)
            if (trigger.id == id)
                return trigger.cue;

        return std::string {};
    };

    CHECK (cueFor (top) == rig.cueId);
    CHECK (cueFor (deep) == member);
    CHECK (cueFor (onTheGroup) == group);
}

TEST_CASE ("trigger index: a trigger that has never been written to reads its defaults")
{
    /*  THE MISTAKE THAT COST AN AFTERNOON IN THE GROUP SCHEDULER, and it is not
        being made twice. The canonical writer omits an attribute holding its
        default and the reader leaves it absent, so a trigger nobody has edited
        has almost no properties at all - and asking the ValueTree directly
        answers `false` for `enabled` on every trigger in the show, which would
        make every one of them silently dead. */
    TriggerRig rig;
    const auto id = rig.add (triggerKind::osc);

    const auto index = rig.index();
    REQUIRE (index->triggers.size() == 1u);

    const auto& trigger = index->triggers.front();
    CHECK (trigger.enabled);
    CHECK (trigger.kind == "osc");
    CHECK (trigger.channel == 0);
    CHECK (trigger.data == -1);
    CHECK (trigger.secondOfDay == -1);
    CHECK (trigger.id == id);
}

//==============================================================================
TEST_CASE ("matchOsc: the address fires it, and nothing else does")
{
    TriggerRig rig;
    const auto id = rig.add (triggerKind::osc);
    rig.set (id, "address", "/desk/go");

    const auto index = rig.index();

    CHECK (matchOsc (*index, "/desk/go", noArgs()) == std::vector<std::string> { id });
    CHECK (matchOsc (*index, "/desk/stop", noArgs()).empty());
    CHECK (matchOsc (*index, "/desk/go/", noArgs()).empty());
    CHECK (matchOsc (*index, "/desk", noArgs()).empty());
}

TEST_CASE ("matchOsc: a trigger with no address fires on nothing")
{
    /*  Rather than on everything, which is what a bare `address != wanted` test
        would have done to a trigger somebody created and had not finished
        writing. A half-authored trigger that fired on every message the show
        received would be the worst possible failure of this feature. */
    TriggerRig rig;
    rig.add (triggerKind::osc);

    CHECK (matchOsc (*rig.index(), "/anything", noArgs()).empty());
}

TEST_CASE ("matchOsc: a disabled trigger fires on nothing either")
{
    TriggerRig rig;
    const auto id = rig.add (triggerKind::osc);
    rig.set (id, "address", "/desk/go");
    rig.set (id, "enabled", "false");

    CHECK (matchOsc (*rig.index(), "/desk/go", noArgs()).empty());
}

TEST_CASE ("matchOsc: an empty value matches any arguments, including none")
{
    /*  Which is what a bare address means to most of the things that will be
        sending one: a foot switch that sends `/go` and nothing else must not
        have to say so. */
    TriggerRig rig;
    const auto id = rig.add (triggerKind::osc);
    rig.set (id, "address", "/desk/go");

    const auto index = rig.index();

    CHECK (matchOsc (*index, "/desk/go", noArgs()).size() == 1u);
    CHECK (matchOsc (*index, "/desk/go", { osc::Value::float32 (1.0f) }).size() == 1u);
    CHECK (matchOsc (*index, "/desk/go", { osc::Value::string ("anything") }).size() == 1u);
}

TEST_CASE ("matchOsc: a value asked for has to be there, in the log's own spelling")
{
    /*  A surface that sends `/go f:1` on press and `/go f:0` on release is the
        ordinary case, and firing on both would be a cue that plays twice a
        press. The atom is spelled the way the log spells one, so an operator
        who has read a single line of a log knows how to write it - and the
        parser is the one the log reader already has, rather than a second
        syntax that could come to disagree with it. */
    TriggerRig rig;
    const auto id = rig.add (triggerKind::osc);
    rig.set (id, "address", "/desk/go");
    rig.set (id, "value", "f:1");

    const auto index = rig.index();

    CHECK (matchOsc (*index, "/desk/go", { osc::Value::float32 (1.0f) }).size() == 1u);
    CHECK (matchOsc (*index, "/desk/go", { osc::Value::float32 (0.0f) }).empty());
    CHECK (matchOsc (*index, "/desk/go", noArgs()).empty());

    /*  ANY ARGUMENT, not the first: a message carrying several may put the one
        that means something second, and insisting on position would make this
        useless against half the surfaces in a rack. */
    CHECK (matchOsc (*index, "/desk/go", { osc::Value::string ("x"),
                                           osc::Value::float32 (1.0f) }).size() == 1u);
}

TEST_CASE ("matchOsc: two triggers on one address both fire")
{
    /*  Which is what a LIST of triggers means. Answering with the first would
        make the second silently dead and would depend on document order for
        which of them lived. */
    TriggerRig rig;

    const auto one = rig.add (triggerKind::osc);
    const auto two = rig.add (triggerKind::osc, rig.other);
    rig.set (one, "address", "/desk/go");
    rig.set (two, "address", "/desk/go");

    CHECK (matchOsc (*rig.index(), "/desk/go", noArgs())
             == std::vector<std::string> { one, two });
}

//==============================================================================
TEST_CASE ("matchMidi: type, number and channel all have to agree")
{
    TriggerRig rig;
    const auto id = rig.add (triggerKind::midi);
    rig.set (id, "type", "noteOn");
    rig.set (id, "number", "60");
    rig.set (id, "channel", "3");

    const auto index = rig.index();

    const auto fires = [&index] (MidiEvent event)
    {
        return ! matchMidi (*index, event).empty();
    };

    CHECK (fires ({ "", "noteOn", 3, 60, 100 }));
    CHECK_FALSE (fires ({ "", "noteOn", 4, 60, 100 }));
    CHECK_FALSE (fires ({ "", "noteOn", 3, 61, 100 }));
    CHECK_FALSE (fires ({ "", "noteOff", 3, 60, 100 }));
}

TEST_CASE ("matchMidi: channel nought is any channel, and it is nought rather than minus one")
{
    /*  Because MIDI channels are one-based everywhere a musician looks at them,
        so nought is a value that cannot be a channel and reads as "not set". */
    TriggerRig rig;
    const auto id = rig.add (triggerKind::midi);
    rig.set (id, "type", "noteOn");
    rig.set (id, "number", "60");

    const auto index = rig.index();

    for (int channel = 1; channel <= 16; ++channel)
        CHECK_FALSE (matchMidi (*index, { "", "noteOn", channel, 60, 100 }).empty());
}

TEST_CASE ("matchMidi: data is -1 for any, because velocity nought means something")
{
    /*  A note-on with velocity nought is a note-off on a great many devices, so
        "any velocity" cannot be spelled nought - somebody will one day want to
        match exactly that, deliberately, and they have to be able to. */
    TriggerRig rig;
    const auto any = rig.add (triggerKind::midi);
    rig.set (any, "type", "noteOn");
    rig.set (any, "number", "60");

    const auto zero = rig.add (triggerKind::midi, rig.other);
    rig.set (zero, "type", "noteOn");
    rig.set (zero, "number", "60");
    rig.set (zero, "data", "0");

    const auto index = rig.index();

    CHECK (matchMidi (*index, { "", "noteOn", 1, 60, 100 })
             == std::vector<std::string> { any });
    CHECK (matchMidi (*index, { "", "noteOn", 1, 60, 0 })
             == std::vector<std::string> { any, zero });
}

TEST_CASE ("matchMidi: an empty port is any input, and a named one is that input")
{
    TriggerRig rig;
    const auto anywhere = rig.add (triggerKind::midi);
    rig.set (anywhere, "type", "controlChange");
    rig.set (anywhere, "number", "7");

    const auto named = rig.add (triggerKind::midi, rig.other);
    rig.set (named, "type", "controlChange");
    rig.set (named, "number", "7");
    rig.set (named, "port", "Desk");

    const auto index = rig.index();

    CHECK (matchMidi (*index, { "Desk", "controlChange", 1, 7, 64 })
             == std::vector<std::string> { anywhere, named });
    CHECK (matchMidi (*index, { "Piano", "controlChange", 1, 7, 64 })
             == std::vector<std::string> { anywhere });
}

//==============================================================================
TEST_CASE ("clock: HH:MM:SS and nothing else")
{
    CHECK (secondOfDayFor ("00:00:00") == 0);
    CHECK (secondOfDayFor ("19:30:00") == 19 * 3600 + 30 * 60);
    CHECK (secondOfDayFor ("23:59:59") == 86399);

    //  A mistyped time is a thing `wfg validate` can mention, rather than a
    //  trigger that fires at some hour nobody meant.
    CHECK (secondOfDayFor ("") == -1);
    CHECK (secondOfDayFor ("19:30") == -1);
    CHECK (secondOfDayFor ("7:30:00") == -1);
    CHECK (secondOfDayFor ("24:00:00") == -1);
    CHECK (secondOfDayFor ("19:60:00") == -1);
    CHECK (secondOfDayFor ("19:30:60") == -1);
    CHECK (secondOfDayFor ("19-30-00") == -1);
    CHECK (secondOfDayFor ("19:30:0a") == -1);
}

TEST_CASE ("clock: a crossing is asked about an INTERVAL, so it happens exactly once")
{
    /*  A tick is twenty milliseconds and a second is fifty of them. Asking
        whether the clock reads 19:30:00 would fire a cue fifty times; asking on
        a tick that happened to be late would miss it altogether. An interval is
        the shape that is right at any rate and for any lateness. */
    TriggerRig rig;
    const auto id = rig.add (triggerKind::clock);
    rig.set (id, "at", "19:30:00");

    const auto index = rig.index();
    const auto at = 19 * 3600 + 30 * 60;

    //  Half open, (previous, now], so the second belongs to exactly one interval.
    CHECK (clockCrossings (*index, at - 1, at) == std::vector<std::string> { id });
    CHECK (clockCrossings (*index, at, at + 1).empty());
    CHECK (clockCrossings (*index, at, at).empty());
    CHECK (clockCrossings (*index, at - 1, at - 1).empty());

    //  And a gap - a machine that went to sleep, a reading that was late -
    //  still crosses it exactly once.
    CHECK (clockCrossings (*index, at - 60, at + 60) == std::vector<std::string> { id });
}

TEST_CASE ("clock: midnight is the interval wrapping, not a special case")
{
    TriggerRig rig;

    const auto midnight = rig.add (triggerKind::clock);
    rig.set (midnight, "at", "00:00:00");

    const auto small = rig.add (triggerKind::clock, rig.other);
    rig.set (small, "at", "00:00:05");

    const auto index = rig.index();

    //  23:59:59 to 00:00:01 crosses midnight and nothing else.
    CHECK (clockCrossings (*index, 86399, 1) == std::vector<std::string> { midnight });

    //  And a wider wrap takes both.
    CHECK (clockCrossings (*index, 86390, 10)
             == std::vector<std::string> { midnight, small });
}

TEST_CASE ("clock: a trigger with no time, or a bad one, never fires")
{
    TriggerRig rig;
    const auto blank = rig.add (triggerKind::clock);
    const auto wrong = rig.add (triggerKind::clock, rig.other);
    rig.set (wrong, "at", "half past seven");

    juce::ignoreUnused (blank);

    const auto index = rig.index();

    for (int second = 0; second < 86400; second += 997)
        CHECK (clockCrossings (*index, second, second + 997).empty());
}

//==============================================================================
TEST_CASE ("triggers: each kind answers only to its own input")
{
    /*  Three kinds share one element, because the grammar cannot refuse
        `channel` on an OSC trigger without an element per kind - and three
        elements for one concept with three sources would be worse. So the
        matchers have to be the ones that keep them apart, and a MIDI trigger
        that answered an OSC address would be the failure that arrangement
        risks. */
    TriggerRig rig;

    const auto viaOsc = rig.add (triggerKind::osc);
    rig.set (viaOsc, "address", "/desk/go");

    const auto viaMidi = rig.add (triggerKind::midi, rig.other);
    rig.set (viaMidi, "type", "noteOn");
    rig.set (viaMidi, "number", "60");

    const auto viaClock = rig.add (triggerKind::clock, rig.other);
    rig.set (viaClock, "at", "19:30:00");

    const auto index = rig.index();

    CHECK (matchOsc (*index, "/desk/go", noArgs()) == std::vector<std::string> { viaOsc });
    CHECK (matchMidi (*index, { "", "noteOn", 1, 60, 100 })
             == std::vector<std::string> { viaMidi });
    CHECK (clockCrossings (*index, 19 * 3600 + 30 * 60 - 1, 19 * 3600 + 30 * 60)
             == std::vector<std::string> { viaClock });
}

//==============================================================================
/*  `trigger.fire` — the command §4.11 requires, and what it does not do.
*/
namespace
{
    struct FiringRig : TriggerRig
    {
        FiringRig()
        {
            engine.log().openInMemory ({});

            doc::registerDocumentCommands (engine.commands(), document);
            registerCueCommands (engine.commands(), document, focus);
            registerRunCommands (engine.commands(), runs);
            registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);
        }

        Engine::TickResult tick (const std::string& name, std::vector<osc::Value> args = {})
        {
            REQUIRE (engine.submit ("cli", name, std::move (args)));
            runner.beforeTick (engine, tickCount);
            return engine.processTick (tickCount++);
        }

        std::string runOf (const std::string& cueId) const
        {
            for (const auto& run : runs.all())
                if (run.cue == cueId)
                    return run.id;

            return {};
        }

        Engine engine;
        RunTable runs;
        Focus focus;
        doc::IdRegistry runIds { doc::IdRegistry::withSeed (11) };
        Runner runner { document, runs, runIds, focus };
        std::int64_t tickCount = 0;
    };
}

TEST_CASE ("trigger.fire: it fires the cue the trigger sits on")
{
    FiringRig rig;
    const auto id = rig.add (triggerKind::osc);
    rig.set (id, "address", "/desk/go");

    CHECK (rig.tick ("trigger.fire", { osc::Value::string (id) }).applied == 1);
    CHECK (! rig.runOf (rig.cueId).empty());
}

TEST_CASE ("trigger.fire: it does not move the standby, whatever the standby was")
{
    /*  §3.5 and §3.7 both say it: only GO moves the pointer. That is the whole
        reason a background list can be driven by something other than a person
        without the person losing their place - and it is why this is not `go`
        with a different name. */
    FiringRig rig;

    REQUIRE (rig.document.setAttribute (standbyAddressOf (rig.listId), rig.other).ok);

    const auto id = rig.add (triggerKind::osc);
    rig.set (id, "address", "/desk/go");

    CHECK (rig.tick ("trigger.fire", { osc::Value::string (id) }).applied == 1);

    CHECK (rig.document.getAttribute ("/godot/list/" + rig.listId + "/standby") == rig.other);
    CHECK (! rig.runOf (rig.cueId).empty());
}

TEST_CASE ("trigger.fire: an identifier that is not a trigger is refused")
{
    /*  Including a cue's. The command takes the TRIGGER because that is what
        fired, and firing a cue by name is `cue.fire` - two commands with two
        meanings rather than one that guesses which was meant. */
    FiringRig rig;

    CHECK (rig.tick ("trigger.fire", { osc::Value::string ("ZZZZZZZZ") }).rejected == 1);
    CHECK (rig.tick ("trigger.fire", { osc::Value::string (rig.cueId) }).rejected == 1);
}

TEST_CASE ("trigger.fire: a manual sequence group is refused, because nobody would advance it")
{
    /*  The same refusal `cue.fire` gives and for the same reason: §3.6 makes
        the operator the parent of a manual group, and fired from here it would
        run its header, start its first member and then wait for a press that is
        never coming - a scene stuck halfway with its voices held. */
    FiringRig rig;

    const auto group = rig.document.createCue (rig.listId, 2, "group", "Scene").id;
    rig.document.createCue (group, 0, "memo", "One");

    const auto id = rig.add (triggerKind::osc, group);
    rig.set (id, "address", "/desk/scene");

    const auto outcome = rig.tick ("trigger.fire", { osc::Value::string (id) });
    CHECK (outcome.rejected == 1);
    CHECK (rig.engine.lastError().find (reason::needsGo) != std::string::npos);

    //  An AUTOMATIC group is a different thing: it advances itself, so firing
    //  it from outside is exactly what a trigger is for.
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + group + "/advance", "auto").ok);
    CHECK (rig.tick ("trigger.fire", { osc::Value::string (id) }).applied == 1);
}

//==============================================================================
TEST_CASE ("validate: an OSC trigger may not listen inside the engine or a mount")
{
    /*  Both are addresses the engine already answers on the same port, so a
        trigger there would be a message that both wrote a value and fired a
        cue - and nobody afterwards could say which had been meant, nor which
        the sender intended, because the sender wrote one message.

        Refused when the show is READ rather than discovered during it: there is
        no reading of the file under which such a trigger does what it says. */
    TriggerRig rig;

    const auto mount = rig.document.createMount ("/ext/console", "namespaces/console.json");
    REQUIRE (mount.ok);

    const auto inside = rig.add (triggerKind::osc);
    rig.set (inside, "address", "/godot/cmd/go");

    auto problems = rig.document.validate();
    CHECK (problems.size() == 1u);
    CHECK (problems.front().find ("/godot/cmd/go") != std::string::npos);

    rig.set (inside, "address", "/ext/console/fader/1");
    problems = rig.document.validate();
    CHECK (problems.size() == 1u);
    CHECK (problems.front().find ("/ext/console") != std::string::npos);

    //  And an address of anybody else's is exactly what a trigger is for.
    rig.set (inside, "address", "/desk/go");
    CHECK (rig.document.validate().empty());
}
