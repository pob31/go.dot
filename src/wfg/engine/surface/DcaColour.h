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
    WHAT A DCA STRIP'S RING SHOWS (namespace draft §38, WR, the author's pick):
    what the DCA rides, as one colour. The author asked for "the same RGB
    spectral view as on the sampler channels" on a DCA's rotary, and for a
    picture "the opacity ... or the average tint".

    THE MEMBERS are what the DCA's trim reaches, read off the tree: a run whose
    cue is marked with the DCA - or with a DCA inside it, or under a group so
    marked - and a sounding run that plays through an output the DCA rides, by
    its direct out or a send; and each canvas the DCA rides, whole. Only runs
    playing or stopping count: a DCA with nothing up is dark, as today.

    THE COLOUR blends them, each at full brightness:
      * a sound by its timbre where it has got to (§3.30), weighted by how loud
        it left its track (`run/meter`, after its fader);
      * a picture or a canvas by its tint (`run/tint`, `canvas/tint`), weighted
        by how bright that tint is - its opacity and its DCA are in it.
    WHILE ANYTHING SOUNDS, the light moves with the loudest member's envelope,
    as a sampler strip's does. With pictures only, it is as bright as the
    brightest of them, never under a tenth - and a picture up but see-through
    shows a tenth of white, so the rotary stays visible (the author's words).

    A pure function of the snapshot, for the bridge and for the window's
    virtual panel alike, every DCA at once: one look at each run, not one per
    strip.
*/

#include <wfg/engine/surface/SurfaceBridge.h>

#include <map>
#include <optional>
#include <string>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::surface
{
    struct DcaLight
    {
        /** The blend at full brightness, 0..127; nothing when nothing of the DCA's is up. */
        std::optional<Rgb> colour;

        /** The loudest sounding member's run, whose envelope moves the light; empty when nothing sounds. */
        std::string loudestRun;

        /** How bright the pictures are, 0.1..1, when nothing sounds. */
        double pictureLight = 0.0;
    };

    /** Every DCA the show declares that has something up, by identifier. */
    std::map<std::string, DcaLight> dcaLights (const tree::TreeSnapshot& snapshot);

    /*  The light as it is shown when nothing sounds: the colour at its picture
        brightness. With a sounding member the caller moves it by the envelope. */
    Rgb restingLight (const DcaLight& light) noexcept;
}
