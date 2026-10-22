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
    THE SHOWS THIS MACHINE OPENED LAST (namespace draft §106, the author,
    2026-10-22: "In the file menu can we add the 10 most recent wfg show
    files?"). File > Open recent.

    One show folder a line, the newest first, in `recent-shows.txt` beside
    `last-show.txt` - a fact about this computer, never about a show (PRD
    §4.10) - and written where that one is, by a window started with
    `--remember`, so a driver's window never enters the author's list.

    A SHOW NOT FOUND STAYS LISTED (BAK): it may be on a drive not plugged in
    tonight. The menu greys it, and Clear the list is the way to forget.

    Two spellings of one folder are one show, as OpenShows.h folds them.
*/

#include <juce_core/juce_core.h>

#include <cstddef>
#include <vector>

namespace wfg::app::recent
{
    /*  How many the list keeps. */
    constexpr std::size_t kept = 10;

    /*  The list in `file`, newest first: absolute paths only, one a show, at
        most `kept`. Empty when there is no file. */
    std::vector<juce::File> read (const juce::File& file);

    /*  `show` put first, any other spelling of it taken out, the oldest past
        `kept` let go. False when the file could not be written. */
    bool remember (const juce::File& file, const juce::File& show);

    /*  The list emptied: the file goes. */
    bool forget (const juce::File& file);
}
