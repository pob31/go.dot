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
    THE PROXY TRANSPORT (Phase 9a, PR 9a.6, §17.6): the region, a lane's spin
    and passthrough with no child at all, the test child answering through a
    real region and a real second process, a child killed mid-flight and the
    entry marked failed within a poll, the restart, the commands, and the
    whole thing inside a Tracktion graph with three plugins on sixteen voices.

    THE CHILD IS THIS BINARY. TestMain dispatches `plugin-host` before doctest
    sees the command line, so the launch names the test executable and no
    other build product has to exist - the CI job that runs this has wfg_tests
    and nothing else in hand.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/audio/AudioHost.h>
#include <wfg/engine/plugin/Catalogue.h>
#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/plugin/PluginCommands.h>
#include <wfg/engine/plugin/PluginTable.h>
#include <wfg/engine/plugin/PluginScan.h>
#include <wfg/engine/plugin/EditorHost.h>
#include <wfg/engine/cue/InsertChain.h>
#include <wfg/engine/plugin/LaneMapping.h>
#include <wfg/engine/plugin/PluginLoad.h>
#include <wfg/engine/plugin/ProxyHost.h>
#include <wfg/engine/plugin/ProxyLane.h>
#include <wfg/engine/plugin/SharedRegion.h>
#include <wfg/engine/rt/RtCheck.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace wfg;

namespace
{
    struct Folder
    {
        Folder()
            : path (juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("wfg-proxy-test-" + juce::Uuid().toDashedString()))
        {
            path.createDirectory();
        }

        ~Folder() { path.deleteRecursively(); }

        std::string string() const { return path.getFullPathName().toStdString(); }

        juce::File path;
    };

    /** A region in a vector: what a lane talks to when no child exists. */
    struct VectorRegion
    {
        VectorRegion (int channels, int maxSamples, int lanes)
            : bytes (plugin::region::regionBytes (channels, maxSamples, lanes), 0),
              channelCount (channels), sampleCount (maxSamples)
        {
            header = new (bytes.data()) plugin::region::Header {};

            for (int i = 0; i < lanes; ++i)
                new (plugin::region::laneAt (bytes.data(), channels, maxSamples, i)) plugin::region::Lane {};
        }

        plugin::region::Lane* lane (int index)
        {
            return plugin::region::laneAt (bytes.data(), channelCount, sampleCount, index);
        }

        void bind (plugin::ProxyLane& proxy, int index)
        {
            auto* l = lane (index);
            proxy.bind (header, l, plugin::region::audioOf (l), channelCount, sampleCount);
        }

        std::vector<char> bytes;
        int channelCount, sampleCount;
        plugin::region::Header* header = nullptr;
    };

    /** The test executable, as `wfg plugin-host` would be the console. */
    plugin::ProxyLaunch launchOfThisBinary()
    {
        plugin::ProxyLaunch launch;
        launch.executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName().toStdString();
        launch.leadingArgs = { "plugin-host" };
        return launch;
    }

    plugin::ProxySpec testGainSpec (const Folder& folder, int lanes, int channels = 2, int block = 64)
    {
        plugin::ProxySpec spec;
        spec.pluginId = "PG7N0001";
        spec.identifier = plugin::Catalogue::testGainIdentifier();
        spec.name = "Test gain";
        spec.lanes = lanes;
        spec.channels = channels;
        spec.maxSamples = block;
        spec.sampleRate = 48000;
        spec.regionFolder = folder.string();
        spec.launch = launchOfThisBinary();
        return spec;
    }

    /** Polls the host until its state reads `word`, or the time is up. */
    bool waitForState (plugin::ProxyHost& host, const char* word, int milliseconds)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds (milliseconds);

        for (;;)
        {
            host.poll();

            if (host.status().state == word)
                return true;

            if (std::chrono::steady_clock::now() > until)
                return false;

            std::this_thread::sleep_for (std::chrono::milliseconds (10));
        }
    }

    struct Block
    {
        Block (int channels, int samples, float value)
            : storage (static_cast<std::size_t> (channels * samples), value), width (samples)
        {
            for (int c = 0; c < channels; ++c)
                pointers.push_back (storage.data() + c * samples);
        }

        float* const* data() { return pointers.data(); }

        bool allEqual (float value) const
        {
            for (const auto sample : storage)
                if (std::fabs (sample - value) > 1.0e-6f)
                    return false;

            return true;
        }

        std::vector<float> storage;
        std::vector<float*> pointers;
        int width;
    };
}

//==============================================================================
TEST_CASE ("proxy: the region's layout is what both sides compute, and every lane is on its own cache line")
{
    using namespace plugin::region;

    CHECK (headerBytes() % 64 == 0);
    CHECK (laneStride (2, 64) % 64 == 0);
    CHECK (laneStride (2, 64) >= sizeof (Lane) + 2 * 64 * sizeof (float));
    CHECK (regionBytes (2, 64, 3) == headerBytes() + 3 * laneStride (2, 64));

    /*  The hash moves with anything the layout depends on, and with nothing
        else - the child refuses a region from another revision by it. */
    CHECK (layoutHashFor (2, 64, 3) == layoutHashFor (2, 64, 3));
    CHECK (layoutHashFor (2, 64, 3) != layoutHashFor (2, 64, 4));
    CHECK (layoutHashFor (2, 64, 3) != layoutHashFor (4, 64, 3));
    CHECK (layoutHashFor (2, 64, 3) != layoutHashFor (2, 128, 3));

    VectorRegion region (2, 64, 3);
    auto* header = region.header;
    header->magic.store (magic);
    header->version.store (version);
    header->channels.store (2);
    header->maxSamples.store (64);
    header->lanes.store (3);
    header->layoutHash.store (layoutHashFor (2, 64, 3));
    CHECK (looksValid (*header));

    header->version.store (version + 1);
    CHECK_FALSE (looksValid (*header));

    /*  The lanes sit where the arithmetic says, and their audio follows. */
    CHECK (reinterpret_cast<char*> (region.lane (1)) - reinterpret_cast<char*> (region.lane (0)) == static_cast<std::ptrdiff_t> (laneStride (2, 64)));
    CHECK (reinterpret_cast<char*> (audioOf (region.lane (0))) - reinterpret_cast<char*> (region.lane (0)) == static_cast<std::ptrdiff_t> (sizeof (Lane)));
}

TEST_CASE ("proxy: a lane that is off, unbound or not to be called leaves the block untouched and signals nothing")
{
    plugin::ProxyLane lane;
    Block block (2, 64, 0.25f);

    /*  Unbound: nothing to talk to. */
    lane.setEnabled (true);
    lane.process (block.data(), 2, 64);
    CHECK (block.allEqual (0.25f));
    CHECK (lane.blocks() == 0);

    VectorRegion region (2, 64, 1);
    region.bind (lane, 0);
    CHECK (lane.isBound());

    /*  Off: bound, but the cue does not switch it in. */
    lane.setEnabled (false);
    lane.process (block.data(), 2, 64);
    CHECK (block.allEqual (0.25f));
    CHECK (lane.blocks() == 0);
    CHECK (region.lane (0)->requestSeq.load() == 0);

    /*  Failed: the host cleared the call. */
    lane.setEnabled (true);
    lane.setCallEnabled (false);
    lane.process (block.data(), 2, 64);
    CHECK (block.allEqual (0.25f));
    CHECK (lane.blocks() == 0);
    CHECK (region.lane (0)->requestSeq.load() == 0);
}

TEST_CASE ("proxy: with nobody answering, a lane misses at its deadline, passes the block through, and counts")
{
    plugin::ProxyLane lane;
    VectorRegion region (2, 64, 1);
    region.bind (lane, 0);
    lane.setEnabled (true);
    lane.setDeadlineMicroseconds (200);

    Block block (2, 64, 0.5f);

    rt::resetCounts();

    const auto started = std::chrono::steady_clock::now();

    for (int i = 0; i < 10; ++i)
    {
        const rt::ScopedRealtimeCheck ours { rt::Region::ours };
        lane.process (block.data(), 2, 64);
    }

    const auto took = std::chrono::steady_clock::now() - started;

    CHECK (block.allEqual (0.5f));
    CHECK (lane.blocks() == 10);
    CHECK (lane.misses() == 10);
    CHECK (lane.consecutiveMisses() == 10);
    CHECK (lane.answered() == 0);
    CHECK (region.lane (0)->requestSeq.load() == 10);

    /*  Ten deadlines of 200 µs is two milliseconds; two seconds is the bound,
        because a thread preempted on a shared box is late without the spin
        being wrong, and what the bound guards is a spin that never ends. */
    CHECK (std::chrono::duration_cast<std::chrono::milliseconds> (took).count() < 2000);

    if (rt::isCounting())
        CHECK (rt::violations() == 0);

    /*  An answer that arrives resets the run of misses. */
    region.lane (0)->responseSeq.store (region.lane (0)->requestSeq.load());
    lane.clearMisses();
    CHECK (lane.consecutiveMisses() == 0);
}

