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
#include <wfg/engine/audio/AudioHost.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/tree/TreeCommands.h>

#include "TestSupport.h"

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
