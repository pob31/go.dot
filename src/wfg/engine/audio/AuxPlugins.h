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

/*  AN audio/-INTERNAL HEADER, for CueOutputPlugin.h's reason: the things it
    declares ARE Tracktion plugins. Only files under src/wfg/engine/audio/ and
    their tests include it; AudioHost.h is the surface the rest of the engine
    sees.
*/

#include <wfg/engine/audio/AuxBus.h>
#include <wfg/engine/audio/CueMatrix.h>

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <tracktion_engine/tracktion_engine.h>

#include <atomic>
#include <vector>

/*  THE TWO ENDS OF AN AUX (namespace draft §93): the tap every track writes its
    sends into the aux bus with, and the stage at the head of an aux's track
    that reads them back out. AuxBus.h says how the two meet and why the order
    between them is the graph's.

    NO AUTOMATABLE PARAMETERS and no latency, for CueOutputPlugin's reasons.
*/
namespace wfg::audio
{
    /*  THE AUX TAP: just before a track's output stage, a matrix of its own
        from the track's channels to every aux input channel, written into the
        track's slot of the bus. The buffer goes on unchanged to the output
        stage, which sends the same signal to the outputs and the mixes.

        BELOW THE CUE'S LEVEL, as every send is: the level the output stage is
        told is copied onto this matrix each block, and it ramps over the same
        one-tick slew, so a fade on the cue moves its sends into the auxes with
        its direct out and its mixes - one DCA over all of them. Nothing that
        writes a level needs to know this stage is here.

        WHY BEFORE THE OUTPUT STAGE AND NOT IN IT. The silent Tracktion send
        that orders the auxes after this track copies the buffer it sits on;
        here that buffer is the track's two channels, after the output stage it
        would be every output the rig has, for every voice. */
    class AuxTapPlugin final : public tracktion::engine::Plugin
    {
    public:
        explicit AuxTapPlugin (tracktion::engine::PluginCreationInfo);
        ~AuxTapPlugin() override;

        static const char* xmlTypeName;

        /*  The track's width, its slot in the bus, and how many aux input
            channels the bus has, carried on the state tree for the other
            stages' reason. */
        static juce::ValueTree create (int numInputs, int writer, int columns);

        /*  The bus and the level to follow, bound once after insertion and
            before the playback context is allocated. A null bus writes
            nothing. */
        void bind (AuxBus* auxBus, const CueMatrix* levelOf) noexcept;

        /** The sends into the auxes. Any thread; atomics, as an output stage's. */
        CueMatrix& matrix() noexcept              { return tap; }
        const CueMatrix& matrix() const noexcept  { return tap; }

        int writerIndex() const noexcept  { return writer; }

        //======================================================================
        juce::String getName() const override;
        juce::String getPluginType() override;
        juce::String getSelectableDescription() override;

        void initialise (const tracktion::engine::PluginInitialisationInfo&) override;
        void deinitialise() override;
        void applyToBuffer (const tracktion::engine::PluginRenderContext&) override;

        BusLayout getBusses() const override;
        int getNumOutputChannelsGivenInputs (int numInputs) override;

        bool producesAudioWhenNoAudioInput() override    { return true; }
        double getLatencySeconds() override              { return 0.0; }
        bool canSidechain() override                     { return false; }
        bool shouldMeasureCpuUsage() const noexcept      { return false; }
        bool canBeAddedToClip() override                 { return false; }
        bool canBeAddedToRack() override                 { return false; }
        void restorePluginStateFromValueTree (const juce::ValueTree&) override {}

    private:
        int inputs = 2;
        int writer = 0;
        int columns = 0;

        AuxBus* bus = nullptr;
        const CueMatrix* level = nullptr;

        CueMatrix tap;
        std::vector<const float*> inputPointers;
        std::vector<float*> rowPointers;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AuxTapPlugin)
    };

    //==============================================================================
    /*  THE AUX INPUT: the first Go.dot stage on an aux's track, where the sum
        of everything sent to it enters - in place of a rack channel's live
        input. Its columns of the bus, added across every writer; a mono aux
        fills the first channel, as a mono input does.

        OPEN UNLESS SHUT (AVU): the show holds an aux, so it takes its sends
        from the moment the graph stands. A double Esc shuts it in a
        millisecond (AWA), its tail goes with the plugins' reset, and the next
        GO opens it again over five milliseconds. Its gain is the sine of how
        far along its ramp it is, the live input's equal-power curve. */
    class AuxInputPlugin final : public tracktion::engine::Plugin
    {
    public:
        explicit AuxInputPlugin (tracktion::engine::PluginCreationInfo);
        ~AuxInputPlugin() override;

        static const char* xmlTypeName;

        /*  The track's width, the aux's first column in the bus and how many
            it takes (one or two). */
        static juce::ValueTree create (int numChannels, int firstColumn, int width);

        /** The bus, bound once after insertion. Null reads silence. */
        void bind (const AuxBus* auxBus) noexcept  { bus = auxBus; }

        /*  THE GATE. Open at once over `rampSeconds`, or shut over `seconds`
            (`killSeconds` for a double Esc). Any thread; atomics. */
        void open() noexcept;
        void shut (double seconds) noexcept;

        /** Open, or still ramping shut. Any thread. */
        bool isPassing() const noexcept;

        static constexpr double rampSeconds = 0.005;
        static constexpr double killSeconds = 0.001;

        int firstColumn() const noexcept  { return first; }
        int columnWidth() const noexcept  { return width; }

        //======================================================================
        juce::String getName() const override;
        juce::String getPluginType() override;
        juce::String getSelectableDescription() override;

        void initialise (const tracktion::engine::PluginInitialisationInfo&) override;
        void deinitialise() override;
        void applyToBuffer (const tracktion::engine::PluginRenderContext&) override;

        BusLayout getBusses() const override;
        int getNumOutputChannelsGivenInputs (int numInputs) override;

        /*  An aux's track has no clip: this keeps it in the graph, as the live
            input does a rack channel's. */
        bool producesAudioWhenNoAudioInput() override    { return true; }
        double getLatencySeconds() override              { return 0.0; }
        bool canSidechain() override                     { return false; }
        bool shouldMeasureCpuUsage() const noexcept      { return false; }
        bool canBeAddedToClip() override                 { return false; }
        bool canBeAddedToRack() override                 { return false; }
        void restorePluginStateFromValueTree (const juce::ValueTree&) override {}

    private:
        int channels = 2;
        int first = 0;
        int width = 2;

        const AuxBus* bus = nullptr;

        std::atomic<bool> wantOpen { true };
        std::atomic<double> rampLength { rampSeconds };
        std::atomic<float> gain { 1.0f };
        double sampleRate = 48000.0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AuxInputPlugin)
    };
}
