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

#include <wfg/engine/audio/AudioHost.h>

#include <wfg/engine/rt/RtCheck.h>

#include <wfg/engine/audio/CueOutputPlugin.h>
#include <wfg/engine/clock/AudioClockSource.h>

/*  juce_core and juce_events are named directly even though tracktion_engine.h
    would drag both in: an explicit include survives a Tracktion header
    reshuffle, an implicit one does not.

    The two naming traps recorded in Console.cpp apply here and this is the
    other file they can bite: tracktion_engine.h:72 bare-`#undef`s __TEXT, and
    tracktion_engine_playback.cpp:124-153 #undefs and redefines VERSION
    mid-translation-unit. Nothing below is called VERSION.
*/
#include <cstdint>
#include <vector>

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <tracktion_engine/tracktion_engine.h>
#include <tracktion_graph/tracktion_graph.h>

/*  Not reachable through the umbrella header: the Edit-side graph builder is an
    internal header, while the graph-side utilities (createNodeGraph,
    areNodeIDsUnique) are already public through tracktion_graph.h. */
#include <juce_audio_formats/juce_audio_formats.h>

#include <tracktion_engine/playback/graph/tracktion_TracktionEngineNode.h>
#include <tracktion_engine/playback/graph/tracktion_EditNodeBuilder.h>

namespace wfg::audio
{
    namespace te = tracktion::engine;

    namespace
    {
        /*  What Go.dot asks of the engine, and every line of it is a decision
            rather than a default worth keeping.

            The one that makes this class possible at all is the first: without
            it, constructing a te::Engine opens the default audio device. That
            is the whole difference between an engine a test can create and one
            that grabs the machine's soundcard while CI runs. */
        struct Behaviour final : te::EngineBehaviour
        {
            explicit Behaviour (int outputChannels) : wideDeviceChannels (outputChannels) {}

            /*  ONE WIDE OUTPUT DEVICE, spanning every hardware channel.

                Tracktion's default is to carve the hardware into stereo pairs.
                Go.dot wants the opposite: one device the whole rig wide, with
                every track routed to it at load, so that where a cue actually
                goes is a coefficient in its output plugin rather than a change
                of output device - which spike 04 measured as a graph rebuild.

                This must be answered before the first playback context exists.
                A wave-device layout changed afterwards destroys the running
                graph and rebuilds the device list, which during a show is not a
                thing to do. */
            bool isDescriptionOfWaveDevicesSupported() override { return true; }

            void describeWaveDevices (std::vector<te::WaveDeviceDescription>& descriptions,
                                      juce::AudioIODevice& device,
                                      bool isInput) override
            {
                descriptions.clear();

                if (isInput)
                    return;

                const auto available = device.getOutputChannelNames().size();
                const auto width = std::min (wideDeviceChannels, available);

                if (width <= 0)
                    return;

                descriptions.push_back (
                    te::WaveDeviceDescription::withNumChannels ("Go.dot outputs", 0u,
                                                                static_cast<std::uint32_t> (width),
                                                                true));
            }

            int wideDeviceChannels = 0;

            /*  We open the device, or in tests nobody does. PRD §6.2 makes the
                sample rate an observed property rather than a setting, so the
                engine must not go and pick one before we have looked. */
            bool autoInitialiseDeviceManager() override { return false; }

            /*  One graph thread in Phase 2, deliberately. Tracktion's default is
                every CPU, and its pool waits on semaphores - which is fine for a
                DAW and is a thing PRD §4.2 would rather measure before inviting
                onto the audio path. The plan re-opens this after the callback
                cost is measured, not before. */
            int getNumberOfCPUsToUseForAudio() override { return 1; }

            /*  Launcher clips are how a cue plays (§3.25). With this false they
                are dropped from the playback graph entirely, which would make
                every cue silent for a reason nothing would report. */
            bool areClipSlotsEnabled() override { return true; }

