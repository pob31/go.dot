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

#include <wfg/client/model/Media.h>

#include <wfg/client/model/Text.h>

namespace wfg::client::model
{
    namespace
    {
        /*  The last dot, and only when something follows it and something
            precedes it: a file called `.hidden` has no extension to strip and
            one called `thunder.` has nothing after the dot to be one. */
        std::size_t extensionAt (const std::string& name)
        {
            const auto dot = name.find_last_of ('.');

            if (dot == std::string::npos || dot == 0 || dot + 1 >= name.size())
                return std::string::npos;

            return dot;
        }
    }

    std::string cueNameFor (const std::string& fileName)
    {
        const auto dot = extensionAt (fileName);

        return dot == std::string::npos ? fileName : fileName.substr (0, dot);
    }

    std::string mediaNameFor (const std::string& fileName)
    {
        /*  THE NAME, NEVER THE PATH. `media/@file` is relative to the bundle's
            `media/` folder because a show travels between machines and an
            absolute path is a fact about the machine it was authored on - the
            parameter table says so in as many words. So whatever was dropped,
            what the cue carries is what the file is called. */
        const auto slash = fileName.find_last_of ("/\\");

        return slash == std::string::npos ? fileName : fileName.substr (slash + 1);
    }

    std::string createdAt (const std::string& orderText, int index)
    {
        if (index < 0)
            return {};

        const auto members = words (orderText);

        if (members.empty())
            return {};

        /*  AN INDEX PAST THE END MEANS THE END, which is the document's own
            rule for a create: `min (index, length)`. So a drop at the foot of
            a list finds the cue it just made rather than nothing. */
        const auto at = static_cast<std::size_t> (index);

        return at < members.size() ? members[at] : members.back();
    }

    bool madeByImport (const Import& job, const std::string& kind,
                       const std::string& name, const std::string& file)
    {
        return kind == "media" && name == job.cueName && file.empty();
    }
}
