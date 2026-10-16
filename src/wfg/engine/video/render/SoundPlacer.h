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
    A CARD'S SOUND, PLACED BY ITS SAMPLE (namespace draft §71, APK).

    The audio thread's blocks arrive on Go.dot's count, at the interface's
    rate; a card plays 48 kHz on its own clock. Every sample the card is sent
    is read from the blocks at the position on Go.dot's count that the card's
    sample stands for - the two clock lines say where - so the sound is in
    step with the picture drawn for the same moment, and the card's clock and
    the interface's never pull the two apart: there is no ratio to steer, only
    a place to read. Between samples a windowed sinc reads at the fraction,
    its cutoff lowered under a source faster than 48 kHz so nothing folds
    back.

    What came: each card channel's samples, on Go.dot's count from where the
    first block began; a block that starts later than the last ended leaves
    silence between; one that starts earlier - the count began again - begins
    the history again. What is placed is what can be read whole: up to the
    card's sample whose read reaches past the history's end.

    Pure: no clock, no card, no thread. The render loop feeds it and hands it
    the line from a card's samples to Go.dot's; a test hands it a made-up one.
*/

#include <cstdint>
#include <functional>
#include <vector>

namespace wfg::video::card
{
    class SoundPlacer
    {
    public:
        /*  How many channels the card is sent - what the card was opened with,
            the extra ones silent. Begins again. */
        void setChannels (int cardChannels);
        int channels() const noexcept  { return channelCount; }

        /*  ONE BLOCK OF GO.DOT'S SOUND: `frames` samples from `sample` on, at
            `rate`, each card channel's from `lane[channel]` - null for
            silence. A new rate begins again. */
        void take (std::int64_t sample, int frames, int rate, const std::vector<const float*>& lane);

        /*  Where Go.dot's sound is known: from the first sample kept to the
            one after the last that came. */
        std::int64_t knownFrom() const noexcept  { return historyStart; }
        std::int64_t knownUntil() const noexcept { return historyStart + historyLength(); }

        /*  The card's sample to place next, from `cardSample` on. */
        void startAt (std::int64_t cardSample) noexcept;
        bool started() const noexcept           { return begun; }
        std::int64_t next() const noexcept      { return cursor; }

        /*  THE CARD'S SAMPLES from `next()` on, until `untilCardSample` or
            until what they read is not here yet: interleaved, as 32-bit
            integers, into `out`. `goDotAt` is the position on Go.dot's count
            a card sample stands for. How many frames were placed. */
        int place (std::int64_t untilCardSample, const std::function<double (std::int64_t)>& goDotAt,
                   std::vector<std::int32_t>& out);

        /*  Card samples read where nothing was kept - the history had gone,
            or a block never came - and placed as silence. */
        std::uint64_t late() const noexcept     { return lateSamples; }

    private:
        int historyLength() const noexcept;
        void makeTable (double cutoff);
        double readAt (int channel, double position) const;

        int channelCount = 0;
        int rate = 0;
        std::vector<std::vector<float>> history;
        std::int64_t historyStart = 0;
        bool historyBegun = false;

        bool begun = false;
        std::int64_t cursor = 0;
        std::uint64_t lateSamples = 0;

        //  The interpolation's table: `phases` rows of `2 * halfWidth` taps.
        static constexpr int phases = 512;
        int halfWidth = 0;
        double tableCutoff = -1.0;
        std::vector<float> table;
    };
}
