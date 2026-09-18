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
