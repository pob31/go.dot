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

#include <wfg/client/model/Reorder.h>

namespace wfg::client::model
{
    namespace
    {
        /*  The band of a row that means ON it rather than AFTER it: the middle
            two fifths. Wide enough to hit without aiming, narrow enough that
            a hand sliding down a list of fades is not aiming every one. */
        constexpr double onBandTop = 0.3;
        constexpr double onBandBottom = 0.7;

        bool inOnBand (double fraction) noexcept
        {
            return fraction >= onBandTop && fraction <= onBandBottom;
        }

        bool aimable (const Row& row) noexcept
        {
            return row.kind == "fade" || row.kind == "stop";
        }
    }

    Drop dropFor (const Row& over, const Row& dragged, double fraction)
    {
        Drop drop;

        if (over.rowKind != RowKind::cue || over.id.empty() || over.id == dragged.id)
            return drop;

        if (inOnBand (fraction))
        {
            if (aimable (over))
            {
                drop.kind = DropKind::target;
                drop.cueId = over.id;
                return drop;
            }

            if (over.isGroup)
            {
                drop.kind = DropKind::into;
                drop.container = over.id;
                drop.index = -1;
                return drop;
            }
        }

        /*  AFTER, in the row's own container - or at the end of it when the
            row is a header, a footer or a persistent cue, which have no member
            position to be after (the dropped-file rule, CueListComponent). */
        drop.kind = DropKind::after;
        drop.container = over.parent;

        if (over.section != Section::member)
        {
            drop.index = -1;
            return drop;
        }

        /*  THE POSITION `object.move` WANTS. Within one container the
            dragged cue is still counted: dropped after a member below it,
            asking for that member's own position lands the cue directly after
            it; after a member above it, or from elsewhere, the position after
            the member is the one. */
        const auto sameContainer = dragged.parent == over.parent;
        const auto movingLater = sameContainer && dragged.indexInParent < over.indexInParent;

        drop.index = movingLater ? over.indexInParent : over.indexInParent + 1;

        //  Directly after itself already: nothing to move.
        if (sameContainer && dragged.indexInParent == over.indexInParent + 1)
            drop.kind = DropKind::none;

        return drop;
    }

    std::string resolveCueRef (const std::string& text, const std::vector<Row>& rows)
    {
        if (text.empty())
            return {};

        for (const auto& row : rows)
            if (row.rowKind == RowKind::cue && row.id == text)
                return row.id;

        std::string found;

        for (const auto& row : rows)
        {
            if (row.rowKind != RowKind::cue || row.number.empty() || row.number != text)
                continue;

            if (! found.empty() && found != row.id)
                return {};

            found = row.id;
        }

        if (! found.empty())
            return found;

        for (const auto& row : rows)
        {
            if (row.rowKind != RowKind::cue || row.name.empty() || row.name != text)
                continue;

            if (! found.empty() && found != row.id)
                return {};

            found = row.id;
        }

        return found;
    }

    std::string describe (const Drop& drop, const Row& over)
    {
        const auto name = over.name.empty() ? over.id : over.name;

        switch (drop.kind)
        {
            case DropKind::none:    return {};
            case DropKind::after:   return "after " + name;
            case DropKind::into:    return "into " + name;
            case DropKind::target:  return "aim " + name + " at it";
        }

        return {};
    }
}
