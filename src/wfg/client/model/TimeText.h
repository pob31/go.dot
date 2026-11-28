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
    HOW A TIME IS WRITTEN ON SCREEN (namespace draft §151, BKM).

    The author, 2026-11-28: "Could times be either in seconds or in hours,
    minutes and seconds (toggle in the show parameters)". The show's row
    `/godot/list/timeFormat` says which: `seconds`, the look every panel had -
    `83.5` in the cue list, `1:04.2` in an editor past a minute - or `clock`,
    minutes and seconds with the hours only past an hour, `1:23.5` and
    `1:02:03.5`, tenths kept (his picks). Every time on screen asks here.

    ONE FORMAT FOR THE PROCESS: a Go.dot window is one show, and each show is
    its own process (§106), so the window's refresh writes what its snapshot
    says and every painter reads it - the waveform's ruler and the take panel
    alike, none of which holds a snapshot of its own.

    Written by hand, never through a stream or printf: under fr_FR either would
    put a comma where the page puts a point.
*/

#include <optional>
#include <string>

namespace wfg::tree
{
    class TreeSnapshot;
}

namespace wfg::client::model
{
    enum class TimeFormat
    {
        seconds,
        clock
    };

    /*  The format the window's show asks for, written by its refresh. */
    void setShownTimeFormat (TimeFormat format) noexcept;
    TimeFormat shownTimeFormat() noexcept;
    bool clockShown() noexcept;

    /*  What the snapshot says: `clock`, or seconds for anything else. */
    TimeFormat timeFormatOf (const tree::TreeSnapshot& snapshot);

    /*  `83.5`: seconds to a tenth, rounded half away from nought. */
    std::string tenthsReading (double seconds);

    /*  `1:23.5`, `1:02:03.5`, `-0:04.0`: minutes and seconds, the hours past an
        hour, `decimals` places (nought for none); `trim` takes off the
        trailing noughts and a point left alone, as an editor's millisecond
        reading does (`1:04.25`, `0:07`). */
    std::string clockTime (double seconds, int decimals = 1, bool trim = false);

    /*  A reading to a tenth in the shown format: `tenthsReading` or `clockTime`. */
    std::string readingTime (double seconds);

    /*  A time as somebody types it, in either format whatever is shown: `83.5`,
        `83,5`, `1:23.5`, `1:02:03.5` - or with its units, `1h34m3s1` (1:34:03.1),
        `2m30`, `90s` (§151, BKQ). Nothing for what is not a time, for a
        negative one, and for sixty or more in a place below the first ("1:75"
        is a slip, not a minute and a quarter). */
    std::optional<double> typedTime (const std::string& typed);

    /*  Whether `typed` is written as a clock: it has a colon, or a unit. */
    bool typedAsClock (const std::string& typed);
}
