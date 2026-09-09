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

/*  WHAT THE SHOW WOULD BE, AT A POSITION SOMEBODY NAMED.

    PRD §3.13. An operator says "take it back to cue 12" and means a STATE:
    which cues are sounding and how far in, what values the desk should hold,
    which levels a fade left somewhere other than where their cue says. The
    solver reconstructs that from the document alone - no engine, no audio, no
    network - which is why every case here builds a show and asks a question,
    and none of them ticks anything.

    THE COORDINATE IS A CUE AND AN OFFSET, and the two halves of that are worth
    separating: -1 means BEFORE the cue has fired, which is standby on it with
    nothing of it done, and 0 means it has just started. An operator asks for
    both and they are different positions.

    THE CONFUSED LIST IS THE FEATURE (§3.24): "a confused solver that says so is
    better than one that guesses." Four of these cases are about a thing the
    walk cannot know, and each asserts both halves - that it landed somewhere
    stated, and that it SAID it did not know.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/cue/ListState.h>
#include <wfg/engine/cue/Solver.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/tree/Mount.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <map>
#include <string>

using namespace wfg;

namespace
{
    /*  A list to point at, with the pieces a solve reasons about: values that
        get overwritten, a level a fade moves, a bed that never ends. */
    struct SolverRig
    {
        SolverRig()
        {
            listId = document.createList ("Main").id;
            durations["tone.wav"] = 4.0;
        }

        std::string osc (int index, const std::string& address, const std::string& atom)
        {
            const auto id = document.createCue (listId, index, "osc", "Desk").id;
            document.setAttribute ("/godot/cue/" + id + "/address", address);
            document.setAttribute ("/godot/cue/" + id + "/value", atom);
            return id;
        }

        std::string media (const std::string& parentId, int index, const std::string& name)
        {
            const auto id = document.createCue (parentId, index, "media", name).id;
            document.setAttribute ("/godot/cue/" + id + "/file", "tone.wav");
            return id;
        }

        cue::Plan solve (const std::string& cueId, double offset)
        {
            return cue::solve (document, &durations, nullptr, { listId, cueId, offset });
        }

        const cue::PlannedValue* valueAt (const cue::Plan& plan, const std::string& address)
        {
            for (const auto& value : plan.values)
                if (value.address == address)
                    return &value;

            return nullptr;
        }

        bool confusedAbout (const cue::Plan& plan, const std::string& cueId,
                            const char* why)
        {
            return std::any_of (plan.confused.begin(), plan.confused.end(),
                                [&cueId, why] (const cue::Confusion& c)
                                {
                                    return c.cue == cueId && c.reason == why;
                                });
        }

        doc::ShowDocument document;
        std::map<std::string, double> durations;
        std::string listId;
    };
}

//==============================================================================
TEST_CASE ("solve: the last writer wins, and the walk does not stop at a group boundary")
{
    /*  §3.13's step 1, and the half of a structural waypoint that is NOT true.

        A group boundary bounds what is RUNNING - the footer has run and nothing
        it started is still going - and bounds nothing at all about what is SET.
        A level somebody chose in act one is still chosen in act three, so the
        value walk is the whole list. Saying so with a test is what stops
        somebody later optimising it to stop at a boundary and quietly losing
        the value. */
    SolverRig rig;

    const auto early = rig.osc (0, "/desk/fader", "f:0.2");

    const auto scene = rig.document.createCue (rig.listId, 1, "group", "Scene").id;
    const auto inside = rig.document.createCue (scene, 0, "osc", "Inside").id;
    rig.document.setAttribute ("/godot/cue/" + inside + "/address", "/desk/other");
    rig.document.setAttribute ("/godot/cue/" + inside + "/value", "f:0.9");

    const auto later = rig.osc (2, "/desk/fader", "f:0.8");
    const auto target = rig.document.createCue (rig.listId, 3, "memo", "Here").id;

    const auto plan = rig.solve (target, -1.0);
    REQUIRE (plan.ok);

    /*  Two addresses, and the fader is the LATER value. */
    const auto* fader = rig.valueAt (plan, "/desk/fader");
    REQUIRE (fader != nullptr);
    CHECK (fader->value == osc::Value::float32 (0.8f));
    CHECK (fader->writer == later);

    /*  And the one written inside a group that has completed is still set. */
    const auto* other = rig.valueAt (plan, "/desk/other");
    REQUIRE (other != nullptr);
    CHECK (other->value == osc::Value::float32 (0.9f));

    juce::ignoreUnused (early);
}

