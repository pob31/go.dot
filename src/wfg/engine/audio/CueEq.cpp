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

#include <wfg/engine/audio/CueEq.h>

#include <algorithm>
#include <cmath>

namespace wfg::audio
{
    namespace
    {
        constexpr int hpfSection = 0;
        constexpr int lpfSection = 1;
        constexpr int firstBand = 2;
    }

    //==============================================================================
    void CueEq::prepare (int numChannelsToUse, double sampleRateToUse, int maxBlockSize)
    {
        channels = std::clamp (numChannelsToUse, 0, maxChannels);
        blockLimit = std::max (0, maxBlockSize);
        rate = sampleRateToUse > 0.0 ? sampleRateToUse : 48000.0;

        /*  Allocated here and nowhere else. Every pointer the audio thread
            follows is sized by this call. */
        for (auto& s : state)
        {
            s.z1.assign (static_cast<std::size_t> (channels), 0.0);
            s.z2.assign (static_cast<std::size_t> (channels), 0.0);
            s.builtRevision = 0;
            s.active = false;
            s.coefficients = eqmath::identity();
        }

        /*  Force a rebuild of every section from the atomics on the first
            block, whatever revision they hold: the rate may have changed. */
        for (auto& c : control)
            c.revision.fetch_add (1, std::memory_order_release);

        for (int i = 0; i < numSections; ++i)
            rebuildIfNeeded (i);
    }

    //==============================================================================
    void CueEq::set (const EqSettings& settingsToUse) noexcept
    {
        const auto& next = settingsToUse;
        const auto first = ! everSet;
        everSet = true;

        onFlag.store (next.on ? 1 : 0, std::memory_order_relaxed);

        /*  The filters: a flag and a frequency each. The stores go before the
            revision bump, and the bump is a release, so the audio thread's
            acquire of the revision sees the numbers it describes. */
        if (first || next.hpf != lastSet.hpf
              || ! EqSettings::sameFloat (next.hpfFreq, lastSet.hpfFreq))
        {
            control[hpfSection].flagOrShape.store (next.hpf ? 1 : 0, std::memory_order_relaxed);
            control[hpfSection].freq.store (next.hpfFreq, std::memory_order_relaxed);
            control[hpfSection].revision.fetch_add (1, std::memory_order_release);
        }

        if (first || next.lpf != lastSet.lpf
              || ! EqSettings::sameFloat (next.lpfFreq, lastSet.lpfFreq))
        {
            control[lpfSection].flagOrShape.store (next.lpf ? 1 : 0, std::memory_order_relaxed);
            control[lpfSection].freq.store (next.lpfFreq, std::memory_order_relaxed);
            control[lpfSection].revision.fetch_add (1, std::memory_order_release);
        }

        for (int b = 0; b < EqSettings::numBands; ++b)
        {
            const auto& band = next.band[b];

            if (! first && EqSettings::sameBand (band, lastSet.band[b]))
                continue;

            auto& c = control[firstBand + b];
            c.flagOrShape.store (static_cast<int> (band.shape), std::memory_order_relaxed);
            c.freq.store (band.freq, std::memory_order_relaxed);
            c.gain.store (band.gain, std::memory_order_relaxed);
            c.q.store (band.q, std::memory_order_relaxed);
            c.bandOn.store (band.on ? 1 : 0, std::memory_order_relaxed);
            c.revision.fetch_add (1, std::memory_order_release);
        }

        lastSet = next;
    }

    EqSettings CueEq::settings() const noexcept
    {
        EqSettings out;
        out.on = onFlag.load (std::memory_order_relaxed) != 0;
        out.hpf = control[hpfSection].flagOrShape.load (std::memory_order_relaxed) != 0;
        out.hpfFreq = control[hpfSection].freq.load (std::memory_order_relaxed);
        out.lpf = control[lpfSection].flagOrShape.load (std::memory_order_relaxed) != 0;
        out.lpfFreq = control[lpfSection].freq.load (std::memory_order_relaxed);

        for (int b = 0; b < EqSettings::numBands; ++b)
        {
            const auto& c = control[firstBand + b];
            auto& band = out.band[b];
            band.shape = static_cast<EqSettings::Shape> (c.flagOrShape.load (std::memory_order_relaxed));
            band.freq = c.freq.load (std::memory_order_relaxed);
            band.gain = c.gain.load (std::memory_order_relaxed);
            band.q = c.q.load (std::memory_order_relaxed);
            band.on = c.bandOn.load (std::memory_order_relaxed) != 0;
        }

        return out;
    }

