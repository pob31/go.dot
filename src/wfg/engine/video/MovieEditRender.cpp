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

#include "MovieEditRender.h"

#include <wfg/engine/video/Hap.h>
#include <wfg/engine/video/HapEncoder.h>
#include <wfg/engine/video/Movie.h>
#include <wfg/engine/video/MovieWriter.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <thread>

namespace wfg::video::movie
{
    namespace
    {
        bool stopRequested (const std::atomic<bool>* stop) noexcept
        {
            return stop != nullptr && stop->load (std::memory_order_relaxed);
        }

        hap::Texture textureOf (const std::string& codec) noexcept
        {
            return codec == "Hap1" ? hap::Texture::dxt1
                 : codec == "Hap5" ? hap::Texture::dxt5
                 : codec == "HapY" ? hap::Texture::ycocgDxt5
                                   : hap::Texture::none;
        }

        /*  A SOURCE SECOND AS STRAIGHT RGBA: the frame showing then, read,
            unpacked and decoded; black beyond either end of the file - clear
            where the codec carries alpha (ADU). */
        struct Decoder
        {
            MovieFile& file;
            hap::Texture texture;
            int width, height;

            std::vector<std::uint8_t> bytes, blocks;

            bool at (double second, std::vector<std::uint8_t>& rgba, std::string& why)
            {
                const auto& info = file.info();

                if (second < 0.0 || second >= info.duration)
                {
                    black (rgba);
                    return true;
                }

                const auto index = info.frameAt (second);

                if (index < 0 || ! file.readFrame (index, bytes))
                {
                    why = "a frame of the movie could not be read";
                    return false;
                }

                auto got = hap::Texture::none;

                if (! hap::unpack (bytes.data(), bytes.size(), got, blocks) || got != texture)
                {
                    why = "a frame of the movie would not unpack";
                    return false;
                }

                if (! hap::decodeTexture (texture, blocks, width, height, rgba))
                {
                    why = "a frame of the movie would not decode";
                    return false;
                }

                return true;
            }

            void black (std::vector<std::uint8_t>& rgba) const
            {
                rgba.assign (static_cast<std::size_t> (width) * static_cast<std::size_t> (height) * 4, 0);

                if (texture != hap::Texture::dxt5)
                    for (std::size_t p = 3; p < rgba.size(); p += 4)
                        rgba[p] = 255;
            }
        };

        /*  A JOIN'S DISSOLVE: centred on the join into section `into`, as
            wide as the crossfade into it. */
        struct Window
        {
            double from = 0.0, to = 0.0, join = 0.0;
            std::size_t into = 0;
        };
    }

