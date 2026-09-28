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
    RECORDING A LANE FROM A FADER, as named commands (namespace draft §20.9).

      lane.arm <cue>      a media cue's lane waits for a fader; empty frees it
      lane.take <strip>   the first fader touched while it waits is taken
      lane.free           the fader goes back to what it rode
      lane.record [from]  a pass: the cue plays, from `from` when given
      lane.stop [how]     the hand's, no argument: end the pass; the Runner's,
                          `kept` or `dropped`, once the lane is written or not

    Every gesture reachable action is one of these (PRD §4.11), from the
    window, a surface or the network alike, and each is logged - so a replay
    reproduces the pick and the pass, and the lane a pass ends in arrives as
    the Runner's own `node.set`, which a replay applies as it applied the
    night's. The table they move is `LaneTable`; the pass itself - the hand
    sampled against the file's clock - is the Runner's hook, on the tick
    thread, where the sample clock is.
*/

#include <wfg/engine/command/CommandRegistry.h>

namespace wfg { class Engine; }
namespace wfg::doc { class ShowDocument; }

namespace wfg::cue
{
    class LaneTable;
    class Runner;

    void registerLaneCommands (CommandRegistry& registry, Engine& engine, Runner& runner,
                               doc::ShowDocument& document, LaneTable& lanes);
}
