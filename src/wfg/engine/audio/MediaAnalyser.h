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

#pragma once

/*
    The colours of a show's media, worked out where nobody has to wait for
    them (PRD §3.30, namespace draft §14.12).

    TWO SHAPES OF ONE PIECE OF WORK. `analyseMediaFile` is the whole of it for
    one file, synchronously: hash the bytes, look for a pyramid already cached
    under that hash, build one if there is none, and try to cache it. `wfg
    analyse` calls it in a loop and prints what each call cost, which is M22's
    instrument. `MediaAnalyser` calls the same function from a thread of its
    own, for `wfg serve`, and publishes each answer into the show's
    `MediaInfo` - so the two can never disagree about what a file looks like.

    THE THREAD IS MOUNTPROBE'S SHAPE, because the problem is: slow work that
    must never be waited for, handed over by a thread that must never wait. A
    mutex, a condition variable, a queue, the work done OUTSIDE the lock, and a
    `stop()` that sets a flag, wakes the thread and joins it. What differs is
    what an answer becomes. A mount's read-back is a state transition and goes
    in the log; a pyramid is arithmetic over bytes that already exist, so it
    becomes nothing a replay could need - no command, no record - and is
    published into the one table §3.30 names for it. `wfg replay` builds no
    analyser at all.

    SHOW LOAD NEVER WAITS FOR IT. The serve verb starts it after the first
    publish; until a file's pyramid lands, its record has no hash and no
    pyramid, which the tree will publish as nothing at all (§14.12: grey is the
    CLIENT's colour for "not yet", and noise is a real reading).

    A KEY BY CONTENT, a cache beside the media: `<media>/.timbre/<sha256>.tpy`.
    The same file under two names is analysed once; a renamed file keeps its
    colours. WHEN THAT FOLDER CANNOT BE WRITTEN - a show run off a read-only
    share - the pyramid is built in memory, published, and nothing is said: the
    session gets its colours and the next one pays again (§14.12).

    WHAT IT DOES NOT NOTICE: a file whose BYTES change under the same name
    while the show is open keeps the colours of its first analysis until the
    show is reopened, as its duration does. A path is queued once a session.
*/

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace wfg::audio
{
    class MediaInfo;
    struct TimbrePyramid;

    /*  WHAT ONE FILE CAME TO, and what it cost - every field the verb prints. */
    struct MediaAnalysis
    {
        enum class Outcome
        {
            built,        // analysed now, and the cache written
            cached,       // read from the cache; nothing analysed
            inMemory,     // analysed now; the cache could not be written
            missing,      // named by the show, and not there
            unreadable,   // there, and not audio this build can decode
            stopped       // the analyser was stopped part way; nothing kept
        };

        Outcome outcome = Outcome::missing;

        /*  Lower-case hex SHA-256 of the file's bytes; empty when it was
            never read. */
        std::string contentHash;

        double seconds = 0.0;
        std::size_t frames = 0;
        std::size_t levels = 0;

        /*  THE WORK, in frames coloured: nought for a cache hit. */
        std::size_t framesAnalysed = 0;

        double hashMilliseconds = 0.0;

        /*  Decoding, colouring and building the levels, and writing or
            reading the cache: everything after the hash. */
        double analysisMilliseconds = 0.0;

        /*  The size of the cache file on the disk now, nought when there is
            none. */
        std::int64_t bytesOnDisk = 0;

        /*  Set for `built`, `cached` and `inMemory`, and only for those. */
        std::shared_ptr<const TimbrePyramid> pyramid;
    };

    /*  One word per outcome, as the verb prints them: built, cached, memory,
        missing, unreadable, stopped. */
    const char* describe (MediaAnalysis::Outcome) noexcept;

    /*  `<mediaFolder>/.timbre`, or empty when there is no media folder - a
        show that is not a bundle has nowhere to keep a cache. */
    std::string timbreCacheFolder (const std::string& mediaFolder);

    /*  THE WHOLE OF THE WORK FOR ONE FILE, on the calling thread.

        `named` is the path the document writes, resolved as the durations are
        (`resolveMediaPath`). `force` builds and writes the pyramid even when
        a valid one is cached - `wfg analyse --force`, which is how M22 is
        taken more than once. `stop`, when given, is looked at between every
        frame and every few kilobytes of the hash, and a raised flag returns
        `stopped` with nothing written and nothing kept.

        Never throws. */
    MediaAnalysis analyseMediaFile (const std::string& mediaFolder, const std::string& named,
                                    bool force, const std::atomic<bool>* stop = nullptr);

    //==============================================================================
    class MediaAnalyser
    {
    public:
        /*  Publishes into `mediaToPublishInto`, which must outlive it - the
            serve verb declares this after its `MediaInfo`, so it is destroyed,
            and stopped, first. */
        MediaAnalyser (MediaInfo& mediaToPublishInto, std::string mediaFolder);
        ~MediaAnalyser();

        MediaAnalyser (const MediaAnalyser&) = delete;
        MediaAnalyser& operator= (const MediaAnalyser&) = delete;
        MediaAnalyser (MediaAnalyser&&) = delete;
        MediaAnalyser& operator= (MediaAnalyser&&) = delete;

        /*  Starts the thread. False if it was already running. */
        bool start();

        /*  Stops it - between two frames, or a few kilobytes into a hash - and
            joins. What was queued is forgotten. The destructor calls it. */
        void stop();

        bool isRunning() const noexcept { return running.load (std::memory_order_relaxed); }

        /*  ANY THREAD - in `wfg serve`, the one that opens the show and then
            the tick thread, after a show edit. Queues a file by the path the
            document names; false, and nothing queued, when this analyser has
            been asked for that path before. Once a session is the rule: a
            show edit re-offers every file the show names, and all but the new
            ones are dropped here, under one short lock each. */
        bool queue (const std::string& named);

        /*  Files waiting or being analysed. It falls only AFTER a file's
            record is published, so a caller that sees nought and then reads
            the snapshot sees every record it waited for. */
        std::size_t outstanding() const;

    private:
        void run();

        MediaInfo* media = nullptr;
        const std::string folder;

        mutable std::mutex guard;
        std::condition_variable wake;
        std::deque<std::string> queued;

        /*  Every path ever queued, so a path is analysed once a session. */
        std::set<std::string> seen;

        /*  Queued plus the one in hand. */
        std::size_t pending = 0;

        std::atomic<bool> running { false };
        std::atomic<bool> stopping { false };

        std::thread thread;
    };
}
