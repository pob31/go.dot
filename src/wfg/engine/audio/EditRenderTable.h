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

#include <map>
#include <string>

/*  THE RENDERS OF THE OPEN EDITS (namespace draft §55, ADM): what the
    renderer has made, is making or could not make of each sound cue's
    sections, published as a snapshot the tick thread holds for a tick - the
    durations' pattern - and read by the one resolver of what a cue plays.

    A type alone, with no thread in it, so the Runner and its tests can be
    handed a table of their own, as they are handed a map of lengths.
*/
namespace wfg::audio
{
    namespace renderState
    {
        inline constexpr const char* rendering = "rendering";
        inline constexpr const char* done      = "done";
        inline constexpr const char* failed    = "failed";
    }

    struct EditRender
    {
        std::string cue;        // the sound cue's identifier
        std::string editText;   // the edit this render is of, as doc::editText spells it
        std::string key;        // the render's key, which names its file
        std::string file;       // the render's name, relative to the media folder (".edits/<key>.wav")
        std::string state;      // renderState's words
        std::string problem;    // why it failed, in words
        int percent = 0;
        double seconds = 0.0;   // the render's length, the sections put together
    };

    /** By the cue's identifier. */
    using EditRenders = std::map<std::string, EditRender>;
}
