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
    A SHOW FOLDER LOOKS LIKE ONE: the slate folder with the stone and the next
    point (packaging/icons/folder.svg), set on each show when it is saved
    (author, 2026-09-30).

    A show is an ordinary folder, and neither Windows nor macOS gives a kind of
    folder an icon of its own - only a folder that carries one. So each show
    carries one:

      - Windows: a hidden `desktop.ini` naming a hidden `.go.dot-folder.ico`
        beside it, both inside the show, and the folder marked read-only -
        the flag Explorer looks for before it reads a desktop.ini. The path is
        relative, so the icon goes where the folder goes.
      - macOS: the Finder's own custom icon, which NSWorkspace writes into the
        folder as its hidden `Icon\r`.
      - Linux: nothing. Its file managers keep a folder's icon outside it.

    PRESENTATION, NOT THE DOCUMENT (PRD §4.10). Nothing here is read by `open`,
    `validate` or a replay, and a show without it is exactly as good. Which is
    why only the windowed serve does it - after a save or a copy lands, and
    when New makes a show - and never a headless serve, a replay or a test.
    Git and some zip tools drop hidden files and attributes; a show fetched
    that way is a plain folder until its next save.

    A desktop.ini somebody else wrote is left alone: it is theirs.
*/

#include <juce_core/juce_core.h>

namespace wfg::app
{
    /*  Gives `folder` the show icon, when it has not got it. On whichever
        thread asks; the Mac's hops to the main thread itself. */
    void markShowFolder (const juce::File& folder);

    /*  Whether `folder` already carries it - the Windows half of the answer,
        for the tests; the Mac's is the Finder's. */
    bool isMarkedShowFolder (const juce::File& folder);
}
