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

#include <wfg/engine/audio/MediaAnalyser.h>

#include <wfg/engine/audio/MediaInfo.h>
#include <wfg/engine/audio/Timbre.h>
#include <wfg/engine/document/Bundle.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>

namespace wfg::audio
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        double millisecondsSince (Clock::time_point start)
        {
            return std::chrono::duration<double, std::milli> (Clock::now() - start).count();
        }

        bool stopRequested (const std::atomic<bool>* stop) noexcept
        {
            return stop != nullptr && stop->load (std::memory_order_relaxed);
        }

        /*  A STREAM THAT ENDS WHEN THE ANALYSER IS TOLD TO STOP. `juce::SHA256`
            has no way to be interrupted - it reads its stream to the end - and
            a gigabyte of WAV is seconds of hashing that Ctrl-C would otherwise
            sit through. So the stream it reads says "no more" the moment the
            flag is up; the digest it then finishes is of a prefix, and is
            thrown away by the caller, which looks at the flag before it looks
            at the hash. */
        class StoppableInputStream final : public juce::InputStream
        {
        public:
            StoppableInputStream (juce::InputStream& sourceToRead, const std::atomic<bool>* stopFlag)
                : source (sourceToRead), stop (stopFlag)
            {
            }

            juce::int64 getTotalLength() override         { return source.getTotalLength(); }
            bool isExhausted() override                   { return stopRequested (stop) || source.isExhausted(); }
            juce::int64 getPosition() override            { return source.getPosition(); }
            bool setPosition (juce::int64 position) override { return source.setPosition (position); }

            int read (void* destination, int maximumBytes) override
            {
                return stopRequested (stop) ? 0 : source.read (destination, maximumBytes);
            }

        private:
            juce::InputStream& source;
            const std::atomic<bool>* stop;
        };

        /*  JUCE's SHA256 asks its stream for 64 bytes at a time, and a bare
            `FileInputStream` answers each ask with a read from the operating
            system - sixteen million of them for a gigabyte. `SHA256 (const
            File&)`, the other spelling §14.12 allowed, is that bare stream. A
            buffer of 64 KB in front of it turns them into sixteen thousand. */
        constexpr int hashBufferBytes = 1 << 16;

        /*  THE BYTES, streamed - never the whole file in memory, which is the
            shape `Bundle::contentHash` has and §14.12 says not to copy for
            media. Empty when the file cannot be opened. */
        std::string hashOf (const juce::File& file, const std::atomic<bool>* stop)
        {
            juce::FileInputStream input { file };

            if (! input.openedOk())
                return {};

            juce::BufferedInputStream buffered { input, hashBufferBytes };
            StoppableInputStream stoppable { buffered, stop };

            const juce::SHA256 digest { stoppable };
            return digest.toHexString().toStdString();
        }

        std::shared_ptr<const TimbrePyramid> readCache (const juce::File& cacheFile)
        {
            if (! cacheFile.existsAsFile())
                return nullptr;

            juce::MemoryBlock bytes;

            if (! cacheFile.loadFileAsData (bytes))
                return nullptr;

            TimbrePyramid pyramid;

            if (! timbre::read (static_cast<const std::uint8_t*> (bytes.getData()), bytes.getSize(), pyramid))
                return nullptr;

            return std::make_shared<const TimbrePyramid> (std::move (pyramid));
        }

        /*  REPLACED WHOLE, BUT NOT MADE DURABLE - which is the one way this
            differs from `Bundle::save`'s write, and on purpose. A show is
            somebody's work and is flushed to the disk before its name moves; a
            pyramid is arithmetic anybody can repeat, and M23 found two durable
            replaces cost the Windows box nineteen milliseconds, nearly all of
            it flushing and replacing - a share an import of hundreds of files
            has no show's sake to pay. So the bytes go to a sibling temp named
            as the bundle names its own (`Bundle::temporaryFor`), and the temp
            takes the target's place. A power cut at the wrong moment can leave
            a file of the right length and the wrong bytes; its checksum refuses
            it (Timbre.h), and the next analysis builds it again.

            False, with nothing left behind, when anything fails - a file where
            the `.timbre` folder should be, a read-only share, a full disk. */
        bool writeCache (const juce::File& target, const std::vector<std::uint8_t>& bytes)
        {
            if (target.getParentDirectory().createDirectory().failed())
                return false;

            const auto temp = doc::Bundle::temporaryFor (target);

            /*  Scoped, so the stream is closed before the replace - Windows
                will not move a file this process still holds open. Its
                destructor writes the buffer out and does not flush the disk. */
            {
                juce::FileOutputStream stream { temp };

                if (! stream.openedOk())
                    return false;

                if (stream.getPosition() > 0)
                {
                    stream.setPosition (0);

                    if (stream.truncate().failed())
                        return false;
                }

                if (! stream.write (bytes.data(), bytes.size()))
                {
                    temp.deleteFile();
                    return false;
                }
            }

            if (temp.getSize() != static_cast<juce::int64> (bytes.size()) || ! temp.replaceFileIn (target))
            {
                temp.deleteFile();
                return false;
            }

            return true;
        }

        void describeInto (MediaAnalysis& analysis, const std::shared_ptr<const TimbrePyramid>& pyramid)
        {
            analysis.pyramid = pyramid;
            analysis.frames = pyramid->frames();
            analysis.levels = pyramid->levels.size();

            if (pyramid->sampleRate > 0)
                analysis.seconds = static_cast<double> (pyramid->samples) / pyramid->sampleRate;
        }
    }

    //==============================================================================
    const char* describe (MediaAnalysis::Outcome outcome) noexcept
    {
        switch (outcome)
        {
            case MediaAnalysis::Outcome::built:       return "built";
            case MediaAnalysis::Outcome::cached:      return "cached";
            case MediaAnalysis::Outcome::inMemory:    return "memory";
            case MediaAnalysis::Outcome::missing:     return "missing";
            case MediaAnalysis::Outcome::unreadable:  return "unreadable";
            case MediaAnalysis::Outcome::stopped:     return "stopped";
        }

        return "unknown";
    }

    std::string timbreCacheFolder (const std::string& mediaFolder)
    {
        if (mediaFolder.empty())
            return {};

        return juce::File (juce::String (mediaFolder)).getChildFile (".timbre")
                   .getFullPathName().toStdString();
    }

    MediaAnalysis analyseMediaFile (const std::string& mediaFolder, const std::string& named,
                                    bool force, const std::atomic<bool>* stop)
    {
        MediaAnalysis analysis;

        const auto path = resolveMediaPath (mediaFolder, named);
        const juce::File file { juce::String (path) };

        if (named.empty() || ! file.existsAsFile())
        {
            analysis.outcome = MediaAnalysis::Outcome::missing;
            return analysis;
        }

        const auto hashing = Clock::now();
        analysis.contentHash = hashOf (file, stop);
        analysis.hashMilliseconds = millisecondsSince (hashing);

        /*  The flag BEFORE the hash: a stopped hash is of a prefix. */
        if (stopRequested (stop))
        {
            analysis.contentHash.clear();
            analysis.outcome = MediaAnalysis::Outcome::stopped;
            return analysis;
        }

        if (analysis.contentHash.empty())
        {
            analysis.outcome = MediaAnalysis::Outcome::unreadable;
            return analysis;
        }

        const auto working = Clock::now();

        const auto cacheFolder = timbreCacheFolder (mediaFolder);
        const auto cacheFile = cacheFolder.empty()
                                 ? juce::File()
                                 : juce::File (juce::String (cacheFolder))
                                       .getChildFile (juce::String (analysis.contentHash) + ".tpy");

        /*  A CACHE THAT DOES NOT READ IS A CACHE MISS, not an error: a torn
            file, one written by another version of the analysis, one somebody
            edited - each is built again and replaced. */
        if (! force && cacheFile != juce::File())
        {
            if (const auto cached = readCache (cacheFile))
            {
                describeInto (analysis, cached);
                analysis.outcome = MediaAnalysis::Outcome::cached;
                analysis.bytesOnDisk = cacheFile.getSize();
                analysis.analysisMilliseconds = millisecondsSince (working);
                return analysis;
            }
        }

        /*  THE FORMATS THE DURATIONS ARE READ WITH (`mediaDurationSeconds`),
            one manager per file for the same reason: this is not a hot path,
            and a shared one would need a lock to be honest about which thread
            asked. */
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        const std::unique_ptr<juce::AudioFormatReader> reader { formats.createReaderFor (file) };

        if (reader == nullptr || ! (reader->sampleRate > 0.0) || reader->numChannels == 0)
        {
            analysis.outcome = MediaAnalysis::Outcome::unreadable;
            analysis.analysisMilliseconds = millisecondsSince (working);
            return analysis;
        }

        const auto channels = static_cast<int> (reader->numChannels);
        juce::AudioBuffer<float> stretch { channels, timbre::hopSize };
        timbre::Analyser analyser { reader->sampleRate };

        /*  A HOP AT A TIME, so an hour of audio is never in memory - the
            finest level of its pyramid is, at four bytes a frame. */
        for (juce::int64 start = 0; start < reader->lengthInSamples; start += timbre::hopSize)
        {
            if (stopRequested (stop))
            {
                analysis.contentHash.clear();
                analysis.outcome = MediaAnalysis::Outcome::stopped;
                return analysis;
            }

            const auto count = static_cast<int> (std::min<juce::int64> (timbre::hopSize,
                                                                         reader->lengthInSamples - start));
            stretch.clear();

            if (! reader->read (stretch.getArrayOfWritePointers(), channels, start, count))
            {
                analysis.outcome = MediaAnalysis::Outcome::unreadable;
                analysis.analysisMilliseconds = millisecondsSince (working);
                return analysis;
            }

            analyser.add (stretch.getArrayOfReadPointers(), channels, count);
        }

        const auto pyramid = std::make_shared<const TimbrePyramid> (analyser.finish());
        describeInto (analysis, pyramid);
        analysis.framesAnalysed = pyramid->frames();

        /*  The seconds the READER says, not the ones the pyramid's rounded
            rate would give back: an odd rate is not rounded into a length. */
        analysis.seconds = static_cast<double> (reader->lengthInSamples) / reader->sampleRate;

        if (cacheFile != juce::File() && writeCache (cacheFile, timbre::write (*pyramid)))
        {
            analysis.outcome = MediaAnalysis::Outcome::built;
            analysis.bytesOnDisk = cacheFile.getSize();
        }
        else
        {
            /*  NOTHING IS SAID HERE, and that is §14.12's rule rather than an
                omission: the session has its colours, and a diagnostic about a
                cache nobody asked for, on the night the show runs off a share,
                would be noise. The verb, which WAS asked, prints the word. */
            analysis.outcome = MediaAnalysis::Outcome::inMemory;
            analysis.bytesOnDisk = 0;
        }

        analysis.analysisMilliseconds = millisecondsSince (working);
        return analysis;
    }

    //==============================================================================
    MediaAnalyser::MediaAnalyser (MediaInfo& mediaToPublishInto, std::string mediaFolder)
        : media (&mediaToPublishInto), folder (std::move (mediaFolder))
    {
    }

    MediaAnalyser::~MediaAnalyser()
    {
        stop();
    }

    bool MediaAnalyser::start()
    {
        if (running.load (std::memory_order_relaxed))
            return false;

        stopping.store (false, std::memory_order_relaxed);
        running.store (true, std::memory_order_relaxed);

        thread = std::thread ([this] { run(); });
        return true;
    }

    void MediaAnalyser::stop()
    {
        if (! running.load (std::memory_order_relaxed))
            return;

        /*  THE FLAG IS RAISED UNDER THE LOCK, and the one thing this does not
            copy from `MountProbe`. The thread tests the flag while holding the
            lock and releases it only by going to sleep, so a flag raised under
            the same lock is either seen by that test or raised after the thread
            is asleep - when the notify below wakes it. Raised without the lock,
            it can land between the thread's test and its sleep, the notify
            finds nobody waiting, and `join` waits for ever. */
        {
            const std::lock_guard<std::mutex> lock { guard };
            stopping.store (true, std::memory_order_relaxed);
        }

        wake.notify_all();

        if (thread.joinable())
            thread.join();

        running.store (false, std::memory_order_relaxed);

        const std::lock_guard<std::mutex> lock { guard };
        queued.clear();
        pending = 0;
    }

    bool MediaAnalyser::queue (const std::string& named)
    {
        if (named.empty())
            return false;

        {
            const std::lock_guard<std::mutex> lock { guard };

            if (! seen.insert (named).second)
                return false;

            queued.push_back (named);
            ++pending;
        }

        wake.notify_one();
        return true;
    }

    std::size_t MediaAnalyser::outstanding() const
    {
        const std::lock_guard<std::mutex> lock { guard };
        return pending;
    }

    //==============================================================================
    void MediaAnalyser::run()
    {
        for (;;)
        {
            std::string named;

            {
                std::unique_lock<std::mutex> lock { guard };

                wake.wait (lock, [this] { return stopping.load (std::memory_order_relaxed)
                                                 || ! queued.empty(); });

                if (stopping.load (std::memory_order_relaxed))
                    return;

                named = std::move (queued.front());
                queued.pop_front();
            }

            /*  OUTSIDE THE LOCK, because this is the part that takes seconds,
                and `queue` is called from the tick thread.

                AND INSIDE A CATCH, which nothing else on this side of the
                engine needs: this is the one thread that reads bytes a show
                merely NAMES, and a decoder that throws on one malformed file
                must cost that file its colours - not stop a performance by
                taking the process down with it. */
            MediaAnalysis analysis;

            try
            {
                analysis = analyseMediaFile (folder, named, false, &stopping);
            }
            catch (const std::exception&)
            {
                analysis = MediaAnalysis {};
            }

            if (stopping.load (std::memory_order_relaxed))
                return;

            /*  NOTHING IS PUBLISHED FOR A FILE WITHOUT A PYRAMID, so a record
                either has its hash and its pyramid or has neither - the one
                state the tree must not have to read is a hash that a client
                would ask the route for and be refused. */
            if (analysis.pyramid != nullptr && media != nullptr)
            {
                MediaRecord record;
                record.seconds = analysis.seconds;
                record.contentHash = analysis.contentHash;
                record.pyramid = analysis.pyramid;

                media->publish (named, std::move (record));
            }

            /*  AFTER the publish, so `outstanding` never reads nought while a
                record it counted is still on its way. */
            const std::lock_guard<std::mutex> lock { guard };

            if (pending > 0)
                --pending;
        }
    }
}
