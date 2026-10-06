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
    A HAP FRAME, UNPACKED TO ITS TEXTURE (Phase 8b, namespace draft 37, VY).

    Go.dot's own, written from the published HAP specification. A frame is a
    section: a header (three bytes of length and a type byte, or a length of
    nought and four more bytes), whose type says the texture's format in its
    low nibble and its second-stage compressor in its high one - none,
    Snappy, or "complex", which means the texture is cut into chunks each
    compressed on its own, described by a decode-instructions section in
    front of them. What comes out is the texture as the graphics card takes
    it, compressed: DXT1 for Hap, DXT5 for Hap Alpha and for Hap Q - whose
    DXT5 holds colour as scaled YCoCg, turned back to RGB when it is drawn.

    And the CPU's decode of those blocks, for the renderer with no window and
    for the tests: the colour of one pixel of a frame, exactly as the GPU's
    fixed-function decode would give it.

    Bounds-checked throughout: a damaged frame is a black frame.
*/

#include <wfg/engine/video/Snappy.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace wfg::video::hap
{
    enum class Texture : std::uint8_t
    {
        none = 0,
        dxt1,       ///< Hap: RGB, 8 bytes a block
        dxt5,       ///< Hap Alpha: RGBA, 16 bytes a block
        ycocgDxt5   ///< Hap Q: scaled YCoCg in DXT5, 16 bytes a block
    };

    inline std::size_t bytesPerBlock (Texture texture) noexcept
    {
        return texture == Texture::dxt1 ? 8 : 16;
    }

    namespace detail
    {
        struct Section
        {
            std::size_t headerBytes = 0;
            std::size_t size = 0;
            std::uint8_t type = 0;
        };

        inline bool readSection (const std::uint8_t* data, std::size_t size, Section& out) noexcept
        {
            if (size < 4)
                return false;

            out.size = static_cast<std::size_t> (data[0]) | (static_cast<std::size_t> (data[1]) << 8)
                     | (static_cast<std::size_t> (data[2]) << 16);
            out.type = data[3];
            out.headerBytes = 4;

            if (out.size == 0)
            {
                if (size < 8)
                    return false;

                out.size = static_cast<std::size_t> (data[4]) | (static_cast<std::size_t> (data[5]) << 8)
                         | (static_cast<std::size_t> (data[6]) << 16) | (static_cast<std::size_t> (data[7]) << 24);
                out.headerBytes = 8;
            }

            return out.headerBytes + out.size <= size;
        }

        inline Texture textureOf (std::uint8_t type) noexcept
        {
            switch (type & 0x0fu)
            {
                case 0x0b: return Texture::dxt1;
                case 0x0e: return Texture::dxt5;
                case 0x0f: return Texture::ycocgDxt5;
                default:   return Texture::none;
            }
        }

        inline std::uint32_t le32 (const std::uint8_t* at) noexcept
        {
            return static_cast<std::uint32_t> (at[0]) | (static_cast<std::uint32_t> (at[1]) << 8)
                 | (static_cast<std::uint32_t> (at[2]) << 16) | (static_cast<std::uint32_t> (at[3]) << 24);
        }

        /*  A COMPLEX FRAME: the decode instructions, then the chunks, each
            uncompressed or Snappy, laid end to end into the texture. */
        inline bool unpackChunks (const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out)
        {
            Section instructions;

            if (! readSection (data, size, instructions) || instructions.type != 0x01)
                return false;

            const auto* inside = data + instructions.headerBytes;
            const auto insideSize = instructions.size;

            std::vector<std::uint8_t> compressors;
            std::vector<std::uint32_t> sizes, offsets;

            for (std::size_t at = 0; at < insideSize;)
            {
                Section part;

                if (! readSection (inside + at, insideSize - at, part))
                    return false;

                const auto* body = inside + at + part.headerBytes;

                if (part.type == 0x02)
                    compressors.assign (body, body + part.size);
                else if (part.type == 0x03 || part.type == 0x04)
                    for (std::size_t n = 0; n + 4 <= part.size; n += 4)
                        (part.type == 0x03 ? sizes : offsets).push_back (le32 (body + n));

                at += part.headerBytes + part.size;
            }

            if (compressors.empty() || compressors.size() != sizes.size()
                  || (! offsets.empty() && offsets.size() != sizes.size()))
                return false;

            const auto* chunks = data + instructions.headerBytes + instructions.size;
            const auto chunksSize = size - instructions.headerBytes - instructions.size;

            out.clear();
            std::vector<std::uint8_t> piece;
            std::size_t running = 0;

            for (std::size_t n = 0; n < sizes.size(); ++n)
            {
                const auto offset = offsets.empty() ? running : static_cast<std::size_t> (offsets[n]);
                running += sizes[n];

                if (offset + sizes[n] > chunksSize)
                    return false;

                if (compressors[n] == 0x0a)
                {
                    out.insert (out.end(), chunks + offset, chunks + offset + sizes[n]);
                }
                else if (compressors[n] == 0x0b)
                {
                    if (! snappy::decompress (chunks + offset, sizes[n], piece))
                        return false;

                    out.insert (out.end(), piece.begin(), piece.end());
                }
                else
                {
                    return false;
                }
            }

            return true;
        }
    }

    /*  A FRAME TO ITS TEXTURE: the format, and the blocks in `out`. False for a
        frame that will not read - a format not taken here (Hap R, Hap Q
        Alpha), or damage. */
    inline bool unpack (const std::uint8_t* data, std::size_t size, Texture& texture, std::vector<std::uint8_t>& out)
    {
        detail::Section frame;

        if (! detail::readSection (data, size, frame))
            return false;

        texture = detail::textureOf (frame.type);

        if (texture == Texture::none)
            return false;

        const auto* body = data + frame.headerBytes;

        switch (frame.type & 0xf0u)
        {
            case 0xa0:
                out.assign (body, body + frame.size);
                return true;

            case 0xb0:
                return snappy::decompress (body, frame.size, out);

            case 0xc0:
                return detail::unpackChunks (body, frame.size, out);

            default:
                return false;
        }
    }

    //==============================================================================
    /*  THE CPU'S DECODE OF ONE PIXEL: (x, y) from the texture's top-left, its
        width in pixels a multiple of four, as 0..255 straight colour and 0..1
        alpha. Hap Q's colour turned back from scaled YCoCg, as its shader
        does. False when the pixel is outside the blocks given. */
    inline bool pixelAt (Texture texture, const std::vector<std::uint8_t>& blocks, int width, int height,
                         int x, int y, double& red, double& green, double& blue, double& alpha) noexcept
    {
        if (texture == Texture::none || width <= 0 || height <= 0 || x < 0 || y < 0 || x >= width || y >= height)
            return false;

        const auto blocksAcross = static_cast<std::size_t> ((width + 3) / 4);
        const auto block = static_cast<std::size_t> (y / 4) * blocksAcross + static_cast<std::size_t> (x / 4);
        const auto stride = bytesPerBlock (texture);

        if ((block + 1) * stride > blocks.size())
            return false;

        const auto* at = blocks.data() + block * stride;
        const auto texel = static_cast<unsigned> ((y % 4) * 4 + (x % 4));

        //  THE ALPHA HALF of a DXT5 block: two ends and eight steps between, or six and the extremes.
        double a = 1.0;

        if (texture != Texture::dxt1)
        {
            const auto a0 = static_cast<int> (at[0]), a1 = static_cast<int> (at[1]);
            std::uint64_t bits = 0;

            for (int n = 0; n < 6; ++n)
                bits |= static_cast<std::uint64_t> (at[2 + n]) << (8 * n);

            const auto code = static_cast<int> ((bits >> (3u * texel)) & 0x07u);
            int value = 0;

            if (code == 0)                  value = a0;
            else if (code == 1)             value = a1;
            else if (a0 > a1)               value = ((8 - code) * a0 + (code - 1) * a1) / 7;
            else if (code == 6)             value = 0;
            else if (code == 7)             value = 255;
            else                            value = ((6 - code) * a0 + (code - 1) * a1) / 5;

            a = static_cast<double> (value) / 255.0;
            at += 8;
        }

        //  THE COLOUR HALF: two 5:6:5 ends, and two more between them or one and black.
        const auto c0 = static_cast<unsigned> (at[0] | (at[1] << 8));
        const auto c1 = static_cast<unsigned> (at[2] | (at[3] << 8));
        const auto indices = detail::le32 (at + 4);
        const auto code = static_cast<int> ((indices >> (2u * texel)) & 0x03u);

        const auto expand = [] (unsigned colour, double rgb[3])
        {
            rgb[0] = static_cast<double> (((colour >> 11) & 0x1fu) * 255u / 31u);
            rgb[1] = static_cast<double> (((colour >> 5) & 0x3fu) * 255u / 63u);
            rgb[2] = static_cast<double> ((colour & 0x1fu) * 255u / 31u);
        };

        double e0[3], e1[3], c[3];
        expand (c0, e0);
        expand (c1, e1);

        const auto fourColours = texture != Texture::dxt1 || c0 > c1;

        for (int n = 0; n < 3; ++n)
        {
            if (code == 0)              c[n] = e0[n];
            else if (code == 1)         c[n] = e1[n];
            else if (fourColours)       c[n] = code == 2 ? (2.0 * e0[n] + e1[n]) / 3.0 : (e0[n] + 2.0 * e1[n]) / 3.0;
            else if (code == 2)         c[n] = (e0[n] + e1[n]) / 2.0;
            else                        c[n] = 0.0;
        }

        if (texture == Texture::dxt1 && ! fourColours && code == 3)
            a = 0.0;

        if (texture == Texture::ycocgDxt5)
        {
            /*  SCALED YCoCg: Co in red, Cg in green, the scale in blue, Y in
                alpha - turned back as the HAP shader does. */
            const auto scale = (c[2] / 255.0) * (255.0 / 8.0) + 1.0;
            const auto co = (c[0] / 255.0 - 128.0 / 255.0) / scale;
            const auto cg = (c[1] / 255.0 - 128.0 / 255.0) / scale;
            const auto luma = a;

            red = std::clamp (luma + co - cg, 0.0, 1.0) * 255.0;
            green = std::clamp (luma + cg, 0.0, 1.0) * 255.0;
            blue = std::clamp (luma - co - cg, 0.0, 1.0) * 255.0;
            alpha = 1.0;
            return true;
        }

        red = c[0];
        green = c[1];
        blue = c[2];
        alpha = a;
        return true;
    }
}
