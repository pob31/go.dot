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

#include <wfg/engine/audio/AuxPlugins.h>
#include <wfg/engine/rt/RtCheck.h>

#include <algorithm>
#include <cmath>

namespace wfg::audio
{
    namespace te = tracktion::engine;

    namespace
    {
        const juce::Identifier inputsProperty  { "wfgInputs" };
        const juce::Identifier writerProperty  { "wfgWriter" };
        const juce::Identifier columnsProperty { "wfgColumns" };
        const juce::Identifier channelsProperty { "wfgChannels" };
        const juce::Identifier firstProperty   { "wfgFirstColumn" };
        const juce::Identifier widthProperty   { "wfgWidth" };

        int readInt (const juce::ValueTree& state, const juce::Identifier& name, int fallback)
        {
            return static_cast<int> (state.getProperty (name, fallback));
        }
    }

    //==============================================================================
    const char* AuxTapPlugin::xmlTypeName = "godotAuxTap";

    juce::ValueTree AuxTapPlugin::create (int numInputs, int writerIndex, int columnCount)
    {
        return te::createValueTree (te::IDs::PLUGIN,
                                    te::IDs::type, xmlTypeName,
                                    inputsProperty, std::max (1, numInputs),
                                    writerProperty, std::max (0, writerIndex),
                                    columnsProperty, std::max (0, columnCount));
    }

    AuxTapPlugin::AuxTapPlugin (te::PluginCreationInfo info)
        : te::Plugin (info)
    {
        inputs = std::clamp (readInt (state, inputsProperty, 2), 1, 64);
        writer = std::max (0, readInt (state, writerProperty, 0));
        columns = std::max (0, readInt (state, columnsProperty, 0));
    }

    AuxTapPlugin::~AuxTapPlugin()
    {
        notifyListenersOfDeletion();
    }

    juce::String AuxTapPlugin::getName() const             { return "Go.dot Aux Tap"; }
    juce::String AuxTapPlugin::getPluginType()             { return xmlTypeName; }
    juce::String AuxTapPlugin::getSelectableDescription()  { return getName(); }

    te::Plugin::BusLayout AuxTapPlugin::getBusses() const
    {
        return BusLayout::singleInOut (te::ChannelConfiguration::none(),
                                       te::ChannelConfiguration::discreteChannels (inputs));
    }

    int AuxTapPlugin::getNumOutputChannelsGivenInputs (int numInputs)
    {
        //  A pass-through: the track as wide as it arrives, never narrower.
        return std::max (inputs, numInputs);
    }

    void AuxTapPlugin::bind (AuxBus* auxBus, const CueMatrix* levelOf) noexcept
    {
        bus = auxBus;
        level = levelOf;
    }

    void AuxTapPlugin::initialise (const te::PluginInitialisationInfo& info)
    {
        /*  Message thread, and the only place this object allocates. A
            rebuild that keeps the plugin runs initialiseWithoutStopping, which
            leaves the matrix's coefficients where the sends put them. */
        tap.prepare (inputs, columns, info.sampleRate, info.blockSizeSamples);
        inputPointers.assign (static_cast<std::size_t> (inputs), nullptr);
        rowPointers.assign (static_cast<std::size_t> (columns), nullptr);

        if (level != nullptr)
            tap.setLevelDb (level->levelDb());

        tap.snapToTargets();
    }

    void AuxTapPlugin::deinitialise()
    {
    }

    void AuxTapPlugin::applyToBuffer (const te::PluginRenderContext& context)
    {
        const rt::ScopedRealtimeCheck goDotsOwnPlugin { rt::Region::ours };

        if (bus == nullptr || columns <= 0 || context.destBuffer == nullptr || context.bufferNumSamples <= 0)
            return;

        const auto& source = *context.destBuffer;
        const auto start = context.bufferStartSample;
        const auto frames = std::min (context.bufferNumSamples, bus->blockLimit() - start);

        if (frames <= 0)
        {
            bus->setSilent (writer, true);
            return;
        }

        //  The cue's level, followed: below it, as every send is.
        if (level != nullptr)
            tap.setLevelDb (level->levelDb());

        const auto usable = std::min (inputs, source.getNumChannels());

        for (int channel = 0; channel < usable; ++channel)
            inputPointers[static_cast<std::size_t> (channel)] = source.getReadPointer (channel, start);

        for (int column = 0; column < columns; ++column)
        {
            auto* row = bus->row (writer, column);
            rowPointers[static_cast<std::size_t> (column)] = row != nullptr ? row + start : nullptr;
        }

        tap.process (inputPointers.data(), usable, rowPointers.data(), columns, frames);
        bus->setSilent (writer, tap.lastBlockSilent());
    }

