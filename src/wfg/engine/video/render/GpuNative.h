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
    THE THREE SYSTEMS' HALVES OF Gpu.h, one file each: Gpu_d3d11.cpp,
    Gpu_metal.mm, Gpu_gl.cpp. Only Gpu.cpp calls them.
*/

#include <wfg/engine/video/render/Gpu.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace wfg::video::gpu::native
{
    /*  The device made, and what sokol_gfx needs of it written into
        `environment`; `adapter` names the graphics card. */
    bool open (const OpenOptions& options, sg_environment& environment, std::string& adapter, std::string& why);

    void close();

    /** The system's graphics, as the readout names it: "Direct3D 11". */
    const char* backendName() noexcept;

    /*  `image`'s pixels, `width` by `height` of `format`, as four floats a
        pixel, rows from the top. */
    bool readBack (sg_image image, int width, int height, sg_pixel_format format, std::vector<float>& rgba);

    /*  A HALF-FLOAT'S VALUE (IEEE 754 binary16), for an RGBA16F picture read
        back. */
    inline float halfToFloat (std::uint16_t half) noexcept
    {
        const auto sign = static_cast<std::uint32_t> (half & 0x8000u) << 16;
        const auto exponent = static_cast<std::uint32_t> ((half >> 10) & 0x1fu);
        const auto mantissa = static_cast<std::uint32_t> (half & 0x3ffu);
        std::uint32_t bits = 0;

        if (exponent == 0)
        {
            if (mantissa != 0)
            {
                //  Subnormal: normalised by hand.
                auto m = mantissa;
                std::uint32_t e = 113;

                while ((m & 0x400u) == 0)
                {
                    m <<= 1;
                    --e;
                }

                bits = sign | (e << 23) | ((m & 0x3ffu) << 13);
            }
            else
            {
                bits = sign;
            }
        }
        else if (exponent == 31)
        {
            bits = sign | 0x7f800000u | (mantissa << 13);
        }
        else
        {
            bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
        }

        float value = 0.0f;
        std::memcpy (&value, &bits, sizeof (value));
        return value;
    }

    /*  ONE ROW OF PIXELS AS FLOATS, from the bytes a backend read back. */
    inline void rowToFloats (const std::uint8_t* row, int width, sg_pixel_format format, float* out) noexcept
    {
        for (int x = 0; x < width; ++x)
        {
            auto* pixel = out + 4 * x;

            if (format == SG_PIXELFORMAT_RGBA16F)
            {
                std::uint16_t halves[4];
                std::memcpy (halves, row + 8 * x, sizeof (halves));

                for (int c = 0; c < 4; ++c)
                    pixel[c] = halfToFloat (halves[c]);
            }
            else if (format == SG_PIXELFORMAT_RGBA16)
            {
                std::uint16_t units[4];
                std::memcpy (units, row + 8 * x, sizeof (units));

                for (int c = 0; c < 4; ++c)
                    pixel[c] = static_cast<float> (units[c]) / 65535.0f;
            }
            else if (format == SG_PIXELFORMAT_RGBA32F)
            {
                std::memcpy (pixel, row + 16 * x, 4 * sizeof (float));
            }
            else
            {
                const auto* bytes = row + 4 * x;
                const auto bgra = format == SG_PIXELFORMAT_BGRA8;
                pixel[0] = static_cast<float> (bytes[bgra ? 2 : 0]) / 255.0f;
                pixel[1] = static_cast<float> (bytes[1]) / 255.0f;
                pixel[2] = static_cast<float> (bytes[bgra ? 0 : 2]) / 255.0f;
                pixel[3] = static_cast<float> (bytes[3]) / 255.0f;
            }
        }
    }

    /*  Bytes a pixel of a format a picture may be read back in; 0 for any other. */
    inline int bytesPerPixel (sg_pixel_format format) noexcept
    {
        //  Not a switch: sokol's enum has fifty more, and -Wswitch-enum names each.
        if (format == SG_PIXELFORMAT_RGBA8 || format == SG_PIXELFORMAT_BGRA8)
            return 4;

        if (format == SG_PIXELFORMAT_RGBA16F || format == SG_PIXELFORMAT_RGBA16)
            return 8;

        return format == SG_PIXELFORMAT_RGBA32F ? 16 : 0;
    }
}
