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

#include <wfg/engine/audio/DynSettings.h>
#include <wfg/engine/rt/RtCheck.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace spatcore::effects
{
    class DynamicsModule;
}

/*
    One track's dynamics stage of Go.dot's own (namespace draft §141, SC.6):
    spatcore's DynamicsModule through its keyed, linked entry (BHN), and the
    atomics a tick writes to move it.

    SEPARATE FROM THE PLUGIN THAT HOLDS IT, for CueEq's reason: tested without a
    Tracktion engine, and naming no Tracktion type. Spatcore's type stays in
    the .cpp too, so nothing that includes this needs spatcore's headers.

    WHAT IT LISTENS TO (BHU). With no key picked, the track itself before its
    level - one or two channels pointed at where they are, wider ones folded to
    two at a half per pair (BHT) - as a console's channel compressor listens.
    With a key picked, the key, whatever the caller found for it: none found is
    a silent key (BHV), on which the compressor rests and the gate shuts. One
    gain on every channel, the louder side of the key heard (BHT).

    THREADS. set() from any thread: relaxed stores and one release bump of a
    revision. process() is the audio thread: a moved revision is applied to the
    module at the start of the block (biquad design: arithmetic, PRD §4.2 holds),
    nothing allocates, nothing locks. reset() is a request the next block
    honours, as CueEq's.

    OFF IS NOT CALLED. Off, process() returns before touching anything; the
    track is the same to the bit. Switched on again it starts from a cleared
    detector and unity gain, never from a sound that stopped minutes ago.
*/
namespace wfg::audio
{
    class CueDyn
    {
    public:
        CueDyn();
        ~CueDyn();

        CueDyn (const CueDyn&) = delete;
        CueDyn& operator= (const CueDyn&) = delete;

        static constexpr int maxChannels = 64;

        /** Sizes everything. Message thread, before the audio thread can see
            this object, and again with the audio stopped. */
        void prepare (int numChannels, double sampleRate, int maxBlockSize);

        /** The settings to move to. Any thread; nothing moves when nothing changed. */
        void set (const DynSettings&) noexcept;

        /** What was last set. Any thread. */
        DynSettings settings() const noexcept;

        /** Clears the detectors and the gain at the start of the next block. Any thread. */
        void reset() noexcept;

        bool isOn() const noexcept  { return onFlag.load (std::memory_order_relaxed) != 0; }

        /** The deepest reduction of the last block, in decibels, nought or
            below; nought while off. Any thread - a meter's. */
        float reductionDb() const noexcept  { return reduction.load (std::memory_order_relaxed); }

        int numChannels() const noexcept    { return channels; }

        /*  The audio thread. `channelData`: `numChannelsToProcess` channels of
            `numSamples` frames, changed in place by one gain. `keyed`: whether
            a key was picked; if so `key` holds `keyChannels` pointers of the
            same span (none is a silent key). Unkeyed, the track hears itself. */
        void process (float* const* channelData, int numChannelsToProcess, int numSamples,
                      bool keyed, const float* const* key, int keyChannels) noexcept WFG_AUDIO_THREAD;

    private:
        void applyIfMoved() noexcept;

        std::unique_ptr<spatcore::effects::DynamicsModule> module;

        int channels = 0;
        int blockLimit = 0;
        double rate = 48000.0;

        //  The settings as atomics, and the revision that says they moved.
        struct Control
        {
            std::atomic<int> on { 0 }, compOn { 1 }, gateOn { 0 };
            std::atomic<float> thresholdDb { -20.0f }, ratio { 4.0f }, attackMs { 10.0f }, releaseMs { 150.0f },
                               kneeDb { 6.0f }, makeupDb { 0.0f };
            std::atomic<float> gateThresholdDb { -50.0f }, gateRangeDb { -40.0f }, gateAttackMs { 1.0f },
                               gateHoldMs { 50.0f }, gateReleaseMs { 150.0f };
            std::atomic<float> keyLowCutHz { 20.0f }, keyHighCutHz { 20000.0f };
        } control;

        std::atomic<std::uint32_t> revision { 0 };
        std::atomic<int> onFlag { 0 };
        std::atomic<std::uint32_t> resetRequested { 0 };
        std::atomic<float> reduction { 0.0f };

        //  The audio thread's own.
        std::uint32_t appliedRevision = 0;
        bool wasOn = false;
        std::vector<float> fold;            // two sides of a wide track folded, sized by prepare

        //  The writer's own, so a set that changes nothing bumps nothing.
        DynSettings lastSet;
        bool everSet = false;
    };
}
