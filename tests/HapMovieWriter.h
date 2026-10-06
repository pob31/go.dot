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

/*  A HAP MOVIE WRITER OF THE TESTS' OWN (Phase 8b): a QuickTime `.mov` of
    HAP frames of one colour each, with the boxes Go.dot's reader reads and
    nothing more - DXT1 textures, sections plain or Snappy (literals only),
    and a chunked frame's decode instructions. Shared by MovieTests and the
    renderer's tests. */

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace wfg::testing::hapmovie
{
    using Bytes = std::vector<std::uint8_t>;

    inline void be32 (Bytes& out, std::uint32_t value)
    {
        for (int shift = 24; shift >= 0; shift -= 8)
            out.push_back (static_cast<std::uint8_t> (value >> shift));
    }

    inline void be16 (Bytes& out, std::uint16_t value)
    {
        out.push_back (static_cast<std::uint8_t> (value >> 8));
        out.push_back (static_cast<std::uint8_t> (value));
    }

    inline void fourcc (Bytes& out, const char* type)
    {
        out.insert (out.end(), type, type + 4);
    }

    inline Bytes box (const char* type, const Bytes& body)
    {
        Bytes out;
        be32 (out, static_cast<std::uint32_t> (body.size() + 8));
        fourcc (out, type);
        out.insert (out.end(), body.begin(), body.end());
        return out;
    }

    inline Bytes join (std::initializer_list<Bytes> parts)
    {
        Bytes out;

        for (const auto& part : parts)
            out.insert (out.end(), part.begin(), part.end());

        return out;
    }

    inline std::uint16_t rgb565 (std::uint32_t rgb)
    {
        const auto r = (rgb >> 16) & 0xffu, g = (rgb >> 8) & 0xffu, b = rgb & 0xffu;
        return static_cast<std::uint16_t> (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }

    /*  A DXT1 TEXTURE OF ONE COLOUR, every block both ends that colour. */
    inline Bytes solidDxt1 (int width, int height, std::uint32_t rgb)
    {
        Bytes out;
        const auto c = rgb565 (rgb);

        for (int block = 0; block < (width / 4) * (height / 4); ++block)
        {
            for (int n = 0; n < 2; ++n)
            {
                out.push_back (static_cast<std::uint8_t> (c & 0xffu));
                out.push_back (static_cast<std::uint8_t> (c >> 8));
            }

            out.insert (out.end(), 4, 0);
        }

        return out;
    }

    /*  A HAP SECTION: three bytes of length and the type. */
    inline Bytes section (std::uint8_t type, const Bytes& body)
    {
        Bytes out { static_cast<std::uint8_t> (body.size()), static_cast<std::uint8_t> (body.size() >> 8),
                    static_cast<std::uint8_t> (body.size() >> 16), type };
        out.insert (out.end(), body.begin(), body.end());
        return out;
    }

    /*  A SNAPPY BLOCK OF LITERALS ONLY - legal, and all a test needs to be
        sure the decoder reads what an encoder may write. */
    inline Bytes snappyLiterals (const Bytes& data)
    {
        Bytes out;
        auto length = data.size();

        do
        {
            auto byte = static_cast<std::uint8_t> (length & 0x7fu);
            length >>= 7;

            if (length != 0)
                byte |= 0x80u;

            out.push_back (byte);
        }
        while (length != 0);

        for (std::size_t at = 0; at < data.size(); at += 60)
        {
            const auto n = std::min<std::size_t> (60, data.size() - at);
            out.push_back (static_cast<std::uint8_t> ((n - 1) << 2));
            out.insert (out.end(), data.begin() + static_cast<long> (at), data.begin() + static_cast<long> (at + n));
        }

        return out;
    }

    /*  A QUICKTIME MOVIE of HAP frames: ftyp, mdat, then moov with one video
        track whose frames all sit in one chunk. */
    inline Bytes hapMovie (int width, int height, const std::vector<Bytes>& frames, std::uint32_t framesPerSecond,
                    const char* codec = "Hap1")
    {
        const auto ftyp = box ("ftyp", join ({ Bytes { 'q', 't', ' ', ' ' }, Bytes (4, 0), Bytes { 'q', 't', ' ', ' ' } }));

        Bytes media;
        std::vector<std::uint32_t> sizes;

        for (const auto& frame : frames)
        {
            media.insert (media.end(), frame.begin(), frame.end());
            sizes.push_back (static_cast<std::uint32_t> (frame.size()));
        }

        const auto mdat = box ("mdat", media);
        const auto firstFrame = static_cast<std::uint32_t> (ftyp.size() + 8);
        const auto count = static_cast<std::uint32_t> (frames.size());

        Bytes mdhd (4, 0);
        be32 (mdhd, 0); be32 (mdhd, 0); be32 (mdhd, framesPerSecond); be32 (mdhd, count);
        mdhd.insert (mdhd.end(), 4, 0);

        Bytes hdlr (4, 0);
        hdlr.insert (hdlr.end(), 4, 0);
        fourcc (hdlr, "vide");
        hdlr.insert (hdlr.end(), 12, 0);

        Bytes entry (6, 0);
        be16 (entry, 1);
        entry.insert (entry.end(), 16, 0);
        be16 (entry, static_cast<std::uint16_t> (width));
        be16 (entry, static_cast<std::uint16_t> (height));
        entry.insert (entry.end(), 50, 0);

        Bytes stsd (4, 0);
        be32 (stsd, 1);
        const auto sample = box (codec, entry);
        stsd.insert (stsd.end(), sample.begin(), sample.end());

        Bytes stts (4, 0);  be32 (stts, 1); be32 (stts, count); be32 (stts, 1);
        Bytes stsc (4, 0);  be32 (stsc, 1); be32 (stsc, 1); be32 (stsc, count); be32 (stsc, 1);
        Bytes stsz (4, 0);  be32 (stsz, 0); be32 (stsz, count);
        for (const auto size : sizes) be32 (stsz, size);
        Bytes stco (4, 0);  be32 (stco, 1); be32 (stco, firstFrame);

        const auto stbl = box ("stbl", join ({ box ("stsd", stsd), box ("stts", stts), box ("stsc", stsc),
                                               box ("stsz", stsz), box ("stco", stco) }));
        const auto mdia = box ("mdia", join ({ box ("mdhd", mdhd), box ("hdlr", hdlr), box ("minf", stbl) }));
        const auto moov = box ("moov", box ("trak", mdia));

        return join ({ ftyp, mdat, moov });
    }

    inline juce::File writeMovie (const juce::File& folder, const char* name, const Bytes& bytes)
    {
        const auto file = folder.getChildFile (name);
        REQUIRE (file.replaceWithData (bytes.data(), bytes.size()));
        return file;
    }
}

