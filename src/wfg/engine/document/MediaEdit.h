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

#include "LevelLane.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/*  A SOUND'S EDIT, AS ARITHMETIC (namespace draft §55).

    A sound cue's sections - pieces of its file, each with an in and an out
    point, a trim and a crossfade at the join into it - in their order are the
    EDITED TIMELINE the cue plays: section k begins where section k-1 ends, and
    the edit is as long as its sections put together. Nothing here reads a
    file: the timeline is a sum of differences, which is what lets a replay
    with no media agree with the session that had it.

    THE TIME INVARIANT (ADK): the cue's file time is always the time of what
    it plays - this timeline while the edit is open, the bounce's when frozen,
    which is the same. So a lane, a range and a start offset keep their
    meaning for every reader, and it is the EDIT that moves them: a change
    that shifts the timeline (an edge moved, a section moved or removed, the
    edit cleared) is a piecewise map from the old timeline to the new, and
    everything on the old one is carried through it here, by the commands, in
    the same Undo step. A split, a join, a trim and a crossfade shift nothing.

    Pure, and shared with nothing that may not name `doc::`: the window's
    model restates the little it needs and the client tests hold the two to
    each other, as the lane's does.
*/
namespace wfg::doc
{
    struct Section
    {
        std::string id;
        double in = 0.0;          // seconds of the file
        double out = 0.0;
        double trimDb = 0.0;
        double crossfade = 0.01;  // seconds, centred on the join into this section; ignored on the first

        double length() const noexcept { return out - in; }
    };

    /*  Two instants the file cannot tell apart: the range door's (a split at
        a cut is no split), and here what makes a join CONTINUOUS. */
    constexpr double sameInstant = 0.001;

    /*  The crossfade a new join gets: the click suppressor the slice moves
        use. And the least a trim difference ramps over at a continuous join. */
    constexpr double defaultCrossfade = 0.01;
    constexpr double leastTrimRamp = 0.005;

    //==============================================================================
    /*  THE TIMELINE. */

    /** Where each section begins on the edited timeline: S_0 = 0, S_k = sum of the lengths before it. */
    std::vector<double> sectionStarts (const std::vector<Section>& sections);

    /** The edit's length: the sections' lengths put together. */
    double editedLength (const std::vector<Section>& sections);

    /*  A join still one in the file: the outgoing section's out point IS the
        incoming one's in point, as a split leaves them. Such a join plays
        plain whatever its crossfade says (ADL). */
    bool isContinuousJoin (const Section& before, const Section& after) noexcept;

    /*  The crossfade a join is rendered with: none on the first section. */
    double crossfadeInto (const std::vector<Section>& sections, std::size_t index) noexcept;

    /*  No edit at all: no sections, or one that is the whole file as recorded.
        A cue in that state plays its file and renders nothing. */
    bool isIdentityEdit (const std::vector<Section>& sections, double sourceLength) noexcept;

    /*  The edit as one line of text - in, out, trim and crossfade of each
        section, the first one's crossfade written as nought since it is not
        heard - spelled the same whatever the locale: the render's key and the
        compare that says whether a render is of the edit as it now is. */
    std::string editText (const std::vector<Section>& sections);

    /** Which section an edited second falls in, and where that is in the file. */
    struct Place
    {
        std::size_t index = 0;
        double fileSecond = 0.0;
    };

    std::optional<Place> placeOf (const std::vector<Section>& sections, double editedSecond) noexcept;

    //==============================================================================
    /*  THE MAP FROM ONE TIMELINE TO THE NEXT. A run is a stretch of material
        kept by the edit: where it was on the old timeline, where it is on the
        new, and how long. Inside a run the map is a shift; between runs it
        may reorder (a move) or leave a gap (a removal). */
    struct TimeRun
    {
        double oldFrom = 0.0;
        double newFrom = 0.0;
        double length = 0.0;
    };

    /*  Sections matched by identifier: for each kept in both lists, the
        material in both its old and its new extent is one run. A split or a
        join is the identity in runs; a trim at an edge loses or gains a
        sliver; a removal loses a section; a move reorders. Sorted by where
        each run was. */
    std::vector<TimeRun> timeMap (const std::vector<Section>& before, const std::vector<Section>& after);

    /*  The map from the edited timeline back to the file's own time - what
        clearing the edit does to everything on it. */
    std::vector<TimeRun> timeMapToFile (const std::vector<Section>& sections);

    /*  A second inside a kept run, carried; nothing for one in material the
        edit no longer has. The end of a run counts as inside it when no run
        begins there, so a point at the very end survives. */
    std::optional<double> carrySecond (const std::vector<TimeRun>& runs, double second) noexcept;

    /*  Where removed material fell: the end, on the new timeline, of the
        latest run that ended at or before the second; else the start of the
        earliest run; nought with no runs. */
    double carryToCut (const std::vector<TimeRun>& runs, double second) noexcept;

    /** carrySecond, else carryToCut. */
    double carried (const std::vector<TimeRun>& runs, double second) noexcept;

    //==============================================================================
    /*  WHAT IS CARRIED. */

    /*  A lane's points through the map: those in kept material move with it,
        those in removed material go with it, and the result climbs strictly
        again - sorted, and of two points landing on one instant the earlier
        one is kept. A slope that crossed a cut is cut. */
    std::vector<LanePoint> carryLane (const std::vector<LanePoint>& points, const std::vector<TimeRun>& runs);

    /** A lane's text, as the row holds it. */
    std::string laneText (const std::vector<LanePoint>& points);

    /*  Text in, text out: the row's lane carried. A text that is no lane - the
        door refuses one, so a document never holds one - is returned as it is. */
    std::string carryLaneText (std::string_view text, const std::vector<TimeRun>& runs);

    struct CarriedRange
    {
        std::string id;
        double in = 0.0;
        double out = 0.0;
        bool removed = false;
    };

    /*  A range's in and out points carried each on its own; one that is left
        ending at or before its start - both points in removed material, or
        the two sides of a move - is marked removed, for the command to take
        out and say. */
    std::vector<CarriedRange> carryRanges (const std::vector<CarriedRange>& ranges, const std::vector<TimeRun>& runs);

    /** A start offset carried; nought stays nought, being the resting value. */
    double carryStartOffset (double offset, const std::vector<TimeRun>& runs) noexcept;

    //==============================================================================
    /*  THE DOOR'S JUDGEMENTS. */

    /*  The crossfades held to the material: a join's crossfade reaches back
        half its length before the incoming section's in point, so it can be
        no longer than twice that point (there is nothing before the file's
        start); and a section's two crossfades, half each, must fit inside it.
        Of two that do not fit, the larger is shrunk. The first section's is
        left as stored: it is not heard. */
    std::vector<Section> clampCrossfades (std::vector<Section> sections);

    /*  Why a list of sections is no edit, in words naming the section at
        fault; empty when it is one. A section's in below nought, an out not
        after its in, a trim outside the level row's range, a crossfade below
        nought, or a number that is not one. */
    std::string whyNotSections (const std::vector<Section>& sections,
                                double trimLow = -120.0, double trimHigh = 12.0);
}
