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

#include <wfg/engine/audio/TakeWriter.h>

#include <wfg/engine/audio/Looper.h>

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace wfg::audio
{
    namespace
    {
        /*  HOW LONG A JOB WAITS FOR THE TAKE TO SETTLE: a closed layer's tail
            and a take's last crossfade are ten milliseconds each, so a second
            is a take that is still being recorded, which is a refusal. */
        constexpr int settleTries = 200;
        constexpr auto settleStep = std::chrono::milliseconds (5);

        /** A second of the take at a time, so a ten-minute take is not a gigabyte in hand. */
        constexpr std::int64_t chunkFrames = 48000;

        /*  A NAME ANY FILE SYSTEM TAKES: the characters Windows refuses and
            every control character made an underscore, and no trailing dot or
            space, which Windows drops. */
        std::string safeStem (const std::string& stem)
        {
            std::string out;

            for (const auto c : stem)
            {
                const auto byte = static_cast<unsigned char> (c);
                const auto refused = byte < 0x20 || c == '\\' || c == '/' || c == ':' || c == '*'
                                     || c == '?' || c == '"' || c == '<' || c == '>' || c == '|';
                out.push_back (refused ? '_' : c);
            }

            while (! out.empty() && (out.back() == '.' || out.back() == ' '))
                out.pop_back();

            return out.empty() ? std::string ("Take") : out;
        }
    }

    TakeWriter::~TakeWriter()
    {
        {
            const std::lock_guard<std::mutex> guard { lock };
            stopping = true;
        }

        wake.notify_all();

        if (thread.joinable())
            thread.join();
    }

    void TakeWriter::queue (Job job)
    {
        {
            const std::lock_guard<std::mutex> guard { lock };
            jobs.push_back (std::move (job));

            if (! thread.joinable())
                thread = std::thread ([this] { run(); });
        }

        wake.notify_one();
    }

    std::vector<TakeWriter::Done> TakeWriter::finished()
    {
        std::vector<Done> out;
        const std::lock_guard<std::mutex> guard { lock };
        out.swap (done);
        return out;
    }

    void TakeWriter::run()
    {
        for (;;)
        {
            Job job;

            {
                std::unique_lock<std::mutex> guard { lock };
                wake.wait (guard, [this] { return stopping || ! jobs.empty(); });

                //  Stopping with nothing left; a job queued before the stop is still written.
                if (jobs.empty())
                    return;

                job = std::move (jobs.front());
                jobs.pop_front();
            }

            auto result = write (job);

            const std::lock_guard<std::mutex> guard { lock };
            done.push_back (std::move (result));
        }
    }

    std::string TakeWriter::freeName (const std::string& folder, const std::string& stem)
    {
        const juce::File where { juce::String::fromUTF8 (folder.c_str()) };
        const auto safe = safeStem (stem);

        for (int number = 1;; ++number)
        {
            const auto name = safe + " take " + std::to_string (number) + ".wav";

            if (! where.getChildFile (juce::String::fromUTF8 (name.c_str())).exists() || number >= 100000)
                return name;
        }
    }

    TakeWriter::Done TakeWriter::write (const Job& job)
    {
        Done out;
        out.channel = job.channel;

        const auto fail = [&out] (const std::string& why)
        {
            out.error = why;
            return out;
        };

        if (job.take == nullptr)
            return fail ("there is no take on this channel");

        /*  STILL FIRST: a take pressed shut a moment ago may be finishing its
            last crossfade, and a closed layer its tail - a few blocks. */
        for (int tries = 0; ! job.take->isSettled() && tries < settleTries; ++tries)
            std::this_thread::sleep_for (settleStep);

        if (! job.take->isSettled())
            return fail ("the take was still being recorded");

        const auto length = job.take->length();
        const auto layers = job.take->layerCount();
        const auto rate = job.take->shape().sampleRate;

        if (length <= 0)
            return fail ("the take is empty");

        const auto folder = juce::File (juce::String::fromUTF8 (job.mediaFolder.c_str())).getChildFile ("takes");

        if (const auto made = folder.createDirectory(); made.failed())
            return fail ("media/takes could not be made: " + made.getErrorMessage().toStdString());

        const auto name = freeName (folder.getFullPathName().toStdString(), job.stem);
        const auto target = folder.getChildFile (juce::String::fromUTF8 (name.c_str()));
        const auto partial = folder.getChildFile ("." + juce::String::fromUTF8 (name.c_str()) + ".part");
        partial.deleteFile();

        {
            std::unique_ptr<juce::OutputStream> stream { partial.createOutputStream() };

            if (stream == nullptr)
                return fail ("media/takes could not be written to");

            juce::WavAudioFormat wav;
            auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                           .withSampleRate (rate)
                                                           .withNumChannels (2)
                                                           .withBitsPerSample (32)
                                                           .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));

            if (writer == nullptr)
            {
                partial.deleteFile();
                return fail ("the WAV writer would not open");
            }

            std::vector<float> left (static_cast<std::size_t> (chunkFrames)), right (static_cast<std::size_t> (chunkFrames));

            for (std::int64_t at = 0; at < length; at += chunkFrames)
            {
                const auto frames = static_cast<int> (std::min (chunkFrames, length - at));
                job.take->copyTake (layers, at, frames, left.data(), right.data());

                const float* channels[] { left.data(), right.data() };

                if (! writer->writeFromFloatArrays (channels, 2, frames))
                {
                    writer.reset();
                    partial.deleteFile();
                    return fail ("the disk would not take the whole take");
                }
            }

            writer.reset();
        }

        //  WHOLE, THEN IN PLACE: a file under media/takes is never half a take.
        if (! partial.moveFileTo (target))
        {
            partial.deleteFile();
            return fail ("the take could not be moved into media/takes");
        }

        out.file = "takes/" + name;
        return out;
    }
}
