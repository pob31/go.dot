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

/*  THE SURFACE BRIDGE, BYTES IN AND BYTES OUT, WITH NO HARDWARE (namespace
    draft §16.6).

    What a Mackie surface's hands send becomes commands - coalesced, touched
    before written, from the origin `surface:<id>` - and what the tree says
    becomes motor moves, scribble strips, LEDs, rings and colour, stepped,
    diffed and rate-limited. A recording sink stands where the MIDI sender
    would, so every case runs on a machine with no MIDI interface, which every
    CI runner is.

    THE TREE IS THE REAL ONE wherever it can be: a show document, the
    parameter tree and the live door a trim is written through, so the
    addresses the bridge reads are the ones the engine publishes. Two rigs go
    further - a sampler bank armed on a fake audio side, for the cases that
    need a cue on a strip - and one case writes its snapshot by hand, because
    a run's timbre needs an analysed file the unit suite does not have.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/client/model/Fader.h>
#include <wfg/client/model/Text.h>
#include <wfg/engine/Engine.h>
#include <wfg/engine/command/Event.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/cue/DcaTable.h>
#include <wfg/engine/cue/LiveRows.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/midi/MidiSink.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/surface/FaderCurve.h>
#include <wfg/engine/surface/McuCodec.h>
#include <wfg/engine/surface/SurfaceBridge.h>
#include <wfg/engine/surface/SurfaceProfile.h>
#include <wfg/engine/surface/SurfaceTable.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/Node.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/tree/Touches.h>
#include <wfg/engine/tree/TreeCommands.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace wfg;

namespace
{
    /** The sink as a notebook: what was sent, to which port, in order. */
    struct RecordingSink final : midi::MidiSink
    {
        struct Message
        {
            std::string port;
            midi::Bytes bytes;
        };

        std::string send (const std::string& port, const midi::Bytes& bytes) override
        {
            sent.push_back ({ port, bytes });
            return {};
        }

        std::vector<Message> sent;
    };

    bool near (double a, double b) { return std::abs (a - b) < 1.0e-9; }

    /** A port with a device behind it, rx and tx on. */
    surface::PortState plugged (const std::string& name)
    {
        surface::PortState port;
        port.bound = true;
        port.rx = true;
        port.tx = true;
        port.name = name;
        return port;
    }

    std::vector<midi::Bytes> sentOn (const RecordingSink& sink, const std::string& port)
    {
        std::vector<midi::Bytes> out;

        for (const auto& message : sink.sent)
            if (message.port == port)
                out.push_back (message.bytes);

        return out;
    }

    std::vector<midi::Bytes> sysexOf (const RecordingSink& sink)
    {
        std::vector<midi::Bytes> out;

        for (const auto& message : sink.sent)
            if (! message.bytes.empty() && message.bytes.front() == 0xf0)
                out.push_back (message.bytes);

        return out;
    }

    bool contains (const std::vector<midi::Bytes>& messages, const midi::Bytes& wanted)
    {
        return std::find (messages.begin(), messages.end(), wanted) != messages.end();
    }

    /** The positions one motor fader was sent, in order. */
    std::vector<int> motorMoves (const RecordingSink& sink, const std::string& port, int element)
    {
        std::vector<int> out;

        for (const auto& message : sink.sent)
            if (message.port == port && message.bytes.size() == 3 && message.bytes[0] == 0xe0 + element)
                out.push_back (message.bytes[1] | (message.bytes[2] << 7));

        return out;
    }

    /** The D700 colour messages one RGB element was sent: channels 2, 3 and 4 at its note. */
    std::vector<midi::Bytes> colourOf (const RecordingSink& sink, const std::string& port, int note)
    {
        std::vector<midi::Bytes> out;

        for (const auto& message : sink.sent)
            if (message.port == port && message.bytes.size() == 3 && message.bytes[1] == note
                && (message.bytes[0] == 0x91 || message.bytes[0] == 0x92 || message.bytes[0] == 0x93))
                out.push_back (message.bytes);

        return out;
    }

    std::vector<Event> named (const std::vector<Event>& events, const std::string& command)
    {
        std::vector<Event> out;

        for (const auto& event : events)
            if (event.command == command)
                out.push_back (event);

        return out;
    }

    bool is (const std::optional<surface::Rgb>& colour, int red, int green, int blue)
    {
        return colour.has_value() && colour->red == red && colour->green == green && colour->blue == blue;
    }

    //==========================================================================
    /*  A SHOW WITH SURFACES AND DCAS, AND NO RUNNER: the real document, the
        real tree, the live door a trim goes through and the touch table. A
        dca strip's target is its DCA's trim, which needs nothing playing. */
    struct Desk
    {
        Desk()
        {
            engine.log().openInMemory ({});
            doc::registerDocumentCommands (engine.commands(), document, {},
                                           cue::liveWriteFor (runs, dcas, document));
            tree::registerTreeCommands (engine.commands(), touches);

            parameters.setDcas (&dcas);
            parameters.setSurfaces (&table);
        }

        /** A surface in the show, its strips kept in `strips` by its identifier. */
        std::string makeSurface (const std::string& profile, const std::string& name)
        {
            std::vector<std::string> made;
            const auto result = document.createSurface (profile, name, {}, {}, made);
            REQUIRE (result.ok);
            strips[result.id] = made;
            return result.id;
        }

        std::string makeDca (const std::string& name)
        {
            const auto result = document.createDca (name);
            REQUIRE (result.ok);
            return result.id;
        }

        void set (const std::string& address, const std::string& text)
        {
            REQUIRE_MESSAGE (document.setAttribute (address, text).ok, address << " = " << text);
        }

        /** A strip pinned to a DCA: the fader rides the DCA's trim. */
        void pin (const std::string& stripId, const std::string& dcaId)
        {
            set ("/godot/slot/" + stripId + "/role", "dca");
            set ("/godot/slot/" + stripId + "/dca", dcaId);
        }

        /** A write from somebody other than the surface, applied on the next tick. */
        void write (const std::string& address, double value, const std::string& from = "window")
        {
            REQUIRE (engine.submit (from, "node.set", { osc::Value::string (address),
                                                        osc::Value::float64 (value) }));
        }

        surface::SurfaceSpec spec (const std::string& surfaceId, const std::string& profile,
                                   std::vector<std::string> ports)
        {
            surface::SurfaceSpec made;
            made.id = surfaceId;
            made.profile = profile;
            made.ports = std::move (ports);
            made.strips = strips[surfaceId];
            return made;
        }

        bool declare (std::vector<surface::SurfaceSpec> specs,
                      std::map<std::string, surface::PortState> ports)
        {
            return bridge.declare (std::move (specs),
                                   [ports] (const std::string& portId)
                                   {
                                       const auto found = ports.find (portId);
                                       return found != ports.end() ? found->second : surface::PortState {};
                                   });
        }

        /*  One tick as serve runs it: the bridge's hook, the drain, the
            publish, and the bridge's after-tick. */
        void tickOnce()
        {
            tableMoved = bridge.beforeTick (submit, tick);
            engine.processTick (tick);
            parameters.markStale();
            snapshot = parameters.publish (tick, state);
            bridge.afterTick (snapshot, touches, tick);
            ++tick;
        }

        void ticks (int count)
        {
            for (int n = 0; n < count; ++n)
                tickOnce();
        }

        /** Bytes from a surface's port, which the bridge must own. */
        void arrive (const std::string& port, midi::Bytes bytes)
        {
            REQUIRE (bridge.arrived (port, bytes));
        }

        std::string published (const std::string& address)
        {
            REQUIRE (snapshot != nullptr);
            return client::model::text (*snapshot, address);
        }

        void clear()
        {
            sink.sent.clear();
            submitted.clear();
        }

        RecordingSink sink;
        surface::SurfaceTable table;
        surface::SurfaceBridge bridge { sink, table };

        /*  What the bridge submitted, and whether it reaches the engine: off,
            a case sees what was asked without an engine that has no Runner
            refusing a `strip.press` it has never heard of. */
        std::vector<Event> submitted;
        bool forward = true;

        surface::SurfaceBridge::Submit submit = [this] (Event event)
        {
            submitted.push_back (event);
            return forward ? engine.submit (std::move (event)) : true;
        };

        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::DcaTable dcas;
        tree::TouchTable touches;
        tree::MountTable mounts;
        tree::ParameterTree parameters { document, engine.commands(), mounts, runs };
        tree::EngineState state;
        std::shared_ptr<const tree::TreeSnapshot> snapshot;
        std::map<std::string, std::vector<std::string>> strips;
        std::int64_t tick = 1;
        bool tableMoved = false;
    };

    //==========================================================================
    /*  An audio side whose arms land when the test says - SamplerTests' fake,
        which is all a strip needs to hold a run. */
    struct FakePlayer final : cue::Player
    {
        int trackCount() const override                      { return tracks; }
        std::int64_t samplesElapsed() const override         { return 0; }
        int blockSize() const override                       { return 128; }
        int channelsPerTrack() const override                { return 2; }
        int slotCount() const override                       { return 1; }
        int sampleRate() const override                      { return 48000; }

        void requestArm (const cue::ArmRequest& request) override { arms.push_back (request); }

        bool launchAtSample (int, int, std::int64_t) override    { return true; }
        bool stop (int) override                                { return true; }
        bool stopAtSample (int, int, std::int64_t) override    { return true; }
        void setLevelDb (int, double) override                  {}
        void setRouting (int, const std::vector<cue::Coefficient>&) override {}
        bool isPlaying (int) const override                     { return false; }
        bool isArmReady (int track) const override              { return ready.count (track) > 0; }

        void completeArms (Engine& engine)
        {
            for (const auto& arm : arms)
            {
                engine.submit (origin::engine, "audio.armed",
                               { osc::Value::string (arm.runId), osc::Value::int32 (arm.track) });
                ready.insert (arm.track);
            }

            arms.clear();
        }

        int tracks = 8;
        std::vector<cue::ArmRequest> arms;
        std::set<int> ready;
    };

    /*  A SAMPLER BANK OF TWO ON ONE SURFACE, armed by GO: each member on a
        strip, holding a run, so a strip has a cue - its name, its colour, its
        `pressure` - and a target, `/godot/run/<id>/trim`. */
    struct Stage
    {
        explicit Stage (const std::string& profile, int memberCount = 2)
        {
            engine.log().openInMemory ({});

            doc::registerDocumentCommands (engine.commands(), document, {},
                                           cue::liveWriteFor (runs, dcas, document));
            cue::registerCueCommands (engine.commands(), document, focus);
            cue::registerRunCommands (engine.commands(), runs);
            cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);
            tree::registerTreeCommands (engine.commands(), touches);

            runner.setPlayer (&audio);
            runner.setSamplesPerTick (960);
            runner.setDcas (&dcas);
            runner.setTouches (&touches);
            parameters.setListState (&runner.listState());
            parameters.setDcas (&dcas);
            parameters.setSurfaces (&table);

            listId = document.createList ("Sound").id;

            std::vector<std::string> made;
            const auto created = document.createSurface (profile, "Desk", {}, {}, made);
            REQUIRE (created.ok);
            surfaceId = created.id;
            strips = made;

            groupId = document.createCue (listId, 0, "group", "Bank").id;
            set ("/godot/cue/" + groupId + "/mode", "sampler");

            for (int i = 0; i < memberCount; ++i)
            {
                const auto member = document.createCue (groupId, i, "media", "Clip " + std::to_string (i)).id;
                set ("/godot/cue/" + member + "/file", "clip" + std::to_string (i) + ".wav");
                members.push_back (member);
            }

            document.createCue (listId, 1, "memo", "After");
        }

        void set (const std::string& address, const std::string& text)
        {
            REQUIRE_MESSAGE (document.setAttribute (address, text).ok, address << " = " << text);
        }

        void tickOnce()
        {
            bridge.beforeTick (submit, tick);
            runner.beforeTick (engine, tick);
            engine.processTick (tick);
            parameters.markStale();
            snapshot = parameters.publish (tick, state);
            bridge.afterTick (snapshot, touches, tick);
            ++tick;
        }

        void ticks (int count)
        {
            for (int n = 0; n < count; ++n)
                tickOnce();
        }

        template <typename Predicate>
        bool tickUntil (Predicate ready, int bound = 400)
        {
            for (int n = 0; n < bound; ++n)
            {
                if (ready())
                    return true;

                tickOnce();
            }

            return ready();
        }

        const cue::Run* liveRunOf (const std::string& cueId) const
        {
            const cue::Run* newest = nullptr;

            for (const auto& run : runs.all())
                if (run.cue == cueId && ! run.isFinished())
                    newest = runs.find (run.id);

            return newest;
        }

        /*  GO on the bank from standby, and let its arms land: both members
            armed, each on its strip. */
        void arm()
        {
            REQUIRE (document.setAttribute (cue::standbyAddressOf (listId), groupId).ok);
            tickOnce();
            REQUIRE (engine.submit ("cli", "go"));
            tickOnce();

            REQUIRE (tickUntil ([this]
                                {
                                    return std::all_of (members.begin(), members.end(),
                                                        [this] (const std::string& member)
                                                        { return liveRunOf (member) != nullptr; });
                                }));

            audio.completeArms (engine);
            tickOnce();
            tickOnce();
        }

        void arrive (const std::string& port, midi::Bytes bytes)
        {
            REQUIRE (bridge.arrived (port, bytes));
        }

        std::string published (const std::string& address)
        {
            REQUIRE (snapshot != nullptr);
            return client::model::text (*snapshot, address);
        }

        RecordingSink sink;
        surface::SurfaceTable table;
        surface::SurfaceBridge bridge { sink, table };

        std::vector<Event> submitted;
        bool forward = true;

        surface::SurfaceBridge::Submit submit = [this] (Event event)
        {
            submitted.push_back (event);
            return forward ? engine.submit (std::move (event)) : true;
        };

        FakePlayer audio;
        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::DcaTable dcas;
        tree::TouchTable touches;
        doc::IdRegistry runIds { doc::IdRegistry::withSeed (29) };
        cue::Focus focus;
        cue::Runner runner { document, runs, runIds, focus };
        tree::MountTable mounts;
        tree::ParameterTree parameters { document, engine.commands(), mounts, runs };
        tree::EngineState state;
        std::shared_ptr<const tree::TreeSnapshot> snapshot;

        std::string listId;
        std::string surfaceId;
        std::string groupId;
        std::vector<std::string> strips;
        std::vector<std::string> members;
        std::int64_t tick = 1;
    };

    //==========================================================================
    /*  A SNAPSHOT WRITTEN BY HAND, for what the unit suite's tree cannot
        publish - a run's timbre wants an analysed file - and for driving the
        bridge's hooks one at a time. Sorted on the way out, because `find` is
        a binary search and says so (TreeSnapshot.cpp). */
    struct FakeTree
    {
        void text (const std::string& address, const std::string& value)
        {
            put (address, osc::Value::string (value), 's');
        }

        void number (const std::string& address, double value)
        {
            put (address, osc::Value::float64 (value), 'd');
        }

        std::shared_ptr<const tree::TreeSnapshot> publish (std::int64_t at) const
        {
            auto nodes = std::make_shared<std::vector<tree::Node>> (leaves);

            std::sort (nodes->begin(), nodes->end(),
                       [] (const tree::Node& a, const tree::Node& b) { return a.address < b.address; });

            return std::make_shared<const tree::TreeSnapshot> (
                at, nodes, std::make_shared<const std::vector<tree::Node>>(), std::vector<tree::Node> {});
        }

        void put (const std::string& address, const osc::Value& value, char tag)
        {
            for (auto& existing : leaves)
                if (existing.address == address)
                {
                    existing.values = { value };
                    return;
                }

            tree::Node leaf;
            leaf.address = address;
            leaf.kind = tree::Kind::state;
            leaf.access = tree::Access::read;
            leaf.typeTags = std::string (1, tag);
            leaf.values = { value };
            leaves.push_back (std::move (leaf));
        }

        std::vector<tree::Node> leaves;
    };
}

