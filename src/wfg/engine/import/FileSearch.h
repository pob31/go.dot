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
    A FOLDER SEARCHED FOR FILES BY NAME (namespace draft §100, AXX).

    The author, 2026-10-20, of a QLab workspace carried off the computer it was
    made on: "Could we have a stage where we can point to a folder if there are
    missing files?" - "As long as the user can point the parser to the right
    folder(s)". A folder somebody points at is walked ONCE, every file in it
    and under it kept by its name - accents folded, case ignored, as two
    copies of one name differ between a Mac and this disk - so a hundred
    missing sounds cost one walk, not a hundred.

    Where one name is found more than once, the copy whose path ends most like
    the path the show remembers wins - `Soundbank/rain.wav` over
    `Backup/rain.wav` - and then the first found. A folder named Backup, and
    Live's `.asd` analysis files, are never a sound's file.

    juce_core only, so the window - which copies a found file into the show's
    `media/` itself (§14.16) - and the importers use the one search.
*/

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace wfg::import
{
    class FolderIndex
    {
    public:
        /*  `folder` walked now, on the calling thread - never the message
            thread for a folder somebody picked, which may be a whole drive.
            `stop`, when given, is asked between files and ends the walk early. */
        explicit FolderIndex (const juce::File& folder, const std::atomic<bool>* stop = nullptr);

        /*  THE FILE OF THIS NAME, or an empty File. `remembered` is the path
            the show or the workspace gives it - relative or absolute, `/` or
            `\` - which breaks a tie between copies; `size` above nought keeps
            only a copy of that size. */
        juce::File find (const std::string& name, const std::string& remembered = {}, std::int64_t size = 0) const;

        const juce::File& folder() const noexcept { return root; }
        std::size_t files() const noexcept { return count; }

        /** A name as the index keys it: accents folded, lower case. */
        static juce::String keyOf (const juce::String& name);

    private:
        juce::File root;
        std::multimap<juce::String, juce::File> byName;
        std::size_t count = 0;
    };

    /*  THE FIRST OF `folders` THAT HOLDS IT, in their order: a person's first
        pick is their best guess. */
    juce::File findInFolders (const std::vector<FolderIndex>& folders, const std::string& name,
                              const std::string& remembered = {}, std::int64_t size = 0);
}
