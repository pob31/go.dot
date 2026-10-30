// This file is part of Go.dot — https://github.com/pob31/go.dot
//
// Copyright (C) 2026 Pierre-Olivier Boulant
//
// Go.dot is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. Go.dot is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
// (LICENSE, at the repository root) for more details.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/*  THE ANGLES FRAMING (namespace draft §124, BER): a Shure receiver's command
    strings, each from `<` to `>` and nothing ending a line -

        < REP 1 AUDIO_GAIN 030 >< REP 1 CHAN_NAME {Lead Vox} >

    - so a frame ends at the `>` OUTSIDE BRACES, since a channel's name may
    hold one (the receiver takes `<=>` in a name). Bytes before a `<` are
    dropped; a `<` outside braces inside a frame is the next one starting,
    the one before cut short and dropped; a frame longer than `longest` is
    dropped whole. Each drop is counted. */

#include <cstddef>
#include <string>
#include <vector>

namespace wfg::serial
{
    struct AngleCutter
    {
        std::string partial;
        bool inFrame = false;
        bool inBraces = false;
        std::size_t dropped = 0;

        /*  The frames the bytes complete, `<` and `>` included, oldest first. */
        std::vector<std::string> feed (const std::string& bytes, std::size_t longest = 4096);
    };
}
