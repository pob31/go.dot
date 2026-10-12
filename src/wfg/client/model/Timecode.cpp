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

#include <wfg/client/model/MidiPorts.h>
#include <wfg/client/model/NewCue.h>
#include <wfg/client/model/OutputList.h>
#include <wfg/client/model/Text.h>
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

        row.mtcLate = count (at ("mtcLate"));
        row.ltcLate = count (at ("ltcLate"));
        row.mtcWorstMs = osc::parseDouble (at ("mtcWorst")).value_or (0.0);

        return row;
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

        for (const auto& output : outputs)
        {
            const auto name = output.name.empty() ? output.id : output.name;

            /*  MARKED RATHER THAN FILTERED, as a cue's direct out marks a busy
                output (PRD 3.9b): the output somebody expected is there, and
                says why it cannot be picked. A direct out and a mix channel
                alike - the engine takes either, so long as it is one channel. */
            if (output.width > 1)
                choices.push_back ({ output.id, name + " (" + std::to_string (output.width)
                                                  + " channels: LTC needs one)", false });
            else
                choices.push_back ({ output.id, name, true });
        }

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
}
