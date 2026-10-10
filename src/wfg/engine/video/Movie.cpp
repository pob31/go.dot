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

#include <wfg/engine/video/Movie.h>
#include <wfg/engine/video/Ffmpeg.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstring>

namespace wfg::video::movie
{
    namespace
    {
        std::uint32_t be32 (const std::uint8_t* at) noexcept
        {
            return (static_cast<std::uint32_t> (at[0]) << 24) | (static_cast<std::uint32_t> (at[1]) << 16)
                 | (static_cast<std::uint32_t> (at[2]) << 8) | static_cast<std::uint32_t> (at[3]);
        }

        std::uint64_t be64 (const std::uint8_t* at) noexcept
        {
            return (static_cast<std::uint64_t> (be32 (at)) << 32) | be32 (at + 4);
        }

        std::uint16_t be16 (const std::uint8_t* at) noexcept
        {
            return static_cast<std::uint16_t> ((at[0] << 8) | at[1]);
        }

        struct Box
        {
            const std::uint8_t* body = nullptr;
            std::size_t size = 0;
            char type[5] {};
        };

        /*  EVERY BOX DIRECTLY INSIDE `data`, in order: a size and a type, the
            size 1 meaning a 64-bit size after the type, and 0 meaning to the
            end. A box that runs past its parent ends the walk. */
        std::vector<Box> boxesIn (const std::uint8_t* data, std::size_t size)
        {
            std::vector<Box> out;

            for (std::size_t at = 0; at + 8 <= size;)
            {
                std::uint64_t length = be32 (data + at);
                std::size_t header = 8;

                if (length == 1)
                {
                    if (at + 16 > size)
                        break;

                    length = be64 (data + at + 8);
                    header = 16;
                }
                else if (length == 0)
                {
                    length = size - at;
                }

                if (length < header || at + length > size)
                    break;

                Box box;
                std::memcpy (box.type, data + at + 4, 4);
                box.body = data + at + header;
                box.size = static_cast<std::size_t> (length) - header;
                out.push_back (box);

                at += static_cast<std::size_t> (length);
            }

            return out;
        }

        const Box* find (const std::vector<Box>& boxes, const char* type)
        {
            for (const auto& box : boxes)
                if (std::memcmp (box.type, type, 4) == 0)
                    return &box;

            return nullptr;
        }

        //  A full box's version and flags come first; tables follow.
        bool tableOf (const Box* box, std::size_t entryBytes, std::uint32_t& count, const std::uint8_t*& entries)
        {
            if (box == nullptr || box->size < 8)
                return false;

            count = be32 (box->body + 4);
            entries = box->body + 8;
            return static_cast<std::size_t> (count) * entryBytes <= box->size - 8;
        }
    }

    int Info::frameAt (double seconds) const noexcept
    {
        if (frames.empty())
            return -1;

        const auto after = std::upper_bound (frames.begin(), frames.end(), seconds,
                                             [] (double at, const Frame& frame) { return at < frame.start; });

        if (after == frames.begin())
            return 0;

        return static_cast<int> (std::distance (frames.begin(), after)) - 1;
    }

