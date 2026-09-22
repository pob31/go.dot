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

#include <wfg/client/model/DirectOuts.h>

#include <wfg/client/model/OutputList.h>
#include <wfg/client/model/Text.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <map>
#include <sstream>

namespace wfg::client::model
{
    namespace
    {
        /*  The engine publishes `<bus> <cue>` pairs, which is the encoding
            `slot/usage` already uses. Read into a map by bus, because that is
            the order a menu asks in. */
        std::map<std::string, std::string> pairsOf (const std::string& text)
        {
            std::map<std::string, std::string> out;
            std::istringstream words { text };

            std::string bus, cue;

            while (words >> bus >> cue)
                out[bus] = cue;

            return out;
        }
    }

    std::vector<OutMark> readOutMarks (const tree::TreeSnapshot& snapshot,
                                       const std::string& cueId)
    {
        return readOutMarks (snapshot, cueId, readOutputs (snapshot));
    }

    std::vector<OutMark> readOutMarks (const tree::TreeSnapshot& snapshot,
                                       const std::string& cueId,
                                       const std::vector<OutputRow>& outputs)
    {
        if (cueId.empty())
            return {};

        const auto prefix = "/godot/cue/" + cueId + "/";
        const auto busy = pairsOf (text (snapshot, prefix + "outsBusy"));
        const auto maybe = pairsOf (text (snapshot, prefix + "outsMaybe"));

        std::vector<OutMark> out;

        for (const auto& row : outputs)
        {
            if (row.kind != "direct")
                continue;

            OutMark mark;
            mark.busId = row.id;

            /*  TAKEN WINS OVER UNDECIDED where both somehow name the same
                output: the analysis puts each output in one list or the other,
                and if that ever stopped being true the more definite answer is
                the one worth showing. */
            if (const auto found = busy.find (row.id); found != busy.end())
            {
                mark.state = OutMark::State::taken;
                mark.byCue = text (snapshot, "/godot/cue/" + found->second + "/name");

                if (mark.byCue.empty())
                    mark.byCue = found->second;
            }
            else if (const auto perhaps = maybe.find (row.id); perhaps != maybe.end())
            {
                mark.state = OutMark::State::undecided;
                mark.byCue = text (snapshot, "/godot/cue/" + perhaps->second + "/name");

                if (mark.byCue.empty())
                    mark.byCue = perhaps->second;
            }

            out.push_back (std::move (mark));
        }

        return out;
    }

    std::string markedLabel (const std::string& name, const std::string& widthWord,
                             const OutMark& mark)
    {
        //  A middle dot, as the patch matrix names its rows.
        auto out = name + " · " + widthWord;

        switch (mark.state)
        {
            case OutMark::State::taken:
                /*  NAMING THE CUE IN THE WAY, which is the fact somebody
                    actually needs: "taken" alone sends them hunting for what
                    took it. */
                out += mark.byCue.empty() ? " — taken"
                                          : " — taken by \"" + mark.byCue + "\"";
                break;

            case OutMark::State::free:
                out += " — free";
                break;

            case OutMark::State::undecided:
                /*  NOTHING AT ALL, on purpose. The author's own rule: where
                    nothing in the show decides it, the operator decides, and
                    an empty space says that better than the word "unknown" -
                    which reads as a fault in the program rather than as an
                    honest answer about the show. */
                break;
        }

        return out;
    }

    std::optional<std::string> newClash (const std::vector<OutMark>& before,
                                         const std::vector<OutMark>& after,
                                         const std::string& ownOut,
                                         const std::string& busName,
                                         const std::string& cueName)
    {
        if (ownOut.empty())
            return std::nullopt;

        const auto stateOf = [&ownOut] (const std::vector<OutMark>& marks)
        {
            for (const auto& mark : marks)
                if (mark.busId == ownOut)
                    return mark.state;

            return OutMark::State::free;
        };

        const auto was = stateOf (before);
        const auto now = stateOf (after);

        /*  ONLY WHAT THE MOVE MADE TRUE. An overlap that was already there is
            not news, and a notice that fires for a state somebody has already
            looked at is one they learn to scroll past. */
        if (now != OutMark::State::taken || was == OutMark::State::taken)
            return std::nullopt;

        std::string blocking;

        for (const auto& mark : after)
            if (mark.busId == ownOut)
                blocking = mark.byCue;

        const auto out = busName.empty() ? std::string ("that output") : "\"" + busName + "\"";
        const auto who = blocking.empty() ? std::string ("another cue") : "\"" + blocking + "\"";
        const auto moved = cueName.empty() ? std::string ("this cue") : "\"" + cueName + "\"";

        /*  WARNED AND NEVER REFUSED. The move has already happened, undo is
            one gesture away, and the two cues will sum in the meantime - which
            is the failure mode the author asked for by name. */
        return moved + " and " + who + " can now both be on " + out
                 + "; they will sum. Mark either as a shared out if that is meant.";
    }
}
