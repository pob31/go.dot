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

#include <wfg/client/model/Shows.h>

#include <algorithm>
#include <cctype>

namespace wfg::client::model
{
    namespace
    {
        //  The path without separators at its end, which a folder may be handed with.
        std::string trimmed (std::string path)
        {
            while (path.size() > 1 && (path.back() == '/' || path.back() == '\\'))
                path.pop_back();

            return path;
        }

        //  The last part of a path, either separator.
        std::string lastPart (const std::string& path)
        {
            const auto cut = path.find_last_of ("/\\");
            return cut == std::string::npos ? path : path.substr (cut + 1);
        }

        std::string parentOf (const std::string& path)
        {
            const auto cut = path.find_last_of ("/\\");
            return cut == std::string::npos ? std::string() : path.substr (0, cut);
        }

        bool endsWithWfg (const std::string& name)
        {
            if (name.size() <= 4)
                return false;

            auto end = name.substr (name.size() - 4);
            std::transform (end.begin(), end.end(), end.begin(), [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
            return end == ".wfg";
        }

        //  A `.wfg` stands for its folder: the folder is what has the name.
        std::string folderOf (const std::string& path)
        {
            const auto whole = trimmed (path);
            return endsWithWfg (lastPart (whole)) ? parentOf (whole) : whole;
        }
    }

    std::string showName (const std::string& path)
    {
        return lastPart (folderOf (path));
    }

    std::vector<std::string> recentShowLines (const std::vector<std::string>& paths)
    {
        std::vector<std::string> names;

        for (const auto& path : paths)
            names.push_back (showName (path));

        std::vector<std::string> lines;

        for (std::size_t at = 0; at < paths.size(); ++at)
        {
            const auto shared = std::count (names.begin(), names.end(), names[at]) > 1;
            const auto beside = lastPart (parentOf (folderOf (paths[at])));

            lines.push_back (shared && ! beside.empty() ? names[at] + " - " + beside : names[at]);
        }

        return lines;
    }

    std::string otherShowsNamed (const std::vector<std::string>& paths)
    {
        std::vector<std::string> names;
        auto unnamed = 0;

        for (const auto& path : paths)
        {
            if (const auto name = showName (path); name.empty())
                ++unnamed;
            else
                names.push_back (name);
        }

        if (unnamed == 1)
            names.push_back (names.empty() ? "a show" : "another show");
        else if (unnamed > 1)
            names.push_back (std::to_string (unnamed) + (names.empty() ? " shows" : " other shows"));

        std::string said;

        for (std::size_t at = 0; at < names.size(); ++at)
        {
            if (at > 0)
                said += at + 1 == names.size() ? " and " : ", ";

            said += names[at];
        }

        return said;
    }
}
