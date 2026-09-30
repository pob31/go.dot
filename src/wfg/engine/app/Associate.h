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
    LINUX: .wfg SHOWS OPEN WITH THIS COPY OF GO.DOT (author, 2026-09-30: a menu
    item and a command, per user). `wfg associate` and the window's "Open .wfg
    files with this Go.dot" both come here; Windows has its installer and the
    Mac its app, and neither needs this.

    A tarball cannot tell a desktop about itself, so this writes, for the user
    and nobody else, under $XDG_DATA_HOME (~/.local/share):

      mime/packages/go.dot.xml       *.wfg is application/x-go.dot-show
      applications/go.dot.desktop    Go.dot, opening that type with go.dot.sh
      icons/hicolor/NxN/apps/go.dot.png                        the app
      icons/hicolor/NxN/mimetypes/application-x-go.dot-show.png  the page

    and asks the desktop's own tools to read them again, where they are
    installed. The desktop entry names the window's class (StartupWMClass =
    Go.dot, which a windowed serve sets: app/WindowApplication.h), so the dock
    shows the app icon for the window too.

    IT POINTS AT WHERE THIS COPY IS. Move the folder and it points at nothing
    until it is run again from the new place - said in README.txt.
*/

#include <juce_core/juce_core.h>

#include <string>

namespace wfg::app
{
    struct AssociateResult
    {
        bool ok = false;
        std::string said;      ///< a sentence for the person who asked
    };

    /*  Writes the four kinds of file above, or with `remove` takes them away.
        `program` is the folder wfg and go.dot.sh are in, with the PNGs in its
        icons/; `dataHome` is where they go. Plain files, so it can be tried
        on any system into a folder of a test's own; the desktop's tools are
        only asked on Linux. */
    AssociateResult associate (bool remove, const juce::File& program, const juce::File& dataHome);

    /*  $XDG_DATA_HOME, or ~/.local/share when that is not set. */
    juce::File defaultDataHome();
}