TEST_CASE ("solve: -1 is before the cue and 0 is the moment it started")
{
    /*  The two halves of the coordinate, and they are different positions: one
        is standby on a cue with nothing of it done, the other is the cue
        sounding from its first sample. An operator asks for both. */
    SolverRig rig;

    const auto first = rig.osc (0, "/desk/fader", "f:0.2");
    const auto target = rig.osc (1, "/desk/other", "f:0.5");

    const auto before = rig.solve (target, -1.0);
    REQUIRE (before.ok);
    CHECK (rig.valueAt (before, "/desk/fader") != nullptr);
    CHECK (rig.valueAt (before, "/desk/other") == nullptr);   // it has not fired

    const auto after = rig.solve (target, 0.0);
    REQUIRE (after.ok);
    CHECK (rig.valueAt (after, "/desk/other") != nullptr);

    juce::ignoreUnused (first);
}

TEST_CASE ("solve: the pointer lands after the target, positionally")
{
    /*  §3.5. A jump puts the operator where the next GO would take the show on,
        which is what "take it back to cue 12" leaves them wanting. */
    SolverRig rig;

    const auto target = rig.document.createCue (rig.listId, 0, "memo", "Here").id;
    const auto next = rig.document.createCue (rig.listId, 1, "memo", "Next").id;

    CHECK (rig.solve (target, -1.0).standby == next);

    /*  At the end of a list there is nowhere to land, and that is an answer:
        §3.5 has no wrap. */
    CHECK (rig.solve (next, -1.0).standby.empty());
}

TEST_CASE ("solve: an event node is not a value, so a jump does not re-fire it")
{
    /*  §3.13 step 4. A jump is a statement about STATE, and "fire the pyro" has
        no state to restore to - re-sending it on the way past a cue that
        already happened would set the theatre alight. */
    SolverRig rig;

    tree::MountTable mounts;
    tree::MountDeclaration mount;
    mount.id = "K3PV7WRB";
    mount.prefix = "/desk";
    mount.namespaceFile = "namespaces/desk.json";
    mount.host = "127.0.0.1";
    mount.port = 9000;

    REQUIRE (mounts.load (mount, R"JSON({
      "FULL_PATH": "/desk",
      "CONTENTS": {
        "fader": { "FULL_PATH": "/desk/fader", "TYPE": "f", "ACCESS": 3 },
        "go":    { "FULL_PATH": "/desk/go", "TYPE": "f", "ACCESS": 2 }
      }
    })JSON").ok);

    rig.osc (0, "/desk/fader", "f:0.4");
    rig.osc (1, "/desk/go", "f:1");
    const auto target = rig.document.createCue (rig.listId, 2, "memo", "Here").id;

    const auto plan = cue::solve (rig.document, &rig.durations, &mounts,
                                  { rig.listId, target, -1.0 });

    REQUIRE (plan.ok);
    CHECK (rig.valueAt (plan, "/desk/fader") != nullptr);

    /*  Write-only with no value: the mount reader infers an EVENT, and the
        solver leaves it alone. */
    CHECK (rig.valueAt (plan, "/desk/go") == nullptr);
}

TEST_CASE ("solve: a finite cue fired at an earlier manual step is over")
{
    /*  §3.13's end-state rule, which is the only honest reading of a manual
        list: between two GOs there is however long the actor took, so a cue
        with an end has reached it. */
    SolverRig rig;

    const auto earlier = rig.media (rig.listId, 0, "Thunder");
    const auto target = rig.document.createCue (rig.listId, 1, "memo", "Here").id;

    const auto plan = rig.solve (target, -1.0);
    REQUIRE (plan.ok);

    CHECK (plan.runs.empty());
    juce::ignoreUnused (earlier);
}

TEST_CASE ("solve: a bed that plays for ever is still going, and says where it is not sure")
{
    /*  §3.24 as a case. Nought passes is for ever, which is what an ambience
        bed is - so it did NOT end at an earlier step. Which pass it is on, and
        how far into it, is a fact about how long somebody held the scene rather
        than about the show, so the solve lands at the start of the range and
        SAYS SO. */
    SolverRig rig;

    const auto bed = rig.media (rig.listId, 0, "Ambience");
    const auto range = rig.document.createRange (bed, 2.0, 6.0);
    REQUIRE (range.ok);
    rig.document.findById (range.id)
       .setProperty (juce::Identifier ("loops"), 0, nullptr);

    const auto target = rig.document.createCue (rig.listId, 1, "memo", "Here").id;

    const auto plan = rig.solve (target, -1.0);
    REQUIRE (plan.ok);

    REQUIRE (plan.runs.size() == 1);
    CHECK (plan.runs[0].cue == bed);
    CHECK (plan.runs[0].range == 0);
    CHECK (plan.runs[0].pass == 1);
    CHECK (plan.runs[0].offset == doctest::Approx (2.0));

    CHECK (rig.confusedAbout (plan, bed, cue::confusion::endlessRange));
}

