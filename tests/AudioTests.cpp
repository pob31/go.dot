/* This file is part of Go.dot — https://github.com/pob31/go.dot
 *
 * Copyright (C) 2026 Pierre-Olivier Boulant
 *
 * Go.dot is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. Go.dot is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * (LICENSE, at the repository root) for more details.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*  The audio section of the show document: the track count that sets the
    polyphony ceiling, and the buses that name ranges of output channels.

    WHAT THESE CASES ARE REALLY ABOUT. `tracks` is the first attribute in the
    parameter table with no default, and that is not a gap - it is how the
    table says REQUIRED. PRD 3.25 makes the track count the polyphony ceiling,
    and the author's Phase 2 decision was that no number could be right for
    every rig, so every show states its own. The rule enforcing it is general
    rather than a named exception, so half of what follows is about the rule
    and only incidentally about audio.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/Engine.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <wfg/engine/audio/AudioCommands.h>
#include <wfg/engine/rt/RtCheck.h>
#include <wfg/engine/audio/HostPlayer.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/audio/AudioHost.h>
#include <wfg/engine/audio/HostedAudioDriver.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/tree/TreeCommands.h>

#include "TestSupport.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace wfg;

namespace
{
    juce::File fixtureBundle()
    {
        const juce::File folder { juce::String (std::string (WFG_TEST_FIXTURES_DIR))
                                    + "/bundles/minimal" };

        REQUIRE_MESSAGE (folder.isDirectory(), "missing fixture bundle: "
                                                 << folder.getFullPathName());
        return folder;
    }

    bool mentions (const std::vector<std::string>& problems, const std::string& fragment)
    {
        for (const auto& problem : problems)
            if (problem.find (fragment) != std::string::npos)
                return true;

        return false;
    }

    std::string firstProblem (const doc::ReadResult& result)
    {
        return result.problems.empty() ? std::string ("(none)") : result.problems.front();
    }

    /*  Document, commands and tree, wired the way `wfg tree` and `serve` wire
        them. No mounts loaded: these cases are about /godot/audio and
        /godot/bus, and an empty mount table is a valid one. */
    struct Rig
    {
        Rig()
        {
            REQUIRE (doc::Bundle::open (fixtureBundle(), document).ok);
            doc::registerDocumentCommands (engine.commands(), document);
            tree::registerTreeCommands (engine.commands(), touches);
        }

        std::shared_ptr<const tree::TreeSnapshot> publish()
        {
            tree::EngineState state;
            state.version = "test";
            return parameters.publish (0, state);
        }

        Engine engine;
        doc::ShowDocument document;
        tree::TouchTable touches;
        tree::MountTable mounts;
        cue::RunTable runs;
        tree::ParameterTree parameters { document, engine.commands(), mounts, runs };
    };
}

//==============================================================================
TEST_CASE ("audio: a fresh document says zero tracks rather than nothing")
{
    /*  Zero is an answer - a show with no audio - and having no default is
        what makes the file always carry the number somebody meant. A document
        that omitted it would be one the canonical writer could not round-trip
        back to itself. */
    doc::ShowDocument document;

    CHECK (document.getAttribute ("/godot/audio/tracks") == "0");

    const auto text = doc::CanonicalXml::write (document);

    INFO ("written: " << text);
    CHECK (text.find ("<Audio tracks=\"0\"/>") != std::string::npos);
}

TEST_CASE ("audio: an Audio element with no tracks is refused, and says what is missing")
{
    doc::ShowDocument document;

    const auto result = doc::CanonicalXml::read ("<Show>\n"
                                                 "  <Lists/>\n"
                                                 "  <Mounts/>\n"
                                                 "  <Audio/>\n"
                                                 "</Show>\n", document);

    INFO ("problems: " << firstProblem (result));
    CHECK_FALSE (result.problems.empty());
    CHECK (mentions (result.problems, "tracks"));
}

TEST_CASE ("audio: a string with no default stays optional, because empty is a value")
{
    /*  The other half of the same rule, and why it is written in terms of the
        type rather than of hasDefault alone. `notes` has no declared default
        either; an absent note is the empty note somebody meant, so requiring
        every no-default attribute would reject every cue ever written. */
    doc::ShowDocument document;

    const auto result = doc::CanonicalXml::read ("<Show>\n"
                                                 "  <Lists>\n"
                                                 "    <List id=\"7K2QM9X4\"/>\n"
                                                 "  </Lists>\n"
                                                 "  <Mounts/>\n"
                                                 "  <Audio tracks=\"0\"/>\n"
                                                 "</Show>\n", document);

    INFO ("problems: " << firstProblem (result));
    CHECK (result.problems.empty());
}

TEST_CASE ("audio: the track count and its buses survive a save and a load")
{
    doc::ShowDocument document;

    const auto source = std::string ("<Show>\n"
                                     "  <Lists/>\n"
                                     "  <Mounts/>\n"
                                     "  <Audio tracks=\"12\">\n"
                                     "    <Bus id=\"J3MT5XYA\" name=\"Main L/R\" width=\"2\"/>\n"
                                     "    <Bus id=\"K4NV6ZB1\" firstChannel=\"2\" name=\"Sub\" width=\"1\"/>\n"
                                     "  </Audio>\n"
                                     "</Show>\n");

    REQUIRE (doc::CanonicalXml::read (source, document).ok);

    CHECK (document.getAttribute ("/godot/audio/tracks") == "12");
    CHECK (document.getAttribute ("/godot/bus/J3MT5XYA/name") == "Main L/R");
    CHECK (document.getAttribute ("/godot/bus/K4NV6ZB1/firstChannel") == "2");
    CHECK (document.getAttribute ("/godot/bus/K4NV6ZB1/width") == "1");

    const auto written = doc::CanonicalXml::write (document);

    doc::ShowDocument reloaded;
    REQUIRE (doc::CanonicalXml::read (written, reloaded).ok);

    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));
    CHECK (doc::CanonicalXml::write (reloaded) == written);
}

TEST_CASE ("audio: the tree publishes the track count and every bus")
{
    Rig rig;
    const auto snapshot = rig.publish();

    const auto* tracks = snapshot->find ("/godot/audio/tracks");

    REQUIRE (tracks != nullptr);
    CHECK (tracks->typeTags == "i");
    CHECK (tracks->access == tree::Access::read);

    /*  Read out of the fixture rather than remembered, so that changing the
        bundle changes what this expects. */
    REQUIRE (tracks->soleValue().has_value());
    CHECK (std::to_string (tracks->soleValue()->getInt32())
             == rig.document.getAttribute ("/godot/audio/tracks"));

    for (const auto& address : { "/godot/audio", "/godot/bus" })
    {
        INFO ("address: " << address);
        const auto* container = snapshot->find (address);

        REQUIRE (container != nullptr);
        CHECK (container->isContainer());
    }

    const auto* name = snapshot->find ("/godot/bus/J3MT5XYA/name");

    REQUIRE (name != nullptr);
    CHECK (name->access == tree::Access::readWrite);
    REQUIRE (name->soleValue().has_value());
    CHECK (name->soleValue()->getString() == "Main L/R");
}

TEST_CASE ("audio: the runtime nodes answer before anything has opened a device")
{
    /*  status, device and outputs describe what the audio side is doing. With
        no device they still exist and still answer, because "stopped" is the
        truthful answer and an absent node would leave a client guessing
        whether it had asked the wrong question. */
    Rig rig;
    const auto snapshot = rig.publish();

    const auto* status = snapshot->find ("/godot/audio/status");

    REQUIRE (status != nullptr);
    REQUIRE (status->soleValue().has_value());
    CHECK (status->soleValue()->getString() == "stopped");

    const auto* outputs = snapshot->find ("/godot/audio/outputs");

    REQUIRE (outputs != nullptr);
    REQUIRE (outputs->soleValue().has_value());
    CHECK (outputs->soleValue()->getInt32() == 0);
}

TEST_CASE ("audio: a write to the track count is refused as read-only, not as unknown")
{
    /*  Which refusal arrives is the point. `tracks` is a real address that
        simply is not writable over the wire, and answering "bad-address"
        would send a client looking for a spelling mistake it did not make. */
    Rig rig;

    const auto edit = rig.document.setAttribute ("/godot/audio/tracks", "16");

    CHECK_FALSE (edit.ok);
    CHECK (edit.reason == reason::readOnly);

    const auto missing = rig.document.setAttribute ("/godot/audio/noSuchThing", "16");

    CHECK_FALSE (missing.ok);
    CHECK (missing.reason == reason::badAddress);
}

//==============================================================================
/*  Tracktion Engine, hosted with no audio hardware.

    THE QUESTION THESE ANSWER is the one that had never been asked in this
    repository: does a tracktion::engine::Engine come up inside our build
    without touching the machine it runs on? Every spike built one, but a spike
    links wfg::thirdparty and never wfg::engine, so none of them proved it for
    the library the product is made of.

    They are slow by the standards of the rest of this suite - constructing the
    engine builds fifteen subsystems - so there are few of them and each earns
    its second.
*/
namespace
{
    /** A folder of our own per case, so two runs cannot share Tracktion's
        preferences and a failed run leaves nothing behind. */
    struct ScopedStorage
    {
        ScopedStorage()
            : folder (juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("wfg-audio-test-"
                                         + juce::Uuid().toDashedString()))
        {
        }

        ~ScopedStorage() { folder.deleteRecursively(); }

        std::string path() const { return folder.getFullPathName().toStdString(); }

        juce::File folder;
    };

    /*  Tracktion reaches MessageManager::getInstance() while it builds. JUCE is
        up for the whole process - TestMain.cpp holds one initialiser for the
        life of main - because bringing it up and down around each rig is what
        broke the OSCQuery cases on macOS. This opens no display on Linux, which
        is why the CI job needs no xvfb, the same reasoning `wfg selftest`
        records. */
    struct HostRig
    {
        ScopedStorage storage;
        audio::AudioHost host { storage.path() };
    };
}

TEST_CASE ("audio host: the engine comes up with no device and pumps a block")
{
    HostRig rig;

    audio::HostSettings settings;
    settings.sampleRate = 48000;
    settings.blockSize = 128;
    settings.outputChannels = 2;

    INFO ("start error: " << rig.host.lastError());
    REQUIRE (rig.host.start (settings));
    CHECK (rig.host.isRunning());
    CHECK (rig.host.lastError().empty());

    CHECK (rig.host.clock().samplesElapsed() == 0);
    CHECK (rig.host.blocksProcessed() == 0);

    rig.host.processBlock();

    CHECK (rig.host.blocksProcessed() == 1);
    CHECK (rig.host.clock().samplesElapsed() == 128);
}

TEST_CASE ("audio host: the sample counter follows the blocks exactly")
{
    /*  Exactly, not approximately. The tick clock converts this number into
        tick indices by division, so a block that advanced it by anything other
        than its own size would put every later tick on the wrong sample -
        silently, and further out the longer the show ran. */
    HostRig rig;

    audio::HostSettings settings;
    settings.sampleRate = 44100;
    settings.blockSize = 512;
    settings.outputChannels = 4;

    REQUIRE (rig.host.start (settings));

    for (int i = 1; i <= 50; ++i)
    {
        rig.host.processBlock();

        REQUIRE (rig.host.clock().samplesElapsed()
                   == static_cast<std::int64_t> (i) * settings.blockSize);
    }

    CHECK (rig.host.blocksProcessed() == 50);
}

TEST_CASE ("audio host: unusable settings are refused, and say so")
{
    HostRig rig;

    for (const auto& settings : { audio::HostSettings { 0, 128, 2 },
                                  audio::HostSettings { 48000, 0, 2 },
                                  audio::HostSettings { 48000, 128, 0 } })
    {
        INFO ("rate " << settings.sampleRate << " block " << settings.blockSize
                       << " outputs " << settings.outputChannels);

        CHECK_FALSE (rig.host.start (settings));
        CHECK_FALSE (rig.host.isRunning());
        CHECK_FALSE (rig.host.lastError().empty());
    }
}

TEST_CASE ("audio host: a stopped host does nothing rather than crashing")
{
    /*  Phase 10's immediate stop and any device that disappears mid-show both
        arrive here. Pumping a stopped host has to be inert, because the thread
        that pumps cannot be asked to check first - by the time it looked, the
        answer could have changed. */
    HostRig rig;

    rig.host.processBlock();
    CHECK (rig.host.blocksProcessed() == 0);

    audio::HostSettings settings;
    settings.sampleRate = 48000;
    settings.blockSize = 64;
    settings.outputChannels = 2;

    REQUIRE (rig.host.start (settings));
    rig.host.processBlock();
    REQUIRE (rig.host.clock().samplesElapsed() == 64);

    rig.host.stop();
    CHECK_FALSE (rig.host.isRunning());

    rig.host.processBlock();
    CHECK (rig.host.clock().samplesElapsed() == 64);
}

TEST_CASE ("audio host: hosting Tracktion does not change how this thread does arithmetic")
{
    /*  THE REGRESSION THIS PINS was found by the suite rather than by reading:
        three number cases that pass on their own started failing once an audio
        case ran before them in the same process. Standing a Tracktion engine up
        sets flush-to-zero and leaves it set, which is right for audio and wrong
        for a document - under it, a subnormal in a show file reads back as
        zero, and PRD §3.20 asks a number to survive a save and a load.

        Written here rather than in OscValueTests because the hazard belongs to
        the host: this is the file whose changes could reintroduce it, and a
        guard beside the thing it guards is one somebody will still understand
        when it fires. */
    const auto smallest = std::numeric_limits<float>::denorm_min();

    REQUIRE (smallest > 0.0f);

    HostRig rig;

    audio::HostSettings settings;
    settings.sampleRate = 48000;
    settings.blockSize = 64;
    settings.outputChannels = 2;

    REQUIRE (rig.host.start (settings));
    rig.host.processBlock();

    /*  The arithmetic, after the engine has been up and a block has run
        through it. Under flush-to-zero both of these become zero. */
    volatile float tiny = smallest;

    CHECK (tiny > 0.0f);
    CHECK (tiny * 0.5f >= 0.0f);

    const auto text = osc::formatFloat (smallest);
    const auto recovered = osc::parseDouble (text);

    INFO ("smallest float formatted as: " << text);
    REQUIRE (recovered.has_value());
    CHECK (juce::exactlyEqual (static_cast<float> (*recovered), smallest));

    rig.host.stop();

    volatile float stillTiny = smallest;
    CHECK (stillTiny > 0.0f);
}