    //==============================================================================
    const char* AuxInputPlugin::xmlTypeName = "godotAuxInput";

    juce::ValueTree AuxInputPlugin::create (int numChannels, int firstColumn, int columnWidth)
    {
        return te::createValueTree (te::IDs::PLUGIN,
                                    te::IDs::type, xmlTypeName,
                                    channelsProperty, std::max (1, numChannels),
                                    firstProperty, std::max (0, firstColumn),
                                    widthProperty, std::clamp (columnWidth, 1, 2));
    }

    AuxInputPlugin::AuxInputPlugin (te::PluginCreationInfo info)
        : te::Plugin (info)
    {
        channels = std::clamp (readInt (state, channelsProperty, 2), 1, 64);
        first = std::max (0, readInt (state, firstProperty, 0));
        width = std::clamp (readInt (state, widthProperty, 2), 1, 2);
    }

    AuxInputPlugin::~AuxInputPlugin()
    {
        notifyListenersOfDeletion();
    }

    juce::String AuxInputPlugin::getName() const             { return "Go.dot Aux Input"; }
    juce::String AuxInputPlugin::getPluginType()             { return xmlTypeName; }
    juce::String AuxInputPlugin::getSelectableDescription()  { return getName(); }

    te::Plugin::BusLayout AuxInputPlugin::getBusses() const
    {
        return BusLayout::singleInOut (te::ChannelConfiguration::none(),
                                       te::ChannelConfiguration::discreteChannels (channels));
    }

    int AuxInputPlugin::getNumOutputChannelsGivenInputs (int numInputs)
    {
        return std::max (channels, numInputs);
    }

    void AuxInputPlugin::open() noexcept
    {
        rampLength.store (rampSeconds, std::memory_order_relaxed);
        wantOpen.store (true, std::memory_order_relaxed);
    }

    void AuxInputPlugin::shut (double seconds) noexcept
    {
        rampLength.store (std::max (killSeconds, seconds), std::memory_order_relaxed);
        wantOpen.store (false, std::memory_order_relaxed);
    }

    bool AuxInputPlugin::isPassing() const noexcept
    {
        return wantOpen.load (std::memory_order_relaxed) || gain.load (std::memory_order_relaxed) > 0.0f;
    }

    void AuxInputPlugin::initialise (const te::PluginInitialisationInfo& info)
    {
        sampleRate = info.sampleRate > 0.0 ? info.sampleRate : 48000.0;
    }

    void AuxInputPlugin::deinitialise()
    {
    }

    void AuxInputPlugin::applyToBuffer (const te::PluginRenderContext& context)
    {
        const rt::ScopedRealtimeCheck goDotsOwnPlugin { rt::Region::ours };

        if (context.destBuffer == nullptr || context.bufferNumSamples <= 0)
            return;

        auto& destination = *context.destBuffer;
        const auto frames = context.bufferNumSamples;
        const auto start = context.bufferStartSample;

        /*  WHAT ARRIVED IS NOT OURS. The track has no clip, and the aux return
            before this stage hands on the silent sends that ordered it - so the
            buffer is cleared and the bus's columns are the whole input. */
        for (int channel = 0; channel < destination.getNumChannels(); ++channel)
            destination.clear (channel, start, frames);

        auto g = gain.load (std::memory_order_relaxed);
        const auto open = wantOpen.load (std::memory_order_relaxed);

        if (! open && g <= 0.0f)
            return;

        const auto taken = std::min ({ width, channels, destination.getNumChannels() });

        if (bus != nullptr)
            for (int channel = 0; channel < taken; ++channel)
                bus->addColumn (first + channel, destination.getWritePointer (channel, start), start, frames);

        if (open && g >= 1.0f)
            return;

        const auto step = static_cast<float> (1.0 / std::max (1.0, rampLength.load (std::memory_order_relaxed)
                                                                      * sampleRate));
        const auto target = open ? 1.0f : 0.0f;

        for (int n = 0; n < frames; ++n)
        {
            g = target > g ? std::min (target, g + step) : std::max (target, g - step);
            const auto shaped = std::sin (juce::MathConstants<float>::halfPi * g);

            for (int channel = 0; channel < taken; ++channel)
                destination.setSample (channel, start + n, destination.getSample (channel, start + n) * shaped);
        }

        gain.store (g, std::memory_order_relaxed);
    }
}
