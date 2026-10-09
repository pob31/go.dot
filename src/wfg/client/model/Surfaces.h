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

#include <wfg/engine/surface/DcaKnob.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <cstddef>
#include <cstdint>
#include <optional>
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
        std::string tint {};     // a picture holder's tint (#RRGGBB, namespace draft §49), or empty
        std::string dcaName;     // for a dca strip: the DCA's shortName, else its name

        /*  FOR A DCA STRIP, WHAT IT RIDES AS ONE COLOUR (namespace draft §38,
            WR): "#RRGGBB" - its members' sound and pictures blended, as its
            rotary lights - or empty with nothing up. */
        std::string dcaColour;

        /*  FOR A DCA STRIP, WHAT THE KNOB ABOVE IT REACHES (namespace draft
            §50): the marks of what plays under its DCA now, the one shown
            first - the bridge's own rule - and whether pictures are assigned
            to it, which is where its knob starts. */
        std::vector<surface::DcaMark> dcaMarks;
        bool dcaPictures = false;

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

    /*  WHETHER THE SHOW HAS A MASTER DIAL TO PUT A NUMBER ON (2026-09-26): a
        Mackie or a D700 it declares and has not switched off. A show with
        none sends no `surface.dial` for every click in the inspector. */
    bool hasMasterDial (const tree::TreeSnapshot&);

    /** Every strip of every surface: surface order, then index. */
    std::vector<StripRow> readStrips (const tree::TreeSnapshot&);

    /*  WHAT THE LAST PASS ENDED IN (namespace draft §30.4, §34), as
        `/godot/surface/lanePass` says it: the tick it ended on - nought or
        less for no pass yet - its cue, and `kept` with the points it wrote,
        the seconds they span (`spans`) and the lanes, by name; `untouched`,
        `locked` or `dropped`. */
    struct LanePass
    {
        std::int64_t tick = -1;
        std::string cue;
        std::string how;
        int points = 0;
        double from = 0.0;
        double to = 0.0;
        bool spans = false;
        std::vector<std::string> lanes;
    };

    /*  ONE FLIPPED FADER (namespace draft §34): the lane it shows - `level`,
        or a mix's identifier - the lane's name as a person reads it, the level
        or the mix's name, whether its REC is armed, and what it rides: the
        number as it is heard. */
    struct LaneFaderReading
    {
        std::string key;
        std::string name;
        bool armed = false;
        bool hasRide = false;
        double rideDb = 0.0;
    };

    /*  THE FADERS FLIPPED TO A CUE (namespace draft §34), as the tree says:
        the cue, whether a pass runs, every lane on the faders in strip order
        - the level, then the show's mixes - and what the last pass ended in. */
    struct LaneRecordReading
    {
        std::string cue;
        bool flipped = false;
        bool recording = false;
        std::vector<LaneFaderReading> faders;
        LanePass pass;

        /** The fader of one lane, or null when the faders show no such lane. */
        const LaneFaderReading* faderOf (const std::string& key) const;

        /** The armed lanes' names, in strip order. */
        std::vector<std::string> armedNames() const;
    };

    LaneRecordReading readLaneRecord (const tree::TreeSnapshot&);

    /*  THE LANE A FLIPPED FADER RIDES, by its target (namespace draft §34):
        `level` for `/godot/surface/laneRide`, a mix's identifier for
        `/godot/bus/<mix>/laneRide`, nothing for any other node. */
    std::optional<std::string> laneKeyOfRide (const std::string& target);

    /*  THE LANE RECORDER'S NAME: on the waveform's button at rest, and at the
        head of every sentence that says what a pass did, so the button and
        what it reports are one thing by one name. "Level autom." was the
        author's (2026-10-05, QY); since the faders record the sends too
        (2026-10-06, §34) it is "Autom." - the implementer's word, which the
        author kept when asked. */
    inline constexpr const char* laneRecorderName = "Autom.";

    /*  WHAT A PASS ENDED IN, IN WORDS (namespace draft §30.4): the points it
        wrote and the seconds they span - "Level autom.: 7 points, 12.0–41.5 s",
        minutes and seconds past a minute as the ruler writes them, a full stop
        for the decimal in every locale - or that nothing was written, and why.
        Empty for no pass. */
    std::string lanePassWords (const LanePass&);

    /** The strips of one surface, from a readStrips result, in index order. */
    std::vector<StripRow> stripsOf (const std::vector<StripRow>& strips, const std::string& surfaceId);

    /** Every declared DCA, in /godot/dca/order. */
    std::vector<DcaRow> readDcas (const tree::TreeSnapshot&);

    /** Menu choices for a DCA reference, {id, label}, "(none)" first with an empty id.
        The label is the DCA's whole name, which a menu has room for, and
        `DcaRow::label()` only when it has none. */
    std::vector<std::pair<std::string, std::string>> dcaChoices (const std::vector<DcaRow>&);

    /*  THE ROLE CELL'S ONE MENU (namespace draft §30, S4). The author,
        2026-10-05: "Removing sampler faders and adding some DCA in the
        surface parameters did not show the DCA anywhere". A DCA reached a
        fader only through two cells, the Role and then the DCA, and the
        second was a dash that took no click until the first said DCA - so
        the one way to do it was hidden behind a step nobody could see.

        Now one menu says both: "Sampler", then "DCA: <name>" for every DCA
        the show declares, in the show's order. A show with no DCA has one
        greyed item instead, pointing at ADD DCA, rather than offering a DCA
        strip that would ride nothing. And what the strip is NOW is always one
        of the items (`now`): a DCA strip riding none, or naming a DCA the show
        no longer declares, is shown as what it is, at the end, so the menu
        never opens with nothing picked - which would read as a strip with no
        role at all. */
    struct RoleChoice
    {
        std::string role;        ///< sampler | dca; empty for the greyed "no DCA yet"
        std::string dca;         ///< the DCA a dca choice rides, by identifier; empty for sampler
        std::string label;
        bool enabled = true;
        bool now = false;        ///< what the strip is now
    };

    std::vector<RoleChoice> roleChoices (const StripRow& strip, const std::vector<DcaRow>& dcas);

    /** What the Role cell shows: the label of the `roleChoices` item that is `now`. */
    std::string roleWords (const StripRow& strip, const std::vector<DcaRow>& dcas);

    /*  THE WRITES ONE CHOICE MAKES, {address, value}, in the order they are
        sent: the role first, then the DCA. A DCA chosen is `role dca` and
        `dca <id>`; Sampler is `role sampler` and `dca` cleared, so a sampler
        strip carries no DCA that nothing shows. What is already so is not
        written, because each write is an undo step and a line in the log; a
        greyed item writes nothing. */
    std::vector<std::pair<std::string, std::string>> roleWrites (const StripRow& strip,
                                                                 const RoleChoice& choice);

    /*  WHICH FADERS RIDE A DCA, IN WORDS (namespace draft §30, S4): the DCA
        list's read-out, so a DCA no fader rides is seen at once rather than
        found out at the desk. "Fader 3", "Faders 3 and 7"; with more than one
        surface in the show each is named as the strip menu names it,
        "Asparion D700 · fader 5", one surface after another with "; "
        between. A pad is a pad. "no fader" when none does. Only a strip whose
        role is dca counts: the engine reads `dca` on no other. */
    std::string fadersRiding (const std::string& dcaId, const std::vector<StripRow>& strips,
                              const std::vector<SurfaceRow>& surfaces);

    /*  WHERE A STRIP DRAGGED IN THE SURFACES TAB LANDS (namespace draft §30,
        S4). `gap` is the gap between rows the hand let go in - 0 above the
        first strip, `count` below the last - and `from` the dragged strip's
        row. The answer is `object.move`'s member position, in the list AS IT
        STANDS with the dragged strip still counted (model/Reorder.h's rule,
        and `bus.move`'s): let go below itself, it takes the position of the
        strip it lands after, and above itself the position of the one it
        lands before. Nothing when letting go there leaves it where it is - in
        the gap either side of itself - or when either number is off the list. */
    std::optional<int> stripMovePosition (int from, int gap, int count);

    /*  THE STRIP MENU OF ONE SAMPLER MEMBER, {id, label} (author, 2026-09-25:
        "There should be a drop down menu to assign the strip. Like for the
        direct outs, it should state what is the previous assignment in
        chronological order of the cuelist unless it's free" - "Same for
        pads").

        "automatic" first, with an empty id, saying where automatic placement
        puts this member now. Then every sampler strip - faders and pads - in
        the order automatic placement fills them, each saying in words what is
        on it: another member of this group, what the nearest earlier sampler
        group in the list put there, or free. Never a refusal, as the
        direct-out menu never refuses: the mark informs the choice.

        What a strip carries is the engine's answer (`stripNow`,
        `stripsBefore`), never worked out here: the rule that places members
        lives where they are armed. */
    std::vector<std::pair<std::string, std::string>> stripChoices (const tree::TreeSnapshot&,
                                                                   const std::string& cueId);

    /** The four profiles, {word, label}: virtual "Virtual panel", mcu "Mackie Control", d700 "Asparion D700", midiPads "Pads". */
    std::vector<std::pair<std::string, std::string>> profileChoices();

    /*  WHAT A SURFACE'S ROTARIES ARE DOING (author, 2026-09-25): the first
        surface, in the show's order, whose EQ or Send page is up, with the
        cue every surface is aimed at and the address its page last wrote -
        empty until a hand has adjusted something. Read by address, a handful
        of lookups a pass, since the window asks at every refresh. */
    struct SurfacePage
    {
        bool up = false;          ///< an EQ or Send page is up on some surface
        std::string surface;
        std::string word;         ///< eq, send or fx
        int index = 0;
        int count = 1;
        std::string edited;       ///< what it last wrote; empty since it came up
        std::string aim;          ///< /godot/surface/aim, whether or not a page is up
    };

    SurfacePage readSurfacePage (const tree::TreeSnapshot&);
}