TEST_CASE ("audio host: the rig is one wide output device, not a row of stereo pairs")
{
    /*  Tracktion's default is to carve the hardware into stereo pairs. Go.dot
        describes one device the whole rig wide, because spike 04 measured that
        changing a track's OUTPUT DEVICE rebuilds the playback graph - so if a
        cue's destination were a device, every destination change would rebuild.
        With one wide device the destination is a coefficient instead.

        This is also the check that would notice the description being ignored:
        a device list built before describeWaveDevices was consulted comes back
        as pairs, and this reads 4 devices of 2 rather than 1 of 8. */
    HostRig rig;

    audio::HostSettings settings;
    settings.sampleRate = 48000;
    settings.blockSize = 128;
    settings.outputChannels = 8;

    REQUIRE (rig.host.start (settings));

    CHECK (rig.host.waveOutputDeviceCount() == 1);
    CHECK (rig.host.waveOutputDeviceWidth() == 8);
}

TEST_CASE ("audio host: the wide device follows the channel count it was asked for")
{
    HostRig rig;

    for (const int channels : { 2, 6, 16, 64 })
    {
        INFO ("output channels: " << channels);

        audio::HostSettings settings;
        settings.sampleRate = 48000;
        settings.blockSize = 64;
        settings.outputChannels = channels;

        REQUIRE (rig.host.start (settings));

        CHECK (rig.host.waveOutputDeviceCount() == 1);
        CHECK (rig.host.waveOutputDeviceWidth() == channels);
    }
}

//==============================================================================
/*  The generated Edit: the fixed track set PRD 3.25 asks for, built from the
    document rather than loaded from one. */
namespace
{
    audio::HostSettings hostFor (int outputs)
    {
        audio::HostSettings settings;
        settings.sampleRate = 48000;
        settings.blockSize = 128;
        settings.outputChannels = outputs;
        return settings;
    }
}

TEST_CASE ("edit: the track set is fixed at load, and every track carries an output stage")
{
    HostRig rig;
    REQUIRE (rig.host.start (hostFor (8)));

    audio::EditSpec spec;
    spec.tracks = 4;
    spec.channelsPerTrack = 2;

    INFO ("build error: " << rig.host.lastError());
    REQUIRE (rig.host.buildEdit (spec));

    CHECK (rig.host.trackCount() == 4);

    for (int track = 0; track < 4; ++track)
    {
        INFO ("track " << track);
        auto* matrix = rig.host.trackMatrix (track);

        REQUIRE (matrix != nullptr);
        CHECK (matrix->numInputs() == 2);

        /*  THE ASSERTION THIS FILE EXISTS FOR. The plugin is as wide as the
            rig, not as wide as a stereo pair. Tracktion sizes a plugin node
            from getNumOutputChannelsGivenInputs, whose default answers 2 - so
            a 2 here would mean six of the eight outputs were being dropped
            with nothing said about it. */
        CHECK (matrix->numOutputs() == 8);
    }

    CHECK (rig.host.trackMatrix (-1) == nullptr);
    CHECK (rig.host.trackMatrix (4) == nullptr);
}

TEST_CASE ("edit: the graph runs with the tracks in it")
{
    /*  Building the Edit puts four plugin nodes in the playback graph. If any
        of them refused to initialise, or the plugin type were unregistered and
        the insert returned null, this is where it would show. */
    HostRig rig;
    REQUIRE (rig.host.start (hostFor (4)));

    audio::EditSpec spec;
    spec.tracks = 3;
    REQUIRE (rig.host.buildEdit (spec));

    for (int i = 0; i < 10; ++i)
        rig.host.processBlock();

    CHECK (rig.host.blocksProcessed() == 10);
    CHECK (rig.host.clock().samplesElapsed() == 10 * 128);
}

TEST_CASE ("edit: a show with no audio builds an Edit with no tracks")
{
    /*  tracks=0 is a legal document (see the schema cases above): a video or
        OSC-only show. It must produce a working engine with nothing in it,
        not a refusal. */
    HostRig rig;
    REQUIRE (rig.host.start (hostFor (2)));

    audio::EditSpec spec;
    spec.tracks = 0;

    REQUIRE (rig.host.buildEdit (spec));
    CHECK (rig.host.trackCount() == 0);

    rig.host.processBlock();
    CHECK (rig.host.blocksProcessed() == 1);
}

TEST_CASE ("edit: building one before the engine is up is refused, with a reason")
{
    HostRig rig;

    audio::EditSpec spec;
    spec.tracks = 2;

    CHECK_FALSE (rig.host.buildEdit (spec));
    CHECK_FALSE (rig.host.lastError().empty());
}

TEST_CASE ("edit: the width follows the rig, from a stereo pair to sixty-four")
{
    for (const int outputs : { 2, 16, 64 })
    {
        INFO ("rig outputs: " << outputs);

        HostRig rig;
        REQUIRE (rig.host.start (hostFor (outputs)));

        audio::EditSpec spec;
        spec.tracks = 2;
        REQUIRE (rig.host.buildEdit (spec));

        REQUIRE (rig.host.trackMatrix (0) != nullptr);
        CHECK (rig.host.trackMatrix (0)->numOutputs() == outputs);
    }
}

TEST_CASE ("edit: every node in the generated graph has an identity of its own")
{
    /*  THE UPSTREAM BUG THIS ANSWERS. Tracktion derives a node's id by
        hash-combining the ids of the items it is built from, and that combine
        barely mixes its value argument. This project reported it upstream
        (docs/spikes/upstream-node-id-collision.md) after seeing duplicate ids
        at 24 of 63 track counts on a rig whose EditItemIDs fell on a regular
        lattice. Two same-type nodes sharing an id adopt one another's state
        across a graph rebuild, on tracks with no dependency between them.

        Tracktion checks this itself, in a debug assertion, and says nothing in
        release. Go.dot asks the question about its OWN generated Edit instead
        of trusting either the hash or the report - and asks it at every track
        count a show might plausibly use, because the failure is a resonance
        between id strides and appears at some counts and not others. */
    HostRig rig;
    REQUIRE (rig.host.start (hostFor (8)));

    for (const int tracks : { 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 24, 31, 32, 48, 63, 64 })
    {
        INFO ("tracks: " << tracks);

        audio::EditSpec spec;
        spec.tracks = tracks;

        REQUIRE (rig.host.buildEdit (spec));

        const auto report = rig.host.inspectNodeIds();

        /*  Reported, not just asserted. A graph whose ids were mostly zero
            would pass a duplicate check while telling us nothing, so the shape
            of what was inspected is printed with the verdict. */
        INFO ("nodes " << report.nodes << ", zero ids " << report.zeroIds
                        << ", duplicates " << report.duplicates);

        CHECK (report.duplicates == 0);

        /*  The check must actually be looking at something. If Tracktion ever
            stopped giving these nodes identities, `duplicates == 0` would go on
            passing for the wrong reason. */
        CHECK (report.nodes > tracks);
        CHECK (report.nodes - report.zeroIds > tracks);
    }
}

TEST_CASE ("edit: every track holds a resident clip, so no track is missing from the graph")
{
    /*  A launcher clip whose slot is empty means no SlotControlNode, and the
        track's output stage goes with it - silently, with the track simply not
        heard. The resident placeholder is what a track sounds like before its
        first cue: silent, and present. */
    HostRig rig;
    REQUIRE (rig.host.start (hostFor (4)));

    audio::EditSpec spec;
    spec.tracks = 6;

    REQUIRE (rig.host.buildEdit (spec));
    CHECK (rig.host.residentClipCount() == 6);
}

//==============================================================================
/*  M1 - ROUTING EXACTNESS, through the real playback graph.

    CueMatrixTests already prove the arithmetic. What these prove is that the
    arithmetic is what the rig actually hears: that a cue reaches the outputs it
    names, at the gains it names, and reaches no others - across a Tracktion
    plugin, a summing node and a wide wave device that were all built from the
    show document rather than by hand.

    The method is spike 01's, reduced: a source whose value is known exactly, so
    a destination either carries it or does not. No FFT, no thresholding.
*/
namespace
{
    /** Records the peak magnitude per output channel, on the audio thread. */
    struct PeakSink final : audio::BlockSink
    {
        void blockProduced (const float* const* channels, int numChannels,
                            int numSamples) noexcept override
        {
            if (static_cast<int> (peak.size()) < numChannels)
                return;                       // sized by the test before it runs

            for (int channel = 0; channel < numChannels; ++channel)
                for (int n = 0; n < numSamples; ++n)
                    peak[static_cast<std::size_t> (channel)]
                        = std::max (peak[static_cast<std::size_t> (channel)],
                                    std::abs (channels[channel][n]));
        }

        void reset (int channels) { peak.assign (static_cast<std::size_t> (channels), 0.0f); }
        float operator[] (int channel) const { return peak[static_cast<std::size_t> (channel)]; }

        std::vector<float> peak;
    };
    /*  A file whose channel c holds a constant, and a different one per
        channel, so a destination does not merely say "something arrived" but
        which input it came from. The values are exact in 16 bits and none of
        them is near the silence floor. */
    constexpr float sourceAmplitude (int channel)  { return 0.5f - 0.0625f * static_cast<float> (channel); }

    /*  `seconds` is an argument because the fade cases play for longer than
        anything before them: a one-second fade measured from a steady level and
        followed to its destination outlasts the two seconds every earlier case
        needed. */
    juce::File writeSteadyTone (const juce::File& folder, int channels, int rate,
                                int seconds = 2)
    {
        const auto file = folder.getChildFile ("tone.wav");
        folder.createDirectory();

        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream { file.createOutputStream() };

        if (stream == nullptr)
            return {};

        auto writer = format.createWriterFor (stream,
                                              juce::AudioFormatWriterOptions{}
                                                .withSampleRate (static_cast<double> (rate))
                                                .withNumChannels (channels)
                                                .withBitsPerSample (16));

        if (writer == nullptr)
            return {};

        juce::AudioBuffer<float> buffer { channels, rate * std::max (1, seconds) };

        for (int channel = 0; channel < channels; ++channel)
            juce::FloatVectorOperations::fill (buffer.getWritePointer (channel),
                                               sourceAmplitude (channel),
                                               buffer.getNumSamples());

        writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
        return file;
    }

    /** One coefficient of the matrix: this input, to this output, at this gain. */
    struct Route
    {
        int input = 0, output = 0;
        float gain = 1.0f;
    };

    /*  Plays one cue through a whole rig and returns what each hardware output
        carried. Everything about the configuration is an argument, because what
        M1 measures is that the answer does not depend on the width. */
    std::vector<float> routeThroughTheRig (int sourceChannels, int outputs,
                                           const std::vector<Route>& routes)
    {
        constexpr int rate = 48000;

        HostRig rig;

        audio::HostSettings settings;
        settings.sampleRate = rate;
        settings.blockSize = 128;
        settings.outputChannels = outputs;

        REQUIRE (rig.host.start (settings));

        audio::EditSpec spec;
        spec.tracks = 1;
        spec.channelsPerTrack = sourceChannels;
        REQUIRE (rig.host.buildEdit (spec));

        const auto tone = writeSteadyTone (rig.storage.folder, sourceChannels, rate);
        REQUIRE (tone.existsAsFile());
        REQUIRE (rig.host.setTrackSource (0, tone.getFullPathName().toStdString()));

        auto* matrix = rig.host.trackMatrix (0);
        REQUIRE (matrix != nullptr);

        matrix->setLevelDb (0.0f);

        for (const auto& route : routes)
            matrix->setGain (route.input, route.output, route.gain);

        matrix->snapToTargets();

        PeakSink sink;
        sink.reset (outputs);
        rig.host.setBlockSink (&sink);

        /*  A few blocks before launching, so the sync point exists - a launch
            handle asked to play at no particular beat dereferences an empty
            optional. */
        for (int i = 0; i < 8; ++i)
            rig.host.processBlock();

        REQUIRE (rig.host.trackSourceLengthSeconds (0) > 1.0);

        /*  The wait is BEFORE the launch, not after it. A wait that ran the
            transport would be a race the test loses in Release: the same block
            count goes by in a tenth of the wall clock, and a two-second clip
            can finish before the disk has answered. */
        REQUIRE (rig.host.waitForTrackSourceReady (0, 10000));
        REQUIRE (rig.host.launchTrack (0));

        for (int i = 0; i < 32; ++i)
            rig.host.processBlock();

        sink.reset (outputs);
        rig.host.resetTrackPeaks (0);

        for (int i = 0; i < 100; ++i)
            rig.host.processBlock();

        const auto playing = rig.host.isTrackPlaying (0);
        const auto arrived = rig.host.trackInputPeak (0);

        rig.host.setBlockSink (nullptr);

        /*  The window measured is inside the clip, not off its end. A tone that
            had stopped early would make every silence check below pass for the
            wrong reason - which is exactly how the auto-tempo defect presented:
            correct routing, and then nothing, halfway through the file. */
        CHECK (playing);
        CHECK (arrived == doctest::Approx (sourceAmplitude (0)).epsilon (0.02));

        return sink.peak;
    }