            /*  Tracktion's 3 ms edge fades would put a fade on material the
                document never asked to fade. Spike 03 measured joins as
                sample-accurate; a fade nobody declared is exactly the kind of
                thing that makes a join not be. */
            bool autoAddClipEdgeFades() override { return false; }
        };

        /*  Where Tracktion keeps its preferences and cache.

            Tracktion writes both whether or not anyone asked, so the only
            question is where. Pointing them at a folder the caller names keeps
            a test out of the developer's real application-data directory - and
            keeps two tests running at once out of each other's. */
        struct Storage final : te::PropertyStorage
        {
            explicit Storage (juce::File root)
                : te::PropertyStorage ("Go.dot"), folder (std::move (root))
            {
                folder.createDirectory();
            }

            juce::File getAppCacheFolder() override  { return folder; }
            juce::File getAppPrefsFolder() override  { return folder; }
            juce::String getApplicationVersion() override { return WFG_VERSION; }

            juce::File folder;
        };
    }

    //==============================================================================
    struct AudioHost::Impl
    {
        explicit Impl (std::string folder)
            : storageFolder (juce::String (std::move (folder)))
        {
        }

        bool start (const HostSettings& requested)
        {
            stop();

            if (requested.sampleRate <= 0 || requested.blockSize <= 0
                  || requested.outputChannels <= 0)
            {
                error = "sample rate, block size and output channel count must all be positive";
                return false;
            }

            error.clear();

            /*  DENORMALS, and this is not a formality - it is a bug that was
                measured here rather than reasoned about.

                Standing a Tracktion engine up sets flush-to-zero on the calling
                thread and LEAVES IT SET. That is correct for audio, where a
                denormal is a performance cliff and inaudible either way. It is
                wrong for every other thing this process does: Go.dot's numbers
                are required to survive a save and a load unchanged, and under
                flush-to-zero a subnormal in a show file - a very small gain, a
                coordinate near an origin - reads back as zero. The test suite
                caught it the honest way: three number cases that pass alone
                began failing once an audio case ran before them in the same
                process.

                So the mode is scoped rather than inherited. ScopedNoDenormals
                saves the FP status register, sets the flag, and restores what
                was there - so the flag lives exactly where it belongs and no
                caller has its arithmetic changed behind its back. */
            const juce::ScopedNoDenormals denormalsOffWhileTracktionStartsUp;

            /*  Constructed here rather than in the constructor so that a failed
                start leaves nothing behind, and so the folder is not created by
                the mere existence of a host that never runs. */
            engine = std::make_unique<te::Engine> (std::make_unique<Storage> (storageFolder),
                                                   std::make_unique<te::UIBehaviour>(),
                                                   std::make_unique<Behaviour> (requested.outputChannels));

            /*  Registered once, here. It only appends to a list and de-dupes on
                the type string, so doing it after Engine construction is safe -
                PluginManager::initialise has already run inside it. */
            engine->getPluginManager().createBuiltInType<CueOutputPlugin>();

            auto& hosted = engine->getDeviceManager().getHostedAudioDeviceInterface();

            te::HostedAudioDeviceInterface::Parameters parameters;
            parameters.sampleRate = requested.sampleRate;
            parameters.blockSize = requested.blockSize;
            parameters.outputChannels = requested.outputChannels;

            /*  No inputs and no MIDI in this phase. Live input is Phase 9's
                rack and MIDI cues are Phase 3; asking for either now would
                build graph nodes nothing drives and make the block cost
                measured here a measurement of the wrong thing. */
            parameters.inputChannels = 0;
            parameters.useMidiDevices = false;

            hosted.initialise (parameters);
            hosted.prepareToPlay (requested.sampleRate, requested.blockSize);

            /*  The device list is built asynchronously. Flushing it here means
                the wide device exists before anything asks for it, rather than
                one message-loop turn later. */
            engine->getDeviceManager().dispatchPendingUpdates();

            /*  Sized once, here, and reused for every block. Allocating inside
                processBlock would be the first violation of §4.2 in a file
                whose whole purpose is to be callable from the audio thread. */
            scratch.setSize (requested.outputChannels, requested.blockSize, false, true, true);
            midi.ensureSize (256);

            current = requested;
            blocks.store (0, std::memory_order_relaxed);
            running = true;

            return true;
        }

