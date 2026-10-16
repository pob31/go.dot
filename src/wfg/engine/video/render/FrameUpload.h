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
    THE NEWEST PICTURE A RECEIVER HAS, AND ITS TEXTURE (namespace draft §72).

    A receiver that takes pictures in on a thread of its own - a card's input,
    a camera - keeps the newest frame here, BGRA, and the render thread uploads
    it once a frame however many cues show it: a new image where the device
    reads BGRA (Direct3D, Metal), its red and blue swapped where it does not
    (OpenGL). The buffer a frame was uploaded from is handed back to be filled
    again, so no frame after the first is a new allocation.

    `keep` and `spare` from the receiving thread; `upload`, `release` and the
    readers from the render thread.
*/

#include <sokol/sokol_gfx.h>

#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace wfg::video::render
{
    class NewestPicture
    {
    public:
        NewestPicture() = default;
        NewestPicture (const NewestPicture&) = delete;
        NewestPicture& operator= (const NewestPicture&) = delete;
        ~NewestPicture()   { release(); }

        /*  A buffer to fill with the next frame - one handed back, when there
            is one. */
        std::vector<std::uint8_t> spare()
        {
            const std::scoped_lock hold (lock);
            std::vector<std::uint8_t> out;
            out.swap (spareBuffer);
            return out;
        }

        /*  `bgra`, `width` by `height`, `width * 4` a row, as the newest. */
        void keep (std::vector<std::uint8_t>&& bgra, int width, int height)
        {
            const std::scoped_lock hold (lock);
            newest.swap (bgra);
            newestWidth = width;
            newestHeight = height;
            ++serial;

            if (spareBuffer.capacity() < bgra.capacity())
                spareBuffer.swap (bgra);
        }

        /*  The render thread's, once a frame: a newer frame made the texture. */
        void upload()
        {
            std::vector<std::uint8_t> pixels;
            int w = 0, h = 0;

            {
                const std::scoped_lock hold (lock);

                if (serial == uploaded)
                    return;

                uploaded = serial;
                pixels.swap (newest);
                w = newestWidth;
                h = newestHeight;
            }

            if (w <= 0 || h <= 0 || pixels.size() < static_cast<std::size_t> (w) * static_cast<std::size_t> (h) * 4)
                return;

            const auto bgra = sg_query_pixelformat (SG_PIXELFORMAT_BGRA8).sample;

            if (! bgra)
                for (std::size_t at = 0; at + 3 < pixels.size(); at += 4)
                    std::swap (pixels[at], pixels[at + 2]);

            release();

            sg_image_desc desc {};
            desc.width = w;
            desc.height = h;
            desc.pixel_format = bgra ? SG_PIXELFORMAT_BGRA8 : SG_PIXELFORMAT_RGBA8;
            desc.data.mip_levels[0].ptr = pixels.data();
            desc.data.mip_levels[0].size = static_cast<std::size_t> (w) * static_cast<std::size_t> (h) * 4;
            image = sg_make_image (desc);

            sg_view_desc view {};
            view.texture.image = image;
            textureView = sg_make_view (view);
            pictureWidth = w;
            pictureHeight = h;

            const std::scoped_lock hold (lock);

            if (spareBuffer.capacity() < pixels.capacity())
                spareBuffer.swap (pixels);
        }

        /*  The texture let go: nothing drawn until the next frame comes. */
        void release()
        {
            if (textureView.id != SG_INVALID_ID)
                sg_destroy_view (textureView);

            if (image.id != SG_INVALID_ID)
                sg_destroy_image (image);

            textureView = {};
            image = {};
        }

        sg_view view() const noexcept   { return textureView; }
        int width() const noexcept      { return pictureWidth; }
        int height() const noexcept     { return pictureHeight; }

    private:
        std::mutex lock;
        std::vector<std::uint8_t> newest, spareBuffer;
        int newestWidth = 0;
        int newestHeight = 0;
        std::uint64_t serial = 0;
        std::uint64_t uploaded = 0;

        sg_image image {};
        sg_view textureView {};
        int pictureWidth = 0;
        int pictureHeight = 0;
    };
}
