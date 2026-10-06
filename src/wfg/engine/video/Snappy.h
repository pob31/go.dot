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
    SNAPPY, DECOMPRESSED (Phase 8b, namespace draft 37, VY): the second stage of
    most HAP frames. Go.dot's own, written from the format's published
    description - a varint of the uncompressed length, then literals and
    copies - and nothing else: a HAP frame is one block, never the framed
    stream format.

    Bounds-checked at every step: a frame read off a disk is data from
    outside, and a damaged one must give a black frame, never a crash in the
    renderer. Pure, no allocation but the output.
*/

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace wfg::video::snappy
{
    /*  The length a block says it will decompress to, or false for a block
        whose header will not read. */
    inline bool uncompressedLength (const std::uint8_t* data, std::size_t size, std::size_t& length,
                                    std::size_t& headerBytes) noexcept
    {
        std::uint64_t value = 0;

        for (std::size_t at = 0, shift = 0; at < size && at < 5; ++at, shift += 7)
        {
            value |= static_cast<std::uint64_t> (data[at] & 0x7fu) << shift;

            if ((data[at] & 0x80u) == 0)
            {
                length = static_cast<std::size_t> (value);
                headerBytes = at + 1;
                return true;
            }
        }

        return false;
    }

    /*  One block into `out`, replaced. False when the block is damaged. */
    inline bool decompress (const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out)
    {
        std::size_t length = 0, at = 0;

        if (! uncompressedLength (data, size, length, at) || length > (std::size_t (1) << 30))
            return false;

        out.resize (length);
        std::size_t written = 0;

        while (at < size)
        {
            const auto tag = data[at++];
            const auto kind = tag & 0x03u;

            if (kind == 0)
            {
                //  A LITERAL: its length in the tag, or in up to four bytes after it.
                std::size_t literal = (tag >> 2u) + 1u;

                if (literal > 60)
                {
                    const auto bytes = literal - 60;

                    if (at + bytes > size)
                        return false;

                    literal = 0;

                    for (std::size_t n = 0; n < bytes; ++n)
                        literal |= static_cast<std::size_t> (data[at + n]) << (8 * n);

                    literal += 1;
                    at += bytes;
                }

                if (at + literal > size || written + literal > length)
                    return false;

                std::memcpy (out.data() + written, data + at, literal);
                at += literal;
                written += literal;
                continue;
            }

            //  A COPY: a length and an offset back into what is already written.
            std::size_t copy = 0, offset = 0;

            if (kind == 1)
            {
                if (at + 1 > size)
                    return false;

                copy = ((tag >> 2u) & 0x07u) + 4u;
                offset = (static_cast<std::size_t> (tag >> 5u) << 8) | data[at];
                at += 1;
            }
            else if (kind == 2)
            {
                if (at + 2 > size)
                    return false;

                copy = (tag >> 2u) + 1u;
                offset = static_cast<std::size_t> (data[at]) | (static_cast<std::size_t> (data[at + 1]) << 8);
                at += 2;
            }
            else
            {
                if (at + 4 > size)
                    return false;

                copy = (tag >> 2u) + 1u;
                offset = static_cast<std::size_t> (data[at]) | (static_cast<std::size_t> (data[at + 1]) << 8)
                       | (static_cast<std::size_t> (data[at + 2]) << 16) | (static_cast<std::size_t> (data[at + 3]) << 24);
                at += 4;
            }

            if (offset == 0 || offset > written || written + copy > length)
                return false;

            /*  BYTE BY BYTE, since a copy may overlap what it is writing - a
                run of one byte repeated is an offset of one. */
            for (std::size_t n = 0; n < copy; ++n)
                out[written + n] = out[written - offset + n];

            written += copy;
        }

        return written == length;
    }
}
