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
    A WINDOW THAT CANNOT OPEN SAYS WHY, IN A BOX.

    `serve --window` refuses a great many things before there is a window: a
    path that is not a show, a show that will not load, a device, a port. Each
    refusal is a sentence on stderr, which is exactly right in a terminal and
    exactly useless from a double-click - Finder, Explorer and the launchers
    give wfg no terminal, so the person who double-clicked a show would see
    nothing at all, and the sentence would sit in a log they have no reason to
    open.

    So a windowed serve copies what it writes to stderr while it starts, and if
    it then fails before its window is up, and nobody is reading a terminal,
    the copy is put in front of them in the system's own alert. Every refusal
    that already exists is covered without being touched, and a refusal added
    next year is covered too.

    THE COPY STOPS WHEN THE WINDOW IS UP. From then on the window is where
    things are said, and a failure on the way out is not a failure to open.

    STD ONLY in this header, as the engine's public ones are; the platform is
    in the .cpp.
*/

#include <memory>
#include <string>

namespace wfg::app
{
    class StartupReport final
    {
    public:
        /*  Starts copying std::cerr. The stream keeps writing where it always
            did: the copy is beside it, never instead of it. */
        StartupReport();

        /*  Puts std::cerr back. Only once every thread that might write to it
            has stopped - which, for serve, is when runServe has returned. */
        ~StartupReport();

        StartupReport (const StartupReport&) = delete;
        StartupReport& operator= (const StartupReport&) = delete;

        /*  The window is up: the live report, if there is one, stops copying.
            Static because the verb that makes the window is not the one that
            holds the report, and needs no handle to say so. */
        static void windowIsUp() noexcept;

        /*  After the verb: when it failed, before its window, with nobody at a
            terminal, the copy goes up in an alert. Otherwise nothing. */
        void showIfFailed (int exitCode);

        //  The copying buffer: opaque, and named here only so the .cpp can hold one.
        struct Tee;

    private:
        std::unique_ptr<Tee> tee;
    };

    /*  Whether stderr reaches a terminal someone is reading. Not when a
        launcher has pointed it at a log file, and not under Finder. */
    bool stderrIsATerminal();

    /*  The system's own alert, waiting for OK. Windows and macOS; elsewhere
        nothing, since stderr already said it and there is no one alert every
        Linux desktop has. */
    void showAlert (const std::string& title, const std::string& message);
}
