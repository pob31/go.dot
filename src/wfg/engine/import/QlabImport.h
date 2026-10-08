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
    A QLAB WORKSPACE, IMPORTED (namespace draft §46, QL.6): the third stage,
    which writes the show - as `AlsImport` does for a Live set.

    `QlabReader` read the workspace and `QlabWalk` decided what it means; this
    finds each sound on this disk and copies it into the bundle's `media/`,
    hands the walk each file's channels, writes the plan into a `ShowDocument`
    through its checked writes (ZN) - so the importer cannot write a show the
    grammar refuses - saves it into a NEW folder, and writes the report beside
    it (ZZ). The open show is never touched.

    WHAT IT WRITES: a cue list per QLab cue list or cart ticked; a mono direct
    bus per QLab cue output in use, patched to the interface output of the same
    number (ZU); an opaque device per network patch a cue sends through (ZW);
    and every cue the plan holds, its targets filled in once every cue exists.

    IDENTIFIERS ARE DRAWN FROM QLAB'S UUIDs (ZY), salted "wfg-qlab:", so a
    workspace imported twice writes the same show byte for byte.
*/

#include <wfg/engine/import/ImportCommon.h>
#include <wfg/engine/import/QlabReader.h>
#include <wfg/engine/import/QlabWalk.h>

#include <juce_core/juce_core.h>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace wfg::doc { class ShowDocument; }

namespace wfg::import::qlab
{
    /** An identifier drawn from a key: eight characters of Crockford base32, salted for QLab. */
    std::string idFor (const std::string& key);

    /*  "1-2,4": cue list NUMBERS, from one, into indices. Nullopt for anything
        that is not a list of numbers and ranges. */
    std::optional<std::set<int>> parseLists (const std::string& text);

    /*  WHERE A SOUND'S FILE IS ON THIS DISK: its relative path from the
        workspace's folder - from the folder above it for QLab 4 - then its
        absolute path, then by name under the workspace's folder. */
    juce::File findMedia (const FileRef&, const juce::File& workspaceFolder);

    struct ImportOptions
    {
        juce::File into;                ///< the show folder to write; refused when it holds a show
        std::set<int> lists;            ///< empty: every list
        bool copyMedia = true;

        /*  A sentence at each step, from whichever thread runs the import. */
        std::function<void (const std::string&)> progress;
    };

    struct ImportOutcome
    {
        bool ok = false;
        std::string error;              ///< why nothing was written
        juce::File show;
        juce::File report;

        int lists = 0;
        int cues = 0;                   ///< QLab cues walked
        int placeholders = 0;           ///< memos in the place of a kind with no equivalent
        int approximated = 0;
        int dropped = 0;
        std::vector<std::string> missingMedia;
        std::vector<std::string> problems;
    };

    ImportOutcome importWorkspace (const juce::File& workspace, const ImportOptions&);

    /*  THE BUILD ALONE, for the tests: the plan written into `document`, which
        should be empty. `media` names each sound's file as the document will -
        its name in `media/` and its channels - by the QLab cue's UUID; a sound
        it does not name keeps its file's own name. */
    struct BuiltShow
    {
        std::vector<Note> notes;
        int lists = 0;
        int cues = 0;                   ///< Go.dot cues written, groups among them
    };

    BuiltShow build (const Workspace&, const Plan&, const std::map<std::string, std::pair<std::string, int>>& media,
                     doc::ShowDocument& document);
}
