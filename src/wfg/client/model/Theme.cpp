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

#include <wfg/client/model/Theme.h>

#include <wfg/engine/json/JsonValue.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wfg::client::model
{
    namespace
    {
        /*  The page's palette, in styles.css's order and with its values, so
            that the first thing the author sees in the window is the thing
            they already judged on the page - and every difference from then
            on is one they made. */
        const std::vector<std::pair<std::string, std::uint32_t>>& palette()
        {
            static const std::vector<std::pair<std::string, std::uint32_t>> table
            {
                { "ink",           0xFFE8E6E1 },
                { "ink-dim",       0xFFB4AFA7 },
                { "ink-faint",     0xFF8F8A83 },
                { "ink-off",       0xFF625F5B },
                { "ground",        0xFF16161A },
                { "panel",         0xFF1D1D22 },
                { "panel-inspect", 0xFF21212A },
                { "panel-high",    0xFF24242B },
                { "panel-in",      0xFF191920 },
                { "rule",          0xFF3A3A45 },
                { "standby",       0xFFE8B04B },
                { "live",          0xFF4BC38A },
                { "stopping",      0xFFD98B4B },
                { "failed",        0xFFE0685D },
                { "waiting",       0xFF86A3DB },
                { "picked",        0xFF9A95E4 },
            };

            return table;
        }

        constexpr std::uint32_t undeclared = 0xFFFF00FF;

        std::optional<int> hexDigit (char c) noexcept
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            return std::nullopt;
        }

        /*  "#rrggbb" or "#rrggbbaa", the CSS spellings the page uses, and
            nothing else: no names, no rgb(), no three-digit shorthand. One
            spelling means one parser and one kind of mistake. */
        std::optional<std::uint32_t> parseColour (std::string_view text)
        {
            if (text.size() != 7 && text.size() != 9)
                return std::nullopt;

            if (text.front() != '#')
                return std::nullopt;

            std::uint32_t rgba = 0;

            for (std::size_t i = 1; i < text.size(); ++i)
            {
                const auto digit = hexDigit (text[i]);

                if (! digit.has_value())
                    return std::nullopt;

                rgba = (rgba << 4) | static_cast<std::uint32_t> (*digit);
            }

            if (text.size() == 7)
                return 0xFF000000u | rgba;

            // rrggbbaa on the wire, aarrggbb in memory
            return ((rgba & 0xFFu) << 24) | (rgba >> 8);
        }
    }

    Theme::Theme()
    {
        for (const auto& [name, value] : palette())
            colours.emplace (name, value);
    }

    const std::vector<std::string>& Theme::colourNames()
    {
        static const std::vector<std::string> names = []
        {
            std::vector<std::string> result;

            for (const auto& entry : palette())
                result.push_back (entry.first);

            return result;
        }();

        return names;
    }

    std::uint32_t Theme::colour (std::string_view name) const
    {
        const auto found = colours.find (std::string (name));
        return found != colours.end() ? found->second : undeclared;
    }

    std::string Theme::apply (std::string_view jsonText)
    {
        const auto parsed = json::parse (jsonText);

        if (! parsed.ok())
            return "theme: " + parsed.error + " (line " + std::to_string (parsed.line) + ")";

        if (! parsed.value->isObject())
            return "theme: the file is not a JSON object of tokens";

        /*  INTO A COPY, so that a refusal on the ninth token leaves the eight
            before it unapplied as well. */
        Theme next = *this;

        for (const auto& [key, value] : parsed.value->asObject())
        {
            if (key == "about")
                continue;   // the slot for a sentence, as commands.json has one

            if (key == "type" || key == "refreshHz" || key == "row")
            {
                if (! value.isNumber() || ! (value.asNumber() > 0.0))
                    return "theme: '" + key + "' wants a number above zero";

                if (key == "type")           next.type = value.asNumber();
                else if (key == "refreshHz") next.refreshHz = value.asNumber();
                else                         next.row = value.asNumber();

                continue;
            }

            if (next.colours.count (key) == 0)
                return "theme: no token named '" + key + "'";

            if (! value.isString())
                return "theme: '" + key + "' wants a colour written like \"#e8b04b\"";

            const auto colour = parseColour (value.asString());

            if (! colour.has_value())
                return "theme: '" + key + "' is not a colour: " + value.asString();

            next.colours[key] = *colour;
        }

        *this = std::move (next);
        return {};
    }

    bool Theme::operator== (const Theme& other) const noexcept
    {
        return type == other.type && refreshHz == other.refreshHz && row == other.row
            && colours == other.colours;
    }
}
