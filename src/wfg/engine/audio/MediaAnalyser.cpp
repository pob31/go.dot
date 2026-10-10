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
#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/video/Movie.h>
#include <wfg/engine/video/StripAnalysis.h>
#include <wfg/engine/video/Strip.h>

#include <wfg/engine/audio/MediaInfo.h>
#include <wfg/engine/audio/Peaks.h>
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
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>
#include <juce_cryptography/juce_cryptography.h>

namespace wfg::audio
{
    namespace
    {
        /*  A STILL PICTURE'S NAME, by its extension - the kinds a video cue's
            picture is (client/model/Video's list). */
        /*  A MOVIE'S STRIP, FROM ITS CACHE OR FOUND (namespace draft §47, AAI).
            Kept as `<key>.tms` in the media's `.timbre` folder, the key a hash
            of the file's size and its first and last megabyte: hashing the
            whole of a movie of many gigabytes would cost more than finding its
            strip. A strip found is written beside the others, through a part
            file renamed when whole. Nothing for a movie that is not HAP. */
        std::shared_ptr<const video::strip::MovieStrip> movieStripOf (const std::string& folder, const std::string& named,
                                                                      const std::string& path,
                                                                      const std::atomic<bool>& stopping)
        {
            const auto key = movieStripKey (path);

            if (key.empty())
                return nullptr;

            const auto cacheFolder = timbreCacheFolder (mediaRootOf (folder, named));
            const auto cached = cacheFolder.empty() ? juce::File()
                                                    : juce::File (juce::String (cacheFolder)).getChildFile (juce::String (key) + ".tms");

            if (cached.existsAsFile())
            {
                juce::MemoryBlock bytes;

                if (cached.loadFileAsData (bytes))
                {
                    auto strip = std::make_shared<video::strip::MovieStrip>();

                    if (video::strip::decode (static_cast<const std::uint8_t*> (bytes.getData()), bytes.getSize(), *strip))
                        return strip;
                }
            }

            auto strip = std::make_shared<video::strip::MovieStrip>();
            std::string why;

            if (! video::strip::analyseHap (path, *strip, why, &stopping))
                return nullptr;

            if (cached != juce::File() && cached.getParentDirectory().createDirectory())
            {
                const auto bytes = video::strip::encode (*strip);
                const auto part = cached.withFileExtension ("tms.part");

                if (part.replaceWithData (bytes.data(), bytes.size()))
                    part.moveFileTo (cached);
            }

            return strip;
        }

        bool isStillPictureName (const std::string& name)
        {
            const auto dot = name.find_last_of ('.');

            if (dot == std::string::npos)
                return false;

            auto extension = name.substr (dot + 1);

            for (auto& c : extension)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char> (c - 'A' + 'a');

            return extension == "png" || extension == "jpg" || extension == "jpeg" || extension == "gif";
        }
    }

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

