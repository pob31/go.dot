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
    AN ABLETON LIVE SET, IMPORTED (namespace draft §29): the third stage, which
    writes the show.

    `AlsReader` read the set and `AlsWalk` decided what it means; this writes
    that into a `ShowDocument` through its checked writes - so the importer
    cannot write a show the grammar refuses (QC) - copies the sounds into the
    bundle's `media/`, saves it, and writes the report beside it (QV).

    WHAT IT WRITES:

    - one list, named after the set;
    - a mix bus for every mix something is sent to, in the order of the
      interface pairs Live patched them to, and `audio/outputPatch` putting
      each back on those channels - a parked return on none (QK, QE);
    - a DCA for every track whose fader a hand rode (QN);
    - a GO per step of the walk: the one cue the scene makes, numbered, or a
      timeline group of its sounds, fades and stops, numbered, named after the
      scene and carrying its annotation as notes (QG);
    - every media cue's file, channels, level, level lane, ranges, speed, EQ
      Eight as the cue's EQ when it fits (QL), sends and send lanes, and DCA.

    IDENTIFIERS ARE DRAWN FROM THE SET (QQ): a hash of the walk's keys - the
    scene's and the track's Live identifiers - so two venues of one tour,
    imported apart, name the same cue the same way.
*/

#include <wfg/engine/import/AlsReader.h>
#include <wfg/engine/import/AlsWalk.h>

#include <juce_core/juce_core.h>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace wfg::doc { class ShowDocument; }

namespace wfg::import::als
{
    /*  THE SCENES A SET IS IMPORTED WITH UNLESS SOMEBODY SAYS OTHERWISE (QS):
        the named ones that do something. A rehearsal stash of unnamed scenes
        stays out, and its gigabyte file with it. */
    std::set<int> defaultScenes (const LiveSet&);

    /*  "1-14,16": scene NUMBERS as Live shows them, from one, into indices.
        Nullopt for anything that is not a list of numbers and ranges. */
    std::optional<std::set<int>> parseScenes (const std::string& text);

    /** An identifier drawn from a key: eight characters of Crockford base32. */
    std::string idFor (const std::string& key);

    /*  WHERE A CLIP'S FILE IS ON THIS DISK (QR): its relative path from the
        set's folder, its absolute path, then a search of the folder by name and
        size - a `:` in a Mac name tried as the characters a copy turns it into.
        An empty File when none of those is there. */
    juce::File findMedia (const FileReference&, const juce::File& setFolder);

    struct ImportOptions
    {
        juce::File into;                ///< the show folder to write; made when it is not there
        std::set<int> scenes;           ///< empty: `defaultScenes`
        bool copyMedia = true;
        Laws laws;

        /*  A sentence at each step - reading, walking, each sound copied -
            from whichever thread runs the import. */
        std::function<void (const std::string&)> progress;
    };

    struct ImportOutcome
    {
        bool ok = false;
        std::string error;              ///< why nothing was written
        juce::File show;
        juce::File report;

        int gos = 0;
        int sounds = 0;
        int approximated = 0;
        int dropped = 0;
        std::vector<std::string> missingMedia;
        std::vector<std::string> problems;  ///< what the document's own validation said, if anything
    };

    /*  THE SHOW, WRITTEN: read, walked, built, its media copied, validated,
        saved, and its report written. Refuses a folder that already holds a
        show rather than writing over somebody's work. */
    ImportOutcome importSet (const juce::File& set, const ImportOptions&);

    /*  A TOUR, WRITTEN AS ONE SHOW (QF, namespace draft §25): the sets of one
        piece - a venue each - read together. The show folder `into` holds the
        sounds every performance shares, copied once into its `media/`, and a
        template cue list made from `templateSet` (the newest set when none is
        named); each set becomes a performance folded inside it, named after its
        date and what is left of its file name once the part every set shares
        is taken off ("Lazzi régie Pau" among "Lazzi régie ..." is "Pau"). The
        scenes are the template's, chosen once and found in every set by their
        Live identifier. A set with no annotations at all takes its notes from
        the template, scene by scene (QT). Identifiers are the set's (QQ), so
        "Update the show's template..." compares cue by cue. */
    struct TourOutcome
    {
        ImportOutcome show;                         ///< the template, in the show folder
        std::vector<ImportOutcome> performances;    ///< one per set, in the order given
    };

    TourOutcome importTour (const std::vector<juce::File>& sets, const juce::File& templateSet,
                            const ImportOptions&);

    /*  A PERFORMANCE'S FOLDER NAME: the file's date, and its name less the
        part every set's name shares - which `sharedPrefix` finds, cut back to
        a whole word. */
    std::string sharedPrefix (const std::vector<std::string>& names);
    std::string performanceName (const juce::File& set, const std::string& prefix);

    /*  THE BUILD ALONE, for the tests: the walk written into `document`, which
        should be empty. `media` names each sound's file as the document will -
        its name in `media/` - by the sound's key; a sound it does not name keeps
        the file's own name. The notes the build adds are appended to `notes`. */
    struct BuiltShow
    {
        std::vector<Note> notes;
        int gos = 0;
        int sounds = 0;
    };

    BuiltShow build (const LiveSet&, const Walk&, const std::string& listName,
                     const std::map<std::string, std::pair<std::string, int>>& media,
                     doc::ShowDocument& document);
}
