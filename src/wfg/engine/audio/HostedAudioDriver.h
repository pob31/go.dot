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

#pragma once

#include <wfg/engine/audio/AudioHost.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

/*  A show running with no audio hardware: the graph, paced in real time, with
    somewhere for the sound to go.

    WHAT IT IS. DummyAudioClock with Tracktion in the middle. Phase 1's dummy
    clock advanced a counter on a paced thread and produced no audio; this
    advances the same counter by running an actual block through an actual
    playback graph, on the same schedule, and can write what comes out to a WAV.
    Everything downstream - TickThread, the tick clock, the whole control plane -
    sees a SampleClock and cannot tell which one it got, which is the point:
    `wfg serve --hosted` and `wfg serve` on a device are the same program with a
    different block source.

    WHY IT IS NOT TRACKTION'S OWN EnginePlayer. That class exists for Tracktion's
    tests and pumps as fast as it can. Go.dot owns time (PRD §3.25); the whole
    reason a hosted mode exists is to run a show at wall-clock rate with no
    hardware attached, so the pacing has to be Go.dot's, on Go.dot's deadline
    arithmetic, reporting Go.dot's lateness. It is the same loop DummyAudioClock
    runs and for the same reasons - the deadline is absolute rather than a delay
    per iteration, so a late block does not push the rest of the session out.

    WHY IT IS HOW CI HEARS ANYTHING. No runner has an audio interface. A render
    is the only way an automated test can assert that a cue made a sound, that a
    fade reached its endpoints, or that a stop stopped - and it is the same graph
    a show plays, minus the device.

    THREADS. The pump thread calls AudioHost::processBlock, which is the audio
    thread as far as PRD §4.2 is concerned; nothing on it allocates, locks or
    writes to disk. The render goes through JUCE's ThreadedWriter, which is a
    lock-free FIFO and a background thread, for exactly that reason. Bringing the
    engine up and building the Edit are message-thread work and both happen
    BEFORE the pump starts - which is why open() and start() are two calls and
    not one.
*/
namespace wfg::audio
{
    class HostedAudioDriver
    {
    public:
        /** `storageFolder` is Tracktion's, and is a parameter for the reason
            AudioHost's is: a default would write into the real application-data
            directory of whoever ran it. */
        explicit HostedAudioDriver (std::string storageFolder);
        ~HostedAudioDriver();

        HostedAudioDriver (const HostedAudioDriver&) = delete;
        HostedAudioDriver& operator= (const HostedAudioDriver&) = delete;

        struct Settings
        {
            int sampleRate = 0;
            int blockSize = 0;

            /** How wide the imaginary rig is. Read from the show's buses. */
            int outputChannels = 0;

            /** Where to write what comes out. Empty for nowhere, which is what
                a run nobody is recording uses. */
            std::string renderFile;
        };

        /*  Brings the engine up and opens the hosted interface. Nothing is
            pumping yet, deliberately: the caller builds the Edit next, and
            building it while blocks were going through would be a structural
            edit racing the graph that reads it. Message thread. */
        bool open (const Settings&);

        /** The host, so the caller can build the Edit between open and start. */
        AudioHost& host() noexcept                { return audioHost; }
        const AudioHost& host() const noexcept    { return audioHost; }

        /*  Starts the pump, and the render if one was asked for. From here the
            graph is live and nothing may touch the Edit. */
        bool start();

        /** Stops the pump, joins it, and closes the render file. Idempotent. */
        void stop();

        bool isRunning() const noexcept { return running.load (std::memory_order_relaxed); }

        /** Why open() or start() failed, empty if neither did. */
        const std::string& lastError() const noexcept { return error; }

        //======================================================================
        /** The counter the blocks advance. Hand it to TickThread. */
        const SampleClock& clock() const noexcept { return audioHost.clock(); }

        const Settings& settings() const noexcept { return current; }

        //======================================================================
        std::int64_t blocksDelivered() const noexcept
        {
            return blocks.load (std::memory_order_relaxed);
        }

        /*  How late the most recent block was against its own deadline, in
            samples, and the worst since start(). Never negative: a block early
            is a block on time as far as anything downstream can tell.

            Reported rather than asserted, exactly as DummyAudioClock reports it.
            A hosted run that says "the pump ran 4 ms behind" is worth having;
            one that quietly pretended otherwise would make every lateness
            number above it a fiction. */
        std::int64_t blockLateness() const noexcept
        {
            return lastLateness.load (std::memory_order_relaxed);
        }

        std::int64_t worstBlockLateness() const noexcept
        {
            return maxLateness.load (std::memory_order_relaxed);
        }

        /** How many frames reached the render file. Zero when not rendering. */
        std::int64_t framesRendered() const noexcept;

    private:
        void run();

        struct Render;

        AudioHost audioHost;
        Settings current;
        std::string error;

        std::unique_ptr<Render> render;

        std::thread worker;
        std::mutex mutex;
        std::condition_variable wakeUp;
        bool stopping = false;

        std::atomic<bool> running { false };
        std::atomic<std::int64_t> blocks { 0 };
        std::atomic<std::int64_t> lastLateness { 0 };
        std::atomic<std::int64_t> maxLateness { 0 };
    };
}
