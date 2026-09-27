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

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

/*
    WHAT A TAKE LOOKS LIKE (Phase 9c, stage 9c.4, namespace draft §19.7): the
    peaks of each sampling channel's take and of every layer on it, for the
    picture at the foot of the window - a door of its own, the third the
    window reads through (§14.16's second rule).

    WHY NOT THE PARAMETER TREE. The tree is text, published at every tick and
    diffed for every socket; a take's picture is thousands of numbers that
    change a block at a time while it records. It is the media table's shape
    instead: an immutable set, taken whole by a reader and replaced, never
    edited underneath anybody.

    NOT A REACH PAST THE TICK THREAD. The recorders are the audio host's, not
    the tick thread's: their peaks are atomics the audio thread writes a chunk
    at a time and any thread may read, and the store that holds them has a lock
    of its own (`AudioHost::takeOf`). A picture is built on the thread that
    asks for one - the window's, which is also the thread every graph is
    rebuilt on, so a source asking whichever host is current cannot race the
    interface being changed.

    AT MOST 2048 COLUMNS A LAYER. A chunk of the recorder is 256 samples; a
    minute at 48 kHz is 11 250 of them and ten minutes 112 500, which no panel
    is wide enough to draw. So `binSamples` grows in whole chunks until the
    take fits, and each column is the loudest chunk in it.

    NAMES NO JUCE AND NO TRACKTION TYPE, as the Looper does, so it is tested
    alone.
*/
namespace wfg::audio
{
    class Looper;

    struct TakePicture
    {
        int sampleRate = 48000;

        /** How many samples a column covers: whole chunks of the recorder. */
        int binSamples = 256;

        /** Columns in each slot, all slots alike. */
        int bins = 0;

        /*  The take, each layer on it, and the one being laid when there is
            one - the recorder's own order. */
        int slots = 0;

        /** The loudest sample of either channel in each column, slot after slot. */
        std::vector<float> peaks;

        float peak (int slot, int bin) const noexcept
        {
            return slot >= 0 && slot < slots && bin >= 0 && bin < bins
                       ? peaks[static_cast<std::size_t> (slot) * static_cast<std::size_t> (bins)
                               + static_cast<std::size_t> (bin)]
                       : 0.0f;
        }

        /** How much of the take the columns cover, in seconds. */
        double seconds() const noexcept
        {
            return sampleRate > 0 ? static_cast<double> (bins) * binSamples / sampleRate : 0.0;
        }
    };

    struct TakePictureSet
    {
        std::map<std::string, TakePicture> byChannel;

        const TakePicture* of (const std::string& channel) const
        {
            const auto found = byChannel.find (channel);
            return found != byChannel.end() ? &found->second : nullptr;
        }
    };

    class TakePictures
    {
    public:
        static constexpr int maxBins = 2048;

        /** Every sampling channel's recorder, by its channel's id, as the host holds them now. */
        using Source = std::function<std::vector<std::pair<std::string, std::shared_ptr<const Looper>>>()>;

        explicit TakePictures (Source);

        /*  THE PICTURES, built on the calling thread from what the recorders
            hold now - at most every forty milliseconds, the rest of the time
            the last set again. Never null. */
        std::shared_ptr<const TakePictureSet> snapshot() const;

        /** One recorder's picture, at most `columns` columns a slot. */
        static TakePicture pictureOf (const Looper&, int columns = maxBins);

    private:
        Source source;
        mutable std::mutex lock;
        mutable std::shared_ptr<const TakePictureSet> last;
        mutable std::int64_t builtAt = 0;
    };
}
