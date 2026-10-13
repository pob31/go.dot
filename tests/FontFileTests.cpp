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

/*
    A FONT FILE'S NAMES (namespace draft §62, AKY): fonts built here byte by
    byte - a name table, an OS/2, a head, a collection of two - so each rule is
    held by a file made to test it; and DejaVu Sans Mono, which Pure Data's
    folder carries, read as a real family of four faces.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/video/FontFile.h>

#include <juce_core/juce_core.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace
{
    namespace ff = wfg::video::fontfile;

    void put16 (std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t v)
    {
        out[at] = static_cast<std::uint8_t> ((v >> 8) & 0xFF);
        out[at + 1] = static_cast<std::uint8_t> (v & 0xFF);
    }

    void put32 (std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t v)
    {
        put16 (out, at, v >> 16);
        put16 (out, at + 2, v & 0xFFFF);
    }

    void push16 (std::vector<std::uint8_t>& out, std::uint32_t v)
    {
        out.push_back (static_cast<std::uint8_t> ((v >> 8) & 0xFF));
        out.push_back (static_cast<std::uint8_t> (v & 0xFF));
    }

    /** UTF-8 to UTF-16, big-endian, as Windows' and Unicode's names are stored. */
    std::vector<std::uint8_t> utf16 (const std::string& text)
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

    Name windows (std::uint16_t id, const std::string& text, std::uint16_t language = 0x0409)
    {
        return { 3, 1, language, id, utf16 (text) };
    }

    Name mac (std::uint16_t id, std::vector<std::uint8_t> romanBytes)
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

    FaceBuilder plain (const std::string& family, const std::string& style, int weight = 400)
    {
        FaceBuilder face;
        face.names = { windows (1, family), windows (2, style), windows (4, family + " " + style),
                       windows (6, family + "-" + style) };
        face.weight = weight;
        return face;
    }

    ff::Reading readBytes (const std::vector<std::uint8_t>& bytes)
    {
        return ff::read (bytes.data(), bytes.size());
    }

    ff::Face only (const std::vector<std::uint8_t>& bytes)
    {
        const auto reading = readBytes (bytes);
        REQUIRE_MESSAGE (reading.faces.size() == 1, reading.error);
        return reading.faces.front();
    }
}

TEST_CASE ("font file: a face's family, style, full and PostScript names, from Windows' English names (§62, AKY)")
{
    const auto face = only (plain ("Stage Sans", "Regular").bytes());
    CHECK (face.family == "Stage Sans");
    CHECK (face.legacyFamily == "Stage Sans");
    CHECK (face.style == "Regular");
    CHECK (face.fullName == "Stage Sans Regular");
    CHECK (face.postScript == "Stage Sans-Regular");
    CHECK (face.index == 0);
    CHECK (face.weight == 400);
    CHECK_FALSE (face.bold);
    CHECK_FALSE (face.italic);
}

TEST_CASE ("font file: the typographic family and style come before the legacy ones, which are kept (§62, AKY)")
{
    auto builder = plain ("Stage Sans Semibold", "Regular", 600);
    builder.names.push_back (windows (16, "Stage Sans"));
    builder.names.push_back (windows (17, "Semibold"));
    const auto face = only (builder.bytes());
    CHECK (face.family == "Stage Sans");
    CHECK (face.legacyFamily == "Stage Sans Semibold");
    CHECK (face.style == "Semibold");
    CHECK (face.weight == 600);
}

