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
    THE WALK (namespace draft §46.2, ZS-ZY): a QLab workspace's cues decided,
    one by one, in Go.dot's terms - a group's mode, a sound's file and levels
    and ranges and where it goes, a fade, a message and its curve, a device -
    and written nowhere: `QlabImport` writes the plan into a show, so what the
    walk decided can be tested without one.

    WHAT IT DECIDES:

    - A GROUP'S MODE IS THE DECISION (ZS). Start first and enter is a manual
      sequence; a playlist an automatic one; a timeline a timeline; start
      random a shuffled sequence of one. Start first with several members is a
      MANUAL sequence: QLab fires the first and moves past the group, so the
      rest run only when somebody fires them, and an automatic one would fire
      them unasked. A continue mode QLab derives is never read; one somebody
      SET, between siblings, makes a group of the chain it starts.
    - A SOUND (ZT, ZU): its file, its main level, its rate, its ranges - the
      start and end, the slices and their loops - and a route to each mono bus
      its matrix reaches, a bus per QLab cue output in use.
    - A FADE (ZV): to a level, with QLab's shape as Go.dot's nearest; relative
      on a group, that group's trim summed in show order.
    - A MESSAGE (ZW): an OSC cue under the device its patch names, its
      arguments typed atoms, a `#v#` fade a curve on that argument.
    - EVERYTHING ELSE (ZX, ZP): Start, Stop, Goto, Arm, Disarm and Devamp as
      their Go.dot verbs; any kind with no equivalent a memo in its place,
      named `[QLab] ...`, the original in its notes - one GO stays one GO.

    Nothing is approximated silently: every choice that changes what plays is a
    `Note` for the report (ZZ).
*/

#include <wfg/engine/import/ImportCommon.h>
#include <wfg/engine/import/QlabReader.h>

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace wfg::import::qlab
{
    using Attributes = std::vector<std::pair<std::string, std::string>>;

    /*  A MONO DIRECT BUS for one QLab cue output (ZU): `key` "out:<patch>:<n>",
        named as QLab named the output, on the interface channel `channel`
        (from nought) - QLab's cue output n is taken to be the interface's
        output n, which a patch routed otherwise would change, and says so. */
    struct Bus
    {
        std::string key;
        std::string name;
        int channel = -1;           ///< -1: no interface channel
    };

    /*  A DEVICE for one QLab network patch (ZW): an opaque mount, its prefix the
        first segments of the addresses its cues send. */
    struct Device
    {
        std::string key;            ///< "patch:<id>"
        std::string name;
        std::string host;
        int port = 0;
        bool tcp = false;
        std::string prefix;         ///< space-separated, as `mount/prefix` takes several
    };

    struct Route
    {
        std::string bus;            ///< a `Bus::key`
        std::vector<double> gains;  ///< one per input channel of the cue, plain multipliers
    };

    struct Range
    {
        double in = 0.0;
        double out = 0.0;
        int loops = 1;              ///< nought for ever
    };

    struct Curve
    {
        int arg = 0;
        std::vector<std::pair<double, double>> points;     ///< seconds and the value itself
    };

    /*  ONE CUE OF THE PLAN. `kind` is what `ShowDocument::createCue` takes:
        "group", "media", "mic", "fade", "osc", "start", "transport", "memo".
        `attributes` are its rows as the document spells them; `target` names
        another item's key, made an identifier when the show is written. */
    struct Item
    {
        std::string kind;
        std::string key;
        std::string name;
        Attributes attributes;
        std::string target;

        //  A media cue.
        std::string cueId;          ///< QLab's UUID, by which the import finds its placed file
        FileRef file;
        std::vector<Range> ranges;
        std::vector<Route> routes;  ///< per input channel; the import trims them to the file's channels
        bool defaultRoute = false;  ///< QLab left the matrix as it was: Go.dot's default route

        //  An OSC cue's curves.
        std::vector<Curve> curves;

        std::vector<Item> children;
    };

    struct List
    {
        std::string key;
        std::string name;
        std::vector<Item> items;
    };

    struct Plan
    {
        std::vector<List> lists;
        std::vector<Bus> buses;
        std::vector<Device> devices;
        std::vector<Note> notes;

        int cues = 0;               ///< QLab cues walked, lists not counted
        int placeholders = 0;       ///< memos written in place of a kind with no equivalent (ZP)
    };

    struct WalkOptions
    {
        /*  The lists to import, by index in the workspace; empty for all. */
        std::set<int> lists;

        /*  EACH SOUND'S CHANNELS, by QLab cue UUID, where the file was found -
            which decides how many rows of its matrix are real: QLab keeps a
            row for every input it might have. A sound missing here gets no
            route. */
        std::map<std::string, int> channels;
    };

    Plan walk (const Workspace&, const WalkOptions& = {});

    /*  QLAB'S TEXT FOR A MESSAGE, split as QLab splits it: the address, then
        each argument after a space, a double-quoted run kept whole. */
    std::vector<std::string> splitMessage (const std::string& text);

    /*  ONE ARGUMENT AS GO.DOT SPELLS IT (§46.2, ZW): `\T` `\F` `\I` `\N`, digits
        an integer, digits and a point a float, anything else a string. */
    std::string atomFor (const std::string& argument);

    /** A linear gain in dB, at or below `floorDb` silence (-120). */
    double decibels (double gain, double floorDb);
}
