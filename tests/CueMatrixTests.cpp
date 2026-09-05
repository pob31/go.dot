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

/*  The cue output matrix: a level and a grid of gains, slewed per block.

    These run without a Tracktion engine, which is the point of the class being
    separate from the plugin that will hold it - the arithmetic is checked in
    microseconds instead of the six seconds an engine takes to build.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/audio/CueMatrix.h>

#include "TestSupport.h"

#include <cmath>
#include <vector>

using namespace wfg;

namespace
{
    /*  A little rig that owns its buffers, so a case reads as what it is about
        rather than as pointer bookkeeping. */
    struct Buffers
    {
        Buffers (int numIn, int numOut, int numSamples)
            : input (numIn, numSamples), output (numOut, numSamples)
        {
            input.clear();
            output.clear();
        }

        const float* const* in() const  { return input.getArrayOfReadPointers(); }
        float* const* out()             { return output.getArrayOfWritePointers(); }

        void fill (int channel, float value)
        {
            for (int n = 0; n < input.getNumSamples(); ++n)
                input.setSample (channel, n, value);
        }

        juce::AudioBuffer<float> input, output;
    };

    constexpr double rate = 48000.0;
    constexpr int block = 64;

    /** Long enough for a 50 ms slew at 48 kHz to have finished, with margin. */
    constexpr int blocksToSettle = 64;

    void run (audio::CueMatrix& matrix, Buffers& buffers, int blocks)
    {
        for (int i = 0; i < blocks; ++i)
            matrix.process (buffers.in(), buffers.input.getNumChannels(),
                            buffers.out(), buffers.output.getNumChannels(),
                            buffers.input.getNumSamples());
    }
}

//==============================================================================
TEST_CASE ("cue matrix: a coefficient of one passes the signal through untouched")
{
    audio::CueMatrix matrix;
    matrix.prepare (1, 1, rate, block);
    matrix.setGain (0, 0, 1.0f);
    matrix.setLevelDb (0.0f);
    matrix.snapToTargets();

    Buffers buffers { 1, 1, block };
    buffers.fill (0, 0.25f);

    run (matrix, buffers, 1);

    for (int n = 0; n < block; ++n)
        REQUIRE (buffers.output.getSample (0, n) == doctest::Approx (0.25f));
}

TEST_CASE ("cue matrix: a cue reaches the destinations it names and no others")
{
    /*  PRD 3.9b: destinations are a list, not a choice. Here one input feeds
        outputs 2 and 5 of a six-channel rig, and the other four stay silent -
        which is the property that makes a matrix the right shape for this. */
    audio::CueMatrix matrix;
    matrix.prepare (1, 6, rate, block);
    matrix.setGain (0, 2, 1.0f);
    matrix.setGain (0, 5, 0.5f);
    matrix.snapToTargets();

    Buffers buffers { 1, 6, block };
    buffers.fill (0, 1.0f);

    run (matrix, buffers, 1);

    CHECK (buffers.output.getSample (2, 0) == doctest::Approx (1.0f));
    CHECK (buffers.output.getSample (5, 0) == doctest::Approx (0.5f));

    for (const int silent : { 0, 1, 3, 4 })
    {
        INFO ("output channel " << silent);
        CHECK (buffers.output.getMagnitude (silent, 0, block) == doctest::Approx (0.0f));
    }
}

TEST_CASE ("cue matrix: two inputs into one output sum")
{
    audio::CueMatrix matrix;
    matrix.prepare (2, 1, rate, block);
    matrix.setGain (0, 0, 1.0f);
    matrix.setGain (1, 0, 1.0f);
    matrix.snapToTargets();

    Buffers buffers { 2, 1, block };
    buffers.fill (0, 0.25f);
    buffers.fill (1, 0.5f);

    run (matrix, buffers, 1);

    CHECK (buffers.output.getSample (0, 0) == doctest::Approx (0.75f));
}

TEST_CASE ("cue matrix: the level is in decibels, and its floor is real silence")
{
    /*  -120 dB is not a very small number, it is zero. A fade to silence that
        left -120 dB of signal on a bus would still be summed with everything
        else on it, sixty-four times over. */
    audio::CueMatrix matrix;
    matrix.prepare (1, 1, rate, block);
    matrix.setGain (0, 0, 1.0f);

    SUBCASE ("minus six decibels is about half")
    {
        matrix.setLevelDb (-6.0f);
        matrix.snapToTargets();

        Buffers buffers { 1, 1, block };
        buffers.fill (0, 1.0f);
        run (matrix, buffers, 1);

        CHECK (buffers.output.getSample (0, 0) == doctest::Approx (0.501f).epsilon (0.01));
    }

    SUBCASE ("the floor is exactly zero")
    {
        matrix.setLevelDb (audio::CueMatrix::silenceDb);
        matrix.snapToTargets();

        Buffers buffers { 1, 1, block };
        buffers.fill (0, 1.0f);
        run (matrix, buffers, 1);

        for (int n = 0; n < block; ++n)
            REQUIRE (buffers.output.getSample (0, n) == 0.0f);
    }
}