    MovieRenderResult renderMovieEdit (const std::string& sourcePath, const std::vector<doc::Section>& sections,
                                       const std::string& targetPath, const std::atomic<bool>* stop,
                                       const std::function<void (int)>& progress)
    {
        MovieRenderResult result;

        const auto fail = [&result] (const std::string& why)
        {
            result.problem = why;
            return result;
        };

        if (sections.empty())
            return fail ("there are no sections to render");

        if (const auto why = doc::whyNotSections (sections); ! why.empty())
            return fail (why);

        MovieFile source;
        std::string why;

        if (! source.open (sourcePath, why))
            return fail ("the movie could not be read: " + why);

        const auto& info = source.info();

        if (! info.isHap())
            return fail (convertFirst);

        if (info.timeScale == 0 || info.frameDuration == 0 || info.frames.empty() || ! (info.duration > 0.0))
            return fail ("the movie has no frames to render");

        const auto texture = textureOf (info.codec);
        const auto delta = static_cast<double> (info.frameDuration) / static_cast<double> (info.timeScale);

        /*  THE EDITED TIMELINE on the grid: as many frames as fit the sections
            put together, each judged at its centre. */
        const auto clamped = doc::clampCrossfades (sections);
        const auto starts = doc::sectionStarts (clamped);
        const auto length = doc::editedLength (clamped);
        const auto count = std::max<std::int64_t> (1, std::llround (length / delta));

        std::vector<Window> windows;

        for (std::size_t j = 1; j < clamped.size(); ++j)
        {
            const auto fade = doc::crossfadeInto (clamped, j);

            if (fade > 0.0 && ! doc::isContinuousJoin (clamped[j - 1], clamped[j]))
                windows.push_back ({ starts[j] - fade / 2.0, starts[j] + fade / 2.0, starts[j], j });
        }

        const juce::File target { juce::String::fromUTF8 (targetPath.c_str()) };
        target.getParentDirectory().createDirectory();
        const auto part = target.getSiblingFile (target.getFileName() + ".part");
        part.deleteFile();

        MovieWriter writer;

        if (! writer.open (part.getFullPathName().toStdString(), info.codec.c_str(), info.width, info.height,
                           info.timeScale, info.frameDuration, why))
            return fail ("the render could not be written: " + why);

        const auto abandon = [&] (const std::string& reason)
        {
            std::string ignored;
            writer.finish (ignored);
            part.deleteFile();
            return fail (reason);
        };

        const auto threads = std::clamp (static_cast<int> (std::thread::hardware_concurrency()) - 2, 1, 16);
        Decoder decoder { source, texture, info.width, info.height, {}, {} };

        std::vector<std::uint8_t> outgoing, incoming, blended, blocks, frame, scratch, bytes, black;
        int lastIndex = -1;     // the source frame `bytes` holds, written verbatim

        const auto write = [&writer] (const std::vector<std::uint8_t>& data)
        {
            return writer.write (data.data(), data.size());
        };

        for (std::int64_t k = 0; k < count; ++k)
        {
            if (stopRequested (stop))
                return abandon ("stopped");

            const auto t = (static_cast<double> (k) + 0.5) * delta;

            //  WHICH SECTION this instant is in, and where that is in the file.
            auto j = clamped.size() - 1;

            if (const auto place = doc::placeOf (clamped, t))
                j = place->index;

            const auto fileSecond = clamped[j].in + (t - starts[j]);

            const Window* window = nullptr;

            for (const auto& candidate : windows)
                if (t >= candidate.from && t < candidate.to)
                {
                    window = &candidate;
                    break;
                }

            if (window != nullptr)
            {
                /*  THE DISSOLVE (ADU): linear in straight RGBA, the outgoing
                    section's material on past its out point, the incoming
                    section's from before its in point. */
                const auto share = (t - window->from) / (window->to - window->from);
                const auto outSecond = clamped[window->into - 1].out + (t - window->join);
                const auto inSecond = clamped[window->into].in + (t - window->join);

                if (! decoder.at (outSecond, outgoing, why) || ! decoder.at (inSecond, incoming, why))
                    return abandon (why);

                blended.resize (outgoing.size());

                for (std::size_t i = 0; i < blended.size(); ++i)
                {
                    const auto mixed = (1.0 - share) * static_cast<double> (outgoing[i]) + share * static_cast<double> (incoming[i]);
                    blended[i] = static_cast<std::uint8_t> (std::clamp (std::lround (mixed), 0l, 255l));
                }

                hap::encodeTextureThreaded (texture, blended.data(), info.width, info.height,
                                            static_cast<std::size_t> (info.width) * 4, blocks, threads);
                hap::packFrame (texture, blocks, frame, scratch);

                if (! write (frame))
                    return abandon ("the disk would not take the whole render");

                lastIndex = -1;
            }
            else if (fileSecond < 0.0 || fileSecond >= info.duration)
            {
                //  BEYOND THE FILE: black, encoded once.
                if (black.empty())
                {
                    decoder.black (blended);
                    hap::encodeTextureThreaded (texture, blended.data(), info.width, info.height,
                                                static_cast<std::size_t> (info.width) * 4, blocks, threads);
                    hap::packFrame (texture, blocks, black, scratch);
                }

                if (! write (black))
                    return abandon ("the disk would not take the whole render");

                lastIndex = -1;
            }
            else
            {
                //  THE SOURCE'S OWN BYTES (ADU), read once while the frame repeats.
                const auto index = info.frameAt (fileSecond);

                if (index != lastIndex)
                {
                    if (index < 0 || ! source.readFrame (index, bytes))
                        return abandon ("a frame of the movie could not be read");

                    lastIndex = index;
                }

                if (! write (bytes))
                    return abandon ("the disk would not take the whole render");
            }

            if (progress)
                progress (static_cast<int> (std::min<std::int64_t> (100, ((k + 1) * 100) / count)));
        }

        if (! writer.finish (why))
        {
            part.deleteFile();
            return fail ("the render could not be finished: " + why);
        }

        target.deleteFile();

        if (! part.moveFileTo (target))
        {
            part.deleteFile();
            return fail ("the render could not be moved into place");
        }

        result.ok = true;
        result.seconds = static_cast<double> (count) * delta;
        result.frames = static_cast<int> (count);
        result.frameRate = info.frameRate();
        result.resampled = ! info.constantRate;
        return result;
    }
}
