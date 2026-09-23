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

#include <wfg/engine/audio/EqPlugin.h>

#include <wfg/engine/rt/RtCheck.h>

#include <algorithm>

namespace wfg::audio
{
    namespace te = tracktion::engine;

    namespace
    {
        /*  Our own property name on the plugin's state tree, prefixed because
            it sits in the same tree as Tracktion's own. */
        const juce::Identifier channelsProperty { "wfgChannels" };
    }

    //==============================================================================
    const char* EqPlugin::xmlTypeName = "godotCueEq";

    juce::ValueTree EqPlugin::create (int numChannels)
    {
        return te::createValueTree (te::IDs::PLUGIN,
                                    te::IDs::type, xmlTypeName,
                                    channelsProperty, std::max (1, numChannels));
    }

    //==============================================================================
    EqPlugin::EqPlugin (te::PluginCreationInfo info)
        : te::Plugin (info)
    {
        const auto stored = static_cast<int> (state.getProperty (channelsProperty, 2));
        channels = std::clamp (stored > 0 ? stored : 2, 1, CueEq::maxChannels);
    }

    EqPlugin::~EqPlugin()
    {
        /*  Required of every Selectable, in the innermost subclass. */
        notifyListenersOfDeletion();
    }

    //==============================================================================
    juce::String EqPlugin::getName() const                { return "Go.dot Cue EQ"; }
    juce::String EqPlugin::getPluginType()                { return xmlTypeName; }
    juce::String EqPlugin::getSelectableDescription()     { return getName(); }

    te::Plugin::BusLayout EqPlugin::getBusses() const
    {
        /*  The input side is `none()` - no channel-count requirement, whatever
            the track carries passes through - and it must be PRESENT, or the
            track reports that its plugin takes no audio. The output answers
            the track's width, which the override below is what actually sizes
            the buffer to. */
        return BusLayout::singleInOut (te::ChannelConfiguration::none(),
                                       te::ChannelConfiguration::discreteChannels (channels));
    }

    int EqPlugin::getNumOutputChannelsGivenInputs (int numInputs)
    {
        /*  The buffer stays as wide as the track: the EQ neither widens nor
            narrows, and what it is handed is what it hands on. */
        return std::max (channels, numInputs);
    }

    //==============================================================================
    void EqPlugin::initialise (const te::PluginInitialisationInfo& info)
    {
        /*  Message thread, and the only place this object allocates. */
        pointers.assign (static_cast<std::size_t> (channels), nullptr);
        cueEq.prepare (channels, info.sampleRate, info.blockSizeSamples);
    }

    void EqPlugin::deinitialise()
    {
    }

    //==============================================================================
    void EqPlugin::applyToBuffer (const te::PluginRenderContext& context)
    {
        /*  Go.dot's code, running inside Tracktion's block (PRD §4.2): counted
            as ours, so an allocation here is a defect and not a dependency's. */
        const rt::ScopedRealtimeCheck goDotsOwnPlugin { rt::Region::ours };

        if (context.destBuffer == nullptr || context.bufferNumSamples <= 0)
            return;

        /*  A register write, not a syscall: a biquad's delays decay towards
            denormals at the end of every sound, and flushing them costs
            nothing where the alternative is a performance cliff. */
        const juce::ScopedNoDenormals noDenormals;

        auto& destination = *context.destBuffer;
        const auto usable = std::min ({ channels, destination.getNumChannels(),
                                        static_cast<int> (pointers.size()) });

        if (usable <= 0)
            return;

        for (int channel = 0; channel < usable; ++channel)
            pointers[static_cast<std::size_t> (channel)]
                = destination.getWritePointer (channel, context.bufferStartSample);

        cueEq.process (pointers.data(), usable, context.bufferNumSamples);
    }
}
