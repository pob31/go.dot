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
    THE PARTS OF A CUE (namespace draft §38, the author's request of
    2026-10-07): a named group of a cue's settings that moves as one - its EQ,
    its sends, its effects, its time and loops - copied from one cue and
    pasted onto others, and kept in the show as a template a new cue is born
    from.

    PRD §3.24 asked for it in these words: "structured copy-paste of named
    field groups rather than a feature per field". This file is the one table
    that says which rows and which children each group holds, so the copy,
    the paste, a template's capture and its stamp cannot come to disagree
    about what "the EQ" is.

    A PASTE REPLACES THE PART WHOLE (WO, the author's pick): the target ends
    up as the source was for that part - a row the source left at its default
    goes back to its default, a send the source does not have is taken away,
    the chain of effects is swapped whole. That is why a part names its rows
    and not merely the ones somebody changed: an absent row is a value.

    WHAT NO PART CARRIES, and each is a decision:
      * the file, the name, the number - what the cue IS rather than how it
        is set (WP);
      * a lane, the level lane or a send's - drawn in the file's own seconds,
        which mean nothing on another file (WV);
      * the strip a sample sits on - a place on a desk, one cue's at a time.

    Vendor-free apart from the words: no JUCE, no document. The doing is
    `ShowDocument::pastePart`.
*/

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::doc::parts
{
    enum class Part
    {
        eq,         ///< the 23 EQ rows                                - media, mic
        sends,      ///< the Send children, by the bus they feed       - media, mic
        fx,         ///< the Fx children, the chain in order           - media, mic, same kind only (WU)
        time,       ///< start offset, speed and the Range children    - media, video
        speed,      ///< speed and how it is applied, alone            - media, video
        mix,        ///< level, DCA, colour, where it plays            - media, video
        play,       ///< how a sample answers a hand on a strip        - media
        picture     ///< canvas, layer, blend, geometry, grade, mask   - video
    };

    /** The word a part is spelt with on the wire and in a fragment. */
    std::string_view wordFor (Part part) noexcept;

    /** The part a word names, or nothing for a word that is none. */
    std::optional<Part> partForWord (std::string_view word) noexcept;

    /*  Several words, space-separated, in the order given: nothing when any
        word is not a part, when one is named twice, or when there are none. */
    std::optional<std::vector<Part>> partsForWords (std::string_view words);

    /** The words again, space-separated, in this order. */
    std::string wordsFor (const std::vector<Part>& chosen);

    /*  Whether a cue of this element - "Media", "Mic", "Video" - has the
        part at all. A part that does not fit is refused, never half-applied. */
    bool fits (Part part, std::string_view element) noexcept;

    /*  THE ROWS on the cue itself that the part holds, by name. The union over
        every kind it fits: a row the element does not carry is passed over
        where it is used (a movie has no `rateMode`), which the schema answers. */
    const std::vector<std::string_view>& rowsOf (Part part);

    /*  THE CHILD ELEMENT the part holds - "Send", "Fx", "Range" - or empty
        for a part that is rows only. */
    std::string_view childOf (Part part) noexcept;

    /*  The rows of each such child that travel with it. A send's lane is not
        among them (WV); a child's identity never is, since a pasted child is
        a new object under a new name. */
    const std::vector<std::string_view>& childRowsOf (Part part);

    /*  WHAT A TEMPLATE OF THIS ELEMENT CARRIES (WP): everything that is not
        bound to the file. A media cue's is mix, play, EQ, sends, effects and
        speed; a picture's is mix, picture and speed. Empty for an element no
        template is made of. */
    std::vector<Part> templatePartsFor (std::string_view element);
}
