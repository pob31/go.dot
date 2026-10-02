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
#include <wfg/engine/audio/RecoveryGate.h>
#include <wfg/engine/rt/RtCheck.h>
#include <wfg/engine/audio/HostPlayer.h>
#include <wfg/engine/audio/Looper.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/audio/AudioHost.h>
#include <wfg/engine/audio/CueMatrix.h>
#include <wfg/engine/audio/MediaInfo.h>

#include <tuple>
#include <wfg/engine/audio/HostedAudioDriver.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/plugin/ProxyLane.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/tree/TreeCommands.h>

#include "TestSupport.h"

#include <algorithm>
#include <iterator>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
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

    /*  WRITABLE SINCE 2026-09-21, and this line used to say `read`. How many
        cues can sound at once is a decision about the shape of a show, and a
        decision no client can state is a decision nobody can take: File - New
        wrote `tracks="0"`, nothing could change it, and every GO in a new show
        ended `no-track`. The graph is built from it when the audio settings
        are applied, so a write lands at the next Apply rather than under a
        running show. */
    CHECK (tracks->access == tree::Access::readWrite);

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

TEST_CASE ("audio: the track count takes a write, and an address nobody has is still refused")
{
    /*  THIS CASE USED TO ASSERT THE OPPOSITE, and the reason it changed is
        worth keeping. `tracks` was read-only with no command behind it, so the
        polyphony ceiling could only be set by editing show.xml in a text
        editor - and a show made by File - New therefore declared nought and
        could never play anything.

        What it still pins is the OTHER half: which refusal arrives for an
        address nobody has. Answering "read-only" there would send a client
        looking for a permission problem, and answering "bad-address" for a
        real row would send them looking for a spelling mistake they did not
        make. */
    Rig rig;

    const auto edit = rig.document.setAttribute ("/godot/audio/tracks", "16");

    CHECK (edit.ok);
    CHECK (rig.document.getAttribute ("/godot/audio/tracks") == "16");

    const auto missing = rig.document.setAttribute ("/godot/audio/noSuchThing", "16");

    CHECK_FALSE (missing.ok);
    CHECK (missing.reason == reason::badAddress);

    /*  And a read-only row still refuses as one, so the distinction above is
        still a distinction: a bus's channel is maintained by the layout
        commands and is not a client's to write. */
    const auto owned = rig.document.setAttribute ("/godot/bus/J3MT5XYA/firstChannel", "8");

    CHECK_FALSE (owned.ok);
    CHECK (owned.reason == reason::readOnly);
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
        between id strides and appears at some counts and not others.

        M10, AND WHY THE SAME QUESTION IS NOW ASKED TWICE OVER. Phase 3 gives
        every track S slots, one per range of the widest media cue in the show
        (§3.24), and a slot is where the collision lives: SlotControlNode takes
        the slot's own EditItemID for its id, and the switching node above it
        hash-combines its internal children's. So slots multiply exactly the
        surface the upstream bug is on, and the ids they mint fall on a lattice
        of their own - a stride within a track, laid over the stride between
        tracks. A count that is unique at one slot says nothing about eight.

        The plan gates the whole slot design on this answer (plan, M10): if the
        ids collide at 1..64 tracks x 1..8 slots then S slots is not the shape,
        and the A/B fallback - two slots, alternated - is what replaces it. */
    HostRig rig;
    REQUIRE (rig.host.start (hostFor (8)));

    for (const int tracks : { 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 24, 31, 32, 48, 63, 64 })
    {
        int atOneSlot = 0;

        for (const int slots : { 1, 2, 3, 4, 5, 6, 7, 8 })
        {
            INFO ("tracks: " << tracks << ", slots: " << slots);

            audio::EditSpec spec;
            spec.tracks = tracks;
            spec.slots = slots;

            REQUIRE (rig.host.buildEdit (spec));

            const auto report = rig.host.inspectNodeIds();

            /*  Reported, not just asserted. A graph whose ids were mostly zero
                would pass a duplicate check while telling us nothing, so the
                shape of what was inspected is printed with the verdict. */
            INFO ("nodes " << report.nodes << " (" << report.outerNodes
                            << " outer), zero ids " << report.zeroIds
                            << ", duplicates " << report.duplicates
                            << " (" << report.typedDuplicates << " same-type)");

            CHECK (report.duplicates == 0);
            CHECK (report.typedDuplicates == 0);

            /*  The check must actually be looking at something. If Tracktion
                ever stopped giving these nodes identities, `duplicates == 0`
                would go on passing for the wrong reason. */
            CHECK (report.nodes > tracks);
            CHECK (report.nodes - report.zeroIds > tracks);

            /*  AND AT THE SLOTS, which is the quiet way this could flatter
                itself. The outer graph does not grow with the slot count at
                all - a launcher slot is an internal node - so a check that
                inspected only what the processor walks would ask the same
                question eight times over and report eight passes.

                What says the slots are being seen is that the collection grew
                when they were asked for. */
            if (slots == 1)
                atOneSlot = report.nodes;
            else
                CHECK (report.nodes > atOneSlot);

            /*  M10's numbers, at the polyphony ceiling, printed so the PR
                carries them and so the shape can be read: what the slot count
                multiplies and what it leaves alone. */
            if (tracks == 64)
                MESSAGE ("M10 at 64 tracks x " << slots << " slots: "
                          << report.nodes << " nodes reachable by id ("
                          << report.outerNodes << " in the outer graph), "
                          << report.zeroIds << " with no id, "
                          << report.duplicates << " duplicates");
        }
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
        REQUIRE (rig.host.setTrackSource (0, 0, tone.getFullPathName().toStdString()));

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
        int slotsPerTrack = 1;

        double percentOfBudget() const { return 100.0 * microsecondsPerBlock / budgetMicroseconds; }
    };

    /** A sink that does nothing, so what is timed is the graph and not the test. */
    struct NullSink final : audio::BlockSink
    {
        void blockProduced (const float* const*, int, int) noexcept override {}
    };

    /*  ONE SOUNDING SLOT PER TRACK, WHATEVER THE SLOT COUNT, and that is the
        shape a show runs rather than an easier one. A media cue's ranges are a
        playlist over one file: one range sounds at a time, two only at the
        instant of a boundary. Eight slots all playing is a rig no show ever
        asks for, and measuring it would answer a question nobody has.

        What the extra slots cost is not nothing, though, which is why they are
        here at all: each one is a clip, a SlotControlNode and a branch of the
        switching node above it, built into the graph at every track and
        processed every block whether or not it is sounding. That is the cost
        the slot design has to be affordable at. */
    BlockCost measureBlockCost (int tracks, int outputs, int rate, int blockSize, int slots = 1)
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
        spec.slots = slots;
        REQUIRE (rig.host.buildEdit (spec));

        const auto tone = writeSteadyTone (rig.storage.folder, 2, rate);
        REQUIRE (tone.existsAsFile());

        /*  Every track playing, and every track routed somewhere different, so
            no two matrices are doing the same work and nothing can be folded
            away. This is a full house: the polyphony ceiling, all of it in use. */
        for (int track = 0; track < tracks; ++track)
        {
            REQUIRE (rig.host.setTrackSource (track, 0, tone.getFullPathName().toStdString()));

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
                 playing,
                 slots };
    }

    juce::String report (const juce::String& name, const BlockCost& cost)
    {
        return name + ": " + juce::String (cost.microsecondsPerBlock, 1) + " us/block of "
             + juce::String (cost.budgetMicroseconds, 1) + " us ("
             + juce::String (cost.percentOfBudget(), 1) + "% of real time), "
             + juce::String (cost.tracksPlaying) + " tracks playing, "
             + juce::String (cost.slotsPerTrack) + " slots each";
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
/*  M11 - WHAT EIGHT SLOTS A TRACK COST, measured against M3's own number.

    THE DECISION THIS IS FOR. §3.24 gives a media cue a list of ranges of one
    file and Go.dot places the boundary between them, which means each range
    wants a clip of its own, armed and waiting, in a slot of its own. The
    chosen shape is S slots on every track, S being the widest range count in
    the show, fixed when the graph is built (§3.25). The alternative, written
    down in the plan and not built, is two slots alternated A/B - re-arming the
    idle one while the other plays, a graph rebuild per boundary.

    S slots is the better mechanism by some distance: no rebuild at a boundary,
    every range ready before it is needed, and a boundary that can be placed as
    far ahead as the launch latency allows. What it costs is a graph eight
    times wider at the launcher, on every track, sounding or not. So the
    question is arithmetic and this is where it gets answered: if eight slots
    at the polyphony ceiling do not fit in a 96 kHz block, S is not affordable
    and A/B is what gets built.

    M3 measured the same rig at one slot. Reading the two together is the
    point - the slot term is the difference, and it is printed at 1, 2, 4 and 8
    so it can be read off rather than extrapolated from a single pair.

    THIS REPORTS, IT DOES NOT GATE, for M3's reasons exactly: a wall-clock
    threshold asserted on a shared CI runner is a flaky test, and Debug is not
    the number a show runs at. The numbers go in the PR.
*/
TEST_CASE ("M11: thirty-two cues, eight slots each, into sixty-four outputs at ninety-six kilohertz")
{
    constexpr int rate = 96000;
    constexpr int blockSize = 64;

    /*  MEASURED IN ALTERNATION, ONE SLOT AND EIGHT, FOUR TIMES OVER - and that
        is not decoration, it is the only way this number can be read.

        Each of these rigs is a Tracktion engine built and torn down inside one
        process, and the cost of a block drifts upwards with how many have been
        built before it. It is a large drift: the same configuration measures
        near 205 us early in a run and near 330 us eight rigs later, and after
        M10 has built a hundred and twenty-eight Edits ahead of it, near 360.

        A sweep from one slot to eight cannot tell a slot term from that, since
        both rise together. Sweeping back down showed how bad it is - measured
        descending, EIGHT slots came out 47 us cheaper than one, which is not a
        fact about slots. So the two configurations that matter are interleaved
        instead: 1, 8, 1, 8, 1, 8, 1, 8. Drift is then one step wide inside each
        pair rather than seven, it falls on both members of the next pair
        equally, and what is left is the difference the slots make. */
    std::vector<BlockCost> one, eight;

    for (int pass = 0; pass < 4; ++pass)
    {
        one.push_back (measureBlockCost (32, 64, rate, blockSize, 1));
        eight.push_back (measureBlockCost (32, 64, rate, blockSize, 8));
    }

    double paired = 0.0;

    for (int pass = 0; pass < 4; ++pass)
    {
        MESSAGE (report ("32 x 64 x 1 slot ", one[static_cast<std::size_t> (pass)]));
        MESSAGE (report ("32 x 64 x 8 slots", eight[static_cast<std::size_t> (pass)]));

        paired += eight[static_cast<std::size_t> (pass)].microsecondsPerBlock
                    - one[static_cast<std::size_t> (pass)].microsecondsPerBlock;
    }

    /*  The whole difference between one slot and eight, undivided. Dividing by
        seven would read as a per-slot figure this rig cannot resolve. */
    MESSAGE ("eight slots less one slot, averaged over four interleaved pairs: "
              << juce::String (paired / 4.0, 1) << " us/block, against a budget of "
              << juce::String (one.front().budgetMicroseconds, 1) << " us");

    /*  Every track really was sounding, at both slot counts - and one slot
        each, which is the shape a show runs. A rig where the extra slots had
        quietly kept the tracks silent would be cheap and would be measuring
        nothing. */
    for (const auto& cost : one)    CHECK (cost.tracksPlaying == 32);
    for (const auto& cost : eight)  CHECK (cost.tracksPlaying == 32);

    CHECK (eight.front().microsecondsPerBlock > 0.0);
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
    REQUIRE (rig.host.setTrackSource (0, 0, tone.getFullPathName().toStdString()));

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
        device, a resync, and until 2026-09-29 its CPU-overload mute, now off
        (namespace draft §22.12) - and that is the number to look at when a
        show has drifted and nobody knows why. */
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
    REQUIRE (rig.host.setTrackSource (0, 0, tone.getFullPathName().toStdString()));
    REQUIRE (rig.host.waitForTrackSourceReady (0, 10000));

    CHECK (rig.host.trackPlayState (0).valid);
    CHECK_FALSE (rig.host.trackPlayState (0).playing);

    /*  An index no track answers to is answered rather than crashed on: this is
        reachable from a cue naming a track that a smaller rig does not have. */
    CHECK_FALSE (rig.host.trackPlayState (99).valid);
    CHECK_FALSE (rig.host.launchTrackAt (99, 0, 1.0));
    CHECK_FALSE (rig.host.stopTrack (99));

    for (int i = 0; i < 8; ++i)
        rig.host.processBlock();

    /*  Placed a quarter of a second ahead, in Go.dot's own samples, converted
        through the anchor. */
    const auto target = rig.host.clock().samplesElapsed() + settings.sampleRate / 4;
    REQUIRE (rig.host.launchTrackAt (0, 0, rig.host.beatsAtSample (target)));

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
    REQUIRE (rig.host.setTrackSource (0, 0, tone.getFullPathName().toStdString()));
    REQUIRE (rig.host.waitForTrackSourceReady (0, 10000));

    for (int i = 0; i < 8; ++i)
        rig.host.processBlock();

    REQUIRE (rig.host.launchTrackAt (0, 0, rig.host.beatsAtSample (rig.host.clock().samplesElapsed() + 1024)));

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

    /*  Standby was asked to move and had nowhere to go: one cue in the list,
        so firing it clears the pointer rather than wrapping or standing on the
        cue that has just gone (author, 2026-09-18). GO applied either way. */
    CHECK (document.findById (listId)[juce::Identifier ("standby")].toString().isEmpty());

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
        REQUIRE (rig.host.setTrackSource (0, 0, tone.getFullPathName().toStdString()));
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

        REQUIRE (rig.host.launchTrackAt (0, 0, rig.host.beatsAtSample (result.expected)));

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

namespace
{
    /*  SMALL BLOCKS ARE NOT RUN ON CI, at the author's decision (2026-09-14).
        The CI workflow sets WFG_SKIP_SMALL_BLOCKS, and M5 and M6 then skip their
        64-sample cases - the cases the shared runners have failed on: M5 once on
        Windows at 44 100 Hz after the spatcore re-pin, one block and one sample
        late, in a test that had passed forty times. The question is whether
        those cases are the ones that flake, and dropping them from CI is how the
        author chose to ask it.

        ONLY THE SMALLEST, and only there. 128 stays in M5 and is what every
        black-box driver runs at, and no developer's machine sets the variable,
        so the arithmetic at 64 samples is still checked wherever a failure means
        something rather than a busy runner.

        SAID OUT LOUD when it happens: a MESSAGE for each case skipped, which any
        run that shows this test's output prints - a local run, or a CI run that
        failed. ctest does NOT print a passing test's output, so on a green CI
        run the record is the variable itself, which GitHub prints in every
        step's environment block, and the comment beside it in ci.yml. *This
        paragraph is a correction (2026-09-14):* it first claimed the MESSAGE
        kept a green CI honest, and the first green run showed no MESSAGE at all
        - M23's timing line, the control, was missing from it too. */
    constexpr int smallestBlockOnCi = 128;

    bool skipsSmallBlocks()
    {
        return juce::SystemStats::getEnvironmentVariable ("WFG_SKIP_SMALL_BLOCKS", {}) == "1";
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
            if (skipsSmallBlocks() && blockSize < smallestBlockOnCi)
            {
                MESSAGE ("M5 skipped at " << rate << " Hz with " << blockSize
                         << "-sample blocks: WFG_SKIP_SMALL_BLOCKS is set, as the CI workflow sets it");
                continue;
            }

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

        REQUIRE (rig.host.setTrackSource (0, 0, tone.getFullPathName().toStdString()));
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
        REQUIRE (rig.host.launchTrackAt (0, 0, rig.host.beatsAtSample (target)));

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
                REQUIRE (rig.host.setTrackSource (1, 0, tone.getFullPathName().toStdString()));
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
        REQUIRE (rig.host.setTrackSource (0, 0, tone.getFullPathName().toStdString()));
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

        REQUIRE (rig.host.launchTrackAt (0, 0, rig.host.beatsAtSample (
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

        REQUIRE (rig.host.stopTrackAt (0, 0, rig.host.beatsAtSample (result.expected)));

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
            if (skipsSmallBlocks() && blockSize < smallestBlockOnCi)
            {
                MESSAGE ("M6 skipped at " << rate << " Hz with " << blockSize
                         << "-sample blocks: WFG_SKIP_SMALL_BLOCKS is set, as the CI workflow sets it");
                continue;
            }

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
                                                 scenario.verb == nullptr ? "fade" : "transport",
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

//==============================================================================
/*  RANGES: A CUE THAT PLAYS PART OF ITS FILE.

    §3.24 lets a media cue carry a list of regions of its file, and the graph
    holds each one as a clip in a launcher slot of its own, armed and waiting.
    What is checked here is the seam - that the slots exist, that arming puts
    the right region in each of them, and that a range plays the part of the
    file it names and keeps playing it.

    THE METHOD IS PHASE 2'S, EXTENDED. A file whose three seconds are three
    DIFFERENT CONSTANTS, so the value coming out of the rig says which second of
    the recording is sounding, as arithmetic. No FFT, no thresholding, no
    guessing: 0.25 is the first second, 0.5 the second, 0.75 the third.

    What is NOT here is the boundary between two ranges - the placed stop-and-
    play pair that carries a cue from its first range into its second. That is
    PR 3.9, and M13 is what will measure it. This is the ground it stands on.
*/
namespace
{
    /** The constant that says which second of the segmented tone this is. */
    constexpr float segmentLevel (int segment)
    {
        return 0.25f * static_cast<float> (segment + 1);
    }

    /*  Three seconds, three constants. Not `tone.wav`: writeSteadyTone uses
        that name, and a test that got the wrong file would measure a flat one
        and pass every check about the first segment. */
    juce::File writeSegmentedTone (const juce::File& folder, int rate, int segments = 3)
    {
        const auto file = folder.getChildFile ("segments.wav");
        folder.createDirectory();

        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream { file.createOutputStream() };

        if (stream == nullptr)
            return {};

        auto writer = format.createWriterFor (stream,
                                              juce::AudioFormatWriterOptions{}
                                                .withSampleRate (static_cast<double> (rate))
                                                .withNumChannels (1)
                                                .withBitsPerSample (16));

        if (writer == nullptr)
            return {};

        juce::AudioBuffer<float> buffer { 1, rate * segments };

        for (int segment = 0; segment < segments; ++segment)
            juce::FloatVectorOperations::fill (buffer.getWritePointer (0, segment * rate),
                                               segmentLevel (segment), rate);

        writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
        return file;
    }

    /** Which segment a rendered sample came from: 0, 1, 2 - or -1 for none. */
    int segmentOf (float sample)
    {
        for (int segment = 0; segment < 3; ++segment)
            if (std::abs (sample - segmentLevel (segment)) < 0.01f)
                return segment;

        return -1;
    }

    /*  The first frame at or after `from` where the render settles into a
        segment and STAYS there.

        STABILITY IS NOT FUSSINESS. A boundary is a few dozen samples of one
        range being taken down while another starts, and somewhere in that decay
        the outgoing constant passes through the incoming one: 0.5 on its way
        down from 0.75 reads as segment 1 for a sample or two. A sweep that
        believed the first matching sample reported five boundaries where there
        are two, and found the second range a second before it started.

        A tenth of the shortest range is stable enough to be a range and far too
        long to be a transient. */
    constexpr int stableFor = 4800;

    int settlesIntoSegmentAt (const RecordingSink& sink, int from, int segment)
    {
        int run = 0;

        for (int n = std::max (0, from); n < sink.written; ++n)
        {
            if (segmentOf (sink.buffer.getSample (0, n)) == segment)
            {
                if (++run >= stableFor)
                    return n - run + 1;
            }
            else
            {
                run = 0;
            }
        }

        return -1;
    }
}

TEST_CASE ("ranges: every slot of every track holds a resident clip")
{
    /*  An empty slot means no SlotControlNode for it, and an empty FIRST slot
        takes the whole track's output stage out of the graph with it. So the
        count is tracks TIMES slots, and a slot count that quietly produced
        fewer clips than it promised would be a cue whose fourth range was
        silent for a reason nothing reported. */
    HostRig rig;
    REQUIRE (rig.host.start (hostFor (4)));

    for (const int slots : { 1, 2, 5, 8 })
    {
        INFO ("slots: " << slots);

        audio::EditSpec spec;
        spec.tracks = 6;
        spec.slots = slots;

        REQUIRE (rig.host.buildEdit (spec));
        CHECK (rig.host.slotCount() == slots);
        CHECK (rig.host.residentClipCount() == 6 * slots);
    }
}

TEST_CASE ("ranges: a cue with more of them than the graph has slots is refused, not truncated")
{
    /*  `no-slot`. The slot count is fixed when the graph is built (§3.25), so a
        range added during a show has nowhere to be armed. Arming the first S of
        them would be a cue that plays most of what it says - which is worse
        than one that says it cannot, because nobody would notice until the part
        that was dropped mattered. */
    HostRig rig;
    REQUIRE (rig.host.start (hostFor (2)));

    audio::EditSpec spec;
    spec.tracks = 1;
    spec.channelsPerTrack = 1;
    spec.slots = 2;
    REQUIRE (rig.host.buildEdit (spec));

    const auto tone = writeSegmentedTone (rig.storage.folder, 48000);
    REQUIRE (tone.existsAsFile());

    const auto path = tone.getFullPathName().toStdString();

    CHECK (rig.host.setTrackRanges (0, path, { { 0.0, 1.0, 1 } }));
    CHECK (rig.host.setTrackRanges (0, path, { { 0.0, 1.0, 1 }, { 1.0, 2.0, 1 } }));

    CHECK_FALSE (rig.host.setTrackRanges (0, path, { { 0.0, 1.0, 1 },
                                                     { 1.0, 2.0, 1 },
                                                     { 2.0, 3.0, 1 } }));

    CHECK (rig.host.lastError().find ("no-slot") != std::string::npos);
}

TEST_CASE ("ranges: one that is not inside the file fails the arm, which is when the file is read")
{
    /*  The document could not have known. A show is authored on one machine and
        its media copied onto another, so `out` against the file length is a
        question for the arm rather than for the load - which is the same rule
        that lets a show with a missing sound still open. */
    HostRig rig;
    REQUIRE (rig.host.start (hostFor (2)));

    audio::EditSpec spec;
    spec.tracks = 1;
    spec.channelsPerTrack = 1;
    spec.slots = 2;
    REQUIRE (rig.host.buildEdit (spec));

    const auto tone = writeSegmentedTone (rig.storage.folder, 48000);
    const auto path = tone.getFullPathName().toStdString();

    CHECK (rig.host.setTrackRanges (0, path, { { 0.0, 3.0, 1 } }));        // the whole file
    CHECK_FALSE (rig.host.setTrackRanges (0, path, { { 0.0, 4.0, 1 } }));  // a second too far
    CHECK_FALSE (rig.host.setTrackRanges (0, path, { { 2.0, 1.0, 1 } }));  // backwards
}

TEST_CASE ("ranges: a range plays the part of the file it names, and goes on playing it")
{
    /*  THE TWO PROPERTIES THE WHOLE MECHANISM RESTS ON.

        The first: the slot sounds the REGION, not the file. Arming the second
        second of a three-second file and hearing the first would mean the loop
        range never reached the clip, and every boundary PR 3.9 places would be
        placed around the wrong material.

        The second: it does not stop at the end of that region. A range clip is
        armed LOOPING precisely so that its launcher builds no stop duration for
        it - and if that has not worked, the cue goes silent at the end of its
        first pass and every loop count in the document means nothing. A second
        and a half of a one-second range is what says the wrap happened. */
    constexpr int rate = 48000;
    constexpr int blockSize = 128;

    HostRig rig;

    audio::HostSettings settings;
    settings.sampleRate = rate;
    settings.blockSize = blockSize;
    settings.outputChannels = 2;

    REQUIRE (rig.host.start (settings));

    audio::EditSpec spec;
    spec.tracks = 1;
    spec.channelsPerTrack = 1;
    spec.slots = 3;
    REQUIRE (rig.host.buildEdit (spec));

    const auto tone = writeSegmentedTone (rig.storage.folder, rate);
    REQUIRE (tone.existsAsFile());

    /*  Three ranges, one per segment, each a single pass - the shape the plan
        names for this PR. All three are armed; only the middle one is launched,
        because which slot sounds when is PR 3.9's question and which slot holds
        what is this one's. */
    REQUIRE (rig.host.setTrackRanges (0, tone.getFullPathName().toStdString(),
                                      { { 0.0, 1.0, 1 }, { 1.0, 2.0, 1 }, { 2.0, 3.0, 1 } }));

    REQUIRE (rig.host.waitForTrackSourceReady (0, 10000));

    auto* matrix = rig.host.trackMatrix (0);
    REQUIRE (matrix != nullptr);
    matrix->setLevelDb (0.0f);
    matrix->setGain (0, 0, 1.0f);
    matrix->snapToTargets();

    for (int i = 0; i < 8; ++i)
        rig.host.processBlock();

    RecordingSink sink;
    sink.prepare (settings.outputChannels, rate * 3);
    rig.host.setBlockSink (&sink);

    const auto target = rig.host.clock().samplesElapsed() + 4 * (rate / 50);
    REQUIRE (rig.host.launchTrackAt (0, 1, rig.host.beatsAtSample (target)));

    /*  Two seconds and a bit of a range that is one second long. */
    for (int block = 0; block < 2 * rate / blockSize + 200; ++block)
        rig.host.processBlock();

    rig.host.setBlockSink (nullptr);

    const auto first = sink.firstSoundAt (0);
    REQUIRE (first >= 0);

    /*  IT IS THE MIDDLE SEGMENT, which is the range that was armed into the
        slot that was launched. */
    CHECK (segmentOf (sink.buffer.getSample (0, first + 1000)) == 1);

    /*  AND IT IS STILL THE MIDDLE SEGMENT A SECOND AND A HALF LATER, past the
        end of the region, which is the wrap. Sampled rather than swept, because
        the wrap INSTANT is M12's subject and not this test's. */
    REQUIRE (sink.written > first + rate * 3 / 2);
    CHECK (segmentOf (sink.buffer.getSample (0, first + rate * 3 / 2)) == 1);

    /*  It never played anything else: no first segment leaking in from the head
        of the file, no third from running past the end. */
    int wrong = 0;

    for (int n = first + 500; n < std::min (sink.written, first + 2 * rate) - 500; n += 97)
        if (segmentOf (sink.buffer.getSample (0, n)) != 1)
            ++wrong;

    INFO ("samples belonging to another segment: " << wrong);
    CHECK (wrong == 0);

    CHECK (rig.host.trackPlayState (0, 1).playing);
    CHECK_FALSE (rig.host.trackPlayState (0, 0).playing);
    CHECK_FALSE (rig.host.trackPlayState (0, 2).playing);

    /*  And the cue-wide question answers yes, because a ranged cue sounds out
        of whichever slot its current range is in. */
    CHECK (rig.host.isTrackPlaying (0));

    /*  AND THE PLAYER ANSWERS IT TOO (2026-09-26). `HostPlayer::isPlaying` is
        what the Runner reads a cue's end from, and it asked slot nought: this
        cue, sounding out of its second slot, read silent there. */
    Engine engine;
    const audio::HostPlayer player { rig.host, engine };
    CHECK (player.isPlaying (0));
}

TEST_CASE ("ranges: a slice armed part-way into its loop starts there, and wraps to its in-point")
{
    /*  K8'S REVIEW (the author, 2026-10-02): a bed Esc paused inside a
        looping slice carries on at the same point of its loop, so the arm
        starts that slice's clip part-way in - its own offset, which inside a
        loop is read from the loop's start and wraps with it. A two-second loop
        over the first two segments, armed a second and a half in: the second
        segment for half a second, then the first from the in-point, then the
        second again. Before, every slice launched at its in-point. */
    constexpr int rate = 48000;
    constexpr int blockSize = 128;

    HostRig rig;

    audio::HostSettings settings;
    settings.sampleRate = rate;
    settings.blockSize = blockSize;
    settings.outputChannels = 2;

    REQUIRE (rig.host.start (settings));

    audio::EditSpec spec;
    spec.tracks = 1;
    spec.channelsPerTrack = 1;
    spec.slots = 2;
    REQUIRE (rig.host.buildEdit (spec));

    const auto tone = writeSegmentedTone (rig.storage.folder, rate);
    REQUIRE (tone.existsAsFile());

    REQUIRE (rig.host.setTrackRanges (0, tone.getFullPathName().toStdString(),
                                      { { 2.0, 3.0, 1 }, { 0.0, 2.0, 0 } }, 0.0, false, 1, 1.5));

    REQUIRE (rig.host.waitForTrackSourceReady (0, 10000));

    auto* matrix = rig.host.trackMatrix (0);
    REQUIRE (matrix != nullptr);
    matrix->setLevelDb (0.0f);
    matrix->setGain (0, 0, 1.0f);
    matrix->snapToTargets();

    for (int i = 0; i < 8; ++i)
        rig.host.processBlock();

    RecordingSink sink;
    sink.prepare (settings.outputChannels, rate * 3);
    rig.host.setBlockSink (&sink);

    const auto target = rig.host.clock().samplesElapsed() + 4 * (rate / 50);
    REQUIRE (rig.host.launchTrackAt (0, 1, rig.host.beatsAtSample (target)));

    for (int block = 0; block < 5 * rate / (2 * blockSize) + 200; ++block)
        rig.host.processBlock();

    rig.host.setBlockSink (nullptr);

    const auto first = sink.firstSoundAt (0);
    REQUIRE (first >= 0);
    REQUIRE (sink.written > first + rate * 9 / 4);

    /*  A second and a half in: the second segment, for half a second. */
    CHECK (segmentOf (sink.buffer.getSample (0, first + rate / 4)) == 1);

    /*  Then the in-point, and the first segment for a second. */
    CHECK (segmentOf (sink.buffer.getSample (0, first + rate * 3 / 4)) == 0);
    CHECK (segmentOf (sink.buffer.getSample (0, first + rate * 5 / 4)) == 0);

    /*  And the second segment again: the loop, from its in-point on. */
    CHECK (segmentOf (sink.buffer.getSample (0, first + rate * 7 / 4)) == 1);

    /*  The other slot is armed at its in-point, as every slice but a resumed
        one is: nothing of its offset leaked into it. */
    CHECK_FALSE (rig.host.trackPlayState (0, 0).playing);
}

TEST_CASE ("ranges: arming a cue with none of them puts the whole file back in the first slot")
{
    /*  A voice is reused. A slot still holding the last cue's third range would
        sound if anything ever launched it, so arming a rangeless cue puts every
        slot past the first back on the silent placeholder - and the first back
        on the whole file, which is the Phase 2 shape and is what most cues are. */
    constexpr int rate = 48000;

    HostRig rig;

    audio::HostSettings settings;
    settings.sampleRate = rate;
    settings.blockSize = 128;
    settings.outputChannels = 2;

    REQUIRE (rig.host.start (settings));

    audio::EditSpec spec;
    spec.tracks = 1;
    spec.channelsPerTrack = 1;
    spec.slots = 3;
    REQUIRE (rig.host.buildEdit (spec));

    const auto tone = writeSegmentedTone (rig.storage.folder, rate);
    const auto path = tone.getFullPathName().toStdString();

    REQUIRE (rig.host.setTrackRanges (0, path, { { 0.0, 1.0, 1 }, { 1.0, 2.0, 1 } }));

    /*  Every armed slot is three seconds long as a SOURCE - it is the same file
        either way - so what says a slot was RELEASED is the placeholder's own
        length coming back. */
    CHECK (rig.host.trackSourceLengthSeconds (0, 0) > 2.5);
    CHECK (rig.host.trackSourceLengthSeconds (0, 1) > 2.5);
    CHECK (rig.host.trackSourceLengthSeconds (0, 2) < 1.5);   // still the placeholder

    REQUIRE (rig.host.setTrackRanges (0, path, {}));

    CHECK (rig.host.trackSourceLengthSeconds (0, 0) > 2.5);   // the whole file
    CHECK (rig.host.trackSourceLengthSeconds (0, 1) < 1.5);   // back to the placeholder
    CHECK (rig.host.trackSourceLengthSeconds (0, 2) < 1.5);
}

//==============================================================================
/*  M13 - A RANGE PLAYLIST, PLAYED IN ORDER, WITH EVERY BOUNDARY WHERE IT SAID.

    §3.24's promise, checked from the render and through the whole stack: a show
    document with three ranges, a GO, and a WAV that says which second of the
    file was sounding at every instant. The scheduler places the boundaries, the
    Player carries them to two slots, and Tracktion does the rest.

    THE MEASUREMENT IS ARITHMETIC, not analysis. The file's three seconds are
    three different constants, so the value at any sample IS the identity of the
    range playing - 0.25 is the first, 0.5 the second, 0.75 the third. Where the
    render changes from one to the next IS the boundary, to the sample, with
    nothing to threshold and nothing to correlate.

    WHAT IS ASSERTED, and each is a different way for this to be wrong:

      - the ranges play IN ORDER and each one only once, so a boundary that
        fired twice or a slot armed with the wrong region is visible;
      - every boundary lands within a block plus forty samples of where the
        arithmetic says, which is the plan's bound and is M12's measured cost of
        a placed boundary (Tracktion's own stop decay) plus the block it can be
        quantised to;
      - the run reports which range it is in while it plays, because a strip
        that cannot say `2/3` is a strip nobody can cue from;
      - and it ends when the last range does, rather than at the first boundary,
        which is the failure `rangesFinished` exists to prevent.
*/
TEST_CASE ("M13: three ranges play in order, and every boundary lands where the arithmetic says")
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
    spec.tracks = 1;
    spec.channelsPerTrack = 1;
    spec.slots = 3;
    REQUIRE (rig.host.buildEdit (spec));

    const auto tone = writeSegmentedTone (rig.storage.folder, rate);
    REQUIRE (tone.existsAsFile());

    //  --- a show: one media cue with three ranges, one pass each -------------
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
    const auto cueId = document.createCue (listId, 0, "media", "Bed").id;

    document.setAttribute ("/godot/cue/" + cueId + "/file", tone.getFileName().toStdString());

    auto audioNode = document.root().getChildWithName ("Audio");
    audioNode.setProperty (juce::Identifier ("tracks"), 1, nullptr);

    juce::ValueTree bus { "Bus" };
    bus.setProperty (juce::Identifier ("id"), "J3MT5XYA", nullptr);
    bus.setProperty (juce::Identifier ("name"), "Main", nullptr);
    bus.setProperty (juce::Identifier ("firstChannel"), 0, nullptr);
    bus.setProperty (juce::Identifier ("width"), 1, nullptr);
    audioNode.appendChild (bus, nullptr);

    auto cue = document.findById (cueId);
    juce::ValueTree route { "Route" };
    route.setProperty (juce::Identifier ("id"), "Z04EH7PH", nullptr);
    route.setProperty (juce::Identifier ("bus"), "J3MT5XYA", nullptr);
    route.setProperty (juce::Identifier ("gains"), "1", nullptr);
    cue.appendChild (route, nullptr);

    for (int segment = 0; segment < 3; ++segment)
    {
        const auto edit = document.createRange (cueId, segment, segment + 1);
        REQUIRE (edit.ok);
    }

    document.setAttribute (cue::standbyAddressOf (listId), cueId);

    //  --- the audio side -----------------------------------------------------
    audio::HostPlayer player { rig.host, engine };
    runner.setPlayer (&player);
    runner.setSamplesPerTick (rate / 50);
    runner.setMediaFolder (rig.storage.folder.getFullPathName().toStdString());

    RecordingSink sink;
    sink.prepare (settings.outputChannels, rate * 6);

    std::int64_t tick = 0;

    const auto oneTick = [&]
    {
        runner.beforeTick (engine, tick);
        engine.processTick (tick++);
        player.serviceArms();

        for (int i = 0; i < (rate / 50) / blockSize; ++i)
            rig.host.processBlock();
    };

    /*  THE DISK IS WAITED FOR BEFORE THE RECORDING STARTS, and that is not
        tidiness - it is what makes this test the same length on every machine.

        Standby arms the cue, and between the arm and the file being mapped
        there is a graph rebuild and a read that Tracktion's cache does on a
        thread of its own. Those are WALL CLOCK. Recorded through, they fill the
        buffer with silence for however long the machine took - which on this
        box is a few ticks and on a loaded macOS runner was enough that the
        third range had nowhere left to be written, and the test failed saying
        the range had not played when it had.

        AND THE WAIT HAS TO BE WALL CLOCK TOO, which the first version of this
        loop was not. It gave the disk six hundred ticks - but a tick here is a
        call, not twenty milliseconds, so six hundred of them ran in a fraction
        of a second while Tracktion's cache thread was still reading - and a
        loaded macOS runner failed the same REQUIRE in M13, whose wait is this
        one's twin, for the very reason this paragraph describes (2026-09-11,
        CI run 34541784784). Each
        tick that finds the source not ready now also waits a real tick's worth,
        so the six hundred are about twelve seconds of wall clock, which is the
        budget the rest of this file gives `waitForTrackSourceReady`. The tick
        count is unchanged, so what the test records afterwards is too: a fast
        machine is ready after a few and never waits at all. */
    for (int i = 0; i < 4; ++i)
        oneTick();

    for (int i = 0; i < 600 && ! rig.host.isTrackSourceReady (0); ++i)
    {
        oneTick();
        std::this_thread::sleep_for (std::chrono::milliseconds (20));
    }

    REQUIRE (rig.host.isTrackSourceReady (0));

    rig.host.setBlockSink (&sink);

    REQUIRE (engine.submit (origin::cli, "go", {}));

    /*  Now the wait is only the launch latency, which is ticks and not disks. */
    for (int i = 0; i < 100 && ! rig.host.isTrackPlaying (0); ++i)
        oneTick();

    REQUIRE (rig.host.isTrackPlaying (0));

    const auto runId = runs.all().empty() ? std::string {} : runs.all().front().id;
    REQUIRE_FALSE (runId.empty());

    /*  The run says which range it is in from the moment the launch is placed,
        which is what a strip reads. */
    {
        const auto* run = runs.find (runId);
        REQUIRE (run != nullptr);
        CHECK (run->range == 0);
    }

    /*  Three seconds of ranges plus a second of slack, so the whole playlist and
        the silence after it are inside the recording. */
    std::vector<int> rangeSeen;

    for (int i = 0; i < 250; ++i)
    {
        oneTick();

        if (const auto* run = runs.find (runId))
            if (rangeSeen.empty() || rangeSeen.back() != run->range)
                rangeSeen.push_back (run->range);
    }

    rig.host.setBlockSink (nullptr);

    /*  --- what the model said -------------------------------------------- */

    /*  IN ORDER AND EACH ONE ONCE. A boundary that fired twice, or a slot armed
        with the wrong region, shows up here as a repeat or a skip. */
    REQUIRE (rangeSeen.size() >= 3);
    CHECK (rangeSeen[0] == 0);
    CHECK (rangeSeen[1] == 1);
    CHECK (rangeSeen[2] == 2);

    {
        const auto* run = runs.find (runId);
        REQUIRE (run != nullptr);

        INFO ("run state " << run->state << ", range " << run->range
               << ", pass " << run->rangeIteration);

        /*  AND IT ENDED WHEN THE LAST RANGE DID, rather than at the first
            boundary - which is what would happen if a poll falling between the
            outgoing stop and the incoming play were read as the cue finishing. */
        CHECK (run->isFinished());
    }

    /*  --- what the render said ------------------------------------------- */
    const auto first = sink.firstSoundAt (0);
    REQUIRE (first >= 0);

    /*  Where the render settles into each segment in turn. Each search starts
        where the last one settled, so a range found out of order is a search
        that fails rather than one that wraps round and finds it anyway. */
    const auto firstRange = settlesIntoSegmentAt (sink, first, 0);
    REQUIRE (firstRange >= 0);

    const auto secondRange = settlesIntoSegmentAt (sink, firstRange + stableFor, 1);
    REQUIRE (secondRange > firstRange);

    const auto thirdRange = settlesIntoSegmentAt (sink, secondRange + stableFor, 2);
    REQUIRE (thirdRange > secondRange);

    /*  AND NOTHING AFTER IT, which is what says each range played once. A
        fourth settling would be a boundary that fired twice or a slot armed
        with a region that is not its own. */
    CHECK (settlesIntoSegmentAt (sink, thirdRange + stableFor, 0) < 0);
    CHECK (settlesIntoSegmentAt (sink, thirdRange + stableFor, 1) < 0);

    /*  EACH BOUNDARY LANDS WHERE THE ARITHMETIC SAYS. The bound is the plan's:
        a block, because a placed instant can be quantised to one, plus forty
        samples, which is Tracktion's own stop decay and which M12 measured as
        the whole cost of a placed boundary.

        The settling point is where the new range is UNAMBIGUOUS, which is at
        or after the boundary rather than before it, so the comparison is
        against the boundary plus at most the damage M12 priced. */
    const auto allowed = blockSize + 40;

    const auto landedWhereItWasPlaced = [&] (int settledAt, int index)
    {
        const auto expected = first + index * rate;
        const auto error = settledAt - expected;

        INFO ("the boundary into range " << index << " landed " << error
               << " samples from where it was placed");
        CHECK (std::abs (error) <= allowed);
    };

    landedWhereItWasPlaced (secondRange, 1);
    landedWhereItWasPlaced (thirdRange, 2);
}

TEST_CASE ("M13: an advance leaves a range that loops for ever, at the end of the pass it is on")
{
    /*  THE DONE-WHEN CLAUSE'S OTHER HALF. §3.24 gives a range a loop count and
        nought means for ever, which is what an ambience bed is; `advance` is the
        only way out of one, and what it promises is that the way out is at a
        boundary rather than wherever the operator's hand came down.

        So: a first range that loops for ever, an advance sent partway through
        its third pass, and a render that goes on playing the first segment until
        the pass ends and only then plays the second. */
    constexpr int rate = 48000;
    constexpr int blockSize = 128;

    HostRig rig;

    audio::HostSettings settings;
    settings.sampleRate = rate;
    settings.blockSize = blockSize;
    settings.outputChannels = 2;

    REQUIRE (rig.host.start (settings));

    audio::EditSpec spec;
    spec.tracks = 1;
    spec.channelsPerTrack = 1;
    spec.slots = 2;
    REQUIRE (rig.host.buildEdit (spec));

    const auto tone = writeSegmentedTone (rig.storage.folder, rate);
    REQUIRE (tone.existsAsFile());

    Engine engine;
    doc::ShowDocument document;
    cue::RunTable runs;
    cue::Focus focus;
    auto runIds = doc::IdRegistry::withSeed (13);
    cue::Runner runner { document, runs, runIds, focus };

    engine.log().openInMemory ({});
    doc::registerDocumentCommands (engine.commands(), document);
    cue::registerCueCommands (engine.commands(), document, focus);
    cue::registerRunCommands (engine.commands(), runs);
    cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);

    const auto listId = document.createList ("Sound").id;
    const auto cueId = document.createCue (listId, 0, "media", "Bed").id;

    document.setAttribute ("/godot/cue/" + cueId + "/file", tone.getFileName().toStdString());

    auto audioNode = document.root().getChildWithName ("Audio");
    audioNode.setProperty (juce::Identifier ("tracks"), 1, nullptr);

    juce::ValueTree bus { "Bus" };
    bus.setProperty (juce::Identifier ("id"), "J3MT5XYA", nullptr);
    bus.setProperty (juce::Identifier ("name"), "Main", nullptr);
    bus.setProperty (juce::Identifier ("firstChannel"), 0, nullptr);
    bus.setProperty (juce::Identifier ("width"), 1, nullptr);
    audioNode.appendChild (bus, nullptr);

    auto cue = document.findById (cueId);
    juce::ValueTree route { "Route" };
    route.setProperty (juce::Identifier ("id"), "Z04EH7PH", nullptr);
    route.setProperty (juce::Identifier ("bus"), "J3MT5XYA", nullptr);
    route.setProperty (juce::Identifier ("gains"), "1", nullptr);
    cue.appendChild (route, nullptr);

    /*  The bed: one second, for ever. Then the outro, once. */
    const auto bed = document.createRange (cueId, 0.0, 1.0);
    REQUIRE (bed.ok);
    REQUIRE (document.setAttribute ("/godot/range/" + bed.id + "/loops", "0").ok);

    REQUIRE (document.createRange (cueId, 1.0, 2.0).ok);

    document.setAttribute (cue::standbyAddressOf (listId), cueId);

    audio::HostPlayer player { rig.host, engine };
    runner.setPlayer (&player);
    runner.setSamplesPerTick (rate / 50);
    runner.setMediaFolder (rig.storage.folder.getFullPathName().toStdString());

    RecordingSink sink;
    sink.prepare (settings.outputChannels, rate * 8);

    std::int64_t tick = 0;

    const auto oneTick = [&]
    {
        runner.beforeTick (engine, tick);
        engine.processTick (tick++);
        player.serviceArms();

        for (int i = 0; i < (rate / 50) / blockSize; ++i)
            rig.host.processBlock();
    };

    /*  The disk before the recording, for the reason above - and waited for in
        wall clock, for the reason above that: this is the loop that failed on
        the macOS runner. */
    for (int i = 0; i < 4; ++i)
        oneTick();

    for (int i = 0; i < 600 && ! rig.host.isTrackSourceReady (0); ++i)
    {
        oneTick();
        std::this_thread::sleep_for (std::chrono::milliseconds (20));
    }

    REQUIRE (rig.host.isTrackSourceReady (0));

    rig.host.setBlockSink (&sink);

    REQUIRE (engine.submit (origin::cli, "go", {}));

    for (int i = 0; i < 100 && ! rig.host.isTrackPlaying (0); ++i)
        oneTick();

    REQUIRE (rig.host.isTrackPlaying (0));

    const auto runId = runs.all().empty() ? std::string {} : runs.all().front().id;
    REQUIRE_FALSE (runId.empty());

    const auto startedAt = sink.written;

    /*  TWO AND A HALF PASSES OF A ONE-SECOND RANGE, which a loop count of one
        would have ended long before. Getting this far at all is the wrap doing
        its work. */
    for (int i = 0; i < 125; ++i)
        oneTick();

    {
        const auto* run = runs.find (runId);
        REQUIRE (run != nullptr);
        CHECK (run->range == 0);
        CHECK (run->rangeIteration >= 3);      // it is on its third pass
        CHECK_FALSE (run->isFinished());
    }

    /*  THE ADVANCE, sent partway through a pass. */
    REQUIRE (engine.submit (origin::cli, "run.advance", { osc::Value::string (runId) }));

    const auto advancedAt = sink.written;

    for (int i = 0; i < 150; ++i)
        oneTick();

    rig.host.setBlockSink (nullptr);

    /*  --- the render ------------------------------------------------------ */
    const auto first = sink.firstSoundAt (0);
    REQUIRE (first >= 0);

    const auto changeAt = settlesIntoSegmentAt (sink, startedAt, 1);

    REQUIRE (changeAt > advancedAt);

    /*  IT WAITED FOR THE BOUNDARY. The advance was sent partway through a pass,
        so the second range begins at the end of THAT pass - a whole number of
        seconds after the sound started, and not at the moment of the asking. */
    const auto sinceFirst = changeAt - first;
    const auto passes = (sinceFirst + rate / 2) / rate;
    const auto error = sinceFirst - passes * rate;

    INFO ("advance was asked at " << (advancedAt - first) << " samples in, and the second"
           " range began at " << sinceFirst << " - " << passes << " whole passes"
           << (error >= 0 ? " plus " : " minus ") << std::abs (error) << " samples");

    CHECK (std::abs (error) <= blockSize + 40);

    /*  And it did not simply stop: the second range is what plays afterwards. */
    {
        const auto* run = runs.find (runId);
        REQUIRE (run != nullptr);
        CHECK (run->range == 1);
    }
}

//==============================================================================
TEST_CASE ("media: how long a file is, asked without an engine")
{
    /*  §3.13's solver cannot answer "is this cue still playing at T" without a
        duration, and nothing in Go.dot has ever known one.

        ASKED OF JUCE AND NOT OF TRACKTION, which the namespace draft first
        said: `te::AudioFile` needs a `te::Engine&`, and at the moment a show is
        read there is none - `wfg tree` and `wfg validate` build no audio at
        all. Tracktion reads through the same JUCE readers, so the two agree
        about every format the engine can actually play. */
    ScopedStorage storage;
    constexpr int rate = 48000;

    const auto tone = writeSegmentedTone (storage.folder, rate);
    REQUIRE (tone.existsAsFile());

    CHECK (audio::mediaDurationSeconds (tone.getFullPathName().toStdString())
             == doctest::Approx (3.0).epsilon (0.001));

    /*  A FILE THAT WILL NOT READ IS NOUGHT, and that is the answer rather than
        an error: a missing file has failed the ARM and never the load since
        Phase 2, and the solver reads a nought as "I do not know how long this
        is" rather than as "this has ended". */
    CHECK (audio::mediaDurationSeconds ("") == doctest::Approx (0.0));
    CHECK (audio::mediaDurationSeconds (storage.folder.getChildFile ("absent.wav")
                                          .getFullPathName().toStdString())
             == doctest::Approx (0.0));

    const auto notAudio = storage.folder.getChildFile ("notes.txt");
    notAudio.replaceWithText ("this is not a sound");
    CHECK (audio::mediaDurationSeconds (notAudio.getFullPathName().toStdString())
             == doctest::Approx (0.0));
}

//==============================================================================
TEST_CASE ("offset: a start offset plays the part of the file it names, and stops when it ends")
{
    /*  THE ROW THAT DID NOTHING. `media/startOffset` has been in the parameter
        table since Phase 2, the grammar has always accepted it and `validate()`
        has always refused it beside a range - and no code ever read it, so a
        show that asked to start two seconds in started at the top. Found by
        auditing §13's claims against the code rather than by anybody hearing
        it.

        TWO PROPERTIES, and the second is the one that is easy to miss.

        The first: the cue sounds from the offset. The second: it STOPS at the
        end of the file rather than playing the file and then `startOffset`
        seconds of nothing. The clip's length has to shorten with the offset, or
        the launcher goes on reporting that it is playing for as long as the
        offset lasts - and `Runner::observeEdges` waits on a stopped edge, so
        the run would end late by exactly that much. */
    constexpr int rate = 48000;
    constexpr int blockSize = 128;

    HostRig rig;

    audio::HostSettings settings;
    settings.sampleRate = rate;
    settings.blockSize = blockSize;
    settings.outputChannels = 2;

    REQUIRE (rig.host.start (settings));

    audio::EditSpec spec;
    spec.tracks = 1;
    spec.channelsPerTrack = 1;
    spec.slots = 1;
    REQUIRE (rig.host.buildEdit (spec));

    const auto tone = writeSegmentedTone (rig.storage.folder, rate);
    REQUIRE (tone.existsAsFile());

    /*  No ranges and an offset of one second: the Phase 2 whole-file shape,
        starting at the second segment. */
    REQUIRE (rig.host.setTrackRanges (0, tone.getFullPathName().toStdString(), {}, 1.0));
    REQUIRE (rig.host.waitForTrackSourceReady (0, 10000));

    auto* matrix = rig.host.trackMatrix (0);
    REQUIRE (matrix != nullptr);
    matrix->setLevelDb (0.0f);
    matrix->setGain (0, 0, 1.0f);
    matrix->snapToTargets();

    for (int i = 0; i < 8; ++i)
        rig.host.processBlock();

    RecordingSink sink;
    sink.prepare (settings.outputChannels, rate * 4);
    rig.host.setBlockSink (&sink);

    const auto target = rig.host.clock().samplesElapsed() + 4 * (rate / 50);
    REQUIRE (rig.host.launchTrackAt (0, 0, rig.host.beatsAtSample (target)));

    for (int block = 0; block < 3 * rate / blockSize + 400; ++block)
        rig.host.processBlock();

    rig.host.setBlockSink (nullptr);

    const auto first = sink.firstSoundAt (0);
    REQUIRE (first >= 0);

    /*  IT STARTS AT THE SECOND SEGMENT. Starting at the first would mean the
        offset never reached the clip - which is what happened for two Tracktion
        reasons at once, since `disableLooping` assigns an offset outright and
        `setLength` subtracts the change in length from it. */
    CHECK (segmentOf (sink.buffer.getSample (0, first + 1000)) == 1);

    /*  And it runs on into the third, so the offset moved the start rather than
        trimming the file to one segment. */
    REQUIRE (sink.written > first + rate + rate / 2);
    CHECK (segmentOf (sink.buffer.getSample (0, first + rate + rate / 2)) == 2);

    /*  AND IT IS OVER AFTER TWO SECONDS, not three. Sampled a quarter of a
        second past where the shortened clip ends, which is well inside the
        second the un-shortened one would still have been sounding through. */
    REQUIRE (sink.written > first + 2 * rate + rate / 4);
    CHECK (segmentOf (sink.buffer.getSample (0, first + 2 * rate + rate / 4)) == -1);

    CHECK_FALSE (rig.host.isTrackPlaying (0));
}

TEST_CASE ("offset: one past the end of the file is a failed arm, not a silence")
{
    /*  Asked where a range's bounds are asked, and for the same reason: the
        document could not have known how long the file is, because a show is
        authored on one machine and its media copied onto another. */
    constexpr int rate = 48000;

    HostRig rig;

    audio::HostSettings settings;
    settings.sampleRate = rate;
    settings.blockSize = 128;
    settings.outputChannels = 2;

    REQUIRE (rig.host.start (settings));

    audio::EditSpec spec;
    spec.tracks = 1;
    spec.channelsPerTrack = 1;
    spec.slots = 1;
    REQUIRE (rig.host.buildEdit (spec));

    const auto tone = writeSegmentedTone (rig.storage.folder, rate);
    REQUIRE (tone.existsAsFile());

    CHECK_FALSE (rig.host.setTrackRanges (0, tone.getFullPathName().toStdString(), {}, 5.0));

    INFO (rig.host.lastError());
    CHECK (rig.host.lastError().find ("start offset") != std::string::npos);

    /*  And an offset the file DOES contain is fine, so the refusal is about the
        number rather than about offsets. */
    CHECK (rig.host.setTrackRanges (0, tone.getFullPathName().toStdString(), {}, 2.0));
}

TEST_CASE ("media: a file that is there and is not audio is a failed arm, and the voice stays whole")
{
    /*  THE CASE THE 2026-09-24 TRACKTION PIN CHANGED. Tracktion now builds no
        node for a clip whose file it cannot read, so a slot pointed at one
        loses its nodes at the next rebuild and its launch handle answers to
        nothing (the track stays: CueOutputPlugin holds it in the graph).
        Before that pin the slot stayed and played silence. Either way the run
        waited for ever on a readiness that never came.

        So the arm refuses the file before the slot is touched, and the graph
        it leaves behind is the graph it found: same node count, and the same
        voice arms a real file straight afterwards. Without the refusal this
        case reads 16 nodes where it found 18. A `.wav` holding text is
        the shape that happens - a copy that stopped half way, a file renamed
        by somebody who thought the extension was the format. */
    constexpr int rate = 48000;

    HostRig rig;

    audio::HostSettings settings;
    settings.sampleRate = rate;
    settings.blockSize = 128;
    settings.outputChannels = 2;

    REQUIRE (rig.host.start (settings));

    audio::EditSpec spec;
    spec.tracks = 1;
    spec.channelsPerTrack = 1;
    spec.slots = 1;
    REQUIRE (rig.host.buildEdit (spec));

    const auto before = rig.host.inspectNodeIds();
    REQUIRE (before.nodes > 0);

    const auto broken = rig.storage.folder.getChildFile ("broken.wav");
    REQUIRE (broken.replaceWithText ("this is not a sound"));

    CHECK_FALSE (rig.host.setTrackRanges (0, broken.getFullPathName().toStdString(), {}));

    INFO (rig.host.lastError());
    CHECK (rig.host.lastError().find ("not audio") != std::string::npos);

    /*  The slot still holds what it held, so nothing left the graph. */
    CHECK (rig.host.inspectNodeIds().nodes == before.nodes);

    /*  And the voice is still a voice. */
    const auto tone = writeSegmentedTone (rig.storage.folder, rate);
    REQUIRE (tone.existsAsFile());

    CHECK (rig.host.setTrackRanges (0, tone.getFullPathName().toStdString(), {}));
    CHECK (rig.host.waitForTrackSourceReady (0, 10000));
    CHECK (rig.host.inspectNodeIds().nodes == before.nodes);
}

//==============================================================================
/*  A file whose sample value IS its position: value at sample n is n / total,
    so a rendered sample says where in the file it came from. The segmented tone
    above answers "which second"; this answers "which sample", which is what a
    landing has to be measured in.  */
namespace
{
    juce::File writeRamp (const juce::File& folder, int rate, int seconds = 4)
    {
        const auto file = folder.getChildFile ("ramp.wav");
        folder.createDirectory();

        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream { file.createOutputStream() };

        if (stream == nullptr)
            return {};

        auto writer = format.createWriterFor (stream,
                                              juce::AudioFormatWriterOptions{}
                                                .withSampleRate (static_cast<double> (rate))
                                                .withNumChannels (1)
                                                .withBitsPerSample (24));

        if (writer == nullptr)
            return {};

        const auto total = rate * seconds;
        juce::AudioBuffer<float> buffer { 1, total };

        for (int n = 0; n < total; ++n)
            buffer.setSample (0, n, 0.8f * static_cast<float> (n) / static_cast<float> (total));

        writer->writeFromAudioSampleBuffer (buffer, 0, total);
        return file;
    }

    /** Which sample of the ramp a rendered value came from, or -1. */
    int rampPositionOf (float sample, int rate, int seconds = 4)
    {
        if (sample <= 0.0f)
            return -1;

        return static_cast<int> (std::lround (static_cast<double> (sample) / 0.8
                                                * static_cast<double> (rate * seconds)));
    }
}

TEST_CASE ("M16: whether a second slot launching stops the first, on one track")
{
    /*  PRD §6.11 asks this BEFORE the allocator is written, because it decides
        what a sampler group's claim is (§3.25, *(proposed)*). If a track keeps
        one playing slot, a member launching stops whatever its track was
        playing - which is a sampler's choke group for free, and a claim can be
        per track. If both sound, a claim has to be per SLOT and a bank of eight
        cells is eight voices rather than one.

        REPORTED AND NOT ASSERTED. The answer is Tracktion's rather than
        Go.dot's, and what a measurement owes the author is the number. */
    constexpr int rate = 48000;
    constexpr int blockSize = 128;

    HostRig rig;

    audio::HostSettings settings;
    settings.sampleRate = rate;
    settings.blockSize = blockSize;
    settings.outputChannels = 2;

    REQUIRE (rig.host.start (settings));

    audio::EditSpec spec;
    spec.tracks = 1;
    spec.channelsPerTrack = 1;
    spec.slots = 2;
    REQUIRE (rig.host.buildEdit (spec));

    const auto tone = writeSegmentedTone (rig.storage.folder, rate);
    REQUIRE (tone.existsAsFile());

    /*  Two ranges on one track: the first segment in slot nought, the third in
        slot one. Different material, so the render says which is sounding. */
    REQUIRE (rig.host.setTrackRanges (0, tone.getFullPathName().toStdString(),
                                      { { 0.0, 1.0, 0 }, { 2.0, 3.0, 0 } }));
    REQUIRE (rig.host.waitForTrackSourceReady (0, 10000));

    auto* matrix = rig.host.trackMatrix (0);
    REQUIRE (matrix != nullptr);
    matrix->setLevelDb (0.0f);
    matrix->setGain (0, 0, 1.0f);
    matrix->snapToTargets();

    for (int i = 0; i < 8; ++i)
        rig.host.processBlock();

    const auto firstAt = rig.host.clock().samplesElapsed() + 4 * (rate / 50);
    REQUIRE (rig.host.launchTrackAt (0, 0, rig.host.beatsAtSample (firstAt)));

    for (int block = 0; block < rate / blockSize; ++block)
        rig.host.processBlock();

    REQUIRE (rig.host.trackPlayState (0, 0).playing);

    /*  What the track carried while only slot nought was playing, so the
        after-picture has a before to be read against. */
    RecordingSink before;
    before.prepare (settings.outputChannels, rate / 2);
    rig.host.setBlockSink (&before);

    for (int block = 0; block < rate / (4 * blockSize); ++block)
        rig.host.processBlock();

    rig.host.setBlockSink (nullptr);

    RecordingSink sink;
    sink.prepare (settings.outputChannels, rate * 2);
    rig.host.setBlockSink (&sink);

    const auto secondAt = rig.host.clock().samplesElapsed() + 4 * (rate / 50);
    REQUIRE (rig.host.launchTrackAt (0, 1, rig.host.beatsAtSample (secondAt)));

    for (int block = 0; block < rate / blockSize; ++block)
        rig.host.processBlock();

    rig.host.setBlockSink (nullptr);

    const bool slotZero = rig.host.trackPlayState (0, 0).playing;
    const bool slotOne = rig.host.trackPlayState (0, 1).playing;

    const auto tally = [] (const RecordingSink& rendered, int from)
    {
        int first = 0, third = 0, neither = 0;
        double sum = 0.0;
        int taken = 0;

        for (int n = from; n < rendered.written - 500; n += 97)
        {
            const auto value = rendered.buffer.getSample (0, n);
            const auto segment = segmentOf (value);

            if (segment == 0)      ++first;
            else if (segment == 2) ++third;
            else                   ++neither;

            sum += std::abs (value);
            ++taken;
        }

        return std::tuple { first, third, neither, taken > 0 ? sum / taken : 0.0 };
    };

    const auto [beforeFirst, beforeThird, beforeNeither, beforeLevel] = tally (before, 500);
    const auto [afterFirst, afterThird, afterNeither, afterLevel] = tally (sink, sink.written / 2);

    MESSAGE ("M16: before the second launch - slot 0's material " << beforeFirst
              << ", slot 1's " << beforeThird << ", neither " << beforeNeither
              << ", mean level " << juce::String (beforeLevel, 4).toStdString());
    MESSAGE ("M16: after it - slot 0's material " << afterFirst
              << ", slot 1's " << afterThird << ", neither " << afterNeither
              << ", mean level " << juce::String (afterLevel, 4).toStdString());
    MESSAGE ("M16: play states after - slot 0 "
              << std::string (slotZero ? "playing" : "stopped")
              << ", slot 1 " << std::string (slotOne ? "playing" : "stopped"));
    MESSAGE ("M16: THE ANSWER - a track "
              << std::string (slotZero
                                ? "KEEPS BOTH SLOTS PLAYING, so a sampler claim is per SLOT and a"
                                  " bank of eight cells is eight voices"
                                : "KEEPS ONE PLAYING SLOT, so a member launching chokes its track:"
                                  " a sampler's choke group for free, and a claim can be per TRACK"));

    /*  The one thing asserted is that the measurement happened: the second slot
        did start, or there is nothing to report either way. */
    CHECK (slotOne);
}

TEST_CASE ("M17: where a clip armed with a start offset actually begins")
{
    /*  Load-to-time places a cue at an offset and has to trust it to the
        sample: §13.9 sets the offset in prepare, which is a graph rebuild, and
        nudges anything already playing. This is the first half - the arm - and
        it is measured on a ramp, whose rendered value IS its position in the
        file.

        REPORTED WITH A BOUND rather than gated on an exact sample: a launch is
        placed at a block boundary, so the first sample out is the placement
        rounded up to one, and the number worth knowing is how far that is. */
    constexpr int rate = 48000;
    constexpr int blockSize = 128;
    constexpr int seconds = 4;

    HostRig rig;

    audio::HostSettings settings;
    settings.sampleRate = rate;
    settings.blockSize = blockSize;
    settings.outputChannels = 2;

    REQUIRE (rig.host.start (settings));

    audio::EditSpec spec;
    spec.tracks = 1;
    spec.channelsPerTrack = 1;
    spec.slots = 1;
    REQUIRE (rig.host.buildEdit (spec));

    const auto ramp = writeRamp (rig.storage.folder, rate, seconds);
    REQUIRE (ramp.existsAsFile());

    auto* matrix = rig.host.trackMatrix (0);
    REQUIRE (matrix != nullptr);
    matrix->setLevelDb (0.0f);
    matrix->setGain (0, 0, 1.0f);
    matrix->snapToTargets();

    for (const double offset : { 0.5, 1.25, 2.0 })
    {
        REQUIRE (rig.host.setTrackRanges (0, ramp.getFullPathName().toStdString(), {}, offset));
        REQUIRE (rig.host.waitForTrackSourceReady (0, 10000));

        for (int i = 0; i < 8; ++i)
            rig.host.processBlock();

        RecordingSink sink;
        sink.prepare (settings.outputChannels, rate);
        rig.host.setBlockSink (&sink);

        const auto target = rig.host.clock().samplesElapsed() + 4 * (rate / 50);
        REQUIRE (rig.host.launchTrackAt (0, 0, rig.host.beatsAtSample (target)));

        for (int block = 0; block < rate / (2 * blockSize); ++block)
            rig.host.processBlock();

        rig.host.setBlockSink (nullptr);

        const auto first = sink.firstSoundAt (0);
        REQUIRE (first >= 0);

        const auto landed = rampPositionOf (sink.buffer.getSample (0, first + 64), rate, seconds);
        const auto wanted = static_cast<int> (offset * rate) + 64;

        MESSAGE ("M17: an offset of " << offset << " s asked for sample " << wanted
                  << " and got " << landed << " - out by " << (landed - wanted)
                  << " samples (" << juce::String (1000.0 * (landed - wanted) / rate, 3)
                  << " ms)");

        /*  A block either way is the placement grid; anything larger means the
            offset itself missed rather than the launch instant. */
        CHECK (std::abs (landed - wanted) <= blockSize * 2);

        rig.host.stopTrack (0);

        for (int i = 0; i < 8; ++i)
            rig.host.processBlock();
    }

    MESSAGE ("M17: nudge on an already-playing clip is NOT measured here - "
              "`LaunchHandle::nudge` is not reachable from Go.dot's own code "
              "(no AudioHost entry point exposes it), so load-to-time relaunches "
              "rather than nudges until one is added");
}

TEST_CASE ("audio recovery: stable callbacks are required and an interrupted validation stays paused")
{
    audio::RecoveryGate gate;
    gate.started (true);
    REQUIRE (gate.resume (true));
    CHECK (gate.callback());
    gate.observe (0);
    CHECK (gate.observe (500).retry);
    CHECK (gate.paused());
    CHECK_FALSE (gate.callback());
    CHECK_FALSE (gate.resume());
    gate.stopped();
    gate.started (false); // wrong interface, rate, block or channel layout
    for (int i = 0; i < 20; ++i) CHECK_FALSE (gate.callback());
    CHECK (gate.observe (600).retry);
    CHECK_FALSE (gate.resume());
    gate.started (true);
    gate.observe (1000);
    CHECK_FALSE (gate.observe (1000).ready); // no callbacks is not recovery
    for (int i = 1; i <= 8; ++i)
    {
        CHECK_FALSE (gate.callback());
        const auto result = gate.observe (1000 + i * 40);
        CHECK (result.ready == (i >= 7));
    }
    gate.stopped(); // loss between validation and the resume command
    CHECK_FALSE (gate.resume());
    gate.started (true);
    gate.observe (2000);
    for (int i = 1; i <= 8; ++i) { gate.callback(); gate.observe (2000 + i * 40); }
    REQUIRE (gate.resume());
    CHECK (gate.callback());
}

TEST_CASE ("audio recovery: the same interface on another clock is told apart, steady, and never resumed")
{
    /*  PRD §6.2's second failure (2026-09-21): not the interface gone, its
        clock MOVED. The gate says `moved` only for the same box, only once it
        has been steady by the rule a return passes, and never lets the paused
        graph run on it - pretending the old show can continue at the new rate
        is the one thing the amendment forbids. */
    audio::RecoveryGate gate;
    gate.started (audio::Verdict::same);
    REQUIRE (gate.resume (true));
    gate.observe (0);

    gate.stopped();
    CHECK (gate.observe (10).retry);
    CHECK_FALSE (gate.observe (10).moved);

    gate.started (audio::Verdict::moved);
    gate.observe (100);
    CHECK_FALSE (gate.observe (100).moved);          // no callbacks yet is not steady
    CHECK_FALSE (gate.observe (100).retry);          // and nothing to put back by retrying

    for (int i = 1; i <= 8; ++i)
    {
        CHECK_FALSE (gate.callback());               // silent: the graph does not run
        const auto result = gate.observe (100 + i * 40);
        CHECK (result.moved == (i >= 7));
        CHECK_FALSE (result.ready);
        CHECK_FALSE (result.retry);
    }

    CHECK (gate.paused());
    CHECK_FALSE (gate.resume());

    //  A moved interface that stalls is gone again, and retried.
    CHECK (gate.observe (1000).retry);
    CHECK_FALSE (gate.observe (1000).moved);

    //  Another interface altogether is not a moved clock.
    gate.started (audio::Verdict::foreign);
    gate.observe (2000);
    for (int i = 1; i <= 8; ++i) { gate.callback(); gate.observe (2000 + i * 40); }
    CHECK_FALSE (gate.observe (2400).moved);
    CHECK (gate.observe (2400).retry);
}

TEST_CASE ("audio recovery: paused media keeps its position and resumes its existing launch handle")
{
    HostRig rig;
    audio::HostSettings settings;
    settings.sampleRate = 48000; settings.blockSize = 128; settings.outputChannels = 2;
    REQUIRE (rig.host.start (settings));
    audio::EditSpec spec; spec.tracks = 1; spec.channelsPerTrack = 1;
    REQUIRE (rig.host.buildEdit (spec));
    const auto tone = writeSteadyTone (rig.storage.folder, 1, settings.sampleRate);
    REQUIRE (rig.host.setTrackSource (0, 0, tone.getFullPathName().toStdString()));
    REQUIRE (rig.host.waitForTrackSourceReady (0, 10000));
    for (int i = 0; i < 8; ++i) rig.host.processBlock();
    REQUIRE (rig.host.launchTrackAt (0, 0, rig.host.beatsAtSample (rig.host.clock().samplesElapsed() + 1024)));
    for (int i = 0; i < 100; ++i) rig.host.processBlock();
    REQUIRE (rig.host.trackPlayState (0).playing);
    const auto sample = rig.host.clock().samplesElapsed();
    const auto position = rig.host.trackPlayState (0).playedBeats;
    audio::RecoveryGate gate;
    gate.started (true);
    gate.observe (0);
    for (int i = 1; i <= 8; ++i)
    {
        if (gate.callback()) rig.host.processBlock();
        gate.observe (i * 40);
    }
    CHECK (rig.host.clock().samplesElapsed() == sample);
    CHECK (rig.host.trackPlayState (0).playedBeats == position);
    REQUIRE (gate.resume());
    for (int i = 0; i < 10; ++i) if (gate.callback()) rig.host.processBlock();
    CHECK (rig.host.clock().samplesElapsed() == sample + 1280);
    CHECK (rig.host.trackPlayState (0).playing);
    CHECK (rig.host.trackPlayState (0).playedBeats == doctest::Approx (position + 1280.0 / 48000.0));
}

TEST_CASE ("audio recovery: connection state is logged and failed validation cannot resume")
{
    Engine engine;
    audio::AudioState state;
    audio::registerAudioCommands (engine.commands(), state);
    state.status = "running";
    state.test.type = 1;
    engine.submit ("engine", "audio.connection", { osc::Value::boolean (false) });
    CHECK (engine.processTick (5).applied == 1);
    CHECK (state.status == "noClock");
    CHECK (state.test.type == 0);
    state.resumePlayback = [] { return false; };
    engine.submit ("engine", "audio.connection", { osc::Value::boolean (true) });
    CHECK (engine.processTick (5).rejected == 1);
    CHECK (state.status == "noClock");
    state.resumePlayback = [] { return true; };
    engine.submit ("engine", "audio.connection", { osc::Value::boolean (true) });
    CHECK (engine.processTick (5).applied == 1);
    CHECK (state.status == "running");
    CHECK (state.settingsError.empty());
}

TEST_CASE ("audio recovery: Doh! is let through an outage, as Esc is, and a GO still is not")
{
    /*  PRD §3.32 (D1, 2026-10-01): Doh! is recovery, so it passes the outage
        the way Esc does (namespace draft §11.1) - the pointer goes back at once,
        the rest waits for the clock. A GO is still dropped, never queued (PRD
        §6.2). Failed before D1: `go.doh` was an unknown command. */
    Engine engine;
    doc::ShowDocument document;
    cue::RunTable runs;
    cue::Focus focus;
    auto runIds = doc::IdRegistry::withSeed (37);
    cue::Runner runner { document, runs, runIds, focus };
    audio::AudioState state;

    doc::registerDocumentCommands (engine.commands(), document);
    cue::registerCueCommands (engine.commands(), document, focus);
    cue::registerRunCommands (engine.commands(), runs);
    cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);
    audio::registerAudioCommands (engine.commands(), state);
    audio::registerAudioSettingsCommands (engine, document, runner, runs, state);

    const auto listId = document.createList ("Show").id;
    const auto first = document.createCue (listId, 0, "memo", "One").id;
    const auto second = document.createCue (listId, 1, "memo", "Two").id;
    REQUIRE (document.setAttribute (cue::standbyAddressOf (listId), first).ok);

    state.status = "running";
    engine.submit ("cli", "go", {});
    REQUIRE (engine.processTick (10).applied == 1);
    REQUIRE (document.getAttribute (cue::standbyAddressOf (listId)) == std::optional<std::string> (second));

    engine.submit ("engine", "audio.connection", { osc::Value::boolean (false) });
    REQUIRE (engine.processTick (11).applied == 1);
    REQUIRE (state.status == "noClock");

    //  At the frozen tick: applied, and the pointer is back.
    engine.submit ("cli", "go.doh", {});
    CHECK (engine.processTick (11).applied == 1);
    CHECK (document.getAttribute (cue::standbyAddressOf (listId)) == std::optional<std::string> (first));

    engine.submit ("cli", "go", {});
    CHECK (engine.processTick (11).rejected == 1);
    CHECK (engine.lastError().find ("audio-reconnecting") != std::string::npos);
}

TEST_CASE ("audio recovery: a moved clock stops what plays the Esc way, says so, and asks to be followed")
{
    /*  PRD §6.2's answer to the clock moving, as the engine applies it
        (2026-09-28): a stop, then an adaptation. At setup nothing plays and the
        show simply follows; mid-show what plays is stopped the Esc way
        (decision DG) - in the handler, before the scheduler can see a member
        fall silent on the new graph and start the next one - and prepared
        runs are revoked, as for any settings operation. */
    Engine engine;
    doc::ShowDocument document;
    cue::RunTable runs;
    cue::Focus focus;
    auto runIds = doc::IdRegistry::withSeed (31);
    cue::Runner runner { document, runs, runIds, focus };
    audio::AudioState state;
    audio::registerAudioCommands (engine.commands(), state);
    audio::registerAudioSettingsCommands (engine, document, runner, runs, state);

    auto follows = 0;
    state.followClock = [&follows] { ++follows; };
    state.sampleRate = 48000;

    const auto moved = [&engine] (int rate, int block)
    {
        engine.submit ("engine", "audio.clockMoved", { osc::Value::int32 (rate), osc::Value::int32 (block) });
    };

    //  An interface that is running has no moved clock to follow.
    state.status = "running";
    moved (96000, 256);
    CHECK (engine.processTick (10).rejected == 1);
    CHECK (engine.lastError().find ("audio-not-reconnecting") != std::string::npos);
    CHECK (follows == 0);

    //  At setup: nothing plays, nothing is stopped, and the show follows.
    engine.submit ("engine", "audio.connection", { osc::Value::boolean (false) });
    CHECK (engine.processTick (11).applied == 1);
    moved (96000, 256);
    CHECK (engine.processTick (12).applied == 1);
    CHECK (follows == 1);
    CHECK (state.rateMovedTick == 12);
    CHECK (state.rateMoved == "The interface's clock moved from 48000 Hz to 96000 Hz; the show runs at 96000 Hz.");

    //  Mid-show: a group playing a member, and a prepared run beside them.
    runs.create ("RUNGRP01", "CUEGRP01", "group");
    runs.create ("RUNMED01", "CUEMED01", "media", "RUNGRP01");
    runs.create ("RUNPRE01", "CUEPRE01", "media");
    runs.find ("RUNGRP01")->state = cue::runState::playing;
    runs.find ("RUNMED01")->state = cue::runState::playing;
    runs.find ("RUNPRE01")->state = cue::runState::preparing;

    moved (44100, 512);
    CHECK (engine.processTick (20).applied == 1);
    CHECK (follows == 2);
    CHECK (runs.find ("RUNGRP01")->state == cue::runState::stopping);
    CHECK_FALSE (runs.find ("RUNGRP01")->skipFooter);                   // the Esc way: its footer runs
    CHECK (runs.find ("RUNMED01")->state == cue::runState::playing);    // its group brings it down, in order
    CHECK (runs.find ("RUNPRE01")->state == cue::runState::done);       // revoked
    CHECK (state.rateMoved.find ("to 44100 Hz") != std::string::npos);
    CHECK (state.rateMoved.find ("the cues that were playing were stopped") != std::string::npos);
    CHECK (state.rateMovedTick == 20);

    //  Apply is let through an outage (decision DI) and refused on its own
    //  terms while anything plays - not as `audio-reconnecting`.
    engine.submit ("udp:127.0.0.1:9000", "audio.apply", {});
    CHECK (engine.processTick (21).rejected == 1);
    CHECK (engine.lastError().find ("audio-busy") != std::string::npos);

    //  Everything else still is, GO among it: dropped, never queued.
    engine.submit ("udp:127.0.0.1:9000", "audio.defaults", {});
    CHECK (engine.processTick (22).rejected == 1);
    CHECK (engine.lastError().find ("audio-reconnecting") != std::string::npos);
}

//==============================================================================
/*  PHASE 9a, HEARD: the EQ on the voice, through the whole chain a show uses -
    the document's rows, the Runner's arm and its live push, the HostPlayer's
    two doors, the EqPlugin before the output stage - measured in the render.
    A sine rather than the steady constant every other case here plays, because
    a peak at one kilohertz leaves a constant exactly alone. */
namespace
{
    juce::File writeSineTone (const juce::File& folder, int rate, double frequency,
                              float amplitude, int seconds)
    {
        const auto file = folder.getChildFile ("sine.wav");
        folder.createDirectory();

        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream { file.createOutputStream() };

        if (stream == nullptr)
            return {};

        auto writer = format.createWriterFor (stream,
                                              juce::AudioFormatWriterOptions{}
                                                .withSampleRate (static_cast<double> (rate))
                                                .withNumChannels (1)
                                                .withBitsPerSample (16));

        if (writer == nullptr)
            return {};

        juce::AudioBuffer<float> buffer { 1, rate * std::max (1, seconds) };
        auto* data = buffer.getWritePointer (0);
        const auto step = 2.0 * juce::MathConstants<double>::pi * frequency / rate;

        for (int n = 0; n < buffer.getNumSamples(); ++n)
            data[n] = amplitude * static_cast<float> (std::sin (step * n));

        writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
        return file;
    }

    double rmsOf (const RecordingSink& sink, int channel)
    {
        const auto* samples = sink.buffer.getReadPointer (channel);
        auto sum = 0.0;

        for (int n = 0; n < sink.written; ++n)
            sum += static_cast<double> (samples[n]) * static_cast<double> (samples[n]);

        return sink.written > 0 ? std::sqrt (sum / sink.written) : 0.0;
    }
}

TEST_CASE ("eq: a peak on the voice doubles a sine at its centre, written live through node.set")
{
    constexpr int rate = 48000;
    constexpr int blockSize = 64;
    constexpr int samplesPerTick = rate / 50;
    constexpr int blocksPerTick = samplesPerTick / blockSize;
    static_assert (blocksPerTick * blockSize == samplesPerTick, "a tick must be whole blocks");

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

    const auto tone = writeSineTone (rig.storage.folder, rate, 1000.0, 0.25f, 6);
    REQUIRE (tone.existsAsFile());

    //  --- a show: one media cue routed to one bus ------------------------------
    Engine engine;
    doc::ShowDocument document;
    cue::RunTable runs;
    cue::Focus focus;
    auto runIds = doc::IdRegistry::withSeed (23);
    cue::Runner runner { document, runs, runIds, focus };

    engine.log().openInMemory ({});
    doc::registerDocumentCommands (engine.commands(), document);
    cue::registerCueCommands (engine.commands(), document, focus);
    cue::registerRunCommands (engine.commands(), runs);
    cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);

    const auto listId = document.createList ("Sound").id;
    const auto mediaId = document.createCue (listId, 0, "media", "Sine").id;
    document.setAttribute ("/godot/cue/" + mediaId + "/file", tone.getFileName().toStdString());

    auto audioNode = document.root().getChildWithName ("Audio");
    audioNode.setProperty (juce::Identifier ("tracks"), 1, nullptr);

    juce::ValueTree bus { "Bus" };
    bus.setProperty (juce::Identifier ("id"), "EQBUS001", nullptr);
    bus.setProperty (juce::Identifier ("name"), "Main", nullptr);
    bus.setProperty (juce::Identifier ("firstChannel"), 0, nullptr);
    bus.setProperty (juce::Identifier ("width"), 1, nullptr);
    audioNode.appendChild (bus, nullptr);

    auto media = document.findById (mediaId);
    juce::ValueTree route { "Route" };
    route.setProperty (juce::Identifier ("id"), "EQRTE001", nullptr);
    route.setProperty (juce::Identifier ("bus"), "EQBUS001", nullptr);
    route.setProperty (juce::Identifier ("gains"), "1", nullptr);
    media.appendChild (route, nullptr);

    document.setAttribute (cue::standbyAddressOf (listId), mediaId);

    //  --- the audio side ----------------------------------------------------------
    audio::HostPlayer player { rig.host, engine };
    runner.setPlayer (&player);
    runner.setSamplesPerTick (samplesPerTick);
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

    //  Flat: what a second of it sounds like untouched.
    for (int i = 0; i < 10; ++i)
        oneTick();

    RecordingSink flat;
    flat.prepare (2, rate);
    rig.host.setBlockSink (&flat);

    for (int i = 0; i < 50; ++i)
        oneTick();

    rig.host.setBlockSink (nullptr);

    const auto flatRms = rmsOf (flat, 0);
    REQUIRE (flatRms > 0.1);

    //  +6 dB at 1 kHz, written to the CUE while it sounds - the rotary's path.
    rt::resetCounts();

    REQUIRE (engine.submit ("cli", "node.set", { osc::Value::string ("/godot/cue/" + mediaId + "/eqB2Freq"),
                                                 osc::Value::float64 (1000.0) }));
    REQUIRE (engine.submit ("cli", "node.set", { osc::Value::string ("/godot/cue/" + mediaId + "/eqB2Gain"),
                                                 osc::Value::float64 (6.0) }));

    //  The tick that applies it, one to push it, and a few for the filter to settle.
    for (int i = 0; i < 10; ++i)
        oneTick();

    RecordingSink shaped;
    shaped.prepare (2, rate);
    rig.host.setBlockSink (&shaped);

    for (int i = 0; i < 50; ++i)
        oneTick();

    rig.host.setBlockSink (nullptr);

    const auto shapedRms = rmsOf (shaped, 0);
    const auto gainDb = 20.0 * std::log10 (shapedRms / flatRms);

    INFO ("flat " << flatRms << " shaped " << shapedRms << " = " << gainDb << " dB");
    CHECK (gainDb == doctest::Approx (6.0).epsilon (0.03));

    //  And nothing Go.dot's allocated on the audio thread on the way.
    if (rt::isCounting())
        CHECK (rt::violations() == 0);

    //  eq.reset takes it back, live.
    REQUIRE (engine.submit ("cli", "eq.reset", { osc::Value::string (mediaId) }));

    for (int i = 0; i < 10; ++i)
        oneTick();

    RecordingSink again;
    again.prepare (2, rate);
    rig.host.setBlockSink (&again);

    for (int i = 0; i < 50; ++i)
        oneTick();

    rig.host.setBlockSink (nullptr);

    CHECK (20.0 * std::log10 (rmsOf (again, 0) / flatRms) == doctest::Approx (0.0).scale (1.0).epsilon (0.05));
}

//==============================================================================
/*  H3 (namespace draft §23.6), HEARD: what a voice's own processing holds
    after each way of stopping it. A peak twelve decibels up at a hundred hertz
    with a Q of ten rings for hundreds of milliseconds after its input has gone
    - the tail a delay or a reverb on an insert would leave, from the EQ every
    voice has, so no plugin is needed. Esc stops the cue and lets that ring on
    at the cue's level, as §4.4 lets it (the panic fade at nought, so what is
    heard is the stop itself). A double Esc takes the sounding voice to silence
    over the press's own tick with its EQ left whole under the fade, so its
    kill a tick later lands on nothing; a double Esc once Esc's cue has ended
    cuts the tail within the press's tick, by the sweep alone; the pane's kill
    is silent within a block of the kill; and each is exactly silent the ticks
    after, a cue whose lane goes on moving its level included. Then the next GO
    plays the standby both keys leave armed, on the other voice, at the level
    it had before - the sweep leaves the voice the next GO needs as its arm
    made it - and the killed cue, fired again onto the voice the kill or the
    sweep took to silence and emptied, plays at its own level too: its arm puts
    the level back, and its EQ rings up again because it was emptied, not
    switched out. */
TEST_CASE ("double Esc: a voice's own EQ is silent within a block of the kill, where Esc lets it ring - and the next GO plays as before")
{
    constexpr int rate = 48000;
    constexpr int blockSize = 64;
    constexpr int samplesPerTick = rate / 50;
    constexpr int blocksPerTick = samplesPerTick / blockSize;
    static_assert (blocksPerTick * blockSize == samplesPerTick, "a tick must be whole blocks");

    HostRig rig;

    audio::HostSettings settings;
    settings.sampleRate = rate;
    settings.blockSize = blockSize;
    settings.outputChannels = 2;
    REQUIRE (rig.host.start (settings));

    //  Two voices: the cue that rings on one, the standby armed ahead on the other.
    audio::EditSpec spec;
    spec.tracks = 2;
    spec.channelsPerTrack = 1;
    REQUIRE (rig.host.buildEdit (spec));

    const auto tone = writeSineTone (rig.storage.folder, rate, 100.0, 0.25f, 8);
    REQUIRE (tone.existsAsFile());

    //  --- a show: two media cues through the same EQ, one bus ------------------
    Engine engine;
    doc::ShowDocument document;
    cue::RunTable runs;
    cue::Focus focus;
    auto runIds = doc::IdRegistry::withSeed (29);
    cue::Runner runner { document, runs, runIds, focus };

    engine.log().openInMemory ({});
    doc::registerDocumentCommands (engine.commands(), document);
    cue::registerCueCommands (engine.commands(), document, focus);
    cue::registerRunCommands (engine.commands(), runs);
    cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);

    auto audioNode = document.root().getChildWithName ("Audio");
    audioNode.setProperty (juce::Identifier ("tracks"), 2, nullptr);

    juce::ValueTree bus { "Bus" };
    bus.setProperty (juce::Identifier ("id"), "KZB00001", nullptr);
    bus.setProperty (juce::Identifier ("name"), "Main", nullptr);
    bus.setProperty (juce::Identifier ("firstChannel"), 0, nullptr);
    bus.setProperty (juce::Identifier ("width"), 1, nullptr);
    audioNode.appendChild (bus, nullptr);

    const auto listId = document.createList ("Sound").id;

    //  Esc a cut, and a GO as soon as a script presses it.
    REQUIRE (document.setAttribute ("/godot/audio/panicFade", "0").ok);
    REQUIRE (document.setAttribute ("/godot/list/goDebounce", "0").ok);

    std::vector<std::string> cueIds;

    for (const auto* name : { "Ring", "After" })
    {
        const auto id = document.createCue (listId, static_cast<int> (cueIds.size()), "media", name).id;
        REQUIRE_FALSE (id.empty());
        document.setAttribute ("/godot/cue/" + id + "/file", tone.getFileName().toStdString());

        //  Strings, as a show file holds them: the suite runs under fr_FR as well.
        REQUIRE (document.setAttribute ("/godot/cue/" + id + "/eqB1Freq", "100").ok);
        REQUIRE (document.setAttribute ("/godot/cue/" + id + "/eqB1Gain", "12").ok);
        REQUIRE (document.setAttribute ("/godot/cue/" + id + "/eqB1Q", "10").ok);

        auto media = document.findById (id);
        juce::ValueTree route { "Route" };
        route.setProperty (juce::Identifier ("id"), cueIds.empty() ? "KZR00001" : "KZR00002", nullptr);
        route.setProperty (juce::Identifier ("bus"), "KZB00001", nullptr);
        route.setProperty (juce::Identifier ("gains"), "1", nullptr);
        media.appendChild (route, nullptr);

        cueIds.push_back (id);
    }

    const auto ringCue = cueIds[0];
    const auto afterCue = cueIds[1];
    REQUIRE (document.setAttribute (cue::standbyAddressOf (listId), ringCue).ok);

    //  --- the audio side ----------------------------------------------------------
    audio::HostPlayer player { rig.host, engine };
    runner.setPlayer (&player);
    runner.setSamplesPerTick (samplesPerTick);
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

    const auto record = [&] (RecordingSink& sink, int ticks)
    {
        sink.prepare (2, ticks * samplesPerTick);
        rig.host.setBlockSink (&sink);

        for (int i = 0; i < ticks; ++i)
            oneTick();

        rig.host.setBlockSink (nullptr);
    };

    //  The loudest sample from `from` on.
    const auto loudest = [] (const RecordingSink& sink, int from)
    {
        return sink.written > from ? sink.buffer.getMagnitude (0, from, sink.written - from) : 0.0f;
    };

    //  The cue's latest run, found again after every tick: the table's storage moves.
    const auto runOf = [&runs] (const std::string& cueId)
    {
        const cue::Run* found = nullptr;

        for (const auto& each : runs.all())
            if (each.cue == cueId)
                found = runs.find (each.id);

        return found;
    };

    const auto sounding = [&rig, &runOf] (const std::string& cueId)
    {
        const auto* run = runOf (cueId);
        return run != nullptr && run->track >= 0 && rig.host.trackPlayState (run->track).playing;
    };

    for (int i = 0; i < 4; ++i)
        oneTick();

    REQUIRE (engine.submit ("udp:127.0.0.1:9000", "go", {}));

    for (int i = 0; i < 600 && ! sounding (ringCue); ++i)
        oneTick();

    REQUIRE (sounding (ringCue));

    //  Settled: a quarter at twelve decibels up is a sine of about one.
    for (int i = 0; i < 25; ++i)
        oneTick();

    RecordingSink steady;
    record (steady, 10);

    const auto steadyRms = rmsOf (steady, 0);
    INFO ("steady " << steadyRms);
    REQUIRE (steadyRms > 0.5);

    //  The standby armed ahead on the other voice, which both keys leave ready (§23.3).
    REQUIRE (runOf (afterCue) != nullptr);
    REQUIRE (runOf (afterCue)->state == cue::runState::armed);
    REQUIRE (runOf (afterCue)->track >= 0);
    REQUIRE (runOf (afterCue)->track != runOf (ringCue)->track);

    const auto ringRun = runOf (ringCue)->id;
    const auto ringVoice = runOf (ringCue)->track;

    SUBCASE ("Esc: a stop, and the EQ rings on at the cue's level")
    {
        REQUIRE (engine.submit ("cli", "run.stopAll", {}));
        oneTick();                                      // the handler: the root asked to stop
        oneTick();                                      // enforceStops: the stop, the clip gone

        RecordingSink tail;
        record (tail, 2);

        INFO ("after Esc " << loudest (tail, 0));
        CHECK (loudest (tail, 0) > 0.2f);
    }

    SUBCASE ("double Esc: the sounding voice faded out over the press's tick with its EQ whole, and its kill lands on silence")
    {
        rt::resetCounts();
        REQUIRE (engine.submit ("cli", "run.killAll", {}));

        RecordingSink pressing, killing, quiet;
        record (pressing, 1);                           // the handler: the stops let go, the sweep
        record (killing, 1);                            // enforceStops: the kill
        record (quiet, 2);

        /*  THE BOOST IS STILL IN at the head of the press's tick: the sweep
            leaves a sounding voice's EQ to its kill (FW). Cleared under the
            sound, the ring would fall to the bare quarter in one sample and
            take a good part of a period to build again, while the level, on
            its one-tick way down, is still three quarters up at sample 256. And
            that way down is over by the tick's end - the sweep silenced the
            voice itself, a tick before its kill: nothing at all is heard in
            the kill's tick. */
        const auto opening = pressing.buffer.getMagnitude (0, 0, 4 * blockSize);

        INFO ("the press's first four blocks " << opening << ", the kill's tick " << loudest (killing, 0)
                << ", after it " << loudest (quiet, 0));
        CHECK (opening > 0.5f);
        CHECK (juce::exactlyEqual (loudest (killing, 0), 0.0f));
        CHECK (juce::exactlyEqual (loudest (quiet, 0), 0.0f));

        if (rt::isCounting())
            CHECK (rt::violations() == 0);
    }

    SUBCASE ("Esc, then double Esc once the cue has ended: the sweep cuts the tail Esc left ringing, within a block of the press")
    {
        REQUIRE (engine.submit ("cli", "run.stopAll", {}));
        oneTick();
        oneTick();

        RecordingSink tail;
        record (tail, 2);

        INFO ("after Esc " << loudest (tail, 0));
        REQUIRE (loudest (tail, 0) > 0.2f);

        /*  NO RUN LEFT TO KILL: what cuts this tail is the sweep, in the
            press's own tick - the EQ of a voice nothing sounds on cleared at
            once. The level's twenty-millisecond way down alone would still let
            seven eighths of the ring through at the third block. */
        REQUIRE (runOf (ringCue)->isFinished());

        rt::resetCounts();
        REQUIRE (engine.submit ("cli", "run.killAll", {}));

        RecordingSink pressing, quiet;
        record (pressing, 1);
        record (quiet, 2);

        INFO ("in the press's tick " << loudest (pressing, 2 * blockSize) << ", after it " << loudest (quiet, 0));
        CHECK (loudest (pressing, 2 * blockSize) < 0.05f);
        CHECK (juce::exactlyEqual (loudest (quiet, 0), 0.0f));

        if (rt::isCounting())
            CHECK (rt::violations() == 0);
    }

    SUBCASE ("the pane's kill: the voice's own kill empties it")
    {
        rt::resetCounts();
        REQUIRE (engine.submit ("cli", "run.kill", { osc::Value::string (ringRun) }));
        oneTick();                                      // the handler: the run marked

        RecordingSink killing, quiet;
        record (killing, 1);                            // enforceStops: HostPlayer::kill on the voice
        record (quiet, 2);

        INFO ("in the kill's tick " << loudest (killing, 2 * blockSize) << ", after it " << loudest (quiet, 0));
        CHECK (loudest (killing, 2 * blockSize) < 0.05f);
        CHECK (juce::exactlyEqual (loudest (quiet, 0), 0.0f));

        if (rt::isCounting())
            CHECK (rt::violations() == 0);
    }

    SUBCASE ("the pane's kill of a cue its lane is moving: the lane does not bring the voice back")
    {
        /*  The lane moves the run's level every tick until `run.ended`, which
            comes a tick after the kill. Written over the kill's silence, that
            level brought back what the voice still gives - here the stop's own
            ten samples through the emptied EQ - and left it there once the run
            had gone. */
        REQUIRE (document.setAttribute ("/godot/cue/" + ringCue + "/levelLane", "0 0 8 -24").ok);

        for (int i = 0; i < 3; ++i)
            oneTick();

        rt::resetCounts();
        REQUIRE (engine.submit ("cli", "run.kill", { osc::Value::string (ringRun) }));
        oneTick();

        RecordingSink killing, quiet;
        record (killing, 1);
        record (quiet, 2);

        INFO ("in the kill's tick " << loudest (killing, 2 * blockSize) << ", after it " << loudest (quiet, 0));
        CHECK (loudest (killing, 2 * blockSize) < 0.05f);
        CHECK (juce::exactlyEqual (loudest (quiet, 0), 0.0f));

        /*  AND THE VOICE IS STILL AT SILENCE, whatever its chain holds: the
            lane wrote nothing over the kill's level, from the press to the
            run's end. Heard, that is only the few samples the stop seeds into
            the emptied EQ; read at the output stage, it is the whole of it. */
        const auto* stage = rig.host.trackMatrix (ringVoice);
        REQUIRE (stage != nullptr);
        INFO ("the voice's level " << stage->levelDb());
        CHECK (stage->levelDb() == doctest::Approx (audio::CueMatrix::silenceDb));

        if (rt::isCounting())
            CHECK (rt::violations() == 0);

        //  The lane goes, so the cue fired again below plays at its own level.
        REQUIRE (document.setAttribute ("/godot/cue/" + ringCue + "/levelLane", "").ok);
    }

    //  THE NEXT GO: the standby the press left armed, at the level it had.
    for (int i = 0; i < 20 && ! runOf (ringCue)->isFinished(); ++i)
        oneTick();

    REQUIRE (runOf (ringCue)->isFinished());
    REQUIRE (runOf (afterCue)->state == cue::runState::armed);

    REQUIRE (engine.submit ("udp:127.0.0.1:9000", "go", {}));

    for (int i = 0; i < 600 && ! sounding (afterCue); ++i)
        oneTick();

    REQUIRE (sounding (afterCue));

    for (int i = 0; i < 50; ++i)
        oneTick();

    RecordingSink again;
    record (again, 10);

    const auto againRms = rmsOf (again, 0);
    INFO ("steady " << steadyRms << ", the next GO " << againRms);
    CHECK (20.0 * std::log10 (againRms / steadyRms) == doctest::Approx (0.0).epsilon (0.01));

    /*  AND THE KILLED CUE AGAIN, ON THE VOICE THE PRESS EMPTIED. The standby
        played on the voice the sweep left alone; this is the other one, left
        at silence by the kill or the sweep, its EQ cleared. Its arm puts the
        level back, and its EQ rings up as it did the first time: emptied, not
        switched out. Fired by name, which is never debounced, once the cue
        just played has gone: the lowest voice free is the first one. */
    REQUIRE (engine.submit ("cli", "run.kill", { osc::Value::string (runOf (afterCue)->id) }));

    for (int i = 0; i < 20 && ! runOf (afterCue)->isFinished(); ++i)
        oneTick();

    REQUIRE (runOf (afterCue)->isFinished());
    REQUIRE (engine.submit ("cli", "cue.fire", { osc::Value::string (ringCue) }));

    for (int i = 0; i < 600 && ! sounding (ringCue); ++i)
        oneTick();

    REQUIRE (sounding (ringCue));
    REQUIRE (runOf (ringCue)->track == ringVoice);

    for (int i = 0; i < 50; ++i)
        oneTick();

    RecordingSink refired;
    record (refired, 10);

    const auto refiredRms = rmsOf (refired, 0);
    INFO ("steady " << steadyRms << ", the killed cue again on its own voice " << refiredRms);
    CHECK (20.0 * std::log10 (refiredRms / steadyRms) == doctest::Approx (0.0).epsilon (0.01));
}

//==============================================================================
TEST_CASE ("input tap: a block's inputs are copied before the graph runs, each one's peak taken once")
{
    /*  Phase 9b (namespace draft §18.4). The rack's input stage reads the tap
        during the block, so the tap must hold THIS block's inputs; a block
        pumped with none must read silence, never the last one again; and each
        input's peak is taken once a tick, the output meter's rule. All of it
        inside Go.dot's own part of the block, which allocates nothing. */
    HostRig rig;

    audio::HostSettings settings;
    settings.sampleRate = 48000;
    settings.blockSize = 128;
    settings.outputChannels = 2;
    settings.inputChannels = 2;

    REQUIRE (rig.host.start (settings));
    CHECK (rig.host.inputChannelCount() == 2);

    std::vector<float> left (128), right (128);

    for (int n = 0; n < 128; ++n)
    {
        left[static_cast<std::size_t> (n)] = 0.5f * std::sin (0.1f * static_cast<float> (n));
        right[static_cast<std::size_t> (n)] = 0.25f;
    }

    const float* inputs[] { left.data(), right.data() };

    rt::resetCounts();
    rig.host.processBlock (inputs, 2);
    CHECK (rt::violations() == 0);

    const auto* tapped = rig.host.inputTapChannel (0);
    REQUIRE (tapped != nullptr);

    auto same = true;

    for (int n = 0; n < 128; ++n)
        same = same && juce::exactlyEqual (tapped[n], left[static_cast<std::size_t> (n)]);

    CHECK (same);
    CHECK (rig.host.inputTapChannel (1)[64] == doctest::Approx (0.25f));

    /*  The peak, taken: the loudest sample, then nothing until another block. */
    CHECK (rig.host.takeInputPeak (1) == doctest::Approx (0.25f));
    CHECK (rig.host.takeInputPeak (0) > 0.49f);
    CHECK (rig.host.takeInputPeak (1) == doctest::Approx (0.0f));

    /*  A block with no inputs is silence, not the last block again. */
    rig.host.processBlock();
    CHECK (rig.host.inputTapChannel (1)[64] == doctest::Approx (0.0f));
    CHECK (rig.host.takeInputPeak (1) == doctest::Approx (0.0f));

    /*  An input the tap does not hold answers nothing rather than something. */
    CHECK (rig.host.inputTapChannel (2) == nullptr);
    CHECK (rig.host.takeInputPeak (7) == doctest::Approx (0.0f));
}

TEST_CASE ("input tap: an interface with no inputs holds none, and reads silence")
{
    HostRig rig;

    audio::HostSettings settings;
    settings.sampleRate = 48000;
    settings.blockSize = 128;
    settings.outputChannels = 2;

    REQUIRE (rig.host.start (settings));
    CHECK (rig.host.inputChannelCount() == 0);

    rig.host.processBlock();
    CHECK (rig.host.inputTapChannel (0) == nullptr);
    CHECK (rig.host.takeInputPeak (0) == doctest::Approx (0.0f));
}

//==============================================================================
TEST_CASE ("M10: rack channels beside the voices keep every node's identity unique")
{
    /*  Phase 9b (decision CK): every rack channel is a track after the voices,
        with the live input stage, the EQ, its own proxies and the output stage,
        and no launcher slot. A new shape of track is a new shape of graph, so
        the identity check M10 asked of the voices is asked of it too - beside
        the voices at the sizes a show uses, each channel with a chain of two -
        and every other one a sampling channel (Phase 9c, §19.2), its recorder
        between a plugin before it and one after. */
    HostRig rig;
    REQUIRE (rig.host.start (hostFor (8)));

    for (const int tracks : { 1, 8, 32, 64 })
        for (const int channels : { 1, 4, 8 })
            for (const int slots : { 1, 4 })
            {
                INFO ("tracks " << tracks << ", rack channels " << channels << ", slots " << slots);

                audio::EditSpec spec;
                spec.tracks = tracks;
                spec.slots = slots;

                for (int at = 0; at < channels; ++at)
                {
                    audio::RackChannelSpec channel;
                    channel.id = "CH0000" + std::to_string (10 + at);

                    for (int plugin = 0; plugin < 2; ++plugin)
                    {
                        audio::PluginSpec entry;
                        entry.id = "PG" + std::to_string (100000 + at * 10 + plugin);
                        entry.identifier = "godot:test-gain";
                        entry.beforeRecorder = plugin == 0;
                        channel.plugins.push_back (entry);
                    }

                    if (at % 2 == 1)
                    {
                        channel.takeSeconds = 0.05;
                        channel.layers = 2;
                    }

                    spec.rack.push_back (channel);
                }

                REQUIRE (rig.host.buildEdit (spec));
                CHECK (rig.host.trackCount() == tracks);
                CHECK (rig.host.allTrackCount() == tracks + channels);

                const auto report = rig.host.inspectNodeIds();

                INFO ("nodes " << report.nodes << ", duplicates " << report.duplicates);
                CHECK (report.duplicates == 0);
                CHECK (report.typedDuplicates == 0);
                CHECK (report.nodes > tracks + channels);
            }
}

//==============================================================================
TEST_CASE ("host player: an arm and a state asked for the same insert land in the order they were asked")
{
    /*  THE PLUGIN-VOICE HANDOFF'S THIRD FINDING (docs/handoffs/2026-09-26-
        plugin-voice-flakes.md). An arm snaps its cue's whole state onto the
        voice, and a state asked for after it - an undo in standby - reaches
        the message thread in the same ten-millisecond batch when the two are
        close. serviceArms kept two queues and applied every state before
        every arm, so the arm's OLDER state was the one the lane held: the
        "processed without the state" shape one of the CI sightings had. One
        queue now, in the order it was filled - and each way round, the one
        asked for last is the one held. */
    constexpr int rate = 48000;

    HostRig rig;

    audio::HostSettings settings;
    settings.sampleRate = rate;
    settings.blockSize = 128;
    settings.outputChannels = 2;
    REQUIRE (rig.host.start (settings));

    audio::EditSpec spec;
    spec.tracks = 1;
    spec.channelsPerTrack = 2;

    audio::PluginSpec gain;
    gain.id = "PG000001";
    gain.identifier = "godot:test-gain";
    gain.name = "Test gain";
    spec.plugins.push_back (gain);

    REQUIRE (rig.host.buildEdit (spec));

    auto* lane = rig.host.proxyLane (0, 0);
    REQUIRE (lane != nullptr);

    Engine engine;
    audio::HostPlayer player { rig.host, engine };

    const auto tone = writeSteadyTone (rig.storage.folder, 2, rate);

    cue::ArmRequest arm;
    arm.runId = "RN000001";
    arm.track = 0;
    arm.mediaFile = tone.getFullPathName().toStdString();

    cue::FxSetting insert;
    insert.slot = 0;
    insert.fxId = "FX000001";
    insert.enabled = true;
    insert.statePath = "older.state";
    arm.fx.push_back (insert);

    SUBCASE ("an arm, then a newer state: the state")
    {
        player.requestArm (arm);
        player.requestFxState (0, 0, "newer.state");
        player.serviceArms();

        CHECK (lane->wantedState() == "newer.state");
    }

    SUBCASE ("a state, then an arm: the arm's")
    {
        player.requestFxState (0, 0, "newer.state");
        player.requestArm (arm);
        player.serviceArms();

        CHECK (lane->wantedState() == "older.state");
    }
}

//==============================================================================
TEST_CASE ("host player: a take's presses reach the recorder at their sample, its closes come back in seconds, and a stop holds it")
{
    /*  Phase 9c, stage 9c.3: the Player's take doors on the real host - a
        sampling channel with no plugin, so no child. A steady input is
        recorded from the sample the press was placed at to the sample the
        close was; the recorder's report comes back in seconds for
        `take.closed`; the points go in as seconds and land in samples; and a
        stop of the rack track holds the take, as a stop's tail must not be
        the loop playing on. */
    constexpr int rate = 48000;
    HostRig rig;

    audio::HostSettings settings;
    settings.sampleRate = rate;
    settings.blockSize = 128;
    settings.outputChannels = 2;
    settings.inputChannels = 2;
    REQUIRE (rig.host.start (settings));

    audio::EditSpec spec;
    spec.tracks = 1;
    spec.channelsPerTrack = 2;

    audio::RackChannelSpec channel;
    channel.id = "TK000011";
    channel.name = "Looper";
    channel.takeSeconds = 1.0;
    channel.layers = 2;
    spec.rack.push_back (channel);
    REQUIRE (rig.host.buildEdit (spec));

    Engine engine;
    audio::HostPlayer player { rig.host, engine };

    const auto track = rig.host.rackTrackOf ("TK000011");
    REQUIRE (track == 1);
    rig.host.setRackSource (track, 0, 1);
    rig.host.openRackGate (track, -1);

    const std::vector<float> steady (128, 0.5f), silent (128, 0.0f);
    const float* inputs[] { steady.data(), silent.data() };
    const auto run = [&rig, &inputs] (int blocks)
    {
        for (int i = 0; i < blocks; ++i)
            rig.host.processBlock (inputs, 2);
    };

    run (10);

    const auto recordAt = rig.host.clock().samplesElapsed() + 256;
    REQUIRE (player.postTake ("TK000011", cue::TakeVerb::record, recordAt, 0.0, 0.0));
    run (40);

    const auto closeAt = rig.host.clock().samplesElapsed() + 64;
    REQUIRE (player.postTake ("TK000011", cue::TakeVerb::record, closeAt, 0.0, 0.0));
    run (4);

    const auto reports = player.takeReports ({ "TK000011" });
    REQUIRE (reports.size() == 1u);
    CHECK (reports[0].channel == "TK000011");
    CHECK (reports[0].how == "pressed");
    CHECK (reports[0].seconds == doctest::Approx (static_cast<double> (closeAt - recordAt) / rate));
    CHECK (player.takeReports ({ "TK000011" }).empty());

    const auto take = rig.host.takeOf ("TK000011");
    REQUIRE (take != nullptr);
    CHECK (take->state() == audio::TakeState::looping);
    CHECK (take->length() == closeAt - recordAt);

    //  The points in seconds, landing in samples at the rate.
    REQUIRE (player.postTake ("TK000011", cue::TakeVerb::points, -1, 0.01, 0.05));
    run (1);
    CHECK (take->loopIn() == 480);
    CHECK (take->loopOut() == 2400);
    CHECK (player.takePlayhead ("TK000011") >= 0.01);
    CHECK (player.takePlayhead ("TK000011") < 0.05);

    player.setTakeThrough ("TK000011", true);
    CHECK (take->isThrough());

    //  A STOP OF THE RACK TRACK HOLDS THE TAKE: the loop fades out and stays.
    REQUIRE (player.stop (track));
    run (10);
    CHECK (take->state() == audio::TakeState::held);
    CHECK (player.takeReports ({ "TK000011" }).empty());

    /*  AND A STOP THAT LANDS WHILE A TAKE RECORDS CLOSES IT HELD, which the
        report says - `take.closed ... held`, so the account holds it too. */
    REQUIRE (player.postTake ("TK000011", cue::TakeVerb::clear, -1, 0.0, 0.0));
    run (1);
    REQUIRE (take->state() == audio::TakeState::empty);

    REQUIRE (player.postTake ("TK000011", cue::TakeVerb::record, -1, 0.0, 0.0));
    run (20);
    REQUIRE (take->state() == audio::TakeState::recording);

    REQUIRE (player.stop (track));
    run (1);

    const auto held = player.takeReports ({ "TK000011" });
    REQUIRE (held.size() == 1u);
    CHECK (held[0].how == "held");
    CHECK (held[0].seconds == doctest::Approx (20.0 * 128.0 / rate));
    CHECK (take->state() == audio::TakeState::held);

    //  Nothing for a channel with no recorder.
    CHECK_FALSE (player.postTake ("NQNQNQNQ", cue::TakeVerb::record, -1, 0.0, 0.0));
    CHECK (player.takeReports ({ "NQNQNQNQ" }).empty());
}

//==============================================================================
TEST_CASE ("M44: a take starts on the sample Rec was placed at, found by a click in the input")
{
    /*  Phase 9c, stage 9c.7 (namespace draft 19.9): through the real host - the
        input stage, the tap and the recorder - a press placed at a sample, and
        a click in the input some way after it. The take's sample k is the
        input's sample `placed + k` when nothing is late, so the click's place
        in the take says where the take began. Read back with Keep's own copy.
        With no plugin before the recorder there is no child to be late. */
    constexpr int rate = 48000;
    constexpr int block = 128;
    HostRig rig;

    audio::HostSettings settings;
    settings.sampleRate = rate;
    settings.blockSize = block;
    settings.outputChannels = 2;
    settings.inputChannels = 2;
    REQUIRE (rig.host.start (settings));

    audio::EditSpec spec;
    spec.tracks = 1;
    spec.channelsPerTrack = 2;

    audio::RackChannelSpec channel;
    channel.id = "TK000011";
    channel.name = "Looper";
    channel.takeSeconds = 1.0;
    channel.layers = 1;
    spec.rack.push_back (channel);
    REQUIRE (rig.host.buildEdit (spec));

    Engine engine;
    audio::HostPlayer player { rig.host, engine };

    const auto track = rig.host.rackTrackOf ("TK000011");
    REQUIRE (track >= 0);
    rig.host.setRackSource (track, 0, 1);
    rig.host.openRackGate (track, -1);

    //  A click: one sample at full scale, at a sample that is no block boundary.
    const std::int64_t click = 20 * block + 77;
    std::vector<float> first (block), second (block, 0.0f);
    const float* inputs[] { first.data(), second.data() };

    const auto run = [&] (int blocks)
    {
        for (int n = 0; n < blocks; ++n)
        {
            const auto at = rig.host.clock().samplesElapsed();

            for (int k = 0; k < block; ++k)
                first[static_cast<std::size_t> (k)] = at + k == click ? 1.0f : 0.0f;

            rig.host.processBlock (inputs, 2);
        }
    };

    //  The gate's own ramp done, then Rec placed a little ahead, inside a block.
    run (4);
    const auto placed = rig.host.clock().samplesElapsed() + 5 * block + 13;
    REQUIRE (placed < click);
    REQUIRE (player.postTake ("TK000011", cue::TakeVerb::record, placed, 0.0, 0.0));
    run (30);
    REQUIRE (player.postTake ("TK000011", cue::TakeVerb::record, -1, 0.0, 0.0));
    run (20);

    const auto take = rig.host.takeOf ("TK000011");
    REQUIRE (take != nullptr);
    REQUIRE (take->isSettled());

    const auto length = take->length();
    REQUIRE (length > click - placed);

    std::vector<float> left (static_cast<std::size_t> (length)), right (static_cast<std::size_t> (length));
    take->copyTake (0, 0, static_cast<int> (length), left.data(), right.data());

    const auto loudest = std::max_element (left.begin(), left.end(), [] (float a, float b) { return std::abs (a) < std::abs (b); });
    const auto found = static_cast<std::int64_t> (std::distance (left.begin(), loudest));
    const auto out = found - (click - placed);

    MESSAGE ("M44: Rec placed at sample " << placed << ", the click at " << click << ": found " << found
             << " samples into the take, out by " << out << " samples");

    CHECK (std::abs (*loudest) == doctest::Approx (1.0f));
    CHECK (out == 0);
}

//==============================================================================
TEST_CASE ("M38: the live rack's cost in the audio callback, with 0, 8 and 32 channels open")
{
    /*  Phase 9b (namespace draft 18.10). Each rack channel is a track of its
        own - the input stage, the EQ and the output stage - and open, it copies
        its input from the tap every block and sends it through the matrix.
        Measured with no plugin on any channel, since the proxy's own cost is
        M31's, beside eight idle voices, at 48 kHz and 128 samples into eight
        outputs. In alternation, twice over, for M11's reason: the cost of a
        block drifts with how many engines this process has built before it. */
    constexpr int rate = 48000;
    constexpr int blockSize = 128;
    constexpr int blocks = 1500;

    const auto measure = [] (int channels)
    {
        HostRig rig;

        audio::HostSettings settings;
        settings.sampleRate = rate;
        settings.blockSize = blockSize;
        settings.outputChannels = 8;
        settings.inputChannels = 2;
        REQUIRE (rig.host.start (settings));

        audio::EditSpec spec;
        spec.tracks = 8;
        spec.channelsPerTrack = 2;

        for (int n = 0; n < channels; ++n)
        {
            audio::RackChannelSpec channel;
            channel.id = "CH" + juce::String (n).paddedLeft ('0', 6).toStdString();
            channel.name = "Mic " + std::to_string (n + 1);
            spec.rack.push_back (channel);
        }

        REQUIRE (rig.host.buildEdit (spec));

        for (int n = 0; n < channels; ++n)
        {
            const auto track = rig.host.rackTrackOf ("CH" + juce::String (n).paddedLeft ('0', 6).toStdString());
            REQUIRE (track >= 0);

            auto* matrix = rig.host.trackMatrix (track);
            REQUIRE (matrix != nullptr);
            matrix->setLevelDb (0.0f);
            matrix->setGain (0, n % 8, 1.0f);
            matrix->snapToTargets();

            rig.host.setRackSource (track, n % 2, 1);
            rig.host.openRackGate (track, -1);
        }

        const std::vector<float> one (blockSize, 0.25f), two (blockSize, -0.25f);
        const float* inputs[] { one.data(), two.data() };

        for (int i = 0; i < 200; ++i)
            rig.host.processBlock (inputs, 2);

        const auto from = std::chrono::steady_clock::now();

        for (int i = 0; i < blocks; ++i)
            rig.host.processBlock (inputs, 2);

        const auto took = std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - from).count();

        rig.host.stop();
        return took / blocks;
    };

    const auto budget = 1.0e6 * blockSize / rate;

    for (int pass = 0; pass < 2; ++pass)
        for (const auto channels : { 0, 8, 32 })
        {
            const auto cost = measure (channels);

            MESSAGE ("M38 pass " << pass + 1 << ": " << channels << " rack channels open, 8 voices idle: "
                     << cost << " us/block of " << budget << " us (" << 100.0 * cost / budget
                     << "% of real time)");

            CHECK (cost > 0.0);
        }
}

//==============================================================================
TEST_CASE ("audio host: a file at another rate plays at its own pitch")
{
    /*  PRD §6.2's own test of following a moved clock, in the author's words
        (2026-09-21): "up or down sample, no Alvin and the Chipmunks". The show
        follows an interface onto a new rate by building its graph again on it,
        and nothing else - no file is converted on disk - so a file written at
        one rate has to come out at its own pitch on a graph running at another.
        A thousand hertz written at the file's rate is counted back out of the
        render by its rising zero crossings: the cheap reinterpretation, the
        same samples played at the graph's rate, would count two thousand or
        five hundred. */
    struct Pair { int file, graph; };

    for (const auto pair : { Pair { 48000, 96000 }, Pair { 96000, 48000 }, Pair { 44100, 48000 } })
    {
        INFO ("a " << pair.file << " Hz file on a " << pair.graph << " Hz graph");

        HostRig rig;

        audio::HostSettings settings;
        settings.sampleRate = pair.graph;
        settings.blockSize = 256;
        settings.outputChannels = 2;
        REQUIRE (rig.host.start (settings));

        audio::EditSpec spec;
        spec.tracks = 1;
        spec.channelsPerTrack = 1;
        REQUIRE (rig.host.buildEdit (spec));

        const auto tone = writeSineTone (rig.storage.folder, pair.file, 1000.0, 0.25f, 3);
        REQUIRE (tone.existsAsFile());
        REQUIRE (rig.host.setTrackSource (0, 0, tone.getFullPathName().toStdString()));
        REQUIRE (rig.host.waitForTrackSourceReady (0, 10000));

        auto* matrix = rig.host.trackMatrix (0);
        REQUIRE (matrix != nullptr);
        matrix->setLevelDb (0.0f);
        matrix->setGain (0, 0, 1.0f);
        matrix->snapToTargets();

        for (int i = 0; i < 8; ++i)
            rig.host.processBlock();

        REQUIRE (rig.host.launchTrackAt (0, 0, rig.host.beatsAtSample (rig.host.clock().samplesElapsed() + 1024)));

        //  Half a second in, well clear of the launch and the click suppressor.
        for (int i = 0; i < pair.graph / 2 / settings.blockSize; ++i)
            rig.host.processBlock();

        REQUIRE (rig.host.trackPlayState (0).playing);

        //  One second of it, at the graph's rate.
        RecordingSink sink;
        sink.prepare (2, pair.graph);
        rig.host.setBlockSink (&sink);

        while (sink.written < pair.graph)
            rig.host.processBlock();

        rig.host.setBlockSink (nullptr);

        const auto* samples = sink.buffer.getReadPointer (0);
        auto rising = 0;
        auto peak = 0.0f;

        for (int n = 1; n < sink.written; ++n)
        {
            peak = std::max (peak, std::abs (samples[n]));

            if (samples[n - 1] < 0.0f && samples[n] >= 0.0f)
                ++rising;
        }

        INFO ("peak " << peak << ", " << rising << " rising crossings in one second");
        REQUIRE (peak > 0.1f);
        CHECK (rising >= 995);
        CHECK (rising <= 1005);
    }
}

TEST_CASE ("audio host: a cue in varispeed is resampled, though a stretcher is compiled in")
{
    /*  Namespace draft §22.3. Signalsmith is compiled in, which makes it
        Tracktion's default stretcher, and Tracktion hands the default to every
        auto-tempo clip whose mode is `disabled` - every slot clip Go.dot has.
        Patch 0001 and the engine behaviour are what keep a cue at its own
        speed, with its own samples, on the resampler. Asked of a real arm,
        with the predicate the graph is built with: if either went missing,
        every cue would be stretched and nothing else would say so, since a
        stretcher at one plays at the right pitch - just not the same samples. */
    HostRig rig;

    audio::HostSettings settings;
    settings.sampleRate = 48000;
    settings.blockSize = 256;
    settings.outputChannels = 2;
    REQUIRE (rig.host.start (settings));

    audio::EditSpec spec;
    spec.tracks = 1;
    spec.channelsPerTrack = 1;
    REQUIRE (rig.host.buildEdit (spec));

    //  The resident clip, before anything is armed on it.
    CHECK_FALSE (rig.host.isTrackStretched (0, 0));

    const auto tone = writeSineTone (rig.storage.folder, 48000, 1000.0, 0.25f, 2);
    REQUIRE (tone.existsAsFile());
    REQUIRE (rig.host.setTrackSource (0, 0, tone.getFullPathName().toStdString()));

    CHECK_FALSE (rig.host.isTrackStretched (0, 0));

    //  And no slot, no clip: a question about nothing is a no.
    CHECK_FALSE (rig.host.isTrackStretched (0, 5));
    CHECK_FALSE (rig.host.isTrackStretched (3, 0));
}

//==============================================================================
/*  A CUE'S SPEED, HEARD (namespace draft §22, patch 0002): a voice's speed
    placed ahead as breakpoints, read by the slot through Tracktion's wave node,
    counted out of the render. Every case launches one file on one sample and
    places its speed from there, the way the Runner will; what they count is
    what an ear would hear - a pitch, an end, a click, a silence, a held sound. */
namespace
{
    struct SpeedRig
    {
        HostRig rig;
        int rate = 48000;
        int block = 256;
        std::int64_t launch = 0;

        /*  One voice with `file` on it, in varispeed or timestretch, its ranges
            if it has any; the voice routed to the first output at unity. */
        bool open (const juce::File& file, bool stretch,
                   const std::vector<audio::AudioHost::RangeSpec>& ranges = {}, int tracks = 1)
        {
            audio::HostSettings settings;
            settings.sampleRate = rate;
            settings.blockSize = block;
            settings.outputChannels = 2;

            if (! rig.host.start (settings))
                return false;

            audio::EditSpec spec;
            spec.tracks = tracks;
            spec.channelsPerTrack = 1;
            spec.slots = std::max (1, static_cast<int> (ranges.size()));

            if (! rig.host.buildEdit (spec)
                 || ! rig.host.setTrackRanges (0, file.getFullPathName().toStdString(), ranges, 0.0, stretch)
                 || ! rig.host.waitForTrackSourceReady (0, 10000))
                return false;

            auto* matrix = rig.host.trackMatrix (0);

            if (matrix == nullptr)
                return false;

            matrix->setLevelDb (0.0f);
            matrix->setGain (0, 0, 1.0f);
            matrix->snapToTargets();

            for (int i = 0; i < 8; ++i)
                rig.host.processBlock();

            return true;
        }

        std::int64_t at (double secondsAfterLaunch) const
        {
            return launch + static_cast<std::int64_t> (std::llround (secondsAfterLaunch * rate));
        }

        /*  Launches slot 0 a few blocks from now at `speed`: the speed held at
            what the voice had until the launch, then a step to this one - the
            two breakpoints the Runner places for a GO. */
        bool go (double speed, int slot = 0)
        {
            launch = rig.host.clock().samplesElapsed() + 4 * block;

            return rig.host.placeTrackRate (0, launch, 1.0)
                && rig.host.placeTrackRate (0, launch, speed)
                && rig.host.launchTrackAt (0, slot, rig.host.beatsAtSample (launch));
        }

        /** From the last breakpoint, a straight line to `speed` at `seconds` after the launch. */
        bool speedAt (double seconds, double speed)
        {
            return rig.host.placeTrackRate (0, at (seconds), speed);
        }

        void runTo (std::int64_t sample)
        {
            while (rig.host.clock().samplesElapsed() + block <= sample)
                rig.host.processBlock();
        }

        /*  `seconds` of the first output, from the first block at or after
            `from` seconds after the launch. `startedAt` is that block's
            sample. */
        juce::AudioBuffer<float> record (double from, double seconds)
        {
            runTo (at (from));
            startedAt = rig.host.clock().samplesElapsed();

            RecordingSink sink;
            sink.prepare (1, static_cast<int> (std::llround (seconds * rate)));
            rig.host.setBlockSink (&sink);

            while (sink.written < sink.buffer.getNumSamples())
                rig.host.processBlock();

            rig.host.setBlockSink (nullptr);
            return sink.buffer;
        }

        std::int64_t startedAt = 0;
    };

    int risingCrossings (const juce::AudioBuffer<float>& buffer, int from = 0, int to = -1)
    {
        const auto* x = buffer.getReadPointer (0);
        const auto end = to < 0 ? buffer.getNumSamples() : to;
        auto count = 0;

        for (int n = std::max (1, from); n < end; ++n)
            if (x[n - 1] < 0.0f && x[n] >= 0.0f)
                ++count;

        return count;
    }

    /** The largest difference between neighbouring samples: a click is a step no tone makes. */
    float largestStep (const juce::AudioBuffer<float>& buffer, int from = 0, int to = -1)
    {
        const auto* x = buffer.getReadPointer (0);
        const auto end = to < 0 ? buffer.getNumSamples() : to;
        auto largest = 0.0f;

        for (int n = std::max (1, from); n < end; ++n)
            largest = std::max (largest, std::abs (x[n] - x[n - 1]));

        return largest;
    }

    float rmsOver (const juce::AudioBuffer<float>& buffer, int from, int to)
    {
        const auto* x = buffer.getReadPointer (0);
        auto sum = 0.0;

        for (int n = from; n < to; ++n)
            sum += static_cast<double> (x[n]) * x[n];

        return static_cast<float> (std::sqrt (sum / std::max (1, to - from)));
    }

    /** The last sample whose magnitude clears `floorLevel`, or -1. */
    int lastSoundAt (const juce::AudioBuffer<float>& buffer, float floorLevel = 0.001f)
    {
        const auto* x = buffer.getReadPointer (0);

        for (int n = buffer.getNumSamples(); --n >= 0;)
            if (std::abs (x[n]) > floorLevel)
                return n;

        return -1;
    }

    /*  A file of silence with one click at the start of every second: what a
        loop's wraps are counted by. */
    juce::File writeClicks (const juce::File& folder, int rate, int seconds)
    {
        const auto file = folder.getChildFile ("clicks.wav");
        folder.createDirectory();

        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream { file.createOutputStream() };

        if (stream == nullptr)
            return {};

        auto writer = format.createWriterFor (stream,
                                              juce::AudioFormatWriterOptions{}
                                                .withSampleRate (static_cast<double> (rate))
                                                .withNumChannels (1)
                                                .withBitsPerSample (24));

        if (writer == nullptr)
            return {};

        juce::AudioBuffer<float> buffer { 1, rate * seconds };
        buffer.clear();

        for (int second = 0; second < seconds; ++second)
            buffer.setSample (0, second * rate, 0.9f);

        writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
        return file;
    }
}

TEST_CASE ("audio host: at twice the speed, varispeed doubles the pitch and timestretch keeps it, and the file ends at half")
{
    for (const auto stretch : { false, true })
    {
        INFO ("mode " << std::string (stretch ? "timestretch" : "varispeed"));

        SpeedRig speed;
        const auto tone = writeSineTone (speed.rig.storage.folder, speed.rate, 1000.0, 0.25f, 4);
        REQUIRE (tone.existsAsFile());
        REQUIRE (speed.open (tone, stretch));
        CHECK (speed.rig.host.isTrackStretched (0, 0) == stretch);

        REQUIRE (speed.go (2.0));

        //  One second of it from half a second in, well clear of the launch.
        const auto heard = speed.record (0.5, 1.0);
        const auto rising = risingCrossings (heard);

        INFO (rising << " rising crossings in a second of a 1 kHz file played at twice its speed");

        if (stretch)
        {
            CHECK (rising >= 990);
            CHECK (rising <= 1010);
        }
        else
        {
            CHECK (rising >= 1995);
            CHECK (rising <= 2005);
        }

        //  A four-second file at twice its speed has ended two seconds after its launch.
        const auto around = speed.record (1.8, 0.5);
        const auto last = speed.startedAt + lastSoundAt (around);
        const auto ends = static_cast<double> (last - speed.launch) / speed.rate;

        /*  A stretched sound also rings the stretcher's own tail, and its place
            in the file is right to within a few of the stretcher's 256-sample
            chunks - one machine to the next (2026-09-29: up to three on the CI
            runners, none here). */
        INFO ("the last sound " << ends << " s after the launch");
        CHECK (ends > 1.99);
        CHECK (ends < (stretch ? 2.05 : 2.01));

        speed.runTo (speed.at (2.3));
        CHECK_FALSE (speed.rig.host.trackPlayState (0).playing);
        CHECK (speed.rig.host.trackRateLateCount (0) == 0);
    }
}

TEST_CASE ("audio host: a cue slowed and brought back to one keeps its place in the file")
{
    /*  A four-second file, at one for half a second, a half for a second, then
        one again: it has played one second of itself at 1.5 s, and ends three
        seconds later, 4.5 s after its launch. What this guards (found in S.3):
        a slot that read its speed as one "from the launch on" once the slow
        second had been forgotten, jumped its file forward by half a second -
        a click, and an end half a second early. */
    SpeedRig speed;
    const auto tone = writeSineTone (speed.rig.storage.folder, speed.rate, 500.0, 0.25f, 4);
    REQUIRE (speed.open (tone, false));

    REQUIRE (speed.go (1.0));
    REQUIRE (speed.speedAt (0.5, 1.0));
    REQUIRE (speed.speedAt (0.5, 0.5));
    REQUIRE (speed.speedAt (1.5, 0.5));
    REQUIRE (speed.speedAt (1.5, 1.0));

    const auto through = speed.record (1.0, 2.5);

    INFO ("largest step " << largestStep (through));
    CHECK (largestStep (through) < 0.02f);

    const auto around = speed.record (4.3, 0.5);
    const auto ends = static_cast<double> (speed.startedAt + lastSoundAt (around) - speed.launch) / speed.rate;

    INFO ("the last sound " << ends << " s after the launch");
    CHECK (ends > 4.49);
    CHECK (ends < 4.51);
}

namespace
{
    /*  A SHOW OF ONE MEDIA CUE ON ONE VOICE, fired through the Runner, the
        HostPlayer and a real graph: what the cases on a cue's speed from the
        document to the speaker share (namespace draft §22). One output, a
        tick of blocks a tick, and the tick hook before each. */
    struct SpeedShow
    {
        static constexpr int rate = 48000;
        static constexpr int blockSize = 128;

        HostRig rig;
        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::Focus focus;
        doc::IdRegistry runIds = doc::IdRegistry::withSeed (3);
        cue::Runner runner { document, runs, runIds, focus };
        std::unique_ptr<audio::HostPlayer> player;

        std::string listId, cueId;
        std::int64_t tick = 0;

        /*  A tone of `hertz` for `seconds` as the list's one media cue, at
            `speed` in `mode`, on standby - the host started, four ticks run,
            nothing fired. */
        bool open (double hertz, int seconds, const char* speed, const char* mode)
        {
            audio::HostSettings settings;
            settings.sampleRate = rate;
            settings.blockSize = blockSize;
            settings.outputChannels = 2;

            audio::EditSpec spec;
            spec.tracks = 1;
            spec.channelsPerTrack = 1;

            if (! rig.host.start (settings) || ! rig.host.buildEdit (spec))
                return false;

            const auto tone = writeSineTone (rig.storage.folder, rate, hertz, 0.25f, seconds);

            if (! tone.existsAsFile())
                return false;

            engine.log().openInMemory ({});
            doc::registerDocumentCommands (engine.commands(), document);
            cue::registerCueCommands (engine.commands(), document, focus);
            cue::registerRunCommands (engine.commands(), runs);
            cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);

            listId = document.createList ("Sound").id;
            cueId = document.createCue (listId, 0, "media", "Tone").id;

            if (! document.setAttribute ("/godot/cue/" + cueId + "/file", tone.getFileName().toStdString()).ok
                  || ! document.setAttribute ("/godot/cue/" + cueId + "/rate", speed).ok
                  || ! document.setAttribute ("/godot/cue/" + cueId + "/rateMode", mode).ok)
                return false;

            auto audioNode = document.root().getChildWithName ("Audio");
            audioNode.setProperty (juce::Identifier ("tracks"), 1, nullptr);

            juce::ValueTree bus { "Bus" };
            bus.setProperty (juce::Identifier ("id"), "J3MT5XYA", nullptr);
            bus.setProperty (juce::Identifier ("name"), "Main", nullptr);
            bus.setProperty (juce::Identifier ("firstChannel"), 0, nullptr);
            bus.setProperty (juce::Identifier ("width"), 1, nullptr);
            audioNode.appendChild (bus, nullptr);

            juce::ValueTree route { "Route" };
            route.setProperty (juce::Identifier ("id"), "Z04EH7PH", nullptr);
            route.setProperty (juce::Identifier ("bus"), "J3MT5XYA", nullptr);
            route.setProperty (juce::Identifier ("gains"), "1", nullptr);
            document.findById (cueId).appendChild (route, nullptr);

            document.setAttribute (cue::standbyAddressOf (listId), cueId);

            player = std::make_unique<audio::HostPlayer> (rig.host, engine);
            runner.setPlayer (player.get());
            runner.setSamplesPerTick (rate / 50);
            runner.setMediaFolder (rig.storage.folder.getFullPathName().toStdString());

            ticks (4);
            return true;
        }

        void oneTick()
        {
            runner.beforeTick (engine, tick);
            engine.processTick (tick++);
            player->serviceArms();

            for (int i = 0; i < (rate / 50) / blockSize; ++i)
                rig.host.processBlock();
        }

        void ticks (int count)
        {
            for (int i = 0; i < count; ++i)
                oneTick();
        }

        /*  GO, and the ticks until the voice sounds - each tick that finds it
            silent waiting a real tick as well. Between the GO and the first
            sample are an arm, a graph rebuild and a disk read on Tracktion's
            own thread, and those are wall clock: four hundred bare ticks ran
            in a fraction of a second, and a loaded macOS runner was still
            reading when they ran out (CI run 36494964799) - the lesson this
            file's older cases already carry. A fast machine never waits. */
        bool go()
        {
            if (! engine.submit ("cli", "go", {}))
                return false;

            for (int i = 0; i < 600 && ! rig.host.trackPlayState (0).playing; ++i)
            {
                oneTick();

                if (! rig.host.trackPlayState (0).playing)
                    std::this_thread::sleep_for (std::chrono::milliseconds (20));
            }

            return rig.host.trackPlayState (0).playing;
        }

        /** `count` ticks of the first output, as they are played. */
        juce::AudioBuffer<float> record (int count)
        {
            RecordingSink sink;
            sink.prepare (2, count * (rate / 50));
            rig.host.setBlockSink (&sink);

            while (sink.written < sink.buffer.getNumSamples())
                oneTick();

            rig.host.setBlockSink (nullptr);
            return sink.buffer;
        }

        /*  A fade cue after the tone and aimed at it that moves its speed
            alone: to `speed`, over `seconds`. */
        std::string speedFade (int at, const char* speed, const char* seconds)
        {
            const auto id = document.createCue (listId, at, "fade", "Speed").id;

            document.setAttribute ("/godot/cue/" + id + "/target", cueId);
            document.setAttribute ("/godot/cue/" + id + "/levelOn", "false");
            document.setAttribute ("/godot/cue/" + id + "/rateOn", "true");
            document.setAttribute ("/godot/cue/" + id + "/rate", speed);
            document.setAttribute ("/godot/cue/" + id + "/duration", seconds);
            return id;
        }

        /** A cue fired by name, standby left where it is. */
        bool fire (const std::string& id)
        {
            return engine.submit ("cli", "cue.fire", { osc::Value::string (id) });
        }

        /** The tone's own run: the first the show made. */
        const cue::Run& tone() const
        {
            return runs.all().front();
        }
    };
}

