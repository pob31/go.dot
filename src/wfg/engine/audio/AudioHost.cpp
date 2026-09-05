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

#include <wfg/engine/clock/AudioClockSource.h>

/*  juce_core and juce_events are named directly even though tracktion_engine.h
    would drag both in: an explicit include survives a Tracktion header
    reshuffle, an implicit one does not.

    The two naming traps recorded in Console.cpp apply here and this is the
    other file they can bite: tracktion_engine.h:72 bare-`#undef`s __TEXT, and
    tracktion_engine_playback.cpp:124-153 #undefs and redefines VERSION
    mid-translation-unit. Nothing below is called VERSION.
*/
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <tracktion_engine/tracktion_engine.h>

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
                                                   std::make_unique<Behaviour>());

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

        void stop()
        {
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

            /*  Denormals off for the block and only for the block - see the
                note in start(). The audio wants the flag; whatever formats a
                number after this returns must not inherit it. */
            const juce::ScopedNoDenormals denormalsOffWhileAudioRuns;

            scratch.clear();
            midi.clear();

            engine->getDeviceManager().getHostedAudioDeviceInterface().processBlock (scratch, midi);

            /*  After the graph has run, not before. A reader that saw the new
                sample count would otherwise be told the block had happened
                while it was still happening. */
            samples.advance (current.blockSize);
            blocks.fetch_add (1, std::memory_order_relaxed);
        }

        juce::File storageFolder;

        std::unique_ptr<te::Engine> engine;
        juce::AudioBuffer<float> scratch;
        juce::MidiBuffer midi;

        AudioClockSource samples;
        std::atomic<std::int64_t> blocks { 0 };

        HostSettings current;
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

    std::int64_t AudioHost::blocksProcessed() const noexcept
    {
        return impl->blocks.load (std::memory_order_relaxed);
    }

    const SampleClock& AudioHost::clock() const noexcept   { return impl->samples; }
    const HostSettings& AudioHost::settings() const noexcept { return impl->current; }
}
