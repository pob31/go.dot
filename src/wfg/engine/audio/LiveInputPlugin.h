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
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <tracktion_engine/tracktion_engine.h>

#include <atomic>
#include <cstdint>

/*  THE LIVE INPUT STAGE (Phase 9b, namespace draft §18.4, decision CH): the
    first plugin on every rack track, where the input a mic cue takes enters
    the channel.

    WHY NOT TRACKTION'S OWN INPUT DEVICES. Its `WaveInputDevice` takes two
    locks a block and sizes a thirty-second retrospective buffer on the audio
    thread - both forbidden by PRD §4.2, and the rtsan job would refuse them.
    So the host copies each block's logical inputs into a TAP of its own before
    Tracktion runs, and this stage reads from it during the same call: the input
    of block n is heard in block n, nothing adds a block of delay, and nothing
    here locks or allocates.

    WHICH INPUT IS A NUMBER, NOT A CONNECTION. The first logical input and the
    width are atomics the tick thread writes, so a mic cue taking another input
    never rebuilds the graph - the coefficient trick of every output stage,
    turned round to face the inputs.

    THE GATE. A channel no cue holds is silent, and a cue opens it at a sample
    Go.dot places, as a launch is placed; it opens and shuts on a short ramp so
    that neither is a click. A shut asked to be fast - a kill - takes a fifth of
    the ramp. And after a gap in the blocks - a device that went away and came
    back (PRD §6.2) - an open gate ramps in again rather than stepping.

    NO AUTOMATABLE PARAMETERS and no latency, for CueOutputPlugin's reasons.
*/
namespace wfg::audio
{
    /*  WHAT THE HOST HANDS EVERY LIVE INPUT STAGE: this block's logical inputs
        and where the block starts in Go.dot's own count of samples. Written by
        `AudioHost::processBlock` before Tracktion is called and read by the
        stages during that call, which is the same block on the same thread; the
        buffer is set aside at the host's start and never resized while blocks
        run. */
    struct LiveInputTap
    {
        const juce::AudioBuffer<float>* buffer = nullptr;
        int channels = 0;
        std::int64_t blockStart = 0;
    };

    class LiveInputPlugin final : public tracktion::engine::Plugin
    {
    public:
        explicit LiveInputPlugin (tracktion::engine::PluginCreationInfo);
        ~LiveInputPlugin() override;

        static const char* xmlTypeName;

        /*  The ValueTree to hand `PluginList::insertPlugin`, carrying the
            track's width as the other stages' do. */
        static juce::ValueTree create (int numChannels);

        /*  The tap this stage reads, bound once after insertion and before the
            playback context is allocated. Null reads silence. */
        void bindTap (const LiveInputTap* tap) noexcept     { boundTap = tap; }

        //======================================================================
        /*  WHAT THE CUE SAYS: the first logical input and how many to take, one
            or two. Tick thread; two relaxed stores. A first input of -1 takes
            nothing. */
        void setSource (int firstInput, int width) noexcept;

        /*  THE GATE, opened at a sample of Go.dot's count - the launch's own
            instant - or at once for -1. Tick thread. */
        void openAt (std::int64_t sample) noexcept;

        /*  And shut: on the ordinary ramp, or on a fifth of it for a kill. */
        void shut (bool fast) noexcept;

        /*  Whether anything is coming through: the gate open, or still ramping
            down. Any thread. */
        bool isPassing() const noexcept;

        static constexpr double rampSeconds = 0.005;

        //======================================================================
        juce::String getName() const override;
        juce::String getPluginType() override;
        juce::String getSelectableDescription() override;

        void initialise (const tracktion::engine::PluginInitialisationInfo&) override;
        void deinitialise() override;
        void applyToBuffer (const tracktion::engine::PluginRenderContext&) override;

        BusLayout getBusses() const override;
        int getNumOutputChannelsGivenInputs (int numInputs) override;

        /*  A rack track has no clip: this is what keeps it in the graph, fed a
            silence the stage writes its input over. */
        bool producesAudioWhenNoAudioInput() override    { return true; }
        double getLatencySeconds() override              { return 0.0; }
        bool canSidechain() override                     { return false; }
        bool shouldMeasureCpuUsage() const noexcept      { return false; }
        bool canBeAddedToClip() override                 { return false; }
        bool canBeAddedToRack() override                 { return false; }
        void restorePluginStateFromValueTree (const juce::ValueTree&) override {}

    private:
        int channels = 2;
        const LiveInputTap* boundTap = nullptr;

        std::atomic<int> sourceFirst { -1 }, sourceWidth { 1 };
        std::atomic<bool> wantOpen { false }, shutFast { false };
        std::atomic<std::int64_t> openSample { -1 };

        /*  The audio thread's own: the gain now, how far a sample moves it, and
            where the last block ended, which is how a gap is noticed. */
        std::atomic<float> gain { 0.0f };
        float rampStep = 1.0f;
        std::int64_t lastBlockEnd = -1;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LiveInputPlugin)
    };
}