TEST_CASE ("audio host: a cue's speed from the show to the speaker - the document's rate, the Runner's clock, the voice's")
{
    /*  S.3, end to end (namespace draft §22): a media cue whose `rate` says
        two, fired by a GO through the Runner, the HostPlayer and a real graph.
        Its kilohertz is heard at two in varispeed and at one in timestretch;
        the playhead reads the file's seconds, twice the clock's; and an edit of
        the rate to one while it sounds is heard a moment later. */
    for (const auto stretch : { false, true })
    {
        INFO ("mode " << std::string (stretch ? "timestretch" : "varispeed"));

        SpeedShow show;
        REQUIRE (show.open (1000.0, 8, "2", stretch ? "timestretch" : "varispeed"));
        REQUIRE (show.go());
        CHECK (show.rig.host.isTrackStretched (0, 0) == stretch);

        //  Half a second in, then a second of it.
        show.ticks (25);

        const auto atTwo = risingCrossings (show.record (50));
        INFO (atTwo << " rising crossings in a second at the document's two");

        if (stretch)
            CHECK (std::abs (atTwo - 1000) <= 12);
        else
            CHECK (std::abs (atTwo - 2000) <= 12);

        REQUIRE (show.runs.all().size() == 1u);
        const auto runId = show.tone().id;
        const auto* run = show.runs.find (runId);

        //  The playhead reads the file: twice the clock since the launch.
        const auto clockSeconds = static_cast<double> (show.rig.host.clock().samplesElapsed() - run->launchedAtSample)
                                    / SpeedShow::rate;
        INFO ("the clock " << clockSeconds << " s after the launch, the playhead at " << run->position);
        CHECK (run->position == doctest::Approx (2.0 * clockSeconds).epsilon (0.02));
        CHECK (run->rateNow == doctest::Approx (2.0));

        //  An edit to one while it sounds, heard once the horizon and the ramp have passed.
        REQUIRE (show.engine.submit ("cli", "node.set", { osc::Value::string ("/godot/cue/" + show.cueId + "/rate"),
                                                          osc::Value::float64 (1.0) }));
        show.ticks (10);

        const auto atOne = risingCrossings (show.record (50));
        INFO (atOne << " rising crossings in a second after the edit to one");
        CHECK (std::abs (atOne - 1000) <= 12);
        CHECK (show.runs.find (runId)->rateNow == doctest::Approx (1.0));
    }
}

