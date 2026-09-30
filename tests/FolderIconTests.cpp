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
    A saved show's folder carries the show icon (app/FolderIcon.h).

    Windows' half is files and attributes, which a test can read back. The
    Mac's is the Finder's, set on the main thread, and is the author's to look
    at; Linux has none, and says so by doing nothing.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/app/FolderIcon.h>

#include <juce_core/juce_core.h>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

namespace
{
    //  A folder of this case's own, gone afterwards - attributes and all.
    struct Scratch
    {
        Scratch()
            : folder (juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("wfg-folder-icon-" + juce::Uuid().toDashedString()))
        {
            folder.createDirectory();
        }

        ~Scratch()
        {
           #if JUCE_WINDOWS
            //  Hidden ones included: that is what findFilesAndDirectories means without ignoreHiddenFiles.
            for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*",
                                                                     juce::File::findFilesAndDirectories))
                SetFileAttributesW (entry.getFile().getFullPathName().toWideCharPointer(), FILE_ATTRIBUTE_NORMAL);

            SetFileAttributesW (folder.getFullPathName().toWideCharPointer(), FILE_ATTRIBUTE_NORMAL);
           #endif
            folder.deleteRecursively();
        }

        juce::File folder;
    };

   #if JUCE_WINDOWS
    DWORD attributesOf (const juce::File& file)
    {
        return GetFileAttributesW (file.getFullPathName().toWideCharPointer());
    }
   #endif
}

#if JUCE_WINDOWS
TEST_CASE ("folder icon: a show folder gets a hidden icon and desktop.ini, and is marked read-only, once")
{
    Scratch scratch;
    const auto show = scratch.folder.getChildFile ("Tuesday");
    REQUIRE (show.createDirectory());

    CHECK_FALSE (wfg::app::isMarkedShowFolder (show));
    wfg::app::markShowFolder (show);
    CHECK (wfg::app::isMarkedShowFolder (show));

    const auto ini = show.getChildFile ("desktop.ini");
    const auto icon = show.getChildFile (".go.dot-folder.ico");
    REQUIRE (ini.existsAsFile());
    REQUIRE (icon.existsAsFile());

    //  The .ico is the folder icon's bytes: an icon directory, seven sizes.
    juce::MemoryBlock bytes;
    REQUIRE (icon.loadFileAsData (bytes));
    REQUIRE (bytes.getSize() > 6);
    CHECK (static_cast<unsigned char> (bytes[2]) == 1);    // type 1: an icon

    //  Relative, so it travels with the folder.
    CHECK (ini.loadFileAsString().contains ("IconResource=.go.dot-folder.ico,0"));

    constexpr DWORD hiddenSystem = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
    CHECK ((attributesOf (ini) & hiddenSystem) == hiddenSystem);
    CHECK ((attributesOf (icon) & hiddenSystem) == hiddenSystem);
    CHECK ((attributesOf (show) & FILE_ATTRIBUTE_READONLY) != 0);

    //  A second save changes nothing, and the show can still be written into.
    const auto written = ini.getLastModificationTime();
    wfg::app::markShowFolder (show);
    CHECK (ini.getLastModificationTime() == written);
    CHECK (show.getChildFile ("show.xml").replaceWithText ("<show/>"));
}

TEST_CASE ("folder icon: somebody else's desktop.ini is left alone")
{
    Scratch scratch;
    const auto show = scratch.folder.getChildFile ("Theirs");
    REQUIRE (show.createDirectory());

    const auto ini = show.getChildFile ("desktop.ini");
    REQUIRE (ini.replaceWithText ("[.ShellClassInfo]\r\nLocalizedResourceName=Mine\r\n"));

    wfg::app::markShowFolder (show);

    CHECK_FALSE (wfg::app::isMarkedShowFolder (show));
    CHECK (ini.loadFileAsString().contains ("Mine"));
    CHECK_FALSE (show.getChildFile (".go.dot-folder.ico").exists());
}
#endif

TEST_CASE ("folder icon: a path that is not a folder is left alone")
{
    Scratch scratch;
    const auto file = scratch.folder.getChildFile ("not-a-folder.txt");
    REQUIRE (file.replaceWithText ("x"));

    wfg::app::markShowFolder (file);
    wfg::app::markShowFolder (scratch.folder.getChildFile ("absent"));

    CHECK_FALSE (wfg::app::isMarkedShowFolder (file));
    CHECK_FALSE (scratch.folder.getChildFile ("absent").exists());
}
