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

#include <algorithm>
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
    //==============================================================================
    /*  AND COMPRESSED (namespace draft 37.6, F.2): what Go.dot's own HAP
        encoder packs a frame's texture with (WK). The format's usual greedy
        way - fragments of 64 KiB, a table of where each four bytes were last
        seen, a copy wherever they repeat and literals between - which is what
        every Snappy decoder expects, and what decompresses above. */
    namespace detail
    {
        inline void literal (const std::uint8_t* from, std::size_t length, std::vector<std::uint8_t>& out)
        {
            while (length > 0)
            {
                const auto run = length < 65536 ? length : std::size_t { 65536 };
                const auto n = run - 1;

                if (n < 60)
                {
                    out.push_back (static_cast<std::uint8_t> (n << 2));
                }
                else if (n < 256)
                {
                    out.push_back (static_cast<std::uint8_t> (60u << 2));
                    out.push_back (static_cast<std::uint8_t> (n));
                }
                else
                {
                    out.push_back (static_cast<std::uint8_t> (61u << 2));
                    out.push_back (static_cast<std::uint8_t> (n & 0xffu));
                    out.push_back (static_cast<std::uint8_t> (n >> 8));
                }

                out.insert (out.end(), from, from + run);
                from += run;
                length -= run;
            }
        }

        /*  A copy of `length` bytes from `offset` back - under 64 KiB, inside
            a fragment - as copies of at most 64. */
        inline void copy (std::size_t offset, std::size_t length, std::vector<std::uint8_t>& out)
        {
            while (length > 0)
            {
                /*  NEVER LEAVE LESS THAN FOUR for the last: a copy of one to
                    three is legal for the two-byte form, but kept to the
                    shape every encoder writes. */
                auto run = length > 64 ? std::size_t { 64 } : length;

                if (length > 64 && length - 64 < 4)
                    run = 60;

                if (run >= 4 && run <= 11 && offset < 2048)
                {
                    out.push_back (static_cast<std::uint8_t> (0x01u | ((run - 4) << 2) | ((offset >> 8) << 5)));
                    out.push_back (static_cast<std::uint8_t> (offset & 0xffu));
                }
                else
                {
                    out.push_back (static_cast<std::uint8_t> (0x02u | ((run - 1) << 2)));
                    out.push_back (static_cast<std::uint8_t> (offset & 0xffu));
                    out.push_back (static_cast<std::uint8_t> (offset >> 8));
                }

                length -= run;
            }
        }

        inline std::uint32_t load32 (const std::uint8_t* at) noexcept
        {
            std::uint32_t value = 0;
            std::memcpy (&value, at, 4);
            return value;
        }
    }

    inline void compress (const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out)
    {
        out.clear();
        out.reserve (32 + size + size / 6);

        /*  THE LENGTH, a varint. */
        auto length = size;

        do
        {
            auto byte = static_cast<std::uint8_t> (length & 0x7fu);
            length >>= 7;

            if (length != 0)
                byte |= 0x80u;

            out.push_back (byte);
        }
        while (length != 0);

        constexpr std::size_t fragmentSize = 65536;
        constexpr int tableBits = 14;
        std::vector<std::uint16_t> table (std::size_t { 1 } << tableBits);

        for (std::size_t fragment = 0; fragment < size; fragment += fragmentSize)
        {
            const auto* base = data + fragment;
            const auto end = size - fragment < fragmentSize ? size - fragment : fragmentSize;

            std::fill (table.begin(), table.end(), std::uint16_t { 0 });

            const auto hashOf = [] (std::uint32_t bytes) noexcept
            {
                return static_cast<std::size_t> ((bytes * 0x1e35a7bdu) >> (32 - tableBits));
            };

            std::size_t at = 0, pending = 0;

            while (at + 4 <= end)
            {
                const auto bytes = detail::load32 (base + at);
                auto& seen = table[hashOf (bytes)];
                const auto candidate = static_cast<std::size_t> (seen);
                seen = static_cast<std::uint16_t> (at);

                if (candidate < at && detail::load32 (base + candidate) == bytes)
                {
                    auto matched = std::size_t { 4 };

                    while (at + matched < end && base[candidate + matched] == base[at + matched])
                        ++matched;

                    detail::literal (base + pending, at - pending, out);
                    detail::copy (at - candidate, matched, out);

                    /*  WHAT WAS COPIED, remembered too, two places in it, so
                        a run that goes on is found again. */
                    if (at + matched >= 2 && at + matched - 2 + 4 <= end)
                        table[hashOf (detail::load32 (base + at + matched - 2))] = static_cast<std::uint16_t> (at + matched - 2);

                    at += matched;
                    pending = at;
                }
                else
                {
                    ++at;
                }
            }

            detail::literal (base + pending, end - pending, out);
        }
    }
}