TEST_CASE ("proxy: a value written before the child is up is in the region once the lane is bound")
{
    plugin::ProxyLane lane;
    lane.setParameter (0, 0.75f);
    lane.setParameter (3, 2.0f);     // clamped to one
    lane.setParameter (5, -3.0f);    // "the baseline"
    CHECK (lane.parameter (0) == doctest::Approx (0.75f));
    CHECK (lane.parameter (3) == doctest::Approx (1.0f));
    CHECK (lane.parameter (5) == doctest::Approx (plugin::region::useBaseline));
    CHECK (lane.parameter (plugin::region::maxParams) == doctest::Approx (plugin::region::useBaseline));

    VectorRegion region (2, 64, 1);
    region.bind (lane, 0);

    auto* l = region.lane (0);
    CHECK (l->params[0].load() == doctest::Approx (0.75f));
    CHECK (l->params[3].load() == doctest::Approx (1.0f));
    CHECK (l->params[5].load() == doctest::Approx (plugin::region::useBaseline));
    CHECK (l->paramRevision.load() >= 1);

    const auto revision = l->paramRevision.load();
    lane.setParameter (1, 0.5f);
    CHECK (l->params[1].load() == doctest::Approx (0.5f));
    CHECK (l->paramRevision.load() == revision + 1);

    lane.requestReset();
    CHECK (l->resetSeq.load() == 1);
}

TEST_CASE ("proxy: the deadline rule is the smaller of 250 microseconds and a quarter of the block")
{
    CHECK (plugin::proxyDeadlineFor (48000, 64, 0) == 250);        // 1333 µs block, a quarter is 333
    CHECK (plugin::proxyDeadlineFor (48000, 32, 0) == 166);        // 666 µs block, a quarter is 166
    CHECK (plugin::proxyDeadlineFor (96000, 16, 0) == 41);
    CHECK (plugin::proxyDeadlineFor (48000, 64, 900) == 900);      // asked for
    CHECK (plugin::proxyDeadlineFor (0, 0, 0) == plugin::ProxyLane::defaultDeadlineMicroseconds);
}

//==============================================================================
TEST_CASE ("proxy: the test child comes up, halves an enabled lane's block, leaves a disabled one alone, and follows a value")
{
    Folder folder;
    plugin::PluginTable table;
    plugin::ProxyLane lanes[2];
    plugin::ProxyHost host (testGainSpec (folder, 2), { &lanes[0], &lanes[1] }, &table);

    int changes = 0;
    host.onChanged ([&changes] { ++changes; });

    std::string problem;
    INFO ("start: " << problem);
    REQUIRE (host.start (problem));
    CHECK (host.status().state == "loading");
    CHECK (juce::File (host.regionPath()).existsAsFile());

    REQUIRE (waitForState (host, "loaded", 5000));
    CHECK (host.childIsRunning());
    CHECK (changes >= 2);
    CHECK (host.status().paramCount == 2);
    CHECK (host.status().latencySamples == 0);
    CHECK (table.statusOf ("PG7N0001").state == "loaded");
    CHECK (host.header()->baseline[0].load() == doctest::Approx (0.5f));

    /*  A generous deadline, because the box a test runs on is shared and a
        preempted child answers late without being wrong; what a round trip
        costs is M31's question, asked on a quiet machine. */
    lanes[0].setDeadlineMicroseconds (200000);
    lanes[1].setDeadlineMicroseconds (200000);
    lanes[0].setEnabled (true);

    /*  The child sleeps between polls while no lane is in; the host's poll
        tells it to spin once one is. */
    host.poll();

    Block first (2, 64, 0.8f);
    Block second (2, 64, 0.8f);

    /*  The first block may wake the worker from its millisecond poll; the
        deadline above covers it. Warm up rather than measure. */
    for (int i = 0; i < 4; ++i)
    {
        std::fill (first.storage.begin(), first.storage.end(), 0.8f);
        lanes[0].process (first.data(), 2, 64);
    }

    CHECK (first.allEqual (0.4f));
    CHECK (lanes[0].answered() >= 1);

    lanes[1].process (second.data(), 2, 64);
    CHECK (second.allEqual (0.8f));
    CHECK (lanes[1].blocks() == 0);

    /*  p0 at one: unity from the next block. */
    lanes[0].setParameter (0, 1.0f);
    std::fill (first.storage.begin(), first.storage.end(), 0.8f);
    lanes[0].process (first.data(), 2, 64);
    CHECK (first.allEqual (0.8f));

    /*  And back to the baseline when the value is withdrawn. */
    lanes[0].setParameter (0, plugin::region::useBaseline);
    std::fill (first.storage.begin(), first.storage.end(), 0.8f);
    lanes[0].process (first.data(), 2, 64);
    CHECK (first.allEqual (0.4f));

    CHECK (lanes[0].misses() == 0);

    host.stop();
    CHECK_FALSE (host.childIsRunning());
    CHECK_FALSE (juce::File (host.regionPath()).existsAsFile());
    CHECK_FALSE (lanes[0].isBound());
    CHECK (table.statusOf ("PG7N0001").state == "unloaded");
}

//==============================================================================
TEST_CASE ("chain: how wide a cue is after its inserts, and why one passes it dry")
{
    using cue::InsertShape;

    const InsertShape stereo { true, 2, 2, 64 };
    const InsertShape mono { true, 1, 1, 0 };
    const InsertShape widen { true, 1, 2, 0 };
    const InsertShape narrow { true, 2, 1, 0 };
    const InsertShape eight { true, 8, 8, 0 };

    //  No insert: the file's width.
    CHECK (cue::chainOf (1, 2, {}, {}).channels == 1);

    //  A mono cue through a stereo plugin comes out stereo; the latency is counted.
    auto chain = cue::chainOf (1, 2, { true }, { stereo });
    CHECK (chain.channels == 2);
    CHECK (chain.steps[0].feed == 1);
    CHECK (chain.steps[0].back == 2);
    CHECK (chain.latencySamples == 64);

    //  Through a widener too.
    CHECK (cue::chainOf (1, 2, { true }, { widen }).channels == 2);

    //  Switched out changes nothing.
    chain = cue::chainOf (1, 2, { false }, { stereo });
    CHECK (chain.channels == 1);
    CHECK (chain.steps[0].feed == 0);
    CHECK (chain.latencySamples == 0);

    //  A stereo cue on a mono plugin, or one that would come out narrower: dry, and said.
    chain = cue::chainOf (2, 2, { true }, { mono });
    CHECK (chain.channels == 2);
    CHECK (chain.steps[0].feed == 0);
    CHECK_FALSE (chain.steps[0].dryWhy.empty());
    CHECK (cue::chainOf (2, 2, { true }, { narrow }).steps[0].feed == 0);

    //  A stereo cue in the first two inputs of a plugin as wide as an eight-channel voice stays stereo;
    //  a mono cue fed into all eight comes out eight.
    CHECK (cue::chainOf (2, 8, { true }, { eight }).channels == 2);
    CHECK (cue::chainOf (1, 8, { true }, { eight }).channels == 8);

    //  Never wider than the voice: a widener on a mono voice leaves it mono.
    CHECK (cue::chainOf (1, 1, { true }, { widen }).channels == 1);

    //  A plugin that has not said what it takes is counted as taking the cue at its width.
    chain = cue::chainOf (1, 2, { true }, { InsertShape {} });
    CHECK (chain.channels == 1);
    CHECK (chain.steps[0].feed == 1);

    //  In the set's order: mono through a widener, then the stereo cue through a stereo plugin.
    chain = cue::chainOf (1, 2, { true, true }, { widen, stereo });
    CHECK (chain.channels == 2);
    CHECK (chain.steps[1].feed == 2);
}

TEST_CASE ("proxy: a lane sends the cue's width, takes the widened sides back, and widens a dry block it could not send")
{
    plugin::ProxyLane lane;
    VectorRegion region (2, 64, 1);
    region.bind (lane, 0);
    lane.setEnabled (true);
    lane.setDeadlineMicroseconds (200);

    //  A mono cue into a widening insert: one channel sent, two taken back.
    lane.setShape (1, 2);

    SUBCASE ("with nobody answering, the dry block is the mono side on both")
    {
        Block block (2, 64, 0.0f);
        std::fill (block.storage.begin(), block.storage.begin() + 64, 0.5f);
        lane.process (block.data(), 2, 64);
        CHECK (region.lane (0)->numChannels.load() == 1u);
        CHECK (block.allEqual (0.5f));
    }

    SUBCASE ("switched out of the chain for this cue, the block is left whole")
    {
        lane.setShape (0, 0);
        Block block (2, 64, 0.0f);
        std::fill (block.storage.begin(), block.storage.begin() + 64, 0.5f);
        lane.process (block.data(), 2, 64);
        CHECK (lane.blocks() == 0);
        CHECK (block.storage[64] == doctest::Approx (0.0f));
    }

    SUBCASE ("a block longer than the region is sent in pieces")
    {
        lane.setShape (-1, -1);
        Block block (2, 150, 0.25f);
        lane.process (block.data(), 2, 150);
        CHECK (lane.blocks() == 1);   // late on the first piece: the rest stays dry
        CHECK (lane.misses() == 1);
    }
}

