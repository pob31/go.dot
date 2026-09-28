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

#include <wfg/client/model/InputList.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/Node.h>
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
        constexpr std::string_view prefix = "/godot/input/";

        /*  A whole number off the tree's own text, never a locale question -
            and never a throw on a malformed reading. `reading` rather than
            `text` for OutputList's reason: GCC's -Wshadow. */
        int whole (const std::string& reading, int fallback)
        {
            const auto parsed = osc::parseDouble (reading);

            return parsed.has_value() ? static_cast<int> (*parsed) : fallback;
        }
    }

    std::string InputRow::widthWord() const
    {
        if (width == 1) return "Mono";
        if (width == 2) return "Stereo";

        return std::to_string (width) + " channels";
    }

    std::string InputRow::channelWord() const
    {
        /*  COUNTED FROM ONE, as the outputs' column is, because it is read
            beside a stage box and an interface that are labelled that way. */
        const auto first = firstChannel + 1;

        if (width <= 1)
            return std::to_string (first);

        return std::to_string (first) + "-" + std::to_string (firstChannel + width);
    }

    double InputRow::meterFill() const
    {
        return std::clamp ((meterDb + 60.0) / 60.0, 0.0, 1.0);
    }

    std::vector<InputRow> readInputs (const tree::TreeSnapshot& snapshot)
    {
        /*  ONE PASS over the tree, gathering by identifier, as `readOutputs`
            does. `order` sits under the same prefix with no identifier after
            it, and is passed over by the slash test below. */
        std::map<std::string, InputRow> found;

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
            else if (name == "width")        row.width = std::max (1, whole (text (node), 1));
            else if (name == "firstChannel") row.firstChannel = std::max (0, whole (text (node), 0));
            else if (name == "meter")        row.meterDb = osc::parseDouble (text (node)).value_or (-120.0);
            else if (name == "problem")      row.problem = text (node);
        }

        std::vector<InputRow> rows;
        rows.reserve (found.size());

        for (auto& [id, row] : found)
        {
            if (row.name.empty())
                row.name = "Input " + std::to_string (row.firstChannel + 1);

            rows.push_back (row);
        }

        std::stable_sort (rows.begin(), rows.end(),
                          [] (const InputRow& a, const InputRow& b)
                          {
                              if (a.firstChannel != b.firstChannel)
                                  return a.firstChannel < b.firstChannel;

                              return a.id < b.id;
                          });

        return rows;
    }

    int inputChannelCount (const std::vector<InputRow>& rows)
    {
        auto total = 0;

        for (const auto& row : rows)
            total = std::max (total, row.firstChannel + row.width);

        return total;
    }

    bool inputPatchHasSettled (const tree::TreeSnapshot& snapshot)
    {
        if (isYes (flag (snapshot, "/godot/audio/inputPatchSettled")))
            return true;

        if (! text (snapshot, "/godot/audio/inputPatch").empty())
            return true;

        auto expected = 0;

        for (const auto& row : readInputs (snapshot))
        {
            if (row.firstChannel != expected)
                return true;

            expected += row.width;
        }

        return false;
    }

    std::string inputRegime (bool settled)
    {
        if (settled)
            return "The input patch is set: each input keeps the interface channels it is on, "
                   "and a new one takes the next free ones.";

        return "The input patch follows this list: add, move and widen inputs freely, "
               "until you patch by hand.";
    }

    std::vector<std::string> inputChannelLabels (const std::vector<InputRow>& rows, int atLeast)
    {
        std::vector<std::string> labels;
        labels.resize (static_cast<std::size_t> (std::max (atLeast, inputChannelCount (rows))));

        for (std::size_t at = 0; at < labels.size(); ++at)
            labels[at] = "Input " + std::to_string (at + 1);

        for (const auto& row : rows)
            for (auto channel = 0; channel < row.width; ++channel)
            {
                const auto at = static_cast<std::size_t> (row.firstChannel + channel);

                if (at >= labels.size())
                    continue;

                /*  The outputs' naming: a mono input is its own name, a
                    stereo one its sides, anything wider its channel numbers. */
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