    void CueEq::reset() noexcept
    {
        resetRequested.store (1, std::memory_order_release);
    }

    bool CueEq::isIdentity() const noexcept
    {
        return settings().isIdentity();
    }

    //==============================================================================
    void CueEq::clearState (SectionState& s) noexcept
    {
        std::fill (s.z1.begin(), s.z1.end(), 0.0);
        std::fill (s.z2.begin(), s.z2.end(), 0.0);
    }

    void CueEq::rebuildIfNeeded (int section) noexcept
    {
        auto& c = control[section];
        auto& s = state[section];

        const auto revision = c.revision.load (std::memory_order_acquire);

        if (revision == s.builtRevision)
            return;

        s.builtRevision = revision;
        const auto wasActive = s.active;

        if (section == hpfSection || section == lpfSection)
        {
            s.active = c.flagOrShape.load (std::memory_order_relaxed) != 0;
            const auto frequency = static_cast<double> (c.freq.load (std::memory_order_relaxed));

            s.coefficients = section == hpfSection ? eqmath::highPass (rate, frequency)
                                                   : eqmath::lowPass (rate, frequency);
        }
        else
        {
            EqSettings::Band band;
            band.shape = static_cast<EqSettings::Shape> (c.flagOrShape.load (std::memory_order_relaxed));
            band.freq = c.freq.load (std::memory_order_relaxed);
            band.gain = c.gain.load (std::memory_order_relaxed);
            band.q = c.q.load (std::memory_order_relaxed);
            band.on = c.bandOn.load (std::memory_order_relaxed) != 0;

            s.active = EqSettings::bandIsActive (band);
            s.coefficients = s.active ? eqmath::forBand (band, rate) : eqmath::identity();
        }

        /*  A section coming in starts from silence rather than from whatever
            its delays held when it last went out - which may have been a
            different cue's tail on this voice. */
        if (s.active && ! wasActive)
            clearState (s);
    }

    //==============================================================================
    void CueEq::process (float* const* channelData, int numChannelsToProcess,
                         int numSamples) noexcept WFG_AUDIO_THREAD
    {
        if (channelData == nullptr || numSamples <= 0 || channels <= 0)
            return;

        if (resetRequested.exchange (0, std::memory_order_acq_rel) != 0)
            for (auto& s : state)
                clearState (s);

        for (int i = 0; i < numSections; ++i)
            rebuildIfNeeded (i);

        /*  OFF, OR NOTHING IN: the block is not touched. Not multiplied by
            one, not copied. See the class comment. */
        if (onFlag.load (std::memory_order_relaxed) == 0)
            return;

        auto anyActive = false;

        for (const auto& s : state)
            anyActive = anyActive || s.active;

        if (! anyActive)
            return;

        const auto frames = std::min (numSamples, blockLimit > 0 ? blockLimit : numSamples);
        const auto usable = std::min (numChannelsToProcess, channels);

        for (int section = 0; section < numSections; ++section)
        {
            auto& s = state[section];

            if (! s.active)
                continue;

            const auto b0 = s.coefficients.b0;
            const auto b1 = s.coefficients.b1;
            const auto b2 = s.coefficients.b2;
            const auto a1 = s.coefficients.a1;
            const auto a2 = s.coefficients.a2;

            for (int ch = 0; ch < usable; ++ch)
            {
                auto* data = channelData[ch];

                if (data == nullptr)
                    continue;

                const auto index = static_cast<std::size_t> (ch);
                auto z1 = s.z1[index];
                auto z2 = s.z2[index];

                for (int n = 0; n < frames; ++n)
                {
                    const auto x = static_cast<double> (data[n]);
                    const auto y = b0 * x + z1;
                    z1 = b1 * x - a1 * y + z2;
                    z2 = b2 * x - a2 * y;
                    data[n] = static_cast<float> (y);
                }

                s.z1[index] = z1;
                s.z2[index] = z2;
            }
        }

    }
}
