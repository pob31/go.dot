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

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/*
    KEEP'S WRITER (Phase 9c, stage 9c.6, namespace draft §19.8): a take made a
    file in the show's media, off every thread the show runs on.

    THE DOCUMENT WRITER'S SHAPE, which this engine has already argued: one
    thread, one mutex, one condition variable, one queue of jobs going in and
    one list of what finished coming out, and the slow work - the copy, the
    encode, the disk - outside the lock. The tick thread only pushes a job and
    takes the finished ones; it never waits for a disk.

    WHAT A FILE IS: the take and every layer closed on it, summed at unity as
    the loop sums them, the whole take and not only the loop, at the rate it
    was recorded at, as a 32-bit float WAV - a sum of layers may pass full
    scale, and a float keeps it. Written under a name nobody has, beside where
    it goes, and moved into place when it is whole, so a file under
    `media/takes/` is never half a take.

    ITS NAME IS FOUND HERE, where the disk is: the channel's name, "take" and
    the first number free in the folder - "Looper take 3.wav". The engine logs
    the name this thread chose (`take.kept`), so a replay, which has no disk to
    ask, reads the same name.

    WHAT IT READS IS STILL. A job waits for the recorder to settle - the take
    closed, no layer's tail still falling - and copies the layers closed when
    it starts; the engine refuses Undo and Clear `busy` until `take.kept`, so
    nothing it reads is emptied under it (Looper::copyTake).
*/
namespace wfg::audio
{
    class Looper;

    class TakeWriter
    {
    public:
        struct Job
        {
            std::string channel;                  ///< the channel's id, for the record
            std::string stem;                     ///< what the file is called before " take <n>": the channel's name
            std::string mediaFolder;              ///< the bundle's media/
            std::shared_ptr<const Looper> take;
        };

        struct Done
        {
            std::string channel;
            std::string file;                     ///< relative to media/: "takes/Looper take 1.wav"
            std::string error;                    ///< empty when the file was written
        };

        TakeWriter() = default;
        ~TakeWriter();

        TakeWriter (const TakeWriter&) = delete;
        TakeWriter& operator= (const TakeWriter&) = delete;

        /** Queues one; the thread starts with the first. */
        void queue (Job);

        /** What has finished since the last asking, oldest first. */
        std::vector<Done> finished();

        /*  ONE JOB'S WORK, on the calling thread - what the thread runs, and
            what a test calls straight. */
        static Done write (const Job&);

        /*  A NAME NOBODY HAS in that folder: "<stem> take <n>.wav" for the
            first n from one that is free, the stem made safe for any file
            system. */
        static std::string freeName (const std::string& folder, const std::string& stem);

    private:
        void run();

        std::mutex lock;
        std::condition_variable wake;
        std::deque<Job> jobs;
        std::vector<Done> done;
        bool stopping = false;
        std::thread thread;
    };
}
