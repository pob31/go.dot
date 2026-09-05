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
    A snapshot as an OSCQuery reply.

    OURS RATHER THAN juce::JSON, and the reason is the one that keeps coming
    up: JUCE serialises a double through its own number writer, which loses 46%
    of them to a round trip (measured; the table is in osc/OscValue.cpp). A
    client that read a range bound or a rate cap out of this reply would get a
    different number from the one the engine holds. Everything Go.dot writes -
    the document, the log, the grammar and now this - goes through one
    formatter, which writes the shortest text that reads back identically.

    IT IS ALSO DETERMINISTIC, which juce::JSON is not required to be: keys come
    out in a fixed order, and the children of a container are sorted by name. A
    reply that reordered itself between two identical states could not be
    compared against a committed golden, and the golden is the only thing that
    can catch the engine and its own schema being wrong in the same direction.

    WHAT A CLIENT GETS. `FULL_PATH`, `TYPE`, `ACCESS`, `VALUE`, `RANGE`, `UNIT`
    and `DESCRIPTION` are the OSCQuery proposal's; `GODOT` is the one vendor key
    Go.dot adds, carrying the four things PRD §3.3 says a node declares. The
    proposal makes custom attributes "intentionally trivial" - a client that
    does not know the key ignores it and loses nothing it understood anyway.

    An EVENT node has no `VALUE`, deliberately and not by omission: an event is
    one-shot and has no value at a given time, so a zero or a null would be an
    answer to a question that has none.
*/

#include <wfg/engine/tree/TreeSnapshot.h>

#include <string>
#include <string_view>

namespace wfg::tree
{
    namespace OscQueryJson
    {
        /*  The subtree at `address` and everything under it, as an OSCQuery
            reply ending in a newline.

            Empty when the address names nothing. `"/"` gives the whole tree,
            which is what `GET /` asks for. */
        std::string describe (const TreeSnapshot& snapshot, std::string_view address);
    }
}
