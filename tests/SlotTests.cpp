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

/*  A SLOT: ONE POSITION IN A POOL DECLARED AT LOAD.

    PRD §3.9e defines the word once, having used it in §3.9b for a processor
    input and in §3.25 for a launcher slot and defined it in neither: one
    position in a pool of fixed size declared at load; typed; exclusive; held
    for a live range; released by a policy; with a failure policy of its own
    kind. Four instances. This file is the two that are DOCUMENT OBJECTS - a
    processor input declared on its mount, and a channel of the live rack -
    together with what a cue says about them.

    What is here is the pool and what the show may say about it. Claims at run
    time are PR 4.3's and the edit-time overlap analysis is 4.4's; a cue's
    coefficients actually reaching the outputs is measured in AudioTests, where
    the graph is.

    THE ONE THING WORTH KNOWING BEFORE READING. A voice is a slot too - a track
    is §3.9e's first instance - and it is deliberately absent from all of this.
    A track is not an object anybody declared: it has no identifier, and
    `Show/Audio/@tracks` is a count rather than a list. Where a voice is held
    has been readable at `/godot/run/<id>/track` since Phase 2. One mechanism
    does not oblige one data structure.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/cue/Run.h>

#include <string>

using namespace wfg;

namespace
{
    /*  A show with a mount, a bus and somewhere to put a slot. */
    struct SlotRig
    {
        SlotRig()
        {
            listId = document.createList ("Main").id;
            cueId = document.createCue (listId, 0, "media", "Voice").id;

            const auto mountEdit = document.createMount ("/wfs", "namespaces/wfs.json");
            REQUIRE (mountEdit.ok);
            mountId = mountEdit.id;

            auto mount = document.findById (mountId);
            mount.setProperty (juce::Identifier ("port"), 9000, nullptr);

            /*  One wide send into the processor, which is the ordinary rig: a
                twelve-input box is fed by one twelve-channel bus, and each
                input is an offset into it. */
            auto audio = document.root().getChildWithName ("Audio");
            REQUIRE (audio.isValid());

            juce::ValueTree bus { "Bus" };
            bus.setProperty (juce::Identifier ("id"), "BS000001", nullptr);
            bus.setProperty (juce::Identifier ("name"), "WFS send", nullptr);
            bus.setProperty (juce::Identifier ("firstChannel"), 8, nullptr);
            bus.setProperty (juce::Identifier ("width"), 12, nullptr);
            audio.addChild (bus, -1, nullptr);
        }

        std::string published (const std::string& address)
        {
            parameters.markStale();

            tree::EngineState state;
            state.version = "test";

            const auto snapshot = parameters.publish (0, state);
            const auto* node = snapshot->find (address);

            if (node == nullptr || ! node->soleValue().has_value())
                return "(absent)";

            const auto value = *node->soleValue();

            return value.isString() ? value.getString()
                                    : std::to_string (value.getInt32());
        }

        bool exists (const std::string& address)
        {
            parameters.markStale();

            tree::EngineState state;
            state.version = "test";

            return parameters.publish (0, state)->find (address) != nullptr;
        }

        Engine engine;
        doc::ShowDocument document;
        tree::MountTable mounts;
        cue::RunTable runs;
        tree::ParameterTree parameters { document, engine.commands(), mounts, runs };

        std::string listId, cueId, mountId;
    };
}

//==============================================================================
TEST_CASE ("slot: a processor input is declared on the mount that carries it")
{
    /*  DECISION P, 2026-09-07. PRD §3.9b marks "the processor declares its own
        slots" as *(proposed)* - Go.dot reading a mounted namespace and inferring
        how many inputs exist. The author's decision is that the SHOW declares
        them and the mounted namespace is what a validate pass checks against.

        How many inputs of a processor a show is using is something somebody
        decided (§4.10), and a pool that changed when a processor was
        reconfigured would change a show nobody had edited. */
    SlotRig rig;

    const auto slot = rig.document.createSlot (rig.mountId, "/wfs/input/3");
    REQUIRE (slot.ok);

    rig.document.setAttribute ("/godot/slot/" + slot.id + "/name", "Voix solo");
    rig.document.setAttribute ("/godot/slot/" + slot.id + "/width", "1");
    rig.document.setAttribute ("/godot/slot/" + slot.id + "/bus", "BS000001");
    rig.document.setAttribute ("/godot/slot/" + slot.id + "/firstChannel", "2");

    CHECK (rig.published ("/godot/slot/" + slot.id + "/name") == "Voix solo");
    CHECK (rig.published ("/godot/slot/" + slot.id + "/address") == "/wfs/input/3");
    CHECK (rig.published ("/godot/slot/" + slot.id + "/firstChannel") == "2");

    /*  ITS KIND IS DERIVED from the element that holds it, never stored - the
        `cue/kind` rule. A client that could write it could turn a processor
        input into a rack channel by writing a word, and the two are released
        and refused by different policies (§3.9e). */
    CHECK (rig.published ("/godot/slot/" + slot.id + "/kind") == "processorInput");
    CHECK_FALSE (rig.document.setAttribute ("/godot/slot/" + slot.id + "/kind",
                                            "rackChannel").ok);

    /*  And it is addressed by identity like every other object (§1), so it is
        in the container's own roster. */
    CHECK (rig.published ("/godot/slot/order") == slot.id);

    /*  A slot belongs to a processor. Asking a cue for one names the wrong
        object, and saying so at the moment of the request is kinder than a
        grammar failure later. */
    CHECK_FALSE (rig.document.createSlot (rig.cueId, "/wfs/input/4").ok);
    CHECK_FALSE (rig.document.createSlot ("ZZ999999", "/wfs/input/4").ok);
}

