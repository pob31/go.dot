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

/*  WHICH CUES CAN BE HOLDING ONE SLOT AT ONCE, WORKED OUT BY READING.

    PRD §3.9c: each binding has a live range over show time, overlapping ranges
    cannot share a resource, and re-analysis on edit is what turns a reorder
    into a warning. This file is that analysis.

    THE TWO COORDINATES, and why there are two. A manual cue list has no time in
    it: between one GO and the next there is a person, and a person is not a
    duration. So across a manual boundary a claim is measured in ROWS - from the
    row of the cue that takes the slot to the row where the structure guarantees
    it is back. Inside a timeline or an automatic chain there is no person, so
    offsets and durations are exact and the analysis uses seconds. The
    difference is visible here as a pair of cases that are the same show twice:
    two members of an automatic sequence do not overlap when their lengths are
    known, and DO when they are not.

    WRONG IN ONE DIRECTION ONLY, on purpose. The analysis proves possible
    overlap and can never prove impossible, so every fallback in it leans
    towards reporting. Which is why an overlap is a warning rather than a
    refusal, why it leaves `wfg validate`'s exit code alone, and why `shared` on
    either cue's `Feed` or `Insert` silences the pair - a designer saying,
    permanently and in the document, that they considered it.

    Claims at RUN time are GoTests' (`claim:`); the pool and what a show may say
    about it are SlotTests'. This is the third of the three and the only one
    that never starts an engine.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/SlotAnalysis.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/cue/Run.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <map>
#include <string>

using namespace wfg;

namespace
{
    /*  A show with one mount carrying two processor inputs, one rack channel,
        and a bus wide enough to feed them. Nothing runs; there is no engine
        under any of this. */
    struct AnalysisRig
    {
        AnalysisRig()
        {
            listId = document.createList ("Main").id;

            const auto mount = document.createMount ("/wfs", "namespaces/wfs.json");
            REQUIRE (mount.ok);
            document.findById (mount.id)
                    .setProperty (juce::Identifier ("port"), 9000, nullptr);

            auto audio = document.root().getChildWithName ("Audio");
            REQUIRE (audio.isValid());

            juce::ValueTree bus { "Bus" };
            bus.setProperty (juce::Identifier ("id"), "BS000001", nullptr);
            bus.setProperty (juce::Identifier ("name"), "WFS send", nullptr);
            bus.setProperty (juce::Identifier ("firstChannel"), 0, nullptr);
            bus.setProperty (juce::Identifier ("width"), 12, nullptr);
            audio.addChild (bus, -1, nullptr);

            slotA = declareSlot (mount.id, "/wfs/input/3", 2);
            slotB = declareSlot (mount.id, "/wfs/input/5", 4);

            const auto channel = document.createRackChannel ("mono");
            REQUIRE (channel.ok);
            channelId = channel.id;

            /*  One file, four seconds long. The side table is keyed by the path
                the document writes, so every media cue naming it is four
                seconds - and a cue naming anything else is a length the
                analysis does not know, which is the case half of these tests
                are about. */
            durations["tone.wav"] = 4.0;
        }

        std::string declareSlot (const std::string& mountId,
                                 const std::string& address, int firstChannel)
        {
            const auto slot = document.createSlot (mountId, address);
            REQUIRE (slot.ok);

            auto node = document.findById (slot.id);
            node.setProperty (juce::Identifier ("bus"), "BS000001", nullptr);
            node.setProperty (juce::Identifier ("width"), 1, nullptr);
            node.setProperty (juce::Identifier ("firstChannel"), firstChannel, nullptr);
            return slot.id;
        }

        /** A media cue that plays the four-second file. */
        std::string media (const std::string& parentId, int index, const std::string& name)
        {
            const auto cue = document.createCue (parentId, index, "media", name);
            REQUIRE (cue.ok);
            document.setAttribute ("/godot/cue/" + cue.id + "/file", "tone.wav");
            return cue.id;
        }

        /** And what it feeds. */
        std::string feed (const std::string& cueId, const std::string& slotId,
                          bool shared = false)
        {
            const auto edit = document.createFeed (cueId, slotId);
            REQUIRE (edit.ok);

            auto node = document.findById (edit.id);
            node.setProperty (juce::Identifier ("gains"), "1", nullptr);

            if (shared)
                node.setProperty (juce::Identifier ("shared"), true, nullptr);

            return edit.id;
        }

        std::string group (const std::string& parentId, int index, const std::string& name,
                           const char* mode, const char* advance)
        {
            const auto cue = document.createCue (parentId, index, "group", name);
            REQUIRE (cue.ok);

            auto node = document.findById (cue.id);
            node.setProperty (juce::Identifier ("mode"), mode, nullptr);
            node.setProperty (juce::Identifier ("advance"), advance, nullptr);
            return cue.id;
        }

        /** The analysis, rebuilt if the show has moved. With the durations. */
        const cue::SlotAnalysis& analysed()
        {
            analysis.ensureBuilt (document, &durations);
            return analysis;
        }

        /** And without them, which is what a session that read no media has. */
        const cue::SlotAnalysis& analysedBlind()
        {
            blind.ensureBuilt (document, nullptr);
            return blind;
        }

        /** Whether these two cues are reported against this slot, either way
            round: a pair has no order. */
        bool clash (const std::string& slotId,
                    const std::string& first, const std::string& second)
        {
            const auto& found = analysed().clashes();

            return std::any_of (found.begin(), found.end(),
                                [&] (const cue::SlotClash& c)
                                {
                                    return c.slot == slotId
                                            && ((c.first == first && c.second == second)
                                                 || (c.first == second && c.second == first));
                                });
        }

        doc::ShowDocument document;
        cue::SlotAnalysis analysis, blind;
        std::map<std::string, double> durations;

        std::string listId, slotA, slotB, channelId;
    };
}