TEST_CASE ("lanes: a voice's channels meet a plugin's by the rules - mono into every input, dry when it will not fit, folded when it gives more")
{
    using plugin::lanemap::Shape;

    //  Who takes what.
    CHECK (plugin::lanemap::takes (Shape { 2, 2, 2, 2 }));
    CHECK (plugin::lanemap::takes (Shape { 1, 2, 2, 2 }));     // a mono cue into a stereo plugin
    CHECK (plugin::lanemap::takes (Shape { 1, 1, 2, 2 }));     // into a widener
    CHECK_FALSE (plugin::lanemap::takes (Shape { 2, 1, 1, 2 }));   // a stereo cue, a mono plugin: dry
    CHECK_FALSE (plugin::lanemap::takes (Shape { 6, 2, 2, 8 }));   // wider than the plugin's inputs
    CHECK_FALSE (plugin::lanemap::takes (Shape { 2, 2, 1, 2 }));   // it would come out narrower
    CHECK_FALSE (plugin::lanemap::takes (Shape { 0, 2, 2, 2 }));

    constexpr int n = 4;
    float laneL[n] = { 1, 2, 3, 4 }, laneR[n] = { 10, 20, 30, 40 };
    float* lane[2] = { laneL, laneR };
    float bufA[n] = {}, bufB[n] = {};
    float* buffer[2] = { bufA, bufB };

    SUBCASE ("a mono feed goes into both inputs of a stereo plugin")
    {
        const Shape shape { 1, 2, 2, 2 };
        plugin::lanemap::feedInto (lane, shape, buffer, n);
        CHECK (bufA[2] == doctest::Approx (3.0f));
        CHECK (bufB[2] == doctest::Approx (3.0f));   // the left, not the lane's right
    }

    SUBCASE ("a stereo feed goes one to one")
    {
        const Shape shape { 2, 2, 2, 2 };
        plugin::lanemap::feedInto (lane, shape, buffer, n);
        CHECK (bufA[1] == doctest::Approx (2.0f));
        CHECK (bufB[1] == doctest::Approx (20.0f));
    }

    SUBCASE ("outputs as wide as the lane come back channel to channel")
    {
        bufA[0] = 0.5f;
        bufB[0] = 0.25f;
        plugin::lanemap::backInto (buffer, Shape { 1, 1, 2, 2 }, lane, n);
        CHECK (laneL[0] == doctest::Approx (0.5f));
        CHECK (laneR[0] == doctest::Approx (0.25f));
    }

    SUBCASE ("two outputs onto a one-channel lane are summed at a half each")
    {
        bufA[0] = 0.5f;
        bufB[0] = 0.25f;
        float* mono[1] = { laneL };
        plugin::lanemap::backInto (buffer, Shape { 1, 1, 2, 1 }, mono, n);
        CHECK (laneL[0] == doctest::Approx (0.375f));
    }
}

namespace
{
    /*  A PLUGIN THAT TAKES WHAT IT IS TOLD TO: its buses, and which layouts
        it agrees to, are the test's. What the ladder is asked against. It
        starts in a layout it accepts, as every real plugin does - JUCE takes
        a layout equal to the one a plugin has without asking it. */
    struct PickyProcessor final : juce::AudioProcessor
    {
        PickyProcessor (bool sidechain, std::function<bool (const BusesLayout&)> acceptsToUse,
                        juce::AudioChannelSet in = juce::AudioChannelSet::stereo(),
                        juce::AudioChannelSet out = juce::AudioChannelSet::stereo())
            : juce::AudioProcessor (sidechain
                                      ? BusesProperties().withInput ("In", in)
                                                         .withInput ("Sidechain", juce::AudioChannelSet::stereo())
                                                         .withOutput ("Out", out)
                                      : BusesProperties().withInput ("In", in).withOutput ("Out", out)),
              accepts (std::move (acceptsToUse))
        {
        }

        bool isBusesLayoutSupported (const BusesLayout& layout) const override { return accepts (layout); }

        const juce::String getName() const override { return "picky"; }
        void prepareToPlay (double, int) override {}
        void releaseResources() override {}
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }
        bool hasEditor() const override { return false; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram (int) override {}
        const juce::String getProgramName (int) override { return {}; }
        void changeProgramName (int, const juce::String&) override {}
        void getStateInformation (juce::MemoryBlock&) override {}
        void setStateInformation (const void*, int) override {}

        std::function<bool (const BusesLayout&)> accepts;
    };
}

TEST_CASE ("layout: the ladder asks the voice's width, then stereo, then mono in and stereo out, then mono - never every bus on")
{
    using Set = juce::AudioChannelSet;
    plugin::InsertLayout layout;
    std::string problem;

    SUBCASE ("a plugin that takes anything takes the voice's width")
    {
        PickyProcessor any (false, [] (const auto& l) { return l.getMainInputChannels() == l.getMainOutputChannels(); });
        REQUIRE (plugin::chooseLayout (any, 2, layout, problem));
        CHECK (layout.inputs == 2);
        CHECK (layout.outputs == 2);
        CHECK (layout.words == "stereo in, stereo out");
    }

    SUBCASE ("a mono-only plugin on a stereo voice is taken mono, and says so")
    {
        PickyProcessor mono (false, [] (const auto& l) { return l.getMainInputChannelSet() == Set::mono()
                                                                && l.getMainOutputChannelSet() == Set::mono(); },
                             Set::mono(), Set::mono());
        REQUIRE (plugin::chooseLayout (mono, 2, layout, problem));
        CHECK (layout.inputs == 1);
        CHECK (layout.outputs == 1);
        CHECK (layout.words == "mono in, mono out");
    }

    SUBCASE ("a widener is taken one in, two out")
    {
        PickyProcessor widen (false, [] (const auto& l) { return l.getMainInputChannelSet() == Set::mono()
                                                                 && l.getMainOutputChannelSet() == Set::stereo(); },
                              Set::mono(), Set::stereo());
        REQUIRE (plugin::chooseLayout (widen, 2, layout, problem));
        CHECK (layout.inputs == 1);
        CHECK (layout.outputs == 2);
    }

    SUBCASE ("a sidechain it will not give up is left on, and the main buses are still the voice's")
    {
        PickyProcessor keyed (true, [] (const auto& l) { return l.inputBuses.size() == 2 && ! l.inputBuses[1].isDisabled()
                                                                && l.getMainInputChannelSet() == Set::stereo()
                                                                && l.getMainOutputChannelSet() == Set::stereo(); });
        REQUIRE (plugin::chooseLayout (keyed, 2, layout, problem));
        CHECK (layout.inputs == 2);
        CHECK (keyed.getBus (true, 1)->isEnabled());
    }

    SUBCASE ("a sidechain it lets go of is switched off")
    {
        PickyProcessor keyed (true, [] (const auto& l) { return l.getMainInputChannelSet() == Set::stereo()
                                                                && l.getMainOutputChannelSet() == Set::stereo(); });
        REQUIRE (plugin::chooseLayout (keyed, 2, layout, problem));
        CHECK_FALSE (keyed.getBus (true, 1)->isEnabled());
    }

    SUBCASE ("one that takes nothing it is offered is refused, with the sentence the entry reads")
    {
        PickyProcessor none (false, [] (const auto& l) { return l.getMainInputChannels() == 6; },
                             Set::create5point1(), Set::create5point1());
        CHECK_FALSE (plugin::chooseLayout (none, 2, layout, problem));
        CHECK (problem.find ("stereo nor mono") != std::string::npos);
    }
}

TEST_CASE ("proxy: the mono and widening test children say their buses, take a mono cue, and pass a stereo one dry")
{
    SUBCASE ("the widener on a mono voice: its two sides folded back into the one")
    {
        Folder folder;
        plugin::PluginTable table;
        plugin::ProxyLane lanes[1];

        auto spec = testGainSpec (folder, 1, 1, 64);
        spec.identifier = plugin::Catalogue::testWidenIdentifier();
        plugin::ProxyHost host (spec, { &lanes[0] }, &table);

        std::string problem;
        REQUIRE (host.start (problem));
        REQUIRE (waitForState (host, "loaded", 5000));
        CHECK (host.status().inputs == 1);
        CHECK (host.status().outputs == 2);
        CHECK (host.status().layout == "mono in, stereo out");

        lanes[0].setDeadlineMicroseconds (200000);
        lanes[0].setEnabled (true);
        host.poll();

        Block block (1, 64, 0.25f);

        for (int i = 0; i < 4; ++i)
        {
            std::fill (block.storage.begin(), block.storage.end(), 0.25f);
            lanes[0].process (block.data(), 1, 64);
        }

        //  Left x.g, right x.g/2, summed at a half each: 0.25 x 0.5 x 0.75.
        CHECK (block.allEqual (0.09375f));
        host.stop();
    }

    SUBCASE ("the mono plugin on a stereo voice passes a stereo cue dry, whole")
    {
        Folder folder;
        plugin::PluginTable table;
        plugin::ProxyLane lanes[1];

        auto spec = testGainSpec (folder, 1, 2, 64);
        spec.identifier = plugin::Catalogue::testMonoIdentifier();
        plugin::ProxyHost host (spec, { &lanes[0] }, &table);

        std::string problem;
        REQUIRE (host.start (problem));
        REQUIRE (waitForState (host, "loaded", 5000));
        CHECK (host.status().layout == "mono in, mono out");

        lanes[0].setDeadlineMicroseconds (200000);
        lanes[0].setEnabled (true);
        host.poll();

        Block block (2, 64, 0.25f);

        for (int i = 0; i < 4; ++i)
        {
            std::fill (block.storage.begin(), block.storage.end(), 0.25f);
            lanes[0].process (block.data(), 2, 64);
        }

        CHECK (block.allEqual (0.25f));
        CHECK (lanes[0].answered() >= 1);
        host.stop();
    }

    SUBCASE ("the gain as wide as the voice, as ever")
    {
        Folder folder;
        plugin::PluginTable table;
        plugin::ProxyLane lanes[1];
        plugin::ProxyHost host (testGainSpec (folder, 1), { &lanes[0] }, &table);

        std::string problem;
        REQUIRE (host.start (problem));
        REQUIRE (waitForState (host, "loaded", 5000));
        CHECK (host.status().inputs == 2);
        CHECK (host.status().outputs == 2);
        CHECK (host.status().layout == "stereo in, stereo out");
        host.stop();
    }
}

