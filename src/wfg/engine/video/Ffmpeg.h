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
    FFMPEG, FOUND AND ASKED (namespace draft 37.5 WF-WK, 37.6 F.1).

    The author's: a movie in a codec that is not HAP is a preview, and adding
    it converts it to HAP in the background, FFmpeg run as a child process
    (WG). FFmpeg only READS here (WK): it says what a file holds, decodes its
    frames and takes out its sound; Go.dot writes the HAP.

    FOUND, in this order: where `WFG_FFMPEG` says - the folder holding
    `ffmpeg` and `ffprobe`, or the `ffmpeg` file itself; beside the running
    `wfg`, or in an `ffmpeg` folder beside it, which is where a package puts
    them (F.7); on the path.

    ASKED with `ffprobe`'s JSON, parsed here by `parseProbe`, which is pure.
*/

#include <string>

namespace wfg::video::ffmpeg
{
    struct Tools
    {
        std::string ffmpeg;     ///< full paths, empty when not found
        std::string ffprobe;

        bool found() const noexcept  { return ! ffmpeg.empty() && ! ffprobe.empty(); }
    };

    /*  Looked for each time it is asked: a program installed while Go.dot
        runs is found on the next question. */
    Tools find();

    /*  WHAT A FILE HOLDS: its first video stream and whether it has sound. */
    struct Probe
    {
        bool ok = false;
        std::string why;            ///< what went wrong, when not ok

        std::string codec;          ///< FFmpeg's name: h264, prores, hap...
        int width = 0;
        int height = 0;
        double frameRate = 0.0;     ///< frames a second, the average
        double duration = 0.0;      ///< seconds
        bool alpha = false;         ///< its pixels carry transparency
        bool sound = false;
        int soundChannels = 0;
        int soundRate = 0;

        bool isHap() const noexcept  { return codec == "hap"; }
    };

    /*  `ffprobe -show_streams -show_format -of json`'s answer, read. */
    Probe parseProbe (const std::string& json);

    /*  Asked of the file, waiting at most `milliseconds` for an answer. */
    Probe probe (const Tools& tools, const std::string& path, int milliseconds = 15000);

    /*  Whether FFmpeg's pixel format carries alpha: yuva420p, rgba, gbrap... */
    bool pixelFormatHasAlpha (const std::string& format);
}
