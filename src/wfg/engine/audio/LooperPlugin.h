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

/*  AN audio/-INTERNAL HEADER, for CueOutputPlugin.h's reason: the thing it
    declares IS a Tracktion plugin. Only files under src/wfg/engine/audio/ and
    their tests include it; AudioHost.h is the surface the rest of the engine
    sees.
*/
#include <wfg/engine/audio/LiveInputPlugin.h>
#include <wfg/engine/audio/Looper.h>

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <tracktion_engine/tracktion_engine.h>

#include <array>

/*  THE RECORDER'S PLACE IN A SAMPLING CHANNEL (Phase 9c, stage 9c.2,
    namespace draft §19.2): between the plugins before the recorder and the EQ,
    so what they do is printed into the take and what comes after is heard as
    it loops -

    `LiveInputPlugin` → the plugins before → THIS → `EqPlugin` → the plugins after → `CueOutputPlugin`

    IT HOLDS NO TAKE. The `Looper` it hands each block to is the audio host's,
    kept beside the Edit in a store of its own, because every media arm
    rebuilds the graph and Load now and a change of interface throw the Edit
    away, and a take outlives all three while the channel's shape holds
    (§19.2). This plugin is fifty lines that find the block's place in
    Go.dot's count - the live input stage's tap, which the host writes before
    Tracktion runs - and pass pointers.

    NO AUTOMATABLE PARAMETERS and no latency, for CueOutputPlugin's reasons.
*/
namespace wfg::audio
{
    class LooperPlugin final : public tracktion::engine::Plugin
    {
    public:
        explicit LooperPlugin (tracktion::engine::PluginCreationInfo);
        ~LooperPlugin() override;

        static const char* xmlTypeName;

        /*  The ValueTree to hand `PluginList::insertPlugin`, carrying the
            track's width as the other stages' do. */
        static juce::ValueTree create (int numChannels);

        /*  The take this stage records and plays, and the tap whose count
            places each block - both the host's, bound once after insertion and
            before the playback context is allocated. A null take passes every
            block as it came. */
        void bind (Looper* take, const LiveInputTap* tap) noexcept;

        //======================================================================
        juce::String getName() const override;
        juce::String getPluginType() override;
        juce::String getSelectableDescription() override;

        void initialise (const tracktion::engine::PluginInitialisationInfo&) override;
        void deinitialise() override;
        void applyToBuffer (const tracktion::engine::PluginRenderContext&) override;

        BusLayout getBusses() const override;
        int getNumOutputChannelsGivenInputs (int numInputs) override;

        /*  A loop sounds whatever its input is doing - an input shut, or a gate
            that has closed, still leaves the take playing. */
        bool producesAudioWhenNoAudioInput() override    { return true; }
        double getLatencySeconds() override              { return 0.0; }
        bool canSidechain() override                     { return false; }
        bool shouldMeasureCpuUsage() const noexcept      { return false; }
        bool canBeAddedToClip() override                 { return false; }
        bool canBeAddedToRack() override                 { return false; }
        void restorePluginStateFromValueTree (const juce::ValueTree&) override {}

    private:
        int channels = 2;
        Looper* boundTake = nullptr;
        const LiveInputTap* boundTap = nullptr;
        std::array<float*, Looper::maxChannels> pointers {};

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LooperPlugin)
    };
}