TEST_CASE ("proxy: a cue's whole state is loaded onto its voice before it may launch, the lane answering dry meanwhile")
{
    /*  The author's decision of 2026-09-25: a plugin's whole state kept per
        cue and loaded onto the voice at the arm. The test gain's Pad - no
        parameter, only state - is what makes it audible: a quarter of the
        gain. What is pinned: the arm waits (`stateSettled`) until the child
        has loaded it; the lane answers DRY while it loads and misses nothing;
        the other voice plays on; the cue's values sit on top; the same state
        twice loads nothing; no state after one is the preset's again; a file
        that is not there is the preset with a sentence. */
    Folder folder;
    plugin::PluginTable table;
    plugin::ProxyLane lanes[2];
    plugin::ProxyHost host (testGainSpec (folder, 2), { &lanes[0], &lanes[1] }, &table);

    std::string problem;
    INFO ("start: " << problem);
    REQUIRE (host.start (problem));
    REQUIRE (waitForState (host, "loaded", 5000));

    for (auto& lane : lanes)
    {
        lane.setDeadlineMicroseconds (200000);
        lane.setEnabled (true);
    }

    host.poll();

    const auto stateFile = [&folder] (const char* name, const char* text)
    {
        const auto file = folder.path.getChildFile (name);
        REQUIRE (file.replaceWithText (text));
        return file.getFullPathName().toStdString();
    };

    const auto settled = [&host] (plugin::ProxyLane& lane, int milliseconds)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds (milliseconds);

        while (std::chrono::steady_clock::now() < until)
        {
            host.poll();

            if (lane.stateSettled())
                return true;

            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        }

        return lane.stateSettled();
    };

    //  What a block of 0.8 comes back as, a few blocks in so any warm-up is past.
    const auto play = [] (plugin::ProxyLane& lane)
    {
        Block block (2, 64, 0.8f);

        for (int i = 0; i < 3; ++i)
        {
            std::fill (block.storage.begin(), block.storage.end(), 0.8f);
            lane.process (block.data(), 2, 64);
        }

        return block.storage.front();
    };

    const auto padded = stateFile ("padded.state", "gain=0.5\ndie=0\npad=1\n");

    SUBCASE ("Pad in the state: the voice is a quarter as loud, the other voice is not, and the table says so")
    {
        lanes[0].wantState (padded);
        CHECK_FALSE (lanes[0].stateSettled());
        REQUIRE (settled (lanes[0], 3000));

        CHECK (play (lanes[0]) == doctest::Approx (0.1f));      // 0.8 x 0.5 x 0.25
        CHECK (play (lanes[1]) == doctest::Approx (0.4f));
        CHECK (table.statusOf ("PG7N0001").stateProblem.empty());

        SUBCASE ("the same state again loads nothing, and waits for nothing")
        {
            lanes[0].wantState (padded);
            host.poll();
            CHECK (lanes[0].stateSettled());
        }

        SUBCASE ("no state after one is the preset's again - or the last cue's would be heard")
        {
            lanes[0].wantState ({});
            REQUIRE (settled (lanes[0], 3000));
            CHECK (play (lanes[0]) == doctest::Approx (0.4f));
        }

        SUBCASE ("the cue's own values sit on top of its state")
        {
            lanes[0].setParameter (0, 1.0f);
            CHECK (play (lanes[0]) == doctest::Approx (0.2f));  // 0.8 x 1 x 0.25
        }
    }

    SUBCASE ("a slow state: the voice waits, answers dry and misses nothing, and the other voice plays on")
    {
        /*  A LOAD LONG ENOUGH TO CATCH IN THE ACT on a loaded runner: the
            child's loader picks the request up on its own timer, so the lane
            is waited for until it is parked rather than assumed parked after
            a fixed pause (macOS CI took longer than 80 ms to get there). */
        const auto slow = stateFile ("slow.state", "gain=0.5\ndie=0\npad=1\nloadDelayMs=1500\n");
        lanes[0].wantState (slow);
        host.poll();

        const auto parkedBy = std::chrono::steady_clock::now() + std::chrono::milliseconds (1200);
        auto parked = false;

        while (! parked && std::chrono::steady_clock::now() < parkedBy)
            parked = std::abs (play (lanes[0]) - 0.8f) < 1.0e-4f;       // parked: dry

        REQUIRE (parked);
        CHECK_FALSE (lanes[0].stateSettled());
        CHECK (play (lanes[1]) == doctest::Approx (0.4f));

        REQUIRE (settled (lanes[0], 5000));
        CHECK (lanes[0].misses() == 0);
        CHECK (play (lanes[0]) == doctest::Approx (0.1f));
        CHECK (table.statusOf ("PG7N0001").stateLoadMs >= 1400.0);
    }

    SUBCASE ("a file that is not there: the preset, and a sentence that says so")
    {
        lanes[0].wantState (folder.path.getChildFile ("nowhere.state").getFullPathName().toStdString());
        REQUIRE (settled (lanes[0], 3000));
        CHECK (table.statusOf ("PG7N0001").stateProblem.find ("not in the bundle") != std::string::npos);
        CHECK (play (lanes[0]) == doctest::Approx (0.4f));
    }

    SUBCASE ("a lane the cue does not switch in has nothing to wait for")
    {
        lanes[1].setEnabled (false);
        lanes[1].wantState (padded);
        CHECK (lanes[1].stateSettled());
    }

    SUBCASE ("a state announced before its path settles nothing until the path arrives")
    {
        lanes[0].expectState();
        host.poll();
        host.poll();
        CHECK_FALSE (lanes[0].stateSettled());

        lanes[0].wantState (padded);
        REQUIRE (settled (lanes[0], 3000));
        CHECK (play (lanes[0]) == doctest::Approx (0.1f));
    }

    host.stop();
}

TEST_CASE ("proxy: a child killed mid-flight leaves the block dry, is marked failed with a sentence, stops being called, and comes back")
{
    Folder folder;
    plugin::PluginTable table;
    plugin::ProxyLane lane;
    plugin::ProxyHost host (testGainSpec (folder, 1), { &lane }, &table);

    std::vector<std::string> failures;
    host.onFailed ([&failures] (const std::string& id, const std::string& why) { failures.push_back (id + ": " + why); });

    std::string problem;
    REQUIRE (host.start (problem));
    REQUIRE (waitForState (host, "loaded", 5000));

    lane.setDeadlineMicroseconds (200000);
    lane.setEnabled (true);
    host.poll();

    Block block (2, 64, 0.8f);

    for (int i = 0; i < 4; ++i)
    {
        std::fill (block.storage.begin(), block.storage.end(), 0.8f);
        lane.process (block.data(), 2, 64);
    }

    REQUIRE (block.allEqual (0.4f));

    /*  As a task manager would. */
    host.killChild();

    for (int waited = 0; waited < 200 && host.childIsRunning(); ++waited)
        std::this_thread::sleep_for (std::chrono::milliseconds (10));

    REQUIRE_FALSE (host.childIsRunning());

    /*  Every block from here passes dry and counts a miss, until the host
        looks: eight misses, or the dead child, trip it within one poll. */
    lane.setDeadlineMicroseconds (500);
    const auto sentBefore = lane.blocks();

    for (int i = 0; i < plugin::ProxyLane::missesBeforeFailure; ++i)
    {
        std::fill (block.storage.begin(), block.storage.end(), 0.8f);
        lane.process (block.data(), 2, 64);
        CHECK (block.allEqual (0.8f));
    }

    CHECK (lane.consecutiveMisses() == static_cast<std::uint32_t> (plugin::ProxyLane::missesBeforeFailure));

    host.poll();
    CHECK (host.status().state == "failed");
    CHECK_FALSE (host.status().problem.empty());
    CHECK (table.statusOf ("PG7N0001").state == "failed");
    REQUIRE (failures.size() == 1);
    CHECK (failures[0].rfind ("PG7N0001: ", 0) == 0);

    /*  Not called any more: the request sequence stops moving. */
    const auto sentAfter = lane.blocks();
    CHECK (sentAfter == sentBefore + static_cast<std::uint64_t> (plugin::ProxyLane::missesBeforeFailure));
    lane.process (block.data(), 2, 64);
    CHECK (lane.blocks() == sentAfter);
    CHECK_FALSE (lane.isCallEnabled());

    /*  The one automatic restart, two seconds on. */
    REQUIRE (waitForState (host, "loaded", plugin::ProxyHost::restartDelayMs + 5000));
    CHECK (host.childIsRunning());
    CHECK (lane.isCallEnabled());
    CHECK (lane.consecutiveMisses() == 0);

    lane.setDeadlineMicroseconds (200000);
    host.poll();

    for (int i = 0; i < 4; ++i)
    {
        std::fill (block.storage.begin(), block.storage.end(), 0.8f);
        lane.process (block.data(), 2, 64);
    }

    CHECK (block.allEqual (0.4f));

    /*  A second death inside the minute stays down until asked. */
    host.killChild();

    for (int waited = 0; waited < 200 && host.childIsRunning(); ++waited)
        std::this_thread::sleep_for (std::chrono::milliseconds (10));

    host.poll();
    CHECK (host.status().state == "failed");
    CHECK (host.status().problem.find ("plugin.restart") != std::string::npos);
    CHECK (failures.size() == 2);

    std::this_thread::sleep_for (std::chrono::milliseconds (plugin::ProxyHost::restartDelayMs + 300));
    host.poll();
    CHECK (host.status().state == "failed");

    REQUIRE (host.restart (problem));
    REQUIRE (waitForState (host, "loaded", 5000));
    CHECK (host.childIsRunning());

    host.stop();
}