TEST_CASE ("slot: a rack channel is the other declared kind, and shares the address space")
{
    /*  §3.18: the rack is a pool of channels declared at load, so many of each
        width class, exactly as `@tracks` declares polyphony. Phase 4 declares
        the pool and allocates from it; the tracks, the sends and the plugins
        inside a channel are Phase 9's. */
    SlotRig rig;

    const auto channel = rig.document.createRackChannel ("monoToStereo");
    REQUIRE (channel.ok);

    rig.document.setAttribute ("/godot/slot/" + channel.id + "/name", "Vocal verb");

    /*  ONE ADDRESS SPACE FOR BOTH KINDS, because §1's first rule is that
        objects are identity-addressed: a client holding an identifier never has
        to know which container it came out of. */
    CHECK (rig.published ("/godot/slot/" + channel.id + "/name") == "Vocal verb");
    CHECK (rig.published ("/godot/slot/" + channel.id + "/class") == "monoToStereo");
    CHECK (rig.published ("/godot/slot/" + channel.id + "/kind") == "rackChannel");

    /*  A SHARED CHANNEL IS DECLARED AND NEVER CLAIMED. §3.9e: a reverb many
        cues send into is a bus with a chain, and a bus is not a slot. The
        document says which it is; the allocator reads it in PR 4.3. */
    CHECK (rig.published ("/godot/slot/" + channel.id + "/access") == "exclusive");
    CHECK (rig.document.setAttribute ("/godot/slot/" + channel.id + "/access", "shared").ok);
    CHECK (rig.published ("/godot/slot/" + channel.id + "/access") == "shared");

    /*  It carries none of a processor input's rows: a rack channel has no
        address on somebody else's namespace and no bus of its own. */
    CHECK_FALSE (rig.exists ("/godot/slot/" + channel.id + "/address"));
    CHECK_FALSE (rig.exists ("/godot/slot/" + channel.id + "/bus"));

    /*  THE RACK IS MADE ON DEMAND and there is one of it, the way a group's
        header is: asking twice does not make a second. */
    const auto second = rig.document.createRackChannel ("stereo");
    REQUIRE (second.ok);

    int racks = 0;

    for (const auto& child : rig.document.root().getChildWithName ("Audio"))
        if (child.getType().toString() == "Rack")
            ++racks;

    CHECK (racks == 1);
    CHECK (rig.published ("/godot/slot/order") == channel.id + " " + second.id);
}

TEST_CASE ("slot: the rack does not become a bus")
{
    /*  The loop that publishes `/godot/bus/<id>` walks every identified child of
        `Audio`, so a `Rack` that carried an identifier would appear there as a
        bus with a default width - and `/godot/bus` would stop being the show's
        buses. It carries none, like `Mounts`, and the existing empty-identifier
        guard passes over it. Worth a case because nothing else would notice. */
    SlotRig rig;

    const auto channel = rig.document.createRackChannel ("mono");
    REQUIRE (channel.ok);

    int buses = 0;

    for (const auto& child : rig.document.root().getChildWithName ("Audio"))
        if (child.hasProperty (juce::Identifier ("id")))
            ++buses;

    CHECK (buses == 1);                       // the one the rig declared
    CHECK (rig.published ("/godot/bus/BS000001/width") == "12");
    CHECK_FALSE (rig.exists ("/godot/bus/" + channel.id + "/width"));
}

