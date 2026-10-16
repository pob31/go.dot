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
#include <wfg/engine/cue/KeyChord.h>
#include <wfg/engine/cue/TriggerIndex.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/ShowDocument.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

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

    /*  A reading of the clocks, said the way a case thinks of one: a time of
        day, where the steady clock stood, and the date. The steady clock's
        origin is anything at all; only its advance between two readings is
        ever asked. */
    constexpr int october25 = 20261025;

    ClockReading secondAt (int secondOfDay, std::int64_t steadyMilliseconds, int date = october25)
    {
        ClockReading reading;
        reading.date = date;
        reading.millisecondOfDay = static_cast<std::int64_t> (secondOfDay) * 1000;
        reading.steadyMilliseconds = steadyMilliseconds;
        return reading;
    }

    ClockReading clockAt (const char* timeOfDay, std::int64_t steadyMilliseconds, int date = october25)
    {
        const auto second = secondOfDayFor (timeOfDay);
        REQUIRE (second >= 0);
        return secondAt (second, steadyMilliseconds, date);
    }

    std::vector<std::string> idsOf (const ClockCrossings& crossings)
    {
        std::vector<std::string> ids;

        for (const auto& crossing : crossings.crossed)
            ids.push_back (crossing.id);

        return ids;
    }

    /*  Time passing from one second of the day to another: the steady clock
        goes on exactly as far as the wall clock does, the near way forward
        round the dial, so nothing here is a step. */
    std::vector<std::string> crossedBetween (const TriggerIndex& index, int from, int to)
    {
        const auto advance = ((to - from) % 86400 + 86400) % 86400;
        const auto crossings = clockCrossings (index, secondAt (from % 86400, 0),
                                               secondAt (to % 86400, static_cast<std::int64_t> (advance) * 1000));
        CHECK_FALSE (crossings.stepped);
        return idsOf (crossings);
    }

    /*  A show with a clock trigger at each of these times, in this order: the
        shape every case below about steps wants, since what a step used to do
        was fire nearly all of them. */
    struct ClockRig : TriggerRig
    {
        explicit ClockRig (std::vector<const char*> times)
        {
            for (const auto* time : times)
            {
                const auto id = add (triggerKind::clock);
                set (id, "at", time);
                ids.push_back (id);
            }
        }

        std::vector<std::string> ids;
    };
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
    CHECK (crossedBetween (*index, at - 1, at) == std::vector<std::string> { id });
    CHECK (crossedBetween (*index, at, at + 1).empty());
    CHECK (crossedBetween (*index, at, at).empty());
    CHECK (crossedBetween (*index, at - 1, at - 1).empty());

    //  And a gap - a machine that went to sleep, a reading that was late -
    //  still crosses it exactly once.
    CHECK (crossedBetween (*index, at - 60, at + 60) == std::vector<std::string> { id });
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
    CHECK (crossedBetween (*index, 86399, 1) == std::vector<std::string> { midnight });

    //  And a wider wrap takes both.
    CHECK (crossedBetween (*index, 86390, 10)
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
        CHECK (crossedBetween (*index, second, second + 997).empty());
}

