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
#include <wfg/engine/app/RecentShows.h>

#include <juce_core/juce_core.h>

#include <algorithm>

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

    //  The file the raise reads names this process first, then the show (§106).
    const auto entry = OpenShow::entryFor (show, entries);
    const auto lines = juce::StringArray::fromLines (entry.loadFileAsString());
    CHECK (lines[0] == juce::String (wfg::app::currentProcessId()));
    CHECK (lines[1] == show.getFullPathName());

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

TEST_CASE ("open shows: another window's shows are listed by their locks, never this one's (§106)")
{
    Scratch scratch;
    const auto entries = scratch.folder.getChildFile ("open");
    const auto mine = scratch.folder.getChildFile ("Mine");
    const auto stale = scratch.folder.getChildFile ("Crashed");
    REQUIRE (mine.createDirectory());
    REQUIRE (stale.createDirectory());

    auto holding = OpenShow::claim (mine, entries);
    REQUIRE (holding != nullptr);

    //  A file a crash left, its lock free: nobody's show.
    REQUIRE (OpenShow::entryFor (stale, entries).replaceWithText ("123\n" + stale.getFullPathName()));

    //  This process's own show is not another window's, and asking did not let it go.
    CHECK (OpenShow::othersOpen (entries).empty());
    CHECK (OpenShow::claim (mine, entries) == nullptr);

    //  And the probe let the stale one go: it can be claimed.
    CHECK (OpenShow::claim (stale, entries) != nullptr);
}

TEST_CASE ("open shows: a request to close is taken once, and a claim clears one left behind (§106)")
{
    Scratch scratch;
    const auto entries = scratch.folder.getChildFile ("open");
    const auto show = scratch.folder.getChildFile ("Asked");
    REQUIRE (show.createDirectory());

    const wfg::app::OpenElsewhere asked { OpenShow::entryFor (show, entries).getFileNameWithoutExtension(), 0, show };

    {
        auto holding = OpenShow::claim (show, entries);
        REQUIRE (holding != nullptr);
        CHECK_FALSE (holding->closeAsked());

        CHECK (OpenShow::askToClose (asked, entries));
        CHECK (holding->closeAsked());
        CHECK_FALSE (holding->closeAsked());

        //  Let go with a request standing: it goes with the claim.
        CHECK (OpenShow::askToClose (asked, entries));
    }

    CHECK_FALSE (entries.getChildFile (asked.key + ".close").exists());

    //  One written while nobody held the show is not the next window's.
    CHECK (OpenShow::askToClose (asked, entries));
    auto next = OpenShow::claim (show, entries);
    REQUIRE (next != nullptr);
    CHECK_FALSE (next->closeAsked());

    //  A key that is not one names no file.
    CHECK_FALSE (OpenShow::askToClose ({ "../../somebody", 0, show }, entries));
}

TEST_CASE ("recent shows: newest first, one line a show, ten kept, and cleared (§106)")
{
    namespace recent = wfg::app::recent;

    Scratch scratch;
    const auto list = scratch.folder.getChildFile ("recent-shows.txt");
    const auto show = [&scratch] (int n) { return scratch.folder.getChildFile ("Show " + juce::String (n)); };

    CHECK (recent::read (list).empty());

    for (auto n = 1; n <= 12; ++n)
        REQUIRE (recent::remember (list, show (n)));

    //  The newest first, the two oldest let go.
    auto shows = recent::read (list);
    REQUIRE (shows.size() == recent::kept);
    CHECK (shows.front() == show (12));
    CHECK (shows.back() == show (3));

    //  Opened again, a show moves to the top rather than being listed twice - whatever its spelling.
    REQUIRE (recent::remember (list, juce::File (show (5).getFullPathName() + juce::File::getSeparatorString())));
    shows = recent::read (list);
    CHECK (shows.size() == recent::kept);
    CHECK (shows.front().getFullPathName().startsWith (show (5).getFullPathName()));
    CHECK (std::count_if (shows.begin(), shows.end(), [&] (const juce::File& f) { return f.getFileName() == "Show 5"; }) == 1);

    //  A show not found stays listed (BAK): it may be on a drive not plugged in.
    CHECK (std::any_of (shows.begin(), shows.end(), [&] (const juce::File& f) { return f == show (11); }));

    //  A line that is not a path is passed over.
    REQUIRE (list.replaceWithText ("not a path\n" + show (1).getFullPathName() + "\n"));
    shows = recent::read (list);
    REQUIRE (shows.size() == 1);
    CHECK (shows.front() == show (1));

    CHECK (recent::forget (list));
    CHECK (recent::read (list).empty());
    CHECK (recent::forget (list));
}
