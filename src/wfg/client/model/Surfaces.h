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
    The control surfaces this show declares, the strips on them, and the DCAs
    those strips can ride (PRD §3.16, §3.27, §3.28; namespace draft §16.7).

    THREE OBJECTS, AND ONE OF THEM IS NEVER STORED. A surface is a box the show
    talks to through declared MIDI ports - "the D700 on P1 and P2, expecting
    the Mackie preset" - and a strip is one fader or pad on it. Both are the
    show's, and travel in the file. A DCA is a named trim that cues are marked
    with; its name is the show's, and its TRIM is what somebody's fader is
    doing tonight, which the tree publishes and the file never holds (§4.10).

    WHAT A STRIP IS DOING IS THE ENGINE'S ANSWER, READ AND NEVER WORKED OUT. The
    node its fader rides now, the word its display shows, the cue on it: all of
    it is published at /godot/slot/<id> from the run table, and a client that
    decided for itself which run held a strip would be a second answer to a
    question with one. What this file adds is what a column has to DRAW - the
    cue's name and colour, the DCA's short name, the value under the fader -
    looked up once per pass so that a panel reads one row per strip.

    BOTH THE SURFACES TAB AND THE VIRTUAL PANEL READ THIS, which is why a strip's
    row carries everything a column shows: two views that looked a strip up
    their own ways would come to disagree about it.

    std only, and a pure reading of the snapshot the window already holds.
*/

#include <wfg/engine/tree/TreeSnapshot.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace wfg::client::model
{
    /** One declared control surface, as the tree publishes it at /godot/surface/<id>. */
    struct SurfaceRow
    {
        std::string id;
        std::string name;
        std::string profile;              // virtual | mcu | d700 | midiPads
        std::vector<std::string> ports;   // port ids, bank order (split on spaces)
        std::string preset;
        bool enabled = true;
        int strips = 0;
        bool connected = false;
        std::string problem;
        std::string serial;
        int channel = 0;                  // midiPads
        int firstNote = 36;               // midiPads

        /** The name, or the profile's own words when it has none ("Mackie Control", "Asparion D700", "Pads", "Virtual panel"). */
        std::string label() const;

        /** Its state in words, never colour alone: "connected", "off" when not enabled, the engine's `problem` sentence when there is one, "not connected" otherwise. A virtual surface is always "connected". */
        std::string stateWord() const;
    };

    /** One strip, as /godot/slot/<id> publishes it (kind = strip), with what is on it. */
    struct StripRow
    {
        std::string id;
        std::string surface;
        int index = 0;
        std::string role;        // sampler | dca
        std::string dca;         // the DCA a dca strip rides
        std::string endpoint;    // absolute (a fader) | gate (a pad)
        std::string target;      // the node its fader rides now, or empty
        std::string word;        // free | dca | unassigned | armed | pending | playing | held | stopping | closing
        std::string cue;         // the member cue on it, or empty
        std::string holder;      // the run holding it, or empty
        std::string cueName;     // /godot/cue/<cue>/name
        std::string cueShortName;
        std::string cueNumber;
        std::string cueColour;   // #RRGGBB or empty
        std::string timbre;      // the holder run's timbre ("h s l"), or empty
        std::string dcaName;     // for a dca strip: the DCA's shortName, else its name
        bool hasLevel = false;   // whether `target` names a node with a value
        double levelDb = -120.0; // that value
        bool held = false;       // the holder run's `held`

        /** What a strip label shows: the cue's shortName, else its name; a dca strip's DCA name; "—" (an em dash) for a free one. Not cut: the caller fits it. */
        std::string label() const;
    };

    struct DcaRow
    {
        std::string id;
        std::string name;
        std::string shortName;
        std::string parent;      // the DCA it sits inside
        double trimDb = 0.0;

        std::string label() const;   // shortName, else name, else the id
    };

    /** Every declared surface, in /godot/surface/order. */
    std::vector<SurfaceRow> readSurfaces (const tree::TreeSnapshot&);

    /** Every strip of every surface: surface order, then index. */
    std::vector<StripRow> readStrips (const tree::TreeSnapshot&);

    /** The strips of one surface, from a readStrips result, in index order. */
    std::vector<StripRow> stripsOf (const std::vector<StripRow>& strips, const std::string& surfaceId);

    /** Every declared DCA, in /godot/dca/order. */
    std::vector<DcaRow> readDcas (const tree::TreeSnapshot&);

    /** Menu choices for a DCA reference, {id, label}, "(none)" first with an empty id.
        The label is the DCA's whole name, which a menu has room for, and
        `DcaRow::label()` only when it has none. */
    std::vector<std::pair<std::string, std::string>> dcaChoices (const std::vector<DcaRow>&);

    /** The four profiles, {word, label}: virtual "Virtual panel", mcu "Mackie Control", d700 "Asparion D700", midiPads "Pads". */
    std::vector<std::pair<std::string, std::string>> profileChoices();
}
