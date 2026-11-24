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
    and its silence with no child at all, the test child answering through a
    real region and a real second process, a child killed mid-flight and the
    entry marked failed within a poll, the restart, the commands, and the
    whole thing inside a Tracktion graph with three plugins on sixteen voices.

    THE CHILD IS THIS BINARY. TestMain dispatches `plugin-host` before doctest
    sees the command line, so the launch names the test executable and no
    other build product has to exist - the CI job that runs this has wfg_tests
    and nothing else in hand.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/Engine.h>
#include <wfg/engine/app/RealtimeRights.h>
#include <wfg/engine/audio/AudioHost.h>
#include <wfg/engine/clock/AudioClockSource.h>
#include <wfg/engine/audio/CueEq.h>
#include <wfg/engine/audio/CueMatrix.h>
#include <wfg/engine/audio/EqSettings.h>
#include <wfg/engine/audio/HostPlayer.h>
#include <wfg/engine/audio/KeyReader.h>
#include <wfg/engine/audio/Looper.h>
#include <wfg/engine/plugin/BlockFence.h>
#include <wfg/engine/plugin/Catalogue.h>
#include <wfg/engine/plugin/GridPlayHead.h>
#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/plugin/PluginCommands.h>
#include <wfg/engine/plugin/PluginTable.h>
#include <wfg/engine/plugin/PluginScan.h>
#include <wfg/engine/plugin/EditorHost.h>
#include <wfg/engine/plugin/EditorRegion.h>
#include <wfg/engine/cue/InsertChain.h>
#include <wfg/engine/plugin/LaneMapping.h>
#include <wfg/engine/plugin/PluginLoad.h>
#include <wfg/engine/plugin/PluginMeasure.h>
#include <wfg/engine/plugin/ProxyHost.h>
#include <wfg/engine/plugin/ProxyLane.h>
#include <wfg/engine/plugin/ScratchBlock.h>
#include <wfg/engine/plugin/SharedRegion.h>
#include <wfg/engine/plugin/WorkgroupShare.h>
#include <wfg/engine/rt/RtCheck.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#if JUCE_MAC
 #include <juce_audio_devices/juce_audio_devices.h>
 #include <AudioToolbox/AudioWorkInterval.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
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
        /*  `slots` is the queue each lane is given (namespace draft 149):
            none unless a case asks, which is the region every case before it
            was written for. */
        VectorRegion (int channels, int maxSamples, int lanes, int slots = 0)
            : bytes (plugin::region::regionBytes (channels, maxSamples, lanes, slots), 0),
              channelCount (channels), sampleCount (maxSamples), laneCount (lanes), slotCount (slots)
        {
            header = new (bytes.data()) plugin::region::Header {};
            header->queueSlots.store (static_cast<std::uint32_t> (slots));

            for (int i = 0; i < lanes; ++i)
                new (plugin::region::laneAt (bytes.data(), channels, maxSamples, i, slots)) plugin::region::Lane {};
        }

        plugin::region::Lane* lane (int index)
        {
            return plugin::region::laneAt (bytes.data(), channelCount, sampleCount, index, slotCount);
        }

        void bind (plugin::ProxyLane& proxy, int index)
        {
            auto* l = lane (index);
            proxy.bind (header, l, plugin::region::audioOf (l), channelCount, sampleCount);
        }

        /*  THE CHILD'S PART, PLAYED BY HAND (namespace draft 139, BHC): every
            reset asked for is made - a lane waits for its reset, so a case
            that switches one in says so before it expects a call. */
        void answerResets()
        {
            for (int i = 0; i < laneCount; ++i)
                lane (i)->resetDoneSeq.store (lane (i)->resetSeq.load());
        }

        /** And its answer to what a lane was last asked, the audio left as it is. */
        void answer (int index)
        {
            lane (index)->responseSeq.store (lane (index)->requestSeq.load());
        }

        std::vector<char> bytes;
        int channelCount, sampleCount, laneCount, slotCount;
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

        /*  NO QUEUE, so its lanes can only wait (namespace draft 149): every
            case written before a plugin could be played later counts late
            blocks one after another, which a lane given more at once does
            not go on having. A case about being played later asks for the
            queue itself. */
        spec.queueSlots = 0;
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

    /*  Polls the host until the lane has nothing left to wait for - the
        reset its switch-in asked for made, its state held - or the time is
        up. What every arm waits for before its launch (namespace draft 139,
        BHC): until then a lane just switched in passes its block as it came. */
    bool waitSettled (plugin::ProxyHost& host, plugin::ProxyLane& lane, int milliseconds = 5000)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds (milliseconds);

        for (;;)
        {
            host.poll();

            if (lane.stateSettled())
                return true;

            if (std::chrono::steady_clock::now() > until)
                return false;

            std::this_thread::sleep_for (std::chrono::milliseconds (2));
        }
    }

    /*  WHERE A REAL PLUGIN'S CASE READS THE SCAN: the developer's own list, or
        WFG_ENGINE_FOLDER - a folder of its own, what `wfg plugins
        --engine-folder=<dir> --scan --retry=<file>` scanned one plugin into,
        so a plugin from a build tree is measured without entering the real
        list (namespace draft 142). */
    std::string realPluginStorage()
    {
        const auto chosen = juce::SystemStats::getEnvironmentVariable ("WFG_ENGINE_FOLDER", {});

        if (chosen.isNotEmpty())
            return juce::File::getCurrentWorkingDirectory().getChildFile (chosen).getFullPathName().toStdString();

        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("Go.dot").getChildFile ("engine").getFullPathName().toStdString();
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

TEST_CASE ("proxy: a lane that is off leaves the block untouched; unbound or not to be called it is silent; none of them signals")
{
    plugin::ProxyLane lane;

    /*  Unbound - a plugin this machine does not have: nothing to talk to, and
        the cue that has it switched in is silent, never dry (the author's
        decision of 2026-09-26, CU). */
    Block missing (2, 64, 0.25f);
    lane.setEnabled (true);
    lane.process (missing.data(), 2, 64);
    CHECK (missing.allEqual (0.0f));
    CHECK (lane.blocks() == 0);

    VectorRegion region (2, 64, 1);
    region.bind (lane, 0);
    CHECK (lane.isBound());

    /*  Off: bound, but the cue does not switch it in - the block as it came. */
    Block off (2, 64, 0.25f);
    lane.setEnabled (false);
    lane.process (off.data(), 2, 64);
    CHECK (off.allEqual (0.25f));
    CHECK (lane.blocks() == 0);
    CHECK (region.lane (0)->requestSeq.load() == 0);

    /*  Failed: the host cleared the call - silent, and not called. */
    Block failed (2, 64, 0.25f);
    lane.setEnabled (true);
    lane.setCallEnabled (false);
    lane.process (failed.data(), 2, 64);
    CHECK (failed.allEqual (0.0f));
    CHECK (lane.blocks() == 0);
    CHECK (region.lane (0)->requestSeq.load() == 0);
}

TEST_CASE ("proxy: with nobody answering, a lane misses at its deadline, is silent for it, counts, and sends nothing more")
{
    plugin::ProxyLane lane;
    VectorRegion region (2, 64, 1);
    region.bind (lane, 0);
    lane.setEnabled (true);
    region.answerResets();
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

    /*  ONE REQUEST, and nine blocks that waited for its answer and sent
        nothing (namespace draft 139, BHC): the audio of a request not yet
        answered is the child's to write in. Late all the same, each of them. */
    CHECK (block.allEqual (0.0f));      // never the dry block (CV)
    CHECK (lane.blocks() == 1);
    CHECK (lane.misses() == 10);
    CHECK (lane.consecutiveMisses() == 10);
    CHECK (lane.answered() == 0);
    CHECK (region.lane (0)->requestSeq.load() == 1);

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

TEST_CASE ("proxy: a late block fades to silence from where the last answer left it, and the next answer fades back in")
{
    /*  The author's decision of 2026-09-26 (CV): a late block is never the
        dry block and never a click. The rest of it falls from the last sample
        the plugin gave back to nothing over the quick fade, and the first
        block answered after silence rises over the same. A child is stood in
        for by the response count: set ahead, every request is answered at
        once with what was sent; left behind, every one is late. */
    constexpr auto fade = plugin::ProxyLane::fadeSamples;
    static_assert (fade < 64);

    plugin::ProxyLane lane;
    VectorRegion region (1, 64, 1);
    region.bind (lane, 0);
    lane.setEnabled (true);
    region.answerResets();
    lane.setDeadlineMicroseconds (200);

    auto* shared = region.lane (0);
    const auto answering = [shared] (bool yes) { shared->responseSeq.store (yes ? (1u << 30) : 0u); };

    rt::resetCounts();

    const auto play = [&lane] (Block& block)
    {
        const rt::ScopedRealtimeCheck ours { rt::Region::ours };
        lane.process (block.data(), 1, 64);
    };

    //  Answered: what was sent comes back, and the lane knows where it left off.
    answering (true);
    Block first (1, 64, 0.5f);
    play (first);
    CHECK (first.allEqual (0.5f));
    CHECK (lane.misses() == 0);

    //  Late: down from 0.5 to nothing over the fade, never rising, then nothing.
    answering (false);
    Block late (1, 64, 0.5f);
    play (late);
    CHECK (lane.misses() == 1);
    CHECK (late.storage[0] == doctest::Approx (0.5f * (1.0f - 1.0f / static_cast<float> (fade))));
    CHECK (late.storage[static_cast<std::size_t> (fade / 2 - 1)] == doctest::Approx (0.25f));

    for (int n = 1; n < 64; ++n)
        CHECK (late.storage[static_cast<std::size_t> (n)] <= late.storage[static_cast<std::size_t> (n - 1)]);

    for (int n = fade - 1; n < 64; ++n)
        CHECK (late.storage[static_cast<std::size_t> (n)] == doctest::Approx (0.0f));

    //  Late again: silence throughout, with nothing left to fall from.
    Block still (1, 64, 0.5f);
    play (still);
    CHECK (still.allEqual (0.0f));

    //  Answered again: up from nothing over the fade, then the answer as it came.
    answering (true);
    Block back (1, 64, 0.5f);
    play (back);
    CHECK (back.storage[0] == doctest::Approx (0.5f / static_cast<float> (fade)));
    CHECK (back.storage[static_cast<std::size_t> (fade - 1)] == doctest::Approx (0.5f));

    for (int n = 1; n < fade; ++n)
        CHECK (back.storage[static_cast<std::size_t> (n)] >= back.storage[static_cast<std::size_t> (n - 1)]);

    for (int n = fade; n < 64; ++n)
        CHECK (back.storage[static_cast<std::size_t> (n)] == doctest::Approx (0.5f));

    //  And a new cue on the voice falls from nothing of the last one's.
    lane.requestReset();
    answering (false);
    Block fresh (1, 64, 0.5f);
    play (fresh);
    CHECK (fresh.allEqual (0.0f));

    if (rt::isCounting())
        CHECK (rt::violations() == 0);
}

TEST_CASE ("proxy: a lane puts each piece's first sample and the beat clock in the region, the clock once per change (namespace draft §123, BEI)")
{
    plugin::ProxyLane lane;
    VectorRegion region (2, 64, 1);
    region.bind (lane, 0);
    lane.setEnabled (true);
    region.answerResets();

    plugin::BlockClock clock;
    clock.blockStart = 1000;
    clock.seq = 1;
    clock.state.anchorSample = 500;
    clock.state.clock.anchorBeat = 2.0;
    clock.state.clock.bpmFrom = 100.0;
    clock.state.clock.bpmTo = 80.0;
    clock.state.clock.rampLength = 3.0;
    clock.state.clock.shape = 0.5;
    clock.state.clock.beatsPerBar = 3;

    //  Nobody answers: the first piece misses, and was placed before it was asked.
    Block block (2, 64, 0.25f);
    lane.process (block.data(), 2, 64, &clock, clock.blockStart);
    const auto* laid = region.lane (0);
    CHECK (laid->blockStartSample.load() == 1000u);

    const auto* header = region.header;
    CHECK (header->gridSeq.load() == 2u);
    CHECK (header->gridAnchorSample.load() == 500u);
    CHECK (header->gridAnchorBeat.load() == doctest::Approx (2.0));
    CHECK (header->gridBpmFrom.load() == doctest::Approx (100.0));
    CHECK (header->gridBpmTo.load() == doctest::Approx (80.0));
    CHECK (header->gridRampLength.load() == doctest::Approx (3.0));
    CHECK (header->gridShape.load() == doctest::Approx (0.5));
    CHECK (header->gridBeatsPerBar.load() == 3u);
    CHECK (header->gridPlaying.load() == 1u);

    /*  The same clock again: not written again. A moved count: written, the
        count even once more. Each request answered - late - before the next,
        since nothing is sent over one still owed. */
    region.answer (0);
    lane.process (block.data(), 2, 64, &clock, 1064);
    CHECK (header->gridSeq.load() == 2u);
    CHECK (laid->blockStartSample.load() == 1064u);

    region.answer (0);
    clock.seq = 2;
    clock.state.clock.bpmTo = 70.0;
    lane.process (block.data(), 2, 64, &clock, 1128);
    CHECK (header->gridSeq.load() == 4u);
    CHECK (header->gridBpmTo.load() == doctest::Approx (70.0));

    //  With no clock, the old call: nothing of the clock moves.
    lane.process (block.data(), 2, 64);
    CHECK (header->gridSeq.load() == 4u);
}

TEST_CASE ("proxy: a plugin's playhead reads the tempo, the beat and the bar at its block from the region (namespace draft §123, BEJ)")
{
    VectorRegion region (2, 64, 1);
    region.header->sampleRate.store (48000);
    auto* laid = region.lane (0);
    const plugin::GridPlayHead head (region.header, laid, 48000.0);

    /*  BEFORE THE PARENT WROTE A CLOCK: the clock as it starts, 120 in four,
        playing, the beats counted from the show's start. */
    laid->blockStartSample.store (96000);
    {
        const auto position = head.getPosition();
        REQUIRE (position.hasValue());
        CHECK (*position->getBpm() == doctest::Approx (120.0));
        CHECK (position->getTimeSignature()->numerator == 4);
        CHECK (position->getTimeSignature()->denominator == 4);
        CHECK (*position->getPpqPosition() == doctest::Approx (4.0));
        CHECK (*position->getPpqPositionOfLastBarStart() == doctest::Approx (4.0));
        CHECK (*position->getBarCount() == 1);
        CHECK (*position->getTimeInSamples() == 96000);
        CHECK (*position->getTimeInSeconds() == doctest::Approx (2.0));
        CHECK (position->getIsPlaying());
        CHECK_FALSE (position->getIsLooping());
    }

    //  A CLOCK IN THE REGION, written as the lane writes it: a ramp from 120 to 60 over four seconds, in three.
    plugin::ProxyLane lane;
    region.bind (lane, 0);
    lane.setEnabled (true);
    region.answerResets();

    plugin::BlockClock clock;
    clock.seq = 1;
    clock.state.anchorSample = 48000;
    clock.state.clock.anchorBeat = 8.0;
    clock.state.clock.bpmFrom = 120.0;
    clock.state.clock.bpmTo = 60.0;
    clock.state.clock.rampLength = 4.0;
    clock.state.clock.beatsPerBar = 3;

    Block block (2, 64, 0.0f);
    lane.process (block.data(), 2, 64, &clock, 48000 + 72000);   // a second and a half into the ramp

    const auto position = head.getPosition();
    REQUIRE (position.hasValue());
    const auto& expected = clock.state.clock;
    CHECK (*position->getBpm() == doctest::Approx (expected.bpmAt (1.5)));
    CHECK (*position->getPpqPosition() == doctest::Approx (expected.beatAt (1.5)));
    CHECK (*position->getPpqPositionOfLastBarStart() == doctest::Approx (expected.lastBarStartBeat (expected.beatAt (1.5))));
    CHECK (position->getTimeSignature()->numerator == 3);
    CHECK (*position->getTimeInSamples() == 48000 + 72000);
    CHECK (*position->getBpm() < 120.0);
    CHECK (*position->getBpm() > 60.0);

    //  A count left odd - the parent mid-write - keeps the last whole clock.
    region.header->gridSeq.store (3);
    region.header->gridBpmFrom.store (999.0);
    const auto held = head.getPosition();
    REQUIRE (held.hasValue());
    CHECK (*held->getBpm() == doctest::Approx (expected.bpmAt (1.5)));
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

TEST_CASE ("proxy: an insert switched in is reset first, so nothing it last held is heard")
{
    /*  Found 2026-09-28: an insert out of the chain is not called, so it keeps
        whatever it last held - the tail of the last cue through it on this
        voice, a delay line frozen when it was switched out - and switching it
        in played that first. The reset goes with the switch, on the edge only:
        a lane already in has nothing to clear, and one switched out is not
        called at all. */
    plugin::ProxyLane lane;
    VectorRegion region (2, 64, 1);
    region.bind (lane, 0);

    auto* l = region.lane (0);
    const auto from = l->resetSeq.load();

    lane.setEnabled (true);
    CHECK (l->resetSeq.load() == from + 1);

    lane.setEnabled (true);
    CHECK (l->resetSeq.load() == from + 1);

    lane.setEnabled (false);
    CHECK (l->resetSeq.load() == from + 1);

    lane.setEnabled (true);
    CHECK (l->resetSeq.load() == from + 2);
}

TEST_CASE ("proxy: a request not answered is never written over, and the next block is sent whole once it is")
{
    /*  Namespace draft 139, BHC. The child answers in the audio it was sent.
        A lane that gave up on a block and wrote the next one over it handed
        the plugin its own half-written answer with new input on top, and took
        the result for an answer. Now the audio of a request still owed is the
        child's: the next block waits for it inside its own limit, late if it
        does not come, and sends nothing. */
    constexpr auto fade = plugin::ProxyLane::fadeSamples;

    plugin::ProxyLane lane;
    VectorRegion region (1, 64, 1);
    region.bind (lane, 0);
    lane.setEnabled (true);
    region.answerResets();
    lane.setDeadlineMicroseconds (200);

    auto* shared = region.lane (0);
    auto* audio = plugin::region::audioOf (shared);

    rt::resetCounts();

    const auto play = [&lane] (Block& block)
    {
        const rt::ScopedRealtimeCheck ours { rt::Region::ours };
        lane.process (block.data(), 1, 64);
    };

    //  Sent, and not answered: late, silent, and the audio is the child's now.
    Block first (1, 64, 0.5f);
    play (first);
    CHECK (first.allEqual (0.0f));
    CHECK (shared->requestSeq.load() == 1);
    CHECK (lane.blocks() == 1);
    CHECK (lane.misses() == 1);

    //  The child, still at it: half its answer written.
    std::fill (audio, audio + 32, 0.125f);

    //  THE NEXT BLOCK, and still no answer: late too, nothing sent, not a sample of that audio touched.
    Block second (1, 64, 0.75f);
    play (second);
    CHECK (second.allEqual (0.0f));
    CHECK (shared->requestSeq.load() == 1);
    CHECK (lane.blocks() == 1);
    CHECK (lane.misses() == 2);
    CHECK (lane.consecutiveMisses() == 2);

    for (int n = 0; n < 64; ++n)
        CHECK (audio[n] == doctest::Approx (n < 32 ? 0.125f : 0.5f));

    /*  ANSWERED AT LAST: thrown away - it answers a block long gone - and the
        block in hand sent whole. The stand-in answers every later request at
        once with what it was sent, so what comes back is this block, rising
        from the silence before it, and nothing of the answer that was late. */
    std::fill (audio + 32, audio + 64, 0.125f);
    shared->responseSeq.store (1u << 30);

    Block third (1, 64, 0.25f);
    play (third);
    CHECK (shared->requestSeq.load() == 2);
    CHECK (lane.blocks() == 2);
    CHECK (lane.misses() == 2);
    CHECK (lane.consecutiveMisses() == 0);
    CHECK (third.storage[0] == doctest::Approx (0.25f / static_cast<float> (fade)));
    CHECK (third.storage[63] == doctest::Approx (0.25f));
    CHECK (audio[0] == doctest::Approx (0.25f));

    if (rt::isCounting())
        CHECK (rt::violations() == 0);
}

TEST_CASE ("proxy: a lane whose reset is still to be made is silent, not called, counts nothing and is not settled")
{
    /*  Namespace draft 139, BHC. A reset is not a block's work - a VST3's is
        a whole deactivation - so the child makes it apart from its answering
        and says when it has. Asked to answer through it, every cue's first
        blocks were late and eight failed the plugin. */
    constexpr auto fade = plugin::ProxyLane::fadeSamples;

    plugin::ProxyLane lane;
    VectorRegion region (1, 64, 1);
    region.bind (lane, 0);

    auto* shared = region.lane (0);
    shared->responseSeq.store (1u << 30);       // a child that answers at once, with what it was sent

    lane.setEnabled (true);
    region.answerResets();
    lane.setDeadlineMicroseconds (200);
    CHECK (lane.stateSettled());

    Block in (1, 64, 0.5f);
    lane.process (in.data(), 1, 64);
    CHECK (in.allEqual (0.5f));
    CHECK (lane.blocks() == 1);

    //  A RESET OF AN INSERT THAT IS IN - an arm, a kill: silent until it is made, and nobody asked.
    lane.requestReset();
    CHECK (shared->resetSeq.load() == 2);
    CHECK (shared->resetDoneSeq.load() == 1);
    CHECK_FALSE (lane.stateSettled());

    rt::resetCounts();

    for (int i = 0; i < 20; ++i)
    {
        Block waiting (1, 64, 0.5f);
        {
            const rt::ScopedRealtimeCheck ours { rt::Region::ours };
            lane.process (waiting.data(), 1, 64);
        }
        CHECK (waiting.allEqual (0.0f));
    }

    if (rt::isCounting())
        CHECK (rt::violations() == 0);

    CHECK (lane.blocks() == 1);
    CHECK (lane.misses() == 0);
    CHECK (lane.consecutiveMisses() == 0);
    CHECK (shared->requestSeq.load() == 1);

    //  MADE: called again, the first answer rising from the silence.
    region.answerResets();
    CHECK (lane.stateSettled());

    Block after (1, 64, 0.5f);
    lane.process (after.data(), 1, 64);
    CHECK (lane.blocks() == 2);
    CHECK (after.storage[0] == doctest::Approx (0.5f / static_cast<float> (fade)));
    CHECK (after.storage[63] == doctest::Approx (0.5f));

    /*  THE HOST'S POLL: one owed for longer than a state may load is a child
        that is stuck - timed from the poll that first saw it owed. */
    lane.requestReset();
    CHECK_FALSE (lane.resetOverdue (1000));
    CHECK_FALSE (lane.resetOverdue (1000 + plugin::ProxyLane::stateLoadLimitMs));
    CHECK (lane.resetOverdue (1001 + plugin::ProxyLane::stateLoadLimitMs));

    region.answerResets();
    CHECK_FALSE (lane.resetOverdue (900000));

    lane.requestReset();
    CHECK_FALSE (lane.resetOverdue (2000000));
    CHECK_FALSE (lane.resetOverdue (2000100));
}

TEST_CASE ("proxy: an insert switched in under a sounding cue passes as it did until its reset is made, and is never a hole")
{
    /*  Namespace draft 139, BHC. The one reset that is not silent: the
        switch-in's own. The insert was out, so the cue was heard without it;
        it goes on so for the moment the plugin takes to be ready, and then
        the plugin is heard - a step, as a switch always was, where silence
        would be a dip at every audition of an insert. */
    plugin::ProxyLane lane;
    VectorRegion region (1, 64, 1);
    region.bind (lane, 0);

    auto* shared = region.lane (0);
    shared->responseSeq.store (1u << 30);

    lane.setEnabled (true);
    lane.setDeadlineMicroseconds (200);
    CHECK_FALSE (lane.stateSettled());      // a launch still waits for it

    Block before (1, 64, 0.5f);
    lane.process (before.data(), 1, 64);
    CHECK (before.allEqual (0.5f));         // as it came, and nobody asked
    CHECK (lane.blocks() == 0);
    CHECK (lane.misses() == 0);

    region.answerResets();

    Block ready (1, 64, 0.5f);
    lane.process (ready.data(), 1, 64);
    CHECK (lane.blocks() == 1);
    CHECK (ready.allEqual (0.5f));          // the stand-in's answer, with no fade: there was no silence

    //  Out and in again is the same; a reset asked of it once it is in is silent.
    lane.setEnabled (false);
    lane.setEnabled (true);
    Block again (1, 64, 0.5f);
    lane.process (again.data(), 1, 64);
    CHECK (again.allEqual (0.5f));
    CHECK (lane.blocks() == 1);

    lane.requestReset();
    Block cleared (1, 64, 0.5f);
    lane.process (cleared.data(), 1, 64);
    CHECK (cleared.allEqual (0.0f));
    CHECK (lane.blocks() == 1);
}

TEST_CASE ("proxy: a block answered while a reset was asked for is silent, neither late nor answered")
{
    /*  The child parks a lane to reset it and answers what it is asked
        meanwhile with the audio as it came - so an answer that crosses a
        reset may be the dry block, and is not taken. */
    plugin::ProxyLane lane;
    VectorRegion region (1, 64, 1);
    region.bind (lane, 0);
    lane.setEnabled (true);
    region.answerResets();
    lane.setDeadlineMicroseconds (2000000);

    auto* shared = region.lane (0);

    std::thread child ([&lane, shared]
    {
        while (shared->requestSeq.load() == 0)
            std::this_thread::yield();

        lane.requestReset();
        shared->responseSeq.store (1);
    });

    Block block (1, 64, 0.5f);
    lane.process (block.data(), 1, 64);
    child.join();

    CHECK (block.allEqual (0.0f));
    CHECK (lane.blocks() == 1);
    CHECK (lane.misses() == 0);
    CHECK (lane.answered() == 0);
}

TEST_CASE ("proxy: a slow reset keeps its own lane silent and uncounted while the child's other lane goes on being answered")
{
    /*  Namespace draft 139, BHC, with a child. The reset is made on the
        child's message thread with that lane parked, as a state is loaded -
        so a cue armed through one copy of a plugin no longer stalls the cue
        sounding through another copy in the same child. The test gain's
        state says how long its reset takes. */
    Folder folder;
    plugin::PluginTable table;
    plugin::ProxyLane lanes[2];
    auto spec = testGainSpec (folder, 2, 2, 64);
    spec.deadlineMicroseconds = 200000;
    plugin::ProxyHost host (spec, { &lanes[0], &lanes[1] }, &table);

    std::string problem;
    REQUIRE (host.start (problem));
    REQUIRE (waitForState (host, "loaded", 5000));

    const auto settled = [&host] (plugin::ProxyLane& lane, int milliseconds)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds (milliseconds);

        while (std::chrono::steady_clock::now() < until)
        {
            host.poll();

            if (lane.stateSettled())
                return true;

            std::this_thread::sleep_for (std::chrono::milliseconds (2));
        }

        return lane.stateSettled();
    };

    const auto slow = folder.path.getChildFile ("slow-reset.state");
    REQUIRE (slow.replaceWithText ("gain=0.5\ndie=0\npad=0\nresetDelayMs=400\n", false, false, "\n"));

    lanes[0].setEnabled (true);
    lanes[1].setEnabled (true);
    lanes[1].wantState (slow.getFullPathName().toStdString());
    REQUIRE (settled (lanes[0], 5000));
    REQUIRE (settled (lanes[1], 5000));

    //  Both answer: a half through a gain of a half.
    for (auto& lane : lanes)
    {
        Block warm (2, 64, 0.5f);
        lane.process (warm.data(), 2, 64);
        CHECK (warm.storage[63] == doctest::Approx (0.25f));
    }

    /*  THE SLOW RESET on the second lane, and blocks on both while it runs.
        A block after which the reset is still owed was played with it owed
        throughout, and must be silent; how many there are is the machine's
        business (a sleep of two milliseconds is fifteen on some), but four
        tenths of a second holds more than a handful anywhere. */
    lanes[1].requestReset();
    CHECK_FALSE (lanes[1].stateSettled());

    auto silentBlocks = 0;

    for (int i = 0; i < 100; ++i)
    {
        Block sounding (2, 64, 0.5f), resetting (2, 64, 0.5f);
        lanes[0].process (sounding.data(), 2, 64);
        lanes[1].process (resetting.data(), 2, 64);

        CHECK (sounding.storage[63] == doctest::Approx (0.25f));

        if (! lanes[1].stateSettled())
        {
            CHECK (resetting.allEqual (0.0f));
            ++silentBlocks;
        }

        host.poll();
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }

    CHECK (silentBlocks >= 5);
    CHECK (lanes[0].misses() == 0);
    CHECK (lanes[1].misses() == 0);
    CHECK (host.status().state == "loaded");

    //  Made: the second lane is called and heard again.
    REQUIRE (settled (lanes[1], 5000));
    Block back (2, 64, 0.5f);
    lanes[1].process (back.data(), 2, 64);
    CHECK (back.storage[63] == doctest::Approx (0.25f));
    CHECK (lanes[1].misses() == 0);

    host.stop();
}

TEST_CASE ("proxy: a lane that has played and had silence in and out for ten seconds rests, and sound arriving is played through it at once")
{
    /*  Namespace draft 139, BHF. Nothing gives a voice's copy back when its
        run ends, so every plugin that had ever played was asked for silence a
        block at a time for the rest of the session, one after another on the
        audio thread. A lane now rests once its cue is over and its tail has
        rung out - and only then: one armed and not yet heard is asked every
        block, as a cue at standby always was. */
    plugin::ProxyLane lane;
    VectorRegion region (1, 64, 1);
    region.header->sampleRate.store (48000);
    region.bind (lane, 0);

    auto* shared = region.lane (0);
    shared->responseSeq.store (1u << 30);       // a child that answers at once, with what it was sent

    lane.setEnabled (true);
    region.answerResets();
    lane.setDeadlineMicroseconds (200);

    constexpr std::uint64_t tenSeconds = 480000 / 64;

    const auto play = [&lane] (float value)
    {
        Block block (1, 64, value);
        lane.process (block.data(), 1, 64);
        return block.allEqual (value);
    };

    //  ARMED AND NOT YET HEARD: silence for as long as it lasts, asked every block.
    for (std::uint64_t i = 0; i < tenSeconds + 10; ++i)
        play (0.0f);

    CHECK_FALSE (lane.isResting());
    CHECK (lane.blocks() == tenSeconds + 10);

    //  HEARD, then silence: asked for ten seconds more - its tail - and then no more.
    CHECK (play (0.5f));
    const auto sent = lane.blocks();

    for (std::uint64_t i = 0; i < tenSeconds; ++i)
        play (0.0f);

    CHECK_FALSE (lane.isResting());
    CHECK (lane.blocks() == sent + tenSeconds);

    for (int i = 0; i < 100; ++i)
        CHECK (play (0.0f));

    CHECK (lane.isResting());
    CHECK (lane.blocks() == sent + tenSeconds);
    CHECK (lane.misses() == 0);

    //  SOUND AGAIN: through the plugin in that very block, whole, and awake.
    CHECK (play (0.25f));
    CHECK_FALSE (lane.isResting());
    CHECK (lane.blocks() == sent + tenSeconds + 1);

    //  At rest once more, and THE NEXT CUE'S ARM wakes it: not yet heard, asked every block.
    for (std::uint64_t i = 0; i < tenSeconds + 5; ++i)
        play (0.0f);

    CHECK (lane.isResting());
    const auto before = lane.blocks();

    lane.requestReset();
    region.answerResets();

    for (int i = 0; i < 20; ++i)
        play (0.0f);

    CHECK_FALSE (lane.isResting());
    CHECK (lane.blocks() == before + 20);
}

TEST_CASE ("proxy: the allowance is a share of the block, never under the old rule, and a deadline asked for is itself")
{
    /*  Namespace draft 139, BHB. Until 2026-11-20 the whole rule was the
        smaller of 250 microseconds and a quarter of the block - a gain's
        round trip, whatever the buffer. It is the floor now: what a lane is
        given however late in the callback it is asked. */
    CHECK (plugin::proxyFloorFor (48000, 64) == 250);        // 1333 µs block, a quarter is 333
    CHECK (plugin::proxyFloorFor (48000, 32) == 166);        // 666 µs block, a quarter is 166
    CHECK (plugin::proxyFloorFor (96000, 16) == 41);
    CHECK (plugin::proxyFloorFor (48000, 512) == 250);
    CHECK (plugin::proxyFloorFor (0, 0) == plugin::ProxyLane::defaultDeadlineMicroseconds);

    //  The cap: three tenths of the block period - 3.2 ms at the 512 a Mac opens with.
    const auto about = [] (std::int64_t value, std::int64_t expected) { return value >= expected - 1 && value <= expected; };
    CHECK (about (plugin::proxyDeadlineFor (48000, 512, 0), 3200));
    CHECK (about (plugin::proxyDeadlineFor (48000, 128, 0), 800));
    CHECK (about (plugin::proxyDeadlineFor (48000, 64, 0), 400));
    CHECK (about (plugin::proxyDeadlineFor (96000, 1024, 0), 3200));

    for (const auto rate : { 44100, 48000, 96000, 192000 })
        for (const auto block : { 16, 32, 64, 128, 256, 512, 1024, 2048 })
            CHECK (plugin::proxyDeadlineFor (rate, block, 0) >= plugin::proxyFloorFor (rate, block));

    CHECK (plugin::proxyDeadlineFor (48000, 64, 900) == 900);      // asked for
    CHECK (plugin::proxyDeadlineFor (48000, 512, 900) == 900);
    CHECK (plugin::proxyDeadlineFor (0, 0, 0) == plugin::ProxyLane::defaultDeadlineMicroseconds);
}

TEST_CASE ("proxy: a lane stops at the callback's end but never before its floor, and cut short it is starved, not failed")
{
    /*  Namespace draft 139, BHB. Every wait in one device callback ends a
        share of the block period after the callback began, so plugins that
        cannot keep up are muted and the interface is still answered. A lane
        the callback's end cut short was not late by its own doing - others
        used the time - and is counted apart: muted, said, never failed. */
    plugin::ProxyLane lane;
    VectorRegion region (1, 64, 1);
    region.bind (lane, 0);
    lane.setEnabled (true);
    region.answerResets();

    const auto nanosNow = []
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds> (
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    };

    const auto play = [&lane] (const plugin::BlockClock& clock)
    {
        Block block (1, 64, 0.5f);
        const auto began = std::chrono::steady_clock::now();
        lane.process (block.data(), 1, 64, &clock, 0);
        const auto took = std::chrono::steady_clock::now() - began;
        CHECK (block.allEqual (0.0f));
        return std::chrono::duration_cast<std::chrono::microseconds> (took).count();
    };

    plugin::BlockClock clock;

    //  THE CALLBACK'S TIME ALREADY UP: the lane waits its floor and no more, and is starved.
    lane.setAllowance (2000000, 2000);
    CHECK (lane.deadlineMicroseconds() == 2000000);
    CHECK (lane.floorMicroseconds() == 2000);
    clock.budgetEndNanos = nanosNow() - 1000000;
    CHECK (play (clock) >= 2000);
    CHECK (lane.starved() == 1);
    CHECK (lane.misses() == 0);
    CHECK (lane.consecutiveMisses() == 0);

    //  WITH NO ENTRY SAID - a test driving blocks by hand - the cap alone: late by its own.
    region.answer (0);
    lane.setAllowance (3000, 1000);
    clock.budgetEndNanos = 0;
    CHECK (play (clock) >= 3000);
    CHECK (lane.starved() == 1);
    CHECK (lane.misses() == 1);

    /*  THE CALLBACK'S END AFTER ITS SHARE (namespace draft 149, BJY): the
        lane waits to the callback's end, not to its share - and late there,
        with its whole share behind it, it is late by its own doing. */
    region.answer (0);
    clock.budgetEndNanos = nanosNow() + 12000000;
    CHECK (play (clock) >= 11000);
    CHECK (lane.starved() == 1);
    CHECK (lane.misses() == 2);

    /*  AND AN ANSWER THAT COMES AFTER THE SHARE AND BEFORE THE CALLBACK'S END
        IS TAKEN, where until 149 it was a late block: the test, as the child,
        answers five milliseconds in; the share is three. */
    {
        region.answer (0);
        clock.budgetEndNanos = nanosNow() + 400000000;
        const auto asked = region.lane (0)->requestSeq.load();

        std::thread child ([&region, asked]
        {
            for (int i = 0; i < 20000 && region.lane (0)->requestSeq.load() == asked; ++i)
                std::this_thread::sleep_for (std::chrono::microseconds (50));

            std::this_thread::sleep_for (std::chrono::milliseconds (5));
            plugin::region::audioOf (region.lane (0))[63] = 0.25f;
            region.lane (0)->responseSeq.store (region.lane (0)->requestSeq.load());
        });

        Block block (1, 64, 0.5f);
        lane.process (block.data(), 1, 64, &clock, 0);
        child.join();
        CHECK (block.storage.back() == doctest::Approx (0.25f));
        CHECK (lane.starved() == 1);
        CHECK (lane.misses() == 2);
        CHECK (lane.consecutiveMisses() == 0);

        /*  Late once more, so what follows counts on from where it did -
            falling to silence from where that answer left it (CV), not
            silent throughout as the blocks above were. */
        clock.budgetEndNanos = 0;
        Block lateAgain (1, 64, 0.5f);
        lane.process (lateAgain.data(), 1, 64, &clock, 0);
        CHECK (lateAgain.storage.back() == 0.0f);
        CHECK (lane.misses() == 3);
    }

    //  A LIMIT ASKED FOR IS FIXED: its floor is itself, and no callback's end comes before it.
    region.answer (0);
    lane.setDeadlineMicroseconds (3000);
    CHECK (lane.floorMicroseconds() == 3000);
    clock.budgetEndNanos = nanosNow() - 1000000;
    CHECK (play (clock) >= 3000);
    CHECK (lane.starved() == 1);
    CHECK (lane.misses() == 4);
    CHECK (lane.consecutiveMisses() == 2);
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
    REQUIRE (waitSettled (host, lanes[0]));

    Block first (2, 64, 0.8f);
    Block second (2, 64, 0.8f);

    /*  WARM UP RATHER THAN MEASURE. The first blocks wake the worker from its
        millisecond poll, and a child on a busy box can take longer than even
        the deadline above to answer them - two of them on the Windows runner
        at 7dfeb6c, which a count of every miss since the start then held
        against the child. So it is fed until a block comes back whole, and
        what is measured starts there: every block after it, in time. */
    for (int i = 0; i < 64; ++i)
    {
        std::fill (first.storage.begin(), first.storage.end(), 0.8f);
        lanes[0].process (first.data(), 2, 64);

        if (first.allEqual (0.4f))
            break;
    }

    CHECK (first.allEqual (0.4f));
    CHECK (lanes[0].answered() >= 1);
    const auto missedWarmingUp = lanes[0].misses();

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

    CHECK (lanes[0].misses() == missedWarmingUp);

    host.stop();
    CHECK_FALSE (host.childIsRunning());
    CHECK_FALSE (juce::File (host.regionPath()).existsAsFile());
    CHECK_FALSE (lanes[0].isBound());
    CHECK (table.statusOf ("PG7N0001").state == "unloaded");
}

//==============================================================================
/*  THE TAP (namespace draft §146; the author, 2026-11-23: Likyū's analyser
    showed nothing in its window, from the sidechain or the cue): what a lane
    was given, kept by its child while the parent asks, relayed into the
    window's region and taken by the helper in order. */
TEST_CASE ("proxy: a lane's tap keeps what it was given while the parent asks, wraps its ring, and reaches the window's end in order (§146)")
{
    constexpr int samples = 64;
    VectorRegion region (2, samples, 1);
    auto* lane = region.lane (0);
    auto* audio = plugin::region::audioOf (lane);
    auto* key = plugin::region::keyOf (lane, 2, samples);

    const auto fill = [&] (int b)
    {
        for (int n = 0; n < samples; ++n)
        {
            audio[n] = static_cast<float> (b * 1000 + n);
            audio[samples + n] = -static_cast<float> (b * 1000 + n);
            key[n] = 0.5f + static_cast<float> (b);
        }
    };

    //  NOBODY ASKED: the gate is shut, and the child keeps nothing.
    plugin::region::TapGate gate;
    CHECK_FALSE (gate.open (*lane, samples, 48000));

    //  ASKED: the lane and its ring, and the gate open from the next block.
    plugin::ProxyLane proxy;
    region.bind (proxy, 0);
    const auto view = proxy.askTap();
    REQUIRE (view.lane == lane);
    CHECK (view.ring == plugin::region::tapOf (lane, 2, samples));
    CHECK (gate.open (*lane, samples, 48000));

    //  Seventy blocks wrap the 4096-frame ring; the newest is where the count says.
    for (int b = 0; b < 70; ++b)
    {
        fill (b);
        plugin::region::tapBlock (lane, 2, samples, 2, samples, 1);
    }

    CHECK (lane->tapWritten.load() == 70u * samples);
    CHECK (lane->tapChannels.load() == 2u);
    CHECK (lane->tapKeyChannels.load() == 1u);

    const auto newest = static_cast<std::size_t> ((70 * samples - 1) % plugin::region::tapFrames);
    CHECK (view.ring[newest] == doctest::Approx (69 * 1000 + samples - 1));
    CHECK (view.ring[plugin::region::tapFrames + newest] == doctest::Approx (-(69 * 1000 + samples - 1)));
    CHECK (view.ring[2 * plugin::region::tapFrames + newest] == doctest::Approx (69.5f));

    //  A SECOND OF THE LANE'S AUDIO WITHOUT A LOOK, and it shuts again.
    for (int b = 0; b < 48000 / samples - 1; ++b)
        CHECK (gate.open (*lane, samples, 48000));

    CHECK_FALSE (gate.open (*lane, samples, 48000));

    //  THE WINDOW'S END: the first look starts where the lane stands.
    auto window = std::make_unique<plugin::editor::Region>();
    plugin::editor::TapCursor cursor;
    CHECK (plugin::editor::relayTap (*view.lane, view.ring, cursor, *window) == 0);

    for (int b = 70; b < 73; ++b)
    {
        fill (b);
        plugin::region::tapBlock (lane, 2, samples, 2, samples, 1);
    }

    CHECK (plugin::editor::relayTap (*view.lane, view.ring, cursor, *window) == 3 * samples);
    CHECK (plugin::editor::relayTap (*view.lane, view.ring, cursor, *window) == 0);

    //  And the helper takes them in order, a block at a time.
    std::vector<std::vector<float>> slots (plugin::editor::tapSlots, std::vector<float> (samples));
    std::vector<float*> into;

    for (auto& slot : slots)
        into.push_back (slot.data());

    std::uint64_t read = 0;
    auto taken = 0;

    for (int b = 70; b < 73; ++b)
    {
        int channels = 0, keyChannels = 0;
        REQUIRE (plugin::editor::takeTap (*window, read, into.data(), samples, channels, keyChannels) == samples);
        CHECK (channels == 2);
        CHECK (keyChannels == 1);
        CHECK (slots[0][0] == doctest::Approx (b * 1000));
        CHECK (slots[1][samples - 1] == doctest::Approx (-(b * 1000 + samples - 1)));
        CHECK (slots[2][5] == doctest::Approx (0.5f + static_cast<float> (b)));
        taken += samples;
    }

    int channels = 0, keyChannels = 0;
    CHECK (plugin::editor::takeTap (*window, read, into.data(), samples, channels, keyChannels) == 0);
    CHECK (taken == 3 * samples);

    //  ANOTHER LANE, and the cursor starts over where it stands.
    VectorRegion other (2, samples, 1);
    CHECK (plugin::editor::relayTap (*other.lane (0), plugin::region::tapOf (other.lane (0), 2, samples), cursor, *window) == 0);
    CHECK (cursor.lane == other.lane (0));
}

TEST_CASE ("proxy: the test child keeps what a lane was given, before its gain, only while the parent asks (§146)")
{
    Folder folder;
    plugin::PluginTable table;
    plugin::ProxyLane lanes[1];
    plugin::ProxyHost host (testGainSpec (folder, 1), { &lanes[0] }, &table);

    std::string problem;
    REQUIRE_MESSAGE (host.start (problem), problem);
    REQUIRE (waitForState (host, "loaded", 5000));

    lanes[0].setDeadlineMicroseconds (200000);
    lanes[0].setEnabled (true);
    host.poll();
    REQUIRE (waitSettled (host, lanes[0]));

    Block block (2, 64, 0.8f);

    for (int i = 0; i < 64; ++i)
    {
        std::fill (block.storage.begin(), block.storage.end(), 0.8f);
        lanes[0].process (block.data(), 2, 64);

        if (block.allEqual (0.4f))
            break;
    }

    REQUIRE (block.allEqual (0.4f));

    //  Where the lane keeps it.
    const auto view = lanes[0].askTap();
    REQUIRE (view.lane != nullptr);
    const auto before = view.lane->tapWritten.load();

    //  Asked before each block: every block kept, as it went in, while the cue hears half.
    for (int i = 0; i < 8; ++i)
    {
        lanes[0].askTap();
        std::fill (block.storage.begin(), block.storage.end(), 0.8f);
        lanes[0].process (block.data(), 2, 64);
        CHECK (block.allEqual (0.4f));
    }

    const auto after = view.lane->tapWritten.load();
    CHECK (after >= before + 7u * 64u);
    CHECK (view.ring[static_cast<std::size_t> ((after - 1) % plugin::region::tapFrames)] == doctest::Approx (0.8f));
    CHECK (view.ring[plugin::region::tapFrames + static_cast<std::size_t> ((after - 1) % plugin::region::tapFrames)]
             == doctest::Approx (0.8f));
    CHECK (view.lane->tapChannels.load() == 2u);
    CHECK (view.lane->tapKeyChannels.load() == 0u);

    host.stop();
}

//==============================================================================
#if JUCE_MAC
namespace
{
    /** Polls the host until the child says it is in the workgroup, or not, or the time is up. */
    bool waitForWorkgroup (plugin::ProxyHost& host, std::uint32_t in, int milliseconds)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds (milliseconds);

        for (;;)
        {
            host.poll();

            if (host.header()->inWorkgroup.load() == in)
                return true;

            if (std::chrono::steady_clock::now() > until)
                return false;

            std::this_thread::sleep_for (std::chrono::milliseconds (10));
        }
    }
}
#endif

TEST_CASE ("proxy: the voice child joins the audio workgroup this process shares, follows it, and leaves when nothing is shared")
{
    /*  Process-wide, so whatever an earlier case left shared is taken back first. */
    plugin::shareWorkgroupHandle (nullptr);

    Folder folder;
    plugin::PluginTable table;
    plugin::ProxyLane lanes[1];
    plugin::ProxyHost host (testGainSpec (folder, 1), { &lanes[0] }, &table);

    std::string problem;
    INFO ("start: " << problem);
    REQUIRE (host.start (problem));
    REQUIRE (waitForState (host, "loaded", 5000));
    CHECK (host.header()->inWorkgroup.load() == 0);

   #if JUCE_MAC
    /*  Work intervals of the test's own, which a runner with no sound device
        can make; the child asks once a second, so a few seconds each. */
    auto* first = AudioWorkIntervalCreate ("Go.dot test", OS_CLOCK_MACH_ABSOLUTE_TIME, nullptr);
    auto* second = AudioWorkIntervalCreate ("Go.dot test", OS_CLOCK_MACH_ABSOLUTE_TIME, nullptr);
    REQUIRE (first != nullptr);
    REQUIRE (second != nullptr);

    CHECK (plugin::shareWorkgroupHandle (first));
    CHECK (waitForWorkgroup (host, 1, 5000));

    SUBCASE ("nothing shared: it leaves; another shared: it joins that")
    {
        CHECK_FALSE (plugin::shareWorkgroupHandle (nullptr));
        CHECK (waitForWorkgroup (host, 0, 5000));

        CHECK (plugin::shareWorkgroupHandle (second));
        CHECK (waitForWorkgroup (host, 1, 5000));
    }

    SUBCASE ("a relaunched child joins it again")
    {
        host.killChild();
        REQUIRE (waitForState (host, "failed", 5000));

        REQUIRE (host.restart (problem));
        CHECK (host.header()->inWorkgroup.load() == 0);
        REQUIRE (waitForState (host, "loaded", 5000));
        CHECK (waitForWorkgroup (host, 1, 5000));
    }

    /*  THE DEVICE'S OWN, found among CoreAudio's devices by JUCE's equality -
        on a Mac with an output; a runner without one says so and goes on. */
    SUBCASE ("an output device's workgroup, as the device layer shares it")
    {
        std::unique_ptr<juce::AudioIODeviceType> type (juce::AudioIODeviceType::createAudioIODeviceType_CoreAudio());
        type->scanForDevices();
        const auto names = type->getDeviceNames (false);
        std::unique_ptr<juce::AudioIODevice> device (names.isEmpty() ? nullptr : type->createDevice (names[0], {}));

        if (device != nullptr && device->getWorkgroup())
        {
            CHECK_FALSE (plugin::shareWorkgroupHandle (nullptr));
            CHECK (waitForWorkgroup (host, 0, 5000));

            CHECK (plugin::shareAudioWorkgroup (device->getWorkgroup()));
            CHECK (waitForWorkgroup (host, 1, 5000));
        }
        else
        {
            MESSAGE ("no CoreAudio output with a workgroup here: the device's handle is not looked for");
        }
    }

    plugin::shareWorkgroupHandle (nullptr);
    CHECK (waitForWorkgroup (host, 0, 5000));

    host.stop();
    os_release (first);
    os_release (second);
   #else
    /*  Elsewhere there is nothing to share and nobody is ever in. */
    CHECK_FALSE (plugin::shareWorkgroupHandle (nullptr));
    host.poll();
    CHECK (host.header()->inWorkgroup.load() == 0);
    host.stop();
   #endif
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

TEST_CASE ("proxy: a lane sends the cue's width, takes the widened sides back, and silences both sides when it could not send")
{
    plugin::ProxyLane lane;
    VectorRegion region (2, 64, 1);
    region.bind (lane, 0);
    lane.setEnabled (true);
    region.answerResets();
    lane.setDeadlineMicroseconds (200);

    //  A mono cue into a widening insert: one channel sent, two taken back.
    lane.setShape (1, 2);

    SUBCASE ("with nobody answering, both sides are silent - never the mono side dry")
    {
        Block block (2, 64, 0.0f);
        std::fill (block.storage.begin(), block.storage.begin() + 64, 0.5f);
        lane.process (block.data(), 2, 64);
        CHECK (region.lane (0)->numChannels.load() == 1u);
        CHECK (block.allEqual (0.0f));
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
        CHECK (lane.blocks() == 1);   // late on the first piece: the rest is silent
        CHECK (lane.misses() == 1);
        CHECK (block.allEqual (0.0f));
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
        REQUIRE (waitSettled (host, lanes[0]));

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
        REQUIRE (waitSettled (host, lanes[0]));

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

TEST_CASE ("proxy: a cue's whole state is loaded onto its voice before it may launch, the lane silent meanwhile")
{
    /*  The author's decision of 2026-09-25: a plugin's whole state kept per
        cue and loaded onto the voice at the arm. The test gain's Pad - no
        parameter, only state - is what makes it audible: a quarter of the
        gain. What is pinned: the arm waits (`stateSettled`) until the child
        has loaded it; the lane is SILENT and not called while it loads (CU,
        2026-09-26) and misses nothing; the other voice plays on; the cue's
        values sit on top; the same state
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

    for (auto& lane : lanes)
        REQUIRE (waitSettled (host, lane));

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

    /*  What a block of 0.8 comes back as, once three in a row have come back
        in time, so the rise after silence is past. Fed until then, as the
        first child test's warm-up is (7dfeb6c), because the box a test runs
        on is shared: two late blocks in a row leave the last one silent, and
        a macOS runner read nought for the Pad that way at 1ac7ef2, with the
        state held and nothing wrong. A lane that is not called never answers,
        and gives back the silence it gave. */
    const auto play = [] (plugin::ProxyLane& lane)
    {
        Block block (2, 64, 0.8f);

        for (int i = 0, inTime = 0; i < 64 && inTime < 3; ++i)
        {
            const auto before = lane.answered();
            std::fill (block.storage.begin(), block.storage.end(), 0.8f);
            lane.process (block.data(), 2, 64);
            inTime = lane.answered() > before ? inTime + 1 : 0;
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

    SUBCASE ("a slow state: the voice waits, silent and not called, misses nothing, and the other voice plays on")
    {
        /*  NEVER DRY WHILE IT LOADS (CU): from the moment the state is asked
            for until the parent has read that the child holds it, the lane is
            not called and gives silence. The child's loader parks the lane on
            its own timer all the same; it is never asked to answer it. */
        const auto slow = stateFile ("slow.state", "gain=0.5\ndie=0\npad=1\nloadDelayMs=1500\n");
        lanes[0].wantState (slow);
        host.poll();

        const auto sent = lanes[0].blocks();
        CHECK (play (lanes[0]) == doctest::Approx (0.0f));
        CHECK (lanes[0].blocks() == sent);
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

TEST_CASE ("proxy: a child killed mid-flight leaves the block silent, is marked failed with a sentence, stops being called, and comes back")
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
    REQUIRE (waitSettled (host, lane));

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

    /*  Every block from here is silent and counts a miss, until the host
        looks: eight misses, or the dead child, trip it within one poll. The
        first falls from the last answer; never the dry block (CV). */
    lane.setDeadlineMicroseconds (500);
    const auto sentBefore = lane.blocks();

    for (int i = 0; i < plugin::ProxyLane::missesBeforeFailure; ++i)
    {
        std::fill (block.storage.begin(), block.storage.end(), 0.8f);
        lane.process (block.data(), 2, 64);
        CHECK (block.storage.front() <= 0.4f);
        CHECK (block.storage.back() == doctest::Approx (0.0f));

        if (i > 0)
            CHECK (block.allEqual (0.0f));
    }

    CHECK (lane.consecutiveMisses() == static_cast<std::uint32_t> (plugin::ProxyLane::missesBeforeFailure));

    host.poll();
    CHECK (host.status().state == "failed");
    CHECK (host.status().problem.find ("every voice using it is silent until it is back") != std::string::npos);
    CHECK (table.statusOf ("PG7N0001").state == "failed");
    REQUIRE (failures.size() == 1);
    CHECK (failures[0].rfind ("PG7N0001: ", 0) == 0);

    /*  Not called any more: the request sequence stops moving. ONE was
        sent to the dead child; the seven after it waited for its answer and
        sent nothing, late each of them (namespace draft 139, BHC). */
    const auto sentAfter = lane.blocks();
    CHECK (sentAfter == sentBefore + 1);
    std::fill (block.storage.begin(), block.storage.end(), 0.8f);
    lane.process (block.data(), 2, 64);
    CHECK (lane.blocks() == sentAfter);
    CHECK (block.allEqual (0.0f));
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

TEST_CASE ("proxy: a relaunched child is given the state its voice held, and the voice is silent until it holds it")
{
    /*  The author's decisions of 2026-09-26 (CU): a failed plugin's voices
        are silent, never dry; the relaunch gives each lane back the whole
        state it held, and a voice that was sounding comes back where its cue
        has got to, faded in, once the new child holds it. Not the state that
        was loading when the child died - that may be what killed it - and not
        the old one over a newer one asked for while the plugin was down. */
    Folder folder;
    plugin::PluginTable table;
    plugin::ProxyLane lane;
    plugin::ProxyHost host (testGainSpec (folder, 1), { &lane }, &table);

    std::string problem;
    INFO ("start: " << problem);
    REQUIRE (host.start (problem));
    REQUIRE (waitForState (host, "loaded", 5000));

    lane.setDeadlineMicroseconds (200000);
    lane.setEnabled (true);
    host.poll();
    REQUIRE (waitSettled (host, lane));

    const auto stateFile = [&folder] (const char* name, const char* text)
    {
        const auto file = folder.path.getChildFile (name);
        REQUIRE (file.replaceWithText (text));
        return file.getFullPathName().toStdString();
    };

    const auto settled = [&host, &lane] (int milliseconds)
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

    //  What a block of 0.8 comes back as, a few blocks in so any fade is past.
    const auto play = [&lane]
    {
        Block block (2, 64, 0.8f);

        for (int i = 0; i < 3; ++i)
        {
            std::fill (block.storage.begin(), block.storage.end(), 0.8f);
            lane.process (block.data(), 2, 64);
        }

        return block.storage.front();
    };

    const auto kill = [&host]
    {
        host.killChild();

        for (int waited = 0; waited < 200 && host.childIsRunning(); ++waited)
            std::this_thread::sleep_for (std::chrono::milliseconds (10));

        host.poll();
    };

    //  The cue's state held: the Pad, a quarter of the half.
    lane.wantState (stateFile ("padded.state", "gain=0.5\ndie=0\npad=1\n"));
    REQUIRE (settled (3000));
    REQUIRE (play() == doctest::Approx (0.1f));

    kill();
    REQUIRE (host.status().state == "failed");
    CHECK (play() == doctest::Approx (0.0f));       // down: silent, never the 0.8 it was sent

    SUBCASE ("relaunched, silent until the new child holds the state, then heard with it")
    {
        REQUIRE (waitForState (host, "loaded", plugin::ProxyHost::restartDelayMs + 5000));

        //  Counted before the lane is called: not settled, not called, silent.
        CHECK_FALSE (lane.stateSettled());
        const auto sent = lane.blocks();
        CHECK (play() == doctest::Approx (0.0f));
        CHECK (lane.blocks() == sent);

        REQUIRE (settled (3000));
        CHECK (play() == doctest::Approx (0.1f));   // the Pad is back
        CHECK (table.statusOf ("PG7N0001").stateProblem.empty());
    }

    SUBCASE ("a state asked for while it was down is the one the new child is given")
    {
        //  An arm on the voice while the plugin is down.
        const auto quieter = stateFile ("quieter.state", "gain=0.25\ndie=0\npad=0\n");
        lane.wantState (quieter);

        REQUIRE (waitForState (host, "loaded", plugin::ProxyHost::restartDelayMs + 5000));
        REQUIRE (settled (3000));
        CHECK (lane.wantedState() == quieter);
        CHECK (play() == doctest::Approx (0.2f));   // 0.8 x 0.25: the newer state, not the Pad
    }

    SUBCASE ("the state that was loading when the child died is not given again")
    {
        REQUIRE (waitForState (host, "loaded", plugin::ProxyHost::restartDelayMs + 5000));
        REQUIRE (settled (3000));

        lane.wantState (stateFile ("slow.state", "gain=0.5\ndie=0\npad=0\nloadDelayMs=1500\n"));
        host.poll();                                // sent: the child is loading it
        kill();                                     // the second death inside the minute
        CHECK (host.status().problem.find ("plugin.restart") != std::string::npos);
        CHECK (play() == doctest::Approx (0.0f));

        REQUIRE (host.restart (problem));
        REQUIRE (waitForState (host, "loaded", 5000));
        REQUIRE (settled (3000));
        CHECK (play() == doctest::Approx (0.4f));   // the preset's own: neither the slow one nor the Pad
    }

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
TEST_CASE ("proxy: a spell of late blocks is said once when it begins and once when it ends, never a block at a time")
{
    /*  Namespace draft 139, BHE. The lanes count late blocks one at a time;
        the host tells them twice a spell - at the poll that first sees one,
        and two seconds after the last - with the total as it then stands, in
        the table and to whoever asked. Until this a count nothing read. */
    Folder folder;
    plugin::PluginTable table;
    plugin::ProxyLane lane;
    plugin::ProxyHost host (testGainSpec (folder, 1), { &lane }, &table);

    struct Told { std::string id; std::uint32_t blocks; std::string words; };
    std::vector<Told> told;
    host.onLate ([&told] (const std::string& id, std::uint32_t blocks, const std::string& words)
    {
        told.push_back ({ id, blocks, words });
    });

    std::string problem;
    REQUIRE (host.start (problem));
    REQUIRE (waitForState (host, "loaded", 5000));

    lane.setDeadlineMicroseconds (200000);
    lane.setEnabled (true);
    REQUIRE (waitSettled (host, lane));

    Block block (2, 64, 0.5f);
    const auto play = [&lane, &block]
    {
        std::fill (block.storage.begin(), block.storage.end(), 0.5f);
        lane.process (block.data(), 2, 64);
    };

    //  Keeping up: nothing is said.
    for (int i = 0; i < 10; ++i)
        play();

    host.poll();
    CHECK (told.empty());
    CHECK (table.statusOf ("PG7N0001").lateBlocks == 0);
    CHECK (table.statusOf ("PG7N0001").late.empty());

    /*  A FEW LATE BLOCKS - a limit no process can meet, and fewer than the
        eight in a row that fail a plugin - then in time again. */
    lane.setDeadlineMicroseconds (1);

    while (lane.misses() < 3)
    {
        play();
        std::this_thread::sleep_for (std::chrono::milliseconds (1));     // and the child answers before the next
    }

    lane.setDeadlineMicroseconds (200000);
    const auto late = lane.misses();
    REQUIRE (late < static_cast<std::uint32_t> (plugin::ProxyLane::missesBeforeFailure));

    host.poll();
    REQUIRE (told.size() == 1);
    CHECK (told[0].id == "PG7N0001");
    CHECK (told[0].blocks == late);
    CHECK (told[0].words == plugin::ProxyHost::lateWords);
    CHECK (table.statusOf ("PG7N0001").lateBlocks == late);
    CHECK (table.statusOf ("PG7N0001").late == plugin::ProxyHost::lateWords);
    CHECK (host.status().state == "loaded");

    //  Polled again and again while the spell lasts: said no more.
    for (int i = 0; i < 20; ++i)
    {
        play();
        host.poll();
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }

    CHECK (told.size() == 1);

    //  Two seconds without another: over, said once, the words gone and the count kept.
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds (plugin::ProxyHost::lateSpellEndsAfterMs + 3000);

    while (told.size() < 2 && std::chrono::steady_clock::now() < until)
    {
        play();
        host.poll();
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }

    REQUIRE (told.size() == 2);
    CHECK (told[1].blocks == late);
    CHECK (told[1].words.empty());
    CHECK (table.statusOf ("PG7N0001").lateBlocks == late);
    CHECK (table.statusOf ("PG7N0001").late.empty());
    CHECK (lane.misses() == late);

    host.stop();
}

TEST_CASE ("proxy: the child notes how long a request waited for its worker and how long the work took, and the parent takes the longest of each")
{
    /*  Namespace draft 148. The parent stamps each request with the steady
        clock; the child notes, before it answers, how long the request had
        waited and how long it then took - the longest of each kept on the
        lane until the parent takes them. Driven here with no process: the
        test is the child. */
    using namespace plugin::region;

    plugin::ProxyLane lane;
    CHECK (lane.takeWorst().pickupMicros == 0);     // unbound: nothing to read

    VectorRegion region (2, 64, 1);
    region.bind (lane, 0);
    lane.setEnabled (true);
    region.answerResets();
    lane.setDeadlineMicroseconds (200);

    CHECK (region.lane (0)->requestNanos.load() == 0);

    const auto before = steadyNanos();
    Block block (2, 64, 0.5f);
    lane.process (block.data(), 2, 64);             // nobody answers: late, and the request stands
    const auto asked = region.lane (0)->requestNanos.load();
    CHECK (asked >= before);
    CHECK (asked <= steadyNanos());

    //  Taken 3 ms after it was asked, answered 0.5 ms later, on processor 4.
    noteTimes (*region.lane (0), asked + 3000000, asked + 3500000, 5);
    //  A quicker one after it moves neither.
    noteTimes (*region.lane (0), asked + 100000, asked + 150000, 2);

    auto worst = lane.takeWorst();
    CHECK (worst.pickupMicros == 3000);
    CHECK (worst.workMicros == 500);
    CHECK (worst.core == 5);

    //  Taken: nought again, until the next.
    worst = lane.takeWorst();
    CHECK (worst.pickupMicros == 0);
    CHECK (worst.workMicros == 0);
    CHECK (worst.core == 0);

    //  A longer work alone moves the work and where it was; a clock that ran backwards is nought.
    noteTimes (*region.lane (0), asked + 100000, asked + 900000, 3);
    noteTimes (*region.lane (0), asked - 5000000, asked - 6000000, 9);
    worst = lane.takeWorst();
    CHECK (worst.pickupMicros == 100);
    CHECK (worst.workMicros == 800);
    CHECK (worst.core == 3);
}

TEST_CASE ("proxy: a spell's note is the same words whatever the locale, and says what was cut short only when something was")
{
    //  Namespace draft 148. Put together from whole numbers: no printf, no comma for a point.
    plugin::ProxyHost::LateSpell spell;
    spell.missed = 12;
    spell.seconds = 3.14;
    spell.pickupMicros = 2304;
    spell.workMicros = 52;
    spell.allowedMicros = 799;

    CHECK (plugin::ProxyHost::lateNoteOf ("Likyu", "YK90V10R", spell, {})
             == "plugin Likyu (YK90V10R): 12 late, in 3.1 s; longest wait for its thread 2.30 ms,"
                " longest in the plugin 0.05 ms, against 0.79 ms allowed");

    spell.starved = 2;
    spell.seconds = 0.0;
    spell.pickupMicros = 41;
    spell.workMicros = 12500;
    CHECK (plugin::ProxyHost::lateNoteOf ("Tails", "71MK49R9", spell, "efficiency")
             == "plugin Tails (71MK49R9): 12 late, 2 cut short by the callback's end, in 0.0 s;"
                " longest wait for its thread 0.04 ms, longest in the plugin 12.50 ms, against 0.79 ms allowed;"
                " its thread was on an efficiency core");

    CHECK (plugin::ProxyHost::lateNoteOf ("Tails", "71MK49R9", spell, "performance").find ("on a performance core")
             != std::string::npos);
}

TEST_CASE ("proxy: a spell of late blocks ends with a note that tells a worker kept waiting from a plugin that took the time")
{
    /*  Namespace draft 148. The test gain made late in each of the two ways
        (TestGainState): twenty milliseconds before its worker takes a request,
        or twenty inside its work. The count is the same; the note is not. */
    const auto slowTake = [] (bool take)
    {
        Folder folder;
        plugin::PluginTable table;
        plugin::ProxyLane lane;
        plugin::ProxyHost host (testGainSpec (folder, 1), { &lane }, &table);

        std::vector<std::string> notes;
        host.onLateNote ([&notes] (const std::string& note) { notes.push_back (note); });

        std::string problem;
        REQUIRE (host.start (problem));
        REQUIRE (waitForState (host, "loaded", 5000));

        const auto file = folder.path.getChildFile ("slow.state");
        REQUIRE (file.replaceWithText (juce::String ("gain=0.5\ndie=0\npad=0\n")
                                         + (take ? "takeDelayUs=20000\n" : "workDelayUs=20000\n")));

        lane.setDeadlineMicroseconds (500000);
        lane.setEnabled (true);
        lane.wantState (file.getFullPathName().toStdString());
        REQUIRE (waitSettled (host, lane, 10000));

        Block block (2, 64, 0.5f);
        const auto play = [&lane, &block]
        {
            std::fill (block.storage.begin(), block.storage.end(), 0.5f);
            lane.process (block.data(), 2, 64);
        };

        /*  IN TIME under a wide limit, and noted all the same: what a bench
            reads a row from, late or not - and nought once it has been taken. */
        (void) host.takeWorstSeen();

        for (int i = 0; i < 3; ++i)
            play();

        host.poll();
        const auto seen = host.takeWorstSeen();
        CHECK ((take ? seen.pickupMicros : seen.workMicros) >= 15000u);
        CHECK (host.takeWorstSeen().pickupMicros == 0u);
        CHECK (notes.empty());

        //  LATE, three times: a limit of a millisecond.
        lane.setDeadlineMicroseconds (1000);

        while (lane.misses() < 3)
        {
            play();
            std::this_thread::sleep_for (std::chrono::milliseconds (40));    // and the child answers before the next
        }

        lane.setDeadlineMicroseconds (500000);
        const auto late = lane.misses();
        REQUIRE (late < static_cast<std::uint32_t> (plugin::ProxyLane::missesBeforeFailure));

        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds (plugin::ProxyHost::lateSpellEndsAfterMs + 5000);

        while (notes.empty() && std::chrono::steady_clock::now() < until)
        {
            host.poll();
            std::this_thread::sleep_for (std::chrono::milliseconds (10));
        }

        REQUIRE (notes.size() == 1);
        const auto spell = host.lastLateSpell();
        CHECK (spell.missed == late);
        CHECK (spell.starved == 0u);
        CHECK (spell.allowedMicros > 0);

        if (take)
        {
            CHECK (spell.pickupMicros >= 15000u);
            CHECK (spell.workMicros < 15000u);      // a gain's work is microseconds
        }
        else
        {
            CHECK (spell.workMicros >= 15000u);
        }

        CHECK (notes[0].find ("plugin ") == 0);
        CHECK (notes[0].find ("(PG7N0001): " + std::to_string (late) + " late") != std::string::npos);
        CHECK (notes[0].find ("longest wait for its thread") != std::string::npos);
        CHECK (notes[0].find ("allowed") != std::string::npos);
        CHECK (host.status().state == "loaded");

        host.stop();
    };

    SUBCASE ("its worker kept from the request") { slowTake (true); }
    SUBCASE ("its work too long for the buffer")  { slowTake (false); }
}

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

    /*  NOT KEEPING UP, and again keeping up (namespace draft 139, BHE): the
        count and the words into the table, the rest of it kept - so a replay
        on a machine with no plugin reads as the session did. */
    outcome = dispatch (registry, context, "plugin.late",
                        { osc::Value::string ("PG7N0001"), osc::Value::int32 (3), osc::Value::string ("not keeping up") });
    CHECK (outcome.applied);
    CHECK (table.statusOf ("PG7N0001").lateBlocks == 3);
    CHECK (table.statusOf ("PG7N0001").late == "not keeping up");
    CHECK (table.statusOf ("PG7N0001").state == "failed");
    CHECK (table.statusOf ("PG7N0001").latencySamples == 12);

    outcome = dispatch (registry, context, "plugin.late",
                        { osc::Value::string ("PG7N0001"), osc::Value::int32 (17), osc::Value::string ({}) });
    CHECK (outcome.applied);
    CHECK (table.statusOf ("PG7N0001").lateBlocks == 17);
    CHECK (table.statusOf ("PG7N0001").late.empty());

    outcome = dispatch (registry, context, "plugin.late",
                        { osc::Value::string ("PG7N0009"), osc::Value::int32 (1), osc::Value::string ("nobody") });
    CHECK_FALSE (outcome.applied);
    CHECK (outcome.reason == reason::unknownId);

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

    const auto storage = realPluginStorage();
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
    REQUIRE (waitSettled (host, lanes[0]));

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

/*  A REAL PLUGIN'S SIDECHAIN, KEYED THROUGH THE CHILD (namespace draft 142,
    what §141's close left owed: "one real sidechain compressor through
    WFG_REAL_VST3"). The plugin in WFG_REAL_VST3, put by WFG_REAL_KEYED_STATE -
    a state file as a cue keeps one - where its sidechain turns it down. The
    lane is keyed, so the child lays a sidechain out in its reset; a quiet tone
    plays through it under a silent key, then under a loud one, and the loud
    key must take at least three decibels off it. Unkeyed again, the sidechain
    is given back. Skipped unless asked for with `--no-skip`. */
TEST_CASE ("proxy: a real VST3's sidechain, keyed through the child, turns it down when the key is loud"
           * doctest::skip())
{
    const auto identifier = juce::SystemStats::getEnvironmentVariable ("WFG_REAL_VST3", {}).toStdString();
    const auto stateName = juce::SystemStats::getEnvironmentVariable ("WFG_REAL_KEYED_STATE", {});
    REQUIRE_MESSAGE (! identifier.empty(), "set WFG_REAL_VST3 to an identifier from wfg plugins --list");
    REQUIRE_MESSAGE (stateName.isNotEmpty(), "set WFG_REAL_KEYED_STATE to a state of it that its sidechain turns down");

    const auto statePath = juce::File::getCurrentWorkingDirectory().getChildFile (stateName).getFullPathName().toStdString();
    const auto xml = plugin::describePlugin (realPluginStorage(), identifier);
    REQUIRE_MESSAGE (! xml.empty(), "the scan does not know " << identifier);

    constexpr int block = 256;
    Folder folder;
    plugin::PluginTable table;
    plugin::ProxyLane lanes[1];

    auto spec = testGainSpec (folder, 1, 2, block);
    spec.identifier = identifier;
    spec.name = "real keyed";
    spec.descriptionXml = xml;
    plugin::ProxyHost host (spec, { &lanes[0] }, &table);

    std::string problem;
    REQUIRE (host.start (problem));
    const auto up = waitForState (host, "loaded", 20000);
    INFO ("state " << host.status().state << ": " << host.status().problem);
    REQUIRE (up);

    host.poll();
    MESSAGE (identifier << ": " << host.status().layout << ", a sidechain of " << lanes[0].keyOffered() << " offered");
    REQUIRE (lanes[0].keyOffered() > 0);

    lanes[0].setDeadlineMicroseconds (200000);
    lanes[0].setEnabled (true);
    lanes[0].wantState (statePath);
    lanes[0].setKeyed (true);
    REQUIRE (waitSettled (host, lanes[0], 10000));
    CHECK (table.statusOf (spec.pluginId).stateProblem.empty());
    CHECK (lanes[0].keyHeld() == lanes[0].keyOffered());

    Block audio (2, block, 0.0f);
    std::vector<float> keyLeft (static_cast<std::size_t> (block)), keyRight (static_cast<std::size_t> (block));
    const float* key[2] = { keyLeft.data(), keyRight.data() };
    std::int64_t at = 0;

    //  A second of a quiet 2 kHz tone, the key a tone of its own or silence; the last tenth's level, in dB.
    const auto play = [&] (float keyLevel)
    {
        double sum = 0.0;
        std::size_t count = 0;
        const auto blocks = 48000 / block;

        for (int b = 0; b < blocks; ++b, at += block)
        {
            for (int i = 0; i < block; ++i)
            {
                const auto phase = 2.0 * juce::MathConstants<double>::pi * 2000.0 * static_cast<double> (at + i) / 48000.0;
                audio.storage[static_cast<std::size_t> (i)] = static_cast<float> (0.05 * std::sin (phase));
                audio.storage[static_cast<std::size_t> (block + i)] = audio.storage[static_cast<std::size_t> (i)];
                keyLeft[static_cast<std::size_t> (i)] = keyRight[static_cast<std::size_t> (i)]
                    = static_cast<float> (keyLevel * std::sin (phase * 1.0001));
            }

            lanes[0].process (audio.data(), 2, block, nullptr, at, key, 2);

            if (b >= blocks - blocks / 10)
                for (const auto sample : audio.storage)
                {
                    sum += static_cast<double> (sample) * sample;
                    ++count;
                }
        }

        return 10.0 * std::log10 (std::max (1.0e-20, sum / static_cast<double> (std::max<std::size_t> (1, count))));
    };

    const auto quiet = play (0.0f);
    const auto loud = play (0.5f);
    MESSAGE ("the tone under a silent key " << juce::String (quiet, 1) << " dB, under a loud one " << juce::String (loud, 1) << " dB");
    CHECK (loud < quiet - 3.0);
    CHECK (lanes[0].misses() == 0);

    lanes[0].setKeyed (false);
    REQUIRE (waitSettled (host, lanes[0], 10000));
    CHECK (lanes[0].keyHeld() == 0);

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
    REQUIRE (waitSettled (host, lanes[0]));

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

    /*  A SHORTER BLOCK AND A FULL ONE AFTER IT (namespace draft 142): the
        child fits its scratch to each block's length in place, so a short one
        is processed whole and the full one after it is full again - at the
        plugin's gain of a half, so a sample it did not process would show. */
    std::fill (block.storage.begin(), block.storage.end(), 0.25f);
    lanes[0].process (block.data(), 2, 37);

    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 37; ++i)
            CHECK (block.storage[static_cast<std::size_t> (c * 64 + i)] == doctest::Approx (0.125f));

    blocksAt (0.25f);
    CHECK (block.allEqual (0.125f));

    lanes[0].setParameter (0, 1.0f);
    blocksAt (0.25f);
    CHECK (block.allEqual (0.25f));
    CHECK (lanes[0].misses() == 0);

    host.stop();
}

/*  THE CHILD'S SCRATCH AT A BLOCK'S LENGTH, IN PLACE (namespace draft 142). A
    real plugin used to be handed a view built over the scratch every block,
    and a view of 32 channels or more mallocs its pointer list - through
    std::malloc, which the counting allocator (rt::, operator new only) does
    not see. So what is pinned is that nothing moves: fitted to any length up
    to the one it was made for, the scratch keeps its own pointer list where it
    was and every channel inside the memory it was made with, each holding what
    is written to it. A reallocation could not pass that, whoever's allocator. */
TEST_CASE ("proxy: the child's scratch is fitted to each block's length in place, at any width, with nothing reallocated")
{
    constexpr int width = 40;
    constexpr int longest = 512;

    juce::AudioBuffer<float> scratch;
    scratch.setSize (width, longest, false, true, true);     // as RealHost::create makes it

    const auto* const list = scratch.getArrayOfReadPointers();
    const auto* const first = list[0];
    const auto* const end = list[width - 1] + longest;
    auto held = true;
    auto inside = true;

    for (const auto length : { 512, 128, 37, 1, 511, 512, 64, 512 })
    {
        plugin::fitScratch (scratch, length);
        CHECK (scratch.getNumSamples() == length);
        CHECK (scratch.getNumChannels() == width);
        CHECK (scratch.getArrayOfReadPointers() == list);

        scratch.clear();

        for (int c = 0; c < width; ++c)
        {
            const auto* channel = scratch.getReadPointer (c);
            inside = inside && channel >= first && channel + length <= end;

            for (int i = 0; i < length; ++i)
                scratch.getWritePointer (c)[i] = static_cast<float> (c * 1000 + i);
        }

        for (int c = 0; c < width; ++c)
            for (int i = 0; i < length; ++i)
                held = held && juce::exactlyEqual (scratch.getReadPointer (c)[i], static_cast<float> (c * 1000 + i));
    }

    CHECK (inside);
    CHECK (held);
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
    REQUIRE (waitSettled (host, lanes[0]));

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
    const auto storage = realPluginStorage();

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

                for (auto& lane : lanes)
                    REQUIRE (waitSettled (host, lane));

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
    REQUIRE (waitSettled (host, lane));

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
        the OTHER voices miss: the loading lane is silent and not called, but
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
    const auto storage = realPluginStorage();

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

            /*  THE PARAMETER THE HAND MOVES: the first, or WFG_M35_PARAM's - a
                plugin whose first is a switch (a bypass) is left as it was by
                0.2, so no new state is kept and nothing is captured (namespace
                draft 142). */
            const auto moved = identifier == plugin::Catalogue::testGainIdentifier()
                                 ? 0
                                 : std::clamp (juce::SystemStats::getEnvironmentVariable ("WFG_M35_PARAM", "0").getIntValue(),
                                               0, count - 1);
            MESSAGE (identifier << ": the hand moves parameter " << moved << " of " << count);

            for (const auto value : { 0.2f, 0.8f })
            {
                std::vector<float> values (static_cast<std::size_t> (count), plugin::editor::restsAtPreset);
                helper.setSubject ({ "CUE00001", "FX000001", "M35", {}, false, values, {} });
                ++seq;
                REQUIRE (until ([&] { return helper.subjectTaken() == seq; }, 5000));

                helper.poke (moved, value);
                values[static_cast<std::size_t> (moved)] = value;
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
        REQUIRE (waitSettled (host, lanes[0]));
        REQUIRE (waitSettled (host, lanes[1]));

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

//==============================================================================
namespace
{
    /*  M86's rig: a child with its own folder, table and lanes, so several can
        stand side by side. Declared in the order they must outlive one another. */
    struct AllowanceChild
    {
        Folder folder;
        plugin::PluginTable table;
        std::vector<std::unique_ptr<plugin::ProxyLane>> lanes;
        std::unique_ptr<plugin::ProxyHost> host;
    };

    struct AllowanceReading
    {
        std::vector<double> watched;      // the watched lane's round trips, in microseconds
        std::vector<double> resets;       // the reset lane's first call after each switch-in
        std::uint32_t misses = 0;         // the watched lane's
        std::uint32_t starved = 0;        // its blocks cut short by the callback's end
        int longestRun = 0;               // its late blocks in a row, at most
        int failures = 0;                 // times a child was seen failed
        plugin::ProxyLane::Worst worst;   // its child's longest wait and work in the row, and where (namespace draft 148)
        std::uint32_t firstMisses = 0;    // the lane asked first, when it is not the watched one
        int laterSamples = 0;             // how much later it is played at the row's end, and how often it was given more (149)
        std::uint32_t movedLater = 0;
    };

    /*  WHAT A ROW IS RUN BESIDE (namespace draft 148): a key for every lane's
        sidechain, as a keyed cue sends one; and so many ordinary threads that
        never rest - "another thread wants this core", on demand. */
    struct AllowanceBeside
    {
        bool key = false;
        int busyThreads = 0;
    };

    /*  A SHOW'S BLOCKS, as near as a test comes: one thread in the real-time
        class calls every lane in `order` once a block period, a second of
        noise then a second of silence so a tail is computed too; another
        polls the hosts every ten milliseconds, as the engine does, so the
        worker's spin and the eight-in-a-row rule are the shipped ones. The
        reset lane, if any, is asked for a reset every half second - what an
        arm does to an insert - and how long it takes timed apart. */
    AllowanceReading driveForAllowance (std::vector<std::unique_ptr<AllowanceChild>>& children,
                                        const std::vector<plugin::ProxyLane*>& order,
                                        plugin::ProxyLane* watched, plugin::ProxyLane* resetLane,
                                        int block, double seconds, int channels = 2, AllowanceBeside beside = {})
    {
        AllowanceReading reading;

        /*  THE WATCHED LANE'S CHILD, whose longest wait and work the row
            reads; cleared of whatever its coming up left. */
        plugin::ProxyHost* watchedHost = nullptr;

        for (auto& child : children)
        {
            (void) child->host->takeWorstSeen();

            for (auto& lane : child->lanes)
                if (lane.get() == watched)
                    watchedHost = child->host.get();
        }

        std::atomic<bool> busy { true };
        std::vector<std::thread> busyThreads;

        for (int i = 0; i < beside.busyThreads; ++i)
            busyThreads.emplace_back ([&busy]
            {
                volatile std::uint64_t turns = 0;

                while (busy.load (std::memory_order_relaxed))
                    turns = turns + 1;
            });
        const auto periodUs = static_cast<double> (block) * 1000000.0 / 48000.0;
        const auto blocks = static_cast<int> (seconds * 1000000.0 / periodUs);
        const auto resetEvery = std::max (1, static_cast<int> (500000.0 / periodUs));
        reading.watched.reserve (static_cast<std::size_t> (blocks));
        reading.resets.reserve (static_cast<std::size_t> (blocks / resetEvery + 1));

        std::atomic<bool> running { true };
        std::atomic<int> failures { 0 };

        std::thread poller ([&children, &running, &failures]
        {
            std::vector<char> wasFailed (children.size(), 0);

            while (running.load())
            {
                for (std::size_t i = 0; i < children.size(); ++i)
                {
                    children[i]->host->poll();
                    const char failed = children[i]->host->status().state == "failed" ? 1 : 0;

                    if (failed != 0 && wasFailed[i] == 0)
                        failures.fetch_add (1);

                    wasFailed[i] = failed;
                }

                std::this_thread::sleep_for (std::chrono::milliseconds (10));
            }
        });

        std::thread driver ([&reading, &order, watched, resetLane, block, blocks, resetEvery, periodUs, channels, beside]
        {
            app::takeRealtime (app::RealtimeRank::sound, periodUs / 1000.0, periodUs / 2000.0);

            std::vector<Block> buffers;
            buffers.reserve (order.size());

            for (std::size_t i = 0; i < order.size(); ++i)
                buffers.emplace_back (channels, block, 0.0f);

            //  The key, when the row has one: the first buffer's noise, two channels of it.
            Block keyBlock (2, block, 0.0f);
            const auto firstMissesBefore = order.front()->misses();

            std::uint32_t noise = 0x9e3779b9u;
            const auto missesBefore = watched->misses();
            const auto starvedBefore = watched->starved();
            auto missesSeen = missesBefore;
            auto run = 0;
            auto next = std::chrono::steady_clock::now();
            auto resetting = false;
            auto resetAskedAt = next;

            /*  NO ROW LONGER THAN THREE TIMES ITS LENGTH: with a dozen
                children spinning the lanes' waits alone outlast the block
                period, and the first run of this case took minutes over one
                row. */
            const auto giveUp = next + std::chrono::nanoseconds (static_cast<std::int64_t> (
                                           3.0 * static_cast<double> (blocks) * periodUs * 1000.0));

            for (int b = 0; b < blocks && std::chrono::steady_clock::now() < giveUp; ++b)
            {
                const auto sounding = static_cast<int> (static_cast<double> (b) * periodUs / 1000000.0) % 2 == 0;

                for (auto& buffer : buffers)
                {
                    for (auto& sample : buffer.storage)
                    {
                        noise ^= noise << 13;
                        noise ^= noise >> 17;
                        noise ^= noise << 5;
                        sample = sounding ? (static_cast<float> (noise & 0xffffu) / 65535.0f - 0.5f) * 0.5f : 0.0f;
                    }
                }

                /*  THE CALLBACK'S END, said as the device layer says it
                    (namespace draft 139, BHB): a share of the period from
                    now. A row with a limit asked for ignores it. */
                plugin::BlockClock clock;
                clock.budgetEndNanos = std::chrono::duration_cast<std::chrono::nanoseconds> (
                                           std::chrono::steady_clock::now().time_since_epoch()).count()
                                     + static_cast<std::int64_t> (plugin::ProxyLane::callbackFraction * periodUs * 1000.0);

                for (std::size_t i = 0; i < order.size(); ++i)
                {
                    auto* lane = order[i];

                    /*  THE RESET, as an arm asks it of an insert that is in:
                        timed from the asking to the block that finds it made
                        (a block's length is the grain). Before namespace
                        draft 139 it ran inside the next block's wait. */
                    if (lane == resetLane && b > 0 && b % resetEvery == 0 && ! resetting)
                    {
                        lane->requestReset();
                        resetting = true;
                        resetAskedAt = std::chrono::steady_clock::now();
                    }

                    if (beside.key && i == 0)
                        for (std::size_t n = 0; n < keyBlock.storage.size(); ++n)
                            keyBlock.storage[n] = buffers[0].storage[n % buffers[0].storage.size()];

                    const auto t0 = std::chrono::steady_clock::now();

                    if (beside.key)
                        lane->process (buffers[i].data(), channels, block, &clock, 0, keyBlock.data(), 2);
                    else
                        lane->process (buffers[i].data(), channels, block, &clock, 0);

                    const auto us = std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count();

                    if (lane == resetLane && resetting && lane->stateSettled())
                    {
                        resetting = false;
                        reading.resets.push_back (std::chrono::duration<double, std::micro> (
                                                      std::chrono::steady_clock::now() - resetAskedAt).count());
                    }

                    if (lane == watched && b > 0)
                        reading.watched.push_back (us);
                }

                const auto missesNow = watched->misses();
                run = missesNow != missesSeen ? run + 1 : 0;
                missesSeen = missesNow;
                reading.longestRun = std::max (reading.longestRun, run);

                next += std::chrono::nanoseconds (static_cast<std::int64_t> (periodUs * 1000.0));
                std::this_thread::sleep_until (next);
            }

            reading.misses = missesSeen - missesBefore;
            reading.starved = watched->starved() - starvedBefore;
            reading.firstMisses = order.front() != watched ? order.front()->misses() - firstMissesBefore : 0;
            reading.laterSamples = watched->lateSamples();
            reading.movedLater = watched->movedLater();
        });

        driver.join();
        busy.store (false);

        for (auto& thread : busyThreads)
            thread.join();

        //  One more look, so the last blocks' times are in, then the poller's own stop.
        std::this_thread::sleep_for (std::chrono::milliseconds (30));
        running.store (false);
        poller.join();
        reading.failures = failures.load();

        if (watchedHost != nullptr)
            reading.worst = watchedHost->takeWorstSeen();

        return reading;
    }
}

/*  M86 - what a real plugin needs of a buffer (namespace draft 139, spike 09).
    M31 timed the transport with a gain child at 64 and 256 samples under a
    limit of twenty milliseconds; no case ran a real plugin at the buffer a
    Mac opens with (512) under the limit that ships. Here, for 512 and 128
    samples at 48 kHz, each under a wide limit - what the plugin truly takes -
    and under the shipped one - what is lost:

      a  one lane (and, on a Mac, the same with a workgroup shared);
      b  two lanes on one child, the pool's shape at the start;
      c  two lanes, one of them switched out and in every half second: the
         reset an arm asks for, timed, and the other lane's blocks meanwhile;
      d  the plugin's child beside one, three, seven and eleven spinning ones;
      e  a second real plugin after it in the same round, two inserts on one
         channel (namespace draft 148, when `WFG_M86_SECOND_VST3` names one).

    Each row also says the longest a request waited for the child's worker
    and the longest the worker took over one, and the core it was on.

    `WFG_REAL_VST3=<identifier> wfg_tests --test-case="M86*" --no-skip`, a
    Release build on a quiet machine; `WFG_M86_SECONDS` is each row's length
    (eight by default). The table goes to docs/spikes/spike09-plugin-allowance.md. */
TEST_CASE ("M86: what a real plugin needs of a buffer - under a wide limit and the shipped one, beside a reset and beside spinning children"
           * doctest::skip())
{
    const auto realIdentifier = juce::SystemStats::getEnvironmentVariable ("WFG_REAL_VST3", {}).toStdString();
    const auto seconds = std::max (1.0, juce::SystemStats::getEnvironmentVariable ("WFG_M86_SECONDS", "8").getDoubleValue());
    const auto storage = realPluginStorage();
    const auto gain = plugin::Catalogue::testGainIdentifier();

    /*  THE WIDTH AND THE STATE (namespace draft 142): a lane of WFG_M86_CHANNELS
        channels (two by default), and the real plugin's lanes loaded with the
        state in WFG_REAL_STATE, a file as a cue keeps one - so a plugin is
        measured doing its work rather than at its preset, which for an EQ
        with every band off is a copy. */
    const auto channels = std::clamp (juce::SystemStats::getEnvironmentVariable ("WFG_M86_CHANNELS", "2").getIntValue(), 1, 64);
    const auto realState = juce::SystemStats::getEnvironmentVariable ("WFG_REAL_STATE", {});
    const auto statePath = realState.isEmpty() ? std::string {}
                                               : juce::File::getCurrentWorkingDirectory().getChildFile (realState)
                                                     .getFullPathName().toStdString();

    plugin::shareWorkgroupHandle (nullptr);

    /*  WHAT THE ROWS ARE RUN BESIDE, AND WHICH ARE RUN (namespace draft 148).
        `WFG_M86_KEY=1`: every lane keyed and sent a key. `WFG_M86_LOAD=<n>`:
        n ordinary threads that never rest, beside each row.
        `WFG_M86_SECOND_VST3` (and `WFG_M86_SECOND_STATE`): a second real
        plugin in a child of its own, asked after the first in the same round
        - two inserts on one channel - as phase e, the second watched.
        `WFG_M86_PHASES` (letters, "abcde"), `WFG_M86_BLOCKS` ("512,128"),
        `WFG_M86_LIMITS` ("wide,shipped") and `WFG_M86_GAIN=0` cut the table
        down to the rows wanted, so a row can be long. The child's worker
        takes its class from `WFG_PROXY_WORKER` (PluginHostChild.cpp). */
    const auto envText = [] (const char* name, const char* fallback)
    {
        return juce::SystemStats::getEnvironmentVariable (name, fallback);
    };

    const auto secondIdentifier = envText ("WFG_M86_SECOND_VST3", "").toStdString();
    const auto secondState = envText ("WFG_M86_SECOND_STATE", "");
    const auto secondStatePath = secondState.isEmpty() ? std::string {}
                                                       : juce::File::getCurrentWorkingDirectory().getChildFile (secondState)
                                                             .getFullPathName().toStdString();
    const AllowanceBeside beside { envText ("WFG_M86_KEY", "0").getIntValue() != 0,
                                   std::clamp (envText ("WFG_M86_LOAD", "0").getIntValue(), 0, 256) };
    const auto phases = envText ("WFG_M86_PHASES", "abcde");
    const auto blocksAsked = juce::StringArray::fromTokens (envText ("WFG_M86_BLOCKS", "512,128"), ",", "");
    const auto limitsAsked = juce::StringArray::fromTokens (envText ("WFG_M86_LIMITS", "wide,shipped"), ",", "");
    const auto withGain = envText ("WFG_M86_GAIN", "1").getIntValue() != 0;
    const auto has = [&phases] (char phase) { return phases.containsChar (phase); };

    MESSAGE ("M86 on " << juce::SystemStats::getOperatingSystemName() << ", "
             << juce::SystemStats::getCpuModel() << ", " << juce::SystemStats::getNumCpus() << " cores, "
             << seconds << " s a row, " << channels << " channel(s)"
             << (statePath.empty() ? std::string() : ", the real plugin in " + statePath)
             << (beside.key ? ", keyed" : "") << ", beside " << beside.busyThreads << " busy thread(s), worker "
             << envText ("WFG_PROXY_WORKER", "as shipped"));
    MESSAGE ("plugin | block | limit | phase | children | lanes | n | p50 us | p99 | p99.9 | max | over 250 us | over 0.3 period"
             " | misses | starved | longest run | failed | resets | reset p50 us | reset max | worst wait us | worst work us | core"
             " | the first's misses | samples later | given more | first late blocks");

    auto childIndex = 0;
    std::vector<std::string> saidLayout;

    const auto make = [&childIndex, &saidLayout, &gain, channels, statePath, secondIdentifier, secondStatePath, beside]
                          (const std::string& identifier, const std::string& xml, int laneCount, int block, std::int64_t limitUs)
    {
        const auto& stateOfIt = ! secondIdentifier.empty() && identifier == secondIdentifier ? secondStatePath : statePath;
        auto child = std::make_unique<AllowanceChild>();
        std::vector<plugin::ProxyLane*> pointers;

        for (int i = 0; i < laneCount; ++i)
        {
            child->lanes.push_back (std::make_unique<plugin::ProxyLane>());
            pointers.push_back (child->lanes.back().get());
        }

        auto spec = testGainSpec (child->folder, laneCount, channels, block);
        spec.pluginId = "PG7N" + juce::String (++childIndex).paddedLeft ('0', 4).toStdString();
        spec.identifier = identifier;
        spec.descriptionXml = xml;
        spec.deadlineMicroseconds = limitUs;

        //  As a show's plugins are given (namespace draft 149): a queue, unless `WFG_M86_QUEUE=0` asks for the lane that only waits.
        spec.queueSlots = juce::SystemStats::getEnvironmentVariable ("WFG_M86_QUEUE", "1").getIntValue() != 0
                            ? plugin::region::queueSlotsMost : 0;

        //  And as late from the start as `WFG_M86_LATE` says, in samples: what a build makes of a plugin's measure.
        spec.lateSamples = std::max (0, juce::SystemStats::getEnvironmentVariable ("WFG_M86_LATE", "0").getIntValue());
        child->host = std::make_unique<plugin::ProxyHost> (spec, pointers, &child->table);

        std::string problem;
        REQUIRE_MESSAGE (child->host->start (problem), problem);
        const auto up = waitForState (*child->host, "loaded", 30000);
        INFO (child->host->status().state << ": " << child->host->status().problem);
        REQUIRE (up);

        //  What the ladder found, once a plugin: a cue wider than it passes dry.
        if (std::find (saidLayout.begin(), saidLayout.end(), identifier) == saidLayout.end())
        {
            saidLayout.push_back (identifier);
            MESSAGE (identifier << " on a lane of " << channels << ": " << child->host->status().layout);
        }

        for (auto& lane : child->lanes)
        {
            if (limitUs > 0)
                lane->setDeadlineMicroseconds (limitUs);

            if (beside.key)
                lane->setKeyed (true);

            lane->setEnabled (true);

            if (identifier != gain && ! stateOfIt.empty())
                lane->wantState (stateOfIt);
        }

        for (auto& lane : child->lanes)
            REQUIRE (waitSettled (*child->host, *lane, 10000));

        if (identifier != gain && ! stateOfIt.empty())
        {
            const auto stateProblem = child->table.statusOf (spec.pluginId).stateProblem;
            INFO ("the state " << stateOfIt << ": " << stateProblem);
            REQUIRE (stateProblem.empty());
        }

        return child;
    };

    const auto say = [] (const std::string& name, int block, std::int64_t limitUs, const std::string& phase,
                         std::size_t childCount, std::size_t laneCount, AllowanceReading reading)
    {
        const auto at = [] (std::vector<double>& sorted, double fraction)
        {
            return sorted.empty() ? 0.0
                                  : sorted[std::min (sorted.size() - 1,
                                                     static_cast<std::size_t> (static_cast<double> (sorted.size()) * fraction))];
        };

        const auto share = 0.3 * static_cast<double> (block) * 1000000.0 / 48000.0;
        const auto over = [&reading] (double limit)
        {
            return std::count_if (reading.watched.begin(), reading.watched.end(), [limit] (double us) { return us > limit; });
        };

        const auto over250 = over (250.0);
        const auto overShare = over (share);

        /*  WHERE THE LATE ONES FALL, the first dozen by block: scattered, in
            runs, or at a period says which kind of lateness it is. */
        std::string lateAt;
        auto listed = 0;

        for (std::size_t i = 0; i < reading.watched.size() && listed < 12; ++i)
        {
            if (reading.watched[i] > 250.0)
            {
                lateAt += (listed == 0 ? "" : " ") + std::to_string (i);
                ++listed;
            }
        }
        std::sort (reading.watched.begin(), reading.watched.end());
        std::sort (reading.resets.begin(), reading.resets.end());

        MESSAGE (name << " | " << block << " | " << (limitUs > 0 ? std::to_string (limitUs) : std::string ("shipped")) << " | "
                 << phase << " | " << childCount << " | " << laneCount << " | " << reading.watched.size() << " | "
                 << juce::String (at (reading.watched, 0.5), 1) << " | " << juce::String (at (reading.watched, 0.99), 1) << " | "
                 << juce::String (at (reading.watched, 0.999), 1) << " | "
                 << juce::String (reading.watched.empty() ? 0.0 : reading.watched.back(), 1) << " | "
                 << over250 << " | " << overShare << " | " << reading.misses << " | " << reading.starved << " | "
                 << reading.longestRun << " | "
                 << reading.failures << " | " << reading.resets.size() << " | "
                 << juce::String (at (reading.resets, 0.5), 1) << " | "
                 << juce::String (reading.resets.empty() ? 0.0 : reading.resets.back(), 1) << " | "
                 << reading.worst.pickupMicros << " | " << reading.worst.workMicros << " | "
                 << (reading.worst.core == 0 ? std::string ("-")
                                             : std::to_string (reading.worst.core - 1) + " " + app::coreKindOf (reading.worst.core))
                 << " | " << reading.firstMisses << " | " << reading.laterSamples << " | " << reading.movedLater
                 << " | " << lateAt);
    };

    const auto stopAll = [] (std::vector<std::unique_ptr<AllowanceChild>>& children)
    {
        for (auto& child : children)
            child->host->stop();
    };

    const auto secondXml = secondIdentifier.empty() ? std::string {} : plugin::describePlugin (storage, secondIdentifier);

    if (! secondIdentifier.empty() && secondXml.empty())
        MESSAGE ("the scan does not know " << secondIdentifier << "; no phase e");

    for (const auto& identifier : std::vector<std::string> { gain, realIdentifier })
    {
        if (identifier.empty() || (identifier == gain && ! withGain))
            continue;

        std::string xml;

        if (identifier != gain)
        {
            xml = plugin::describePlugin (storage, identifier);

            if (xml.empty())
            {
                MESSAGE ("the scan does not know " << identifier << "; skipped");
                continue;
            }
        }

        const auto name = identifier == gain ? std::string ("test-gain") : identifier;

        for (const auto block : { 512, 128 })
        {
            if (! blocksAsked.contains (juce::String (block)))
                continue;

            for (const std::int64_t limitUs : { std::int64_t { 20000 }, std::int64_t { 0 } })
            {
                if (! limitsAsked.contains (limitUs > 0 ? "wide" : "shipped"))
                    continue;

                //  a - one lane.
                if (has ('a'))
                {
                    std::vector<std::unique_ptr<AllowanceChild>> children;
                    children.push_back (make (identifier, xml, 1, block, limitUs));
                    auto* lane = children[0]->lanes[0].get();
                    say (name, block, limitUs, "a one lane", 1, 1,
                         driveForAllowance (children, { lane }, lane, nullptr, block, seconds, channels, beside));

                   #if JUCE_MAC
                    /*  THE SAME IN A WORKGROUP, a work interval of the test's
                        own as the case above makes: the child's worker joins
                        it, as it joins the interface's in a show. */
                    auto* interval = AudioWorkIntervalCreate ("Go.dot M86", OS_CLOCK_MACH_ABSOLUTE_TIME, nullptr);

                    if (interval != nullptr && plugin::shareWorkgroupHandle (interval)
                         && waitForWorkgroup (*children[0]->host, 1, 5000))
                        say (name, block, limitUs, "a in a workgroup", 1, 1,
                             driveForAllowance (children, { lane }, lane, nullptr, block, seconds, channels, beside));
                    else
                        MESSAGE ("no workgroup joined here; that row is not taken");

                    plugin::shareWorkgroupHandle (nullptr);

                    if (interval != nullptr)
                        os_release (interval);
                   #endif

                    stopAll (children);
                }

                //  b - two lanes on one child, the second watched.
                if (has ('b'))
                {
                    std::vector<std::unique_ptr<AllowanceChild>> children;
                    children.push_back (make (identifier, xml, 2, block, limitUs));
                    auto* first = children[0]->lanes[0].get();
                    auto* second = children[0]->lanes[1].get();
                    say (name, block, limitUs, "b two lanes", 1, 2,
                         driveForAllowance (children, { first, second }, second, nullptr, block, seconds, channels, beside));
                    stopAll (children);
                }

                /*  c - the reset. The lane switched in is called first, so
                    under the shipped limit the parent has given up on it and
                    asks the watched lane while the worker is still resetting. */
                if (has ('c'))
                {
                    std::vector<std::unique_ptr<AllowanceChild>> children;
                    children.push_back (make (identifier, xml, 2, block, limitUs));
                    auto* sounding = children[0]->lanes[0].get();
                    auto* armed = children[0]->lanes[1].get();
                    say (name, block, limitUs, "c beside a reset", 1, 2,
                         driveForAllowance (children, { armed, sounding }, sounding, armed, block, seconds, channels, beside));
                    stopAll (children);
                }

                /*  e - two inserts on one channel (namespace draft 148): the
                    second plugin, in a child of its own, asked after the
                    first in the same round and watched; how often the first
                    was late is said beside it. */
                if (has ('e') && identifier != gain && ! secondXml.empty())
                {
                    std::vector<std::unique_ptr<AllowanceChild>> children;
                    children.push_back (make (identifier, xml, 1, block, limitUs));
                    children.push_back (make (secondIdentifier, secondXml, 1, block, limitUs));
                    auto* first = children[0]->lanes[0].get();
                    auto* second = children[1]->lanes[0].get();
                    say (name + " then " + secondIdentifier, block, limitUs, "e the second after the first", 2, 2,
                         driveForAllowance (children, { first, second }, second, nullptr, block, seconds, channels, beside));
                    stopAll (children);
                }

                //  d - beside spinning children: how many workers the machine stands.
                if (has ('d') && identifier != gain)
                {
                    for (const auto others : { 1, 3, 7, 11 })
                    {
                        std::vector<std::unique_ptr<AllowanceChild>> children;
                        children.push_back (make (identifier, xml, 1, block, limitUs));
                        std::vector<plugin::ProxyLane*> order { children[0]->lanes[0].get() };

                        for (int i = 0; i < others; ++i)
                        {
                            children.push_back (make (gain, {}, 1, block, limitUs));
                            order.push_back (children.back()->lanes[0].get());
                        }

                        say (name, block, limitUs, "d beside spinning children", children.size(), order.size(),
                             driveForAllowance (children, order, order.front(), nullptr, block, seconds, channels, beside));
                        stopAll (children);
                    }
                }
            }
        }
    }
}

TEST_CASE ("proxy: a plugin added since the graph was built is given a child with nothing rebuilt, and any place takes any plugin")
{
    /*  Namespace draft 133, stage 4. A voice's insert places are no plugin's
        for good: a cue's inserts fill them in its own order, each naming the
        entry it is lent a copy of. And the set is not the graph's shape: the
        runner says which plugins the show plays through (`prepareInserts`),
        one the pool has no child for gets one at once, one that is a plugin
        it has is one more entry that child stands for. */
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
    host.setProxyServices (services);

    //  Built with one plugin and four places a voice: a show's first plugins need no rebuild.
    auto spec = specWithPlugins (2, 1);
    spec.insertPlaces = 4;
    REQUIRE (host.buildEdit (spec));
    CHECK (host.insertPlaces() == 4);
    CHECK (table.builtPlaces() == 4);
    CHECK (host.proxyCount() == 1);
    REQUIRE (host.proxy (0) != nullptr);
    CHECK (host.proxy (1) == nullptr);

    for (int slot = 0; slot < 4; ++slot)
        CHECK (host.proxyLane (0, slot) != nullptr);

    CHECK (host.proxyLane (0, 4) == nullptr);

    const auto poll = [&host] (auto done, int milliseconds = 10000)
    {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds (milliseconds);

        while (std::chrono::steady_clock::now() < end)
        {
            host.pollProxies();

            if (done())
                return true;

            std::this_thread::sleep_for (std::chrono::milliseconds (10));
        }

        return done();
    };

    REQUIRE (poll ([&] { return table.statusOf ("PG7N0001").state == "loaded"; }));

    const auto entryOf = [] (const std::string& id, const std::string& preset)
    {
        audio::PluginSpec entry;
        entry.id = id;
        entry.identifier = plugin::Catalogue::testGainIdentifier();
        entry.name = "Test gain " + id;
        entry.presetPath = preset;
        return entry;
    };

    /*  THE SAME PLUGIN ON ANOTHER CUE, and another kind - told apart here by
        a preset: the first is one more entry the child that is there stands
        for, the second gets a child of its own, now. */
    host.prepareInserts ({ entryOf ("PG7N0001", {}), entryOf ("PG7N0002", {}), entryOf ("PG7N0003", "other.preset") });

    CHECK (host.proxyOfEntry ("PG7N0002") == host.proxy (0));
    CHECK (host.proxy (0)->serves ("PG7N0002"));
    REQUIRE (host.proxy (1) != nullptr);
    CHECK (host.proxyOfEntry ("PG7N0003") == host.proxy (1));
    CHECK (host.pooledCopies (std::string ("PG7N0003")) == 2);
    CHECK (table.built() == std::vector<std::string> { "PG7N0001", "PG7N0002", "PG7N0003" });

    //  Said again, nothing more is made.
    host.prepareInserts ({ entryOf ("PG7N0001", {}), entryOf ("PG7N0002", {}), entryOf ("PG7N0003", "other.preset") });
    CHECK (host.proxy (2) == nullptr);

    REQUIRE (poll ([&] { return table.statusOf ("PG7N0002").state == "loaded"
                                  && table.statusOf ("PG7N0003").state == "loaded"; }));

    /*  A CUE WHOSE CHAIN IS THE THIRD THEN THE FIRST: the places take them in
        that order, each a copy of its own plugin. */
    host.snapTrackFx (0, 0, true, {}, {}, true, "PG7N0003");
    host.snapTrackFx (0, 1, true, {}, {}, true, "PG7N0001");
    CHECK (host.pooledCopiesLent (std::string ("PG7N0003")) == 1);
    CHECK (host.pooledCopiesLent (std::string ("PG7N0001")) == 1);
    auto* const third = host.trackInsertTarget (0, 0);
    auto* const first = host.trackInsertTarget (0, 1);
    REQUIRE (third != nullptr);
    REQUIRE (first != nullptr);
    CHECK (third != first);

    /*  THE NEXT CUE ON THAT VOICE has them the other way round: each place
        gives back the copy of the wrong plugin and is lent one of the right. */
    host.snapTrackFx (0, 0, true, {}, {}, true, "PG7N0001");
    host.snapTrackFx (0, 1, true, {}, {}, true, "PG7N0003");
    CHECK (poll ([&] { return host.isTrackFxSettled (0); }));
    CHECK (host.pooledCopiesLent (std::string ("PG7N0003")) == 1);
    CHECK (host.pooledCopiesLent (std::string ("PG7N0001")) == 1);
    REQUIRE (host.trackInsertTarget (0, 0) != nullptr);
    CHECK (host.trackInsertTarget (0, 0) != third);
    CHECK (host.trackInsertTarget (0, 0) != first);
    CHECK (host.trackInsertTarget (0, 0)->isEnabled());
    REQUIRE (host.trackInsertTarget (0, 1) != nullptr);
    CHECK (host.trackInsertTarget (0, 1) != first);

    /*  A PLUGIN THE POOL HAS NOT BEEN TOLD OF waits - not ready, never dry -
        and is lent its copy once it has. */
    host.snapTrackFx (1, 0, true, {}, {}, true, "PG7N0009");
    CHECK_FALSE (host.isTrackFxSettled (1));
    REQUIRE (host.trackInsertTarget (1, 0) != nullptr);
    CHECK (host.trackInsertTarget (1, 0)->isEnabled());

    host.prepareInserts ({ entryOf ("PG7N0001", {}), entryOf ("PG7N0002", {}), entryOf ("PG7N0003", "other.preset"),
                           entryOf ("PG7N0009", {}) });
    CHECK (poll ([&] { return host.isTrackFxSettled (1); }));
    CHECK (host.pooledCopiesLent (std::string ("PG7N0009")) == 2);       // the first plugin's kind: two voices hold one

    rt::resetCounts();

    for (int i = 0; i < 50; ++i)
        host.processBlock();

    if (rt::isCounting())
        CHECK (rt::violations() == 0);

    host.stop();
}

TEST_CASE ("block fence: what was taken away is free once the block that could be in it has ended")
{
    /*  Namespace draft 133, stage 2. A voice's insert place is pointed at
        another lane while audio runs; the lane it pointed at may be in use
        for the block in flight, and the fence says when it no longer can be. */
    plugin::BlockFence fence;

    SUBCASE ("with no audio it has passed at once")
    {
        CHECK (fence.passed (fence.stamp()));
    }

    SUBCASE ("a change inside a block waits for that block's end")
    {
        fence.begin();
        const auto stamped = fence.stamp();
        CHECK_FALSE (fence.passed (stamped));
        fence.end();
        CHECK (fence.passed (stamped));

        //  And one between two blocks waits for nothing.
        const auto between = fence.stamp();
        CHECK (fence.passed (between));
        fence.begin();
        CHECK (fence.passed (between));
        fence.end();
    }

    SUBCASE ("under a running audio thread, nothing is ever in what the fence called free")
    {
        /*  The audio thread reads the pointer once a block and stays in what
            it found for a while; the other thread swaps the pointer, waits
            for the fence, and then looks at the one it took away. */
        std::atomic<int> inUse[2] { { 0 }, { 0 } };
        std::atomic<int> pointed { 0 };
        std::atomic<bool> stop { false };
        std::atomic<std::uint64_t> blocks { 0 };

        std::thread audio ([&]
        {
            while (! stop.load())
            {
                fence.begin();
                const auto at = pointed.load (std::memory_order_seq_cst);
                inUse[at].store (1, std::memory_order_seq_cst);

                for (int spin = 0; spin < 200; ++spin)
                    (void) blocks.load (std::memory_order_relaxed);

                inUse[at].store (0, std::memory_order_seq_cst);
                fence.end();
                blocks.fetch_add (1, std::memory_order_relaxed);
            }
        });

        auto found = 0;
        auto swaps = 0;
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds (400);

        while (std::chrono::steady_clock::now() < until)
        {
            const auto old = pointed.load();
            pointed.store (1 - old, std::memory_order_seq_cst);
            const auto stamped = fence.stamp();

            while (! fence.passed (stamped))
                std::this_thread::yield();

            found += inUse[old].load (std::memory_order_seq_cst);
            ++swaps;
        }

        stop.store (true);
        audio.join();

        CHECK (found == 0);
        CHECK (swaps > 100);
        CHECK (blocks.load() > 100);
    }
}

TEST_CASE ("proxy: a voice's insert place is pointed at another lane, or at none, while the graph runs")
{
    /*  Namespace draft 133, stage 2, and stage 3's start: every place starts
        on NONE - a voice no cue has armed plays through no plugin - and
        `proxyLane` answers the place's own lane there, so a write lands
        somewhere harmless. The host points a place elsewhere and hands back
        the fence's stamp; a rack channel's place is not a voice's and is
        never re-pointed. */
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
    host.setProxyServices (services);

    REQUIRE (host.buildEdit (specWithPlugins (2, 2)));

    for (int track = 0; track < 2; ++track)
        for (int slot = 0; slot < 2; ++slot)
        {
            REQUIRE (host.proxyLane (track, slot) != nullptr);
            CHECK (host.trackInsertTarget (track, slot) == nullptr);
        }

    //  At its own lane, and back at none. With no block in flight the old lane is free at once.
    auto* const own = host.proxyLane (1, 0);
    auto stamped = host.setTrackInsertTarget (1, 0, own);
    CHECK (host.trackInsertTarget (1, 0) == own);
    CHECK (host.insertFencePassed (stamped));
    stamped = host.setTrackInsertTarget (1, 0, nullptr);
    CHECK (host.trackInsertTarget (1, 0) == nullptr);
    CHECK (host.insertFencePassed (stamped));
    CHECK (host.proxyLane (1, 0) == own);

    //  At a lane that is nobody's place: a pooled copy's, later.
    plugin::ProxyLane lent;
    stamped = host.setTrackInsertTarget (1, 0, &lent);
    CHECK (host.trackInsertTarget (1, 0) == &lent);

    /*  Switched in, and the graph run through it, re-pointed between blocks:
        nothing of ours allocates, whatever the place points at - a lane no
        child answers silences its block, which is the lane's own doing. */
    lent.setEnabled (true);
    rt::resetCounts();

    for (int i = 0; i < 60; ++i)
    {
        host.processBlock();

        if (i % 3 == 0)
            stamped = host.setTrackInsertTarget (1, 0, i % 2 == 0 ? &lent : nullptr);

        CHECK (host.insertFencePassed (stamped));
    }

    if (rt::isCounting())
        CHECK (rt::violations() == 0);

    //  Off it before `lent` goes: a place never outlives what it points at.
    stamped = host.setTrackInsertTarget (1, 0, nullptr);
    CHECK (host.insertFencePassed (stamped));
    CHECK (host.trackInsertTarget (1, 0) == nullptr);

    //  What is not a voice's place changes nothing and answers none.
    stamped = host.setTrackInsertTarget (9, 0, &lent);
    CHECK (host.insertFencePassed (stamped));
    CHECK (host.trackInsertTarget (9, 0) == nullptr);
    CHECK (host.trackInsertTarget (0, 9) == nullptr);
    CHECK (host.trackInsertTarget (-1, 0) == nullptr);

    host.stop();
    CHECK (host.trackInsertTarget (0, 0) == nullptr);
}

TEST_CASE ("proxy: the set's plugins are pooled - one child a plugin, a few copies, its ids stay unique, and stop removes every region")
{
    /*  Namespace draft 133, stage 3. It was one proxy per set entry on every
        voice (decision AE): three entries on sixteen voices, forty-eight
        instances. Entries that are the same plugin with the same preset are
        one kind now, with one child and two copies to begin with, whatever
        the voices - one alone for a single voice. */
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

        //  Three entries, one plugin: one child, which stands for all three.
        REQUIRE (host.proxy (0) != nullptr);
        CHECK (host.proxy (1) == nullptr);
        CHECK (juce::File (host.proxy (0)->regionPath()).existsAsFile());

        for (int slot = 0; slot < 3; ++slot)
        {
            CHECK (host.proxyOfSlot (slot) == host.proxy (0));
            CHECK (host.proxy (0)->serves ("PG7N000" + std::to_string (slot + 1)));
            CHECK (host.pooledCopies (slot) == std::min (tracks, 2));
            CHECK (host.pooledCopiesLent (slot) == 0);
        }

        CHECK (host.proxyOfSlot (3) == nullptr);
        CHECK (host.pooledCopies (3) == 0);
    }

    /*  The child up, and every entry it stands for reads loaded. */
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds (10);

    for (;;)
    {
        host.pollProxies();

        auto loaded = 0;

        for (int slot = 0; slot < 3; ++slot)
            if (table.statusOf ("PG7N000" + std::to_string (slot + 1)).state == "loaded")
                ++loaded;

        if (loaded == 3 || std::chrono::steady_clock::now() > until)
        {
            CHECK (loaded == 3);
            break;
        }

        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }

    /*  The tick thread's setters reach a lane whether or not a cue holds the
        place - its own, where nothing is lent; a block through a graph with
        nothing switched in allocates nothing of ours. */
    host.setTrackFxEnabled (3, 1, true);
    host.setTrackFxParameter (3, 1, 0, 0.25f);
    CHECK (host.proxyLane (3, 1) != nullptr);
    CHECK (host.proxyLane (3, 1)->isEnabled());
    CHECK (host.proxyLane (3, 1)->parameter (0) == doctest::Approx (0.25f));
    CHECK (host.proxyLane (99, 1) == nullptr);
    CHECK (host.proxyLane (3, 9) == nullptr);
    host.setTrackFxEnabled (3, 1, false);

    rt::resetCounts();

    for (int i = 0; i < 50; ++i)
        host.processBlock();

    if (rt::isCounting())
        CHECK (rt::violations() == 0);

    const auto region = host.proxy (0)->regionPath();
    host.stop();
    CHECK_FALSE (juce::File (region).existsAsFile());
    CHECK (failures == 0);
}

TEST_CASE ("proxy: the lane playing an insert is found by its Fx for the plugin's window, lent or waited for, and lost when given back (§146)")
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
    host.setProxyServices (services);

    REQUIRE (host.buildEdit (specWithPlugins (4, 1)));

    const auto poll = [&host] (auto done, int milliseconds = 10000)
    {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds (milliseconds);

        while (std::chrono::steady_clock::now() < end)
        {
            host.pollProxies();

            if (done())
                return true;

            std::this_thread::sleep_for (std::chrono::milliseconds (10));
        }

        return done();
    };

    REQUIRE (poll ([&] { return table.statusOf ("PG7N0001").state == "loaded"; }));

    //  Nobody holds it yet.
    CHECK (host.tapFor ("FXTAP001").lane == nullptr);

    //  An arm lends a copy, and the copy is the one found.
    host.snapTrackFx (0, 0, true, {}, {}, true, {}, "FXTAP001");
    auto* const lent = host.trackInsertTarget (0, 0);
    REQUIRE (lent != nullptr);
    CHECK (lent->owner() == "FXTAP001");
    REQUIRE (poll ([&] { return host.isTrackFxSettled (0); }));

    const auto first = host.tapFor ("FXTAP001");
    REQUIRE (first.lane != nullptr);
    CHECK (first.lane->tapPulse.load() >= 1u);
    CHECK (host.tapFor ("FXOTHER1").lane == nullptr);

    //  A claim that waits for its copy is labelled once lent.
    host.snapTrackFx (1, 0, true, {}, {}, true, {}, "FXTAP002");
    host.snapTrackFx (2, 0, true, {}, {}, true, {}, "FXTAP003");
    REQUIRE (poll ([&] { return host.isTrackFxSettled (2); }));
    CHECK (host.trackInsertTarget (2, 0)->owner() == "FXTAP003");
    CHECK (host.tapFor ("FXTAP003").lane != nullptr);
    CHECK (host.tapFor ("FXTAP003").lane != host.tapFor ("FXTAP001").lane);

    //  Given back, it is nobody's.
    host.snapTrackFx (0, 0, false, {}, {}, false);
    CHECK (lent->owner().empty());
    CHECK (host.tapFor ("FXTAP001").lane == nullptr);

    host.stop();
}

TEST_CASE ("proxy: a voice is lent a copy for the cue that has the insert, gives it back, and one more is made when the last is taken")
{
    /*  Namespace draft 133, stage 3. `snapTrackFx` is the arm: held, the
        voice's place is lent a free copy, which takes the cue's switch and
        values; not held, the place gives its copy back. When the last free
        copy goes another is made in a child of its own - one to spare - and
        a cue armed while none is free waits for it, its arm not ready, and
        is lent it when it comes. */
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
    host.setProxyServices (services);

    REQUIRE (host.buildEdit (specWithPlugins (4, 1)));
    REQUIRE (host.pooledCopies (0) == 2);

    const auto poll = [&host] (auto done, int milliseconds = 10000)
    {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds (milliseconds);

        while (std::chrono::steady_clock::now() < end)
        {
            host.pollProxies();

            if (done())
                return true;

            std::this_thread::sleep_for (std::chrono::milliseconds (10));
        }

        return done();
    };

    REQUIRE (poll ([&] { return table.statusOf ("PG7N0001").state == "loaded"; }));

    //  THE FIRST CUE: lent a copy, switched in, with its value.
    host.snapTrackFx (0, 0, true, { { 0, 0.25f } }, {}, true);
    auto* const first = host.trackInsertTarget (0, 0);
    REQUIRE (first != nullptr);
    CHECK (first == host.proxyLane (0, 0));
    CHECK (first->isEnabled());
    CHECK (first->parameter (0) == doctest::Approx (0.25f));
    CHECK (host.pooledCopiesLent (0) == 1);
    CHECK (host.pooledCopies (0) == 2);
    CHECK (poll ([&] { return host.isTrackFxSettled (0); }));

    //  The same voice armed again keeps the copy it has.
    host.snapTrackFx (0, 0, false, {}, {}, true);
    CHECK (host.trackInsertTarget (0, 0) == first);
    CHECK_FALSE (first->isEnabled());
    CHECK (host.pooledCopiesLent (0) == 1);

    //  A SECOND VOICE takes the last free copy, and one more is made to spare.
    host.snapTrackFx (1, 0, true, {}, {}, true);
    auto* const second = host.trackInsertTarget (1, 0);
    REQUIRE (second != nullptr);
    CHECK (second != first);
    CHECK (host.pooledCopiesLent (0) == 2);
    CHECK (host.pooledCopies (0) == 3);
    REQUIRE (host.proxy (1) != nullptr);
    CHECK (host.proxy (1)->serves ("PG7N0001"));
    CHECK (host.proxy (1)->regionPath() != host.proxy (0)->regionPath());

    /*  A THIRD, AT ONCE: the spare's child is still coming up, so the cue
        waits - silent through its place's own lane, its arm not ready -
        and is lent the copy when it is there. */
    host.snapTrackFx (2, 0, true, { { 0, 0.5f } }, {}, true);
    CHECK_FALSE (host.isTrackFxSettled (2));
    CHECK (host.pooledCopiesLent (0) == 2);
    REQUIRE (host.trackInsertTarget (2, 0) != nullptr);
    CHECK (host.trackInsertTarget (2, 0)->isEnabled());

    REQUIRE (poll ([&] { return host.isTrackFxSettled (2); }));
    auto* const third = host.trackInsertTarget (2, 0);
    CHECK (third != first);
    CHECK (third != second);
    CHECK (third->isEnabled());
    CHECK (third->parameter (0) == doctest::Approx (0.5f));
    CHECK (host.pooledCopiesLent (0) == 3);
    CHECK (host.pooledCopies (0) == 4);

    //  A CUE WITHOUT THE INSERT on the first voice: the place gives its copy back.
    host.snapTrackFx (0, 0, false, {}, {}, false);
    CHECK (host.trackInsertTarget (0, 0) == nullptr);
    CHECK_FALSE (first->isEnabled());
    CHECK (host.pooledCopiesLent (0) == 2);

    //  And the last voice finds one free: nothing more is made than every voice could hold.
    host.snapTrackFx (3, 0, true, {}, {}, true);
    CHECK (poll ([&] { return host.isTrackFxSettled (3); }));
    CHECK (host.trackInsertTarget (3, 0) != nullptr);
    CHECK (host.pooledCopiesLent (0) == 3);
    CHECK (host.pooledCopies (0) == 4);

    //  The graph runs through it all with nothing of ours allocating.
    rt::resetCounts();

    for (int i = 0; i < 50; ++i)
        host.processBlock();

    if (rt::isCounting())
        CHECK (rt::violations() == 0);

    host.stop();
    CHECK (host.trackInsertTarget (1, 0) == nullptr);
    CHECK (host.pooledCopies (0) == 0);
}

//==============================================================================
namespace
{
    /*  The last sample of each output channel of every block: enough to read a
        constant input's level once the gates have settled. The audio thread
        writes it, so fixed storage and nothing allocated. */
    struct LastSamples final : audio::BlockSink
    {
        std::array<std::atomic<float>, 2> last {};

        void blockProduced (const float* const* channels, int numChannels, int numSamples) noexcept override
        {
            for (int channel = 0; channel < std::min (numChannels, 2); ++channel)
                last[static_cast<std::size_t> (channel)].store (channels[channel][numSamples - 1],
                                                                std::memory_order_relaxed);
        }
    };
}

TEST_CASE ("rack: each channel is a track after the voices, its chain one child per distinct plugin, and its input comes out through it")
{
    /*  Phase 9b (namespace draft 18.4 and 18.6, decisions CH, CK and CL). Two
        rack channels, each with the test-gain plugin in its chain: two tracks
        after the one voice, which is still the polyphony; ONE child for the
        two entries, a lane each, both entries loaded; and each channel's input
        heard at its own output once its gate opens - through its plugin where
        that is switched in, dry where it is not - SILENT, never dry, where it
        is switched in and the child is killed (CU), and gone again on a shut. */
    ScopedStorage storage;
    audio::AudioHost host { storage.path() };

    audio::HostSettings settings;
    settings.sampleRate = 48000;
    settings.blockSize = 64;
    settings.outputChannels = 2;
    settings.inputChannels = 2;
    REQUIRE (host.start (settings));

    plugin::PluginTable table;
    audio::ProxyServices services;
    services.table = &table;
    services.launch = launchOfThisBinary();
    int failures = 0;
    services.onFailed = [&failures] (const std::string&, const std::string&) { ++failures; };
    host.setProxyServices (services);

    audio::EditSpec spec;
    spec.tracks = 1;
    spec.channelsPerTrack = 2;
    spec.proxyDeadlineMicroseconds = 200000;

    for (const auto& [channelId, name, pluginId] : { std::tuple<const char*, const char*, const char*> { "CH000001", "Vox 1", "PG7N0011" },
                                                     std::tuple<const char*, const char*, const char*> { "CH000002", "Vox 2", "PG7N0012" } })
    {
        audio::RackChannelSpec channel;
        channel.id = channelId;
        channel.name = name;

        audio::PluginSpec entry;
        entry.id = pluginId;
        entry.identifier = plugin::Catalogue::testGainIdentifier();
        entry.name = "Test gain";
        channel.plugins.push_back (entry);

        spec.rack.push_back (channel);
    }

    REQUIRE (host.buildEdit (spec));

    CHECK (host.trackCount() == 1);
    CHECK (host.allTrackCount() == 3);
    CHECK (host.rackTrackOf ("CH000001") == 1);
    CHECK (host.rackTrackOf ("CH000002") == 2);
    CHECK (host.rackTrackOf ("NQNQNQNQ") == -1);
    CHECK (host.inspectNodeIds().ok());
    CHECK (table.builtRackOf ("CH000001") == std::vector<std::string> { "PG7N0011" });

    /*  One child stands for both entries of the one plugin. */
    REQUIRE (host.proxy (0) != nullptr);
    CHECK (host.proxy (0)->serves ("PG7N0011"));
    CHECK (host.proxy (0)->serves ("PG7N0012"));
    CHECK (host.proxy (1) == nullptr);

    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds (10);

    while (! (table.statusOf ("PG7N0011").state == "loaded" && table.statusOf ("PG7N0012").state == "loaded")
             && std::chrono::steady_clock::now() < until)
    {
        host.pollProxies();
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }

    REQUIRE (table.statusOf ("PG7N0011").state == "loaded");
    REQUIRE (table.statusOf ("PG7N0012").state == "loaded");

    /*  Each channel takes an input of its own and sends it to an output of its
        own; the first channel's plugin switched in, the second's left out. */
    for (int rack = 1; rack <= 2; ++rack)
    {
        auto* matrix = host.trackMatrix (rack);
        REQUIRE (matrix != nullptr);
        matrix->setLevelDb (0.0f);
        matrix->setGain (0, rack - 1, 1.0f);
        matrix->snapToTargets();
        host.setRackSource (rack, rack - 1, 1);
        host.setTrackFxShape (rack, 0, 1, 1);
    }

    host.setTrackFxEnabled (1, 0, true);

    //  And its insert ready, as a launch waits for (namespace draft 139, BHC).
    for (int waited = 0; waited < 500; ++waited)
    {
        host.pollProxies();

        if (host.isTrackFxSettled (1))
            break;

        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }

    REQUIRE (host.isTrackFxSettled (1));

    LastSamples sink;
    host.setBlockSink (&sink);

    const std::vector<float> one (64, 0.8f), two (64, 0.6f);
    const float* inputs[] { one.data(), two.data() };

    /*  SHUT: a channel no cue has opened is silent. */
    for (int i = 0; i < 4; ++i)
        host.processBlock (inputs, 2);

    CHECK (sink.last[0].load() == doctest::Approx (0.0f));
    CHECK (sink.last[1].load() == doctest::Approx (0.0f));

    /*  OPEN, past the ramp: the first through its plugin at half, the second
        dry. */
    host.openRackGate (1, -1);
    host.openRackGate (2, -1);

    for (int i = 0; i < 20; ++i)
        host.processBlock (inputs, 2);

    CHECK (sink.last[0].load() == doctest::Approx (0.4f).epsilon (0.001));
    CHECK (sink.last[1].load() == doctest::Approx (0.6f).epsilon (0.001));
    CHECK (host.isRackPassing (1));

    /*  THE CHILD KILLED, through the graph (the author's decision of
        2026-09-26, CU): the channel that has the plugin in is silent - never
        its dry 0.8 - and the one that has it switched out plays on. The graph
        is where it matters: the Tracktion plugin around the lane once returned
        before the lane was asked, and a failed plugin's block went out dry. */
    host.proxy (0)->killChild();

    for (int waited = 0; waited < 200 && host.proxy (0)->childIsRunning(); ++waited)
        std::this_thread::sleep_for (std::chrono::milliseconds (10));

    host.pollProxies();
    REQUIRE (table.statusOf ("PG7N0011").state == "failed");
    CHECK (table.statusOf ("PG7N0011").problem.find ("Vox 1 and Vox 2 are silent until it is back") != std::string::npos);

    for (int i = 0; i < 20; ++i)
        host.processBlock (inputs, 2);

    CHECK (sink.last[0].load() == doctest::Approx (0.0f));
    CHECK (sink.last[1].load() == doctest::Approx (0.6f).epsilon (0.001));

    /*  A fast shut on the first: silent inside a block or two, the second
        untouched. */
    host.killRack (1);

    for (int i = 0; i < 4; ++i)
        host.processBlock (inputs, 2);

    CHECK (sink.last[0].load() == doctest::Approx (0.0f));
    CHECK (sink.last[1].load() == doctest::Approx (0.6f).epsilon (0.001));
    CHECK_FALSE (host.isRackPassing (1));
    CHECK (host.isRackPassing (2));

    host.setBlockSink (nullptr);
    host.stop();
    CHECK (failures == 2);      // the one child, heard of by both entries it stands for
}

TEST_CASE ("rack: a gate opens over its fade-in, a stop rings until the output is quiet, and a kill leaves nothing")
{
    /*  Phase 9b (namespace draft 18.5, decisions CG and CN), at the host and
        with no plugin on the channel, so no child is needed. The fade-in is
        equal power: half way through it the gain is the sine of an eighth of
        a turn, 0.707, not a half. A stop is heard as sounding - the run is not
        over - until what reaches the output stage has been quiet for a
        quarter of a second. A kill is silent inside a block and leaves nothing
        ringing. */
    ScopedStorage storage;
    audio::AudioHost host { storage.path() };

    audio::HostSettings settings;
    settings.sampleRate = 48000;
    settings.blockSize = 64;
    settings.outputChannels = 2;
    settings.inputChannels = 2;
    REQUIRE (host.start (settings));

    audio::EditSpec spec;
    spec.tracks = 1;
    spec.channelsPerTrack = 2;

    audio::RackChannelSpec channel;
    channel.id = "CH000001";
    channel.name = "Vox 1";
    spec.rack.push_back (channel);

    REQUIRE (host.buildEdit (spec));

    const auto rack = host.rackTrackOf ("CH000001");
    REQUIRE (rack == 1);
    CHECK (host.isRackTrack (rack));
    CHECK_FALSE (host.isRackTrack (0));
    CHECK_FALSE (host.isRackSounding (rack));

    auto* matrix = host.trackMatrix (rack);
    REQUIRE (matrix != nullptr);
    matrix->setLevelDb (0.0f);
    matrix->setGain (0, 0, 1.0f);
    matrix->snapToTargets();
    host.setRackSource (rack, 0, 1);

    LastSamples sink;
    host.setBlockSink (&sink);

    const std::vector<float> steady (64, 0.5f), silent (64, 0.0f);
    const float* inputs[] { steady.data(), silent.data() };

    const auto run = [&host, &inputs] (double seconds)
    {
        const auto blocks = static_cast<int> (std::lround (seconds * 48000.0 / 64.0));

        for (int i = 0; i < blocks; ++i)
            host.processBlock (inputs, 2);
    };

    /*  THE FADE-IN, over a fifth of a second: equal power at its middle. */
    host.openRackGate (rack, -1, 0.2);
    run (0.1);
    CHECK (sink.last[0].load() == doctest::Approx (0.5 * std::sin (juce::MathConstants<double>::pi / 4.0)).epsilon (0.02));

    run (0.15);
    CHECK (sink.last[0].load() == doctest::Approx (0.5f).epsilon (0.001));
    CHECK (host.isRackSounding (rack));

    /*  A STOP: the input shut in five milliseconds, and the channel still
        sounding while the output stage has been quiet for less than a quarter
        of a second - the tail a reverb would be ringing - and not after. */
    host.shutRackGate (rack);
    run (0.1);
    CHECK (sink.last[0].load() == doctest::Approx (0.0f));
    CHECK (host.isRackSounding (rack));

    run (0.2);
    CHECK_FALSE (host.isRackSounding (rack));

    /*  A KILL: open again at once, then killed - silent inside two blocks, and
        not sounding at all, with no quarter of a second to wait out. */
    host.openRackGate (rack, -1);
    run (0.05);
    CHECK (sink.last[0].load() == doctest::Approx (0.5f).epsilon (0.001));

    host.killRack (rack);
    host.processBlock (inputs, 2);
    host.processBlock (inputs, 2);
    CHECK (sink.last[0].load() == doctest::Approx (0.0f));
    CHECK_FALSE (host.isRackSounding (rack));

    host.setBlockSink (nullptr);
    host.stop();
}

TEST_CASE ("kill: a voice's kill empties its EQ and inserts and no other track's, a stop empties none, and double Esc's sweep silences every voice and empties every EQ not sounding, but resets no voice's insert")
{
    /*  H3 (namespace draft §23.6), at the host and with no child. Every lane
        is bound to a region in a vector, so a reset reads as that lane's own
        count; and every track's EQ is driven by hand - a Q-10 peak at a
        hundred hertz rung up with its own frequency, then one block of silence
        through it, which gives back the ring when nothing cleared it and
        nothing at all when something did. The graph is run twice to bring its
        plugins up, and never again.

        A VOICE'S INSERTS ARE RESET BY A KILL THAT FINDS THE VOICE STILL
        HEARD, AND BY NOTHING ELSE HERE (GE). One child serves a plugin's lane
        on every voice and runs their resets one after another - a VST3's is a
        whole deactivation - so a burst of them in one pass leaves every lane
        it serves waiting past its deadline, block after block, until the
        plugin is failed for it. A voice the sweep or an earlier fade has
        already taken to silence has nothing of its inserts to be heard; its
        next arm resets them before anything plays through them again. */
    VectorRegion region (2, 64, 5);                     // before the host: it outlives every binding

    ScopedStorage storage;
    audio::AudioHost host { storage.path() };

    audio::HostSettings settings;
    settings.sampleRate = 48000;
    settings.blockSize = 64;
    settings.outputChannels = 2;
    settings.inputChannels = 2;
    REQUIRE (host.start (settings));

    //  Two voices with two inserts each, and a rack channel with one: tracks 0, 1 and 2.
    auto spec = specWithPlugins (2, 2);

    audio::RackChannelSpec channel;
    channel.id = "CH000001";
    channel.name = "Verb";

    audio::PluginSpec entry;
    entry.id = "PG7N0021";
    entry.identifier = plugin::Catalogue::testGainIdentifier();
    entry.name = "Test gain";
    channel.plugins.push_back (entry);
    spec.rack.push_back (channel);

    REQUIRE (host.buildEdit (spec));

    const auto rack = host.rackTrackOf ("CH000001");
    REQUIRE (rack == 2);

    host.processBlock();
    host.processBlock();

    /*  A CUE ON EACH VOICE WITH BOTH INSERTS, so each place holds a copy
        (namespace draft 133, stage 3): a voice's inserts are the copies it
        was lent, and one no cue has armed has none to empty. */
    for (int track = 0; track < 2; ++track)
        for (int slot = 0; slot < 2; ++slot)
            host.snapTrackFx (track, slot, false, {}, {}, true);

    const std::array<std::pair<int, int>, 5> slots { { { 0, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { rack, 0 } } };

    for (std::size_t i = 0; i < slots.size(); ++i)
    {
        auto* lane = host.proxyLane (slots[i].first, slots[i].second);
        REQUIRE (lane != nullptr);
        region.bind (*lane, static_cast<int> (i));
    }

    const auto resets = [&region]
    {
        std::array<std::uint32_t, 5> out {};

        for (std::size_t i = 0; i < out.size(); ++i)
            out[i] = region.lane (static_cast<int> (i))->resetSeq.load();

        return out;
    };

    //  Every track at a level of its own, so silence can be told from it.
    for (int track = 0; track <= rack; ++track)
    {
        REQUIRE (host.trackMatrix (track) != nullptr);
        host.trackMatrix (track)->setLevelDb (-6.0f);
    }

    audio::EqSettings boost;
    boost.band[0].freq = 100.0f;
    boost.band[0].gain = 12.0f;
    boost.band[0].q = 10.0f;

    for (int track = 0; track <= rack; ++track)
        host.setTrackEq (track, boost);

    //  A tenth of a second of the band's own frequency through a track's EQ.
    const auto ringUp = [&host] (int track)
    {
        auto* eq = host.trackEq (track);
        REQUIRE (eq != nullptr);

        std::array<float, 64> left {}, right {};
        float* channels[] { left.data(), right.data() };
        auto loudest = 0.0f;

        for (int block = 0; block < 75; ++block)
        {
            for (std::size_t n = 0; n < left.size(); ++n)
            {
                const auto at = static_cast<double> (block) * 64.0 + static_cast<double> (n);
                left[n] = right[n] = 0.25f * static_cast<float> (std::sin (juce::MathConstants<double>::twoPi * 100.0 * at / 48000.0));
            }

            eq->process (channels, 2, 64);

            for (const auto sample : left)
                loudest = std::max (loudest, std::abs (sample));
        }

        REQUIRE (loudest > 0.5f);                       // twelve decibels up: the EQ is in
    };

    //  One block of silence through it: the ring, or nothing.
    const auto ringOf = [&host] (int track)
    {
        std::array<float, 64> left {}, right {};
        float* channels[] { left.data(), right.data() };
        host.trackEq (track)->process (channels, 2, 64);

        auto loudest = 0.0f;

        for (const auto sample : left)
            loudest = std::max (loudest, std::abs (sample));

        return loudest;
    };

    Engine engine;
    audio::HostPlayer player { host, engine };

    const auto from = resets();

    //  A STOP empties nothing: the inserts, the EQ and the level as they were.
    ringUp (0);
    player.stop (0);
    CHECK (resets() == from);
    CHECK (ringOf (0) > 0.1f);
    CHECK (host.trackMatrix (0)->levelDb() == doctest::Approx (-6.0f));

    //  A VOICE'S KILL empties that voice, and nothing else.
    ringUp (0);
    ringUp (1);
    player.kill (0);
    CHECK (resets() == std::array<std::uint32_t, 5> { from[0] + 1, from[1] + 1, from[2], from[3], from[4] });

    const auto killedRing = ringOf (0);
    INFO ("the killed voice's EQ gives back " << killedRing);
    CHECK (juce::exactlyEqual (killedRing, 0.0f));
    CHECK (ringOf (1) > 0.1f);
    CHECK (host.trackMatrix (0)->levelDb() == doctest::Approx (audio::CueMatrix::silenceDb));
    CHECK (host.trackMatrix (1)->levelDb() == doctest::Approx (-6.0f));

    //  killTrack is a voice's: a rack channel's track, and no track, answer false and touch nothing.
    CHECK_FALSE (host.killTrack (rack));
    CHECK_FALSE (host.killTrack (9));
    CHECK_FALSE (host.killTrack (-1));
    CHECK (resets()[4] == from[4]);
    CHECK (host.trackMatrix (rack)->levelDb() == doctest::Approx (-6.0f));

    //  A RACK CHANNEL'S KILL resets its plugins, as it did, and now empties its EQ as well.
    ringUp (rack);
    player.kill (rack);
    CHECK (resets()[4] == from[4] + 1);

    const auto rackRing = ringOf (rack);
    INFO ("the killed rack channel's EQ gives back " << rackRing);
    CHECK (juce::exactlyEqual (rackRing, 0.0f));
    CHECK (host.trackMatrix (rack)->levelDb() == doctest::Approx (audio::CueMatrix::silenceDb));

    /*  DOUBLE ESC'S SWEEP: every voice silenced and every EQ emptied, held by a
        run or not, and every rack channel's inserts reset - but no voice's
        inserts, which the voice's own silence covers until its next arm resets
        them (GE); nothing on the voice the press leaves armed for the next GO,
        which keeps what its arm gave it; and no rack channel's level moved. */
    host.trackMatrix (0)->setLevelDb (-6.0f);
    host.trackMatrix (rack)->setLevelDb (-6.0f);
    const auto swept = resets();

    for (int track = 0; track <= rack; ++track)
        ringUp (track);

    player.resetEffects ({ 1 });
    CHECK (resets() == std::array<std::uint32_t, 5> { swept[0], swept[1], swept[2], swept[3], swept[4] + 1 });

    for (const int track : { 0, rack })
    {
        const auto ring = ringOf (track);
        INFO ("track " << track << " gives back " << ring);
        CHECK (juce::exactlyEqual (ring, 0.0f));
    }

    CHECK (ringOf (1) > 0.1f);                                                  // left ready
    CHECK (host.trackMatrix (0)->levelDb() == doctest::Approx (audio::CueMatrix::silenceDb));
    CHECK (host.trackMatrix (1)->levelDb() == doctest::Approx (-6.0f));
    CHECK (host.trackMatrix (rack)->levelDb() == doctest::Approx (-6.0f));    // a rack channel's level is left

    /*  AND THE KILL THAT FOLLOWS, on a voice the sweep has already silenced:
        the stop, the EQ emptied - and no insert reset, there being nothing of
        them left to hear (GE). */
    const auto killedAfter = resets();
    ringUp (0);
    player.kill (0);
    CHECK (resets() == killedAfter);
    CHECK (juce::exactlyEqual (ringOf (0), 0.0f));
    CHECK (host.trackMatrix (0)->levelDb() == doctest::Approx (audio::CueMatrix::silenceDb));

    //  Nothing left ready: that voice goes with the rest, its inserts left to its next arm.
    const auto again = resets();
    ringUp (1);
    player.resetEffects ({});
    CHECK (resets() == std::array<std::uint32_t, 5> { again[0], again[1], again[2], again[3], again[4] + 1 });
    CHECK (juce::exactlyEqual (ringOf (1), 0.0f));
    CHECK (host.trackMatrix (1)->levelDb() == doctest::Approx (audio::CueMatrix::silenceDb));
    CHECK (host.trackMatrix (rack)->levelDb() == doctest::Approx (-6.0f));

    /*  WHAT STILL SOUNDS IS LEFT TO ITS OWN KILL: a rack channel whose gate is
        open keeps its EQ and its plugins through the sweep - emptied under
        the sound they would click - and its kill, a tick later, empties them. */
    host.openRackGate (rack, -1);
    REQUIRE (host.isRackPassing (rack));
    ringUp (rack);

    const auto open = resets();
    player.resetEffects ({});
    CHECK (resets()[4] == open[4]);
    CHECK (ringOf (rack) > 0.1f);

    player.kill (rack);
    CHECK (resets()[4] == open[4] + 1);
    CHECK (juce::exactlyEqual (ringOf (rack), 0.0f));

    for (const auto& [track, slot] : slots)
        host.proxyLane (track, slot)->unbind();

    host.stop();
}

TEST_CASE ("rack: a gap in the blocks brings the input back over the de-click ramp, and never starts a fade-in again")
{
    /*  The mic driver on macOS CI (a6952dd): a voice that went silent while
        Tracktion muted blocks under a late plugin came back over its cue's
        whole half-second fade-in - the gate saw the gap, started again from
        nought, and ramped over the last length it had been given. The fade-in
        is the cue's entrance, made once: a gap brings the input back over
        five milliseconds of its own, and the gate's ramp goes on from where it
        was - through a fade-in under way, and after one that has arrived. A
        block skipped is made here as the gate sees one: Go.dot's count moved
        on with no block run through the graph. A fresh open after a shut
        takes its own fade-in again. */
    ScopedStorage storage;
    audio::AudioHost host { storage.path() };

    audio::HostSettings settings;
    settings.sampleRate = 48000;
    settings.blockSize = 64;
    settings.outputChannels = 2;
    settings.inputChannels = 2;
    REQUIRE (host.start (settings));

    audio::EditSpec spec;
    spec.tracks = 1;
    spec.channelsPerTrack = 2;

    audio::RackChannelSpec channel;
    channel.id = "CH000001";
    channel.name = "Vox 1";
    spec.rack.push_back (channel);
    REQUIRE (host.buildEdit (spec));

    const auto rack = host.rackTrackOf ("CH000001");
    REQUIRE (rack == 1);

    auto* matrix = host.trackMatrix (rack);
    REQUIRE (matrix != nullptr);
    matrix->setLevelDb (0.0f);
    matrix->setGain (0, 0, 1.0f);
    matrix->snapToTargets();
    host.setRackSource (rack, 0, 1);

    LastSamples sink;
    host.setBlockSink (&sink);

    const std::vector<float> steady (64, 0.5f), silent (64, 0.0f);
    const float* inputs[] { steady.data(), silent.data() };

    const auto run = [&host, &inputs] (double seconds)
    {
        const auto blocks = static_cast<int> (std::lround (seconds * 48000.0 / 64.0));

        for (int i = 0; i < blocks; ++i)
            host.processBlock (inputs, 2);
    };

    /*  A BLOCK SKIPPED, as Tracktion skips one: the count moves on, no block
        runs. The host's clock is the audio side's own counter, reached here as
        a test reaches nothing else. */
    const auto skipBlock = [&host]
    {
        const_cast<AudioClockSource&> (static_cast<const AudioClockSource&> (host.clock())).advance (64);
    };

    //  A fifth of a second's fade-in - 9600 samples - half made: equal power at its middle.
    host.openRackGate (rack, -1, 0.2);
    run (0.1);
    REQUIRE (sink.last[0].load() == doctest::Approx (0.5 * std::sin (juce::MathConstants<double>::pi / 4.0)).epsilon (0.01));

    /*  A GAP HALFWAY: the fade goes on from where it was once the input is
        back, 4800 + 64 + 960 samples along - not from nought again, which
        would read 0.083 here. */
    skipBlock();
    host.processBlock (inputs, 2);
    run (0.02);
    CHECK (sink.last[0].load() == doctest::Approx (0.5 * std::sin (juce::MathConstants<double>::halfPi * 5824.0 / 9600.0))
                                      .epsilon (0.01));

    //  ARRIVED, and a gap then: back at once, over five milliseconds.
    run (0.2);
    REQUIRE (sink.last[0].load() == doctest::Approx (0.5f).epsilon (0.001));
    skipBlock();
    host.processBlock (inputs, 2);
    run (0.01);
    CHECK (sink.last[0].load() == doctest::Approx (0.5f).epsilon (0.001));

    //  A FRESH OPEN after a shut is an entrance again: equal power at its middle.
    host.shutRackGate (rack);
    run (0.1);
    REQUIRE (sink.last[0].load() == doctest::Approx (0.0f));
    host.openRackGate (rack, -1, 0.2);
    run (0.1);
    CHECK (sink.last[0].load() == doctest::Approx (0.5 * std::sin (juce::MathConstants<double>::pi / 4.0)).epsilon (0.02));

    host.setBlockSink (nullptr);
    host.stop();
}

TEST_CASE ("sampling: a pad on a voice plays a slice of the live take through the voice's own output, and is done at its out (§105)")
{
    /*  Namespace draft §105, AZR: a voice given a sampling channel's take
        plays the slice at the head of its own chain - here to the second
        output, the channel looping on the first - from its launch's sample,
        once, and is no longer playing at its out. */
    ScopedStorage storage;
    audio::AudioHost host { storage.path() };

    audio::HostSettings settings;
    settings.sampleRate = 48000;
    settings.blockSize = 64;
    settings.outputChannels = 2;
    settings.inputChannels = 2;
    REQUIRE (host.start (settings));

    audio::EditSpec spec;
    spec.tracks = 1;
    spec.channelsPerTrack = 2;

    audio::RackChannelSpec channel;
    channel.id = "SM000002";
    channel.name = "Pads";
    channel.takeSeconds = 1.0;
    channel.layers = 1;
    spec.rack.push_back (channel);
    REQUIRE (host.buildEdit (spec));
    CHECK (host.inspectNodeIds().ok());

    const auto rack = host.rackTrackOf ("SM000002");
    auto take = host.takeOf ("SM000002");
    REQUIRE (take != nullptr);

    //  THE CHANNEL: its input to the first output, its gate open - a mic cue's arm and launch.
    {
        auto* matrix = host.trackMatrix (rack);
        REQUIRE (matrix != nullptr);
        matrix->setLevelDb (0.0f);
        matrix->setGain (0, 0, 1.0f);
        matrix->snapToTargets();
        host.setRackSource (rack, 0, 1);
        host.openRackGate (rack, -1);
    }

    LastSamples sink;
    host.setBlockSink (&sink);

    const std::vector<float> steady (64, 0.5f), silent (64, 0.0f);
    const float* input[] { steady.data(), silent.data() };
    const float* quiet[] { silent.data(), silent.data() };
    const auto run = [&host] (const float* const* inputs, int blocks)
    {
        for (int i = 0; i < blocks; ++i)
            host.processBlock (inputs, 2);
    };

    //  A tenth of a second and more of a steady half, recorded and closed.
    REQUIRE (take->post ({ audio::Looper::Verb::record, -1, 0, 0 }));
    run (input, 100);
    REQUIRE (take->post ({ audio::Looper::Verb::loop, -1, 0, 0 }));
    run (quiet, 20);
    REQUIRE (take->state() == audio::TakeState::looping);

    //  THE VOICE: to the second output only, given the slice - and refused a channel with no take.
    {
        auto* matrix = host.trackMatrix (0);
        REQUIRE (matrix != nullptr);
        matrix->setLevelDb (0.0f);
        matrix->setGain (0, 1, 1.0f);
        matrix->snapToTargets();
    }

    CHECK_FALSE (host.setTrackTake (0, "NQNQNQNQ", 0.0, 0.05, 1));
    CHECK_FALSE (host.isTakeTrack (0));
    REQUIRE (host.setTrackTake (0, "SM000002", 0.0, 0.05, 1));
    CHECK (host.isTakeTrack (0));
    CHECK_FALSE (host.isTrackPlaying (0));

    //  Nothing on the second output before the launch's sample; the take's half once it has risen.
    run (quiet, 5);
    CHECK (sink.last[1].load() == doctest::Approx (0.0f));

    REQUIRE (host.launchTrackTake (0, host.clock().samplesElapsed() + 640));
    CHECK (host.isTrackPlaying (0));
    run (quiet, 5);
    CHECK (sink.last[1].load() == doctest::Approx (0.0f));
    run (quiet, 10);
    CHECK (sink.last[1].load() == doctest::Approx (0.5f).epsilon (0.001));

    //  A twentieth of a second is under forty blocks: then silence, and done.
    run (quiet, 40);
    CHECK (sink.last[1].load() == doctest::Approx (0.0f));
    CHECK_FALSE (host.isTrackPlaying (0));

    host.setBlockSink (nullptr);
    host.stop();
}

TEST_CASE ("sampling: a take recorded through the plugin before the recorder loops through the plugin after, and a rebuild keeps it")
{
    /*  Phase 9c, stage 9c.2 (namespace draft 19.2, decision BZ). A rack
        channel with a recorder of a second and two test-gain plugins, the first
        BEFORE the recorder and the second AFTER the player. A steady 0.8 at
        its input is recorded at a half - the first plugin printed into the
        take - and loops at a quarter through the second. The second switched
        out is heard at once, since it is after the player; the first switched
        out changes nothing, since the take already has it. The graph built
        again with the same shape keeps the take and plays it on; built with a
        longer take it empties it and says why; built with no recorder it lets
        it go. */
    ScopedStorage storage;
    audio::AudioHost host { storage.path() };

    audio::HostSettings settings;
    settings.sampleRate = 48000;
    settings.blockSize = 64;
    settings.outputChannels = 2;
    settings.inputChannels = 2;
    REQUIRE (host.start (settings));

    plugin::PluginTable table;
    audio::ProxyServices services;
    services.table = &table;
    services.launch = launchOfThisBinary();
    host.setProxyServices (services);

    audio::EditSpec spec;
    spec.tracks = 1;
    spec.channelsPerTrack = 2;
    spec.proxyDeadlineMicroseconds = 200000;

    audio::RackChannelSpec channel;
    channel.id = "SM000001";
    channel.name = "Loops";
    channel.takeSeconds = 1.0;
    channel.layers = 2;

    for (const auto& [pluginId, before] : { std::pair<const char*, bool> { "SM000011", true },
                                            std::pair<const char*, bool> { "SM000012", false } })
    {
        audio::PluginSpec entry;
        entry.id = pluginId;
        entry.identifier = plugin::Catalogue::testGainIdentifier();
        entry.name = "Test gain";
        entry.beforeRecorder = before;
        channel.plugins.push_back (entry);
    }

    spec.rack.push_back (channel);
    REQUIRE (host.buildEdit (spec));
    CHECK (host.inspectNodeIds().ok());

    auto take = host.takeOf ("SM000001");
    REQUIRE (take != nullptr);
    CHECK (host.takeOfTrack (host.rackTrackOf ("SM000001")) == take);
    CHECK (take->capacity() == 48000);
    CHECK (table.builtTakeOf ("SM000001").bytes == take->bytes());
    CHECK (table.builtTakeOf ("SM000001").layers == 2);
    CHECK (host.takeOf ("NQNQNQNQ") == nullptr);

    const auto loaded = [&host, &table]
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds (10);

        while (! (table.statusOf ("SM000011").state == "loaded" && table.statusOf ("SM000012").state == "loaded")
                 && std::chrono::steady_clock::now() < until)
        {
            host.pollProxies();
            std::this_thread::sleep_for (std::chrono::milliseconds (10));
        }

        return table.statusOf ("SM000011").state == "loaded" && table.statusOf ("SM000012").state == "loaded";
    };

    const auto rack = host.rackTrackOf ("SM000001");
    REQUIRE (rack == 1);

    //  The input to the first output, both plugins in, the gate open - what a mic cue's arm and launch do.
    const auto openChannel = [&host, rack]
    {
        auto* matrix = host.trackMatrix (rack);
        REQUIRE (matrix != nullptr);
        matrix->setLevelDb (0.0f);
        matrix->setGain (0, 0, 1.0f);
        matrix->snapToTargets();
        host.setRackSource (rack, 0, 1);

        for (int slot = 0; slot < 2; ++slot)
        {
            host.setTrackFxShape (rack, slot, 1, 1);
            host.setTrackFxEnabled (rack, slot, true);
        }

        /*  And its inserts ready, as a launch waits for (namespace draft 139,
            BHC): until an insert's reset is made it passes the block as it
            came. */
        for (int waited = 0; waited < 500; ++waited)
        {
            host.pollProxies();

            if (host.isTrackFxSettled (rack))
                break;

            std::this_thread::sleep_for (std::chrono::milliseconds (10));
        }

        REQUIRE (host.isTrackFxSettled (rack));
        host.openRackGate (rack, -1);
    };

    REQUIRE (loaded());
    openChannel();

    LastSamples sink;
    host.setBlockSink (&sink);

    const std::vector<float> steady (64, 0.8f), silent (64, 0.0f);
    const float* input[] { steady.data(), silent.data() };
    const float* quiet[] { silent.data(), silent.data() };
    const auto run = [&host] (const float* const* inputs, int blocks)
    {
        for (int i = 0; i < blocks; ++i)
            host.processBlock (inputs, 2);
    };

    //  Open and answering, and silent: the take is empty and `through` is off.
    run (input, 20);
    CHECK (sink.last[0].load() == doctest::Approx (0.0f));

    //  Rec, a quarter of a second of the steady input, Rec: the take closes and loops.
    REQUIRE (take->post ({ audio::Looper::Verb::record, -1, 0, 0 }));
    run (input, 188);
    REQUIRE (take->post ({ audio::Looper::Verb::loop, -1, 0, 0 }));
    run (quiet, 40);

    CHECK (take->state() == audio::TakeState::looping);
    CHECK (take->length() == 188 * 64);
    CHECK (sink.last[0].load() == doctest::Approx (0.2f).epsilon (0.001));    // a half printed, a half heard

    //  The plugin after the player out: heard at once - the loop at the half it was recorded at.
    host.setTrackFxEnabled (rack, 1, false);
    run (quiet, 20);
    CHECK (sink.last[0].load() == doctest::Approx (0.4f).epsilon (0.001));

    //  The plugin before the recorder out: nothing moves - the take already has it.
    host.setTrackFxEnabled (rack, 0, false);
    run (quiet, 20);
    CHECK (sink.last[0].load() == doctest::Approx (0.4f).epsilon (0.001));

    //  BUILT AGAIN WITH THE SAME SHAPE - Load now: the same take, still looping, through the new graph.
    host.setBlockSink (nullptr);
    REQUIRE (host.buildEdit (spec));
    CHECK (host.takeOf ("SM000001") == take);
    CHECK (take->length() == 188 * 64);
    CHECK (take->state() == audio::TakeState::looping);
    CHECK (table.builtTakeOf ("SM000001").problem.empty());

    REQUIRE (loaded());
    openChannel();
    host.setBlockSink (&sink);
    run (quiet, 40);
    CHECK (sink.last[0].load() == doctest::Approx (0.2f).epsilon (0.001));

    //  A longer take asked for: set aside afresh, which empties it, and said.
    host.setBlockSink (nullptr);
    spec.rack[0].takeSeconds = 2.0;
    REQUIRE (host.buildEdit (spec));

    auto longer = host.takeOf ("SM000001");
    REQUIRE (longer != nullptr);
    CHECK (longer->capacity() == 96000);
    CHECK (longer->state() == audio::TakeState::empty);
    CHECK (table.builtTakeOf ("SM000001").problem == "the take was cleared: its longest take or its layers changed");

    /*  A day with every layer (namespace draft §105, AZX): far past half of
        any computer this runs on, so nothing is set aside, the channel plays
        as one with no recorder, and the graph is built all the same - as the
        show asks, so it does not read as differing - saying why. */
    spec.rack[0].takeSeconds = 86400.0;
    spec.rack[0].layers = 16;
    REQUIRE (host.buildEdit (spec));
    CHECK (host.takeOf ("SM000001") == nullptr);
    CHECK (table.builtTakeOf ("SM000001").bytes == 0);
    CHECK (table.builtTakeOf ("SM000001").seconds > 86399.0);
    CHECK (table.builtTakeOf ("SM000001").problem.find ("the take was not set aside") == 0);
    CHECK (table.builtTakeOf ("SM000001").problem.find ("more than this computer can spare") != std::string::npos);

    //  And a channel that no longer samples lets its take go.
    spec.rack[0].takeSeconds = 0.0;
    spec.rack[0].layers = 4;
    REQUIRE (host.buildEdit (spec));
    CHECK (host.takeOf ("SM000001") == nullptr);
    CHECK (table.builtTakeOf ("SM000001").bytes == 0);

    host.stop();
}

TEST_CASE ("M40: the rack's children are its distinct plugins, not its channel slots")
{
    /*  Phase 9b (namespace draft 18.6 and 18.10, decision CL). A child spins
        a core while any lane of it is switched in, so what the rack costs in
        cores is how many children it has. Eight channels: four carry the test
        gain, four the mono test plugin, and two of them carry both - ten
        plugin slots, two plugins. Two children, every lane of each one its
        channel's, whatever the number of channels. */
    ScopedStorage storage;
    audio::AudioHost host { storage.path() };

    audio::HostSettings settings;
    settings.sampleRate = 48000;
    settings.blockSize = 64;
    settings.outputChannels = 2;
    settings.inputChannels = 2;
    REQUIRE (host.start (settings));

    plugin::PluginTable table;
    audio::ProxyServices services;
    services.table = &table;
    services.launch = launchOfThisBinary();
    host.setProxyServices (services);

    audio::EditSpec spec;
    spec.tracks = 1;
    spec.channelsPerTrack = 2;
    spec.proxyDeadlineMicroseconds = 200000;

    auto slots = 0;
    auto made = 0;

    for (int n = 0; n < 8; ++n)
    {
        audio::RackChannelSpec channel;
        channel.id = "CH00000" + std::to_string (n);
        channel.name = "Mic " + std::to_string (n + 1);

        const auto add = [&channel, &made] (const char* identifier)
        {
            audio::PluginSpec entry;
            entry.id = "PG9Q" + juce::String (made++).paddedLeft ('0', 4).toStdString();
            entry.identifier = identifier;
            entry.name = identifier;
            channel.plugins.push_back (entry);
        };

        add (n < 4 ? plugin::Catalogue::testGainIdentifier() : plugin::Catalogue::testMonoIdentifier());

        if (n == 1 || n == 6)
            add (n < 4 ? plugin::Catalogue::testMonoIdentifier() : plugin::Catalogue::testGainIdentifier());

        slots += static_cast<int> (channel.plugins.size());
        spec.rack.push_back (channel);
    }

    REQUIRE (host.buildEdit (spec));
    CHECK (slots == 10);

    auto children = 0;

    for (int k = 0; host.proxy (k) != nullptr; ++k)
        ++children;

    MESSAGE ("M40: 8 rack channels, " << slots << " plugin slots, 2 distinct plugins: " << children
             << " children, so at most " << children << " cores spinning");

    CHECK (children == 2);

    host.stop();
}

//==============================================================================
//  A KEY FOR AN EFFECT (namespace draft 141): the plugin's road.

TEST_CASE ("key: the region carries a key after each lane's audio, and its version says so")
{
    using namespace plugin::region;

    CHECK (version >= 7u);          // the key came with 7; the tap (8) and the times (9) since
    CHECK (keyChannelsMax == 2);
    CHECK (laneStride (2, 64) >= sizeof (Lane) + (2 + keyChannelsMax) * 64 * sizeof (float));

    VectorRegion region (2, 64, 2);
    auto* lane = region.lane (1);
    CHECK (keyOf (lane, 2, 64) == audioOf (lane) + 2 * 64);

    //  The key's room ends inside the lane's stride: the next lane starts after it.
    CHECK (reinterpret_cast<char*> (keyOf (region.lane (0), 2, 64) + keyChannelsMax * 64)
             <= reinterpret_cast<char*> (region.lane (1)));
}

TEST_CASE ("key: into a sidechain, a mono key goes to both sides, a stereo one side to side, two into one at a half")
{
    constexpr int n = 4;
    float left[n] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float right[n] = { 0.5f, 0.5f, 0.5f, 0.5f };
    const float* key[2] = { left, right };

    float a[n] {}, b[n] {};
    float* sidechain[2] = { a, b };

    SUBCASE ("mono into stereo")
    {
        plugin::lanemap::keyInto (key, 1, sidechain, 2, n);
        CHECK (a[3] == doctest::Approx (1.0f));
        CHECK (b[3] == doctest::Approx (1.0f));
    }

    SUBCASE ("stereo into stereo")
    {
        plugin::lanemap::keyInto (key, 2, sidechain, 2, n);
        CHECK (a[0] == doctest::Approx (1.0f));
        CHECK (b[0] == doctest::Approx (0.5f));
    }

    SUBCASE ("stereo into mono")
    {
        plugin::lanemap::keyInto (key, 2, sidechain, 1, n);
        CHECK (a[2] == doctest::Approx (0.75f));
        CHECK (b[2] == doctest::Approx (0.0f));
    }

    SUBCASE ("no key leaves the silence that is there")
    {
        plugin::lanemap::keyInto (key, 0, sidechain, 2, n);
        CHECK (a[0] == doctest::Approx (0.0f));
        CHECK (b[0] == doctest::Approx (0.0f));
    }
}

namespace
{
    /*  THE CHILD'S PART FOR A KEY, PLAYED BY A THREAD: every request answered
        as it comes, the key channels and the key's first sample noted. */
    struct KeyListener
    {
        explicit KeyListener (VectorRegion& regionToUse)
            : region (regionToUse),
              thread ([this]
              {
                  while (! stop.load())
                  {
                      auto* lane = region.lane (0);
                      const auto asked = lane->requestSeq.load (std::memory_order_acquire);

                      if (asked > lane->responseSeq.load (std::memory_order_relaxed))
                      {
                          channels.store (static_cast<int> (lane->keyChannels.load (std::memory_order_relaxed)));
                          first.store (plugin::region::keyOf (lane, region.channelCount, region.sampleCount)[0]);
                          lane->responseSeq.store (asked, std::memory_order_release);
                      }

                      std::this_thread::yield();
                  }
              })
        {
        }

        ~KeyListener()
        {
            stop.store (true);
            thread.join();
        }

        VectorRegion& region;
        std::atomic<bool> stop { false };
        std::atomic<int> channels { -1 };
        std::atomic<float> first { 0.0f };
        std::thread thread;
    };
}

TEST_CASE ("key: a lane whose cue picked a key sends it beside the block, and one that did not sends none")
{
    VectorRegion region (2, 64, 1);
    plugin::ProxyLane lane;
    region.bind (lane, 0);
    lane.setDeadlineMicroseconds (200000);
    lane.setEnabled (true);
    region.answerResets();

    std::vector<float> keyLeft (64, 0.75f), keyRight (64, 0.25f);
    const float* key[2] = { keyLeft.data(), keyRight.data() };

    KeyListener child (region);
    Block block (2, 64, 0.5f);

    SUBCASE ("unkeyed: no key reaches the region, whatever is handed in")
    {
        lane.process (block.data(), 2, 64, nullptr, 0, key, 2);
        CHECK (child.channels.load() == 0);
        CHECK (region.lane (0)->keyWanted.load() == 0u);
    }

    SUBCASE ("keyed: both sides of it, in the lane's key area")
    {
        lane.setKeyed (true);
        CHECK (region.lane (0)->keyWanted.load() == 1u);
        region.answerResets();

        lane.process (block.data(), 2, 64, nullptr, 0, key, 2);
        CHECK (child.channels.load() == 2);
        CHECK (child.first.load() == doctest::Approx (0.75f));
    }

    SUBCASE ("keyed, a key wider than the region carries: two of it")
    {
        lane.setKeyed (true);
        region.answerResets();

        const float* wide[3] = { keyLeft.data(), keyRight.data(), keyRight.data() };
        lane.process (block.data(), 2, 64, nullptr, 0, wide, 3);
        CHECK (child.channels.load() == 2);
    }
}

TEST_CASE ("key: picking a key under a sounding cue asks the child to lay its plugin out again, and the cue passes as it was meanwhile")
{
    VectorRegion region (2, 64, 1);
    plugin::ProxyLane lane;
    region.bind (lane, 0);
    lane.setDeadlineMicroseconds (200000);
    lane.setEnabled (true);
    region.answerResets();

    const auto resetsBefore = region.lane (0)->resetSeq.load();
    lane.setKeyed (true);

    CHECK (region.lane (0)->resetSeq.load() == resetsBefore + 1);
    CHECK (region.lane (0)->keyWanted.load() == 1u);

    //  The reset not made yet: the block as it came, and nothing sent.
    const auto requestsBefore = region.lane (0)->requestSeq.load();
    Block block (2, 64, 0.5f);
    lane.process (block.data(), 2, 64);

    CHECK (block.allEqual (0.5f));
    CHECK (region.lane (0)->requestSeq.load() == requestsBefore);

    //  The same pick again asks nothing more.
    lane.setKeyed (true);
    CHECK (region.lane (0)->resetSeq.load() == resetsBefore + 1);
}

TEST_CASE ("key: a plugin's sidechain is asked for after the ladder, switched back off, and switched on only for a key")
{
    using Set = juce::AudioChannelSet;
    plugin::InsertLayout layout;
    std::string problem;

    const auto mainStereo = [] (const juce::AudioProcessor::BusesLayout& l)
    {
        return l.getMainInputChannelSet() == Set::stereo() && l.getMainOutputChannelSet() == Set::stereo();
    };

    SUBCASE ("a stereo sidechain it lets go of: offered, off, then on and off again")
    {
        PickyProcessor keyed (true, mainStereo);
        REQUIRE (plugin::chooseLayout (keyed, 2, layout, problem));
        plugin::probeKey (keyed, layout);

        CHECK (layout.keyOffer == 2);
        CHECK_FALSE (layout.keyForced);
        CHECK (layout.keyedInputs == 4);
        CHECK_FALSE (keyed.getBus (true, 1)->isEnabled());

        CHECK (plugin::layKey (keyed, layout, true) == 2);
        CHECK (keyed.getBus (true, 1)->isEnabled());
        CHECK (keyed.getMainBusNumInputChannels() == 2);

        int at[2] {};
        plugin::keyChannelIndices (keyed, 2, at);
        CHECK (at[0] == 2);
        CHECK (at[1] == 3);

        CHECK (plugin::layKey (keyed, layout, false) == 0);
        CHECK_FALSE (keyed.getBus (true, 1)->isEnabled());
    }

    SUBCASE ("one that takes only a mono sidechain is offered as mono")
    {
        PickyProcessor keyed (true, [mainStereo] (const auto& l)
        {
            return mainStereo (l) && (l.inputBuses[1].isDisabled() || l.inputBuses[1] == Set::mono());
        });

        REQUIRE (plugin::chooseLayout (keyed, 2, layout, problem));
        plugin::probeKey (keyed, layout);
        CHECK (layout.keyOffer == 1);
        CHECK (plugin::layKey (keyed, layout, true) == 1);
    }

    SUBCASE ("one it will not give up is on from the start, and stays on")
    {
        PickyProcessor keyed (true, [mainStereo] (const auto& l)
        {
            return mainStereo (l) && l.inputBuses.size() == 2 && ! l.inputBuses[1].isDisabled();
        });

        REQUIRE (plugin::chooseLayout (keyed, 2, layout, problem));
        plugin::probeKey (keyed, layout);
        CHECK (layout.keyOffer == 2);
        CHECK (layout.keyForced);
        CHECK (plugin::layKey (keyed, layout, false) == 2);
    }

    SUBCASE ("a plugin with no sidechain offers none")
    {
        PickyProcessor plain (false, mainStereo);
        REQUIRE (plugin::chooseLayout (plain, 2, layout, problem));
        plugin::probeKey (plain, layout);
        CHECK (layout.keyOffer == 0);
        CHECK (plugin::layKey (plain, layout, true) == 0);
    }
}

TEST_CASE ("key: the test ducker hears its key through its child, and only when its cue picked one")
{
    Folder folder;
    plugin::PluginTable table;
    plugin::ProxyLane lanes[1];

    auto spec = testGainSpec (folder, 1, 2, 64);
    spec.identifier = plugin::Catalogue::testDuckIdentifier();
    plugin::ProxyHost host (spec, { &lanes[0] }, &table);

    std::string problem;
    REQUIRE (host.start (problem));
    REQUIRE (waitForState (host, "loaded", 5000));
    CHECK (host.status().keyInputs == 2);
    CHECK (lanes[0].keyOffered() == 2);

    lanes[0].setDeadlineMicroseconds (200000);

    std::vector<float> keyLeft (64, 0.5f), keyRight (64, 0.0f);
    const float* key[2] = { keyLeft.data(), keyRight.data() };
    Block block (2, 64, 0.25f);

    const auto play = [&]
    {
        for (int i = 0; i < 4; ++i)
        {
            std::fill (block.storage.begin(), block.storage.end(), 0.25f);
            lanes[0].process (block.data(), 2, 64, nullptr, 0, key, 2);
        }
    };

    SUBCASE ("no key picked: the gain alone, whatever key is handed in")
    {
        lanes[0].setEnabled (true);
        host.poll();
        REQUIRE (waitSettled (host, lanes[0]));
        CHECK (lanes[0].keyHeld() == 0);

        play();
        CHECK (block.allEqual (0.125f));
    }

    SUBCASE ("a key picked: down by the louder side of it")
    {
        lanes[0].setKeyed (true);
        lanes[0].setEnabled (true);
        host.poll();
        REQUIRE (waitSettled (host, lanes[0]));
        CHECK (lanes[0].keyHeld() == 2);

        //  0.25 x the gain's 0.5 x (1 - 0.5).
        play();
        CHECK (block.allEqual (0.0625f));
    }

    SUBCASE ("a key picked under the sounding insert: dry a moment, then ducked")
    {
        lanes[0].setEnabled (true);
        host.poll();
        REQUIRE (waitSettled (host, lanes[0]));
        play();
        CHECK (block.allEqual (0.125f));

        lanes[0].setKeyed (true);
        host.poll();
        REQUIRE (waitSettled (host, lanes[0]));
        CHECK (lanes[0].keyHeld() == 2);

        play();
        CHECK (block.allEqual (0.0625f));
    }

    host.stop();
}

TEST_CASE ("key: this cue after its level - its own sound under the level its output stage is told, ramped, folded")
{
    /*  Namespace draft 141, SC.4 (BHQ, BHT): a cue's own key is the sound
        arriving at the effect times the gain its output stage is told - its
        level, trims, DCAs and lane as one number - ramped from the last
        block's gain to this one's across the block. Wider than two, folded;
        a level at nought on both ends, no key. */
    constexpr int samples = 64;

    audio::CueMatrix level;
    level.prepare (1, 1, 48000.0, samples);
    level.setLevelDb (0.0f);

    audio::KeyReader reader;
    reader.prepare (samples);
    reader.bindLevel (&level);

    audio::KeySource self;
    self.kind = audio::KeySource::Kind::self;
    reader.setSource (self);
    REQUIRE (reader.hasKey());

    std::vector<float> left (samples, 0.5f), right (samples, -0.25f);
    const float* own[] { left.data(), right.data() };
    const float* key[2] {};

    //  None handed in: no key.
    CHECK (reader.read (0, samples, key) == 0);

    //  At unity: the sound itself, side to side.
    REQUIRE (reader.read (0, samples, key, own, 2) == 2);
    CHECK (key[0][0] == doctest::Approx (0.5f));
    CHECK (key[1][samples - 1] == doctest::Approx (-0.25f));

    //  Six decibels down: a ramp from unity to a half across the block, then steady.
    level.setLevelDb (-6.0206f);
    REQUIRE (reader.read (0, samples, key, own, 2) == 2);
    CHECK (key[0][0] > 0.49f);
    CHECK (key[0][samples / 2 - 1] == doctest::Approx (0.375f).epsilon (0.01));
    CHECK (key[0][samples - 1] == doctest::Approx (0.25f).epsilon (0.001));

    REQUIRE (reader.read (0, samples, key, own, 2) == 2);
    CHECK (key[0][0] == doctest::Approx (0.25f).epsilon (0.001));
    CHECK (key[1][0] == doctest::Approx (-0.125f).epsilon (0.001));

    //  A mono cue: one side.
    CHECK (reader.read (0, samples, key, own, 1) == 1);

    //  Four channels: folded to two at a half each, 0 and 2 left, 1 and 3 right.
    std::vector<float> third (samples, 0.5f), fourth (samples, -0.25f);
    const float* wide[] { left.data(), right.data(), third.data(), fourth.data() };
    level.setLevelDb (0.0f);
    reader.read (0, samples, key, own, 2);                      // the ramp back to unity, spent
    REQUIRE (reader.read (0, samples, key, wide, 4) == 2);
    CHECK (key[0][10] == doctest::Approx (0.5f));
    CHECK (key[1][10] == doctest::Approx (-0.25f));

    //  At nought, once the ramp down is spent: no key.
    level.setLevelDb (audio::CueMatrix::silenceDb);
    CHECK (reader.read (0, samples, key, own, 2) == 2);         // unity down to nought, across this block
    CHECK (key[0][samples - 1] == doctest::Approx (0.0f));
    CHECK (reader.read (0, samples, key, own, 2) == 0);

    //  Longer than it was prepared for: no key, never past the scratch.
    level.setLevelDb (0.0f);
    std::vector<float> longer (2 * samples, 0.5f);
    const float* tooLong[] { longer.data() };
    CHECK (reader.read (0, 2 * samples, key, tooLong, 1) == 0);
}

//==============================================================================
//  A PLUGIN PLAYED LATER (namespace draft 149): the queue.

TEST_CASE ("queue: a region made with a queue has so many slots after each lane's tap, and its hash says so")
{
    using namespace plugin::region;

    CHECK (version >= 10u);

    //  None asked for, none there: the lane's stride is what it was.
    CHECK (laneStride (2, 64, 0) == laneStride (2, 64));
    CHECK (laneStride (2, 64) == queueOffset (2, 64));
    CHECK (layoutHashFor (2, 64, 3, 0) == layoutHashFor (2, 64, 3));

    CHECK (slotStride (2, 64) % 64 == 0);
    CHECK (slotStride (2, 64) >= sizeof (Slot) + (2 + keyChannelsMax) * 64 * sizeof (float));
    CHECK (laneStride (2, 64, queueSlotsMost) == laneStride (2, 64) + queueSlotsMost * slotStride (2, 64));
    CHECK (regionBytes (2, 64, 2, queueSlotsMost) == headerBytes() + 2 * laneStride (2, 64, queueSlotsMost));
    CHECK (layoutHashFor (2, 64, 3, queueSlotsMost) != layoutHashFor (2, 64, 3));

    std::vector<char> bytes (regionBytes (2, 64, 2, queueSlotsMost), 0);
    auto* header = new (bytes.data()) Header {};
    header->magic.store (magic);
    header->version.store (version);
    header->channels.store (2);
    header->maxSamples.store (64);
    header->lanes.store (2);
    header->queueSlots.store (queueSlotsMost);
    header->layoutHash.store (layoutHashFor (2, 64, 2, queueSlotsMost));
    CHECK (looksValid (*header));

    //  A header that says another number of slots is another layout.
    header->queueSlots.store (queueSlotsMost - 1);
    CHECK_FALSE (looksValid (*header));
    header->queueSlots.store (queueSlotsMost + 1);
    CHECK_FALSE (looksValid (*header));
    header->queueSlots.store (queueSlotsMost);

    /*  The queue begins after the tap's ring and ends where the next lane
        begins; a slot's audio follows its head, and its key its audio. */
    auto* first = laneAt (bytes.data(), 2, 64, 0, queueSlotsMost);
    auto* second = laneAt (bytes.data(), 2, 64, 1, queueSlotsMost);
    const auto* tapEnd = tapOf (first, 2, 64) + (tapChannelsMax + keyChannelsMax) * tapFrames;
    CHECK (reinterpret_cast<const char*> (slotAt (first, 2, 64, 0)) >= reinterpret_cast<const char*> (tapEnd));
    CHECK (reinterpret_cast<char*> (slotAt (first, 2, 64, queueSlotsMost - 1)) + slotStride (2, 64)
             == reinterpret_cast<char*> (second));
    CHECK (reinterpret_cast<char*> (slotAt (first, 2, 64, 1)) - reinterpret_cast<char*> (slotAt (first, 2, 64, 0))
             == static_cast<std::ptrdiff_t> (slotStride (2, 64)));

    auto* slot = slotAt (first, 2, 64, 3);
    CHECK (reinterpret_cast<char*> (audioOf (slot)) - reinterpret_cast<char*> (slot) == static_cast<std::ptrdiff_t> (sizeof (Slot)));
    CHECK (keyOf (slot, 2, 64) == audioOf (slot) + 2 * 64);
}

namespace
{
    /*  THE CHILD'S PART OF THE QUEUE, PLAYED BY HAND (namespace draft 149):
        every request of a lane's queue not yet answered is, in order, its
        audio halved in its slot - a gain at a half - and the waiting one the
        same on the lane itself. */
    void answerQueueHalved (VectorRegion& region, int index)
    {
        using namespace plugin::region;
        auto* lane = region.lane (index);
        const auto slots = static_cast<std::uint64_t> (region.slotCount);

        for (auto number = lane->queueDone.load() + 1; number <= lane->queueSeq.load(); ++number)
        {
            auto* slot = slotAt (lane, region.channelCount, region.sampleCount, static_cast<int> (number % slots));
            auto* audio = audioOf (slot);

            for (int n = 0; n < region.channelCount * region.sampleCount; ++n)
                audio[n] *= 0.5f;

            lane->queueDone.store (number);
        }
    }

    void answerWaitingHalved (VectorRegion& region, int index)
    {
        using namespace plugin::region;
        auto* lane = region.lane (index);

        if (lane->responseSeq.load() >= lane->requestSeq.load())
            return;

        auto* audio = audioOf (lane);

        for (int n = 0; n < region.channelCount * region.sampleCount; ++n)
            audio[n] *= 0.5f;

        lane->responseSeq.store (lane->requestSeq.load());
    }

    /** One block of `value` through the lane; what its last sample came back as. */
    float playLater (plugin::ProxyLane& lane, Block& block, float value)
    {
        std::fill (block.storage.begin(), block.storage.end(), value);
        lane.process (block.data(), 2, 64);
        return block.storage.back();
    }
}

TEST_CASE ("later: a lane played later sends without waiting and plays what was answered that long ago")
{
    //  Namespace draft 149, BJW. Two channels of 64 in a region with a queue; the test is the child.
    using namespace plugin::region;

    plugin::ProxyLane lane;
    VectorRegion region (2, 64, 1, queueSlotsMost);
    region.bind (lane, 0);
    lane.setEnabled (true);
    region.answerResets();
    lane.setDeadlineMicroseconds (200000);      // a wait this long would be seen: none is made
    lane.setLate (64, false);
    CHECK (lane.lateSamples() == 0 + 64);

    Block block (2, 64, 0.0f);
    const auto started = std::chrono::steady_clock::now();

    //  THE FIRST BLOCK is the delay itself: sent, and silence back.
    CHECK (playLater (lane, block, 1.0f) == 0.0f);
    CHECK (block.allEqual (0.0f));
    CHECK (region.lane (0)->queueSeq.load() == 1);
    CHECK (region.lane (0)->requestSeq.load() == 0);    // never the one that waits
    CHECK (slotAt (region.lane (0), 2, 64, 1)->numSamples.load() == 64);
    CHECK (audioOf (slotAt (region.lane (0), 2, 64, 1))[0] == 1.0f);

    //  THE SECOND plays the first's answer, rising over the quick fade since it follows silence.
    answerQueueHalved (region, 0);
    CHECK (playLater (lane, block, 2.0f) == doctest::Approx (0.5f));
    CHECK (block.storage.front() == doctest::Approx (0.5f / static_cast<float> (plugin::ProxyLane::fadeSamples)));
    CHECK (block.storage[static_cast<std::size_t> (plugin::ProxyLane::fadeSamples)] == doctest::Approx (0.5f));

    //  AND SO ON, each a block behind, whole.
    answerQueueHalved (region, 0);
    CHECK (playLater (lane, block, 3.0f) == doctest::Approx (1.0f));
    CHECK (block.allEqual (1.0f));
    answerQueueHalved (region, 0);
    CHECK (playLater (lane, block, 4.0f) == doctest::Approx (1.5f));

    CHECK (lane.misses() == 0);
    CHECK (lane.movedLater() == 0);
    CHECK (lane.lateSamples() == 64);
    CHECK (lane.blocks() == 4);
    CHECK (std::chrono::steady_clock::now() - started < std::chrono::milliseconds (150));
}

TEST_CASE ("later: a lane that finds its answer missing waits one more buffer at once, and plays the very block that was late")
{
    //  Namespace draft 149, BJV. It starts as today's lane, waiting inside the callback.
    using namespace plugin::region;

    plugin::ProxyLane lane;
    VectorRegion region (2, 64, 1, queueSlotsMost);
    region.bind (lane, 0);
    lane.setEnabled (true);
    region.answerResets();
    lane.setDeadlineMicroseconds (200);
    lane.setLate (0, true);

    Block block (2, 64, 0.0f);

    //  LATE: silent as ever, counted - and given one buffer.
    CHECK (playLater (lane, block, 1.0f) == 0.0f);
    CHECK (lane.misses() == 1);
    CHECK (lane.movedLater() == 1);
    CHECK (lane.lateSamples() == 64);
    CHECK (region.lane (0)->requestSeq.load() == 1);

    //  STILL NOT THERE: one more, and this block goes out through the queue meanwhile.
    CHECK (playLater (lane, block, 2.0f) == 0.0f);
    CHECK (lane.misses() == 2);
    CHECK (lane.movedLater() == 2);
    CHECK (lane.lateSamples() == 128);
    CHECK (region.lane (0)->queueSeq.load() == 1);
    CHECK (region.lane (0)->requestSeq.load() == 1);    // the lane itself is not asked again

    //  THE CHILD CATCHES UP: the block that was late first, then every one after it, in order.
    answerWaitingHalved (region, 0);
    answerQueueHalved (region, 0);
    CHECK (playLater (lane, block, 3.0f) == doctest::Approx (0.5f));
    answerQueueHalved (region, 0);
    CHECK (playLater (lane, block, 4.0f) == doctest::Approx (1.0f));
    CHECK (block.allEqual (1.0f));
    answerQueueHalved (region, 0);
    CHECK (playLater (lane, block, 5.0f) == doctest::Approx (1.5f));

    CHECK (lane.misses() == 2);                 // nothing since
    CHECK (lane.consecutiveMisses() == 0);
    CHECK (lane.lateSamples() == 128);

    /*  A RESET EMPTIES THE QUEUE AND GIVES IT BACK (BKC, BKD): what the
        build resolved - nought here - so it waits inside the callback again. */
    lane.requestReset();
    CHECK (playLater (lane, block, 6.0f) == 0.0f);      // being reset: silent, not called
    region.answerResets();
    lane.setDeadlineMicroseconds (200000);

    std::thread child ([&region]
    {
        for (int i = 0; i < 2000 && region.lane (0)->requestSeq.load() < 2; ++i)
            std::this_thread::sleep_for (std::chrono::microseconds (100));

        answerWaitingHalved (region, 0);
    });

    CHECK (playLater (lane, block, 8.0f) == doctest::Approx (4.0f));    // in the same call
    child.join();
    CHECK (lane.lateSamples() == 0);
    CHECK (region.lane (0)->requestSeq.load() == 2);
}

TEST_CASE ("later: eight buffers is as late as a lane goes, and a lane that may not be given more is given none")
{
    //  Namespace draft 149, BKB.
    using namespace plugin::region;

    SUBCASE ("as late as a lane goes, then silent blocks as ever, and the stream where it would have been")
    {
        plugin::ProxyLane lane;
        VectorRegion region (2, 64, 1, queueSlotsMost);
        region.bind (lane, 0);
        lane.setEnabled (true);
        region.answerResets();
        lane.setDeadlineMicroseconds (100);
        lane.setLate (0, true);

        Block block (2, 64, 0.0f);

        //  Nobody answers for fourteen blocks.
        for (int k = 1; k <= 14; ++k)
            CHECK (playLater (lane, block, static_cast<float> (k)) == 0.0f);

        CHECK (lane.lateSamples() == plugin::ProxyLane::lateBuffersMost * 64);
        CHECK (lane.movedLater() == static_cast<std::uint32_t> (plugin::ProxyLane::lateBuffersMost));
        CHECK (lane.misses() == 14);
        CHECK (lane.consecutiveMisses() >= static_cast<std::uint32_t> (plugin::ProxyLane::missesBeforeFailure));

        /*  THE CHILD COMES BACK: block 15 plays what was sent eight blocks
            before it - block 7's answer - and not the whole backlog, late. */
        answerWaitingHalved (region, 0);
        answerQueueHalved (region, 0);
        CHECK (playLater (lane, block, 15.0f) == doctest::Approx (3.5f));
        answerQueueHalved (region, 0);
        CHECK (playLater (lane, block, 16.0f) == doctest::Approx (4.0f));
        CHECK (lane.consecutiveMisses() == 0);
        CHECK (lane.lateSamples() == plugin::ProxyLane::lateBuffersMost * 64);
    }

    SUBCASE ("never played later: a late block is silent and the lane stays where it is")
    {
        plugin::ProxyLane lane;
        VectorRegion region (2, 64, 1, queueSlotsMost);
        region.bind (lane, 0);
        lane.setEnabled (true);
        region.answerResets();
        lane.setDeadlineMicroseconds (100);
        lane.setLate (0, false);

        Block block (2, 64, 0.0f);
        CHECK (playLater (lane, block, 1.0f) == 0.0f);
        CHECK (playLater (lane, block, 2.0f) == 0.0f);
        CHECK (lane.misses() == 2);
        CHECK (lane.movedLater() == 0);
        CHECK (lane.lateSamples() == 0);
        CHECK (region.lane (0)->queueSeq.load() == 0);
    }

    SUBCASE ("a region made without a queue: the lane can only wait, whatever it is told")
    {
        plugin::ProxyLane lane;
        VectorRegion region (2, 64, 1);
        region.bind (lane, 0);
        lane.setEnabled (true);
        region.answerResets();
        lane.setDeadlineMicroseconds (100);
        lane.setLate (128, true);

        Block block (2, 64, 0.0f);
        CHECK (playLater (lane, block, 1.0f) == 0.0f);
        CHECK (lane.misses() == 1);
        CHECK (lane.lateSamples() == 0);
        CHECK (region.lane (0)->requestSeq.load() == 1);
    }
}

TEST_CASE ("later: at rest a lane is as late as the build resolved again")
{
    //  Namespace draft 149, BKD. A region whose ten seconds are a hundred samples, so the rest comes in two blocks.
    using namespace plugin::region;

    plugin::ProxyLane lane;
    VectorRegion region (2, 64, 1, queueSlotsMost);
    region.header->sampleRate.store (10);
    region.bind (lane, 0);
    lane.setEnabled (true);
    region.answerResets();
    lane.setDeadlineMicroseconds (100);
    lane.setLate (0, true);

    Block block (2, 64, 0.0f);
    CHECK (playLater (lane, block, 1.0f) == 0.0f);      // late: given a buffer
    CHECK (lane.lateSamples() == 64);

    answerWaitingHalved (region, 0);

    //  Silence in and silence out until it rests.
    for (int k = 0; k < 6 && ! lane.isResting(); ++k)
    {
        answerQueueHalved (region, 0);
        (void) playLater (lane, block, 0.0f);
    }

    CHECK (lane.isResting());
    CHECK (lane.lateSamples() == 0);
}

TEST_CASE ("later: a plugin too slow for the buffer is moved later, heard whole from then on, and its delay is said with its own")
{
    /*  Namespace draft 149, with the real child: the test gain taking a
        millisecond over every block of 64 at 48 kHz (1.33 ms), where a lane
        is allowed 0.4 ms. Late at first - and then given the time, at the
        price of being later. */
    Folder folder;
    plugin::PluginTable table;
    plugin::ProxyLane lane;
    auto spec = testGainSpec (folder, 1);
    spec.queueSlots = plugin::region::queueSlotsMost;       // as every plugin of a show is given
    spec.engineFolder = folder.string();                    // and where what it teaches is kept
    plugin::ProxyHost host (spec, { &lane }, &table);

    std::vector<std::string> notes;
    host.onLateNote ([&notes] (const std::string& note) { notes.push_back (note); });

    std::string problem;
    REQUIRE (host.start (problem));
    REQUIRE (waitForState (host, "loaded", 5000));

    const auto file = folder.path.getChildFile ("slow.state");
    REQUIRE (file.replaceWithText (juce::String ("gain=0.5\ndie=0\npad=0\nworkDelayUs=1000\n")));
    lane.setLate (0, true);
    lane.setEnabled (true);
    lane.wantState (file.getFullPathName().toStdString());
    REQUIRE (waitSettled (host, lane, 10000));

    Block block (2, 64, 0.5f);
    auto wet = 0, silent = 0, other = 0;

    /*  Four hundred blocks, never nearer one another than an interface's
        would be, the host polled as the engine polls it. A machine that
        keeps the child from its processor for a while moves the lane later
        again - one silent block each time - so what is asked of the second
        half is what holds on any machine: the gain's 0.25 or silence, never
        the block as it came, and the gain's nearly always. */
    for (int k = 0; k < 400; ++k)
    {
        std::fill (block.storage.begin(), block.storage.end(), 0.5f);
        lane.process (block.data(), 2, 64);

        if (k >= 200)
        {
            const auto last = block.storage.back();

            if (std::fabs (last - 0.25f) < 1.0e-6f)
                ++wet;
            else if (std::fabs (last) < 1.0e-6f)
                ++silent;
            else
                ++other;
        }

        if (k % 8 == 0)
            host.poll();

        std::this_thread::sleep_for (std::chrono::microseconds (1333));
    }

    CHECK (lane.lateSamples() >= 64);
    CHECK (lane.lateSamples() <= plugin::ProxyLane::lateBuffersMost * 64);
    CHECK (lane.movedLater() >= 1u);
    CHECK (other == 0);
    CHECK (wet >= 150);
    CHECK (wet + silent == 200);
    CHECK (host.status().state == "loaded");

    //  ITS DELAY, SAID WITH ITS OWN (BJX): the gain declares none, so the entry's is the lane's.
    host.poll();
    CHECK (table.statusOf ("PG7N0001").latencySamples == lane.lateSamples());

    //  AND THE SPELL'S NOTE SAYS IT WAS MOVED.
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds (plugin::ProxyHost::lateSpellEndsAfterMs + 5000);

    while (notes.empty() && std::chrono::steady_clock::now() < until)
    {
        host.poll();
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }

    REQUIRE (notes.size() >= 1);
    CHECK (notes[0].find (" samples later from now") != std::string::npos);
    CHECK (host.lastLateSpell().workMicros >= 900u);

    /*  AND IT TAUGHT THE MACHINE (BJZ): the gain's own work, a millisecond,
        is this plugin's measure now, so a build that reads it starts the
        lane a buffer later and it is never silent for it again. */
    plugin::PluginMeasure learnt;
    REQUIRE (plugin::measure::find (folder.string(), plugin::Catalogue::testGainIdentifier(), learnt));
    CHECK (learnt.workMicros >= 900u);
    CHECK (learnt.blockSize == 64);
    CHECK (learnt.by == "show");
    CHECK (plugin::measure::lateSamplesFor (learnt.workMicros, 48000, 64) >= 64);

    host.stop();
}

//==============================================================================
//  WHAT A PLUGIN NEEDS OF A BUFFER, MEASURED (namespace draft 149, BJU and BJZ).

TEST_CASE ("measure: a plugin is played later from the start by as many whole buffers as its measured work needs, and no more than eight")
{
    using plugin::measure::lateSamplesFor;

    //  Never measured, or nothing to measure against: not later at all.
    CHECK (lateSamplesFor (0, 48000, 128) == 0);
    CHECK (lateSamplesFor (1500, 0, 128) == 0);
    CHECK (lateSamplesFor (1500, 48000, 0) == 0);

    /*  128 frames at 48 kHz: a period of 2667 microseconds, of which a lane
        has six tenths, 1600. A measure is planned for at a quarter more. */
    CHECK (lateSamplesFor (100, 48000, 128) == 0);
    CHECK (lateSamplesFor (1270, 48000, 128) == 0);         // 1588 with the margin: it fits
    CHECK (lateSamplesFor (1300, 48000, 128) == 128);
    CHECK (lateSamplesFor (1769, 48000, 128) == 128);       // Tails, as M89 found it
    CHECK (lateSamplesFor (3570, 48000, 128) == 256);       // and its worst
    CHECK (lateSamplesFor (60000, 48000, 128) == plugin::ProxyLane::lateBuffersMost * 128);

    //  The same plugin on a larger buffer fits it; on a smaller one it needs the more.
    CHECK (lateSamplesFor (1769, 48000, 512) == 0);
    CHECK (lateSamplesFor (1769, 48000, 64) == 2 * 64);
    CHECK (lateSamplesFor (1000, 48000, 64) == 64);

    //  What the lane and the measure each call the callback's share, and the most, are one.
    CHECK (plugin::measure::callbackShare == doctest::Approx (plugin::ProxyLane::callbackFraction));
    CHECK (plugin::measure::buffersMost == plugin::ProxyLane::lateBuffersMost);
}

TEST_CASE ("measure: what this machine measured is kept a line a plugin, raised by a show and never lowered by one")
{
    Folder folder;
    const auto engine = folder.string();
    const std::string tails = "VST3-Unfiltered Audio Tails-2565c3d5-dbbf6a19";     // identifiers carry spaces
    const std::string likyu = "VST3-Likyu-2aee588c-59639613";

    plugin::PluginMeasure found;
    CHECK_FALSE (plugin::measure::find (engine, tails, found));
    CHECK (plugin::measure::readAll (engine).empty());

    //  THE SCAN'S.
    plugin::PluginMeasure scanned;
    scanned.workMicros = 1500;
    scanned.by = "scan";
    scanned.fileTime = 1763900000123;
    CHECK (plugin::measure::keep (engine, tails, scanned, false));

    plugin::PluginMeasure other;
    other.workMicros = 12;
    CHECK (plugin::measure::keep (engine, likyu, other, false));

    REQUIRE (plugin::measure::find (engine, tails, found));
    CHECK (found.workMicros == 1500);
    CHECK (found.blockSize == 128);
    CHECK (found.sampleRate == 48000);
    CHECK (found.by == "scan");
    CHECK (found.fileTime == 1763900000123);
    CHECK (plugin::measure::readAll (engine).size() == 2);

    //  A SHOW THAT SAW LESS teaches nothing.
    plugin::PluginMeasure learnt;
    learnt.workMicros = 900;
    learnt.blockSize = 64;
    learnt.by = "show";
    CHECK_FALSE (plugin::measure::keep (engine, tails, learnt, true));
    REQUIRE (plugin::measure::find (engine, tails, found));
    CHECK (found.workMicros == 1500);
    CHECK (found.by == "scan");

    //  ONE THAT SAW MORE raises it, and what the scan said of the file stands.
    learnt.workMicros = 3570;
    CHECK (plugin::measure::keep (engine, tails, learnt, true));
    REQUIRE (plugin::measure::find (engine, tails, found));
    CHECK (found.workMicros == 3570);
    CHECK (found.blockSize == 64);
    CHECK (found.by == "show");
    CHECK (found.fileTime == 1763900000123);

    //  The other plugin's line is as it was.
    REQUIRE (plugin::measure::find (engine, likyu, found));
    CHECK (found.workMicros == 12);

    //  A NEW SCAN'S MEASURE replaces, lower or not.
    scanned.workMicros = 1400;
    CHECK (plugin::measure::keep (engine, tails, scanned, false));
    REQUIRE (plugin::measure::find (engine, tails, found));
    CHECK (found.workMicros == 1400);

    //  A file somebody wrote half a line into is read for what reads.
    plugin::measure::fileIn (engine).appendText ("not a line\n12 128\n", false, false, "\n");
    CHECK (plugin::measure::readAll (engine).size() == 2);
}

TEST_CASE ("measure: a child measures one plugin alone, and a host starts its lanes as late as the measure says")
{
    Folder folder;

    /*  MEASURED AS IT IS PLAYED, the test gain through a hosting child of
        its own - this binary, as everywhere here - for a little under a
        second: a gain's own handful of microseconds, whatever they are on
        this machine today, and something. A plugin the scan does not know
        is nought, and says so. */
    std::string why;
    CHECK (plugin::measurePlugin (folder.string(), plugin::Catalogue::testGainIdentifier(), why, 0.8) > 0u);
    CHECK (why.empty());
    CHECK (plugin::measurePlugin (folder.string(), "VST3-Nothing Here-00000000-00000000", why, 0.8) == 0u);
    CHECK (why.find ("does not know") != std::string::npos);

    //  Two at one moment, each its own figure, in the order they were asked.
    std::vector<std::string> problems;
    const auto both = plugin::measurePlugins (folder.string(),
                                              { plugin::Catalogue::testGainIdentifier(), "VST3-Nothing Here-00000000-00000000",
                                                plugin::Catalogue::testDuckIdentifier() },
                                              problems, 0.8);
    REQUIRE (both.size() == 3);
    REQUIRE (problems.size() == 3);
    CHECK (both[0] > 0u);
    CHECK (both[1] == 0u);
    CHECK (both[2] > 0u);
    CHECK (problems[0].empty());
    CHECK (problems[1].find ("does not know") != std::string::npos);
    CHECK (problems[2].empty());

    //  A HOST GIVEN A MEASURE'S WORTH starts every lane that late, and may give it more.
    {
        plugin::PluginTable table;
        plugin::ProxyLane lane;
        auto spec = testGainSpec (folder, 1);
        spec.queueSlots = plugin::region::queueSlotsMost;
        spec.lateSamples = plugin::measure::lateSamplesFor (1000, spec.sampleRate, spec.maxSamples);
        REQUIRE (spec.lateSamples == 64);
        plugin::ProxyHost host (spec, { &lane }, &table);

        std::string problem;
        REQUIRE (host.start (problem));
        REQUIRE (waitForState (host, "loaded", 5000));
        CHECK (lane.lateSamples() == 64);

        //  And the entry says so before a block has gone through it.
        host.poll();
        CHECK (table.statusOf ("PG7N0001").latencySamples == 64);
        host.stop();
    }

    //  WITHOUT A QUEUE the measure is nothing to it: its lanes can only wait.
    {
        plugin::PluginTable table;
        plugin::ProxyLane lane;
        auto spec = testGainSpec (folder, 1);
        spec.lateSamples = 64;
        plugin::ProxyHost host (spec, { &lane }, &table);

        std::string problem;
        REQUIRE (host.start (problem));
        REQUIRE (waitForState (host, "loaded", 5000));
        CHECK (lane.lateSamples() == 0);
        host.stop();
    }
}