    /*  What each output should carry: the sum of the routes that name it. The
        source is a constant of one sign, so the sum is exact and there is
        nothing to threshold. */
    std::vector<float> expectedFrom (int outputs, const std::vector<Route>& routes)
    {
        std::vector<float> expected (static_cast<std::size_t> (outputs), 0.0f);

        for (const auto& route : routes)
            expected[static_cast<std::size_t> (route.output)]
                += sourceAmplitude (route.input) * route.gain;

        return expected;
    }

    void checkRouting (int sourceChannels, int outputs, const std::vector<Route>& routes)
    {
        INFO (sourceChannels << " channels into " << outputs << " outputs");

        const auto measured = routeThroughTheRig (sourceChannels, outputs, routes);
        const auto expected = expectedFrom (outputs, routes);

        REQUIRE (measured.size() == expected.size());

        for (int channel = 0; channel < outputs; ++channel)
        {
            const auto index = static_cast<std::size_t> (channel);

            INFO ("output channel " << channel
                   << ": expected " << expected[index] << ", measured " << measured[index]);

            if (expected[index] > 0.0f)
                CHECK (measured[index] == doctest::Approx (expected[index]).epsilon (0.02));
            else
                CHECK (measured[index] < 0.001f);   // and nowhere else
        }
    }
}

//==============================================================================
TEST_CASE ("M1: a cue reaches the outputs it names, at the gains it names, and no others")
{
    /*  PRD 3.9b: a cue's destinations are a list, not a choice. One input feeds
        two of eight outputs at different gains, and the other six stay silent.

        The second half is the half that matters. A routing bug that merely
        leaked into a neighbouring channel would pass the first two checks. */
    checkRouting (1, 8, { { 0, 3, 1.0f }, { 0, 6, 0.5f } });
}

TEST_CASE ("M1: unity is unity, all the way through the master chain")
{
    /*  A coefficient of one and a level of 0 dB must arrive as the sample that
        was in the file - not approximately, and not through a pan law. Between
        the two sit a summing node, Tracktion's master VolumeAndPanPlugin, a
        level meter and a channel remap at the device boundary; any of them
        could apply a gain nobody asked for. */
    const auto measured = routeThroughTheRig (1, 2, { { 0, 0, 1.0f } });

    REQUIRE (measured.size() == 2u);
    CHECK (measured[0] == doctest::Approx (sourceAmplitude (0)).epsilon (0.005));
    CHECK (measured[1] < 0.001f);
}

TEST_CASE ("M1: a stereo cue splits, sums and fans out without either channel leaking")
{
    /*  Two inputs, and one output that both of them feed - which is where a
        matrix earns its shape. Output 1 carries the sum; output 4 only the
        left; output 6 only the right, halved. */
    checkRouting (2, 8, { { 0, 1, 1.0f }, { 1, 1, 1.0f },
                          { 0, 4, 1.0f },
                          { 1, 6, 0.5f } });
}

TEST_CASE ("M1: routing is exact at the width the rig is actually built for")
{
    /*  Eight channels into sixty-four outputs, with destinations near the top of
        the range. This is the configuration the whole design rests on: one wide
        device, and a cue placed by coefficients rather than by rewiring. A
        buffer sized from getBusses() instead of getNumOutputChannelsGivenInputs
        would drop every channel above the second, and this case would be silent
        everywhere. */
    checkRouting (8, 64, { { 0, 0,  1.0f },
                           { 1, 17, 1.0f },
                           { 2, 33, 0.5f },
                           { 7, 63, 1.0f },
                           { 3, 33, 1.0f } });
}

//==============================================================================
/*  M3 - WHAT ONE BLOCK COSTS, at the configuration the design is least likely
    to survive.

    The whole architecture rests on one decision: ONE output device as wide as
    the rig, and a cue placed by coefficients inside a per-track matrix rather
    than by rewiring the graph (PRD 3.9b, 3.25). Spike 04 measured why - changing
    a track's output rebuilds the playback graph, and the graph is fixed at show
    load. The price is that every track writes every hardware output on every
    block, whether or not it is going there: 32 tracks into 64 outputs is 2 048
    coefficients per sample.

    The fallback, written down in the plan and not built, is per-destination
    devices: one track per cue-times-destination. It trades this arithmetic for
    graph nodes, and it is only worth reaching for if the arithmetic does not
    fit. So what is measured here is the arithmetic, at 96 kHz and 64 frames,
    where the budget per block is 667 microseconds and there is the least of it.

    THIS REPORTS, IT DOES NOT GATE. A wall-clock threshold asserted on a shared
    CI runner is a flaky test that teaches people to re-run the suite, and the
    Debug number is not the number a show runs at anyway. What is asserted is
    only that the configuration stands up and stays exact; the cost is printed,
    and the two widths are printed together so the per-output term can be read
    off rather than guessed at.
*/
namespace
{
    struct BlockCost
    {
        double microsecondsPerBlock = 0.0;
        double budgetMicroseconds = 0.0;
        int tracksPlaying = 0;

        double percentOfBudget() const { return 100.0 * microsecondsPerBlock / budgetMicroseconds; }
    };

    /** A sink that does nothing, so what is timed is the graph and not the test. */
    struct NullSink final : audio::BlockSink
    {
        void blockProduced (const float* const*, int, int) noexcept override {}
    };

    BlockCost measureBlockCost (int tracks, int outputs, int rate, int blockSize)
    {
        HostRig rig;

        audio::HostSettings settings;
        settings.sampleRate = rate;
        settings.blockSize = blockSize;
        settings.outputChannels = outputs;

        REQUIRE (rig.host.start (settings));

        audio::EditSpec spec;
        spec.tracks = tracks;
        spec.channelsPerTrack = 2;
        REQUIRE (rig.host.buildEdit (spec));

        const auto tone = writeSteadyTone (rig.storage.folder, 2, rate);
        REQUIRE (tone.existsAsFile());

        /*  Every track playing, and every track routed somewhere different, so
            no two matrices are doing the same work and nothing can be folded
            away. This is a full house: the polyphony ceiling, all of it in use. */
        for (int track = 0; track < tracks; ++track)
        {
            REQUIRE (rig.host.setTrackSource (track, tone.getFullPathName().toStdString()));

            if (auto* matrix = rig.host.trackMatrix (track))
            {
                matrix->setLevelDb (-12.0f);
                matrix->setGain (0, track % outputs, 1.0f);
                matrix->setGain (1, (track + outputs / 2) % outputs, 1.0f);
                matrix->snapToTargets();
            }
        }

        NullSink sink;
        rig.host.setBlockSink (&sink);

        for (int i = 0; i < 8; ++i)
            rig.host.processBlock();

        /*  Every track past the file cache BEFORE any of them is launched, so
            what is timed is a graph in flight rather than one still finding its
            files. A clip that has not been mapped yet produces silence, and
            silence is cheap - timing that would be timing the wrong rig. */
        for (int track = 0; track < tracks; ++track)
            REQUIRE (rig.host.waitForTrackSourceReady (track, 10000));

        for (int track = 0; track < tracks; ++track)
            REQUIRE (rig.host.launchTrack (track));

        /*  A fifth of a second, counted in blocks, so the wait is the same
            length of TIME whatever the block size - the launch is placed a
            fraction of a beat ahead, and at 96 kHz with 64-frame blocks a fixed
            block count lands before it has happened. */
        for (int i = 0, blocks = static_cast<int> (0.2 * rate / blockSize); i < blocks; ++i)
            rig.host.processBlock();

        int playing = 0;

        for (int track = 0; track < tracks; ++track)
            rig.host.resetTrackPeaks (track);

        for (int i = 0; i < 16; ++i)
            rig.host.processBlock();

        /*  Audible, not merely launched. A track whose file the cache had not
            mapped would report itself as playing and cost almost nothing to
            process, and the whole measurement would be of a silent rig. */
        for (int track = 0; track < tracks; ++track)
            if (rig.host.isTrackPlaying (track) && rig.host.trackInputPeak (track) > 0.0f)
                ++playing;

        const auto timedBlocks = static_cast<int> (0.5 * rate / blockSize);
        const auto start = std::chrono::steady_clock::now();

        for (int i = 0; i < timedBlocks; ++i)
            rig.host.processBlock();

        const auto elapsed = std::chrono::steady_clock::now() - start;

        rig.host.setBlockSink (nullptr);

        const auto micros = std::chrono::duration<double, std::micro> (elapsed).count();

        return { micros / timedBlocks,
                 1.0e6 * blockSize / rate,
                 playing };
    }

    juce::String report (const juce::String& name, const BlockCost& cost)
    {
        return name + ": " + juce::String (cost.microsecondsPerBlock, 1) + " us/block of "
             + juce::String (cost.budgetMicroseconds, 1) + " us ("
             + juce::String (cost.percentOfBudget(), 1) + "% of real time), "
             + juce::String (cost.tracksPlaying) + " tracks playing";
    }
}

TEST_CASE ("M3: thirty-two cues into sixty-four outputs, at ninety-six kilohertz")
{
    constexpr int rate = 96000;
    constexpr int blockSize = 64;

    const auto wide = measureBlockCost (32, 64, rate, blockSize);
    const auto narrow = measureBlockCost (32, 8, rate, blockSize);
    const auto single = measureBlockCost (1, 64, rate, blockSize);

    MESSAGE (report ("32 tracks x 64 outputs", wide));
    MESSAGE (report ("32 tracks x  8 outputs", narrow));
    MESSAGE (report (" 1 track  x 64 outputs", single));

    /*  Every track really was making sound while it was timed. A measurement
        taken with half the tracks silent would be a measurement of a different
        rig, and a cheaper one. */
    CHECK (wide.tracksPlaying == 32);
    CHECK (narrow.tracksPlaying == 32);
    CHECK (single.tracksPlaying == 1);

    /*  The configuration survives being asked for, which is the part that can
        be asserted honestly. The cost itself is reported above and carried into
        the PR rather than pinned to a number this machine happened to produce. */
    CHECK (wide.microsecondsPerBlock > 0.0);
}

//==============================================================================
/*  The hosted driver: the dummy clock with a playback graph in the middle.

    What these establish is that it is a BLOCK SOURCE, indistinguishable from
    Phase 1's dummy clock to everything above it, and that the render it writes
    is a file somebody can actually open.
*/
namespace
{
    /** Reads a WAV back as it stands on disk, header and all. */
    struct RenderedFile
    {
        explicit RenderedFile (const juce::File& file)
        {
            juce::WavAudioFormat format;
            std::unique_ptr<juce::AudioFormatReader> reader {
                format.createReaderFor (file.createInputStream().release(), true) };

            if (reader == nullptr)
                return;

            channels = static_cast<int> (reader->numChannels);
            sampleRate = static_cast<int> (reader->sampleRate);
            frames = reader->lengthInSamples;
            isFloat = reader->usesFloatingPointData;

            if (frames <= 0)
                return;

            juce::AudioBuffer<float> buffer { channels, static_cast<int> (frames) };
            reader->read (&buffer, 0, static_cast<int> (frames), 0, true, true);

            for (int channel = 0; channel < channels; ++channel)
                peak = std::max (peak, buffer.getMagnitude (channel, 0, static_cast<int> (frames)));

            readable = true;
        }

        bool readable = false;
        bool isFloat = false;
        int channels = 0;
        int sampleRate = 0;
        std::int64_t frames = 0;
        float peak = 0.0f;
    };

    /*  Pumps the driver's own paced thread for a while. Wall clock, because
        that is what the driver runs on - it is pacing itself against a real
        deadline, which is the whole point of it. */
    void letItRunFor (int milliseconds)
    {
        juce::Thread::sleep (milliseconds);
    }
}

TEST_CASE ("hosted driver: the blocks come from a graph, and the counter cannot tell")
{
    ScopedStorage storage;

    audio::HostedAudioDriver driver { storage.folder.getFullPathName().toStdString() };

    audio::HostedAudioDriver::Settings settings;
    settings.sampleRate = 48000;
    settings.blockSize = 512;
    settings.outputChannels = 4;

    REQUIRE (driver.open (settings));

    audio::EditSpec spec;
    spec.tracks = 2;
    REQUIRE (driver.host().buildEdit (spec));

    /*  Nothing has been pumped yet. open() brings the engine up and stops
        there, deliberately: the Edit is built between the two calls, and
        building it while blocks went through would be a structural edit racing
        the graph that reads it. */
    CHECK (driver.blocksDelivered() == 0);
    CHECK (driver.clock().samplesElapsed() == 0);

    REQUIRE (driver.start());
    letItRunFor (400);
    driver.stop();

    const auto blocks = driver.blocksDelivered();

    INFO ("blocks in 400 ms at 48 kHz / 512: " << blocks
           << ", counter " << driver.clock().samplesElapsed());

    /*  PACED, not free-running. 400 ms at 48 kHz and 512 frames is about 37
        blocks; a loop with no deadline would deliver thousands, which is a
        difference no scheduler jitter can disguise. The bounds are wide
        because what is being asserted is the pacing, not the scheduler. */
    CHECK (blocks > 5);
    CHECK (blocks < 400);

    /*  And the counter is the blocks. Everything above this reads a
        SampleClock and must not be able to tell which source filled it. */
    CHECK (driver.clock().samplesElapsed() == blocks * settings.blockSize);
}

TEST_CASE ("hosted driver: a show with nothing playing renders digital silence")
{
    ScopedStorage storage;

    const auto render = storage.folder.getChildFile ("render.wav");

    audio::HostedAudioDriver driver { storage.folder.getFullPathName().toStdString() };

    audio::HostedAudioDriver::Settings settings;
    settings.sampleRate = 48000;
    settings.blockSize = 256;
    settings.outputChannels = 4;
    settings.renderFile = render.getFullPathName().toStdString();

    REQUIRE (driver.open (settings));

    audio::EditSpec spec;
    spec.tracks = 4;
    REQUIRE (driver.host().buildEdit (spec));
    REQUIRE (driver.start());

    letItRunFor (400);
    driver.stop();

    const RenderedFile rendered { render };

    REQUIRE (rendered.readable);
    CHECK (rendered.channels == 4);
    CHECK (rendered.sampleRate == 48000);

    /*  Float, because the render is a MEASUREMENT. PR 2.4 asserts that a fade
        reaches -120 dB and sixteen bits cannot express that; quantising the
        evidence to make the file smaller would be measuring the quantiser. */
    CHECK (rendered.isFloat);

    CHECK (rendered.frames > 0);

    /*  EXACTLY zero, not nearly. Four tracks are in the graph, each with a
        resident clip and an output plugin, and none of them has been fired -
        so every one of those outputs must be silent in the sense a mixing desk
        means it. Anything else is a leak. */
    CHECK (juce::exactlyEqual (rendered.peak, 0.0f));
}

