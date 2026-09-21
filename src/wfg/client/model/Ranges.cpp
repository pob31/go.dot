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

#include <wfg/client/model/Ranges.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wfg::client::model
{
    namespace
    {
        constexpr std::string_view prefix = "/godot/range/";

        double seconds (const std::string& reading)
        {
            /*  Through the OSC reader, which is the one locale-independent
                parse this project has: a French locale must not make 0.5
                unreadable, and `osc::formatDouble` is what wrote it. */
            return osc::parseDouble (reading).value_or (0.0);
        }

        int count (const std::string& reading, int fallback)
        {
            const auto parsed = osc::parseDouble (reading);
            return parsed.has_value() ? static_cast<int> (*parsed) : fallback;
        }
    }

    bool sameInstant (double a, double b) noexcept
    {
        /*  A millisecond. Ranges are written in seconds by a hand on a bar, so
            anything closer than this is the same edge as far as a designer is
            concerned - and the engine stores what the hand produced rather
            than a rounded copy, so exact equality would miss joins that were
            made by dragging one edge onto another. */
        return std::abs (a - b) < 0.001;
    }

    std::map<std::string, std::vector<RangeRow>> rangesByCue (const tree::TreeSnapshot& snapshot)
    {
        /*  ONE PASS, gathering by identifier. `childrenOf` walks the whole tree
            per call and is banned for it; this is the shape `inspect` and
            `readOutputs` both use. */
        std::map<std::string, RangeRow> found;
        std::map<std::string, std::string> ownerOf;

        for (const auto* node : snapshot.all())
        {
            if (node->address.rfind (prefix, 0) != 0)
                continue;

            const auto rest = node->address.substr (prefix.size());
            const auto slash = rest.find ('/');

            if (slash == std::string::npos)
                continue;

            const auto id = rest.substr (0, slash);
            const auto name = rest.substr (slash + 1);
            const auto reading = text (node);

            auto& row = found[id];
            row.id = id;

            if (name == "cue")         ownerOf[id] = reading;
            else if (name == "name")   row.name = reading;
            else if (name == "in")     row.in = seconds (reading);
            else if (name == "out")    row.out = seconds (reading);
            else if (name == "loops")  row.loops = count (reading, 1);
            else if (name == "index")  row.index = count (reading, 0);
        }

        /*  A RANGE WITH NO OWNER IS NOBODY'S. One whose `cue` never arrived in
            the pass - because the snapshot is mid-publish - is dropped rather
            than shown against the wrong cue: an editor that drew somebody
            else's regions over this file would be inviting an edit to the
            wrong show. */
        std::map<std::string, std::vector<RangeRow>> out;

        for (auto& [id, row] : found)
        {
            const auto owner = ownerOf.find (id);

            if (owner == ownerOf.end() || owner->second.empty())
                continue;

            out[owner->second].push_back (row);
        }

        //  Document order is playlist order (§3.24), and `index` is what says so.
        for (auto& [owner, rows] : out)
            std::stable_sort (rows.begin(), rows.end(),
                              [] (const RangeRow& a, const RangeRow& b)
                              {
                                  if (a.index != b.index)
                                      return a.index < b.index;

                                  return a.id < b.id;
                              });

        return out;
    }

    std::vector<RangeRow> readRanges (const tree::TreeSnapshot& snapshot, const std::string& cueId)
    {
        if (cueId.empty())
            return {};

        auto all = rangesByCue (snapshot);
        const auto found = all.find (cueId);

        return found == all.end() ? std::vector<RangeRow> {} : std::move (found->second);
    }

    Hit hitTest (const std::vector<RangeRow>& rows, double seconds_, double tolerance)
    {
        Hit best;
        auto nearest = tolerance;

        /*  SLICES FIRST, and that order is the whole point. Where one range
            ends and the next begins the two handles are on the same instant,
            so whichever were tested first would win and the other would never
            be reachable - and moving one of them alone tears a loop open. */
        for (std::size_t at = 0; at + 1 < rows.size(); ++at)
        {
            if (! sameInstant (rows[at].out, rows[at + 1].in))
                continue;

            const auto distance = std::abs (rows[at].out - seconds_);

            if (distance <= nearest)
            {
                nearest = distance;
                best = { Handle::slice, rows[at].id, rows[at + 1].id };
            }
        }

        if (best.handle != Handle::none)
            return best;

        for (const auto& row : rows)
        {
            if (const auto distance = std::abs (row.in - seconds_); distance <= nearest)
            {
                nearest = distance;
                best = { Handle::in, row.id, {} };
            }

            if (const auto distance = std::abs (row.out - seconds_); distance <= nearest)
            {
                nearest = distance;
                best = { Handle::out, row.id, {} };
            }
        }

        return best;
    }

    std::string rangeAddress (const std::string& rangeId, const char* attribute)
    {
        return std::string (prefix) + rangeId + "/" + attribute;
    }

    std::string timeText (double secondsIn)
    {
        /*  TO THE MILLISECOND, which is `sameInstant`'s own resolution: a
            table that showed more than the editor can tell apart would invite
            an edit nobody could see the effect of. */
        const auto rounded = std::round (std::max (0.0, secondsIn) * 1000.0) / 1000.0;
        const auto whole = static_cast<long long> (std::floor (rounded));

        if (whole < 60)
            return osc::formatDouble (rounded);

        const auto minutes = whole / 60;

        /*  ROUNDED AGAIN AFTER THE SUBTRACTION. 187.4 less three minutes is
            7.400000000000006 in binary, and `formatDouble` is honest enough to
            print all of it - which is the true number and nobody's idea of a
            time. The millisecond is the resolution this table works at, and
            the arithmetic has to be taken back to it twice. */
        const auto rest = std::round ((rounded - static_cast<double> (minutes * 60)) * 1000.0)
                            / 1000.0;
        const auto body = osc::formatDouble (rest);

        return std::to_string (minutes) + ":" + (rest < 10.0 ? "0" + body : body);
    }

    std::optional<double> timeFrom (const std::string& typed)
    {
        const auto first = typed.find_first_not_of (" \t");

        if (first == std::string::npos)
            return std::nullopt;

        const auto trimmed = typed.substr (first, typed.find_last_not_of (" \t") - first + 1);
        const auto colon = trimmed.find (':');

        if (colon == std::string::npos)
        {
            const auto plain = osc::parseDouble (trimmed);
            return plain.has_value() && *plain >= 0.0 ? plain : std::nullopt;
        }

        const auto minutes = osc::parseDouble (trimmed.substr (0, colon));
        const auto rest = osc::parseDouble (trimmed.substr (colon + 1));

        if (! minutes.has_value() || ! rest.has_value())
            return std::nullopt;

        /*  "1:75" is not a time. Sixty-one seconds into a minute is a typing
            slip and not a minute and a quarter, and guessing which it was
            would be the panel deciding what somebody meant. */
        if (*minutes < 0.0 || *rest < 0.0 || *rest >= 60.0)
            return std::nullopt;

        return *minutes * 60.0 + *rest;
    }

    std::vector<RangeWrite> copyLengthToNext (const std::vector<RangeRow>& rows, std::size_t index,
                                              double fileLength)
    {
        if (index + 1 >= rows.size())
            return {};

        const auto& from = rows[index];
        const auto& next = rows[index + 1];

        if (! (from.length() > 0.0))
            return {};

        const auto wanted = next.in + from.length();

        if (fileLength > 0.0 && wanted > fileLength)
            return {};

        return { { next.id, "out", wanted } };
    }

    RangeAdd addAt (const std::vector<RangeRow>& rows, double seconds, double fileLength,
                    double smallest)
    {
        RangeAdd out;

        if (! (fileLength > 0.0))
        {
            out.why = "the length of this file is not known yet";
            return out;
        }

        /*  NOTHING TO DIVIDE AT EITHER END OF THE MATERIAL. A cut at nought
            would put a range of no length before it and a cut at the end one
            after it, and neither is a thing anybody meant to make. */
        if (sameInstant (seconds, 0.0) || sameInstant (seconds, fileLength)
              || seconds < 0.0 || seconds > fileLength)
        {
            out.why = "the playhead is at the edge of the file - move it into the sound";
            return out;
        }

        //  A cue that has said nothing yet: the whole file becomes its one range.
        if (rows.empty())
        {
            out.kind = RangeAdd::Kind::create;
            out.in = 0.0;
            out.out = fileLength;
            return out;
        }

        /*  ALREADY A CUT HERE. Tested before the containment below, because an
            instant that is a range's edge is inside no range and the useful
            answer is the reason rather than the fall-through. */
        for (const auto& row : rows)
            if (sameInstant (seconds, row.in) || sameInstant (seconds, row.out))
            {
                out.why = "there is already a cut here";
                return out;
            }

        for (const auto& row : rows)
            if (seconds > row.in && seconds < row.out)
            {
                out.kind = RangeAdd::Kind::split;
                out.at = seconds;
                return out;
            }

        /*  IN A GAP BETWEEN REGIONS, which §3.24 allows: the ranges of a cue
            need be neither contiguous nor in file order. The new one runs from
            the head to whatever the file offers next - the nearest in-point
            after it, or the end. */
        auto until = fileLength;

        for (const auto& row : rows)
            if (row.in > seconds)
                until = std::min (until, row.in);

        if (until - seconds < smallest)
        {
            out.why = "there is no room here for a range";
            return out;
        }

        out.kind = RangeAdd::Kind::create;
        out.in = seconds;
        out.out = until;

        return out;
    }

    std::optional<std::pair<double, double>> nextRange (const std::vector<RangeRow>& rows,
                                                        double fileLength, double smallest)
    {
        if (! (fileLength > 0.0))
            return std::nullopt;

        /*  AFTER THE LAST ONE IN FILE ORDER, which is not always the last one
            in the list: §3.24 lets a cue walk its file out of order, so "the
            end of the playlist" and "the end of the material used" are two
            different instants and only the second leaves room. */
        auto used = 0.0;

        for (const auto& row : rows)
            used = std::max (used, row.out);

        if (fileLength - used < smallest)
            return std::nullopt;

        return std::make_pair (used, fileLength);
    }

    std::vector<RangeWrite> dragTo (const Hit& hit, double seconds_,
                                    const std::vector<RangeRow>& rows, double fileLength,
                                    double smallest)
    {
        const auto find = [&rows] (const std::string& id) -> const RangeRow*
        {
            for (const auto& row : rows)
                if (row.id == id)
                    return &row;

            return nullptr;
        };

        const auto inside = [fileLength] (double value)
        {
            return std::min (std::max (value, 0.0), fileLength > 0.0 ? fileLength : value);
        };

        switch (hit.handle)
        {
            case Handle::none:
                break;

            case Handle::in:
            {
                const auto* row = find (hit.rangeId);

                if (row == nullptr)
                    break;

                /*  A range is never dragged through its own far edge: below
                    `smallest` the drag stops, because a range of no length has
                    no handle left to pull it back out by. */
                const auto wanted = std::min (inside (seconds_), row->out - smallest);

                if (wanted < 0.0)
                    break;

                return { { row->id, "in", wanted } };
            }

            case Handle::out:
            {
                const auto* row = find (hit.rangeId);

                if (row == nullptr)
                    break;

                const auto wanted = std::max (inside (seconds_), row->in + smallest);

                if (fileLength > 0.0 && wanted > fileLength)
                    break;

                return { { row->id, "out", wanted } };
            }

            case Handle::slice:
            {
                const auto* earlier = find (hit.rangeId);
                const auto* later = find (hit.nextId);

                if (earlier == nullptr || later == nullptr)
                    break;

                /*  BOTH SIDES, and the join stays a join. It may not be pushed
                    through either neighbour's far edge, or one of the two would
                    be turned inside out by a gesture aimed at the other. */
                const auto wanted = std::min (std::max (inside (seconds_), earlier->in + smallest),
                                              later->out - smallest);

                if (! (wanted > earlier->in) || ! (wanted < later->out))
                    break;

                return { { earlier->id, "out", wanted }, { later->id, "in", wanted } };
            }
        }

        return {};
    }

    std::vector<double> snapTargets (const std::vector<RangeRow>& rows, const std::string& movingId,
                                     double fileLength)
    {
        std::vector<double> targets { 0.0 };

        if (fileLength > 0.0)
            targets.push_back (fileLength);

        for (const auto& row : rows)
        {
            /*  NOT THE EDGE BEING MOVED, or it would snap to where it already
                is and never leave. Its OWN far edge stays in the list: a range
                dragged shut against itself is stopped by `smallest` rather than
                by the absence of a target. */
            if (row.id == movingId)
                continue;

            targets.push_back (row.in);
            targets.push_back (row.out);
        }

        std::sort (targets.begin(), targets.end());
        targets.erase (std::unique (targets.begin(), targets.end(),
                                    [] (double a, double b) { return sameInstant (a, b); }),
                       targets.end());

        return targets;
    }

    double snapTo (double seconds_, const std::vector<double>& targets, double tolerance)
    {
        auto best = seconds_;
        auto nearest = tolerance;

        for (const auto target : targets)
            if (const auto distance = std::abs (target - seconds_); distance <= nearest)
            {
                nearest = distance;
                best = target;
            }

        return best;
    }
}
