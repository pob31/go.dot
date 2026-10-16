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

#include <wfg/engine/video/render/Camera.h>

#include <wfg/engine/video/VideoModes.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>

namespace wfg::video::camera
{
    System& system()
    {
        static const std::unique_ptr<System> made =
            juce::SystemStats::getEnvironmentVariable ("WFG_CAMERA_FAKE", {}) == "1" ? makeFakeCameras() : makeSystemCameras();
        return *made;
    }

    bool isCamera (const std::string& id) noexcept
    {
        return id.rfind (idPrefix, 0) == 0;
    }

    std::string chooseAutomatic (const std::vector<std::string>& formats)
    {
        std::string best;
        bool bestFast = false;
        std::int64_t bestArea = -1;
        double bestRate = 0.0;

        for (const auto& word : formats)
        {
            const auto format = modes::parse (word);

            if (! format)
                continue;

            const auto rate = format->frameRate() * (format->interlaced ? 2.0 : 1.0);
            const auto fast = rate >= 24.5;
            const auto area = static_cast<std::int64_t> (format->width) * format->height;

            //  Fast enough first; then the largest; then the faster.
            const auto better = best.empty() || (fast && ! bestFast)
                             || (fast == bestFast && (area > bestArea || (area == bestArea && rate > bestRate)));

            if (better)
            {
                best = word;
                bestFast = fast;
                bestArea = area;
                bestRate = rate;
            }
        }

        return best;
    }

    void yuyvToBgra (const std::uint8_t* yuyv, int width, int height, int rowBytes, std::uint8_t* bgra, bool fullRange) noexcept
    {
        //  BT.601 for standard definition, BT.709 from 720 lines up.
        const auto hd = height >= 720;
        const auto a = hd ? 1.5748 : 1.402;
        const auto b = hd ? 0.187324 : 0.344136;
        const auto c = hd ? 0.468124 : 0.714136;
        const auto d = hd ? 1.8556 : 1.772;
        const auto lumaFrom = fullRange ? 0.0 : 16.0;
        const auto lumaSpan = fullRange ? 255.0 : 219.0;
        const auto chromaSpan = fullRange ? 255.0 : 224.0;

        const auto byte = [] (double v) noexcept
        {
            return static_cast<std::uint8_t> (std::clamp (std::lround (v * 255.0), 0L, 255L));
        };

        for (int y = 0; y < height; ++y)
        {
            const auto* row = yuyv + static_cast<std::size_t> (y) * static_cast<std::size_t> (rowBytes);
            auto* out = bgra + static_cast<std::size_t> (y) * static_cast<std::size_t> (width) * 4;

            for (int x = 0; x + 1 < width; x += 2)
            {
                const auto* group = row + static_cast<std::size_t> (x) * 2;
                const auto cb = (static_cast<double> (group[1]) - 128.0) / chromaSpan;
                const auto cr = (static_cast<double> (group[3]) - 128.0) / chromaSpan;

                for (int k = 0; k < 2; ++k)
                {
                    const auto luma = (static_cast<double> (group[2 * k]) - lumaFrom) / lumaSpan;
                    auto* px = out + static_cast<std::size_t> (x + k) * 4;
                    px[0] = byte (luma + d * cb);
                    px[1] = byte (luma - b * cb - c * cr);
                    px[2] = byte (luma + a * cr);
                    px[3] = 255;
                }
            }
        }
    }
}
