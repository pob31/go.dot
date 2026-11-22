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

#include <wfg/engine/audio/CueDyn.h>

#include <cstddef>      // spatcore's EffectsTypes.h before 880af3d used size_t without it

#include <spatcore/effects/modules/DynamicsModule.h>

#include <algorithm>

namespace wfg::audio
{
    namespace fx = spatcore::effects;

    CueDyn::CueDyn()
        : module (std::make_unique<fx::DynamicsModule>())
    {
    }

    CueDyn::~CueDyn() = default;

    //==============================================================================
    void CueDyn::prepare (int numChannelsToUse, double sampleRateToUse, int maxBlockSize)
    {
        channels = std::clamp (numChannelsToUse, 0, maxChannels);
        blockLimit = std::max (1, maxBlockSize);
        rate = sampleRateToUse > 0.0 ? sampleRateToUse : 48000.0;

        //  Allocated here and nowhere else: the module's delay lines, the fold.
        fx::ChainConfig config;
        config.sampleRate = rate;
        config.maxBlock = blockLimit;
        module->prepare (config);

        fold.assign (static_cast<std::size_t> (2 * blockLimit), 0.0f);

        //  Applied again on the first block, whatever the revision says: the rate may have moved.
        appliedRevision = revision.load (std::memory_order_relaxed) - 1;
        wasOn = false;
        reduction.store (0.0f, std::memory_order_relaxed);
    }

    //==============================================================================
    void CueDyn::set (const DynSettings& next) noexcept
    {
        if (everSet && next.sameAs (lastSet))
            return;

        everSet = true;
        lastSet = next;

        auto& c = control;
        c.on.store (next.on ? 1 : 0, std::memory_order_relaxed);
        c.compOn.store (next.compOn ? 1 : 0, std::memory_order_relaxed);
        c.gateOn.store (next.gateOn ? 1 : 0, std::memory_order_relaxed);
        c.thresholdDb.store (next.thresholdDb, std::memory_order_relaxed);
        c.ratio.store (next.ratio, std::memory_order_relaxed);
        c.attackMs.store (next.attackMs, std::memory_order_relaxed);
        c.releaseMs.store (next.releaseMs, std::memory_order_relaxed);
        c.kneeDb.store (next.kneeDb, std::memory_order_relaxed);
        c.makeupDb.store (next.makeupDb, std::memory_order_relaxed);
        c.gateThresholdDb.store (next.gateThresholdDb, std::memory_order_relaxed);
        c.gateRangeDb.store (next.gateRangeDb, std::memory_order_relaxed);
        c.gateAttackMs.store (next.gateAttackMs, std::memory_order_relaxed);
        c.gateHoldMs.store (next.gateHoldMs, std::memory_order_relaxed);
        c.gateReleaseMs.store (next.gateReleaseMs, std::memory_order_relaxed);
        c.keyLowCutHz.store (next.keyLowCutHz, std::memory_order_relaxed);
        c.keyHighCutHz.store (next.keyHighCutHz, std::memory_order_relaxed);

        onFlag.store (next.on ? 1 : 0, std::memory_order_relaxed);
        revision.fetch_add (1, std::memory_order_release);
    }

    DynSettings CueDyn::settings() const noexcept
    {
        const auto& c = control;
        DynSettings out;
        out.on = c.on.load (std::memory_order_relaxed) != 0;
        out.compOn = c.compOn.load (std::memory_order_relaxed) != 0;
        out.gateOn = c.gateOn.load (std::memory_order_relaxed) != 0;
        out.thresholdDb = c.thresholdDb.load (std::memory_order_relaxed);
        out.ratio = c.ratio.load (std::memory_order_relaxed);
        out.attackMs = c.attackMs.load (std::memory_order_relaxed);
        out.releaseMs = c.releaseMs.load (std::memory_order_relaxed);
        out.kneeDb = c.kneeDb.load (std::memory_order_relaxed);
        out.makeupDb = c.makeupDb.load (std::memory_order_relaxed);
        out.gateThresholdDb = c.gateThresholdDb.load (std::memory_order_relaxed);
        out.gateRangeDb = c.gateRangeDb.load (std::memory_order_relaxed);
        out.gateAttackMs = c.gateAttackMs.load (std::memory_order_relaxed);
        out.gateHoldMs = c.gateHoldMs.load (std::memory_order_relaxed);
        out.gateReleaseMs = c.gateReleaseMs.load (std::memory_order_relaxed);
        out.keyLowCutHz = c.keyLowCutHz.load (std::memory_order_relaxed);
        out.keyHighCutHz = c.keyHighCutHz.load (std::memory_order_relaxed);
        return out;
    }

