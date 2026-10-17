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
    JSON, written - small, and for one job (namespace draft §76, ARY): the
    `GODOT` header a saved preset gains, and the whitespace a pretty-printed
    description sheds on its way into your presets folder.

    COMPACT, because the shipped presets are (§57: generated presets are
    compact), and numbers through osc::formatDouble - the shortest text that
    reads back as the same double, in no locale - so a VERSION of 2 is written
    `2`, never `2.0` or `2,0`. A non-finite number, which JSON cannot carry, is
    written `null`.

    NOT A REWRITER OF DOCUMENTS. `json::Object` is sorted by key, so a whole
    description read and written back would come out reordered, every number
    retyped; what a saved preset changes is its header alone, written here and
    put into the file's own text in place of the old one.
*/

#include <wfg/engine/json/JsonValue.h>

#include <string>
#include <string_view>

namespace wfg::json
{
    /** A value as compact JSON: no space anywhere outside a string. */
    std::string write (const Value& value);

    /** A string as a JSON string, quotes included, escaped as RFC 8259 asks. */
    std::string quote (std::string_view text);

    /*  THE TEXT WITHOUT ITS WHITESPACE outside strings: what a pretty-printed
        description becomes when it is kept as a preset. Nothing is parsed, so
        nothing is reordered or retyped; a string's own spaces stay. */
    std::string compact (std::string_view text);
}