TEST_CASE ("font file: names in UTF-16 and Mac Roman read as UTF-8, American English preferred (§62, AKY)")
{
    SUBCASE ("Windows' Unicode, beyond ASCII and beyond the plane")
    {
        FaceBuilder builder;
        builder.names = { windows (1, "Théâtre Œil \xF0\x9F\x8E\xAD"), windows (2, "Normal") };
        CHECK (only (builder.bytes()).family == "Théâtre Œil \xF0\x9F\x8E\xAD");
    }
    SUBCASE ("the Macintosh's Mac Roman, when Windows says nothing")
    {
        FaceBuilder builder;
        builder.names = { mac (1, { 'C', 0x8E, 'l', 0x8E, 'b', 'r', 'e' }), mac (2, { 'R', 'o', 'm', 'a', 'i', 'n' }) };
        const auto face = only (builder.bytes());
        CHECK (face.family == "Célébre");
        CHECK (face.style == "Romain");
    }
    SUBCASE ("American English over French over the Macintosh's, whatever their order in the table")
    {
        FaceBuilder builder;
        builder.names = { mac (1, { 'M', 'a', 'c' }), windows (1, "Française", 0x040C), windows (1, "English"),
                          windows (2, "Regular") };
        CHECK (only (builder.bytes()).family == "English");
        builder.names = { mac (1, { 'M', 'a', 'c' }), windows (1, "Française", 0x040C), windows (2, "Regular") };
        CHECK (only (builder.bytes()).family == "Française");
        builder.names = { mac (1, { 'M', 'a', 'c' }), windows (2, "Regular") };
        CHECK (only (builder.bytes()).family == "Mac");
    }
    SUBCASE ("British English before another language")
    {
        FaceBuilder builder;
        builder.names = { windows (1, "Deutsch", 0x0407), windows (1, "British", 0x0809), windows (2, "Regular") };
        CHECK (only (builder.bytes()).family == "British");
    }
}

TEST_CASE ("font file: bold, italic and weight from OS/2, from head where there is none (§62, AKY)")
{
    SUBCASE ("OS/2's bold and italic bits and its weight")
    {
        auto builder = plain ("Stage Sans", "Bold Italic", 700);
        builder.selection = 0x0021;     // ITALIC | BOLD
        const auto face = only (builder.bytes());
        CHECK (face.bold);
        CHECK (face.italic);
        CHECK (face.weight == 700);
    }
    SUBCASE ("an oblique is an italic")
    {
        auto builder = plain ("Stage Sans", "Oblique");
        builder.selection = 0x0200;     // OBLIQUE
        CHECK (only (builder.bytes()).italic);
    }
    SUBCASE ("a heavy weight without the bold bit is not called bold, but weighs what it says")
    {
        auto builder = plain ("Stage Sans", "Black", 900);
        const auto face = only (builder.bytes());
        CHECK_FALSE (face.bold);
        CHECK (face.weight == 900);
    }
    SUBCASE ("head's style bits, in a font with no OS/2")
    {
        auto builder = plain ("Old Face", "Bold");
        builder.weight.reset();
        builder.macStyle = 0x0001;
        auto face = only (builder.bytes());
        CHECK (face.bold);
        CHECK_FALSE (face.italic);
        CHECK (face.weight == 700);
        builder.macStyle = 0x0002;
        face = only (builder.bytes());
        CHECK_FALSE (face.bold);
        CHECK (face.italic);
        CHECK (face.weight == 400);
    }
    SUBCASE ("no OS/2 and no head: regular")
    {
        auto builder = plain ("Bare", "Regular");
        builder.weight.reset();
        const auto face = only (builder.bytes());
        CHECK_FALSE (face.bold);
        CHECK_FALSE (face.italic);
        CHECK (face.weight == 400);
    }
}

TEST_CASE ("font file: only a restricted licence is restricted (§62, ALB)")
{
    const auto embedding = [] (std::uint16_t bits)
    {
        auto builder = plain ("Licensed", "Regular");
        builder.embedding = bits;
        return only (builder.bytes());
    };
    CHECK (embedding (0x0002).restricted());
    CHECK (embedding (0x0302).restricted());    // restricted, whatever the subsetting bits say
    CHECK_FALSE (embedding (0x0000).restricted());  // installable
    CHECK_FALSE (embedding (0x0004).restricted());  // preview and print
    CHECK_FALSE (embedding (0x0008).restricted());  // editable
    CHECK_FALSE (embedding (0x0100).restricted());  // no subsetting
    CHECK_FALSE (embedding (0x0200).restricted());  // bitmap only
    CHECK (embedding (0x0302).embedding == 0x0302);
}

