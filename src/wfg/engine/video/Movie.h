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
    A MOVIE FILE'S FRAMES: where each is, how large, and when it starts
    (Phase 8b, namespace draft 37, VY).

    A QuickTime `.mov`, read as far as a video track needs and no further:
    the `moov` box's first video track - its time scale (`mdhd`), its codec
    and size (`stsd`), and its sample tables: how long each frame lasts
    (`stts`), how frames sit in chunks (`stsc`), how large each is (`stsz`),
    and where each chunk is (`stco`, or `co64` for a file past four gigabytes).
    Go.dot's own, from the published file format; the codec is HAP's to say
    (Hap.h), and a file of another codec reads as one Go.dot does not play.

    `parse` is pure, over the bytes of the `moov` box; `MovieFile` finds that
    box in a file and reads frames - the renderer's, on a thread of its own,
    and the engine's, once, for a movie's length.
*/

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wfg::video::movie
{
    struct Frame
    {
        std::uint64_t offset = 0;
        std::uint32_t size = 0;
        double start = 0.0;     ///< seconds from the file's start
    };

    struct Info
    {
        std::string codec;      ///< the sample entry's four characters: Hap1, Hap5, HapY...
        int width = 0;
        int height = 0;
        double duration = 0.0;  ///< seconds: the last frame's end
        std::vector<Frame> frames;

        bool isHap() const noexcept  { return codec == "Hap1" || codec == "Hap5" || codec == "HapY"; }

        /*  The frame showing at `seconds`: the last to start at or before it,
            the first before the start, the last after the end. -1 with none. */
        int frameAt (double seconds) const noexcept;

        double frameRate() const noexcept
        {
            return frames.empty() || ! (duration > 0.0) ? 0.0 : static_cast<double> (frames.size()) / duration;
        }
    };

    /*  The `moov` box's contents - after its own header - to the first video
        track's frames. False, with `why` said, for a movie that will not read. */
    bool parse (const std::uint8_t* moov, std::size_t size, Info& out, std::string& why);

    /*  A FILE OPEN FOR READING FRAMES. Not shared between threads: each reader
        has its own. */
    class MovieFile
    {
    public:
        MovieFile();
        ~MovieFile();

        MovieFile (const MovieFile&) = delete;
        MovieFile& operator= (const MovieFile&) = delete;

        bool open (const std::string& path, std::string& why);
        const Info& info() const noexcept  { return details; }

        /** Frame `index`'s bytes into `out`. False past the end, or on a read that fails. */
        bool readFrame (int index, std::vector<std::uint8_t>& out);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
        Info details;
    };

    /*  A movie's length in seconds, its file opened and closed: the engine's
        question, asked off the tick thread. -1 for a file that will not read. */
    double durationOf (const std::string& path);
}
