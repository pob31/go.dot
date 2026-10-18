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
    A CUE'S MARKERS, AND THE MARKS A DOUBLE CLICK SELECTS BETWEEN.

    The author, 2026-10-18 (namespace draft §81): on the waveform's playhead,
    "Add marker"; and "Double clicking between markers (scene or additional
    manual markers, start, end, slice) then select the range. Once a range is
    selected, shift clicking on intervals adds to the selection in a
    continuous range".

    A MARKER is the show's: a named second of what the cue plays, which the
    engine carries through an edit with its material. A MARK is anything the
    hand can see a boundary at - a marker, a movie's scene change, a range's
    in or out (a slice is two of those at one instant), an edit's cut, where a
    cue with no ranges starts, and the two ends of the material. Between two
    neighbouring marks is an INTERVAL, which is what a double click selects.

    Also here, the one question the menus ask before they send anything: would
    "Set to start" or "Set to end" leave the cue anything to play. The engine
    refuses the same; asked here, the menu can say why instead of sending a
    command that will be refused.

    std only and pure, as Ranges is: every case can be asserted with no window.
*/

#include <wfg/client/model/Ranges.h>
#include <wfg/client/model/Sections.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    struct MarkerRow
    {
        std::string id;
        std::string name;
        double at = 0.0;
    };

    /** This cue's markers, in time order. */
    std::vector<MarkerRow> readMarkers (const tree::TreeSnapshot&, const std::string& cueId);

    /*  THE NAME A NEW ONE IS GIVEN: "Marker" and one more than the highest
        number a "Marker N" on this cue already has - so a marker taken away
        does not hand its number to the next, which would be two different
        points called the same thing on two days. */
    std::string nextMarkerName (const std::vector<MarkerRow>&);

    /*  EVERY MARK ON THE BAR, sorted, one per instant: the material's two
        ends, the start offset of a cue with no ranges, every range's in and
        out, every edit cut (where each section begins and ends on the edited
        timeline), the scene changes passed in, and the markers. */
    std::vector<double> marksOf (const std::vector<MarkerRow>&, const std::vector<RangeRow>&,
                                 const std::vector<SectionRow>&, const std::vector<double>& cuts,
                                 double startOffset, double length);

    /*  THE INTERVAL A SECOND IS IN: the mark at or before it and the first
        after it. Nothing when it lies outside the marks, or there are fewer
        than two. */
    std::optional<std::pair<double, double>> intervalAround (const std::vector<double>& marks, double seconds);

    /*  A SELECTION GROWN OVER ANOTHER INTERVAL, continuous: from the earlier
        start to the later end, whatever lay between taken in. */
    std::pair<double, double> spanOver (std::pair<double, double> selection, std::pair<double, double> interval);

    /*  WHY "Set to start" / "Set to end" / "Set start and end" WOULD BE
        REFUSED, in words for the panel - or empty when it would go. Mirrors
        `ShowDocument::spanRanges`: no ranges, an end must come after where the
        cue starts; with ranges, something must be left after the start and
        before the end. A side not given is `nullopt`. */
    std::string spanRefusal (const std::vector<RangeRow>&, double startOffset,
                             std::optional<double> start, std::optional<double> end);

    /*  HOW MANY RANGES THE CUE WOULD HOLD once a selection is made `count`
        slices, as the engine makes them: a cue with none first gets the whole
        file, then every range inside the selection goes, one across an end
        stays, one holding all of it becomes two, and the slices come in. */
    int rangesAfterSlicing (const std::vector<RangeRow>&, double from, double to, int count);
}