TEST_CASE ("audio host: a fade cue takes a cue's speed to nought and back - a tape stop in varispeed, a held sound in timestretch")
{
    /*  S.4, end to end (namespace draft §22.6): a fade cue with only its speed
        switch on, fired while a half-kilohertz tone sounds, takes it to nought
        over a second; a second fade brings it back to one over half a second.
        Varispeed slides down into silence - exact zeros, and no step on the
        way larger than the tone makes at one - with the voice still playing,
        stopped only in time. Timestretch holds the instant at its own pitch.
        Both come back to the tone they were, and the level is never touched. */
    for (const auto stretch : { false, true })
    {
        INFO ("mode " << std::string (stretch ? "timestretch" : "varispeed"));

        SpeedShow show;
        REQUIRE (show.open (500.0, 8, "1", stretch ? "timestretch" : "varispeed"));
        REQUIRE (show.go());
        show.ticks (25);

        const auto down = show.speedFade (1, "0", "1");
        const auto up = show.speedFade (2, "1", "0.5");

        REQUIRE (show.fire (down));

        //  The fade's second, the horizon, the last ramp, and a little over.
        const auto gliding = show.record (60);

        /*  No step larger than the tone's own at one, 2 pi x 500 / 48000 x 0.25,
            in varispeed. A stretched sound is resynthesised, with phases
            Signalsmith seeds from the machine's random device, so its steps run
            a little above a pure tone's and vary from run to run (0.0207 was
            seen); a click is ten times that - a stretched loop's wrap is 0.22. */
        INFO ("largest step on the way down " << largestStep (gliding));
        CHECK (largestStep (gliding) < (stretch ? 0.04f : 0.02f));
        CHECK (show.tone().ownRate == doctest::Approx (0.0));

        const auto held = show.record (25);
        const auto level = rmsOver (held, 0, held.getNumSamples());
        const auto rising = risingCrossings (held);
        INFO ("held at nought: RMS " << level << ", " << rising << " rising crossings in half a second");

        if (stretch)
        {
            CHECK (level > 0.05f);
            CHECK (rising >= 240);
            CHECK (rising <= 260);
        }
        else
        {
            CHECK (held.getMagnitude (0, 0, held.getNumSamples()) <= 0.0f);
        }

        //  Stopped in time and not ended: the voice plays, the playhead stands.
        CHECK (show.rig.host.trackPlayState (0).playing);

        const auto standing = show.tone().position;
        show.ticks (10);
        CHECK (show.tone().position == doctest::Approx (standing));

        //  Back up: half a second of fade, the horizon, then a second at one.
        REQUIRE (show.fire (up));
        show.ticks (40);

        const auto again = risingCrossings (show.record (50));
        INFO (again << " rising crossings in a second, back at one");
        CHECK (std::abs (again - 500) <= 8);
        CHECK (show.tone().rateNow == doctest::Approx (1.0));
        CHECK (show.tone().ownLevel == doctest::Approx (0.0));
    }
}

