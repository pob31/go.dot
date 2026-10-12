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
    WHAT KIND OF GROUP THIS IS, ASKED ONE WAY EVERYWHERE (namespace draft §60,
    TG.2).

    A group's behaviour is two words, `mode` and `advance`, and for most of the
    project's life the code asked them as text wherever it needed an answer:
    `mode != "timeline" && advance != "auto"` for a manual sequence, `mode ==
    "sampler"` for a bank a hand plays, some thirty places in all. That was
    safe while there were three modes, because every compare that was not
    about the one it named fell to the right side by elimination. A fourth
    mode breaks the elimination: a time group read by `mode != "timeline" &&
    advance != "auto"` is a manual sequence - its `advance` reads `manual` by
    default - and `trigger.fire` of a member would be refused `needs-go`, the
    pointer would walk into it, a seek would be offered on it. So the
    questions are named here once, by what they mean, and each place asks the
    one it meant.

    THE QUESTIONS, and what a time group answers to each:
    - `isSampler`: a bank of strips a hand plays (PRD §3.27). Asked by what is
      about strips - a takeover, a member's strip, a press - and nothing else.
    - `isTimeGroup`: a group whose members a time starts - LTC, MTC or the wall
      clock (§60, AIE).
    - `armedUntilStopped`: either of those two. GO arms it, it spawns nothing
      by itself and it never ends on its own; the standby steps past it and
      never into it, the horizon prepares nothing inside it, a sequence that
      holds one hands over at once, Doh! starts it again from its top.
    - `isTimeline`: every member scheduled at entry, by its offset.
    - `isAutoSequence`: members one after another with no GO between them.
    - `pacedByMachine`: a timeline or an automatic sequence - a scene whose
      seconds the walk can know, which a seek can move and which reaches a cue
      inside it under the GO that started it. NOT a time group: its members
      wait for a code, and nothing under the GO reaches them.
    - `isManual`: a sequence a person steps through with GO, the only group the
      walk descends into and the pointer lands inside of.

    AN EMPTY WORD IS THE TABLE'S DEFAULT, as an absent attribute is: `mode`
    reads `sequence` and `advance` reads `manual` when nobody wrote them down.
    The document's getters put the defaults back, and most readers ask through
    them; a node read raw - the pointer's walk, the document's own door, a row
    the tree has not published yet - gets the same answer without them. An
    invalid node reads two empty words, and every caller asks validity first.

    STD ONLY, so the desktop client's model asks the published tree the same
    questions with the same functions (namespace draft §14.16), as FadeMoves.h
    lets it read a fade's lists with the engine's own parser.
*/

#include <string>
#include <string_view>

namespace wfg::cue
{
    /*  The two words, as the reader that made this read them. Copied, not
        viewed: a reader's text is usually a temporary. */
    struct GroupMode
    {
        std::string mode;
        std::string advance;
    };

    /*  The words `group/mode` and `group/advance` take (docs/parameters). */
    inline constexpr std::string_view timelineMode = "timeline";
    inline constexpr std::string_view sequenceMode = "sequence";
    inline constexpr std::string_view samplerMode  = "sampler";
    inline constexpr std::string_view timeMode     = "time";
    inline constexpr std::string_view autoAdvance  = "auto";

    inline bool isSampler (const GroupMode& group) noexcept
    {
        return group.mode == samplerMode;
    }

    inline bool isTimeGroup (const GroupMode& group) noexcept
    {
        return group.mode == timeMode;
    }

    inline bool armedUntilStopped (const GroupMode& group) noexcept
    {
        return isSampler (group) || isTimeGroup (group);
    }

    inline bool isTimeline (const GroupMode& group) noexcept
    {
        return group.mode == timelineMode;
    }

    inline bool isSequence (const GroupMode& group) noexcept
    {
        return group.mode.empty() || group.mode == sequenceMode;
    }

    inline bool isAutoSequence (const GroupMode& group) noexcept
    {
        return isSequence (group) && group.advance == autoAdvance;
    }

    inline bool pacedByMachine (const GroupMode& group) noexcept
    {
        return isTimeline (group) || isAutoSequence (group);
    }

    inline bool isManual (const GroupMode& group) noexcept
    {
        return isSequence (group) && group.advance != autoAdvance;
    }
}
