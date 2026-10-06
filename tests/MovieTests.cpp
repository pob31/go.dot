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

/*  PHASE 8b'S MOVIE READER (namespace draft 37, VY): Snappy's blocks, HAP's
    sections - plain, Snappy and chunked - the DXT blocks a frame unpacks to,
    and a QuickTime file's frames found, timed and read.

    The movies are made here, by a writer of the test's own: a `.mov` of HAP
    frames of one colour each, with the boxes the reader reads and nothing
    more. A real encoder's file is the bench's (§37.4).
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "HapMovieWriter.h"
#include "TestSupport.h"

#include <wfg/engine/video/Hap.h>
#include <wfg/engine/video/Movie.h>
#include <wfg/engine/video/Snappy.h>

#include <juce_core/juce_core.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace wfg;

using namespace wfg::testing::hapmovie;

//==============================================================================
TEST_CASE ("snappy: literals and copies, and a damaged block refused")
{
    const Bytes text { 'a', 'b', 'c', 'd', 'a', 'b', 'c', 'd', 'a', 'b' };

    /*  "abcd" as a literal, then a copy of six bytes from four back - an
        overlapping copy, which is how a run repeats. */
    const Bytes block { 10, 3 << 2, 'a', 'b', 'c', 'd', static_cast<std::uint8_t> (((6 - 4) << 2) | 1), 4 };

    Bytes out;
    REQUIRE (video::snappy::decompress (block.data(), block.size(), out));
    CHECK (out == text);

    const auto literals = snappyLiterals (Bytes (200, 7));
    REQUIRE (video::snappy::decompress (literals.data(), literals.size(), out));
    CHECK (out == Bytes (200, 7));

    /*  A COPY FROM BEFORE THE START, and a length the block does not keep. */
    const Bytes backwards { 4, static_cast<std::uint8_t> ((0 << 2) | 1), 9 };
    CHECK_FALSE (video::snappy::decompress (backwards.data(), backwards.size(), out));

    const Bytes short_ { 20, 3 << 2, 'a', 'b', 'c', 'd' };
    CHECK_FALSE (video::snappy::decompress (short_.data(), short_.size(), out));
}

TEST_CASE ("hap: a frame plain, Snappy and in chunks unpacks to the same DXT1 texture, and a pixel reads back")
{
    const auto texture = solidDxt1 (8, 8, 0xFF8000);

    video::hap::Texture format {};
    Bytes out;

    const auto plain = section (0xAB, texture);
    REQUIRE (video::hap::unpack (plain.data(), plain.size(), format, out));
    CHECK (format == video::hap::Texture::dxt1);
    CHECK (out == texture);

    const auto snappy = section (0xBB, snappyLiterals (texture));
    REQUIRE (video::hap::unpack (snappy.data(), snappy.size(), format, out));
    CHECK (out == texture);

    /*  TWO CHUNKS, one plain and one Snappy, described in front. */
    const Bytes first (texture.begin(), texture.begin() + 16);
    const Bytes second (texture.begin() + 16, texture.end());
    const auto secondPacked = snappyLiterals (second);

    Bytes sizes;
    for (const auto size : { static_cast<std::uint32_t> (first.size()), static_cast<std::uint32_t> (secondPacked.size()) })
        for (int n = 0; n < 4; ++n)
            sizes.push_back (static_cast<std::uint8_t> (size >> (8 * n)));

    const auto instructions = section (0x01, join ({ section (0x02, Bytes { 0x0A, 0x0B }), section (0x03, sizes) }));
    const auto chunked = section (0xCB, join ({ instructions, first, secondPacked }));

    REQUIRE (video::hap::unpack (chunked.data(), chunked.size(), format, out));
    CHECK (out == texture);

    /*  THE PIXEL: 5:6:5 of #FF8000 read back as near it. */
    double r = 0, g = 0, b = 0, a = 0;
    REQUIRE (video::hap::pixelAt (format, out, 8, 8, 5, 6, r, g, b, a));
    CHECK (r == doctest::Approx (255.0));
    CHECK (g == doctest::Approx (130.0).epsilon (0.02));
    CHECK (b == doctest::Approx (0.0));
    CHECK (a == doctest::Approx (1.0));

    CHECK_FALSE (video::hap::pixelAt (format, out, 8, 8, 9, 0, r, g, b, a));

    /*  A FORMAT NOT TAKEN HERE - Hap R's BC7 - is refused, not misread. */
    const auto bc7 = section (0xAC, texture);
    CHECK_FALSE (video::hap::unpack (bc7.data(), bc7.size(), format, out));
}

