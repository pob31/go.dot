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
    SLIP, AS OSC 1.1 USES IT (namespace draft §51, ACR; PC.11): an OSC packet
    over a serial line, each one framed by END bytes (0xC0) with any END or ESC
    (0xDB) inside escaped as ESC 0xDC and ESC 0xDD - RFC 1055's framing, which
    is what CNMAT's OSC library for the Arduino sends with SLIPEncodedSerial.

    Written with an END at both ends, as OSC 1.1 asks ("double-ended"), so a
    device that missed the end of the last packet is in step by the start of
    this one; read whatever way it comes, an empty frame between two ENDs being
    nothing. A frame longer than 64 kB is dropped to its END rather than grown.
*/

#include <cstdint>
#include <string>
#include <vector>

namespace wfg::serial::slip
{
    inline constexpr std::uint8_t end = 0xC0;
    inline constexpr std::uint8_t esc = 0xDB;
    inline constexpr std::uint8_t escapedEnd = 0xDC;
    inline constexpr std::uint8_t escapedEsc = 0xDD;
    inline constexpr std::size_t largestFrame = 65536;

    /*  One packet as the bytes that go down the line. */
    std::string encode (const std::vector<std::uint8_t>& packet);

    /*  Bytes in, as a port reads them, whole packets out. */
    class Decoder
    {
    public:
        std::vector<std::vector<std::uint8_t>> feed (const std::string& bytes);

        /*  Frames given up on: too long, or an ESC followed by what it may not be. */
        std::uint64_t dropped() const noexcept { return lost; }

    private:
        std::vector<std::uint8_t> frame;
        bool escaping = false;
        bool spoiled = false;
        std::uint64_t lost = 0;
    };
}
