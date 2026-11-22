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
      - AN AUX and THIS CUE come with their stages (SC.3, SC.4); until then
        they are no key.
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
            nought for no key. The pointers are good until the block ends. */
        int read (int start, int samples, const float* (&out)[2]) noexcept
        {
            out[0] = out[1] = nullptr;
            const auto wanted = source();

            if (wanted.kind == KeySource::Kind::input)
                return readInput (wanted, start, samples, out);

            return 0;
        }

    private:
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
        std::vector<float> scratch;
        int scratchSamples = 0;
    };
}