TEST_CASE ("audio host: a speed ramped from one to two has no step in it")
{
    /*  Half a kilohertz, ramped to twice its speed over a second: the pitch
        glides to a kilohertz. A click is a step between two samples no tone of
        that pitch could make; the largest step a 1 kHz sine at a quarter of
        full scale makes is 2 pi x 1000 / 48000 x 0.25, about 0.033.

        What this case found, in Tracktion's Lagrange reader, before patch 0002
        mended it: fed a whole number of frames a block and moved to the rounded
        request every block, it clicked faintly at the first sample of a block
        whenever the ratio was not a whole number of frames a block - 0.045 here,
        against the tone's 0.033. */
    SpeedRig speed;
    const auto tone = writeSineTone (speed.rig.storage.folder, speed.rate, 500.0, 0.25f, 5);
    REQUIRE (speed.open (tone, false));

    rt::resetCounts();

    REQUIRE (speed.go (1.0));
    REQUIRE (speed.speedAt (0.5, 1.0));
    REQUIRE (speed.speedAt (1.5, 2.0));

    const auto heard = speed.record (0.25, 1.5);

    INFO ("largest step " << largestStep (heard));
    CHECK (largestStep (heard) < 0.035f);

    //  Where it lands: a kilohertz over the last quarter of a second.
    const auto tail = risingCrossings (heard, static_cast<int> (1.25 * speed.rate));
    INFO (tail << " rising crossings in the last quarter second");
    CHECK (tail >= 245);
    CHECK (tail <= 255);

    INFO ("Go.dot's own code allocated " << rt::violations() << " times while the speed moved");
    CHECK (rt::violations() == 0);
    CHECK (speed.rig.host.trackRateLateCount (0) == 0);
}

