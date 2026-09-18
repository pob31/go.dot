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
#include <cstring>
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
        /*  The page's palette, in styles.css's order - and no longer with all
            of its values, which is a divergence worth naming.

            THREE GREYS ARE LIGHTER HERE THAN ON THE PAGE, and the inspector's
            ground is blue rather than violet, because the author read both
            surfaces and asked (2026-09-18): "make the grey a little lighter in
            general for more contrast with the background. Details is
            especially hard to read", and "make the background of the inspector
            a dark blue of the same density to tell it apart from the cuelist
            and active cues".

            IT IS NOT A DISAGREEMENT ABOUT THE PALETTE. A browser and JUCE do
            not lay type down the same way - the page's greys are rendered with
            subpixel antialiasing over a stylesheet's own gamma, and the same
            hex here reads thinner and darker - so keeping the numbers
            identical would have kept the LOOK identical only on paper. What
            both surfaces share is the intention; the numbers serve it
            separately. `ink-off` moved furthest because it carries the details
            fold, which is where it was least readable. */
        const std::vector<std::pair<std::string, std::uint32_t>>& palette()
        {
            static const std::vector<std::pair<std::string, std::uint32_t>> table
            {
                { "ink",           0xFFE8E6E1 },
                { "ink-dim",       0xFFC6C1B9 },
                { "ink-faint",     0xFFA9A49C },
                { "ink-off",       0xFF8B867F },
                { "ground",        0xFF16161A },
                { "panel",         0xFF1D1D22 },
                { "panel-inspect", 0xFF1C2433 },
                { "panel-high",    0xFF24242B },
                { "panel-in",      0xFF191920 },

                /*  A HEADER, A FOOTER AND A PERSISTENT SECTION ARE NOT MEMBERS
                    and now do not look like them (author, 2026-09-18: "the
                    header and footer sections can have a slightly different
                    shade"). Cooler than `panel-in` by a hair rather than
                    darker, so the difference reads as a different KIND of row
                    and not as another level of nesting. */
                { "panel-section", 0xFF1B1E26 },
                { "rule",          0xFF3A3A45 },
                { "standby",       0xFFE8B04B },
                { "live",          0xFF4BC38A },
                { "stopping",      0xFFD98B4B },
                { "failed",        0xFFE0685D },
                { "waiting",       0xFF86A3DB },

                /*  A FADE IS NOT A SOUND AND IS NOT A WAIT (author,
                    2026-09-18: "fades another colour, maybe over a shaded
                    background"). It is the one cue kind that changes something
                    already sounding rather than starting or stopping anything,
                    and a colour of its own is what lets a glance sort the
                    three bars in the running pane. */
                { "fade",          0xFF5FB8C9 },
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

            const auto argb = parseColour (value.asString());   // not `colour`, which is the member function's name

            if (! argb.has_value())
                return "theme: '" + key + "' is not a colour: " + value.asString();

            next.colours[key] = *argb;
        }

        *this = std::move (next);
        return {};
    }

    bool Theme::operator== (const Theme& other) const noexcept
    {
        /*  BIT FOR BIT, as OscValue compares its floats: "the same theme" means
            the same tokens, and a number that read back from the file the author
            wrote is the same bits, not nearly the same. It is also what keeps
            -Wfloat-equal quiet without pretending there is a tolerance. */
        const auto sameBits = [] (double a, double b) noexcept
        {
            std::uint64_t x = 0, y = 0;
            std::memcpy (&x, &a, sizeof x);
            std::memcpy (&y, &b, sizeof y);
            return x == y;
        };

        return sameBits (type, other.type) && sameBits (refreshHz, other.refreshHz)
            && sameBits (row, other.row) && colours == other.colours;
    }
}