    bool parse (const std::uint8_t* moov, std::size_t size, Info& out, std::string& why)
    {
        out = {};

        for (const auto& trak : boxesIn (moov, size))
        {
            if (std::memcmp (trak.type, "trak", 4) != 0)
                continue;

            const auto inTrack = boxesIn (trak.body, trak.size);
            const auto* mdia = find (inTrack, "mdia");

            if (mdia == nullptr)
                continue;

            const auto inMedia = boxesIn (mdia->body, mdia->size);
            const auto* hdlr = find (inMedia, "hdlr");
            const auto* mdhd = find (inMedia, "mdhd");
            const auto* minf = find (inMedia, "minf");

            //  A VIDEO TRACK: its handler says `vide`.
            if (hdlr == nullptr || hdlr->size < 12 || std::memcmp (hdlr->body + 8, "vide", 4) != 0
                  || mdhd == nullptr || minf == nullptr)
                continue;

            const auto version = mdhd->body[0];
            const auto timeScale = version == 1 ? (mdhd->size >= 24 ? be32 (mdhd->body + 20) : 0u)
                                                : (mdhd->size >= 16 ? be32 (mdhd->body + 12) : 0u);

            const auto inInfo = boxesIn (minf->body, minf->size);
            const auto* stbl = find (inInfo, "stbl");

            if (stbl == nullptr || timeScale == 0)
                continue;

            const auto tables = boxesIn (stbl->body, stbl->size);
            const auto* stsd = find (tables, "stsd");

            //  THE SAMPLE ENTRY: its codec, and its width and height 24 bytes into it.
            if (stsd == nullptr || stsd->size < 8 + 8 + 28)
            {
                why = "the video track has no sample description";
                return false;
            }

            const auto* entry = stsd->body + 8;
            out.codec.assign (reinterpret_cast<const char*> (entry + 4), 4);
            out.width = be16 (entry + 8 + 24);
            out.height = be16 (entry + 8 + 26);

            std::uint32_t timeCount = 0, chunkRuns = 0, chunkCount = 0;
            const std::uint8_t* times = nullptr;
            const std::uint8_t* runs = nullptr;
            const std::uint8_t* chunks = nullptr;
            const auto* co64 = find (tables, "co64");

            if (! tableOf (find (tables, "stts"), 8, timeCount, times)
                  || ! tableOf (find (tables, "stsc"), 12, chunkRuns, runs)
                  || ! tableOf (co64 != nullptr ? co64 : find (tables, "stco"), co64 != nullptr ? 8 : 4, chunkCount, chunks))
            {
                why = "the video track's sample tables will not read";
                return false;
            }

            //  SIZES: one for all, or a table of them.
            const auto* stsz = find (tables, "stsz");

            if (stsz == nullptr || stsz->size < 12)
            {
                why = "the video track has no sample sizes";
                return false;
            }

            const auto sameSize = be32 (stsz->body + 4);
            const auto sampleCount = be32 (stsz->body + 8);

            if (sameSize == 0 && static_cast<std::size_t> (sampleCount) * 4 > stsz->size - 12)
            {
                why = "the video track's sample sizes run short";
                return false;
            }

            out.frames.resize (sampleCount);

            for (std::uint32_t n = 0; n < sampleCount; ++n)
                out.frames[n].size = sameSize != 0 ? sameSize : be32 (stsz->body + 12 + 4 * n);

            //  TIMES: runs of frames of one duration each.
            std::uint64_t clock = 0;
            std::uint32_t frame = 0;

            /*  AND THE GRID (55.5): the duration the most frames have, and
                whether every frame has it. */
            std::uint32_t commonest = 0, commonestCount = 0, firstDelta = 0;
            out.timeScale = timeScale;
            out.constantRate = true;

            for (std::uint32_t run = 0; run < timeCount && frame < sampleCount; ++run)
            {
                const auto count = be32 (times + 8 * run);
                const auto delta = be32 (times + 8 * run + 4);

                if (count != 0 && delta != 0)
                {
                    if (firstDelta == 0)
                        firstDelta = delta;
                    else if (delta != firstDelta)
                        out.constantRate = false;

                    if (count > commonestCount)
                    {
                        commonest = delta;
                        commonestCount = count;
                    }
                }

                for (std::uint32_t n = 0; n < count && frame < sampleCount; ++n, ++frame)
                {
                    out.frames[frame].start = static_cast<double> (clock) / static_cast<double> (timeScale);
                    clock += delta;
                }
            }

            out.duration = static_cast<double> (clock) / static_cast<double> (timeScale);
            out.frameDuration = commonest;

            //  WHERE: chunk by chunk, each run of chunks holding so many frames, laid end to end.
            frame = 0;

            for (std::uint32_t chunk = 0; chunk < chunkCount && frame < sampleCount; ++chunk)
            {
                std::uint32_t perChunk = 0;

                for (std::uint32_t run = 0; run < chunkRuns; ++run)
                    if (be32 (runs + 12 * run) <= chunk + 1)
                        perChunk = be32 (runs + 12 * run + 4);

                auto offset = co64 != nullptr ? be64 (chunks + 8 * chunk) : static_cast<std::uint64_t> (be32 (chunks + 4 * chunk));

                for (std::uint32_t n = 0; n < perChunk && frame < sampleCount; ++n, ++frame)
                {
                    out.frames[frame].offset = offset;
                    offset += out.frames[frame].size;
                }
            }

            if (frame < sampleCount || out.frames.empty())
            {
                why = "the video track's frames are not all placed";
                return false;
            }

            return true;
        }

        why = "no video track";
        return false;
    }

