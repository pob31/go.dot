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
    A media cue's RANGES, and what dragging one of their edges would write.

    PRD §3.24: a cue's content carries an ordered list of regions of its file,
    each with an in-point, an out-point, a loop count from one to for ever, and
    a name. Playback walks the list, so document order is playlist order and
    the regions need be neither contiguous nor in file order — a cue is then a
    playlist over one file, which is how alternate takes and versioned sections
    live in a show without duplicating media.

    THREE THINGS CAN BE DRAGGED and only three, which is what keeps this small:
    an in-point, an out-point, and a SLICE — the place where one range ends and
    the next begins at the same instant. A slice is the interesting one. Two
    ranges that meet are what a designer means by a loop boundary inside a
    file, and dragging the join has to move BOTH numbers or the two will part
    and leave a silent gap nobody asked for. So a slice is one gesture and two
    writes, and it is found rather than declared: any two neighbours that meet
    are a slice, and they stop being one the moment somebody pulls them apart.

    WHAT IT DOES NOT DO is judge. `range/@in` and `@out` are ordinary writable
    rows and the engine already refuses what it must; this file answers where
    the hand is and what number that implies, and the write goes through
    `node.set` like every other. It does clamp into the file and keep a range
    from turning inside out, because a drag that produced an impossible number
    would be a gesture that refuses itself halfway.

    std only, and pure: every case below can be asserted with no window, no
    engine and no file.