//==============================================================================
TEST_CASE ("feed: a cue's destinations are a list, not a choice")
{
    /*  §3.9b: "A cue may hold a slot AND a bus routing simultaneously - a source
        into WFS plus a stereo feed to foldback is ordinary." So a `Feed` sits
        beside a `Route` rather than instead of one. */
    SlotRig rig;

    const auto slot = rig.document.createSlot (rig.mountId, "/wfs/input/3");
    REQUIRE (slot.ok);
    rig.document.setAttribute ("/godot/slot/" + slot.id + "/bus", "BS000001");

    const auto route = rig.document.createRoute (rig.cueId, "BS000001");
    REQUIRE (route.ok);

    const auto feed = rig.document.createFeed (rig.cueId, slot.id);
    REQUIRE (feed.ok);

    const auto insert = rig.document.createInsert (rig.cueId,
                                                   rig.document.createRackChannel ("mono").id);
    REQUIRE (insert.ok);

    /*  EACH IS PUBLISHED AS ITSELF, and not as a nested cue. The tree walk
        recurses into any identified child it does not recognise, so a `Feed`
        left off that lookup would appear at `/godot/cue/<id>` carrying the rows
        of a kind it is not - the failure mode that lookup exists to prevent,
        and a silent one. */
    CHECK (rig.published ("/godot/feed/" + feed.id + "/slot") == slot.id);
    CHECK (rig.published ("/godot/feed/" + feed.id + "/cue") == rig.cueId);
    CHECK (rig.published ("/godot/insert/" + insert.id + "/cue") == rig.cueId);

    CHECK_FALSE (rig.exists ("/godot/cue/" + feed.id + "/kind"));
    CHECK_FALSE (rig.exists ("/godot/cue/" + insert.id + "/kind"));

    /*  And only a media cue has anywhere for a sound to go. */
    const auto memo = rig.document.createCue (rig.listId, 1, "memo", "House to half").id;
    CHECK_FALSE (rig.document.createFeed (memo, slot.id).ok);
    CHECK_FALSE (rig.document.createInsert (memo, "ANYTHING").ok);
}

//==============================================================================
TEST_CASE ("validate: a slot has to be an input of the processor that declares it")
{
    /*  A slot whose address falls outside its own mount's prefix would claim
        exclusivity over parameters the mount does not carry - and the osc cues
        that write those parameters would go somewhere else entirely. */
    SlotRig rig;

    const auto slot = rig.document.createSlot (rig.mountId, "/wfs/input/3");
    REQUIRE (slot.ok);
    rig.document.setAttribute ("/godot/slot/" + slot.id + "/bus", "BS000001");

    CHECK (rig.document.validate().empty());

    rig.document.setAttribute ("/godot/slot/" + slot.id + "/address", "/xoa/input/3");

    const auto problems = rig.document.validate();
    REQUIRE (problems.size() == 1u);
    INFO (problems.front());
    CHECK (problems.front().find ("/wfs") != std::string::npos);
    CHECK (problems.front().find (slot.id) != std::string::npos);
}

TEST_CASE ("validate: a slot has to fit inside the bus that feeds it")
{
    /*  `firstChannel` is an offset into the bus, exactly as a bus's own is an
        offset into the hardware outputs. A slot that ran off the end would send
        part of a source into channels nobody declared. */
    SlotRig rig;

    const auto slot = rig.document.createSlot (rig.mountId, "/wfs/input/3");
    REQUIRE (slot.ok);
    rig.document.setAttribute ("/godot/slot/" + slot.id + "/bus", "BS000001");
    rig.document.setAttribute ("/godot/slot/" + slot.id + "/width", "2");
    rig.document.setAttribute ("/godot/slot/" + slot.id + "/firstChannel", "10");

    CHECK (rig.document.validate().empty());        // 10 and 11 of twelve

    rig.document.setAttribute ("/godot/slot/" + slot.id + "/firstChannel", "11");

    const auto problems = rig.document.validate();
    REQUIRE (problems.size() == 1u);
    INFO (problems.front());
    CHECK (problems.front().find ("do not fit") != std::string::npos);

    /*  A BUS THAT IS NOT THERE AT ALL IS A WARNING and not this refusal: the
        `refers` column reports a dangling pointer, and §3.8 makes deleting the
        thing a cue points at a silent no-op during tech rather than a show that
        will not open. */
    rig.document.setAttribute ("/godot/slot/" + slot.id + "/firstChannel", "0");
    rig.document.setAttribute ("/godot/slot/" + slot.id + "/bus", "ZZ999999");

    CHECK (rig.document.validate().empty());
    CHECK_FALSE (rig.document.warnings().empty());
}

