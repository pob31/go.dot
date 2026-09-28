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

#include <wfg/engine/audio/LooperPlugin.h>

#include <wfg/engine/rt/RtCheck.h>

#include <algorithm>

namespace wfg::audio
{
    namespace te = tracktion::engine;

    namespace
    {
        const juce::Identifier channelsProperty { "wfgChannels" };
    }

    //==============================================================================
    const char* LooperPlugin::xmlTypeName = "godotLooper";

    juce::ValueTree LooperPlugin::create (int numChannels)
    {
        return te::createValueTree (te::IDs::PLUGIN,
                                    te::IDs::type, xmlTypeName,
                                    channelsProperty, std::max (1, numChannels));
    }

    //==============================================================================
    LooperPlugin::LooperPlugin (te::PluginCreationInfo info)
        : te::Plugin (info)
    {
        const auto stored = static_cast<int> (state.getProperty (channelsProperty, 2));
        channels = std::clamp (stored > 0 ? stored : 2, 1, 64);
    }

    LooperPlugin::~LooperPlugin()
    {
        notifyListenersOfDeletion();
    }

    juce::String LooperPlugin::getName() const             { return "Go.dot Recorder"; }
    juce::String LooperPlugin::getPluginType()             { return xmlTypeName; }
    juce::String LooperPlugin::getSelectableDescription()  { return getName(); }

    te::Plugin::BusLayout LooperPlugin::getBusses() const
    {
        return BusLayout::singleInOut (te::ChannelConfiguration::discreteChannels (channels),
                                       te::ChannelConfiguration::discreteChannels (channels));
    }

    int LooperPlugin::getNumOutputChannelsGivenInputs (int numInputs)
    {
        return std::max (channels, numInputs);
    }

    void LooperPlugin::bind (Looper* take, const LiveInputTap* tap) noexcept
    {
        boundTake = take;
        boundTap = tap;
    }

    //==============================================================================
    void LooperPlugin::initialise (const te::PluginInitialisationInfo&)
    {
        /*  Nothing to size: the take's memory is the host's, set aside when the
            show opened, and a graph rebuilt around this stage finds it as it
            was. */
    }

    void LooperPlugin::deinitialise()
    {
    }

    //==============================================================================
    void LooperPlugin::applyToBuffer (const te::PluginRenderContext& context)
    {
        const rt::ScopedRealtimeCheck goDotsOwnPlugin { rt::Region::ours };

        if (context.destBuffer == nullptr || context.bufferNumSamples <= 0 || boundTake == nullptr)
            return;

        auto& destination = *context.destBuffer;
        const auto width = std::min ({ channels, destination.getNumChannels(), Looper::maxChannels });

        for (int channel = 0; channel < width; ++channel)
            pointers[static_cast<std::size_t> (channel)] = destination.getWritePointer (channel, context.bufferStartSample);

        /*  WHERE THIS STRETCH OF THE BLOCK SITS in Go.dot's count - the live
            input stage's reckoning - which is what every press is placed against. */
        const auto firstSample = (boundTap != nullptr ? boundTap->blockStart : 0) + context.bufferStartSample;

        boundTake->process (pointers.data(), width, context.bufferNumSamples, firstSample);
    }
}
