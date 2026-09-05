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

#include <wfg/engine/audio/HostedAudioDriver.h>

#include <wfg/engine/clock/SampleTime.h>

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>

namespace wfg::audio
{
    /*  The render, as a BlockSink.

        WHY juce::AudioFormatWriter::ThreadedWriter AND NOT A FILE WRITE. The
        pump thread is the audio thread as far as PRD §4.2 is concerned, and a
        write() is a syscall. ThreadedWriter is a lock-free FIFO plus a
        background thread and exists for exactly this; its write() takes no lock
        and touches no file. A full FIFO drops the block and says so rather than
        blocking, which is the right trade: a render that stuttered because the
        disk was busy is a bad recording, a graph that stalled because the disk
        was busy is a bad show. */
    struct HostedAudioDriver::Render final : BlockSink
    {
        Render (const juce::File& file, int sampleRate, int channels)
            : backgroundThread ("wfg render")
        {
            file.getParentDirectory().createDirectory();
            file.deleteFile();

            std::unique_ptr<juce::FileOutputStream> stream { file.createOutputStream() };

            if (stream == nullptr)
                return;

            juce::WavAudioFormat format;

            /*  Float, not 16-bit. The render is a MEASUREMENT - PR 2.4 asserts
                that a fade reaches -120 dB, and 16 bits cannot express that.
                Quantising the evidence to make the file smaller would be
                measuring the quantiser. */
            std::unique_ptr<juce::AudioFormatWriter> writer {
                format.createWriterFor (stream.get(), static_cast<double> (sampleRate),
                                        static_cast<unsigned int> (channels), 32,
                                        {}, 0) };

            if (writer == nullptr)
                return;

            stream.release();

            backgroundThread.startThread (juce::Thread::Priority::normal);

            /*  Two seconds of FIFO. Enough that an ordinary disk hiccup is
                absorbed rather than heard, and small enough that a 64-channel
                render does not reserve a gigabyte to record silence. */
            threaded = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (
                           writer.release(), backgroundThread, sampleRate * 2);

            /*  THE HEADER IS REWRITTEN EVERY SECOND, and this is not a
                nicety. A WAV's header carries the length, so it is only
                correct once the file is closed - and the black-box harness
                stops the server with terminate(), which on Windows is
                TerminateProcess and runs no destructor at all. Without this a
                render would be a RIFF header saying zero frames on top of
                several megabytes of perfectly good audio, and the test that
                went to listen to it would find nothing.

                The event log already survives a hard kill for the same reason;
                a render that did not would be the one artefact of a session
                that needed the process to have been asked politely. One second
                is the most that can be lost, and the flush happens on the
                writer's own background thread. */
            threaded->setFlushInterval (sampleRate);
        }

        ~Render() override
        {
            threaded.reset();               // flushes and closes the file
            backgroundThread.stopThread (2000);
        }

        bool isOpen() const noexcept { return threaded != nullptr; }

        void blockProduced (const float* const* channels, int numChannels,
                            int numSamples) noexcept override
        {
            if (threaded == nullptr)
                return;

            if (threaded->write (channels, numSamples))
                frames.fetch_add (numSamples, std::memory_order_relaxed);
            else
                dropped.fetch_add (numSamples, std::memory_order_relaxed);

            juce::ignoreUnused (numChannels);
        }

        juce::TimeSliceThread backgroundThread;
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> threaded;

        std::atomic<std::int64_t> frames { 0 };
        std::atomic<std::int64_t> dropped { 0 };
    };

    //==============================================================================
    HostedAudioDriver::HostedAudioDriver (std::string storageFolder)
        : audioHost (std::move (storageFolder))
    {
    }

    HostedAudioDriver::~HostedAudioDriver()
    {
        stop();
    }

    //==============================================================================
    bool HostedAudioDriver::open (const Settings& requested)
    {
        error.clear();

        if (requested.sampleRate <= 0 || requested.blockSize <= 0 || requested.outputChannels <= 0)
        {
            error = "the hosted driver needs a positive rate, block size and output count";
            return false;
        }

        HostSettings hostSettings;
        hostSettings.sampleRate = requested.sampleRate;
        hostSettings.blockSize = requested.blockSize;
        hostSettings.outputChannels = requested.outputChannels;

        if (! audioHost.start (hostSettings))
        {
            error = audioHost.lastError();
            return false;
        }

        current = requested;
        return true;
    }

    bool HostedAudioDriver::start()
    {
        if (worker.joinable())
            return true;

        if (! audioHost.isRunning())
        {
            error = "the hosted driver was started before it was opened";
            return false;
        }

        if (! current.renderFile.empty())
        {
            render = std::make_unique<Render> (juce::File (juce::String (current.renderFile)),
                                               current.sampleRate, current.outputChannels);

            if (! render->isOpen())
            {
                error = "cannot write the render at " + current.renderFile;
                render.reset();
                return false;
            }

            audioHost.setBlockSink (render.get());
        }

        {
            const std::lock_guard<std::mutex> lock { mutex };
            stopping = false;
        }

        blocks.store (0, std::memory_order_relaxed);
        lastLateness.store (0, std::memory_order_relaxed);
        maxLateness.store (0, std::memory_order_relaxed);

        running.store (true, std::memory_order_relaxed);
        worker = std::thread ([this] { run(); });

        return true;
    }

    void HostedAudioDriver::stop()
    {
        if (worker.joinable())
        {
            {
                const std::lock_guard<std::mutex> lock { mutex };
                stopping = true;
            }

            wakeUp.notify_all();
            worker.join();
        }

        running.store (false, std::memory_order_relaxed);

        /*  The sink goes before the render does, and both go before the host.
            A pointer the audio thread reads without synchronisation is only
            safe to change with the audio stopped, which by here it is. */
        audioHost.setBlockSink (nullptr);
        render.reset();
        audioHost.stop();
    }

    std::int64_t HostedAudioDriver::framesRendered() const noexcept
    {
        return render != nullptr ? render->frames.load (std::memory_order_relaxed) : 0;
    }

    //==============================================================================
    void HostedAudioDriver::run()
    {
        using Clock = std::chrono::steady_clock;

        const auto begin = Clock::now();
        std::int64_t delivered = 0;

        std::unique_lock<std::mutex> lock { mutex };

        while (! stopping)
        {
            /*  The deadline for the NEXT block, measured from the absolute
                start. A delay per iteration instead would let every late block
                push the whole schedule out, and the driver would run slower
                than its own sample rate for the rest of the show rather than
                catching up. DummyAudioClock's loop, and the same reasoning. */
            const auto due = begin + samplesToDuration ((delivered + 1) * current.blockSize,
                                                        current.sampleRate);

            if (wakeUp.wait_until (lock, due, [this] { return stopping; }))
                break;

            /*  Out of the lock to run the block: stop() must not be made to
                wait behind one, and nothing the graph touches is protected by
                this mutex. */
            lock.unlock();

            audioHost.processBlock();

            const auto late = durationToSamples (
                std::chrono::duration_cast<std::chrono::nanoseconds> (Clock::now() - due),
                current.sampleRate);

            lastLateness.store (late, std::memory_order_relaxed);

            if (late > maxLateness.load (std::memory_order_relaxed))
                maxLateness.store (late, std::memory_order_relaxed);

            ++delivered;
            blocks.store (delivered, std::memory_order_relaxed);

            lock.lock();
        }
    }
}
