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
    A new cue, asked for by a button that is always in the same place.

    The author (2026-09-18): "I would place buttons so people have a stable UI
    for this. Lock makes them disappear. It also helps getting started." So the
    window has a row of buttons, one per kind, that does not move and does not
    depend on what is picked - an empty show has a way to begin, and a show in
    show mode has no way to grow (§14.7: the lock is what stops a hand in the
    dark from editing). The page keeps its own add buttons in the inspector,
    which is what keeps §14.16's third rule: every one of these is `cue.create`
    and reachable there.

    WHERE THE NEW CUE GOES is the page's rule: after the picked cue, in the
    picked cue's own parent - a group is a cue and a new cue lands after the
    group as a whole, not inside it - and at the end of the focused list when
    nothing is picked. One guess, and the one that needs no second click.

    AND THE CLIENT DOES NOT DRAW IDENTIFIERS (Media.h): a create is followed
    by finding what it made, at the member position it was asked for, and
    checking that the cue standing there is the one asked for before it is
    picked. std only, like the rest of model/.
*/

#include <cstdint>
#include <string>
#include <vector>

namespace wfg::client::model
{
    /*  The kinds a button is offered for, in the order the buttons stand:
        the ones a show is made of first, then what a cue does to another,
        then the outside world, then structure. Every one of them is a word
        `cue.create` takes; the test holds the two lists together. */
    const std::vector<std::string>& cueKinds();

    /*  The member position a new cue takes to land AFTER `cueId` in the
        order given, or -1 - "the end" - when the cue is not among those
        members, which is what a picked cue that has just been deleted or a
        stale picture would ask. */
    int positionAfter (const std::string& orderText, const std::string& cueId);

    /*  A CREATE STILL TO BE FOUND: the cue asked for, and where. Held by the
        window between one pass and the next, exactly as an Import is. */
    struct Creation
    {
        std::string parent;          ///< the list or group the cue was created in
        int index = 0;               ///< the member position it was asked for
        std::string kind;            ///< the kind the create was told to make
        std::uint64_t askedAt = 0;   ///< the revision when the create was sent
        int waited = 0;              ///< passes since, so a refusal is not waited on for ever
    };

    /*  WHETHER THE CUE AT THE ASKED-FOR POSITION IS THE ONE THIS CREATE
        MADE. Two things must agree - the kind, and no name yet, since a new
        cue is made unnamed so the first thing typed into the inspector is
        what it is called. A stranger passing both is an unnamed cue of the
        same kind somebody else just made in the same place, and picking that
        one costs nothing: picking is this client's own state and never a
        write. */
    bool madeByCreate (const Creation& job, const std::string& kind, const std::string& name);
}
