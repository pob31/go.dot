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

#include <wfg/client/model/OscGrid.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <utility>

namespace wfg::client::model
{
    namespace
    {
        std::vector<std::string> segmentsOf (const std::string& address)
        {
            std::vector<std::string> out;
            std::string current;

            for (const auto c : address)
            {
                if (c == '/')
                {
                    if (! current.empty())
                        out.push_back (std::move (current));

                    current.clear();
                    continue;
                }

                current += c;
            }

            if (! current.empty())
                out.push_back (std::move (current));

            return out;
        }

        bool isNumber (const std::string& word)
        {
            return ! word.empty() && word.size() <= 9
                && std::all_of (word.begin(), word.end(), [] (char c) { return std::isdigit (static_cast<unsigned char> (c)) != 0; });
        }

        std::vector<std::string> wordsOf (const std::string& text)
        {
            std::vector<std::string> out;
            std::istringstream in (text);

            for (std::string word; in >> word;)
                out.push_back (word);

            return out;
        }
    }

    std::optional<OscGridPlace> gridPlaceOf (const std::string& address)
    {
        const auto segments = segmentsOf (address);

        //  A family before it and a parameter after it: never the first segment, never the last.
        for (std::size_t at = 1; at + 1 < segments.size(); ++at)
        {
            if (! isNumber (segments[at]))
                continue;

            OscGridPlace place;

            for (std::size_t k = 0; k < at; ++k)
                place.family += "/" + segments[k];

            place.column = segments[at];

            for (std::size_t k = at + 1; k < segments.size(); ++k)
                place.parameter += (k == at + 1 ? "" : "/") + segments[k];

            return place;
        }

        return std::nullopt;
    }

    OscGrid gridOf (const OscMessagesReading& reading)
    {
        OscGrid grid;

        struct Placed
        {
            OscGridPlace place;
            int row = 0;
        };

        std::vector<Placed> placed;

        for (std::size_t at = 0; at < reading.rows.size(); ++at)
        {
            if (const auto place = gridPlaceOf (reading.rows[at].address))
                placed.push_back ({ *place, static_cast<int> (at) });
            else
                ++grid.outside;
        }

        for (const auto& one : placed)
        {
            auto table = std::find_if (grid.tables.begin(), grid.tables.end(),
                                       [&one] (const OscGridTable& t) { return t.family == one.place.family; });

            if (table == grid.tables.end())
            {
                grid.tables.push_back ({});
                grid.tables.back().family = one.place.family;
                table = grid.tables.end() - 1;
            }

            if (std::find (table->columns.begin(), table->columns.end(), one.place.column) == table->columns.end())
                table->columns.push_back (one.place.column);

            if (std::find (table->parameters.begin(), table->parameters.end(), one.place.parameter) == table->parameters.end())
                table->parameters.push_back (one.place.parameter);
        }

        //  The channels in their order as numbers: 2 before 10.
        for (auto& table : grid.tables)
            std::sort (table.columns.begin(), table.columns.end(), [] (const std::string& a, const std::string& b)
                       { return std::stoll (a) < std::stoll (b); });

        for (auto& table : grid.tables)
            table.cells.assign (table.parameters.size(), std::vector<int> (table.columns.size(), -1));

        //  A cell the cue holds twice shows the later message, as the later one is what is heard last.
        for (const auto& one : placed)
        {
            auto& table = *std::find_if (grid.tables.begin(), grid.tables.end(),
                                         [&one] (const OscGridTable& t) { return t.family == one.place.family; });

            const auto row = static_cast<std::size_t> (std::find (table.parameters.begin(), table.parameters.end(),
                                                                  one.place.parameter) - table.parameters.begin());
            const auto column = static_cast<std::size_t> (std::find (table.columns.begin(), table.columns.end(),
                                                                     one.place.column) - table.columns.begin());
            table.cells[row][column] = one.row;
        }

        return grid;
    }

    std::string cellText (const OscMessageRow& row)
    {
        if (! row.spells)
            return row.value;

        std::string out;

        for (const auto& arg : row.args)
            out += (out.empty() ? "" : " ") + arg.payload;

        return out;
    }

    std::optional<std::string> cellEdit (const OscMessageRow& row, const std::string& typed)
    {
        if (! row.spells || row.args.empty())
            return std::nullopt;

        //  One value takes the whole text, so words with spaces stay one value.
        std::vector<std::string> words;

        if (row.args.size() == 1)
        {
            const auto first = typed.find_first_not_of (" \t");
            const auto last = typed.find_last_not_of (" \t");
            words.push_back (first == std::string::npos ? std::string {} : typed.substr (first, last - first + 1));
        }
        else
        {
            words = wordsOf (typed);
        }

        if (words.size() != row.args.size())
            return std::nullopt;

        auto list = row.value;

        for (std::size_t at = 0; at < words.size(); ++at)
        {
            const auto atom = atomFor (row.args[at].tag, words[at]);

            if (! atom.has_value())
                return std::nullopt;

            const auto replaced = withArgument (list, at, *atom);

            if (! replaced.has_value())
                return std::nullopt;

            list = *replaced;
        }

        return list;
    }
}