//==============================================================================
TEST_CASE ("surface bridge: the engine's fader curve is the client's, point for point")
{
    /*  TWO COPIES OF FOUR POINTS, because the engine links no client: this is
        what makes moving one without the other fail a build. */
    for (auto tenth = -1300; tenth <= 200; tenth += 5)
    {
        const auto decibels = static_cast<double> (tenth) / 10.0;
        INFO ("at " << decibels << " dB");
        CHECK (near (surface::fractionForDb (decibels), client::model::fractionForDb (decibels)));
    }

    for (auto step = 0; step <= 1000; ++step)
    {
        const auto fraction = static_cast<double> (step) / 1000.0;
        INFO ("at " << fraction);
        CHECK (near (surface::dbForFraction (fraction), client::model::dbForFraction (fraction)));
    }

    /*  THE ENDS ARE EXACT: the bottom of the travel is silence, which is where
        a parked fader lives, and the top is the row's +12. */
    CHECK (surface::fourteenBitForDb (-120.0) == 0);
    CHECK (surface::fourteenBitForDb (-500.0) == 0);
    CHECK (surface::fourteenBitForDb (12.0) == 16383);
    CHECK (surface::fourteenBitForDb (0.0) == 13926);
    CHECK (near (surface::dbForFourteenBit (0), -120.0));
    CHECK (near (surface::dbForFourteenBit (16383), 12.0));

    /*  AND A POSITION TAKEN TO DECIBELS AND BACK IS THE SAME POSITION, at
        every one of the sixteen thousand: that is what lets the bridge know
        its own fader's echo when the engine answers with it. */
    auto strays = 0;

    for (auto position = 0; position <= surface::faderTop; ++position)
        if (surface::fourteenBitForDb (surface::dbForFourteenBit (position)) != position)
            ++strays;

    CHECK (strays == 0);
}

TEST_CASE ("surface bridge: a D700 fader lands on its engraving - +7 at the top, and 0, -24 and -48 where they are engraved")
{
    /*  Measured on the author's unit, 2026-09-25 ("The maximum I see engraved
        is +7dB. When set at 0dB (engraved) it reads -6dB on the screen"): the
        positions the fader sent while it was stopped on each engraved mark.
        Each one reads what is engraved beside it. */
    const auto law = surface::FaderLaw::d700;

    CHECK (near (surface::dbForFourteenBit (16383, law), 7.0));
    CHECK (std::abs (surface::dbForFourteenBit (13040, law) - 0.0) < 0.05);
    CHECK (std::abs (surface::dbForFourteenBit (5600, law) + 24.0) < 0.05);
    CHECK (std::abs (surface::dbForFourteenBit (2095, law) + 48.0) < 0.05);
    CHECK (near (surface::dbForFourteenBit (0, law), -120.0));

    //  Above the engraving's top is the top: a trim at +12 puts a D700 fader as high as it goes.
    CHECK (surface::fourteenBitForDb (12.0, law) == 16383);
    CHECK (surface::fourteenBitForDb (7.0, law) == 16383);
    CHECK (surface::fourteenBitForDb (-120.0, law) == 0);

    //  The generic curve is untouched: unity where the virtual panel draws it.
    CHECK (surface::fourteenBitForDb (0.0) == 13926);
    CHECK (surface::topologyOf (surface::Profile::d700).faderLaw == law);
    CHECK (surface::topologyOf (surface::Profile::mcu).faderLaw == surface::FaderLaw::generic);

    //  A position to decibels and back is the same position under this law too: the echo rule.
    auto strays = 0;

    for (auto position = 0; position <= surface::faderTop; ++position)
        if (surface::fourteenBitForDb (surface::dbForFourteenBit (position, law), law) != position)
            ++strays;

    CHECK (strays == 0);

    //  AND THE BRIDGE USES IT: a D700 fader on its engraved 0 writes nought.
    Desk desk;
    const auto d700 = desk.makeSurface ("d700", "The D700");
    const auto band = desk.makeDca ("Band");
    desk.pin (desk.strips[d700][0], band);

    desk.declare ({ desk.spec (d700, "d700", { "PORTBNK1", "PORTBNK2" }) },
                  { { "PORTBNK1", plugged ("D700 bank 1") }, { "PORTBNK2", plugged ("D700 bank 2") } });
    desk.ticks (3);
    desk.clear();

    desk.arrive ("PORTBNK1", { 0xe0, 0x70, 0x65 });     // 13040: (0x65 << 7) | 0x70
    desk.tickOnce();

    REQUIRE (desk.submitted.size() == 1u);
    CHECK (desk.submitted[0].args[0].getString() == "/godot/dca/" + band + "/trim");
    CHECK (std::abs (desk.submitted[0].args[1].getFloat64()) < 0.05);

    //  And the motor goes where the engraving says: a DCA written to -24 flies to its mark.
    desk.clear();
    desk.write ("/godot/dca/" + band + "/trim", -24.0);
    desk.ticks (30);

    const auto moves = motorMoves (desk.sink, "PORTBNK1", 0);
    REQUIRE_FALSE (moves.empty());
    CHECK (std::abs (moves.back() - 5600) <= 4);
}

TEST_CASE ("surface bridge: the profiles say what each surface has and what its buttons mean")
{
    CHECK (surface::profileFor ("mcu") == surface::Profile::mcu);
    CHECK (surface::profileFor ("d700") == surface::Profile::d700);
    CHECK (surface::profileFor ("midiPads") == surface::Profile::midiPads);
    CHECK (surface::profileFor ("virtual") == surface::Profile::virtualPanel);
    CHECK_FALSE (surface::profileFor ("hui").has_value());

    CHECK (surface::topologyOf (surface::Profile::d700).hasRgb);
    CHECK (surface::topologyOf (surface::Profile::d700).nativeDisplay);
    CHECK_FALSE (surface::topologyOf (surface::Profile::mcu).hasRgb);
    CHECK_FALSE (surface::topologyOf (surface::Profile::midiPads).hasFaders);
    CHECK (surface::topologyOf (surface::Profile::midiPads).hasPads);
    CHECK_FALSE (surface::topologyOf (surface::Profile::virtualPanel).drivenOverMidi);

    const auto mcu = surface::Profile::mcu;
    CHECK (surface::actionFor (mcu, surface::buttonForNote (0x23)) == surface::Action::gate);
    CHECK (surface::actionFor (mcu, surface::buttonForNote (0x5e)) == surface::Action::go);
    CHECK (surface::actionFor (mcu, surface::buttonForNote (0x5d)) == surface::Action::stop);
    CHECK (surface::actionFor (mcu, surface::buttonForNote (0x5b)) == surface::Action::rewind);
    CHECK (surface::actionFor (mcu, surface::buttonForNote (0x5c)) == surface::Action::forward);

    //  SELECT is reserved (plan decision 12); banking is decided in hand (§3.9d).
    CHECK (surface::actionFor (mcu, surface::buttonForNote (0x18)) == surface::Action::none);
    CHECK (surface::actionFor (mcu, surface::buttonForNote (0x2e)) == surface::Action::none);
    CHECK (surface::actionFor (mcu, surface::buttonForNote (0x5f)) == surface::Action::none);
    CHECK (surface::actionFor (surface::Profile::midiPads, surface::buttonForNote (0x5e))
             == surface::Action::none);

    //  The numbers the bench revises, pinned where they are today.
    CHECK (surface::motorStepPerTick == 819);
    CHECK (surface::colourIntervalTicks == 5);
    CHECK (surface::idleColourReassertTicks == 100);
    CHECK (surface::doubleStopTicks == 38);
}