//==============================================================================
TEST_CASE ("analysis: a top-level claim in a manual list runs to the end of it")
{
    /*  THE HONEST ANSWER WHERE THERE IS NO TIME. A media cue fired by GO at the
        top of a manual list may have finished long before the next GO, and may
        equally still be sounding: nothing in the document says which, because
        what is between two GOs is a person. So the claim is live to the end of
        the list, and two cues on one slot in one manual list are reported.

        Which is exactly the case §3.9c is about: the second one WILL go pending
        and wait, which PR 4.3 built, and finding that out at eleven in the
        morning is the whole point of doing this at edit time. */
    AnalysisRig rig;

    const auto first = rig.media (rig.listId, 0, "Voice");
    const auto second = rig.media (rig.listId, 1, "Crowd");
    rig.feed (first, rig.slotA);
    rig.feed (second, rig.slotA);

    REQUIRE (rig.analysed().uses().size() == 2);
    CHECK (rig.analysed().usageOf (rig.slotA) == first + " " + second + " "
                                                   + second + " " + second);
    CHECK (rig.clash (rig.slotA, first, second));
    CHECK (rig.analysed().overlapsOf (rig.slotA) == first + " " + second);
}

TEST_CASE ("analysis: a claim inside a group is given back when the group ends")
{
    /*  A GROUP ENDING MEANS ITS MEMBERS' RUNS HAVE ENDED - a footer blocks, and
        the group is not done until its cues are (§3.6) - so a cue AFTER the
        group cannot be fighting a cue inside it. This is the reordering case
        from the other side: the arrangement that is fine. */
    AnalysisRig rig;

    const auto scene = rig.group (rig.listId, 0, "Scene", "sequence", "manual");
    const auto inside = rig.media (scene, 0, "Voice");
    const auto after = rig.media (rig.listId, 1, "Crowd");

    rig.feed (inside, rig.slotA);
    rig.feed (after, rig.slotA);

    CHECK_FALSE (rig.clash (rig.slotA, inside, after));
    CHECK (rig.analysed().overlapsOf (rig.slotA).empty());
}

TEST_CASE ("analysis: moving a cue above the group that holds the slot adds the warning, "
           "and moving it back takes it away")
{
    /*  §3.9c: "the reorder warning is liveness re-analysis on edit". One
        `object.move` and the answer changes, in both directions, with nothing
        told to rebuild - the analysis notices because the DOCUMENT'S revision
        moved, which is the shape PR 3.2 put the mounted namespace on. */
    AnalysisRig rig;

    const auto scene = rig.group (rig.listId, 0, "Scene", "sequence", "manual");
    const auto inside = rig.media (scene, 0, "Voice");
    const auto after = rig.media (rig.listId, 1, "Crowd");

    rig.feed (inside, rig.slotA);
    rig.feed (after, rig.slotA);

    REQUIRE_FALSE (rig.clash (rig.slotA, inside, after));

    /*  ABOVE THE GROUP, and now it is live across the whole scene: nothing in
        a manual list ends it before the list does. */
    REQUIRE (rig.document.move (after, rig.listId, 0).ok);
    CHECK (rig.clash (rig.slotA, inside, after));

    /*  And back. A warning that appears and never goes is a warning people
        learn to scroll past. */
    REQUIRE (rig.document.move (after, rig.listId, 1).ok);
    CHECK_FALSE (rig.clash (rig.slotA, inside, after));
}

