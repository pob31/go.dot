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

/*  AN audio/-INTERNAL HEADER: it reads the live input tap, which names JUCE's
    buffer. Only files under src/wfg/engine/audio/ and their tests include it. */
#include <wfg/engine/audio/AuxBus.h>
#include <wfg/engine/audio/CueMatrix.h>
#include <wfg/engine/audio/KeySource.h>
#include <wfg/engine/audio/LiveInputPlugin.h>

#include <atomic>
#include <cstdint>
#include <vector>

/*
    ONE EFFECT'S KEY, FOUND FOR A BLOCK (namespace draft §141). Shared by the
    two things that listen to a key - a plugin place's sidechain and Go.dot's
    own dynamics stage - so both find an input, an aux and a cue's own sound
    the same way.

    THE SOURCE IS ONE WORD (KeySource::packed), stored by the tick thread and
    read once a block here. What it names is turned into at most two channel
    pointers for the block's span:

      - AN INPUT is read from the tap the host fills before Tracktion runs
        (LiveInputTap): heard in the same block (BHP). One or two channels are
        pointed at where they are; a wider input is folded to two at a half
        per pair into scratch made before any block runs (BHT).
      - AN AUX is its row of the key bus, which the aux's own tap writes after
        its EQ and inserts, at its level (SC.3): two channels, read where they
        are. When the aux has run before the reader - an aux lower in the list -
        the row is this block's; when it runs after - a cue, a mic channel, an
        aux higher up - it is the block before's (BHP). A row its writer said
        is silent is no key.
      - THIS CUE AFTER ITS LEVEL (SC.4, BHQ) is the sound arriving at the
        effect - handed in by the caller, before the effect touches it -
        multiplied by the gain the cue's output stage is told at this block:
        its own level, trims, DCAs and lane, as one number. The gain ramps
        from the last block's to this one's across the block, so a DCA moved
        in steps is a key moving smoothly. Into scratch, folded to two when
        the cue is wider (BHT). A level at nought on both ends is no key.
      - NO KEY, or one past what the tap holds - an input the interface does
        not give tonight - is nought channels: the caller feeds silence (BHV).

    THE AUDIO THREAD (PRD §4.2): `read` allocates nothing and locks nothing;
    the scratch is sized by `prepare` on the message thread and never after.
*/
namespace wfg::audio
{
    class KeyReader
    {
    public:
        /** The tap an input is read from, bound once before blocks run. Null reads no input. */
        void bindTap (const LiveInputTap* tap) noexcept     { boundTap = tap; }

        /** The key bus an aux is read from (§141, SC.3), bound once. Null reads no aux. */
        void bindKeyBus (const AuxBus* keys) noexcept       { boundKeys = keys; }

        /*  The level a cue's own key is under (§141, SC.4): its output stage's
            matrix, bound once the stage exists. Null is unity. */
        void bindLevel (const CueMatrix* levelOf) noexcept  { boundLevel = levelOf; }

        /** Message thread, before blocks run: room for a folded key of this many samples. */
        void prepare (int maxBlockSamples)
        {
            scratchSamples = std::max (1, maxBlockSamples);
            scratch.assign (static_cast<std::size_t> (2 * scratchSamples), 0.0f);
        }

        /** Any thread but the audio one: what the key is. */
        void setSource (const KeySource& key) noexcept
        {
            packed.store (key.packed(), std::memory_order_relaxed);
        }

        KeySource source() const noexcept
        {
            return KeySource::unpacked (packed.load (std::memory_order_relaxed));
        }

        bool hasKey() const noexcept
        {
            return packed.load (std::memory_order_relaxed) != 0;
        }

        /*  THE AUDIO THREAD: the key for samples [start, start + samples) of
            this block, as at most two pointers into `out`; answers how many -
            nought for no key. The pointers are good until the block ends.
            `own` is the sound arriving at the effect for the same span, from
            its first sample, read for a key of the cue's own. */
        int read (int start, int samples, const float* (&out)[2],
                  const float* const* own = nullptr, int ownChannels = 0) noexcept
        {
            out[0] = out[1] = nullptr;
            const auto wanted = source();

            if (wanted.kind == KeySource::Kind::self)
                return readSelf (own, ownChannels, samples, out);

            if (wanted.kind == KeySource::Kind::input)
                return readInput (wanted, start, samples, out);

            if (wanted.kind == KeySource::Kind::aux)
                return readAux (wanted, start, samples, out);

            return 0;
        }