    //==============================================================================
    struct MovieFile::Impl
    {
        std::unique_ptr<juce::FileInputStream> stream;
    };

    MovieFile::MovieFile() : impl (std::make_unique<Impl>()) {}
    MovieFile::~MovieFile() = default;

    bool MovieFile::open (const std::string& path, std::string& why)
    {
        impl->stream = std::make_unique<juce::FileInputStream> (juce::File (juce::String::fromUTF8 (path.c_str())));

        if (! impl->stream->openedOk())
        {
            why = "could not open " + path;
            impl->stream.reset();
            return false;
        }

        auto& in = *impl->stream;
        const auto total = in.getTotalLength();

        /*  THE TOP-LEVEL BOXES, walked by their headers alone - an `mdat` of
            gigabytes is stepped over - until `moov`, which is read whole. */
        for (std::int64_t at = 0; at + 8 <= total;)
        {
            std::uint8_t header[16];
            in.setPosition (at);

            if (in.read (header, 8) != 8)
                break;

            std::uint64_t length = be32 (header);
            std::int64_t headerBytes = 8;

            if (length == 1)
            {
                if (in.read (header + 8, 8) != 8)
                    break;

                length = be64 (header + 8);
                headerBytes = 16;
            }
            else if (length == 0)
            {
                length = static_cast<std::uint64_t> (total - at);
            }

            if (length < static_cast<std::uint64_t> (headerBytes))
                break;

            if (std::memcmp (header + 4, "moov", 4) == 0)
            {
                const auto bodySize = static_cast<std::size_t> (length) - static_cast<std::size_t> (headerBytes);

                if (bodySize > (std::size_t (64) << 20))
                {
                    why = "the movie's index is larger than Go.dot reads";
                    return false;
                }

                std::vector<std::uint8_t> moov (bodySize);
                in.setPosition (at + headerBytes);

                if (in.read (moov.data(), static_cast<int> (bodySize)) != static_cast<int> (bodySize))
                {
                    why = "the movie's index runs past the end of the file";
                    return false;
                }

                return parse (moov.data(), moov.size(), details, why);
            }

            at += static_cast<std::int64_t> (length);
        }

        why = "no movie index (moov) in " + path;
        return false;
    }

    bool MovieFile::readFrame (int index, std::vector<std::uint8_t>& out)
    {
        if (impl->stream == nullptr || index < 0 || index >= static_cast<int> (details.frames.size()))
            return false;

        const auto& frame = details.frames[static_cast<std::size_t> (index)];
        out.resize (frame.size);

        return impl->stream->setPosition (static_cast<std::int64_t> (frame.offset))
            && impl->stream->read (out.data(), static_cast<int> (frame.size)) == static_cast<int> (frame.size);
    }

    double durationOf (const std::string& path)
    {
        MovieFile file;
        std::string why;

        if (file.open (path, why) && file.info().duration > 0.0)
            return file.info().duration;

        /*  A MOVIE THAT IS NOT HAP (namespace draft 37.5, WF): its length as
            FFmpeg says it, where FFmpeg is here - what its preview plays for. */
        if (const auto tools = ffmpeg::find(); tools.found())
            if (const auto probed = ffmpeg::probe (tools, path); probed.ok && probed.duration > 0.0)
                return probed.duration;

        return -1.0;
    }
}
