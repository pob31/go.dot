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

#include <wfg/engine/tree/MidiEchoes.h>

#include <algorithm>

namespace wfg::tree
{
    namespace
    {
        int channelOf (const MidiShape& shape, int baseChannel)
        {
            return std::clamp (shape.channel > 0 ? shape.channel : baseChannel + shape.offset, 1, 16);
        }

        /*  The value a shape's node reports from the data a console sent: a
            switch where the shape has two codes, the number otherwise. */
        osc::Values valueFor (const MidiShape& shape, int data)
        {
            if (shape.hasOnOff)
            {
                if (data == shape.on)
                    return { osc::Value::boolean (true) };

                if (data == shape.off)
                    return { osc::Value::boolean (false) };

                //  A code that is neither - a mute at 7E - reads by its side of the two.
                return { osc::Value::boolean (std::abs (data - shape.on) < std::abs (data - shape.off)) };
            }

            return { osc::Value::int32 (data) };
        }
    }

    void MidiEchoes::learn (const std::string& mountId, const std::vector<const Node*>& nodes, int baseChannel)
    {
        std::map<Key, Target> targets;

        for (const auto* node : nodes)
        {
            if (node == nullptr)
                continue;

            const auto& shape = node->midi;
            const auto channel = channelOf (shape, baseChannel);

            if (shape.kind == "note")
                targets[{ channel, 'n', shape.note, 0 }] = { node->address, shape };
            else if (shape.kind == "cc")
                targets[{ channel, 'c', shape.cc, 0 }] = { node->address, shape };
            else if (shape.kind == "nrpn")
                targets[{ channel, 'p', shape.msb, shape.lsb }] = { node->address, shape };
        }

        const std::lock_guard<std::mutex> held (guard);
        learned[mountId] = std::move (targets);
        parsers[mountId] = Parser {};
    }

    void MidiEchoes::forget (const std::string& mountId)
    {
        const std::lock_guard<std::mutex> held (guard);
        learned.erase (mountId);
        parsers.erase (mountId);
    }

    std::vector<MidiEchoes::Heard> MidiEchoes::feed (const std::string& mountId, const std::vector<std::uint8_t>& bytes)
    {
        std::vector<Heard> out;
        const std::lock_guard<std::mutex> held (guard);
        const auto found = learned.find (mountId);

        if (found == learned.end())
            return out;

        auto& parser = parsers[mountId];

        for (const auto byte : bytes)
        {
            if (byte >= 0xF8)               // real time: never part of a message
                continue;

            if (parser.inSysEx)
            {
                if (byte == 0xF7)
                    parser.inSysEx = false;
                else if (byte >= 0x80)      // a status inside a SysEx ends it too
                    parser.inSysEx = false;
                else
                    continue;

                if (byte == 0xF7)
                    continue;
            }

            if (byte == 0xF0)
            {
                parser.inSysEx = true;
                parser.status = 0;
                parser.data.clear();
                continue;
            }

            if (byte >= 0x80)
            {
                parser.status = byte < 0xF0 ? byte : 0;     // a system common message cancels running status
                parser.data.clear();
                continue;
            }

            if (parser.status == 0)
                continue;

            parser.data.push_back (byte);
            const auto kind = parser.status & 0xF0;
            const auto needed = (kind == 0xC0 || kind == 0xD0) ? 1u : 2u;

            if (parser.data.size() == needed)
            {
                complete (found->second, parser, parser.status, parser.data, out);
                parser.data.clear();
            }
        }

        return out;
    }

    void MidiEchoes::complete (const std::map<Key, Target>& targets, Parser& parser, std::uint8_t status,
                               const std::vector<std::uint8_t>& data, std::vector<Heard>& out) const
    {
        const auto channel = static_cast<int> (status & 0x0F) + 1;
        const auto kind = status & 0xF0;
        const auto at = static_cast<std::size_t> (channel - 1);

        if (kind == 0x90)
        {
            /*  A Note On's velocity is the switch: 40 to 7F on, 01 to 3F off,
                00 - a Note Off in disguise - ignored, as the consoles say. */
            if (data.size() < 2 || data[1] == 0)
                return;

            const auto target = targets.find ({ channel, 'n', static_cast<int> (data[0]), 0 });

            if (target != targets.end())
                out.push_back ({ target->second.address, { osc::Value::boolean (data[1] >= 0x40) } });

            return;
        }

        if (kind != 0xB0 || data.size() < 2)
            return;

        const auto controller = static_cast<int> (data[0]);
        const auto value = static_cast<int> (data[1]);

        if (controller == 99)
        {
            parser.nrpnMsb[at] = value;
            return;
        }

        if (controller == 98)
        {
            parser.nrpnLsb[at] = value;
            return;
        }

        if (controller == 6 || controller == 38)
        {
            const auto target = targets.find ({ channel, 'p', parser.nrpnMsb[at], parser.nrpnLsb[at] });

            if (target == targets.end())
                return;

            const auto& shape = target->second.shape;

            if (controller == 6)
            {
                parser.dataMsb[at] = value;

                //  A 14-bit parameter waits for its fine byte; a 7-bit one is whole here.
                if (shape.bits == 14)
                    return;

                out.push_back ({ target->second.address, valueFor (shape, value) });
                return;
            }

            if (shape.bits == 14)
                out.push_back ({ target->second.address, valueFor (shape, (parser.dataMsb[at] << 7) | value) });

            return;
        }

        const auto target = targets.find ({ channel, 'c', controller, 0 });

        if (target != targets.end())
            out.push_back ({ target->second.address, { osc::Value::int32 (value) } });
    }
}
