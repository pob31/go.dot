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

#include "FontFile.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <optional>

namespace wfg::video::fontfile
{
    namespace
    {
        /*  The file's bytes with every read checked: a value past the end is
            nought and marks the view broken, so a table that lies about its
            size ends the reading in words rather than in a crash. */
        struct Bytes
        {
            const std::uint8_t* data = nullptr;
            std::size_t size = 0;

            bool has (std::size_t at, std::size_t count) const noexcept
            {
                return at <= size && count <= size - at;
            }

            std::uint16_t u16 (std::size_t at) const noexcept
            {
                if (! has (at, 2))
                    return 0;
                return static_cast<std::uint16_t> ((data[at] << 8) | data[at + 1]);
            }

            std::uint32_t u32 (std::size_t at) const noexcept
            {
                if (! has (at, 4))
                    return 0;
                return (static_cast<std::uint32_t> (data[at]) << 24) | (static_cast<std::uint32_t> (data[at + 1]) << 16)
                     | (static_cast<std::uint32_t> (data[at + 2]) << 8) | static_cast<std::uint32_t> (data[at + 3]);
            }
        };

        constexpr std::uint32_t tag (char a, char b, char c, char d) noexcept
        {
            return (static_cast<std::uint32_t> (static_cast<unsigned char> (a)) << 24)
                 | (static_cast<std::uint32_t> (static_cast<unsigned char> (b)) << 16)
                 | (static_cast<std::uint32_t> (static_cast<unsigned char> (c)) << 8)
                 | static_cast<std::uint32_t> (static_cast<unsigned char> (d));
        }

        struct Table
        {
            std::size_t offset = 0;
            std::size_t length = 0;
        };

        void appendUtf8 (std::string& out, std::uint32_t code)
        {
            if (code < 0x80)
                out += static_cast<char> (code);
            else if (code < 0x800)
            {
                out += static_cast<char> (0xC0 | (code >> 6));
                out += static_cast<char> (0x80 | (code & 0x3F));
            }
            else if (code < 0x10000)
            {
                out += static_cast<char> (0xE0 | (code >> 12));
                out += static_cast<char> (0x80 | ((code >> 6) & 0x3F));
                out += static_cast<char> (0x80 | (code & 0x3F));
            }
            else
            {
                out += static_cast<char> (0xF0 | (code >> 18));
                out += static_cast<char> (0x80 | ((code >> 12) & 0x3F));
                out += static_cast<char> (0x80 | ((code >> 6) & 0x3F));
                out += static_cast<char> (0x80 | (code & 0x3F));
            }
        }

        std::string fromUtf16 (const Bytes& bytes, std::size_t at, std::size_t length)
        {
            std::string out;
            for (std::size_t i = 0; i + 1 < length; i += 2)
            {
                std::uint32_t code = bytes.u16 (at + i);
                if (code >= 0xD800 && code < 0xDC00 && i + 3 < length)
                {
                    const std::uint32_t low = bytes.u16 (at + i + 2);
                    if (low >= 0xDC00 && low < 0xE000)
                    {
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        i += 2;
                    }
                }
                if (code != 0)
                    appendUtf8 (out, code);
            }
            return out;
        }

        //  Mac OS Roman's upper half, the encoding of the Macintosh platform's names.
        constexpr std::array<std::uint16_t, 128> macRoman {
            0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
            0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
            0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
            0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
            0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
            0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
            0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
            0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7 };

        std::string fromMacRoman (const Bytes& bytes, std::size_t at, std::size_t length)
        {
            std::string out;
            for (std::size_t i = 0; i < length && bytes.has (at + i, 1); ++i)
            {
                const auto c = bytes.data[at + i];
                if (c != 0)
                    appendUtf8 (out, c < 0x80 ? static_cast<std::uint32_t> (c)
                                              : static_cast<std::uint32_t> (macRoman[static_cast<std::size_t> (c - 0x80)]));
            }
            return out;
        }