        bool buildEdit (const EditSpec& spec)
        {
            if (engine == nullptr || ! running)
            {
                error = "the engine is not running";
                return false;
            }

            if (spec.tracks < 0 || spec.channelsPerTrack <= 0)
            {
                error = "a track count cannot be negative and a track must have channels";
                return false;
            }

            edit.reset();
            matrices.clear();
            plugins.clear();

            /*  createEmptyEdit touches no disk. Edit::createEdit is the only
                non-test, non-preview factory; the Edit ctor and
                createSingleTrackEdit both hard-code one track. */
            /*  Every member named, because GCC's -Wmissing-field-initializers
                is an error in the strict build and because a partially braced
                aggregate is a trap: adding a field upstream would silently
                value-initialise it here. */
            te::Edit::Options options { *engine, {}, {} };
            options.editState = te::createEmptyEdit (*engine);
            options.editProjectItemID = te::ProjectItemID::createNewID (te::ProjectID{});

            /*  forEditing is the role that plays. The bitmask also lets us turn
                proxy rendering off structurally rather than per clip - Go.dot
                streams the original file and writes nothing beside it. */
            options.role = static_cast<te::Edit::EditRole> (te::Edit::proxiesDisabled);
            options.loadContext = nullptr;

            /*  The Edit is generated from the document and never saved (§3.25),
                so there is nothing to undo in it and no file to resolve to. */
            options.numUndoLevelsToStore = 1;
            options.editFileRetriever = [] { return juce::File(); };
            options.filePathResolver = [] (const juce::String& path) { return juce::File (path); };

            options.numAudioTracks = static_cast<std::uint32_t> (std::max (0, spec.tracks));

            /*  Tracktion's default is -3 dB on the master. A show that asked for
                0 dB and got -3 would be quietly wrong by half a level. */
            options.defaultMasterVolumedB = 0.0f;

            edit = te::Edit::createEdit (std::move (options));

            if (edit == nullptr)
            {
                error = "Tracktion could not create the Edit";
                return false;
            }

            /*  Both of these call restartPlayback(), which is free while there
                is no playback context and a full graph rebuild once there is.
                They belong here, before any clip and before the transport. */
            edit->setLatencyCompensationEnabled (false);

            /*  60 bpm, so one beat is one second. Launch instants are expressed
                as a MonotonicBeat, and Go.dot counts in samples and seconds -
                this is what makes the conversion arithmetic rather than tempo. */
            if (auto* tempo = edit->tempoSequence.getTempo (0))
                tempo->setBpm (60.0);

            const auto placeholder = ensureSilentPlaceholder (spec.channelsPerTrack);
            const auto tracks = te::getAudioTracks (*edit);

            for (auto* track : tracks)
            {
                if (track == nullptr)
                    continue;

                /*  Tracktion's own volume and meter plugins go. The volume one
                    takes a spin lock on the audio thread for VCA support Go.dot
                    does not use, and it is stereo-shaped - it would remap a
                    mono cue to two channels before our matrix ever saw it,
                    which is exactly the silent widening §3.9b forbids. */
                if (auto* volume = track->getVolumePlugin())
                    volume->removeFromParent();

                if (auto* meter = track->getLevelMeterPlugin())
                    meter->removeFromParent();

                /*  One slot per track in Phase 2: one cue per track at a time,
                    which is what the polyphony ceiling means. */
                track->getClipSlotList().ensureNumberOfSlots (1);

                /*  The resident clip. It stays for the life of the show; arming
                    a cue later points it at real media. Without one the slot is
                    empty, the launcher node is not built, and the track's output
                    stage is not in the graph at all. */
                if (placeholder.existsAsFile())
                {
                    const auto slots = track->getClipSlotList().getClipSlots();

                    if (! slots.isEmpty() && slots[0] != nullptr)
                        if (auto clip = te::insertWaveClip (*slots[0], "resident", placeholder,
                                                            { { tracktion::TimePosition(),
                                                                tracktion::TimeDuration::fromSeconds (1.0) } },
                                                            te::DeleteExistingClips::yes))
                            makeClipPlayAtItsOwnRate (*clip);
                }

                auto plugin = track->pluginList.insertPlugin (
                    CueOutputPlugin::create (spec.channelsPerTrack, current.outputChannels), -1);

                auto* output = dynamic_cast<CueOutputPlugin*> (plugin.get());

                if (output == nullptr)
                {
                    error = "the cue output plugin would not insert";
                    edit.reset();
                    matrices.clear();
                    return false;
                }

                matrices.push_back (&output->matrix());
                plugins.push_back (output);

                /*  Routed once, to the one wide device. This is the structural
                    edit that never happens again. */
                if (auto& manager = engine->getDeviceManager();
                    manager.getNumWaveOutDevices() > 0)
                    if (auto* wide = manager.getWaveOutDevice (0))
                        track->getOutput().setOutputToDeviceID (wide->getDeviceID());
            }

            /*  The engine's own idiom before allocating a context: flush the
                asynchronous updates the edits above queued, rather than pumping
                a message loop and hoping. */
            edit->dispatchPendingUpdatesSynchronously();

            /*  THE TRANSPORT STARTS HERE AND IS NEVER STOPPED (PRD §3.25). It
                is not a play button - it is the clock everything else is placed
                against, and stopping it would be stopping time.

                It also has to happen for the graph to exist at all: a plugin's
                initialise() runs when the playback context is allocated, and
                until then the output stages have no buffers and no sample rate.

                The rate is checked rather than assumed. createNode falls back to
                44100/256 if the device manager reports nothing, building a
                complete and perfectly working graph at the wrong rate, with
                nothing logged - which would show up as a show running slightly
                fast and no clue why. */
            auto& manager = engine->getDeviceManager();

            if (manager.getSampleRate() <= 0.0 || manager.getBlockSize() <= 0)
            {
                error = "the device manager reports no sample rate, so the graph would"
                        " be built at a fallback rate nobody asked for";
                edit.reset();
                matrices.clear();
                return false;
            }

            auto& transport = edit->getTransport();

            transport.ensureContextAllocated();
            transport.play (false);

            error.clear();
            return true;
        }

