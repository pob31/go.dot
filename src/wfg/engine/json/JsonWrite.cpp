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

#include <wfg/engine/json/JsonWrite.h>

#include <wfg/engine/osc/OscValue.h>

#include <cmath>
#include <utility>

namespace wfg::json
{
    namespace
    {
        void writeInto (std::string& out, const Value& value)
        {
            switch (value.type())
            {
                case Value::Type::null:
                    out += "null";
                    return;

                case Value::Type::boolean:
                    out += value.asBool() ? "true" : "false";
                    return;

                case Value::Type::number:
                    out += std::isfinite (value.asNumber()) ? osc::formatDouble (value.asNumber()) : std::string ("null");
                    return;

                case Value::Type::string:
                    out += quote (value.asString());
                    return;

                case Value::Type::array:
                {
                    out += '[';
                    bool first = true;

                    for (const auto& each : value.asArray())
                    {
                        if (! std::exchange (first, false))
                            out += ',';

                        writeInto (out, each);
                    }

                    out += ']';
                    return;
                }

                case Value::Type::object:
                {
                    out += '{';
                    bool first = true;

                    for (const auto& [key, each] : value.asObject())
                    {
                        if (! std::exchange (first, false))
                            out += ',';

                        out += quote (key);
                        out += ':';
                        writeInto (out, each);
                    }

                    out += '}';
                    return;
                }
            }
        }
    }

    std::string write (const Value& value)
    {
        std::string out;
        writeInto (out, value);
        return out;
    }

    std::string quote (std::string_view text)
    {
        static constexpr const char* hex = "0123456789abcdef";
        std::string out;
        out.reserve (text.size() + 2);
        out += '"';

        for (const auto c : text)
        {
            switch (c)
            {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                {
                    const auto byte = static_cast<unsigned char> (c);

                    //  The rest of what RFC 8259 forbids unescaped; UTF-8 passes as it is.
                    if (byte < 0x20)
                    {
                        out += "\\u00";
                        out += hex[byte >> 4];
                        out += hex[byte & 0x0F];
                    }
                    else
                    {
                        out += c;
                    }
                }
            }
        }

        out += '"';
        return out;
    }

    std::string compact (std::string_view text)
    {
        std::string out;
        out.reserve (text.size());
        bool inString = false;
        bool escaped = false;

        for (const auto c : text)
        {
            if (inString)
            {
                out += c;

                if (escaped)
                    escaped = false;
                else if (c == '\\')
                    escaped = true;
                else if (c == '"')
                    inString = false;

                continue;
            }

            if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
                continue;

            if (c == '"')
                inString = true;

            out += c;
        }

        return out;
    }
}
