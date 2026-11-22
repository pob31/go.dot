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

#include <wfg/engine/audio/CueDyn.h>
#include <wfg/engine/audio/KeyReader.h>

/*  AN audio/-INTERNAL HEADER, for EqPlugin.h's reason: the thing it declares
    IS a Tracktion plugin. Only files under src/wfg/engine/audio/ and their
    tests include it. */
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <tracktion_engine/tracktion_engine.h>

#include <vector>

/*
    A track's dynamics stage of Go.dot's own, as a Tracktion plugin (namespace
    draft §141, SC.7): CueDyn wrapped so it sits after the EQ and before the
    inserts on every voice, rack channel and aux (BHU). EqPlugin's shape line
    for line, and for its reasons - no automatable parameter, no latency, no
    Tracktion sidechain, the width on the state, in place.

    ITS KEY is a KeyReader, as an insert's place has (§141): an input from the
    tap, an aux from the key bus, or this cue after its level - the sound here
    times what its output stage is told. Bound once when the track is built;
    the source a word the tick thread stores. The key is read only while the
    stage is on.

    OFF IS NOT TOUCHED: CueDyn returns before reading a sample, so every render
    driver in the tree renders what it rendered before this plugin existed.
*/
namespace wfg::audio
{
    class DynPlugin final : public tracktion::engine::Plugin
    {
    public:
        explicit DynPlugin (tracktion::engine::PluginCreationInfo);
        ~DynPlugin() override;

        static const char* xmlTypeName;
        static juce::ValueTree create (int numChannels);

        CueDyn& dyn() noexcept              { return cueDyn; }
        const CueDyn& dyn() const noexcept  { return cueDyn; }

        /*  What it listens to, and where an input, an aux and the level are
            read: the source from the tick thread, the rest bound once before
            the playback context is allocated. */
        void setKey (const KeySource& source) noexcept       { keyReader.setSource (source); }
        KeySource key() const noexcept                       { return keyReader.source(); }
        void bindTap (const LiveInputTap* tap) noexcept      { keyReader.bindTap (tap); }
        void bindKeyBus (const AuxBus* keys) noexcept        { keyReader.bindKeyBus (keys); }
        void bindOutputs (const LiveInputTap* outs) noexcept { keyReader.bindOutputs (outs); }
        void bindLevel (const CueMatrix* levelOf) noexcept   { keyReader.bindLevel (levelOf); }

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
        bool shouldMeasureCpuUsage() const noexcept override { return false; }
        bool canBeAddedToClip() override                 { return false; }
        bool canBeAddedToRack() override                 { return false; }
        void restorePluginStateFromValueTree (const juce::ValueTree&) override {}

    private:
        int channels = 2;
        CueDyn cueDyn;
        KeyReader keyReader;
        std::vector<float*> pointers;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DynPlugin)
    };
}