TEST_CASE ("analysis: two lists always overlap, because two lists can be live at once")
{
    /*  §13.5, and §3.9c's *(proposed)* cross-list refusal declined in favour of
        the section's own rule: warn, don't refuse. Nothing orders the rows of
        one list against another - a trigger can fire the second list while the
        first is playing - so a pair on one slot in two lists is the same
        warning as a pair in one. */
    AnalysisRig rig;

    const auto other = rig.document.createList ("Effects").id;

    const auto here = rig.media (rig.listId, 0, "Voice");
    const auto there = rig.media (other, 0, "Thunder");

    rig.feed (here, rig.slotA);
    rig.feed (there, rig.slotA);

    CHECK (rig.clash (rig.slotA, here, there));
}

TEST_CASE ("analysis: shared on either destination silences that pair and nothing else")
{
    /*  PRD §3.9c: "allow marking deliberate sharing". The analysis is
        conservative by design, so a designer needs a way to say of ONE pair
        that they considered it - permanently, in the document, where the next
        person reading the show can see it.

        On EITHER destination, because sharing takes two and either of them
        saying so is the fact being recorded. And of that pair only: a third cue
        on the same slot that nobody marked is still reported. */
    AnalysisRig rig;

    const auto first = rig.media (rig.listId, 0, "Voice");
    const auto second = rig.media (rig.listId, 1, "Crowd");
    const auto third = rig.media (rig.listId, 2, "Thunder");

    rig.feed (first, rig.slotA);
    rig.feed (second, rig.slotA, true);
    rig.feed (third, rig.slotA);

    CHECK_FALSE (rig.clash (rig.slotA, first, second));
    CHECK_FALSE (rig.clash (rig.slotA, second, third));
    CHECK (rig.clash (rig.slotA, first, third));
}

TEST_CASE ("analysis: an insert on a rack channel is analysed exactly as a feed is")
{
    /*  §3.18: a rack channel is a slot. In Phase 4 an `Insert` changes no sound
        - the tracks and the plugins inside a channel are Phase 9's - and it is
        claimed, released and warned about like anything else. */
    AnalysisRig rig;

    const auto first = rig.media (rig.listId, 0, "Voice");
    const auto second = rig.media (rig.listId, 1, "Crowd");

    for (const auto& cueId : { first, second })
    {
        const auto insert = rig.document.createInsert (cueId, rig.channelId);
        REQUIRE (insert.ok);
    }

    CHECK (rig.clash (rig.channelId, first, second));
}

//==============================================================================
TEST_CASE ("analysis: inside an automatic sequence the seconds are known, "
           "so two members do not overlap")
{
    /*  THE REFINEMENT THAT MAKES THE WARNING WORTH READING. Inside an automatic
        chain there is no person between the members: the second starts when the
        first completes, and both lengths are known, so the analysis can say
        that the two intervals do not touch rather than that they might.

        Without it, every automatic sequence of two cues on one processor input
        would warn - which is most shows, and a check that fires on most shows
        is a check nobody reads. */
    AnalysisRig rig;

    const auto chain = rig.group (rig.listId, 0, "Chain", "sequence", "auto");
    const auto first = rig.media (chain, 0, "One");
    const auto second = rig.media (chain, 1, "Two");

    rig.feed (first, rig.slotA);
    rig.feed (second, rig.slotA);

    CHECK_FALSE (rig.clash (rig.slotA, first, second));

    const auto& uses = rig.analysed().uses();
    REQUIRE (uses.size() == 2);
    CHECK (uses[0].timed);
    CHECK (uses[0].from == doctest::Approx (0.0));
    CHECK (uses[0].to == doctest::Approx (4.0));
    CHECK (uses[1].from == doctest::Approx (4.0));
    CHECK (uses[1].to == doctest::Approx (8.0));
    CHECK (uses[0].chain == chain);
    CHECK (uses[1].chain == chain);
}

