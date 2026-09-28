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

/*
    ONE WAY TO BRING A PLUGIN UP AS AN INSERT, for both children that do it.

    The voice child (`plugin-host`) makes one instance per voice and plays
    them; the editing helper (`plugin-editor`, author's decision of
    2026-09-25) makes one and shows its window. The two must start from the
    SAME place - the voice's width on the main buses, the same preparation,
    the set entry's preset applied the same way - or a cue edited in the
    window would begin from values no voice ever had. So the making is here,
    once, and both call it.

    Message thread only: a VST3 is made and takes its state there.
*/

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <string>

namespace wfg::plugin
{
    /*  THE BUSES A PLUGIN TOOK, as an insert on a voice `channels` wide
        (2026-09-26). The main input and output widths are what LaneMapping.h
        decides by; the words are what the entry says on screen. */
    struct InsertLayout
    {
        int inputs = 0;
        int outputs = 0;
        std::string words;
    };

    /*  THE LADDER, asked of the plugin in order until it agrees - never the
        old answer, every bus switched on without a word:

          the voice's own width, in and out
          two in, two out
          one in, two out
          one in, one out

        (a mono voice asks one in one out first, then one in two out, then
        two in two out). Each width is asked in the standard layout for its
        count and then as plain numbered channels - an LV2 whose ports name
        no speaker describes itself so. Each rung is tried with every bus but the main ones
        switched off, and then with the others left as the plugin has them -
        fed silence, since only the main inputs are ever written. False with
        the sentence the entry reads when no rung is taken. */
    bool chooseLayout (juce::AudioProcessor&, int voiceChannels, InsertLayout&, std::string& problem);

    /*  One instance, laid out by the ladder above, prepared, and the preset
        applied (a .vstpreset to the VST3 client, whose loader is the SDK's
        own; any other file the JUCE way). Null with a sentence when it will
        not come up - including one that takes no audio in or gives none out,
        which cannot be an insert on a voice. */
    std::unique_ptr<juce::AudioPluginInstance> makeInsertInstance (juce::AudioPluginFormatManager&,
                                                                   const juce::PluginDescription&,
                                                                   int channels, double sampleRate, int blockSize,
                                                                   const juce::File& preset, InsertLayout& layout,
                                                                   std::string& problem);

    /*  THE ONE FORMAT THE PLUGIN IS, registered alone (2026-09-26): a child
        hosting a VST3 has no business standing up an LV2 world, which JUCE
        does by reading every bundle on the default folders the moment the
        format is made. An LV2 is told the bundle folder the scan found it
        in, so one found through `--path` - outside the folders an LV2 world
        reads by itself - can still be made. A format this build does not
        name gets every compiled format, as before. */
    void addFormatFor (juce::AudioPluginFormatManager&, const juce::PluginDescription&,
                       const juce::String& bundle);

    /** The description written beside the region, back into a PluginDescription. */
    bool readDescription (const std::string& path, juce::PluginDescription&, std::string& problem);

    /** The same, and the LV2 bundle folder the scan recorded beside it
        (known.xml's `bundle` attribute) - empty for any other plugin. */
    bool readDescription (const std::string& path, juce::PluginDescription&, juce::String& bundle,
                          std::string& problem);
}
