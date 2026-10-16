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

#include <wfg/engine/cue/GroupJoin.h>

#include <wfg/engine/document/MediaEdit.h>

#include <algorithm>
#include <cmath>

namespace wfg::cue::join
{
    namespace
    {
        //  What a voice calls silence (`CueMatrix::silenceDb`), and the gain under which a join gives it.
        constexpr double silenceDb = -120.0;
        constexpr double silentGain = 1.0e-6;

        double progressOf (std::int64_t sample, std::int64_t from, std::int64_t length) noexcept
        {
            if (sample < from)
                return 0.0;

            if (length <= 0 || sample >= from + length)
                return 1.0;

            return static_cast<double> (sample - from) / static_cast<double> (length);
        }
    }

    Kind kindOf (std::string_view word) noexcept
    {
        if (word == "gapless")
            return Kind::gapless;

        if (word == "crossfade")
            return Kind::crossfade;

        return Kind::letPlay;
    }

    double gainIn (std::int64_t sample, std::int64_t from, std::int64_t length, double curve,
                   bool picture) noexcept
    {
        return doc::fadeGain (progressOf (sample, from, length), curve, picture);
    }

    double gainOut (std::int64_t sample, std::int64_t from, std::int64_t length, double curve,
                    bool picture) noexcept
    {
        return doc::fadeGain (1.0 - progressOf (sample, from, length), curve, picture);
    }

    double decibelsOf (double gain) noexcept
    {
        if (! (gain > silentGain))
            return silenceDb;

        return std::max (silenceDb, 20.0 * std::log10 (gain));
    }
}
