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

#include <wfg/client/model/TimeText.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <atomic>
#include <cmath>
#include <vector>

namespace wfg::client::model
{
    namespace
    {
        std::atomic<TimeFormat> shown { TimeFormat::seconds };

        std::string twoDigits (long long value)
        {
            return (value < 10 ? "0" : "") + std::to_string (value);
        }

        std::string trimmed (const std::string& typed)
        {
            const auto first = typed.find_first_not_of (" \t");

            if (first == std::string::npos)
                return {};

            return typed.substr (first, typed.find_last_not_of (" \t") - first + 1);
        }

        //  A number as typed, a comma taken for the point.
        std::optional<double> number (std::string part)
        {
            for (auto& c : part)
                if (c == ',')
                    c = '.';

            if (part.empty())
                return std::nullopt;

            const auto value = osc::parseDouble (part);

            if (! value.has_value() || ! std::isfinite (*value))
                return std::nullopt;

            return value;
        }
    }

    void setShownTimeFormat (TimeFormat format) noexcept   { shown.store (format); }
    TimeFormat shownTimeFormat() noexcept                  { return shown.load(); }
    bool clockShown() noexcept                             { return shown.load() == TimeFormat::clock; }

    TimeFormat timeFormatOf (const tree::TreeSnapshot& snapshot)
    {
        return text (snapshot, "/godot/list/timeFormat") == "clock" ? TimeFormat::clock : TimeFormat::seconds;
    }

    std::string tenthsReading (double seconds)
    {
        const auto tenths = static_cast<long long> (seconds * 10.0 + (seconds < 0 ? -0.5 : 0.5));
        const auto whole = tenths / 10;
        const auto fraction = tenths % 10;

        /*  -0.04 rounds to nought tenths and is written "0.0"; -0.4 keeps its sign
            although its whole part is nought. */
        const auto sign = tenths < 0 && whole == 0 ? "-" : "";
        return sign + std::to_string (whole) + "." + std::to_string (fraction < 0 ? -fraction : fraction);
    }

    std::string clockTime (double seconds, int decimals, bool trim)
    {
        decimals = decimals < 0 ? 0 : (decimals > 3 ? 3 : decimals);

        long long scale = 1;

        for (int n = 0; n < decimals; ++n)
            scale *= 10;

        /*  ROUNDED ONCE, IN WHOLE UNITS OF THE LAST PLACE, and every part taken
            from that count: 59.96 at one decimal is 600 tenths, which is 1:00.0
            and never 0:60.0. */
        const auto negative = seconds < 0.0;
        const auto units = static_cast<long long> (std::llround (std::abs (seconds) * static_cast<double> (scale)));
        const auto whole = units / scale;
        const auto fraction = units % scale;

        const auto hours = whole / 3600;
        const auto minutes = (whole / 60) % 60;
        const auto secs = whole % 60;

        std::string out = negative && units > 0 ? "-" : "";

        if (hours > 0)
            out += std::to_string (hours) + ":" + twoDigits (minutes) + ":" + twoDigits (secs);
        else
            out += std::to_string (minutes) + ":" + twoDigits (secs);

        if (decimals > 0)
        {
            auto digits = std::to_string (fraction);
            digits.insert (0, static_cast<std::size_t> (decimals) - digits.size(), '0');

            if (trim)
                while (! digits.empty() && digits.back() == '0')
                    digits.pop_back();

            if (! digits.empty())
                out += "." + digits;
        }

        return out;
    }

    std::string readingTime (double seconds)
    {
        return clockShown() ? clockTime (seconds, 1) : tenthsReading (seconds);
    }

    bool typedAsClock (const std::string& typed)
    {
        return typed.find (':') != std::string::npos;
    }

    std::optional<double> typedTime (const std::string& typed)
    {
        const auto text = trimmed (typed);

        if (text.empty())
            return std::nullopt;

        std::vector<std::string> parts;
        std::size_t from = 0;

        for (auto colon = text.find (':'); colon != std::string::npos; colon = text.find (':', from))
        {
            parts.push_back (text.substr (from, colon - from));
            from = colon + 1;
        }

        parts.push_back (text.substr (from));

        //  Seconds; minutes and seconds; hours, minutes and seconds.
        if (parts.size() > 3)
            return std::nullopt;

        double total = 0.0;

        for (std::size_t at = 0; at < parts.size(); ++at)
        {
            const auto value = number (trimmed (parts[at]));

            if (! value.has_value() || *value < 0.0)
                return std::nullopt;

            //  Every place but the first counts below sixty.
            if (at > 0 && *value >= 60.0)
                return std::nullopt;

            //  Only the last place takes a fraction: "1.5:20" is not a time.
            if (at + 1 < parts.size() && std::floor (*value) < *value)
                return std::nullopt;

            total = total * 60.0 + *value;
        }

        return total;
    }
}
