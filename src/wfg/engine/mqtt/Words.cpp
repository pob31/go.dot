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

#include <wfg/engine/mqtt/Words.h>

#include <wfg/engine/mqtt/Packet.h>
#include <wfg/engine/osc/OscAddress.h>
#include <wfg/engine/serial/SerialTable.h>
#include <wfg/engine/tree/Mount.h>

namespace wfg::mqtt
{
    std::optional<std::string> addressOfTopic (const std::string& prefixRow, std::string_view topic)
    {
        const auto roots = tree::prefixesOf (prefixRow);
        if (roots.empty() || topic.empty())
            return std::nullopt;

        std::string address = roots.front();
        if (address.empty() || address.back() != '/')
            address.push_back ('/');
        address.append (topic.data(), topic.size());

        if (! osc::isValidAddress (address))
            return std::nullopt;
        return address;
    }

    std::optional<std::string> topicOfAddress (const std::string& prefixRow, const std::string& address)
    {
        const auto covered = tree::prefixMatchLength (address, prefixRow);
        if (covered == 0)
            return std::nullopt;

        auto topic = address.substr (covered);
        if (! topic.empty() && topic.front() == '/')
            topic.erase (0, 1);
        if (! validTopic (topic))
            return std::nullopt;
        return topic;
    }

    bool isText (std::string_view bytes) noexcept
    {
        std::size_t at = 0;
        const auto size = bytes.size();

        while (at < size)
        {
            const auto byte = static_cast<unsigned char> (bytes[at]);

            if (byte < 0x80)
            {
                if (byte < 0x20 && byte != '\t' && byte != '\n' && byte != '\r')
                    return false;
                if (byte == 0x7F)
                    return false;
                ++at;
                continue;
            }

            std::size_t more = 0;
            if ((byte & 0xE0) == 0xC0 && byte >= 0xC2)      more = 1;
            else if ((byte & 0xF0) == 0xE0)                 more = 2;
            else if ((byte & 0xF8) == 0xF0 && byte <= 0xF4) more = 3;
            else
                return false;

            if (at + more >= size)
                return false;

            for (std::size_t k = 1; k <= more; ++k)
                if ((static_cast<unsigned char> (bytes[at + k]) & 0xC0) != 0x80)
                    return false;

            at += more + 1;
        }

        return true;
    }

    osc::Values valuesOfPayload (const std::string& payload)
    {
        osc::Values values;

        if (payload.empty())
            return values;

        if (! isText (payload))
        {
            values.push_back (osc::Value::blob (osc::Blob { std::vector<std::uint8_t> (payload.begin(), payload.end()) }));
            return values;
        }

        /*  JSON, or anything that looks like it, is one string for a script
            to parse (ANC): split on its commas it would be nothing. */
        if (payload.front() == '{' || payload.front() == '[')
        {
            values.push_back (osc::Value::string (payload));
            return values;
        }

        for (const auto& word : serial::wordsOfLine (payload))
            values.push_back (word.isNumber ? osc::Value::float64 (word.number) : osc::Value::string (word.text));

        return values;
    }

    std::string payloadOfValues (const osc::Values& values)
    {
        if (values.size() == 1 && values.front().isString())
            return values.front().getString();

        std::string out;

        for (const auto& value : values)
        {
            std::string word;

            if (value.isNumber())
                word = value.isFloat32() ? osc::formatFloat (value.getFloat32()) : osc::formatDouble (value.asDouble());
            else if (value.isString())
                word = value.getString();
            else if (value.isBool())
                word = value.getBool() ? "true" : "false";
            else if (value.isBlob())
                word.assign (value.getBlob().bytes.begin(), value.getBlob().bytes.end());
            else
                continue;        // nil, impulse, a time tag: nothing to say

            if (! out.empty())
                out.push_back (' ');
            out += word;
        }

        return out;
    }
}
