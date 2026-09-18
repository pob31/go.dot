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

#include <wfg/client/model/Selection.h>

#include <algorithm>

namespace wfg::client::model
{
    namespace
    {
        int cueRowIndex (const std::vector<Row>& rows, const std::string& id)
        {
            for (std::size_t at = 0; at < rows.size(); ++at)
                if (rows[at].rowKind == RowKind::cue && rows[at].id == id)
                    return static_cast<int> (at);

            return -1;
        }
    }

    void Selection::click (const std::string& id, bool extend, bool toggle,
                           const std::vector<Row>& rows)
    {
        if (id.empty())
            return;

        if (extend && ! anchorId.empty())
        {
            /*  EVERYTHING BETWEEN THE ANCHOR AND HERE, in the drawn order,
                cues only. Added to what is picked rather than replacing it,
                which is what shift means in every list; the anchor stays,
                so a second shift-click re-aims the range from the same end. */
            const auto from = cueRowIndex (rows, anchorId);
            const auto to = cueRowIndex (rows, id);

            if (from < 0 || to < 0)
            {
                set (id);
                return;
            }

            for (auto at = std::min (from, to); at <= std::max (from, to); ++at)
            {
                const auto& row = rows[static_cast<std::size_t> (at)];

                if (row.rowKind == RowKind::cue && ! contains (row.id))
                    chosen.push_back (row.id);
            }

            return;
        }

        if (toggle)
        {
            const auto found = std::find (chosen.begin(), chosen.end(), id);

            if (found == chosen.end())
            {
                chosen.push_back (id);
                anchorId = id;
                return;
            }

            chosen.erase (found);

            //  The anchor follows what is still picked, or goes.
            if (anchorId == id)
                anchorId = chosen.empty() ? std::string {} : chosen.back();

            return;
        }

        set (id);
    }

    void Selection::set (const std::string& id)
    {
        chosen.clear();
        anchorId.clear();

        if (id.empty())
            return;

        chosen.push_back (id);
        anchorId = id;
    }

    void Selection::all (const std::vector<Row>& rows)
    {
        chosen.clear();

        for (const auto& row : rows)
            if (row.rowKind == RowKind::cue && ! row.id.empty())
                chosen.push_back (row.id);

        if (! contains (anchorId))
            anchorId = chosen.empty() ? std::string {} : chosen.front();
    }

    void Selection::clear()
    {
        chosen.clear();
        anchorId.clear();
    }

    void Selection::retain (const std::vector<Row>& rows)
    {
        chosen.erase (std::remove_if (chosen.begin(), chosen.end(),
                                      [&rows] (const std::string& id)
                                      {
                                          return cueRowIndex (rows, id) < 0;
                                      }),
                      chosen.end());

        if (! contains (anchorId))
            anchorId = chosen.empty() ? std::string {} : chosen.back();
    }

    bool Selection::contains (const std::string& id) const
    {
        return std::find (chosen.begin(), chosen.end(), id) != chosen.end();
    }
}
