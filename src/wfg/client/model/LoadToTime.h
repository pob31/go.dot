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
    Load to time, as the window reads it: the list's history, the aim, and
    the engine's answer to it (PRD §3.13).

    The author's design (2026-09-18): "Load to time is opened with an item of
    the Show menu. The Inspector panel turns into a history vertical stack. The
    cue list items can be spaced vertically to show the various intermediary
    steps recorded with the focus on the selected cue or group. Cue or group
    can be changed and the load to time readjusts to it. The panel is only
    closed when the user triggers Go. A numerical time value appears so the
    value can be set from the keyboard and a pointer shows when this is in the
    history."

    THREE NODES AND ONE DOCUMENT. `list/history` is what the list did, newest
    first; `list/aim` is where the operator is pointing; `list/solve` is what
    the show would be there, as JSON the engine writes - and since 2026-09-19
    it says whether it was read from the history or from the list's order,
    and the instant on the wall clock it describes. Nothing here decides
    anything: the reading is the engine's, and the rows this file builds are
    that reading laid under the aimed cue so it can be looked at.

    std only, like the rest of model/.
*/

#include <wfg/client/model/ShowModel.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    /** One thing the list did, as `list/history` spells it. */
    struct HistoryStep
    {
        std::int64_t tick = 0;
        std::string cue;
        char origin = 'g';   ///< g a GO, f fired by name, t a trigger
    };

    /** `<tick>:<cue>:<origin>` space-separated, newest first, as published. */
    std::vector<HistoryStep> readHistory (const std::string& text);

    /** The letter as the word an operator reads: GO, by name, trigger. */
    std::string originWord (char origin);

    /** One run the engine's answer names. */
    struct PlannedLine
    {
        std::string cue;
        std::string when;      ///< sounding, finished, due
        double offset = 0.0;   ///< seconds in, when sounding
        double startsIn = 0.0; ///< seconds until, when due
    };

    struct LoadToTimeReading
    {
        std::string listId;
        std::string listName;
        std::int64_t tick = 0;                 ///< the engine's tick, for "ago"

        std::vector<HistoryStep> steps;        ///< newest first

        bool aimed = false;
        std::string aimCue;
        double aimOffset = -1.0;

        /*  The engine's answer, when there is one: how it was read, the wall
            tick it describes, what would be sounding and where the pointer
            would land. `ok` is the answer's own word for "the aim named
            something this list holds". */
        bool ok = false;
        std::string how;
        std::int64_t instant = -1;
        std::vector<PlannedLine> runs;
        std::string standby;
        std::vector<std::string> confused;

        /** Names for every cue the steps and the answer mention, by id. */
        std::map<std::string, std::string> names;

        std::string nameOf (const std::string& cueId) const;
    };

    LoadToTimeReading readLoadToTime (const tree::TreeSnapshot& snapshot,
                                      const std::string& listId);

    /*  THE STEPS THAT SIT UNDER THE AIMED CUE: everything the list did after
        that cue was last fired, each at its offset into the cue, and marked
        `undone` when it lies past the instant - a load would take it back.
        Oldest first, which is the order they are drawn in. Empty when the
        aimed cue was never fired: there is no clock to place them on. */
    struct StepLine
    {
        std::string cue;
        char origin = 'g';
        double offset = 0.0;
        bool undone = false;
    };

    std::vector<StepLine> stepsUnder (const std::vector<HistoryStep>& steps,
                                      const std::string& aimCue, std::int64_t instant);

    /*  The rows to insert under the aimed cue's row: the step lines in time
        order, with the aim's own pointer row among them where its offset
        falls - first, when the aim is "before". */
    std::vector<Row> stepRows (const std::vector<StepLine>& lines, double aimOffset,
                               const std::map<std::string, std::string>& names, int depth);

    /** `+m:ss.t` for a second into the cue, `before` for -1. */
    std::string offsetText (double offset);

    /** "12 s ago" / "3 min ago" from two ticks at fifty a second. */
    std::string agoText (std::int64_t at, std::int64_t now);
}
