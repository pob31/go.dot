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

#include <algorithm>
#include <cstddef>

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
            return row.kind == "fade" || row.kind == "transport";
        }
    }

    std::string dropTone (DropKind kind)
    {
        switch (kind)
        {
            case DropKind::target:      return "drop-aim";
            case DropKind::preset:      return "drop-header";
            case DropKind::header:      return "drop-header";
            case DropKind::footer:      return "drop-footer";

            case DropKind::into:
            case DropKind::none:
            case DropKind::after:
            case DropKind::clearPreset: break;
        }

        return "drop-into";
    }

    std::string containerOf (const Row& row)
    {
        return row.section == Section::member || row.sectionId.empty() ? row.parent : row.sectionId;
    }

    Drop presetLineDropFor (const Row* over, const std::string& cueId, const std::string& current,
                            const std::vector<Row>& rows)
    {
        Drop drop;
        drop.cueId = cueId;

        //  The group under the hand: a group's own row, or a header band's group.
        std::string group;

        if (over != nullptr)
        {
            if (over->rowKind == RowKind::cue && over->isGroup && ! over->derived)
                group = over->id;
            else if (over->rowKind == RowKind::band && over->section == Section::header)
                group = over->parent;
        }

        if (! group.empty())
        {
            if (group == current)
                return {};                                   // where it already is

            const auto ancestors = ancestorsOf (cueId, rows);

            if (std::find (ancestors.begin(), ancestors.end(), group) != ancestors.end())
            {
                drop.kind = DropKind::preset;
                drop.cueId = group;
                return drop;
            }
        }

        //  Anywhere else is out of the header: the mark goes.
        drop.kind = DropKind::clearPreset;
        return drop;
    }

    Drop footerDropFor (const Row& over, const Row& dragged)
    {
        Drop drop;

        if (over.rowKind != RowKind::cue || ! over.isGroup || over.id == dragged.id)
            return drop;

        drop.kind = DropKind::footer;
        drop.cueId = over.id;
        return drop;
    }

    Drop dropFor (const Row& over, const Row& dragged, double fraction)
    {
        Drop drop;

        /*  A SECTION'S BAND TAKES THE CUE INTO THAT SECTION (author,
            2026-09-18: "drag and drop directly in the footer if it already
            exists"): the header, the footer or the persistent section, at
            its end. */
        if (over.rowKind == RowKind::band)
        {
            if (! over.sectionId.empty())
            {
                drop.kind = DropKind::into;
                drop.container = over.sectionId;
                drop.index = -1;
                return drop;
            }

            /*  AND A BAND WITH NO SECTION YET MAKES ONE (author, 2026-09-22:
                "drag and drop to an empty group header or footer is not
                working").

                It used to take nothing, which was right while such a band was
                only ever drawn for a section that had been emptied - there was
                nothing on screen to aim at either way. Now the list grows the
                two bands of whichever group is being dragged over, so the band
                IS the target, and letting go on it has to make the section the
                cue is being put into. The footer already had that path from a
                group title (`moveToFooter`: the role, then the move, queued
                until the section lands); the header now has the same. */
            if (! over.parent.empty() && over.parent != dragged.id)
            {
                drop.kind = over.section == Section::header ? DropKind::header
                                                            : DropKind::footer;
                drop.cueId = over.parent;
            }

            return drop;
        }

        if (over.rowKind != RowKind::cue || over.id.empty() || over.id == dragged.id || over.derived)
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

        /*  AFTER, in the row's own container: a member's parent, or the
            section a header, footer or persistent row is in - which is a
            container `object.move` names like any other, now that the tree
            says what each section is called. A section row whose section is
            not published takes the cue to the end of the group's members. */
        drop.kind = DropKind::after;
        drop.container = containerOf (over);

        if (over.section != Section::member && over.sectionId.empty())
        {
            drop.index = -1;
            return drop;
        }

        /*  THE POSITION `object.move` WANTS. Within one container the
            dragged cue is still counted: dropped after a member below it,
            asking for that member's own position lands the cue directly after
            it; after a member above it, or from elsewhere, the position after
            the member is the one. */
        const auto sameContainer = containerOf (dragged) == containerOf (over);
        const auto movingLater = sameContainer && dragged.indexInParent < over.indexInParent;

        drop.index = movingLater ? over.indexInParent : over.indexInParent + 1;

        //  Directly after itself already: nothing to move.
        if (sameContainer && dragged.indexInParent == over.indexInParent + 1)
            drop.kind = DropKind::none;

        return drop;
    }

    const Row* endingAt (const std::vector<Row>& rows, std::size_t at, int depth)
    {
        if (at >= rows.size())
            return nullptr;

        const auto& row = rows[at];

        if (depth >= row.depth)
            return &row;

        //  The shallowest the line can reach: the next row's depth, the list's past the end.
        const auto next = at + 1 < rows.size() ? rows[at + 1].depth : 0;

        const Row* chosen = &row;

        for (const auto& id : ancestorsOf (row.id, rows))       // innermost first
        {
            const Row* group = nullptr;

            for (const auto& candidate : rows)
                if (candidate.rowKind == RowKind::cue && candidate.id == id && ! candidate.derived)
                {
                    group = &candidate;
                    break;
                }

            //  A group the next row is still inside does not end here, and nor does any outside it.
            if (group == nullptr || next > group->depth)
                break;

            chosen = group;

            if (group->depth <= depth)
                break;
        }

        return chosen;
    }

    Drop dropAtDepth (const std::vector<Row>& rows, std::size_t at, const Row& dragged,
                      double fraction, int depth, int* landed)
    {
        if (at >= rows.size())
            return {};

        const auto& over = rows[at];
        const auto onIt = inOnBand (fraction) && (aimable (over) || over.isGroup);

        /*  OUT OF THE GROUP WHEN THE HAND IS LEFT OF IT: after the group the
            row ends, in the group's own container - and so also when the row
            is the dragged cue itself, which is how the last member of a group
            is taken out to sit directly below it. */
        if (over.rowKind == RowKind::cue && ! over.derived && ! onIt)
            if (const auto* ending = endingAt (rows, at, depth); ending != nullptr && ending != &over)
            {
                if (landed != nullptr)
                    *landed = ending->depth;

                return dropFor (*ending, dragged, 1.0);
            }

        if (landed != nullptr)
            *landed = over.depth;

        return dropFor (over, dragged, fraction);
    }

    FileDrop fileDropAt (const std::vector<Row>& rows, std::size_t at, double fraction, int depth)
    {
        FileDrop drop;

        if (at >= rows.size())
            return drop;

        const auto& over = rows[at];

        //  A container said by its name: a group's own, or the list's.
        const auto nameOf = [&rows] (const std::string& id)
        {
            for (const auto& row : rows)
                if (row.rowKind == RowKind::cue && row.id == id && ! row.derived)
                    return row.name.empty() ? row.id : row.name;

            return std::string ("the list");
        };

        const auto ownCue = over.rowKind == RowKind::cue && ! over.derived;

        if (ownCue && over.isGroup)
        {
            if (inOnBand (fraction))
            {
                drop.parent = over.id;
                drop.lit = true;
                drop.words = "into " + nameOf (over.id) + ", at the end";
                return drop;
            }

            if (! over.shut)
            {
                drop.parent = over.id;
                drop.index = 0;
                drop.depth = over.depth + 1;
                drop.words = "first in " + nameOf (over.id);
                return drop;
            }
        }

        /*  AFTER A MEMBER, or after the group it ends when the hand is left of
            it. A section row, or a group in a section, has no member position
            to be after, and falls through to the end. */
        const auto* after = ownCue && over.section == Section::member ? endingAt (rows, at, depth) : nullptr;

        if (after != nullptr && after->section == Section::member)
        {
            drop.parent = after->parent;
            drop.index = after->indexInParent + 1;
            drop.depth = after->depth;
            drop.words = "after " + (after->name.empty() ? after->id : after->name);
            return drop;
        }

        const auto& container = after != nullptr ? after->parent : over.parent;

        drop.parent = container;
        drop.words = "at the end of " + nameOf (container);
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

    std::vector<std::string> ancestorsOf (const std::string& cueId, const std::vector<Row>& rows)
    {
        /*  THE CUE'S OWN ROW, never a derived line of it: a derived line sits
            under the header of the group it names, and walking up from there
            would skip every group between the cue and that header. */
        const auto rowOf = [&rows] (const std::string& id) -> const Row*
        {
            for (const auto& row : rows)
                if (row.rowKind == RowKind::cue && row.id == id && ! row.derived)
                    return &row;

            return nullptr;
        };

        std::vector<std::string> out;
        const auto* at = rowOf (cueId);

        //  Bounded by the row count, so a cycle nothing publishes cannot hang it.
        for (std::size_t steps = 0; at != nullptr && steps < rows.size(); ++steps)
        {
            const auto* parent = rowOf (at->parent);

            if (parent == nullptr || ! parent->isGroup)
                break;

            out.push_back (parent->id);
            at = parent;
        }

        return out;
    }

    Drop presetDropFor (const Row& over, const Row& dragged, const std::vector<Row>& rows)
    {
        Drop drop;

        if (over.rowKind != RowKind::cue || ! over.isGroup || over.id == dragged.id)
            return drop;

        const auto ancestors = ancestorsOf (dragged.id, rows);

        if (std::find (ancestors.begin(), ancestors.end(), over.id) == ancestors.end())
            return drop;

        drop.kind = DropKind::preset;
        drop.cueId = over.id;
        return drop;
    }

    std::optional<std::string> presetStep (const std::string& cueId, const std::string& current,
                                           int direction, const std::vector<Row>& rows)
    {
        const auto ancestors = ancestorsOf (cueId, rows);   // innermost first

        if (ancestors.empty() || direction == 0)
            return std::nullopt;

        const auto found = std::find (ancestors.begin(), ancestors.end(), current);
        const auto at = current.empty() || found == ancestors.end()
                          ? -1
                          : static_cast<int> (found - ancestors.begin());

        if (direction > 0)
        {
            //  Outward: from none to the innermost, then further out; the outermost stays.
            if (at + 1 >= static_cast<int> (ancestors.size()))
                return std::nullopt;

            return ancestors[static_cast<std::size_t> (at + 1)];
        }

        //  Inward: from the innermost to none; none stays none.
        if (at < 0)
            return std::nullopt;

        if (at == 0)
            return std::string {};

        return ancestors[static_cast<std::size_t> (at - 1)];
    }

    std::string editAttributeFor (EditCell cell, const std::string& kind)
    {
        switch (cell)
        {
            case EditCell::number:   return "number";
            case EditCell::name:     return "name";
            case EditCell::preWait:  return "preWait";
            case EditCell::postWait: return "postWait";
            case EditCell::duration: return kind == "fade" || kind == "transport" ? "duration" : "";
            case EditCell::none:     break;
        }

        return {};
    }

    std::string describe (const Drop& drop, const Row& over, bool intoTimeline)
    {
        const auto name = over.name.empty() ? over.id : over.name;
        const auto timelineNote = intoTimeline
            ? std::string (" - a timeline group: its members start together, each after its own "
                           "pre-wait, whatever their order")
            : std::string {};

        switch (drop.kind)
        {
            case DropKind::none:    return {};
            case DropKind::after:   return "after " + name + timelineNote;
            case DropKind::into:    return "into " + name + timelineNote;
            case DropKind::target:  return "aim " + name + " at it";
            case DropKind::preset:  return "prepare it in " + name + "'s header";
            case DropKind::header:  return "into " + name + "'s header";
            case DropKind::footer:  return "into " + name + "'s footer";
            case DropKind::clearPreset: return "no longer prepared ahead";
        }

        return {};
    }
}