TEST_CASE ("proxy: a plugin the scan describes but the child cannot make reads failed, with the child's sentence")
{
    Folder folder;
    plugin::PluginTable table;
    plugin::ProxyLane lane;

    /*  A description the scan could have written, of a file that is not
        there: the child takes it, asks the format for an instance, and is
        told no - which is the path a plugin uninstalled since the scan, or
        one that will not load tonight, takes. */
    juce::PluginDescription nothing;
    nothing.name = "Nothing Here";
    nothing.pluginFormatName = "VST3";
    nothing.fileOrIdentifier = folder.path.getChildFile ("Nothing Here.vst3").getFullPathName();
    nothing.uniqueId = 0x4e6f7468;
    nothing.deprecatedUid = nothing.uniqueId;

    auto spec = testGainSpec (folder, 1);
    spec.identifier = nothing.createIdentifierString().toStdString();
    spec.descriptionXml = nothing.createXml()->toString().toStdString();
    plugin::ProxyHost host (spec, { &lane }, &table);

    std::string problem;
    REQUIRE (host.start (problem));
    REQUIRE (waitForState (host, "failed", 10000));
    CHECK (host.status().problem.find ("Nothing Here") != std::string::npos);
    CHECK_FALSE (lane.isCallEnabled());
    host.stop();
}

//==============================================================================
TEST_CASE ("proxy: plugin.failed writes the table and plugin.restart reaches the hook; an unknown id is refused")
{
    CommandRegistry registry;
    plugin::PluginTable table;
    table.set ("PG7N0001", { "loaded", "", 12, 3, 0.0, {}, 0, 0, {} });

    std::vector<std::string> restarted;
    plugin::PluginCommandHooks hooks;
    hooks.restart = [&restarted] (const std::string& id, std::string&) { restarted.push_back (id); return true; };
    plugin::registerPluginCommands (registry, table, hooks);

    CommandContext context;
    const std::string origin = "engine";
    context.origin = &origin;

    const auto dispatch = [] (CommandRegistry& in, CommandContext& with, const char* name,
                              std::vector<osc::Value> args)
    {
        const auto* command = in.find (name);
        return command != nullptr ? command->handler (with, args) : Outcome::rejected (reason::unknownCommand);
    };

    auto outcome = dispatch (registry, context, "plugin.failed",
                                      { osc::Value::string ("PG7N0001"), osc::Value::string ("the child died") });
    CHECK (outcome.applied);
    CHECK (table.statusOf ("PG7N0001").state == "failed");
    CHECK (table.statusOf ("PG7N0001").problem == "the child died");
    CHECK (table.statusOf ("PG7N0001").latencySamples == 12);
    CHECK (table.statusOf ("PG7N0001").paramCount == 3);

    /*  Twice is once: idempotent, as every engine report is. */
    outcome = dispatch (registry, context, "plugin.failed",
                                 { osc::Value::string ("PG7N0001"), osc::Value::string ("the child died") });
    CHECK (outcome.applied);

    outcome = dispatch (registry, context, "plugin.failed",
                                 { osc::Value::string ("PG7N0009"), osc::Value::string ("nobody") });
    CHECK_FALSE (outcome.applied);
    CHECK (outcome.reason == reason::unknownId);

    outcome = dispatch (registry, context, "plugin.restart", { osc::Value::string ("PG7N0001") });
    CHECK (outcome.applied);
    REQUIRE (restarted.size() == 1);
    CHECK (restarted[0] == "PG7N0001");

    /*  A replay has no hook and applies the record as a no-op. */
    CommandRegistry quiet;
    plugin::registerPluginCommands (quiet, table, {});
    outcome = dispatch (quiet, context, "plugin.restart", { osc::Value::string ("PG7N0001") });
    CHECK (outcome.applied);
}

//==============================================================================
TEST_CASE ("proxy: a plugin this machine's scan does not know reads missing, with a sentence, and launches nothing")
{
    Folder folder;
    plugin::PluginTable table;
    plugin::ProxyLane lane;

    auto spec = testGainSpec (folder, 1);
    spec.identifier = "VST3-Nowhere-00000000-00000000";
    spec.descriptionXml.clear();

    /*  Asked once more at the start (2026-09-26), since a scan may have found
        it since the graph was built - and this machine's list still says no. */
    std::vector<std::string> asked;
    spec.describe = [&asked] (const std::string& identifier) { asked.push_back (identifier); return std::string(); };
    plugin::ProxyHost host (spec, { &lane }, &table);

    std::string problem;
    CHECK_FALSE (host.start (problem));
    CHECK (asked == std::vector<std::string> { "VST3-Nowhere-00000000-00000000" });
    CHECK (host.status().state == "missing");
    CHECK (host.status().problem.find ("Show settings, Plugins") != std::string::npos);
    CHECK_FALSE (host.childIsRunning());
    CHECK (table.statusOf ("PG7N0001").state == "missing");
    host.stop();
}

TEST_CASE ("proxy: the child's catalogue is written beside the region, read into the store once it is up, and gone after stop")
{
    Folder folder;
    plugin::PluginTable table;
    plugin::CatalogueStore store { folder.path.getChildFile ("catalogue").getFullPathName().toStdString() };
    plugin::ProxyLane lane;

    auto spec = testGainSpec (folder, 1);
    spec.catalogues = &store;
    plugin::ProxyHost host (spec, { &lane }, &table);

    std::string problem;
    REQUIRE (host.start (problem));
    REQUIRE (waitForState (host, "loaded", 5000));

    const juce::File catalogueFile = juce::File (host.regionPath()).withFileExtension ("catalogue.json");
    CHECK (catalogueFile.existsAsFile());

    /*  The pickup is a put of what the child wrote: the test child's
        catalogue is the built-in one, so the store's content is unchanged
        and its revision does not move - which is the right answer, and the
        file's existence is the proof the path ran. */
    host.poll();
    const auto held = store.find (plugin::Catalogue::testGainIdentifier());
    REQUIRE (held != nullptr);
    CHECK (held->params.size() == 2u);

    host.stop();
    CHECK_FALSE (catalogueFile.existsAsFile());
}

TEST_CASE ("proxy: the catalogue-only verb answers the test child's catalogue as JSON on its stdout")
{
    juce::StringArray command;
    command.add (juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName());
    command.add ("plugin-host");
    command.add ("--catalogue-only");
    command.add (juce::String ("--plugin=") + plugin::Catalogue::testGainIdentifier());

    juce::ChildProcess child;
    REQUIRE (child.start (command, juce::ChildProcess::wantStdOut));
    const auto text = child.readAllProcessOutput();
    CHECK (child.waitForProcessToFinish (10000));
    CHECK (child.getExitCode() == 0);

    plugin::Catalogue catalogue;
    std::string problem;
    REQUIRE (plugin::Catalogue::fromJson (text.toStdString(), catalogue, problem));
    CHECK (catalogue.identifier == plugin::Catalogue::testGainIdentifier());
    REQUIRE (catalogue.params.size() == 2u);
    CHECK (catalogue.params[0].name == "Gain");
    CHECK (catalogue.params[1].discrete);
}

/*  A REAL PLUGIN, on a machine that has one: skipped unless asked for with
    `--no-skip` and WFG_REAL_VST3 set to an identifier from `wfg plugins
    --list`. The child is this test binary, which links the same hosting code
    the console does; the description comes off the machine's own scan. What
    it proves is the whole of 9a.7 against a plugin nobody wrote for it: the
    instances come up, the catalogue has its names, a block goes through and
    comes back, a value written reaches the instance. M31 and M34 measure. */
