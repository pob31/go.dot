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

#include <wfg/client/model/UndoHistory.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <cstddef>
#include <string>

namespace wfg::client::model
{
    namespace
    {
        std::vector<std::string> words (const std::string& text)
        {
            std::vector<std::string> out;
            std::size_t at = 0;

            while (at < text.size())
            {
                const auto space = text.find (' ', at);
                const auto word = text.substr (at, space == std::string::npos ? std::string::npos
                                                                                : space - at);
                at = space == std::string::npos ? text.size() : space + 1;

                if (! word.empty())
                    out.push_back (word);
            }

            return out;
        }
    }

    UndoReading readUndoHistory (const tree::TreeSnapshot& snapshot)
    {
        UndoReading reading;
        reading.undo = words (text (snapshot, "/godot/document/undoHistory"));
        reading.redo = words (text (snapshot, "/godot/document/redoHistory"));
        return reading;
    }

    std::vector<Standing> standings (const UndoReading& reading)
    {
        std::vector<Standing> out;
        const auto applied = reading.position();

        /*  THE FUTURE FIRST, furthest first: Redo's list is nearest first,
            so it is walked backwards to put the newest at the top. */
        for (std::size_t n = reading.redo.size(); n-- > 0;)
        {
            Standing standing;
            standing.name = reading.redo[n];
            standing.index = applied + static_cast<int> (n) + 1;
            standing.applied = false;
            out.push_back (standing);
        }

        for (std::size_t n = 0; n < reading.undo.size(); ++n)
        {
            Standing standing;
            standing.name = reading.undo[n];
            standing.index = applied - static_cast<int> (n);
            standing.applied = true;
            out.push_back (standing);
        }

        Standing opening;
        opening.name = "as opened";
        opening.index = 0;
        opening.applied = true;
        opening.opening = true;
        out.push_back (opening);

        return out;
    }

    Picture pictureOf (const std::vector<Row>& rows)
    {
        Picture picture;

        for (const auto& row : rows)
        {
            if (row.rowKind != RowKind::cue || row.derived || row.id.empty())
                continue;

            /*  WHAT THE ROW SAYS, as one string: every column the list draws,
                and where the cue stands. A move shows as a change of parent
                or place; a rename, a retimed wait, a preset mark all show. */
            picture.saying[row.id] = row.number + "|" + row.name + "|" + row.kind + "|"
                                   + row.preWait + "|" + row.duration + "|" + row.postWait + "|"
                                   + row.mode + "|" + row.selection + "|" + row.loops + "|"
                                   + row.preset + "|" + row.parent + "|"
                                   + std::to_string (row.indexInParent) + "|"
                                   + (row.enabled ? "on" : "off");
            picture.name[row.id] = row.name;
        }

        return picture;
    }

    Diff diff (const Picture& before, const Picture& now)
    {
        Diff out;

        for (const auto& [id, saying] : now.saying)
        {
            const auto was = before.saying.find (id);

            if (was == before.saying.end())
                out.added.push_back (id);
            else if (was->second != saying)
                out.changed.push_back (id);
        }

        for (const auto& [id, saying] : before.saying)
            if (now.saying.count (id) == 0)
            {
                const auto name = before.name.find (id);
                out.removed.push_back (name != before.name.end() && ! name->second.empty()
                                         ? name->second : id);
            }

        return out;
    }
}
