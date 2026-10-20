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

#include <algorithm>

/*  WHERE A RANGE ENDS, SAID ONCE (namespace draft §99, AXQ-AXS).

    A range's `out` is a number of seconds, and the file it is a number of is
    not: a frozen edit is bounced to whole samples, a twentieth of one shorter
    than its sections put together; a sound locked to a movie takes the
    movie's ranges and is often a few frames shorter; a file is replaced by
    one of another length. A number written against one of those and armed
    against another refused the cue - the author's two cues of 2026-10-20,
    one 1.2 microseconds past its bounce and one 0.42 seconds past its sound,
    both said as "missing".

    So a range that runs to the end SAYS so (`toEnd`), and is as long as what
    the cue plays, however long that is; and any other is as long as its
    number or what the cue plays, whichever is shorter. A length nobody knows
    yet - the file not read, a replay of a log that never named it - leaves
    the number as it is, and the arm, which has the file open, asks again.

    Std only, so the window's model reads it as the engine does (the client
    boundary, §14.16 rule d).
*/
namespace wfg::cue
{
    /*  A MILLISECOND: the resolution a range is placed at by a hand on a bar
        (`sameInstant`), and how near the end a number must come to BE the
        end - at a door that knows the length, and for the mark that says a
        range reaches past it. */
    inline constexpr double rangeEndSlack = 0.001;

    /*  The second a range ends at: what the cue plays, as long as it is, for
        one that runs to the end; its number, no further than that, for any
        other. `playedLength` of nought or less is a length not known. */
    inline double rangeOutOf (double out, bool toEnd, double playedLength) noexcept
    {
        if (! (playedLength > 0.0))
            return out;

        return toEnd ? playedLength : std::min (out, playedLength);
    }

    /*  How far a range's number reaches past the end of what the cue plays,
        when that is more than the slack; nought for one that runs to the end,
        one that stops short of it, or a length not known. What the cue's mark
        says (§99, AXT): the range plays to the end, and somebody may want to
        know that it was written longer. */
    inline double rangeOverrunOf (double out, bool toEnd, double playedLength) noexcept
    {
        if (toEnd || ! (playedLength > 0.0) || ! (out - playedLength > rangeEndSlack))
            return 0.0;

        return out - playedLength;
    }

    /*  Whether a number at a door that knows the length means the end: within
        the slack of it, or past it. */
    inline bool reachesTheEnd (double out, double playedLength) noexcept
    {
        return playedLength > 0.0 && out >= playedLength - rangeEndSlack;
    }
}
