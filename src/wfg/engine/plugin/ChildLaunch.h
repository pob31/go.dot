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
    A CHILD PROCESS, LAUNCHED WITHOUT INHERITING A HANDLE (Phase 9a, §17.6).

    JUCE's ChildProcess creates a Windows process with handle inheritance ON,
    and a socket is an inheritable handle: the first hosted serve on this
    machine handed its HTTP sockets to the plugin child, and every client
    waiting for the server to close a connection waited for a process that
    never would. So on Windows the child is created here, with inheritance off
    and no console window; on the other platforms JUCE's fork-and-exec is
    used, where the sockets carry close-on-exec.

    Two kinds of child use it: the voice child that plays a plugin for every
    voice (ProxyHost), and the editing helper that shows one plugin's own
    window (EditorHost, author's decision of 2026-09-25). What either needs of
    it is whether the child is running, its pid, ending it - and, for the
    helper, letting it bring its window to the front, which Windows allows
    only when the process in front says so.

    NAMES NO JUCE TYPE. The command is words, UTF-8.
*/

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wfg::plugin
{
    class ChildLaunch
    {
    public:
        ChildLaunch();
        ~ChildLaunch();

        ChildLaunch (const ChildLaunch&) = delete;
        ChildLaunch& operator= (const ChildLaunch&) = delete;

        /** The executable, then its words. False when the process could not be made. */
        bool start (const std::vector<std::string>& command);

        bool isRunning() const;

        /** Ends it without asking. */
        void kill();

        /** Nought where the launcher cannot say (JUCE's, off Windows). */
        std::int64_t pid() const noexcept;

        /*  Lets the child take the foreground for its next window. Windows
            gives the foreground only to a process the one in front allows;
            elsewhere there is no such rule and this does nothing. */
        void allowForeground() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