TEST_CASE ("proxy: a real VST3 from this machine's scan comes up, reports its catalogue, and processes a block"
           * doctest::skip())
{
    const auto identifier = juce::SystemStats::getEnvironmentVariable ("WFG_REAL_VST3", {}).toStdString();
    REQUIRE_MESSAGE (! identifier.empty(), "set WFG_REAL_VST3 to an identifier from wfg plugins --list");

    const auto storage = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                            .getChildFile ("Go.dot").getChildFile ("engine").getFullPathName().toStdString();
    const auto xml = plugin::describePlugin (storage, identifier);
    REQUIRE_MESSAGE (! xml.empty(), "the scan does not know " << identifier);

    Folder folder;
    plugin::PluginTable table;
    plugin::CatalogueStore store { folder.path.getChildFile ("catalogue").getFullPathName().toStdString() };
    plugin::ProxyLane lanes[2];

    auto spec = testGainSpec (folder, 2, 2, 256);
    spec.identifier = identifier;
    spec.name = "real";
    spec.descriptionXml = xml;
    spec.catalogues = &store;
    plugin::ProxyHost host (spec, { &lanes[0], &lanes[1] }, &table);

    std::string problem;
    REQUIRE (host.start (problem));
    const auto up = waitForState (host, "loaded", 20000);
    INFO ("state " << host.status().state << ": " << host.status().problem);
    REQUIRE (up);

    for (int i = 0; i < 50 && store.find (identifier) == nullptr; ++i)
    {
        host.poll();
        std::this_thread::sleep_for (std::chrono::milliseconds (20));
    }

    const auto catalogue = store.find (identifier);
    REQUIRE (catalogue != nullptr);
    MESSAGE (catalogue->name << ": " << catalogue->params.size() << " parameters, latency "
             << catalogue->latencySamples << " samples");

    for (std::size_t n = 0; n < std::min<std::size_t> (8, catalogue->params.size()); ++n)
        MESSAGE ("  p" << n << " " << catalogue->params[n].name << " [" << catalogue->params[n].unit << "] default "
                 << catalogue->params[n].defaultValue << " text " << catalogue->params[n].text.front()
                 << " .. " << catalogue->params[n].text.back() << std::string (catalogue->params[n].bipolar ? " bipolar" : ""));

    CHECK (host.status().paramCount == static_cast<int> (catalogue->params.size()));

    lanes[0].setDeadlineMicroseconds (200000);
    lanes[0].setEnabled (true);
    host.poll();

    Block block (2, 256, 0.25f);

    for (int i = 0; i < 20; ++i)
    {
        std::fill (block.storage.begin(), block.storage.end(), 0.25f);
        lanes[0].process (block.data(), 2, 256);
    }

    MESSAGE ("blocks " << lanes[0].blocks() << ", answered " << lanes[0].answered() << ", misses " << lanes[0].misses());
    CHECK (lanes[0].answered() >= 1);

    if (! catalogue->params.empty())
    {
        lanes[0].setParameter (0, 1.0f);
        std::fill (block.storage.begin(), block.storage.end(), 0.25f);
        lanes[0].process (block.data(), 2, 256);
        CHECK (lanes[0].misses() == 0);
    }

    host.stop();
}

/*  A REAL LV2, ON EVERY RUNNER (2026-09-26): the in-tree test bundle, built
    with the tests, described by JUCE's own LV2 format as a scan would, handed
    to a child that registers the LV2 format alone and loads the bundle from
    the folder the description names - so a plugin found through --path,
    outside every folder an LV2 world reads by itself, still comes up. A block
    goes through at the plugin's default gain of a half, and a value written
    to its one parameter reaches it. */
TEST_CASE ("proxy: the in-tree LV2 comes up in the child through JUCE's real LV2 host, and processes a block at its gain")
{
    const juce::File bundle { juce::String (WFG_TEST_LV2_BUNDLE) };
    REQUIRE (bundle.getChildFile ("manifest.ttl").existsAsFile());

    juce::LV2PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (found, bundle.getFullPathName());

    const juce::PluginDescription* gain = nullptr;

    for (const auto* description : found)
        if (description->fileOrIdentifier == "urn:godot:test-lv2-gain")
            gain = description;

    REQUIRE (gain != nullptr);
    CHECK (gain->pluginFormatName == "LV2");
    CHECK (gain->name == "Go.dot test LV2 gain");
    CHECK (gain->numInputChannels == 2);
    CHECK (gain->numOutputChannels == 2);

    auto xml = gain->createXml();
    REQUIRE (xml != nullptr);
    xml->setAttribute ("bundle", bundle.getFullPathName());

    Folder folder;
    plugin::PluginTable table;
    plugin::CatalogueStore store { folder.path.getChildFile ("catalogue").getFullPathName().toStdString() };
    plugin::ProxyLane lanes[1];

    auto spec = testGainSpec (folder, 1, 2, 64);
    spec.identifier = gain->createIdentifierString().toStdString();
    spec.name = "LV2 gain";
    spec.descriptionXml = xml->toString().toStdString();
    spec.catalogues = &store;
    plugin::ProxyHost host (spec, { &lanes[0] }, &table);

    std::string problem;
    REQUIRE (host.start (problem));
    const auto up = waitForState (host, "loaded", 20000);
    INFO ("state " << host.status().state << ": " << host.status().problem);
    REQUIRE (up);
    CHECK (host.status().paramCount >= 1);

    //  Its two ports name no speaker: taken as two numbered channels, in and out.
    CHECK (host.status().inputs == 2);
    CHECK (host.status().outputs == 2);

    lanes[0].setDeadlineMicroseconds (200000);
    lanes[0].setEnabled (true);
    host.poll();

    Block block (2, 64, 0.25f);

    const auto blocksAt = [&] (float in)
    {
        for (int i = 0; i < 10; ++i)
        {
            std::fill (block.storage.begin(), block.storage.end(), in);
            lanes[0].process (block.data(), 2, 64);
        }
    };

    blocksAt (0.25f);
    CHECK (lanes[0].answered() >= 1);
    CHECK (block.allEqual (0.125f));

    lanes[0].setParameter (0, 1.0f);
    blocksAt (0.25f);
    CHECK (block.allEqual (0.25f));
    CHECK (lanes[0].misses() == 0);

    host.stop();
}

#if JUCE_MAC
/*  A REAL AU, ON THE macOS RUNNER (2026-09-26): Apple's own AUBandpass, which
    every Mac has, described by JUCE's AU format as a scan would and hosted in
    the child through that format alone. A band-pass takes a constant away:
    a block of DC goes in and, once the filter has settled, next to nothing
    comes back - which proves the AU both came up and sounds. */
TEST_CASE ("proxy: Apple's AUBandpass comes up in the child through JUCE's AU host, and takes a constant away")
{
    juce::AudioUnitPluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (found, "AudioUnit:Effects/aufx,bpas,appl");
    REQUIRE (found.size() >= 1);
    CHECK (found[0]->pluginFormatName == "AudioUnit");

    auto xml = found[0]->createXml();
    REQUIRE (xml != nullptr);

    Folder folder;
    plugin::PluginTable table;
    plugin::ProxyLane lanes[1];

    auto spec = testGainSpec (folder, 1, 2, 256);
    spec.identifier = found[0]->createIdentifierString().toStdString();
    spec.name = "AUBandpass";
    spec.descriptionXml = xml->toString().toStdString();
    plugin::ProxyHost host (spec, { &lanes[0] }, &table);

    std::string problem;
    REQUIRE (host.start (problem));
    const auto up = waitForState (host, "loaded", 20000);
    INFO ("state " << host.status().state << ": " << host.status().problem);
    REQUIRE (up);

    lanes[0].setDeadlineMicroseconds (200000);
    lanes[0].setEnabled (true);
    host.poll();

    Block block (2, 256, 0.5f);

    for (int i = 0; i < 200; ++i)
    {
        std::fill (block.storage.begin(), block.storage.end(), 0.5f);
        lanes[0].process (block.data(), 2, 256);
    }

    auto loudest = 0.0f;

    for (const auto sample : block.storage)
        loudest = std::max (loudest, std::fabs (sample));

    CHECK (lanes[0].answered() >= 1);
    CHECK (loudest < 0.05f);
    host.stop();
}
#endif

//==============================================================================
namespace
{
    struct Percentiles { double p50 = 0.0, p99 = 0.0, max = 0.0; };

    Percentiles percentilesOf (std::vector<double> samples)
    {
        if (samples.empty())
            return {};

        std::sort (samples.begin(), samples.end());
        const auto at = [&samples] (double fraction)
        {
            return samples[std::min (samples.size() - 1, static_cast<std::size_t> (static_cast<double> (samples.size()) * fraction))];
        };
        return { at (0.5), at (0.99), samples.back() };
    }

