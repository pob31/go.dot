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

#pragma once

#include <wfg/engine/plugin/ProxyLane.h>

/*  AN audio/-INTERNAL HEADER, for CueOutputPlugin.h's reason: it names
    Tracktion types, because the thing it declares IS a Tracktion plugin. Only
    files under src/wfg/engine/audio/ and their tests should include it. */
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <tracktion_engine/tracktion_engine.h>

#include <vector>

/*
    A voice's slot for one plugin of the show's set, as a Tracktion plugin: a
    ProxyLane wrapped so it sits in the track's chain between the EQ and the
    output stage (Phase 9a, decisions AE and AF).

    CueOutputPlugin's shape line for line, and for its reasons. No automatable
    parameters - the values are atomics on the lane that the tick thread
    writes, as it writes the level. Zero declared latency, deliberately: the
    round trip is absorbed inside the block, and a plugin's own latency
    arrives uncompensated (§17.6). No sidechain, the width and the slot carried
    on the state because PluginCreationInfo carries nothing else, and the
    `getNumOutputChannelsGivenInputs` override that actually sizes the buffer.

    TRACKTION'S OWN BYPASS IS NEVER TOUCHED (plan decision 13). `isEnabled` is
    a CachedValue on the Edit's tree, message-thread only, and leaving it alone
    means PluginNode never bypasses this plugin and never builds a latency
    processor for it. The bypass is the lane's `enabled` flag, and a lane that
    is off returns before it reads a sample - so a voice whose cue switches
    nothing in renders bit-for-bit what it rendered before this plugin existed.

    UNBOUND UNTIL THE HOST BINDS IT. The plugin exists from the moment the Edit
    is built; the region it talks through exists once ProxyHost has made it and
    launched the child. Between the two, and after a failure, the lane passes
    the block through untouched.
*/
namespace wfg::audio
{
    class ProxyPlugin final : public tracktion::engine::Plugin
    {
    public:
        explicit ProxyPlugin (tracktion::engine::PluginCreationInfo);
        ~ProxyPlugin() override;

        /** The name Tracktion stores in the Edit and looks the type up by. */
        static const char* xmlTypeName;

        /** The ValueTree to hand `PluginList::insertPlugin`; the width and
            the set index ride on it. */
        static juce::ValueTree create (int numChannels, int slot);

        /** The lane and its atomics. Any thread; see ProxyLane. */
        plugin::ProxyLane& lane() noexcept              { return proxyLane; }
        const plugin::ProxyLane& lane() const noexcept  { return proxyLane; }

        int numChannels() const noexcept  { return channels; }

        /** Which plugin of the set this is, in `plugins/order`. */
        int slot() const noexcept         { return setSlot; }

        //======================================================================
        juce::String getName() const override;
        juce::String getPluginType() override;
        juce::String getSelectableDescription() override;

        void initialise (const tracktion::engine::PluginInitialisationInfo&) override;
        void deinitialise() override;
        void applyToBuffer (const tracktion::engine::PluginRenderContext&) override;

        BusLayout getBusses() const override;
        int getNumOutputChannelsGivenInputs (int numInputs) override;

        bool producesAudioWhenNoAudioInput() override    { return false; }
        double getLatencySeconds() override              { return 0.0; }
        bool canSidechain() override                     { return false; }
        bool shouldMeasureCpuUsage() const noexcept      { return false; }
        bool canBeAddedToClip() override                 { return false; }
        bool canBeAddedToRack() override                 { return false; }
        void restorePluginStateFromValueTree (const juce::ValueTree&) override {}

    private:
        int channels = 2;
        int setSlot = 0;
        plugin::ProxyLane proxyLane;

        /*  The channel pointers for one call, sized in initialise() and never
            after. A vector of pointers, not of samples: the buffer is the
            graph's. */
        std::vector<float*> pointers;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProxyPlugin)
    };
}
