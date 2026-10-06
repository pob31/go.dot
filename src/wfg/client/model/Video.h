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
    The canvases this show declares and the video outputs that show them
    (Phase 8a, namespace draft 35), in the author's words: "Canvas (surfaces in
    QLab lingo) render to Outputs with their mapping."

    A canvas is the show's: a name and a size. An output is the show's too -
    its name, the canvas it shows, the display it opens on - and beside that,
    what the machine found tonight: whether a display is behind it, why not,
    and how its frames are going. Read off the tree and never worked out here.
*/

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    struct CanvasRow
    {
        std::string id;
        std::string name;
        int width = 1920;
        int height = 1080;

        std::string label() const;   // the name, else the id
        std::string sizeWord() const;   // "1920 × 1080"
    };

    struct VideoOutputRow
    {
        std::string id;
        std::string name;
        std::string canvas;
        std::string display;
        bool enabled = true;
        bool bound = false;
        std::string problem;
        bool testPattern = false;
        std::int64_t framesPresented = 0;
        std::int64_t framesLate = 0;

        std::string label() const;
    };

    /** Every canvas, in /godot/canvas/order. */
    std::vector<CanvasRow> readCanvases (const tree::TreeSnapshot&);

    /** Every video output, in /godot/videoOutput/order. */
    std::vector<VideoOutputRow> readVideoOutputs (const tree::TreeSnapshot&);

    /** The displays this machine has, by the names the system gives them. */
    std::vector<std::string> readDisplays (const tree::TreeSnapshot&);

    /*  A cue's or an output's canvas as a menu: "(none)" first, which shows
        nothing, then every canvas by name with its size. The key is the
        identifier, so a canvas renamed leaves every cue on it where it was. */
    std::vector<std::pair<std::string, std::string>> canvasChoices (const std::vector<CanvasRow>&);

    /*  WHETHER A FILE IS A PICTURE the renderer reads (namespace draft 36): by
        its extension, the ones JUCE's image readers take - PNG, JPEG, GIF.
        What makes a dropped file a picture cue rather than a sound. */
    bool isPictureFile (const std::string& name);

    /*  The wildcard a file chooser offers for a picture. */
    const char* pictureWildcard();
}
