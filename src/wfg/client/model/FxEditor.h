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
    WHAT A PLUGIN'S OWN WINDOW IS ABOUT, read off the tree (the author's
    decision of 2026-09-25: Edit... opens the plugin's native window, in a
    helper process, and it follows the pick).

    THE SUBJECT is the picked cue as that window should see it: its title in
    words, whether the window is greyed and the sentence that says why, and
    the cue's value for every parameter - or "rests at the preset" for one
    the cue does not mention, since a cue's values are sparse (§17.4) and a
    parameter nobody set is where the set entry's preset left it.

    GREYED IS NOT BROKEN: nothing picked, a cue that plays no file, or a cue
    that does not have this insert switched in. Each is a different sentence
    because each has a different answer.

    THE LAUNCH is what starting a helper needs that the tree carries: the
    entry's identifier and name, its preset as a path in the open bundle, and
    the audio's rate, block and width, so the helper's copy of the plugin is
    prepared as a voice's is.

    std only, one snapshot door as every model file.
*/

#include <string>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    struct EditorSubject
    {
        std::string cueId;
        std::string fxId;               ///< empty when greyed
        std::string title;
        std::string reason;             ///< why it is greyed; otherwise a line about the insert
        bool greyed = true;

        /** One per parameter: 0..1, or -1 where the cue does not say. */
        std::vector<float> values;

        /** The same cue and the same words: only the values may differ. */
        bool sameSubjectAs (const EditorSubject& other) const
        {
            return cueId == other.cueId && fxId == other.fxId && title == other.title
                && reason == other.reason && greyed == other.greyed;
        }
    };

    EditorSubject readEditorSubject (const tree::TreeSnapshot&, const std::string& pickedCueId,
                                     const std::string& pluginId);

    struct EditorStart
    {
        std::string pluginId;
        std::string identifier;         ///< empty when the set has no such entry
        std::string name;
        std::string presetPath;         ///< absolute, in the open bundle; empty for none

        int sampleRate = 48000;
        int blockSize = 512;
        int channels = 2;
    };

    EditorStart readEditorStart (const tree::TreeSnapshot&, const std::string& pluginId);
}