TEST_CASE ("analysis: the same show with no durations read warns, "
           "which is the direction it is allowed to be wrong in")
{
    /*  THE CONSERVATISM, MADE EXECUTABLE. It is the same two cues in the same
        automatic sequence; the only difference is that nothing read how long
        the file is - a replay, a `wfg tree` of a bundle with no media folder, a
        file in a format this build has no reader for.

        An interval with one end is not an interval. Comparing it would prove
        that nothing ever overlaps it, which is the one thing this analysis is
        never allowed to conclude, so it falls back to rows and reports. More
        warnings rather than fewer. */
    AnalysisRig rig;

    const auto chain = rig.group (rig.listId, 0, "Chain", "sequence", "auto");
    const auto first = rig.media (chain, 0, "One");
    const auto second = rig.media (chain, 1, "Two");

    rig.feed (first, rig.slotA);
    rig.feed (second, rig.slotA);

    const auto& found = rig.analysedBlind().clashes();

    REQUIRE (found.size() == 1);
    CHECK (found[0].slot == rig.slotA);
    CHECK (rig.analysedBlind().uses()[0].timed == false);
}

TEST_CASE ("analysis: in a timeline group the offsets decide, both ways")
{
    /*  A timeline group schedules its members at entry, each at its own
        pre-wait from the group's own entry (§3.6). So two four-second members
        two seconds apart are sounding together and two five seconds apart are
        not, and the analysis says exactly that. */
    AnalysisRig rig;

    const auto timeline = rig.group (rig.listId, 0, "Timeline", "timeline", "manual");
    const auto first = rig.media (timeline, 0, "One");
    const auto second = rig.media (timeline, 1, "Two");

    rig.feed (first, rig.slotA);
    rig.feed (second, rig.slotA);

    rig.document.setAttribute ("/godot/cue/" + second + "/preWait", "2");
    CHECK (rig.clash (rig.slotA, first, second));

    rig.document.setAttribute ("/godot/cue/" + second + "/preWait", "5");
    CHECK_FALSE (rig.clash (rig.slotA, first, second));
}

TEST_CASE ("analysis: a bed that loops for ever holds its slot to the end of the list")
{
    /*  Nought passes is for ever, which is what an ambience bed is - and a
        group containing one never advances past it, so the group runs for ever
        too however finite its own `loops` says it is. A group is only as
        bounded as its members.

        This is the case that produces an overlap nobody can remove by
        rearranging, and it is exactly why an overlap must not fail a build:
        the show is right and the warning is right. `shared` is the answer. */
    AnalysisRig rig;

    const auto scene = rig.group (rig.listId, 0, "Scene", "sequence", "manual");
    const auto bed = rig.media (scene, 0, "Ambience");
    const auto after = rig.media (rig.listId, 1, "Crowd");

    rig.feed (bed, rig.slotA);
    rig.feed (after, rig.slotA);

    REQUIRE_FALSE (rig.clash (rig.slotA, bed, after));

    /*  One range, played for ever. */
    const auto range = rig.document.createRange (bed, 0.0, 2.0);
    REQUIRE (range.ok);
    rig.document.findById (range.id)
       .setProperty (juce::Identifier ("loops"), 0, nullptr);

    CHECK (rig.clash (rig.slotA, bed, after));
}

TEST_CASE ("analysis: a stop cue aimed at the holder gives the slot back where it sits")
{
    /*  A release somebody wrote down, and the first answer the analysis looks
        for. A stop cue between the two is what a designer reaches for when they
        DO want the same input twice in one list, and the analysis has to see it
        or the advice it gives would be wrong. */
    AnalysisRig rig;

    const auto first = rig.media (rig.listId, 0, "Voice");

    const auto stop = rig.document.createCue (rig.listId, 1, "stop", "Stop the voice");
    REQUIRE (stop.ok);
    rig.document.setAttribute ("/godot/cue/" + stop.id + "/target", first);

    const auto second = rig.media (rig.listId, 2, "Crowd");

    rig.feed (first, rig.slotA);
    rig.feed (second, rig.slotA);

    CHECK_FALSE (rig.clash (rig.slotA, first, second));
    CHECK (rig.analysed().uses()[0].until == stop.id);

    /*  And a stop aimed at something else is not a release. */
    rig.document.setAttribute ("/godot/cue/" + stop.id + "/target", second);
    CHECK (rig.clash (rig.slotA, first, second));
}