TEST_CASE ("hosted driver: the render is readable even if nobody closed it")
{
    /*  A WAV's header carries its length, so it is only right once the file is
        closed - and the black-box harness stops the server with terminate(),
        which on Windows runs no destructor at all. The writer rewrites the
        header as it goes for exactly that reason, so this asks the question
        the harness will: is the file on disk readable while the process that
        is writing it is still alive? */
    ScopedStorage storage;

    const auto render = storage.folder.getChildFile ("unclosed.wav");

    audio::HostedAudioDriver driver { storage.folder.getFullPathName().toStdString() };

    audio::HostedAudioDriver::Settings settings;
    settings.sampleRate = 48000;
    settings.blockSize = 256;
    settings.outputChannels = 2;
    settings.renderFile = render.getFullPathName().toStdString();

    REQUIRE (driver.open (settings));

    audio::EditSpec spec;
    spec.tracks = 1;
    REQUIRE (driver.host().buildEdit (spec));
    REQUIRE (driver.start());

    /*  Past two flush intervals' worth of wall clock, so at least one header
        rewrite has certainly happened. */
    letItRunFor (2500);

    const RenderedFile whileRunning { render };

    INFO ("frames visible while still recording: " << whileRunning.frames);

    CHECK (whileRunning.readable);
    CHECK (whileRunning.frames > 0);
    CHECK (whileRunning.channels == 2);

    driver.stop();
}

TEST_CASE ("hosted driver: unusable settings are refused, and say so")
{
    ScopedStorage storage;

    audio::HostedAudioDriver driver { storage.folder.getFullPathName().toStdString() };

    SUBCASE ("nothing was asked for")
    {
        CHECK (! driver.open ({}));
        CHECK (! driver.lastError().empty());
    }

    SUBCASE ("started before it was opened")
    {
        CHECK (! driver.start());
        CHECK (! driver.lastError().empty());
    }

    SUBCASE ("a render nobody can write")
    {
        audio::HostedAudioDriver::Settings settings;
        settings.sampleRate = 48000;
        settings.blockSize = 256;
        settings.outputChannels = 2;

        /*  A directory, not a file. The failure has to arrive as a refusal
            with a reason, not as a server that runs happily and records
            nothing anybody will find. */
        settings.renderFile = storage.folder.getFullPathName().toStdString();

        REQUIRE (driver.open (settings));
        CHECK (! driver.start());
        CHECK (! driver.lastError().empty());
    }
}

TEST_CASE ("hosted driver: stopping twice, and stopping without starting, are quiet")
{
    ScopedStorage storage;

    audio::HostedAudioDriver driver { storage.folder.getFullPathName().toStdString() };

    driver.stop();

    audio::HostedAudioDriver::Settings settings;
    settings.sampleRate = 48000;
    settings.blockSize = 256;
    settings.outputChannels = 2;

    REQUIRE (driver.open (settings));

    audio::EditSpec spec;
    spec.tracks = 1;
    REQUIRE (driver.host().buildEdit (spec));
    REQUIRE (driver.start());

    letItRunFor (100);

    driver.stop();
    driver.stop();

    CHECK (! driver.isRunning());
}

//==============================================================================
/*  audio.editBuilt - the audio side reporting itself as a command.

    The replay fixture proves the RECORDS reproduce. These prove the STATE does:
    that what a client reads at /godot/audio after the event is what the event
    said, on a machine with no audio in it at all.
*/
TEST_CASE ("audio commands: the report becomes the state a client reads")
{
    Engine engine;
    audio::AudioState state;
    audio::registerAudioCommands (engine.commands(), state);

    CHECK (state.status == "stopped");
    CHECK (state.device.empty());
    CHECK (state.outputs == 0);

    REQUIRE (engine.submit ("engine", "audio.editBuilt",
                            { osc::Value::string ("hosted"), osc::Value::int32 (8),
                              osc::Value::int32 (4), osc::Value::int32 (64) }));
    engine.processTick (0);

    CHECK (state.device == "hosted");
    CHECK (state.tracks == 8);
    CHECK (state.outputs == 4);
    CHECK (state.nodes == 64);
    CHECK (state.status == "running");
}

TEST_CASE ("audio commands: reporting the same graph twice leaves the same state")
{
    /*  The graph is fixed at show load (PRD §3.25), so in a real session this
        happens once. It has to be idempotent anyway: a log replayed twice, or
        replayed after a session that already ran, must converge - otherwise
        "reproduces the session" would depend on what the engine had been doing
        beforehand. */
    Engine engine;
    audio::AudioState state;
    audio::registerAudioCommands (engine.commands(), state);

    const std::vector<osc::Value> report { osc::Value::string ("hosted"), osc::Value::int32 (8),
                                           osc::Value::int32 (4), osc::Value::int32 (64) };

    REQUIRE (engine.submit ("engine", "audio.editBuilt", report));
    engine.processTick (0);

    const auto once = state;

    REQUIRE (engine.submit ("engine", "audio.editBuilt", report));
    engine.processTick (1);

    CHECK (state.device == once.device);
    CHECK (state.tracks == once.tracks);
    CHECK (state.outputs == once.outputs);
    CHECK (state.nodes == once.nodes);
    CHECK (state.status == once.status);
}

TEST_CASE ("audio commands: a show with no audio is a real show, and reports zero")
{
    /*  Show/Audio/@tracks is required and has no default precisely so that a
        show can say zero and mean it. A range check written in a hurry refuses
        exactly this case. */
    Engine engine;
    audio::AudioState state;
    audio::registerAudioCommands (engine.commands(), state);

    REQUIRE (engine.submit ("engine", "audio.editBuilt",
                            { osc::Value::string (""), osc::Value::int32 (0),
                              osc::Value::int32 (0), osc::Value::int32 (0) }));

    const auto outcome = engine.processTick (0);

    CHECK (outcome.applied == 1);
    CHECK (state.tracks == 0);
    CHECK (state.outputs == 0);
    CHECK (state.status == "running");
}

TEST_CASE ("audio commands: a mangled count is refused rather than clamped")
{
    /*  A graph with minus four nodes is a message that got damaged on the way
        in. Storing 0 would put a number nobody measured in front of every
        client reading the tree, and it would look exactly like a graph that
        had been measured and found empty. */
    Engine engine;
    audio::AudioState state;
    audio::registerAudioCommands (engine.commands(), state);

    REQUIRE (engine.submit ("engine", "audio.editBuilt",
                            { osc::Value::string ("hosted"), osc::Value::int32 (8),
                              osc::Value::int32 (4), osc::Value::int32 (-4) }));

    const auto outcome = engine.processTick (0);

    CHECK (outcome.applied == 0);
    CHECK (outcome.rejected == 1);

    /*  And nothing was written. A handler that refused after storing three of
        its four arguments would leave the tree describing a graph that never
        existed. */
    CHECK (state.device.empty());
    CHECK (state.tracks == 0);
    CHECK (state.status == "stopped");
}

//==============================================================================
/*  PRD §4.2 made checkable: "the audio thread is a lipogram - no allocation, no
    locks, no exceptions, no syscalls, no logging."

    These are the enforcement, and every PR from here adds its scenario to them.
*/

TEST_CASE ("rt: the counter can count, which is the first thing to establish")
{
    /*  A test that only ever asserted zero would pass beautifully against an
        instrument that was not plugged in. So before anything is proved silent,
        the instrument is made to make a noise. */
    REQUIRE (rt::isCounting());

    rt::resetCounts();
    CHECK (rt::violations() == 0);

    {
        const rt::ScopedRealtimeCheck inside { rt::Region::ours };

        /*  volatile so the optimiser cannot decide this allocation is
            unobservable and remove it, which in a release build it otherwise
            would - taking the test's whole subject with it. */
        volatile auto* leaked = new int (7);
        delete leaked;
    }

    CHECK (rt::violations() >= 1);

    /*  And outside a scope nothing is counted, or every allocation the process
        made while loading a show would land on the audio thread's tally. */
    const auto after = rt::violations();

    {
        volatile auto* elsewhere = new int (9);
        delete elsewhere;
    }

    CHECK (rt::violations() == after);
}

TEST_CASE ("rt: a dependency's allocations are counted apart from ours, and the inner region wins")
{
    /*  Tracktion's block runs INSIDE Go.dot's callback. Without restoring the
        previous region on the way out, either every allocation TE makes would
        be charged to us, or the epilogue after TE returns would be charged to
        TE - and the epilogue is Go.dot's code and the part that must be zero. */
    REQUIRE (rt::isCounting());
    rt::resetCounts();

    {
        const rt::ScopedRealtimeCheck outer { rt::Region::ours };

        {
            const rt::ScopedRealtimeCheck inner { rt::Region::foreign };

            volatile auto* theirs = new int (1);
            delete theirs;
        }

        /*  Back in our region, because the inner scope restored it rather than
            clearing it. This allocation is ours. */
        volatile auto* ours = new int (2);
        delete ours;
    }

    CHECK (rt::violations() >= 1);
    CHECK (rt::foreignAllocations() >= 1);
    CHECK (rt::foreignRegions() == 1);
}

TEST_CASE ("rt: Go.dot allocates nothing on the audio thread, and Tracktion's cost is reported")
{
    /*  THE ONE THAT MATTERS. A whole graph, running, with the counter armed.

        What is ASSERTED is that Go.dot's own regions - the callback's prologue
        and epilogue, and CueOutputPlugin::applyToBuffer inside Tracktion's own
        block - allocate nothing at all.

        What is REPORTED is Tracktion's. It is not asserted, and hiding it would
        be worse than either: Tracktion's device callback takes a shared lock
        every block by design and its node player uses semaphores, which is a
        fact about a dependency Go.dot chose rather than a defect in it. The
        number is published at /godot/engine/rtForeignAllocations for the same
        reason. Whether PRD §4.2 should say so is the author's amendment.
    */
    REQUIRE (rt::isCounting());

    /*  Three track counts, because the question the number has to answer is not
        "how many" but "how many MORE at sixty-four tracks". A per-block cost
        that grows with the show is a different conversation from a fixed one. */
    juce::String report;

    for (const int tracks : { 1, 4, 16 })
    {
        HostRig rig;

        audio::HostSettings settings;
        settings.sampleRate = 48000;
        settings.blockSize = 128;
        settings.outputChannels = 8;

        REQUIRE (rig.host.start (settings));

        audio::EditSpec spec;
        spec.tracks = tracks;
        REQUIRE (rig.host.buildEdit (spec));

        /*  Warmed up before the counter is reset. The first blocks through a
            new graph allocate for reasons that are not steady state - buffers
            being sized, nodes being prepared - and a baseline taken across them
            would describe a rig that had just started rather than one running. */
        for (int i = 0; i < 200; ++i)
            rig.host.processBlock();

        rt::resetCounts();

        constexpr int blocks = 500;

        for (int i = 0; i < blocks; ++i)
            rig.host.processBlock();

        const auto ours = rt::violations();
        const auto theirs = rt::foreignAllocations();
        const auto regions = rt::foreignRegions();

        report << juce::String (tracks).paddedLeft (' ', 3) << " tracks: "
               << juce::String (regions > 0 ? static_cast<double> (theirs)
                                                / static_cast<double> (regions) : 0.0, 2)
               << " Tracktion allocations per block (" << (int) theirs << " over "
               << (int) regions << ")" << juce::newLine;

        INFO ("at " << tracks << " tracks, Go.dot's own regions allocated "
               << ours << " times in " << blocks << " blocks");

        /*  ZERO. Not small, not typical - none. */
        CHECK (ours == 0);

        /*  The measurement covered the blocks it claims to have covered. */
        CHECK (regions == blocks);
    }

    MESSAGE ("Tracktion steady state, 8 outputs at 48 kHz / 128:" << juce::newLine << report);
}

TEST_CASE ("rt: a cue playing through the matrix still allocates nothing of ours")
{
    /*  The idle graph is the easy case. This one has a clip playing, the output
        plugin doing its copy and its matrix, and coefficients being changed
        underneath it - which is what a fade will do at 50 Hz from Phase 3, and
        exactly where an allocation would hide. */
    REQUIRE (rt::isCounting());

    HostRig rig;

    audio::HostSettings settings;
    settings.sampleRate = 48000;
    settings.blockSize = 128;
    settings.outputChannels = 8;

    REQUIRE (rig.host.start (settings));

    audio::EditSpec spec;
    spec.tracks = 1;
    spec.channelsPerTrack = 1;
    REQUIRE (rig.host.buildEdit (spec));

    const auto tone = writeSteadyTone (rig.storage.folder, 1, settings.sampleRate);
    REQUIRE (tone.existsAsFile());
    REQUIRE (rig.host.setTrackSource (0, tone.getFullPathName().toStdString()));

    auto* matrix = rig.host.trackMatrix (0);
    REQUIRE (matrix != nullptr);
    matrix->setGain (0, 3, 1.0f);
    matrix->snapToTargets();

    for (int i = 0; i < 8; ++i)
        rig.host.processBlock();

    REQUIRE (rig.host.waitForTrackSourceReady (0, 10000));
    REQUIRE (rig.host.launchTrack (0));

    for (int i = 0; i < 100; ++i)
        rig.host.processBlock();

    REQUIRE (rig.host.trackInputPeak (0) > 0.0f);

    rt::resetCounts();

    for (int i = 0; i < 300; ++i)
    {
        /*  Written from this thread while the block runs, which is what the
            tick thread will be doing during a fade. Every setter is one relaxed
            atomic store and must stay that way. */
        matrix->setLevelDb (-6.0f + static_cast<float> (i % 12));
        matrix->setGain (0, 3, 0.5f + 0.01f * static_cast<float> (i % 20));

        rig.host.processBlock();
    }

    INFO ("Go.dot's own regions allocated " << rt::violations()
           << " times while a cue was playing and its matrix was moving");

    CHECK (rt::violations() == 0);
}