TEST_CASE ("solve: a stop cue before the target ends what it names")
{
    /*  A release somebody wrote down. Without it, a bed stopped in act one
        would be planned as sounding in act three - which is the show playing
        something the designer explicitly silenced. */
    SolverRig rig;

    const auto bed = rig.media (rig.listId, 0, "Ambience");
    const auto range = rig.document.createRange (bed, 0.0, 4.0);
    REQUIRE (range.ok);
    rig.document.findById (range.id)
       .setProperty (juce::Identifier ("loops"), 0, nullptr);

    const auto halt = rig.document.createCue (rig.listId, 1, "stop", "Halt").id;
    rig.document.setAttribute ("/godot/cue/" + halt + "/target", bed);

    const auto target = rig.document.createCue (rig.listId, 2, "memo", "Here").id;

    CHECK (rig.solve (target, -1.0).runs.empty());
}

TEST_CASE ("solve: a fade leaves a trim behind, as base plus what it moved")
{
    /*  PR 3.12's shape. The cue says what somebody chose; the fade says where
        it was taken. The plan carries the DIFFERENCE, because that is the thing
        a level can be trimmed by and the thing a jump has to reproduce. */
    SolverRig rig;

    const auto bed = rig.media (rig.listId, 0, "Ambience");
    rig.document.setAttribute ("/godot/cue/" + bed + "/level", "-6");

    const auto fade = rig.document.createCue (rig.listId, 1, "fade", "Down").id;
    rig.document.setAttribute ("/godot/cue/" + fade + "/target", bed);
    rig.document.setAttribute ("/godot/cue/" + fade + "/level", "-18");

    const auto target = rig.document.createCue (rig.listId, 2, "memo", "Here").id;

    const auto plan = rig.solve (target, -1.0);
    REQUIRE (plan.trims.size() == 1);

    CHECK (plan.trims[0].cue == bed);
    CHECK (plan.trims[0].decibels == doctest::Approx (-12.0));
}

TEST_CASE ("solve: the target's own groups come with it")
{
    /*  A member sounds as part of its scene, so the plan has to say the scene
        is running too - that is what a run tree is, and what a load-to-time has
        to build. Outermost first, which is the order a GO would create them in. */
    SolverRig rig;

    const auto scene = rig.document.createCue (rig.listId, 0, "group", "Scene").id;
    const auto inner = rig.document.createCue (scene, 0, "group", "Inner").id;
    const auto sound = rig.media (inner, 0, "Thunder");

    const auto plan = rig.solve (sound, 1.5);
    REQUIRE (plan.ok);

    REQUIRE (plan.runs.size() == 3);
    CHECK (plan.runs[0].cue == scene);
    CHECK (plan.runs[1].cue == inner);
    CHECK (plan.runs[2].cue == sound);
    CHECK (plan.runs[2].offset == doctest::Approx (1.5));
    CHECK (plan.runs[2].ancestors == std::vector<std::string> { scene, inner });
}

TEST_CASE ("solve: an offset lands in the right range and the right pass")
{
    /*  A cue with ranges is a playlist over one file (§3.24), so where five
        seconds into it lands is arithmetic over the passes in order. */
    SolverRig rig;

    const auto cueId = rig.media (rig.listId, 0, "Playlist");

    //  Two seconds, played twice; then three seconds, played once.
    const auto first = rig.document.createRange (cueId, 0.0, 2.0);
    REQUIRE (first.ok);
    rig.document.findById (first.id)
       .setProperty (juce::Identifier ("loops"), 2, nullptr);

    const auto second = rig.document.createRange (cueId, 10.0, 13.0);
    REQUIRE (second.ok);

    const auto at = [&rig, &cueId] (double offset)
    {
        const auto plan = rig.solve (cueId, offset);
        REQUIRE (! plan.runs.empty());
        return plan.runs.back();
    };

    CHECK (at (0.5).range == 0);
    CHECK (at (0.5).pass == 1);
    CHECK (at (0.5).offset == doctest::Approx (0.5));

    CHECK (at (2.5).range == 0);
    CHECK (at (2.5).pass == 2);              // the second pass of the first range
    CHECK (at (2.5).offset == doctest::Approx (0.5));

    CHECK (at (5.0).range == 1);
    CHECK (at (5.0).pass == 1);
    CHECK (at (5.0).offset == doctest::Approx (11.0));
}

