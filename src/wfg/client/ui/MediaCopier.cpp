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

#include <wfg/client/ui/MediaCopier.h>

#include <juce_events/juce_events.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace wfg::client::ui
{
    namespace
    {
        /*  A megabyte between two looks at `stopping`: a window closing waits
            for one of these at most, and a copy reads in steps large enough
            that the asking costs nothing. */
        constexpr int chunk = 1 << 20;

        /*  As many bytes as there are, up to `size`. A read may answer short
            before the end, and two files compared a short read apart would
            look different when they are not. */
        int readFully (juce::InputStream& in, char* into, int size)
        {
            auto got = 0;

            while (got < size)
            {
                const auto now = in.read (into + got, size - got);

                if (now <= 0)
                    break;

                got += now;
            }

            return got;
        }

        /*  THE SAME BYTES, compared and not hashed (SK): both files are on
            this machine, so there is nothing a digest would let travel, and a
            compare stops at the first difference - which in two renders of one
            sound is usually inside the first megabyte - where a hash reads both
            files to the end whatever they hold. */
        bool sameBytes (const juce::File& a, const juce::File& b, const std::function<bool()>& stopping)
        {
            juce::FileInputStream one (a), two (b);

            if (one.failedToOpen() || two.failedToOpen())
                return false;

            juce::HeapBlock<char> left (chunk), right (chunk);

            for (;;)
            {
                if (stopping())
                    return false;

                const auto x = readFully (one, left.get(), chunk);
                const auto y = readFully (two, right.get(), chunk);

                if (x != y)
                    return false;

                if (x == 0)
                    return true;

                if (std::memcmp (left.get(), right.get(), static_cast<std::size_t> (x)) != 0)
                    return false;
            }
        }

        /** The names of the files in a folder, as the disk spells them. */
        std::vector<std::string> namesIn (const juce::File& folder)
        {
            std::vector<std::string> names;

            for (const auto& file : folder.findChildFiles (juce::File::findFiles, false))
                names.push_back (file.getFileName().toStdString());

            return names;
        }

        /*  The system's own words, as the tail of a sentence of ours: no full
            stop and no line end, which Windows puts on both. */
        std::string tail (const juce::String& said)
        {
            return said.trim().trimCharactersAtEnd (".").trim().toStdString();
        }

        model::MediaWork failed (std::string why)
        {
            model::MediaWork work;
            work.found = model::Found::failed;
            work.why = std::move (why);
            return work;
        }

        model::MediaWork answered (model::Found found, std::string name = {})
        {
            model::MediaWork work;
            work.found = found;
            work.name = std::move (name);
            return work;
        }

        /*  THE COPY ITSELF, into a hidden part-file beside `target` and then
            over it in one move, so `media/` never holds half a sound under a
            name a cue could play. */
        model::MediaWork copyInto (const juce::File& source, const juce::File& target,
                                   const std::function<bool()>& stopping)
        {
            if (! source.existsAsFile())
                return answered (model::Found::missing);

            juce::FileInputStream in (source);

            if (in.failedToOpen())
                return answered (model::Found::unreadable);

            const auto replacing = target.existsAsFile();
            const juce::TemporaryFile part (target, juce::TemporaryFile::useHiddenFile);

            {
                juce::FileOutputStream out (part.getFile());

                if (out.failedToOpen())
                    return failed (tail (out.getStatus().getErrorMessage()));

                juce::HeapBlock<char> buffer (chunk);

                for (;;)
                {
                    if (stopping())
                        return failed ("the window was closed before it finished");

                    const auto got = in.read (buffer.get(), chunk);

                    if (got < 0 || (got == 0 && ! in.isExhausted()))
                        return failed (in.getStatus().failed() ? tail (in.getStatus().getErrorMessage())
                                                               : std::string ("reading it stopped part of the way"));

                    if (got == 0)
                        break;

                    if (! out.write (buffer.get(), static_cast<std::size_t> (got)))
                        return failed (tail (out.getStatus().getErrorMessage()));
                }

                out.flush();

                if (out.getStatus().failed())
                    return failed (tail (out.getStatus().getErrorMessage()));
            }

            /*  A REPLACE THAT CANNOT MOVE OVER THE OLD FILE leaves it where it
                was, and says the likeliest reason: on Windows a file a cue has
                open cannot be written over. */
            if (! part.overwriteTargetFileWithTemporary())
                return failed (replacing ? "the one in the show could not be replaced - it may be in use"
                                         : "it could not be put in place");

            return answered (model::Found::copied, target.getFileName().toStdString());
        }
    }

    model::MediaWork doMediaJob (const model::MediaJob& job, const juce::File& folder,
                                 const std::function<bool()>& stopping)
    {
        if (folder == juce::File())
            return answered (model::Found::noFolder);

        const juce::File source { juce::String (job.source) };

        if (const auto made = folder.createDirectory(); made.failed())
            return failed ("the show's media folder could not be made: " + tail (made.getErrorMessage()));

        const auto wanted = source.getFileName().toStdString();
        const auto present = namesIn (folder);

        /*  TAKEN, as the rule says (case-blind, model::sameFileName) and as
            this disk says - which folds whatever the rule does not, and sees a
            folder of the name too. */
        const auto taken = [&present, &folder] (const std::string& name)
        {
            return ! model::nameAmong (present, name).empty()
                || folder.getChildFile (juce::String (name)).exists();
        };

        //  An answered copy: over the file it met, or beside it under the first free number.
        if (job.copy && job.answer == model::Clash::replace)
            return copyInto (source, folder.getChildFile (juce::String (job.met)), stopping);

        if (job.copy && job.answer == model::Clash::keepBoth)
        {
            const auto name = model::freeName (wanted, taken);

            return name.empty() ? failed ("every name like it is taken")
                                : copyInto (source, folder.getChildFile (juce::String (name)), stopping);
        }

        /*  A LOOK - or a copy under the file's own name, which looks again
            first: two files of one name in one drop were both free when they
            were looked at, and the second must not land on the first. */
        model::Arrival arrival;
        arrival.exists = source.existsAsFile();
        arrival.readable = arrival.exists && juce::FileInputStream (source).openedOk();
        arrival.inShow = arrival.exists && source.getParentDirectory() == folder;
        arrival.meets = model::nameAmong (present, wanted);

        if (arrival.meets.empty() && folder.getChildFile (source.getFileName()).existsAsFile())
            arrival.meets = wanted;

        const auto met = folder.getChildFile (juce::String (arrival.meets));
        arrival.size = static_cast<std::uint64_t> (juce::jmax (juce::int64 { 0 }, source.getSize()));
        arrival.meetsSize = arrival.meets.empty() ? 0u
                                                  : static_cast<std::uint64_t> (juce::jmax (juce::int64 { 0 }, met.getSize()));

        const auto found = model::verdictFor (arrival, [&source, &met, &stopping] { return sameBytes (source, met, stopping); });

        switch (found)
        {
            case model::Found::inShow:
                return answered (found, wanted);

            case model::Found::same:
            case model::Found::other:
                return answered (found, arrival.meets);

            case model::Found::free:
                return job.copy ? copyInto (source, folder.getChildFile (source.getFileName()), stopping)
                                : answered (found, wanted);

            case model::Found::missing:
            case model::Found::unreadable:
            case model::Found::copied:
            case model::Found::failed:
            case model::Found::noFolder:
                break;
        }

        return answered (found);
    }

    //==============================================================================
    MediaCopier::MediaCopier (Done whenDone)
        : juce::Thread ("Go.dot media copy"), done (std::move (whenDone))
    {
    }

    MediaCopier::~MediaCopier()
    {
        stop();
    }

    void MediaCopier::start (const model::MediaJob& job, const juce::File& folder)
    {
        if (stopped || working)
            return;

        working = true;

        {
            const juce::ScopedLock held (jobLock);
            queued = std::make_pair (job, folder);
        }

        /*  Started on the first file, so a window that never imports has no
            thread; low, because a copy can wait and the show cannot. */
        if (! isThreadRunning())
            startThread (juce::Thread::Priority::low);

        wake.signal();
    }

    void MediaCopier::stop()
    {
        if (stopped)
            return;

        stopped = true;
        *alive = false;
        signalThreadShouldExit();
        wake.signal();
        stopThread (10000);
    }

    void MediaCopier::run()
    {
        while (! threadShouldExit())
        {
            if (! wake.wait (-1.0))
                continue;

            std::optional<std::pair<model::MediaJob, juce::File>> job;

            {
                const juce::ScopedLock held (jobLock);
                job.swap (queued);
            }

            if (! job.has_value() || threadShouldExit())
                continue;

            const auto work = doMediaJob (job->first, job->second, [this] { return threadShouldExit(); });

            if (threadShouldExit())
                return;

            juce::MessageManager::callAsync ([this, stillHere = alive, work]
            {
                if (! *stillHere)
                    return;

                working = false;

                if (done)
                    done (work);
            });
        }
    }
}