TEST_CASE ("audio host: varispeed down to nought slides into silence with no click, and the cue still plays")
{
    SpeedRig speed;
    const auto tone = writeSineTone (speed.rig.storage.folder, speed.rate, 500.0, 0.25f, 5);
    REQUIRE (speed.open (tone, false));

    REQUIRE (speed.go (1.0));
    REQUIRE (speed.speedAt (0.5, 1.0));
    REQUIRE (speed.speedAt (1.5, 0.0));

    const auto heard = speed.record (0.25, 2.0);

    //  No step larger than the tone's own at its full speed: 2 pi x 500 / 48000 x 0.25.
    INFO ("largest step " << largestStep (heard));
    CHECK (largestStep (heard) < 0.02f);

    //  Silent - exactly - once the speed is below a hundredth, which a ramp
    //  from one to nought over a second is for its last ten milliseconds.
    const auto* x = heard.getReadPointer (0);
    auto nonZero = 0;

    for (int n = static_cast<int> (1.3 * speed.rate); n < heard.getNumSamples(); ++n)
        if (std::abs (x[n]) > 0.0f)     // not `!=`: -Wfloat-equal
            ++nonZero;

    CHECK (nonZero == 0);

    //  A stopped tape is still a loaded one: the file has not ended.
    CHECK (speed.rig.host.trackPlayState (0).playing);
}

