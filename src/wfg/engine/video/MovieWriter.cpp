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

#include <wfg/engine/video/MovieWriter.h>

#include <juce_core/juce_core.h>

#include <cstring>
#include <initializer_list>
#include <vector>

namespace wfg::video::movie
{
    namespace
    {
        using Bytes = std::vector<std::uint8_t>;

        void be16 (Bytes& out, std::uint32_t value)
        {
            out.push_back (static_cast<std::uint8_t> ((value >> 8) & 0xffu));
            out.push_back (static_cast<std::uint8_t> (value & 0xffu));
        }

        void be32 (Bytes& out, std::uint32_t value)
        {
            for (int shift = 24; shift >= 0; shift -= 8)
                out.push_back (static_cast<std::uint8_t> ((value >> shift) & 0xffu));
        }

        void be64 (Bytes& out, std::uint64_t value)
        {
            for (int shift = 56; shift >= 0; shift -= 8)
                out.push_back (static_cast<std::uint8_t> ((value >> shift) & 0xffu));
        }

        void zeros (Bytes& out, std::size_t count)
        {
            out.insert (out.end(), count, 0);
        }

        void fourcc (Bytes& out, const char* type)
        {
            out.insert (out.end(), type, type + 4);
        }

        Bytes box (const char* type, const Bytes& body)
        {
            Bytes out;
            be32 (out, static_cast<std::uint32_t> (body.size() + 8));
            fourcc (out, type);
            out.insert (out.end(), body.begin(), body.end());
            return out;
        }

        Bytes join (std::initializer_list<Bytes> parts)
        {
            Bytes out;

            for (const auto& part : parts)
                out.insert (out.end(), part.begin(), part.end());

            return out;
        }

        //  THE IDENTITY, as QuickTime writes a matrix: 16.16 but for the last column's 2.30.
        void matrix (Bytes& out)
        {
            for (const auto value : { 0x00010000u, 0u, 0u, 0u, 0x00010000u, 0u, 0u, 0u, 0x40000000u })
                be32 (out, value);
        }

        Bytes handler (const char* componentType, const char* subtype, const char* name)
        {
            Bytes body;
            zeros (body, 4);
            fourcc (body, componentType);
            fourcc (body, subtype);
            zeros (body, 12);

            const auto length = std::strlen (name);
            body.push_back (static_cast<std::uint8_t> (length));
            body.insert (body.end(), name, name + length);
            return box ("hdlr", body);
        }
    }

    struct MovieWriter::Impl
    {
        std::unique_ptr<juce::FileOutputStream> stream;
        juce::File file;
        char codec[4] {};
        int width = 0;
        int height = 0;
        std::uint32_t timeScale = 0;
        std::uint32_t frameDuration = 0;
        std::int64_t mdatAt = 0;
        std::uint64_t written = 0;
        std::vector<std::uint32_t> sizes;
        std::vector<std::uint64_t> offsets;
    };

    MovieWriter::MovieWriter() : impl (std::make_unique<Impl>()) {}
    MovieWriter::~MovieWriter() = default;

    std::size_t MovieWriter::frames() const noexcept
    {
        return impl->sizes.size();
    }

    bool MovieWriter::open (const std::string& path, const char* codec, int width, int height,
                            std::uint32_t timeScale, std::uint32_t frameDuration, std::string& why)
    {
        *impl = Impl {};

        if (width <= 0 || height <= 0 || width > 0xffff || height > 0xffff || timeScale == 0 || frameDuration == 0
              || codec == nullptr || std::strlen (codec) != 4)
        {
            why = "a movie of no size or no rate";
            return false;
        }

        impl->file = juce::File (juce::String::fromUTF8 (path.c_str()));
        impl->file.deleteFile();
        impl->stream = std::make_unique<juce::FileOutputStream> (impl->file);

        if (! impl->stream->openedOk())
        {
            why = "the file could not be made";
            impl->stream.reset();
            return false;
        }

        std::memcpy (impl->codec, codec, 4);
        impl->width = width;
        impl->height = height;
        impl->timeScale = timeScale;
        impl->frameDuration = frameDuration;

        Bytes head;
        Bytes ftyp;
        fourcc (ftyp, "qt  ");
        be32 (ftyp, 0x00000200u);
        fourcc (ftyp, "qt  ");
        head = box ("ftyp", ftyp);

        /*  THE MEDIA'S BOX, its size one - "see the 64 bits after" - and those
            patched at the end. */
        impl->mdatAt = static_cast<std::int64_t> (head.size());
        be32 (head, 1);
        fourcc (head, "mdat");
        be64 (head, 0);

        if (! impl->stream->write (head.data(), head.size()))
        {
            why = "the file could not be written";
            return false;
        }

        impl->written = head.size();
        return true;
    }

    bool MovieWriter::write (const std::uint8_t* data, std::size_t size)
    {
        if (impl->stream == nullptr || size == 0 || size > 0xffffffffu)
            return false;

        if (! impl->stream->write (data, size))
            return false;

        impl->offsets.push_back (impl->written);
        impl->sizes.push_back (static_cast<std::uint32_t> (size));
        impl->written += size;
        return true;
    }