//==============================================================================
TEST_CASE ("analysis: it is published on the slot and on the document")
{
    /*  §13.2 and §13.5. `usage` and `overlaps` come out of the CACHED half of
        the tree, because they change when somebody edits the show and at no
        other time; `/godot/document/warnings` comes out of the runtime half,
        because a client asking what is wrong with the show is asking about the
        show as it is now. */
    AnalysisRig rig;

    Engine engine;
    tree::MountTable mounts;
    cue::RunTable runs;
    tree::ParameterTree parameters { rig.document, engine.commands(), mounts, runs };

    const auto first = rig.media (rig.listId, 0, "Voice");
    const auto second = rig.media (rig.listId, 1, "Crowd");
    rig.feed (first, rig.slotA);
    rig.feed (second, rig.slotA);

    parameters.markStale();

    tree::EngineState state;
    state.version = "test";

    const auto snapshot = parameters.publish (0, state);

    const auto* overlaps = snapshot->find ("/godot/slot/" + rig.slotA + "/overlaps");
    REQUIRE (overlaps != nullptr);
    REQUIRE (overlaps->soleValue().has_value());
    CHECK (overlaps->soleValue()->getString() == first + " " + second);

    const auto* usage = snapshot->find ("/godot/slot/" + rig.slotA + "/usage");
    REQUIRE (usage != nullptr);
    REQUIRE (usage->soleValue().has_value());
    CHECK_FALSE (usage->soleValue()->getString().empty());

    const auto* warnings = snapshot->find ("/godot/document/warnings");
    REQUIRE (warnings != nullptr);
    REQUIRE (warnings->soleValue().has_value());

    /*  ONE PER LINE, which no other list-shaped readout here is: a warning is a
        sentence and sentences contain spaces. */
    const auto text = warnings->soleValue()->getString();
    CHECK (text.find (first) != std::string::npos);
    CHECK (text.find (second) != std::string::npos);
    CHECK (text.find ('\n') == std::string::npos);   // exactly one warning, so no separator
}

