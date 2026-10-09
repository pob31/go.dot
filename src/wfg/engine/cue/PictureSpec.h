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
    WHAT A VIDEO CUE PUTS ON ITS CANVAS, read from the document (namespace draft
    §35, §36, §44; §47, AAE).

    One reading, used three ways: at GO, where the Runner brings a layer up; on
    every tick a playing cue's picture rows change, where the layer is told what
    it is now (the author, 2026-10-09: a playing cue's picture follows its edits
    at once); and for the video monitor's tile of the picked cue (§47, AAH),
    which shows a cue that is not playing as it would look.

    Everything is read through the document's own getter, which gives a row's
    default where the file leaves it out, and parsed once here, so the far side
    never reads document text (VM). The run's own parts - its identifier, its
    order among layers, its opacity and its playhead - are the caller's.
*/

#include <wfg/engine/video/VideoSink.h>

#include <string>

namespace wfg::doc { class ShowDocument; }

namespace wfg::cue
{
    /*  The layer `cueId` would put up now, its file resolved against
        `mediaFolder` as a sound's is. An empty spec for a cue that is not
        there. */
    video::LayerSpec pictureSpecOf (const doc::ShowDocument& document, const std::string& cueId,
                                    const std::string& mediaFolder);

    /*  WHETHER TWO SPECS LOOK THE SAME, in what a playing layer is told when its
        cue is edited (`Sink::restate`): the blend, the colour, the fit, the
        geometry's own numbers and flips, the grade with its curves, the mask.
        Numbers compared bit for bit: "changed" means somebody wrote it. */
    bool sameLook (const video::LayerSpec& a, const video::LayerSpec& b) noexcept;

    /*  The look of `from` written over `into`, the rest of `into` - which
        canvas, which layer, which file, which run - left as it was. */
    void takeLook (video::LayerSpec& into, const video::LayerSpec& from) noexcept;
}