TEST_CASE ("audio host: timestretch at nought holds the sound, and goes on when the speed does")
{
    /*  Decision DR: timestretch's nought is the instant held, pitch and all.
        Down to nought over half a second, held for a second, back to one. */
    SpeedRig speed;
    const auto tone = writeSineTone (speed.rig.storage.folder, speed.rate, 500.0, 0.25f, 6);
    REQUIRE (speed.open (tone, true));

    REQUIRE (speed.go (1.0));
    REQUIRE (speed.speedAt (0.5, 1.0));
    REQUIRE (speed.speedAt (1.0, 0.0));
    REQUIRE (speed.speedAt (2.0, 0.0));
    REQUIRE (speed.speedAt (2.5, 1.0));

    const auto held = speed.record (1.25, 0.5);
    const auto level = rmsOver (held, 0, held.getNumSamples());
    const auto rising = risingCrossings (held);

    INFO ("held at nought: RMS " << level << ", " << rising << " rising crossings in half a second");
    CHECK (level > 0.05f);
    CHECK (rising >= 240);
    CHECK (rising <= 260);

    const auto after = speed.record (2.75, 0.5);
    CHECK (rmsOver (after, 0, after.getNumSamples()) > 0.05f);
    CHECK (speed.rig.host.trackPlayState (0).playing);
}

