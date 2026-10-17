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

/*
    LINUX: fontconfig, which JUCE already reads its fonts through. A list - not
    a match, which always answers with something - of the fonts having that
    family or that PostScript name, each one's file.
*/

#include <wfg/engine/video/InstalledFonts.h>

#include <fontconfig/fontconfig.h>

#include <algorithm>

namespace wfg::video::installed
{
    namespace
    {
        std::vector<std::string> filesWhere (const char* object, const std::string& value)
        {
            std::vector<std::string> paths;
            if (value.empty())
                return paths;

            FcPattern* pattern = FcPatternCreate();
            if (pattern == nullptr)
                return paths;

            FcPatternAddString (pattern, object, reinterpret_cast<const FcChar8*> (value.c_str()));
            FcObjectSet* wanted = FcObjectSetBuild (FC_FILE, static_cast<char*> (nullptr));
            FcFontSet* fonts = wanted != nullptr ? FcFontList (nullptr, pattern, wanted) : nullptr;

            if (fonts != nullptr)
            {
                for (int i = 0; i < fonts->nfont; ++i)
                {
                    FcChar8* file = nullptr;
                    if (FcPatternGetString (fonts->fonts[i], FC_FILE, 0, &file) == FcResultMatch && file != nullptr)
                    {
                        std::string path (reinterpret_cast<const char*> (file));
                        if (std::find (paths.begin(), paths.end(), path) == paths.end())
                            paths.push_back (path);
                    }
                }
                FcFontSetDestroy (fonts);
            }

            if (wanted != nullptr)
                FcObjectSetDestroy (wanted);
            FcPatternDestroy (pattern);

            std::sort (paths.begin(), paths.end());
            return paths;
        }
    }

    std::vector<std::string> filesOfFamily (const std::string& family)
    {
        return filesWhere (FC_FAMILY, family);
    }

    std::vector<std::string> filesOfPostScript (const std::string& postScript)
    {
        return filesWhere (FC_POSTSCRIPT_NAME, postScript);
    }
}