        /*  The finer level's file beside the pyramid's (Peaks.h): read the
            same way, and a miss the same way - nothing, and built again. */
        std::shared_ptr<const PeakTrack> readPeaks (const juce::File& peaksFile)
        {
            if (! peaksFile.existsAsFile())
                return nullptr;

            juce::MemoryBlock bytes;

            if (! peaksFile.loadFileAsData (bytes))
                return nullptr;

            PeakTrack track;

            if (! peaks::read (static_cast<const std::uint8_t*> (bytes.getData()), bytes.getSize(), track))
                return nullptr;

            return std::make_shared<const PeakTrack> (std::move (track));
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

        const auto cacheFolder = timbreCacheFolder (mediaRootOf (mediaFolder, named));
        const auto cacheFile = cacheFolder.empty()
                                 ? juce::File()
                                 : juce::File (juce::String (cacheFolder))
                                       .getChildFile (juce::String (analysis.contentHash) + ".tpy");

        const auto peaksFile = cacheFile == juce::File()
                                 ? juce::File()
                                 : cacheFile.withFileExtension ("tpk");

        /*  A CACHE THAT DOES NOT READ IS A CACHE MISS, not an error: a torn
            file, one written by another version of the analysis, one somebody
            edited - each is built again and replaced. BOTH FILES, or neither:
            one pass makes the colours and the finer level together. */
        if (! force && cacheFile != juce::File())
        {
            if (const auto cached = readCache (cacheFile))
            {
                if (const auto level = readPeaks (peaksFile))
                {
                    describeInto (analysis, cached);
                    analysis.peaks = level;
                    analysis.outcome = MediaAnalysis::Outcome::cached;
                    analysis.bytesOnDisk = cacheFile.getSize() + peaksFile.getSize();
                    analysis.analysisMilliseconds = millisecondsSince (working);
                    return analysis;
                }
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
        peaks::Collector level;

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
            level.add (stretch.getArrayOfReadPointers(), channels, count);
        }

        const auto pyramid = std::make_shared<const TimbrePyramid> (analyser.finish());
        describeInto (analysis, pyramid);
        analysis.peaks = std::make_shared<const PeakTrack> (level.finish (pyramid->sampleRate));
        analysis.framesAnalysed = pyramid->frames();

        /*  The seconds the READER says, not the ones the pyramid's rounded
            rate would give back: an odd rate is not rounded into a length. */
        analysis.seconds = static_cast<double> (reader->lengthInSamples) / reader->sampleRate;

        if (cacheFile != juce::File() && writeCache (cacheFile, timbre::write (*pyramid))
             && writeCache (peaksFile, peaks::write (*analysis.peaks)))
        {
            analysis.outcome = MediaAnalysis::Outcome::built;
            analysis.bytesOnDisk = cacheFile.getSize() + peaksFile.getSize();
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

    std::string movieStripKey (const std::string& path)
    {
        const juce::File file { juce::String (path) };
        const auto size = file.getSize();

        if (size <= 0)
            return {};

        juce::MemoryBlock keyed;
        keyed.append (juce::String (size).toRawUTF8(), static_cast<std::size_t> (juce::String (size).getNumBytesAsUTF8()));

        {
            juce::FileInputStream in { file };

            if (! in.openedOk())
                return {};

            constexpr juce::int64 mebibyte = 1 << 20;
            in.readIntoMemoryBlock (keyed, std::min<juce::int64> (size, mebibyte));

            if (size > 2 * mebibyte)
            {
                in.setPosition (size - mebibyte);
                in.readIntoMemoryBlock (keyed, mebibyte);
            }
        }

        keyed.append (&video::strip::formatVersion, sizeof (video::strip::formatVersion));

        return juce::SHA256 (keyed).toHexString().toStdString();
    }

    namespace
    {
        /*  THE SOUNDS' HASHES, KEPT BETWEEN SESSIONS in `.timbre/sounds.index`
            (namespace draft §52): a line a sound - its size, when it was last
            written, its hash and its path inside the media folder, a tab
            between each - so a sweep reads again only a sound that is new or
            has changed. Without it every sound in the folder, a cue naming it
            or not, would be read whole at every launch. A line that does not
            read is passed by: the sound is hashed again, which is all a lost
            index ever costs. */
        constexpr const char* indexName = "sounds.index";

        void readIndex (const juce::File& folder, const juce::File& cache, KnownHashes& known)
        {
            juce::StringArray lines;
            lines.addLines (cache.getChildFile (indexName).loadFileAsString());

            for (const auto& line : lines)
            {
                const auto fields = juce::StringArray::fromTokens (line, "\t", "");

                if (fields.size() != 4 || fields[2].length() != 64)
                    continue;

                const auto path = folder.getChildFile (fields[3]).getFullPathName().toStdString();

                if (known.find (path) == known.end())
                    known[path] = { fields[0].getLargeIntValue(), fields[1].getLargeIntValue(), fields[2].toStdString() };
            }
        }

        void writeIndex (const juce::File& cache, const std::vector<std::pair<juce::String, KnownHash>>& sounds)
        {
            juce::String text;

            for (const auto& [relative, hash] : sounds)
                text << juce::String (hash.size) << "\t" << juce::String (hash.modified) << "\t"
                     << juce::String (hash.hash) << "\t" << relative << "\n";

            const auto bytes = text.toStdString();
            writeCache (cache.getChildFile (indexName), std::vector<std::uint8_t> (bytes.begin(), bytes.end()));
        }
    }

    CacheSweep sweepAnalysisCache (const std::string& root, KnownHashes& known, std::int64_t before,
                                   const std::atomic<bool>* stop)
    {
        CacheSweep result;

        const juce::File folder { juce::String (root) };
        const auto cache = folder.getChildFile (".timbre");

        /*  NO CACHE, NOTHING TO SWEEP - and no sound in the folder is read. */
        if (root.empty() || ! cache.isDirectory())
        {
            result.swept = true;
            return result;
        }

        try
        {
            readIndex (folder, cache, known);

            juce::AudioFormatManager formats;
            formats.registerBasicFormats();

            std::set<std::string> sounds, movies;
            std::vector<std::pair<juce::String, KnownHash>> indexed;

            const auto refuse = [&result] (const juce::String& relative)
            {
                result.problem = "could not read " + relative.toStdString();
                return result;
            };

            //  EVERY FILE IN THE FOLDER, a cue naming it or not: presence is the rule.
            for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*", juce::File::findFiles))
            {
                if (stopRequested (stop))
                {
                    result.problem = "stopped";
                    return result;
                }

                const auto file = entry.getFile();
                const auto relative = file.getRelativePathFrom (folder).replaceCharacter ('\\', '/');
                const auto name = file.getFileName();

                /*  The cache itself, and what is still being written - a take's
                    hidden part, a save's or a conversion's temporary. */
                if (relative.startsWith (".timbre/") || name.startsWith (".") || name.contains (".tmp-")
                      || name.endsWith (".part"))
                    continue;

                const auto path = file.getFullPathName().toStdString();

                //  A movie by its strip's key, as the analyser keys it: first.
                if (video::movie::isMovieName (name.toStdString()))
                {
                    const auto key = movieStripKey (path);

                    if (key.empty())
                        return refuse (relative);

                    movies.insert (key);
                    continue;
                }

                //  A sound by its hash; anything no format reads has no analysis.
                if (formats.findFormatForFileExtension (file.getFileExtension()) == nullptr)
                    continue;

                const auto size = file.getSize();
                const auto modified = file.getLastModificationTime().toMilliseconds();
                const auto found = known.find (path);

                KnownHash memo;

                if (found != known.end() && found->second.size == size && found->second.modified == modified
                      && ! found->second.hash.empty())
                {
                    memo = found->second;
                }
                else
                {
                    auto hash = hashOf (file, stop);

                    if (stopRequested (stop))
                    {
                        result.problem = "stopped";
                        return result;
                    }

                    if (hash.empty())
                        return refuse (relative);

                    memo = { size, modified, std::move (hash) };
                    known[path] = memo;
                }

                sounds.insert (memo.hash);
                indexed.emplace_back (relative, memo);
            }

            /*  WHAT NO FILE HAS ANY MORE, in `.timbre` alone, written before
                the sweep began - an hour before it for a temporary. Gathered
                first and removed after, so the folder is not changed under
                the walk of it. */
            constexpr std::int64_t hour = 60 * 60 * 1000;
            std::vector<juce::File> stale;

            for (const auto& entry : juce::RangedDirectoryIterator (cache, false, "*", juce::File::findFiles))
            {
                const auto file = entry.getFile();
                const auto name = file.getFileName();
                const auto stem = name.upToFirstOccurrenceOf (".", false, false).toStdString();
                const auto modified = file.getLastModificationTime().toMilliseconds();

                if (name.contains (".tpy.tmp-") || name.contains (".tpk.tmp-") || name.endsWith (".tms.part"))
                {
                    if (modified < before - hour)
                        stale.push_back (file);
                }
                else if (name.endsWith (".tpy") || name.endsWith (".tpk"))
                {
                    if (sounds.count (stem) == 0 && modified < before)
                        stale.push_back (file);
                }
                else if (name.endsWith (".tms"))
                {
                    if (movies.count (stem) == 0 && modified < before)
                        stale.push_back (file);
                }
            }

            for (const auto& file : stale)
            {
                const auto bytes = file.getSize();

                if (file.deleteFile())
                {
                    ++result.removed;
                    result.bytes += bytes;
                }
            }

            writeIndex (cache, indexed);
            result.swept = true;
        }
        catch (const std::exception& failure)
        {
            result.swept = false;
            result.problem = failure.what();
        }

        return result;
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

    void MediaAnalyser::sweep (bool asked)
    {
        {
            const std::lock_guard<std::mutex> lock { guard };
            sweepWanted = true;
            sweepAsked = sweepAsked || asked;
        }

        wake.notify_one();
    }

    MediaAnalyser::SweepStatus MediaAnalyser::sweepStatus() const
    {
        const std::lock_guard<std::mutex> lock { guard };
        return swept;
    }

    std::string MediaAnalyser::sweepText (const SweepStatus& status)
    {
        if (status.number == 0)
            return {};

        return std::to_string (status.number) + "\t" + status.state + "\t" + (status.asked ? "asked" : "auto")
             + "\t" + std::to_string (status.removed) + "\t" + std::to_string (status.bytes) + "\t" + status.problem;
    }

    /*  THE SHOW'S OWN `media/` AND THE ONE AROUND IT, each with the analysis
        it holds (`mediaRootOf` puts a file's beside the file). Nothing is
        said while it runs: a sweep nobody asked for is the machine's own
        housekeeping, and the window says only the end of one somebody did. */
    void MediaAnalyser::runSweep (bool asked)
    {
        const auto before = juce::Time::currentTimeMillis();

        SweepStatus done;
        done.asked = asked;
        done.state = "done";

        if (! folder.empty())
        {
            std::vector<std::string> roots { folder };
            const auto around = mediaFolderAround (folder);

            if (! around.empty() && juce::File (juce::String (around)) != juce::File (juce::String (folder))
                  && juce::File (juce::String (around)).isDirectory())
                roots.push_back (around);

            for (const auto& root : roots)
            {
                const auto result = sweepAnalysisCache (root, hashed, before, &stopping);

                if (stopping.load (std::memory_order_relaxed))
                    return;

                done.removed += result.removed;
                done.bytes += result.bytes;

                if (! result.swept)
                {
                    done.state = "skipped";

                    if (done.problem.empty())
                        done.problem = result.problem;
                }
            }
        }

        const std::lock_guard<std::mutex> lock { guard };
        done.number = swept.number;
        swept = done;
        changes.fetch_add (1, std::memory_order_acq_rel);
    }

    //==============================================================================
    void MediaAnalyser::run()
    {
        for (;;)
        {
            std::string named;
            auto sweeping = false, asked = false;

            {
                std::unique_lock<std::mutex> lock { guard };

                wake.wait (lock, [this] { return stopping.load (std::memory_order_relaxed)
                                                 || ! queued.empty() || sweepWanted; });

                if (stopping.load (std::memory_order_relaxed))
                    return;

                /*  THE FILES BEFORE A SWEEP (namespace draft §52): their
                    colours are what somebody is waiting to see. */
                if (! queued.empty())
                {
                    named = std::move (queued.front());
                    queued.pop_front();
                }
                else
                {
                    sweeping = true;
                    asked = sweepAsked;
                    sweepWanted = sweepAsked = false;

                    swept = SweepStatus {};
                    swept.number = ++sweeps;
                    swept.state = "sweeping";
                    swept.asked = asked;
                    changes.fetch_add (1, std::memory_order_acq_rel);
                }
            }

            if (sweeping)
            {
                try
                {
                    runSweep (asked);
                }
                catch (const std::exception&)
                {
                }

                continue;
            }

            /*  OUTSIDE THE LOCK, because this is the part that takes seconds,
                and `queue` is called from the tick thread.

                AND INSIDE A CATCH, which nothing else on this side of the
                engine needs: this is the one thread that reads bytes a show
                merely NAMES, and a decoder that throws on one malformed file
                must cost that file its colours - not stop a performance by
                taking the process down with it. */
            /*  A MOVIE IS NOT ANALYSED (Phase 8b): it has no waveform. Its
                length is read from its index and published alone - the shape
                every file has before its analysis, with no hash and no
                pyramid. */
            if (video::movie::isMovieName (named))
            {
                const auto path = resolveMediaPath (folder, named);
                const auto seconds = video::movie::durationOf (path);

                if (seconds > 0.0 && media != nullptr)
                {
                    MediaRecord record;
                    record.seconds = seconds;

                    /*  AND ITS SIZE (namespace draft §47, AAG), from the same
                        index, for the picture panel's frame. A movie Go.dot
                        does not play itself is nought, drawn the canvas's shape. */
                    video::movie::MovieFile file;
                    std::string why;

                    if (file.open (path, why))
                    {
                        record.width = file.info().width;
                        record.height = file.info().height;
                    }

                    media->publish (named, record);

                    /*  AND ITS STRIP (namespace draft §47, AAI): kept beside the
                        analysis of sounds, found once and read back after -
                        published again, whole, when it is there. */
                    if (auto strip = movieStripOf (folder, named, path, stopping); strip != nullptr)
                    {
                        record.strip = std::move (strip);
                        media->publish (named, std::move (record));
                    }
                }

                const std::lock_guard<std::mutex> lock { guard };

                if (pending > 0)
                    --pending;

                continue;
            }

            /*  A STILL PICTURE (namespace draft §47, AAG): no length and no
                waveform, only its size, read by decoding it - a second at the
                worst, on this thread, once. */
            if (isStillPictureName (named))
            {
                const auto image = juce::ImageFileFormat::loadFrom (juce::File (resolveMediaPath (folder, named)));

                if (image.isValid() && media != nullptr)
                {
                    MediaRecord record;
                    record.width = image.getWidth();
                    record.height = image.getHeight();
                    media->publish (named, std::move (record));
                }

                const std::lock_guard<std::mutex> lock { guard };

                if (pending > 0)
                    --pending;

                continue;
            }

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

            /*  THE HASH KEPT for the sweep (§52), which then need not read
                this sound again. */
            if (! analysis.contentHash.empty())
            {
                const juce::File file { juce::String (resolveMediaPath (folder, named)) };
                hashed[file.getFullPathName().toStdString()] = { file.getSize(),
                                                                 file.getLastModificationTime().toMilliseconds(),
                                                                 analysis.contentHash };
            }

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
                record.peaks = analysis.peaks;

                media->publish (named, std::move (record));
            }

            /*  AFTER the publish, so `outstanding` never reads nought while a
                record it counted is still on its way. */
            const std::lock_guard<std::mutex> lock { guard };

            if (pending > 0)
                --pending;
        }
    }

    //==============================================================================
    void registerAnalyserCommands (CommandRegistry& registry, MediaAnalyser* analyser)
    {
        registry.add ({ "media.cleanCache",
                        "Sweeps the analysis cache (namespace draft 52): in the media folder's .timbre, the"
                        " colours, levels and movie strips of files no longer in the folder are removed - a"
                        " file still there keeps its analysis whether a cue names it or not. Done by itself"
                        " when a show opens; this asks for it now, and /godot/engine/mediaCacheSweep says"
                        " what it removed. Taken and ignored where nothing analyses.",
                        {},
                        false,
                        [analyser] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            if (analyser != nullptr)
                                analyser->sweep (true);

                            return Outcome::ok (args);
                        } });
    }
}
