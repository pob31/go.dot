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

#include <wfg/engine/audio/LiveInputPlugin.h>

#include <wfg/engine/rt/RtCheck.h>

#include <algorithm>
#include <cmath>

namespace wfg::audio
{
    namespace te = tracktion::engine;

    namespace
    {
        const juce::Identifier channelsProperty { "wfgChannels" };

        /*  At most two channels are taken from the tap: a mono input or a
            stereo one, the rack's width classes (PRD §3.18). */
        constexpr int maxTaken = 2;
    }

    //==============================================================================
    const char* LiveInputPlugin::xmlTypeName = "godotLiveInput";

    juce::ValueTree LiveInputPlugin::create (int numChannels)
    {
        return te::createValueTree (te::IDs::PLUGIN,
                                    te::IDs::type, xmlTypeName,
                                    channelsProperty, std::max (1, numChannels));
    }

    //==============================================================================
    LiveInputPlugin::LiveInputPlugin (te::PluginCreationInfo info)
        : te::Plugin (info)
    {
        const auto stored = static_cast<int> (state.getProperty (channelsProperty, 2));
        channels = std::clamp (stored > 0 ? stored : 2, 1, 64);
    }

    LiveInputPlugin::~LiveInputPlugin()
    {
        notifyListenersOfDeletion();
    }

    juce::String LiveInputPlugin::getName() const             { return "Go.dot Live Input"; }
    juce::String LiveInputPlugin::getPluginType()             { return xmlTypeName; }
    juce::String LiveInputPlugin::getSelectableDescription()  { return getName(); }

    te::Plugin::BusLayout LiveInputPlugin::getBusses() const
    {
        return BusLayout::singleInOut (te::ChannelConfiguration::none(),
                                       te::ChannelConfiguration::discreteChannels (channels));
    }

    int LiveInputPlugin::getNumOutputChannelsGivenInputs (int numInputs)
    {
        return std::max (channels, numInputs);
    }

    //==============================================================================
    void LiveInputPlugin::setSource (int firstInput, int width) noexcept
    {
        sourceFirst.store (firstInput, std::memory_order_relaxed);
        sourceWidth.store (std::clamp (width, 1, maxTaken), std::memory_order_relaxed);
    }

    void LiveInputPlugin::openAt (std::int64_t sample) noexcept
    {
        openSample.store (sample, std::memory_order_relaxed);
        shutFast.store (false, std::memory_order_relaxed);
        wantOpen.store (true, std::memory_order_relaxed);
    }

    void LiveInputPlugin::shut (bool fast) noexcept
    {
        shutFast.store (fast, std::memory_order_relaxed);
        wantOpen.store (false, std::memory_order_relaxed);
    }

    bool LiveInputPlugin::isPassing() const noexcept
    {
        return wantOpen.load (std::memory_order_relaxed) || gain.load (std::memory_order_relaxed) > 0.0f;
    }

    //==============================================================================
    void LiveInputPlugin::initialise (const te::PluginInitialisationInfo& info)
    {
        /*  Message thread. A rebuild of the graph that keeps this plugin calls
            `initialiseWithoutStopping` instead, so a gate open across a media
            arm's rebuild stays open; this runs at the first build and at a
            change of rate or block. */
        const auto rampSamples = std::max (1.0, std::round (rampSeconds * info.sampleRate));
        rampStep = static_cast<float> (1.0 / rampSamples);
        gain.store (0.0f, std::memory_order_relaxed);
        lastBlockEnd = -1;
    }

    void LiveInputPlugin::deinitialise()
    {
    }

    //==============================================================================
    void LiveInputPlugin::applyToBuffer (const te::PluginRenderContext& context)
    {
        const rt::ScopedRealtimeCheck goDotsOwnPlugin { rt::Region::ours };

        if (context.destBuffer == nullptr || context.bufferNumSamples <= 0)
            return;

        auto& destination = *context.destBuffer;
        const auto frames = context.bufferNumSamples;
        const auto start = context.bufferStartSample;
        const auto width = std::min (channels, destination.getNumChannels());
        const auto* tap = boundTap;

        /*  WHERE THIS STRETCH OF THE BLOCK SITS in Go.dot's count, which is
            what an open is placed against. */
        const auto firstSample = (tap != nullptr ? tap->blockStart : 0) + start;

        /*  A GAP IN THE BLOCKS - the last one did not end where this one
            starts: an open gate ramps in again rather than stepping onto
            whatever the input is doing now. */
        auto g = gain.load (std::memory_order_relaxed);

        if (lastBlockEnd >= 0 && firstSample != lastBlockEnd)
            g = 0.0f;

        lastBlockEnd = firstSample + frames;

        const auto first = sourceFirst.load (std::memory_order_relaxed);
        const auto take = std::clamp (sourceWidth.load (std::memory_order_relaxed), 1, maxTaken);
        const auto open = wantOpen.load (std::memory_order_relaxed);
        const auto at = openSample.load (std::memory_order_relaxed);
        const auto step = ! open && shutFast.load (std::memory_order_relaxed) ? rampStep * 5.0f : rampStep;

        const float* source[maxTaken] { nullptr, nullptr };

        if (tap != nullptr && tap->buffer != nullptr && first >= 0
              && start + frames <= tap->buffer->getNumSamples())
            for (int channel = 0; channel < take; ++channel)
                if (const auto logical = first + channel; logical < tap->channels)
                    source[channel] = tap->buffer->getReadPointer (logical, start);

        /*  THE STEADY CASES FIRST, which are nearly every block: open and at
            full gain for the whole of it, a straight copy; shut and silent, a
            clear. Only a block the gate moves in is gained sample by sample. */
        const auto openAll = open && at < firstSample && g >= 1.0f;
        const auto shutAll = (! open || at >= firstSample + frames) && g <= 0.0f;

        if (openAll || shutAll)
        {
            for (int channel = 0; channel < width; ++channel)
            {
                const auto* from = openAll && channel < maxTaken ? source[channel] : nullptr;

                if (from != nullptr)
                    destination.copyFrom (channel, start, from, frames);
                else
                    destination.clear (channel, start, frames);
            }

            gain.store (openAll ? 1.0f : 0.0f, std::memory_order_relaxed);
            return;
        }

        for (int n = 0; n < frames; ++n)
        {
            const auto target = open && (at < 0 || firstSample + n >= at) ? 1.0f : 0.0f;
            g = target > g ? std::min (target, g + step) : std::max (target, g - step);

            for (int channel = 0; channel < width; ++channel)
            {
                const auto* from = channel < maxTaken ? source[channel] : nullptr;
                destination.setSample (channel, start + n, from != nullptr ? from[n] * g : 0.0f);
            }
        }

        gain.store (g, std::memory_order_relaxed);
    }
}
