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

#include <wfg/engine/audio/CueOutputPlugin.h>

#include <algorithm>

namespace wfg::audio
{
    namespace te = tracktion::engine;

    namespace
    {
        /*  Our own property names on the plugin's state tree. Prefixed because
            they sit in the same tree as Tracktion's own, and a collision would
            be silent. */
        const juce::Identifier inputsProperty  { "wfgInputs" };
        const juce::Identifier outputsProperty { "wfgOutputs" };

        int readCount (const juce::ValueTree& state, const juce::Identifier& name, int fallback)
        {
            const auto value = static_cast<int> (state.getProperty (name, fallback));

            return value > 0 ? value : fallback;
        }
    }

    //==============================================================================
    const char* CueOutputPlugin::xmlTypeName = "godotCueOutput";

    juce::ValueTree CueOutputPlugin::create (int numInputs, int numOutputs)
    {
        return te::createValueTree (te::IDs::PLUGIN,
                                    te::IDs::type, xmlTypeName,
                                    inputsProperty, std::max (1, numInputs),
                                    outputsProperty, std::max (1, numOutputs));
    }

    //==============================================================================
    CueOutputPlugin::CueOutputPlugin (te::PluginCreationInfo info)
        : te::Plugin (info)
    {
        /*  Read once, here, and never changed. The width is what the playback
            graph is built around, so changing it later would be a structural
            edit - which the PRD forbids after load. The document's <Bus>
            elements therefore have to be parsed before the tracks are made. */
        inputs = readCount (state, inputsProperty, 2);
        outputs = readCount (state, outputsProperty, 2);
    }

    CueOutputPlugin::~CueOutputPlugin()
    {
        /*  Required of every Selectable, in the innermost subclass. Without it
            Tracktion asserts when something is still listening. */
        notifyListenersOfDeletion();
    }

    //==============================================================================
    juce::String CueOutputPlugin::getName() const                { return "Go.dot Cue Output"; }
    juce::String CueOutputPlugin::getPluginType()                { return xmlTypeName; }
    juce::String CueOutputPlugin::getSelectableDescription()     { return getName(); }

    te::Plugin::BusLayout CueOutputPlugin::getBusses() const
    {
        /*  The input side is `none()`, meaning "no channel-count requirement" -
            whatever the track carries passes through. It must still be PRESENT:
            `takesAudioInput()` is `! isSynth() && ! getBusses().inputs.empty()`,
            and a track whose plugin takes no audio reports that it cannot play
            any.

            This declaration is honest but it is NOT what widens the buffer.
            That is getNumOutputChannelsGivenInputs, below. */
        return BusLayout::singleInOut (te::ChannelConfiguration::none(),
                                       te::ChannelConfiguration::discreteChannels (outputs));
    }

    int CueOutputPlugin::getNumOutputChannelsGivenInputs (int)
    {
        /*  THE LOAD-BEARING LINE. `createNodeForPlugin` sizes the node to
            max (mainInputBus, this), so the default - which answers 2, from the
            base class's left/right channel names - would give a stereo buffer
            and silently drop every channel above the second. */
        return outputs;
    }

    //==============================================================================
    void CueOutputPlugin::initialise (const te::PluginInitialisationInfo& info)
    {
        /*  Message thread, and the only place this object allocates. Everything
            the audio thread will touch is sized here (PRD §4.2). */
        scratch.setSize (inputs, info.blockSizeSamples, false, true, false);
        outputPointers.assign (static_cast<std::size_t> (outputs), nullptr);

        cueMatrix.prepare (inputs, outputs, info.sampleRate, info.blockSizeSamples);

        /*  Snapped rather than ramped. initialise() runs again on every graph
            rebuild, and a rebuild is not a musical event - sliding the level
            back up from wherever the last graph left it would be. */
        cueMatrix.snapToTargets();
    }

    void CueOutputPlugin::deinitialise()
    {
    }

    void CueOutputPlugin::resetPeaks() noexcept
    {
        lastInputPeak.store (0.0f, std::memory_order_relaxed);
        lastOutputPeak.store (0.0f, std::memory_order_relaxed);
    }

    //==============================================================================
    void CueOutputPlugin::applyToBuffer (const te::PluginRenderContext& context)
    {
        if (context.destBuffer == nullptr || context.bufferNumSamples <= 0)
            return;

        auto& destination = *context.destBuffer;

        /*  ONE BUFFER, BOTH WAYS. The graph hands a buffer that is already as
            wide as this plugin asked to be, with the track's own channels in the
            first rows and the rest zeroed. Output therefore aliases input, and
            CueMatrix overwrites what it writes - so the input rows are copied
            aside first. That copy is why `scratch` exists.

            The buffer is NEVER resized. Plugin.h's own comment says a plugin
            should resize it to its output count; in this pin that is wrong for
            this path - the buffer is a non-owning wrapper around the graph's
            channel pointers, and resizing it would allocate on the audio thread
            and then write somewhere nothing reads. */
        const auto channels = destination.getNumChannels();
        const auto copied = std::min (inputs, channels);

        auto remaining = context.bufferNumSamples;
        auto offset = 0;

        /*  Chunked against `scratch`'s size rather than assuming the block is no
            larger than the one initialise() was told about. It should not be,
            but a block that was would otherwise walk off the end of the scratch
            buffer, which is not a thing to discover during a show. */
        while (remaining > 0)
        {
            const auto frames = std::min (remaining, scratch.getNumSamples());
            const auto start = context.bufferStartSample + offset;

            for (int channel = 0; channel < copied; ++channel)
                juce::FloatVectorOperations::copy (scratch.getWritePointer (channel),
                                                   destination.getReadPointer (channel, start),
                                                   frames);

            for (int channel = 0; channel < channels; ++channel)
                outputPointers[static_cast<std::size_t> (channel)]
                    = destination.getWritePointer (channel, start);

            {
                auto peak = lastInputPeak.load (std::memory_order_relaxed);

                for (int channel = 0; channel < copied; ++channel)
                    peak = std::max (peak, scratch.getMagnitude (channel, 0, frames));

                lastInputPeak.store (peak, std::memory_order_relaxed);
            }

            cueMatrix.process (scratch.getArrayOfReadPointers(), copied,
                               outputPointers.data(), channels, frames);

            {
                auto peak = lastOutputPeak.load (std::memory_order_relaxed);

                for (int channel = 0; channel < channels; ++channel)
                    peak = std::max (peak, destination.getMagnitude (channel, start, frames));

                lastOutputPeak.store (peak, std::memory_order_relaxed);
            }

            remaining -= frames;
            offset += frames;
        }
    }
}
