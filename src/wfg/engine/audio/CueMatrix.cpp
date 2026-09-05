/* This file is part of Go.dot — https://github.com/pob31/go.dot
 *
 * Copyright (C) 2026 Pierre-Olivier Boulant
 *
 * Go.dot is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. Go.dot is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * (LICENSE, at the repository root) for more details.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <wfg/engine/audio/CueMatrix.h>

#include <algorithm>

namespace wfg::audio
{
    namespace
    {
        /*  dB to a linear gain, with a floor that really is zero.

            juce::Decibels::decibelsToGain takes the floor as its second
            argument and returns exactly 0 at or below it, which is what makes a
            fade to silenceDb reach digital silence rather than -120 dB of
            something. */
        float gainForDecibels (float decibels) noexcept
        {
            return juce::Decibels::decibelsToGain (decibels, CueMatrix::silenceDb);
        }
    }

    //==============================================================================
    void CueMatrix::prepare (int numInputs, int numOutputs, double sampleRate, int maxBlockSize)
    {
        inputs = std::max (0, numInputs);
        outputs = std::max (0, numOutputs);
        blockLimit = std::max (0, maxBlockSize);

        const auto cells = static_cast<std::size_t> (inputs) * static_cast<std::size_t> (outputs);

        /*  Allocated here and nowhere else. Every pointer the audio thread
            follows is sized by this call, which is the whole of how §4.2 is
            kept on this path. */
        gainTargets = std::make_unique<std::atomic<float>[]> (cells);
        gainSmoothers.assign (cells, {});
        levelRamp.assign (static_cast<std::size_t> (blockLimit), 0.0f);

        for (std::size_t i = 0; i < cells; ++i)
            gainTargets[i].store (0.0f, std::memory_order_relaxed);

        const auto rate = sampleRate > 0.0 ? sampleRate : 1.0;

        for (auto& smoother : gainSmoothers)
            smoother.reset (rate, slewSeconds);

        levelSmoother.reset (rate, levelSlewSeconds);

        snapToTargets();
    }

    //==============================================================================
    void CueMatrix::setLevelDb (float decibels) noexcept
    {
        levelTargetDb.store (juce::jlimit (silenceDb, maximumDb, decibels),
                             std::memory_order_relaxed);
    }

    void CueMatrix::setGain (int input, int output, float gain) noexcept
    {
        if (input < 0 || input >= inputs || output < 0 || output >= outputs)
            return;

        gainTargets[static_cast<std::size_t> (index (input, output))]
            .store (gain, std::memory_order_relaxed);
    }

    float CueMatrix::levelDb() const noexcept
    {
        return levelTargetDb.load (std::memory_order_relaxed);
    }

    float CueMatrix::gain (int input, int output) const noexcept
    {
        if (input < 0 || input >= inputs || output < 0 || output >= outputs)
            return 0.0f;

        return gainTargets[static_cast<std::size_t> (index (input, output))]
                 .load (std::memory_order_relaxed);
    }

    void CueMatrix::snapToTargets() noexcept
    {
        for (std::size_t i = 0; i < gainSmoothers.size(); ++i)
            gainSmoothers[i].setCurrentAndTargetValue (gainTargets[i].load (std::memory_order_relaxed));

        levelSmoother.setCurrentAndTargetValue (gainForDecibels (levelDb()));
    }

    //==============================================================================
    void CueMatrix::process (const float* const* input, int numInputChannels,
                             float* const* output, int numOutputChannels,
                             int numSamples) noexcept WFG_AUDIO_THREAD
    {
        if (output == nullptr || numOutputChannels <= 0 || numSamples <= 0)
            return;

        /*  A block larger than we were prepared for is a wiring mistake. Caught
            in debug; in release the excess stays silent rather than reading past
            the level ramp. */
        jassert (numSamples <= blockLimit);

        const auto frames = std::min (numSamples, blockLimit);

        /*  Channels this matrix was never sized for are cleared rather than
            left alone. A device with more outputs than the show declares is a
            real case - the rig is bigger than the show - and leaving those rows
            untouched would play whatever the last graph put there. */
        for (int out = 0; out < numOutputChannels; ++out)
            if (output[out] != nullptr)
                juce::FloatVectorOperations::clear (output[out], numSamples);

        if (frames <= 0 || inputs <= 0 || outputs <= 0 || input == nullptr)
            return;

        const auto usableIn = std::min (numInputChannels, inputs);
        const auto usableOut = std::min (numOutputChannels, outputs);

        /*  The level's ramp for this block, computed once. Applied per output
            below rather than per (input, output) pair, so a cue's level moves
            at the rate it was asked to move at however many destinations it
            feeds. */
        levelSmoother.setTargetValue (gainForDecibels (levelDb()));

        const auto levelIsMoving = levelSmoother.isSmoothing();

        if (levelIsMoving)
            for (int n = 0; n < frames; ++n)
                levelRamp[static_cast<std::size_t> (n)] = levelSmoother.getNextValue();

        const auto steadyLevel = levelIsMoving ? 0.0f : levelSmoother.getCurrentValue();

        for (int out = 0; out < usableOut; ++out)
        {
            auto* destination = output[out];

            if (destination == nullptr)
                continue;

            for (int in = 0; in < usableIn; ++in)
            {
                const auto* source = input[in];

                if (source == nullptr)
                    continue;

                auto& smoother = gainSmoothers[static_cast<std::size_t> (index (in, out))];

                smoother.setTargetValue (
                    gainTargets[static_cast<std::size_t> (index (in, out))]
                        .load (std::memory_order_relaxed));

                if (smoother.isSmoothing())
                {
                    /*  Per sample only while it is actually moving. This is the
                        expensive path and it lasts 50 ms after a change. */
                    for (int n = 0; n < frames; ++n)
                        destination[n] += source[n] * smoother.getNextValue();
                }
                else
                {
                    const auto steady = smoother.getCurrentValue();

                    /*  A coefficient of zero is most of a real matrix - a cue
                        feeds two destinations out of sixty-four - so skipping
                        it is not a micro-optimisation, it is the common case. */
                    if (! juce::exactlyEqual (steady, 0.0f))
                        juce::FloatVectorOperations::addWithMultiply (destination, source,
                                                                      steady, frames);
                }
            }

            if (levelIsMoving)
                juce::FloatVectorOperations::multiply (destination, levelRamp.data(), frames);
            else if (! juce::exactlyEqual (steadyLevel, 1.0f))
                juce::FloatVectorOperations::multiply (destination, steadyLevel, frames);
        }
    }
}
