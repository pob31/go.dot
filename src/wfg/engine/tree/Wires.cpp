// This file is part of Go.dot — https://github.com/pob31/go.dot
//
// Copyright (C) 2026 Pierre-Olivier Boulant
//
// Go.dot is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. Go.dot is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
// (LICENSE, at the repository root) for more details.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <wfg/engine/tree/Wires.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace wfg::tree::wire
{
    namespace
    {
        std::vector<std::string> segmentsOf (const std::string& address)
        {
            std::vector<std::string> out;
            std::string current;

            for (const char c : address)
            {
                if (c == '/')
                {
                    if (! current.empty())
                        out.push_back (current);
                    current.clear();
                }
                else
                {
                    current.push_back (c);
                }
            }

            if (! current.empty())
                out.push_back (current);

            return out;
        }

        bool wholeNumber (const std::string& text)
        {
            return ! text.empty() && std::all_of (text.begin(), text.end(), [] (unsigned char c) { return std::isdigit (c) != 0; });
        }

        bool integerText (const std::string& text)
        {
            if (text.empty())
                return false;

            const auto start = text[0] == '-' ? 1u : 0u;
            return text.size() > start && std::all_of (text.begin() + static_cast<std::ptrdiff_t> (start), text.end(),
                                                       [] (unsigned char c) { return std::isdigit (c) != 0; });
        }

        std::string quoted (const std::string& text)
        {
            std::string out = "\"";

            for (const char c : text)
            {
                if (c == '"' || c == '\\')
                    out.push_back ('\\');
                out.push_back (c);
            }

            out.push_back ('"');
            return out;
        }

        /*  One atom as RCP spells it: integers as they are, a float rounded
            (the protocol takes integers and a curve's move arrives as a
            float), a bool as 1 or 0, a string quoted. Anything else - a blob,
            nil, an impulse - has no spelling and is left out. */
        std::string atom (const osc::Value& value)
        {
            if (value.isInt32())   return std::to_string (value.getInt32());
            if (value.isInt64())   return std::to_string (value.getInt64());
            if (value.isFloat32()) return std::to_string (std::lround (value.getFloat32()));
            if (value.isFloat64()) return std::to_string (std::lround (value.getFloat64()));
            if (value.isBool())    return value.getBool() ? "1" : "0";
            if (value.isString())  return quoted (value.getString());
            return {};
        }

        bool indexedVerb (const std::string& verb)
        {
            return verb == "set" || verb == "get";
        }

        /*  The line's tokens: split on blanks, a quoted run one token with
            its quotes taken off and its escapes undone. `quotedToken` says
            which were quoted, so "12" stays a string and 12 an integer. */
        std::vector<std::pair<std::string, bool>> tokensOf (const std::string& line)
        {
            std::vector<std::pair<std::string, bool>> out;
            std::string current;
            auto inQuotes = false;
            auto wasQuoted = false;
            auto escaped = false;

            const auto close = [&]
            {
                if (! current.empty() || wasQuoted)
                    out.emplace_back (current, wasQuoted);
                current.clear();
                wasQuoted = false;
            };

            for (const char c : line)
            {
                if (inQuotes)
                {
                    if (escaped)
                    {
                        current.push_back (c);
                        escaped = false;
                    }
                    else if (c == '\\')
                    {
                        escaped = true;
                    }
                    else if (c == '"')
                    {
                        inQuotes = false;
                    }
                    else
                    {
                        current.push_back (c);
                    }
                }
                else if (c == '"')
                {
                    inQuotes = true;
                    wasQuoted = true;
                }
                else if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                {
                    close();
                }
                else
                {
                    current.push_back (c);
                }
            }

            close();
            return out;
        }

        osc::Value valueOf (const std::pair<std::string, bool>& token)
        {
            if (! token.second && integerText (token.first))
                return osc::Value::int32 (static_cast<std::int32_t> (std::atol (token.first.c_str())));

            return osc::Value::string (token.first);
        }
    }

    std::string renderRcp (const std::string& address, const osc::Values& values, const RcpSpec& spec)
    {
        const auto segments = segmentsOf (address);
        const auto verb = spec.verb.empty() ? std::string ("set") : spec.verb;
        const auto indexed = indexedVerb (verb);

        auto indexes = spec.indexes;

        if (indexes < 0)
        {
            indexes = 0;

            for (auto at = segments.size(); at > 0 && indexes < 2 && wholeNumber (segments[at - 1]); --at)
                ++indexes;
        }

        if (! indexed)
            indexes = 0;

        indexes = std::min (indexes, static_cast<int> (segments.size()));
        const auto parameterCount = segments.size() - static_cast<std::size_t> (indexes);

        std::string out = verb;
        out += ' ';

        for (std::size_t at = 0; at < parameterCount; ++at)
        {
            if (at > 0)
                out += '/';
            out += segments[at];
        }

        if (indexed)
        {
            auto x = 0, y = 0;

            if (indexes >= 1)
                x = std::max (0, std::atoi (segments[parameterCount].c_str()) - 1);
            if (indexes >= 2)
                y = std::max (0, std::atoi (segments[parameterCount + 1].c_str()) - 1);

            out += ' ' + std::to_string (x) + ' ' + std::to_string (y);
        }

        for (const auto& value : values)
            if (const auto text = atom (value); ! text.empty())
                out += ' ' + text;

        return out;
    }

    std::optional<RcpLine> parseRcpLine (const std::string& line)
    {
        const auto tokens = tokensOf (line);

        if (tokens.empty())
            return std::nullopt;

        RcpLine out;
        out.text = line;

        while (! out.text.empty() && (out.text.back() == '\r' || out.text.back() == '\n'))
            out.text.pop_back();

        out.word = tokens[0].first;

        if (tokens.size() < 2)
            return out;

        /*  OK, OKm and NOTIFY echo a request: the verb, the parameter, and
            for a set or a get the two indexes before the value. ERROR says
            the verb and a word. Anything else is kept whole as values. */
        if (out.word == "OK" || out.word == "OKm" || out.word == "NOTIFY" || out.word == "ERROR")
        {
            out.verb = tokens[1].first;
            std::size_t next = 2;

            if (tokens.size() > 2)
            {
                out.parameter = tokens[2].first;
                next = 3;
            }

            if (indexedVerb (out.verb) && tokens.size() >= 5 && integerText (tokens[3].first) && integerText (tokens[4].first))
            {
                out.x = std::atoi (tokens[3].first.c_str());
                out.y = std::atoi (tokens[4].first.c_str());
                out.indexed = true;
                next = 5;
            }

            for (auto at = next; at < tokens.size(); ++at)
                out.values.push_back (valueOf (tokens[at]));

            return out;
        }

        for (std::size_t at = 1; at < tokens.size(); ++at)
            out.values.push_back (valueOf (tokens[at]));

        return out;
    }

    std::vector<std::string> rcpAddressesOf (const RcpLine& line)
    {
        if (line.parameter.empty())
            return {};

        const auto bare = "/" + line.parameter;

        if (! line.indexed)
            return { bare };

        const auto x = std::to_string (line.x + 1);
        const auto y = std::to_string (line.y + 1);
        return { bare + "/" + x + "/" + y, bare + "/" + x, bare };
    }

    namespace
    {
        /*  One atom as a command line spells it: a number plainly, a float
            with its fraction only where it has one, a string as it is. */
        std::string lineAtom (const osc::Value& value)
        {
            if (value.isInt32())   return std::to_string (value.getInt32());
            if (value.isInt64())   return std::to_string (value.getInt64());
            if (value.isBool())    return value.getBool() ? "1" : "0";
            if (value.isString())  return value.getString();

            if (value.isFloat32() || value.isFloat64())
            {
                const auto number = value.isFloat32() ? static_cast<double> (value.getFloat32()) : value.getFloat64();
                const auto whole = std::lround (number);

                if (std::abs (number - static_cast<double> (whole)) < 1e-6)
                    return std::to_string (whole);

                /*  THE SHORTEST SPELLING THAT READS BACK, and with a dot
                    whatever the locale says: to_string would write "12,5" under
                    fr_FR, which no console reads as a number. */
                return value.isFloat32() ? osc::formatFloat (value.getFloat32()) : osc::formatDouble (number);
            }

            return {};
        }
    }

    std::string renderLine (const std::string& address, const osc::Values& values, const std::string& templateText)
    {
        if (templateText.empty())
        {
            std::string out;

            for (const auto& value : values)
            {
                const auto text = lineAtom (value);

                if (text.empty())
                    continue;

                if (! out.empty())
                    out += ' ';

                out += text;
            }

            return out;
        }

        std::vector<std::string> numbers;

        for (const auto& segment : segmentsOf (address))
            if (wholeNumber (segment) && numbers.size() < 2)
                numbers.push_back (segment);

        std::string out;

        for (std::size_t at = 0; at < templateText.size(); ++at)
        {
            const auto c = templateText[at];

            if (c == '{' && at + 2 < templateText.size() && templateText[at + 2] == '}')
            {
                const auto key = templateText[at + 1];

                if (key == 'x' || key == 'y')
                {
                    const auto index = key == 'x' ? 0u : 1u;

                    if (index < numbers.size())
                        out += numbers[index];

                    at += 2;
                    continue;
                }

                if (key >= '1' && key <= '9')
                {
                    const auto index = static_cast<std::size_t> (key - '1');

                    if (index < values.size())
                        out += lineAtom (values[index]);

                    at += 2;
                    continue;
                }
            }

            out.push_back (c);
        }

        return out;
    }

    std::string printableLine (const std::string& line)
    {
        std::string out;
        auto escape = 0;        // 1: after ESC; 2: inside ESC [ ... until its final byte

        for (const char c : line)
        {
            const auto byte = static_cast<unsigned char> (c);

            /*  AN ANSI ESCAPE SEQUENCE - a colour, a cleared screen, what a
                telnet console dresses its prompt in - goes whole: ESC, an
                optional bracket, then everything up to a final byte in the
                range the standard gives it. */
            if (escape == 1)
            {
                escape = c == '[' ? 2 : 0;
                continue;
            }

            if (escape == 2)
            {
                if (byte >= 0x40 && byte <= 0x7E)
                    escape = 0;
                continue;
            }

            if (byte == 0x1B)
            {
                escape = 1;
                continue;
            }

            if ((byte >= 0x20 && byte < 0x7F) || c == '\t')
                out.push_back (c);
        }

        return out;
    }
}
