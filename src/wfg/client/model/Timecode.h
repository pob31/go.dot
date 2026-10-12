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
    The timecode a show generates, as the window reads it and offers it
    (namespace draft §59, TC.8).

    THE SHOW SAYS WHERE AND AT WHAT RATE, a cue only where its code starts and
    how long it runs (AHA): so the Timecode tab of Show settings holds the rate,
    the MIDI port MTC leaves on and the output LTC plays on, and every one of
    them is an ordinary `node.set` on a `/godot/sync` row - one the engine keeps
    in `<Sync>` whether or not anybody has set it, so there is no command of its
    own to send. This file reads those rows, says them in words, and builds the
    menus the tab and the inspector offer; it never decides anything the engine
    would refuse differently.

    THE WORDS ARE IN ONE PLACE, HERE. They were worded by the implementer and
    the author accepted them as offered on 2026-10-12, so a later change to one
    of them is one edit: every list line, tab row, menu item, inspector label
    and sentence the window shows about timecode is a constant below, and the
    window and the new-cue lists read them from here rather than spelling them
    again.

    std only, and a pure reading of the snapshot the window already holds, like
    the rest of model/.
*/

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    struct PortRow;
    struct OutputRow;

    namespace timecodeWords
    {
        //  The tab of Show settings, and the heading the + midi and + audio lists put their line under.
        inline constexpr const char* tab = "Timecode";
        inline constexpr const char* section = "Timecode";

        //  The + midi list's line, and the + audio list's after the templates.
        inline constexpr const char* mtcLine = "MTC";
        inline constexpr const char* mtcExplanation = "MIDI Time Code, on the port Show settings > Timecode names";
        inline constexpr const char* ltcLine = "LTC timecode";
        inline constexpr const char* ltcExplanation = "SMPTE linear timecode, on the output Show settings > Timecode names";

        //  The tab's rows, the first item of its two menus, and the level's unit.
        inline constexpr const char* frameRate = "Frame rate";
        inline constexpr const char* mtcOut = "MTC leaves on";
        inline constexpr const char* ltcOut = "LTC plays on";
        inline constexpr const char* ltcLevel = "LTC level";
        inline constexpr const char* ltcLevelUnit = "dBFS";
        inline constexpr const char* none = "None";

        //  Under the LTC row while an output is chosen.
        inline constexpr const char* timecodeOnly = "This output carries timecode only: no sound is heard on it.";

        /*  The inspector's three rows of a timecode cue, in the house's lower
            case as every inspector label is, and what a length of nought reads. */
        inline constexpr const char* format = "format";
        inline constexpr const char* startsAt = "starts at";
        inline constexpr const char* runsFor = "runs for";
        inline constexpr const char* untilStopped = "until stopped";

        //  The kind column, and the inspector's two formats.
        inline constexpr const char* mtcKind = "MTC";
        inline constexpr const char* ltcKind = "LTC";
        inline constexpr const char* mtcFormat = "MTC (MIDI)";
        inline constexpr const char* ltcFormat = "LTC (audio)";
    }

    /*  THE SHOW'S `<Sync>` ROWS, as the tree publishes them: the three it
        stores - which the tab edits - and the three the machine reports. Each
        stored one starts at the engine's own default, so a snapshot that has
        not said anything yet reads as a show nobody has set. */
    struct SyncRow
    {
        std::string frameRate { "25" };     ///< as the row stores it: 23.976, 24, 25, 29.97df, 29.97, 30
        std::string mtcOut;                 ///< a `<Port>`'s identifier, empty for none
        std::string ltcOut;                 ///< a `<Bus>`'s identifier, empty for none
        std::string ltcLevel { "-18" };     ///< dBFS, as the tree wrote it

        long long mtcLate = 0;              ///< MTC messages that left more than 2 ms after their moment
        double mtcWorstMs = 0.0;            ///< the latest of them
        long long ltcLate = 0;              ///< LTC starts, stops and jumps that missed their sample
    };

    SyncRow readSync (const tree::TreeSnapshot&);

    /*  THE RATES, as the row stores each and as a person reads it, in the
        row's own order: only 29.97 drop-frame is spelled differently, because
        "29.97df" is a word for a file and not for a menu. */
    const std::vector<std::pair<std::string, std::string>>& rateChoices();

    /** "29.97df" -> "29.97 drop-frame"; a word the table does not know is shown as it is. */
    std::string rateWord (std::string_view stored);

    /** The other way: "29.97 drop-frame" -> "29.97df"; empty for a word nobody offered. */
    std::string rateStored (std::string_view word);

    /*  ONE ITEM OF THE TAB'S TWO MENUS: what the row would store, what the
        menu says, and whether it can be picked. */
    struct SyncChoice
    {
        std::string id;
        std::string label;
        bool enabled = true;

        bool operator== (const SyncChoice&) const = default;
    };

    /** "None", then every declared MIDI port by its name, in identifier order. */
    std::vector<SyncChoice> mtcPortChoices (const std::vector<PortRow>&);

    /*  "None", then every output by its name, in list order - and an output
        wider than one channel there too but DISABLED, with the reason after
        its name: LTC replaces one channel (AHI), so a stereo output is never
        offered, and leaving it out would leave somebody wondering where it
        went. */
    std::vector<SyncChoice> ltcOutputChoices (const std::vector<OutputRow>&);

    /** The tab's quiet line: "MTC late: 0 (worst 0 ms) · LTC late: 0". */
    std::string lateLine (const SyncRow&);

    /** What the kind column says of a timecode cue: MTC or LTC, else the kind's own word. */
    std::string formatWord (std::string_view format);

    /** The inspector's format menu: each stored word and what it reads. */
    const std::vector<std::pair<std::string, std::string>>& formatChoices();

    /*  A START AS A PERSON READS IT AT THIS RATE. The engine stores a label
        with colons whatever the rate - what somebody decided, readable at the
        next venue (AHF) - and a reader shows a semicolon before the frames at
        29.97 drop-frame, as the run's own readout does: that is the one
        difference here. Anything else is shown as it is stored. */
    std::string shownLabel (std::string_view stored, std::string_view rate);

    /*  THE LABEL A CODE STARTED AT `from` READS `seconds` PAST ITS START, at
        `rate` (a stored rate word), as a reader shows it - a semicolon before
        the frames at drop-frame: the start's frame plus the whole frames the
        second falls in, which is where the Runner puts a jump (TC.7), wrapped
        at midnight. What a scrub's ghost says it would land on (TC.8). Empty
        when the start or the rate does not read. */
    std::string labelAt (std::string_view from, std::string_view rate, double seconds);
}
