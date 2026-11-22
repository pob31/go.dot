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
    A sound's own dynamics stage, as the window reads it and draws it
    (namespace draft §141, SC.8): the eighteen dyn* rows read back into the one
    value the engine uses - the same `audio::DynSettings` the voice is given -
    its key and what it may be, and what the stage took off a sounding run.

    std only, one snapshot door as every model file.
*/

#include <wfg/client/model/Fx.h>
#include <wfg/engine/audio/DynSettings.h>

#include <string>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    struct DynReading
    {
        /** Whether the cue has the stage at all: an audio cue, a mic cue or an aux. */
        bool present = false;

        audio::DynSettings settings;

        /*  WHAT IT LISTENS TO (§141, BHL, BHU): the word the row holds, and the
            menu's choices - "this cue, before its level" (the empty word), "this
            cue, after its level", each input and each aux but itself. */
        std::string key;
        std::vector<KeyChoice> keyChoices;

        /*  Where its rows are, with the slash: a cue's /godot/cue/<id>/ or an
            aux's /godot/slot/<id>/ (§93). */
        std::string base;

        /*  THE REDUCTION of the run sounding it, in dB (`run/reduction`), and
            whether one is: a meter that reads nothing while nothing plays. */
        bool running = false;
        double reductionDb = 0.0;

        /** Why there is nothing to draw, when there is nothing to draw. */
        std::string notice;
    };

    /** The stage of a cue or an aux, by its rows. */
    DynReading readDyn (const tree::TreeSnapshot&, const std::string& cueId);

    /** The words the menu says for a key: its label, or the word "not in this show" when it is gone. */
    std::string dynKeyWords (const DynReading&, const std::string& key);

    /** The row of a number of the stage, by what the panel calls it: "dynThreshold" and so on. */
    std::string dynRowAt (const DynReading&, const std::string& row);
}
