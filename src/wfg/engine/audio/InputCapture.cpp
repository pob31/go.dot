/*
    This file is part of Go.dot — https://github.com/pob31/go.dot

    Copyright (C) 2026 Pierre-Olivier Boulant

    Go.dot is free software: you can redistribute it and/or modify it under the
    terms of the GNU General Public License as published by the Free Software
    Foundation, either version 3 of the License, or (at your option) any later
    version. Go.dot is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
    or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
    (LICENSE, at the repository root) for more details.

    SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <wfg/engine/audio/InputCapture.h>

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <utility>

namespace wfg::audio
{
    /*  The WAV, as the writer thread holds it: 32-bit float, as `--render`
        writes, because what is in it is a measurement - a click at -20 dBFS
        found to the sample - and quantising the evidence would be measuring
        the quantiser. */
    struct InputCapture::File
    {
        std::unique_ptr<juce::AudioFormatWriter> writer;
    };

    InputCapture::InputCapture (Settings settings) : wanted (std::move (settings)) {}

    InputCapture::~InputCapture()
    {
        stop();
    }

    std::string InputCapture::sidecarPath() const
    {
        return wanted.file + ".json";
    }

    std::string InputCapture::lastError() const
    {
        const std::lock_guard<std::mutex> lock { mutex };
        return ! error.empty() ? error : reason;
    }

    std::vector<int> InputCapture::inputs() const
    {
        std::vector<int> numbers;

        for (int channel = 0; channel < channelCount; ++channel)
            numbers.push_back (firstChannel + channel + 1);

        return numbers;
    }

    //==========================================================================
    bool InputCapture::prepare (int deviceInputs, int sampleRate, int blockSize)
    {
        if (ended.load (std::memory_order_acquire))
        {
            const std::lock_guard<std::mutex> lock { mutex };

            if (error.empty())
                error = "the capture has ended: " + reason;

            return false;
        }

        /*  AGAIN, ON THE SAME INTERFACE: a recovery that reopened the device
            on the clock it had, or a rebuild. The same shape carries on, and
            whether its clock did is the writer's to see (a count that went
            back ends it there). Another shape cannot be one file. */
        if (prepared.load (std::memory_order_acquire))
        {
            if (sampleRate == rate && blockSize == block && deviceInputs >= firstChannel + channelCount)
                return true;

            finishWith ("the interface was brought up again at " + std::to_string (sampleRate) + " Hz, "
                        + std::to_string (blockSize) + " frames and " + std::to_string (deviceInputs)
                        + " inputs, not the capture's " + std::to_string (rate) + " Hz, "
                        + std::to_string (block) + " frames and " + std::to_string (deviceWidth));
            return false;
        }

        const auto last = wanted.lastInput > 0 ? wanted.lastInput : deviceInputs;

        if (sampleRate <= 0 || blockSize <= 0)
            return refuse ("the capture needs a rate and a block size");

        if (wanted.firstInput < 1 || last < wanted.firstInput || last > deviceInputs)
            return refuse (deviceInputs == 0
                             ? std::string ("the interface has no inputs to capture")
                             : "inputs " + std::to_string (wanted.firstInput) + " to " + std::to_string (last)
                                 + " are not on an interface of " + std::to_string (deviceInputs) + " inputs");

        rate = sampleRate;
        block = blockSize;
        deviceWidth = deviceInputs;
        firstChannel = wanted.firstInput - 1;
        channelCount = last - wanted.firstInput + 1;

        /*  A SLOT A BLOCK, enough of them for the ring's seconds and never
            fewer than one: what the audio thread fills is one block, published
            whole, so nothing it writes is read half-made. */
        slotFrames = blockSize;
        slotCount = static_cast<std::size_t> (std::max (1.0, std::ceil (wanted.ringSeconds * sampleRate / blockSize)));
        slots.assign (slotCount, Slot {});
        samples.assign (slotCount * static_cast<std::size_t> (channelCount) * static_cast<std::size_t> (slotFrames), 0.0f);
        silence.assign (static_cast<std::size_t> (slotFrames), 0.0f);
        pointers.assign (static_cast<std::size_t> (channelCount), nullptr);

        const juce::File target { juce::String::fromUTF8 (wanted.file.c_str()) };
        target.getParentDirectory().createDirectory();
        target.deleteFile();

        std::unique_ptr<juce::OutputStream> stream { target.createOutputStream() };

        if (stream == nullptr)
            return refuse ("cannot write the capture at " + wanted.file);

        juce::WavAudioFormat format;
        auto made = format.createWriterFor (stream,
                                              juce::AudioFormatWriterOptions{}
                                                .withSampleRate (static_cast<double> (sampleRate))
                                                .withNumChannels (channelCount)
                                                .withBitsPerSample (32)
                                                .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));

        if (made == nullptr)
            return refuse ("cannot make a WAV writer for " + wanted.file);

        file = std::make_unique<File>();
        file->writer = std::move (made);

        filled.store (0, std::memory_order_relaxed);
        emptied.store (0, std::memory_order_relaxed);
        dropped.store (0, std::memory_order_relaxed);

        {
            const std::lock_guard<std::mutex> lock { mutex };
            stopping = false;
        }

        writeSidecar (false);

        //  The ring and the file exist before the first block can reach them.
        prepared.store (true, std::memory_order_release);
        writer = std::thread ([this] { run(); });
        return true;
    }

    //==========================================================================
    void InputCapture::write (const float* const* channels, int numChannels, int numSamples,
                              std::int64_t firstSample) noexcept WFG_AUDIO_THREAD
    {
        if (! prepared.load (std::memory_order_acquire) || ended.load (std::memory_order_relaxed))
            return;

        Slot* const slotData = slots.data();
        float* const sampleData = samples.data();
        const auto stride = static_cast<std::size_t> (channelCount) * static_cast<std::size_t> (slotFrames);

        for (int done = 0; done < numSamples;)
        {
            const auto frames = std::min (numSamples - done, slotFrames);
            const auto at = filled.load (std::memory_order_relaxed);

            /*  FULL: the block is dropped and counted, never waited for. The
                writer finds the hole from the next slot's sample and this
                count, and writes it as silence. */
            if (at - emptied.load (std::memory_order_acquire) >= slotCount)
            {
                dropped.fetch_add (frames, std::memory_order_relaxed);
                done += frames;
                continue;
            }

            const auto index = static_cast<std::size_t> (at % slotCount);
            Slot& slot = slotData[index];
            slot.sample = firstSample + done;
            slot.frames = frames;
            slot.droppedBefore = dropped.load (std::memory_order_relaxed);

            float* const base = sampleData + index * stride;

            for (int channel = 0; channel < channelCount; ++channel)
            {
                float* const out = base + static_cast<std::size_t> (channel) * static_cast<std::size_t> (slotFrames);
                const auto source = firstChannel + channel;
                const float* const in = source < numChannels && channels != nullptr ? channels[source] : nullptr;

                if (in != nullptr)
                    std::memcpy (out, in + done, static_cast<std::size_t> (frames) * sizeof (float));
                else
                    for (int frame = 0; frame < frames; ++frame)
                        out[frame] = 0.0f;
            }

            filled.store (at + 1, std::memory_order_release);
            done += frames;
        }
    }

    //==========================================================================
    void InputCapture::run()
    {
        using Clock = std::chrono::steady_clock;

        auto lastFlush = Clock::now();
        auto sidecarOwed = true;

        std::unique_lock<std::mutex> lock { mutex };

        for (;;)
        {
            /*  POLLED, NOT WOKEN: the audio thread signals nothing (the class
                comment says why). Five milliseconds against a ring seconds
                deep. */
            wake.wait_for (lock, std::chrono::milliseconds (5), [this] { return stopping; });

            const auto finishing = stopping;
            lock.unlock();

            const auto hadStarted = started.load (std::memory_order_relaxed);

            if (! held.load (std::memory_order_acquire) || finishing)
                drain();

            //  The first block's sample said as soon as it is known.
            if (! hadStarted && started.load (std::memory_order_relaxed))
                sidecarOwed = true;

            if (finishing)
                break;

            /*  THE HEADER AND THE SIDECAR EVERY SECOND, so a process killed
                without its destructors leaves a WAV that says its length and a
                sidecar that says where it begins. */
            if (const auto now = Clock::now(); sidecarOwed || now - lastFlush >= std::chrono::seconds (1))
            {
                if (now - lastFlush >= std::chrono::seconds (1) && file != nullptr && file->writer != nullptr)
                {
                    file->writer->flush();
                    lastFlush = now;
                }

                writeSidecar (false);
                sidecarOwed = false;
            }

            lock.lock();
        }
    }

    void InputCapture::drain()
    {
        /*  Nothing after a clock that moved: what the ring still holds, or a
            block the audio thread filled as the end was being said, belongs to
            another stretch of the clock. */
        if (file == nullptr || file->writer == nullptr || axisBroken)
        {
            emptied.store (filled.load (std::memory_order_acquire), std::memory_order_release);
            return;
        }

        const auto upTo = filled.load (std::memory_order_acquire);
        auto at = emptied.load (std::memory_order_relaxed);
        const auto stride = static_cast<std::size_t> (channelCount) * static_cast<std::size_t> (slotFrames);

        while (at < upTo)
        {
            const auto index = static_cast<std::size_t> (at % slotCount);
            const Slot slot = slots[index];

            if (! started.load (std::memory_order_relaxed))
            {
                expected = slot.sample;
                droppedAccounted = slot.droppedBefore;
                first.store (slot.sample, std::memory_order_release);
                started.store (true, std::memory_order_release);
            }

            /*  WHERE THIS SLOT SITS AGAINST THE LAST: right after it, or after
                exactly the frames dropped since - a hole, written as silence so
                the file keeps the axis. Anything else is a clock that moved, and
                one file is one stretch of one clock. */
            const auto gap = slot.sample - expected;
            const auto missed = slot.droppedBefore - droppedAccounted;

            if (gap != missed || gap < 0)
            {
                finishWith ("the audio clock moved: a block at sample " + std::to_string (slot.sample)
                            + " where " + std::to_string (expected) + " was due, "
                            + std::to_string (missed) + " frames dropped since");
                axisBroken = true;
                emptied.store (upTo, std::memory_order_release);
                return;
            }

            for (auto hole = gap; hole > 0;)
            {
                const auto frames = static_cast<int> (std::min<std::int64_t> (hole, slotFrames));

                for (auto& pointer : pointers)
                    pointer = silence.data();

                file->writer->writeFromFloatArrays (pointers.data(), channelCount, frames);
                framesInFile.fetch_add (frames, std::memory_order_release);
                hole -= frames;
            }

            const float* const base = samples.data() + index * stride;

            for (int channel = 0; channel < channelCount; ++channel)
                pointers[static_cast<std::size_t> (channel)] = base + static_cast<std::size_t> (channel)
                                                                        * static_cast<std::size_t> (slotFrames);

            file->writer->writeFromFloatArrays (pointers.data(), channelCount, slot.frames);
            framesInFile.fetch_add (slot.frames, std::memory_order_release);

            expected = slot.sample + slot.frames;
            droppedAccounted = slot.droppedBefore;

            ++at;
            emptied.store (at, std::memory_order_release);
        }
    }

    //==========================================================================
    void InputCapture::writeSidecar (bool closed)
    {
        auto* object = new juce::DynamicObject();
        const juce::var root { object };

        juce::Array<juce::var> numbers;

        for (const auto number : inputs())
            numbers.add (number);

        std::string why;

        {
            const std::lock_guard<std::mutex> lock { mutex };
            why = reason;
        }

        object->setProperty ("file", juce::String::fromUTF8 (wanted.file.c_str()));
        object->setProperty ("sampleRate", rate);
        object->setProperty ("blockSize", block);

        /*  THE SAMPLE OF THE FILE'S FIRST FRAME, on the count launches are
            placed on - the audio side's own, which `/godot/run/<id>/launchSample`
            reads - or null until a block has landed. */
        object->setProperty ("firstSample", started.load (std::memory_order_acquire)
                                              ? juce::var (static_cast<juce::int64> (first.load (std::memory_order_acquire)))
                                              : juce::var());
        object->setProperty ("count", "the audio side's own: the count /godot/run/<id>/launchSample reads");
        object->setProperty ("inputs", numbers);
        object->setProperty ("deviceInputs", deviceWidth);
        object->setProperty ("frames", static_cast<juce::int64> (framesInFile.load (std::memory_order_acquire)));
        object->setProperty ("droppedFrames", static_cast<juce::int64> (dropped.load (std::memory_order_acquire)));
        object->setProperty ("closed", closed);
        object->setProperty ("ended", juce::String::fromUTF8 (why.c_str()));

        /*  Through a temporary file and a move, so a reader never meets half
            of one. A reader holding the old one open on Windows makes the move
            fail; the next second writes it again. */
        const juce::File sidecar { juce::String::fromUTF8 (sidecarPath().c_str()) };
        sidecar.replaceWithText (juce::JSON::toString (root) + "\n");
    }

    bool InputCapture::refuse (const std::string& why)
    {
        const std::lock_guard<std::mutex> lock { mutex };
        error = why;
        return false;
    }

    void InputCapture::finishWith (const std::string& why)
    {
        {
            const std::lock_guard<std::mutex> lock { mutex };

            if (reason.empty())
                reason = why;
        }

        ended.store (true, std::memory_order_release);
    }

    void InputCapture::end (const std::string& why)
    {
        if (! prepared.load (std::memory_order_acquire))
            return;

        finishWith (why);
    }

    void InputCapture::holdWriter (bool hold) noexcept
    {
        held.store (hold, std::memory_order_release);
    }

    void InputCapture::stop()
    {
        if (! writer.joinable())
            return;

        {
            const std::lock_guard<std::mutex> lock { mutex };
            stopping = true;
        }

        wake.notify_all();
        writer.join();

        if (file != nullptr && file->writer != nullptr)
        {
            file->writer->flush();
            file->writer.reset();       // closes the file, its header whole
        }

        writeSidecar (true);
    }
}
