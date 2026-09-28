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

#include <wfg/engine/audio/TakePictures.h>

#include <wfg/engine/audio/Looper.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace wfg::audio
{
    namespace
    {
        /*  HOW OFTEN A PICTURE IS BUILT AGAIN, at the most: a window refreshing
            at sixty a second asks every pass, and a ten-minute take of sixteen
            layers is two million atomic loads a build. Twenty-five a second is
            smoother than a take grows. */
        constexpr std::int64_t rebuildMilliseconds = 40;

        std::int64_t nowMilliseconds()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds> (
                       std::chrono::steady_clock::now().time_since_epoch()).count();
        }
    }

    TakePictures::TakePictures (Source sourceToUse)
        : source (std::move (sourceToUse))
    {
    }

    std::shared_ptr<const TakePictureSet> TakePictures::snapshot() const
    {
        const std::lock_guard<std::mutex> guard { lock };
        const auto now = nowMilliseconds();

        if (last != nullptr && now - builtAt < rebuildMilliseconds)
            return last;

        auto set = std::make_shared<TakePictureSet>();

        if (source)
            for (const auto& [channel, take] : source())
                if (take != nullptr)
                    set->byChannel[channel] = pictureOf (*take);

        last = std::move (set);
        builtAt = now;
        return last;
    }

    TakePicture TakePictures::pictureOf (const Looper& take, int columns)
    {
        TakePicture picture;
        picture.sampleRate = static_cast<int> (std::lround (take.shape().sampleRate));

        /*  THE TAKE, ITS LAYERS, AND THE PASS BEING LAID - read apart, so a
            state and a count published a block apart draw one frame wrong at
            the very worst, and never read a slot the recorder does not use. */
        const auto chunks = take.peakCount();
        const auto laying = take.state() == TakeState::overdubbing ? 1 : 0;
        picture.slots = 1 + std::max (0, take.layerCount()) + laying;

        if (chunks <= 0 || columns <= 0)
            return picture;

        const auto per = std::max (1, (chunks + columns - 1) / columns);
        picture.binSamples = per * Looper::peakSamples;
        picture.bins = (chunks + per - 1) / per;
        picture.peaks.assign (static_cast<std::size_t> (picture.slots) * static_cast<std::size_t> (picture.bins),
                              0.0f);

        for (int slot = 0; slot < picture.slots; ++slot)
        {
            for (int bin = 0; bin < picture.bins; ++bin)
            {
                auto loudest = 0.0f;
                const auto end = std::min (chunks, (bin + 1) * per);

                for (int chunk = bin * per; chunk < end; ++chunk)
                    loudest = std::max (loudest, take.peak (slot, chunk));

                picture.peaks[static_cast<std::size_t> (slot) * static_cast<std::size_t> (picture.bins)
                              + static_cast<std::size_t> (bin)] = loudest;
            }
        }

        return picture;
    }
}
