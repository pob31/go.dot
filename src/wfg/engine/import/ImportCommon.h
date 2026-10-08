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
    WHAT EVERY IMPORTER SHARES (namespace draft §46, QL.2): the parts the Live
    set's importer had alone until a second importer needed them - an
    identifier drawn from a key, a sound found on this disk and copied into the
    bundle, the show validated and saved where it goes, and the note a report
    is made of.

    Nothing here knows what a Live set or a QLab workspace is. Each importer
    keeps its reader, its walk and its report; these are the pieces that would
    otherwise be written twice and drift.
*/

#include <juce_core/juce_core.h>

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace wfg::doc { class ShowDocument; }

namespace wfg::import
{
    /*  A NOTE FOR THE REPORT: what was seen and what was done about it, where.
        `approximated` is something that came over changed; `dropped` is
        something that did not come over; `info` is a fact a designer should
        know. `scene` and `track` say where - a Live set's scene and track, a
        QLab workspace's cue list (-1 for the whole) and cue. */
    struct Note
    {
        enum class Kind { info, approximated, dropped } kind = Kind::info;
        int scene = -1;             ///< where in the source, -1 for the source as a whole
        std::string track;          ///< what in it, empty for nothing in particular
        std::string text;
    };

    /*  ROUNDED BEFORE THEY ARE WRITTEN: `decimals` places, through the
        engine's own formatter, so the file reads and no locale moves a digit. */
    std::string number (double value, int decimals);

    std::uint64_t fnv1a (const std::string& text);

    /** An identifier drawn from `salt` and a key: eight characters of Crockford base32. */
    std::string idFor (const std::string& salt, const std::string& key);

    /*  IDENTIFIERS, ONE PER KEY, NEVER TWO ALIKE: a key that hashes onto one
        already given takes the next of its own line, so the answer is still
        the same every time the same source is imported. */
    struct Identities
    {
        explicit Identities (std::string saltToUse) : salt (std::move (saltToUse)) {}

        std::string of (const std::string& key);

        std::string salt;
        std::map<std::string, std::string> given;
        std::set<std::string> taken;
    };

    /*  A FILE NAME WITH ITS ACCENTS FOLDED AWAY, for comparing two spellings of
        one name: macOS writes a name decomposed, a copy made elsewhere
        composed. */
    juce::String folded (const juce::String& name);

    /** The names a `:` in a Mac file name becomes on a copy that left the Mac. */
    std::vector<std::string> spellings (const std::string& name);

    /*  A FILE AS ITS SOURCE NAMED IT: a path relative to a folder, the absolute
        path it had where it was saved, its own name, and its size when known. */
    struct FileSought
    {
        std::string relativePath;   ///< folders joined with '/'
        std::string absolutePath;
        std::string name;
        std::int64_t size = 0;      ///< nought when not said
    };

    /*  WHERE IT IS ON THIS DISK: its relative path from each of `folders` in
        turn, a folder at a time so a `:` is tried as its spellings; its
        absolute path; then a search of the first folder by name - accents
        folded - and size. An empty File when none of those is there. */
    juce::File findFile (const FileSought&, const std::vector<juce::File>& folders);

    /** The channels of a sound file, nought when it cannot be read. */
    int channelsOf (const juce::File&);

    /*  THE SOUNDS OF ONE IMPORT, copied into one `media/` once each: a source
        file to the name it has there, and the names taken. */
    struct MediaBook
    {
        std::map<juce::String, std::string> copied;
        std::set<std::string> namesUsed;
    };

    struct PlacedFile
    {
        std::string name;           ///< its name in `media/`
        int channels = 0;
        bool copyFailed = false;
    };

    /*  ONE FILE PLACED: copied into `mediaFolder` under its own name - or
        another, when a different file took it - unless the same size is there
        already, and read for its channels. A source copied before keeps the
        name it was given. */
    PlacedFile placeFile (const juce::File& source, const juce::File& mediaFolder, bool copy, MediaBook&,
                          const std::function<void (const std::string&)>& say);

    /** A folder that already holds a show, which no import writes over. */
    bool holdsAShow (const juce::File& folder);

    struct SavedShow
    {
        bool ok = false;
        std::string error;
        std::vector<std::string> problems;  ///< what the document's own validation said
    };

    /*  THE SHOW VALIDATED AND SAVED: refused, and nothing written, when the
        document does not validate. `what` names the source in the sentence. */
    SavedShow saveShow (doc::ShowDocument&, const juce::File& folder, const std::string& what);
}
