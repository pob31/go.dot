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

#include <wfg/engine/audio/ProxyPlugin.h>
#include <wfg/engine/rt/RtCheck.h>

#include <algorithm>

namespace wfg::audio
{
    namespace te = tracktion::engine;

    namespace
    {
        /*  Our own property names on the plugin's state tree, prefixed because
            they sit in the same tree as Tracktion's own. */
        const juce::Identifier channelsProperty { "wfgChannels" };
        const juce::Identifier slotProperty { "wfgSlot" };

        /** The widest voice this stage carries; wider input passes beyond it. */
        constexpr int maxChannels = 64;
    }

    //==============================================================================
    const char* ProxyPlugin::xmlTypeName = "godotProxy";

    juce::ValueTree ProxyPlugin::create (int numChannels, int slot)
    {
        return te::createValueTree (te::IDs::PLUGIN,
                                    te::IDs::type, xmlTypeName,
                                    channelsProperty, std::max (1, numChannels),
                                    slotProperty, std::max (0, slot));
    }

    //==============================================================================
    ProxyPlugin::ProxyPlugin (te::PluginCreationInfo info)
        : te::Plugin (info)
    {
        const auto stored = static_cast<int> (state.getProperty (channelsProperty, 2));
        channels = std::clamp (stored > 0 ? stored : 2, 1, maxChannels);
        setSlot = std::max (0, static_cast<int> (state.getProperty (slotProperty, 0)));
    }

    ProxyPlugin::~ProxyPlugin()
    {
        /*  Required of every Selectable, in the innermost subclass. */
        notifyListenersOfDeletion();
    }

    //==============================================================================
    juce::String ProxyPlugin::getName() const                { return "Go.dot Plugin Proxy"; }
    juce::String ProxyPlugin::getPluginType()                { return xmlTypeName; }
    juce::String ProxyPlugin::getSelectableDescription()     { return getName(); }

    te::Plugin::BusLayout ProxyPlugin::getBusses() const
    {
        /*  As EqPlugin: no requirement on the input side, so whatever the
            track carries passes through, and the output answers the track's
            width. */
        return BusLayout::singleInOut (te::ChannelConfiguration::none(),
                                       te::ChannelConfiguration::discreteChannels (channels));
    }

    int ProxyPlugin::getNumOutputChannelsGivenInputs (int numInputs)
    {
        return std::max (channels, numInputs);
    }

    //==============================================================================
    void ProxyPlugin::initialise (const te::PluginInitialisationInfo&)
    {
        /*  Message thread, and the only place this object allocates. */
        pointers.assign (static_cast<std::size_t> (channels), nullptr);
    }

    void ProxyPlugin::deinitialise()
    {
    }

    //==============================================================================
    void ProxyPlugin::applyToBuffer (const te::PluginRenderContext& context)
    {
        /*  Go.dot's code, running inside Tracktion's block (PRD §4.2): counted
            as ours, so an allocation here is a defect and not a dependency's. */
        const rt::ScopedRealtimeCheck goDotsOwnPlugin { rt::Region::ours };

        if (context.destBuffer == nullptr || context.bufferNumSamples <= 0)
            return;

        /*  The cheap test first, before a single pointer is gathered: a lane
            the cue has switched out costs the voice nothing. One switched in
            goes to the lane whatever became of its plugin - unbound, failed or
            taking a state, it is the lane that silences the block, never dry
            (the author's decision of 2026-09-26, CU), which a return here
            would have left dry. */
        if (! proxyLane.isEnabled())
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

        proxyLane.process (pointers.data(), usable, context.bufferNumSamples);
    }
}
