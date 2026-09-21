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

#include <wfg/client/model/OutputList.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::client::model
{
    namespace
    {
        constexpr std::string_view prefix = "/godot/bus/";

        /*  `reading` and not `text`, which is the free function three lines
            below this one: GCC's -Wshadow reports a parameter shadowing a
            global declaration, and the repo's convention is to rename the
            parameter rather than the thing it hid. */
        int number (const std::string& reading, int fallback)
        {
            /*  The tree's own text, which came through `osc::formatDouble` or
                an integer, so this is never a locale question - but it is
                still a place a malformed reading must not throw. */
            try
            {
                return reading.empty() ? fallback : std::stoi (reading);
            }
            catch (...)
            {
                return fallback;
            }
        }
    }

    std::string OutputRow::kindWord() const
    {
        return kind == "mix" ? "Mix channel" : "Direct out";
    }

    std::string OutputRow::widthWord() const
    {
        if (width == 1) return "Mono";
        if (width == 2) return "Stereo";

        return std::to_string (width) + " channels";
    }

    std::string OutputRow::channelWord() const
    {
        /*  COUNTED FROM ONE, because that is how a patch panel and an
            interface are labelled and this column is read beside them. Every
            address inside the engine counts from nought, and the two meet
            here. */
        const auto first = firstChannel + 1;

        if (width <= 1)
            return std::to_string (first);

        return std::to_string (first) + "-" + std::to_string (firstChannel + width);
    }

    std::vector<OutputRow> readOutputs (const tree::TreeSnapshot& snapshot)
    {
        /*  ONE PASS over the tree, gathering by identifier, the way `inspect`
            does: `childrenOf` walks the whole tree per call and is banned for
            it. */
        std::map<std::string, OutputRow> found;

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

            auto& row = found[id];
            row.id = id;

            if (name == "name")              row.name = text (node);
            else if (name == "kind")         row.kind = text (node);
            else if (name == "width")        row.width = number (text (node), 1);
            else if (name == "firstChannel") row.firstChannel = number (text (node), 0);
        }

        std::vector<OutputRow> rows;
        rows.reserve (found.size());

        for (auto& [id, row] : found)
        {
            if (row.kind.empty())
                row.kind = "direct";

            /*  A bus with no name is still a row somebody has to point at, so
                it gets one here rather than being drawn blank. The engine
                names every bus it creates; this is for a show written by
                hand. */
            if (row.name.empty())
                row.name = "Output " + std::to_string (row.firstChannel + 1);

            rows.push_back (row);
        }

        /*  Channel order, ties by identifier so the list never flickers
            between two readings of the same show. The engine keeps document
            order the same; a show written by hand may not, and the list is
            read beside an interface rather than beside a file. */
        std::stable_sort (rows.begin(), rows.end(),
                          [] (const OutputRow& a, const OutputRow& b)
                          {
                              if (a.firstChannel != b.firstChannel)
                                  return a.firstChannel < b.firstChannel;

                              return a.id < b.id;
                          });

        return rows;
    }

    int outputChannelCount (const std::vector<OutputRow>& rows)
    {
        auto total = 0;

        for (const auto& row : rows)
            total = std::max (total, row.firstChannel + row.width);

        return total;
    }

    bool patchHasSettled (const tree::TreeSnapshot& snapshot)
    {
        if (isYes (flag (snapshot, "/godot/audio/patchSettled")))
            return true;

        if (! text (snapshot, "/godot/audio/outputPatch").empty())
            return true;

        /*  AND A LAYOUT THAT IS NOT PACKED, which is the third of
            `doc::applyLayoutEdit`'s three and the one a client would forget:
            a show written by hand with a hole in its channels is already
            settled, and telling the designer it will follow their edits
            would be a promise the engine does not keep. */
        auto expected = 0;

        for (const auto& row : readOutputs (snapshot))
        {
            if (row.firstChannel != expected)
                return true;

            expected += row.width;
        }

        return false;
    }

    std::string outputRegime (bool settled)
    {
        if (settled)
            return "The interface patch is set: each output keeps the channels it is on, "
                   "and a new one takes the next free ones.";

        return "The interface patch follows this list: add, move and widen outputs freely, "
               "until something plays or you patch by hand.";
    }

    std::vector<std::string> channelLabels (const std::vector<OutputRow>& rows, int atLeast)
    {
        std::vector<std::string> labels;
        labels.resize (static_cast<std::size_t> (std::max (atLeast, outputChannelCount (rows))));

        for (std::size_t at = 0; at < labels.size(); ++at)
            labels[at] = "Output " + std::to_string (at + 1);

        for (const auto& row : rows)
            for (auto channel = 0; channel < row.width; ++channel)
            {
                const auto at = static_cast<std::size_t> (row.firstChannel + channel);

                if (at >= labels.size())
                    continue;

                /*  A STEREO PAIR IS NAMED BY ITS SIDES and anything wider by
                    its channel number: "Main L/R · L" is what somebody wiring
                    it would say, and "WFS send · 3" is what they would say for
                    a twelve-channel processor feed. A mono output is its own
                    name and nothing else - "Voice · 1" would be noise. */
                if (row.width == 1)
                    labels[at] = row.name;
                else if (row.width == 2)
                    labels[at] = row.name + " · " + (channel == 0 ? "L" : "R");
                else
                    labels[at] = row.name + " · " + std::to_string (channel + 1);
            }

        return labels;
    }
}
