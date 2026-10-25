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

#include <wfg/client/model/Timecode.h>

#include <wfg/client/model/InputList.h>
#include <wfg/client/model/MidiPorts.h>
#include <wfg/client/model/NewCue.h>
#include <wfg/client/model/OutputList.h>
#include <wfg/client/model/Text.h>
#include <wfg/engine/cue/GroupMode.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/timecode/Timecode.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wfg::client::model
{
    namespace
    {
        constexpr std::string_view syncPrefix = "/godot/sync/";

        /*  A COUNT THE ENGINE WROTE as a whole number, read so a malformed one
            is nought rather than a throw: the readouts are a quiet line, and a
            line is never worth a window that does not come back. */
        long long count (const std::string& reading)
        {
            const auto value = osc::parseDouble (reading);
            return value.has_value() && *value > 0.0 ? static_cast<long long> (std::llround (*value)) : 0;
        }

        /*  ONE CHANNEL OR NOT AT ALL, for both ends of the code: LTC replaces
            one output channel (AHI) and is decoded from one input channel
            (AIM). An output or an input wider than that is listed - marked
            rather than filtered, so the one somebody expected is there - and
            says why it cannot be picked, in the same words either way. */
        SyncChoice oneChannel (const std::string& id, const std::string& name, int width)
        {
            if (width > 1)
                return { id, name + " (" + std::to_string (width) + " channels: LTC needs one)", false };

            return { id, name, true };
        }

        /*  HOW FAR A CODE'S SPEED IS FROM GO.DOT'S CLOCK, in parts per million
            - "+12 ppm", "-3 ppm", "0 ppm" - which is the size a drift between
            two crystals comes in. A code a hundredth or more away is not
            drifting but running at another speed, a deck wound or a DAW
            varispeeded, and says so as the window says any speed: "×0.5". */
        std::string speedFromOne (double speed)
        {
            const auto off = (speed - 1.0) * 1.0e6;

            if (! (std::abs (off) < 10000.0))
                return speedText (speed);

            const auto ppm = std::llround (off);
            return (ppm > 0 ? "+" : "") + std::to_string (ppm) + " ppm";
        }

        //  The separator the pane's words use between two readings: a middle dot, as UTF-8.
        constexpr const char* separator = " \xc2\xb7 ";
    }

    SyncRow readSync (const tree::TreeSnapshot& snapshot)
    {
        /*  SIX ADDRESSES, ASKED BY NAME: a container of six rows has nothing
            to gain from a walk of the whole tree. A row the engine has not
            published keeps the default the struct starts with, which is the
            engine's own. */
        SyncRow row;

        const auto at = [&snapshot] (const char* name)
        {
            return text (snapshot, std::string (syncPrefix) + name);
        };

        if (const auto rate = at ("frameRate"); ! rate.empty())
            row.frameRate = rate;

        row.mtcOut = at ("mtcOut");
        row.ltcOut = at ("ltcOut");

        if (const auto level = at ("ltcLevel"); ! level.empty())
            row.ltcLevel = level;

        //  The beat grid's rows (namespace draft §122), the engine's defaults when unsaid.
        if (const auto bpm = at ("bpm"); ! bpm.empty())
            row.bpm = bpm;

        if (const auto beats = at ("beatsPerBar"); ! beats.empty())
            row.beatsPerBar = beats;

        row.mtcLate = count (at ("mtcLate"));
        row.ltcLate = count (at ("ltcLate"));
        row.mtcWorstMs = osc::parseDouble (at ("mtcWorst")).value_or (0.0);

        /*  AND THE CODES COMING IN (namespace draft §60, TG.10), asked by name
            as the rest: where each comes in, the four chase settings - kept as
            the tree wrote them, so a box shows "0.25" and not a double's tail -
            and what the machine hears of each. */
        row.ltcIn = at ("ltcIn");
        row.mtcIn = at ("mtcIn");

        for (auto [stored, name] : { std::pair { &row.chaseTolerance, "chaseTolerance" },
                                     std::pair { &row.chaseJump, "chaseJump" },
                                     std::pair { &row.chaseRate, "chaseRate" },
                                     std::pair { &row.chaseCrossfade, "chaseCrossfade" } })
            if (const auto value = at (name); ! value.empty())
                *stored = value;

        row.ltcHeard = readCodeIn (snapshot, "ltc");
        row.mtcHeard = readCodeIn (snapshot, "mtc");

        return row;
    }

    SyncRow::CodeIn readCodeIn (const tree::TreeSnapshot& snapshot, std::string_view source)
    {
        const auto base = std::string (syncPrefix) + std::string (source) + "In";
        SyncRow::CodeIn heard;

        if (const auto state = text (snapshot, base + "State"); ! state.empty())
            heard.state = state;

        heard.code = text (snapshot, base + "Code");
        heard.rate = text (snapshot, base + "Rate");
        heard.speed = osc::parseDouble (text (snapshot, base + "Speed")).value_or (0.0);

        return heard;
    }

    const std::vector<std::pair<std::string, std::string>>& rateChoices()
    {
        static const std::vector<std::pair<std::string, std::string>> choices {
            { "23.976",  "23.976" },
            { "24",      "24" },
            { "25",      "25" },
            { "29.97df", "29.97 drop-frame" },
            { "29.97",   "29.97" },
            { "30",      "30" },
        };

        return choices;
    }

    std::string rateWord (std::string_view stored)
    {
        for (const auto& [value, word] : rateChoices())
            if (value == stored)
                return word;

        return std::string (stored);
    }

    std::string rateStored (std::string_view word)
    {
        for (const auto& [value, shown] : rateChoices())
            if (shown == word)
                return value;

        return {};
    }

    std::vector<SyncChoice> mtcPortChoices (const std::vector<PortRow>& ports)
    {
        std::vector<SyncChoice> choices;
        choices.reserve (ports.size() + 1);

        /*  NONE IS A CHOICE AND NOT AN ABSENCE, as it is for a cue's port: a
            show that sends no MTC says so, and it has to be possible to go back
            to it. First, because that is where a hand goes looking for it. */
        choices.push_back ({ {}, timecodeWords::none, true });

        for (const auto& port : ports)
            choices.push_back ({ port.id, port.label(), true });

        return choices;
    }

    std::vector<SyncChoice> ltcOutputChoices (const std::vector<OutputRow>& outputs)
    {
        std::vector<SyncChoice> choices;
        choices.reserve (outputs.size() + 1);
        choices.push_back ({ {}, timecodeWords::none, true });

        /*  MARKED RATHER THAN FILTERED, as a cue's direct out marks a busy
            output (PRD 3.9b): the output somebody expected is there, and says
            why it cannot be picked. A direct out and a mix channel alike - the
            engine takes either, so long as it is one channel. */
        for (const auto& output : outputs)
            choices.push_back (oneChannel (output.id, output.name.empty() ? output.id : output.name, output.width));

        return choices;
    }

    std::vector<SyncChoice> ltcInputChoices (const std::vector<InputRow>& inputs)
    {
        std::vector<SyncChoice> choices;
        choices.reserve (inputs.size() + 1);
        choices.push_back ({ {}, timecodeWords::none, true });

        //  The output menu's rule, from the other side (namespace draft §60, AIM).
        for (const auto& input : inputs)
            choices.push_back (oneChannel (input.id, input.name.empty() ? input.id : input.name, input.width));

        return choices;
    }

    std::string lateLine (const SyncRow& row)
    {
        /*  IN NO LOCALE'S SPELLING, the worst to the hundredth the engine keeps
            it to; the separator is a middle dot in UTF-8, the run pane's own. */
        return "MTC late: " + std::to_string (row.mtcLate) + " (worst " + shownNumber (row.mtcWorstMs, "ms")
               + " ms) \xc2\xb7 LTC late: " + std::to_string (row.ltcLate);
    }

    std::string formatWord (std::string_view format)
    {
        if (format == "mtc")
            return timecodeWords::mtcKind;

        if (format == "ltc")
            return timecodeWords::ltcKind;

        //  A format the engine grows later keeps a word rather than a blank cell.
        return kindWord ("timecode");
    }

    const std::vector<std::pair<std::string, std::string>>& formatChoices()
    {
        static const std::vector<std::pair<std::string, std::string>> choices {
            { "mtc", timecodeWords::mtcFormat },
            { "ltc", timecodeWords::ltcFormat },
        };

        return choices;
    }

    std::string shownLabel (std::string_view stored, std::string_view rate)
    {
        std::string out (stored);

        /*  THE LAST SEPARATOR ONLY, the one before the frames, and only when
            the label has the four parts a reader shows - a box somebody is
            halfway through typing into is left alone. */
        if (rate != "29.97df" || out.size() != 11)
            return out;

        if (out[2] == ':' && out[5] == ':' && out[8] == ':')
            out[8] = ';';

        return out;
    }

    std::string labelAt (std::string_view from, std::string_view rate, double seconds)
    {
        /*  THE ENGINE'S OWN ARITHMETIC (engine/timecode/Timecode.h, pure, the
            one copy of the drop-frame rule), so the box and the code the desk
            then reads cannot disagree. */
        const auto start = wfg::timecode::parseLabel (from);
        const auto code = wfg::timecode::rateFromWord (rate);

        if (! start.has_value() || ! code.has_value())
            return {};

        /*  WHOLE FRAMES, as the Runner's `framesInto` counts a jump: a floor,
            with a millionth of a frame added for a second that was itself
            made of whole frames, and nothing before the start. */
        auto frames = std::int64_t { 0 };

        if (seconds > 0.0)
        {
            const auto exact = seconds * static_cast<double> (wfg::timecode::numOf (*code))
                                 / static_cast<double> (wfg::timecode::denOf (*code));
            frames = static_cast<std::int64_t> (std::floor (std::min (exact, 1.0e15) + 1.0e-6));
        }

        return wfg::timecode::formatLabel (wfg::timecode::labelOf (wfg::timecode::frameOf (*start, *code) + frames,
                                                                   *code),
                                           *code, true);
    }

    //==========================================================================
    std::string codeInLine (std::string_view source, const SyncRow::CodeIn& heard)
    {
        if (heard.state.empty() || heard.state == "none")
            return {};

        /*  THE ENGINE'S OWN STATE WORD, as the readout spells it - running,
            stopped, absent - then only what that state has to say: a code
            nobody hears has no label, a stopped one no speed. */
        auto line = std::string (source == "mtc" ? timeGroupWords::mtcIn : timeGroupWords::ltcIn) + ": " + heard.state;

        if (heard.state == "absent")
            return line;

        if (! heard.code.empty())
            line += ", " + shownLabel (heard.code, heard.rate);

        if (! heard.rate.empty())
            line += (heard.code.empty() ? ", at " : " at ") + rateWord (heard.rate);

        if (heard.state == "running")
            line += ", " + speedFromOne (heard.speed);

        return line;
    }

    std::vector<std::string> comingInLines (const SyncRow& row)
    {
        std::vector<std::string> lines;

        for (const auto& line : { codeInLine ("ltc", row.ltcHeard), codeInLine ("mtc", row.mtcHeard) })
            if (! line.empty())
                lines.push_back (line);

        return lines;
    }

    const std::vector<std::pair<std::string, std::string>>& timeSourceChoices()
    {
        static const std::vector<std::pair<std::string, std::string>> choices {
            { "ltc",   timeGroupWords::ltcIn },
            { "mtc",   timeGroupWords::mtcIn },
            { "clock", timeGroupWords::clock },
        };

        return choices;
    }

    const std::vector<std::pair<std::string, std::string>>& chaseChoices()
    {
        static const std::vector<std::pair<std::string, std::string>> choices {
            { "free",        timeGroupWords::freeRun },
            { "jump",        timeGroupWords::jump },
            { "varispeed",   timeGroupWords::varispeed },
            { "timestretch", timeGroupWords::timestretch },
        };

        return choices;
    }

    std::string groupModeWord (std::string_view mode)
    {
        //  The engine's own question (cue/GroupMode.h), so the two cannot disagree about what a time group is.
        if (cue::isTimeGroup (cue::GroupMode { std::string (mode), {} }))
            return timeGroupWords::kind;

        return std::string (mode);
    }

    std::string memberTime (std::string_view at, std::string_view source, std::string_view rate)
    {
        /*  A CLOCK MEMBER'S TIME IS ALREADY HOW A PERSON READS IT, `HH:MM:SS`
            - the judge keeps it as typed (TG.2) - and a label is a label at
            the show's rate (AIL), its drop-frame semicolon shown as the
            timecode cue's start is. */
        if (source == "clock")
            return std::string (at);

        return shownLabel (at, rate);
    }

    std::string clockLabel (long long millisecondOfDay)
    {
        if (millisecondOfDay < 0)
            return {};

        const auto second = millisecondOfDay % 86400000 / 1000;

        //  By hand, as every number here: no locale groups or pads it differently.
        const auto two = [] (long long value)
        {
            return std::string (value < 10 ? "0" : "") + std::to_string (value);
        };

        return two (second / 3600) + ":" + two (second / 60 % 60) + ":" + two (second % 60);
    }

    std::string followsWords (std::string_view source, const SyncRow::CodeIn& heard, long long clockMilliseconds)
    {
        if (source == "clock")
        {
            const auto now = clockLabel (clockMilliseconds);
            return now.empty() ? std::string (timeGroupWords::clockCode)
                               : std::string (timeGroupWords::clockCode) + " " + now;
        }

        /*  LTC BY DEFAULT, as the row's table has it: an empty word read raw
            is the default. A running code is its label alone - the state the
            label moving says - and any other says its state word first. */
        auto words = std::string (source == "mtc" ? timeGroupWords::mtcCode : timeGroupWords::ltcCode);

        if (heard.state != "running" && ! heard.state.empty())
            words += " " + heard.state;

        if (! heard.code.empty())
            words += " " + shownLabel (heard.code, heard.rate);

        return words;
    }

    std::string chaseWords (std::string_view state, double errorMilliseconds)
    {
        if (state.empty() || state == "free")
            return {};

        /*  WHOLE MILLISECONDS, AND NO SIDE: what an operator wants of a chaser
            is whether it is with its code, and a millisecond is the finest a
            catch-up settles to (AIS). The two states worth a word are the ones
            that are doing something about it. */
        auto words = std::to_string (std::llround (std::abs (errorMilliseconds))) + " " + timeGroupWords::msOut;

        if (state == "catching" || state == "jumped")
            words += separator + std::string (state);

        return words;
    }
}
