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

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <memory>

/*  One cue's output stage: a level, and a matrix of gains from its channels to
    the rig's output channels.

    WHY THIS IS A SEPARATE CLASS FROM THE PLUGIN THAT HOLDS IT. Two reasons, and
    the second is the load-bearing one. It can be tested without standing a
    Tracktion engine up, so the arithmetic is checked in microseconds rather
    than in the six seconds an engine takes to build. And it names no Tracktion
    type at all, so it is liftable into spatcore, where the author's other
    projects could use the same matrix rather than each growing their own.

    WHY A MATRIX AT ALL. PRD §3.9b: a cue's destinations are "a list, not a
    choice" - a source into a spatial processor AND a stereo feed to foldback is
    ordinary, not exotic. Spike 04 measured that changing a track's output device
    rebuilds the playback graph, so destinations cannot be structural. They are
    coefficients, and changing one is an atomic store.

    THREADS. Every setter is callable from any thread and does one relaxed
    atomic store. process() is the audio thread and allocates nothing, takes no
    lock and makes no syscall (PRD §4.2) - everything it touches is sized in
    prepare().

    SLEWING, and the one detail that is not a preference. The smoothers are
    LINEAR, never multiplicative. A multiplicative smoother cannot ramp to or
    from zero: it produces a NaN burst, which on a PA is a loud crack, and JUCE
    guards it with nothing but a debug assertion. This is WFS-DIY's convention
    and it is here for the reason WFS-DIY records - having been bitten once.
*/
namespace wfg::audio
{
    class CueMatrix
    {
    public:
        CueMatrix() = default;

        /** Silence. A level of -120 dB or below is exactly zero gain, not a very
            small one, so a fade to silence reaches digital silence. */
        static constexpr float silenceDb = -120.0f;

        /** The loudest a cue may be asked to be. Above unity because a quiet
            recording is a real thing; not unbounded, because a typo should not
            be able to ask for +400 dB. */
        static constexpr float maximumDb = 12.0f;

        /** How long a changed coefficient takes to arrive, in seconds. */
        static constexpr double slewSeconds = 0.05;

        /*  Sizes everything. Message thread, before the audio thread can see
            this object. Calling it again re-sizes and snaps every smoother to
            its target, which is what makes a device change safe: the matrix
            does not ramp from whatever the old rig happened to be at. */
        void prepare (int numInputs, int numOutputs, double sampleRate, int maxBlockSize);

        int numInputs() const noexcept   { return inputs; }
        int numOutputs() const noexcept  { return outputs; }

        /** The cue's level. Any thread. */
        void setLevelDb (float decibels) noexcept;

        /** The gain from one input channel to one output channel. Any thread.
            Out-of-range indices are ignored rather than asserted: this is
            reachable from a client's node write, and a bad index is that
            client's mistake to be told about, not this object's to crash on. */
        void setGain (int input, int output, float gain) noexcept;

        float levelDb() const noexcept;
        float gain (int input, int output) const noexcept;

        /** Snaps every smoother to its target, so nothing ramps from stale
            state. Message thread, with the audio stopped. */
        void snapToTargets() noexcept;

        /*  The audio thread.

            `input` holds `numInputChannels` pointers of at least `numSamples`
            frames; `output` holds `numOutputChannels` of the same. Output is
            OVERWRITTEN, not added to - this stage is the whole of what the cue
            contributes, so anything already there was somebody else's and would
            be summed twice.

            Extra channels beyond what prepare() was told about are cleared
            rather than left, because a stale buffer is worse than silence. */
        void process (const float* const* input, int numInputChannels,
                      float* const* output, int numOutputChannels,
                      int numSamples) noexcept;

    private:
        int index (int input, int output) const noexcept { return input * outputs + output; }

        int inputs = 0;
        int outputs = 0;
        int blockLimit = 0;

        /*  std::atomic is not movable, so it cannot live in a std::vector - the
            same reason WFS-DIY holds its attenuation targets this way. */
        std::unique_ptr<std::atomic<float>[]> gainTargets;
        std::atomic<float> levelTargetDb { 0.0f };

        std::vector<juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>> gainSmoothers;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> levelSmoother;

        /*  The level's ramp for one block, computed once and applied to every
            output - otherwise a smoother shared across N outputs would advance
            N times per sample and arrive N times too fast. */
        std::vector<float> levelRamp;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CueMatrix)
    };
}
