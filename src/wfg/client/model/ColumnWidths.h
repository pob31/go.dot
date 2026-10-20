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
    COLUMN WIDTHS A HAND DRAGGED, KEPT ON THIS COMPUTER (namespace draft §98).
    The author, 2026-10-20: "Screen resolution might change from one computer
    to the next so this can be local and not saved with the show." So they live
    in Go.dot's own folder, beside `last-show.txt`, and never in the document
    (§4.10): a width is how one screen looks at the show, not what it is.

    A width is kept in the units the type size scales (`Theme::type`), so a
    column dragged and then a bigger type keeps its words in it. One line a
    column, its name and its width: `osc.address 260`. A line that does not
    read is passed over, never fatal - the file is this machine's and nobody's
    show depends on it.
*/

#include <map>
#include <optional>
#include <string>

namespace wfg::client::model
{
    class ColumnWidths
    {
    public:
        /*  The narrowest and widest any column is kept at, whatever the file says. */
        static constexpr int least = 16, most = 2000;

        /** The width `column` was last dragged to, or nothing: then its own. */
        std::optional<int> of (const std::string& column) const;

        /** Keeps a width for `column`; nought or less forgets it, so it is its own again. */
        void keep (const std::string& column, int width);

        /** The file's text, one column a line, in name order. */
        std::string written() const;

        /** What a file says, every line that reads. */
        static ColumnWidths read (const std::string& text);

    private:
        std::map<std::string, int> widths;
    };
}
