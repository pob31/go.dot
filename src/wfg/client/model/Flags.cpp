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

#include <wfg/client/model/Flags.h>
#include <wfg/client/model/Text.h>

#include <algorithm>
#include <string>

namespace wfg::client::model
{
    Mark flagMark()
    {
        return { Icon::flag, {}, "flagged: somebody marked it to look at afterwards - F on it takes the flag off" };
    }

    std::set<std::string> flaggedOf (const tree::TreeSnapshot& snapshot, const std::vector<Row>& rows)
    {
        std::set<std::string> out;

        for (const auto& row : rows)
        {
            if (row.rowKind != RowKind::cue || row.derived || row.id.empty())
                continue;

            if (isYes (flag (snapshot, "/godot/cue/" + row.id + "/flagged")))
                out.insert (row.id);
        }

        return out;
    }

    namespace
    {
        /*  "12 Rain", "Rain", or "12" - how the foot names one cue, as the
            list shows it. */
        std::string named (const std::string& cueId, const std::vector<Row>& rows)
        {
            const auto found = std::find_if (rows.begin(), rows.end(), [&cueId] (const Row& row)
                                             { return row.rowKind == RowKind::cue && ! row.derived && row.id == cueId; });

            if (found == rows.end())
                return "the picked cue";

            const auto& name = found->name.empty() ? std::string ("(unnamed)") : found->name;
            return found->number.empty() ? name : found->number + " " + name;
        }
    }

    FlagToggle flagToggle (const std::set<std::string>& flagged, const std::vector<std::string>& picked,
                           const std::vector<Row>& rows)
    {
        FlagToggle out;

        if (picked.empty())
            return out;

        out.flagging = ! std::all_of (picked.begin(), picked.end(),
                                      [&flagged] (const std::string& id) { return flagged.count (id) > 0; });

        for (const auto& id : picked)
            if ((flagged.count (id) > 0) != out.flagging)
                out.writes.emplace_back ("/godot/cue/" + id + "/flagged", out.flagging ? "true" : "false");

        const auto who = picked.size() == 1 ? named (picked.front(), rows)
                                            : std::to_string (picked.size()) + " cues";

        out.said = out.flagging ? "Flagged " + who + " to look at afterwards"
                                : "Flag taken off " + who;
        return out;
    }

    std::string nothingPickedToFlag()
    {
        return "Nothing picked to flag: pick a cue and press F, or Shift+F flags the cue the last GO fired";
    }
}
