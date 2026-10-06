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
    RECORDING A CUE'S LANES FROM THE FADERS, as named commands (namespace draft
    §34, which replaced §20.9's one fader taken by touch).

      lane.arm <cue>        every fader surface flips to the cue; empty flips back
      lane.free             the faders flip back to what they rode
      lane.rec <lane> <on>  a strip's REC: arms or disarms one lane, by its key
      lane.record [from]    a pass: the cue plays, from `from` when given
      lane.write <cue> ...  the Runner's: every lane a pass rode, in one step
      lane.stop [how]       the hand's, no argument: end the pass; the Runner's,
                            `kept`, `untouched`, `locked` or `dropped`, once the
                            lanes are written or not

    Every gesture reachable action is one of these (PRD §4.11), from the
    window, a surface or the network alike, and each is logged - so a replay
    reproduces the flip, the REC choices and the pass, and the lanes a pass
    ends in arrive as the Runner's own `lane.write`, which a replay applies as
    it applied the night's. The table they move is `LaneTable`; the pass
    itself - each hand sampled against the file's clock - is the Runner's
    hook, on the tick thread, where the sample clock is.
*/

#include <wfg/engine/command/CommandRegistry.h>

#include <string>
#include <vector>

namespace wfg { class Engine; }
namespace wfg::doc { class ShowDocument; }

namespace wfg::cue
{
    class LaneTable;
    class Runner;

    /*  THE LANES ON THE FADERS, in strip order (UN): the cue's level, then the
        show's mixes as `/godot/audio/mixes` lists them - by first channel, then
        identifier - which is the order the rotaries' Send page walks too. The
        same for every cue: a mix the cue does not send to is a lane it can be
        given (UQ). */
    std::vector<std::string> flippedLanes (const doc::ShowDocument& document);

    void registerLaneCommands (CommandRegistry& registry, Engine& engine, Runner& runner,
                               doc::ShowDocument& document, LaneTable& lanes);
}
