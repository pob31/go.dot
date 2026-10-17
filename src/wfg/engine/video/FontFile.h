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
    A FONT FILE'S NAMES, READ BY GO.DOT ITSELF (namespace draft §62, AKY): the
    family a face belongs to, its style, whether it is bold or italic, its
    PostScript name, and what its licence says about carrying it - from the
    OpenType `name`, `OS/2` and `head` tables of a `.ttf`, an `.otf`, or each
    face of a `.ttc`.

    WHY NOT JUCE'S READING: JUCE asks the system, and the same file names its
    family differently on Windows (DirectWrite's localised names), macOS
    (CoreText) and Linux (FreeType). A show carrying a font must find it under
    one name on every machine it opens on (AKW), so the names come from the
    file's own tables, read the same way everywhere.

    A FONT FILE IS OUTSIDE INPUT - it may come from anywhere (`fonts.add`) - so
    every read is checked against the file's end and a file that does not read
    is an error in words, never a crash.

    std only: the engine indexes the show's fonts with it, the window names
    them, the QLab importer matches a PostScript name.
*/

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::video::fontfile
{
    /** One face of a font file: a `.ttf` or `.otf` has one, a `.ttc` several. */
    struct Face
    {
        int index = 0;              ///< the face's place in a collection, nought in a single file
        std::string family;         ///< the typographic family (name 16), else the family (name 1)
        std::string legacyFamily;   ///< name 1, which Windows groups four styles under
        std::string style;          ///< the typographic subfamily (name 17), else the subfamily (name 2)
        std::string fullName;       ///< name 4, "DejaVu Sans Mono Bold"
        std::string postScript;     ///< name 6, "DejaVuSansMono-Bold"
        int weight = 400;           ///< OS/2's weight class, 100 to 900
        bool bold = false;
        bool italic = false;        ///< italic or oblique
        std::uint16_t embedding = 0;    ///< OS/2's fsType, the licence's word on carrying the font

        /** The licence says "restricted licence embedding": not to be carried without asking (ALB). */
        bool restricted() const noexcept { return (embedding & 0x000Fu) == 0x0002u; }
    };

    struct Reading
    {
        std::vector<Face> faces;
        std::string error;          ///< why nothing was read; empty when the faces are good
    };

    Reading read (const std::uint8_t* bytes, std::size_t size);

    /** `.ttf`, `.otf`, `.ttc` or `.otc`, case-blind: the files `fonts/` holds. */
    bool isFontFileName (std::string_view name);

    /** Two family names are the same family: compared without case, ASCII letters folded. */
    bool sameFamily (std::string_view a, std::string_view b);

    /*  THE FACE THAT DRAWS bold or italic from a family's faces (AKZ): the italic
        ones when italic is asked and there are any, then the weight nearest
        700 for bold or 400 for regular. What the face lacks Go.dot draws
        itself - a family with no bold face is emboldened, one with no italic
        slanted - so `embolden` and `slant` say so. `index` is into `faces`, -1
        when it is empty. */
    struct Pick
    {
        int index = -1;
        bool embolden = false;
        bool slant = false;
    };

    Pick pickFace (const std::vector<Face>& faces, bool bold, bool italic);

    /*  ONE FACE OF A COLLECTION AS A FILE OF ITS OWN: its table directory
        rewritten and its tables copied after it. JUCE loads a font's first
        face only, so a `.ttc`'s second face is drawn from this. A single
        face's file comes back whole for index nought; empty when the face is
        not there or does not read. */
    std::vector<std::uint8_t> extractFace (const std::uint8_t* bytes, std::size_t size, int index);
}