TEST_CASE ("surface bridge: colours are read from what the show writes, and compared in eight levels")
{
    CHECK (is (surface::colourFromHex ("#FF8000"), 127, 64, 0));
    CHECK (is (surface::colourFromHex ("#ff8000"), 127, 64, 0));
    CHECK_FALSE (surface::colourFromHex ("FF8000").has_value());
    CHECK_FALSE (surface::colourFromHex ("#FF80").has_value());
    CHECK_FALSE (surface::colourFromHex ("#GG8000").has_value());
    CHECK_FALSE (surface::colourFromHex ("").has_value());

    //  A timbre is "h s l": the hue in degrees, then saturation and lightness.
    CHECK (is (surface::colourFromTimbre ("0 1 0.5"), 127, 0, 0));
    CHECK (is (surface::colourFromTimbre ("120 1 0.5"), 0, 127, 0));
    CHECK (is (surface::colourFromTimbre ("240 1 0.5"), 0, 0, 127));
    CHECK (is (surface::colourFromTimbre ("200 0 1"), 127, 127, 127));

    /*  THE SATURATION IS KEPT, whatever the lightness (author, 2026-09-23): a
        pure tone is the same vivid red high or low - read as HSL it would have
        been a dark red low and a pink high - and a broad spectrum is paler. */
    CHECK (is (surface::colourFromTimbre ("0 1 0.15"), 127, 0, 0));
    CHECK (is (surface::colourFromTimbre ("0 1 0.85"), 127, 0, 0));
    CHECK (is (surface::colourFromTimbre ("0 0.5 0.85"), 127, 64, 64));

    //  SILENCE HAS NO COLOUR, and on an LED that is dark.
    CHECK (is (surface::colourFromTimbre ("0 0 0"), 0, 0, 0));

    //  Not analysed yet, or not a timbre: nothing, and the strip falls back.
    CHECK_FALSE (surface::colourFromTimbre ("").has_value());
    CHECK_FALSE (surface::colourFromTimbre ("0 1").has_value());
    CHECK_FALSE (surface::colourFromTimbre ("0 1 0.5 7").has_value());

    const auto levels = surface::colourLevels ({ 127, 64, 15 });
    CHECK (levels.red == 7);
    CHECK (levels.green == 4);
    CHECK (levels.blue == 0);
}

//==============================================================================
TEST_CASE ("surface bridge: a fader's positions in one tick are one write, the last, from the surface")
{
    Desk desk;
    const auto mcu = desk.makeSurface ("mcu", "Desk");
    const auto band = desk.makeDca ("Band");
    desk.pin (desk.strips[mcu][0], band);

    desk.declare ({ desk.spec (mcu, "mcu", { "PORTMCU1" }) }, { { "PORTMCU1", plugged ("Desk port") } });
    desk.ticks (3);
    desk.clear();

    /*  THREE POSITIONS IN ONE TICK - a hand moving - and one write: the tree
        shows one value a tick, and the log would otherwise fill with positions
        nobody could have seen. */
    desk.arrive ("PORTMCU1", { 0xe0, 0x00, 0x20 });
    desk.arrive ("PORTMCU1", { 0xe0, 0x00, 0x40 });
    desk.arrive ("PORTMCU1", { 0xe0, 0x7f, 0x5f });
    desk.tickOnce();

    const auto last = surface::dbForFourteenBit ((0x5f << 7) | 0x7f);

    REQUIRE (desk.submitted.size() == 1u);
    const auto& write = desk.submitted.front();
    CHECK (write.origin == "surface:" + mcu);
    CHECK (write.command == "node.set");
    REQUIRE (write.args.size() == 2u);
    CHECK (write.args[0].getString() == "/godot/dca/" + band + "/trim");
    CHECK (write.args[1].isFloat64());
    CHECK (near (write.args[1].getFloat64(), last));

    //  Applied through the live door: the DCA is where the hand left the fader.
    CHECK (near (desk.dcas.trimOf (band), last));

    /*  AND THE MOTOR IS NOT SENT BACK TO WHERE THE HAND ALREADY PUT IT: the
        engine's answer is the fader's own position, which is an echo. */
    CHECK (motorMoves (desk.sink, "PORTMCU1", 0).empty());
}

TEST_CASE ("surface bridge: a touch holds the strip's target, and reaches the queue before that tick's write")
{
    Desk desk;
    const auto mcu = desk.makeSurface ("mcu", "Desk");
    const auto band = desk.makeDca ("Band");
    const auto& strips = desk.strips[mcu];
    desk.pin (strips[0], band);

    desk.declare ({ desk.spec (mcu, "mcu", { "PORTMCU1" }) }, { { "PORTMCU1", plugged ("Desk port") } });
    desk.ticks (3);
    desk.clear();

    const auto trim = "/godot/dca/" + band + "/trim";
    const auto origin = "surface:" + mcu;

    desk.arrive ("PORTMCU1", { 0x90, 0x68, 0x7f });     // a finger on fader one
    desk.arrive ("PORTMCU1", { 0xe0, 0x00, 0x40 });     // and it moves
    desk.tickOnce();

    REQUIRE (desk.submitted.size() == 2u);
    CHECK (desk.submitted[0].command == "node.touch");
    CHECK (desk.submitted[0].args[0].getString() == trim);
    CHECK (desk.submitted[1].command == "node.set");
    CHECK (desk.touches.isHeld (origin, trim));

    /*  THE TOUCH TABLE HOLDS IT FOR THIS SURFACE, and for nobody else: the
        window still hears the trim move. */
    CHECK (desk.touches.holdersOf (trim) == std::vector<std::string> { origin });

    /*  A POSITION THAT ARRIVED BEFORE THE RELEASE is still written after it:
        the write is the tick's, and the tick's touches go first. */
    desk.clear();
    desk.arrive ("PORTMCU1", { 0xe0, 0x00, 0x30 });
    desk.arrive ("PORTMCU1", { 0x90, 0x68, 0x00 });
    desk.tickOnce();

    REQUIRE (desk.submitted.size() == 2u);
    CHECK (desk.submitted[0].command == "node.release");
    CHECK (desk.submitted[0].args[0].getString() == trim);
    CHECK (desk.submitted[1].command == "node.set");
    CHECK_FALSE (desk.touches.isHeld (origin, trim));

    for (const auto& event : desk.submitted)
        CHECK (event.origin == origin);

    //  A STRIP RIDING NOTHING writes nothing, touched or moved.
    desk.clear();
    desk.arrive ("PORTMCU1", { 0x90, 0x69, 0x7f });
    desk.arrive ("PORTMCU1", { 0xe1, 0x00, 0x40 });
    desk.tickOnce();
    CHECK (desk.submitted.empty());
}

TEST_CASE ("surface bridge: the V-Pot press is a sampler strip's gate, and puts a DCA back to nought")
{
    Desk desk;
    const auto mcu = desk.makeSurface ("mcu", "Desk");
    const auto band = desk.makeDca ("Band");
    const auto& strips = desk.strips[mcu];
    desk.pin (strips[1], band);
    desk.write ("/godot/dca/" + band + "/trim", -6.0, "cli");

    desk.declare ({ desk.spec (mcu, "mcu", { "PORTMCU1" }) }, { { "PORTMCU1", plugged ("Desk port") } });
    desk.ticks (3);
    desk.forward = false;
    desk.clear();

    //  Strip one is a sampler strip, the default: its V-Pot, down and up.
    desk.arrive ("PORTMCU1", { 0x90, 0x20, 0x7f });
    desk.tickOnce();

    REQUIRE (desk.submitted.size() == 1u);
    CHECK (desk.submitted[0].command == "strip.press");
    REQUIRE (desk.submitted[0].args.size() == 1u);          // a button carries no velocity
    CHECK (desk.submitted[0].args[0].getString() == strips[0]);
    CHECK (desk.submitted[0].origin == "surface:" + mcu);

    desk.clear();
    desk.arrive ("PORTMCU1", { 0x90, 0x20, 0x00 });
    desk.tickOnce();

    REQUIRE (desk.submitted.size() == 1u);
    CHECK (desk.submitted[0].command == "strip.release");
    CHECK (desk.submitted[0].args[0].getString() == strips[0]);

    //  ON A DCA STRIP the gate is its DCA back to nought, and letting go is nothing.
    desk.clear();
    desk.arrive ("PORTMCU1", { 0x90, 0x21, 0x7f });
    desk.arrive ("PORTMCU1", { 0x90, 0x21, 0x00 });
    desk.tickOnce();

    REQUIRE (desk.submitted.size() == 1u);
    CHECK (desk.submitted[0].command == "node.set");
    CHECK (desk.submitted[0].args[0].getString() == "/godot/dca/" + band + "/trim");
    CHECK (near (desk.submitted[0].args[1].getFloat64(), 0.0));

    //  SELECT is left alone (plan decision 12).
    desk.clear();
    desk.arrive ("PORTMCU1", { 0x90, 0x18, 0x7f });
    desk.arrive ("PORTMCU1", { 0x90, 0x18, 0x00 });
    desk.tickOnce();
    CHECK (desk.submitted.empty());
}

TEST_CASE ("surface bridge: STOP is Esc, STOP again inside the window is double Esc, and PLAY is GO")
{
    Desk desk;
    const auto mcu = desk.makeSurface ("mcu", "Desk");

    desk.declare ({ desk.spec (mcu, "mcu", { "PORTMCU1" }) }, { { "PORTMCU1", plugged ("Desk port") } });
    desk.ticks (2);
    desk.forward = false;

    /*  A button pressed and let go in one tick, and what the bridge made of
        it: every command it submitted, by name. */
    const auto pressing = [&desk, &mcu] (std::uint8_t note)
    {
        desk.clear();
        desk.arrive ("PORTMCU1", { 0x90, note, 0x7f });
        desk.arrive ("PORTMCU1", { 0x90, note, 0x00 });
        desk.tickOnce();

        std::string said;

        for (const auto& event : desk.submitted)
        {
            CHECK (event.origin == "surface:" + mcu);
            said += (said.empty() ? "" : " ") + event.command;
        }

        return said;
    };

    CHECK (pressing (0x5d) == "run.stopAll");

    /*  THIRTY-EIGHT TICKS LATER, counted inclusively, is still the second
        press; thirty-nine after the one before is a first press again. */
    desk.ticks (37);
    CHECK (pressing (0x5d) == "run.killAll");
    desk.ticks (38);
    CHECK (pressing (0x5d) == "run.stopAll");

    CHECK (pressing (0x5e) == "go");
    CHECK (pressing (0x5b) == "standby.previous");
    CHECK (pressing (0x5c) == "standby.next");

    //  The bank and channel arrows, and REC: nothing (§3.9d; §16.6).
    CHECK (pressing (0x2e).empty());
    CHECK (pressing (0x2f).empty());
    CHECK (pressing (0x30).empty());
    CHECK (pressing (0x31).empty());
    CHECK (pressing (0x5f).empty());
}

