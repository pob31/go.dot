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
    A show is open in at most one window (app/OpenShows.h).

    What one process can prove about itself: a claim is exclusive, it names
    this process where the raise will look, and letting go gives the show and
    the file back. Two processes on one show are the window's to try - `wfg
    serve X --window` twice - since a lock that only one process ever took
    proves nothing about the second.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/app/OpenShows.h>
#include <wfg/engine/app/Raise.h>

#include <juce_core/juce_core.h>

using wfg::app::OpenShow;

namespace
{
    //  A folder of this case's own, gone afterwards.
    struct Scratch
    {
        Scratch()
            : folder (juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("wfg-open-shows-" + juce::Uuid().toDashedString()))
        {
            folder.createDirectory();
        }

        ~Scratch() { folder.deleteRecursively(); }

        juce::File folder;
    };
}

TEST_CASE ("open shows: a claim is this process's, named in its file, and one at a time")
{
    Scratch scratch;
    const auto show = scratch.folder.getChildFile ("Tuesday");
    const auto entries = scratch.folder.getChildFile ("open");
    REQUIRE (show.createDirectory());

    auto first = OpenShow::claim (show, entries);
    REQUIRE (first != nullptr);

    //  The file the raise reads names this process, and nothing else.
    const auto entry = OpenShow::entryFor (show, entries);
    CHECK (entry.loadFileAsString().trim() == juce::String (wfg::app::currentProcessId()));

    //  A second claim fails while the first holds - in this process too.
    CHECK (OpenShow::claim (show, entries) == nullptr);

    //  This process's own show is not held ELSEWHERE, and asking must not let it go.
    CHECK_FALSE (OpenShow::heldElsewhere (show));
    CHECK (OpenShow::claim (show, entries) == nullptr);

    first.reset();

    //  Let go: the file is gone and the show can be claimed again.
    CHECK_FALSE (entry.exists());
    auto again = OpenShow::claim (show, entries);
    CHECK (again != nullptr);
}

TEST_CASE ("open shows: two spellings of one folder are one show, two folders are two")
{
    Scratch scratch;
    const auto entries = scratch.folder.getChildFile ("open");
    const auto one = scratch.folder.getChildFile ("One");
    const auto two = scratch.folder.getChildFile ("Two");
    REQUIRE (one.createDirectory());
    REQUIRE (two.createDirectory());

    //  A trailing separator is the same folder.
    CHECK (OpenShow::entryFor (one, entries)
             == OpenShow::entryFor (juce::File (one.getFullPathName() + juce::File::getSeparatorString()), entries));

   #if JUCE_WINDOWS || JUCE_MAC
    //  Where the file system ignores case, so does the claim.
    CHECK (OpenShow::entryFor (one, entries)
             == OpenShow::entryFor (juce::File (one.getFullPathName().toUpperCase()), entries));
   #endif

    CHECK (OpenShow::entryFor (one, entries) != OpenShow::entryFor (two, entries));

    auto holdingOne = OpenShow::claim (one, entries);
    auto holdingTwo = OpenShow::claim (two, entries);
    CHECK (holdingOne != nullptr);
    CHECK (holdingTwo != nullptr);
}

TEST_CASE ("open shows: a show nobody holds is not held, and has no window to raise")
{
    Scratch scratch;
    const auto show = scratch.folder.getChildFile ("Nobody");
    const auto entries = scratch.folder.getChildFile ("open");
    REQUIRE (show.createDirectory());

    CHECK_FALSE (OpenShow::heldElsewhere (show));
    CHECK_FALSE (OpenShow::raiseHolder (show, entries));

    //  A stale file names nobody the raise should trust.
    REQUIRE (entries.createDirectory());
    REQUIRE (OpenShow::entryFor (show, entries).replaceWithText ("not a process"));
    CHECK_FALSE (OpenShow::raiseHolder (show, entries));

    //  And the probe let go: the show can still be claimed.
    CHECK (OpenShow::claim (show, entries) != nullptr);
}
