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
#include <wfg/engine/plugin/ProxyHost.h>
#include <wfg/engine/plugin/ProxyLane.h>
#include <wfg/engine/plugin/SharedRegion.h>
#include <wfg/engine/rt/RtCheck.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <new>
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
    table.set ("PG7N0001", { "loaded", "", 12, 3 });

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
    plugin::ProxyHost host (spec, { &lane }, &table);

    std::string problem;
    CHECK_FALSE (host.start (problem));
    CHECK (host.status().state == "missing");
    CHECK (host.status().problem.find ("wfg plugins --scan") != std::string::npos);
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
