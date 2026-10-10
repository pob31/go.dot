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

#include <wfg/engine/audio/EditRenderTable.h>
#include <wfg/engine/document/MediaEdit.h>

#include <juce_data_structures/juce_data_structures.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

/*  THE ONE ANSWER TO "WHICH FILE DOES THIS CUE PLAY, AND HOW LONG IS IT"
    (namespace draft §55, ADK, ADM).

    A sound cue - or a HAP movie's, edited the same way (§55.5, ADS) - with
    no sections, or a frozen one, plays its `file`: the
    bounce IS the file while frozen, so nothing here knows a frozen cue from a
    plain one. A cue whose edit is OPEN plays the render of its sections, once
    the renderer has made one of the edit as it now is, and is as long as its
    sections put together - never as long as a file, so a replay with no
    render and no media agrees with the session that had both. Until the
    render is there the cue plays nothing and says so (`rendering`).

    Every reader of the file a cue plays asks here: the Runner's arm and its
    lengths, the solver, the show walk, the tree's `duration` row. The readers
    of the file a cue NAMES - Save as, the analyser, the log's header, the
    importers - read the row, which is the right thing for them.
*/
namespace wfg::cue
{
    /** The sections a sound cue's or a movie cue's node holds, in document order; none for anything else. */
    std::vector<doc::Section> sectionsIn (const juce::ValueTree& cue);

    /** The edit's text, as the render's key is made from; empty with no sections. */
    std::string editTextOf (const juce::ValueTree& cue);

    /*  The edit's length, when the cue's edit is OPEN - sections and no
        `editSource`; nothing otherwise, and the file's length is the answer. */
    std::optional<double> editedLengthOf (const juce::ValueTree& cue);

    /*  THE SECOND IN THE FILE an edited second falls at (namespace draft
        §55.5, ADV): the second itself for a cue with no open edit; past the
        end, the last section's out; before the top, the first's in. What the
        monitor's tile shows of a movie being edited, the file's frame at the
        second the edit maps to. */
    double fileSecondOf (const juce::ValueTree& cue, double editedSecond);

    /*  WHAT THE VIDEO MONITOR SHOWS OF A MOVIE at an edited second (namespace
        draft §55.13, ADV amended): the render of its open edit at that second -
        its dissolves, its fades and its black as they play; while the render is
        not there yet, the source's frame at the second the edit maps to, and
        nothing in its silence; the file itself otherwise, a bounce included.
        `file` empty is the file the cue names, as its spec reads it. */
    struct TileFrame
    {
        std::string file;       // relative to the media folder; empty: the cue's own
        double seconds = 0.0;   // of that file
        bool shown = true;      // false in an edit's silence
    };

    TileFrame tileFrameOf (const juce::ValueTree& cue, double editedSecond,
                           const std::map<std::string, double>* durations, const audio::EditRenders* renders);

    struct PlayedMedia
    {
        std::string name;             // the file the cue plays, relative to media/; empty when there is none yet
        double lengthSeconds = 0.0;
        bool lengthKnown = false;
        bool openEdit = false;        // playing the render of its sections, or waiting for one
        bool frozen = false;
    };

    /*  `durations` and `renders` may each be nothing: a replay and a test
        hand in what they have. */
    PlayedMedia playedMediaOf (const juce::ValueTree& cue,
                               const std::map<std::string, double>* durations,
                               const audio::EditRenders* renders);

    /** The length a cue plays, from the edit or the lengths; nothing when it is not known. */
    std::optional<double> playedLengthOf (const juce::ValueTree& cue, const std::map<std::string, double>* durations);
}
