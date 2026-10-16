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
    `wfg decklink-probe` - THE TESTERS' PROBE (namespace draft §71, APP).

    Nobody who builds Go.dot has a DeckLink card; the people who test it do.
    The probe is what they run and send back: every card Desktop Video finds,
    what it can do, and the formats it sends and takes in, written so that a
    reader who has never seen Go.dot can check it against the card's box.

        wfg decklink-probe          lists the cards
        WFG_DECKLINK_FAKE=1 wfg decklink-probe
                                    the same, of the test card

    Exit 0 when Desktop Video answered (with cards or none), 2 when it is not
    installed or too old.
*/

#include <wfg/engine/video/render/Card.h>

#include <ostream>
#include <string>
#include <vector>

namespace wfg::video
{
    /*  The probe over `cards`, written to `out`. `args` are the words after
        the verb. */
    int runDeckLinkProbe (card::System& cards, const std::vector<std::string>& args, std::ostream& out);

    inline constexpr const char* deckLinkProbeVerb = "decklink-probe";
}
