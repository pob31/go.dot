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
    WHAT A CUE LIST IS DOING THAT NOBODY WROTE DOWN.

    PRD §4.10 keeps what the machine happens to be doing out of the document,
    and these are exactly that: where an operator is POINTING when they ask what
    the show would be at some position, and where the last jump actually landed.
    Neither is a decision anybody saved - a show reopened tomorrow has no aim,
    and that is correct rather than a loss.

    TWO POINTERS AND NOT ONE, which is §3.13's own shape. The AIM is where a
    finger is: a client drags it along the list and the solve follows. The STATE
    POSITION is where a `list.loadToTime` last put the show. After a jump the two
    agree; after a GO or a manual tweak they do not, and that divergence is the
    thing a running view has to show - it is the difference between where the
    operator asked to be and where the show has got to since.

    A CUE AND AN OFFSET, NEVER A WALL TIME. A manual list has no time in it, so
    "the state at 04:12" is a question the document cannot answer; "cue C has
    been running for `offset` seconds" is one it can. An offset of -1 means
    BEFORE C has fired - standby on it, nothing of it done - which is what "take
    it back to cue 12" means and is a different position from nought seconds in.

    THREADING: none of its own. The tick thread writes it through a command; the
    parameter tree reads it while publishing, on that same thread.
*/

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace wfg::cue
{
    /** A position in a list: a cue, and how far into it. */
    struct ListAim
    {
        std::string cue;

        /** Seconds, or -1 for "before this cue has fired at all". */
        double offset = -1.0;

        bool isSet() const noexcept { return ! cue.empty(); }
    };

    /** `"<cue> <offset>"`, which is how both nodes spell one. */
    std::string spellAim (const ListAim& aim);

    /** The other direction. An empty or malformed text is an unset aim. */
    ListAim readAim (const std::string& text);

    /*  ONE STEP A LIST TOOK: when, what, and how it was asked for.

        `origin` is a letter because the node spells sixty-four of these in one
        string and a word each would be a paragraph: `g` for a GO, `f` for a cue
        fired by name, `t` for a trigger. */
    struct Step
    {
        std::int64_t tick = 0;
        std::string cue;
        char origin = 'g';
    };

    /** `<tick>:<cue>:<origin>`, which is how the node spells one. */
    std::string spellStep (const Step& step);

    //==============================================================================
    class ListState
    {
    public:
        void aimAt (const std::string& list, const ListAim& aim) { aims[list] = aim; }
        void landedAt (const std::string& list, const ListAim& aim) { positions[list] = aim; }

        ListAim aimOf (const std::string& list) const { return lookUp (aims, list); }
        ListAim positionOf (const std::string& list) const { return lookUp (positions, list); }

        /*  A step taken, newest kept last. Bounded at sixty-four because the
            node that publishes it is one string, and because the use is going
            BACK a few steps rather than reading an evening: an operator who
            wants act one again asks the solver for act one, not the history.

            §3.13's manual waypoints, kept for the operator rather than by them
            (decision R): each of these is a load-to-time target, `<cue> -1`. */
        void stepped (const std::string& list, const Step& step)
        {
            auto& steps = histories[list];
            steps.push_back (step);
            ++taken;

            while (steps.size() > kept)
                steps.erase (steps.begin());
        }

        /** Newest last. */
        const std::vector<Step>& historyOf (const std::string& list) const
        {
            static const std::vector<Step> none;
            const auto found = histories.find (list);
            return found == histories.end() ? none : found->second;
        }

        /** How many steps every list has taken in total, for a hook that wants
            to notice a new one without comparing lists. */
        std::uint64_t stepsTaken() const noexcept { return taken; }

        /** A show being closed takes all of it with it: it is about a session. */
        void clear() { aims.clear(); positions.clear(); histories.clear(); }

        static constexpr std::size_t kept = 64;

    private:
        static ListAim lookUp (const std::map<std::string, ListAim>& from,
                               const std::string& list)
        {
            const auto found = from.find (list);
            return found == from.end() ? ListAim {} : found->second;
        }

        std::map<std::string, ListAim> aims, positions;
        std::map<std::string, std::vector<Step>> histories;
        std::uint64_t taken = 0;
    };
}