//==============================================================================
/*  THE ANCHOR: where Go.dot's sample counter meets Tracktion's beat axis.

    Every launch instant is computed through this, so these are the numbers that
    would rot silently. A wrong anchor does not crash, it makes every cue play
    slightly early or late, every night, on a machine nobody can reproduce it on.

    The anchor is MEASURED rather than derived, once per block, in the callback
    where both numbers describe the same instant. That it currently comes out at
    exactly zero is a coincidence of two facts that cancel - Tracktion seeds its
    reference range one block ahead, and its sync point reports the END of the
    block - and it is not hard-coded anywhere. These cases pin the behaviour, not
    the coincidence.
*/
TEST_CASE ("anchor: the beat axis and the sample counter agree, and keep agreeing")
{
    HostRig rig;

    audio::HostSettings settings;
    settings.sampleRate = 48000;
    settings.blockSize = 128;
    settings.outputChannels = 2;

    REQUIRE (rig.host.start (settings));

    audio::EditSpec spec;
    spec.tracks = 1;
    REQUIRE (rig.host.buildEdit (spec));

    /*  Nothing has been pumped, so there is no anchor yet - and the host says
        so rather than answering with a confident zero. */
    CHECK (rig.host.anchoredAtSample() == 0);

    for (int i = 0; i < 500; ++i)
        rig.host.processBlock();

    const auto anchoredAt = rig.host.anchoredAtSample();
    INFO ("anchored at sample " << anchoredAt);

    /*  Taken at the end of the last block, which is what makes it pair with
        Go.dot's counter rather than with the block in flight. */
    CHECK (anchoredAt == 500 * settings.blockSize);

    /*  ONE BEAT IS ONE SECOND, so the beat at sample N is N / sampleRate. That
        is the whole arithmetic, and the Edit's tempo is asserted at buildEdit
        precisely so it stays true. */
    const auto beatsAtOneSecond = rig.host.beatsAtSample (settings.sampleRate);
    INFO ("beats at one second: " << beatsAtOneSecond);
    CHECK (beatsAtOneSecond == doctest::Approx (1.0).epsilon (1.0e-9));

    const auto beatsAtTen = rig.host.beatsAtSample (10 * settings.sampleRate);
    CHECK (beatsAtTen == doctest::Approx (10.0).epsilon (1.0e-9));

    /*  And it does not drift. The offset is republished every block, so a
        thousand blocks later the same future sample must answer the same beat -
        an anchor that accumulated error would show up here as a difference in
        the last digits rather than as anything anybody would notice live. */
    const auto before = rig.host.beatsAtSample (1000 * settings.sampleRate);

    for (int i = 0; i < 1000; ++i)
        rig.host.processBlock();

    const auto after = rig.host.beatsAtSample (1000 * settings.sampleRate);

    INFO ("before " << before << ", after " << after);
    CHECK (before == doctest::Approx (after).epsilon (1.0e-12));
}

TEST_CASE ("anchor: Tracktion's own counter runs exactly one block ahead, and says so")
{
    /*  REPORTED, NOT ASSERTED AS A CONSTANT. The skew is one block in a healthy
        run because Tracktion publishes its sync range before the graph runs
        while Go.dot advances its counter after. What matters is that it does
        not CHANGE: a change means Tracktion skipped blocks - a suspended
        device, the CPU-overload mute, a resync - and that is the number to look
        at when a show has drifted and nobody knows why. */
    HostRig rig;

    audio::HostSettings settings;
    settings.sampleRate = 48000;
    settings.blockSize = 256;
    settings.outputChannels = 2;

    REQUIRE (rig.host.start (settings));

    audio::EditSpec spec;
    spec.tracks = 1;
    REQUIRE (rig.host.buildEdit (spec));

    for (int i = 0; i < 100; ++i)
        rig.host.processBlock();

    const auto skew = rig.host.referenceSkewSamples();
    INFO ("reference skew: " << skew << " samples, block size " << settings.blockSize);

    CHECK (skew == settings.blockSize);

    for (int i = 0; i < 400; ++i)
        rig.host.processBlock();

    /*  Unchanged over four hundred more blocks. If this ever fails, the launch
        arithmetic is still correct - the anchor measures around it - but
        something upstream dropped audio, and that is worth finding out about. */
    CHECK (rig.host.referenceSkewSamples() == skew);
}

TEST_CASE ("anchor: the beat is asked for in samples, so the rate is the only conversion")
{
    /*  Two rates, one arithmetic. If beatsAtSample ever started depending on
        something other than the rate - a block size, a tick length - this is
        where it would show. */
    for (const int rate : { 44100, 96000 })
    {
        INFO ("at " << rate << " Hz");

        HostRig rig;

        audio::HostSettings settings;
        settings.sampleRate = rate;
        settings.blockSize = 512;
        settings.outputChannels = 2;

        REQUIRE (rig.host.start (settings));

        audio::EditSpec spec;
        spec.tracks = 1;
        REQUIRE (rig.host.buildEdit (spec));

        for (int i = 0; i < 200; ++i)
            rig.host.processBlock();

        CHECK (rig.host.beatsAtSample (rate) == doctest::Approx (1.0).epsilon (1.0e-9));
        CHECK (rig.host.beatsAtSample (rate / 2) == doctest::Approx (0.5).epsilon (1.0e-9));
    }
}

TEST_CASE ("launch: the tick-safe path reaches the same handle the diagnostic one does")
{
    /*  launchTrackAt is what GO uses, and it must not be a second mechanism
        that could disagree with the first. Both end at the clip's one
        LaunchHandle; this asserts they do.

        It also covers the reason launchTrackAt exists: it reaches a handle
        cached at buildEdit rather than walking to the clip, which costs two
        heap allocations every time and would be four hundred of them a second
        at 50 Hz over four tracks. */
    HostRig rig;

    audio::HostSettings settings;
    settings.sampleRate = 48000;
    settings.blockSize = 128;
    settings.outputChannels = 2;

    REQUIRE (rig.host.start (settings));

    audio::EditSpec spec;
    spec.tracks = 2;
    spec.channelsPerTrack = 1;
    REQUIRE (rig.host.buildEdit (spec));

    const auto tone = writeSteadyTone (rig.storage.folder, 1, settings.sampleRate);
    REQUIRE (rig.host.setTrackSource (0, tone.getFullPathName().toStdString()));
    REQUIRE (rig.host.waitForTrackSourceReady (0, 10000));

    CHECK (rig.host.trackPlayState (0).valid);
    CHECK_FALSE (rig.host.trackPlayState (0).playing);

    /*  An index no track answers to is answered rather than crashed on: this is
        reachable from a cue naming a track that a smaller rig does not have. */
    CHECK_FALSE (rig.host.trackPlayState (99).valid);
    CHECK_FALSE (rig.host.launchTrackAt (99, 1.0));
    CHECK_FALSE (rig.host.stopTrack (99));

    for (int i = 0; i < 8; ++i)
        rig.host.processBlock();

    /*  Placed a quarter of a second ahead, in Go.dot's own samples, converted
        through the anchor. */
    const auto target = rig.host.clock().samplesElapsed() + settings.sampleRate / 4;
    REQUIRE (rig.host.launchTrackAt (0, rig.host.beatsAtSample (target)));

    /*  Not yet: the instant is ahead of the blocks pumped so far. */
    for (int i = 0; i < 10; ++i)
        rig.host.processBlock();

    CHECK_FALSE (rig.host.trackPlayState (0).playing);

    /*  And now it is, having crossed the instant. */
    for (int i = 0; i < 200; ++i)
        rig.host.processBlock();

    const auto playing = rig.host.trackPlayState (0);
    INFO ("played beats: " << playing.playedBeats);

    CHECK (playing.playing);
    CHECK (playing.playedBeats > 0.0);

    /*  The other track was never launched and must not have started on its own -
        the handles are per track, and an index mistake would show up here. */
    CHECK_FALSE (rig.host.trackPlayState (1).playing);
}

TEST_CASE ("launch: a stop that lands is confirmed, not assumed")
{
    /*  Cancelling a queued launch is best effort in Tracktion - the queue is
        read through a try-lock that answers "nothing queued" when the audio
        thread holds it - so Go.dot confirms on a later tick rather than
        believing the call. This asserts the confirm, which is the part Go.dot
        controls. */
    HostRig rig;

    audio::HostSettings settings;
    settings.sampleRate = 48000;
    settings.blockSize = 128;
    settings.outputChannels = 2;

    REQUIRE (rig.host.start (settings));

    audio::EditSpec spec;
    spec.tracks = 1;
    spec.channelsPerTrack = 1;
    REQUIRE (rig.host.buildEdit (spec));

    const auto tone = writeSteadyTone (rig.storage.folder, 1, settings.sampleRate);
    REQUIRE (rig.host.setTrackSource (0, tone.getFullPathName().toStdString()));
    REQUIRE (rig.host.waitForTrackSourceReady (0, 10000));

    for (int i = 0; i < 8; ++i)
        rig.host.processBlock();

    REQUIRE (rig.host.launchTrackAt (0, rig.host.beatsAtSample (rig.host.clock().samplesElapsed() + 1024)));

    for (int i = 0; i < 100; ++i)
        rig.host.processBlock();

    REQUIRE (rig.host.trackPlayState (0).playing);

    REQUIRE (rig.host.stopTrack (0));

    for (int i = 0; i < 20; ++i)
        rig.host.processBlock();

    CHECK_FALSE (rig.host.trackPlayState (0).playing);
}

