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

/*
    Linux's .wfg association, as files (app/Associate.h).

    What a desktop makes of them is the desktop's, and needs one; that the
    right files say the right things in the right places does not, so it runs
    everywhere, into folders of the case's own.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/app/Associate.h>

#include <juce_core/juce_core.h>

namespace
{
    struct Scratch
    {
        Scratch()
            : folder (juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("wfg-associate-" + juce::Uuid().toDashedString()))
        {
            folder.createDirectory();
        }

        ~Scratch() { folder.deleteRecursively(); }

        juce::File folder;
    };

    //  A test build's folder as the tarball lays it out: go.dot.sh and two sizes of each icon.
    juce::File tarball (const juce::File& where)
    {
        where.getChildFile ("icons").createDirectory();
        where.getChildFile ("go.dot.sh").replaceWithText ("#!/bin/sh\n");

        for (const auto* name : { "go.dot-16.png", "go.dot-256.png", "go.dot-document-16.png", "go.dot-document-256.png" })
            where.getChildFile ("icons").getChildFile (name).replaceWithText (name);

        return where;
    }
}

TEST_CASE ("associate: the type, the entry and the icons are written, and taken away again")
{
    Scratch scratch;
    const auto program = tarball (scratch.folder.getChildFile ("go.dot-0.1.0-linux-x86_64"));
    const auto home = scratch.folder.getChildFile ("share");

    const auto made = wfg::app::associate (false, program, home);
    CHECK (made.ok);

    const auto mime = home.getChildFile ("mime/packages/go.dot.xml");
    const auto desktop = home.getChildFile ("applications/go.dot.desktop");
    REQUIRE (mime.existsAsFile());
    REQUIRE (desktop.existsAsFile());

    CHECK (mime.loadFileAsString().contains ("<glob pattern=\"*.wfg\"/>"));
    CHECK (mime.loadFileAsString().contains ("type=\"application/x-go.dot-show\""));

    const auto entry = desktop.loadFileAsString();
    CHECK (entry.contains ("MimeType=application/x-go.dot-show;"));
    CHECK (entry.contains ("StartupWMClass=Go.dot"));
    //  The script in this copy's folder, quoted, then the file. (The path's own
    //  separators are the next case's business: a Windows run has backslashes.)
    CHECK (entry.contains ("Exec=\""));
    const auto slash = entry.contains (program.getFileName() + "/go.dot.sh\" %f\n");
    const auto backslash = entry.contains (program.getFileName() + "\\\\\\\\go.dot.sh\" %f\n");   // four in the file
    CHECK ((slash || backslash));
    CHECK_FALSE (entry.contains ("\r"));

    //  Each icon where the theme looks for it, and only the sizes this copy carries.
    const auto hicolor = home.getChildFile ("icons/hicolor");
    CHECK (hicolor.getChildFile ("16x16/apps/go.dot.png").loadFileAsString() == "go.dot-16.png");
    CHECK (hicolor.getChildFile ("256x256/mimetypes/application-x-go.dot-show.png").loadFileAsString()
             == "go.dot-document-256.png");
    CHECK_FALSE (hicolor.getChildFile ("48x48/apps/go.dot.png").exists());

    const auto taken = wfg::app::associate (true, program, home);
    CHECK (taken.ok);
    CHECK_FALSE (mime.exists());
    CHECK_FALSE (desktop.exists());
    CHECK_FALSE (hicolor.getChildFile ("16x16/apps/go.dot.png").exists());
    CHECK_FALSE (hicolor.getChildFile ("256x256/mimetypes/application-x-go.dot-show.png").exists());
}

TEST_CASE ("associate: a folder with no go.dot.sh is refused, and nothing is written")
{
    Scratch scratch;
    const auto program = scratch.folder.getChildFile ("build");
    const auto home = scratch.folder.getChildFile ("share");
    REQUIRE (program.createDirectory());

    const auto result = wfg::app::associate (false, program, home);

    CHECK_FALSE (result.ok);
    CHECK (juce::String (result.said).contains ("go.dot.sh"));
    CHECK_FALSE (home.getChildFile ("applications/go.dot.desktop").exists());
}

TEST_CASE ("associate: a path with spaces, a dollar, a backquote and a percent is one Exec argument")
{
    Scratch scratch;

    //  No double quote: Windows refuses one in a name, and the rule is the dollar's.
    const auto program = tarball (scratch.folder.getChildFile ("my shows $HOME `x` 100%"));
    const auto home = scratch.folder.getChildFile ("share");

    REQUIRE (wfg::app::associate (false, program, home).ok);

    const auto entry = home.getChildFile ("applications/go.dot.desktop").loadFileAsString();

    /*  The Desktop Entry rules: in double quotes, `"`, `` ` `` and `$`
        escaped by a backslash that the file's string escaping doubles, and
        `%` doubled. */
    CHECK (entry.contains ("my shows \\\\$HOME \\\\`x\\\\` 100%%"));
}