TEST_CASE ("font file: each face of a collection is read, and numbered (§62, AKY)")
{
    const auto regular = plain ("Pair", "Regular");
    auto bold = plain ("Pair", "Bold", 700);
    bold.selection = 0x0020;

    const std::size_t header = 12 + 4 * 2;
    const auto first = regular.bytes (header);
    const auto second = bold.bytes (header + first.size());
    std::vector<std::uint8_t> file (header, 0);
    put32 (file, 0, 0x74746366u);    // 'ttcf'
    put32 (file, 4, 0x00010000u);
    put32 (file, 8, 2);
    put32 (file, 12, static_cast<std::uint32_t> (header));
    put32 (file, 16, static_cast<std::uint32_t> (header + first.size()));
    file.insert (file.end(), first.begin(), first.end());
    file.insert (file.end(), second.begin(), second.end());

    const auto reading = readBytes (file);
    REQUIRE (reading.faces.size() == 2);
    CHECK (reading.error.empty());
    CHECK (reading.faces[0].index == 0);
    CHECK (reading.faces[0].style == "Regular");
    CHECK (reading.faces[1].index == 1);
    CHECK (reading.faces[1].style == "Bold");
    CHECK (reading.faces[1].bold);

    SUBCASE ("a collection whose second face is broken still gives the first")
    {
        auto broken = file;
        put32 (broken, 16, static_cast<std::uint32_t> (broken.size() + 40));
        const auto partial = readBytes (broken);
        REQUIRE (partial.faces.size() == 1);
        CHECK (partial.faces[0].style == "Regular");
    }
}

TEST_CASE ("font file: a file that does not read is an error in words, never a crash (§62, AKY)")
{
    CHECK (ff::read (nullptr, 0).error == "too short to be a font file");
    CHECK (readBytes ({ 1, 2, 3 }).error == "too short to be a font file");

    std::vector<std::uint8_t> notAFont (64, 0x41);
    CHECK (readBytes (notAFont).error == "not a font file");

    auto cut = plain ("Cut", "Regular").bytes();
    put16 (cut, 4, 900);        // more tables than the file holds
    CHECK (readBytes (cut).error == "its table of contents is cut short");

    FaceBuilder nameless;
    nameless.names = { windows (2, "Regular") };
    CHECK (readBytes (nameless.bytes()).error == "it names no family");

    auto noName = plain ("Gone", "Regular").bytes();
    put32 (noName, 12, 0x6E6F6E65u);   // the name table's tag changed
    CHECK (readBytes (noName).error == "it has no names");

    SUBCASE ("every length of a good file, and a thousand files with bytes changed at random")
    {
        auto builder = plain ("Robust", "Regular", 500);
        builder.names.push_back (windows (16, "Robust Family"));
        const auto good = builder.bytes();
        for (std::size_t length = 0; length < good.size(); ++length)
        {
            const std::vector<std::uint8_t> prefix (good.begin(), good.begin() + static_cast<std::ptrdiff_t> (length));
            const auto reading = readBytes (prefix);
            CHECK ((reading.faces.empty() != reading.error.empty()));
        }

        std::mt19937 random (62);
        for (int round = 0; round < 1000; ++round)
        {
            auto changed = good;
            for (int k = 0; k < 4; ++k)
                changed[random() % changed.size()] = static_cast<std::uint8_t> (random() & 0xFF);
            const auto reading = readBytes (changed);
            CHECK ((reading.faces.empty() != reading.error.empty()));
        }
    }
}

TEST_CASE ("font file: the files fonts/ holds, and families compared without case (§62, AKX)")
{
    CHECK (ff::isFontFileName ("DejaVuSansMono.ttf"));
    CHECK (ff::isFontFileName ("Title.OTF"));
    CHECK (ff::isFontFileName ("Collection.ttc"));
    CHECK (ff::isFontFileName ("a.b.otc"));
    CHECK_FALSE (ff::isFontFileName ("readme.txt"));
    CHECK_FALSE (ff::isFontFileName ("ttf"));
    CHECK_FALSE (ff::isFontFileName ("font.ttf.bak"));

    CHECK (ff::sameFamily ("DejaVu Sans Mono", "dejavu sans MONO"));
    CHECK_FALSE (ff::sameFamily ("DejaVu Sans", "DejaVu Sans Mono"));
    CHECK_FALSE (ff::sameFamily ("Arial", "Arial "));
}