    /*  One lane driven `blocks` times, each call timed; the first call after an
        idle spell timed on its own (the worker sleeps in one-millisecond polls
        while no lane is switched in, and the host's poll tells it to spin). */
    struct RoundTrip
    {
        Percentiles all;
        double firstAfterIdleUs = 0.0;
        std::uint32_t misses = 0;
    };

    RoundTrip driveLane (plugin::ProxyLane& lane, int channels, int block, int blocks)
    {
        Block audio (channels, block, 0.25f);
        std::vector<double> times;
        times.reserve (static_cast<std::size_t> (blocks));

        const auto missesBefore = lane.misses();

        for (int i = 0; i < blocks; ++i)
        {
            const auto t0 = std::chrono::steady_clock::now();
            lane.process (audio.data(), channels, block);
            const auto us = std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count();

            if (i == 0)
                continue;   // the first call carries the wake-up; measured below

            times.push_back (us);
        }

        RoundTrip out;
        out.all = percentilesOf (times);
        out.misses = lane.misses() - missesBefore;

        /*  Idle for a while with the lane off, so the worker goes to its
            millisecond polls; then one call. */
        lane.setEnabled (false);
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
        lane.setEnabled (true);
        const auto t0 = std::chrono::steady_clock::now();
        lane.process (audio.data(), channels, block);
        out.firstAfterIdleUs = std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count();
        return out;
    }
}

/*  M31 - the proxy's round trip (§17.9, PRD §6.11): p50, p99 and max of one
    lane's process() with the test child, at 1, 8 and 16 lanes on one child,
    each lane switched in and driven in turn; the wake-up cost of the first
    request after the worker's idle poll; and, with WFG_REAL_VST3 set, the same
    through a real plugin. A generous deadline, so a late answer is measured
    rather than dropped; the misses column says how many were later than
    that. Run with --no-skip on a quiet machine; the figures go to §17.9. */
TEST_CASE ("M31: the proxy's round trip at one, eight and sixteen lanes, the wake-up after idle, and a real plugin"
           * doctest::skip())
{
    const auto realIdentifier = juce::SystemStats::getEnvironmentVariable ("WFG_REAL_VST3", {}).toStdString();
    const auto storage = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                            .getChildFile ("Go.dot").getChildFile ("engine").getFullPathName().toStdString();

    MESSAGE ("M31 on " << juce::SystemStats::getOperatingSystemName() << ", "
             << juce::SystemStats::getCpuModel() << ", " << juce::SystemStats::getNumCpus() << " cores");
    MESSAGE ("plugin | lanes | block | p50 us | p99 us | max us | misses of 2000 | first after idle us");

    for (const auto& identifier : std::vector<std::string> { plugin::Catalogue::testGainIdentifier(), realIdentifier })
    {
        if (identifier.empty())
            continue;

        std::string xml;

        if (identifier != plugin::Catalogue::testGainIdentifier())
        {
            xml = plugin::describePlugin (storage, identifier);

            if (xml.empty())
            {
                MESSAGE ("the scan does not know " << identifier << "; skipped");
                continue;
            }
        }

        for (const auto laneCount : { 1, 8, 16 })
        {
            for (const auto block : { 64, 256 })
            {
                Folder folder;
                plugin::PluginTable table;
                std::vector<plugin::ProxyLane> lanes (static_cast<std::size_t> (laneCount));
                std::vector<plugin::ProxyLane*> pointers;

                for (auto& lane : lanes)
                    pointers.push_back (&lane);

                auto spec = testGainSpec (folder, laneCount, 2, block);
                spec.identifier = identifier;
                spec.descriptionXml = xml;
                spec.deadlineMicroseconds = 20000;
                plugin::ProxyHost host (spec, pointers, &table);

                std::string problem;
                REQUIRE (host.start (problem));
                const auto up = waitForState (host, "loaded", 20000);
                INFO (host.status().state << ": " << host.status().problem);
                REQUIRE (up);

                for (auto& lane : lanes)
                {
                    lane.setDeadlineMicroseconds (20000);
                    lane.setEnabled (true);
                }

                host.poll();

                /*  Every lane driven, as a block does when every voice is in;
                    the lane reported is the last, warmed by the rest. */
                std::vector<double> times;
                RoundTrip last;

                for (int round = 0; round < 2000; ++round)
                {
                    Block audio (2, block, 0.25f);

                    for (int i = 0; i < laneCount; ++i)
                    {
                        const auto t0 = std::chrono::steady_clock::now();
                        lanes[static_cast<std::size_t> (i)].process (audio.data(), 2, block);

                        if (i == laneCount - 1 && round > 0)
                            times.push_back (std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count());
                    }
                }

                const auto stats = percentilesOf (times);
                std::uint32_t misses = 0;

                for (const auto& lane : lanes)
                    misses += lane.misses();

                last = driveLane (lanes.back(), 2, block, 200);

                MESSAGE (std::string (identifier == plugin::Catalogue::testGainIdentifier() ? "test-gain" : "real") << " | "
                         << laneCount << " | " << block << " | " << juce::String (stats.p50, 1) << " | "
                         << juce::String (stats.p99, 1) << " | " << juce::String (stats.max, 1) << " | "
                         << misses << " | " << juce::String (last.firstAfterIdleUs, 1));

                host.stop();
            }
        }
    }
}

/*  M32 - a failed strip (§17.9): blocks late before the threshold trips, the
    time from the child's death to `failed` at the host's poll cadence, and
    what a block costs before and after. */
TEST_CASE ("M32: a strip that fails - misses to the trip, kill to failed, the block's cost before and after"
           * doctest::skip())
{
    Folder folder;
    plugin::PluginTable table;
    plugin::ProxyLane lane;
    plugin::ProxyHost host (testGainSpec (folder, 1, 2, 64), { &lane }, &table);

    std::string problem;
    REQUIRE (host.start (problem));
    REQUIRE (waitForState (host, "loaded", 5000));

    lane.setDeadlineMicroseconds (250);
    lane.setEnabled (true);
    host.poll();

    Block audio (2, 64, 0.25f);

    for (int i = 0; i < 200; ++i)
        lane.process (audio.data(), 2, 64);

    const auto before = driveLane (lane, 2, 64, 500);
    MESSAGE ("healthy, 250 us deadline: p50 " << juce::String (before.all.p50, 1) << " us, p99 "
             << juce::String (before.all.p99, 1) << " us, max " << juce::String (before.all.max, 1)
             << " us, misses " << before.misses << " of 500");

    host.killChild();
    const auto killedAt = std::chrono::steady_clock::now();

    /*  Blocks at the block rate until the host trips it. */
    auto blocksUntilTrip = 0;
    std::vector<double> lateCosts;

    while (host.status().state == "loaded")
    {
        const auto t0 = std::chrono::steady_clock::now();
        lane.process (audio.data(), 2, 64);
        lateCosts.push_back (std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count());
        ++blocksUntilTrip;
        host.poll();
        std::this_thread::sleep_for (std::chrono::microseconds (1333));
    }

    const auto failedAfterMs = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - killedAt).count();
    const auto late = percentilesOf (lateCosts);

    std::vector<double> afterCosts;

    for (int i = 0; i < 500; ++i)
    {
        const auto t0 = std::chrono::steady_clock::now();
        lane.process (audio.data(), 2, 64);
        afterCosts.push_back (std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count());
    }

    const auto after = percentilesOf (afterCosts);

    MESSAGE ("kill to failed: " << juce::String (failedAfterMs, 1) << " ms, " << blocksUntilTrip
             << " blocks at the block rate, each a miss costing p50 " << juce::String (late.p50, 1)
             << " us, max " << juce::String (late.max, 1) << " us");
    MESSAGE ("failed, not called: p50 " << juce::String (after.p50, 2) << " us, max " << juce::String (after.max, 2) << " us");
    CHECK (host.status().state == "failed");

    host.stop();
}

//==============================================================================
namespace
{
    struct ScopedStorage
    {
        ScopedStorage()
            : folder (juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("wfg-proxy-host-" + juce::Uuid().toDashedString()))
        {
        }

        ~ScopedStorage() { folder.deleteRecursively(); }

        std::string path() const { return folder.getFullPathName().toStdString(); }

        juce::File folder;
    };

    audio::EditSpec specWithPlugins (int tracks, int plugins)
    {
        audio::EditSpec spec;
        spec.tracks = tracks;
        spec.channelsPerTrack = 2;

        for (int i = 0; i < plugins; ++i)
        {
            audio::PluginSpec entry;
            entry.id = "PG7N000" + std::to_string (i + 1);
            entry.identifier = plugin::Catalogue::testGainIdentifier();
            entry.name = "Test gain " + std::to_string (i + 1);
            spec.plugins.push_back (entry);
        }

        return spec;
    }
}