        void stop()
        {
            edit.reset();
            matrices.clear();
            plugins.clear();

            if (engine != nullptr)
            {
                /*  Tearing the engine down touches the same flag that starting
                    it did. */
                const juce::ScopedNoDenormals denormalsOffWhileTracktionShutsDown;

                /*  The device manager is told before the engine goes, because
                    the hosted interface holds a pointer into it. */
                engine->getDeviceManager().closeDevices();
                engine.reset();
            }

            running = false;
            current = {};
        }

        void processBlock()
        {
            if (! running)
                return;

            /*  THE LIPOGRAM, IN TWO PARTS (PRD §4.2). Everything Go.dot does in
                this function is inside `ours` and must allocate nothing; the
                Tracktion call is inside `foreign`, whose allocations are
                counted and published rather than judged. TE's device callback
                takes a shared lock every block by design, and pretending our
                rule covers code we did not write would make the number
                meaningless in the one direction that matters. */
            const rt::ScopedRealtimeCheck goDotsOwnBlock { rt::Region::ours };

            /*  Denormals off for the block and only for the block - see the
                note in start(). The audio wants the flag; whatever formats a
                number after this returns must not inherit it. */
            const juce::ScopedNoDenormals denormalsOffWhileAudioRuns;

            scratch.clear();
            midi.clear();

            {
                const rt::ScopedRealtimeCheck tracktionsBlock { rt::Region::foreign };

                engine->getDeviceManager().getHostedAudioDeviceInterface().processBlock (scratch, midi);
            }

            if (sink != nullptr)
                sink->blockProduced (scratch.getArrayOfReadPointers(),
                                     scratch.getNumChannels(), current.blockSize);

            /*  After the graph has run, not before. A reader that saw the new
                sample count would otherwise be told the block had happened
                while it was still happening. */
            samples.advance (current.blockSize);
            blocks.fetch_add (1, std::memory_order_relaxed);
        }

