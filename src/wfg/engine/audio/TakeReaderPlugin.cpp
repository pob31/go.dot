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

#include <wfg/engine/audio/TakeReaderPlugin.h>

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
    const char* TakeReaderPlugin::xmlTypeName = "godotTakeReader";

    juce::ValueTree TakeReaderPlugin::create (int numChannels)
    {
        return te::createValueTree (te::IDs::PLUGIN,
                                    te::IDs::type, xmlTypeName,
                                    channelsProperty, std::max (1, numChannels));
    }

    //==============================================================================
    TakeReaderPlugin::TakeReaderPlugin (te::PluginCreationInfo info)
        : te::Plugin (info)
    {
        const auto stored = static_cast<int> (state.getProperty (channelsProperty, 2));
        channels = std::clamp (stored > 0 ? stored : 2, 1, 64);
    }

    TakeReaderPlugin::~TakeReaderPlugin()
    {
        notifyListenersOfDeletion();
    }

    juce::String TakeReaderPlugin::getName() const             { return "Go.dot Take Reader"; }
    juce::String TakeReaderPlugin::getPluginType()             { return xmlTypeName; }
    juce::String TakeReaderPlugin::getSelectableDescription()  { return getName(); }

    te::Plugin::BusLayout TakeReaderPlugin::getBusses() const
    {
        return BusLayout::singleInOut (te::ChannelConfiguration::discreteChannels (channels),
                                       te::ChannelConfiguration::discreteChannels (channels));
    }

    int TakeReaderPlugin::getNumOutputChannelsGivenInputs (int numInputs)
    {
        return std::max (channels, numInputs);
    }

    void TakeReaderPlugin::bind (TakeVoice* voice, const LiveInputTap* tap) noexcept
    {
        boundVoice = voice;
        boundTap = tap;
    }

    //==============================================================================
    void TakeReaderPlugin::initialise (const te::PluginInitialisationInfo&)
    {
        //  Nothing to size: the take is the host's, and the voice's scratch is fixed.
    }

    void TakeReaderPlugin::deinitialise()
    {
    }

    //==============================================================================
    void TakeReaderPlugin::applyToBuffer (const te::PluginRenderContext& context)
    {
        const rt::ScopedRealtimeCheck goDotsOwnPlugin { rt::Region::ours };

        if (context.destBuffer == nullptr || context.bufferNumSamples <= 0 || boundVoice == nullptr)
            return;

        auto& destination = *context.destBuffer;
        const auto width = std::min (channels, destination.getNumChannels());

        if (width <= 0)
            return;

        /*  WHERE THIS STRETCH OF THE BLOCK SITS in Go.dot's count - the live
            input stage's reckoning - which every launch and stop is placed against. */
        const auto firstSample = (boundTap != nullptr ? boundTap->blockStart : 0) + context.bufferStartSample;

        boundVoice->process (destination.getWritePointer (0, context.bufferStartSample),
                             width > 1 ? destination.getWritePointer (1, context.bufferStartSample) : nullptr,
                             context.bufferNumSamples, firstSample);
    }
}
