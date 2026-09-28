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
        /*  Release: a lane given back to the call has the state it is to be
            given back counted already, and the audio thread reads this first. */
        callEnabled.store (shouldBeCalled ? 1 : 0, std::memory_order_release);
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

        //  And the fades start over: nothing of the last cue's is faded from.
        clearRequests.fetch_add (1, std::memory_order_relaxed);
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
        /*  WHAT THE NEW CHILD IS TO BE GIVEN BACK (CU): the state this lane
            held when its child went down. Not when one was loading - that may
            be what took the child down, and the cue it was for plays on the
            preset's own - and not when a newer one is still to be sent: that
            goes to the new child as it would have gone to the old. */
        const auto pending = stateInFlight
                               || settledStateSeq.load (std::memory_order_acquire)
                                    < wantedStateSeq.load (std::memory_order_acquire);
        restoreStatePath = pending ? std::string() : heldStatePath;

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

    void ProxyLane::restoreState()
    {
        /*  Unless something was asked for since - an arm on the voice while
            the plugin was down: the last word on what a lane holds is the last
            one said. The preset's own state is what a new child holds already,
            so an empty path asks for nothing. */
        const auto newer = settledStateSeq.load (std::memory_order_acquire)
                             < wantedStateSeq.load (std::memory_order_acquire);

        if (! restoreStatePath.empty() && ! newer)
            wantState (restoreStatePath);

        restoreStatePath.clear();
    }

    //==============================================================================
    void ProxyLane::setShape (int feed, int back) noexcept
    {
        shapeFeed.store (feed, std::memory_order_relaxed);
        shapeBack.store (back, std::memory_order_relaxed);
    }

    void ProxyLane::silenceFrom (float* const* channelData, int channels, int from, int numSamples) noexcept
    {
        /*  DOWN TO SILENCE OVER A MILLISECOND (CV), from where the last answer
            left each channel, then nothing: a dip and not a click, and never
            the dry block the caller's buffer still holds. Already silent, it is
            silence throughout. Every channel the plugin would have given back
            is silenced, a widening insert's added side with the rest. */
        const auto fade = std::min (numSamples - from, fadeSamples);

        for (int channel = 0; channel < channels; ++channel)
        {
            auto* out = channelData[channel] + from;
            const auto kept = channel < maxFadedChannels;
            const auto index = static_cast<std::size_t> (kept ? channel : 0);
            const auto startAt = silenced || ! kept ? 0.0f : lastSample[index];

            for (int n = 0; n < fade; ++n)
                out[n] = startAt * (1.0f - static_cast<float> (n + 1) / static_cast<float> (fade));

            std::fill (out + fade, out + (numSamples - from), 0.0f);

            if (kept)
                lastSample[index] = 0.0f;
        }

        silenced = true;
    }

    void ProxyLane::process (float* const* channelData, int numChannels, int numSamples) noexcept
    {
        if (enabled.load (std::memory_order_relaxed) == 0 || channelData == nullptr || numSamples <= 0)
            return;

        /*  THE CUE'S WIDTH HERE (setShape): what is sent, and what is taken
            back - never more than the voice or the region carries. Nought
            sent is an insert this cue passes dry, whole: a configuration, said
            on the insert, and not a failure. */
        const auto feedSaid = shapeFeed.load (std::memory_order_relaxed);
        const auto backSaid = shapeBack.load (std::memory_order_relaxed);

        if (feedSaid == 0)
            return;

        //  A NEW CUE ON THE VOICE: nothing of the last one's to fade from.
        if (const auto asked = clearRequests.load (std::memory_order_relaxed); asked != clearsSeen)
        {
            clearsSeen = asked;
            lastSample.fill (0.0f);
            silenced = false;
        }

        auto* bound = lane.load (std::memory_order_acquire);

        /*  A PLUGIN THIS MACHINE DOES NOT HAVE is never bound: the cue has it
            switched in and nothing can play it as the cue says, so the voice
            is silent (CU) - all of it, the plugin's shape being unknown. */
        if (bound == nullptr)
        {
            silenceFrom (channelData, numChannels, 0, numSamples);
            return;
        }

        const auto width = std::min (numChannels, regionChannels);
        const auto channels = feedSaid > 0 ? std::min (feedSaid, width) : width;
        const auto back = backSaid > 0 ? std::max (channels, std::min (backSaid, width)) : channels;

        if (channels <= 0)
            return;

        /*  FAILED, OR STILL TAKING A WHOLE STATE (CU): not called, and silent
            until the plugin holds the cue's state again - after a relaunch,
            the state it held when it went down. The host's switch is read
            first, with acquire: a lane given back to the call has that state
            counted already, so no block slips through on the preset's own. */
        if (callEnabled.load (std::memory_order_acquire) == 0
             || settledStateSeq.load (std::memory_order_acquire) < wantedStateSeq.load (std::memory_order_acquire))
        {
            silenceFrom (channelData, back, 0, numSamples);
            return;
        }

        const auto started = std::chrono::steady_clock::now();
        const auto limit = std::chrono::microseconds (deadlineUs.load (std::memory_order_relaxed));

        /*  A BLOCK LONGER THAN THE REGION CARRIES is sent in pieces under the
            one deadline (2026-09-26), where it used to be cut short with its
            tail left dry. The pieces follow one another, so the plugin hears
            the block in order. */
        for (int offset = 0; offset < numSamples;)
        {
            const auto samples = std::min (numSamples - offset, regionMaxSamples);

            for (int channel = 0; channel < channels; ++channel)
                std::copy_n (channelData[channel] + offset, samples, audio + channel * regionMaxSamples);

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
                /*  LATE: the rest of the block silent over the quick fade (CV),
                    never the dry block still in the caller's buffer - on a
                    wet-only reverb that is the original signal, loud and out of
                    place. Counted for the host, which fails a plugin eight
                    blocks late in a row. */
                missCount.fetch_add (1, std::memory_order_relaxed);
                consecutive.fetch_add (1, std::memory_order_relaxed);

                silenceFrom (channelData, back, offset, numSamples);
                return;
            }

            /*  ANSWERED, and taken in place - the first answer after silence
                rising over the same quick fade, so a return is no click either. */
            const auto rise = silenced ? std::min (samples, fadeSamples) : 0;

            for (int channel = 0; channel < back; ++channel)
            {
                auto* out = channelData[channel] + offset;
                std::copy_n (audio + channel * regionMaxSamples, samples, out);

                for (int n = 0; n < rise; ++n)
                    out[n] *= static_cast<float> (n + 1) / static_cast<float> (rise);

                if (channel < maxFadedChannels)
                    lastSample[static_cast<std::size_t> (channel)] = out[samples - 1];
            }

            silenced = false;
            offset += samples;
        }

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