        /*  How much a name record is worth, when one name is said several ways:
            Windows' Unicode in American English first - what every tool reads -
            then Windows' in another English, in another language, the Unicode
            platform's, the Macintosh's English in Mac Roman, and last a
            Windows symbol font's. Nought: a record this cannot read. */
        int worth (std::uint16_t platform, std::uint16_t encoding, std::uint16_t language) noexcept
        {
            if (platform == 3 && (encoding == 1 || encoding == 10))
                return language == 0x0409 ? 7 : ((language & 0x03FF) == 0x09 ? 6 : 5);
            if (platform == 0)
                return 4;
            if (platform == 1 && encoding == 0)
                return language == 0 ? 3 : 2;
            if (platform == 3 && encoding == 0)
                return 1;
            return 0;
        }

        /** The best record of each name id from 1 to 17, read as UTF-8. */
        std::array<std::string, 18> namesOf (const Bytes& bytes, const Table& name)
        {
            std::array<std::string, 18> names {};
            std::array<int, 18> best {};

            const auto count = static_cast<std::size_t> (bytes.u16 (name.offset + 2));
            const auto strings = name.offset + static_cast<std::size_t> (bytes.u16 (name.offset + 4));
            if (count > 10000 || ! bytes.has (name.offset + 6, count * 12))
                return names;

            for (std::size_t r = 0; r < count; ++r)
            {
                const auto record = name.offset + 6 + r * 12;
                const auto platform = bytes.u16 (record);
                const auto encoding = bytes.u16 (record + 2);
                const auto language = bytes.u16 (record + 4);
                const auto id = static_cast<std::size_t> (bytes.u16 (record + 6));
                const auto length = static_cast<std::size_t> (bytes.u16 (record + 8));
                const auto at = strings + static_cast<std::size_t> (bytes.u16 (record + 10));

                const int value = worth (platform, encoding, language);
                if (id >= names.size() || value <= best[id] || ! bytes.has (at, length))
                    continue;

                auto text = platform == 1 ? fromMacRoman (bytes, at, length) : fromUtf16 (bytes, at, length);
                if (text.empty())
                    continue;
                names[id] = std::move (text);
                best[id] = value;
            }
            return names;
        }

        /** One face whose table directory starts at `at`; an error in words when it does not read. */
        std::optional<Face> faceAt (const Bytes& bytes, std::size_t at, int index, std::string& error)
        {
            const auto version = bytes.u32 (at);
            if (version != 0x00010000u && version != tag ('O', 'T', 'T', 'O') && version != tag ('t', 'r', 'u', 'e'))
            {
                error = "not a font file";
                return std::nullopt;
            }
            const auto tables = static_cast<std::size_t> (bytes.u16 (at + 4));
            if (tables == 0 || tables > 1000 || ! bytes.has (at + 12, tables * 16))
            {
                error = "its table of contents is cut short";
                return std::nullopt;
            }

            std::optional<Table> name, os2, head;
            for (std::size_t t = 0; t < tables; ++t)
            {
                const auto record = at + 12 + t * 16;
                const Table table { bytes.u32 (record + 8), bytes.u32 (record + 12) };
                if (! bytes.has (table.offset, table.length))
                    continue;   // a table past the end is not read; one the face needs is missed below
                const auto which = bytes.u32 (record);
                if (which == tag ('n', 'a', 'm', 'e'))
                    name = table;
                else if (which == tag ('O', 'S', '/', '2'))
                    os2 = table;
                else if (which == tag ('h', 'e', 'a', 'd'))
                    head = table;
            }
            if (! name.has_value() || name->length < 6)
            {
                error = "it has no names";
                return std::nullopt;
            }

            const auto names = namesOf (bytes, *name);
            Face face;
            face.index = index;
            face.legacyFamily = names[1];
            face.family = names[16].empty() ? names[1] : names[16];
            face.style = names[17].empty() ? names[2] : names[17];
            face.fullName = names[4];
            face.postScript = names[6];
            if (face.family.empty())
            {
                error = "it names no family";
                return std::nullopt;
            }

            if (os2.has_value() && os2->length >= 64)
            {
                const auto weight = static_cast<int> (bytes.u16 (os2->offset + 4));
                face.embedding = bytes.u16 (os2->offset + 8);
                const auto selection = bytes.u16 (os2->offset + 62);
                face.italic = (selection & 0x0001u) != 0 || (selection & 0x0200u) != 0;
                face.bold = (selection & 0x0020u) != 0;
                face.weight = weight >= 1 && weight <= 1000 ? weight : (face.bold ? 700 : 400);
            }
            else if (head.has_value() && head->length >= 46)
            {
                const auto style = bytes.u16 (head->offset + 44);
                face.bold = (style & 0x0001u) != 0;
                face.italic = (style & 0x0002u) != 0;
                face.weight = face.bold ? 700 : 400;
            }
            return face;
        }