    bool MovieWriter::finish (std::string& why)
    {
        if (impl->stream == nullptr)
        {
            why = "the movie was not open";
            return false;
        }

        const auto count = static_cast<std::uint32_t> (impl->sizes.size());
        const auto duration = static_cast<std::uint64_t> (count) * impl->frameDuration;
        const auto duration32 = static_cast<std::uint32_t> (std::min<std::uint64_t> (duration, 0xffffffffu));
        const auto alpha = std::memcmp (impl->codec, "Hap1", 4) != 0 && std::memcmp (impl->codec, "HapY", 4) != 0;

        //  mvhd
        Bytes mvhd;
        zeros (mvhd, 12);
        be32 (mvhd, impl->timeScale);
        be32 (mvhd, duration32);
        be32 (mvhd, 0x00010000u);
        be16 (mvhd, 0x0100u);
        zeros (mvhd, 10);
        matrix (mvhd);
        zeros (mvhd, 24);
        be32 (mvhd, 2);

        //  tkhd: enabled, in the movie, the preview and the poster.
        Bytes tkhd;
        be32 (tkhd, 0x0000000fu);
        zeros (tkhd, 8);
        be32 (tkhd, 1);
        zeros (tkhd, 4);
        be32 (tkhd, duration32);
        zeros (tkhd, 8);
        zeros (tkhd, 8);
        matrix (tkhd);
        be32 (tkhd, static_cast<std::uint32_t> (impl->width) << 16);
        be32 (tkhd, static_cast<std::uint32_t> (impl->height) << 16);

        Bytes mdhd;
        zeros (mdhd, 12);
        be32 (mdhd, impl->timeScale);
        be32 (mdhd, duration32);
        zeros (mdhd, 4);

        Bytes vmhd;
        be32 (vmhd, 0x00000001u);
        be16 (vmhd, 0x0040u);
        zeros (vmhd, 6);

        Bytes dref;
        zeros (dref, 4);
        be32 (dref, 1);
        Bytes alis;
        be32 (alis, 0x00000001u);
        const auto aliasEntry = box ("alis", alis);
        dref.insert (dref.end(), aliasEntry.begin(), aliasEntry.end());

        //  THE SAMPLE DESCRIPTION: what the frames are, and how large.
        Bytes entry;
        zeros (entry, 6);
        be16 (entry, 1);
        zeros (entry, 8);
        zeros (entry, 4);
        be32 (entry, 0x00000200u);
        be16 (entry, static_cast<std::uint32_t> (impl->width));
        be16 (entry, static_cast<std::uint32_t> (impl->height));
        be32 (entry, 0x00480000u);
        be32 (entry, 0x00480000u);
        zeros (entry, 4);
        be16 (entry, 1);

        const char* name = std::memcmp (impl->codec, "HapY", 4) == 0 ? "Hap Q"
                         : std::memcmp (impl->codec, "Hap5", 4) == 0 ? "Hap Alpha" : "Hap";
        Bytes compressor (32, 0);
        compressor[0] = static_cast<std::uint8_t> (std::strlen (name));
        std::memcpy (compressor.data() + 1, name, std::strlen (name));
        entry.insert (entry.end(), compressor.begin(), compressor.end());
        be16 (entry, alpha ? 32u : 24u);
        be16 (entry, 0xffffu);

        char codec[5] {};
        std::memcpy (codec, impl->codec, 4);

        Bytes stsd;
        zeros (stsd, 4);
        be32 (stsd, 1);
        const auto sample = box (codec, entry);
        stsd.insert (stsd.end(), sample.begin(), sample.end());

        Bytes stts;
        zeros (stts, 4);
        be32 (stts, 1);
        be32 (stts, count);
        be32 (stts, impl->frameDuration);

        Bytes stsc;
        zeros (stsc, 4);
        be32 (stsc, 1);
        be32 (stsc, 1);
        be32 (stsc, 1);
        be32 (stsc, 1);

        Bytes stsz;
        zeros (stsz, 4);
        be32 (stsz, 0);
        be32 (stsz, count);

        for (const auto size : impl->sizes)
            be32 (stsz, size);

        Bytes co64;
        zeros (co64, 4);
        be32 (co64, count);

        for (const auto offset : impl->offsets)
            be64 (co64, offset);

        const auto stbl = box ("stbl", join ({ box ("stsd", stsd), box ("stts", stts), box ("stsc", stsc),
                                               box ("stsz", stsz), box ("co64", co64) }));
        const auto minf = box ("minf", join ({ box ("vmhd", vmhd), handler ("dhlr", "alis", "DataHandler"),
                                               box ("dinf", box ("dref", dref)), stbl }));
        const auto mdia = box ("mdia", join ({ box ("mdhd", mdhd), handler ("mhlr", "vide", "VideoHandler"), minf }));
        const auto trak = box ("trak", join ({ box ("tkhd", tkhd), mdia }));
        const auto moov = box ("moov", join ({ box ("mvhd", mvhd), trak }));

        /*  THE MEDIA'S SIZE, now it is known; then the index after it. */
        const auto mdatSize = impl->written - static_cast<std::uint64_t> (impl->mdatAt);
        Bytes size;
        be64 (size, mdatSize);

        const auto end = impl->stream->getPosition();
        auto ok = impl->stream->setPosition (impl->mdatAt + 8)
               && impl->stream->write (size.data(), size.size())
               && impl->stream->setPosition (end)
               && impl->stream->write (moov.data(), moov.size());

        impl->stream->flush();
        ok = ok && impl->stream->getStatus().wasOk();
        impl->stream.reset();

        if (! ok)
            why = "the file could not be written";

        return ok;
    }
}
