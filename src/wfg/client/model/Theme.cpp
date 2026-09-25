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
                    and do not look like them (author, 2026-09-18: "the header
                    and footer sections can have a slightly different shade").
                    COOLER rather than darker, so the difference reads as a
                    different KIND of row and not as another level of nesting.

                    IT WAS A HAIR COOLER AND THAT WAS NOT A SHADE, it was
                    nothing: at #1B1E26 against a #1D1D22 panel the bands
                    vanished, and the author's next word on them was "I can't
                    see header and footers anymore". A difference nobody can
                    see is not a subtle difference. Then it was lighter, and
                    the author asked for darker, and then for BLACK (2026-09-18:
                    "can you make them black instead?"): a section is a recess,
                    not a highlight, and the deepest recess there is says so
                    without a hue at all. */
                /*  THE ROWS OF THE LIST ITSELF ARE BLACK and a section is the
                    dark blue-grey - the author's final word after we had it
                    the other way round (2026-09-18: "the normal cues should be
                    black and header and footer dark grey, blueish tint"). Black
                    is where the eye rests; a section is the thing that differs. */
                { "panel-cue",     0xFF000000 },
                { "panel-section", 0xFF10141C },

                /*  THE ROWS INSIDE A SECTION ARE NOT ITS BAND (author,
                    2026-09-18: "invert the colour of the footers, headers
                    (black) and the colour of the cues (greyish blue)"): the
                    band is the frame and the rows are what it holds, and one
                    tone for both had made a section a black block. The rows
                    take the greyish blue the sections wore for an hour. */
                { "panel-section-cue", 0xFF10141C },

                /*  THE RUNNING PANE IS BLACK (author, 2026-09-18: "can you also
                    make the running cue backgrounds black?"), which is also
                    where a waveform's colours have most to stand against. One
                    tone for every row: the stripe the cue list lost is gone
                    from here too. */
                { "panel-runs",    0xFF000000 },
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

                /*  WHAT LETTING GO WOULD DO, one colour each (author,
                    2026-09-21: "so the drag and drop has a clear colour coding
                    for the user to be sure what they're doing").

                    Four gestures land ON a row rather than between two, and
                    until now all four lit it the same green: into a group,
                    aimed at a fade, marked as prepared by a group's header, and
                    moved into a footer. They do very different things to a
                    show, and the moment of choosing between them is the moment
                    the hand is already moving - so each gets a tone, and the
                    sentence under the list still says it in words (§4.8).

                    Their own tokens rather than a reuse of `live`, `waiting`,
                    `failed` and `picked`, although they start at those values:
                    somebody retheming what a running cue looks like should not
                    find their drops had moved with it. */
                { "drop-into",     0xFF4BC38A },   // green, as it has always been
                { "drop-aim",      0xFF86A3DB },   // blue: this fade would point here
                { "drop-header",   0xFFE0685D },   // red: prepared by this group's header
                { "drop-footer",   0xFF9A95E4 },   // purple: into this group's footer

                /*  GO SAYS WHETHER THE SOUND WILL LEAVE (author, 2026-09-25):
                    bright yellow with black letters while the audio runs, grey
                    while it does not - and a word under it then (§4.8). Their
                    own tokens and not `standby`'s amber: the author asked for
                    a brighter yellow than the cue it fires. */
                { "go",            0xFFFFD60A },
                { "go-ink",        0xFF000000 },
                { "go-idle",       0xFF4A4A52 },

                /*  AN EQ HANDLE EACH (author, 2026-09-25: "Having different
                    colours on each handle like on the EQ of the spatcore
                    library really helps"): spatcore's own first six, red to
                    purple along the field as the handles stand by default -
                    the high-pass, the four bands, the low-pass. */
                { "eq-hp",         0xFFE74C3C },
                { "eq-1",          0xFFE67E22 },
                { "eq-2",          0xFFFFEB3B },
                { "eq-3",          0xFF2ECC71 },
                { "eq-4",          0xFF3498DB },
                { "eq-lp",         0xFF9B59B6 },
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
