/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <wfg/engine/audio/AudioSettings.h>
#include <wfg/engine/clock/SampleClock.h>
#include <juce_events/juce_events.h>
#include <deque>
#include <mutex>
#include <utility>

namespace wfg::audio
{
    // The tick thread posts immutable requests; device APIs run on the message
    // thread. Destruction is synchronous, so no queued lambda outlives the show.
    class SettingsPump final : private juce::Timer
    {
    public:
        explicit SettingsPump (SettingsRequest work) : perform (std::move (work)) { startTimer (40); }
        ~SettingsPump() override { stopTimer(); }
        std::function<void()> maintenance;

        /** `plugin.load`'s work (2026-09-26): the graph built again on what plays now. */
        std::function<void()> rebuild;

        /** `audio.clockMoved`'s work (2026-09-28): the show brought up on the interface's new clock. */
        std::function<void()> follow;

        void post (const AudioSettings& settings, bool defaultsOnly)
        {
            const std::lock_guard<std::mutex> lock (mutex);
            queued.push_back ({ settings, defaultsOnly });
        }

        void postRebuild()
        {
            const std::lock_guard<std::mutex> lock (mutex);
            rebuildAsked = true;
        }

        void postFollow()
        {
            const std::lock_guard<std::mutex> lock (mutex);
            followAsked = true;
        }
    private:
        void timerCallback() override
        {
            std::deque<std::pair<AudioSettings, bool>> work;
            auto rebuildNow = false, followNow = false;
            {
                const std::lock_guard<std::mutex> lock (mutex);
                work.swap (queued);
                rebuildNow = std::exchange (rebuildAsked, false);
                followNow = std::exchange (followAsked, false);
            }
            for (const auto& item : work) perform (item.first, item.second);
            if (rebuildNow && rebuild) rebuild();
            if (followNow && follow) follow();
            if (maintenance) maintenance();
        }
        SettingsRequest perform;
        std::mutex mutex;
        std::deque<std::pair<AudioSettings, bool>> queued;
        bool rebuildAsked = false, followAsked = false;
    };

    // Repointed only while the tick thread is joined. Device sample counters
    // may restart; the session's sample coordinate and tick indices do not.
    class SessionAudioClock final : public SampleClock
    {
    public:
        explicit SessionAudioClock (const SampleClock& initial) : source (&initial) {}
        std::int64_t samplesElapsed() const noexcept override { return offset + source->samplesElapsed(); }
        void use (const SampleClock& next, std::int64_t boundary)
        { source = &next; offset = boundary - next.samplesElapsed(); }

        // The block clock this one counts from, and what it adds to that clock's
        // count (namespace draft §58, RD.9): the session's sample less the
        // device's. Read on the tick thread, which `use` only runs beside joined.
        const SampleClock& counting() const noexcept { return *source; }
        std::int64_t sessionOffset() const noexcept { return offset; }

        // The device's block stamp moved onto the session's count (RD.9).
        bool lastBlock (BlockStamp::Reading& out) const noexcept override
        {
            if (! source->lastBlock (out))
                return false;

            out.sample += offset;
            return true;
        }

        // Held where the count it follows is held (PRD §3.34).
        bool isHeld() const noexcept override { return source->isHeld(); }
    private:
        const SampleClock* source;
        std::int64_t offset = 0;
    };
}