TEST_CASE ("audio host: a stretched cue starts at its first sample and keeps its place in its file, at any speed")
{
    /*  What M14 found in Tracktion's stretch reader (2026-09-29), both mended in
        patch 0002. It was primed when it was built - before its file had been
        read, with the graph, while the cache was still loading - so a stretched
        cue opened on 150 ms of silence with its file's first moments lost; and
        primed at one, so played at another speed it was (1 - speed) x its
        output latency out of place in its source, 95 ms early at half the speed
        and 47 ms late at twice. A tone says where the sound starts and ends;
        a click at each of the file's seconds says where in the file it is - at
        the speeds where a stretcher still passes a lone click. */
    for (const auto speed : { 0.5, 1.0, 2.0, 4.0, 20.0 })
    {
        INFO ("x" << speed);

        SpeedRig voice;
        const auto tone = writeSineTone (voice.rig.storage.folder, voice.rate, 750.0, 0.25f, 2);
        REQUIRE (voice.open (tone, true));
        REQUIRE (voice.go (speed));

        const auto heard = voice.record (0.0, 2.0 / speed + 0.3);
        const auto base = voice.startedAt - voice.launch;
        const auto* x = heard.getReadPointer (0);
        auto first = -1, last = -1;

        for (int n = 0; n < heard.getNumSamples(); ++n)
            if (std::abs (x[n]) > 0.005f)
            {
                if (first < 0)
                    first = n;

                last = n;
            }

        REQUIRE (first >= 0);
        const auto end = static_cast<std::int64_t> (2.0 / speed * voice.rate);

        CHECK (first + base <= 2);

        //  The stretcher's own tail, stopped with the clip - and its place is
        //  right to within a few 256-sample chunks, one run and one machine to
        //  the next (five seen, 2026-09-29), early as well as late: a macOS
        //  runner ended the tone at half the speed 247 samples early (CI run
        //  36558400206). So the end is judged as the clicks are, to 1024
        //  either side of it, and the tail to fifty milliseconds. The author,
        //  the same day - "The timestretch doesn't have to be so accurate.
        //  This is mostly for fine tuning up or down or for sound design."
        //  The faults this case is for were at the start.
        CHECK (last + base >= end - 1024);
        CHECK (last + base <= end + 2400);
    }

    for (const auto speed : { 0.5, 1.0, 2.0 })
    {
        INFO ("x" << speed);

        SpeedRig voice;
        const auto clicks = writeClicks (voice.rig.storage.folder, voice.rate, 3);
        REQUIRE (voice.open (clicks, true));
        REQUIRE (voice.go (speed));

        const auto heard = voice.record (0.0, 3.0 / speed + 0.3);
        const auto base = voice.startedAt - voice.launch;
        const auto* x = heard.getReadPointer (0);

        for (int second = 0; second < 3; ++second)
        {
            const auto expected = static_cast<int> (second / speed * voice.rate) - static_cast<int> (base);
            auto peakAt = -1;
            auto peak = 0.0f;

            for (int n = std::max (0, expected - 12000); n < std::min (heard.getNumSamples(), expected + 12000); ++n)
                if (std::abs (x[n]) > peak)
                {
                    peak = std::abs (x[n]);
                    peakAt = n;
                }

            INFO ("the file's click at " << second << " s peaks at " << peakAt << ", expected " << expected);
            CHECK (peak > 0.3f);
            CHECK (std::abs (peakAt - expected) <= 1024);    // within four of the stretcher's chunks
        }
    }
}

TEST_CASE ("audio host: a looping range at one and a half wraps where the source says")
{
    /*  A range of the first second of a file with a click at its start, looped,
        at one and a half: the click comes back every two thirds of a second -
        32 000 samples at 48 kHz - however many passes go by. */
    SpeedRig speed;
    const auto clicks = writeClicks (speed.rig.storage.folder, speed.rate, 2);
    REQUIRE (clicks.existsAsFile());
    REQUIRE (speed.open (clicks, false, { { 0.0, 1.0, 0 } }));

    REQUIRE (speed.go (1.5));

    const auto heard = speed.record (0.1, 3.0);
    const auto* x = heard.getReadPointer (0);

    std::vector<std::int64_t> wraps;

    for (int n = 0; n < heard.getNumSamples(); ++n)
    {
        if (std::abs (x[n]) < 0.2f)
            continue;

        //  The loudest sample of this click, and past it.
        auto peak = n;

        while (n < heard.getNumSamples() && std::abs (x[n]) >= 0.05f)
        {
            if (std::abs (x[n]) > std::abs (x[peak]))
                peak = n;

            ++n;
        }

        wraps.push_back (speed.startedAt + peak);
    }

    REQUIRE (wraps.size() >= 4);

    for (std::size_t i = 1; i < wraps.size(); ++i)
    {
        INFO ("wrap " << i << " after " << (wraps[i] - wraps[i - 1]) << " samples");
        CHECK (std::llabs (wraps[i] - wraps[i - 1] - 32000) <= 2);
    }
}

TEST_CASE ("audio host: a looping range whose length is not a whole number of samples wraps without drifting")
{
    /*  A range of 47 999.5 samples at 48 kHz, looped at one and a half: a pass
        is 31 999.67 samples of output, so the click at the range's start comes
        back on a grid that is not a whole number of samples. Twelve passes in,
        a loop that wrapped a whole number of samples at a time - 47 999 or
        48 000 every pass - would be four samples off that grid. The loop sits
        below the reader of a launched clip (patch 0002), where the file is
        counted in whole frames, and keeps the range's exact length. */
    SpeedRig speed;
    const auto clicks = writeClicks (speed.rig.storage.folder, speed.rate, 2);
    REQUIRE (clicks.existsAsFile());

    const auto length = 47999.5 / speed.rate;
    REQUIRE (speed.open (clicks, false, { { 0.0, length, 0 } }));
    REQUIRE (speed.go (1.5));

    const auto heard = speed.record (0.1, 8.0);
    const auto* x = heard.getReadPointer (0);

    std::vector<std::int64_t> wraps;

    for (int n = 0; n < heard.getNumSamples(); ++n)
    {
        if (std::abs (x[n]) < 0.2f)
            continue;

        auto peak = n;

        while (n < heard.getNumSamples() && std::abs (x[n]) >= 0.05f)
        {
            if (std::abs (x[n]) > std::abs (x[peak]))
                peak = n;

            ++n;
        }

        wraps.push_back (speed.startedAt + peak);
    }

    REQUIRE (wraps.size() >= 12);

    const auto pass = 47999.5 / 1.5;

    for (std::size_t i = 1; i < wraps.size(); ++i)
    {
        const auto expected = static_cast<double> (i) * pass;
        INFO ("wrap " << i << " at " << (wraps[i] - wraps[0]) << " samples after the first, " << expected << " expected");
        CHECK (std::abs (static_cast<double> (wraps[i] - wraps[0]) - expected) <= 2.0);
    }
}

TEST_CASE ("audio host: a stretched range loops with no gap and no click at its wraps")
{
    /*  The second second of a 500 Hz tone - five hundred whole periods, so the
        file itself joins with no step - looped, in timestretch, at three
        speeds. A pass that started the stretcher again left a gap of about one
        of its chunks and a click at every wrap (M47, 2026-09-29): the loop was
        split above the stretcher, so a wrap was a jump back in its source and
        a new prime. The loop of a launched clip now sits below the stretcher
        (patch 0002), which reads one unbroken stream through every pass. */
    constexpr float amplitude = 0.25f;
    //  A folder of this run's own: the C and fr_FR runs go side by side, and
    //  two writers of one fixed file fail each other.
    const ScopedStorage media;
    const auto folder = media.folder;
    const auto tone = writeSineTone (folder, 48000, 500.0, amplitude, 4);
    REQUIRE (tone.existsAsFile());

    //  The largest step a 500 Hz sine at a quarter of full scale makes.
    const auto ownStep = static_cast<float> (2.0 * juce::MathConstants<double>::pi * 500.0 / 48000.0 * amplitude);

    for (const auto speed : { 1.0, 1.5, 0.75 })
    {
        INFO ("at x" << speed);

        SpeedRig voice;
        REQUIRE (voice.open (tone, true, { { 1.0, 2.0, 0 } }));
        REQUIRE (voice.go (speed));

        //  From half a pass in, three passes and more: at least two wraps at every speed.
        const auto heard = voice.record (0.5 / speed, 3.0);
        auto quietest = 1.0f;

        for (int from = 0; from + 256 <= heard.getNumSamples(); from += 64)
            quietest = std::min (quietest, rmsOver (heard, from, from + 256));

        const auto quietestDb = 20.0 * std::log10 (std::max (1.0e-9, static_cast<double> (quietest) / (amplitude / std::sqrt (2.0))));
        const auto step = largestStep (heard);

        INFO ("the quietest 256 samples at " << quietestDb << " dB re the tone; the largest step "
              << step << " against the tone's own " << ownStep);
        CHECK (quietestDb > -3.0);
        CHECK (step < 1.5f * ownStep);
    }
}