    void CueDyn::reset() noexcept
    {
        resetRequested.store (1, std::memory_order_release);
    }

    //==============================================================================
    void CueDyn::applyIfMoved() noexcept
    {
        const auto now = revision.load (std::memory_order_acquire);

        if (now == appliedRevision)
            return;

        appliedRevision = now;
        const auto s = settings();

        /*  Spatcore's numbers for ours. No lookahead and no detector delay:
            nothing here delays the audio. One filter pair for both stages. */
        fx::EffectChannelParams params;
        auto& d = params.dyn[0];
        d.bypass = 0;
        d.detector = 0;
        d.autoMakeup = 0;
        d.lookaheadMs = 0.0f;
        d.compOn = s.compOn ? 1 : 0;
        d.expOn = s.gateOn ? 1 : 0;
        d.makeupDb = s.makeupDb;
        d.compThresholdDb = s.thresholdDb;
        d.compRatio = s.ratio;
        d.compKneeDb = s.kneeDb;
        d.compAttackMs = s.attackMs;
        d.compReleaseMs = s.releaseMs;
        d.compDetectorDelayMs = 0.0f;
        d.compScLoCutHz = s.keyLowCutHz;
        d.compScHiCutHz = s.keyHighCutHz;
        d.expThresholdDb = s.gateThresholdDb;
        d.expRatio = DynSettings::gateRatio;
        d.expRangeDb = s.gateRangeDb;
        d.expAttackMs = s.gateAttackMs;
        d.expHoldMs = s.gateHoldMs;
        d.expReleaseMs = s.gateReleaseMs;
        d.expScLoCutHz = s.keyLowCutHz;
        d.expScHiCutHz = s.keyHighCutHz;

        module->applyParams (params, 0);
    }

    void CueDyn::process (float* const* channelData, int numChannelsToProcess, int numSamples,
                          bool keyed, const float* const* key, int keyChannels) noexcept WFG_AUDIO_THREAD
    {
        if (channelData == nullptr || numSamples <= 0 || channels <= 0)
            return;

        applyIfMoved();

        const auto on = onFlag.load (std::memory_order_relaxed) != 0;

        if (resetRequested.exchange (0, std::memory_order_acq_rel) != 0 || (on && ! wasOn))
            module->reset();

        wasOn = on;

        //  OFF IS NOT CALLED: the block untouched, to the bit.
        if (! on)
        {
            reduction.store (0.0f, std::memory_order_relaxed);
            return;
        }

        const auto frames = std::min (numSamples, blockLimit);
        const auto usable = std::min (numChannelsToProcess, channels);

        if (usable <= 0)
            return;

        const float* own[2] = { nullptr, nullptr };
        const float* const* heard = key;
        auto heardChannels = keyed ? std::clamp (keyChannels, 0, 2) : 0;

        if (! keyed)
        {
            /*  ITSELF, BEFORE ITS LEVEL (BHU). One or two channels pointed at
                where they are - the module reads a sample's key before it
                writes that sample, so the alias is safe; wider, folded. */
            if (usable <= 2)
            {
                for (int ch = 0; ch < usable; ++ch)
                    own[ch] = channelData[ch];

                heardChannels = usable;
            }
            else
            {
                float* sides[2] = { fold.data(), fold.data() + blockLimit };
                const auto scale = 2.0f / static_cast<float> (usable);
                std::fill_n (sides[0], frames, 0.0f);
                std::fill_n (sides[1], frames, 0.0f);

                for (int ch = 0; ch < usable; ++ch)
                    if (const auto* from = channelData[ch])
                        for (int n = 0; n < frames; ++n)
                            sides[ch % 2][n] += from[n] * scale;

                own[0] = sides[0];
                own[1] = sides[1];
                heardChannels = 2;
            }

            heard = own;
        }

        module->processKeyed (channelData, usable, heardChannels > 0 ? heard : nullptr, heardChannels, frames);
        reduction.store (module->getMeterDb(), std::memory_order_relaxed);
    }
}
