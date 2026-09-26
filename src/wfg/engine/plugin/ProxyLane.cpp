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

#include <wfg/engine/plugin/ProxyLane.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace wfg::plugin
{
    ProxyLane::ProxyLane()
    {
        for (auto& value : shadow)
            value.store (region::useBaseline, std::memory_order_relaxed);
    }

    //==============================================================================
    void ProxyLane::bind (region::Header* headerToUse, region::Lane* laneToUse, float* audioToUse,
                          int channels, int maxSamples) noexcept
    {
        if (laneToUse == nullptr || audioToUse == nullptr || channels <= 0 || maxSamples <= 0)
            return;

        header = headerToUse;
        audio = audioToUse;
        regionChannels = channels;
        regionMaxSamples = maxSamples;

        /*  The shadow first, then the pointer with release, then the shadow
            once more: a store that landed between the first copy and the
            publish went only to the shadow, and the second copy carries it
            across; one that lands after the publish is written to the region
            by its own thread as well. Either way the child sees it. */
        for (int i = 0; i < region::maxParams; ++i)
            laneToUse->params[i].store (shadow[i].load (std::memory_order_relaxed), std::memory_order_relaxed);

        lane.store (laneToUse, std::memory_order_release);

        for (int i = 0; i < region::maxParams; ++i)
            laneToUse->params[i].store (shadow[i].load (std::memory_order_relaxed), std::memory_order_relaxed);

        laneToUse->paramRevision.fetch_add (1, std::memory_order_release);
    }

    void ProxyLane::unbind() noexcept
    {
        lane.store (nullptr, std::memory_order_release);
        header = nullptr;
        audio = nullptr;
        regionChannels = 0;
        regionMaxSamples = 0;
    }

    bool ProxyLane::isBound() const noexcept
    {
        return lane.load (std::memory_order_acquire) != nullptr;
    }

    void ProxyLane::setDeadlineMicroseconds (std::int64_t microseconds) noexcept
    {
        deadlineUs.store (std::max<std::int64_t> (1, microseconds), std::memory_order_relaxed);
    }

    std::int64_t ProxyLane::deadlineMicroseconds() const noexcept
    {
        return deadlineUs.load (std::memory_order_relaxed);
    }

    //==============================================================================
    void ProxyLane::setEnabled (bool shouldBeEnabled) noexcept
    {
        enabled.store (shouldBeEnabled ? 1 : 0, std::memory_order_relaxed);
    }

    bool ProxyLane::isEnabled() const noexcept
    {
        return enabled.load (std::memory_order_relaxed) != 0;
    }

    void ProxyLane::setCallEnabled (bool shouldBeCalled) noexcept
    {
        callEnabled.store (shouldBeCalled ? 1 : 0, std::memory_order_relaxed);
    }

    bool ProxyLane::isCallEnabled() const noexcept
    {
        return callEnabled.load (std::memory_order_relaxed) != 0;
    }

    void ProxyLane::setParameter (int index, float normalised) noexcept
    {
        if (index < 0 || index >= region::maxParams)
            return;

        const auto value = normalised < 0.0f ? region::useBaseline : std::min (1.0f, normalised);
        shadow[index].store (value, std::memory_order_relaxed);

        if (auto* bound = lane.load (std::memory_order_acquire))
        {
            bound->params[index].store (value, std::memory_order_relaxed);
            bound->paramRevision.fetch_add (1, std::memory_order_release);
        }
    }

    float ProxyLane::parameter (int index) const noexcept
    {
        if (index < 0 || index >= region::maxParams)
            return region::useBaseline;

        return shadow[index].load (std::memory_order_relaxed);
    }

    void ProxyLane::setValues (const std::vector<std::pair<int, float>>& values) noexcept
    {
        for (auto& value : shadow)
            value.store (region::useBaseline, std::memory_order_relaxed);

        for (const auto& [index, normalised] : values)
            if (index >= 0 && index < region::maxParams)
                shadow[index].store (normalised < 0.0f ? region::useBaseline : std::min (1.0f, normalised),
                                     std::memory_order_relaxed);

        if (auto* bound = lane.load (std::memory_order_acquire))
        {
            for (int i = 0; i < region::maxParams; ++i)
                bound->params[i].store (shadow[i].load (std::memory_order_relaxed), std::memory_order_relaxed);

            bound->paramRevision.fetch_add (1, std::memory_order_release);
        }
    }

    void ProxyLane::requestReset() noexcept
    {
        if (auto* bound = lane.load (std::memory_order_acquire))
            bound->resetSeq.fetch_add (1, std::memory_order_release);
    }

    //==============================================================================
    void ProxyLane::wantState (const std::string& path)
    {
        wantedStatePath = path;
        pathStateSeq = wantedStateSeq.fetch_add (1, std::memory_order_acq_rel) + 1;
    }

    void ProxyLane::expectState() noexcept
    {
        wantedStateSeq.fetch_add (1, std::memory_order_acq_rel);
    }

    bool ProxyLane::stateSettled() const noexcept
    {
        if (enabled.load (std::memory_order_relaxed) == 0
             || callEnabled.load (std::memory_order_relaxed) == 0
             || lane.load (std::memory_order_acquire) == nullptr)
            return true;

        return settledStateSeq.load (std::memory_order_acquire) >= wantedStateSeq.load (std::memory_order_acquire);
    }

    ProxyLane::StateNews ProxyLane::serviceState (std::uint32_t nowMs)
    {
        StateNews news;
        auto* bound = lane.load (std::memory_order_acquire);

        if (bound == nullptr)
            return news;

        if (stateInFlight)
        {
            if (bound->stateDoneSeq.load (std::memory_order_acquire) < flightStateSeq)
            {
                news.late = nowMs - flightSentAt > stateLoadLimitMs;
                return news;
            }

            /*  ANSWERED. A load that failed left the instance on the preset's
                own state, so that is what the lane holds - and it is not
                tried again until the next arm asks. */
            news.arrived = true;
            news.failed = bound->stateFailed.load (std::memory_order_relaxed) != 0;
            news.loadMs = static_cast<double> (bound->stateLoadMicros.load (std::memory_order_relaxed)) / 1000.0;
            news.latencySamples = static_cast<int> (bound->latencySamples.load (std::memory_order_relaxed));

            if (news.failed)
            {
                bound->stateProblem[region::problemChars - 1] = 0;
                news.problem = bound->stateProblem;
            }

            heldStatePath = news.failed ? std::string() : flightStatePath;
            stateInFlight = false;
            settledStateSeq.store (flightStateSeq, std::memory_order_release);
            return news;
        }

        const auto wanted = wantedStateSeq.load (std::memory_order_acquire);

        if (wanted <= settledStateSeq.load (std::memory_order_relaxed))
            return news;

        /*  A STATE ANNOUNCED AND NOT YET NAMED: the tick thread counted it,
            and its path is still on the way from the message thread's queue.
            Neither settled nor sent until it arrives, so no launch slips
            through on the path before it. */
        if (wanted != pathStateSeq)
            return news;

        //  HELD ALREADY: nothing to load, and the arm need not wait.
        if (wantedStatePath == heldStatePath)
        {
            settledStateSeq.store (wanted, std::memory_order_release);
            return news;
        }

        std::memset (bound->statePath, 0, sizeof (bound->statePath));
        std::snprintf (bound->statePath, sizeof (bound->statePath), "%s", wantedStatePath.c_str());

        flightStatePath = wantedStatePath;
        flightStateSeq = wanted;
        flightSentAt = nowMs;
        stateInFlight = true;
        bound->stateRequestSeq.store (wanted, std::memory_order_release);
        return news;
    }

    void ProxyLane::forgetState()
    {
        if (stateInFlight)
        {
            stateInFlight = false;
            settledStateSeq.store (flightStateSeq, std::memory_order_release);
        }

        heldStatePath.clear();

        if (auto* bound = lane.load (std::memory_order_acquire))
            bound->stateDoneSeq.store (bound->stateRequestSeq.load (std::memory_order_acquire),
                                       std::memory_order_release);
    }

    //==============================================================================
    void ProxyLane::process (float* const* channelData, int numChannels, int numSamples) noexcept
    {
        if (enabled.load (std::memory_order_relaxed) == 0
             || callEnabled.load (std::memory_order_relaxed) == 0)
            return;

        auto* bound = lane.load (std::memory_order_acquire);

        if (bound == nullptr || channelData == nullptr)
            return;

        const auto channels = std::min (numChannels, regionChannels);
        const auto samples = std::min (numSamples, regionMaxSamples);

        if (channels <= 0 || samples <= 0)
            return;

        const auto started = std::chrono::steady_clock::now();
        const auto limit = std::chrono::microseconds (deadlineUs.load (std::memory_order_relaxed));

        for (int channel = 0; channel < channels; ++channel)
            std::copy_n (channelData[channel], samples, audio + channel * regionMaxSamples);

        bound->numChannels.store (static_cast<std::uint32_t> (channels), std::memory_order_relaxed);
        bound->numSamples.store (static_cast<std::uint32_t> (samples), std::memory_order_relaxed);

        const auto seq = bound->requestSeq.load (std::memory_order_relaxed) + 1;
        bound->requestSeq.store (seq, std::memory_order_release);
        blockCount.fetch_add (1, std::memory_order_relaxed);

        /*  THE BOUNDED SPIN (§17.6). No condition variable, no semaphore, no
            sleep: those enter the kernel and PRD §4.2 forbids that here. */
        auto late = false;

        for (int turns = 0; bound->responseSeq.load (std::memory_order_acquire) < seq;)
        {
            if (++turns >= 64)
            {
                turns = 0;

                if (std::chrono::steady_clock::now() - started > limit)
                {
                    late = true;
                    break;
                }
            }
        }

        if (late)
        {
            /*  The dry block is still in the caller's buffer: degradation,
                not a dropout (§3.18). Counted for the host. */
            missCount.fetch_add (1, std::memory_order_relaxed);
            consecutive.fetch_add (1, std::memory_order_relaxed);
            return;
        }

        for (int channel = 0; channel < channels; ++channel)
            std::copy_n (audio + channel * regionMaxSamples, samples, channelData[channel]);

        answeredCount.fetch_add (1, std::memory_order_relaxed);
        consecutive.store (0, std::memory_order_relaxed);
    }

    //==============================================================================
    std::uint64_t ProxyLane::blocks() const noexcept             { return blockCount.load (std::memory_order_relaxed); }
    std::uint64_t ProxyLane::answered() const noexcept           { return answeredCount.load (std::memory_order_relaxed); }
    std::uint32_t ProxyLane::misses() const noexcept             { return missCount.load (std::memory_order_relaxed); }
    std::uint32_t ProxyLane::consecutiveMisses() const noexcept  { return consecutive.load (std::memory_order_relaxed); }

    void ProxyLane::clearMisses() noexcept
    {
        consecutive.store (0, std::memory_order_relaxed);
    }
}
