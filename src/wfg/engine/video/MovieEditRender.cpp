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

        /*  WHAT IS SEEN AT AN INSTANT (namespace draft 55.9, AEG): each section
            heard there, at the second of the file it shows and the weight its
            fades give it - linear raised to its curve. */
        struct Seen
        {
            double fileSecond = 0.0;
            double weight = 0.0;
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
            and their gaps, each judged at its centre. */
        const auto heard = doc::heardFades (sections);
        const auto starts = doc::sectionStarts (sections);
        const auto length = doc::editedLength (sections);
        const auto count = std::max<std::int64_t> (1, std::llround (length / delta));

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

        std::vector<std::uint8_t> outgoing, blended, blocks, frame, scratch, bytes, black;
        int lastIndex = -1;     // the source frame `bytes` holds, written verbatim

        const auto write = [&writer] (const std::vector<std::uint8_t>& data)
        {
            return writer.write (data.data(), data.size());
        };

        std::vector<Seen> seen;
        const auto alpha = texture == hap::Texture::dxt5;

        for (std::int64_t k = 0; k < count; ++k)
        {
            if (stopRequested (stop))
                return abandon ("stopped");

            const auto t = (static_cast<double> (k) + 0.5) * delta;

            seen.clear();

            for (std::size_t j = 0; j < sections.size(); ++j)
            {
                const auto sectionLength = sections[j].length();

                if (t < doc::heardFrom (starts[j], heard[j]) || t >= doc::heardTo (starts[j], sectionLength, heard[j]))
                    continue;

                if (const auto weight = doc::fadeWeight (heard[j], starts[j], sectionLength, t, true); weight > 0.0)
                    seen.push_back ({ sections[j].in + (t - starts[j]), weight });
            }

            const auto inFile = [&info] (double second) { return second >= 0.0 && second < info.duration; };

            if (seen.size() == 1 && ! (seen.front().weight < 1.0) && inFile (seen.front().fileSecond))
            {
                //  THE SOURCE'S OWN BYTES (ADU), read once while the frame repeats.
                const auto index = info.frameAt (seen.front().fileSecond);

                if (index != lastIndex)
                {
                    if (index < 0 || ! source.readFrame (index, bytes))
                        return abandon ("a frame of the movie could not be read");

                    lastIndex = index;
                }

                if (! write (bytes))
                    return abandon ("the disk would not take the whole render");
            }
            else if (seen.empty() || std::none_of (seen.begin(), seen.end(),
                                                   [&inFile] (const Seen& s) { return inFile (s.fileSecond); }))
            {
                //  NOTHING TO SHOW - a gap, or beyond the file: black, encoded once.
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
                /*  A DISSOLVE, OR A FADE FROM BLACK (AEG): each side's picture by
                    its weight, added in straight RGBA - the linear dissolve when
                    the two weights make one - black or clear where nothing is. */
                std::vector<double> sum (static_cast<std::size_t> (info.width) * static_cast<std::size_t> (info.height) * 4, 0.0);

                for (const auto& side : seen)
                {
                    if (! decoder.at (side.fileSecond, outgoing, why))
                        return abandon (why);

                    for (std::size_t i = 0; i < sum.size() && i < outgoing.size(); ++i)
                        sum[i] += side.weight * static_cast<double> (outgoing[i]);
                }

                blended.resize (sum.size());

                for (std::size_t i = 0; i < sum.size(); ++i)
                    blended[i] = static_cast<std::uint8_t> (std::clamp (std::lround (sum[i]), 0l, 255l));

                if (! alpha)
                    for (std::size_t i = 3; i < blended.size(); i += 4)
                        blended[i] = 255;

                hap::encodeTextureThreaded (texture, blended.data(), info.width, info.height,
                                            static_cast<std::size_t> (info.width) * 4, blocks, threads);
                hap::packFrame (texture, blocks, frame, scratch);

                if (! write (frame))
                    return abandon ("the disk would not take the whole render");

                lastIndex = -1;
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
