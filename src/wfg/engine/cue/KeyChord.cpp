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

#include <wfg/engine/cue/KeyChord.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <vector>

namespace wfg::cue
{
    namespace
    {
        std::string lower (std::string_view text)
        {
            std::string out (text);

            for (auto& c : out)
                c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));

            return out;
        }

        /*  The named keys, in their spelling. A letter, a digit and a function
            key are made, not listed. */
        constexpr std::array<std::string_view, 15> namedKeys {
            "ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown", "Enter", "Tab", "Space", "Escape",
            "Delete", "Backspace", "Home", "End", "PageUp", "PageDown", "Insert"
        };

        /*  THE KEY'S OWN NAME, spelled as this file spells it, or empty. */
        std::optional<std::string> keyName (std::string_view text)
        {
            if (text.size() == 1)
            {
                const auto c = static_cast<unsigned char> (text.front());

                if (std::isalpha (c) != 0)
                    return std::string (1, static_cast<char> (std::toupper (c)));

                if (std::isdigit (c) != 0)
                    return std::string (text);

                return std::nullopt;
            }

            const auto folded = lower (text);

            for (const auto named : namedKeys)
                if (folded == lower (named))
                    return std::string (named);

            //  F1 to F24.
            if (folded.size() >= 2 && folded.size() <= 3 && folded.front() == 'f'
                  && std::all_of (folded.begin() + 1, folded.end(), [] (char c) { return std::isdigit (static_cast<unsigned char> (c)) != 0; }))
            {
                const auto number = std::stoi (folded.substr (1));

                if (number >= 1 && number <= 24)
                    return "F" + std::to_string (number);
            }

            //  Num0 to Num9, the number pad.
            if (folded.size() == 4 && folded.rfind ("num", 0) == 0
                  && std::isdigit (static_cast<unsigned char> (folded[3])) != 0)
                return "Num" + folded.substr (3);

            return std::nullopt;
        }

        /*  EVERY SHORTCUT EITHER CLIENT ANSWERS TO, besides the three the show
            keeps whatever the modifiers (`isReservedChord`). Read off the
            window's menu table (`Client.cpp`, `keyFor`) and its panes, and the
            page's key table and `gestures/keys.js`: a trigger on one of these
            would be a key that does two things, or nothing at all. */
        constexpr std::array<std::string_view, 29> clientShortcuts {
            "F5", "F", "Shift+F", "ArrowUp", "ArrowDown", "Mod+ArrowUp", "Mod+ArrowDown",
            "Enter", "Tab", "Shift+Tab", "Delete", "Backspace", "Mod+Backspace",
            "Mod+A", "Mod+C", "Mod+L", "Mod+N", "Mod+O", "Mod+S", "Mod+Shift+S", "Mod+T",
            "Mod+V", "Mod+W", "Mod+X", "Mod+Y", "Mod+Z", "Mod+Shift+Z", "Mod+Shift+U", "Mod+Shift+R"
        };
    }

    std::optional<std::string> chordOf (bool mod, bool alt, bool shift, std::string_view key)
    {
        const auto name = keyName (key);

        if (! name.has_value())
            return std::nullopt;

        std::string out;

        if (mod)   out += "Mod+";
        if (alt)   out += "Alt+";
        if (shift) out += "Shift+";

        return out + *name;
    }

    std::optional<std::string> canonicalChord (std::string_view text)
    {
        if (text.empty())
            return std::nullopt;

        std::vector<std::string_view> parts;
        std::size_t from = 0;

        while (from <= text.size())
        {
            const auto plus = text.find ('+', from);
            const auto part = text.substr (from, plus == std::string_view::npos ? std::string_view::npos : plus - from);

            if (part.empty())
                return std::nullopt;

            parts.push_back (part);

            if (plus == std::string_view::npos)
                break;

            from = plus + 1;
        }

        bool mod = false, alt = false, shift = false;

        for (std::size_t at = 0; at + 1 < parts.size(); ++at)
        {
            const auto word = lower (parts[at]);

            if (word == "mod" || word == "ctrl" || word == "control" || word == "cmd" || word == "command")
                mod = true;
            else if (word == "alt" || word == "option")
                alt = true;
            else if (word == "shift")
                shift = true;
            else
                return std::nullopt;
        }

        return chordOf (mod, alt, shift, parts.back());
    }

    bool isReservedChord (std::string_view canonical)
    {
        /*  GO, ESC AND DOH! WHATEVER THE MODIFIERS (AQK): the last part names
            the key. */
        const auto plus = canonical.rfind ('+');
        const auto key = plus == std::string_view::npos ? canonical : canonical.substr (plus + 1);

        if (key == "Space" || key == "Escape" || key == "F9")
            return true;

        return std::find (clientShortcuts.begin(), clientShortcuts.end(), canonical) != clientShortcuts.end();
    }
}
