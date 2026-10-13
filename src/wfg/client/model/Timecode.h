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

    AND SINCE TG.10 THE CODE COMING IN (namespace draft §60): the time group
    a code starts the members of, the input LTC comes in on and the port MTC
    does, how a member chases, and what the window says of each - the same
    rows, the same tab and the same rule that every word is a constant in one
    place, `timeGroupWords` beside `timecodeWords`.
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
    struct InputRow;

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

    /*  THE WORDS OF A TIME GROUP (namespace draft §60, AJA, TG.10). Worded by
        the implementer and accepted by the author as offered (2026-10-12/13),
        each in this one place so a later change is one edit, as the outgoing
        side's are above. */
    namespace timeGroupWords
    {
        //  The + group list's line and what it says after the dash.
        inline constexpr const char* line = "Time group";
        inline constexpr const char* explanation = "its members start when LTC, MTC or the clock reaches their time";

        //  The group's inspector row, `timeSource`, and its three answers.
        inline constexpr const char* follows = "follows";
        inline constexpr const char* ltcIn = "LTC in";
        inline constexpr const char* mtcIn = "MTC in";
        inline constexpr const char* clock = "the clock";

        /*  A member's two rows, `at` and `chase`, and the four ways it keeps
            up with its code. "starts at" is the timecode cue's own words too,
            kept apart because it is another row a later edit may word apart. */
        inline constexpr const char* startsAt = "starts at";
        inline constexpr const char* chase = "chase";
        inline constexpr const char* freeRun = "free run";
        inline constexpr const char* jump = "jump";
        inline constexpr const char* varispeed = "catch up, varispeed";
        inline constexpr const char* timestretch = "catch up, timestretch";

        //  The cue list's kind column for a time group.
        inline constexpr const char* kind = "time";

        //  The Timecode tab's part for the codes that come in, its rows and their units.
        inline constexpr const char* comingIn = "Coming in";
        inline constexpr const char* ltcInOn = "LTC comes in on";
        inline constexpr const char* mtcInOn = "MTC comes in on";
        inline constexpr const char* inStepWithin = "in step within";
        inline constexpr const char* jumpsPast = "jumps past";
        inline constexpr const char* catchesUpBy = "catches up by at most";
        inline constexpr const char* jumpCrossfade = "jump crossfade";
        inline constexpr const char* milliseconds = "ms";
        inline constexpr const char* seconds = "s";
        inline constexpr const char* percent = "%";

        /*  The running pane: the code a time group's row follows - "LTC
            01:02:03:04", "clock 20:15:03" - and how far a chaser is out of
            step, "3 ms out". */
        inline constexpr const char* ltcCode = "LTC";
        inline constexpr const char* mtcCode = "MTC";
        inline constexpr const char* clockCode = "clock";
        inline constexpr const char* msOut = "ms out";
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

        /*  THE CODES COMING IN (namespace draft §60, AIM, AIN): where each
            comes in, and how a member that chases keeps up - stored rows the
            tab's Coming in part edits, each starting at the engine's default. */
        std::string ltcIn;                      ///< an `<Input>`'s identifier, empty for none
        std::string mtcIn;                      ///< a `<Port>`'s identifier, empty for none
        std::string chaseTolerance { "10" };    ///< ms out of step before a chaser acts
        std::string chaseJump { "0.25" };       ///< s out before a catch-up chaser jumps instead
        std::string chaseRate { "1" };          ///< % the most a catch-up adds
        std::string chaseCrossfade { "10" };    ///< ms a jump's crossfade takes

        /*  WHAT THE MACHINE HEARS OF ONE OF THEM, `sync/{ltc,mtc}In{State,
            Code,Rate,Speed}`: `none` while the show names no source, `absent`
            while it names one and nothing readable comes, else `running` or
            `stopped`; the label it reads, the rate it is read at in the words
            `sync/frameRate` takes, and its speed against Go.dot's own clock. */
        struct CodeIn
        {
            std::string state { "none" };
            std::string code;
            std::string rate;
            double speed = 0.0;
        };

        CodeIn ltcHeard;
        CodeIn mtcHeard;
    };

    SyncRow readSync (const tree::TreeSnapshot&);

    /** One incoming code's readouts, by its source's word: "ltc" or "mtc". */
    SyncRow::CodeIn readCodeIn (const tree::TreeSnapshot&, std::string_view source);

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

    //==========================================================================
    //  THE CODE COMING IN, AND THE TIME GROUPS IT STARTS (namespace draft §60, TG.10)

    /*  "None", then every named input by its name, in list order - one wider
        than a channel there but DISABLED, with the reason after its name, as
        `ltcOutputChoices` marks an output: the decoder reads one channel
        (AIM), and an input somebody expected that is simply missing reads as
        a fault. The port MTC comes in on is offered by `mtcPortChoices`. */
    std::vector<SyncChoice> ltcInputChoices (const std::vector<InputRow>&);

    /*  ONE LINE OF WHAT A CODE COMING IN IS DOING, in the Coming in part:
        "LTC in: running, 01:02:03:04 at 25, +12 ppm" - the state, the label it
        reads (a semicolon before the frames at drop-frame, as a reader shows
        it), the rate it is read at, and how far its speed is from Go.dot's
        clock in parts per million; a stopped code says where it stopped, an
        absent one says no more. Empty for a source the show does not name. */
    std::string codeInLine (std::string_view source, const SyncRow::CodeIn&);

    /** A line for each source the show names, LTC first. */
    std::vector<std::string> comingInLines (const SyncRow&);

    /** The group inspector's `timeSource` menu, each stored word and what it reads. */
    const std::vector<std::pair<std::string, std::string>>& timeSourceChoices();

    /** A member inspector's `chase` menu, each stored word and what it reads. */
    const std::vector<std::pair<std::string, std::string>>& chaseChoices();

    /*  WHAT THE KIND COLUMN SAYS OF A GROUP: "time" for a time group, and
        any other group's mode as the tree spells it, as it always has. */
    std::string groupModeWord (std::string_view mode);

    /*  A MEMBER'S TIME AS THE LIST AND THE INSPECTOR SHOW IT, under its
        group's source: a label at the show's rate with its semicolon at
        drop-frame for LTC and MTC, the stored `HH:MM:SS` for the clock. */
    std::string memberTime (std::string_view at, std::string_view source, std::string_view rate);

    /** A millisecond of the day as the wall clock reads: "20:15:03". Empty below nought. */
    std::string clockLabel (long long millisecondOfDay);

    /*  WHAT A TIME GROUP'S RUN SAYS IT FOLLOWS, in the running pane: "LTC
        01:02:03:04" while the code runs, "LTC stopped 01:02:03:04" or "MTC
        absent" otherwise, and "clock 20:15:03" for the wall clock, read from
        `clockMilliseconds` - the window's own clock, which is the engine
        machine's - or "clock" alone when it is not known (below nought). */
    std::string followsWords (std::string_view source, const SyncRow::CodeIn& heard, long long clockMilliseconds);

    /*  HOW FAR A CHASING MEMBER IS OUT OF STEP, in the running pane: "3 ms
        out", whichever side of its code, and the engine's own word after it
        while it is catching up or on the tick it jumped. Empty for a run that
        does not chase - `state` empty or `free`. */
    std::string chaseWords (std::string_view state, double errorMilliseconds);
}
