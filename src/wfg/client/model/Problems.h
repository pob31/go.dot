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
    WHAT IS MISSING, SAID WHERE IT IS USED (namespace draft §64; the author,
    2026-10-15: "when a cue is missing something (screen is not connected for a
    video output, audio interface for a audio/mic cue, missing plugin, video
    insert etc...) mark the cues in the cue list", "Add warnings in the main
    window for each type of error. Clicking on one of these icons opens the
    show settings to the relevant tab", "In the Show settings, mark sections or
    tabs with problems").

    WHAT COUNTS (the author's choice, offered as "Missing or failed"): a thing
    a cue needs that is absent or broken tonight - no audio interface running,
    a screen an output was given and is not there, a MIDI or serial port not
    found, a plugin missing or failed, a named input past the interface, a
    video input or insert with nothing arriving, a network device that says it
    cannot be used, a sound or a movie whose file is missing. Waiting is not a
    problem: timecode not arriving, a device not answering, plugins waiting for
    Load now, a renderer starting, a port opening.

    THE ENGINE KEEPS THE FACTS; THIS JOINS THEM. Every one of those states is
    already published on the object it is about - `port/<id>/problem`,
    `videoOutput/<id>/bound`, `plugin/<id>/state` and the rest - and never on
    the cues that use it. The join is here, in the client, as the read-ahead's
    marks are: what each cue names is read when the show changes, what each
    object says is read by address on every pass, and nothing walks the whole
    tree on a pass. No JUCE: the snapshot's words in, problems and marks out.
*/

#include <wfg/client/model/Icons.h>
#include <wfg/client/model/ShowModel.h>

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    /*  ONE THING MISSING OR FAILED. `page` is the Show settings page it is put
        right on (model/SettingsPages.h), empty for a cue's own file; `key` is
        what a cue's needs name it by - "audio", "port:<id>", "plugin:<id>",
        "videoOutput:<id>", "file:<cue>", "slices:<cue>" and the like; `word`
        is what a cue row says in a few letters; `sentence` says what is wrong
        and where it is put right, whole, for a tooltip. */
    struct Problem
    {
        std::string page;
        std::string key;
        std::string word;
        std::string sentence;

        bool operator== (const Problem&) const = default;
    };

    /*  WHAT THE SHOW DECLARES THAT CAN GO MISSING, by identifier and name, read
        when the show changes and never on a pass: the readers that list
        objects walk the whole tree, which is a cost to pay once per edit. */
    struct ObjectIndex
    {
        struct Named
        {
            std::string id;
            std::string name;
            bool enabled = true;

            bool operator== (const Named&) const = default;
        };

        std::string revision;       ///< `/godot/document/revision` it was read at

        std::vector<Named> ports, serials, inputs, devices, surfaces;
        std::vector<Named> videoOutputs, videoInputs, videoInserts;

        //  The plugins of the set, and those of each rack channel, with the channel's name.
        std::vector<Named> setPlugins;
        std::vector<std::pair<Named, std::vector<Named>>> rackChannels;

        //  A canvas, by identifier, and the outputs it is shown on - as an output's canvas or a zone's.
        std::map<std::string, std::vector<std::string>> outputsOfCanvas;

        int outputChannels = 0;     ///< the channels the show's outputs take
        bool needsSound = false;    ///< a cue somewhere in the show plays sound or takes a mic

        /*  THE SOUND CUES WITH MORE SLICES THAN THE FEWEST THE AUDIO IS EVER
            BUILT WITH (eight, PRD §3.25), by identifier, with how many and
            what the row is called: the only ones that can have gone past
            `audio/slots` since the graph was built. */
        struct Sliced
        {
            std::string id;
            std::string label;
            int slices = 0;

            bool operator== (const Sliced&) const = default;
        };

        std::vector<Sliced> sliced;

        bool operator== (const ObjectIndex&) const = default;
    };

    ObjectIndex indexObjects (const tree::TreeSnapshot&);

    /*  EVERYTHING THE MACHINE LACKS TONIGHT, in the order of the pages that put
        each right, read by address on every pass. */
    std::vector<Problem> machineProblems (const tree::TreeSnapshot&, const ObjectIndex&);

    /*  WHAT EACH CUE OF THE LIST SHOWN NEEDS, by the keys `Problem` uses - read
        when the rows are built again. A shut group's members are not rows, so
        what they need is kept under the group as well, which carries their
        marks while it is shut ("inside"). `files` are the cues whose file is
        looked at on every pass, in the list's order, the hidden ones with them. */
    struct CueNeeds
    {
        std::map<std::string, std::vector<std::string>> keysOf;
        std::map<std::string, std::vector<std::string>> hiddenIn;
        std::vector<std::pair<std::string, std::string>> files;     ///< cue, its file

        bool operator== (const CueNeeds&) const = default;
    };

    CueNeeds needsOf (const tree::TreeSnapshot&, const std::vector<Row>&, const ObjectIndex&);

    /*  The cues of the list whose file was looked for and is not there
        (`cue/<id>/missing`, namespace draft §64), one problem each, page empty. */
    std::vector<Problem> fileProblems (const tree::TreeSnapshot&, const CueNeeds&);

    /*  EVERY CUE THAT NEEDS SOMETHING MISSING, marked: the warning shape, the
        first problem's word ("inside" on a shut group whose hidden members
        need one), and every sentence for the tooltip. */
    std::map<std::string, Mark> cueProblems (const CueNeeds&, const std::vector<Problem>&);

    /*  THE ROW'S ONE CELL: a problem wins over how ready a cue is - "ready"
        is untrue of a cue whose screen is missing. */
    std::map<std::string, Mark> withProblems (std::map<std::string, Mark> readiness,
                                              const std::map<std::string, Mark>& problems);

    /*  BY PAGE, for the main window's warnings and the settings window's marks:
        one entry per page with a problem, in the settings window's order, the
        missing files last with an empty page. `cues` are the cues a missing
        file names, in the list's order, for the warning that reveals them. */
    struct PageProblems
    {
        std::string page;
        Icon icon = Icon::none;
        std::vector<std::string> sentences;
        std::vector<std::string> cues;

        bool operator== (const PageProblems&) const = default;
    };

    std::vector<PageProblems> byPage (const std::vector<Problem>&);

    /*  The icon a page's warning wears: a cue kind's where one fits (the audio
        interface a sound, MIDI its socket, the network an OSC message), its own
        otherwise; the missing files the read-ahead's struck ring. */
    Icon iconForPage (std::string_view page);
}