TEST_CASE ("surface bridge: a turn of a rotary moves nothing - the level is the fader's")
{
    /*  The author, 2026-09-25: "The rotaries don't have to move with the
        faders. It's either or. We'll find other uses for the rotaries." Until
        this date a detent was half a decibel on the strip's target; now a
        turn, either way and however many, writes nothing at all. */
    Desk desk;
    const auto mcu = desk.makeSurface ("mcu", "Desk");
    const auto band = desk.makeDca ("Band");
    desk.pin (desk.strips[mcu][1], band);

    const auto trim = "/godot/dca/" + band + "/trim";
    desk.write (trim, -6.0, "cli");

    desk.declare ({ desk.spec (mcu, "mcu", { "PORTMCU1" }) }, { { "PORTMCU1", plugged ("Desk port") } });
    desk.ticks (3);
    desk.forward = false;
    desk.clear();

    desk.arrive ("PORTMCU1", { 0xb0, 0x11, 0x02 });     // strip two, two detents clockwise
    desk.arrive ("PORTMCU1", { 0xb0, 0x11, 0x41 });     // and one back
    desk.tickOnce();

    CHECK (desk.submitted.empty());
}

TEST_CASE ("surface bridge: a pad is a press with its velocity and a release, on its note and its channel")
{
    Desk desk;
    const auto pads = desk.makeSurface ("midiPads", "Pads");
    const auto& strips = desk.strips[pads];
    REQUIRE (strips.size() == 16u);

    auto spec = desk.spec (pads, "midiPads", { "PORTPADS" });
    spec.channel = 10;
    spec.firstNote = 36;

    desk.declare ({ spec }, { { "PORTPADS", plugged ("Pads") } });
    desk.ticks (2);
    desk.forward = false;
    desk.clear();

    desk.arrive ("PORTPADS", { 0x99, 37, 90 });         // channel 10, note 37: pad two
    desk.tickOnce();

    REQUIRE (desk.submitted.size() == 1u);
    CHECK (desk.submitted[0].command == "strip.press");
    CHECK (desk.submitted[0].origin == "surface:" + pads);
    REQUIRE (desk.submitted[0].args.size() == 2u);
    CHECK (desk.submitted[0].args[0].getString() == strips[1]);
    CHECK (desk.submitted[0].args[1] == osc::Value::int32 (90));

    desk.clear();
    desk.arrive ("PORTPADS", { 0x89, 37, 64 });         // note-off, whatever its velocity
    desk.tickOnce();

    REQUIRE (desk.submitted.size() == 1u);
    CHECK (desk.submitted[0].command == "strip.release");
    CHECK (desk.submitted[0].args[0].getString() == strips[1]);

    //  A NOTE-ON OF VELOCITY NOUGHT IS A RELEASE TOO.
    desk.clear();
    desk.arrive ("PORTPADS", { 0x99, 36, 100 });
    desk.arrive ("PORTPADS", { 0x99, 36, 0 });
    desk.tickOnce();

    REQUIRE (desk.submitted.size() == 2u);
    CHECK (desk.submitted[0].command == "strip.press");
    CHECK (desk.submitted[1].command == "strip.release");
    CHECK (desk.submitted[1].args[0].getString() == strips[0]);

    //  Another channel, and a note below the first pad: owned, and nothing.
    desk.clear();
    desk.arrive ("PORTPADS", { 0x90, 37, 90 });
    desk.arrive ("PORTPADS", { 0x99, 35, 90 });
    desk.tickOnce();
    CHECK (desk.submitted.empty());

    //  Pressure on a pad with nothing on it rides nothing.
    desk.arrive ("PORTPADS", { 0x99, 38, 90 });
    desk.arrive ("PORTPADS", { 0xa9, 38, 70 });
    desk.arrive ("PORTPADS", { 0xd9, 80 });
    desk.tickOnce();
    CHECK (named (desk.submitted, "node.set").empty());

    //  And a pad controller is shown nothing.
    CHECK (desk.sink.sent.empty());
}

TEST_CASE ("surface bridge: pressure rides the trim of a pad that is down, when its clip asks for it")
{
    Stage stage { "midiPads" };
    stage.set ("/godot/cue/" + stage.members[0] + "/pressure", "true");
    stage.set ("/godot/cue/" + stage.members[0] + "/velocityFloor", "-40");
    stage.arm();

    const auto* first = stage.liveRunOf (stage.members[0]);
    REQUIRE (first != nullptr);
    const auto trim = "/godot/run/" + first->id + "/trim";
    REQUIRE (stage.published ("/godot/slot/" + stage.strips[0] + "/target") == trim);

    surface::SurfaceSpec spec;
    spec.id = stage.surfaceId;
    spec.profile = "midiPads";
    spec.ports = { "PORTPADS" };
    spec.strips = stage.strips;
    stage.bridge.declare ({ spec }, [] (const std::string&) { return plugged ("Pads"); });
    stage.tickOnce();
    stage.forward = false;
    stage.submitted.clear();

    //  Pressure on a pad that is not down: nothing.
    stage.arrive ("PORTPADS", { 0xa0, 36, 100 });
    stage.tickOnce();
    CHECK (stage.submitted.empty());

    //  DOWN, THEN PRESSED HARDER: one write, on velocity's line.
    stage.arrive ("PORTPADS", { 0x90, 36, 100 });
    stage.arrive ("PORTPADS", { 0xa0, 36, 64 });
    stage.tickOnce();

    REQUIRE (stage.submitted.size() == 2u);
    CHECK (stage.submitted[0].command == "strip.press");
    CHECK (stage.submitted[1].command == "node.set");
    CHECK (stage.submitted[1].args[0].getString() == trim);
    CHECK (near (stage.submitted[1].args[1].getFloat64(), cue::Runner::levelForByte (64, -40.0)));

    //  Channel pressure goes to the pad pressed last and still down.
    stage.submitted.clear();
    stage.arrive ("PORTPADS", { 0xd0, 127 });
    stage.tickOnce();

    REQUIRE (stage.submitted.size() == 1u);
    CHECK (stage.submitted[0].args[0].getString() == trim);
    CHECK (near (stage.submitted[0].args[1].getFloat64(), 0.0));

    //  A PRESSURE OF NOUGHT IS IGNORED (plan decision 15).
    stage.submitted.clear();
    stage.arrive ("PORTPADS", { 0xd0, 0 });
    stage.arrive ("PORTPADS", { 0xa0, 36, 0 });
    stage.tickOnce();
    CHECK (stage.submitted.empty());

    //  A clip that does not ask for pressure is not ridden by it...
    stage.arrive ("PORTPADS", { 0x90, 37, 100 });
    stage.arrive ("PORTPADS", { 0xa0, 37, 64 });
    stage.tickOnce();

    REQUIRE (stage.submitted.size() == 1u);
    CHECK (stage.submitted[0].command == "strip.press");

    //  ...and channel pressure now goes to it, the pad pressed last: nothing.
    stage.submitted.clear();
    stage.arrive ("PORTPADS", { 0xd0, 90 });
    stage.tickOnce();
    CHECK (stage.submitted.empty());

    //  Let go, a pad's pressure is nothing.
    stage.arrive ("PORTPADS", { 0x80, 36, 0 });
    stage.arrive ("PORTPADS", { 0xa0, 36, 90 });
    stage.tickOnce();

    REQUIRE (stage.submitted.size() == 1u);
    CHECK (stage.submitted[0].command == "strip.release");
    CHECK (stage.submitted[0].args[0].getString() == stage.strips[0]);
}

//==============================================================================
TEST_CASE ("surface bridge: a motor follows its target a twentieth at a time, and never against the hand")
{
    Desk desk;
    const auto mcu = desk.makeSurface ("mcu", "Desk");
    const auto band = desk.makeDca ("Band");
    desk.pin (desk.strips[mcu][0], band);

    const auto trim = "/godot/dca/" + band + "/trim";
    const auto unity = surface::fourteenBitForDb (0.0);

    desk.declare ({ desk.spec (mcu, "mcu", { "PORTMCU1" }) }, { { "PORTMCU1", plugged ("Desk port") } });
    desk.tickOnce();

    /*  THE FIRST PAINT, with nobody knowing where any fader is: the strip at
        unity goes there, and a strip riding nothing stops a step short of the
        bottom before it lands - never the whole travel into the stop. */
    CHECK (motorMoves (desk.sink, "PORTMCU1", 0) == std::vector<int> { unity });
    CHECK (motorMoves (desk.sink, "PORTMCU1", 1) == std::vector<int> { surface::motorStepPerTick });

    desk.clear();
    desk.tickOnce();
    CHECK (motorMoves (desk.sink, "PORTMCU1", 1) == std::vector<int> { 0 });
    CHECK (motorMoves (desk.sink, "PORTMCU1", 0).empty());

    //  THE TRIM MOVED FROM THE WINDOW, and the motor follows, a step a tick.
    desk.clear();
    desk.write (trim, -20.0);
    desk.ticks (10);

    const auto moves = motorMoves (desk.sink, "PORTMCU1", 0);
    REQUIRE_FALSE (moves.empty());
    CHECK (moves.back() == surface::fourteenBitForDb (-20.0));
    CHECK (moves.size() == 5u);

    auto from = unity;

    for (const auto position : moves)
    {
        CHECK (std::abs (position - from) <= surface::motorStepPerTick);
        CHECK (position < from);
        from = position;
    }

    /*  NOT WHILE THIS SURFACE HOLDS IT: the finger on the fader is the one
        setting it, and a write from elsewhere moves no motor under it. */
    desk.clear();
    desk.arrive ("PORTMCU1", { 0x90, 0x68, 0x7f });
    desk.tickOnce();
    desk.write (trim, -40.0);
    desk.ticks (5);
    CHECK (motorMoves (desk.sink, "PORTMCU1", 0).empty());

    //  NOR TO WHERE THE HAND PUT IT: written and read back, that is an echo.
    desk.arrive ("PORTMCU1", surface::faderPosition (0, 12000));
    desk.ticks (3);
    CHECK (motorMoves (desk.sink, "PORTMCU1", 0).empty());
    CHECK (near (desk.dcas.trimOf (band), surface::dbForFourteenBit (12000)));

    //  LET GO: the value is sent once, where the hand left it, and nothing after.
    desk.arrive ("PORTMCU1", { 0x90, 0x68, 0x00 });
    desk.tickOnce();
    CHECK (motorMoves (desk.sink, "PORTMCU1", 0) == std::vector<int> { 12000 });

    desk.ticks (5);
    CHECK (motorMoves (desk.sink, "PORTMCU1", 0) == std::vector<int> { 12000 });
}

