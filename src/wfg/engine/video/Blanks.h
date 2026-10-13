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
    THE BLANKS IN A TEXT CUE'S WORDS (namespace draft 61, AKC, AKM): `{score}`
    is filled by the field of that name, which a message to
    `/godot/text/score` sets - one of the author's ways to make the words live.

    A NAME is one to thirty-two letters, digits, `-` and `_`. `{{` and `}}` are
    a brace written out. A `{` that does not begin a blank - no closing brace,
    or something that is not a name between - is shown as written, and the show
    warns of it; a lone `}` is shown as written. A blank no value has reached
    shows nothing.

    std only: the engine fills the blanks, the window lists them.
*/

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::video
{
    constexpr std::size_t fieldNameMost = 32;

    inline bool isFieldName (std::string_view name) noexcept
    {
        if (name.empty() || name.size() > fieldNameMost)
            return false;

        for (const auto c : name)
        {
            const auto letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            const auto digit = c >= '0' && c <= '9';

            if (! letter && ! digit && c != '-' && c != '_')
                return false;
        }

        return true;
    }

    /*  THE WORDS WALKED: `literal` for each run of text as shown, braces
        written out undoubled, `blank` for each name between braces, in order.
        `loose` is told of each `{` that begins no blank. */
    template <typename Literal, typename Blank, typename Loose>
    void walkBlanks (std::string_view words, Literal&& literal, Blank&& blank, Loose&& loose)
    {
        std::string run;
        std::size_t at = 0;

        const auto flush = [&run, &literal]
        {
            if (! run.empty())
            {
                literal (std::string_view (run));
                run.clear();
            }
        };

        while (at < words.size())
        {
            const auto c = words[at];

            if ((c == '{' || c == '}') && at + 1 < words.size() && words[at + 1] == c)
            {
                run += c;
                at += 2;
                continue;
            }

            if (c == '{')
            {
                const auto close = words.find ('}', at + 1);

                if (close != std::string_view::npos && isFieldName (words.substr (at + 1, close - at - 1)))
                {
                    flush();
                    blank (words.substr (at + 1, close - at - 1));
                    at = close + 1;
                    continue;
                }

                loose();
            }

            run += c;
            ++at;
        }

        flush();
    }

    /*  The names of the blanks in `words`, each once, in the order they first come. */
    inline std::vector<std::string> blanksIn (std::string_view words)
    {
        std::vector<std::string> names;

        walkBlanks (words, [] (std::string_view) {},
                    [&names] (std::string_view name)
                    {
                        for (const auto& known : names)
                            if (known == name)
                                return;

                        names.emplace_back (name);
                    },
                    [] {});

        return names;
    }

    /*  Whether `words` holds a `{` that begins no blank: shown as written, and warned. */
    inline bool hasLooseBrace (std::string_view words)
    {
        auto found = false;
        walkBlanks (words, [] (std::string_view) {}, [] (std::string_view) {}, [&found] { found = true; });
        return found;
    }

    /*  THE WORDS AS SHOWN: each blank filled by `valueOf (name)`, which answers
        nothing for a field no value has reached. */
    template <typename ValueOf>
    std::string fillBlanks (std::string_view words, ValueOf&& valueOf)
    {
        std::string out;
        out.reserve (words.size());
        walkBlanks (words, [&out] (std::string_view text) { out += text; },
                    [&out, &valueOf] (std::string_view name) { out += valueOf (std::string (name)); },
                    [] {});
        return out;
    }
}
