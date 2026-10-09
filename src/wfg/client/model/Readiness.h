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
    HOW READY EACH ROW'S CUE IS FOR GO (PRD §3.12; namespace draft §48, AAM,
    AAS): the mark a cue list row carries in a cell of its own - getting ready,
    ready, partly ready, or missing - for every cue the engine gets ready ahead,
    a sound's and a picture's alike, read from `cue/prepare` and
    `cue/prepareError`.

    A SHAPE, its meaning in words for a tooltip, and the word "missing" written
    out where something is missing: colour is never the only telling (PRD §4.8).

    Kept apart from the row because it moves at tick rate while the rows are
    built again only when the show changes. No JUCE: the snapshot's words in,
    marks out.
*/

#include <wfg/client/model/Icons.h>
#include <wfg/client/model/ShowModel.h>

#include <map>
#include <string>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    /*  ONE CUE'S MARK, from its two words and its kind ("media", "video",
        "mic", "group"): none for idle and for a kind nothing is got ready for. */
    Mark readinessMark (const std::string& prepare, const std::string& error, const std::string& kind);

    /*  EVERY ROW'S THAT HAS ONE, by cue identifier: the cue rows of a kind the
        engine gets ready - a sound, a mic, a picture or a movie, a group. */
    std::map<std::string, Mark> readinessOf (const tree::TreeSnapshot& snapshot, const std::vector<Row>& rows);
}