//==============================================================================
/*  THE WALL CLOCK'S STEP RULE (namespace draft §60, AIY).

    The wall clock is the one clock in the machine somebody else may move, and
    until 2026-10-12 a step BACK of it landed in the branch meant for midnight
    and fired every clock trigger in the show but the ones it stepped over. The
    steady clock, read beside it, tells a step from time passing: when time
    passes they advance alike. Every case below is a pair of readings a test
    made up, which is the point of the rule being a pure function.
*/
TEST_CASE ("clock: the night summer time ends fires nothing, and says it stepped")
{
    /*  02:59:59 is followed by 02:00:00 one second later. Read as two seconds
        of the day, that was "previous greater than now", which was midnight,
        and midnight fired everything after 02:59:59 and everything up to
        02:00:00 - the cue at noon, the cue at half past seven, the one at
        midnight - on the night the clocks went back. */
    ClockRig rig ({ "00:00:00", "02:00:00", "02:30:00", "12:00:00", "19:30:00" });
    const auto index = rig.index();

    const auto crossings = clockCrossings (*index, clockAt ("02:59:59", 5'000),
                                                   clockAt ("02:00:00", 6'000));

    CHECK (crossings.crossed.empty());
    CHECK (crossings.stepped);

    //  The wall clock went back 3599 s while the steady clock went on 1 s.
    CHECK (crossings.stepMilliseconds == -3'600'000);

    //  And the next tick is time passing again, from where the step landed.
    const auto after = clockCrossings (*index, clockAt ("02:00:00", 6'000),
                                               clockAt ("02:00:01", 7'000));
    CHECK_FALSE (after.stepped);
    CHECK (after.crossed.empty());
}

TEST_CASE ("clock: summer time starting fires nothing across the hour it skips")
{
    /*  01:59:59 is followed by 03:00:00. The rule fires nothing across a step
        whichever way it went, so the cue at half past two is NOT fired that
        night - nor the one at three, which the step landed on rather than
        reached (AIY: the author may overrule this direction). */
    ClockRig rig ({ "02:30:00", "03:00:00", "19:30:00" });
    const auto index = rig.index();

    const auto crossings = clockCrossings (*index, clockAt ("01:59:59", 0),
                                                   clockAt ("03:00:00", 1'000));

    CHECK (crossings.crossed.empty());
    CHECK (crossings.stepped);
    CHECK (crossings.stepMilliseconds == 3'600'000);
}

TEST_CASE ("clock: a time-sync step of five seconds, back or forward, fires nothing")
{
    ClockRig rig ({ "19:30:00", "19:30:02", "19:30:05", "12:00:00" });
    const auto index = rig.index();

    //  Back: 19:30:05 to 19:30:00 inside one tick of the steady clock.
    const auto back = clockCrossings (*index, clockAt ("19:30:05", 100'000),
                                              clockAt ("19:30:00", 100'020));
    CHECK (back.stepped);
    CHECK (back.crossed.empty());
    CHECK (back.stepMilliseconds == -5'020);

    //  Forward: 19:30:00 to 19:30:05 inside one tick.
    const auto forward = clockCrossings (*index, clockAt ("19:30:00", 100'000),
                                                 clockAt ("19:30:05", 100'020));
    CHECK (forward.stepped);
    CHECK (forward.crossed.empty());
    CHECK (forward.stepMilliseconds == 4'980);
}

TEST_CASE ("clock: a nudge back under two seconds is no step, crosses nothing, and fires nothing twice")
{
    /*  The smallest case of the fault: one second back, from 19:30:05 to
        19:30:04, fired nearly every trigger in the show. Under the
        threshold it is not a step - the steady clock went on a tick - but the
        wall clock went back, so it crosses nothing at all. */
    ClockRig rig ({ "19:30:00", "19:30:05", "12:00:00", "00:00:00" });
    const auto index = rig.index();

    const auto back = clockCrossings (*index, clockAt ("19:30:05", 0), clockAt ("19:30:04", 20));
    CHECK_FALSE (back.stepped);
    CHECK (back.crossed.empty());

    /*  And going on from there crosses 19:30:05 a second time. The interval
        says so; the day remembers it fired, so it fires once. */
    ClockDays days;
    CHECK (days.admit (*index, clockCrossings (*index, clockAt ("19:30:04", 0), clockAt ("19:30:05", 1'000)).crossed)
             == std::vector<std::string> { rig.ids[1] });

    const auto again = clockCrossings (*index, clockAt ("19:30:04", 1'020), clockAt ("19:30:05", 2'020));
    CHECK (idsOf (again) == std::vector<std::string> { rig.ids[1] });
    CHECK (days.admit (*index, again.crossed).empty());
}

TEST_CASE ("clock: midnight counts when the steady clock crosses it, and only then")
{
    ClockRig rig ({ "00:00:00", "12:00:00", "23:59:59" });
    const auto index = rig.index();

    //  23:59:59 to 00:00:00 with a second of the steady clock: the day turned.
    const auto turned = clockCrossings (*index, clockAt ("23:59:59", 0, october25),
                                                clockAt ("00:00:00", 1'000, october25 + 1));
    CHECK_FALSE (turned.stepped);
    CHECK (idsOf (turned) == std::vector<std::string> { rig.ids[0] });

    //  A crossing falls on the date of its side of midnight.
    const auto wide = clockCrossings (*index, clockAt ("23:59:58", 0, october25),
                                              clockAt ("00:00:01", 3'000, october25 + 1));
    REQUIRE (wide.crossed.size() == 2u);
    CHECK (wide.crossed[0].id == rig.ids[0]);
    CHECK (wide.crossed[0].date == october25 + 1);
    CHECK (wide.crossed[1].id == rig.ids[2]);
    CHECK (wide.crossed[1].date == october25);

    /*  23:00:00 to 01:00:00 in a second of the steady clock is the wall clock
        moved two hours on, not a night that passed: nothing, midnight's cue
        included. */
    const auto moved = clockCrossings (*index, clockAt ("23:00:00", 0, october25),
                                               clockAt ("01:00:00", 1'000, october25 + 1));
    CHECK (moved.stepped);
    CHECK (moved.crossed.empty());

    //  And back across midnight the other way - 00:30:00 to 23:30:00 - likewise.
    const auto backAcross = clockCrossings (*index, clockAt ("00:30:00", 0, october25 + 1),
                                                    clockAt ("23:30:00", 1'000, october25));
    CHECK (backAcross.stepped);
    CHECK (backAcross.crossed.empty());
}

TEST_CASE ("clock: a slow tick is time passing, and its seconds' triggers fire")
{
    /*  The hook not run for three seconds - a machine that was busy - moves
        both clocks three seconds. They agree, and the three seconds' triggers
        fire, each once. */
    ClockRig rig ({ "19:30:00", "19:30:01", "19:30:02", "19:30:03", "19:30:04" });
    const auto index = rig.index();

    const auto crossings = clockCrossings (*index, clockAt ("19:30:00", 40'000),
                                                   clockAt ("19:30:03", 43'000));
    CHECK_FALSE (crossings.stepped);
    CHECK (idsOf (crossings) == std::vector<std::string> { rig.ids[1], rig.ids[2], rig.ids[3] });
}

TEST_CASE ("clock: a slew of fifty parts per million over an hour fires every trigger once")
{
    /*  A time-sync service that disciplines the clock by its rate rather than
        by a step: the wall clock runs 50 ppm fast, or slow, against the steady
        clock for an hour, read every tenth of a second. Each reading agrees
        with the steady clock to a few microseconds, so nothing is ever a step,
        and a trigger every thirty seconds fires exactly once. */
    std::vector<std::string> times;

    for (int second = 19 * 3600 + 30; second < 20 * 3600; second += 30)
    {
        const auto twoDigits = [] (int value) { return (value < 10 ? "0" : "") + std::to_string (value); };
        times.push_back (twoDigits (second / 3600) + ":" + twoDigits (second / 60 % 60) + ":"
                         + twoDigits (second % 60));
    }

    std::vector<const char*> names;

    for (const auto& time : times)
        names.push_back (time.c_str());

    ClockRig rig (names);
    const auto index = rig.index();
    REQUIRE (rig.ids.size() == 119u);

    for (const std::int64_t partsFast : { 50, -50 })
    {
        CAPTURE (partsFast);

        std::map<std::string, int> fires;
        ClockDays days;
        bool stepped = false;
        const std::int64_t start = 19LL * 3600 * 1000;
        auto previous = ClockReading { october25, start, 0 };

        for (std::int64_t steady = 100; steady <= 3'600'000 + 1'000; steady += 100)
        {
            //  Whole milliseconds, as a reading has them: 50 ppm is one in 20 000.
            const auto now = ClockReading { october25, start + steady + steady * partsFast / 1'000'000, steady };
            const auto crossings = clockCrossings (*index, previous, now);
            stepped = stepped || crossings.stepped;

            for (const auto& id : days.admit (*index, crossings.crossed))
                ++fires[id];

            previous = now;
        }

        CHECK_FALSE (stepped);
        CHECK (fires.size() == rig.ids.size());

        for (const auto& [id, count] : fires)
        {
            CAPTURE (id);
            CHECK (count == 1);
        }
    }
}

TEST_CASE ("clock: a trigger fires once a calendar day, and again the next")
{
    /*  §12.8 said it from the start - "fires once each day it is crossed" -
        and nothing kept it until now. Kept by `ClockDays`, so an hour the wall
        clock replays cannot fire a trigger twice even if a step ever got past
        the steady clock. */
    ClockRig rig ({ "02:30:00", "19:30:00" });
    const auto index = rig.index();
    ClockDays days;

    const auto crossing = clockCrossings (*index, clockAt ("02:29:59", 0), clockAt ("02:30:00", 1'000));
    REQUIRE (idsOf (crossing) == std::vector<std::string> { rig.ids[0] });

    CHECK (days.admit (*index, crossing.crossed) == std::vector<std::string> { rig.ids[0] });

    //  The same interval forced through again, on the same date: nothing.
    CHECK (days.admit (*index, crossing.crossed).empty());
    CHECK (days.admit (*index, clockCrossings (*index, clockAt ("02:29:00", 2'000), clockAt ("02:31:00", 122'000)).crossed)
             .empty());

    //  The next date, it fires again.
    const auto tomorrow = clockCrossings (*index, clockAt ("02:29:59", 86'400'000, october25 + 1),
                                                  clockAt ("02:30:00", 86'401'000, october25 + 1));
    CHECK (days.admit (*index, tomorrow.crossed) == std::vector<std::string> { rig.ids[0] });
}

TEST_CASE ("clock: a trigger re-timed fires on its new time the same day")
{
    /*  In a tech, somebody sets a trigger a minute ahead to try it, then a
        minute ahead again. The second time is a new decision and fires; the
        memory is of a trigger AT a time, not of a trigger. */
    ClockRig rig ({ "19:30:00" });
    ClockDays days;

    CHECK (days.admit (*rig.index(), clockCrossings (*rig.index(), clockAt ("19:29:59", 0),
                                                                   clockAt ("19:30:00", 1'000)).crossed)
             == rig.ids);

    rig.set (rig.ids[0], "at", "19:31:00");
    const auto index = rig.index();

    CHECK (days.admit (*index, clockCrossings (*index, clockAt ("19:30:59", 60'000),
                                                       clockAt ("19:31:00", 61'000)).crossed)
             == rig.ids);
}

TEST_CASE ("clock: a step is said in a sentence the log can carry")
{
    const auto back = clockStepNote (clockAt ("02:59:59", 0), clockAt ("02:00:00", 1'000), -3'600'000);
    CAPTURE (back);
    CHECK (back.find ("back") != std::string::npos);
    CHECK (back.find ("02:59:59") != std::string::npos);
    CHECK (back.find ("02:00:00") != std::string::npos);
    CHECK (back.find ("3600") != std::string::npos);

    const auto forward = clockStepNote (clockAt ("01:59:59", 0), clockAt ("03:00:00", 1'000), 3'600'000);
    CAPTURE (forward);
    CHECK (forward.find ("forward") != std::string::npos);
    CHECK (forward.find ("not fired") != std::string::npos);
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
    CHECK (crossedBetween (*index, 19 * 3600 + 30 * 60 - 1, 19 * 3600 + 30 * 60)
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

        std::string runOf (const std::string& wanted) const
        {
            for (const auto& run : runs.all())
                if (run.cue == wanted)
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

TEST_CASE ("trigger.fire: a disabled trigger is refused, and nothing runs")
{
    /*  Until 2026-10-12 only the matchers asked: a disabled trigger matched
        nothing, but a `trigger.fire` sent by hand, or replayed from a log,
        fired it all the same (namespace draft §60, AIY). The command asks
        now, with the word a disabled cue is refused with - read through the
        document, since a trigger nobody has touched holds no `enabled` at
        all and the ValueTree would answer false for it. */
    FiringRig rig;
    const auto id = rig.add (triggerKind::clock);
    rig.set (id, "at", "19:30:00");
    rig.set (id, "enabled", "false");

    const auto outcome = rig.tick ("trigger.fire", { osc::Value::string (id) });
    CHECK (outcome.rejected == 1);
    CHECK (outcome.applied == 0);
    CHECK (rig.engine.lastError().find (reason::disabled) != std::string::npos);
    CHECK (rig.runs.all().empty());

    //  Switched back on, the same command fires it.
    rig.set (id, "enabled", "true");
    CHECK (rig.tick ("trigger.fire", { osc::Value::string (id) }).applied == 1);
    CHECK (! rig.runOf (rig.cueId).empty());

    //  And one never written to is enabled: its default, read through the document.
    const auto untouched = rig.add (triggerKind::osc, rig.other);
    CHECK (rig.tick ("trigger.fire", { osc::Value::string (untouched) }).rejected == 0);
    CHECK (! rig.runOf (rig.other).empty());
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

//==============================================================================
/*  KEY TRIGGERS (namespace draft §74, AQK, AQW): one spelling for a key, the
    page's, and never a key the show keeps. */
TEST_CASE ("key chord: one spelling, whatever order and case it came in")
{
    CHECK (canonicalChord ("shift+mod+k") == std::optional<std::string> ("Mod+Shift+K"));
    CHECK (canonicalChord ("ctrl+F6") == std::optional<std::string> ("Mod+F6"));
    CHECK (canonicalChord ("Cmd+Option+1") == std::optional<std::string> ("Mod+Alt+1"));
    CHECK (canonicalChord ("num3") == std::optional<std::string> ("Num3"));
    CHECK (canonicalChord ("arrowleft") == std::optional<std::string> ("ArrowLeft"));
    CHECK (canonicalChord ("F24") == std::optional<std::string> ("F24"));

    for (const auto* nonsense : { "", "F25", "F0", "Mod+", "+K", "Hyper+K", ",", "Mod+Shift", "Num10" })
    {
        INFO (nonsense);
        CHECK_FALSE (canonicalChord (nonsense).has_value());
    }

    CHECK (chordOf (true, false, true, "k") == std::optional<std::string> ("Mod+Shift+K"));
    CHECK_FALSE (chordOf (false, false, false, "comma").has_value());

    //  GO, Esc and Doh! whatever the modifiers, and every shortcut either client answers to.
    for (const auto* kept : { "Space", "Mod+Space", "Shift+Escape", "Escape", "F9", "Alt+F9", "F5", "F",
                              "Shift+F", "ArrowUp", "ArrowDown", "Mod+Z", "Mod+Shift+Z", "Mod+S", "Mod+T", "Delete" })
    {
        INFO (kept);
        CHECK (isReservedChord (kept));
    }

    for (const auto* free : { "F6", "Mod+K", "Shift+1", "ArrowLeft", "Num1", "G", "Alt+F", "Mod+F6" })
    {
        INFO (free);
        CHECK_FALSE (isReservedChord (free));
    }
}

TEST_CASE ("key trigger: its key is spelled once, is never one the show keeps, and can be born with it")
{
    TriggerRig rig;
    const auto id = rig.add (triggerKind::key);

    const auto key = "/godot/trigger/" + id + "/key";

    CHECK (rig.document.setAttribute (key, "F6").ok);
    CHECK (rig.document.setAttribute (key, "Mod+Shift+K").ok);
    CHECK (rig.document.setAttribute (key, "").ok);

    //  Not the spelling, a key the show keeps, no key at all: refused, and the key stays.
    REQUIRE (rig.document.setAttribute (key, "F7").ok);

    for (const auto* refused : { "shift+mod+k", "Space", "F9", "Mod+S", "Comma" })
    {
        INFO (refused);
        CHECK_FALSE (rig.document.setAttribute (key, refused).ok);
    }

    CHECK (rig.document.getAttribute (key) == std::optional<std::string> ("F7"));

    //  Born with its key, in one step; and born with a key the show keeps, not at all.
    const auto born = rig.document.createTrigger (rig.cueId, triggerKind::key, {}, { { "key", "F8" } });
    REQUIRE (born.ok);
    CHECK (rig.document.getAttribute ("/godot/trigger/" + born.id + "/key") == std::optional<std::string> ("F8"));

    CHECK_FALSE (rig.document.createTrigger (rig.cueId, triggerKind::key, {}, { { "key", "Escape" } }).ok);
    CHECK_FALSE (rig.document.createTrigger (rig.cueId, triggerKind::key, {}, { { "kind", "osc" } }).ok);

    //  The engine's own inputs never answer one.
    const auto index = rig.index();
    CHECK (matchOsc (*index, "/desk/go", noArgs()).empty());
}

TEST_CASE ("key trigger: trigger.create makes one with its key, and trigger.fire fires its cue, standby still")
{
    FiringRig rig;
    REQUIRE (rig.document.setAttribute (standbyAddressOf (rig.listId), rig.other).ok);

    CHECK (rig.tick ("trigger.create", { osc::Value::string (rig.cueId), osc::Value::string ("key"),
                                         osc::Value::string (""), osc::Value::string ("key"),
                                         osc::Value::string ("F6") }).rejected == 0);

    const auto trigger = rig.document.findById (rig.cueId).getChildWithName ("Trigger");
    REQUIRE (trigger.isValid());
    const auto id = trigger["id"].toString().toStdString();
    CHECK (rig.document.getAttribute ("/godot/trigger/" + id + "/key") == std::optional<std::string> ("F6"));

    CHECK (rig.tick ("trigger.fire", { osc::Value::string (id) }).rejected == 0);
    CHECK (! rig.runOf (rig.cueId).empty());
    CHECK (rig.document.getAttribute ("/godot/list/" + rig.listId + "/standby") == rig.other);

    //  Its key let go: nothing to release on a memo, and no mistake.
    CHECK (rig.tick ("trigger.release", { osc::Value::string (id) }).rejected == 0);
}