//==============================================================================
TEST_CASE ("M18: the analysis is a cache asked and not told, counted rather than timed")
{
    /*  MEASUREMENT M18 (§13.14). What the design guarantees is countable and
        exact - publishes with nothing edited cost no rebuild at all, and one
        edit costs one - and a wall-clock threshold on a shared CI runner is a
        flaky test that teaches people to re-run the suite. So the assertion is
        the count; the clock is reported and gates nothing.

        The shape is PR 3.2's, which moved the mounted half of the tree off a
        `markStale` flag and onto the mount table's own revision for exactly
        this reason: a flag is a line every future write path has to remember,
        and the one that forgets produces a stale reading indistinguishable
        from a correct one. */
    AnalysisRig rig;

    Engine engine;
    tree::MountTable mounts;
    cue::RunTable runs;
    tree::ParameterTree parameters { rig.document, engine.commands(), mounts, runs };

    tree::EngineState state;
    state.version = "test";

    //--------------------------------------------------------------------------
    /*  Five hundred cues over twenty slots, which is a large show rather than
        an impossible one: §13.14 sizes it at the show nobody wants to find out
        about on the night. */
    constexpr int slotCount = 20;
    constexpr int cueCount = 500;

    std::vector<std::string> slots { rig.slotA, rig.slotB };

    for (int n = 2; n < slotCount; ++n)
        slots.push_back (rig.declareSlot (
            rig.document.root().getChildWithName ("Mounts").getChild (0)
                       [juce::Identifier ("id")].toString().toStdString(),
            "/wfs/input/" + std::to_string (10 + n), n));

    /*  BUILT STRAIGHT INTO THE TREE rather than through `cue.create`, which is
        the one place in this suite that is worth doing.

        Every write door finds its parent with `findById`, and `findById` is a
        depth-first walk of the whole show - so five hundred cues built through
        the door is a quadratic walk that costs a minute of a CI run and
        measures the door rather than the thing under test. The analysis cannot
        tell the difference: it reads the tree, and the revision counter is a
        listener ON the tree, so a cue put here is a cue the door would have put
        here. Every other case in this file uses the door. */
    auto list = rig.document.findById (rig.listId);
    REQUIRE (list.isValid());

    std::vector<std::string> cues;

    for (int n = 0; n < cueCount; ++n)
    {
        /*  Crockford base32 without I, L, O or U, which is what an identifier
            is (§1) - and eight characters, or `validate` would call it
            malformed. */
        const auto cueId = "MA" + juce::String (100000 + n).toStdString();

        juce::ValueTree media { "Media" };
        media.setProperty (juce::Identifier ("id"), juce::String (cueId), nullptr);
        media.setProperty (juce::Identifier ("name"), juce::String ("Cue ") + juce::String (n),
                           nullptr);
        media.setProperty (juce::Identifier ("file"), "tone.wav", nullptr);

        juce::ValueTree feed { "Feed" };
        feed.setProperty (juce::Identifier ("id"), juce::String ("FA" + std::to_string (100000 + n)),
                          nullptr);
        feed.setProperty (juce::Identifier ("slot"),
                          juce::String (slots[static_cast<std::size_t> (n % slotCount)]), nullptr);
        feed.setProperty (juce::Identifier ("gains"), "1", nullptr);
        media.addChild (feed, -1, nullptr);

        list.addChild (media, -1, nullptr);
        cues.push_back (cueId);
    }

    /*  THE ANALYSIS ON ITS OWN FIRST, because the number that matters is its
        own. A `publish` also rebuilds the document half of the tree - seven
        thousand nodes for a show this size - and that cost is Phase 1's and
        unchanged by any of this. */
    cue::SlotAnalysis alone;
    const auto beforeAlone = juce::Time::getMillisecondCounterHiRes();
    alone.ensureBuilt (rig.document, &rig.durations);
    const auto analysisCost = juce::Time::getMillisecondCounterHiRes() - beforeAlone;

    /*  And the half of it that is not the slot walk at all. */
    const auto beforeRefs = juce::Time::getMillisecondCounterHiRes();
    const auto references = rig.document.warnings().size();
    const auto referenceCost = juce::Time::getMillisecondCounterHiRes() - beforeRefs;

    const auto before = juce::Time::getMillisecondCounterHiRes();
    parameters.publish (0, state);
    const auto firstBuild = juce::Time::getMillisecondCounterHiRes() - before;

    const auto baseline = parameters.analysisRebuilds();
    REQUIRE (baseline > 0);

    //--------------------------------------------------------------------------
    /*  PUBLISHES WITH NOTHING EDITED. Every one of them asks; none of them
        rebuilds. This is the whole guarantee.

        Twenty rather than the hundred it was first written with, because each
        publish MERGES the document half into a fresh snapshot - seven thousand
        nodes for a show this size - and a hundred of those measured the
        snapshot rather than the cache, at a minute of a CI run per locale. What
        is being asserted is a count, and a count of zero is a count of zero. */
    constexpr int quietPublishes = 20;

    for (int n = 0; n < quietPublishes; ++n)
        parameters.publish (n + 1, state);

    CHECK (parameters.analysisRebuilds() == baseline);

    //--------------------------------------------------------------------------
    /*  AND THE MOVES. One mutation, one rebuild - not two for a move that is a
        remove and an add, and not one for every publish that follows it. */
    constexpr int moves = 20;

    const auto beforeMoves = juce::Time::getMillisecondCounterHiRes();

    for (int n = 0; n < moves; ++n)
    {
        REQUIRE (rig.document.move (cues[static_cast<std::size_t> (n)],
                                    rig.listId, cueCount - 1 - n).ok);
        parameters.markStale();
        parameters.publish (200 + n, state);
    }

    const auto moveCost = juce::Time::getMillisecondCounterHiRes() - beforeMoves;

    CHECK (parameters.analysisRebuilds() == baseline + moves);

    MESSAGE ("M18  " << cueCount << " cues, " << slotCount << " slots, Debug build: "
                     << "the analysis alone " << analysisCost << " ms, of which "
                     << referenceCost << " ms is the document's own reference walk ("
                     << references << " warnings); a whole publish "
                     << firstBuild << " ms; " << quietPublishes
                     << " publishes with nothing edited rebuilt "
                     << (parameters.analysisRebuilds() - baseline - moves)
                     << " times; " << moves << " object.move each followed by a publish "
                     << moveCost << " ms, " << (moveCost / moves) << " ms each");
}
