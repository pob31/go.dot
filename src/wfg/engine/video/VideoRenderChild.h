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
    THE PICTURES, DRAWN IN A PROCESS OF THEIR OWN (Phase 8a, namespace draft
    35.4, decision UV).

        wfg video-render --region=<path> --parent-pid=P [--no-window]

    WHY ANOTHER PROCESS: a graphics driver's fault is the one kind of crash a
    show program cannot prevent, and it must take away a picture for a second,
    never the sound or the GO button. So the engine writes the scene into the
    region and this program draws it - one fullscreen window per output, on
    the display the show names, never taking the focus and never showing a
    pointer. Started by the engine, watched by it, started again if it stops
    answering; it reads the region afresh and draws again what was up (VC).

    EVERY FRAME IS DRAWN FOR THE SAMPLE IT WILL BE SEEN AT: the region's clock
    pairs make an estimate of where Go.dot's samples are (VideoClock.h), and a
    layer's opacity is read off its points there - never stepped at fifty
    hertz, never on the window's vsync alone (PRD §3.19d).

    OpenGL, through JUCE (UX): the window is the output sink of V.1; the
    offscreen canvas a mesh warps arrives with the mesh (V.5), and a DeckLink
    card is another sink.

    `--no-window` is everything but the windows, for a machine with no screen:
    the same reading of the region at a clock, through the CPU compositor, the
    colour at the middle of each canvas published back - how CI looks at a
    picture.

    NAMES NO JUCE TYPE.
*/

#include <string>
#include <vector>

namespace wfg::video
{
    /** Runs the renderer to completion. `args` are the words after `video-render`. */
    int runVideoRender (const std::vector<std::string>& args);

    /*  If argv[1] is `video-render`, runs it, stores the exit code and answers
        true; false for an ordinary run. */
    bool runVideoRenderIfAsked (int argc, char** argv, int& exitCode);

    /** The verb, spelled once. */
    inline constexpr const char* videoRenderVerb = "video-render";
}
