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
    `wfg video-probe` - THE TESTERS' PROBE (namespace draft §71 APP, §72 APV).

    Nobody who builds Go.dot has a DeckLink card; the people who test it do.
    The probe is what they run and send back: every card Desktop Video finds,
    what it can do, and the formats it sends and takes in, written so that a
    reader who has never seen Go.dot can check it against the card's box.

        wfg video-probe             lists the cards and the cameras
        wfg video-probe --run [--card=<id or name>] [--mode=1080p50]
                         [--seconds=60] [--key] [--sound] [--ten-bit]
                         [--report=<file>]
                                    and plays to the first card that sends
                                    pictures, or the one named: colour bars,
                                    a block stepping along the bottom, a
                                    white frame and - with --sound - a beep
                                    on channels 1 and 2 once a second, both on
                                    the same moment of the card's stream; then
                                    says what came of the frames, how much
                                    the card held, its clock against the
                                    computer's and its reference
        WFG_CARD_FAKE=1 wfg video-probe ...
                                    the same, of the test card

    It opens no sound interface and no show: a card on its own. --report
    writes what was said to a file, to send back.

    Exit 0 when Desktop Video answered (with cards or none) and a run ran, 2
    when Desktop Video is not installed or too old or there is no card to run,
    3 when the card would not open.
*/

#include <wfg/engine/video/render/Card.h>

#include <ostream>
#include <string>
#include <vector>

namespace wfg::video
{
    /*  The probe over `cards`, written to `out`. `args` are the words after
        the verb. */
    int runVideoProbe (card::System& cards, const std::vector<std::string>& args, std::ostream& out);

    inline constexpr const char* videoProbeVerb = "video-probe";
}
