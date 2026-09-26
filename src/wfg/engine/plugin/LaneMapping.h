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
    HOW A VOICE'S CHANNELS MEET A PLUGIN'S (2026-09-26), in the child, between
    the lane and the plugin's buffer. Before this, the voice's channels were
    poured into the first channels of a buffer as wide as the plugin wanted,
    whatever its buses were - a stereo voice on a mono-only plugin put its
    right channel into the plugin's sidechain - and only as many channels came
    back as went in, so a stereo reverb on a mono cue lost its right side.

    THE RULES, which the author's decision of the same day sets for a mono cue
    and a stereo plugin and which are written down here for every width:

      - The cue's channels (`feed`) go to the plugin's main inputs one to one.
        A mono feed goes to EVERY main input: a mono sound through a stereo
        reverb is heard on both of its sides.
      - The plugin takes the block only when the cue is no wider than its
        inputs and its outputs are at least as wide as the cue - a cue never
        comes out narrower than it went in. Otherwise the block passes DRY,
        whole, and the entry says why: never half the channels wet and half dry.
      - What comes back is the plugin's main outputs, channel to channel, as
        many as the lane carries; outputs beyond the lane's width are folded
        in, output `i` onto lane channel `i mod lane`, each at lane / outputs -
        two sides onto one voice channel at a half each.
      - Every other input the plugin has (a sidechain) is fed silence: the
        buffer is cleared first, and only the main inputs are written.

    NAMES NO JUCE TYPE, allocates nothing, locks nothing: the child's worker
    calls it once per block, and a test calls it with plain arrays.
*/

#include <algorithm>

namespace wfg::plugin::lanemap
{
    struct Shape
    {
        /** The channels the cue sends: the lane's width for this block. */
        int feed = 0;

        /** The plugin's main input and main output widths. */
        int inputs = 0;
        int outputs = 0;

        /** How many channels the lane carries back. */
        int laneChannels = 0;
    };

    /** Whether the plugin processes the block, or it passes dry. */
    constexpr bool takes (const Shape& shape) noexcept
    {
        return shape.feed >= 1 && shape.feed <= shape.inputs && shape.outputs >= shape.feed
                 && shape.laneChannels >= 1;
    }

    /*  The cue's channels into the plugin's main inputs - the buffer's first
        `inputs` channels, the rest of it already cleared. A mono feed is
        copied into every one. */
    inline void feedInto (const float* const* lane, const Shape& shape, float* const* buffer, int samples) noexcept
    {
        for (int input = 0; input < shape.inputs; ++input)
        {
            const auto from = shape.feed == 1 ? 0 : input;

            if (from < shape.feed)
                std::copy_n (lane[from], samples, buffer[input]);
        }
    }

    /*  The plugin's main outputs back into the lane: channel to channel up to
        the lane's width, the rest folded in at lane / outputs. */
    inline void backInto (const float* const* buffer, const Shape& shape, float* const* lane, int samples) noexcept
    {
        const auto into = std::min (shape.outputs, shape.laneChannels);

        if (shape.outputs <= shape.laneChannels)
        {
            for (int channel = 0; channel < into; ++channel)
                std::copy_n (buffer[channel], samples, lane[channel]);

            return;
        }

        const auto scale = static_cast<float> (shape.laneChannels) / static_cast<float> (shape.outputs);

        for (int channel = 0; channel < into; ++channel)
            std::fill_n (lane[channel], samples, 0.0f);

        for (int output = 0; output < shape.outputs; ++output)
        {
            auto* target = lane[output % shape.laneChannels];
            const auto* source = buffer[output];

            for (int n = 0; n < samples; ++n)
                target[n] += source[n] * scale;
        }
    }
}