TEST_CASE ("hap: DXT5's alpha, and Hap Q's grey turned back from YCoCg")
{
    /*  ONE DXT5 BLOCK: alpha ends 255 and 0, every pixel index 1 (zero); the
        colour block white. */
    Bytes block { 255, 0 };
    std::uint64_t bits = 0;

    for (int n = 0; n < 16; ++n)
        bits |= std::uint64_t (1) << (3 * n);

    for (int n = 0; n < 6; ++n)
        block.push_back (static_cast<std::uint8_t> (bits >> (8 * n)));

    block.insert (block.end(), { 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0 });

    double r = 0, g = 0, b = 0, a = 0;
    REQUIRE (video::hap::pixelAt (video::hap::Texture::dxt5, block, 4, 4, 1, 1, r, g, b, a));
    CHECK (r == doctest::Approx (255.0));
    CHECK (a == doctest::Approx (0.0));

    /*  HAP Q: Co and Cg at their middle (128) mean no colour, so what comes
        back is grey at the luma alpha carries - here all of it. */
    Bytes q { 255, 255, 0, 0, 0, 0, 0, 0 };
    const auto middle = rgb565 (0x808000);
    q.insert (q.end(), { static_cast<std::uint8_t> (middle & 0xffu), static_cast<std::uint8_t> (middle >> 8),
                         static_cast<std::uint8_t> (middle & 0xffu), static_cast<std::uint8_t> (middle >> 8), 0, 0, 0, 0 });

    REQUIRE (video::hap::pixelAt (video::hap::Texture::ycocgDxt5, q, 4, 4, 2, 2, r, g, b, a));
    CHECK (r == doctest::Approx (255.0).epsilon (0.03));
    CHECK (g == doctest::Approx (255.0).epsilon (0.03));
    CHECK (b == doctest::Approx (255.0).epsilon (0.03));
}

TEST_CASE ("movie: a QuickTime file's HAP frames found, timed and read")
{
    juce::TemporaryFile scratch;
    const auto folder = scratch.getFile().getSiblingFile ("godot-movie-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));
    folder.createDirectory();

    /*  TEN FRAMES AT 25 A SECOND, red then blue then green and so on, some
        Snappy. */
    const std::uint32_t colours[] { 0xFF0000, 0x0000FF, 0x00FF00 };
    std::vector<Bytes> frames;

    for (int n = 0; n < 10; ++n)
    {
        const auto texture = solidDxt1 (16, 8, colours[n % 3]);
        frames.push_back (n % 2 == 0 ? section (0xAB, texture) : section (0xBB, snappyLiterals (texture)));
    }

    const auto file = writeMovie (folder, "colours.mov", hapMovie (16, 8, frames, 25));

    video::movie::MovieFile movie;
    std::string why;
    REQUIRE_MESSAGE (movie.open (file.getFullPathName().toStdString(), why), why);

    const auto& info = movie.info();
    CHECK (info.codec == "Hap1");
    CHECK (info.isHap());
    CHECK (info.width == 16);
    CHECK (info.height == 8);
    REQUIRE (info.frames.size() == 10);
    CHECK (info.duration == doctest::Approx (0.4));
    CHECK (info.frameRate() == doctest::Approx (25.0));

    CHECK (info.frameAt (0.0) == 0);
    CHECK (info.frameAt (0.039) == 0);
    CHECK (info.frameAt (0.04) == 1);
    CHECK (info.frameAt (0.25) == 6);
    CHECK (info.frameAt (5.0) == 9);
    CHECK (info.frameAt (-1.0) == 0);

    /*  FRAME 7 IS BLUE (7 % 3 == 1), read off the disk and unpacked. */
    Bytes bytes, texture;
    video::hap::Texture format {};
    REQUIRE (movie.readFrame (7, bytes));
    REQUIRE (video::hap::unpack (bytes.data(), bytes.size(), format, texture));

    double r = 0, g = 0, b = 0, a = 0;
    REQUIRE (video::hap::pixelAt (format, texture, 16, 8, 3, 3, r, g, b, a));
    CHECK (b == doctest::Approx (255.0));
    CHECK (r == doctest::Approx (0.0));

    CHECK_FALSE (movie.readFrame (10, bytes));
    CHECK (video::movie::durationOf (file.getFullPathName().toStdString()) == doctest::Approx (0.4));

    /*  A FILE THAT IS NOT A MOVIE says so. */
    const auto junk = writeMovie (folder, "junk.mov", Bytes (64, 0x41));
    video::movie::MovieFile nothing;
    CHECK_FALSE (nothing.open (junk.getFullPathName().toStdString(), why));
    CHECK (video::movie::durationOf (junk.getFullPathName().toStdString()) < 0.0);

    folder.deleteRecursively();
}