//==============================================================================
/*  THE PHASE'S DONE-WHEN CLAUSE, in one process: GO makes a sound.

    Everything below the command is real - a generated Edit, a Tracktion
    playback graph, a launcher clip, Go.dot's own output plugin and its routing
    matrix, and a WAV written from what came out. What is faked is nothing.

    It is here rather than in the black-box driver because a failure here says
    WHICH layer broke, and because the black-box driver is a separate program
    that cannot be stepped through. PR 2.8 does the same thing from outside, on
    the shipped binary, over a socket; this is the version that fails usefully.
*/
TEST_CASE ("first sound: GO reaches the outputs the cue names, at the level it names")
{
    constexpr int rate = 48000;
    constexpr int blockSize = 128;

    HostRig rig;

    audio::HostSettings settings;
    settings.sampleRate = rate;
    settings.blockSize = blockSize;
    settings.outputChannels = 8;

    REQUIRE (rig.host.start (settings));

    audio::EditSpec spec;
    spec.tracks = 2;
    spec.channelsPerTrack = 1;
    REQUIRE (rig.host.buildEdit (spec));

    const auto tone = writeSteadyTone (rig.storage.folder, 1, rate);
    REQUIRE (tone.existsAsFile());

    //  --- a show: one media cue, routed to a bus at channels 4 and 5 ---------
    Engine engine;
    doc::ShowDocument document;
    cue::RunTable runs;
    cue::Focus focus;
    auto runIds = doc::IdRegistry::withSeed (3);
    cue::Runner runner { document, runs, runIds, focus };

    engine.log().openInMemory ({});
    doc::registerDocumentCommands (engine.commands(), document);
    cue::registerCueCommands (engine.commands(), document, focus);
    cue::registerRunCommands (engine.commands(), runs);
    cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);

    const auto listId = document.createList ("Sound").id;
    const auto cueId = document.createCue (listId, 0, "media", "Thunder").id;

    document.setAttribute ("/godot/cue/" + cueId + "/file",
                           tone.getFileName().toStdString());

    auto audioNode = document.root().getChildWithName ("Audio");
    audioNode.setProperty (juce::Identifier ("tracks"), 2, nullptr);

    juce::ValueTree bus { "Bus" };
    bus.setProperty (juce::Identifier ("id"), "J3MT5XYA", nullptr);
    bus.setProperty (juce::Identifier ("name"), "Foldback", nullptr);
    bus.setProperty (juce::Identifier ("firstChannel"), 4, nullptr);
    bus.setProperty (juce::Identifier ("width"), 2, nullptr);
    audioNode.appendChild (bus, nullptr);

    auto cue = document.findById (cueId);
    juce::ValueTree route { "Route" };
    route.setProperty (juce::Identifier ("id"), "Z04EH7PH", nullptr);
    route.setProperty (juce::Identifier ("bus"), "J3MT5XYA", nullptr);

    /*  One channel into two, at different gains, so a mistake in the row-major
        order or in the bus offset shows up as the wrong number in the wrong
        place rather than as silence. */
    route.setProperty (juce::Identifier ("gains"), "1 0.5", nullptr);
    cue.appendChild (route, nullptr);

    document.setAttribute (cue::standbyAddressOf (listId), cueId);

    //  --- the audio side ------------------------------------------------------
    audio::HostPlayer player { rig.host, engine };
    runner.setPlayer (&player);
    runner.setSamplesPerTick (rate / 50);
    runner.setMediaFolder (rig.storage.folder.getFullPathName().toStdString());

    PeakSink sink;
    sink.reset (settings.outputChannels);
    rig.host.setBlockSink (&sink);

    std::int64_t tick = 0;

    /*  One tick of the real loop: the Runner observes, the engine applies, the
        graph runs its share of blocks. The message loop is pumped too, because
        arming is posted to it - in a show that is the main thread, and here it
        is this one. */
    const auto oneTick = [&]
    {
        runner.beforeTick (engine, tick);
        engine.processTick (tick++);

        /*  The message thread's share, driven rather than waited for. In a
            show a timer does this; here the test does, which is the same
            function on the same thread. */
        player.serviceArms();

        for (int i = 0; i < (rate / 50) / blockSize; ++i)
            rig.host.processBlock();
    };

    for (int i = 0; i < 4; ++i)
        oneTick();

    REQUIRE (engine.submit ("udp:127.0.0.1:9000", "go", {}));

    /*  PUMPED UNTIL IT IS SOUNDING, not for a count, and this is the second
        time that distinction has cost a red build in this one test case.

        Between the GO and the first sample there is an arm queued to the
        message thread, a ValueTree write, a graph rebuild, and a disk read that
        Tracktion's cache does on a thread of its own. Every one of those is
        WALL-CLOCK, and the loop below is samples: how many ticks they add up to
        is the machine's answer, not this file's. Forty-five was enough on the
        Windows box and was not enough on a loaded macOS runner, where it failed
        as "the cue is not playing and the outputs are silent" - which reads
        exactly like a routing bug and is not one.

        The cue then needs to be still sounding when it is measured, so this
        stops as soon as it starts rather than running on. The tone is two
        seconds and the wait is bounded well inside it. */
    for (int i = 0; i < 400 && ! rig.host.trackPlayState (0).playing; ++i)
        oneTick();

    /*  And a few more, so what the sink holds is the cue's steady level rather
        than the first partial block of it. */
    for (int i = 0; i < 5; ++i)
        oneTick();

    rig.host.setBlockSink (nullptr);

    REQUIRE (runs.all().size() == 1u);
    const auto& run = runs.all().front();

    INFO ("run " << run.id << " state " << run.state
           << " track " << run.track << " error " << run.error);

    CHECK (run.state == cue::runState::playing);
    CHECK (run.track == 0);

    INFO ("what reached the output stage: " << rig.host.trackInputPeak (0));
    INFO ("peaks: " << sink[0] << " " << sink[1] << " " << sink[2] << " " << sink[3]
           << " " << sink[4] << " " << sink[5] << " " << sink[6] << " " << sink[7]);

    /*  THE CUE IS AUDIBLE, at the gains it was written with, on the channels the
        bus put it on. The tone is 0.5 in the file, so channel 4 carries 0.5 and
        channel 5 carries a quarter. */
    CHECK (sink[4] == doctest::Approx (0.5f).epsilon (0.02));
    CHECK (sink[5] == doctest::Approx (0.25f).epsilon (0.02));

    /*  And nowhere else. A cue that leaked into the main pair would still pass
        the two checks above. */
    for (const int silent : { 0, 1, 2, 3, 6, 7 })
    {
        INFO ("output channel " << silent << " should be silent");
        CHECK (sink[silent] < 0.001f);
    }

    /*  Standby was asked to move and had nowhere to go: one cue in the list, so
        it stays put rather than wrapping or clearing. That is the end-of-list
        rule, and GO applied either way. */
    CHECK (document.findById (listId)[juce::Identifier ("standby")].toString()
             == juce::String (cueId));

    const auto parsed = LogFile::parse (engine.log().contents());
    const auto go = std::find_if (parsed.records.begin(), parsed.records.end(),
                                  [] (const auto& r) { return r.command == "go"; });

    REQUIRE (go != parsed.records.end());
    REQUIRE_FALSE (go->args.empty());
    CHECK (go->args[0].getString() == run.id);

    //  --- and it ends, and stays ended ---------------------------------------
    /*  WRITTEN BECAUSE OF SOMETHING I SAW ONCE AND COULD NOT REPRODUCE. An
        earlier version of this case measured after the tone had finished and
        read TWICE the file's amplitude. The live render shows a clean level
        throughout and nothing here reproduces it, so rather than leave an
        unexplained observation lying about, this pins the property that would
        have caught it: when a cue ends it goes silent, it stays silent, and it
        does not come back at any level at all.

        A cue that replayed itself at the end - or summed with itself - would be
        the worst kind of show fault, because it happens after the moment
        anybody is watching. */
    /*  PUMPED UNTIL IT FINISHES RATHER THAN FOR A FIXED COUNT, and the
        difference is a flake this caught. A tick here is 960 samples and a
        block is 128, so the loop pumps seven blocks where a tick is seven and a
        half - it runs 7% slow, and a fixed count that was just enough passed
        one run in two. Waiting for the condition says what the case means and
        does not depend on arithmetic about the test. */
    for (int i = 0; i < 500 && ! runs.all().front().isFinished(); ++i)
        oneTick();

    REQUIRE (runs.all().front().isFinished());

    sink.reset (settings.outputChannels);

    for (int i = 0; i < 90; ++i)
        oneTick();

    INFO ("after the cue finished: " << sink[4] << " " << sink[5]);

    for (int channel = 0; channel < settings.outputChannels; ++channel)
    {
        INFO ("channel " << channel << " after the end");
        CHECK (sink[channel] < 0.001f);
    }
}

//==============================================================================
/*  M5 - WHERE THE SOUND ACTUALLY STARTS.

    Everything else about GO is arithmetic that can be checked on its own. This
    is the one measurement that says the arithmetic and the graph agree: a
    launch placed at sample N produces its first non-zero sample at N, in the
    audio that came out.

    It matters more than it looks. A launch that lands late does not merely
    start late - Tracktion renders the block in hand from the head of the file
    and back-dates only the blocks after it, so a late cue is late AND has a
    hole in it. The failure is a click and a shortened cue, on a machine nobody
    can reproduce it on, and the only way to know it is not happening is to
    measure where the sound begins.

    Five block sizes at three rates, including 44.1 kHz with 512-sample blocks,
    where a tick is 882 samples and a block is more than half of one.
*/
namespace
{
    /*  Records every sample the rig produced, so a test can ask WHERE something
        happened rather than only whether it did.

        Sized once, before anything runs. Appending on the audio thread would
        allocate, which is the rule this suite exists to protect. */
    struct RecordingSink final : audio::BlockSink
    {
        void prepare (int numChannels, int numFrames)
        {
            buffer.setSize (numChannels, numFrames);
            buffer.clear();
            written = 0;
        }

        void blockProduced (const float* const* channels, int numChannels,
                            int numSamples) noexcept override
        {
            const auto room = buffer.getNumSamples() - written;
            const auto frames = std::min (numSamples, room);

            if (frames <= 0)
                return;

            for (int channel = 0; channel < std::min (numChannels, buffer.getNumChannels()); ++channel)
                juce::FloatVectorOperations::copy (buffer.getWritePointer (channel, written),
                                                   channels[channel], frames);

            written += frames;
        }

        /** The first frame whose magnitude clears the floor, or -1. */
        int firstSoundAt (int channel, float floorLevel = 0.01f) const
        {
            const auto* samples = buffer.getReadPointer (channel);

            for (int n = 0; n < written; ++n)
                if (std::abs (samples[n]) > floorLevel)
                    return n;

            return -1;
        }

        juce::AudioBuffer<float> buffer;
        int written = 0;
    };

    struct LandingResult
    {
        std::int64_t expected = 0;
        std::int64_t actual = 0;
        std::int64_t error() const { return actual - expected; }
    };

    /*  Places one launch at a sample of Go.dot's own choosing and reports where
        the sound actually began. */
    LandingResult measureLanding (int rate, int blockSize)
    {
        LandingResult result;

        HostRig rig;

        audio::HostSettings settings;
        settings.sampleRate = rate;
        settings.blockSize = blockSize;
        settings.outputChannels = 2;

        REQUIRE (rig.host.start (settings));

        audio::EditSpec spec;
        spec.tracks = 1;
        spec.channelsPerTrack = 1;
        REQUIRE (rig.host.buildEdit (spec));

        const auto tone = writeSteadyTone (rig.storage.folder, 1, rate);
        REQUIRE (rig.host.setTrackSource (0, tone.getFullPathName().toStdString()));
        REQUIRE (rig.host.waitForTrackSourceReady (0, 10000));

        auto* matrix = rig.host.trackMatrix (0);
        REQUIRE (matrix != nullptr);
        matrix->setLevelDb (0.0f);
        matrix->setGain (0, 0, 1.0f);
        matrix->snapToTargets();

        /*  A constant tone, so the first sample that is not zero IS the moment
            the cue started - no attack to guess at and nothing to threshold. */
        RecordingSink sink;
        sink.prepare (settings.outputChannels, rate);       // one second is plenty

        /*  The anchor needs blocks before it means anything. */
        for (int i = 0; i < 8; ++i)
            rig.host.processBlock();

        rig.host.setBlockSink (&sink);

        const auto recordingBegan = rig.host.clock().samplesElapsed();

        /*  THE LAUNCH INSTANT, chosen the way the Runner chooses it: a whole
            number of ticks ahead, at the rule's own distance. */
        const auto samplesPerTick = rate / 50;
        const auto ticksAhead = cue::launchLatencyTicks (blockSize, samplesPerTick);

        result.expected = rig.host.clock().samplesElapsed()
                            + static_cast<std::int64_t> (ticksAhead) * samplesPerTick;

        REQUIRE (rig.host.launchTrackAt (0, rig.host.beatsAtSample (result.expected)));

        const auto blocks = (rate / 2) / blockSize;

        for (int i = 0; i < blocks; ++i)
            rig.host.processBlock();

        rig.host.setBlockSink (nullptr);

        const auto found = sink.firstSoundAt (0);
        REQUIRE (found >= 0);

        result.actual = recordingBegan + found;
        return result;
    }
}

TEST_CASE ("M5: the sound starts on the sample the launch was placed at")
{
    /*  ONE BLOCK OF TOLERANCE, and not because the arithmetic is approximate.
        Tracktion splits a launch inside a block to the nearest frame, so an
        exactly-placed instant lands exactly - but the audio thread reads the
        launch queue through a try-lock, and on a block where it misses, the
        launch is seen one block later. That is the only slack in the system and
        it is one-sided: a launch may be up to a block LATE and can never be
        early. Anything outside that is a defect in the arithmetic. */
    for (const int rate : { 44100, 48000, 96000 })
    {
        for (const int blockSize : { 64, 128, 256, 512, 1024 })
        {
            INFO ("at " << rate << " Hz with " << blockSize << "-sample blocks");

            const auto landing = measureLanding (rate, blockSize);

            INFO ("placed at " << landing.expected << ", started at " << landing.actual
                   << ", error " << landing.error() << " samples ("
                   << (1000.0 * static_cast<double> (landing.error()) / rate) << " ms)");

            /*  Never early. A cue that started before it was asked to would
                mean the instant was computed against the wrong clock. */
            CHECK (landing.error() >= 0);

            /*  And never more than one block late. */
            CHECK (landing.error() <= blockSize);
        }
    }
}

//==============================================================================
/*  M4 - DOES ARMING ONE CUE DISTURB ANOTHER THAT IS ALREADY SOUNDING?

    It is the question the whole arming design rests on and the one nothing so
    far has asked. Pointing a clip at a file writes a Tracktion ValueTree, and
    that REBUILDS THE PLAYBACK GRAPH - every WaveNode and every file reader is
    made again, while a cue is playing through the old one. Spike 04 measured
    that a rebuild happens; what it never measured was what it does to audio in
    flight.

    A show does this constantly. Standby moves to the next cue while the last
    one is still sounding, so an arm during playback is the ordinary case rather
    than the awkward one - which means a dropout here would be a click in the
    middle of every cue, on every show, and nobody would know where it came
    from.

    The method is a null test, which is the only kind that can answer it: render
    the same cue twice, once with an arm in the middle and once without, and
    subtract. Anything the rebuild did to the audio is what is left.
*/
namespace
{
    /*  Plays one cue and records it, optionally arming a second track partway
        through. Everything is driven block by block, so the two runs differ in
        exactly one thing. */
    void renderWithOptionalArm (bool armDuringPlayback, RecordingSink& sink,
                                juce::File& toneOut)
    {
        constexpr int rate = 48000;
        constexpr int blockSize = 128;

        HostRig rig;

        audio::HostSettings settings;
        settings.sampleRate = rate;
        settings.blockSize = blockSize;
        settings.outputChannels = 2;

        REQUIRE (rig.host.start (settings));

        audio::EditSpec spec;
        spec.tracks = 2;
        spec.channelsPerTrack = 1;
        REQUIRE (rig.host.buildEdit (spec));

        const auto tone = writeSteadyTone (rig.storage.folder, 1, rate);
        toneOut = tone;

        REQUIRE (rig.host.setTrackSource (0, tone.getFullPathName().toStdString()));
        REQUIRE (rig.host.waitForTrackSourceReady (0, 10000));

        auto* matrix = rig.host.trackMatrix (0);
        REQUIRE (matrix != nullptr);
        matrix->setLevelDb (0.0f);
        matrix->setGain (0, 0, 1.0f);
        matrix->snapToTargets();

        for (int i = 0; i < 8; ++i)
            rig.host.processBlock();

        /*  Launched at a fixed distance from where the counter is, so both runs
            have the same shape however long the disk took to answer. */
        const auto target = rig.host.clock().samplesElapsed() + 4 * (rate / 50);
        REQUIRE (rig.host.launchTrackAt (0, rig.host.beatsAtSample (target)));

        /*  Recording starts after the launch is placed and runs across it, so
            the comparison covers the start of the cue as well as its middle. */
        sink.prepare (settings.outputChannels, rate);
        rig.host.setBlockSink (&sink);

        constexpr int totalBlocks = 300;
        constexpr int armAtBlock = 200;          // well inside the sounding part

        for (int block = 0; block < totalBlocks; ++block)
        {
            if (armDuringPlayback && block == armAtBlock)
            {
                /*  THE THING BEING MEASURED. A ValueTree write on a second
                    track, which rebuilds the graph the first one is playing
                    through. */
                REQUIRE (rig.host.setTrackSource (1, tone.getFullPathName().toStdString()));
            }

            rig.host.processBlock();
        }

        rig.host.setBlockSink (nullptr);

        /*  The cue really was sounding for the whole comparison, or the null
            test would be comparing two silences and passing. */
        REQUIRE (rig.host.trackPlayState (0).playing);
    }
}

