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

#include <wfg/engine/video/StripAnalysis.h>

#include <wfg/engine/video/Hap.h>
#include <wfg/engine/video/Movie.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <thread>
#include <vector>

namespace wfg::video::strip
{
    namespace
    {
        //  Read below this, so a show playing off the same disk keeps its frames.
        constexpr double gentleBytesPerSecond = 150.0e6;

        //  Frames looked at a second, at most: a cut is found within this, then placed by halving.
        constexpr double lookedAtPerSecond = 7.5;

        /*  ONE FRAME DECODED: its blocks, and how to read a colour off them at
            (u, v) from its top-left. */
        struct Decoded
        {
            hap::Texture texture = hap::Texture::none;
            std::vector<std::uint8_t> blocks;
            int width = 0;
            int height = 0;

            bool colourAt (double u, double v, double& red, double& green, double& blue) const
            {
                if (texture == hap::Texture::none || width <= 0 || height <= 0)
                    return false;

                const auto padded = (width + 3) / 4 * 4;
                const auto paddedHeight = (height + 3) / 4 * 4;
                const auto x = std::clamp (static_cast<int> (u * width), 0, width - 1);
                const auto y = std::clamp (static_cast<int> (v * height), 0, height - 1);
                double alpha = 1.0;

                return hap::pixelAt (texture, blocks, padded, paddedHeight, x, y, red, green, blue, alpha);
            }
        };

        class Reader
        {
        public:
            Reader (movie::MovieFile& fileToRead, const std::atomic<bool>* stoppingToWatch)
                : file (fileToRead), stopping (stoppingToWatch), began (std::chrono::steady_clock::now()) {}

            bool stopped() const noexcept
            {
                return stopping != nullptr && stopping->load (std::memory_order_relaxed);
            }

            bool decode (int index, Decoded& out)
            {
                if (! file.readFrame (index, bytes))
                    return false;

                read += static_cast<double> (bytes.size());
                beGentle();

                out.width = file.info().width;
                out.height = file.info().height;
                return hap::unpack (bytes.data(), bytes.size(), out.texture, out.blocks);
            }

            Signature signatureAt (int index)
            {
                if (const auto found = signatures.find (index); found != signatures.end())
                    return found->second;

                Decoded frame;
                Signature signature;

                if (decode (index, frame))
                    signature = signatureOf ([&frame] (double u, double v, double& red, double& green, double& blue)
                                             { return frame.colourAt (u, v, red, green, blue); });

                signatures[index] = signature;
                return signature;
            }

        private:
            movie::MovieFile& file;
            const std::atomic<bool>* stopping;
            std::chrono::steady_clock::time_point began;
            std::vector<std::uint8_t> bytes;
            std::map<int, Signature> signatures;
            double read = 0.0;

            void beGentle()
            {
                const auto due = std::chrono::duration<double> (read / gentleBytesPerSecond);
                const auto spent = std::chrono::steady_clock::now() - began;

                if (spent < due)
                    std::this_thread::sleep_for (std::chrono::duration_cast<std::chrono::milliseconds> (due - spent));
            }
        };
    }

    bool analyseHap (const std::string& path, MovieStrip& out, std::string& why, const std::atomic<bool>* stopping)
    {
        movie::MovieFile file;

        if (! file.open (path, why))
            return false;

        const auto& info = file.info();

        if (! info.isHap() || info.frames.empty())
        {
            why = "not a HAP movie";
            return false;
        }

        Reader reader { file, stopping };
        const auto frameCount = static_cast<int> (info.frames.size());
        const auto rate = info.frameRate() > 0.0 ? info.frameRate() : 25.0;
        const auto every = std::max (1, static_cast<int> (std::lround (rate / lookedAtPerSecond)));

        //  THE CUTS: every few frames looked at, then each placed on its first frame.
        CutFinder finder;
        std::vector<int> looked;

        for (auto index = 0; index < frameCount; index += every)
        {
            if (reader.stopped())
            {
                why = "stopped";
                return false;
            }

            finder.add (info.frames[static_cast<std::size_t> (index)].start, reader.signatureAt (index));
            looked.push_back (index);
        }

        auto cuts = finder.finish();

        for (auto& cut : cuts)
        {
            if (cut.gradual)
                continue;

            //  The frame looked at where it was seen, and the one looked at before it.
            const auto seenAt = std::find_if (looked.begin(), looked.end(),
                                              [&] (int index) { return info.frames[static_cast<std::size_t> (index)].start >= cut.seconds - 1.0e-9; });

            if (seenAt == looked.end() || seenAt == looked.begin())
                continue;

            const auto first = firstFrameOfShot (*std::prev (seenAt), *seenAt,
                                                 [&reader] (int index) { return reader.signatureAt (index); });

            cut.seconds = info.frames[static_cast<std::size_t> (first)].start;
        }

        //  THE PICTURES, each 80 across in the movie's shape.
        MovieStrip strip;
        strip.duration = info.duration;
        strip.width = info.width;
        strip.height = info.height;
        strip.cuts = cuts;

        const auto height = thumbnailHeightFor (info.width, info.height);

        for (const auto seconds : thumbnailTimes (info.duration, cuts))
        {
            if (reader.stopped())
            {
                why = "stopped";
                return false;
            }

            const auto index = info.frameAt (seconds);
            Decoded frame;

            if (index < 0 || ! reader.decode (index, frame))
                continue;

            Thumbnail thumbnail;
            thumbnail.seconds = info.frames[static_cast<std::size_t> (index)].start;
            thumbnail.width = thumbnailWidth;
            thumbnail.height = height;
            thumbnail.rgb.resize (static_cast<std::size_t> (thumbnailWidth * height * 3));

            for (int row = 0; row < height; ++row)
                for (int column = 0; column < thumbnailWidth; ++column)
                {
                    double red = 0.0, green = 0.0, blue = 0.0;
                    frame.colourAt ((column + 0.5) / thumbnailWidth, (row + 0.5) / height, red, green, blue);

                    auto* pixel = thumbnail.rgb.data() + 3 * (row * thumbnailWidth + column);
                    pixel[0] = static_cast<std::uint8_t> (std::clamp (std::lround (red), 0L, 255L));
                    pixel[1] = static_cast<std::uint8_t> (std::clamp (std::lround (green), 0L, 255L));
                    pixel[2] = static_cast<std::uint8_t> (std::clamp (std::lround (blue), 0L, 255L));
                }

            if (strip.thumbnails.empty() || strip.thumbnails.back().seconds < thumbnail.seconds)
                strip.thumbnails.push_back (std::move (thumbnail));
        }

        out = std::move (strip);
        return true;
    }
}
