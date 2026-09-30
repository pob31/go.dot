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
    A SHOW IS OPEN IN AT MOST ONE WINDOW (author, 2026-09-30: a double-click on
    a show that is already open brings its window forward).

    Each show is its own process (namespace draft §14.16), and nothing stopped
    two of them opening the same folder: Open show... on a show already open, and
    now a double-click, would stand up a second engine whose autosave writes
    the same recovery/ as the first. So a windowed serve CLAIMS its show before
    it loads it, and one that finds the show claimed brings the claimant's
    window forward and goes.

    THE CLAIM IS A SYSTEM LOCK, NOT A FILE SAYING SO. A named mutex on Windows,
    an fcntl lock elsewhere (juce::InterProcessLock): the system releases it
    when the process ends however it ends, so a crash cannot leave a show
    looking open. Beside it, in Go.dot's own folder and never in the show
    (PRD §4.10), one small file names the process holding it, which is what
    the window is found by. That file can go stale; the lock cannot, and the
    file is only read when the lock says the show is held.

    KEYED BY THE SHOW'S FULL PATH, folded to lower case where the file system
    ignores case, so that two spellings of one folder are one show.
*/

#include <juce_core/juce_core.h>

#include <memory>

namespace wfg::app
{
    class OpenShow final
    {
    public:
        /*  This process takes `show`: null when another process has it, or
            this one already does. `entries` is where the holder's file goes;
            a test names its own. */
        static std::unique_ptr<OpenShow> claim (const juce::File& show,
                                                const juce::File& entries = defaultEntries());

        /*  Whether ANOTHER process has `show` open. This process's own shows
            are not asked about - an fcntl lock taken twice by one process is
            the same lock, and letting go of the probe would let go of it. */
        static bool heldElsewhere (const juce::File& show);

        /*  Brings forward the window of the process holding `show`. False when
            nobody is named, or the system would not (Raise.h). */
        static bool raiseHolder (const juce::File& show, const juce::File& entries = defaultEntries());

        /*  `<user application data>/Go.dot/open`. */
        static juce::File defaultEntries();

        /*  The file naming the process that holds `show`. Public for the tests. */
        static juce::File entryFor (const juce::File& show, const juce::File& entries);

        /*  Lets the show go: the file first, then the lock. */
        ~OpenShow();

        OpenShow (const OpenShow&) = delete;
        OpenShow& operator= (const OpenShow&) = delete;

    private:
        OpenShow (const juce::String& key, const juce::File& entry);

        juce::String key;
        juce::InterProcessLock lock;
        juce::File entry;
        bool claimed = false;
    };
}
