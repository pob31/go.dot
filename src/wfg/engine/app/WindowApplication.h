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

#pragma once

/*
    WHAT THE SYSTEM TALKS TO WHEN IT TALKS TO GO.DOT, in a windowed serve.

    wfg is a console program with a plain main() (app/Main.cpp), and JUCE only
    passes on what the system says to an APPLICATION - a JUCEApplicationBase -
    of which it had none. Three things were being dropped on the floor:

      - macOS's "open this file", which is how Finder hands over a
        double-clicked show (juce_MessageManager_mac.mm, application:openFiles:);
      - "quit" - ⌘Q, the Dock's Quit, a Windows shutdown - which with no
        application terminated the process there and then, running cues,
        show lock and unsaved work alike;
      - on Linux, the window's X11 class, which is how a desktop entry
        matches a window to its icon.

    This is that application, and nothing more: it is never run through
    JUCE's main(), it has no window of its own, and it hands each message to
    the console that made it. Headless serve, replay and every other verb are
    untouched - only `serve --window` makes one.

    DECLARE BEFORE JUCE STARTS. JUCE decides at start-up, from whether an
    application is declared, whether this process is an app (isStandaloneApp)
    - on macOS whether its delegate is NSApp's at all - so `declareStandalone`
    runs before the verb's ScopedJuceInitialiser_GUI, and the object after.
*/

#include <juce_events/juce_events.h>

#include <functional>

namespace wfg::app
{
    class WindowApplication final : public juce::JUCEApplicationBase
    {
    public:
        /*  Says to JUCE that this process is an application. Before anything
            reaches the MessageManager; once; never undone. */
        static void declareStandalone();

        /*  THE MAC'S LAUNCH, FINISHED BEFORE THE SHOW IS CHOSEN: NSApp runs
            until it has finished launching and then stops, which is when a
            file double-clicked to start Go.dot has been handed over - to
            anotherInstanceStarted, so `onOpen` must be listening. The loop
            JUCE runs afterwards is the ordinary one. Everywhere else a file
            arrives on the command line, and this does nothing. */
        static void finishLaunching();

        WindowApplication() = default;

        /*  A file the system asked Go.dot to open: one call per file, on the
            message thread, the path as the system gave it. */
        std::function<void (const juce::String& path)> onOpen;

        /*  The system asked Go.dot to quit: the window's own way out, which
            asks first and refuses in show mode. */
        std::function<void()> onQuit;

        const juce::String getApplicationName() override;
        const juce::String getApplicationVersion() override;
        bool moreThanOneInstanceAllowed() override            { return true; }
        void initialise (const juce::String&) override         {}
        void shutdown() override                                {}
        void anotherInstanceStarted (const juce::String& commandLine) override;
        void systemRequestedQuit() override;
        void suspended() override                               {}
        void resumed() override                                 {}
        void unhandledException (const std::exception*, const juce::String&, int) override {}
    };
}
