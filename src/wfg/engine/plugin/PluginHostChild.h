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
    THE CHILD PROCESS THAT HOSTS A PLUGIN (Phase 9a, §17.6): the same binary
    re-invoked as `wfg plugin-host`, dispatched at the top of runConsole before
    any verb is read, beside Tracktion's own scan child.

        wfg plugin-host --region=<path> --plugin=<identifier> --instances=N
                        --channels=C --parent-pid=P [--preset=<file>]

    It maps the region the parent laid out, checks the layout is the one it
    was built for, brings the plugin up once per lane, says so in the header,
    and then a worker at real-time priority polls the lanes: on a request it
    applies the lane's changed values, resets the instance if asked, processes
    the block in place and answers. It leaves when the parent sets
    `childShouldExit`, or when the parent's process is gone - checked once a
    second, so a crashed engine leaves no orphan behind.

    THIS PR BUILDS THE TEST CHILD ONLY (plan decision 15). The reserved
    identifier `godot:test-gain` makes it do what spike 07's child did: p0 is a
    linear gain resting at a half, and p1 at one makes the process abort -
    which is how a driver kills a plugin mid-show with no task manager. Any
    other identifier is a `failed` with a sentence until PR 9a.7 brings the
    format manager in.

    NAMES NO JUCE TYPE. The .cpp maps the file with JUCE and nothing else.
*/

#include <string>
#include <vector>

namespace wfg::plugin
{
    /** Runs the child to completion. `args` are the words after `plugin-host`. */
    int runPluginHost (const std::vector<std::string>& args);

    /*  If argv[1] is `plugin-host`, runs it, stores the exit code and answers
        true - the caller then exits with it without reading a verb. False for
        an ordinary run. */
    bool runPluginHostIfAsked (int argc, char** argv, int& exitCode);

    /** The verb, spelled once. */
    inline constexpr const char* pluginHostVerb = "plugin-host";
}