TEST_CASE ("validate: coefficients that do not divide into their destination")
{
    /*  THE ROW'S OWN PROMISE, KEPT AT LAST. `route/gains` has said since Phase 2
        that "a length that does not match is refused when the show loads rather
        than discovered mid-cue", and nothing ever checked it: the only check was
        at arm, where it failed the run.

        What is checkable here is that the list is a whole number of input
        channels wide. How many channels the cue HAS is the file's, and the file
        arrives on a different machine from the one the show was written on. */
    SlotRig rig;

    const auto route = rig.document.createRoute (rig.cueId, "BS000001");
    REQUIRE (route.ok);

    /*  Twelve into a bus twelve wide: one input channel, and sound. */
    /*  Written onto the node rather than through `setAttribute`, which is how
        every other test writes a list-valued row: the write door parses a
        scalar and a routing matrix is not one. */
    auto routeNode = rig.document.findById (route.id);
    routeNode.setProperty (juce::Identifier ("gains"), "1 0 0 0 0 0 0 0 0 0 0 0", nullptr);
    CHECK (rig.document.validate().empty());

    /*  Seven is not a matrix into anything twelve wide. */
    routeNode.setProperty (juce::Identifier ("gains"), "1 0 0 0 0 0 0", nullptr);

    const auto problems = rig.document.validate();
    REQUIRE (problems.size() == 1u);
    INFO (problems.front());
    CHECK (problems.front().find ("do not divide") != std::string::npos);
}

TEST_CASE ("validate: a feed's coefficients answer to the SLOT's width, not the bus's")
{
    /*  Which is the whole point of a slot having one. A feed lands in the
        slot's own channels of the slot's bus, so a stereo source into a mono
        input is a mistake even though the bus is twelve wide. */
    SlotRig rig;

    const auto slot = rig.document.createSlot (rig.mountId, "/wfs/input/3");
    REQUIRE (slot.ok);
    rig.document.setAttribute ("/godot/slot/" + slot.id + "/bus", "BS000001");
    rig.document.setAttribute ("/godot/slot/" + slot.id + "/width", "1");

    const auto feed = rig.document.createFeed (rig.cueId, slot.id);
    REQUIRE (feed.ok);

    auto feedNode = rig.document.findById (feed.id);
    feedNode.setProperty (juce::Identifier ("gains"), "1", nullptr);
    CHECK (rig.document.validate().empty());

    /*  Two coefficients into a one-channel input is two input channels, which
        is legal arithmetic and a legal show: a stereo cue folded into one
        input. Three is not. */
    feedNode.setProperty (juce::Identifier ("gains"), "0.7 0.7", nullptr);
    CHECK (rig.document.validate().empty());

    rig.document.setAttribute ("/godot/slot/" + slot.id + "/width", "2");
    feedNode.setProperty (juce::Identifier ("gains"), "1 0 0", nullptr);

    const auto problems = rig.document.validate();
    REQUIRE (problems.size() == 1u);
    INFO (problems.front());
    CHECK (problems.front().find ("2 channels wide") != std::string::npos);
}

//==============================================================================
TEST_CASE ("refers: a feed names a slot and an insert names a channel, and not each other")
{
    /*  The by-kind half of the `refers` check, which is why `Slot` and
        `Channel` answer `ownerForElement` differently although they share the
        rows they have in common. Without it, `feed/@slot` naming a rack channel
        would pass every check the document makes and fail at arm. */
    SlotRig rig;

    const auto slot = rig.document.createSlot (rig.mountId, "/wfs/input/3");
    const auto channel = rig.document.createRackChannel ("mono");
    REQUIRE (slot.ok);
    REQUIRE (channel.ok);

    rig.document.setAttribute ("/godot/slot/" + slot.id + "/bus", "BS000001");

    const auto feed = rig.document.createFeed (rig.cueId, slot.id);
    REQUIRE (feed.ok);
    rig.document.findById (feed.id).setProperty (juce::Identifier ("gains"), "1", nullptr);

    CHECK (rig.document.warnings().empty());

    /*  Pointed at the rack channel instead, it is the wrong KIND of object -
        found by identifier and refused by kind. A warning and not a refusal,
        because §3.8 makes a pointer at the wrong thing a run-time failure
        rather than a show that will not open. */
    rig.document.setAttribute ("/godot/feed/" + feed.id + "/slot", channel.id);

    const auto warnings = rig.document.warnings();
    REQUIRE_FALSE (warnings.empty());
    INFO (warnings.front());
    CHECK (warnings.front().find (feed.id) != std::string::npos);
}
