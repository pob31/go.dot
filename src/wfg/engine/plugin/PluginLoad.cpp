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

#include <wfg/engine/plugin/PluginLoad.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace wfg::plugin
{
    namespace
    {
        juce::String widthWord (int channels)
        {
            if (channels == 1) return "mono";
            if (channels == 2) return "stereo";
            return juce::String (channels) + " channels";
        }

        /*  One rung: the main buses at `in` and `out`, every other bus off -
            or, `othersAsTheyAre`, left as the plugin had them. */
        bool tryRung (juce::AudioProcessor& processor, const juce::AudioChannelSet& in,
                      const juce::AudioChannelSet& out, bool othersAsTheyAre)
        {
            auto layout = processor.getBusesLayout();

            if (layout.inputBuses.isEmpty() || layout.outputBuses.isEmpty())
                return false;

            layout.inputBuses.getReference (0) = in;
            layout.outputBuses.getReference (0) = out;

            if (! othersAsTheyAre)
            {
                for (int bus = 1; bus < layout.inputBuses.size(); ++bus)
                    layout.inputBuses.getReference (bus) = juce::AudioChannelSet::disabled();

                for (int bus = 1; bus < layout.outputBuses.size(); ++bus)
                    layout.outputBuses.getReference (bus) = juce::AudioChannelSet::disabled();
            }

            return processor.setBusesLayout (layout)
                     && processor.getMainBusNumInputChannels() == in.size()
                     && processor.getMainBusNumOutputChannels() == out.size();
        }
    }

    bool chooseLayout (juce::AudioProcessor& processor, int voiceChannels, InsertLayout& chosen, std::string& problem)
    {
        using Set = juce::AudioChannelSet;
        const auto w = std::clamp (voiceChannels, 1, 64);

        /*  THE WIDTHS, in order; each asked first in the standard layout for
            its count and then as plain numbered channels - which is how an
            LV2 whose ports name no speaker describes itself, and it matches
            nothing else. */
        std::vector<std::pair<int, int>> widths;

        if (w == 1)
            widths = { { 1, 1 }, { 1, 2 }, { 2, 2 } };
        else
        {
            widths.push_back ({ w, w });

            if (w != 2)
                widths.push_back ({ 2, 2 });

            widths.push_back ({ 1, 2 });
            widths.push_back ({ 1, 1 });
        }

        std::vector<std::pair<Set, Set>> rungs;

        for (const auto& [in, out] : widths)
        {
            rungs.push_back ({ Set::canonicalChannelSet (in), Set::canonicalChannelSet (out) });
            rungs.push_back ({ Set::discreteChannels (in), Set::discreteChannels (out) });
        }

        for (const auto& [in, out] : rungs)
            for (const auto othersAsTheyAre : { false, true })
                if (tryRung (processor, in, out, othersAsTheyAre))
                {
                    chosen.inputs = processor.getMainBusNumInputChannels();
                    chosen.outputs = processor.getMainBusNumOutputChannels();
                    chosen.words = (widthWord (chosen.inputs) + " in, " + widthWord (chosen.outputs) + " out").toStdString();
                    return true;
                }

        problem = "it takes neither the voice's " + std::to_string (w) + " channels, stereo nor mono on its"
                  " main buses, so it cannot be an insert here";
        return false;
    }

    std::unique_ptr<juce::AudioPluginInstance> makeInsertInstance (juce::AudioPluginFormatManager& manager,
                                                                   const juce::PluginDescription& description,
                                                                   int channels, double sampleRate, int blockSize,
                                                                   const juce::File& preset, InsertLayout& layout,
                                                                   std::string& problem)
    {
        juce::String error;
        auto instance = manager.createPluginInstance (description, sampleRate, blockSize, error);

        if (instance == nullptr)
        {
            problem = "could not create " + description.name.toStdString() + ": "
                        + (error.isEmpty() ? std::string ("no reason given") : error.toStdString());
            return nullptr;
        }

        /*  ONE THAT HAS NO MAIN INPUT AT ALL - an instrument - is refused: an
            insert on a voice must take audio. Otherwise the ladder. */
        if (instance->getBusCount (true) == 0 || instance->getBusCount (false) == 0)
        {
            problem = description.name.toStdString() + " takes no audio in or gives none out, so it"
                      " cannot be an insert on a voice";
            return nullptr;
        }

        if (! chooseLayout (*instance, channels, layout, problem))
        {
            problem = description.name.toStdString() + ": " + problem;
            return nullptr;
        }

        instance->setNonRealtime (false);
        instance->prepareToPlay (sampleRate, blockSize);

        if (preset.existsAsFile())
        {
            juce::MemoryBlock bytes;

            if (! preset.loadFileAsData (bytes))
            {
                problem = "could not read the preset " + preset.getFullPathName().toStdString();
                return nullptr;
            }

            /*  A .vstpreset goes to the VST3 client, whose loader is the SDK's
                own; any other state goes the JUCE way. */
            auto applied = false;

            if (auto* vst3 = instance->getVST3Client())
                applied = vst3->setPreset (bytes);

            if (! applied)
                instance->setStateInformation (bytes.getData(), static_cast<int> (bytes.getSize()));
        }

        return instance;
    }

    void addFormatFor (juce::AudioPluginFormatManager& manager, const juce::PluginDescription& description,
                       const juce::String& bundle)
    {
        const auto& name = description.pluginFormatName;

       #if JUCE_INTERNAL_HAS_VST3
        if (name == "VST3")
        {
            manager.addFormat (std::make_unique<juce::VST3PluginFormat>());
            return;
        }
       #endif

       #if JUCE_INTERNAL_HAS_LV2
        if (name == "LV2")
        {
            auto lv2 = std::make_unique<juce::LV2PluginFormat>();

            /*  Loading the bundle by its folder is what JUCE's own scan does
                for a file it is given; the plugins in it join the world. */
            if (bundle.isNotEmpty())
            {
                juce::OwnedArray<juce::PluginDescription> found;
                lv2->findAllTypesForFile (found, bundle);
            }

            manager.addFormat (std::move (lv2));
            return;
        }
       #endif

       #if JUCE_INTERNAL_HAS_AU
        if (name == "AudioUnit")
        {
            manager.addFormat (std::make_unique<juce::AudioUnitPluginFormat>());
            return;
        }
       #endif

        juce::ignoreUnused (bundle);
        juce::addDefaultFormatsToManager (manager);
    }

    bool readDescription (const std::string& path, juce::PluginDescription& description, std::string& problem)
    {
        juce::String bundle;
        return readDescription (path, description, bundle, problem);
    }

    bool readDescription (const std::string& path, juce::PluginDescription& description, juce::String& bundle,
                          std::string& problem)
    {
        const juce::File file { juce::String (path) };
        const auto xml = juce::parseXML (file);

        if (xml == nullptr || ! description.loadFromXml (*xml))
        {
            problem = "no plugin description at " + path;
            return false;
        }

        bundle = xml->getStringAttribute ("bundle");
        return true;
    }
}
