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

#include <wfg/client/model/Text.h>

#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::client::model
{
    std::string text (const osc::Value& value)
    {
        switch (value.type())
        {
            case osc::Value::Type::int32:     return std::to_string (value.getInt32());
            case osc::Value::Type::int64:     return std::to_string (value.getInt64());
            case osc::Value::Type::float32:   return osc::formatFloat (value.getFloat32());
            case osc::Value::Type::float64:   return osc::formatDouble (value.getFloat64());
            case osc::Value::Type::string:    return value.getString();
            case osc::Value::Type::boolTrue:  return "true";
            case osc::Value::Type::boolFalse: return "false";
            case osc::Value::Type::timeTag:   return std::to_string (value.getTimeTag().raw);

            /*  A blob is bytes, and the bytes are not a sentence: what a cell
                can honestly say about one is how much of it there is. */
            case osc::Value::Type::blob:      return std::to_string (value.getBlob().bytes.size()) + " bytes";

            case osc::Value::Type::nil:
            case osc::Value::Type::impulse:   return {};
        }

        return {};
    }

    std::string text (const tree::Node* node)
    {
        if (node == nullptr)
            return {};

        if (const auto sole = node->soleValue())
            return text (*sole);

        return {};
    }

    std::string text (const tree::TreeSnapshot& snapshot, std::string_view address)
    {
        return text (snapshot.find (address));
    }

    std::string shownCueName (const tree::TreeSnapshot& snapshot, std::string_view cueId)
    {
        const auto base = "/godot/cue/" + std::string (cueId) + "/";

        if (auto name = text (snapshot, base + "name"); ! name.empty())
            return name;

        return text (snapshot, base + "autoName");
    }

    Flag flag (const tree::TreeSnapshot& snapshot, std::string_view address)
    {
        const auto* node = snapshot.find (address);

        if (node == nullptr)
            return Flag::unsaid;

        const auto sole = node->soleValue();

        /*  A `T` ROW THAT IS NOT A BOOLEAN HAS NOT ANSWERED, rather than
            answering no: the engine publishes these as T or F and anything
            else means the node is not the one this caller thinks it is. */
        if (! sole.has_value() || ! sole->isBool())
            return Flag::unsaid;

        return sole->getBool() ? Flag::yes : Flag::no;
    }

    std::string speedText (double rate)
    {
        const auto rounded = std::round (rate * 1000.0) / 1000.0;

        if (! (rounded < 1.0) && ! (rounded > 1.0))
            return {};

        return "\xc3\x97" + osc::formatDouble (rounded);
    }

    std::string shownNumber (double value, std::string_view unit)
    {
        if (! std::isfinite (value))
            return osc::formatDouble (value);

        auto decimals = unit == "s" || unit == "x" ? 3 : 2;
        const auto size = std::abs (value);

        if (size > 0.0 && size < 1.0)
            decimals = std::max (decimals, 3 - static_cast<int> (std::floor (std::log10 (size))));

        decimals = std::min (decimals, 12);

        std::array<char, 64> buffer {};
        const auto written = std::to_chars (buffer.data(), buffer.data() + buffer.size(), value,
                                            std::chars_format::fixed, decimals);

        if (written.ec != std::errc {})
            return osc::formatDouble (value);

        std::string out (buffer.data(), written.ptr);

        if (out.find ('.') != std::string::npos)
        {
            while (! out.empty() && out.back() == '0')
                out.pop_back();

            if (! out.empty() && out.back() == '.')
                out.pop_back();
        }

        //  A tail rounded away from a small negative number is nought, not "-0".
        if (out == "-0")
            out = "0";

        return out;
    }

    std::vector<std::string> words (std::string_view line)
    {
        std::vector<std::string> result;
        std::size_t at = 0;

        while (at < line.size())
        {
            const auto start = line.find_first_not_of (' ', at);

            if (start == std::string_view::npos)
                break;

            const auto end = line.find (' ', start);
            const auto word = line.substr (start, end == std::string_view::npos ? std::string_view::npos
                                                                                : end - start);
            result.emplace_back (word);

            if (end == std::string_view::npos)
                break;

            at = end + 1;
        }

        return result;
    }
}
