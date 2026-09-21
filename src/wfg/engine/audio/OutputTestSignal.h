/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <wfg/engine/audio/AudioSettings.h>
#include <spatcore/io/TestSignalGenerator.h>
#include <atomic>
#include <cstdint>

namespace wfg::audio
{
    // A single atomic message transfers controls to the callback. Every shared
    // generator setter runs on the audio thread, avoiding its non-atomic phase
    // and frequency members being written concurrently by the UI/tick thread.
    class OutputTestSignal
    {
    public:
        void prepare (double rate, int frames)
        {
            scratch.setSize (1, frames);
            generator.setDeterministicSeed (1);
            generator.prepare (rate, frames);
            generator.reset();
            desired.store (0); previous = 0;
        }

        void set (const OutputTestSettings& settings) noexcept
        {
            const auto old = desired.load (std::memory_order_relaxed);
            auto bits = static_cast<std::uint64_t> (settings.type)
                      | (static_cast<std::uint64_t> (settings.channel + 1) << 3)
                      | (static_cast<std::uint64_t> (settings.frequency) << 13)
                      | (static_cast<std::uint64_t> (juce::roundToInt ((settings.level + 92.0) * 10.0)) << 28);
            // Retain start/stop changes even if they arrive within one block.
            const auto generation = (old >> 40) + ((old & 8191) != (bits & 8191) ? 1u : 0u);
            bits |= (generation & 0xffffff) << 40;
            desired.store (bits, std::memory_order_release);
        }

        void render (float* const* outputs, int channels, int frames) noexcept
        {
            const auto bits = desired.load (std::memory_order_acquire);
            const auto type = static_cast<int> (bits & 7);
            const auto channel = static_cast<int> ((bits >> 3) & 1023) - 1;
            if (bits != previous)
            {
                if ((bits >> 40) != (previous >> 40)) generator.reset();
                generator.setFrequency (static_cast<float> ((bits >> 13) & 32767));
                generator.setLevel (static_cast<float> ((bits >> 28) & 1023) * 0.1f - 92.0f);
                generator.setSignalType (static_cast<spatcore::io::TestSignalGenerator::SignalType> (type));
                generator.setOutputChannel (channel >= 0 ? 0 : -1);
                previous = bits;
            }
            if (type == 0 || channel < 0 || channel >= channels || outputs[channel] == nullptr
                || frames > scratch.getNumSamples()) return;
            generator.renderNextBlock (scratch, 0, frames);
            juce::FloatVectorOperations::copy (outputs[channel], scratch.getReadPointer (0), frames);
        }
    private:
        static_assert (std::atomic<std::uint64_t>::is_always_lock_free);
        std::atomic<std::uint64_t> desired { 0 };
        std::uint64_t previous = 0;
        spatcore::io::TestSignalGenerator generator;
        juce::AudioBuffer<float> scratch;
    };
}