TEST_CASE ("solve: a media file with no length says it does not know")
{
    /*  A file that is missing, or in a format this build has no reader for, is
        nought seconds and a confused entry - never a refusal and never a guess.
        The same rule as a missing media file, which has failed the ARM and not
        the load since Phase 2. */
    SolverRig rig;

    const auto cueId = rig.document.createCue (rig.listId, 0, "media", "Nowhere").id;
    rig.document.setAttribute ("/godot/cue/" + cueId + "/file", "absent.wav");

    const auto plan = rig.solve (cueId, 1.0);
    REQUIRE (plan.ok);

    CHECK (rig.confusedAbout (plan, cueId, cue::confusion::unknownLength));
}

TEST_CASE ("solve: a shuffled group with no seed says the order is not in the document")
{
    /*  §3.6: a seed of nought means a fresh order drawn when the run starts and
        written into its log - so a shuffled show is different every night AND
        every night reproduces exactly. What it is NOT is in the document, so a
        solve of a group that ran earlier cannot know which members played. */
    SolverRig rig;

    const auto scene = rig.document.createCue (rig.listId, 0, "group", "Scene").id;
    rig.document.setAttribute ("/godot/cue/" + scene + "/selection", "shuffle");

    const auto sound = rig.media (scene, 0, "Thunder");

    const auto plan = rig.solve (sound, 0.5);
    REQUIRE (plan.ok);

    CHECK (rig.confusedAbout (plan, scene, cue::confusion::drawnOrder));

    /*  And a seed somebody chose is a decision the document holds, so it is not
        confused about it at all. */
    rig.document.setAttribute ("/godot/cue/" + scene + "/seed", "7");
    CHECK_FALSE (rig.confusedAbout (rig.solve (sound, 0.5), scene,
                                    cue::confusion::drawnOrder));
}

TEST_CASE ("solve: a looping group says nothing tells it which round")
{
    /*  Standby says which member; nothing says which round. It takes the first
        and says so, which is §3.24's rule again in a different clothes. */
    SolverRig rig;

    const auto scene = rig.document.createCue (rig.listId, 0, "group", "Scene").id;
    rig.document.setAttribute ("/godot/cue/" + scene + "/loops", "0");

    const auto sound = rig.media (scene, 0, "Thunder");

    CHECK (rig.confusedAbout (rig.solve (sound, 0.5), scene, cue::confusion::unknownRound));
}

TEST_CASE ("solve: an aim at nothing is not a plan")
{
    /*  A list that is not there, or a cue that is not in it, answers "no" -
        rather than an empty plan that a caller could mistake for "the show is
        silent and every value is at its default". */
    SolverRig rig;

    const auto cueId = rig.document.createCue (rig.listId, 0, "memo", "Here").id;

    CHECK_FALSE (cue::solve (rig.document, &rig.durations, nullptr,
                             { "ZZ999999", cueId, -1.0 }).ok);
    CHECK_FALSE (rig.solve ("ZZ999999", -1.0).ok);
}

TEST_CASE ("solve: an aim spells the same way it reads")
{
    /*  Both nodes carry `"<cue> <offset>"`, and the offset goes through the
        project's own formatter - a `std::to_string` would put a comma in it
        under `fr_FR` and a client would read three fields where there are two. */
    const cue::ListAim aim { "F7HR8TVD", 1.25 };

    const auto spelt = cue::spellAim (aim);
    CHECK (spelt == "F7HR8TVD 1.25");

    const auto read = cue::readAim (spelt);
    CHECK (read.cue == aim.cue);
    CHECK (read.offset == doctest::Approx (aim.offset));

    /*  Half an aim is not an aim: the difference between -1 and nought is the
        difference between standby on a cue and the cue sounding, so it has to
        be written down. */
    CHECK_FALSE (cue::readAim ("F7HR8TVD").isSet());
    CHECK_FALSE (cue::readAim ("").isSet());
    CHECK_FALSE (cue::readAim (" 1.0").isSet());
}

