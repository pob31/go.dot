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

#include <wfg/engine/cue/TakeTable.h>

#include <algorithm>
#include <cmath>

namespace wfg::cue
{
    const Take& TakeTable::of (const std::string& channel) const
    {
        static const Take nothing;
        const auto found = takes.find (channel);
        return found != takes.end() ? found->second : nothing;
    }

    void TakeTable::ask (const std::string& channel, TakeVerb verb, double in, double out)
    {
        if (queueing)
            pending.push_back ({ channel, verb, in, out });
    }

    bool TakeTable::wouldStartLayer (const std::string& channel, TakeVerb verb) const
    {
        const auto& state = of (channel).state;

        return (verb == TakeVerb::record || verb == TakeVerb::overdub)
                 && (state == "looping" || state == "held");
    }

    void TakeTable::press (const std::string& channel, TakeVerb verb, int maxLayers)
    {
        auto& take = takes[channel];
        const auto room = take.layers < maxLayers;
        const auto& state = take.state;

        /*  WHAT MOVES IS WHAT IS ASKED FOR: each branch that changes the
            account queues the press, and one that changes nothing asks the
            take for nothing either. */
        const auto moveTo = [this, &take, &channel, verb] (const char* next, int layersAdded = 0)
        {
            take.state = next;
            take.layers += layersAdded;
            ask (channel, verb);
        };

        switch (verb)
        {
            case TakeVerb::record:
                if (state == "empty")
                {
                    take.length = 0.0;
                    take.problem.clear();
                    moveTo ("recording");
                }
                else if (state == "recording")    moveTo ("looping");
                else if (state == "looping")      { if (room) moveTo ("overdubbing"); }
                else if (state == "overdubbing")  moveTo ("looping", 1);
                else if (state == "held")         moveTo (room ? "overdubbing" : "looping");
                break;

            case TakeVerb::loop:
                if (state == "recording")         moveTo ("looping");
                else if (state == "overdubbing")  moveTo ("looping", 1);
                else if (state == "held")         moveTo ("looping");
                break;

            case TakeVerb::overdub:
                if (state == "recording")         moveTo ("looping");
                else if (state == "looping")      { if (room) moveTo ("overdubbing"); }
                else if (state == "overdubbing")  moveTo ("looping", 1);
                else if (state == "held")         moveTo (room ? "overdubbing" : "looping");
                break;

            case TakeVerb::undo:
                if (state == "recording")
                {
                    take = Take {};
                    ask (channel, verb);
                }
                else if (state == "overdubbing")
                {
                    moveTo ("looping");
                }
                else if ((state == "looping" || state == "held") && take.layers > 0)
                {
                    --take.layers;
                    ask (channel, verb);
                }
                break;

            case TakeVerb::clear:
                if (state != "empty")
                {
                    take = Take {};
                    ask (channel, verb);
                }
                break;

            case TakeVerb::hold:
                if (state == "recording" || state == "looping")  moveTo ("held");
                else if (state == "overdubbing")                 moveTo ("held", 1);
                break;

            case TakeVerb::points:
                break;
        }
    }

    void TakeTable::closed (const std::string& channel, double seconds, const std::string& how,
                            double longestSeconds)
    {
        auto& take = takes[channel];

        //  TOO LATE: emptied since the audio thread closed it.
        if (take.state == "empty")
            return;

        take.length = std::max (0.0, seconds);
        take.loopIn = 0.0;
        take.loopOut = take.length;

        if (take.state == "recording")
            take.state = how == "held" ? "held" : "looping";

        if (how == "full")
            take.problem = "the take reached its " + std::to_string (std::llround (longestSeconds))
                             + " seconds and was closed";
    }

    void TakeTable::release (const std::string& channel)
    {
        if (takes.count (channel) > 0)
            press (channel, TakeVerb::hold, 0);
    }

    bool TakeTable::setPoint (const std::string& channel, bool inPoint, double seconds)
    {
        auto found = takes.find (channel);

        if (found == takes.end() || found->second.length <= 0.0
              || found->second.state == "empty" || found->second.state == "recording")
            return false;

        auto& take = found->second;
        const auto length = take.length;

        /*  IN THE TAKE, AND TWO CROSSFADES APART, the moved point giving way:
            an in point pushed past the out stops two crossfades before it, and
            the out point pulled under the in stops two after. */
        if (inPoint)
            take.loopIn = std::clamp (seconds, 0.0, std::max (0.0, take.loopOut - shortestLoopSeconds));
        else
            take.loopOut = std::clamp (seconds, std::min (length, take.loopIn + shortestLoopSeconds), length);

        ask (channel, TakeVerb::points, take.loopIn, take.loopOut);
        return true;
    }

    void TakeTable::setPlayhead (const std::string& channel, double seconds)
    {
        if (auto found = takes.find (channel); found != takes.end())
            found->second.playhead = seconds;
    }

    std::vector<std::string> TakeTable::channels() const
    {
        std::vector<std::string> out;
        out.reserve (takes.size());

        for (const auto& [channel, take] : takes)
            out.push_back (channel);

        return out;
    }

    std::vector<TakePress> TakeTable::takePresses()
    {
        std::vector<TakePress> out;
        out.swap (pending);
        return out;
    }

    void TakeTable::clear()
    {
        takes.clear();
        pending.clear();
    }
}
