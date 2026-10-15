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

#include <wfg/client/model/Code.h>
#include <wfg/client/model/Text.h>

#include <cctype>

namespace wfg::client::model
{
    CodeReading readCodeFoot (const tree::TreeSnapshot& snapshot, const std::string& cueId)
    {
        CodeReading out;
        const auto base = "/godot/cue/" + cueId + "/";

        if (cueId.empty() || text (snapshot, base + "kind") != "script")
            return out;

        out.cueId = cueId;
        out.language = text (snapshot, base + "language");
        out.text = text (snapshot, base + "source");
        out.locked = isYes (flag (snapshot, "/godot/document/locked"));

        for (const auto& run : words (text (snapshot, "/godot/run/order")))
        {
            const auto at = "/godot/run/" + run + "/";
            const auto state = text (snapshot, at + "state");
            if (text (snapshot, at + "cue") != cueId || state == "done")
                continue;

            out.runId = run;
            out.state = state == "failed" ? std::string ("stuck") : text (snapshot, at + "processState");
            out.said = text (snapshot, at + "said");
            if (state == "failed")
                out.runId.clear();
        }
        return out;
    }

    int lineOfProblem (const std::string& said)
    {
        static constexpr std::string_view mark = "(line ";
        const auto at = said.rfind (mark);
        if (at == std::string::npos)
            return 0;

        int line = 0;
        for (auto i = at + mark.size(); i < said.size() && std::isdigit (static_cast<unsigned char> (said[i])); ++i)
            line = line * 10 + (said[i] - '0');
        return line;
    }

    std::string codeStatusWords (const CodeReading& reading)
    {
        if (reading.cueId.empty())
            return {};

        const auto language = reading.language == "lua" ? std::string ("Lua") : std::string ("JavaScript");

        if (reading.state.empty())
            return language + " - runs when its cue runs";
        if (reading.state == "starting")
            return language + " - starting";
        if (reading.state == "stuck")
            return language + " - stuck, and ended";
        if (reading.state == "late")
            return language + " - running, its last tick past the budget";

        return language + " - running";
    }
}