TEST_CASE ("surface bridge: an MCU strip shows its name and its word in seven characters, once")
{
    Desk desk;
    const auto mcu = desk.makeSurface ("mcu", "Desk");
    const auto ambiences = desk.makeDca ("Ambiences");
    desk.pin (desk.strips[mcu][0], ambiences);

    desk.declare ({ desk.spec (mcu, "mcu", { "PORTMCU1" }) }, { { "PORTMCU1", plugged ("Desk port") } });
    desk.tickOnce();

    const auto sent = sentOn (desk.sink, "PORTMCU1");

    /*  ROW ONE: the DCA's name, cut to the field - its short name, when
        somebody writes one. Written out by hand as the guide prints it. */
    CHECK (contains (sent, midi::Bytes { 0xf0, 0x00, 0x00, 0x66, 0x14, 0x12, 0x00,
                                         'A', 'm', 'b', 'i', 'e', 'n', 'c', 0xf7 }));

    //  ROW TWO: the strip's word, padded to seven - the buffer is flat.
    CHECK (contains (sent, midi::Bytes { 0xf0, 0x00, 0x00, 0x66, 0x14, 0x12, 0x38,
                                         'd', 'c', 'a', ' ', ' ', ' ', ' ', 0xf7 }));

    //  A free strip says so, on both rows, in its own cell.
    CHECK (contains (sent, surface::lcdCell (0x14, 0, 1, "free")));
    CHECK (contains (sent, surface::lcdCell (0x14, 1, 1, "free")));

    /*  Its ring is dark - it does not repeat the fader (author, 2026-09-25) -
        and SELECT is dark. */
    CHECK (contains (sent, surface::ringMcu (0, 0, 2, false)));
    CHECK (contains (sent, surface::led (0x18, surface::Led::off)));

    //  MCU's display only: no native D700 row reaches a generic surface.
    for (const auto& message : sysexOf (desk.sink))
    {
        CHECK (message[5] != 0x1a);
        CHECK (message[5] != 0x19);
    }

    //  NOTHING CHANGED, NOTHING SENT.
    desk.ticks (3);
    desk.clear();
    desk.ticks (3);
    CHECK (sysexOf (desk.sink).empty());

    //  An authored short name replaces the cut: row one again, and only it.
    desk.set ("/godot/dca/" + ambiences + "/shortName", "Amb");
    desk.clear();
    desk.tickOnce();

    const auto again = sysexOf (desk.sink);
    REQUIRE (again.size() == 1u);
    CHECK (again[0] == surface::lcdCell (0x14, 0, 0, "Amb"));
}

TEST_CASE ("surface bridge: a D700 strip shows three native rows and a number, and never MCU's display")
{
    Desk desk;
    const auto d700 = desk.makeSurface ("d700", "The D700");
    const auto band = desk.makeDca ("Band");
    const auto& strips = desk.strips[d700];
    REQUIRE (strips.size() == 16u);
    desk.pin (strips[0], band);
    desk.pin (strips[9], band);

    const auto trim = "/godot/dca/" + band + "/trim";
    desk.write (trim, -6.2, "cli");

    desk.declare ({ desk.spec (d700, "d700", { "PORTBNK1", "PORTBNK2" }) },
                  { { "PORTBNK1", plugged ("D700 bank 1") }, { "PORTBNK2", plugged ("D700 bank 2") } });
    desk.tickOnce();

    const auto first = sentOn (desk.sink, "PORTBNK1");
    const auto second = sentOn (desk.sink, "PORTBNK2");

    //  The name in twelve; the level while it rides one; what it is for, in eight.
    CHECK (contains (first, surface::d700DisplayRow (0, 0, "Band")));
    CHECK (contains (first, midi::Bytes { 0xf0, 0x00, 0x00, 0x66, 0x14, 0x1a, 0x00, 0x02,
                                          '-', '6', '.', '2', ' ', 'd', 'B', ' ', ' ', ' ', ' ', ' ',
                                          0xf7 }));
    CHECK (contains (first, surface::d700DisplayRow3 (0, "dca")));

    CHECK (contains (first, surface::d700DisplayRow (1, 0, "free")));
    CHECK (contains (first, surface::d700DisplayRow (1, 1, "free")));
    CHECK (contains (first, surface::d700DisplayRow3 (1, "free")));

    //  THE SECOND BANK IS THE SECOND PORT: strip ten is element two there.
    CHECK (contains (second, surface::d700DisplayRow (1, 0, "Band")));
    CHECK (contains (second, surface::d700DisplayRow3 (1, "dca")));
    CHECK_FALSE (contains (first, surface::d700DisplayRow3 (1, "dca")));

    //  The numbers: no cue on any strip, so each strip's own.
    CHECK (contains (first, surface::d700TrackNumbers ({ 1, 2, 3, 4, 5, 6, 7, 8 })));
    CHECK (contains (second, surface::d700TrackNumbers ({ 9, 10, 11, 12, 13, 14, 15, 16 })));

    //  The ring dark: it does not repeat the fader (author, 2026-09-25).
    CHECK (contains (first, surface::d700Ring (0, 0, 2)));
    CHECK (contains (second, surface::d700Ring (1, 0, 2)));

    //  NEVER 0x12 ON A D700, and every SysEx one it survives.
    for (const auto& message : sysexOf (desk.sink))
    {
        CHECK (message[5] != 0x12);
        CHECK (surface::isSafeSysEx (message));
    }

    //  Nothing again while nothing changes; the level again when it does.
    desk.ticks (3);
    desk.clear();
    desk.ticks (2);
    CHECK (sysexOf (desk.sink).empty());

    desk.write (trim, -12.0);
    desk.clear();
    desk.tickOnce();

    const auto rows = sysexOf (desk.sink);
    CHECK (rows.size() == 2u);
    CHECK (contains (rows, surface::d700DisplayRow (0, 1, "-12.0 dB")));
    CHECK (contains (sentOn (desk.sink, "PORTBNK2"), surface::d700DisplayRow (1, 1, "-12.0 dB")));
}

TEST_CASE ("surface bridge: a D700 strip wears its cue's colour, blue last, re-asserted, never more than ten a second")
{
    Stage stage { "d700" };
    stage.set ("/godot/cue/" + stage.members[0] + "/colour", "#FF8000");
    stage.arm();

    surface::SurfaceSpec spec;
    spec.id = stage.surfaceId;
    spec.profile = "d700";
    spec.ports = { "PORTBNK1", "PORTBNK2" };
    spec.strips = stage.strips;
    stage.bridge.declare ({ spec }, [] (const std::string& port) { return plugged (port); });
    stage.tickOnce();

    /*  THREE MESSAGES, red, green, then blue - the one that refreshes the
        ring - carrying #FF8000 as the LEDs are sent it: the 127, 64, 0 it is,
        shaped for their light (2026-09-25, `forTheLeds`). */
    const auto orange = surface::forTheLeds (surface::Rgb { 127, 64, 0 });
    CHECK (orange.green < 64);
    CHECK (colourOf (stage.sink, "PORTBNK1", 0x20)
             == std::vector<midi::Bytes> { { 0x91, 0x20, static_cast<std::uint8_t> (orange.red) },
                                           { 0x92, 0x20, static_cast<std::uint8_t> (orange.green) },
                                           { 0x93, 0x20, static_cast<std::uint8_t> (orange.blue) } });

    //  A clip with no colour authored is dark, and so is a free strip.
    CHECK (colourOf (stage.sink, "PORTBNK1", 0x21)
             == std::vector<midi::Bytes> { { 0x91, 0x21, 0 }, { 0x92, 0x21, 0 }, { 0x93, 0x21, 0 } });
    CHECK (colourOf (stage.sink, "PORTBNK2", 0x27).size() == 3u);

    //  And the display says what is on the strip.
    CHECK (contains (sentOn (stage.sink, "PORTBNK1"), surface::d700DisplayRow (0, 0, "Clip 0")));
    CHECK (contains (sentOn (stage.sink, "PORTBNK1"), surface::d700DisplayRow3 (0, "sampler")));

    /*  RE-ASSERTED EVERY TWO SECONDS, unchanged, because the firmware's idle
        animation takes an undriven LED back. */
    stage.sink.sent.clear();
    stage.ticks (static_cast<int> (surface::idleColourReassertTicks) - 1);
    CHECK (colourOf (stage.sink, "PORTBNK1", 0x20).empty());

    stage.tickOnce();
    CHECK (colourOf (stage.sink, "PORTBNK1", 0x20).size() == 3u);

    //  NEVER MORE THAN TEN A SECOND, however often the colour changes.
    stage.sink.sent.clear();

    for (int n = 0; n < 50; ++n)
    {
        stage.set ("/godot/cue/" + stage.members[0] + "/colour", n % 2 == 0 ? "#0000FF" : "#FF0000");
        stage.tickOnce();
    }

    const auto blues = std::count_if (stage.sink.sent.begin(), stage.sink.sent.end(),
                                      [] (const RecordingSink::Message& message)
                                      {
                                          return message.port == "PORTBNK1" && message.bytes.size() == 3
                                                   && message.bytes[0] == 0x93 && message.bytes[1] == 0x20;
                                      });

    CHECK (blues <= 10);
    CHECK (blues >= 5);
    CHECK (stage.bridge.refusedSysEx() == 0u);
}

TEST_CASE ("surface bridge: a sounding strip wears what it sounds like, and its authored colour when it stops")
{
    RecordingSink sink;
    surface::SurfaceTable table;
    surface::SurfaceBridge bridge { sink, table };
    tree::TouchTable touches;

    FakeTree fake;
    fake.text ("/godot/slot/STRIP001/role", "sampler");
    fake.text ("/godot/slot/STRIP001/word", "playing");
    fake.text ("/godot/slot/STRIP001/target", "/godot/run/RUN00001/trim");
    fake.text ("/godot/slot/STRIP001/cue", "CUE00001");
    fake.text ("/godot/slot/STRIP001/holder", "RUN00001");
    fake.number ("/godot/run/RUN00001/trim", 0.0);
    fake.text ("/godot/run/RUN00001/timbre", "0 1 0.5");           // pure red
    fake.text ("/godot/cue/CUE00001/colour", "#0000FF");
    fake.text ("/godot/cue/CUE00001/name", "Rain");

    surface::SurfaceSpec spec;
    spec.id = "SURF0001";
    spec.profile = "d700";
    spec.ports = { "PORTBNK1" };
    spec.strips = { "STRIP001" };
    bridge.declare ({ spec }, [] (const std::string&) { return plugged ("D700"); });

    std::int64_t tick = 1;
    bridge.afterTick (fake.publish (tick), touches, tick);

    CHECK (colourOf (sink, "PORTBNK1", 0x20)
             == std::vector<midi::Bytes> { { 0x91, 0x20, 127 }, { 0x92, 0x20, 0 }, { 0x93, 0x20, 0 } });

    //  IT STOPS: the authored colour, as soon as the rate allows.
    fake.text ("/godot/slot/STRIP001/word", "armed");
    sink.sent.clear();

    for (tick = 2; tick <= 10; ++tick)
        bridge.afterTick (fake.publish (tick), touches, tick);

    CHECK (colourOf (sink, "PORTBNK1", 0x20)
             == std::vector<midi::Bytes> { { 0x91, 0x20, 0 }, { 0x92, 0x20, 0 }, { 0x93, 0x20, 127 } });

    /*  SOUNDING, WITH NO ANALYSIS YET: the authored colour stays - which is
        what is on the ring already, so nothing is sent. */
    fake.text ("/godot/slot/STRIP001/word", "playing");
    fake.text ("/godot/run/RUN00001/timbre", "");
    sink.sent.clear();

    for (tick = 11; tick <= 30; ++tick)
        bridge.afterTick (fake.publish (tick), touches, tick);

    CHECK (colourOf (sink, "PORTBNK1", 0x20).empty());
}

