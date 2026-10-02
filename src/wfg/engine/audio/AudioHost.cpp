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
#include <wfg/engine/audio/EqPlugin.h>
#include <wfg/engine/audio/LiveInputPlugin.h>
#include <wfg/engine/audio/Looper.h>
#include <wfg/engine/audio/LooperPlugin.h>
#include <wfg/engine/audio/ProxyPlugin.h>
#include <wfg/engine/audio/RateVoice.h>
#include <wfg/engine/clock/AudioClockSource.h>

/*  juce_core and juce_events are named directly even though tracktion_engine.h
    would drag both in: an explicit include survives a Tracktion header
    reshuffle, an implicit one does not.

    The two naming traps recorded in Console.cpp apply here and this is the
    other file they can bite: tracktion_engine.h:72 bare-`#undef`s __TEXT, and
    tracktion_engine_playback.cpp:124-153 #undefs and redefines VERSION
    mid-translation-unit. Nothing below is called VERSION.
*/
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <typeinfo>
#include <utility>
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

            /*  Scanning is out of process, always (PRD §3.18); the scan verb
                stands its own engine up to do it, but the flag has to be true
                here too or the list this engine reads back would refuse the
                separate-process setting the scan wrote. */
            bool canScanPluginsOutOfProcess() override { return true; }

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

            /*  A CUE IN VARISPEED IS RESAMPLED, NOT STRETCHED - the question
                patch 0001 adds to Tracktion (patches/tracktion_engine/, decision
                DQ, namespace draft §22.3).

                Every slot clip is auto-tempo, and Tracktion hands an auto-tempo
                clip whose mode is `disabled` the default stretcher. Since
                Signalsmith is compiled in (§22, DZ) that default is Signalsmith,
                so without this every cue - those at one included - would go
                through a stretcher: no longer bit-exact, primed, and its CPU spent
                for nothing. With it, `disabled` means the resampler, and a cue in
                timestretch names Signalsmith itself at the arm. */
            bool autoTempoClipsUseDefaultTimeStretcher() override { return false; }
        };

        /*  ONE SLOT'S ANSWER TO TRACKTION'S QUESTION ABOUT SPEED (patch 0002,
            namespace draft §22.3-22.4): how far beyond one-for-one a clip
            launched at a beat has played, and when it will have played a length
            of its file. Both are read off the voice's RateClock, in the
            monotonic beats the launch handles are placed in.

            AT EXACTLY ONE FROM THE LAUNCH ON, the answers are Tracktion's own
            arithmetic - nought, and `launch + length`, the same doubles - so a
            cue at one renders bit for bit as it did before there was a speed.

            Where the file was AT the launch is kept here, the first time a
            launch is asked about: the clock lets go of its past a block at a
            time, and a clip is asked about its launch for as long as it plays.

            Asked on the audio thread, inside Tracktion's block, twice a block at
            most. Its own code is ours to judge (PRD §4.2): nothing allocates,
            locks or throws, and the region says so. */
        struct SlotSpeed final : te::LaunchHandle::SpeedSource
        {
            explicit SlotSpeed (const RateVoice& voiceToRead) noexcept : voice (voiceToRead) {}

            double extraSourceBeats (te::MonotonicBeat launch, tracktion::BeatDuration sinceLaunch) noexcept WFG_AUDIO_THREAD override
            {
                const rt::ScopedRealtimeCheck goDotsOwn { rt::Region::ours };

                const auto& clock = voice.clock();
                const auto at = launch.v.inBeats();

                if (clock.isIdentityFrom (at))
                    return 0.0;

                const auto since = sinceLaunch.inBeats();
                return clock.sourceAt (at + since) - sourceAtLaunch (at) - since;
            }

            std::optional<te::MonotonicBeat> whenSourceHasPlayed (te::MonotonicBeat launch,
                                                                  tracktion::BeatDuration length) noexcept WFG_AUDIO_THREAD override
            {
                const rt::ScopedRealtimeCheck goDotsOwn { rt::Region::ours };

                const auto& clock = voice.clock();
                const auto at = launch.v.inBeats();

                if (clock.isIdentityFrom (at))
                    return te::MonotonicBeat { launch.v + length };

                if (const auto when = clock.whenSourceReaches (sourceAtLaunch (at) + length.inBeats()))
                    return te::MonotonicBeat { tracktion::BeatPosition::fromBeats (*when) };

                return std::nullopt;
            }

        private:
            double sourceAtLaunch (double at) noexcept
            {
                if (std::bit_cast<std::uint64_t> (at) != std::bit_cast<std::uint64_t> (launchSeen))
                {
                    launchSeen = at;
                    sourceSeen = voice.clock().sourceAt (at);
                }

                return sourceSeen;
            }

            const RateVoice& voice;
            double launchSeen = std::numeric_limits<double>::quiet_NaN();
            double sourceSeen = 0.0;
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
            engine->getPluginManager().createBuiltInType<EqPlugin>();
            engine->getPluginManager().createBuiltInType<ProxyPlugin>();
            engine->getPluginManager().createBuiltInType<LiveInputPlugin>();
            engine->getPluginManager().createBuiltInType<LooperPlugin>();

            /*  TRACKTION'S CPU-OVERLOAD MUTE, OFF (the author, 2026-09-29).
                DeviceManager answers a block that took more than 0.98 of its
                budget by writing the NEXT block as silence without playing it
                (tracktion_DeviceManager.cpp:1376): a certain, unlogged gap, and
                everything after it a block late against the sample count the
                show is placed on. A long block is the device's to absorb or not,
                as in any host - a show over its budget has an xrun to show for
                it, not a hole Go.dot made. The mute was the "gap of one
                stretcher chunk" M47 heard at every pass of a stretched loop, and
                at a stretched range's launch (namespace draft §22.12).
                Tracktion's own test player sets it out of reach the same way. */
            engine->getDeviceManager().setCpuLimitBeforeMuting (std::numeric_limits<double>::infinity());

            auto& hosted = engine->getDeviceManager().getHostedAudioDeviceInterface();

            te::HostedAudioDeviceInterface::Parameters parameters;
            parameters.sampleRate = requested.sampleRate;
            parameters.blockSize = requested.blockSize;
            parameters.outputChannels = requested.outputChannels;

            /*  The inputs are handed over, and Tracktion is given no input
                device to take them (`describeWaveDevices`): its own path locks
                and allocates on the audio thread, so the live rack reads them
                from the tap below instead (Phase 9b, namespace draft §18.4). No
                MIDI: nothing drives it. */
            parameters.inputChannels = requested.inputChannels;
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
            scratch.setSize (std::max (requested.outputChannels, requested.inputChannels),
                             requested.blockSize, false, true, true);
            midi.ensureSize (256);

            /*  THE TAP, for every logical input the interface was opened with
                - one channel at least, so an interface with none still has a
                buffer to read silence from. */
            tapChannels = std::max (0, requested.inputChannels);
            tap.setSize (std::max (1, tapChannels), requested.blockSize, false, true, true);
            inputPeaks = std::make_unique<std::atomic<float>[]> (static_cast<std::size_t> (std::max (1, tapChannels)));

            for (int channel = 0; channel < std::max (1, tapChannels); ++channel)
                inputPeaks[static_cast<std::size_t> (channel)].store (0.0f, std::memory_order_relaxed);

            current = requested;
            sampleRate = static_cast<double> (requested.sampleRate);
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

            /*  THE PROXIES FIRST, before the Edit whose plugins hold the lanes
                they are bound to: a host unbinds its lanes as it goes. */
            proxies.clear();
            trackLanes.clear();
            proxySlots = 0;

            edit.reset();
            matrices.clear();
            eqs.clear();
            plugins.clear();
            liveInputs.clear();
            rackTracks.clear();
            rackShutAt.clear();
            voices = 0;
            handles.clear();
            slotSpeeds.clear();
            rateVoices.clear();
            context = nullptr;

            /*  THE TAKES, with no graph left to reach them (Phase 9c, §19.2). */
            refreshTakes (spec);

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

            /*  THE VOICES, AND THEN THE RACK (Phase 9b, decision CK): every rack
                channel is a track of its own after the voices, outside their
                count, so the polyphony ceiling is still `tracks` and a live
                input never competes with playback for a voice. */
            options.numAudioTracks = static_cast<std::uint32_t> (std::max (0, spec.tracks)
                                                                 + static_cast<int> (spec.rack.size()));

            /*  Tracktion's default is -3 dB on the master. A show that asked for
                0 dB and got -3 would be quietly wrong by half a level. */
            options.defaultMasterVolumedB = 0.0f;

            edit = te::Edit::createEdit (std::move (options));
            proxySlots = static_cast<int> (spec.plugins.size());
            voices = std::max (0, spec.tracks);

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
            auto trackIndex = 0;

            for (auto* track : tracks)
            {
                if (track == nullptr)
                    continue;

                const auto index = trackIndex++;

                /*  A RACK CHANNEL'S TRACK: built on its own branch below, with
                    no launcher slot and the live input stage at its head. */
                if (index >= voices)
                {
                    const auto& channel = spec.rack[static_cast<std::size_t> (index - voices)];

                    if (! buildRackTrack (*track, channel, index))
                        return false;

                    continue;
                }

                /*  Tracktion's own volume and meter plugins go. The volume one
                    takes a spin lock on the audio thread for VCA support Go.dot
                    does not use, and it is stereo-shaped - it would remap a
                    mono cue to two channels before our matrix ever saw it,
                    which is exactly the silent widening §3.9b forbids. */
                if (auto* volume = track->getVolumePlugin())
                    volume->removeFromParent();

                if (auto* meter = track->getLevelMeterPlugin())
                    meter->removeFromParent();

                /*  ONE SLOT PER RANGE, and the count is the show's.

                    Phase 2 had one: one cue per track at a time, which is what
                    the polyphony ceiling means. §3.24 gives a media cue a list
                    of ranges of one file, each a clip in a slot of its own, and
                    Go.dot places the boundary between them - so the track needs
                    as many slots as the widest cue in the show has ranges.

                    Every new slot mints an EditItemID, and that id becomes a
                    node id verbatim. Which is why the identity check is asked
                    again at every slot count a show might use, and not only at
                    every track count. */
                track->getClipSlotList().ensureNumberOfSlots (std::max (1, spec.slots));

                /*  A RESIDENT CLIP IN EVERY ONE OF THEM. It stays for the life
                    of the show; arming a cue later points it at real media.
                    Without one the slot is empty, the launcher node is not
                    built, and - because a track's output stage hangs off its
                    launcher nodes - the track is not in the graph at all.

                    All of them rather than the first, so that the graph the
                    identity check inspects and the callback cost measures is
                    the graph a show with ranges actually plays. A slot filled
                    only when a range is armed would make both of those answer
                    about a shape that never runs. */
                if (placeholder.existsAsFile())
                {
                    const auto slots = track->getClipSlotList().getClipSlots();

                    for (auto* slot : slots)
                    {
                        if (slot == nullptr)
                            continue;

                        if (auto clip = te::insertWaveClip (*slot, "resident", placeholder,
                                                            { { tracktion::TimePosition(),
                                                                tracktion::TimeDuration::fromSeconds (1.0) } },
                                                            te::DeleteExistingClips::yes))
                            makeClipPlayAtItsOwnRate (*clip);
                    }
                }

                /*  THE EQ, BEFORE THE OUTPUT STAGE (Phase 9a, decision AD): one
                    on every voice, flat until a cue says otherwise and bit-exact
                    passthrough while it is, so nothing already rendering through
                    this chain changes by a bit. Its settings are atomics a tick
                    writes; nothing about it is structural after this line. */
                auto eqPlugin = track->pluginList.insertPlugin (
                    EqPlugin::create (spec.channelsPerTrack), -1);

                auto* eqStage = dynamic_cast<EqPlugin*> (eqPlugin.get());

                if (eqStage == nullptr)
                {
                    error = "the EQ plugin would not insert";
                    edit.reset();
                    matrices.clear();
                    trackLanes.clear();
                    eqs.clear();
                    return false;
                }

                eqs.push_back (&eqStage->eq());
                trackLanes.emplace_back();

                /*  THE SET'S PROXIES, between the EQ and the output stage, in
                    plugins/order (Phase 9a, decisions AE and AF): one per entry
                    on every voice, unbound - and so untouched passthrough -
                    until its host has made the region and bound it. */
                for (int slot = 0; slot < proxySlots; ++slot)
                {
                    auto proxyPlugin = track->pluginList.insertPlugin (
                        ProxyPlugin::create (spec.channelsPerTrack, slot), -1);

                    auto* proxy = dynamic_cast<ProxyPlugin*> (proxyPlugin.get());

                    if (proxy == nullptr)
                    {
                        error = "the plugin proxy would not insert";
                        trackLanes.clear();
                        edit.reset();
                        matrices.clear();
                        eqs.clear();
                        return false;
                    }

                    trackLanes.back().push_back (&proxy->lane());
                }

                auto plugin = track->pluginList.insertPlugin (
                    CueOutputPlugin::create (spec.channelsPerTrack, current.outputChannels), -1);

                auto* output = dynamic_cast<CueOutputPlugin*> (plugin.get());

                if (output == nullptr)
                {
                    error = "the cue output plugin would not insert";
                    edit.reset();
                    matrices.clear();
                    trackLanes.clear();
                    eqs.clear();
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
                trackLanes.clear();
                eqs.clear();
                return false;
            }

            auto& transport = edit->getTransport();

            transport.ensureContextAllocated();
            transport.play (false);

            /*  ONE BEAT IS ONE SECOND, ASSERTED RATHER THAN ASSUMED.

                A launch is placed at a beat, and every time Go.dot computes one
                it divides a sample count by the sample rate and calls the answer
                beats. That is only true while a beat lasts a second, and 60 bpm
                is not on its own enough to make it so: Tracktion's default
                behaviour makes a beat's length depend on the time signature
                DENOMINATOR, so seconds-per-beat is 240 / (bpm * denominator).
                At 60 bpm in 6/8 a beat is half a second and every cue would
                launch at twice its intended distance into the future.

                Nothing in Go.dot writes a time signature today, so this holds -
                which is exactly why it is worth a check rather than a comment.
                The failure it guards against is silent and rhythmic. */
            {
                const auto& tempoSequence = edit->tempoSequence;
                const auto bpm = tempoSequence.getTempo (0)->getBpm();
                const auto denominator = tempoSequence.getTimeSig (0)->denominator.get();
                const auto beatsPerSecond = bpm * static_cast<double> (denominator) / 240.0;

                if (std::abs (beatsPerSecond - 1.0) > 1.0e-9)
                {
                    error = "the Edit's tempo does not make one beat one second, so every"
                            " launch would be placed at the wrong distance";
                    trackLanes.clear();
                    edit.reset();
                    matrices.clear();
                    plugins.clear();
                    return false;
                }
            }

            /*  RESOLVED HERE, ON THE MESSAGE THREAD, so the GO path never has to.
                See the note on `handles`. */
            editChannels = std::max (1, spec.channelsPerTrack);
            editSlots = std::max (1, spec.slots);
            context = edit->getCurrentPlaybackContext();
            handles.clear();

            /*  ONE HANDLE PER SLOT, in a flat vector indexed track-major, so
                that the GO path reaches any of them with one multiply and no
                allocation. A vector of vectors would be two indirections and a
                heap block per track for a thing whose shape never changes. */
            for (int track = 0; track < voices; ++track)
            {
                for (int slot = 0; slot < editSlots; ++slot)
                {
                    std::shared_ptr<te::LaunchHandle> handle;

                    if (auto* clip = clipOn (track, slot))
                        handle = clip->getLaunchHandle();

                    handles.push_back (std::move (handle));
                }
            }

            /*  A SPEED FOR EVERY VOICE, and every slot of it reading that speed
                (namespace draft §22.4): made here, beside the handles, because a
                handle's speed source is set once, on the message thread, before
                the graph that asks it runs. A voice starts at one, the identity,
                so until a cue says otherwise every read is the read it was. */
            rateVoices.reserve (static_cast<std::size_t> (voices));
            slotSpeeds.reserve (handles.size());

            for (int track = 0; track < voices; ++track)
                rateVoices.push_back (std::make_unique<RateVoice>());

            for (std::size_t at = 0; at < handles.size(); ++at)
            {
                const auto track = at / static_cast<std::size_t> (editSlots);
                slotSpeeds.push_back (std::make_unique<SlotSpeed> (*rateVoices[track]));

                if (handles[at] != nullptr)
                    handles[at]->setSpeedSource (slotSpeeds.back().get());
            }

            /*  THE FASTEST A STRETCHED CUE CAN GO at this graph's rate: the
                stretcher takes 256 x speed frames a chunk into a buffer as long
                as its latency (tracktion_TimeStretch.cpp:937-945), so the limit
                is that latency over 256, a frame short. */
            stretchLimit.store (static_cast<double> (te::TimeStretcher::getLatencySamplesForMode (
                                    te::TimeStretcher::signalsmithDefault, sampleRate, true) - 1) / 256.0,
                                std::memory_order_relaxed);

            beatOffset.store (0.0, std::memory_order_relaxed);
            anchorSample.store (0, std::memory_order_relaxed);
            referenceSkew.store (0, std::memory_order_relaxed);

            /*  THE CHILDREN, beside the transport that already plays (§17.6):
                one host per entry, handed every voice's lane for its slot.
                Here, on the message thread, and never on the GO path; a child
                that will not start is a failed entry in the table, not a
                failed show. */
            startProxies (spec);

            error.clear();
            return true;
        }

        void startProxies (const EditSpec& spec)
        {
            proxies.clear();

            /*  WHICH ENTRY IS WHICH SLOT, for as long as this graph stands
                (2026-09-26): the runner sends a cue's inserts by it, and the
                tree says which entries of the set the graph does not hold. */
            if (services.table != nullptr)
            {
                std::vector<std::string> ids;

                for (const auto& entry : spec.plugins)
                    ids.push_back (entry.id);

                services.table->setBuilt (std::move (ids));
                toldTable = true;
            }

            /*  THE RACK'S CHAINS, as the graph has them, for the runner to send
                a mic cue's inserts by (Phase 9b). */
            if (services.table != nullptr)
            {
                std::map<std::string, std::vector<std::string>> rackIds;

                for (const auto& channel : spec.rack)
                {
                    auto& ids = rackIds[channel.id];

                    for (const auto& entry : channel.plugins)
                        ids.push_back (entry.id);
                }

                services.table->setBuiltRack (std::move (rackIds));

                std::map<std::string, plugin::PluginTable::BuiltTake> built;

                for (const auto& channel : spec.rack)
                {
                    const auto found = takes.find (channel.id);

                    if (found == takes.end() || found->second.take == nullptr)
                        continue;

                    auto& each = built[channel.id];
                    each.seconds = found->second.shape.takeSeconds;
                    each.layers = found->second.shape.layers;
                    each.bytes = static_cast<std::uint64_t> (found->second.take->bytes());
                    each.problem = found->second.problem;

                    for (const auto& entry : channel.plugins)
                        if (entry.beforeRecorder)
                            each.before.push_back (entry.id);
                }

                services.table->setBuiltTakes (std::move (built));
            }

            /*  No voices, nothing of the set's to host: its entries stay
                `unloaded`, which is the truth about a show with plugins and no
                tracks. The rack's channels are tracks of their own. */
            if (voices <= 0)
            {
                startRackProxies (spec);
                return;
            }

            for (int slot = 0; slot < proxySlots; ++slot)
            {
                const auto& entry = spec.plugins[static_cast<std::size_t> (slot)];

                auto proxySpec = proxySpecFor (entry, spec, voices, editChannels);

                std::vector<plugin::ProxyLane*> slotLanes;
                slotLanes.reserve (static_cast<std::size_t> (voices));

                for (int track = 0; track < voices; ++track)
                    slotLanes.push_back (trackLanes[static_cast<std::size_t> (track)][static_cast<std::size_t> (slot)]);

                auto host = std::make_unique<plugin::ProxyHost> (std::move (proxySpec), std::move (slotLanes),
                                                                 services.table);

                if (services.onFailed)
                    host->onFailed (services.onFailed);

                if (services.onChanged)
                    host->onChanged (services.onChanged);

                /*  Started only when somebody gave the host a table to write:
                    a test rig with no services builds the graph and no child. */
                if (services.table != nullptr)
                {
                    std::string problem;
                    host->start (problem);
                }

                proxies.push_back (std::move (host));
            }

            startRackProxies (spec);
        }

        /*  WHAT A CHILD IS STARTED FROM, for an entry of the set or of the
            rack alike: the plugin, its preset, how many lanes and how wide.

            THE DESCRIPTION, off this machine's scan (PR 9a.7): what the child
            makes the plugin from. An identifier the scan does not know leaves
            it empty, and the host reads `missing` - and asks again at its next
            start, since a scan may find it. */
        plugin::ProxySpec proxySpecFor (const PluginSpec& entry, const EditSpec& spec,
                                        int laneCount, int laneChannels)
        {
            plugin::ProxySpec proxySpec;
            proxySpec.pluginId = entry.id;
            proxySpec.identifier = entry.identifier;
            proxySpec.name = entry.name;
            proxySpec.presetPath = entry.presetPath;
            proxySpec.lanes = laneCount;
            proxySpec.channels = laneChannels;
            proxySpec.maxSamples = current.blockSize;
            proxySpec.sampleRate = current.sampleRate;
            proxySpec.deadlineMicroseconds = spec.proxyDeadlineMicroseconds;
            proxySpec.regionFolder = storageFolder.getChildFile ("proxy").getFullPathName().toStdString();
            proxySpec.launch = services.launch;
            proxySpec.catalogues = services.catalogues;
            proxySpec.describe = services.describe;

            if (! plugin::Catalogue::isTestIdentifier (entry.identifier))
            {
                if (services.describe)
                    proxySpec.descriptionXml = services.describe (entry.identifier);
                else if (const auto description = engine->getPluginManager().knownPluginList
                                                        .getTypeForIdentifierString (juce::String (entry.identifier)))
                    if (const auto xml = description->createXml())
                        proxySpec.descriptionXml = xml->toString().toStdString();
            }

            return proxySpec;
        }

        /*  ONE RACK CHANNEL'S TRACK (Phase 9b, namespace draft §18.4). No
            launcher slot - its sound is the live input - and a chain of its own:
            the input stage bound to the tap, the EQ, a proxy for each of the
            channel's plugins in order, and the output stage, routed once to the
            one wide device as a voice is. Two channels wide whatever its class:
            a mono input fills the first, and the routing reads the width after
            the chain, as a voice's does. The track stays in the graph with no
            clip because the input stage says it makes sound with no input. */
        bool buildRackTrack (te::AudioTrack& track, const RackChannelSpec& channel, int index)
        {
            constexpr int rackChannels = 2;

            if (auto* volume = track.getVolumePlugin())
                volume->removeFromParent();

            if (auto* meter = track.getLevelMeterPlugin())
                meter->removeFromParent();

            const auto fail = [this] (const char* why)
            {
                error = why;
                edit.reset();
                matrices.clear();
                trackLanes.clear();
                eqs.clear();
                liveInputs.clear();
                return false;
            };

            auto inputPlugin = track.pluginList.insertPlugin (LiveInputPlugin::create (rackChannels), -1);
            auto* stage = dynamic_cast<LiveInputPlugin*> (inputPlugin.get());

            if (stage == nullptr)
                return fail ("the live input stage would not insert");

            stage->bindTap (&tapView);
            liveInputs.push_back (stage);

            /*  A SAMPLING CHANNEL (Phase 9c, namespace draft §19.2): the
                plugins before the recorder, the recorder, then the EQ and the
                plugins after the player - so what comes before is printed into
                the take and what comes after is heard as it loops. With no
                recorder a plugin's side means nothing and the chain is 9b's:
                the EQ, then every plugin. A lane keeps its place in the
                channel's own order whichever side it is built on, since a mic
                cue's inserts are sent by that place. */
            const auto take = channel.takeSeconds > 0.0 ? takeFor (channel.id) : nullptr;
            trackLanes.emplace_back (channel.plugins.size(), nullptr);

            const auto insertProxies = [&] (bool beforeTheRecorder)
            {
                for (std::size_t slot = 0; slot < channel.plugins.size(); ++slot)
                {
                    if ((take != nullptr && channel.plugins[slot].beforeRecorder) != beforeTheRecorder)
                        continue;

                    auto proxyPlugin = track.pluginList.insertPlugin (
                        ProxyPlugin::create (rackChannels, static_cast<int> (slot)), -1);
                    auto* proxy = dynamic_cast<ProxyPlugin*> (proxyPlugin.get());

                    if (proxy == nullptr)
                        return false;

                    trackLanes.back()[slot] = &proxy->lane();
                }

                return true;
            };

            if (take != nullptr)
            {
                if (! insertProxies (true))
                    return fail ("the plugin proxy would not insert on a rack channel");

                auto recorderPlugin = track.pluginList.insertPlugin (LooperPlugin::create (rackChannels), -1);
                auto* recorder = dynamic_cast<LooperPlugin*> (recorderPlugin.get());

                if (recorder == nullptr)
                    return fail ("the recorder would not insert on a sampling channel");

                recorder->bind (take.get(), &tapView);

                const std::lock_guard<std::mutex> lock { takesLock };
                takeTracks[index] = channel.id;
            }

            auto eqPlugin = track.pluginList.insertPlugin (EqPlugin::create (rackChannels), -1);
            auto* eqStage = dynamic_cast<EqPlugin*> (eqPlugin.get());

            if (eqStage == nullptr)
                return fail ("the EQ plugin would not insert on a rack channel");

            eqs.push_back (&eqStage->eq());

            if (! insertProxies (false))
                return fail ("the plugin proxy would not insert on a rack channel");

            auto outputPlugin = track.pluginList.insertPlugin (
                CueOutputPlugin::create (rackChannels, current.outputChannels), -1);
            auto* output = dynamic_cast<CueOutputPlugin*> (outputPlugin.get());

            if (output == nullptr)
                return fail ("the output stage would not insert on a rack channel");

            matrices.push_back (&output->matrix());
            plugins.push_back (output);

            if (auto& manager = engine->getDeviceManager(); manager.getNumWaveOutDevices() > 0)
                if (auto* wide = manager.getWaveOutDevice (0))
                    track.getOutput().setOutputToDeviceID (wide->getDeviceID());

            rackTracks[channel.id] = index;
            rackShutAt.push_back (-1);
            return true;
        }

        /*  THE RACK'S CHILDREN (Phase 9b, decision CL): one child per distinct
            plugin and preset across every channel, a lane for each channel that
            has it - a child spins a core while any of its lanes is switched in,
            so the number spinning is the number of distinct plugins in use and
            not the number of channel slots. Grouped in the order the channels
            and their chains are read, so the same show builds the same children.
            After the set's in `proxies`, so a set slot is still its index there. */
        void startRackProxies (const EditSpec& spec)
        {
            struct Group
            {
                const PluginSpec* entry = nullptr;
                std::vector<std::string> alsoIds, laneWords, channelNames;
                std::vector<plugin::ProxyLane*> groupLanes;
            };

            std::vector<Group> groups;

            for (std::size_t at = 0; at < spec.rack.size(); ++at)
            {
                const auto& channel = spec.rack[at];
                const auto track = static_cast<std::size_t> (voices) + at;

                if (track >= trackLanes.size())
                    continue;

                const auto called = channel.name.empty() ? channel.id : channel.name;

                for (std::size_t slot = 0; slot < channel.plugins.size() && slot < trackLanes[track].size(); ++slot)
                {
                    const auto& entry = channel.plugins[slot];

                    auto group = std::find_if (groups.begin(), groups.end(), [&entry] (const Group& candidate)
                    {
                        return candidate.entry->identifier == entry.identifier
                                 && candidate.entry->presetPath == entry.presetPath;
                    });

                    if (group == groups.end())
                    {
                        groups.push_back ({});
                        group = std::prev (groups.end());
                        group->entry = &entry;
                    }
                    else
                    {
                        group->alsoIds.push_back (entry.id);
                    }

                    group->groupLanes.push_back (trackLanes[track][slot]);
                    group->laneWords.push_back ("channel " + called);

                    if (std::find (group->channelNames.begin(), group->channelNames.end(), called)
                          == group->channelNames.end())
                        group->channelNames.push_back (called);
                }
            }

            for (auto& group : groups)
            {
                auto proxySpec = proxySpecFor (*group.entry, spec,
                                               static_cast<int> (group.groupLanes.size()), 2);
                proxySpec.alsoIds = group.alsoIds;
                proxySpec.laneWords = group.laneWords;

                std::string names;

                for (std::size_t at = 0; at < group.channelNames.size(); ++at)
                    names += (at == 0 ? "" : at + 1 == group.channelNames.size() ? " and " : ", ")
                             + group.channelNames[at];

                proxySpec.silentWords = names + (group.channelNames.size() == 1 ? " is" : " are")
                                      + " silent until it is back";

                auto host = std::make_unique<plugin::ProxyHost> (std::move (proxySpec),
                                                                 std::move (group.groupLanes),
                                                                 services.table);

                if (services.onFailed)
                    host->onFailed (services.onFailed);

                if (services.onChanged)
                    host->onChanged (services.onChanged);

                if (services.table != nullptr)
                {
                    std::string problem;
                    host->start (problem);
                }

                proxies.push_back (std::move (host));
            }
        }

        /*  THE TAKE STORE (Phase 9c, namespace draft §19.2), refreshed each
            time a graph is built and never emptied by one: a sampling channel
            whose rate, width, longest take and layers are what they were keeps
            its take, and one whose shape changed is set aside afresh - which
            empties it, and a sentence says so. A channel no longer sampling,
            or gone, lets its take go. Message thread, with the old Edit gone,
            so nothing still plays the takes this lets go of. */
        void refreshTakes (const EditSpec& spec)
        {
            /*  BUILT BESIDE THE STORE AND SWAPPED IN UNDER THE LOCK: setting a
                minute aside and touching it is tens of milliseconds, which the
                tick thread must not wait behind. Read here without the lock,
                since this thread is the store's only writer. */
            std::map<std::string, TakeStore> kept;

            for (const auto& channel : spec.rack)
            {
                if (channel.takeSeconds <= 0.0)
                    continue;

                Looper::Shape shape;
                shape.sampleRate = static_cast<double> (current.sampleRate);
                shape.channels = 2;
                shape.takeSeconds = channel.takeSeconds;
                shape.layers = channel.layers;

                auto found = takes.find (channel.id);

                if (found != takes.end() && found->second.take != nullptr
                      && std::abs (found->second.shape.sampleRate - shape.sampleRate) < 0.5
                      && std::abs (found->second.shape.takeSeconds - shape.takeSeconds) < 1.0e-9
                      && found->second.shape.layers == shape.layers)
                {
                    auto same = found->second;
                    same.problem.clear();
                    kept[channel.id] = std::move (same);
                    continue;
                }

                TakeStore store;
                store.shape = shape;
                store.take = std::make_shared<Looper>();
                store.take->prepare (shape);

                if (found != takes.end() && found->second.take != nullptr
                      && found->second.take->state() != TakeState::empty)
                    store.problem = std::abs (found->second.shape.sampleRate - shape.sampleRate) >= 0.5
                                        ? "the take was cleared: the interface's rate changed"
                                        : "the take was cleared: its longest take or its layers changed";

                kept[channel.id] = std::move (store);
            }

            {
                const std::lock_guard<std::mutex> lock { takesLock };
                takes.swap (kept);
                takeTracks.clear();
            }

            //  The stores let go of go here, outside the lock.
        }

        std::shared_ptr<Looper> takeFor (const std::string& channelId)
        {
            const std::lock_guard<std::mutex> lock { takesLock };
            const auto found = takes.find (channelId);
            return found != takes.end() ? found->second.take : nullptr;
        }

        bool keepTake (const std::string& channelId, const std::string& stem, const std::string& mediaFolder)
        {
            auto take = takeFor (channelId);

            if (take == nullptr)
                return false;

            /*  THE WRITER BESIDE THE TAKES, made with the first Keep: a show
                that never keeps a take starts no thread for it. */
            if (takeWriter == nullptr)
                takeWriter = std::make_unique<TakeWriter>();

            takeWriter->queue ({ channelId, stem, mediaFolder, std::move (take) });
            return true;
        }

        std::vector<TakeWriter::Done> keptTakes()
        {
            return takeWriter != nullptr ? takeWriter->finished() : std::vector<TakeWriter::Done> {};
        }

        std::vector<std::pair<std::string, std::shared_ptr<const Looper>>> allTakes()
        {
            std::vector<std::pair<std::string, std::shared_ptr<const Looper>>> out;
            const std::lock_guard<std::mutex> lock { takesLock };

            for (const auto& [channelId, store] : takes)
                if (store.take != nullptr)
                    out.emplace_back (channelId, store.take);

            return out;
        }

        std::shared_ptr<Looper> takeForTrack (int trackIndex)
        {
            const std::lock_guard<std::mutex> lock { takesLock };
            const auto channel = takeTracks.find (trackIndex);

            if (channel == takeTracks.end())
                return nullptr;

            const auto found = takes.find (channel->second);
            return found != takes.end() ? found->second.take : nullptr;
        }

        /** A rack track's input stage, or null for a voice or no such track. */
        LiveInputPlugin* liveInputOf (int trackIndex) const noexcept
        {
            const auto rack = trackIndex - voices;

            if (trackIndex < 0 || rack < 0 || rack >= static_cast<int> (liveInputs.size()))
                return nullptr;

            return liveInputs[static_cast<std::size_t> (rack)];
        }

        void stop()
        {
            proxies.clear();
            trackLanes.clear();
            liveInputs.clear();
            rackTracks.clear();
            rackShutAt.clear();
            voices = 0;
            proxySlots = 0;

            /*  ONLY WHEN IT TOLD THE TABLE SOMETHING: `stop` runs again from
                the destructor, and a table that went first - a test's, declared
                after the host - must not be touched then (the macOS job aborted
                on the destroyed mutex, 2026-09-26). The proxies go by the same
                rule, being gone after the first stop. */
            if (toldTable && services.table != nullptr)
                services.table->clearBuilt();

            toldTable = false;

            edit.reset();
            matrices.clear();
            eqs.clear();
            plugins.clear();
            handles.clear();

            /*  After the Edit, whose graph could still ask a slot about its
                speed while it was torn down. */
            slotSpeeds.clear();
            rateVoices.clear();
            stretchLimit.store (0.0, std::memory_order_relaxed);
            context = nullptr;

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

        void processBlock (const float* const* inputs = nullptr, int numInputs = 0)
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
            for (int channel = 0; channel < std::min (numInputs, current.inputChannels); ++channel)
                if (inputs != nullptr && inputs[channel] != nullptr)
                    scratch.copyFrom (channel, 0, inputs[channel], current.blockSize);
            midi.clear();

            /*  Where this block starts in Go.dot's count, for the rack's gates
                to place an open against; and the tap the stages read. */
            tapView.buffer = &tap;
            tapView.channels = tapChannels;
            tapView.blockStart = samples.samplesElapsed();

            /*  THE TAP, filled before Tracktion runs (namespace draft §18.4):
                the rack's input stage reads it during the call below, in this
                same block, so a live input adds no block of delay. Every
                logical input is written - its samples, or zeros where this
                block brought none - and its peak kept for the tick. */
            for (int channel = 0; channel < tapChannels; ++channel)
            {
                if (inputs != nullptr && channel < numInputs && inputs[channel] != nullptr)
                {
                    tap.copyFrom (channel, 0, inputs[channel], current.blockSize);

                    const auto peak = tap.getMagnitude (channel, 0, current.blockSize);
                    auto& held = inputPeaks[static_cast<std::size_t> (channel)];

                    if (peak > held.load (std::memory_order_relaxed))
                        held.store (peak, std::memory_order_relaxed);
                }
                else
                {
                    tap.clear (channel, 0, current.blockSize);
                }
            }

            /*  EVERY VOICE'S SPEED, brought up to date before the graph reads
                it (namespace draft §22.4): the breakpoints the tick thread placed
                since the last block go into each voice's clock, and what is
                before this block is let go. Ours, and allocation-free: a fixed
                queue into a fixed clock. */
            {
                const auto blockStartBeat = beatsAtSample (samples.samplesElapsed());

                for (auto& voice : rateVoices)
                    voice->drain (blockStartBeat);
            }

            {
                const rt::ScopedRealtimeCheck tracktionsBlock { rt::Region::foreign };

                engine->getDeviceManager().getHostedAudioDeviceInterface().processBlock (scratch, midi);
            }

            if (sink != nullptr)
                sink->blockProduced (scratch.getArrayOfReadPointers(),
                                     current.outputChannels, current.blockSize);

            /*  After the graph has run, not before. A reader that saw the new
                sample count would otherwise be told the block had happened
                while it was still happening. */
            samples.advance (current.blockSize);
            blocks.fetch_add (1, std::memory_order_relaxed);

            /*  THE ANCHOR, published here and nowhere else, because here is the
                only place the two numbers describe the same instant. Read from
                the tick thread anywhere is not the same thing: the sync range
                is published BEFORE the graph runs and Go.dot's counter advances
                AFTER it returns, so a reader that catches the gap gets a
                different answer from one that does not.

                Allocation-free, lock-free and syscall-free (PRD §4.2): the
                seqlock behind getSyncPoint is being read on the same thread that
                wrote it, so it cannot spin, and the three stores are relaxed. */
            if (context != nullptr)
            {
                if (const auto syncPoint = context->getSyncPoint())
                {
                    const auto elapsed = samples.samplesElapsed();
                    const auto beats = syncPoint->monotonicBeat.v.inBeats();

                    beatOffset.store (beats - static_cast<double> (elapsed) / sampleRate,
                                      std::memory_order_relaxed);
                    anchorSample.store (elapsed, std::memory_order_relaxed);
                    referenceSkew.store (syncPoint->referenceSamplePosition - elapsed,
                                         std::memory_order_relaxed);
                }
            }
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

        te::WaveAudioClip* clipOn (int trackIndex, int slotIndex = 0) const
        {
            if (edit == nullptr || slotIndex < 0)
                return nullptr;

            const auto tracks = te::getAudioTracks (*edit);

            if (trackIndex < 0 || trackIndex >= tracks.size())
                return nullptr;

            auto* track = tracks[trackIndex];

            if (track == nullptr)
                return nullptr;

            const auto slots = track->getClipSlotList().getClipSlots();

            if (slotIndex >= slots.size() || slots[slotIndex] == nullptr)
                return nullptr;

            return dynamic_cast<te::WaveAudioClip*> (slots[slotIndex]->getClip());
        }

        /*  Where a (track, slot) sits in the flat handle cache, or a size that
            fails every bounds check when there is no such pair. Not an optional
            because the GO path branches on it once, on a size comparison it was
            going to make anyway. */
        std::size_t handleIndex (int trackIndex, int slotIndex) const noexcept
        {
            if (trackIndex < 0 || slotIndex < 0 || slotIndex >= editSlots)
                return handles.size();

            return static_cast<std::size_t> (trackIndex) * static_cast<std::size_t> (editSlots)
                     + static_cast<std::size_t> (slotIndex);
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
        static void makeClipPlayAtItsOwnRate (te::WaveAudioClip& clip, bool stretch = false)
        {
            const auto seconds = clip.getSourceLength().inSeconds();

            if (seconds <= 0.0)
                return;

            auto info = clip.getLoopInfo();
            info.setNumBeats (seconds);          // 60 bpm: one beat, one second
            clip.setLoopInfo (info);

            clip.setAutoPitch (false);
            clip.setSpeedRatio (1.0);

            /*  THE MODE IS NAMED, NEVER INHERITED, and it is the cue's
                `rateMode` (namespace draft §22.2): `disabled` is the resampler -
                varispeed, the engine behaviour above sees to that - and
                Signalsmith's default preset is timestretch (decision DZ). It is
                written on every arm because Tracktion gives a clip created from
                a loopable file a stretch mode of its own
                (tracktion_ClipOwner.cpp:291-297). A mode is on Tracktion's
                restart list, which is why it changes at the arm and never under
                a sounding cue (DV); writing the value a clip already holds
                changes nothing and rebuilds nothing. */
            clip.setTimeStretchMode (stretch ? te::TimeStretcher::signalsmithDefault
                                             : te::TimeStretcher::disabled);
        }

        /*  Points one slot's clip at one file, whole - the Phase 2 arm, now
            told which slot. Everything a RANGE needs on top of this is in
            armRangeInto below; this stays the plain case because a cue with no
            ranges is still most cues.

            It does not dispatch: the caller does, once, so that arming eight
            slots is one round of pending updates rather than eight. */
        bool pointSlotAtFile (int trackIndex, int slotIndex, const juce::File& file,
                              double startOffset = 0.0, bool stretch = false)
        {
            auto* clip = clipOn (trackIndex, slotIndex);

            if (clip == nullptr || engine == nullptr || ! file.existsAsFile())
                return false;

            /*  A FILE THAT IS THERE AND IS NOT AUDIO IS A FAILED ARM, asked with
                the very predicate Tracktion builds the graph with. Since the
                pin of 2026-09-24, createNodeForAudioClip builds NO node for a
                clip whose playback file is not valid (tracktion_EditNodeBuilder
                .cpp:422-426): a slot pointed at one loses its two nodes at the
                next rebuild - measured, 18 to 16 on one track - and its launch
                handle then has nothing behind it. The track itself stays, held
                in the graph by CueOutputPlugin's producesAudioWhenNoAudioInput.
                Before that pin the slot stayed and played silence. Either way
                the run then waited for ever on isArmReady, which asks this same
                question and never gets a yes - a GO that sounded nothing and
                said nothing.
                Refused here, the slot keeps what it held and the run fails
                while the operator is still reading the next line. */
            if (! te::AudioFile { *engine, file }.isValid())
            {
                error = "\"" + file.getFileName().toStdString()
                          + "\" is there and is not audio this build can decode";
                return false;
            }

            clip->getSourceFileReference().setToFile (file, te::SourceFileReference::PathStyle::alwaysAbsolute, false);

            /*  The new file's own loop info, not the placeholder's. Everything
                below reads a length off the clip, and until this runs those
                lengths still describe the file that was there before.

                IT DESCRIBES THE WHOLE FILE even when an offset means only part
                of it will sound. `numBeats` is what makes auto-tempo 1:1 at
                60 bpm, and it is read against the SOURCE's own length - set it
                to the shortened length and the file's declared tempo rises
                above sixty, Tracktion stretches it, and a cue starting two
                seconds in would also play back fast. */
            makeClipPlayAtItsOwnRate (*clip, stretch);

            const auto sourceSeconds = clip->getSourceLength().inSeconds();

            /*  AN OFFSET PAST THE END IS A FAILED ARM, asked here for the same
                reason a range is (`armRangeInto` below): the document could not
                have known how long the file is, because the file arrives on a
                different machine from the one the show was written on. */
            if (startOffset > 0.0 && startOffset >= sourceSeconds)
            {
                const auto seconds = [] (double value)
                {
                    return juce::String (value, 3).toStdString();
                };

                error = "a start offset of " + seconds (startOffset) + " seconds is not inside \""
                          + file.getFileName().toStdString() + "\", which is "
                          + seconds (sourceSeconds) + " seconds long";
                return false;
            }

            /*  Looping is off before anything else touches position: turning it
                off rewrites the clip's offset, so doing it afterwards would
                silently discard whatever was set. */
            clip->disableLooping();

            /*  THE LENGTH IS WHAT WILL SOUND, which is the file less whatever
                the offset skips. Leaving it at the source's length would give a
                cue that played to the end of the file and then `startOffset`
                seconds of nothing - with the launch handle still reporting that
                it was playing, and `Runner::observeEdges` waiting on a
                stopped edge that arrives late by exactly the offset. */
            clip->setLength (tracktion::TimeDuration::fromSeconds (
                                 sourceSeconds - std::max (0.0, startOffset)), false);

            /*  AND THE OFFSET LAST, after both of the calls that rewrite it.
                `disableLooping` assigns one outright, and `setLength` with
                `preserveSync` false subtracts the change in length from it -
                which here is a change of two seconds or more, since the
                resident clip was created one second long against the silent
                placeholder. An offset set before either call comes out
                negative, `Clip::setOffset` clamps it to nought, and the cue
                plays from the top with nothing reported. That is the same
                silent-wrong shape as the beat-count bug above, and this is the
                order that avoids it. */
            if (startOffset > 0.0)
                clip->setOffset (tracktion::TimeDuration::fromSeconds (startOffset));

            return true;
        }

        /*  One range into one slot: the file, and then the section of it, armed
            LOOPING so that the launcher builds no stop duration for it.

            WHY LOOPING IS THE MECHANISM AND NOT A SETTING. SlotControlNode
            captures a stop duration when the graph is built - the clip's length
            in beats when `isLooping()` is false, nothing at all when it is
            (tracktion_EditNodeBuilder.cpp:1031-1032) - and every block, before
            it advances, it queues a stop for the block containing that duration
            (tracktion_SlotControlNode.cpp:134-153). So a clip armed NOT looping
            can never be made to loop afterwards: LaunchHandle::setLooping is a
            rebuild-free store, but the queued stop pre-empts the wrap.

            Armed looping, the section repeats for ever inside WaveNodeRealTime
            with no click suppressor at the boundary, and Go.dot ends it by
            placing a stop at a sample it computed. Which is the arrangement
            §3.24 describes: Go.dot places every boundary.

            At 60 bpm one beat is one second, which is what makes the loop range
            in beats the same number as the range in seconds. */
        bool armRangeInto (int trackIndex, int slotIndex, const juce::File& file,
                           const AudioHost::RangeSpec& range, bool stretch = false,
                           double intoLoop = 0.0)
        {
            if (! pointSlotAtFile (trackIndex, slotIndex, file, 0.0, stretch))
                return false;

            auto* clip = clipOn (trackIndex, slotIndex);

            if (clip == nullptr)
                return false;

            const auto length = clip->getSourceLength().inSeconds();

            /*  A RANGE PAST THE END OF THE FILE IS A FAILED ARM, and this is
                where the file is finally open to be asked. The document could
                not have known: a show is authored on one machine and its media
                copied onto another, and refusing the load would have made a
                sound that had not arrived yet into a show nobody could work on. */
            if (! (range.out > range.in) || range.in < 0.0 || range.out > length + 1.0e-6)
            {
                /*  juce::String rather than the canonical formatter: this is
                    a sentence for a person, and the audio layer names nothing
                    from the osc layer. */
                const auto seconds = [] (double value)
                {
                    return juce::String (value, 3).toStdString();
                };

                error = "a range of " + seconds (range.in) + " to " + seconds (range.out)
                          + " seconds is not inside \"" + file.getFileName().toStdString()
                          + "\", which is " + seconds (length) + " seconds long";
                return false;
            }

            clip->setLoopRangeBeats ({ tracktion::BeatPosition::fromBeats (range.in),
                                       tracktion::BeatPosition::fromBeats (range.out) });

            /*  PART-WAY INTO ITS LOOP (2026-10-02, K8's review): a bed Esc paused
                inside a looping slice carries on at the same point of it. The
                clip's own offset does it, and the reader wraps it with the loop -
                it reads the loop's start plus the offset, modulo the loop's
                length - so the first pass starts there and every pass after it
                at the in-point. Set after the loop range, which leaves an offset
                alone, and only when there is one: every other slice's arm is
                what it was. */
            if (intoLoop > 0.0)
                clip->setOffset (tracktion::TimeDuration::fromSeconds (
                                     std::fmod (intoLoop, range.out - range.in)));

            return clip->isLooping();
        }

        bool setTrackSource (int trackIndex, int slotIndex, const std::string& mediaFile,
                             bool stretch = false)
        {
            const juce::File file { juce::String (mediaFile) };

            if (! pointSlotAtFile (trackIndex, slotIndex, file, 0.0, stretch))
                return false;

            edit->dispatchPendingUpdatesSynchronously();
            return true;
        }

        bool setTrackRanges (int trackIndex, const std::string& mediaFile,
                             const std::vector<AudioHost::RangeSpec>& ranges,
                             double startOffset, bool stretch, int startSlot, double sliceOffset)
        {
            const juce::File file { juce::String (mediaFile) };

            if (edit == nullptr || ! file.existsAsFile())
            {
                error = "there is no graph, or \"" + mediaFile + "\" is not a file";
                return false;
            }

            /*  NO SLOT, and it is a refusal rather than a truncation. The slot
                count is fixed when the graph is built (§3.25), so a cue that
                grew a ninth range during a show has nowhere to arm it - and
                arming the first eight would be a cue that plays most of what it
                says, which is worse than one that says it cannot. */
            if (static_cast<int> (ranges.size()) > editSlots)
            {
                error = "no-slot: this cue has " + std::to_string (ranges.size())
                          + " ranges and the graph was built with " + std::to_string (editSlots)
                          + " slots a track, which is fixed until the show is reloaded";
                return false;
            }

            const auto placeholder = ensureSilentPlaceholder (editChannels);

            /*  ONE REBUILD FOR THE LOT, and it is the dispatch at the end that
                does it rather than any scoped object.

                Every write below is on Tracktion's restart list, and each one
                calls Edit::restartPlayback - which sets a BOOL and starts a
                timer. Eight writes therefore set one flag eight times, and the
                single synchronous dispatch afterwards turns it into one graph
                rebuild. Nothing has to be inhibited for that to be true.

                THE THING THAT LOOKED RIGHT AND CRASHED: Edit::ScopedRenderStatus
                is the obvious "batch these" object and it calls
                freePlaybackContext() on construction (tracktion_Edit.cpp:793-
                800). It destroys the running playback context - which AudioHost
                caches, and which the audio thread reads every block - and builds
                a different one when it goes out of scope. Arming a cue is not a
                render, and taking a show's playback context away mid-block is a
                segmentation fault, which is how this was found.

                TransportControl::ReallocationInhibitor is the safe one of the
                two, and it is not needed either: it defers the rebuild to the
                transport's own JUCE timer, which is one more thing that has to
                run before a cue can sound. */
            bool armed = true;

            for (int slot = 0; slot < editSlots; ++slot)
            {
                if (slot < static_cast<int> (ranges.size()))
                {
                    armed = armRangeInto (trackIndex, slot, file,
                                          ranges[static_cast<std::size_t> (slot)], stretch,
                                          slot == startSlot ? sliceOffset : 0.0) && armed;
                    continue;
                }

                /*  BACK ONTO THE PLACEHOLDER, because a voice is reused. A slot
                    still holding the last cue's third range would sound if
                    anything ever launched it, and the thing that eventually
                    launches it is a bug in a later phase rather than never. */
                /*  THE ONE PLACE A START OFFSET CAN APPLY, and the document
                    guarantees it: `validate()` refuses a `startOffset` beside a
                    `Range`, because a cue with ranges plays its ranges and the
                    offset belongs in the first one's `in`. So the whole-file
                    arm is the only branch that takes it. */
                if (ranges.empty() && slot == 0)
                {
                    armed = pointSlotAtFile (trackIndex, 0, file, startOffset, stretch) && armed;
                    continue;
                }

                /*  Never the placeholder: a silent one-second clip has nothing
                    to start two seconds into, and an offset would fail its own
                    past-the-end check. */
                pointSlotAtFile (trackIndex, slot, placeholder);
            }

            edit->dispatchPendingUpdatesSynchronously();

            return armed;
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
        bool isSlotSourceReady (int trackIndex, int slotIndex) const
        {
            auto* clip = clipOn (trackIndex, slotIndex);

            if (clip == nullptr || engine == nullptr)
                return false;

            const te::AudioFile file { *engine, clip->getSourceFileReference().getFile() };

            if (! file.isValid())
                return false;

            return engine->getAudioFileManager().cache.hasMappedReader (file, 0);
        }

        /*  EVERY SLOT OF THE TRACK, because a ranged cue is not ready until
            every range it might enter is. A cue that reported itself ready with
            its second range unmapped would play its first range perfectly and
            then go silent at the boundary - which is the failure this whole
            wait exists to prevent, moved four seconds later.

            The slots past the cue's ranges hold the silent placeholder, which
            is a real file and maps once for the life of the show. */
        bool isTrackSourceReady (int trackIndex) const
        {
            for (int slot = 0; slot < editSlots; ++slot)
                if (! isSlotSourceReady (trackIndex, slot))
                    return false;

            return true;
        }

        bool isTrackStretched (int trackIndex, int slotIndex) const
        {
            const auto* clip = clipOn (trackIndex, slotIndex);

            return clip != nullptr
                     && clip->getActualTimeStretchMode() != te::TimeStretcher::disabled;
        }

        void setTrackRouting (int trackIndex, double levelDb,
                              const std::vector<std::array<double, 3>>& coefficients)
        {
            if (trackIndex < 0 || trackIndex >= static_cast<int> (matrices.size()))
                return;

            auto* matrix = matrices[static_cast<std::size_t> (trackIndex)];

            if (matrix == nullptr)
                return;

            /*  Cleared first, because a track is reused. Whatever the last cue
                on this voice was routed to would otherwise still be there, and
                a cue would play out of a speaker belonging to the one before
                it - which is the kind of fault nobody finds in rehearsal
                because it only happens on the second GO. */
            for (int input = 0; input < matrix->numInputs(); ++input)
                for (int output = 0; output < matrix->numOutputs(); ++output)
                    matrix->setGain (input, output, 0.0f);

            for (const auto& coefficient : coefficients)
                matrix->setGain (static_cast<int> (coefficient[0]),
                                 static_cast<int> (coefficient[1]),
                                 static_cast<float> (coefficient[2]));

            matrix->setLevelDb (static_cast<float> (levelDb));

            /*  SNAPPED, NOT SLEWED. This is an arm, which happens while the
                voice is silent; sliding the coefficients up from whatever the
                previous cue left would be a fade nobody asked for, and at the
                wrong moment. A fade is Phase 3's, and it writes the same
                atomics while the sound is running. */
            matrix->snapToTargets();
        }

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

        double beatsAtSample (std::int64_t sample) const noexcept
        {
            if (sampleRate <= 0.0)
                return 0.0;

            return beatOffset.load (std::memory_order_relaxed)
                     + static_cast<double> (sample) / sampleRate;
        }

        /*  A breakpoint for a voice's speed, turned from Go.dot's sample into
            the beat the launches are placed in by the same anchor, and queued
            for the audio thread (namespace draft §22.4). Tick thread. */
        bool placeTrackRate (int trackIndex, std::int64_t atSample, double rate) noexcept
        {
            if (trackIndex < 0 || trackIndex >= static_cast<int> (rateVoices.size()) || ! (rate >= 0.0))
                return false;

            return rateVoices[static_cast<std::size_t> (trackIndex)]->post ({ beatsAtSample (atSample), rate });
        }

        std::uint32_t trackRateLateCount (int trackIndex) const noexcept
        {
            if (trackIndex < 0 || trackIndex >= static_cast<int> (rateVoices.size()))
                return 0;

            return rateVoices[static_cast<std::size_t> (trackIndex)]->lateCount();
        }

        bool launchTrackAt (int trackIndex, int slotIndex, double monotonicBeat) noexcept
        {
            const auto at = handleIndex (trackIndex, slotIndex);

            if (at >= handles.size())
                return false;

            auto& handle = handles[at];

            if (handle == nullptr)
                return false;

            /*  Two stores under a spin mutex the audio thread only ever
                try_locks, which is the whole of what GO does to Tracktion. */
            handle->play (te::MonotonicBeat { tracktion::BeatPosition::fromBeats (monotonicBeat) });
            return true;
        }

        bool stopTrackAt (int trackIndex, int slotIndex,
                          std::optional<double> monotonicBeat) noexcept
        {
            const auto at = handleIndex (trackIndex, slotIndex);

            if (at >= handles.size())
                return false;

            auto& handle = handles[at];

            if (handle == nullptr)
                return false;

            if (monotonicBeat.has_value())
                handle->stop (te::MonotonicBeat { tracktion::BeatPosition::fromBeats (*monotonicBeat) });
            else
                handle->stop ({});

            return true;
        }

        /*  Every slot, at the next block. A stop cue stops the CUE, and which
            of its ranges was sounding is not something the caller knows or
            should have to. Answers true when at least one slot took it. */
        bool stopEverySlot (int trackIndex) noexcept
        {
            bool stopped = false;

            for (int slot = 0; slot < editSlots; ++slot)
                stopped = stopTrackAt (trackIndex, slot, {}) || stopped;

            return stopped;
        }

        AudioHost::TrackPlayState trackPlayState (int trackIndex, int slotIndex) const noexcept
        {
            AudioHost::TrackPlayState out;

            const auto at = handleIndex (trackIndex, slotIndex);

            if (at >= handles.size())
                return out;

            const auto& handle = handles[at];

            if (handle == nullptr)
                return out;

            out.valid = true;
            out.playing = handle->getPlayingStatus() == te::LaunchHandle::PlayState::playing;

            if (out.playing)
                if (const auto played = handle->getPlayedRange())
                    out.playedBeats = played->getLength().inBeats();

            return out;
        }

        bool launchTrack (int trackIndex, int slotIndex)
        {
            auto* clip = clipOn (trackIndex, slotIndex);

            if (clip == nullptr || edit == nullptr)
                return false;

            auto handle = clip->getLaunchHandle();
            auto* playbackContext = edit->getTransport().getCurrentPlaybackContext();

            if (handle == nullptr || playbackContext == nullptr)
                return false;

            const auto syncPoint = playbackContext->getSyncPoint();

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

        /*  ANY SLOT, because the question is whether the CUE is sounding and a
            ranged cue sounds out of whichever slot its current range is in.

            This asked slot nought until PR 3.8, which was the same question
            while there was only one slot and is a DIFFERENT one now: a cue on
            its second range would have reported itself finished, its run would
            have ended, and the sound would have gone on playing with nothing
            holding the voice.

            Through the cached handles rather than through clipOn, because the
            Runner asks this once a tick for every live run and reaching a clip
            costs two heap allocations. */
        bool isTrackPlaying (int trackIndex) const
        {
            for (int slot = 0; slot < editSlots; ++slot)
            {
                const auto at = handleIndex (trackIndex, slot);

                if (at >= handles.size())
                    continue;

                const auto& handle = handles[at];

                if (handle != nullptr
                      && handle->getPlayingStatus() == te::LaunchHandle::PlayState::playing)
                    return true;
            }

            return false;
        }

        double trackSourceLengthSeconds (int trackIndex, int slotIndex) const
        {
            auto* clip = clipOn (trackIndex, slotIndex);

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

            /*  The one buildEdit cached, rather than a second lookup that
                could disagree with it. Named for what it is so it does not
                shadow the member. */
            auto* playbackContext = context;

            if (playbackContext == nullptr)
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
            auto node = te::createNodeForEdit (*playbackContext, audibleTime, params);

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

            /*  sortedNodes, NOT orderedNodes, and at eight slots a track that
                is the whole difference between a check and a gesture.

                orderedNodes is the outer graph: what the processor walks, and
                what Tracktion's own debug assertion looks at. A launcher slot
                is not in it. SlotControlNode is an INTERNAL child of the
                switching node above it (ArrangerLauncherSwitchingNode.cpp:70-
                79), so a graph with eight slots on every track has exactly as
                many ordered nodes as one with a single slot - measured, and it
                is why the anti-vacuity bound here is not a count of slots.

                But sortedNodes is built by createNodeMap, which recurses
                through getInternalNodes (tracktion_Node.h:773-779), and it is
                sortedNodes that findNodeWithID searches (tracktion_Utility.h:
                82-97) - the lookup by which a rebuilt node adopts its
                predecessor's state. SlotControlNode::prepareToPlay does
                exactly that with the raw slot id (SlotControlNode.cpp:87-89).

                So sortedNodes is the collection where a collision does harm,
                the slots are in it and only in it, and it is what gets asked. */
            std::vector<std::pair<std::size_t, const char*>> ids;

            for (const auto& entry : graph->sortedNodes)
                if (entry.node != nullptr)
                    ids.emplace_back (entry.id, typeid (*entry.node).name());

            report.nodes = static_cast<int> (ids.size());
            report.outerNodes = static_cast<int> (graph->orderedNodes.size());

            std::vector<std::pair<std::size_t, const char*>> nonZero;

            for (const auto& entry : ids)
            {
                if (entry.first == 0)
                    ++report.zeroIds;
                else
                    nonZero.push_back (entry);
            }

            std::sort (nonZero.begin(), nonZero.end(),
                       [] (const auto& a, const auto& b) { return a.first < b.first; });

            for (std::size_t i = 1; i < nonZero.size(); ++i)
            {
                if (nonZero[i].first != nonZero[i - 1].first)
                    continue;

                ++report.duplicates;

                /*  std::type_info::name is not guaranteed unique across
                    translation units, but the pointers here all come from one
                    graph in one process, and a string compare is what makes
                    the answer readable rather than pointer-identical. */
                if (std::strcmp (nonZero[i].second, nonZero[i - 1].second) == 0)
                    ++report.typedDuplicates;
            }

            return report;
        }

        std::unique_ptr<te::Engine> engine;

        /*  EVERY VOICE'S SPEED, and every slot's adaptor onto it (namespace
            draft §22.4): one RateVoice a track, one SlotSpeed a launch handle,
            in `handles`' order. DECLARED BEFORE `edit` so that they are
            destroyed after it: a graph still being torn down may ask a slot one
            last question, and the answer must still be there. Made by buildEdit
            and let go by stop, like the handles, never while a block runs. */
        std::vector<std::unique_ptr<RateVoice>> rateVoices;
        std::vector<std::unique_ptr<SlotSpeed>> slotSpeeds;
        std::atomic<double> stretchLimit { 0.0 };

        std::unique_ptr<te::Edit> edit;
        std::vector<CueMatrix*> matrices;
        std::vector<CueEq*> eqs;
        std::vector<CueOutputPlugin*> plugins;

        /*  THE SANDBOX'S HALF (Phase 9a): every track's lanes, in the order of
            its chain - a voice's by the set's slots, a rack channel's by its own
            chain (Phase 9b) - and the hosts, destroyed before the Edit because
            the lanes live in it. `proxySlots` is the SET's size. */
        std::vector<std::vector<plugin::ProxyLane*>> trackLanes;
        int proxySlots = 0;

        /*  THE LIVE RACK (Phase 9b): how many of the tracks are voices, every
            rack track's input stage (index - voices), which track each channel
            was built as, and the view of the tap the stages read. */
        int voices = 0;
        std::vector<LiveInputPlugin*> liveInputs;
        std::map<std::string, int> rackTracks;
        LiveInputTap tapView;

        /*  WHEN EACH RACK CHANNEL WAS LAST SHUT, in Go.dot's count - the start
            of its ring-out - or -1 while it is open or has never been, and -2
            after a kill, which rings out nothing. Tick thread's own, sized with
            the rack when the graph is built. */
        std::vector<std::int64_t> rackShutAt;

        /*  THE TAKES (Phase 9c, namespace draft §19.2): each sampling channel's
            recorder, by the channel's id, owned here beside the Edit and never
            by a plugin, with the shape its memory was set aside for and what
            became of a take a rebuild could not keep. */
        struct TakeStore
        {
            Looper::Shape shape;
            std::shared_ptr<Looper> take;
            std::string problem;
        };

        std::map<std::string, TakeStore> takes;

        /** Which rack track records which channel's take, for a stop that knows the track. */
        std::map<int, std::string> takeTracks;
        std::mutex takesLock;

        /*  KEEP'S WRITER (Phase 9c, §19.8), made with the first Keep. A job
            holds its take by a shared pointer, so a take the store lets go of
            while it is written lives until the file is whole. */
        std::unique_ptr<TakeWriter> takeWriter;

        /** Whether the plugin table holds this graph's slots, to clear at stop. */
        bool toldTable = false;
        std::vector<std::unique_ptr<plugin::ProxyHost>> proxies;
        ProxyServices services;

        /*  RESOLVED ONCE, AT BUILD, because reaching them is not free. Every
            path to a clip goes through getAudioTracks(), which unconditionally
            does ensureStorageAllocated(32), and getClipSlots(), which returns a
            juce::Array by value - two heap allocations per call. That is
            tolerable in a diagnostic and not on the GO path, and at 50 Hz over
            four tracks it would be four hundred allocations a second on the
            thread that owns the model.

            getLaunchHandle() also make_shared's on first call and is not
            synchronised, so it is called here on the message thread and never
            again. The member it fills is assigned once and never reset, so the
            pointer is good for the life of the show. */
        std::vector<std::shared_ptr<te::LaunchHandle>> handles;

        /** The width each track was built with, for a cue's routing. */
        int editChannels = 2;

        /*  How many launcher slots every track was built with: the widest range
            count in the show, at least one. Fixed by buildEdit and never after,
            which is what makes the flat handle cache indexable. */
        int editSlots = 1;

        /*  The playback context, cached for the same reason. Message thread
            writes it, the audio thread reads it; both only while the graph is
            not being rebuilt, which is never after load (PRD §3.25). */
        te::EditPlaybackContext* context = nullptr;

        /*  THE ANCHOR, and it is the whole of the launch arithmetic.

            A launch is placed at a MonotonicBeat, and Go.dot has to turn one of
            its own future sample positions into one. The two numbers are
            related by a constant, and the honest way to find a constant is to
            measure it rather than to derive it - so once per block, from the
            callback, where Go.dot's counter and Tracktion's monotonic beat
            describe the SAME instant, this records the difference:

                beatOffset = monotonicBeat - samplesElapsed / sampleRate

            Read from the tick thread as one relaxed load. It is 0.0 in a
            healthy run today, and it is NOT hard-coded as zero: that zero is a
            coincidence of two facts that happen to cancel, and four paths in
            Tracktion break it without announcing themselves - a suspended
            device, the CPU-overload mute, a cleared node graph and a transport
            re-prepare. Measuring it every block is what makes it self-heal. */
        std::atomic<double> beatOffset { 0.0 };

        /*  Diagnostics for the same measurement. `anchorSample` says how stale
            the offset is; `referenceSkew` is Tracktion's own sample counter
            minus Go.dot's, which is one block in a healthy run and CHANGES when
            Tracktion has skipped blocks. A change there is the thing to look at
            when a show drifts. */
        std::atomic<std::int64_t> anchorSample { 0 };
        std::atomic<std::int64_t> referenceSkew { 0 };

        /** The rate as a double, so the anchor does not convert one per block. */
        double sampleRate = 0.0;
        juce::AudioBuffer<float> scratch;
        juce::MidiBuffer midi;

        /*  THE INPUT TAP and each logical input's peak since the last take
            (Phase 9b). Sized at `start`, never on the audio thread; the peaks
            are written there with relaxed stores and taken on the tick thread
            with an exchange, the output stage's pattern. */
        juce::AudioBuffer<float> tap;
        std::unique_ptr<std::atomic<float>[]> inputPeaks;
        int tapChannels = 0;

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
    void AudioHost::processBlock (const float* const* inputs, int numInputs)
    { impl->processBlock (inputs, numInputs); }

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
        return impl->voices;
    }

    int AudioHost::allTrackCount() const noexcept
    {
        return static_cast<int> (impl->matrices.size());
    }

    int AudioHost::rackTrackOf (const std::string& channelId) const noexcept
    {
        const auto found = impl->rackTracks.find (channelId);
        return found != impl->rackTracks.end() ? found->second : -1;
    }

    std::shared_ptr<Looper> AudioHost::takeOf (const std::string& channelId)
    {
        return impl->takeFor (channelId);
    }

    std::shared_ptr<Looper> AudioHost::takeOfTrack (int trackIndex)
    {
        return impl->takeForTrack (trackIndex);
    }

    std::vector<std::pair<std::string, std::shared_ptr<const Looper>>> AudioHost::allTakes()
    {
        return impl->allTakes();
    }

    bool AudioHost::keepTake (const std::string& channelId, const std::string& stem, const std::string& mediaFolder)
    {
        return impl->keepTake (channelId, stem, mediaFolder);
    }

    std::vector<TakeWriter::Done> AudioHost::keptTakes()
    {
        return impl->keptTakes();
    }

    void AudioHost::setRackSource (int trackIndex, int firstInput, int width) noexcept
    {
        if (auto* stage = impl->liveInputOf (trackIndex))
            stage->setSource (firstInput, width);
    }

    void AudioHost::openRackGate (int trackIndex, std::int64_t sample, double rampSeconds) noexcept
    {
        if (auto* stage = impl->liveInputOf (trackIndex))
        {
            stage->openAt (sample, rampSeconds);
            impl->rackShutAt[static_cast<std::size_t> (trackIndex - impl->voices)] = -1;
        }
    }

    void AudioHost::shutRackGate (int trackIndex, double rampSeconds) noexcept
    {
        if (auto* stage = impl->liveInputOf (trackIndex))
        {
            stage->shut (rampSeconds);

            /*  THE RING-OUT STARTS NOW, and a second shut - a stop's fade that
                ends in a stop - starts it again from there. A kill's stays a
                kill. */
            auto& shutAt = impl->rackShutAt[static_cast<std::size_t> (trackIndex - impl->voices)];

            if (shutAt != -2)
                shutAt = impl->samples.samplesElapsed();
        }
    }

    void AudioHost::killRack (int trackIndex) noexcept
    {
        auto* stage = impl->liveInputOf (trackIndex);

        if (stage == nullptr)
            return;

        stage->shut (LiveInputPlugin::killSeconds);
        impl->rackShutAt[static_cast<std::size_t> (trackIndex - impl->voices)] = -2;

        if (auto* matrix = trackMatrix (trackIndex))
            matrix->setLevelDb (CueMatrix::silenceDb);

        /*  THE EQ AS WELL (2026-10-01, namespace draft §23.6): its filter
            memory is a tail like a plugin's, heard under the next cue's
            fade-in until that cue's arm cleared it. */
        if (auto* eq = trackEq (trackIndex))
            eq->reset();

        for (auto* lane : impl->trackLanes[static_cast<std::size_t> (trackIndex)])
            if (lane != nullptr)
                lane->requestReset();
    }

    bool AudioHost::killTrack (int trackIndex) noexcept
    {
        /*  BOUNDED BY THE LANES AS WELL AS THE VOICES: a graph that failed to
            build lets go of every lane and keeps its voice count. */
        if (trackIndex < 0 || trackIndex >= impl->voices
              || trackIndex >= static_cast<int> (impl->trackLanes.size()))
            return false;

        /*  THE STOP FIRST, then the rest, all landing at the next block. Its
            ten-sample fade still reaches the chain after the reset, which is
            what the level is for: silence at the output stage a tick on,
            whatever the chain was seeded with or could not let go of. */
        const auto stopped = impl->stopEverySlot (trackIndex);

        auto* matrix = trackMatrix (trackIndex);
        const auto alreadySilent = matrix != nullptr && matrix->levelDb() <= CueMatrix::silenceDb;

        if (matrix != nullptr)
            matrix->setLevelDb (CueMatrix::silenceDb);

        if (auto* eq = trackEq (trackIndex))
            eq->reset();

        /*  THE INSERTS ONLY ON A VOICE STILL HEARD (2026-10-01, namespace draft
            §23.6, GE). One child serves a plugin's lane on every voice and runs
            their resets one after another in a pass - a VST3's is a whole
            deactivation - while every lane it serves waits out its deadline;
            enough of them at once and each lane misses block after block until
            the plugin is failed. A double Esc's kills are exactly that many at
            once - a scene's members in one tick - and they land on voices the
            press's sweep has already taken to silence, as an earlier fade to
            nothing would have: nothing of their inserts can be heard, and the
            next arm resets them before anything plays through them again. */
        if (! alreadySilent)
            for (auto* lane : impl->trackLanes[static_cast<std::size_t> (trackIndex)])
                if (lane != nullptr)
                    lane->requestReset();

        return stopped;
    }

    void AudioHost::resetEffects (const std::vector<int>& ready) noexcept
    {
        /*  EVERY TRACK THE GRAPH WAS BUILT WITH, held or not - a tail outlives
            the run that made it - and nothing switched: a reset is not a
            bypass, and an insert switched out would pass the block dry and
            keep its tail frozen in it.

            SAFE BESIDE buildEdit BECAUSE NO HANDLER RUNS WHILE A GRAPH IS
            BUILT: every rebuild - an Apply, Load now, a clock followed - joins
            the tick thread, the only one that runs handlers, and takes the
            Player away before it touches the graph. An outage is the one time
            `run.killAll` is let through on purpose (AudioSettings.cpp), and the
            graph is then the one the show had, paused and not torn down: what
            is asked here waits for its next block. */
        const auto tracks = static_cast<int> (impl->trackLanes.size());

        for (int track = 0; track < tracks; ++track)
        {
            if (std::find (ready.begin(), ready.end(), track) != ready.end())
                continue;

            const auto voice = track < impl->voices;

            if (voice)
                if (auto* matrix = trackMatrix (track))
                    matrix->setLevelDb (CueMatrix::silenceDb);

            /*  WHAT STILL SOUNDS IS EMPTIED BY ITS OWN KILL, a tick from now
                and under the silence begun above. Cleared here, its EQ would
                drop the boost it carries in one sample and a plugin miss a
                block in the middle of the sound: a click before the cut. */
            if (voice ? impl->isTrackPlaying (track) : isRackPassing (track))
                continue;

            if (auto* eq = trackEq (track))
                eq->reset();

            /*  A VOICE'S INSERTS ARE LEFT TO ITS SILENCE AND ITS NEXT ARM
                (2026-10-01, namespace draft §23.6, GE). One child serves a
                plugin's lane on every voice and resets them one after another
                in a pass - a VST3's reset is a whole deactivation - while every
                lane it serves waits out its deadline, and a lane late eight
                blocks running fails the plugin for every voice. A sweep that
                reset every idle voice's inserts at once was exactly that burst,
                once a press, for nothing heard: the level above takes the voice
                to exact silence in a tick, and every arm resets the inserts
                again before its launch. A rack channel's level is left, so
                nothing else would empty its inserts, and they are reset here -
                which a rack whose idle channels share one plugin can still feel
                as a smaller burst (§23.6's limits). */
            if (voice)
                continue;

            for (auto* lane : impl->trackLanes[static_cast<std::size_t> (track)])
                if (lane != nullptr)
                    lane->requestReset();
        }
    }

    bool AudioHost::isRackTrack (int trackIndex) const noexcept
    {
        return impl->liveInputOf (trackIndex) != nullptr;
    }

    bool AudioHost::isRackSounding (int trackIndex) const noexcept
    {
        const auto* stage = impl->liveInputOf (trackIndex);

        if (stage == nullptr)
            return false;

        if (stage->isPassing())
            return true;

        const auto shutAt = impl->rackShutAt[static_cast<std::size_t> (trackIndex - impl->voices)];

        if (shutAt < 0)
            return false;

        /*  RINGING UNTIL QUIET, OR UNTIL THE CAP. What reaches the output
            stage is the chain's output - the reverb's tail after the input has
            gone - so a quarter of a second of it under -60 dB is the tail
            over. Read against the rate the graph runs at. */
        const auto rate = impl->current.sampleRate > 0 ? impl->current.sampleRate : 48000;
        const auto* output = impl->plugins[static_cast<std::size_t> (trackIndex)];
        const auto quietEnough = output != nullptr
                                   && output->quietSamples() >= static_cast<std::int64_t> (rackQuietSeconds * rate);
        const auto capped = impl->samples.samplesElapsed() - shutAt
                              >= static_cast<std::int64_t> (rackTailCapSeconds * rate);

        return ! quietEnough && ! capped;
    }

    bool AudioHost::isRackPassing (int trackIndex) const noexcept
    {
        const auto* stage = impl->liveInputOf (trackIndex);
        return stage != nullptr && stage->isPassing();
    }

    CueMatrix* AudioHost::trackMatrix (int trackIndex) noexcept
    {
        if (trackIndex < 0 || trackIndex >= allTrackCount())
            return nullptr;

        return impl->matrices[static_cast<std::size_t> (trackIndex)];
    }

    CueEq* AudioHost::trackEq (int trackIndex) noexcept
    {
        if (trackIndex < 0 || trackIndex >= static_cast<int> (impl->eqs.size()))
            return nullptr;

        return impl->eqs[static_cast<std::size_t> (trackIndex)];
    }

    void AudioHost::setTrackEq (int trackIndex, const EqSettings& settings) noexcept
    {
        if (auto* eq = trackEq (trackIndex))
            eq->set (settings);
    }

    void AudioHost::snapTrackEq (int trackIndex, const EqSettings& settings) noexcept
    {
        if (auto* eq = trackEq (trackIndex))
        {
            eq->set (settings);
            eq->reset();
        }
    }

    //==============================================================================
    void AudioHost::setProxyServices (ProxyServices services)
    {
        impl->services = std::move (services);
    }

    int AudioHost::proxyCount() const noexcept
    {
        return impl->proxySlots;
    }

    plugin::ProxyHost* AudioHost::proxy (int slot) noexcept
    {
        if (slot < 0 || slot >= static_cast<int> (impl->proxies.size()))
            return nullptr;

        return impl->proxies[static_cast<std::size_t> (slot)].get();
    }

    plugin::ProxyLane* AudioHost::proxyLane (int trackIndex, int slot) noexcept
    {
        if (trackIndex < 0 || slot < 0 || trackIndex >= static_cast<int> (impl->trackLanes.size()))
            return nullptr;

        const auto& chain = impl->trackLanes[static_cast<std::size_t> (trackIndex)];

        return slot < static_cast<int> (chain.size()) ? chain[static_cast<std::size_t> (slot)] : nullptr;
    }

    void AudioHost::setTrackFxShape (int trackIndex, int slot, int feed, int back) noexcept
    {
        if (auto* lane = proxyLane (trackIndex, slot))
            lane->setShape (feed, back);
    }

    void AudioHost::setTrackFxEnabled (int trackIndex, int slot, bool enabled) noexcept
    {
        if (auto* lane = proxyLane (trackIndex, slot))
            lane->setEnabled (enabled);
    }

    void AudioHost::setTrackFxParameter (int trackIndex, int slot, int parameter, float normalised) noexcept
    {
        if (auto* lane = proxyLane (trackIndex, slot))
            lane->setParameter (parameter, normalised);
    }

    void AudioHost::snapTrackFx (int trackIndex, int slot, bool enabled,
                                 const std::vector<std::pair<int, float>>& values,
                                 const std::string& statePath)
    {
        if (auto* lane = proxyLane (trackIndex, slot))
        {
            lane->setValues (values);
            lane->setEnabled (enabled);
            lane->requestReset();

            /*  AND ITS WHOLE STATE, when the cue switches it in: loaded before
                the launch, the values above set again on top of it. A cue
                with none asks for the preset's own - no state is a state, or
                the last cue's would be heard under this one. */
            if (enabled)
                lane->wantState (statePath);
        }
    }

    bool AudioHost::isTrackFxSettled (int trackIndex) noexcept
    {
        if (trackIndex < 0 || trackIndex >= static_cast<int> (impl->trackLanes.size()))
            return true;

        for (auto* lane : impl->trackLanes[static_cast<std::size_t> (trackIndex)])
            if (lane != nullptr && ! lane->stateSettled())
                return false;

        return true;
    }

    void AudioHost::pollProxies()
    {
        for (auto& proxy : impl->proxies)
            proxy->poll();
    }

    bool AudioHost::restartProxy (const std::string& pluginId, std::string& problem)
    {
        for (auto& proxy : impl->proxies)
            if (proxy->serves (pluginId))
                return proxy->restart (problem);

        problem = "no plugin of the set or the rack has the id " + pluginId + " in this graph";
        return false;
    }

    void AudioHost::startMissingProxies()
    {
        for (auto& proxy : impl->proxies)
            if (proxy->status().state == "missing")
            {
                std::string problem;
                proxy->start (problem);
            }
    }

    AudioHost::NodeIdReport AudioHost::inspectNodeIds() const  { return impl->inspectNodeIds(); }
    int AudioHost::residentClipCount() const { return impl->residentClipCount(); }

    bool AudioHost::setTrackSource (int trackIndex, int slot, const std::string& mediaFile, bool stretch)
    {
        return impl->setTrackSource (trackIndex, slot, mediaFile, stretch);
    }

    bool AudioHost::setTrackRanges (int trackIndex, const std::string& mediaFile,
                                    const std::vector<RangeSpec>& ranges, double startOffset,
                                    bool stretch, int startSlot, double sliceOffset)
    {
        return impl->setTrackRanges (trackIndex, mediaFile, ranges, startOffset, stretch,
                                     startSlot, sliceOffset);
    }

    int AudioHost::slotCount() const noexcept  { return impl->editSlots; }

    bool AudioHost::isTrackSourceReady (int trackIndex) const
    {
        return impl->isTrackSourceReady (trackIndex);
    }

    bool AudioHost::isTrackStretched (int trackIndex, int slot) const
    {
        return impl->isTrackStretched (trackIndex, slot);
    }

    void AudioHost::setTrackRouting (int trackIndex, double levelDb,
                                     const std::vector<std::array<double, 3>>& coefficients)
    {
        impl->setTrackRouting (trackIndex, levelDb, coefficients);
    }

    bool AudioHost::waitForTrackSourceReady (int trackIndex, int timeoutMilliseconds)
    {
        return impl->waitForTrackSourceReady (trackIndex, timeoutMilliseconds);
    }

    bool AudioHost::launchTrack (int trackIndex, int slot)
    {
        return impl->launchTrack (trackIndex, slot);
    }

    double AudioHost::beatsAtSample (std::int64_t sample) const noexcept
    {
        return impl->beatsAtSample (sample);
    }

    std::int64_t AudioHost::anchoredAtSample() const noexcept
    {
        return impl->anchorSample.load (std::memory_order_relaxed);
    }

    std::int64_t AudioHost::referenceSkewSamples() const noexcept
    {
        return impl->referenceSkew.load (std::memory_order_relaxed);
    }

    bool AudioHost::launchTrackAt (int trackIndex, int slot, double monotonicBeat) noexcept
    {
        return impl->launchTrackAt (trackIndex, slot, monotonicBeat);
    }

    bool AudioHost::placeTrackRate (int trackIndex, std::int64_t atSample, double rate) noexcept
    {
        return impl->placeTrackRate (trackIndex, atSample, rate);
    }

    std::uint32_t AudioHost::trackRateLateCount (int trackIndex) const noexcept
    {
        return impl->trackRateLateCount (trackIndex);
    }

    double AudioHost::stretchSpeedLimit() const noexcept
    {
        return impl->stretchLimit.load (std::memory_order_relaxed);
    }

    bool AudioHost::stopTrackAt (int trackIndex, int slot, double monotonicBeat) noexcept
    {
        return impl->stopTrackAt (trackIndex, slot, monotonicBeat);
    }

    bool AudioHost::stopTrack (int trackIndex) noexcept
    {
        return impl->stopEverySlot (trackIndex);
    }

    AudioHost::TrackPlayState AudioHost::trackPlayState (int trackIndex, int slot) const noexcept
    {
        return impl->trackPlayState (trackIndex, slot);
    }

    bool AudioHost::isTrackPlaying (int trackIndex) const
    {
        return impl->isTrackPlaying (trackIndex);
    }

    double AudioHost::trackSourceLengthSeconds (int trackIndex, int slot) const
    {
        return impl->trackSourceLengthSeconds (trackIndex, slot);
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

    float AudioHost::takeTrackOutputPeak (int trackIndex)
    {
        if (trackIndex < 0 || trackIndex >= static_cast<int> (impl->plugins.size()))
            return 0.0f;

        return impl->plugins[static_cast<std::size_t> (trackIndex)]->takeOutputPeak();
    }

    int AudioHost::inputChannelCount() const noexcept
    {
        return impl->tapChannels;
    }

    float AudioHost::takeInputPeak (int channel) noexcept
    {
        if (channel < 0 || channel >= impl->tapChannels || impl->inputPeaks == nullptr)
            return 0.0f;

        return impl->inputPeaks[static_cast<std::size_t> (channel)].exchange (0.0f, std::memory_order_relaxed);
    }

    const float* AudioHost::inputTapChannel (int channel) const noexcept
    {
        if (channel < 0 || channel >= impl->tapChannels)
            return nullptr;

        return impl->tap.getReadPointer (channel);
    }

    int AudioHost::editChannelsPerTrack() const noexcept  { return impl->editChannels; }

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

        return device != nullptr ? static_cast<int> (device->getChannels().size()) : 0;
    }
}
