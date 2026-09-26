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
    private:
        void timerCallback() override
        {
            std::deque<std::pair<AudioSettings, bool>> work;
            auto rebuildNow = false;
            {
                const std::lock_guard<std::mutex> lock (mutex);
                work.swap (queued);
                rebuildNow = std::exchange (rebuildAsked, false);
            }
            for (const auto& item : work) perform (item.first, item.second);
            if (rebuildNow && rebuild) rebuild();
            if (maintenance) maintenance();
        }
        SettingsRequest perform;
        std::mutex mutex;
        std::deque<std::pair<AudioSettings, bool>> queued;
        bool rebuildAsked = false;
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
    private:
        const SampleClock* source;
        std::int64_t offset = 0;
    };
}