TEST_CASE ("surface bridge: a sounding strip's light is half its level and half how it moves, shaped for the LEDs")
{
    /*  The author, 2026-09-25, in three steps: the brightness modulated by
        the sound's variation; then more adaptive; then "The low level sounds
        with a little variation come out with as much variation in the lights
        as a more dynamic sound. Maybe make part of the LED level match the
        long term level of the music and the other 'half' the shorter term
        variations" - and "The white 'looks' louder". */
    RecordingSink sink;
    surface::SurfaceTable table;
    surface::SurfaceBridge bridge { sink, table };
    tree::TouchTable touches;

    FakeTree fake;
    fake.text ("/godot/slot/STRIP001/role", "sampler");
    fake.text ("/godot/slot/STRIP001/word", "playing");
    fake.text ("/godot/slot/STRIP001/target", "/godot/run/RUN00001/trim");
    fake.text ("/godot/slot/STRIP001/cue", "CUE00001");
    fake.text ("/godot/slot/STRIP001/holder", "RUN00001");
    fake.number ("/godot/run/RUN00001/trim", 0.0);
    fake.text ("/godot/run/RUN00001/timbre", "0 1 0.5");           // pure red
    fake.text ("/godot/cue/CUE00001/name", "Rain");

    surface::SurfaceSpec spec;
    spec.id = "SURF0001";
    spec.profile = "d700";
    spec.ports = { "PORTBNK1" };
    spec.strips = { "STRIP001" };
    bridge.declare ({ spec }, [] (const std::string&) { return plugged ("D700"); });

    //  The red of the LAST colour written since the sink was cleared: three notes a write.
    const auto redNow = [&sink]
    {
        const auto colour = colourOf (sink, "PORTBNK1", 0x20);
        return colour.size() >= 3u ? static_cast<int> (colour[colour.size() - 3][2]) : -1;
    };

    std::int64_t tick = 0;
    std::string run = "RUN00001";

    //  Some seconds of an envelope given tick by tick, to the run on the strip; the reds sent while it ran.
    const auto play = [&] (int ticks, const std::function<double (int)>& envelopeAt)
    {
        std::vector<int> reds;

        for (int n = 0; n < ticks; ++n)
        {
            ++tick;
            fake.text ("/godot/run/" + run + "/envelope", osc::formatDouble (envelopeAt (n)));
            sink.sent.clear();
            bridge.afterTick (fake.publish (tick), touches, tick);

            if (const auto red = redNow(); red >= 0)
                reds.push_back (red);
        }

        return reds;
    };

    const auto swing = [] (const std::vector<int>& reds)
    {
        return reds.empty() ? 0 : *std::max_element (reds.begin(), reds.end())
                                    - *std::min_element (reds.begin(), reds.end());
    };

    //  STEADY, THE LEVEL DECIDES: a loud bed glows well above a quiet one.
    auto loud = play (400, [] (int) { return -8.0; });
    REQUIRE_FALSE (loud.empty());
    const auto loudRest = loud.back();

    const auto onStrip = [&] (const std::string& id, const std::string& timbre)
    {
        run = id;
        fake.text ("/godot/slot/STRIP001/holder", id);                 // a new run starts afresh
        fake.text ("/godot/run/" + id + "/timbre", timbre);
    };

    onStrip ("RUN00002", "0 1 0.5");
    auto quiet = play (400, [] (int) { return -40.0; });
    onStrip ("RUN00001", "0 1 0.5");
    REQUIRE_FALSE (quiet.empty());
    const auto quietRest = quiet.back();

    CHECK (quietRest > 0);                                          // a glow, never dark
    CHECK (loudRest > quietRest + 30);

    //  A HIT on the loud bed flashes towards full.
    play (300, [] (int) { return -8.0; });
    const auto hit = play (6, [] (int) { return -2.0; });
    REQUIRE_FALSE (hit.empty());
    CHECK (hit.back() > loudRest + 20);

    /*  A QUIET SOUND THAT BARELY MOVES BARELY CHANGES, and a dynamic one
        swings: half a decibel of wobble is under the least that counts as a
        flash, six decibels is well over it. */
    onStrip ("RUN00003", "0 1 0.5");
    const auto still = play (600, [] (int n) { return -36.0 + ((n / 5) % 2 == 0 ? 0.5 : -0.5); });

    onStrip ("RUN00004", "0 1 0.5");
    const auto lively = play (600, [] (int n) { return -12.0 + ((n / 5) % 2 == 0 ? 6.0 : -6.0); });

    const auto late = [] (const std::vector<int>& reds)
    {
        return std::vector<int> (reds.begin() + static_cast<std::ptrdiff_t> (reds.size() / 2), reds.end());
    };

    CHECK (swing (late (still)) <= 8);
    CHECK (swing (late (lively)) >= 20);
    CHECK (swing (late (lively)) >= 3 * swing (late (still)));

    /*  WHITE IS HELD TO THE LIGHT BUDGET: a grey timbre lights all three
        channels, and each is sent far below what a pure colour of the same
        brightness gets - "the white looks louder". */
    onStrip ("RUN00005", "0 0 0.5");                                // no saturation: white
    play (400, [] (int) { return -8.0; });
    sink.sent.clear();
    play (6, [] (int) { return -8.0; });
    ++tick;
    bridge.afterTick (fake.publish (tick), touches, tick);

    const auto white = colourOf (sink, "PORTBNK1", 0x20);

    if (white.size() >= 3u)
    {
        const auto whiteRed = static_cast<int> (white[white.size() - 3][2]);
        CHECK (whiteRed < loudRest);
    }

    //  With no envelope yet, the colour is the timbre's, at full: a pure red is a full red.
    onStrip ("RUN00006", "0 1 0.5");
    fake.text ("/godot/run/RUN00006/envelope", "");
    sink.sent.clear();

    for (const auto end = tick + 18; tick <= end; ++tick)
        bridge.afterTick (fake.publish (tick), touches, tick);

    CHECK (redNow() == 127);
}

TEST_CASE ("surface bridge: MUTE on a sampler strip kills what it plays, like the running pane's cross")
{
    /*  The author, 2026-09-25: "Can the mute switch of a sampler fader be a
        kill switch for it? Not temporary muting, kill as the X in the active
        cue list panel." */
    RecordingSink sink;
    surface::SurfaceTable table;
    surface::SurfaceBridge bridge { sink, table };
    tree::TouchTable touches;

    FakeTree fake;
    fake.text ("/godot/slot/STRIP001/role", "sampler");
    fake.text ("/godot/slot/STRIP001/word", "playing");
    fake.text ("/godot/slot/STRIP001/target", "/godot/run/RUN00001/trim");
    fake.text ("/godot/slot/STRIP001/cue", "CUE00001");
    fake.text ("/godot/slot/STRIP001/holder", "RUN00001");
    fake.number ("/godot/run/RUN00001/trim", 0.0);
    fake.text ("/godot/cue/CUE00001/name", "Rain");

    surface::SurfaceSpec spec;
    spec.id = "SURF0001";
    spec.profile = "d700";
    spec.ports = { "PORTBNK1" };
    spec.strips = { "STRIP001" };
    bridge.declare ({ spec }, [] (const std::string&) { return plugged ("D700"); });

    std::vector<Event> submitted;
    const auto collect = [&submitted] (Event event)
    {
        submitted.push_back (std::move (event));
        return true;
    };

    std::int64_t tick = 1;
    bridge.afterTick (fake.publish (tick), touches, tick);

    //  MUTE on strip one, pressed and let go: one kill, of the run on it.
    bridge.arrived ("PORTBNK1", { 0x90, 0x10, 0x7f });
    bridge.arrived ("PORTBNK1", { 0x90, 0x10, 0x00 });
    bridge.beforeTick (collect, ++tick);

    REQUIRE (submitted.size() == 1u);
    CHECK (submitted[0].command == "run.kill");
    REQUIRE (submitted[0].args.size() == 1u);
    CHECK (submitted[0].args[0].getString() == "RUN00001");
    CHECK (submitted[0].origin == "surface:SURF0001");

    /*  AND THE RED LIGHT SAYS SO for half a second (author, 2026-09-25:
        "Can you flash for 0.5s the red mute switch to have feedback on the
        killed sample?"), then goes out. */
    const auto muteOn = midi::Bytes { 0x90, 0x10, 0x7f };
    const auto muteOff = midi::Bytes { 0x90, 0x10, 0x00 };

    sink.sent.clear();
    bridge.afterTick (fake.publish (tick), touches, tick);
    CHECK (contains (sentOn (sink, "PORTBNK1"), muteOn));

    const auto killedAt = tick;
    sink.sent.clear();

    while (tick < killedAt + surface::killFlashTicks - 1)
    {
        ++tick;
        bridge.afterTick (fake.publish (tick), touches, tick);
    }

    CHECK_FALSE (contains (sentOn (sink, "PORTBNK1"), muteOff));

    ++tick;
    bridge.afterTick (fake.publish (tick), touches, tick);
    CHECK (contains (sentOn (sink, "PORTBNK1"), muteOff));

    //  A member armed and waiting has nothing to kill: its fader is ready for the next touch.
    fake.text ("/godot/slot/STRIP001/word", "armed");
    ++tick;
    bridge.afterTick (fake.publish (tick), touches, tick);
    submitted.clear();

    bridge.arrived ("PORTBNK1", { 0x90, 0x10, 0x7f });
    bridge.beforeTick (collect, ++tick);
    CHECK (submitted.empty());

    //  And nothing killed lights nothing.
    sink.sent.clear();
    bridge.afterTick (fake.publish (tick), touches, tick);
    CHECK_FALSE (contains (sentOn (sink, "PORTBNK1"), muteOn));

    //  Nor has a DCA strip: MUTE there is no temporary mute either, it is nothing.
    fake.text ("/godot/slot/STRIP001/word", "dca");
    fake.text ("/godot/slot/STRIP001/role", "dca");
    ++tick;
    bridge.afterTick (fake.publish (tick), touches, tick);
    submitted.clear();

    bridge.arrived ("PORTBNK1", { 0x90, 0x10, 0x7f });
    bridge.beforeTick (collect, ++tick);
    CHECK (submitted.empty());
}

