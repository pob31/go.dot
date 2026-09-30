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

//  Windows and Linux. The Mac's is FolderIcon_mac.mm.
#if ! defined (__APPLE__)

#include <wfg/engine/app/FolderIcon.h>

#include <cstddef>

#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <shlobj.h>

namespace wfg::app
{
    //  The generated FolderIconData.cpp (src/CMakeLists.txt): Go.dot-folder.ico.
    extern const unsigned char folderIconBytes[];
    extern const std::size_t folderIconSize;

    namespace
    {
        constexpr const char* iconName = ".go.dot-folder.ico";

        //  What desktop.ini says when it is ours - and how it is known to be.
        const juce::String iniText = juce::String ("[.ShellClassInfo]\r\nIconResource=")
                                       + iconName + ",0\r\n";

        /*  Written whole, hidden and system from the start. A CREATE_ALWAYS on a
            file that is already hidden and system must say so again, or
            Windows refuses it. */
        bool writeHidden (const juce::File& file, const void* data, std::size_t size)
        {
            constexpr DWORD attributes = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;

            const auto handle = CreateFileW (file.getFullPathName().toWideCharPointer(), GENERIC_WRITE, 0, nullptr,
                                             CREATE_ALWAYS, attributes, nullptr);

            if (handle == INVALID_HANDLE_VALUE)
                return false;

            DWORD written = 0;
            const auto ok = WriteFile (handle, data, static_cast<DWORD> (size), &written, nullptr) != 0
                              && written == static_cast<DWORD> (size);
            CloseHandle (handle);
            return ok;
        }
    }

    bool isMarkedShowFolder (const juce::File& folder)
    {
        const auto ini = folder.getChildFile ("desktop.ini");

        return folder.getChildFile (iconName).existsAsFile()
                 && ini.existsAsFile() && ini.loadFileAsString().contains (iconName)
                 && (GetFileAttributesW (folder.getFullPathName().toWideCharPointer()) & FILE_ATTRIBUTE_READONLY) != 0;
    }

    void markShowFolder (const juce::File& folder)
    {
        if (! folder.isDirectory() || isMarkedShowFolder (folder))
            return;

        const auto ini = folder.getChildFile ("desktop.ini");

        //  Somebody else's desktop.ini: theirs, whatever it says.
        if (ini.existsAsFile() && ! ini.loadFileAsString().contains (iconName))
            return;

        if (! writeHidden (folder.getChildFile (iconName), folderIconBytes, folderIconSize))
            return;

        const auto text = iniText.toStdString();

        if (! writeHidden (ini, text.data(), text.size()))
            return;

        //  Read-only is what makes Explorer read desktop.ini; it does not stop files being written inside.
        const auto path = folder.getFullPathName();
        const auto attributes = GetFileAttributesW (path.toWideCharPointer());

        if (attributes != INVALID_FILE_ATTRIBUTES)
            SetFileAttributesW (path.toWideCharPointer(), attributes | FILE_ATTRIBUTE_READONLY);

        SHChangeNotify (SHCNE_UPDATEDIR, SHCNF_PATHW, path.toWideCharPointer(), nullptr);
    }
}

#else

namespace wfg::app
{
    //  Linux: its file managers keep a folder's icon outside the folder.
    void markShowFolder (const juce::File&) {}
    bool isMarkedShowFolder (const juce::File&) { return false; }
}

#endif
#endif
