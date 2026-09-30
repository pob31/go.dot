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

#include <wfg/engine/app/Associate.h>

#include <array>
#include <utility>

namespace wfg::app
{
    namespace
    {
        constexpr const char* mimeType = "application/x-go.dot-show";
        constexpr const char* mimeIcon = "application-x-go.dot-show";
        constexpr std::array<int, 8> sizes { 16, 24, 32, 48, 64, 128, 256, 512 };

        juce::File mimeFile (const juce::File& home)    { return home.getChildFile ("mime/packages/go.dot.xml"); }
        juce::File desktopFile (const juce::File& home) { return home.getChildFile ("applications/go.dot.desktop"); }

        juce::File iconFile (const juce::File& home, int size, const char* context, const juce::String& name)
        {
            return home.getChildFile ("icons/hicolor")
                       .getChildFile (juce::String (size) + "x" + juce::String (size))
                       .getChildFile (context)
                       .getChildFile (name + ".png");
        }

        /*  One argument of an Exec line, as the Desktop Entry specification
            has it: in double quotes, with `"`, `` ` ``, `$` and `\` escaped by
            a backslash - which the file's own string escaping then doubles -
            and `%` doubled, since a lone one is a field code. */
        juce::String execArgument (const juce::String& path)
        {
            juce::String quoted { "\"" };

            for (const auto c : path)
            {
                if (c == '"' || c == '`' || c == '$')
                    quoted << "\\\\" << juce::String::charToString (c);
                else if (c == '\\')
                    quoted << "\\\\\\\\";
                else if (c == '%')
                    quoted << "%%";
                else
                    quoted << juce::String::charToString (c);
            }

            return quoted + "\"";
        }

        //  The desktop's own tools, where it has them; a desktop without them reads the files anyway.
        void refresh (const juce::File& home)
        {
           #if JUCE_LINUX
            const auto run = [] (juce::StringArray command)
            {
                juce::ChildProcess tool;

                if (tool.start (command, 0))
                    tool.waitForProcessToFinish (30000);
            };

            run ({ "update-mime-database", home.getChildFile ("mime").getFullPathName() });
            run ({ "update-desktop-database", home.getChildFile ("applications").getFullPathName() });
            run ({ "gtk-update-icon-cache", "-f", "-t", home.getChildFile ("icons/hicolor").getFullPathName() });
           #else
            (void) home;
           #endif
        }
    }

    juce::File defaultDataHome()
    {
        const auto xdg = juce::SystemStats::getEnvironmentVariable ("XDG_DATA_HOME", {});

        if (xdg.isNotEmpty() && juce::File::isAbsolutePath (xdg))
            return juce::File (xdg);

        return juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (".local/share");
    }

    AssociateResult associate (bool remove, const juce::File& program, const juce::File& home)
    {
        AssociateResult result;

        if (remove)
        {
            mimeFile (home).deleteFile();
            desktopFile (home).deleteFile();

            for (const auto size : sizes)
            {
                iconFile (home, size, "apps", "go.dot").deleteFile();
                iconFile (home, size, "mimetypes", mimeIcon).deleteFile();
            }

            refresh (home);
            result.ok = true;
            result.said = ".wfg files no longer open with Go.dot";
            return result;
        }

        const auto launcher = program.getChildFile ("go.dot.sh");

        if (! launcher.existsAsFile())
        {
            result.said = "there is no go.dot.sh beside wfg in " + program.getFullPathName().toStdString()
                            + ": this is for the test build's folder";
            return result;
        }

        const auto mime = mimeFile (home);
        const auto desktop = desktopFile (home);

        if (! mime.getParentDirectory().createDirectory() || ! desktop.getParentDirectory().createDirectory())
        {
            result.said = "could not make the folders under " + home.getFullPathName().toStdString();
            return result;
        }

        const juce::String mimeText =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!-- Written by `wfg associate` for Go.dot; `wfg associate --remove` takes it away. -->\n"
            "<mime-info xmlns=\"http://www.freedesktop.org/standards/shared-mime-info\">\n"
            "  <mime-type type=\"" + juce::String (mimeType) + "\">\n"
            "    <comment>Go.dot show</comment>\n"
            "    <icon name=\"" + juce::String (mimeIcon) + "\"/>\n"
            "    <glob pattern=\"*.wfg\"/>\n"
            "  </mime-type>\n"
            "</mime-info>\n";

        const juce::String desktopText =
            "# Written by `wfg associate` for Go.dot; `wfg associate --remove` takes it away.\n"
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Go.dot\n"
            "Comment=Show control\n"
            "Exec=" + execArgument (launcher.getFullPathName()) + " %f\n"
            "Icon=go.dot\n"
            "Terminal=false\n"
            "Categories=AudioVideo;Audio;\n"
            "MimeType=" + juce::String (mimeType) + ";\n"
            "StartupWMClass=Go.dot\n";

        /*  "\n", said: replaceWithText writes "\r\n" unless told, on every
            system, and a desktop entry's Exec would end in a carriage return. */
        if (! mime.replaceWithText (mimeText, false, false, "\n")
              || ! desktop.replaceWithText (desktopText, false, false, "\n"))
        {
            result.said = "could not write under " + home.getFullPathName().toStdString();
            return result;
        }

        //  The icons this copy carries; an entry with none still opens shows.
        auto icons = 0;

        for (const auto size : sizes)
        {
            const auto app = program.getChildFile ("icons/go.dot-" + juce::String (size) + ".png");
            const auto page = program.getChildFile ("icons/go.dot-document-" + juce::String (size) + ".png");

            for (const auto& [from, to] : { std::pair { app, iconFile (home, size, "apps", "go.dot") },
                                            std::pair { page, iconFile (home, size, "mimetypes", mimeIcon) } })
                if (from.existsAsFile() && to.getParentDirectory().createDirectory() && from.copyFileTo (to))
                    ++icons;
        }

        refresh (home);

        result.ok = true;
        result.said = ".wfg files now open with this Go.dot ("
                        + program.getFullPathName().toStdString() + ")"
                        + (icons == 0 ? ", with no icons: there are none beside it" : "");
        return result;
    }
}
