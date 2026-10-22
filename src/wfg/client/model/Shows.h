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
    SHOWS NAMED IN THE FILE MENU (namespace draft §106): Open recent's lines,
    and the sentence Close other shows asks with.

    A show is named by its folder, as the window's title names it. Two shows
    of one name in the list - a show and its copy for another venue, say -
    each say the folder they sit in, so the two lines are never the same words
    (BAL).
*/

#include <string>
#include <vector>

namespace wfg::client::model
{
    /*  A show folder's name, from its full path: the last part, a `.wfg`
        named as its folder is. */
    std::string showName (const std::string& path);

    /*  One line a show, in the order given: its name, and ` - <the folder it
        is in>` when another show in the list has the same name. */
    std::vector<std::string> recentShowLines (const std::vector<std::string>& paths);

    /*  What Close other shows asks (§106): the shows by name, "and" before
        the last, a show whose Go.dot does not say counted as "another show". */
    std::string otherShowsNamed (const std::vector<std::string>& paths);
}