*/

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    struct RangeRow
    {
        std::string id;
        std::string name;
        double in = 0.0;
        double out = 0.0;
        int loops = 1;      ///< nought is for ever
        int index = 0;      ///< where it sits in the cue's list

        double length() const noexcept { return out - in; }
    };

    /** Every range of this cue, in the order playback walks them. */
    std::vector<RangeRow> readRanges (const tree::TreeSnapshot&, const std::string& cueId);

    /*  EVERY RANGE IN THE SHOW, gathered in ONE pass and grouped by the cue
        that owns it, each cue's list in playlist order.

        The running pane wants the ranges of every cue it is drawing, and
        asking `readRanges` per row would walk the tree once per row, twenty-
        five times a second - the per-row habit the boundary check exists to
        stop. One walk answers for all of them, and `readRanges` is this with
        one cue picked out of it. */
    std::map<std::string, std::vector<RangeRow>> rangesByCue (const tree::TreeSnapshot&);

    /*  WHAT THE POINTER IS OVER. `slice` names the pair that meet there, and
        is preferred to either edge on its own: at a join the two handles sit
        on the same pixel, and picking one of them would silently tear the
        loop. */
    enum class Handle { none, in, out, slice };

    struct Hit
    {
        Handle handle = Handle::none;
        std::string rangeId;   ///< the range whose edge it is; the EARLIER one of a slice
        std::string nextId;    ///< for a slice, the later one
    };

    /*  Which edge is within `tolerance` seconds of `seconds`, the nearest
        winning. A tolerance in seconds rather than pixels because this file
        knows nothing about pixels; the caller turns its grab radius into
        seconds through `View`. */
    Hit hitTest (const std::vector<RangeRow>&, double seconds, double tolerance);

    /*  Whether two numbers are the same instant. Named because a slice is
        defined by it and because `==` on doubles is not what this repository
        writes (the strict build compiles with -Wfloat-equal, and the question
        here is genuinely "near enough" rather than "bit for bit"). */
    bool sameInstant (double a, double b) noexcept;

    /** One write a drag implies: an address suffix, and the seconds to put there. */
    struct RangeWrite
    {
        std::string rangeId;
        const char* attribute = "in";   ///< "in" or "out"
        double seconds = 0.0;
    };

    /*  WHERE THAT DRAG LANDS, as the writes it implies — one for an edge, two
        for a slice. Empty when the hit was nothing, or when the drag would
        turn a range inside out or push it past the file: a gesture that cannot
        be honoured writes nothing rather than writing half of itself.

        `smallest` is how short a range may be got to; below it the drag stops
        rather than collapsing the range to nothing, because a range of no
        length is not a shorter range, it is a mistake with no visible handle
        left to undo it by. */
    std::vector<RangeWrite> dragTo (const Hit&, double seconds,
                                    const std::vector<RangeRow>&, double fileLength,
                                    double smallest = 0.05);

    /*  THE ADDRESS OF ONE OF A RANGE'S ROWS. In one place because two callers
        now assemble it - the bar's drag and the table's boxes - and a prefix
        spelt twice is a prefix that will one day be spelt two ways. */
    std::string rangeAddress (const std::string& rangeId, const char* attribute);

    /*  A TIME AS THE TABLE WRITES IT AND READS IT BACK: seconds to the
        millisecond, and minutes once there are any, so a cue point three
        minutes into a file is not read off as "187.4".

        `timeFrom` takes either form and nothing else - `nullopt` for anything
        that is not a time, so a mistyped cell leaves the number where it was
        instead of moving it to nought. Both go through `osc::parseDouble` and
        `osc::formatDouble`, which is this project's one locale-independent
        pair: in fr_FR a comma is what a hand types and a full stop is what the
        document holds, and only these two know that. */
    std::string timeText (double seconds);
    std::optional<double> timeFrom (const std::string& text);

    /*  THE SAME LENGTH AGAIN, ONE RANGE FURTHER ON (author, 2026-09-21: *"be
        able to copy a duration from one slice to the next so the next out
        point is at the same time from the previous"*).

        It moves the NEXT range's out-point and nothing else: its in-point
        stays where it is, so a join with this range survives the gesture and
        a list of equal-length slices is built by pressing the same button
        down the table. Empty - no write at all - when there is no next range,
        when this one has no length to copy, or when the answer would run past
        the end of the file; a button that silently produced a range beyond
        the material would be worse than one that declines. */
    std::vector<RangeWrite> copyLengthToNext (const std::vector<RangeRow>&, std::size_t index,
                                              double fileLength);

    /*  WHERE A NEW RANGE WOULD GO: after the last one, to the end of the file,
        or the whole file when there are none yet. `nullopt` when there is no
        room left - the material is already spoken for, and the honest answer
        is to say so rather than to make a range of no length. */
    std::optional<std::pair<double, double>> nextRange (const std::vector<RangeRow>&,
                                                        double fileLength,
                                                        double smallest = 0.05);

    /*  WHAT THE PLUS WOULD DO WITH THE PLAYHEAD WHERE IT IS (author,
        2026-09-21: *"even if the ranges amount to the full file, pressing the
        [+] range button will split the range where the cursor is. No split if
        the cursor is already on a cut or either the start or end of the
        file"*).

        ONE BUTTON, THREE ANSWERS, and which one it is depends only on where
        the head is standing:

          - inside a range, it CUTS it there, so a cue whose ranges already
            cover the whole file can still be divided - which is the ordinary
            way a bed becomes a set of slices;
          - in a gap, or on a cue with no ranges at all, it MAKES one from the
            head to whatever comes next;
          - on a cut, at the top of the file or at its end, it does NOTHING,
            and says why - there is no range to divide at a place that is
            already a boundary, and a second cut on a cut would make a region
            of no length.

        `why` is filled only for `nothing`, and is a sentence for the panel to
        show rather than a code: the button is live-looking either way, and an
        operator who presses it deserves to learn what would have made it
        work. */
    struct RangeAdd
    {
        enum class Kind { nothing, split, create };

        Kind kind = Kind::nothing;
        double at = 0.0;              ///< where to cut, for `split`
        double in = 0.0, out = 0.0;   ///< the span to make, for `create`
        std::string why;              ///< for `nothing`, in words
    };

    RangeAdd addAt (const std::vector<RangeRow>&, double seconds, double fileLength,
                    double smallest = 0.05);

    /*  THE INSTANTS WORTH SNAPPING TO while a range edge is dragged: every
        other edge, the ends of the file, and the playhead if one is running.
        Returned rather than applied, so the caller decides the tolerance and
        so a test can read the list. */
    std::vector<double> snapTargets (const std::vector<RangeRow>&, const std::string& movingId,
                                     double fileLength);

    /** `seconds` pulled to the nearest target within `tolerance`, or left alone. */
    double snapTo (double seconds, const std::vector<double>& targets, double tolerance);
}
