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
    WHAT A FADE MOVES, as its mixer reads it (namespace draft §26, PG; the
    author, 2026-10-03: "What is already represented as a slider should be a
    slider in the foot panel when editing a fade [...] Changing a parameter
    highlights the parameter to show the fade will affect it").

    ONE STRIP PER THING A SLIDER ALREADY STANDS FOR: the target's level (the
    DCA's trim, for a fade on a DCA), its speed when it plays a file, and a
    send into every mix the show declares - Sends.h's desk, for its reason.
    Each strip is MOVED or not: moved, its number is where the fade takes it;
    not, its number is what the target holds now, which is where a first move
    of the strip starts from.

    AND THE REST BY NAME: the EQ numbers and plugin values the fade moves,
    each a row with its tick box, since their editors are the EQ panel and the
    plugin's own window; and the target's inserts, each a door to that window.

    std only, one snapshot door as every model file.
*/

#include <string>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    /** One strip of the fade's mixer. */
    struct FadeStrip
    {
        /** level, dca, speed or send. */
        std::string kind;

        std::string name;
        std::string under;      ///< the word under the name: the bus's width and channels, "the cue's level"

        /*  WHERE A HAND WRITES IT: the fade's own row for the level and the
            speed, the door for a send (`/godot/cue/<fade>/moves/send/<bus>`).
            The tick box writes `switchAddress`: `levelOn`, `rateOn`, or the
            door again, with an empty text to take the entry out. */
        std::string address;
        std::string switchAddress;
        std::string busId;

        bool moved = false;
        double value = 0.0;     ///< where it goes when moved, where it stands when not
    };

    /** One EQ number or plugin value the fade moves, by name. */
    struct FadeMoveRow
    {
        std::string entry;      ///< `eq/<row>` or `fx/<plugin>/<n>`
        std::string label;      ///< "Band 2 gain", "Reverb - Mix"
        std::string valueText;  ///< "-6.0 dB", "0.40"
        std::string address;    ///< the door, for its tick box to empty
    };

    /** One insert the target has, as a door to its plugin's own window. */
    struct FadeInsert
    {
        std::string pluginId;
        std::string name;
        bool enabled = true;
    };

    struct FadeMixReading
    {
        bool present = false;
        std::string notice;

        std::string fadeId, targetId, targetName, targetKind;
        std::string dcaId;

        std::vector<FadeStrip> strips;
        std::vector<FadeMoveRow> moves;
        std::vector<FadeInsert> inserts;

        /** Whether the target has an EQ to open - a media or a mic cue. */
        bool hasEq = false;
    };

    FadeMixReading readFadeMix (const tree::TreeSnapshot&, const std::string& fadeId);

    /*  THE ADDRESS OF ONE ENTRY'S DOOR: `/godot/cue/<fade>/moves/<entry>` -
        a number sets it, an empty text takes it out (PF). */
    std::string fadeMoveAddress (const std::string& fadeId, const std::string& entry);

    /*  The speed's throw: nought to twenty, an octave the same height
        everywhere above the lowest step and nought at the bottom - varispeed's
        own grid, so one is in the middle. */
    double fractionForSpeed (double speed);
    double speedForFraction (double fraction);
}
