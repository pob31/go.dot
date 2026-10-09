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
    A HAP MOVIE'S STRIP, READ FROM ITS FILE (namespace draft §47, AAI).

    Every few frames - seven or eight a second - is read and decoded on the
    CPU, but only at the points a signature samples (Strip.h): two thousand
    and some of a frame's millions, which is what makes a ten-minute movie a
    few seconds of one core rather than minutes. A cut found between two
    frames read is placed on its first frame by halving the frames between;
    the pictures are taken where `thumbnailTimes` says.

    READ GENTLY: a show plays movies off the same disk, so reading is held
    below 150 megabytes a second, and `stopping` ends it between two frames.

    A movie that is not HAP has no strip until it is: adding one to a show
    converts it (37.5, WF).
*/

#include <wfg/engine/video/Strip.h>

#include <atomic>
#include <string>

namespace wfg::video::strip
{
    bool analyseHap (const std::string& path, MovieStrip& out, std::string& why,
                     const std::atomic<bool>* stopping = nullptr);
}
