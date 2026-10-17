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

#include <wfg/engine/video/InstalledFonts.h>

#include <wfg/engine/video/FontFile.h>

#include <juce_core/juce_core.h>

namespace wfg::video::installed
{
    std::optional<Face> byPostScript (const std::string& postScript)
    {
        if (postScript.empty())
            return std::nullopt;

        for (const auto& path : filesOfPostScript (postScript))
        {
            juce::MemoryBlock data;
            if (! juce::File (juce::String::fromUTF8 (path.c_str())).loadFileAsData (data))
                continue;

            const auto reading = fontfile::read (static_cast<const std::uint8_t*> (data.getData()), data.getSize());
            for (const auto& face : reading.faces)
                if (juce::String::fromUTF8 (face.postScript.c_str()).equalsIgnoreCase (juce::String::fromUTF8 (postScript.c_str())))
                    return Face { face.family, face.style, face.bold, face.italic, path, face.index };
        }
        return std::nullopt;
    }
}