TEST_CASE ("surface bridge: a hand resting through a handover lets go of the old node, and touches the new one only by landing again")
{
    RecordingSink sink;
    surface::SurfaceTable table;
    surface::SurfaceBridge bridge { sink, table };
    tree::TouchTable touches;

    FakeTree fake;
    fake.text ("/godot/slot/STRIP001/role", "sampler");
    fake.text ("/godot/slot/STRIP001/word", "armed");
    fake.text ("/godot/slot/STRIP001/target", "/godot/run/RUN00001/trim");
    fake.number ("/godot/run/RUN00001/trim", -120.0);

    surface::SurfaceSpec spec;
    spec.id = "SURF0001";
    spec.profile = "mcu";
    spec.ports = { "PORTMCU1" };
    spec.strips = { "STRIP001" };
    bridge.declare ({ spec }, [] (const std::string&) { return plugged ("Desk"); });
    bridge.afterTick (fake.publish (1), touches, 1);

    std::vector<Event> events;
    const surface::SurfaceBridge::Submit recording = [&events] (Event event)
    {
        events.push_back (std::move (event));
        return true;
    };

    REQUIRE (bridge.arrived ("PORTMCU1", { 0x90, 0x68, 0x7f }));
    bridge.beforeTick (recording, 2);

    REQUIRE (events.size() == 1u);
    CHECK (events[0].command == "node.touch");
    CHECK (events[0].args[0].getString() == "/godot/run/RUN00001/trim");

    /*  THE CLIP ENDS AND THE MEMBER IS ARMED AGAIN: a new run under the same
        finger. A touch starts a sampler clip (author, 2026-09-23), so the
        finger that did not move must not touch the new run - it lets go of the
        old one and holds nothing. */
    fake.text ("/godot/slot/STRIP001/target", "/godot/run/RUN00002/trim");
    fake.number ("/godot/run/RUN00002/trim", -120.0);
    bridge.afterTick (fake.publish (2), touches, 2);

    events.clear();
    bridge.beforeTick (recording, 3);

    REQUIRE (events.size() == 1u);
    CHECK (events[0].command == "node.release");
    CHECK (events[0].args[0].getString() == "/godot/run/RUN00001/trim");

    //  Nor on any tick after, while the hand stays where it is.
    events.clear();
    bridge.beforeTick (recording, 4);
    CHECK (events.empty());

    //  WHAT IT MOVES GOES TO THE NEW RUN, a ride with no touch, which starts nothing.
    REQUIRE (bridge.arrived ("PORTMCU1", surface::faderPosition (0, 9000)));
    bridge.beforeTick (recording, 5);

    REQUIRE (events.size() == 1u);
    CHECK (events[0].command == "node.set");
    CHECK (events[0].args[0].getString() == "/godot/run/RUN00002/trim");

    //  Lifted, nothing is held to give back; landing again touches the new run.
    events.clear();
    REQUIRE (bridge.arrived ("PORTMCU1", { 0x90, 0x68, 0x00 }));
    bridge.beforeTick (recording, 6);
    CHECK (events.empty());

    REQUIRE (bridge.arrived ("PORTMCU1", { 0x90, 0x68, 0x7f }));
    bridge.beforeTick (recording, 7);

    REQUIRE (events.size() == 1u);
    CHECK (events[0].command == "node.touch");
    CHECK (events[0].args[0].getString() == "/godot/run/RUN00002/trim");

    for (const auto& event : events)
        CHECK (event.origin == "surface:SURF0001");
}

//==============================================================================
TEST_CASE ("surface bridge: a surface nobody can reach is neither driven nor heard, and says why")
{
    Desk desk;
    const auto mcu = desk.makeSurface ("mcu", "Desk");
    const auto band = desk.makeDca ("Band");
    desk.pin (desk.strips[mcu][0], band);

    const auto spec = desk.spec (mcu, "mcu", { "PORTMCU1" });
    const auto base = "/godot/surface/" + mcu + "/";

    //  NO DEVICE BEHIND THE PORT.
    surface::PortState unplugged;
    unplugged.name = "Desk port";

    CHECK (desk.declare ({ spec }, { { "PORTMCU1", unplugged } }));
    desk.ticks (3);

    CHECK (desk.sink.sent.empty());
    CHECK (desk.published (base + "connected") == "false");
    CHECK (desk.published (base + "problem") == "the port \"Desk port\" has no device behind it");

    //  Owned all the same - no trigger fires on a surface's traffic - and dropped.
    CHECK (desk.bridge.arrived ("PORTMCU1", { 0xe0, 0x00, 0x40 }));
    desk.tickOnce();
    CHECK (desk.submitted.empty());

    //  RX OFF: nothing it sends is heard.
    auto deaf = plugged ("Desk port");
    deaf.rx = false;

    CHECK (desk.declare ({ spec }, { { "PORTMCU1", deaf } }));
    desk.tickOnce();
    CHECK (desk.published (base + "problem") == "the port \"Desk port\" has rx turned off");
    CHECK (desk.bridge.arrived ("PORTMCU1", { 0xe0, 0x00, 0x40 }));
    desk.tickOnce();
    CHECK (desk.submitted.empty());
    CHECK (desk.sink.sent.empty());

    //  TX OFF: nothing is sent to it.
    auto mute = plugged ("Desk port");
    mute.tx = false;

    CHECK (desk.declare ({ spec }, { { "PORTMCU1", mute } }));
    desk.ticks (3);
    CHECK (desk.published (base + "problem") == "the port \"Desk port\" has tx turned off");
    CHECK (desk.sink.sent.empty());

    //  NO PORT AT ALL.
    CHECK (desk.declare ({ desk.spec (mcu, "mcu", {}) }, {}));
    desk.tickOnce();
    CHECK (desk.published (base + "problem") == "an mcu surface needs its ports: none is declared");
    CHECK (desk.published (base + "connected") == "false");

    //  PLUGGED IN: connected, asked who it is, and painted whole.
    CHECK (desk.declare ({ spec }, { { "PORTMCU1", plugged ("Desk port") } }));
    desk.tickOnce();

    CHECK (desk.published (base + "connected") == "true");
    CHECK (desk.published (base + "problem").empty());

    const auto sent = sentOn (desk.sink, "PORTMCU1");
    REQUIRE_FALSE (sent.empty());
    CHECK (sent.front() == surface::deviceQuery (0x14));
    CHECK_FALSE (motorMoves (desk.sink, "PORTMCU1", 0).empty());
    CHECK (contains (sent, surface::lcdCell (0x14, 0, 0, "Band")));

    //  A declaration that changes nothing changes nothing, and repaints nothing.
    desk.ticks (3);
    CHECK_FALSE (desk.declare ({ spec }, { { "PORTMCU1", plugged ("Desk port") } }));
    desk.clear();
    desk.ticks (3);
    CHECK (desk.sink.sent.empty());
}

TEST_CASE ("surface bridge: a surface that goes lets go of what its hands held")
{
    Desk desk;
    const auto mcu = desk.makeSurface ("mcu", "Desk");
    const auto band = desk.makeDca ("Band");
    desk.pin (desk.strips[mcu][0], band);

    const auto spec = desk.spec (mcu, "mcu", { "PORTMCU1" });
    const auto trim = "/godot/dca/" + band + "/trim";
    const auto origin = "surface:" + mcu;

    desk.declare ({ spec }, { { "PORTMCU1", plugged ("Desk port") } });
    desk.ticks (2);

    desk.arrive ("PORTMCU1", { 0x90, 0x68, 0x7f });
    desk.tickOnce();
    REQUIRE (desk.touches.isHeld (origin, trim));

    /*  UNPLUGGED MID-GESTURE. A surface that vanished cannot let go, and a
        touch left behind would keep the trim gated for the rest of the show
        (PRD §3.16) - so the bridge gives it back for it. */
    surface::PortState gone;
    gone.name = "Desk port";
    desk.declare ({ spec }, { { "PORTMCU1", gone } });

    desk.clear();
    desk.tickOnce();

    REQUIRE (desk.submitted.size() == 1u);
    CHECK (desk.submitted[0].command == "node.releaseAll");
    CHECK (desk.submitted[0].origin == origin);
    CHECK_FALSE (desk.touches.isHeld (origin, trim));
}

TEST_CASE ("surface bridge: the handshake's serial is recorded, and never answered")
{
    Desk desk;
    const auto mcu = desk.makeSurface ("mcu", "Desk");

    desk.declare ({ desk.spec (mcu, "mcu", { "PORTMCU1" }) }, { { "PORTMCU1", plugged ("Desk port") } });
    desk.tickOnce();
    CHECK (contains (sentOn (desk.sink, "PORTMCU1"), surface::deviceQuery (0x14)));

    //  F0 00 00 66 14 01 <serial> <challenge> F7: who it is, and a challenge nobody answers.
    desk.arrive ("PORTMCU1", { 0xf0, 0x00, 0x00, 0x66, 0x14, 0x01,
                               'D', '7', '0', '0', 'R', 'T', 'B',
                               0x01, 0x02, 0x03, 0x04, 0xf7 });
    desk.tickOnce();

    CHECK (desk.tableMoved);
    CHECK (desk.table.statusOf (mcu).serial == "D700RTB");
    CHECK (desk.published ("/godot/surface/" + mcu + "/serial") == "D700RTB");

    //  No reply: command 02 never leaves (plan decision 6).
    for (const auto& message : sysexOf (desk.sink))
        CHECK (message[5] != 0x02);
}

TEST_CASE ("surface bridge: a port carries one surface, the first that names it")
{
    Desk desk;
    const auto one = desk.makeSurface ("mcu", "One");
    const auto two = desk.makeSurface ("mcu", "Two");

    desk.declare ({ desk.spec (one, "mcu", { "PORTSHAR" }), desk.spec (two, "mcu", { "PORTSHAR" }) },
                  { { "PORTSHAR", plugged ("Shared") } });

    CHECK (desk.table.statusOf (one).connected);
    CHECK_FALSE (desk.table.statusOf (two).connected);
    CHECK (desk.table.statusOf (two).problem == "the port \"Shared\" already carries another surface");
}

TEST_CASE ("surface bridge: a surface switched off in the show owns no port, and neither does one nobody knows")
{
    Desk desk;
    const auto mcu = desk.makeSurface ("mcu", "Desk");

    auto off = desk.spec (mcu, "mcu", { "PORTMCU1" });
    off.enabled = false;

    auto odd = desk.spec (mcu, "hui", { "PORTHUI1" });
    odd.id = "SURFHUI1";

    desk.declare ({ off, odd }, { { "PORTMCU1", plugged ("Desk port") }, { "PORTHUI1", plugged ("Other") } });
    desk.ticks (2);

    CHECK_FALSE (desk.table.statusOf (mcu).connected);
    CHECK (desk.table.statusOf (mcu).problem == "it is switched off in the show");
    CHECK (desk.table.statusOf ("SURFHUI1").problem == "the profile \"hui\" is not one Go.dot can drive");

    //  Neither port is a surface's: what arrives on them is the triggers' to hear.
    CHECK_FALSE (desk.bridge.arrived ("PORTMCU1", { 0xe0, 0x00, 0x40 }));
    CHECK_FALSE (desk.bridge.arrived ("PORTHUI1", { 0xe0, 0x00, 0x40 }));

    desk.tickOnce();
    CHECK (desk.sink.sent.empty());
    CHECK (desk.submitted.empty());
}

