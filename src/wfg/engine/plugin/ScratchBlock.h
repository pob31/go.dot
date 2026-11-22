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
    THE PLUGIN'S BLOCK, AT ANY WIDTH, WITH NOTHING ALLOCATED (namespace draft
    §142). The child's worker hands a real plugin its scratch at the length the
    parent sent. It used to build a view for that - a `juce::AudioBuffer` over
    the scratch's channel pointers - every block; JUCE keeps such a view's
    pointer list in the buffer itself only up to 31 channels, and mallocs it
    from 32. A lane that wide (a 31-channel plugin and a stereo key is one) then
    allocated on the worker every block.

    So the scratch itself is set to the block's length, in place: made once at
    the longest block, a shorter or a longer one up to that is only its channel
    pointers laid out again inside the memory it already holds - nothing
    allocated, nothing copied (`setSize` with `avoidReallocating`, its content
    not kept: every block clears and fills it). The caller never asks for more
    than the scratch was made for; that is what keeps it in place.
*/

#include <juce_audio_basics/juce_audio_basics.h>

namespace wfg::plugin
{
    /** The worker's thread. `numSamples` is at most what `scratch` was made for. */
    inline void fitScratch (juce::AudioBuffer<float>& scratch, int numSamples)
    {
        if (scratch.getNumSamples() != numSamples)
            scratch.setSize (scratch.getNumChannels(), numSamples, false, false, true);
    }
}
