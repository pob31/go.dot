/* This file is part of Go.dot — https://github.com/pob31/go.dot
 *
 * Copyright (C) 2026 Pierre-Olivier Boulant
 *
 * Go.dot is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. Go.dot is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * (LICENSE, at the repository root) for more details.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <wfg/engine/audio/CueMatrix.h>

/*  AN audio/-INTERNAL HEADER. It names Tracktion types, because the thing it
    declares IS a Tracktion plugin, so including it drags the two macro traps
    Console.cpp records - a bare `#undef __TEXT` and a mid-translation-unit
    redefinition of VERSION - into whatever includes it. Only files under
    src/wfg/engine/audio/ and their tests should. AudioHost.h is the vendor-free
    surface the rest of the engine talks to.
*/
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <tracktion_engine/tracktion_engine.h>

#include <vector>

/*  A track's output stage, as a Tracktion plugin: the cue's level and its
    routing matrix, sitting at the end of the track and writing every hardware
    output channel the rig has.

    WHY A PLUGIN AND NOT A TRACK OUTPUT. Because spike 04 measured that changing
    a track's output device rebuilds the playback graph, and PRD §3.25 fixes the
    graph at show load. Every track is routed once, to one wide device, and where
    a cue actually goes is decided by coefficients inside this plugin - an atomic
    store, not a structural edit.

    THE ONE FACT THAT MAKES IT WORK, and it cost an adversarial review to find:
    `getBusses()` does NOT size the buffer. `createNodeForPlugin` sizes the node
    to `max (mainInputBus, getNumOutputChannelsGivenInputs (incoming))`, so a
    plugin that declares a 64-channel output bus and leaves that method at its
    default gets a TWO channel buffer and quietly drops sixty-two channels.
    Both are declared below and the override is the load-bearing one.

    NO AUTOMATABLE PARAMETERS, deliberately. `PluginNode` chops a block into
    fine-grained sub-blocks whenever `isAutomationNeeded()` is true, which would
    make the block this plugin sees a different length from the one the tick
    clock counted. The level and the matrix are plain atomics on CueMatrix -
    Phase 3's fades write them at 50 Hz from the tick thread, which is exactly
    what §3.4 says control rate is for.
*/
namespace wfg::audio
{
    class CueOutputPlugin final : public tracktion::engine::Plugin
    {
    public:
        explicit CueOutputPlugin (tracktion::engine::PluginCreationInfo);
        ~CueOutputPlugin() override;

        /** The name Tracktion stores in the Edit and looks the type up by. */
        static const char* xmlTypeName;

        /*  The ValueTree to hand `PluginList::insertPlugin`. The channel counts
            ride on it because `PluginCreationInfo` carries only the Edit, a
            state tree and a flag - there is no other way to tell the plugin how
            wide it is, and it must know before the graph is built. */
        static juce::ValueTree create (int numInputs, int numOutputs);

        /** The level and coefficients. Any thread; every setter is an atomic
            store. This is what a cue, a fade and a stop all write. */
        CueMatrix& matrix() noexcept              { return cueMatrix; }
        const CueMatrix& matrix() const noexcept  { return cueMatrix; }

        int numOutputChannels() const noexcept    { return outputs; }
        int numInputChannels() const noexcept     { return inputs; }

        //======================================================================
        juce::String getName() const override;
        juce::String getPluginType() override;
        juce::String getSelectableDescription() override;

        void initialise (const tracktion::engine::PluginInitialisationInfo&) override;
        void deinitialise() override;
        void applyToBuffer (const tracktion::engine::PluginRenderContext&) override;

        BusLayout getBusses() const override;

        /*  THE ONE THAT SIZES THE BUFFER. See the class comment. */
        int getNumOutputChannelsGivenInputs (int numInputs) override;

        /*  A track whose slot is empty produces no node at all, and its plugin
            disappears from the graph with it. Saying yes here keeps the track
            present and silent, which is what a cue that has not been fired yet
            should sound like. */
        bool producesAudioWhenNoAudioInput() override    { return true; }

        /*  Go.dot owns time (PRD §3.25). A plugin reporting latency would drag a
            LatencyProcessor into the graph and shift everything else to match. */
        double getLatencySeconds() override              { return 0.0; }

        /*  A sidechain would flip the node's channel count to "whatever" and add
            a remapping node - a structural change to a graph the PRD fixes at
            load. */
        bool canSidechain() override                     { return false; }

        /*  Two high-resolution clock reads and an atomic store per block, for a
            meter nothing displays. VolumeAndPanPlugin declines it too. */
        bool shouldMeasureCpuUsage() const noexcept      { return false; }

        bool canBeAddedToClip() override                 { return false; }
        bool canBeAddedToRack() override                 { return false; }

        /*  The base implementation is `jassertfalse`. Nothing in Go.dot's load
            path reaches it - the Edit is generated, never read back (§3.25) -
            but a preset or UI path would, and a debug break is not what should
            happen then. */
        void restorePluginStateFromValueTree (const juce::ValueTree&) override {}

    private:
        int inputs = 2;
        int outputs = 2;

        CueMatrix cueMatrix;

        /*  The graph hands one buffer that is both input and output, so the
            input rows have to be copied aside before the matrix overwrites
            them. Sized in initialise() and never after. */
        juce::AudioBuffer<float> scratch;
        std::vector<float*> outputPointers;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CueOutputPlugin)
    };
}