TEST_CASE ("surface bridge: every SysEx that leaves is one the D700 is known to survive")
{
    Desk desk;
    const auto mcu = desk.makeSurface ("mcu", "Desk");
    const auto d700 = desk.makeSurface ("d700", "The D700");
    const auto accents = desk.makeDca ("Entrée à jardin");
    desk.pin (desk.strips[mcu][0], accents);
    desk.pin (desk.strips[d700][3], accents);
    desk.pin (desk.strips[d700][12], accents);

    const auto trim = "/godot/dca/" + accents + "/trim";

    desk.declare ({ desk.spec (mcu, "mcu", { "PORTMCU1" }),
                    desk.spec (d700, "d700", { "PORTBNK1", "PORTBNK2" }) },
                  { { "PORTMCU1", plugged ("Desk") },
                    { "PORTBNK1", plugged ("D700 bank 1") },
                    { "PORTBNK2", plugged ("D700 bank 2") } });

    desk.ticks (3);
    desk.write (trim, -30.0);
    desk.ticks (25);
    desk.write (trim, 6.5);
    desk.ticks (25);

    std::size_t sysex = 0;

    for (const auto& message : sysexOf (desk.sink))
    {
        ++sysex;
        CHECK (surface::isSafeSysEx (message));
    }

    CHECK (sysex > 0u);
    CHECK (desk.bridge.refusedSysEx() == 0u);

    //  The name folded to what a display can show.
    CHECK (contains (sentOn (desk.sink, "PORTMCU1"), surface::lcdCell (0x14, 0, 0, "Entree ")));
}

TEST_CASE ("surface bridge: the virtual panel is always connected, and the bridge neither drives nor hears it")
{
    Desk desk;
    const auto panel = desk.makeSurface ("virtual", "Panel");

    //  Even a port written on it by mistake is nothing to the bridge.
    desk.declare ({ desk.spec (panel, "virtual", { "PORTVIRT" }) }, { { "PORTVIRT", plugged ("Virtual") } });
    desk.ticks (3);

    CHECK (desk.table.statusOf (panel).connected);
    CHECK (desk.published ("/godot/surface/" + panel + "/connected") == "true");
    CHECK_FALSE (desk.bridge.arrived ("PORTVIRT", { 0xe0, 0x00, 0x40 }));
    CHECK_FALSE (desk.bridge.arrived ("ANYTHING", { 0x90, 0x5e, 0x7f }));

    desk.tickOnce();
    CHECK (desk.sink.sent.empty());
    CHECK (desk.submitted.empty());
}

//==============================================================================
TEST_CASE ("surface bridge: the standing test - two surfaces on different protocols bound to one node")
{
    /*  PRD §3.16's STANDING TEST, which the devplan has carried since it was
        written and namespace draft §16.8 draws: the virtual panel and a Mackie
        surface, each with a strip pinned to one DCA. The two protocols are the
        client's commands and Mackie's bytes, and both end as `node.set` on one
        address - which is the whole of the claim. What must hold is that each
        hears the other, that neither is fought by the other's echo, and that
        the touch table knows whose hand is on the node. */
    Desk desk;
    const auto panel = desk.makeSurface ("virtual", "Panel");
    const auto mcu = desk.makeSurface ("mcu", "Desk");
    const auto band = desk.makeDca ("Band");

    desk.pin (desk.strips[panel][0], band);
    desk.pin (desk.strips[mcu][0], band);

    desk.declare ({ desk.spec (panel, "virtual", {}), desk.spec (mcu, "mcu", { "PORTMCU1" }) },
                  { { "PORTMCU1", plugged ("Desk port") } });
    desk.ticks (3);

    const auto trim = "/godot/dca/" + band + "/trim";
    const auto surfaceOrigin = "surface:" + mcu;

    //  ONE NODE: both strips ride the same address, and both surfaces are up.
    CHECK (desk.published ("/godot/slot/" + desk.strips[panel][0] + "/target") == trim);
    CHECK (desk.published ("/godot/slot/" + desk.strips[mcu][0] + "/target") == trim);
    CHECK (desk.table.statusOf (panel).connected);
    CHECK (desk.table.statusOf (mcu).connected);

    const auto touch = [&desk] (const char* command, const std::string& address)
    {
        REQUIRE (desk.engine.submit ("window", command, { osc::Value::string (address) }));
    };

    /*  THE PANEL'S HAND: a touch, a drag to -20 dB, a release - what
        SurfacePanelComponent sends from the mouse. The Mackie's motor follows
        it, because the hand on the node is not this surface's. */
    desk.clear();
    touch ("node.touch", trim);
    desk.write (trim, -20.0);
    desk.tickOnce();

    CHECK (desk.touches.holdersOf (trim) == std::vector<std::string> { "window" });

    desk.ticks (10);

    auto moves = motorMoves (desk.sink, "PORTMCU1", 0);
    REQUIRE_FALSE (moves.empty());
    CHECK (moves.back() == surface::fourteenBitForDb (-20.0));

    touch ("node.release", trim);
    desk.tickOnce();
    CHECK (desk.touches.holdersOf (trim).empty());

    /*  THE MACKIE'S HAND: a finger on fader one, a move. The DCA follows the
        bytes, the panel reads the new value where it reads everything - the
        published node - and the motor under the finger is left alone. */
    desk.clear();
    desk.arrive ("PORTMCU1", { 0x90, 0x68, 0x7f });
    desk.arrive ("PORTMCU1", surface::faderPosition (0, 12000));
    desk.tickOnce();

    const auto handDb = surface::dbForFourteenBit (12000);

    CHECK (near (desk.dcas.trimOf (band), handDb));
    CHECK (desk.touches.holdersOf (trim) == std::vector<std::string> { surfaceOrigin });
    CHECK_FALSE (desk.touches.isHeld ("window", trim));

    desk.tickOnce();

    const auto* seen = desk.snapshot->find (trim);
    REQUIRE (seen != nullptr);
    REQUIRE_FALSE (seen->values.empty());
    CHECK (near (seen->values.front().asDouble(), handDb));

    desk.ticks (5);
    CHECK (motorMoves (desk.sink, "PORTMCU1", 0).empty());

    /*  LET GO: exactly one resend, where the hand left it - the surface
        amended in §3.16 re-asserts the node's value on release - and nothing
        after it. */
    desk.arrive ("PORTMCU1", { 0x90, 0x68, 0x00 });
    desk.tickOnce();
    CHECK (motorMoves (desk.sink, "PORTMCU1", 0) == std::vector<int> { 12000 });

    desk.ticks (5);
    CHECK (motorMoves (desk.sink, "PORTMCU1", 0) == std::vector<int> { 12000 });
    CHECK (desk.touches.holdersOf (trim).empty());

    //  AND THE PANEL IS NEVER SENT A BYTE: it has no port, and needs none.
    for (const auto& message : desk.sink.sent)
        CHECK (message.port == "PORTMCU1");
}

//==============================================================================
TEST_CASE ("m29: what a full refresh of a sixteen-strip D700 costs the tick thread" * doctest::skip())
{
    /*  M29 - AN INSTRUMENT, NOT A GATE (namespace draft §16.9). Skipped in
        every ordinary run, and taken with

            wfg_tests --test-case="m29*" --no-skip

        on a Release build of a machine nobody else is using: a Debug figure is
        a figure about the Debug build. It prints what the bridge's after-tick
        costs the tick thread for a D700 with all sixteen strips filled, in the
        three shapes a tick comes in - nothing changed, every fader riding, every
        strip's name changing - against the twenty milliseconds a tick has.
        What it cannot see is the sending: the bytes are queued for the MIDI
        sender's worker, and what that thread spends is not the tick's. */
    Stage stage { "d700", 16 };
    stage.audio.tracks = 16;

    for (std::size_t n = 0; n < stage.members.size(); ++n)
        stage.set ("/godot/cue/" + stage.members[n] + "/colour", n % 2 == 0 ? "#FF8000" : "#2080FF");

    stage.arm();

    surface::SurfaceSpec spec;
    spec.id = stage.surfaceId;
    spec.profile = "d700";
    spec.ports = { "PORTBNK1", "PORTBNK2" };
    spec.strips = stage.strips;
    stage.bridge.declare ({ spec }, [] (const std::string& port) { return plugged (port); });

    /*  A WARM-UP THE FIGURES LEAVE OUT: the first ticks after a declaration
        paint everything and fill every cache, which is not what a show pays
        on every tick after it. */
    stage.ticks (200);

    const auto measure = [&stage] (const char* label, int count, const std::function<void (int)>& change)
    {
        std::vector<double> micros;
        std::vector<double> publishing;
        micros.reserve (static_cast<std::size_t> (count));
        publishing.reserve (static_cast<std::size_t> (count));
        std::size_t bytes = 0;

        for (int n = 0; n < count; ++n)
        {
            change (n);

            //  A tick as serve runs it, with the bridge's after-tick timed alone.
            stage.bridge.beforeTick (stage.submit, stage.tick);
            stage.runner.beforeTick (stage.engine, stage.tick);
            stage.engine.processTick (stage.tick);
            stage.parameters.markStale();

            /*  THE PUBLISH IT SITS BESIDE, timed the same way: the question is
                whether the bridge fits next to it, so the answer is a ratio. */
            const auto publishStarted = std::chrono::steady_clock::now();
            stage.snapshot = stage.parameters.publish (stage.tick, stage.state);
            publishing.push_back (std::chrono::duration<double, std::micro> (
                                      std::chrono::steady_clock::now() - publishStarted).count());

            stage.sink.sent.clear();

            const auto started = std::chrono::steady_clock::now();
            stage.bridge.afterTick (stage.snapshot, stage.touches, stage.tick);
            const auto took = std::chrono::steady_clock::now() - started;

            micros.push_back (std::chrono::duration<double, std::micro> (took).count());

            for (const auto& message : stage.sink.sent)
                bytes += message.bytes.size();

            ++stage.tick;
        }

        std::sort (micros.begin(), micros.end());
        std::sort (publishing.begin(), publishing.end());

        MESSAGE (std::string (label) << ": median " << micros[micros.size() / 2] << " us, p99 "
                       << micros[micros.size() * 99 / 100] << " us, worst " << micros.back()
                       << " us; " << bytes / static_cast<std::size_t> (count) << " bytes a tick"
                       << " - beside a publish of " << publishing[publishing.size() / 2] << " us");
    };

    measure ("idle, nothing changed", 500, [] (int) {});

    /*  EVERY FADER RIDING, below the start threshold so no clip launches and
        every tick has sixteen motors, sixteen level rows and sixteen rings to
        move. */
    measure ("every fader riding", 500, [&stage] (int n)
    {
        for (const auto& member : stage.members)
            if (const auto* run = stage.liveRunOf (member))
                REQUIRE (stage.engine.submit ("window", "node.set",
                                              { osc::Value::string ("/godot/run/" + run->id + "/trim"),
                                                osc::Value::float64 (-120.0 + static_cast<double> (n % 9)) }));
    });

    //  EVERY NAME CHANGING, which is every display row written again.
    measure ("every name changing", 200, [&stage] (int n)
    {
        for (std::size_t m = 0; m < stage.members.size(); ++m)
            stage.set ("/godot/cue/" + stage.members[m] + "/name",
                       "Clip " + std::to_string (m) + "." + std::to_string (n));
    });

    CHECK (stage.bridge.refusedSysEx() == 0u);
}
