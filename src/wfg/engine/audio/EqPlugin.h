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

#include <wfg/engine/audio/CueEq.h>

/*  AN audio/-INTERNAL HEADER, for CueOutputPlugin.h's reason: it names
    Tracktion types, because the thing it declares IS a Tracktion plugin. Only
    files under src/wfg/engine/audio/ and their tests should include it. */
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <tracktion_engine/tracktion_engine.h>

#include <vector>

/*
    A voice's EQ stage, as a Tracktion plugin: CueEq wrapped so it sits in the
    track's chain before the output stage (Phase 9a, decision AD).

    CueOutputPlugin's shape line for line, and for its reasons. No automatable
    parameters - `isAutomationNeeded()` would make PluginNode chop the block -
    so the nineteen settings are atomics on CueEq that the tick thread writes
    directly, as it writes the level. Zero latency, no sidechain, the width
    carried on the state because PluginCreationInfo carries nothing else, and
    the `getNumOutputChannelsGivenInputs` override that actually sizes the
    buffer. In place: the graph hands one buffer that is both input and output,
    and an EQ has no reason to copy it aside.

    FLAT IS NOT TOUCHED. CueEq returns before it reads a sample when every
    section is out, so a voice whose cue has no EQ renders bit-for-bit what it
    rendered before this plugin existed - which every render driver in the
    tree asserts, arithmetically, from the first sample on.

    initialise() runs the first time and on a rate or block-size change
    (tracktion_Plugin.cpp's initialiseCount), not on every arm's rebuild: the
    filter state survives a rebuild, and a cue still sounding on another voice
    hears no click. An arm clears the state through a request the next block
    honours (CueEq::reset), while the voice it belongs to is silent.
*/
namespace wfg::audio
{
    class EqPlugin final : public tracktion::engine::Plugin
    {
    public:
        explicit EqPlugin (tracktion::engine::PluginCreationInfo);
        ~EqPlugin() override;

        /** The name Tracktion stores in the Edit and looks the type up by. */
        static const char* xmlTypeName;

        /** The ValueTree to hand `PluginList::insertPlugin`; the width rides on it. */
        static juce::ValueTree create (int numChannels);

        /** The filters and their atomics. Any thread; see CueEq. */
        CueEq& eq() noexcept              { return cueEq; }
        const CueEq& eq() const noexcept  { return cueEq; }

        int numChannels() const noexcept  { return channels; }

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
        CueEq cueEq;

        /*  The channel pointers for one call, sized in initialise() and never
            after. A vector of pointers, not of samples: the buffer is the
            graph's. */
        std::vector<float*> pointers;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqPlugin)
    };
}
