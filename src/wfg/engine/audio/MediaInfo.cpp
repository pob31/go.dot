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
#include <memory>

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

    std::map<std::string, double> mediaDurations (const doc::ShowDocument& document,
                                                 const std::string& mediaFolder)
    {
        std::map<std::string, double> durations;

        const juce::File folder { juce::String (mediaFolder) };

        /*  WALKED RATHER THAN STORED, and never written into the document: how
            long a file is, is a fact about the file rather than something
            somebody decided (§4.10). The walk is the shape `widestRangeCount`
            already uses in the console - once, at load, on the thread that
            opens the show. */
        const std::function<void (const juce::ValueTree&)> visit =
            [&] (const juce::ValueTree& node)
        {
            for (const auto& child : node)
                visit (child);

            if (node.getType().toString() != "Media")
                return;

            const auto named = node[juce::Identifier ("file")].toString().toStdString();

            if (named.empty() || durations.count (named) != 0)
                return;

            /*  Resolved the way the runner resolves it, so that the duration
                published and the file played are the same file: relative to the
                bundle's media folder, or taken as given when there is no folder
                to be relative to. */
            const auto path = mediaFolder.empty()
                                ? named
                                : folder.getChildFile (juce::String (named))
                                        .getFullPathName().toStdString();

            durations[named] = mediaDurationSeconds (path);
        };

        visit (document.root());

        return durations;
    }
}
