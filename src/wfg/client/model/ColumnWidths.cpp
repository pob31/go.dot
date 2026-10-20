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

#include <wfg/client/model/ColumnWidths.h>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace wfg::client::model
{
    std::optional<int> ColumnWidths::of (const std::string& column) const
    {
        if (const auto found = widths.find (column); found != widths.end())
            return found->second;

        return std::nullopt;
    }

    void ColumnWidths::keep (const std::string& column, int width)
    {
        if (column.empty())
            return;

        if (width <= 0)
            widths.erase (column);
        else
            widths[column] = std::clamp (width, least, most);
    }

    std::string ColumnWidths::written() const
    {
        std::string out;

        for (const auto& [column, width] : widths)
            out += column + " " + std::to_string (width) + "\n";

        return out;
    }

    ColumnWidths ColumnWidths::read (const std::string& text)
    {
        ColumnWidths out;
        std::istringstream lines (text);
        std::string line;

        while (std::getline (lines, line))
        {
            //  A name with no space in it, then a whole number, and nothing after.
            std::istringstream words (line);
            std::string column, number, more;

            if (! (words >> column >> number) || (words >> more))
                continue;

            if (number.empty() || number.size() > 6
                  || ! std::all_of (number.begin(), number.end(), [] (unsigned char c) { return std::isdigit (c) != 0; }))
                continue;

            out.keep (column, std::stoi (number));
        }

        return out;
    }
}