TEST_CASE ("solve: inside a timeline group, what else is sounding is arithmetic")
{
    /*  THE ONE PLACE WHERE "EVERYTHING BEFORE THE TARGET IS OVER" IS FALSE, and
        it is false because there was no GO in between to make it true. A
        timeline group schedules its members at entry, each at its own offset
        (PRD 3.6), so the document knows exactly what else is going when one of
        them is `offset` seconds in.

        A load-to-time into the middle of a scene depends on this entirely:
        without it the jump would land the cue somebody asked for and silence
        everything it was playing against. */
    SolverRig rig;

    const auto scene = rig.document.createCue (rig.listId, 0, "group", "Scene").id;
    rig.document.setAttribute ("/godot/cue/" + scene + "/mode", "timeline");

    //  Three four-second members, at nought, two and ten seconds in.
    const auto early = rig.media (scene, 0, "One");
    const auto middle = rig.media (scene, 1, "Two");
    const auto late = rig.media (scene, 2, "Three");

    rig.document.setAttribute ("/godot/cue/" + middle + "/preWait", "2");
    rig.document.setAttribute ("/godot/cue/" + late + "/preWait", "10");

    const auto sounding = [] (const cue::Plan& plan)
    {
        std::vector<std::string> out;

        for (const auto& run : plan.runs)
            if (! run.ancestors.empty())
                out.push_back (run.cue);

        std::sort (out.begin(), out.end());
        return out;
    };

    /*  ONE SECOND INTO THE SECOND MEMBER IS THREE INTO THE SCENE: the first is
        still going - it ends at four - and the third has not started. */
    auto expected = std::vector<std::string> { early, middle };
    std::sort (expected.begin(), expected.end());
    CHECK (sounding (rig.solve (middle, 1.0)) == expected);

    /*  Three seconds in is five into the scene, and the first has ended: only
        the second is left. */
    CHECK (sounding (rig.solve (middle, 3.0)) == std::vector<std::string> { middle });

    /*  And the offset a member gets is where IT has got to, not where the aim
        is: the first member is three seconds into its own file when the second
        is one second into its. */
    const auto plan = rig.solve (middle, 1.0);

    for (const auto& run : plan.runs)
        if (run.cue == early)
            CHECK (run.offset == doctest::Approx (3.0));
}

//==============================================================================
TEST_CASE ("M20: what a solve costs, over a show nobody wants to find out about on the night")
{
    /*  MEASUREMENT M20 (§13.14). A solve sits behind a dragged finger - the
        live answer of §3.17's dual-touch gesture - so it has to fit inside a
        tick with room. Reported and not gated: a wall-clock threshold on a
        shared CI runner is a flaky test that teaches people to re-run the
        suite, and what this is for is that a client dragging an aim knows what
        it is asking for. */
    SolverRig rig;

    constexpr int cues = 500;

    auto list = rig.document.findById (rig.listId);
    REQUIRE (list.isValid());

    /*  Built straight into the tree, for the reason M18's show is: every write
        door finds its parent with `findById`, which is a walk of the whole
        show, so five hundred cues through the door measures the door. */
    for (int n = 0; n < cues; ++n)
    {
        juce::ValueTree cue { n % 3 == 0 ? "Osc" : "Media" };
        cue.setProperty (juce::Identifier ("id"),
                         juce::String ("SA" + std::to_string (100000 + n)), nullptr);

        if (n % 3 == 0)
        {
            cue.setProperty (juce::Identifier ("address"),
                             juce::String ("/desk/" + std::to_string (n % 40)), nullptr);
            cue.setProperty (juce::Identifier ("value"), "f:0.5", nullptr);
        }
        else
        {
            cue.setProperty (juce::Identifier ("file"), "tone.wav", nullptr);
        }

        list.addChild (cue, -1, nullptr);
    }

    const auto target = "SA" + std::to_string (100000 + cues - 1);

    /*  Warm, then measured: what is wanted is the cost of a drag, and a drag is
        the second solve onwards. */
    rig.solve (target, -1.0);

    constexpr int solves = 20;
    const auto before = juce::Time::getMillisecondCounterHiRes();

    for (int n = 0; n < solves; ++n)
        rig.solve (target, -1.0);

    const auto each = (juce::Time::getMillisecondCounterHiRes() - before) / solves;

    const auto plan = rig.solve (target, -1.0);
    CHECK (plan.ok);

    MESSAGE ("M20  " << cues << " cues: " << each << " ms per solve in a DEBUG build with"
                " iterator debugging on, which is where every number in this suite is taken and"
                " is roughly an order of magnitude above the shipped one; "
             << plan.values.size() << " distinct addresses out of " << (cues / 3 + 1)
             << " network cues, " << plan.runs.size() << " runs, "
             << plan.confused.size() << " confused entries. The node is capped at 5 Hz and a"
                " drag re-solves at the drag's own rate, so what this has to fit inside is a"
                " gesture rather than a tick");
}
