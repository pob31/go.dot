/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <atomic>
#include <cstdint>

namespace wfg::audio
{
    /*  What came back after an outage (PRD §6.2): the interface the show ran
        on, on the clock it ran on; the SAME interface insisting on another
        rate, block or channel count - a Dante domain moved, a MADI box at
        double speed with half its channels; or something else entirely. */
    enum class Verdict { foreign, moved, same };

    // Callbacks only touch atomics. The message-thread watchdog supplies time;
    // silent validation blocks never advance the playback graph or show clock.
    class RecoveryGate
    {
    public:
        void started (Verdict verdict) noexcept
        {
            const auto next = ((state.load() >> 3) + 1) << 3;
            state.store (next | (verdict == Verdict::moved ? movedBit : 0)
                              | (verdict == Verdict::same ? checking : invalid));
        }
        void started (bool compatible) noexcept { started (compatible ? Verdict::same : Verdict::foreign); }
        void stopped() noexcept { started (Verdict::foreign); }
        bool callback() noexcept
        { heartbeat.fetch_add (1); return (state.load() & mask) == flowing; }
        bool paused() const noexcept { return (state.load() & mask) != flowing; }
        bool resume (bool initialOpen = false) noexcept
        {
            auto expected = state.load();
            if ((expected & mask) != (initialOpen ? checking : ready)) return false;
            return state.compare_exchange_strong (expected, (expected & ~mask) | flowing);
        }

        /*  `moved`: the same interface on another clock, and steady on it by
            the rule a return has to pass - so a device still settling after
            its clock changed is not followed half-way. Nothing is retried
            while it holds: there is nothing to put back that has not already
            been asked for (DeviceAudioDriver::serviceRecovery asks once). */
        struct Observation { bool ready = false, retry = false, moved = false; };
        Observation observe (double now)
        {
            auto observed = state.load();
            const auto serial = observed >> 3;
            const auto count = heartbeat.load();
            if (serial != seenGeneration || lastProgress < 0)
            { seenGeneration = serial; stableSince = now; lastProgress = now; seenHeartbeat = count; firstHeartbeat = count; }
            if (count != seenHeartbeat)
            { seenHeartbeat = count; lastProgress = now; }
            const auto stalled = now - lastProgress >= 500.0;
            const auto steady = count - firstHeartbeat >= 3 && now - lastProgress < 100.0
                                && now - stableSince >= 250.0;
            if (stalled)
            {
                if ((observed & mask) != invalid)
                    state.compare_exchange_strong (observed, (observed & ~mask) | checking);
                stableSince = now;
                firstHeartbeat = count;
            }
            else if ((observed & mask) == checking && steady)
                state.compare_exchange_strong (observed, (observed & ~mask) | ready);
            const auto after = state.load();
            const auto moved = (after & movedBit) != 0 && (after & mask) == invalid;
            return { (after & mask) == ready,
                     ((after & mask) == invalid && ! moved) || stalled,
                     moved && ! stalled && steady };
        }
    private:
        static constexpr std::uint64_t invalid = 0, checking = 1, ready = 2, flowing = 3, mask = 3,
                                       movedBit = 4;
        // Generation, verdict and gate state share one atomic: a stop/restart cannot be
        // overwritten by an approval for an earlier device callback generation.
        std::atomic<std::uint64_t> state { 0 }, heartbeat { 0 };
        std::uint64_t seenHeartbeat = 0, seenGeneration = 0, firstHeartbeat = 0;
        double stableSince = 0, lastProgress = -1;
    };
}
