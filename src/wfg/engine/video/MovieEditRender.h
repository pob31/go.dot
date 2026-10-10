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

#include <wfg/engine/document/MediaEdit.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

/*  THE RENDER OF A MOVIE'S EDIT (namespace draft §55.5, ADU, ADV): a HAP
    movie's sections laid on the file's own frame grid as a new HAP movie in
    the source's codec, size and time base.

    Every frame outside a dissolve is the source's own bytes, copied - the
    reader hands a frame's HAP chunk and the writer appends it verbatim - so an
    edit of a constant-rate movie loses nothing. A DISSOLVE is the picture's
    crossfade: linear, centred on the join, as wide as the crossfade into the
    incoming section, the outgoing picture going on past its out point and the
    incoming beginning before its in point - material from beyond the edges,
    as the sound's crossfade takes it - and black beyond the file (clear where
    the codec carries alpha). A join still one in the file plays plain. Only
    dissolve frames are decoded, blended in straight RGBA and encoded again.

    Output frame k is judged at its centre, (k + 1/2) of a frame; a source
    second picks the frame showing then, so a variable-rate source lands on
    its dominant grid with some frames picked twice or skipped, which the
    result says. Written to `<target>.part` and moved into place at the end;
    `stop` raised between two frames leaves no file behind.
*/
namespace wfg::video::movie
{
    struct MovieRenderResult
    {
        bool ok = false;
        double seconds = 0.0;       ///< the render's length: its frames on the grid
        int frames = 0;
        double frameRate = 0.0;     ///< the grid, frames a second
        bool resampled = false;     ///< a variable-rate source laid on its dominant grid
        std::string problem;        ///< why not, when not ok
    };

    MovieRenderResult renderMovieEdit (const std::string& sourcePath, const std::vector<doc::Section>& sections,
                                       const std::string& targetPath, const std::atomic<bool>* stop = nullptr,
                                       const std::function<void (int)>& progress = {});

    /** The words a movie that is not HAP is refused with, here and at the verbs (ADX). */
    inline constexpr const char* convertFirst = "convert the movie to HAP first (Show > Convert the movie to HAP)";
}
