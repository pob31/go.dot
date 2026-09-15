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

/*  WHAT THE READER IS LOOKING AT, which is the page's and never the
    engine's (§14.1): the groups folded shut, and the inspected cue - NOT the
    standby (§3.5). */

const folded = new Set();          // group ids the reader has closed

/*  In an object rather than a variable of its own, because it is written
    from more than one module - a click picks, a new show clears - and an
    imported binding is one no importer may assign to. */
const selection = { picked: null };

export { folded, selection };
