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
    AN OSC CUE'S MESSAGES AS A GRID (the author, 2026-10-17: "Could clicking
    with the mouse cursor show the same grid as the scope of WFS-DIY with the
    columns relevant to the cue and the values in the cells where something
    was recorded? This would allow the user to see the data and eventually
    edit it. Don't add columns."; namespace draft §80).

    A snapshot a processor writes into the show is one cue of many messages -
    `/wfs/input/3/positionX`, `/wfs/input/3/positionY`, `/wfs/input/5/...` - and
    a list of a hundred addresses is not how anybody reads it. Its own window
    draws it as WFS-DIY's scope does: a row per parameter, a column per
    channel.

    HOW AN ADDRESS IS READ: its first segment that is a number is the channel;
    what comes before it is the family the grid is of (`/wfs/input`), what comes
    after it the parameter (`positionX`, or `eq/2/gain`). A family a table, its
    columns the channels this cue holds and no others, in their order as
    numbers; its rows the parameters in the order the cue first holds them,
    which is the order the processor sent them. A message with no number in its
    address is no cell of any grid, and is counted.

    std only, and pure.
*/

#include <wfg/client/model/OscMessages.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace wfg::client::model
{
    /*  One family's table. `cells[row][column]` is the index of the message in
        the reading's rows that the cell shows, or -1 where the cue holds none. */
    struct OscGridTable
    {
        std::string family;
        std::vector<std::string> columns;
        std::vector<std::string> parameters;
        std::vector<std::vector<int>> cells;
    };

    struct OscGrid
    {
        std::vector<OscGridTable> tables;

        /** The messages no grid holds: no number in the address. */
        std::size_t outside = 0;

        bool empty() const noexcept { return tables.empty(); }
    };

    /*  Where an address sits, or nothing when it has no channel: the family,
        the channel and the parameter, as the header above reads them. */
    struct OscGridPlace
    {
        std::string family, column, parameter;
    };

    std::optional<OscGridPlace> gridPlaceOf (const std::string& address);

    OscGrid gridOf (const OscMessagesReading& reading);

    /** What a cell says: its message's values, their words, a space between. */
    std::string cellText (const OscMessageRow& row);

    /*  WHAT A CELL TYPED IN WRITES: the message's `value` row with each value
        as typed, kept at its type - "0.5" a float stays a float. One word a
        value, unless the message holds a single value, which takes the whole
        text (words with spaces in them). nullopt when the text cannot be the
        values' types, or does not have as many words as the message values. */
    std::optional<std::string> cellEdit (const OscMessageRow& row, const std::string& typed);
}
