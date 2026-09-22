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

#include <wfg/engine/audio/MediaInfo.h>

#include <wfg/engine/document/ShowDocument.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

namespace wfg::audio
{
    double mediaDurationSeconds (const std::string& absolutePath)
    {
        const juce::File file { juce::String (absolutePath) };

        if (absolutePath.empty() || ! file.existsAsFile())
            return 0.0;

        /*  ONE MANAGER PER CALL rather than a shared one, because a show is
            opened once and this is not a hot path: a hundred cues is a hundred
            registrations of five formats, which is nothing beside a hundred
            file headers being parsed off a disk. A static would need a mutex to
            be honest about which thread asked. */
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        const std::unique_ptr<juce::AudioFormatReader> reader { formats.createReaderFor (file) };

        /*  NO READER IS NOUGHT AND NOT A FAILURE. A format this build cannot
            read is the same answer as a file that is not there: the cue will
            fail its arm when somebody fires it, with a reason, and the show
            still opens (Phase 2's rule). */
        if (reader == nullptr || reader->sampleRate <= 0.0)
            return 0.0;

        return static_cast<double> (reader->lengthInSamples) / reader->sampleRate;
    }

    std::string resolveMediaPath (const std::string& mediaFolder, const std::string& named)
    {
        if (mediaFolder.empty())
            return named;

        return juce::File (juce::String (mediaFolder)).getChildFile (juce::String (named))
                   .getFullPathName().toStdString();
    }

    std::vector<std::string> mediaFilesNamedBy (const doc::ShowDocument& document)
    {
        std::vector<std::string> named;
        std::set<std::string> seen;

        /*  WALKED RATHER THAN STORED, and never written into the document:
            which files a show plays is read off the cues that name them. The
            walk is the shape `widestRangeCount` already uses in the console. A
            media cue's children are its routes, so visiting them first changes
            no order that matters. */
        const std::function<void (const juce::ValueTree&)> visit =
            [&] (const juce::ValueTree& node)
        {
            for (const auto& child : node)
                visit (child);

            if (node.getType().toString() != "Media")
                return;

            auto file = node[juce::Identifier ("file")].toString().toStdString();

            if (! file.empty() && seen.insert (file).second)
                named.push_back (std::move (file));
        };

        visit (document.root());

        return named;
    }

    std::map<std::string, double> mediaDurations (const doc::ShowDocument& document,
                                                 const std::string& mediaFolder)
    {
        std::map<std::string, double> durations;

        /*  How long a file is, is a fact about the file rather than something
            somebody decided (§4.10): read here, once, at load, on the thread
            that opens the show - through the one resolution of a `file` there
            is, so that the duration published and the file played are the
            same file. */
        for (const auto& named : mediaFilesNamedBy (document))
            durations[named] = mediaDurationSeconds (resolveMediaPath (mediaFolder, named));

        return durations;
    }

    //==============================================================================
    namespace
    {
        /*  The first snapshot: every file the show named, with the seconds just
            read, no hash and no pyramid. Built from the frozen map rather than
            by a second walk, so the records and the durations have the same
            keys by construction. */
        std::shared_ptr<const MediaRecords> unanalysedRecordsOf (const std::map<std::string, double>& lengths)
        {
            auto records = std::make_shared<MediaRecords>();

            for (const auto& [path, seconds] : lengths)
            {
                MediaRecord record;
                record.seconds = seconds;
                records->emplace (path, std::move (record));
            }

            return records;
        }
    }

    MediaInfo::MediaInfo (const doc::ShowDocument& document, const std::string& mediaFolder)
        : frozenDurations (mediaDurations (document, mediaFolder)),
          lengths (std::make_shared<const std::map<std::string, double>> (frozenDurations)),
          published (unanalysedRecordsOf (frozenDurations))
    {
    }

    std::shared_ptr<const std::map<std::string, double>> MediaInfo::durations() const
    {
        const std::lock_guard<std::mutex> lock { swapMutex };
        return lengths;
    }

    std::shared_ptr<const MediaRecords> MediaInfo::snapshot() const
    {
        const std::lock_guard<std::mutex> lock { swapMutex };
        return published;
    }

    void MediaInfo::publish (const std::string& path, MediaRecord record)
    {
        /*  THE FROZEN SECONDS, whatever the publisher passed, for a file both
            halves know. A record whose length disagreed with `durations()`
            would be the second copy §13.4 warns about, and the tree and the
            timbre route would each be right about a different number. A file
            imported after the open has no frozen length to agree with, so its
            record keeps the seconds the analyser read - and `durations()`,
            which is read only at open, never hears of it. */
        auto learned = false;

        if (const auto frozen = frozenDurations.find (path); frozen != frozenDurations.end())
            record.seconds = frozen->second;
        else
            learned = record.seconds > 0.0;

        const std::lock_guard<std::mutex> publishing { publisherMutex };

        /*  A LENGTH NOBODY HAD, WRITTEN DOWN. A file the show named at open has
            its length already and the line above keeps the two halves agreeing;
            a file imported since has none, and this is the moment it becomes
            known. Swapped rather than edited, so the caches that key on this
            map's address rebuild once and see it. */
        if (learned)
        {
            const std::lock_guard<std::mutex> lock { swapMutex };

            /*  AND ONLY WHEN IT IS NEWS. The analyser republishes a file every
                time it reads one, and swapping the map for a length already in
                it would move the address for nothing - which every cache
                keyed on that address would answer by doing its whole walk
                again. Compared with two `<` rather than `==`: the strict job's
                -Wfloat-equal is about doubles that were ARITHMETIC, and this
                is a copy of the same number, but the warning does not know
                that and one exception is one too many. */
            const auto known = lengths->find (path);

            if (known == lengths->end()
                 || known->second < record.seconds || record.seconds < known->second)
            {
                auto grown = std::make_shared<std::map<std::string, double>> (*lengths);
                (*grown)[path] = record.seconds;
                lengths = std::move (grown);
            }
        }

        std::shared_ptr<const MediaRecords> current;

        {
            const std::lock_guard<std::mutex> lock { swapMutex };
            current = published;
        }

        /*  BUILT OUTSIDE THE READERS' LOCK, which is the point of the shape: a
            reader on the tick thread waits for a pointer copy and never for a
            map copy. Nothing it can see is edited - this is a new map. */
        auto next = std::make_shared<MediaRecords> (*current);
        next->insert_or_assign (path, std::move (record));

        {
            const std::lock_guard<std::mutex> lock { swapMutex };
            published = std::move (next);
        }

        /*  `current` is released after the swap and outside the readers' lock,
            so a previous map nobody else holds is freed here, on the
            publisher's thread. One a reader still holds is freed by that reader
            when it lets go - the tick thread at worst, which may free, and never
            the audio thread, which reads none of this. */
    }
}
