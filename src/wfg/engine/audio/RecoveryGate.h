/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <atomic>
#include <cstdint>

namespace wfg::audio
{
    // Callbacks only touch atomics. The message-thread watchdog supplies time;
    // silent validation blocks never advance the playback graph or show clock.
    class RecoveryGate
    {
    public:
        void started (bool compatible) noexcept
        {
            const auto next = ((state.load() >> 2) + 1) << 2;
            state.store (next | (compatible ? checking : invalid));
        }
        void stopped() noexcept { started (false); }
        bool callback() noexcept
        { heartbeat.fetch_add (1); return (state.load() & mask) == flowing; }
        bool paused() const noexcept { return (state.load() & mask) != flowing; }
        bool resume (bool initialOpen = false) noexcept
        {
            auto expected = state.load();
            if ((expected & mask) != (initialOpen ? checking : ready)) return false;
            return state.compare_exchange_strong (expected, (expected & ~mask) | flowing);
        }
        struct Observation { bool ready = false, retry = false; };
        Observation observe (double now)
        {
            auto observed = state.load();
            const auto serial = observed >> 2;
            const auto count = heartbeat.load();
            if (serial != seenGeneration || lastProgress < 0)
            { seenGeneration = serial; stableSince = now; lastProgress = now; seenHeartbeat = count; firstHeartbeat = count; }
            if (count != seenHeartbeat)
            { seenHeartbeat = count; lastProgress = now; }
            const auto stalled = now - lastProgress >= 500.0;
            if (stalled)
            {
                if ((observed & mask) != invalid)
                    state.compare_exchange_strong (observed, (observed & ~mask) | checking);
                stableSince = now;
                firstHeartbeat = count;
            }
            else if ((observed & mask) == checking && count - firstHeartbeat >= 3
                     && now - lastProgress < 100.0 && now - stableSince >= 250.0)
                state.compare_exchange_strong (observed, (observed & ~mask) | ready);
            return { (state.load() & mask) == ready,
                     (state.load() & mask) == invalid || stalled };
        }
    private:
        static constexpr std::uint64_t invalid = 0, checking = 1, ready = 2, flowing = 3, mask = 3;
        // Generation and gate state share one atomic: a stop/restart cannot be
        // overwritten by an approval for an earlier device callback generation.
        std::atomic<std::uint64_t> state { 0 }, heartbeat { 0 };
        std::uint64_t seenHeartbeat = 0, seenGeneration = 0, firstHeartbeat = 0;
        double stableSince = 0, lastProgress = -1;
    };
}
