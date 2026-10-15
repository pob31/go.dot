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
    A SCRIPT CUE'S CODE, AS THE EDITOR AT THE FOOT READS IT (namespace draft
    §66, AMO, AMT): the source and its language, the lock, and its live run's
    state and last line - what the editor shows under the code. The inspector
    does not show the source (AMO): this is its one face.
*/

#include <string>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    struct CodeReading
    {
        std::string cueId;
        std::string language;       // javascript or lua
        std::string text;
        bool locked = false;

        //  Its live run, the newest: a script cue has one at a time.
        std::string runId;
        std::string state;          // starting, running, late, stuck
        std::string said;           // the last line it printed, or its problem
    };

    CodeReading readCodeFoot (const tree::TreeSnapshot& snapshot, const std::string& cueId);

    /*  THE LINE A PROBLEM NAMES, as the language said it - "... (line 7)" - or
        nought when it names none: where the editor marks it. */
    int lineOfProblem (const std::string& said);

    /*  How the state and the last line read under the code, in words. */
    std::string codeStatusWords (const CodeReading& reading);
}