TEST_CASE ("cue matrix: a changed coefficient arrives smoothly rather than as a step")
{
    /*  The reason is a loudspeaker: a coefficient that jumped would be a click.
        What is checked is that it moves, that it does not overshoot, and that
        it arrives - not the exact shape, which is JUCE's to define. */
    audio::CueMatrix matrix;
    matrix.prepare (1, 1, rate, block);
    matrix.setGain (0, 0, 0.0f);
    matrix.snapToTargets();

    Buffers buffers { 1, 1, block };
    buffers.fill (0, 1.0f);

    matrix.setGain (0, 0, 1.0f);
    run (matrix, buffers, 1);

    const auto firstBlock = buffers.output.getSample (0, block - 1);

    INFO ("after one block: " << firstBlock);
    CHECK (firstBlock > 0.0f);
    CHECK (firstBlock < 1.0f);

    /*  Monotonic while it climbs. A smoother that overshot would put a
        coefficient somewhere nobody asked for, briefly. */
    for (int n = 1; n < block; ++n)
        REQUIRE (buffers.output.getSample (0, n) >= buffers.output.getSample (0, n - 1));

    run (matrix, buffers, blocksToSettle);

    CHECK (buffers.output.getSample (0, block - 1) == doctest::Approx (1.0f));
}

TEST_CASE ("cue matrix: a level change slews too, and settles exactly on its target")
{
    audio::CueMatrix matrix;
    matrix.prepare (1, 1, rate, block);
    matrix.setGain (0, 0, 1.0f);
    matrix.setLevelDb (0.0f);
    matrix.snapToTargets();

    Buffers buffers { 1, 1, block };
    buffers.fill (0, 1.0f);

    matrix.setLevelDb (-20.0f);
    run (matrix, buffers, 1);

    const auto partway = buffers.output.getSample (0, block - 1);

    INFO ("partway: " << partway);
    CHECK (partway < 1.0f);
    CHECK (partway > 0.1f);

    run (matrix, buffers, blocksToSettle);

    CHECK (buffers.output.getSample (0, block - 1) == doctest::Approx (0.1f).epsilon (0.01));
}

TEST_CASE ("cue matrix: outputs the show never mentioned are cleared, not left alone")
{
    /*  A rig with more outputs than the show declares is ordinary - the
        building has more speakers than tonight needs. Those channels must be
        silent, and silent means written, because whatever the last graph left
        in that buffer would otherwise play. */
    audio::CueMatrix matrix;
    matrix.prepare (1, 2, rate, block);
    matrix.setGain (0, 0, 1.0f);
    matrix.snapToTargets();

    Buffers buffers { 1, 4, block };
    buffers.fill (0, 1.0f);

    for (int channel = 0; channel < 4; ++channel)
        for (int n = 0; n < block; ++n)
            buffers.output.setSample (channel, n, 0.9f);

    run (matrix, buffers, 1);

    CHECK (buffers.output.getSample (0, 0) == doctest::Approx (1.0f));

    for (const int silent : { 1, 2, 3 })
    {
        INFO ("output channel " << silent);
        CHECK (buffers.output.getMagnitude (silent, 0, block) == doctest::Approx (0.0f));
    }
}

TEST_CASE ("cue matrix: an out-of-range coefficient is ignored rather than fatal")
{
    /*  These indices arrive from a client's write, so a bad one is that
        client's mistake to be told about through the ordinary refusal - not
        this object's to die on, on the audio thread, mid-show. */
    audio::CueMatrix matrix;
    matrix.prepare (2, 2, rate, block);

    matrix.setGain (-1, 0, 1.0f);
    matrix.setGain (0, 99, 1.0f);
    matrix.setGain (99, 99, 1.0f);

    CHECK (matrix.gain (-1, 0) == 0.0f);
    CHECK (matrix.gain (0, 99) == 0.0f);
    CHECK (matrix.gain (0, 0) == 0.0f);
}

TEST_CASE ("cue matrix: the level is clamped to the range the table declares")
{
    audio::CueMatrix matrix;
    matrix.prepare (1, 1, rate, block);

    matrix.setLevelDb (400.0f);
    CHECK (matrix.levelDb() == doctest::Approx (audio::CueMatrix::maximumDb));

    matrix.setLevelDb (-9999.0f);
    CHECK (matrix.levelDb() == doctest::Approx (audio::CueMatrix::silenceDb));
}
