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
    WHAT A CUE'S ROW SAYS ABOUT HOW READY IT IS (PRD §3.12; namespace draft §48,
    AAR): the word published as the cue's `prepare`, and - when getting it ready
    failed - why, as its `prepareError`.

    Two sources, one vocabulary. A sound has a run the standby armed, and the
    word is that run's - except that `armed` now waits for the audio side to say
    the voice is real (`audio.armed`), and is `preparing` until it does. A
    picture or a movie has no run (AAN): its word is made from what the engine
    named to read ahead and what the renderer says it holds of it.

    Pure functions of what they are handed, so the rules are tested without a
    tick, a player or a renderer.
*/

#include <wfg/engine/cue/Run.h>
#include <wfg/engine/video/VideoRegion.h>

#include <string>

namespace wfg::cue
{
    struct PrepareWord
    {
        std::string word;           ///< empty is idle
        std::string error {};       ///< a run's error word, when getting it ready failed
    };

    /*  A RUN'S: its own `prepare`, but `preparing` for a sound whose voice the
        audio side has not confirmed yet - and `partial`, with its error, for a
        prepared run that failed (the caller decides whether that run still
        speaks for its cue). */
    PrepareWord runWordOf (const Run& run);

    /*  A PICTURE'S OR A MOVIE'S, read ahead without a run: `partial` and
        `media-missing` for a file the show does not have, or one the renderer
        could not read; idle while no renderer runs, or when it was not named
        (past the region's room); `preparing` while it is read, or while the
        renderer's answer is to an older list; `armed` once it is held ready. */
    PrepareWord videoWordOf (bool missing, const std::string& renderer, bool heldCurrent,
                             const video::region::HeldReading* held);
}