        juce::File storageFolder;

        /*  A second of silence, written once into the folder Tracktion already
            uses for its own cache.

            WHY A FILE AT ALL. A clip slot holds a clip for the life of the show
            so that arming a cue changes a source rather than adding a node - but
            a launcher clip whose source names nothing readable is dropped from
            the graph entirely, taking its track's output stage with it. The
            placeholder is what a track sounds like before its first cue: silent,
            and present.

            It is in the cache and never in the bundle. A show is what somebody
            decided (§4.10); a second of silence is not. */
        juce::File ensureSilentPlaceholder (int channels)
        {
            const auto file = storageFolder.getChildFile ("silence.wav");

            if (file.existsAsFile())
                return file;

            storageFolder.createDirectory();

            juce::WavAudioFormat format;
            std::unique_ptr<juce::OutputStream> stream { file.createOutputStream() };

            if (stream == nullptr)
                return {};

            const auto rate = current.sampleRate > 0 ? current.sampleRate : 48000;

            /*  The options overload, not the six-argument one: JUCE deprecated
                that at this pin and the strict build is -Werror. It also takes
                ownership through the unique_ptr, so there is no release() to
                forget. */
            auto writer = format.createWriterFor (stream,
                                                  juce::AudioFormatWriterOptions{}
                                                    .withSampleRate (static_cast<double> (rate))
                                                    .withNumChannels (channels)
                                                    .withBitsPerSample (16));

            if (writer == nullptr)
                return {};

            juce::AudioBuffer<float> silence { channels, rate };
            silence.clear();
            writer->writeFromAudioSampleBuffer (silence, 0, rate);

            return file;
        }

        te::WaveAudioClip* clipOn (int trackIndex) const
        {
            if (edit == nullptr)
                return nullptr;

            const auto tracks = te::getAudioTracks (*edit);

            if (trackIndex < 0 || trackIndex >= tracks.size())
                return nullptr;

            auto* track = tracks[trackIndex];

            if (track == nullptr)
                return nullptr;

            const auto slots = track->getClipSlotList().getClipSlots();

            if (slots.isEmpty() || slots[0] == nullptr)
                return nullptr;

            return dynamic_cast<te::WaveAudioClip*> (slots[0]->getClip());
        }

        /*  GO.DOT OWNS TIME, AND THIS IS WHERE THAT STOPS BEING A SLOGAN.

            A launcher clip is a musical object. It is played through auto-tempo:
            Tracktion stretches it so that its length in BEATS - taken from the
            loop info the file was scanned with - fits the Edit's tempo map, and
            the launch handle schedules it in beats. Turning auto-tempo off does
            not make it play at its own rate, it makes it play NOTHING: with no
            beat length there is nothing for the launcher to schedule. Measured,
            twice, in both directions.

            So the rate is made honest from the other end. The Edit runs at 60
            bpm (buildEdit, and the reason is here): one beat is one second, so a
            clip whose beat count equals its length in seconds is stretched by
            exactly 1:1 and plays as recorded.

            THE BUG THIS FIXES, because it fails as silence rather than as an
            error: the resident clip is created against a ONE-SECOND placeholder,
            so its loop info says one beat. Pointing it at a two-second cue later
            changes the source but not the beat count - so the file is squeezed
            into one second, and at one second the cue goes quiet while the
            launch handle still cheerfully reports that it is playing. M1 lost
            exactly the second half of its tone. */
        static void makeClipPlayAtItsOwnRate (te::WaveAudioClip& clip)
        {
            const auto seconds = clip.getSourceLength().inSeconds();

            if (seconds <= 0.0)
                return;

            auto info = clip.getLoopInfo();
            info.setNumBeats (seconds);          // 60 bpm: one beat, one second
            clip.setLoopInfo (info);

            clip.setAutoPitch (false);
            clip.setSpeedRatio (1.0);
        }

