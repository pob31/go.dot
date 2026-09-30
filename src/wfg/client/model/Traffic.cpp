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

#include <wfg/client/model/Traffic.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/osc/OscCodec.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace wfg::client::model
{
    namespace
    {
        std::string roadWord (monitor::Road road)
        {
            switch (road)
            {
                case monitor::Road::udp:  return "udp";
                case monitor::Road::page: return "page";
                case monitor::Road::midi: return "midi";
            }

            return {};
        }

        /*  A VALUE AS THE MONITOR WRITES IT: the cell's own text, a string in
            quotes so an empty one and a space can be seen, and the two kinds
            that carry no value by their names. */
        std::string valueText (const osc::Value& value)
        {
            switch (value.type())
            {
                case osc::Value::Type::string:  return "\"" + value.getString() + "\"";
                case osc::Value::Type::nil:     return "nil";
                case osc::Value::Type::impulse: return "impulse";

                case osc::Value::Type::int32:
                case osc::Value::Type::int64:
                case osc::Value::Type::float32:
                case osc::Value::Type::float64:
                case osc::Value::Type::boolTrue:
                case osc::Value::Type::boolFalse:
                case osc::Value::Type::timeTag:
                case osc::Value::Type::blob:
                    break;
            }

            return text (value);
        }

        void addMessages (const osc::Packet& packet, const TrafficRow& shape, std::vector<TrafficRow>& out)
        {
            if (packet.isBundle())
            {
                for (const auto& element : packet.elements)
                    addMessages (element, shape, out);

                return;
            }

            auto row = shape;
            row.address = packet.address;
            row.types = osc::typeTagString (packet.args);

            for (const auto& value : packet.args)
                row.arguments += (row.arguments.empty() ? "" : "  ") + valueText (value);

            out.push_back (std::move (row));
        }

        /*  THE ADDRESS OF A PACKET THE CODEC REFUSED, when one can be read off
            the front of it: an OSC message begins with its address, a slash
            and printable characters up to a nought. */
        std::string addressOf (const std::uint8_t* bytes, std::size_t size)
        {
            if (size == 0 || bytes[0] != '/')
                return {};

            std::string out;

            for (std::size_t at = 0; at < size && bytes[at] != 0; ++at)
            {
                if (bytes[at] < 0x20 || bytes[at] > 0x7E)
                    return {};

                out += static_cast<char> (bytes[at]);
            }

            return out;
        }

        std::string lower (std::string text)
        {
            std::transform (text.begin(), text.end(), text.begin(),
                            [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
            return text;
        }
    }

    void describeMidi (const std::uint8_t* bytes, std::size_t size, TrafficRow& row)
    {
        row.address.clear();
        row.arguments.clear();

        if (size == 0)
        {
            row.problem = "an empty MIDI message";
            return;
        }

        const auto status = bytes[0];
        const auto at = [bytes, size] (std::size_t index) { return index < size ? static_cast<int> (bytes[index]) : -1; };
        const auto channel = "ch " + std::to_string ((status & 0x0F) + 1);
        const auto numbers = [&at, &channel] (std::initializer_list<std::size_t> indices)
        {
            auto said = channel;

            for (const auto index : indices)
                if (at (index) >= 0)
                    said += "  " + std::to_string (at (index));

            return said;
        };

        switch (status & 0xF0)
        {
            case 0x80: row.address = "note off";         row.arguments = numbers ({ 1, 2 }); return;
            case 0x90: row.address = at (2) == 0 ? "note off" : "note on";
                       row.arguments = numbers ({ 1, 2 }); return;
            case 0xA0: row.address = "key pressure";     row.arguments = numbers ({ 1, 2 }); return;
            case 0xB0: row.address = "control change";   row.arguments = numbers ({ 1, 2 }); return;
            case 0xC0: row.address = "program change";   row.arguments = numbers ({ 1 }); return;
            case 0xD0: row.address = "channel pressure"; row.arguments = numbers ({ 1 }); return;

            case 0xE0:
                row.address = "pitch bend";
                row.arguments = channel + (at (2) >= 0 ? "  " + std::to_string (((at (2) << 7) | at (1)) - 8192) : "");
                return;

            default:
                break;
        }

        //  The system messages, which carry no channel.
        switch (status)
        {
            case 0xF0:
            {
                row.address = "sysex";
                row.arguments = std::to_string (size) + " bytes";

                static constexpr char hex[] = "0123456789ABCDEF";
                std::string dump;

                for (std::size_t index = 0; index < std::min<std::size_t> (size, 16); ++index)
                {
                    dump += dump.empty() ? "" : " ";
                    dump += hex[bytes[index] >> 4];
                    dump += hex[bytes[index] & 0x0F];
                }

                row.arguments += "  " + dump + (size > 16 ? " ..." : "");
                return;
            }

            case 0xF8: row.address = "clock";          return;
            case 0xFA: row.address = "start";          return;
            case 0xFB: row.address = "continue";       return;
            case 0xFC: row.address = "stop";           return;
            case 0xFE: row.address = "active sensing"; return;
            case 0xFF: row.address = "reset";          return;
            default:   break;
        }

        row.address = "system";
        row.arguments = std::to_string (status);
    }

    std::vector<TrafficRow> describe (const monitor::Capture& capture)
    {
        TrafficRow shape;
        shape.wallMicros = capture.wallMicros;
        shape.incoming = capture.direction == monitor::Direction::in;
        shape.midi = capture.medium == monitor::Medium::midi;
        shape.road = roadWord (capture.road);
        shape.peer = std::string (capture.peerName());

        std::vector<TrafficRow> out;

        if (shape.midi)
        {
            describeMidi (capture.bytes, capture.size, shape);

            if (capture.truncated())
                shape.problem = "only the first " + std::to_string (capture.size) + " of "
                                  + std::to_string (capture.fullSize) + " bytes were kept";

            out.push_back (std::move (shape));
            return out;
        }

        /*  CUT SHORT BY THE MONITOR, NOT BY THE SENDER: said as such, and the
            address shown, because the codec would call the rest truncated and
            that would be a fault the wire never had. */
        if (capture.truncated())
        {
            shape.address = addressOf (capture.bytes, capture.size);
            shape.problem = "long packet: only the first " + std::to_string (capture.size) + " of "
                              + std::to_string (capture.fullSize) + " bytes were kept";
            out.push_back (std::move (shape));
            return out;
        }

        const auto decoded = osc::decode (capture.bytes, capture.size);

        if (! decoded.ok)
        {
            shape.address = addressOf (capture.bytes, capture.size);
            shape.problem = decoded.error.empty() ? decoded.reason : decoded.error;
            out.push_back (std::move (shape));
            return out;
        }

        addMessages (decoded.packet, shape, out);

        //  An empty bundle is still something that crossed the wire.
        if (out.empty())
        {
            shape.address = "(an empty bundle)";
            out.push_back (std::move (shape));
        }

        return out;
    }

    bool TrafficFilter::matches (const TrafficRow& row) const
    {
        if (row.incoming ? ! in : ! out)
            return false;

        if (row.midi ? ! midi : ! osc)
            return false;

        if (! page && row.road == "page")
            return false;

        if (text.empty())
            return true;

        const auto wanted = lower (text);

        for (const auto* field : { &row.address, &row.arguments, &row.peer, &row.problem })
            if (lower (*field).find (wanted) != std::string::npos)
                return true;

        return false;
    }

    std::string csvOf (const std::vector<TrafficRow>& rows,
                       const std::function<std::string (std::int64_t)>& timeText)
    {
        const auto field = [] (const std::string& value)
        {
            if (value.find_first_of (",\"\n\r") == std::string::npos)
                return value;

            std::string quoted = "\"";

            for (const auto c : value)
            {
                if (c == '"')
                    quoted += '"';

                quoted += c;
            }

            return quoted + "\"";
        };

        std::string out = "time,direction,medium,road,peer,address,types,arguments,problem\n";

        for (const auto& row : rows)
        {
            out += field (timeText ? timeText (row.wallMicros) : std::to_string (row.wallMicros)) + ","
                 + (row.incoming ? "in" : "out") + ","
                 + (row.midi ? "midi" : "osc") + ","
                 + field (row.road) + ","
                 + field (row.peer) + ","
                 + field (row.address) + ","
                 + field (row.types) + ","
                 + field (row.arguments) + ","
                 + field (row.problem) + "\n";
        }

        return out;
    }
}
