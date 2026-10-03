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
    WHAT A FADE MOVES BESIDE THE LEVEL AND THE SPEED (namespace draft §26,
    2026-10-03): its target's sends, EQ numbers and plugin values, as three
    lists on the fade - `fade/sends` (`bus:dB`), `fade/eq` (`row:value`) and
    `fade/fx` (`plugin/index:value`, normalised). Being in a list is being
    moved (OY); the tick box in the fade's mixer is presence.

    ONE NAME FOR AN ENTRY EVERYWHERE: `send/<bus>`, `eq/<row>`,
    `fx/<plugin>/<index>` - what the run holds a moved value under, what a
    takeover key ends with, and the tail of the door a hand writes one entry
    through, `/godot/cue/<fade>/moves/<entry>` (PF).

    std only, so the desktop client's model reads the lists with the engine's
    own parser.
*/

#include <map>
#include <string>
#include <vector>

namespace wfg::cue
{
    /*  HOW A VALUE TRAVELS (PA): straight in dB for a send and a band's gain,
        straight in the logarithm for a frequency and a Q, straight in the
        number for a plugin's 0..1. */
    enum class MoveDomain { decibels, logarithm, linear };

    struct FadeMove
    {
        std::string entry;      ///< `send/<bus>`, `eq/<row>`, `fx/<plugin>/<index>`
        double value = 0.0;
        MoveDomain domain = MoveDomain::linear;
    };

    /*  A list as the row spells it, `key:value` pairs: the key is everything
        before the LAST colon, so a plugin's `entry/index` reads whole. A pair
        that does not read is skipped, as `parseFxValues` skips one. */
    std::map<std::string, double> parseMoveList (const std::string& text);

    /** The row's spelling, sorted, each value through the canonical formatter. */
    std::string formatMoveList (const std::map<std::string, double>& values);

    /*  The EQ rows a fade may move: the numbers, never a switch or a shape
        (PB) - `eqHpfFreq`, `eqLpfFreq`, `eqB<n>Freq`, `eqB<n>Gain`, `eqB<n>Q`. */
    bool isMovableEqRow (const std::string& row);

    /** The range an EQ row takes, for the door's refusal and the job's clamp. */
    void eqRowRange (const std::string& row, double& lowest, double& highest);

    /** The domain of an EQ row: a gain in dB, the rest logarithmic. */
    MoveDomain eqRowDomain (const std::string& row);

    /** Every entry of the three lists, in a stable order: sends, EQ, plugins. */
    std::vector<FadeMove> readFadeMoves (const std::string& sends, const std::string& eq,
                                         const std::string& fx);

    /*  An entry split into what it names: `kind` is send, eq or fx; `name`
        the bus, the row or the plugin entry; `index` the parameter, for fx.
        False for anything else. */
    bool splitMoveEntry (const std::string& entry, std::string& kind, std::string& name, int& index);

    /** The fade attribute an entry kind lives in: sends, eq, fx. */
    const char* moveListAttribute (const std::string& kind);

    /** The value along a move: `progress` 0..1, shaped by `sCurve` when asked. */
    double moveValueAt (double from, double to, double progress, bool sCurve, MoveDomain);
}
