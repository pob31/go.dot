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

#include <wfg/engine/app/WindowApplication.h>

namespace wfg::app
{
    void WindowApplication::declareStandalone()
    {
        /*  isStandaloneApp() is `createInstance != nullptr` and nothing else;
            only JUCE's own main() ever calls it, and that main is not ours. */
        juce::JUCEApplicationBase::createInstance = [] () -> juce::JUCEApplicationBase* { return nullptr; };
    }

    const juce::String WindowApplication::getApplicationName()
    {
        //  Also the Linux window's class: a desktop entry's StartupWMClass.
        return "Go.dot";
    }

    const juce::String WindowApplication::getApplicationVersion()
    {
        return WFG_VERSION;
    }

    void WindowApplication::anotherInstanceStarted (const juce::String& commandLine)
    {
        /*  JUCE joins the files it was handed with spaces and quotes the ones
            that have their own (quotedIfContainsSpaces), so the tokens,
            quotes kept and then taken off, are the paths. */
        juce::StringArray paths;
        paths.addTokens (commandLine, " ", "\"");

        for (auto path : paths)
        {
            path = path.unquoted().trim();

            if (path.isNotEmpty() && onOpen)
                onOpen (path);
        }
    }

    void WindowApplication::systemRequestedQuit()
    {
        if (onQuit)
            onQuit();
        else
            quit();
    }
}