        char folded (char c) noexcept
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char> (c - 'A' + 'a') : c;
        }
    }

    Reading read (const std::uint8_t* data, std::size_t size)
    {
        Reading reading;
        const Bytes bytes { data, data != nullptr ? size : 0 };

        if (! bytes.has (0, 12))
        {
            reading.error = "too short to be a font file";
            return reading;
        }

        std::vector<std::size_t> starts;
        if (bytes.u32 (0) == tag ('t', 't', 'c', 'f'))
        {
            const auto count = static_cast<std::size_t> (bytes.u32 (8));
            if (count == 0 || count > 256 || ! bytes.has (12, count * 4))
            {
                reading.error = "its collection's table of faces is cut short";
                return reading;
            }
            for (std::size_t f = 0; f < count; ++f)
                starts.push_back (bytes.u32 (12 + f * 4));
        }
        else
            starts.push_back (0);

        for (std::size_t f = 0; f < starts.size(); ++f)
        {
            std::string error;
            if (auto face = faceAt (bytes, starts[f], static_cast<int> (f), error))
                reading.faces.push_back (std::move (*face));
            else if (reading.error.empty())
                reading.error = starts.size() > 1 ? "face " + std::to_string (f + 1) + ": " + error : error;
        }
        if (! reading.faces.empty())
            reading.error.clear();      // a collection with one bad face still carries the others
        return reading;
    }

    bool isFontFileName (std::string_view name)
    {
        const auto dot = name.rfind ('.');
        if (dot == std::string_view::npos)
            return false;
        std::string extension;
        for (const char c : name.substr (dot + 1))
            extension += folded (c);
        return extension == "ttf" || extension == "otf" || extension == "ttc" || extension == "otc";
    }

    bool sameFamily (std::string_view a, std::string_view b)
    {
        return a.size() == b.size()
            && std::equal (a.begin(), a.end(), b.begin(), [] (char x, char y) { return folded (x) == folded (y); });
    }

    Pick pickFace (const std::vector<Face>& faces, bool bold, bool italic)
    {
        Pick pick;
        if (faces.empty())
            return pick;

        const bool anyItalic = std::any_of (faces.begin(), faces.end(), [] (const Face& f) { return f.italic; });
        const bool wantItalic = italic && anyItalic;
        const int wantWeight = bold ? 700 : 400;

        int bestCost = 0;
        for (std::size_t i = 0; i < faces.size(); ++i)
        {
            const auto& face = faces[i];
            //  The right slant first, then the nearest weight - the heavier of
            //  two as near, as CSS chooses Medium before Light for a regular.
            const int cost = (face.italic == wantItalic ? 0 : 100000) + std::abs (face.weight - wantWeight) * 2
                           + (face.weight < wantWeight ? 1 : 0);
            if (pick.index < 0 || cost < bestCost)
            {
                pick.index = static_cast<int> (i);
                bestCost = cost;
            }
        }

        const auto& chosen = faces[static_cast<std::size_t> (pick.index)];
        pick.embolden = bold && chosen.weight < 600 && ! chosen.bold;
        pick.slant = italic && ! chosen.italic;
        return pick;
    }
}
