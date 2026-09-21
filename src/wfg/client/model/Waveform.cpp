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

#include <wfg/client/model/Waveform.h>

#include <wfg/engine/audio/Timbre.h>

#include <algorithm>

namespace wfg::client::model
{
    std::size_t levelFor (const audio::TimbrePyramid& pyramid, int width)
    {
        if (pyramid.levels.empty() || width <= 0)
            return 0;

        const auto wanted = static_cast<std::size_t> (width);

        /*  From the coarsest down, the first level with a frame per column. The
            levels halve going up, so this is the one that costs least and still
            has something to say in every column. */
        for (auto level = pyramid.levels.size(); level-- > 0;)
            if (pyramid.levels[level].size() >= wanted)
                return level;

        //  A bar wider than the file has frames: the finest there is.
        return 0;
    }

    std::vector<Column> waveform (const audio::TimbrePyramid& pyramid, int width)
    {
        if (width <= 0 || pyramid.levels.empty())
            return {};

        const auto& frames = pyramid.levels[levelFor (pyramid, width)];

        if (frames.empty())
            return {};

        std::vector<Column> columns;
        columns.reserve (static_cast<std::size_t> (width));

        const auto count = frames.size();

        for (auto at = std::size_t { 0 }; at < static_cast<std::size_t> (width); ++at)
        {
            /*  The span of frames this column stands for, and at least one:
                a bar wider than the level has frames repeats rather than
                leaving gaps, which is what stretching a short file to a wide
                bar has to do. */
            const auto from = at * count / static_cast<std::size_t> (width);
            const auto to = std::max (from + 1, (at + 1) * count / static_cast<std::size_t> (width));

            const auto* loudest = &frames[from];

            for (auto i = from; i < to && i < count; ++i)
                if (frames[i].peak > loudest->peak)
                    loudest = &frames[i];

            Column column;
            column.hue = audio::timbre::hueOf (*loudest);
            column.saturation = audio::timbre::saturationOf (*loudest);
            column.lightness = audio::timbre::lightnessOf (*loudest);
            column.peak = audio::timbre::peakOf (*loudest);

            columns.push_back (column);
        }

        return columns;
    }

    double lengthOf (const audio::TimbrePyramid& pyramid)
    {
        if (pyramid.sampleRate == 0 || pyramid.samples == 0)
            return 0.0;

        return static_cast<double> (pyramid.samples) / static_cast<double> (pyramid.sampleRate);
    }

    std::vector<Column> waveform (const audio::TimbrePyramid& pyramid, int width,
                                  double fromSeconds, double toSeconds)
    {
        const auto length = lengthOf (pyramid);

        if (width <= 0 || pyramid.levels.empty() || ! (length > 0.0) || ! (toSeconds > fromSeconds))
            return {};

        const auto from = std::max (0.0, fromSeconds);
        const auto to = std::min (length, toSeconds);

        if (! (to > from))
            return {};

        /*  THE LEVEL IS PICKED FOR THE WINDOW, not for the file. A level holds
            `levels[n].size()` frames across the whole file, so the window holds
            that many times its share - and asking `levelFor` for the width
            scaled by the inverse of that share is the same question as "which
            level gives this window a frame per column". Walking down the
            pyramid as the window narrows is what keeps a repaint the same cost
            at every zoom. */
        const auto share = (to - from) / length;
        const auto wanted = share > 0.0
                              ? static_cast<int> (std::min (static_cast<double> (width) / share,
                                                            1.0e9))
                              : width;

        const auto& frames = pyramid.levels[levelFor (pyramid, std::max (1, wanted))];

        if (frames.empty())
            return {};

        const auto count = frames.size();
        const auto firstFrame = static_cast<double> (count) * from / length;
        const auto lastFrame = static_cast<double> (count) * to / length;

        std::vector<Column> columns;
        columns.reserve (static_cast<std::size_t> (width));

        for (auto at = 0; at < width; ++at)
        {
            const auto a = firstFrame + (lastFrame - firstFrame) * at / width;
            const auto b = firstFrame + (lastFrame - firstFrame) * (at + 1) / width;

            auto fromIndex = static_cast<std::size_t> (std::max (0.0, a));
            auto toIndex = static_cast<std::size_t> (std::max (0.0, b));

            fromIndex = std::min (fromIndex, count - 1);
            toIndex = std::max (fromIndex + 1, std::min (toIndex, count));

            const auto* loudest = &frames[fromIndex];

            for (auto i = fromIndex; i < toIndex; ++i)
                if (frames[i].peak > loudest->peak)
                    loudest = &frames[i];

            Column column;
            column.hue = audio::timbre::hueOf (*loudest);
            column.saturation = audio::timbre::saturationOf (*loudest);
            column.lightness = audio::timbre::lightnessOf (*loudest);
            column.peak = audio::timbre::peakOf (*loudest);

            columns.push_back (column);
        }

        return columns;
    }

    double playhead (double position, double from, double to)
    {
        if (! (to > from))
            return 0.0;

        return std::min (1.0, std::max (0.0, (position - from) / (to - from)));
    }

    double playhead (double position, double length)
    {
        if (! (length > 0.0))
            return 0.0;

        return std::min (1.0, std::max (0.0, position / length));
    }

    double countdown (double remaining, double total)
    {
        if (! (total > 0.0))
            return 1.0;

        return std::min (1.0, std::max (0.0, remaining / total));
    }

    bool runsLeftToRight (const std::string& state)
    {
        //  Only a post-wait does. A pre-wait arrives at the cue from the right.
        return state == "postWait";
    }
}
