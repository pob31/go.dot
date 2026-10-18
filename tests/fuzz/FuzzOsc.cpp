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

/*
    FUZZ TARGET: the OSC decoder (OscCodec.h: "never throws, never reads
    outside the buffer, never returns a packet it had to guess at").

    Beyond not crashing, it checks the oracle the codec gives for free: a
    packet that decodes must ENCODE, and what it encodes to must decode to
    the same packet. A decoder that accepts what the encoder cannot say, or
    reads one packet where the wire carried another, breaks that and traps.

    The exception is a NaN or infinite float: the decoder delivers them and
    every entry point downstream refuses them (CommandRegistry, BindingBridge,
    Mount), while the encoder refuses to write one. Such a packet is checked
    for not crashing and nothing more.
*/

#include "FuzzTarget.h"

#include <wfg/engine/osc/OscCodec.h>

#include <string>

namespace osc = wfg::osc;

namespace
{
    bool hasNonFinite (const osc::Packet& p)
    {
        for (const auto& a : p.args)
            if (a.isNonFinite())
                return true;
        for (const auto& e : p.elements)
            if (hasNonFinite (e))
                return true;
        return false;
    }
}

extern "C" int LLVMFuzzerTestOneInput (const std::uint8_t* data, std::size_t size)
{
    const auto first = osc::decode (data, size);

    if (! first.ok)
    {
        if (first.reason.empty() || first.error.empty())
            __builtin_trap();   // "a dropped packet with neither is a dropped packet nobody can act on"
        return 0;
    }

    if (hasNonFinite (first.packet))
        return 0;

    std::string why;
    const auto wire = osc::encode (first.packet, why);
    if (! wire)
        __builtin_trap();       // decoded, but the encoder cannot say it

    const auto second = osc::decode (wire->data(), wire->size());
    if (! second.ok || second.packet != first.packet)
        __builtin_trap();       // what it said is not what it read

    return 0;
}

namespace wfg::fuzz
{
    std::vector<Bytes> seeds()
    {
        using osc::Packet;
        using osc::Value;

        const std::vector<Packet> packets
        {
            Packet::message ("/go"),
            Packet::message ("/cue/1/start", { Value::int32 (-12), Value::float32 (0.5f), Value::string ("text") }),
            Packet::message ("/x", { Value::int64 (1234), Value::float64 (0.25), Value::boolean (true), Value::nil(), Value::impulse(),
                                     Value::timeTag (osc::TimeTag { 0x0000000100000002ull }),
                                     Value::blob (osc::Blob { { 1, 2, 3, 4, 5 } }) }),
            Packet::bundle (osc::TimeTag { 1 }, { Packet::message ("/a", { Value::int32 (1) }),
                                                  Packet::bundle (osc::TimeTag { 7 }, { Packet::message ("/b") }) }),
        };

        std::vector<Bytes> out;
        for (const auto& p : packets)
        {
            std::string why;
            if (auto wire = osc::encode (p, why))
                out.push_back (*wire);
        }
        return out;
    }
}