        bool setTrackSource (int trackIndex, const std::string& mediaFile)
        {
            auto* clip = clipOn (trackIndex);
            const juce::File file { juce::String (mediaFile) };

            if (clip == nullptr || ! file.existsAsFile())
                return false;

            clip->getSourceFileReference().setToFile (file, te::SourceFileReference::PathStyle::alwaysAbsolute, false);

            /*  The new file's own loop info, not the placeholder's. Everything
                below reads a length off the clip, and until this runs those
                lengths still describe the file that was there before. */
            makeClipPlayAtItsOwnRate (*clip);

            /*  Looping is off before anything else touches position: turning it
                off rewrites the clip's offset, so doing it afterwards would
                silently discard whatever was set. */
            clip->disableLooping();
            clip->setLength (tracktion::TimeDuration::fromSeconds (
                                 clip->getSourceLength().inSeconds()), false);

            edit->dispatchPendingUpdatesSynchronously();

            return true;
        }

        /*  Waits until the track's source is mapped into the audio file cache,
            pumping blocks meanwhile because the cache only maps a file while
            something holds a Reader for it - and nothing does until the graph
            has run. The clip is not launched yet, so those blocks cost the
            transport nothing: a launcher clip that has not been told to play
            does not advance.

            WHY THIS IS ON THE HOST AND NOT IN A TEST. A cue that is fired
            before its file is mapped plays silence for as long as the disk
            takes, and reports itself as playing throughout - which is the worst
            failure a show can have, because nothing looks wrong. PR 2.3's arm
            calls this from standby, so the wait happens while the operator is
            reading the next line rather than after they press GO.

            Message thread, and it sleeps: never the audio thread, never the
            tick thread on the GO path. */
        bool waitForTrackSourceReady (int trackIndex, int timeoutMilliseconds)
        {
            auto* clip = clipOn (trackIndex);

            if (clip == nullptr || engine == nullptr)
                return false;

            const te::AudioFile file { *engine, clip->getSourceFileReference().getFile() };

            if (! file.isValid())
                return false;

            const auto deadline = juce::Time::getMillisecondCounter()
                                    + static_cast<juce::uint32> (std::max (0, timeoutMilliseconds));

            for (;;)
            {
                if (engine->getAudioFileManager().cache.hasMappedReader (file, 0))
                    return true;

                if (juce::Time::getMillisecondCounter() > deadline)
                    return false;

                for (int i = 0; i < 8; ++i)
                    processBlock();

                juce::Thread::sleep (5);
            }
        }

        bool launchTrack (int trackIndex)
        {
            auto* clip = clipOn (trackIndex);

            if (clip == nullptr || edit == nullptr)
                return false;

            auto handle = clip->getLaunchHandle();
            auto* context = edit->getTransport().getCurrentPlaybackContext();

            if (handle == nullptr || context == nullptr)
                return false;

            const auto syncPoint = context->getSyncPoint();

            if (! syncPoint.has_value())
                return false;

            /*  Just ahead of now. A beat already past launches BACK-DATED - the
                file is skipped forward by the lateness rather than delayed -
                which is the trap PR 2.3's launch-tick rule exists to avoid. At
                60 bpm a twentieth of a beat is 50 ms. */
            const tracktion::engine::MonotonicBeat at {
                tracktion::BeatPosition::fromBeats (syncPoint->monotonicBeat.v.inBeats() + 0.05) };

            handle->play (at);
            return true;
        }

        bool isTrackPlaying (int trackIndex) const
        {
            auto* clip = clipOn (trackIndex);

            if (clip == nullptr)
                return false;

            auto handle = clip->getLaunchHandle();

            return handle != nullptr
                     && handle->getPlayingStatus() == te::LaunchHandle::PlayState::playing;
        }

