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

#include <wfg/engine/app/RecentShows.h>

#include <algorithm>

namespace wfg::app::recent
{
    namespace
    {
        //  OpenShows.cpp's folding: no trailing separator, and no case where the file system has none.
        juce::String folded (const juce::File& show)
        {
            auto path = show.getFullPathName();

            while (path.length() > 1 && (path.endsWithChar ('/') || path.endsWithChar ('\\')))
                path = path.dropLastCharacters (1);

           #if JUCE_WINDOWS || JUCE_MAC
            path = path.toLowerCase();
           #endif

            return path;
        }
    }

    std::vector<juce::File> read (const juce::File& file)
    {
        std::vector<juce::File> shows;

        for (const auto& line : juce::StringArray::fromLines (file.loadFileAsString()))
        {
            const auto path = line.trim();

            if (path.isEmpty() || ! juce::File::isAbsolutePath (path))
                continue;

            const juce::File show { path };
            const auto same = [&show] (const juce::File& listed) { return folded (listed) == folded (show); };

            if (std::none_of (shows.begin(), shows.end(), same))
                shows.push_back (show);

            if (shows.size() == kept)
                break;
        }

        return shows;
    }

    bool remember (const juce::File& file, const juce::File& show)
    {
        if (! juce::File::isAbsolutePath (show.getFullPathName()))
            return false;

        juce::StringArray lines { show.getFullPathName() };

        for (const auto& listed : read (file))
            if (folded (listed) != folded (show) && static_cast<std::size_t> (lines.size()) < kept)
                lines.add (listed.getFullPathName());

        file.getParentDirectory().createDirectory();
        return file.replaceWithText (lines.joinIntoString ("\n") + "\n");
    }

    bool forget (const juce::File& file)
    {
        return ! file.existsAsFile() || file.deleteFile();
    }
}
