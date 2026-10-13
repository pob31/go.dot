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
    WINDOWS: DirectWrite's system font collection, asked for a family by name
    (its families are those JUCE lists), each font's files found through the
    local file loader - the one that knows a path. A face DirectWrite makes up
    from another (a bold simulated from a regular) adds no file of its own.
*/

#include <wfg/engine/video/InstalledFonts.h>

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <juce_core/juce_core.h>

#include <algorithm>

#pragma comment(lib, "dwrite.lib")

namespace wfg::video::installed
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        ComPtr<IDWriteFontCollection> systemFonts()
        {
            ComPtr<IDWriteFactory> factory;
            if (FAILED (DWriteCreateFactory (DWRITE_FACTORY_TYPE_SHARED, __uuidof (IDWriteFactory),
                                             reinterpret_cast<IUnknown**> (factory.GetAddressOf()))))
                return {};

            ComPtr<IDWriteFontCollection> collection;
            if (FAILED (factory->GetSystemFontCollection (collection.GetAddressOf(), FALSE)))
                return {};
            return collection;
        }

        void addOnce (std::vector<std::string>& paths, std::string path)
        {
            if (! path.empty() && std::find (paths.begin(), paths.end(), path) == paths.end())
                paths.push_back (std::move (path));
        }

        /** The files a font is drawn from, where the local loader knows them. */
        void filesOf (IDWriteFont& font, std::vector<std::string>& paths)
        {
            ComPtr<IDWriteFontFace> face;
            if (FAILED (font.CreateFontFace (face.GetAddressOf())))
                return;

            UINT32 count = 0;
            if (FAILED (face->GetFiles (&count, nullptr)) || count == 0)
                return;

            std::vector<IDWriteFontFile*> files (count, nullptr);
            if (FAILED (face->GetFiles (&count, files.data())))
                return;

            for (auto* file : files)
            {
                ComPtr<IDWriteFontFile> owned;
                owned.Attach (file);

                const void* key = nullptr;
                UINT32 keySize = 0;
                ComPtr<IDWriteFontFileLoader> loader;
                ComPtr<IDWriteLocalFontFileLoader> local;
                if (FAILED (owned->GetReferenceKey (&key, &keySize)) || FAILED (owned->GetLoader (loader.GetAddressOf()))
                      || FAILED (loader.As (&local)))
                    continue;

                UINT32 length = 0;
                if (FAILED (local->GetFilePathLengthFromKey (key, keySize, &length)))
                    continue;

                std::wstring path (static_cast<std::size_t> (length) + 1, L'\0');
                if (FAILED (local->GetFilePathFromKey (key, keySize, path.data(), length + 1)))
                    continue;

                path.resize (length);
                addOnce (paths, juce::String (path.c_str()).toStdString());
            }
        }

        template <typename EachFont>
        void forEachFont (IDWriteFontFamily& family, EachFont&& each)
        {
            const auto count = family.GetFontCount();
            for (UINT32 i = 0; i < count; ++i)
            {
                ComPtr<IDWriteFont> font;
                if (SUCCEEDED (family.GetFont (i, font.GetAddressOf())) && font->GetSimulations() == DWRITE_FONT_SIMULATIONS_NONE)
                    each (*font.Get());
            }
        }
    }

    std::vector<std::string> filesOfFamily (const std::string& family)
    {
        std::vector<std::string> paths;
        const auto fonts = systemFonts();
        if (fonts == nullptr || family.empty())
            return paths;

        UINT32 index = 0;
        BOOL exists = FALSE;
        const auto wide = juce::String::fromUTF8 (family.c_str());
        if (FAILED (fonts->FindFamilyName (wide.toWideCharPointer(), &index, &exists)) || ! exists)
            return paths;

        ComPtr<IDWriteFontFamily> found;
        if (FAILED (fonts->GetFontFamily (index, found.GetAddressOf())))
            return paths;

        forEachFont (*found.Get(), [&paths] (IDWriteFont& font) { filesOf (font, paths); });
        return paths;
    }

    std::vector<std::string> filesOfPostScript (const std::string& postScript)
    {
        std::vector<std::string> paths;
        const auto fonts = systemFonts();
        if (fonts == nullptr || postScript.empty())
            return paths;

        const auto wanted = juce::String::fromUTF8 (postScript.c_str());
        const auto families = fonts->GetFontFamilyCount();

        for (UINT32 f = 0; f < families && paths.empty(); ++f)
        {
            ComPtr<IDWriteFontFamily> family;
            if (FAILED (fonts->GetFontFamily (f, family.GetAddressOf())))
                continue;

            forEachFont (*family.Get(), [&] (IDWriteFont& font)
            {
                ComPtr<IDWriteLocalizedStrings> strings;
                BOOL exists = FALSE;
                if (FAILED (font.GetInformationalStrings (DWRITE_INFORMATIONAL_STRING_POSTSCRIPT_NAME,
                                                          strings.GetAddressOf(), &exists)) || ! exists)
                    return;

                UINT32 length = 0;
                if (FAILED (strings->GetStringLength (0, &length)))
                    return;

                std::wstring name (static_cast<std::size_t> (length) + 1, L'\0');
                if (FAILED (strings->GetString (0, name.data(), length + 1)))
                    return;

                name.resize (length);
                if (juce::String (name.c_str()).equalsIgnoreCase (wanted))
                    filesOf (font, paths);
            });
        }
        return paths;
    }
}
