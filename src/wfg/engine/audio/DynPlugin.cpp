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

#include <wfg/engine/audio/DynPlugin.h>

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
    const char* DynPlugin::xmlTypeName = "godotCueDyn";

    juce::ValueTree DynPlugin::create (int numChannels)
    {
        return te::createValueTree (te::IDs::PLUGIN,
                                    te::IDs::type, xmlTypeName,
                                    channelsProperty, std::max (1, numChannels));
    }

    DynPlugin::DynPlugin (te::PluginCreationInfo info)
        : te::Plugin (info)
    {
        const auto stored = static_cast<int> (state.getProperty (channelsProperty, 2));
        channels = std::clamp (stored > 0 ? stored : 2, 1, CueDyn::maxChannels);
    }

    DynPlugin::~DynPlugin()
    {
        notifyListenersOfDeletion();
    }

    juce::String DynPlugin::getName() const             { return "Go.dot Cue Dynamics"; }
    juce::String DynPlugin::getPluginType()             { return xmlTypeName; }
    juce::String DynPlugin::getSelectableDescription()  { return getName(); }

    te::Plugin::BusLayout DynPlugin::getBusses() const
    {
        //  EqPlugin's reasons: any input passes, the output as wide as the track.
        return BusLayout::singleInOut (te::ChannelConfiguration::none(),
                                       te::ChannelConfiguration::discreteChannels (channels));
    }

    int DynPlugin::getNumOutputChannelsGivenInputs (int numInputs)
    {
        return std::max (channels, numInputs);
    }

    //==============================================================================
    void DynPlugin::initialise (const te::PluginInitialisationInfo& info)
    {
        //  Message thread, and the only place this object allocates.
        pointers.assign (static_cast<std::size_t> (channels), nullptr);
        cueDyn.prepare (channels, info.sampleRate, info.blockSizeSamples);
        keyReader.prepare (info.blockSizeSamples);
    }

    void DynPlugin::deinitialise()
    {
    }

    //==============================================================================
    void DynPlugin::applyToBuffer (const te::PluginRenderContext& context)
    {
        const rt::ScopedRealtimeCheck goDotsOwnPlugin { rt::Region::ours };

        if (context.destBuffer == nullptr || context.bufferNumSamples <= 0)
            return;

        const juce::ScopedNoDenormals noDenormals;

        auto& destination = *context.destBuffer;
        const auto usable = std::min ({ channels, destination.getNumChannels(),
                                        static_cast<int> (pointers.size()) });

        if (usable <= 0)
            return;

        for (int channel = 0; channel < usable; ++channel)
            pointers[static_cast<std::size_t> (channel)]
                = destination.getWritePointer (channel, context.bufferStartSample);

        /*  THE KEY, only for a stage that is on and has one picked: off costs
            the read of two atomics. */
        const float* key[2] = { nullptr, nullptr };
        const auto keyed = cueDyn.isOn() && keyReader.hasKey();
        const auto keyChannels = keyed ? keyReader.read (context.bufferStartSample, context.bufferNumSamples, key,
                                                         pointers.data(), usable)
                                       : 0;

        cueDyn.process (pointers.data(), usable, context.bufferNumSamples, keyed, key, keyChannels);
    }
}