TEST_CASE ("M4: arming a second cue does not disturb the one already playing")
{
    RecordingSink quiet, armed;
    juce::File toneA, toneB;

    renderWithOptionalArm (false, quiet, toneA);
    renderWithOptionalArm (true, armed, toneB);

    REQUIRE (quiet.written > 0);
    REQUIRE (quiet.written == armed.written);

    /*  The cue is audible in both, so a difference of zero means something. */
    REQUIRE (quiet.firstSoundAt (0) >= 0);
    REQUIRE (armed.firstSoundAt (0) >= 0);
    CHECK (quiet.firstSoundAt (0) == armed.firstSoundAt (0));

    /*  THE NULL TEST. Sample by sample, both channels. Whatever the graph
        rebuild did to audio in flight is the difference, and there should not
        be one. */
    double worst = 0.0;
    int worstAt = -1;

    for (int channel = 0; channel < quiet.buffer.getNumChannels(); ++channel)
    {
        const auto* a = quiet.buffer.getReadPointer (channel);
        const auto* b = armed.buffer.getReadPointer (channel);

        for (int n = 0; n < quiet.written; ++n)
        {
            const auto difference = std::abs (static_cast<double> (a[n]) - b[n]);

            if (difference > worst)
            {
                worst = difference;
                worstAt = n;
            }
        }
    }

    INFO ("largest difference " << worst << " at sample " << worstAt
           << " of " << quiet.written
           << " (" << (worstAt >= 0 ? 1000.0 * worstAt / 48000.0 : 0.0) << " ms in)");

    /*  BIT-IDENTICAL, and it is worth being exact about why that is the right
        bar rather than "small". The two renders run the same graph over the
        same file with the same coefficients; every sample is a copy or a
        multiply by a constant. There is no summation order to differ and no
        interpolation to round. If the rebuild has not touched the playing cue,
        the two are the same numbers - and if they are merely CLOSE, something
        happened and got smoothed over. */
    CHECK (juce::exactlyEqual (worst, 0.0));
}

//==============================================================================
/*  M6 - WHERE THE SOUND ACTUALLY ENDS.

    M5's twin, and a separate measurement because it exercises different
    Tracktion code - code that, unlike the launch path, has no field history
    behind it at all. Every launcher-based project starts clips; placing a STOP
    at an instant of its own choosing is what a show does and what a loop pedal
    never needs, so `LaunchHandle::stop (beat)` arrives here untested by
    anybody else's use.

    IT MATTERS FROM THE OTHER END, for the same reason M5 does. A stop that
    lands late leaves a cue running past the moment the show says it ends - a
    tail over the top of the next scene, which is the fault operators describe
    as "it didn't stop". A stop that lands early truncates the cue. And because
    a fade ARRIVES at a stop, a stop that does not land where it was placed
    makes the fade before it a fade to somewhere else.

    WHAT IS MEASURED. The source is DC, so the output is a constant and the
    first sample that is not at full level IS the moment the cue stopped - no
    attack, no decay, nothing to threshold. Tracktion adds a ten-sample decaying
    tail from the stop point (`SampleFader`, triggered in
    `ArrangerLauncherSwitchingNode` - its click suppression, which a hard stop
    is allowed to take), which is why the case looks for the DEPARTURE from full
    level rather than the arrival at zero.
*/
namespace
{
    /** The first frame whose magnitude falls below a level, or -1. */
    int firstBelow (const RecordingSink& sink, int channel, float level)
    {
        const auto* samples = sink.buffer.getReadPointer (channel);

        for (int n = 0; n < sink.written; ++n)
            if (std::abs (samples[n]) < level)
                return n;

        return -1;
    }

    /*  Plays a cue, places one stop at a sample of Go.dot's own choosing, and
        reports where the sound actually ended. */
    LandingResult measureStopLanding (int rate, int blockSize)
    {
        LandingResult result;

        HostRig rig;

        audio::HostSettings settings;
        settings.sampleRate = rate;
        settings.blockSize = blockSize;
        settings.outputChannels = 2;

        REQUIRE (rig.host.start (settings));

        audio::EditSpec spec;
        spec.tracks = 1;
        spec.channelsPerTrack = 1;
        REQUIRE (rig.host.buildEdit (spec));

        const auto tone = writeSteadyTone (rig.storage.folder, 1, rate);
        REQUIRE (rig.host.setTrackSource (0, tone.getFullPathName().toStdString()));
        REQUIRE (rig.host.waitForTrackSourceReady (0, 10000));

        auto* matrix = rig.host.trackMatrix (0);
        REQUIRE (matrix != nullptr);
        matrix->setLevelDb (0.0f);
        matrix->setGain (0, 0, 1.0f);
        matrix->snapToTargets();

        for (int i = 0; i < 8; ++i)
            rig.host.processBlock();

        const auto samplesPerTick = rate / 50;
        const auto ticksAhead = cue::launchLatencyTicks (blockSize, samplesPerTick);

        REQUIRE (rig.host.launchTrackAt (0, rig.host.beatsAtSample (
                   rig.host.clock().samplesElapsed()
                     + static_cast<std::int64_t> (ticksAhead) * samplesPerTick)));

        /*  Until it is really sounding rather than for a count, because how
            long the disk takes is wall-clock and this loop is samples. */
        for (int i = 0; i < 2000 && ! rig.host.trackPlayState (0).playing; ++i)
            rig.host.processBlock();

        REQUIRE (rig.host.trackPlayState (0).playing);

        RecordingSink sink;
        sink.prepare (settings.outputChannels, rate / 2);
        rig.host.setBlockSink (&sink);

        const auto recordingBegan = rig.host.clock().samplesElapsed();

        /*  A few blocks at full level first, so the recording has something to
            depart FROM and the case can read the level rather than assume it. */
        for (int i = 0; i < 4; ++i)
            rig.host.processBlock();

        /*  THE STOP INSTANT, placed the way the Runner places one: the same
            rule as a launch, because the queue, the try-lock and the block in
            flight are the same on the way out as on the way in. */
        result.expected = rig.host.clock().samplesElapsed()
                            + static_cast<std::int64_t> (ticksAhead) * samplesPerTick;

        REQUIRE (rig.host.stopTrackAt (0, rig.host.beatsAtSample (result.expected)));

        const auto blocks = (rate / 4) / blockSize;

        for (int i = 0; i < blocks; ++i)
            rig.host.processBlock();

        rig.host.setBlockSink (nullptr);

        const auto full = std::abs (sink.buffer.getSample (0, 8));
        REQUIRE (full > 0.4f);

        const auto found = firstBelow (sink, 0, full * 0.99f);
        REQUIRE (found >= 0);

        result.actual = recordingBegan + found;

        /*  It really stopped rather than dipped: the end of the recording is
            digitally silent, and the handle agrees. */
        CHECK_FALSE (rig.host.trackPlayState (0).playing);

        for (int n = sink.written - 64; n < sink.written; ++n)
            REQUIRE (juce::exactlyEqual (sink.buffer.getSample (0, n), 0.0f));

        return result;
    }
}

TEST_CASE ("M6: a stop lands on the sample it was placed at")
{
    /*  THE SAME TOLERANCE AS M5 AND FOR THE SAME REASON, plus Tracktion's
        tail. The launch queue and the stop queue are one spin-locked state that
        the audio thread reads through a try-lock, so a stop may be seen one
        block later than it was placed and can never be seen early. The extra
        ten samples are the click suppressor's ramp, whose first frame is at
        full level by construction - so the departure can be a sample or two
        beyond the instant even when the split was exact. */
    for (const int rate : { 44100, 48000, 96000 })
    {
        for (const int blockSize : { 64, 256, 1024 })
        {
            INFO ("at " << rate << " Hz with " << blockSize << "-sample blocks");

            const auto landing = measureStopLanding (rate, blockSize);

            INFO ("placed at " << landing.expected << ", ended at " << landing.actual
                   << ", error " << landing.error() << " samples ("
                   << (1000.0 * static_cast<double> (landing.error()) / rate) << " ms)");

            /*  Never early. A cue that stopped before it was asked to would be
                a truncated cue, and the arithmetic would be against the wrong
                clock. */
            CHECK (landing.error() >= 0);

            /*  And never more than one block plus the suppressor's tail late. */
            CHECK (landing.error() <= blockSize + 10);
        }
    }
}

//==============================================================================
/*  M7 - THE FADE, AS IT COMES OUT OF THE GRAPH.

    The arithmetic of the curve is checked on its own in GoTests, and the run's
    level is checked there too. Neither can tell you that what a designer drew
    is what a room hears. Between the two sits everything this case exists for:
    fifty values a second written by the tick thread, a smoother on the audio
    thread interpolating between them, and a block size that has nothing to do
    with either.

    THE MEASUREMENT IS EXACT RATHER THAN APPROXIMATE, which is worth saying
    because a rendered envelope sounds like the sort of thing you can only check
    loosely. The source is DC, so the output sample IS the gain. The smoother
    ramps over exactly one tick of samples and JUCE lands its last step ON the
    target rather than accumulating into it. So the last sample of every tick is
    the level the Runner wrote one tick earlier, to the bit - and the case can
    compare a rendered envelope against the curve that produced it, tick by
    tick, and demand a fiftieth of a decibel.

    WHAT WOULD BREAK IT, and each is a real bug rather than a hypothetical. A
    smoother longer than a tick would lag and round the corners off the curve -
    the reason `levelSlewSeconds` is what it is. A smoother shorter than a tick
    would arrive early and hold, turning a fade into fifty steps. Interpolating
    the dB rather than the gain would put the curve somewhere else entirely. And
    a fade-and-stop that stopped before it was silent would leave a step in the
    audio, which is a click, which is the thing the ordering exists to prevent.
*/
namespace
{
    struct FadeScenario
    {
        /*  The document's own spelling, because these go through setAttribute
            and the suite runs under fr_FR: a number formatted here rather than
            written here would be the test formatting it, not Go.dot. */
        const char* level = "-20";
        const char* duration = "1";
        const char* curve = "linear";

        /** Null for a Fade cue; `hard` or `fade` for a Stop cue. */
        const char* verb = nullptr;

        double toDb = -20.0;
        double seconds = 1.0;
    };

    struct FadeRender
    {
        /*  The first sample of the fade's own audio: the tick that applied the
            GO has been rendered, and the level has not moved yet. */
        int fadeBegan = 0;

        /** The level the cue was sounding at before the fade, as rendered. */
        float steady = 0.0f;

        int samplesPerTick = 0;
        int fadeTicks = 0;

        bool targetPlaying = false;
        bool targetFinished = false;
        double targetLevelDb = 0.0;
    };

    /*  Stands the whole stack up - a document, a Runner, a real Tracktion
        graph, Go.dot's output stage - plays a DC tone, fires one fade or stop
        cue at it, and records every sample that came out.
    */
    FadeRender renderFade (RecordingSink& sink, const FadeScenario& scenario)
    {
        FadeRender result;

        constexpr int rate = 48000;

        /*  A BLOCK THAT DIVIDES A TICK EXACTLY, and it is not tidiness. The
            level smoother ramps over one tick of SAMPLES; a rig that pumped a
            whole number of blocks adding up to less than a tick would cut every
            ramp short, and the shortfall would compound down the whole fade
            into a rendered curve that is not the one asked for. At 48 kHz a
            tick is 960 samples and fifteen 64-sample blocks are exactly that.
            What is being measured is Go.dot, not the test's arithmetic. */
        constexpr int blockSize = 64;

        result.samplesPerTick = rate / 50;
        const auto blocksPerTick = result.samplesPerTick / blockSize;

        HostRig rig;

        audio::HostSettings settings;
        settings.sampleRate = rate;
        settings.blockSize = blockSize;
        settings.outputChannels = 2;

        REQUIRE (rig.host.start (settings));

        audio::EditSpec spec;
        spec.tracks = 1;
        spec.channelsPerTrack = 1;
        REQUIRE (rig.host.buildEdit (spec));

        const auto tone = writeSteadyTone (rig.storage.folder, 1, rate, 4);
        REQUIRE (tone.existsAsFile());

        //  --- a show: one media cue, and one cue that acts on it -------------
        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::Focus focus;
        auto runIds = doc::IdRegistry::withSeed (11);
        cue::Runner runner { document, runs, runIds, focus };

        engine.log().openInMemory ({});
        doc::registerDocumentCommands (engine.commands(), document);
        cue::registerCueCommands (engine.commands(), document, focus);
        cue::registerRunCommands (engine.commands(), runs);
        cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);

        const auto listId = document.createList ("Sound").id;
        const auto mediaId = document.createCue (listId, 0, "media", "Rain").id;

        document.setAttribute ("/godot/cue/" + mediaId + "/file",
                               tone.getFileName().toStdString());

        auto audioNode = document.root().getChildWithName ("Audio");
        audioNode.setProperty (juce::Identifier ("tracks"), 1, nullptr);

        juce::ValueTree bus { "Bus" };
        bus.setProperty (juce::Identifier ("id"), "J3MT5XYA", nullptr);
        bus.setProperty (juce::Identifier ("name"), "Main", nullptr);
        bus.setProperty (juce::Identifier ("firstChannel"), 0, nullptr);
        bus.setProperty (juce::Identifier ("width"), 1, nullptr);
        audioNode.appendChild (bus, nullptr);

        auto media = document.findById (mediaId);
        juce::ValueTree route { "Route" };
        route.setProperty (juce::Identifier ("id"), "Z04EH7PH", nullptr);
        route.setProperty (juce::Identifier ("bus"), "J3MT5XYA", nullptr);
        route.setProperty (juce::Identifier ("gains"), "1", nullptr);
        media.appendChild (route, nullptr);

