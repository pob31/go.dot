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
#include <wfg/engine/audio/AudioHost.h>
#include <wfg/engine/audio/HostedAudioDriver.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/ShowDocument.h>
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
        tree::ParameterTree parameters { document, engine.commands(), mounts };
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
    REQUIRE (tracks->value.has_value());
    CHECK (std::to_string (tracks->value->getInt32())
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
    REQUIRE (name->value.has_value());
    CHECK (name->value->getString() == "Main L/R");
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
    REQUIRE (status->value.has_value());
    CHECK (status->value->getString() == "stopped");

    const auto* outputs = snapshot->find ("/godot/audio/outputs");

    REQUIRE (outputs != nullptr);
    REQUIRE (outputs->value.has_value());
    CHECK (outputs->value->getInt32() == 0);
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

    /*  Tracktion reaches MessageManager::getInstance() while it builds. This
        opens no display on Linux, which is why the CI job needs no xvfb - the
        same reasoning `wfg selftest` records. */
    struct HostRig
    {
        ScopedStorage storage;
        juce::ScopedJuceInitialiser_GUI juceInitialiser;
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

    juce::File writeSteadyTone (const juce::File& folder, int channels, int rate)
    {
        const auto file = folder.getChildFile ("tone.wav");
        folder.createDirectory();

        juce::WavAudioFormat format;
        std::unique_ptr<juce::FileOutputStream> stream { file.createOutputStream() };

        if (stream == nullptr)
            return {};

        std::unique_ptr<juce::AudioFormatWriter> writer {
            format.createWriterFor (stream.get(), rate, static_cast<unsigned int> (channels),
                                    16, {}, 0) };

        if (writer == nullptr)
            return {};

        stream.release();

        juce::AudioBuffer<float> buffer { channels, rate * 2 };

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
    juce::ScopedJuceInitialiser_GUI juce;

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
    juce::ScopedJuceInitialiser_GUI juce;

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
    juce::ScopedJuceInitialiser_GUI juce;

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
    juce::ScopedJuceInitialiser_GUI juce;

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
    juce::ScopedJuceInitialiser_GUI juce;

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
