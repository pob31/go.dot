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
#include <algorithm>
#include <cstdint>
#include <cstring>
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

            edit.reset();
            matrices.clear();
            plugins.clear();
            handles.clear();
            context = nullptr;

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
            for (int track = 0; track < static_cast<int> (matrices.size()); ++track)
            {
                for (int slot = 0; slot < editSlots; ++slot)
                {
                    std::shared_ptr<te::LaunchHandle> handle;

                    if (auto* clip = clipOn (track, slot))
                        handle = clip->getLaunchHandle();

                    handles.push_back (std::move (handle));
                }
            }

            beatOffset.store (0.0, std::memory_order_relaxed);
            anchorSample.store (0, std::memory_order_relaxed);
            referenceSkew.store (0, std::memory_order_relaxed);

            error.clear();
            return true;
        }

        void stop()
        {
            edit.reset();
            matrices.clear();
            plugins.clear();
            handles.clear();
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

        /*  Points one slot's clip at one file, whole - the Phase 2 arm, now
            told which slot. Everything a RANGE needs on top of this is in
            armRangeInto below; this stays the plain case because a cue with no
            ranges is still most cues.

            It does not dispatch: the caller does, once, so that arming eight
            slots is one round of pending updates rather than eight. */
        bool pointSlotAtFile (int trackIndex, int slotIndex, const juce::File& file)
        {
            auto* clip = clipOn (trackIndex, slotIndex);

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

            return true;
        }

        /*  One range into one slot: the file, and then the section of it, armed
            LOOPING so that the launcher builds no stop duration for it.

            WHY LOOPING IS THE MECHANISM AND NOT A SETTING. SlotControlNode
            captures a stop duration when the graph is built - the clip's length
            in beats when `isLooping()` is false, nothing at all when it is
            (tracktion_EditNodeBuilder.cpp:1025-1026) - and every block, before
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
                           const AudioHost::RangeSpec& range)
        {
            if (! pointSlotAtFile (trackIndex, slotIndex, file))
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

            return clip->isLooping();
        }

        bool setTrackSource (int trackIndex, int slotIndex, const std::string& mediaFile)
        {
            const juce::File file { juce::String (mediaFile) };

            if (! pointSlotAtFile (trackIndex, slotIndex, file))
                return false;

            edit->dispatchPendingUpdatesSynchronously();
            return true;
        }

        bool setTrackRanges (int trackIndex, const std::string& mediaFile,
                             const std::vector<AudioHost::RangeSpec>& ranges)
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
                                          ranges[static_cast<std::size_t> (slot)]) && armed;
                    continue;
                }

                /*  BACK ONTO THE PLACEHOLDER, because a voice is reused. A slot
                    still holding the last cue's third range would sound if
                    anything ever launched it, and the thing that eventually
                    launches it is a bug in a later phase rather than never. */
                if (ranges.empty() && slot == 0)
                {
                    armed = pointSlotAtFile (trackIndex, 0, file) && armed;
                    continue;
                }

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
        std::unique_ptr<te::Edit> edit;
        std::vector<CueMatrix*> matrices;
        std::vector<CueOutputPlugin*> plugins;

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

    bool AudioHost::setTrackSource (int trackIndex, int slot, const std::string& mediaFile)
    {
        return impl->setTrackSource (trackIndex, slot, mediaFile);
    }

    bool AudioHost::setTrackRanges (int trackIndex, const std::string& mediaFile,
                                    const std::vector<RangeSpec>& ranges)
    {
        return impl->setTrackRanges (trackIndex, mediaFile, ranges);
    }

    int AudioHost::slotCount() const noexcept  { return impl->editSlots; }

    bool AudioHost::isTrackSourceReady (int trackIndex) const
    {
        return impl->isTrackSourceReady (trackIndex);
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
