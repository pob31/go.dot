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
#include <chrono>
#include <cmath>
#include <mutex>
#include <set>

namespace wfg::video::camera
{
    namespace
    {
        struct Taken
        {
            std::mutex lock;
            std::set<std::string> ids;
        };

        /*  A Source that gives its camera back when it goes. */
        class Held final : public Source
        {
        public:
            Held (std::unique_ptr<Source> sourceToUse, std::shared_ptr<Taken> takenToUse, std::string idToUse)
                : source (std::move (sourceToUse)), taken (std::move (takenToUse)), id (std::move (idToUse))
            {
            }

            ~Held() override
            {
                source.reset();
                const std::scoped_lock hold (taken->lock);
                taken->ids.erase (id);
            }

            bool newest (std::vector<std::uint8_t>& bgra, int& width, int& height, std::uint64_t& serial) override
            {
                return source->newest (bgra, width, height, serial);
            }

            bool signal() override                        { return source->signal(); }
            std::string formatWord() override             { return source->formatWord(); }
            std::string problem() override                { return source->problem(); }
            std::vector<std::string> formats() override   { return source->formats(); }

        private:
            std::unique_ptr<Source> source;
            const std::shared_ptr<Taken> taken;
            const std::string id;
        };

        class OneInputEach final : public System
        {
        public:
            explicit OneInputEach (std::unique_ptr<System> camerasToUse) : cameras (std::move (camerasToUse)) {}

            Found find() override   { return cameras->find(); }

            std::unique_ptr<Source> open (const std::string& id, const std::string& format, std::string& why) override
            {
                {
                    const std::scoped_lock hold (taken->lock);

                    if (! taken->ids.insert (id).second)
                    {
                        why = "the camera is already taken";
                        return nullptr;
                    }
                }

                auto source = cameras->open (id, format, why);

                if (source == nullptr)
                {
                    const std::scoped_lock hold (taken->lock);
                    taken->ids.erase (id);
                    return nullptr;
                }

                return std::make_unique<Held> (std::move (source), taken, id);
            }

        private:
            const std::unique_ptr<System> cameras;
            const std::shared_ptr<Taken> taken = std::make_shared<Taken>();
        };
    }

    std::unique_ptr<System> oneInputEach (std::unique_ptr<System> cameras)
    {
        return std::make_unique<OneInputEach> (std::move (cameras));
    }

    System& system()
    {
        static const std::unique_ptr<System> made =
            oneInputEach (juce::SystemStats::getEnvironmentVariable ("WFG_CAMERA_FAKE", {}) == "1" ? makeFakeCameras() : makeSystemCameras());
        return *made;
    }

    std::int64_t nowNanos() noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds> (std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    bool arrivedLately (std::int64_t arrivedAt) noexcept
    {
        return arrivedAt != 0 && nowNanos() - arrivedAt < 1'000'000'000;
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