TEST_CASE ("font file: the face that draws bold or italic, and what Go.dot must draw itself (§62, AKZ)")
{
    const auto face = [] (int weight, bool italic, bool bold = false)
    {
        ff::Face f;
        f.weight = weight;
        f.italic = italic;
        f.bold = bold;
        return f;
    };

    CHECK (ff::pickFace ({}, true, true).index == -1);

    const std::vector<ff::Face> four { face (400, false), face (700, false, true), face (400, true), face (700, true, true) };
    auto pick = ff::pickFace (four, false, false);
    CHECK (pick.index == 0);
    CHECK_FALSE (pick.embolden);
    CHECK_FALSE (pick.slant);
    CHECK (ff::pickFace (four, true, false).index == 1);
    CHECK (ff::pickFace (four, false, true).index == 2);
    pick = ff::pickFace (four, true, true);
    CHECK (pick.index == 3);
    CHECK_FALSE (pick.embolden);
    CHECK_FALSE (pick.slant);

    SUBCASE ("a regular face alone: emboldened, slanted")
    {
        const std::vector<ff::Face> one { face (400, false) };
        pick = ff::pickFace (one, true, true);
        CHECK (pick.index == 0);
        CHECK (pick.embolden);
        CHECK (pick.slant);
    }
    SUBCASE ("weights between: the heavier of two as near, for regular and for bold")
    {
        const std::vector<ff::Face> weights { face (300, false), face (500, false), face (900, false) };
        CHECK (ff::pickFace (weights, false, false).index == 1);   // 300 and 500 both 100 from 400: Medium
        pick = ff::pickFace (weights, true, false);
        CHECK (pick.index == 2);                                     // 500 and 900 both 200 from 700: Black
        CHECK_FALSE (pick.embolden);
        const std::vector<ff::Face> light { face (300, false), face (500, false) };
        pick = ff::pickFace (light, true, false);
        CHECK (pick.index == 1);
        CHECK (pick.embolden);                                       // Medium is not bold: Go.dot emboldens it
    }
    SUBCASE ("italic asked of a family whose only italic is bold: the italic face, not a slanted regular")
    {
        const std::vector<ff::Face> mixed { face (400, false), face (700, true, true) };
        pick = ff::pickFace (mixed, false, true);
        CHECK (pick.index == 1);
        CHECK_FALSE (pick.slant);
    }
}

TEST_CASE ("font file: DejaVu Sans Mono, carried by Pure Data's folder, read as a family of four (§62, AKY)")
{
    const auto folder = juce::File (WFG_REPO_ROOT).getChildFile ("ThirdParty/pure-data/font");
    struct Expected
    {
        const char* file;
        const char* postScript;
        bool bold, italic;
    };
    const Expected expected[] {
        { "DejaVuSansMono.ttf", "DejaVuSansMono", false, false },
        { "DejaVuSansMono-Bold.ttf", "DejaVuSansMono-Bold", true, false },
        { "DejaVuSansMono-Oblique.ttf", "DejaVuSansMono-Oblique", false, true },
        { "DejaVuSansMono-BoldOblique.ttf", "DejaVuSansMono-BoldOblique", true, true },
    };
    std::vector<ff::Face> family;
    for (const auto& e : expected)
    {
        juce::MemoryBlock data;
        REQUIRE_MESSAGE (folder.getChildFile (e.file).loadFileAsData (data), e.file);
        const auto reading = ff::read (static_cast<const std::uint8_t*> (data.getData()), data.getSize());
        REQUIRE_MESSAGE (reading.faces.size() == 1, e.file);
        const auto& face = reading.faces.front();
        CHECK (face.family == "DejaVu Sans Mono");
        CHECK (face.postScript == e.postScript);
        CHECK (face.bold == e.bold);
        CHECK (face.italic == e.italic);
        CHECK_FALSE (face.restricted());
        family.push_back (face);
    }
    CHECK (family[1].weight > family[0].weight);
    CHECK (ff::pickFace (family, true, true).index == 3);
    CHECK (ff::pickFace (family, false, true).index == 2);
}
