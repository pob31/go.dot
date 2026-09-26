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

namespace wfg::plugin
{
    std::unique_ptr<juce::AudioPluginInstance> makeInsertInstance (juce::AudioPluginFormatManager& manager,
                                                                   const juce::PluginDescription& description,
                                                                   int channels, double sampleRate, int blockSize,
                                                                   const juce::File& preset, std::string& problem)
    {
        juce::String error;
        auto instance = manager.createPluginInstance (description, sampleRate, blockSize, error);

        if (instance == nullptr)
        {
            problem = "could not create " + description.name.toStdString() + ": "
                        + (error.isEmpty() ? std::string ("no reason given") : error.toStdString());
            return nullptr;
        }

        /*  THE VOICE'S WIDTH, asked for on the main buses. A plugin that
            answers with another width is not argued with: the scratch buffer
            is as wide as it wants, and the voice's channels are the first of
            them. One that has no main input at all - an instrument - is
            refused: an insert on a voice must take audio. */
        const auto wanted = juce::AudioChannelSet::canonicalChannelSet (channels);
        auto layout = instance->getBusesLayout();

        if (! layout.inputBuses.isEmpty())  layout.inputBuses.getReference (0) = wanted;
        if (! layout.outputBuses.isEmpty()) layout.outputBuses.getReference (0) = wanted;

        if (! instance->setBusesLayout (layout))
            instance->enableAllBuses();

        if (instance->getTotalNumInputChannels() <= 0 || instance->getTotalNumOutputChannels() <= 0)
        {
            problem = description.name.toStdString() + " takes no audio in or gives none out, so it"
                      " cannot be an insert on a voice";
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