        const auto moverId = document.createCue (listId, 1,
                                                 scenario.verb == nullptr ? "fade" : "stop",
                                                 "Out").id;

        const auto attribute = [&] (const char* name, const std::string& value)
        {
            document.setAttribute ("/godot/cue/" + moverId + "/" + name, value);
        };

        attribute ("target", mediaId);
        attribute ("duration", scenario.duration);
        attribute ("curve", scenario.curve);

        if (scenario.verb != nullptr)
            attribute ("verb", scenario.verb);
        else
            attribute ("level", scenario.level);

        document.setAttribute (cue::standbyAddressOf (listId), mediaId);

        //  --- the audio side --------------------------------------------------
        audio::HostPlayer player { rig.host, engine };
        runner.setPlayer (&player);
        runner.setSamplesPerTick (result.samplesPerTick);
        runner.setMediaFolder (rig.storage.folder.getFullPathName().toStdString());

        std::int64_t tick = 0;

        const auto oneTick = [&]
        {
            runner.beforeTick (engine, tick);
            engine.processTick (tick++);
            player.serviceArms();

            for (int i = 0; i < blocksPerTick; ++i)
                rig.host.processBlock();
        };

        for (int i = 0; i < 4; ++i)
            oneTick();

        REQUIRE (engine.submit ("udp:127.0.0.1:9000", "go", {}));

        for (int i = 0; i < 600 && ! rig.host.trackPlayState (0).playing; ++i)
            oneTick();

        REQUIRE (rig.host.trackPlayState (0).playing);
        REQUIRE (runs.all().size() == 1u);

        const auto mediaRun = runs.all().front().id;

        /*  Steady before anything moves, so the departure is measurable and the
            level it left is a reading rather than an assumption. */
        for (int i = 0; i < 3; ++i)
            oneTick();

        sink.prepare (settings.outputChannels, rate * 3);
        rig.host.setBlockSink (&sink);

        for (int i = 0; i < 3; ++i)
            oneTick();

        REQUIRE (engine.submit ("udp:127.0.0.1:9000", "cue.fire",
                                { osc::Value::string (moverId) }));

        /*  The tick that APPLIES the fade renders at the old level - the job
            exists but has advanced no ticks - so the fade's own audio begins
            after it. */
        oneTick();

        result.fadeBegan = sink.written;
        result.steady = sink.buffer.getSample (0, result.fadeBegan - 1);
        result.fadeTicks = static_cast<int> (std::lround (scenario.seconds * 50.0));

        for (int i = 0; i < result.fadeTicks + 12; ++i)
            oneTick();

        rig.host.setBlockSink (nullptr);

        result.targetPlaying = rig.host.trackPlayState (0).playing;

        if (const auto* run = runs.find (mediaRun))
        {
            result.targetFinished = run->isFinished();
            result.targetLevelDb = run->level;
        }

        return result;
    }

    /** The rendered level at a sample, in dB below where the cue was sitting. */
    double renderedDb (const RecordingSink& sink, int at, float steady)
    {
        const auto ratio = std::abs (static_cast<double> (sink.buffer.getSample (0, at)))
                             / static_cast<double> (steady);

        return ratio <= 0.0 ? -1000.0 : 20.0 * std::log10 (ratio);
    }

    /** The largest jump between two consecutive rendered samples. */
    double largestStep (const RecordingSink& sink, int from, int to)
    {
        double worst = 0.0;

        for (int n = from + 1; n < to; ++n)
            worst = std::max (worst,
                              std::abs (static_cast<double> (sink.buffer.getSample (0, n))
                                          - sink.buffer.getSample (0, n - 1)));

        return worst;
    }

    /*  WHAT FLOAT ARITHMETIC ITSELF PUTS INTO A RAMP, in output units.

        JUCE accumulates a linear ramp by repeated addition and then SNAPS the
        last step onto the target rather than letting it arrive - so the whole
        of the rounding drift accumulated across a tick is spent in ONE sample
        at the end of it. That is a real step in the audio, it is measured here
        at around -96 dBFS, and any bound on the rendered steps has to allow for
        it.

        The number is derived rather than fitted: each addition rounds by at
        most half an ulp of a value near unity, so a ramp of `samplesPerTick`
        additions cannot drift by more than that many halves of an epsilon. It
        is four orders of magnitude below the steps this case exists to catch -
        a fade arriving as fifty jumps, or a stop taken at full level - so
        allowing it costs the measurement nothing. */
    double rampRoundingNoise (float steady, int samplesPerTick)
    {
        return static_cast<double> (steady) * std::numeric_limits<float>::epsilon()
                 * samplesPerTick * 0.5;
    }

    /*  The largest per-sample step the design ALLOWS, derived from the same
        curve the fade is running: the biggest gain change between two of the
        fifty values a second, spread over the tick the smoother has to cross it
        in. A rendered step larger than this is a discontinuity - something
        arriving in one sample that should have taken a tick. */
    double allowedStepPerSample (double toDb, int ticks, cue::FadeCurve curve,
                                 int samplesPerTick)
    {
        const auto gainAt = [] (double db)
        {
            return static_cast<double> (juce::Decibels::decibelsToGain (
                     static_cast<float> (db), audio::CueMatrix::silenceDb));
        };

        double worst = 0.0;
        auto previous = gainAt (0.0);

        for (int k = 1; k <= ticks; ++k)
        {
            const auto now = gainAt (cue::fadeLevelDb (0.0, toDb,
                                                       static_cast<double> (k) / ticks,
                                                       curve));
            worst = std::max (worst, std::abs (now - previous));
            previous = now;
        }

        return worst / samplesPerTick;
    }
}

TEST_CASE ("M7: a rendered fade is the curve it was given, tick by tick")
{
    for (const char* shape : { "linear", "sCurve" })
    {
        INFO ("curve " << std::string (shape));

        FadeScenario scenario;
        scenario.curve = shape;

        RecordingSink sink;
        const auto render = renderFade (sink, scenario);
        const auto curve = cue::fadeCurveFrom (shape);

        REQUIRE (render.steady > 0.4f);
        REQUIRE (render.fadeTicks == 50);
        REQUIRE (sink.written >= render.fadeBegan + render.fadeTicks * render.samplesPerTick);

        /*  BOTH ENDPOINTS. The level it left is the level it was sounding at,
            and the level it arrives at is the one the cue names - not near it,
            because a fade that stopped a decibel short would leave every cue in
            a show a decibel loud.

            Compared as a difference rather than through doctest::Approx, whose
            tolerance is RELATIVE: a relative tolerance around zero decibels can
            never be met, and around minus twenty it would be twenty times
            looser than around one. A fade is measured in decibels, so the
            tolerance is stated in decibels. */
        CHECK (std::abs (renderedDb (sink, render.fadeBegan - 1, render.steady)) < 0.001);

        const auto arrival = render.fadeBegan + render.fadeTicks * render.samplesPerTick - 1;

        INFO ("arrived at " << renderedDb (sink, arrival, render.steady) << " dB");
        CHECK (std::abs (renderedDb (sink, arrival, render.steady) - scenario.toDb) < 0.01);

        /*  AND EVERY VALUE IN BETWEEN. The last sample of each tick is the
            level the Runner wrote one tick before, so this compares fifty
            rendered numbers against the curve that produced them. */
        double worstDeviation = 0.0;
        int worstAt = 0;

        for (int k = 1; k <= render.fadeTicks; ++k)
        {
            const auto at = render.fadeBegan + k * render.samplesPerTick - 1;
            const auto ideal = cue::fadeLevelDb (0.0, scenario.toDb,
                                                 static_cast<double> (k) / render.fadeTicks,
                                                 curve);
            const auto deviation = std::abs (renderedDb (sink, at, render.steady) - ideal);

            if (deviation > worstDeviation)
            {
                worstDeviation = deviation;
                worstAt = k;
            }
        }

        INFO ("worst deviation " << worstDeviation << " dB, at tick " << worstAt
               << " of " << render.fadeTicks);

        CHECK (worstDeviation < 0.02);

        /*  MONOTONIC IN THE AUDIO, not only in the arithmetic. A curve that is
            monotonic and an interpolation that overshot between its points
            would still put a level nobody asked for into the room.

            Above the rounding noise rather than above zero, because the snap at
            the end of each tick corrects a drift that can go either way - and
            an sCurve, whose first tick barely moves, spends more of its step on
            that correction than on the fade. An overshoot worth the name is
            orders of magnitude larger. */
        const auto* samples = sink.buffer.getReadPointer (0);
        const auto noise = rampRoundingNoise (render.steady, render.samplesPerTick);
        int roseAt = -1;

        for (int n = render.fadeBegan + 1; n <= arrival && roseAt < 0; ++n)
            if (static_cast<double> (samples[n]) - samples[n - 1] > noise)
                roseAt = n;

        INFO ("first sample that rose: " << roseAt);
        CHECK (roseAt < 0);

        /*  NO STEP BEYOND WHAT THE SLEW ALLOWS. Fifty values a second reaching
            the audio as fifty steps would be a fade that ticks, and the bound
            is derived from the same curve rather than chosen. */
        const auto step = largestStep (sink, render.fadeBegan, arrival);
        const auto allowed = static_cast<double> (render.steady)
                               * allowedStepPerSample (scenario.toDb, render.fadeTicks,
                                                       curve, render.samplesPerTick)
                               + noise;

        INFO ("largest step " << step << ", allowed " << allowed);
        CHECK (step <= allowed);

        /*  The fade moved the run's level and left it where the cue said. */
        CHECK (std::abs (render.targetLevelDb - scenario.toDb) < 1.0e-9);

        /*  A fade is not a stop: the cue is still sounding, quietly. */
        CHECK (render.targetPlaying);
        CHECK_FALSE (render.targetFinished);
    }
}

TEST_CASE ("M7: a fade to silence renders digital silence, not a very small number")
{
    /*  -120 dB IS ZERO, and the difference is not academic. A cue left at some
        tiny gain is a cue still summing into every output for the rest of the
        show: it costs what a loud one costs, it denormalises, and on a rig with
        sixty-four of them it is noise. decibelsToGain is given the floor so the
        arithmetic reaches exactly zero rather than approaching it. */
    FadeScenario scenario;
    scenario.level = "-120";
    scenario.duration = "0.5";
    scenario.toDb = -120.0;
    scenario.seconds = 0.5;

    RecordingSink sink;
    const auto render = renderFade (sink, scenario);

    REQUIRE (render.steady > 0.4f);

    const auto arrival = render.fadeBegan + render.fadeTicks * render.samplesPerTick - 1;
    REQUIRE (sink.written > arrival + render.samplesPerTick);

    /*  Halfway down it is still audible, so what follows is a fade that
        happened rather than a cue that was never there. */
    const auto halfway = render.fadeBegan + (render.fadeTicks / 2) * render.samplesPerTick - 1;

    INFO ("halfway: " << renderedDb (sink, halfway, render.steady) << " dB");
    CHECK (renderedDb (sink, halfway, render.steady) < -40.0);
    CHECK (renderedDb (sink, halfway, render.steady) > -80.0);

    /*  And at the bottom, exactly nothing - every sample of it. */
    for (int n = arrival; n < sink.written; ++n)
    {
        INFO ("sample " << n << " of " << sink.written);
        REQUIRE (juce::exactlyEqual (sink.buffer.getSample (0, n), 0.0f));
    }

    /*  Still running, at silence. A fade to -120 is not a stop, and the
        difference matters to whatever is waiting on the run. */
    CHECK (render.targetPlaying);
}

TEST_CASE ("M7: a stop that fades is silent before it stops, so there is nothing to click")
{
    /*  THE ORDER IS THE WHOLE POINT OF THE VERB. Tracktion ramps ten samples
        out of a clip it stops, from whatever level the clip was at - which for
        a stop at full level is a tenth of the signal gone in one sample, and
        that is a click. The `fade` verb exists so that by the time the clip
        stops there is nothing left for that ramp to ramp, and the way to assert
        it is to go looking for the click: no step anywhere in the render bigger
        than the fade's own slew allows, right through the stop. */
    FadeScenario scenario;
    scenario.duration = "0.5";
    scenario.curve = "linear";
    scenario.verb = "fade";
    scenario.toDb = -120.0;
    scenario.seconds = 0.5;

    RecordingSink sink;
    const auto render = renderFade (sink, scenario);

    REQUIRE (render.steady > 0.4f);

    const auto arrival = render.fadeBegan + render.fadeTicks * render.samplesPerTick - 1;
    REQUIRE (sink.written > arrival + render.samplesPerTick);

    /*  It faded rather than jumped: audible at the top, gone at the bottom. */
    CHECK (renderedDb (sink, render.fadeBegan, render.steady) > -1.0);

    for (int n = arrival; n < sink.written; ++n)
    {
        INFO ("sample " << n << " of " << sink.written << ", after the fade arrived");
        REQUIRE (juce::exactlyEqual (sink.buffer.getSample (0, n), 0.0f));
    }

    /*  NO CLICK, ANYWHERE - and the stop is inside this range, so this is the
        assertion the ordering exists for. */
    const auto step = largestStep (sink, render.fadeBegan, sink.written);
    const auto allowed = static_cast<double> (render.steady)
                           * allowedStepPerSample (scenario.toDb, render.fadeTicks,
                                                   cue::FadeCurve::linear,
                                                   render.samplesPerTick)
                           + rampRoundingNoise (render.steady, render.samplesPerTick);

    INFO ("largest step " << step << ", allowed " << allowed);
    CHECK (step <= allowed);

    /*  And it stopped. A fade to silence that left the clip running would pass
        every check above and hold a voice for the rest of the show. */
    CHECK_FALSE (render.targetPlaying);
    CHECK (render.targetFinished);
}
