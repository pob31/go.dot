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

/*  WHICH STRIP EACH MEMBER OF A SAMPLER GROUP IS PLAYED FROM (PRD §3.27).

    Until 2026-09-25 the answer was only positional - the Nth media member on
    the Nth sampler strip - and PRD §3.27 held the other half as proposed: "a
    member may pin its strip". The author asked for it with the D700 on the
    desk: "I need to specify which track goes where", from a menu on the
    member that says, like the direct-out menu, what the cue list put on each
    strip before - "unless it's free" - and the same for pads.

    THE RULE, in one place so the arm, the re-arm and the menu cannot disagree:
    1. a member whose `strip` names a sampler strip of this layout has it -
       the first in member order, when two of them name the same one;
    2. every other media member takes the next sampler strip nobody in its
       group has pinned, in the roster's order (surfaces in document order,
       strips in index order, DCA strips skipped);
    3. a member left with no strip is not armed, as before: the group is
       partially armed and says so.

    A pin that names nothing usable - a strip deleted since, a DCA strip - is
    treated as no pin. The dangling reference is `wfg validate`'s to report;
    a sound that still plays from the next free strip beats one that is
    silently not armed.

    A PURE FUNCTION OF THE DOCUMENT, so a replay, load-to-time and a client
    reading the tree all see the placement the Runner acts on.
*/

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <juce_data_structures/juce_data_structures.h>

namespace wfg::doc { class ShowDocument; }

namespace wfg::cue
{
    /*  Every sampler strip of the show, in the order the automatic placement
        fills them: surfaces in document order, their strips in index order,
        strips whose role is not `sampler` left out. */
    std::vector<std::string> samplerStripsOf (const doc::ShowDocument& document);

    /** One media member of a sampler group and the strip it is played from. */
    struct Placement
    {
        std::string cue;
        std::string strip;          ///< empty: no strip left for it
        bool pinned = false;        ///< on the strip its own `strip` row names
    };

    /*  A sampler group's enabled media members, in member order, each on its
        strip by the rule above. `roster` is `samplerStripsOf`, passed in so a
        caller asking about many groups reads the surfaces once. */
    std::vector<Placement> placeMembers (const doc::ShowDocument& document,
                                         const juce::ValueTree& group,
                                         const std::vector<std::string>& roster);

    //==========================================================================
    /*  THE PLACEMENT OF EVERY SAMPLER MEMBER IN THE SHOW, AND WHAT CAME BEFORE
        IT, cached by show revision for the tree.

        "Before" is the author's word (2026-09-25): for each strip, the member
        that the nearest EARLIER sampler group in the same list put on it -
        list order, which is the order a show runs in - so the menu can say
        what a choice replaces. A strip no earlier group used is free. */
    class SamplerLayout
    {
    public:
        void ensureBuilt (const doc::ShowDocument& document);

        /** The strip this member is played from, or empty. `/godot/cue/<id>/stripNow`. */
        std::string stripOf (const std::string& cueId) const;

        /*  `<strip> <cue>` pairs, space-separated: every strip an earlier
            sampler group in this member's list used, and the member it put
            there last. `/godot/cue/<id>/stripsBefore`. */
        std::string stripsBeforeOf (const std::string& cueId) const;

    private:
        bool built = false;
        std::uint64_t builtRevision = 0;
        std::map<std::string, std::string> placed;
        std::map<std::string, std::string> before;
    };
}
