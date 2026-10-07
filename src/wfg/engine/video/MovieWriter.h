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
    A QUICKTIME MOVIE, WRITTEN (namespace draft 37.6, F.2): the file Go.dot's
    own HAP encoder makes (WK), and the reverse of Movie.h's reader.

    STREAMED: `ftyp`, then an `mdat` with a 64-bit size the frames are
    appended to as they come, then at `finish` the size patched and the
    `moov` written after - one video track, every frame its own chunk at a
    64-bit offset (`co64`), so a 4K movie of an hour is a file like any
    other. The boxes are the ones QuickTime, FFmpeg and every HAP player
    read: `mvhd`, `tkhd`, `mdhd`, the handlers, `vmhd`, `dinf`, and the
    sample tables.

    Its frame rate is a fraction - 30000/1001 is 30000 time units a second,
    each frame 1001 of them - so NTSC rates stay exact over an hour.
*/

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace wfg::video::movie
{
    class MovieWriter
    {
    public:
        MovieWriter();
        ~MovieWriter();

        MovieWriter (const MovieWriter&) = delete;
        MovieWriter& operator= (const MovieWriter&) = delete;

        /*  A new file at `path`, replacing one there. `codec` is the sample
            entry's four characters - Hap1, Hap5 or HapY. */
        bool open (const std::string& path, const char* codec, int width, int height,
                   std::uint32_t timeScale, std::uint32_t frameDuration, std::string& why);

        /** One frame's bytes, appended. */
        bool write (const std::uint8_t* data, std::size_t size);

        /*  The index written and the file closed. A writer not finished
            leaves a file no reader takes, which the conversion deletes. */
        bool finish (std::string& why);

        std::size_t frames() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
