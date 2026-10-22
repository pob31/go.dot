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
#include <wfg/engine/audio/TakeVoice.h>

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <tracktion_engine/tracktion_engine.h>

/*  A PAD ON THE LIVE TAKE, AT THE HEAD OF A VOICE (namespace draft §105,
    AZR): on every voice, ahead of the EQ, so a slice of a take goes through
    the cue's EQ, plugins, sends and output as a file would -

    the slots' clips + THIS → `EqPlugin` → the set's plugins → the aux tap → `CueOutputPlugin`

    It adds what the voice's `TakeVoice` plays into the block, which is the
    clips' silence while a pad plays: a voice plays a take or its clips, never
    both. With no slice it adds nothing and touches nothing. Fifty lines that
    find the block's place in Go.dot's count - the live input stage's tap, as
    the recorder's does - and pass pointers.

    NO AUTOMATABLE PARAMETERS and no latency, for CueOutputPlugin's reasons.
*/
namespace wfg::audio
{
    class TakeReaderPlugin final : public tracktion::engine::Plugin
    {
    public:
        explicit TakeReaderPlugin (tracktion::engine::PluginCreationInfo);
        ~TakeReaderPlugin() override;

        static const char* xmlTypeName;

        /*  The ValueTree to hand `PluginList::insertPlugin`, carrying the
            track's width as the other stages' do. */
        static juce::ValueTree create (int numChannels);

        /*  The voice this stage plays and the tap whose count places each
            block - both the host's, bound once after insertion and before the
            playback context is allocated. */
        void bind (TakeVoice* voice, const LiveInputTap* tap) noexcept;

        //======================================================================
        juce::String getName() const override;
        juce::String getPluginType() override;
        juce::String getSelectableDescription() override;

        void initialise (const tracktion::engine::PluginInitialisationInfo&) override;
        void deinitialise() override;
        void applyToBuffer (const tracktion::engine::PluginRenderContext&) override;

        BusLayout getBusses() const override;
        int getNumOutputChannelsGivenInputs (int numInputs) override;

        //  A pad sounds whatever the clips are doing - which is nothing.
        bool producesAudioWhenNoAudioInput() override    { return true; }
        double getLatencySeconds() override              { return 0.0; }
        bool canSidechain() override                     { return false; }
        bool shouldMeasureCpuUsage() const noexcept override { return false; }
        bool canBeAddedToClip() override                 { return false; }
        bool canBeAddedToRack() override                 { return false; }
        void restorePluginStateFromValueTree (const juce::ValueTree&) override {}

    private:
        int channels = 2;
        TakeVoice* boundVoice = nullptr;
        const LiveInputTap* boundTap = nullptr;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TakeReaderPlugin)
    };
}
