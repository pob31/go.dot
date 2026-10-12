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
    WHICH SAMPLE A FRAME IS SHOWN AT (Phase 8a, namespace draft 35.4; PRD
    §3.19d: "video frames are presented against the audio position").

    The renderer never asks the system what time it is for the show: it is
    told, fifty times a second, where Go.dot's samples were and when they were
    read, and from those pairs it keeps an estimate - a line, sample against
    the machine's steady nanoseconds - that it can read at the moment a frame
    will reach the screen.

    THE LINE ITSELF LIVES WITH THE CLOCKS NOW (namespace draft §59, TC.5):
    MIDI Time Code reads it the other way round - the moment a sample plays -
    and RD.9's map between two machines builds on it, so it is
    `wfg::ClockEstimate` in clock/ClockEstimate.h. The name the video side has
    always used stays, so the renderer and its tests read as they did.
*/

#include <wfg/engine/clock/ClockEstimate.h>

namespace wfg::video
{
    using ClockEstimate = wfg::ClockEstimate;
}