        double trackSourceLengthSeconds (int trackIndex) const
        {
            auto* clip = clipOn (trackIndex);

            return clip != nullptr ? clip->getSourceLength().inSeconds() : 0.0;
        }

        int residentClipCount() const
        {
            if (edit == nullptr)
                return 0;

            int found = 0;

            for (auto* track : te::getAudioTracks (*edit))
            {
                if (track == nullptr)
                    continue;

                for (auto* slot : track->getClipSlotList().getClipSlots())
                    if (slot != nullptr && slot->getClip() != nullptr)
                        ++found;
            }

            return found;
        }

        AudioHost::NodeIdReport inspectNodeIds() const
        {
            AudioHost::NodeIdReport report;

            if (edit == nullptr || engine == nullptr)
                return report;

            auto* context = edit->getCurrentPlaybackContext();

            if (context == nullptr)
                return report;

            /*  A throwaway graph, built the way the playback context builds one,
                purely to be inspected. Its PlayHead never runs; nothing here
                touches the graph that is actually playing. */
            tracktion::graph::PlayHead playHead;
            tracktion::graph::PlayHeadState playHeadState { playHead };

            /*  The TempoSequence overload is not optional - CombiningNode
                asserts on a ProcessState that has none. */
            te::ProcessState processState { playHeadState, edit->tempoSequence };

            te::CreateNodeParams params { processState };
            params.sampleRate = static_cast<double> (current.sampleRate);
            params.blockSize = current.blockSize;

            /*  THE PLAYBACK-CONTEXT OVERLOAD, and the distinction is the whole
                check. createNodeForEdit (Edit&, params) builds the tracks and
                the master chain and stops there: no per-device summing node, no
                click node, no ChannelRemappingNode at the device boundary, no
                PlayHeadPositionNode. Measured on one track at eight outputs it
                answers 9 nodes where the graph that actually plays has 15 - so
                the six nodes nearest the hardware, the ones a wide device is
                most likely to collide on, were exactly the ones not looked at.

                This overload is what EditPlaybackContext itself calls. */
            std::atomic<double> audibleTime { 0.0 };
            auto node = te::createNodeForEdit (*context, audibleTime, params);

            if (node == nullptr)
                return report;


            /*  THROUGH createNodeGraph, and that is the whole point of this
                function rather than a detail of it.

                The obvious check - areNodeIDsUnique (Node&, bool) - walks the
                graph with visitNodes, which follows getDirectInputNodes() and
                NEVER getInternalNodes(). The collision this project reported
                upstream is in ArrangerLauncherSwitchingNode, which folds its
                INTERNAL children's ids into its own; a launcher clip's nodes are
                exactly the ones that walk misses. So that overload would have
                answered "unique" without ever having looked.

                What Tracktion itself asserts on is nodeGraph->orderedNodes, from
                createNodeGraph (NodePlayerUtilities.h:122). This asks the same
                question of the same collection. */
            auto graph = tracktion::graph::createNodeGraph (std::move (node), true);

            if (graph == nullptr)
                return report;

            std::vector<std::size_t> ids;

            for (auto* n : graph->orderedNodes)
                if (n != nullptr)
                    ids.push_back (n->getNodeProperties().nodeID);

            report.nodes = static_cast<int> (ids.size());

            std::vector<std::size_t> nonZero;

            for (const auto id : ids)
            {
                if (id == 0)
                    ++report.zeroIds;
                else
                    nonZero.push_back (id);
            }

            std::sort (nonZero.begin(), nonZero.end());

            for (std::size_t i = 1; i < nonZero.size(); ++i)
                if (nonZero[i] == nonZero[i - 1])
                    ++report.duplicates;

            return report;
        }

        std::unique_ptr<te::Engine> engine;
        std::unique_ptr<te::Edit> edit;
        std::vector<CueMatrix*> matrices;
        std::vector<CueOutputPlugin*> plugins;
        juce::AudioBuffer<float> scratch;
        juce::MidiBuffer midi;

        AudioClockSource samples;
        std::atomic<std::int64_t> blocks { 0 };

