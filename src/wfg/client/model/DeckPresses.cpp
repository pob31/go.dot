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

#include <wfg/client/model/DeckPresses.h>

#include <wfg/client/model/Gestures.h>

#include <algorithm>

namespace wfg::client::model
{
    void DeckPresses::letGo (const Held& down, std::vector<Event>& out)
    {
        if (! down.cue.empty())
            out.push_back (gesture::releaseCue (down.cue));

        if (! down.trigger.empty())
            out.push_back (gesture::releaseTrigger (down.trigger));
    }

    std::vector<Event> DeckPresses::press (int pointer, std::size_t index, const DeckKeyRow& key, int dragging)
    {
        std::vector<Event> out;

        for (const auto stale : turns.sweep (dragging))
            if (const auto found = holding.find (stale); found != holding.end())
            {
                letGo (found->second, out);
                holding.erase (found);
            }

        if (! turns.down (pointer) || held (index))
            return out;

        Held down;
        down.index = index;

        if (key.action == "cue" && ! key.cue.empty())
        {
            out.push_back (gesture::pressCue (key.cue));
            down.cue = key.cue;
        }
        else if (key.action == "learnt" && ! key.trigger.empty())
        {
            out.push_back (gesture::fireTrigger (key.trigger));
            down.trigger = key.trigger;
        }
        else if (key.action == "go")          out.push_back (gesture::go());
        else if (key.action == "esc")         out.push_back (gesture::stopAll());
        else if (key.action == "doh")         out.push_back (gesture::doh());
        else if (key.action == "pause")       out.push_back (key.label == "Play" ? gesture::play() : gesture::pause());
        else if (key.action == "standbyUp")   out.push_back (gesture::standbyPrevious());
        else if (key.action == "standbyDown") out.push_back (gesture::standbyNext());

        //  Held until the lift even when it is a show key: a second finger on it is not a second press.
        holding.emplace (pointer, down);
        return out;
    }

    std::vector<Event> DeckPresses::release (int pointer)
    {
        std::vector<Event> out;
        turns.up (pointer);

        if (const auto found = holding.find (pointer); found != holding.end())
        {
            letGo (found->second, out);
            holding.erase (found);
        }

        return out;
    }

    std::vector<Event> DeckPresses::releaseAll()
    {
        std::vector<Event> out;

        for (const auto& entry : holding)
            letGo (entry.second, out);

        holding.clear();
        turns.clear();
        return out;
    }

    bool DeckPresses::held (std::size_t index) const
    {
        return std::any_of (holding.begin(), holding.end(),
                            [index] (const auto& entry) { return entry.second.index == index; });
    }
}
