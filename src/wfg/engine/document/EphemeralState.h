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
    `state.xml`: what the machine happened to be doing.

    PRD §4.10 draws the line and this file is the other side of it. show.xml
    holds what someone DECIDED - a cue's name, its number, how long it waits.
    state.xml holds where the engine had got to when it was last saved, and the
    first thing in it is each list's standby: the cue GO will act on.

    WHICH ATTRIBUTES, decided by the parameter table and not here. A row's
    `persist` column says `show`, `state` or `none`, and this writes exactly the
    `state` ones. Adding a piece of ephemeral state is a CSV edit; no code here
    changes.

    THE SHAPE IS FLAT, one entry per object that has something to remember,
    found by identifier:

        <State formatVersion="1">
          <List id="7K2QM9X4" standby="B3N8R5TW"/>
        </State>

    Not a mirror of show.xml's nesting, which would have to carry every
    container and every object that had nothing to say, and would then have to
    be kept in step with a structure it does not own.

    LOSING IT IS NOT AN ERROR. A bundle with no state.xml opens with every
    ephemeral value at its default - no standby set - because that is what
    "ephemeral" has to mean for a file someone may reasonably delete, or never
    have had, or have excluded from version control. An entry naming an object
    the show no longer contains is reported and skipped, for the same reason:
    the show is what matters, and a stale pointer into it must not cost anyone
    their document.

    STANDBY SURVIVES A SAVE AND LOAD, which is the plan's open question A
    answered the way it recommends. A rehearsal reopened where it was left is
    the kinder default, and reversing it is one column of one CSV row.
*/

#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/ShowDocument.h>

#include <string>
#include <string_view>

namespace wfg::doc
{
    namespace EphemeralState
    {
        /** The document's ephemeral state as canonical XML, ending in a
            newline. Same rules as show.xml: LF, two-space indent, attributes
            sorted, defaults omitted. */
        std::string write (const ShowDocument& document);

        /*  Applies `text` to a document that has already been loaded from
            show.xml.

            Unlike the show reader this one is FORGIVING BY DESIGN, and the
            asymmetry is the point: a broken show.xml means someone's work is at
            risk and the right answer is to refuse and say why, while a broken
            state.xml costs at most a standby position. Problems are collected
            and reported; whatever else parsed is still applied. */
        ReadResult read (std::string_view text, ShowDocument& document);
    }
}
