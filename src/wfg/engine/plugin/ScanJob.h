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
    THE APP'S PLUGIN SCAN (2026-09-26, the author's decision: a Scan button
    in Show settings, Plugins, running the same safe scan as the command line).

    IT IS THE COMMAND LINE'S SCAN, run as a child: `wfg plugins --scan`, with
    a progress file to read and a stop file to leave. Not the scan's code run
    inside serve, for three reasons the code gave: Tracktion's scan
    coordinator launches its workers with JUCE's ChildProcess, which on
    Windows hands them every inheritable handle - serve's sockets among them,
    the hang §17.12 records; the scan sets a process-wide environment
    variable for its workers; and it waits in a message loop of its own. So
    the child is launched through ChildLaunch, with no handle to inherit, and
    everything it has to say is in two files: its progress, and known.xml.

    MESSAGE THREAD, every method. A timer reads the progress file into the
    scan table while the child runs; when it has gone, `Finished` is called
    with what the machine knows, how many files were given up on, and why it
    failed if it did - the caller refreshes its list from known.xml and says
    so as `plugin.scanned`, which ends the scan in the table.

    NAMES NO JUCE TYPE.
*/

#include <wfg/engine/plugin/ScanTable.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wfg::plugin
{
    class ScanJob
    {
    public:
        /*  The executable and the words before the verb's own, as ProxyLaunch:
            the console itself in a session, and whatever a test names. */
        struct Launch
        {
            std::string executable;
            std::vector<std::string> leadingArgs;
        };

        using Finished = std::function<void (int found, int skipped, const std::string& problem)>;

        /** Milliseconds between two looks at the progress file. */
        static constexpr int pollMs = 200;

        ScanJob (std::string storageFolder, Launch launch, ScanTable& table, Finished finished);

        /** Stops a scan still running: the stop file, a moment, then the kill. */
        ~ScanJob();

        ScanJob (const ScanJob&) = delete;
        ScanJob& operator= (const ScanJob&) = delete;

        /*  `formatWord` empty for every format; `retryFile` one skipped file
            to scan alone; `folder` one to search beside the format's own.
            Nothing happens while a scan runs. When the child cannot be made,
            `Finished` is called at once with the sentence. */
        void start (const std::string& formatWord, const std::string& retryFile, const std::string& folder = {});

        bool running() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
