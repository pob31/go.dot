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
    FONTS FOR THE TESTS (namespace draft §62): font files built byte by byte -
    a name table, an OS/2, a head, never a glyph - for what is read from a
    font's tables; DejaVu Sans Mono's four faces, which Pure Data's folder
    carries under a licence that allows it, for what is drawn; and a
    collection made of real files, its offsets moved.
*/

#include <juce_core/juce_core.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wfg::test::fonts
{
    inline void put16 (std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t v)
    {
        out[at] = static_cast<std::uint8_t> ((v >> 8) & 0xFF);
        out[at + 1] = static_cast<std::uint8_t> (v & 0xFF);
    }

    inline void put32 (std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t v)
    {
        put16 (out, at, v >> 16);
        put16 (out, at + 2, v & 0xFFFF);
    }

    inline std::uint32_t get32 (const std::vector<std::uint8_t>& in, std::size_t at)
    {
        return (static_cast<std::uint32_t> (in[at]) << 24) | (static_cast<std::uint32_t> (in[at + 1]) << 16)
             | (static_cast<std::uint32_t> (in[at + 2]) << 8) | static_cast<std::uint32_t> (in[at + 3]);
    }

    inline void push16 (std::vector<std::uint8_t>& out, std::uint32_t v)
    {
        out.push_back (static_cast<std::uint8_t> ((v >> 8) & 0xFF));
        out.push_back (static_cast<std::uint8_t> (v & 0xFF));
    }

    /** UTF-8 to UTF-16, big-endian, as Windows' and Unicode's names are stored. */
    inline std::vector<std::uint8_t> utf16 (const std::string& text)
    {
        std::vector<std::uint8_t> out;
        const auto string = juce::String::fromUTF8 (text.c_str());     // kept alive while its UTF-16 is read
        const auto wide = string.toUTF16();
        for (auto p = wide.getAddress(); *p != 0; ++p)
            push16 (out, static_cast<std::uint32_t> (*p));
        return out;
    }

    struct Name
    {
        std::uint16_t platform = 3, encoding = 1, language = 0x0409, id = 1;
        std::vector<std::uint8_t> bytes;
    };

    inline Name windows (std::uint16_t id, const std::string& text, std::uint16_t language = 0x0409)
    {
        return { 3, 1, language, id, utf16 (text) };
    }

    inline Name mac (std::uint16_t id, std::vector<std::uint8_t> romanBytes)
    {
        return { 1, 0, 0, id, std::move (romanBytes) };
    }

    /*  A face's tables: a name table, and an OS/2 or a head when given. The
        glyphs are not there - the reader never asks for them. */
    struct FaceBuilder
    {
        std::vector<Name> names;
        std::optional<int> weight;
        std::uint16_t embedding = 0;
        std::uint16_t selection = 0x0040;   // REGULAR
        std::optional<std::uint16_t> macStyle;

        std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> tables() const
        {
            std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> out;

            std::vector<std::uint8_t> name;
            push16 (name, 0);
            push16 (name, static_cast<std::uint32_t> (names.size()));
            push16 (name, static_cast<std::uint32_t> (6 + 12 * names.size()));
            std::vector<std::uint8_t> strings;
            for (const auto& n : names)
            {
                push16 (name, n.platform);
                push16 (name, n.encoding);
                push16 (name, n.language);
                push16 (name, n.id);
                push16 (name, static_cast<std::uint32_t> (n.bytes.size()));
                push16 (name, static_cast<std::uint32_t> (strings.size()));
                strings.insert (strings.end(), n.bytes.begin(), n.bytes.end());
            }
            name.insert (name.end(), strings.begin(), strings.end());
            out.emplace_back (0x6E616D65u, name);   // 'name'

            if (weight.has_value())
            {
                std::vector<std::uint8_t> os2 (96, 0);
                put16 (os2, 0, 4);
                put16 (os2, 4, static_cast<std::uint32_t> (*weight));
                put16 (os2, 8, embedding);
                put16 (os2, 62, selection);
                out.emplace_back (0x4F532F32u, os2);    // 'OS/2'
            }
            if (macStyle.has_value())
            {
                std::vector<std::uint8_t> head (54, 0);
                put32 (head, 0, 0x00010000u);
                put32 (head, 12, 0x5F0F3CF5u);
                put16 (head, 44, *macStyle);
                out.emplace_back (0x68656164u, head);   // 'head'
            }
            return out;
        }

        /** The face's table directory and tables, its offsets counted from `base`. */
        std::vector<std::uint8_t> bytes (std::size_t base = 0) const
        {
            const auto all = tables();
            std::vector<std::uint8_t> out (12 + 16 * all.size(), 0);
            put32 (out, 0, 0x00010000u);
            put16 (out, 4, static_cast<std::uint32_t> (all.size()));
            for (std::size_t t = 0; t < all.size(); ++t)
            {
                while (out.size() % 4 != 0)
                    out.push_back (0);
                const auto record = 12 + 16 * t;
                put32 (out, record, all[t].first);
                put32 (out, record + 8, static_cast<std::uint32_t> (base + out.size()));
                put32 (out, record + 12, static_cast<std::uint32_t> (all[t].second.size()));
                out.insert (out.end(), all[t].second.begin(), all[t].second.end());
            }
            return out;
        }
    };

    inline FaceBuilder plain (const std::string& family, const std::string& style, int weight = 400)
    {
        FaceBuilder face;
        face.names = { windows (1, family), windows (2, style), windows (4, family + " " + style),
                       windows (6, family + "-" + style) };
        face.weight = weight;
        return face;
    }

    /*  A COLLECTION OF WHOLE FONT FILES: each copied in after the header, the
        offsets in its table directory moved by where it now starts. */
    inline std::vector<std::uint8_t> collectionOf (const std::vector<std::vector<std::uint8_t>>& faces)
    {
        std::vector<std::uint8_t> out (12 + 4 * faces.size(), 0);
        put32 (out, 0, 0x74746366u);    // 'ttcf'
        put32 (out, 4, 0x00010000u);
        put32 (out, 8, static_cast<std::uint32_t> (faces.size()));

        for (std::size_t f = 0; f < faces.size(); ++f)
        {
            while (out.size() % 4 != 0)
                out.push_back (0);
            const auto base = out.size();
            put32 (out, 12 + 4 * f, static_cast<std::uint32_t> (base));
            out.insert (out.end(), faces[f].begin(), faces[f].end());

            const auto tables = (static_cast<std::size_t> (out[base + 4]) << 8) | out[base + 5];
            for (std::size_t t = 0; t < tables; ++t)
            {
                const auto record = base + 12 + t * 16;
                put32 (out, record + 8, get32 (out, record + 8) + static_cast<std::uint32_t> (base));
            }
        }
        return out;
    }

    /** One of DejaVu Sans Mono's four files, from Pure Data's own folder. */
    inline juce::File dejaVu (const char* fileName)
    {
        return juce::File (WFG_REPO_ROOT).getChildFile ("ThirdParty/pure-data/font").getChildFile (fileName);
    }

    inline std::vector<std::uint8_t> bytesOf (const juce::File& file)
    {
        juce::MemoryBlock data;
        file.loadFileAsData (data);
        const auto* begin = static_cast<const std::uint8_t*> (data.getData());
        return { begin, begin + data.getSize() };
    }

    inline void write (const juce::File& file, const std::vector<std::uint8_t>& bytes)
    {
        file.getParentDirectory().createDirectory();
        file.replaceWithData (bytes.data(), bytes.size());
    }

    /** A folder of its own for a test, deleted when it ends. */
    struct Folder
    {
        juce::File file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getNonexistentChildFile ("godot-fonts", {}, false);

        Folder() { file.createDirectory(); }
        ~Folder() { file.deleteRecursively(); }

        Folder (const Folder&) = delete;
        Folder& operator= (const Folder&) = delete;
    };
}