TEST_CASE ("audio host: a stretched cue passes from one range to the next with no gap")
{
    /*  Two ranges end to end - the second and third seconds of a 500 Hz tone -
        and the boundary placed as the Runner places it: the outgoing range's
        stop and the incoming one's launch on the same sample. M12 priced that
        pair at 25 to 33 samples of the outgoing range's decay, in either mode.

        The incoming range's stretcher is primed at its launch, on the audio
        thread - a long block (M46: 1 to 5 ms in Release, far more in Debug).
        Tracktion's DeviceManager answered a block over 0.98 of its budget by
        writing the NEXT block as silence without playing it: a 256-sample gap
        a block or two after the boundary, every time in Debug. The author
        turned that mute off (2026-09-29, namespace draft §22.12), and this
        case is what says it stays off.

        What is left is named rather than judged: at one the boundary steps no
        further than a resampled one; away from one, two stretchers are
        spliced with their phases not lined up - 0.26 against varispeed's 0.07
        at one and a half. */
    constexpr float amplitude = 0.25f;
    //  A folder of this run's own: the C and fr_FR runs go side by side, and
    //  two writers of one fixed file fail each other.
    const ScopedStorage media;
    const auto folder = media.folder;
    const auto tone = writeSineTone (folder, 48000, 500.0, amplitude, 4);
    REQUIRE (tone.existsAsFile());

    for (const auto speed : { 1.0, 1.5 })
    {
        float steps[2] {};
        double quietest[2] {};

        for (const auto stretch : { false, true })
        {
            INFO ("at x" << speed << (stretch ? ", in timestretch" : ", in varispeed"));

            SpeedRig voice;
            REQUIRE (voice.open (tone, stretch, { { 1.0, 2.0, 1 }, { 2.0, 3.0, 1 } }));
            REQUIRE (voice.go (speed));

            //  Once the first range sounds, as the Runner places a boundary: a
            //  stop sent to a handle that has not started cancels its launch.
            voice.runTo (voice.at (0.1));
            REQUIRE (voice.rig.host.trackPlayState (0, 0).playing);

            const auto boundary = voice.at (1.0 / speed);
            REQUIRE (voice.rig.host.stopTrackAt (0, 0, voice.rig.host.beatsAtSample (boundary)));
            REQUIRE (voice.rig.host.launchTrackAt (0, 1, voice.rig.host.beatsAtSample (boundary)));

            //  A quarter of a second either side of the boundary.
            const auto heard = voice.record (1.0 / speed - 0.25, 0.5);
            auto quiet = 1.0f;

            for (int from = 0; from + 256 <= heard.getNumSamples(); from += 64)
                quiet = std::min (quiet, rmsOver (heard, from, from + 256));

            quietest[stretch ? 1 : 0] = 20.0 * std::log10 (std::max (1.0e-9, static_cast<double> (quiet) / (amplitude / std::sqrt (2.0))));
            steps[stretch ? 1 : 0] = largestStep (heard);
        }

        MESSAGE ("x" << speed << ": the boundary's largest step " << steps[0] << " resampled, " << steps[1]
                 << " stretched; the quietest 256 samples " << quietest[0] << " and " << quietest[1] << " dB");

        INFO ("at x" << speed);
        CHECK (quietest[0] > -3.0);
        CHECK (quietest[1] > -3.0);

        //  At one only: away from it the splice is named, not judged.
        if (speed < 1.25)
            CHECK (steps[1] <= steps[0] * 1.5f);
    }
}

TEST_CASE ("audio host: a graph rebuilt in the middle of a ramp keeps its place")
{
    /*  Tracktion keeps no answer about speed between blocks, so a rebuild
        needs nothing carried across it (patch 0002). Two voices; one plays a
        ramp, and halfway through it the other is armed - a rebuild. Against the
        same ramp with no rebuild, the voice that plays must not have moved. */
    auto render = [] (bool rebuild)
    {
        SpeedRig speed;
        const auto tone = writeSineTone (speed.rig.storage.folder, speed.rate, 500.0, 0.25f, 5);
        const auto other = writeSineTone (speed.rig.storage.folder.getChildFile ("other"), speed.rate, 300.0, 0.25f, 2);
        REQUIRE (speed.open (tone, false, {}, 2));

        //  Both renders launch on the same sample, whatever the disk made open() pump.
        speed.runTo (30 * speed.rate);

        REQUIRE (speed.go (1.0));
        REQUIRE (speed.speedAt (0.25, 1.0));
        REQUIRE (speed.speedAt (1.25, 2.0));

        auto first = speed.record (0.1, 0.6);

        if (rebuild)
            REQUIRE (speed.rig.host.setTrackSource (1, 0, other.getFullPathName().toStdString()));

        auto second = speed.record (0.75, 1.0);
        return std::make_pair (first, second);
    };

    const auto plain = render (false);
    const auto rebuilt = render (true);

    const auto* a = plain.second.getReadPointer (0);
    const auto* b = rebuilt.second.getReadPointer (0);
    auto largest = 0.0f;

    for (int n = 0; n < plain.second.getNumSamples(); ++n)
        largest = std::max (largest, std::abs (a[n] - b[n]));

    INFO ("largest difference after the rebuild: " << largest);
    CHECK (largest < 1.0e-6f);
}

TEST_CASE ("audio host: a cue at one is exact, bit for bit, whatever speeds the voice had before")
{
    /*  §22.4: at exactly one from a launch on, a slot answers with Tracktion's
        own arithmetic. So a voice that played a cue at a half and then plays one
        at one must sound exactly as a voice that never had a speed. */
    auto render = [] (bool history)
    {
        SpeedRig speed;
        const auto tone = writeSineTone (speed.rig.storage.folder, speed.rate, 700.0, 0.25f, 3);
        REQUIRE (speed.open (tone, false));

        if (history)
        {
            REQUIRE (speed.go (0.5));
            speed.runTo (speed.at (0.6));
            REQUIRE (speed.rig.host.stopTrackAt (0, 0, speed.rig.host.beatsAtSample (speed.at (0.6) + 4 * speed.block)));
        }

        //  Both launch the second cue on the same sample, whatever the disk
        //  made open() pump - thirty seconds is past any of it.
        speed.runTo (30 * speed.rate);
        REQUIRE (speed.go (1.0));

        return speed.record (0.05, 0.5);
    };

    const auto fresh = render (false);
    const auto after = render (true);

    CHECK (fresh.getNumSamples() == after.getNumSamples());
    CHECK (std::memcmp (fresh.getReadPointer (0), after.getReadPointer (0),
                        sizeof (float) * static_cast<std::size_t> (fresh.getNumSamples())) == 0);
    CHECK (rmsOver (fresh, 0, fresh.getNumSamples()) > 0.1f);
}

TEST_CASE ("audio host: speed renders for the ear, written when WFG_SPEED_WAVS names a folder")
{
    /*  Not a check: the S.2 stage's promise to the author, who judges a speed
        by listening. With WFG_SPEED_WAVS set, five short renders land there -
        both modes at a half and at two, a tape stop, and a freeze that lets go.
        Without it, the case runs one of them and writes nothing. */
    const auto folderName = juce::SystemStats::getEnvironmentVariable ("WFG_SPEED_WAVS", {});
    const auto writing = folderName.isNotEmpty();
    const juce::File folder = writing ? juce::File (folderName) : juce::File();

    struct Take
    {
        const char* name;
        bool stretch;
        std::vector<std::pair<double, double>> speeds;   // (seconds after the launch, speed), after the launch's own
        double launchSpeed;
    };

    const std::vector<Take> takes {
        { "varispeed-half",  false, {}, 0.5 },
        { "varispeed-twice", false, {}, 2.0 },
        { "stretch-half",    true,  {}, 0.5 },
        { "stretch-twice",   true,  {}, 2.0 },
        { "tape-stop",       false, { { 1.0, 1.0 }, { 3.0, 0.0 } }, 1.0 },
        { "freeze",          true,  { { 1.0, 1.0 }, { 2.0, 0.0 }, { 4.0, 0.0 }, { 4.5, 1.0 } }, 1.0 },
    };

    for (const auto& take : takes)
    {
        if (! writing && std::string (take.name) != "tape-stop")
            continue;

        SpeedRig speed;

        /*  A chord with some movement in it - a fifth and an octave, and a
            slow tremolo - so a stretcher's work and a resampler's are both
            audible. */
        const auto file = speed.rig.storage.folder.getChildFile ("chord.wav");
        speed.rig.storage.folder.createDirectory();
        {
            juce::WavAudioFormat format;
            std::unique_ptr<juce::OutputStream> stream { file.createOutputStream() };
            REQUIRE (stream != nullptr);
            auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                             .withSampleRate (speed.rate)
                                                             .withNumChannels (1)
                                                             .withBitsPerSample (24));
            REQUIRE (writer != nullptr);
            juce::AudioBuffer<float> buffer { 1, speed.rate * 6 };

            for (int n = 0; n < buffer.getNumSamples(); ++n)
            {
                const auto t = static_cast<double> (n) / speed.rate;
                const auto tremolo = 0.75 + 0.25 * std::sin (2.0 * juce::MathConstants<double>::pi * 3.0 * t);
                const auto value = tremolo * (std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * t)
                                               + 0.6 * std::sin (2.0 * juce::MathConstants<double>::pi * 330.0 * t)
                                               + 0.4 * std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * t));
                buffer.setSample (0, n, static_cast<float> (0.2 * value));
            }

            writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
        }

        REQUIRE (speed.open (file, take.stretch));
        REQUIRE (speed.go (take.launchSpeed));

        for (const auto& [seconds, value] : take.speeds)
            REQUIRE (speed.speedAt (seconds, value));

        const auto heard = speed.record (0.0, 6.0);
        CHECK (rmsOver (heard, 0, speed.rate / 2) > 0.01f);

        if (writing)
        {
            folder.createDirectory();
            const auto out = folder.getChildFile (juce::String (take.name) + ".wav");
            out.deleteFile();
            juce::WavAudioFormat format;
            std::unique_ptr<juce::OutputStream> stream { out.createOutputStream() };
            REQUIRE (stream != nullptr);
            auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                             .withSampleRate (speed.rate)
                                                             .withNumChannels (1)
                                                             .withBitsPerSample (24));
            REQUIRE (writer != nullptr);
            writer->writeFromAudioSampleBuffer (heard, 0, heard.getNumSamples());
            MESSAGE ("wrote " << out.getFullPathName());
        }
    }
}

//==============================================================================
namespace
{
    /*  The amplitude of `hertz` in a recording, by Goertzel under a Hann
        window: a sine of amplitude A reads A, give or take the window's own
        leakage. What M47 reads a speed's fundamental with. */
    double amplitudeAt (const juce::AudioBuffer<float>& buffer, double hertz, int rate)
    {
        const auto* x = buffer.getReadPointer (0);
        const auto n = buffer.getNumSamples();
        const auto w = 2.0 * juce::MathConstants<double>::pi * hertz / rate;
        const auto c = 2.0 * std::cos (w);

        auto s1 = 0.0, s2 = 0.0, sum = 0.0;

        for (int i = 0; i < n; ++i)
        {
            const auto window = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / (n - 1));
            const auto s0 = window * static_cast<double> (x[i]) + c * s1 - s2;
            s2 = s1;
            s1 = s0;
            sum += window;
        }

        const auto power = s1 * s1 + s2 * s2 - c * s1 * s2;
        return 2.0 * std::sqrt (std::max (0.0, power)) / sum;
    }

    double decibels (double ratio)
    {
        return 20.0 * std::log10 (std::max (ratio, 1.0e-12));
    }
}

/*  M46 - A VOICE'S COST AT SPEED (namespace draft §22.8). One voice, launched
    at each speed in each mode, at 48 and 96 kHz, in blocks of 64, 256 and
    1024: the mean microseconds a block over a second of output, after a
    quarter second's settling, against the block's period; the worst of the
    first twenty blocks from the launch, which is where a stretched voice
    primes; and whether Go.dot's own code allocated on the way. Then a
    stretched loop, whose every wrap primes again (the loop is split above
    the stretcher, `WaveNode.cpp:1312-1336`): the worst block beside a wrap
    against the median block. Run with --no-skip on a quiet machine, in
    Release; the figures go to §22.8. */
TEST_CASE ("M46: a voice's cost in the audio callback at speed - by mode, speed, rate and block" * doctest::skip())
{
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("wfg-m46");

    const auto timeBlock = [] (SpeedRig& voice)
    {
        const auto from = std::chrono::steady_clock::now();
        voice.rig.host.processBlock();
        return std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - from).count();
    };

    for (const auto rate : { 48000, 96000 })
    {
        //  Thirty seconds, so twenty times its speed has a second and a half to play.
        const auto file = writeSineTone (folder.getChildFile (juce::String (rate)), rate, 750.0, 0.25f, 30);
        REQUIRE (file.existsAsFile());

        for (const auto block : { 64, 256, 1024 })
            for (const auto stretch : { false, true })
                for (const auto speed : { 1.0, 2.0, 8.0, 20.0 })
                {
                    SpeedRig voice;
                    voice.rate = rate;
                    voice.block = block;
                    REQUIRE (voice.open (file, stretch));

                    rt::resetCounts();
                    REQUIRE (voice.go (speed));
                    voice.runTo (voice.launch - block);

                    auto worstAtLaunch = 0.0;

                    for (int i = 0; i < 20; ++i)
                        worstAtLaunch = std::max (worstAtLaunch, timeBlock (voice));

                    voice.runTo (voice.at (0.25));

                    const auto blocks = rate / block;
                    auto total = 0.0;

                    for (int i = 0; i < blocks; ++i)
                        total += timeBlock (voice);

                    const auto budget = 1.0e6 * block / rate;
                    const auto mean = total / blocks;

                    MESSAGE ("M46 " << std::string (stretch ? "timestretch" : "varispeed") << " x" << speed
                             << " at " << rate << " Hz, blocks of " << block << ": " << mean << " us a block ("
                             << 100.0 * mean / budget << "% of " << budget << " us); the worst of the first "
                             << "twenty from the launch " << worstAtLaunch << " us ("
                             << 100.0 * worstAtLaunch / budget << "%); " << rt::violations()
                             << " real-time violations");

                    CHECK (mean > 0.0);
                    CHECK (rt::violations() == 0);
                }

        //  A stretched loop of a second, played at one: every wrap primed, until the
        //  loop moved below the stretcher (§22.12) - now a wrap is an ordinary block.
        for (const auto block : { 64, 256 })
        {
            SpeedRig voice;
            voice.rate = rate;
            voice.block = block;
            REQUIRE (voice.open (file, true, { { 1.0, 2.0, 0 } }));
            REQUIRE (voice.go (1.0));
            voice.runTo (voice.launch - block);

            std::vector<double> costs;
            std::vector<std::int64_t> starts;

            while (voice.rig.host.clock().samplesElapsed() < voice.at (3.5))
            {
                starts.push_back (voice.rig.host.clock().samplesElapsed());
                costs.push_back (timeBlock (voice));
            }

            auto sorted = costs;
            std::sort (sorted.begin(), sorted.end());
            const auto median = sorted[sorted.size() / 2];

            auto worstAtWrap = 0.0;

            for (std::size_t i = 0; i < costs.size(); ++i)
                for (const auto wrap : { voice.at (1.0), voice.at (2.0), voice.at (3.0) })
                    if (std::abs (starts[i] - wrap) <= 2 * block)
                        worstAtWrap = std::max (worstAtWrap, costs[i]);

            const auto budget = 1.0e6 * block / rate;

            MESSAGE ("M46 a stretched loop at " << rate << " Hz, blocks of " << block << ": the median block "
                     << median << " us, the worst beside a wrap " << worstAtWrap << " us ("
                     << 100.0 * worstAtWrap / budget << "% of " << budget << " us)");

            CHECK (median > 0.0);
        }
    }
}

/*  M47 - WHAT A SPEED DOES TO THE SOUND (namespace draft §22.8), at 48 kHz:
    - above one, Lagrange's aliasing: a 5 kHz tone at 1.3, 2, 4 and 20 times,
      the fundamental where it should be and everything else - above Nyquist,
      where a proper resampler would give silence, everything is alias;
    - the varispeed gate at blocks of 64: the largest step a tone makes as it
      is slid from a tenth to nought, against the step it makes at one;
    - a freeze: the level timestretch holds at nought, against the tone's;
    - a stretched wrap: the largest step, and the quietest 256 samples, at the
      wraps of a looping second played at one.
    Run with --no-skip; the figures go to §22.8, and say whether Lagrange is
    good enough above one and whether the gate's two numbers are right. */
TEST_CASE ("M47: what a speed does to the sound - aliasing above one, the gate's step, a freeze, a stretched wrap" * doctest::skip())
{
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("wfg-m47");
    constexpr int rate = 48000;
    constexpr double tone = 5000.0;
    constexpr float amplitude = 0.25f;

    //  Aliasing, a speed at a time.
    {
        const auto file = writeSineTone (folder.getChildFile ("high"), rate, tone, amplitude, 30);
        REQUIRE (file.existsAsFile());

        for (const auto speed : { 1.0, 1.3, 2.0, 4.0, 20.0 })
        {
            SpeedRig voice;
            voice.rate = rate;
            voice.block = 256;
            REQUIRE (voice.open (file, false));
            REQUIRE (voice.go (speed));

            const auto heard = voice.record (0.25, 0.5);
            const auto total = rmsOver (heard, 0, heard.getNumSamples());
            const auto wanted = tone * speed;
            const auto below = wanted < 0.5 * rate;
            const auto fundamental = below ? amplitudeAt (heard, wanted, rate) : 0.0;
            const auto rest = std::sqrt (std::max (0.0, static_cast<double> (total) * total
                                                          - 0.5 * fundamental * fundamental));

            if (below)
                MESSAGE ("M47 a " << tone << " Hz tone at x" << speed << ": its " << wanted << " Hz at "
                         << decibels (fundamental / amplitude) << " dB, everything else "
                         << decibels (rest / (amplitude / std::sqrt (2.0))) << " dB re the tone");
            else
                MESSAGE ("M47 a " << tone << " Hz tone at x" << speed << ": above Nyquist, where nothing "
                         << "should sound, " << decibels (rest / (amplitude / std::sqrt (2.0)))
                         << " dB re the tone");

            CHECK (total >= 0.0f);
        }
    }

    const auto low = writeSineTone (folder.getChildFile ("low"), rate, 500.0, amplitude, 30);
    REQUIRE (low.existsAsFile());

    //  The gate at blocks of 64, slid from a tenth to nought over a second.
    {
        SpeedRig voice;
        voice.rate = rate;
        voice.block = 64;
        REQUIRE (voice.open (low, false));
        REQUIRE (voice.go (0.1));
        REQUIRE (voice.speedAt (0.5, 0.1));
        REQUIRE (voice.speedAt (1.5, 0.0));

        const auto heard = voice.record (0.4, 1.3);
        const auto ownAtOne = 2.0 * juce::MathConstants<double>::pi * 500.0 / rate * amplitude;

        MESSAGE ("M47 the varispeed gate at blocks of 64: the largest step " << largestStep (heard)
                 << ", against the tone's own " << ownAtOne << " at one");

        CHECK (largestStep (heard) >= 0.0f);
    }

    //  A freeze: timestretch at nought, held.
    {
        SpeedRig voice;
        voice.rate = rate;
        REQUIRE (voice.open (low, true));
        REQUIRE (voice.go (1.0));
        REQUIRE (voice.speedAt (0.5, 1.0));
        REQUIRE (voice.speedAt (1.0, 0.0));

        const auto held = voice.record (1.5, 1.0);
        const auto level = rmsOver (held, 0, held.getNumSamples());

        MESSAGE ("M47 a freeze holds " << decibels (level / (amplitude / std::sqrt (2.0)))
                 << " dB re the tone it froze, " << risingCrossings (held) << " rising crossings in its second");

        CHECK (level >= 0.0f);
    }

    //  A stretched wrap: a looping second, played at one.
    {
        SpeedRig voice;
        voice.rate = rate;
        voice.block = 256;
        REQUIRE (voice.open (low, true, { { 1.0, 2.0, 0 } }));
        REQUIRE (voice.go (1.0));

        const auto heard = voice.record (0.5, 2.0);
        const auto ownAtOne = 2.0 * juce::MathConstants<double>::pi * 500.0 / rate * amplitude;
        auto quietest = 1.0f;

        for (int from = 0; from + 256 <= heard.getNumSamples(); from += 64)
            quietest = std::min (quietest, rmsOver (heard, from, from + 256));

        MESSAGE ("M47 a stretched loop at one: the largest step " << largestStep (heard)
                 << " against the tone's own " << ownAtOne << "; the quietest 256 samples "
                 << decibels (quietest / (amplitude / std::sqrt (2.0))) << " dB re the tone");

        CHECK (quietest >= 0.0f);
    }
}

TEST_CASE ("audio host: varispeed above one filters out what the speed would lift past Nyquist")
{
    /*  The author, 2026-09-29: "If we can improve on the plain varispeed, to
        avoid aliasing, this would be great. Or we could simply filter out the
        high frequency content." Patch 0002 low-passes the file before the
        interpolator above one, at 0.4 of the output's rate over the speed.
        M47 heard a 5 kHz tone at twenty come out at 4 kHz, as loud. */
    constexpr int rate = 48000;
    constexpr float amplitude = 0.25f;
    //  A folder of this run's own: the C and fr_FR runs go side by side, and
    //  two writers of one fixed file fail each other.
    const ScopedStorage media;
    const auto folder = media.folder;

    struct Case { double hertz; double speed; bool passes; };

    for (const auto& c : { Case { 5000.0, 20.0, false },    // 100 kHz: nothing should sound
                           Case { 15000.0, 2.0, false },    // 30 kHz: would fold to 18
                           Case { 5000.0, 2.0, true },      // 10 kHz: through, as loud
                           Case { 4000.0, 4.0, true },      // 16 kHz: through, as loud
                           Case { 1000.0, 1.3, true } })    // 1.3 kHz: through
    {
        INFO (c.hertz << " Hz at x" << c.speed);

        const auto tone = writeSineTone (folder.getChildFile (juce::String (c.hertz) + "-" + juce::String (c.speed)),
                                         rate, c.hertz, amplitude, 30);
        REQUIRE (tone.existsAsFile());

        SpeedRig voice;
        voice.rate = rate;
        REQUIRE (voice.open (tone, false));
        REQUIRE (voice.go (c.speed));

        const auto heard = voice.record (0.25, 0.5);
        const auto level = rmsOver (heard, 0, heard.getNumSamples()) / (amplitude / std::sqrt (2.0f));

        if (c.passes)
        {
            const auto fundamental = amplitudeAt (heard, c.hertz * c.speed, rate) / amplitude;
            INFO ("the fundamental at " << decibels (fundamental) << " dB");
            CHECK (std::abs (decibels (fundamental)) < 0.5);
        }
        else
        {
            INFO ("everything at " << decibels (level) << " dB re the tone");
            CHECK (decibels (level) < (c.speed > 10.0 ? -60.0 : -25.0));
        }
    }
}
