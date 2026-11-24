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
    WHAT A PLUGIN NEEDS OF A BUFFER, AS THIS MACHINE MEASURED IT (namespace
    draft §149, BJZ; the author, 2026-11-23, asked how a plugin should be given
    a larger buffer: "Can we test this when scanning to have a preset?").

    One figure a plugin: the longest it took over one block, in microseconds,
    and the block and rate that was at. Taken when the plugin is scanned - a
    child runs it alone over a few seconds of sound with onsets, where a
    reverb's bursts are - and RAISED by what a show shows: when a plugin's own
    work in a show is longer than its measure, the measure becomes that, so the
    next build starts where this one ended. Never lowered but by a new scan's
    measure of a plugin whose file has changed.

    THIS MACHINE'S KNOWLEDGE, like the scan's list, never the show's (PRD §4.10:
    the document holds what someone decided). One small text file under the
    engine's folder, a plugin a line: the figure, the block, the rate, who
    measured it (`scan` or `show`), the plugin's file time, then the identifier
    to the end of the line - identifiers carry spaces. Read and written whole;
    two writers at one moment lose one line's update, which the next late
    block teaches again.

    WHAT THE BUILD MAKES OF IT is `lateSamplesFor`: how many samples later the
    plugin's lane is played from the start, so that a plugin known to need more
    than the callback leaves it is never silent for it, not even once.
*/

#include <juce_core/juce_core.h>

#include <cmath>
#include <cstdint>
#include <locale>
#include <map>
#include <sstream>
#include <string>

namespace wfg::plugin
{
    struct PluginMeasure
    {
        std::uint32_t workMicros = 0;
        int blockSize = 128;
        int sampleRate = 48000;
        std::string by = "scan";

        /** The plugin's file as it was when a scan measured it, in milliseconds since the epoch; nought when not said. */
        std::int64_t fileTime = 0;
    };

    namespace measure
    {
        /*  A measure is a quarter more than itself when it is planned for: a
            plugin measured at 1.3 ms is not promised 1.3. */
        constexpr double margin = 1.25;

        /** The share of the block period a lane has, to the callback's end (ProxyLane::callbackFraction). */
        constexpr double callbackShare = 0.60;

        /** As late as a lane goes (ProxyLane::lateBuffersMost). */
        constexpr int buffersMost = 8;

        /*  HOW MUCH LATER, IN SAMPLES, a plugin measured at `workMicros` is
            played from the start on an interface of this rate and block:
            nought while its work, and the margin, fits what the callback
            leaves it; otherwise as many whole buffers as the work is long,
            and never more than a lane goes. A lane played one buffer later
            has one whole period for its answer. */
        inline int lateSamplesFor (std::uint32_t workMicros, int sampleRate, int blockSize) noexcept
        {
            if (workMicros == 0 || sampleRate <= 0 || blockSize <= 0)
                return 0;

            const auto periodUs = 1000000.0 * static_cast<double> (blockSize) / static_cast<double> (sampleRate);
            const auto needUs = static_cast<double> (workMicros) * margin;

            if (needUs <= callbackShare * periodUs)
                return 0;

            const auto buffers = static_cast<int> (std::ceil (needUs / periodUs));
            return (buffers < 1 ? 1 : buffers > buffersMost ? buffersMost : buffers) * blockSize;
        }

        inline juce::File fileIn (const std::string& engineFolder)
        {
            return juce::File (juce::String::fromUTF8 (engineFolder.c_str()))
                       .getChildFile ("plugins").getChildFile ("measures.txt");
        }

        /** Every measure this machine keeps, by identifier. A line that does not read is passed over. */
        inline std::map<std::string, PluginMeasure> readAll (const std::string& engineFolder)
        {
            std::map<std::string, PluginMeasure> all;
            const auto file = fileIn (engineFolder);

            if (! file.existsAsFile())
                return all;

            juce::StringArray lines;
            lines.addLines (file.loadFileAsString());

            for (const auto& line : lines)
            {
                std::istringstream in (line.toStdString());
                in.imbue (std::locale::classic());

                PluginMeasure measure;
                std::string identifier;

                if (! (in >> measure.workMicros >> measure.blockSize >> measure.sampleRate >> measure.by >> measure.fileTime))
                    continue;

                std::getline (in >> std::ws, identifier);

                if (! identifier.empty() && identifier.back() == '\r')
                    identifier.pop_back();

                if (! identifier.empty() && measure.blockSize > 0 && measure.sampleRate > 0)
                    all[identifier] = measure;
            }

            return all;
        }

        inline bool find (const std::string& engineFolder, const std::string& identifier, PluginMeasure& into)
        {
            const auto all = readAll (engineFolder);
            const auto found = all.find (identifier);

            if (found == all.end())
                return false;

            into = found->second;
            return true;
        }

        /*  KEEPS A MEASURE. `onlyIfMore` is a show's: it raises what is kept
            and never lowers it. A scan's replaces. True when the file was
            written. */
        inline bool keep (const std::string& engineFolder, const std::string& identifier, const PluginMeasure& measure,
                          bool onlyIfMore)
        {
            if (identifier.empty() || identifier.find ('\n') != std::string::npos)
                return false;

            auto all = readAll (engineFolder);

            if (const auto found = all.find (identifier);
                onlyIfMore && found != all.end() && found->second.workMicros >= measure.workMicros)
                return false;

            auto kept = measure;

            //  A show does not know the file's time; what a scan said of it stands.
            if (const auto found = all.find (identifier); found != all.end() && kept.fileTime == 0)
                kept.fileTime = found->second.fileTime;

            all[identifier] = kept;

            std::ostringstream out;
            out.imbue (std::locale::classic());

            for (const auto& [id, each] : all)
                out << each.workMicros << ' ' << each.blockSize << ' ' << each.sampleRate << ' '
                    << (each.by.empty() ? std::string ("scan") : each.by) << ' ' << each.fileTime << ' ' << id << '\n';

            const auto file = fileIn (engineFolder);
            file.getParentDirectory().createDirectory();
            return file.replaceWithText (juce::String::fromUTF8 (out.str().c_str()), false, false, "\n");
        }
    }
}