    private:
        int readSelf (const float* const* own, int ownChannels, int samples, const float* (&out)[2]) noexcept
        {
            if (own == nullptr || ownChannels <= 0 || samples <= 0 || samples > scratchSamples)
                return 0;

            const auto target = boundLevel != nullptr
                                  ? juce::Decibels::decibelsToGain (boundLevel->levelDb(), CueMatrix::silenceDb)
                                  : 1.0f;
            const auto from = selfGain < 0.0f ? target : selfGain;
            selfGain = target;

            if (juce::exactlyEqual (from, 0.0f) && juce::exactlyEqual (target, 0.0f))
                return 0;

            const auto sides = std::min (ownChannels, 2);
            const auto fold = ownChannels > 2 ? 2.0f / static_cast<float> (ownChannels) : 1.0f;
            const auto step = (target - from) / static_cast<float> (samples);
            float* to[2] = { scratch.data(), scratch.data() + scratchSamples };

            for (int side = 0; side < sides; ++side)
                std::fill_n (to[side], samples, 0.0f);

            for (int channel = 0; channel < ownChannels; ++channel)
            {
                const auto* in = own[channel];

                if (in == nullptr)
                    continue;

                auto* side = to[channel % 2];

                for (int n = 0; n < samples; ++n)
                    side[n] += in[n] * fold * (from + step * static_cast<float> (n + 1));
            }

            for (int side = 0; side < sides; ++side)
                out[side] = to[side];

            return sides;
        }

        int readAux (const KeySource& key, int start, int samples, const float* (&out)[2]) const noexcept
        {
            const auto* keys = boundKeys;

            if (keys == nullptr || key.first >= keys->writerCount() || keys->columnCount() < 2
                 || start < 0 || samples <= 0 || start + samples > keys->blockLimit() || keys->isSilent (key.first))
                return 0;

            const auto* left = keys->row (key.first, 0);
            const auto* right = keys->row (key.first, 1);

            if (left == nullptr || right == nullptr)
                return 0;

            out[0] = left + start;
            out[1] = right + start;
            return 2;
        }

        int readInput (const KeySource& key, int start, int samples, const float* (&out)[2]) noexcept
        {
            const auto* tap = boundTap;

            if (tap == nullptr || tap->buffer == nullptr || key.first >= tap->channels || key.width <= 0
                 || start < 0 || start + samples > tap->buffer->getNumSamples())
                return 0;

            const auto width = std::min (key.width, tap->channels - key.first);

            if (width <= 2)
            {
                for (int channel = 0; channel < width; ++channel)
                    out[channel] = tap->buffer->getReadPointer (key.first + channel, start);

                return width;
            }

            //  Wider than two: input i onto side i mod 2, each side at two over the width (§42's fold).
            if (samples > scratchSamples)
                return 0;

            float* sides[2] = { scratch.data(), scratch.data() + scratchSamples };
            const auto scale = 2.0f / static_cast<float> (width);

            std::fill_n (sides[0], samples, 0.0f);
            std::fill_n (sides[1], samples, 0.0f);

            for (int channel = 0; channel < width; ++channel)
            {
                const auto* from = tap->buffer->getReadPointer (key.first + channel, start);
                auto* to = sides[channel % 2];

                for (int n = 0; n < samples; ++n)
                    to[n] += from[n] * scale;
            }

            out[0] = sides[0];
            out[1] = sides[1];
            return 2;
        }

        std::atomic<std::uint32_t> packed { 0 };
        const LiveInputTap* boundTap = nullptr;
        const AuxBus* boundKeys = nullptr;
        const CueMatrix* boundLevel = nullptr;
        float selfGain = -1.0f;             // the last block's level on a cue's own key; the audio thread's
        std::vector<float> scratch;
        int scratchSamples = 0;
    };
}