TEST_CASE ("M35: a cue's whole state loaded onto a voice - how long it takes, and whether the other voices miss while it loads"
           * doctest::skip())
{
    /*  THE COST OF THE AUTHOR'S CHOICE (decision AI, 2026-09-25, §17.13): a
        plugin's whole state kept per cue and loaded onto the voice before the
        cue launches. Two questions. How long a load takes - a cue fired cold
        is late by that much, and `run.late` says so. And whether a load makes
        the OTHER voices miss: the loading lane is parked and answers dry, but
        a plugin whose setState takes a lock its process() also takes on other
        instances would stall them - which only a real plugin can answer.

        The states are made the way the product makes them: the editing
        helper, headless, a parameter moved as a hand would and the state
        captured. Then a two-voice child: voice one plays a block every block
        period at the default deadline, voice two is loaded with the two states
        in turn, twenty times; and the same length again with no loads, for
        the misses a busy box has anyway. Run with
        `WFG_REAL_VST3=<identifier> wfg_tests --test-case="M35*" --no-skip`. */
    const auto realIdentifier = juce::SystemStats::getEnvironmentVariable ("WFG_REAL_VST3", {}).toStdString();
    const auto storage = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                            .getChildFile ("Go.dot").getChildFile ("engine").getFullPathName().toStdString();

    MESSAGE ("M35 on " << juce::SystemStats::getOperatingSystemName() << ", "
             << juce::SystemStats::getCpuModel() << ", " << juce::SystemStats::getNumCpus() << " cores");
    MESSAGE ("plugin | state bytes | load min ms | median ms | max ms | voice-one misses while loading | the same time idle");

    for (const auto& identifier : std::vector<std::string> { plugin::Catalogue::testGainIdentifier(), realIdentifier })
    {
        if (identifier.empty())
            continue;

        std::string xml;

        if (identifier != plugin::Catalogue::testGainIdentifier())
        {
            xml = plugin::describePlugin (storage, identifier);

            if (xml.empty())
            {
                MESSAGE ("the scan does not know " << identifier << "; skipped");
                continue;
            }
        }

        Folder folder;
        const auto stateFolder = folder.path.getChildFile ("state");

        //  TWO STATES, as a hand makes them in the plugin's own window.
        std::vector<std::string> states;
        {
            plugin::EditorSpec edit;
            edit.pluginId = "PG7N0001";
            edit.identifier = identifier;
            edit.descriptionXml = xml;
            edit.workFolder = folder.string();
            edit.stateFolder = stateFolder.getFullPathName().toStdString();
            edit.headless = true;
            edit.launch.executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                                         .getFullPathName().toStdString();

            plugin::EditorHost helper (std::move (edit));
            std::string why;
            REQUIRE_MESSAGE (helper.start (why), why);

            const auto until = [&helper] (auto done, int milliseconds)
            {
                const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds (milliseconds);

                while (std::chrono::steady_clock::now() < end)
                {
                    helper.poll();

                    if (done())
                        return true;

                    std::this_thread::sleep_for (std::chrono::milliseconds (10));
                }

                return done();
            };

            REQUIRE (until ([&] { return helper.status() == plugin::EditorHost::Status::open; }, 30000));
            const auto count = std::max (1, helper.paramCount());
            std::uint32_t seq = 0;

            for (const auto value : { 0.2f, 0.8f })
            {
                std::vector<float> values (static_cast<std::size_t> (count), plugin::editor::restsAtPreset);
                helper.setSubject ({ "CUE00001", "FX000001", "M35", {}, false, values, {} });
                ++seq;
                REQUIRE (until ([&] { return helper.subjectTaken() == seq; }, 5000));

                helper.poke (0, value);
                values[0] = value;
                helper.setLive (values);

                std::optional<plugin::EditorHost::Capture> kept;
                REQUIRE (until ([&] { kept = helper.takeCapture(); return kept.has_value(); }, 8000));
                states.push_back (folder.path.getChildFile (juce::String (kept->stateFile)).getFullPathName().toStdString());
            }

            helper.leave (false);
        }

        REQUIRE (states.size() == 2);
        const auto bytes = juce::File (juce::String (states.front())).getSize();

        //  A TWO-VOICE CHILD: voice one plays, voice two is loaded.
        plugin::PluginTable table;
        plugin::ProxyLane lanes[2];
        auto spec = testGainSpec (folder, 2, 2, 64);
        spec.identifier = identifier;
        spec.descriptionXml = xml;
        plugin::ProxyHost host (spec, { &lanes[0], &lanes[1] }, &table);

        std::string problem;
        REQUIRE (host.start (problem));
        const auto up = waitForState (host, "loaded", 20000);
        INFO (host.status().state << ": " << host.status().problem);
        REQUIRE (up);

        lanes[0].setEnabled (true);
        lanes[1].setEnabled (true);
        host.poll();

        std::atomic<bool> playing { true };
        std::thread voiceOne ([&lanes, &playing]
        {
            Block block (2, 64, 0.5f);
            auto next = std::chrono::steady_clock::now();

            while (playing.load())
            {
                lanes[0].process (block.data(), 2, 64);
                next += std::chrono::microseconds (1333);
                std::this_thread::sleep_until (next);
            }
        });

        /*  WARMED UP FIRST: voice one's first block may find the worker in
            its idle sleep, which is M31's "first after idle" and not a load's
            doing - so the count starts once voice one has been playing. */
        std::this_thread::sleep_for (std::chrono::milliseconds (200));

        std::vector<double> loads;
        const auto missesBefore = lanes[0].misses();
        const auto loadingBegan = std::chrono::steady_clock::now();

        for (int i = 0; i < 20; ++i)
        {
            lanes[1].wantState (states[static_cast<std::size_t> (i % 2)]);
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds (10);

            while (! lanes[1].stateSettled() && std::chrono::steady_clock::now() < end)
            {
                host.poll();
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
            }

            REQUIRE (lanes[1].stateSettled());
            loads.push_back (table.statusOf ("PG7N0001").stateLoadMs);
        }

        const auto loadingTook = std::chrono::steady_clock::now() - loadingBegan;
        const auto missesLoading = lanes[0].misses() - missesBefore;

        //  The same length again with no loads: what this box misses anyway.
        const auto idleBefore = lanes[0].misses();
        std::this_thread::sleep_for (loadingTook);
        const auto missesIdle = lanes[0].misses() - idleBefore;

        playing.store (false);
        voiceOne.join();
        host.stop();

        std::sort (loads.begin(), loads.end());
        MESSAGE ((identifier == plugin::Catalogue::testGainIdentifier() ? std::string ("test-gain") : identifier) << " | "
                 << bytes << " | " << loads.front() << " | " << loads[loads.size() / 2] << " | " << loads.back()
                 << " | " << missesLoading << " | " << missesIdle);
    }
}

TEST_CASE ("proxy: the graph carries one proxy per set entry on every voice, its ids stay unique, and stop removes every region")
{
    ScopedStorage storage;
    audio::AudioHost host { storage.path() };

    audio::HostSettings settings;
    settings.sampleRate = 48000;
    settings.blockSize = 64;
    settings.outputChannels = 2;
    REQUIRE (host.start (settings));

    plugin::PluginTable table;
    audio::ProxyServices services;
    services.table = &table;
    services.launch = launchOfThisBinary();
    int failures = 0;
    services.onFailed = [&failures] (const std::string&, const std::string&) { ++failures; };
    host.setProxyServices (services);

    for (const auto tracks : { 1, 8, 16 })
    {
        INFO ("tracks " << tracks);
        REQUIRE (host.buildEdit (specWithPlugins (tracks, 3)));
        CHECK (host.proxyCount() == 3);
        CHECK (host.inspectNodeIds().ok());

        for (int slot = 0; slot < 3; ++slot)
        {
            REQUIRE (host.proxy (slot) != nullptr);
            CHECK (juce::File (host.proxy (slot)->regionPath()).existsAsFile());
        }
    }

    /*  Every child up, on all three. */
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds (10);

    for (;;)
    {
        host.pollProxies();

        auto loaded = 0;

        for (int slot = 0; slot < 3; ++slot)
            if (table.statusOf (host.proxy (slot)->pluginId()).state == "loaded")
                ++loaded;

        if (loaded == 3 || std::chrono::steady_clock::now() > until)
        {
            CHECK (loaded == 3);
            break;
        }

        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }

    /*  The tick thread's setters reach the lanes; a block through a graph
        with nothing switched in allocates nothing of ours. */
    host.setTrackFxEnabled (3, 1, true);
    host.setTrackFxParameter (3, 1, 0, 0.25f);
    CHECK (host.proxyLane (3, 1) != nullptr);
    CHECK (host.proxyLane (3, 1)->isEnabled());
    CHECK (host.proxyLane (3, 1)->parameter (0) == doctest::Approx (0.25f));
    CHECK (host.proxyLane (99, 1) == nullptr);
    CHECK (host.proxyLane (3, 9) == nullptr);

    host.snapTrackFx (3, 1, false, {});
    CHECK_FALSE (host.proxyLane (3, 1)->isEnabled());

    rt::resetCounts();

    for (int i = 0; i < 50; ++i)
        host.processBlock();

    if (rt::isCounting())
        CHECK (rt::violations() == 0);

    std::vector<std::string> regions;

    for (int slot = 0; slot < 3; ++slot)
        regions.push_back (host.proxy (slot)->regionPath());

    host.stop();

    for (const auto& region : regions)
        CHECK_FALSE (juce::File (region).existsAsFile());

    CHECK (failures == 0);
}
