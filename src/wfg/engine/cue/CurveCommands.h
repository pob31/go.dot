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
    RECORDING AN OSC CUE'S CURVES (namespace draft 45, O.9): the commands that
    arm a cue and its curves, start a pass, ride a curve by hand, and end it.

        curve.arm <cue>                 an OSC cue armed for recording; empty frees it
        curve.free                      the same as an empty arm
        curve.rec <curve> <on>          one curve of the armed cue armed or not
        curve.record [from] [run]       a pass: the cue played from a second of its clock
        curve.stop [how ...]            asked by a hand, or said by the Runner when written
        curve.ride <curve> <value>...   a hand's value for curves (the SpaceMouse, O.11)

    What a pass ends in is written with `node.setMany` on the curves' points,
    one step of undo, by the Runner's hook - then `curve.stop kept`.
*/

#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/osc/OscValue.h>

#include <array>
#include <vector>

namespace wfg { class Engine; }
namespace wfg::doc { class ShowDocument; }
namespace wfg::surface { class SpaceMouse; }
namespace wfg::tree { class MountTable; }

namespace wfg::cue
{
    class CurveTable;
    class Runner;

    void registerCurveCommands (CommandRegistry& registry, Engine& engine, Runner& runner,
                                doc::ShowDocument& document, CurveTable& curves,
                                surface::SpaceMouse* puck = nullptr);

    /*  WHETHER THE PUCK IS WANTED (O.11, ZF): a curve with a movement armed. */
    bool puckWanted (const doc::ShowDocument& document, const CurveTable& curves);

    /*  WHAT THE PUCK MOVES THIS TICK (O.11), as `curve.ride`'s pairs: each armed
        curve with a movement whose axis is pushed past the dead zone, stepped
        from what it rides by its push at its `speed` over `seconds`, held to
        its range - its own, else the device's for that value. Empty during no
        pass, and for a puck nobody pushes: letting go writes nothing, and the
        latch holds the last value. */
    std::vector<osc::Value> puckRides (const doc::ShowDocument& document, const tree::MountTable* mounts,
                                       const CurveTable& curves, const std::array<double, 6>& axes,
                                       double seconds);
}
