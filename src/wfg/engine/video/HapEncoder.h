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
    HAP, ENCODED (namespace draft 37.5 WK, 37.6 F.2): the reverse of Hap.h.
    A frame of straight RGBA pixels becomes DXT blocks - DXT1 for Hap, DXT5
    for Hap Alpha, scaled YCoCg in DXT5 for Hap Q - and the blocks one HAP
    section, packed with Snappy when that is smaller.

    EACH BLOCK: the colours' principal axis through their mean, the two ends
    the colours reach along it, pulled in a sixteenth so the steps between
    land on more of them; then each pixel's nearest of the four colours, and
    the ends solved again from those choices by least squares, kept when the
    block comes out closer. What every good DXT encoder does in some form;
    the error is measured the way the decoder in Hap.h decodes, so what is
    chosen is what is seen.

    Pure, and no allocation but the output: blocks are independent, so a
    caller may encode a frame's rows of blocks on as many threads as it has.
*/

#include <wfg/engine/video/Hap.h>
#include <wfg/engine/video/Snappy.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace wfg::video::hap
{
    namespace encode
    {
        //  The decoder's own expansion of a 5:6:5 colour (Hap.h).
        inline void expand565 (unsigned colour, double rgb[3]) noexcept
        {
            rgb[0] = static_cast<double> (((colour >> 11) & 0x1fu) * 255u / 31u);
            rgb[1] = static_cast<double> (((colour >> 5) & 0x3fu) * 255u / 63u);
            rgb[2] = static_cast<double> ((colour & 0x1fu) * 255u / 31u);
        }

        inline unsigned pack565 (const double rgb[3]) noexcept
        {
            const auto quantise = [] (double value, int levels)
            {
                return static_cast<unsigned> (std::clamp (static_cast<int> (std::lround (std::clamp (value, 0.0, 255.0)
                                                                                        * levels / 255.0)),
                                                          0, levels));
            };

            return (quantise (rgb[0], 31) << 11) | (quantise (rgb[1], 63) << 5) | quantise (rgb[2], 31);
        }

        /*  THE COLOUR HALF OF A BLOCK, always the four-colour form: 8 bytes
            at `out`. `weights` say how much each channel's error counts -
            Hap Q's blue is its scale, held, and weighs nothing. `fixedBlue`,
            when not negative, is the blue both ends carry, in 0..31. */
        inline void colourBlock (const double pixels[16][3], std::uint8_t* out,
                                 const double weights[3] = nullptr, int fixedBlue = -1) noexcept
        {
            static constexpr double even[3] { 1.0, 1.0, 1.0 };
            const auto* w = weights != nullptr ? weights : even;

            /*  THE MEAN AND THE SPREAD. */
            double mean[3] {};

            for (int p = 0; p < 16; ++p)
                for (int c = 0; c < 3; ++c)
                    mean[c] += pixels[p][c] / 16.0;

            double cov[6] {};   // xx xy xz yy yz zz

            for (int p = 0; p < 16; ++p)
            {
                const double d[3] { (pixels[p][0] - mean[0]) * w[0], (pixels[p][1] - mean[1]) * w[1],
                                    (pixels[p][2] - mean[2]) * w[2] };
                cov[0] += d[0] * d[0]; cov[1] += d[0] * d[1]; cov[2] += d[0] * d[2];
                cov[3] += d[1] * d[1]; cov[4] += d[1] * d[2]; cov[5] += d[2] * d[2];
            }

            /*  THE PRINCIPAL AXIS, by power iteration from the widest channel. */
            double axis[3] { 1.0, 1.0, 1.0 };

            if (cov[0] >= cov[3] && cov[0] >= cov[5])      axis[1] = axis[2] = 0.5;
            else if (cov[3] >= cov[5])                      axis[0] = axis[2] = 0.5;
            else                                            axis[0] = axis[1] = 0.5;

            for (int iteration = 0; iteration < 8; ++iteration)
            {
                const double next[3] { cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2],
                                       cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2],
                                       cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2] };
                const auto length = std::sqrt (next[0] * next[0] + next[1] * next[1] + next[2] * next[2]);

                if (! (length > 1e-12))
                    break;

                for (int c = 0; c < 3; ++c)
                    axis[c] = next[c] / length;
            }

            //  Back from the weighted space, for the ends.
            for (int c = 0; c < 3; ++c)
                axis[c] = w[c] > 0.0 ? axis[c] / w[c] : 0.0;

            double low = 0.0, high = 0.0;

            for (int p = 0; p < 16; ++p)
            {
                const auto along = (pixels[p][0] - mean[0]) * axis[0] + (pixels[p][1] - mean[1]) * axis[1]
                                 + (pixels[p][2] - mean[2]) * axis[2];
                low = std::min (low, along);
                high = std::max (high, along);
            }

            const auto inset = (high - low) / 16.0;
            double ends[2][3];

            for (int c = 0; c < 3; ++c)
            {
                ends[0][c] = mean[c] + (high - inset) * axis[c];
                ends[1][c] = mean[c] + (low + inset) * axis[c];
            }

            /*  THE BLOCK FOR TWO ENDS: quantised, ordered for the four-colour
                form, each pixel's nearest, and how far off that leaves it. */
            struct Candidate
            {
                unsigned c0 = 0, c1 = 0;
                std::uint32_t indices = 0;
                double error = 0.0;
            };

            const auto build = [&pixels, w, fixedBlue] (const double (&e)[2][3])
            {
                Candidate candidate;
                candidate.c0 = pack565 (e[0]);
                candidate.c1 = pack565 (e[1]);

                if (fixedBlue >= 0)
                {
                    candidate.c0 = (candidate.c0 & ~0x1fu) | static_cast<unsigned> (fixedBlue);
                    candidate.c1 = (candidate.c1 & ~0x1fu) | static_cast<unsigned> (fixedBlue);
                }

                //  THE FOUR-COLOUR FORM WANTS c0 > c1; equal ends are one colour.
                if (candidate.c0 < candidate.c1)
                    std::swap (candidate.c0, candidate.c1);

                double palette[4][3];
                expand565 (candidate.c0, palette[0]);
                expand565 (candidate.c1, palette[1]);

                for (int c = 0; c < 3; ++c)
                {
                    palette[2][c] = (2.0 * palette[0][c] + palette[1][c]) / 3.0;
                    palette[3][c] = (palette[0][c] + 2.0 * palette[1][c]) / 3.0;
                }

                const auto colours = candidate.c0 == candidate.c1 ? 1 : 4;

                for (int p = 0; p < 16; ++p)
                {
                    auto best = 0;
                    auto bestError = 1e300;

                    for (int k = 0; k < colours; ++k)
                    {
                        auto error = 0.0;

                        for (int c = 0; c < 3; ++c)
                        {
                            const auto d = (pixels[p][c] - palette[k][c]) * w[c];
                            error += d * d;
                        }

                        if (error < bestError)
                        {
                            bestError = error;
                            best = k;
                        }
                    }

                    candidate.indices |= static_cast<std::uint32_t> (best) << (2 * p);
                    candidate.error += bestError;
                }

                return candidate;
            };

            auto chosen = build (ends);

            /*  AND THE ENDS AGAIN, by least squares from those choices: each
                pixel is (1 - t) of one end and t of the other, t in thirds. */
            if (chosen.c0 != chosen.c1)
            {
                static constexpr double along[4] { 0.0, 1.0, 1.0 / 3.0, 2.0 / 3.0 };
                double aa = 0.0, ab = 0.0, bb = 0.0, ax[3] {}, bx[3] {};

                for (int p = 0; p < 16; ++p)
                {
                    const auto t = along[(chosen.indices >> (2 * p)) & 3u];
                    const auto a = 1.0 - t, b = t;
                    aa += a * a; ab += a * b; bb += b * b;

                    for (int c = 0; c < 3; ++c)
                    {
                        ax[c] += a * pixels[p][c];
                        bx[c] += b * pixels[p][c];
                    }
                }

                const auto determinant = aa * bb - ab * ab;

                if (std::abs (determinant) > 1e-9)
                {
                    double solved[2][3];

                    for (int c = 0; c < 3; ++c)
                    {
                        solved[0][c] = (bb * ax[c] - ab * bx[c]) / determinant;
                        solved[1][c] = (aa * bx[c] - ab * ax[c]) / determinant;
                    }

                    if (const auto again = build (solved); again.error < chosen.error)
                        chosen = again;
                }
            }

            out[0] = static_cast<std::uint8_t> (chosen.c0 & 0xffu);
            out[1] = static_cast<std::uint8_t> (chosen.c0 >> 8);
            out[2] = static_cast<std::uint8_t> (chosen.c1 & 0xffu);
            out[3] = static_cast<std::uint8_t> (chosen.c1 >> 8);
            out[4] = static_cast<std::uint8_t> (chosen.indices & 0xffu);
            out[5] = static_cast<std::uint8_t> ((chosen.indices >> 8) & 0xffu);
            out[6] = static_cast<std::uint8_t> ((chosen.indices >> 16) & 0xffu);
            out[7] = static_cast<std::uint8_t> (chosen.indices >> 24);
        }

        /*  THE ALPHA HALF: 8 bytes, the eight-value form, ends the lowest
            and highest. Values 0..255. */
        inline void alphaBlock (const int values[16], std::uint8_t* out) noexcept
        {
            auto low = 255, high = 0;

            for (int p = 0; p < 16; ++p)
            {
                low = std::min (low, values[p]);
                high = std::max (high, values[p]);
            }

            out[0] = static_cast<std::uint8_t> (high);
            out[1] = static_cast<std::uint8_t> (low);

            std::uint64_t bits = 0;

            if (high > low)
            {
                //  The decoder's eight: the ends, then six between (Hap.h).
                int palette[8] { high, low };

                for (int code = 2; code < 8; ++code)
                    palette[code] = ((8 - code) * high + (code - 1) * low) / 7;

                for (int p = 0; p < 16; ++p)
                {
                    auto best = 0, bestError = 1 << 30;

                    for (int code = 0; code < 8; ++code)
                        if (const auto error = std::abs (values[p] - palette[code]); error < bestError)
                        {
                            bestError = error;
                            best = code;
                        }

                    bits |= static_cast<std::uint64_t> (best) << (3 * p);
                }
            }

            for (int n = 0; n < 6; ++n)
                out[2 + n] = static_cast<std::uint8_t> ((bits >> (8 * n)) & 0xffu);
        }
    }

    /*  ONE BLOCK OF A FRAME: the 4x4 pixels at block (bx, by) of an RGBA
        frame `width` x `height`, its rows `stride` bytes apart, encoded as
        `texture` at `out` - 8 bytes for DXT1, 16 for the others. Pixels past
        the edge repeat the edge. */
    inline void encodeBlock (Texture texture, const std::uint8_t* rgba, int width, int height, std::size_t stride,
                             int bx, int by, std::uint8_t* out) noexcept
    {
        double colour[16][3];
        int alpha[16];

        for (int p = 0; p < 16; ++p)
        {
            const auto x = std::min (bx * 4 + (p % 4), width - 1);
            const auto y = std::min (by * 4 + (p / 4), height - 1);
            const auto* at = rgba + static_cast<std::size_t> (y) * stride + static_cast<std::size_t> (x) * 4;

            colour[p][0] = at[0];
            colour[p][1] = at[1];
            colour[p][2] = at[2];
            alpha[p] = at[3];
        }

        if (texture == Texture::dxt1)
        {
            encode::colourBlock (colour, out);
            return;
        }

        if (texture == Texture::dxt5)
        {
            encode::alphaBlock (alpha, out);
            encode::colourBlock (colour, out + 8);
            return;
        }

        /*  HAP Q: SCALED YCoCg (the reverse of Hap.h's decode). Y goes to the
            alpha half; Co and Cg, centred on 128 and scaled up by 1, 2 or 4
            as far as the block's spread allows, to red and green; the scale
            to blue, as (scale - 1) x 8 - 0, 1 or 3 of blue's 31. */
        int luma[16];
        double chroma[16][3];
        auto widest = 0.0;

        for (int p = 0; p < 16; ++p)
        {
            const auto r = colour[p][0], g = colour[p][1], b = colour[p][2];
            luma[p] = std::clamp (static_cast<int> (std::lround (r / 4.0 + g / 2.0 + b / 4.0)), 0, 255);
            chroma[p][0] = r / 2.0 - b / 2.0;
            chroma[p][1] = -r / 4.0 + g / 2.0 - b / 4.0;
            widest = std::max ({ widest, std::abs (chroma[p][0]), std::abs (chroma[p][1]) });
        }

        const auto scale = widest < 31.0 ? 4 : widest < 63.0 ? 2 : 1;
        const auto blue = scale == 4 ? 3 : scale == 2 ? 1 : 0;

        for (int p = 0; p < 16; ++p)
        {
            chroma[p][0] = chroma[p][0] * scale + 128.0;
            chroma[p][1] = chroma[p][1] * scale + 128.0;
            chroma[p][2] = 0.0;
        }

        static constexpr double chromaOnly[3] { 1.0, 1.0, 0.0 };
        encode::alphaBlock (luma, out);
        encode::colourBlock (chroma, out + 8, chromaOnly, blue);
    }

    /*  A WHOLE FRAME'S BLOCKS, rows of blocks top to bottom. */
    inline void encodeTexture (Texture texture, const std::uint8_t* rgba, int width, int height, std::size_t stride,
                               std::vector<std::uint8_t>& out, int firstRow = 0, int rows = -1)
    {
        const auto across = (width + 3) / 4;
        const auto down = (height + 3) / 4;
        const auto bytes = bytesPerBlock (texture);

        if (out.size() < static_cast<std::size_t> (across * down) * bytes)
            out.resize (static_cast<std::size_t> (across * down) * bytes);

        const auto last = rows < 0 ? down : std::min (down, firstRow + rows);

        for (int by = firstRow; by < last; ++by)
            for (int bx = 0; bx < across; ++bx)
                encodeBlock (texture, rgba, width, height, stride, bx, by,
                             out.data() + (static_cast<std::size_t> (by) * static_cast<std::size_t> (across)
                                           + static_cast<std::size_t> (bx)) * bytes);
    }

    /*  A WHOLE FRAME'S BLOCKS ON EVERY THREAD SPARED, a band of rows of
        blocks each: the converter's loop, shared with the render of an
        edit's dissolves (namespace draft §55.5). `threads` at least one. */
    inline void encodeTextureThreaded (Texture texture, const std::uint8_t* rgba, int width, int height,
                                       std::size_t stride, std::vector<std::uint8_t>& out, int threads)
    {
        const auto across = (width + 3) / 4;
        const auto rowsOfBlocks = (height + 3) / 4;
        const auto workers = std::clamp (threads, 1, std::max (1, rowsOfBlocks));

        out.resize (static_cast<std::size_t> (across * rowsOfBlocks) * bytesPerBlock (texture));
        const auto band = (rowsOfBlocks + workers - 1) / workers;
        std::vector<std::thread> others;

        for (int t = 1; t < workers; ++t)
            others.emplace_back ([&, t]
                                 {
                                     encodeTexture (texture, rgba, width, height, stride, out, t * band, band);
                                 });

        encodeTexture (texture, rgba, width, height, stride, out, 0, band);

        for (auto& other : others)
            other.join();
    }

    /*  THE FRAME AS HAP STORES IT: one section, Snappy-packed when that is
        smaller, else as it is; a long header past 16 MiB. */
    inline void packFrame (Texture texture, const std::vector<std::uint8_t>& blocks, std::vector<std::uint8_t>& out,
                           std::vector<std::uint8_t>& scratch)
    {
        const auto format = texture == Texture::dxt1 ? 0x0bu : texture == Texture::dxt5 ? 0x0eu : 0x0fu;

        snappy::compress (blocks.data(), blocks.size(), scratch);
        const auto packed = scratch.size() < blocks.size();
        const auto& body = packed ? scratch : blocks;
        const auto type = static_cast<std::uint8_t> ((packed ? 0xb0u : 0xa0u) | format);

        out.clear();

        if (body.size() < 0x1000000u)
        {
            out.push_back (static_cast<std::uint8_t> (body.size() & 0xffu));
            out.push_back (static_cast<std::uint8_t> ((body.size() >> 8) & 0xffu));
            out.push_back (static_cast<std::uint8_t> ((body.size() >> 16) & 0xffu));
            out.push_back (type);
        }
        else
        {
            out.insert (out.end(), 3, 0);
            out.push_back (type);

            for (int shift = 0; shift < 32; shift += 8)
                out.push_back (static_cast<std::uint8_t> ((body.size() >> shift) & 0xffu));
        }

        out.insert (out.end(), body.begin(), body.end());
    }
}
