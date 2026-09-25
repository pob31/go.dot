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
    /*  One instance, laid out to `channels` on its main buses, prepared, and
        the preset applied (a .vstpreset to the VST3 client, whose loader is
        the SDK's own; any other file the JUCE way). Null with a sentence when
        it will not come up - including one that takes no audio in or gives
        none out, which cannot be an insert on a voice. */
    std::unique_ptr<juce::AudioPluginInstance> makeInsertInstance (juce::AudioPluginFormatManager&,
                                                                   const juce::PluginDescription&,
                                                                   int channels, double sampleRate, int blockSize,
                                                                   const juce::File& preset, std::string& problem);

    /** The description written beside the region, back into a PluginDescription. */
    bool readDescription (const std::string& path, juce::PluginDescription&, std::string& problem);
}