        HostSettings current;
        BlockSink* sink = nullptr;
        bool running = false;
        std::string error;
    };

    //==============================================================================
    AudioHost::AudioHost (std::string storageFolder)
        : impl (std::make_unique<Impl> (std::move (storageFolder)))
    {
    }

    AudioHost::~AudioHost()
    {
        impl->stop();
    }

    bool AudioHost::start (const HostSettings& settings)   { return impl->start (settings); }
    void AudioHost::stop()                                 { impl->stop(); }
    bool AudioHost::isRunning() const noexcept             { return impl->running; }
    const std::string& AudioHost::lastError() const noexcept { return impl->error; }
    void AudioHost::processBlock()                         { impl->processBlock(); }

    void AudioHost::setBlockSink (BlockSink* sink) noexcept { impl->sink = sink; }

    std::int64_t AudioHost::blocksProcessed() const noexcept
    {
        return impl->blocks.load (std::memory_order_relaxed);
    }

    const SampleClock& AudioHost::clock() const noexcept   { return impl->samples; }
    const HostSettings& AudioHost::settings() const noexcept { return impl->current; }

    bool AudioHost::buildEdit (const EditSpec& spec)  { return impl->buildEdit (spec); }

    int AudioHost::trackCount() const noexcept
    {
        return static_cast<int> (impl->matrices.size());
    }

    CueMatrix* AudioHost::trackMatrix (int trackIndex) noexcept
    {
        if (trackIndex < 0 || trackIndex >= trackCount())
            return nullptr;

        return impl->matrices[static_cast<std::size_t> (trackIndex)];
    }

    AudioHost::NodeIdReport AudioHost::inspectNodeIds() const  { return impl->inspectNodeIds(); }
    int AudioHost::residentClipCount() const { return impl->residentClipCount(); }

    bool AudioHost::setTrackSource (int trackIndex, const std::string& mediaFile)
    {
        return impl->setTrackSource (trackIndex, mediaFile);
    }

    bool AudioHost::waitForTrackSourceReady (int trackIndex, int timeoutMilliseconds)
    {
        return impl->waitForTrackSourceReady (trackIndex, timeoutMilliseconds);
    }

    bool AudioHost::launchTrack (int trackIndex)  { return impl->launchTrack (trackIndex); }

    bool AudioHost::isTrackPlaying (int trackIndex) const
    {
        return impl->isTrackPlaying (trackIndex);
    }

    double AudioHost::trackSourceLengthSeconds (int trackIndex) const
    {
        return impl->trackSourceLengthSeconds (trackIndex);
    }

    float AudioHost::trackInputPeak (int trackIndex) const
    {
        if (trackIndex < 0 || trackIndex >= static_cast<int> (impl->plugins.size()))
            return 0.0f;

        return impl->plugins[static_cast<std::size_t> (trackIndex)]->inputPeak();
    }

    float AudioHost::trackOutputPeak (int trackIndex) const
    {
        if (trackIndex < 0 || trackIndex >= static_cast<int> (impl->plugins.size()))
            return 0.0f;

        return impl->plugins[static_cast<std::size_t> (trackIndex)]->outputPeak();
    }

    void AudioHost::resetTrackPeaks (int trackIndex)
    {
        if (trackIndex >= 0 && trackIndex < static_cast<int> (impl->plugins.size()))
            impl->plugins[static_cast<std::size_t> (trackIndex)]->resetPeaks();
    }

    int AudioHost::waveOutputDeviceCount() const noexcept
    {
        if (impl->engine == nullptr)
            return 0;

        return impl->engine->getDeviceManager().getNumWaveOutDevices();
    }

    int AudioHost::waveOutputDeviceWidth() const noexcept
    {
        if (impl->engine == nullptr)
            return 0;

        auto& manager = impl->engine->getDeviceManager();

        if (manager.getNumWaveOutDevices() < 1)
            return 0;

        auto* device = manager.getWaveOutDevice (0);

        return device != nullptr ? device->getChannels().size() : 0;
    }
}
