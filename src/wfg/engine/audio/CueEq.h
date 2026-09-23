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

#include <wfg/engine/audio/EqMath.h>
#include <wfg/engine/audio/EqSettings.h>
#include <wfg/engine/rt/RtCheck.h>

#include <atomic>
#include <cstdint>
#include <vector>

/*
    One voice's EQ: six second-order sections per channel, and the atomics a
    tick writes to move them.

    WHY THIS IS A SEPARATE CLASS FROM THE PLUGIN THAT HOLDS IT - CueMatrix's
    reason, exactly. It can be tested without standing a Tracktion engine up,
    so a band's response is checked against EqMath in microseconds rather than
    in the six seconds an engine takes to build; and it names no Tracktion
    type, so the arithmetic is liftable.

    THREADS. set() is callable from any thread and does relaxed stores plus one
    release store per section whose numbers changed; process() is the audio
    thread and allocates nothing, takes no lock and makes no syscall (PRD §4.2)
    - everything it touches is sized in prepare(). A section whose revision
    moved rebuilds its coefficients at the start of the next block, on the
    audio thread, from the atomics: a few transcendental functions, which is
    arithmetic and not a violation. There is no crossfade between the old
    coefficients and the new (plan decision 7): a rotary at fifty writes a
    second makes small steps, and a one-block ramp is the fix if a large step
    ever clicks.

    IDENTITY IS BIT-EXACT. When the EQ is off, or every section is out, the
    block is not touched at all - not multiplied by one, not copied - because
    every render driver in the tree runs through this stage from now on, and
    their arithmetic is that each output sample equals the gain.

    RESET IS A REQUEST. Clearing the filter state from the message thread while
    the audio thread is running the filters would be a race; instead reset()
    raises a flag the next process() honours before it filters anything. An
    arm asks for it while the voice is silent, so the previous cue's tail is
    not in the delay lines when the next one starts.
*/
namespace wfg::audio
{
    class CueEq
    {
    public:
        CueEq() = default;

        /** The most channels one voice may carry through this stage. Wider
            input is passed through untouched beyond it. */
        static constexpr int maxChannels = 64;

        /** Sizes everything. Message thread, before the audio thread can see
            this object; again on a sample-rate or block-size change, with the
            audio stopped. Snaps every section to the current settings. */
        void prepare (int numChannels, double sampleRate, int maxBlockSize);

        /** The settings to move to. Any thread. Sections whose numbers changed
            rebuild on the next block; the rest are untouched. */
        void set (const EqSettings&) noexcept;

        /** What was last set, read back from the atomics. Any thread. */
        EqSettings settings() const noexcept;

        /** Clears the filter state at the start of the next block. Any thread. */
        void reset() noexcept;

        /** Whether the settings last set would change a signal. Any thread. */
        bool isIdentity() const noexcept;

        int numChannels() const noexcept    { return channels; }
        double sampleRate() const noexcept  { return rate; }

        /*  The audio thread. Filters `numChannelsToProcess` channels of
            `numSamples` frames in place, up to the count prepare() was given;
            channels beyond it are left as they are. */
        void process (float* const* channelData, int numChannelsToProcess,
                      int numSamples) noexcept WFG_AUDIO_THREAD;

    private:
        /** hpf, lpf, then the four bands. */
        static constexpr int numSections = 2 + EqSettings::numBands;

        struct SectionControl
        {
            std::atomic<std::uint32_t> revision { 0 };
            std::atomic<int> flagOrShape { 0 };
            std::atomic<float> freq { 1000.0f };
            std::atomic<float> gain { 0.0f };
            std::atomic<float> q { 0.7f };
        };

        struct SectionState
        {
            eqmath::Coefficients coefficients;
            bool active = false;
            std::uint32_t builtRevision = 0;

            /** Transposed direct form II, two delays per channel. */
            std::vector<double> z1, z2;
        };

        void rebuildIfNeeded (int section) noexcept;
        void clearState (SectionState&) noexcept;

        int channels = 0;
        int blockLimit = 0;
        double rate = 48000.0;

        std::atomic<int> onFlag { 1 };
        std::atomic<std::uint32_t> resetRequested { 0 };

        SectionControl control[numSections];
        SectionState state[numSections];

        /*  What set() last stored, kept on the writing side so a call that
            changes nothing bumps no revision. Owned by whoever calls set(),
            which is one thread at a time by contract (the tick thread, or the
            message thread while the voice is silent). */
        EqSettings lastSet;
        bool everSet = false;
    };
}
