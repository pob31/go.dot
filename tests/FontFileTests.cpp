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

#include "FontFixture.h"

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
    using namespace wfg::test::fonts;

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

TEST_CASE ("font file: a collection's face copied out as a file of its own, which reads as that face (§62, AKZ)")
{
    const auto regular = bytesOf (dejaVu ("DejaVuSansMono.ttf"));
    const auto bold = bytesOf (dejaVu ("DejaVuSansMono-Bold.ttf"));
    REQUIRE_FALSE (regular.empty());
    REQUIRE_FALSE (bold.empty());

    const auto both = collectionOf ({ regular, bold });
    const auto whole = ff::read (both.data(), both.size());
    REQUIRE (whole.faces.size() == 2);
    CHECK (whole.faces[1].postScript == "DejaVuSansMono-Bold");

    const auto second = ff::extractFace (both.data(), both.size(), 1);
    REQUIRE_FALSE (second.empty());
    const auto alone = ff::read (second.data(), second.size());
    REQUIRE (alone.faces.size() == 1);
    CHECK (alone.faces[0].postScript == "DejaVuSansMono-Bold");
    CHECK (alone.faces[0].bold);
    CHECK (alone.faces[0].index == 0);

    const auto first = ff::extractFace (both.data(), both.size(), 0);
    REQUIRE_FALSE (first.empty());
    CHECK (ff::read (first.data(), first.size()).faces.at (0).postScript == "DejaVuSansMono");

    //  A single face's file is itself, for its only face; no other face, and nothing from nothing.
    CHECK (ff::extractFace (regular.data(), regular.size(), 0) == regular);
    CHECK (ff::extractFace (regular.data(), regular.size(), 1).empty());
    CHECK (ff::extractFace (both.data(), both.size(), 2).empty());
    CHECK (ff::extractFace (both.data(), both.size(), -1).empty());
    CHECK (ff::extractFace (nullptr, 0, 0).empty());

    //  A face whose table runs past the end is not copied out.
    auto cut = both;
    cut.resize (cut.size() - 1000);
    CHECK (ff::extractFace (cut.data(), cut.size(), 1).empty());
}
