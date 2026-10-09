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

/*  A MOVIE'S STRIP (namespace draft §47, AAI): its cuts found in frames made
    up of colours, its pictures' times, its cache's bytes, and a HAP movie's
    read end to end. */

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "HapMovieWriter.h"

#include <wfg/engine/video/Strip.h>
#include <wfg/engine/video/StripAnalysis.h>

#include <juce_core/juce_core.h>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace wfg;
namespace strip = wfg::video::strip;

namespace
{
    strip::Signature solid (double red, double green, double blue)
    {
        return strip::signatureOf ([=] (double, double, double& r, double& g, double& b)
                                   {
                                       r = red;
                                       g = green;
                                       b = blue;
                                       return true;
                                   });
    }

    //  Across the frame from dark to light, slid along by `shift` of its width.
    strip::Signature gradient (double shift)
    {
        return strip::signatureOf ([=] (double u, double, double& r, double& g, double& b)
                                   {
                                       const auto at = std::fmod (u + shift, 1.0);
                                       r = g = b = 40.0 + 180.0 * at;
                                       return true;
                                   });
    }
}

TEST_CASE ("strip: shots of one colour each are cut where they change, to the frame (§47)")
{
    strip::CutFinder finder;

    for (int frame = 0; frame < 120; ++frame)
    {
        const auto seconds = frame / 25.0;

        if (frame < 30)       finder.add (seconds, solid (220, 20, 20));
        else if (frame < 75)  finder.add (seconds, solid (20, 20, 220));
        else                  finder.add (seconds, solid (20, 200, 30));
    }

    const auto cuts = finder.finish();
    REQUIRE (cuts.size() == 2);
    CHECK (cuts[0].seconds == doctest::Approx (30 / 25.0));
    CHECK (cuts[1].seconds == doctest::Approx (75 / 25.0));
    CHECK_FALSE (cuts[0].gradual);
    CHECK (cuts[0].strength > 0.3);
}

TEST_CASE ("strip: a camera move is no cut, nor is a single flash (§47)")
{
    strip::CutFinder moving;

    for (int frame = 0; frame < 100; ++frame)
        moving.add (frame / 25.0, gradient (frame * 0.01));

    CHECK (moving.finish().empty());

    strip::CutFinder flashed;

    for (int frame = 0; frame < 100; ++frame)
        flashed.add (frame / 25.0, frame == 50 ? solid (255, 255, 255) : solid (180, 30, 30));

    CHECK (flashed.finish().empty());
}

TEST_CASE ("strip: a dissolve is one cut, gradual, near its middle (§47)")
{
    strip::CutFinder finder;

    for (int frame = 0; frame < 100; ++frame)
    {
        //  Dark red until frame 40, light cyan from 55, a mix between.
        const auto mix = std::clamp ((frame - 40) / 15.0, 0.0, 1.0);
        finder.add (frame / 25.0, solid (200 * (1.0 - mix) + 10 * mix, 10 + 210 * mix, 10 + 210 * mix));
    }

    const auto cuts = finder.finish();
    REQUIRE (cuts.size() == 1);
    CHECK (cuts[0].gradual);
    CHECK (cuts[0].seconds == doctest::Approx (47.0 / 25.0).epsilon (0.05));
}

TEST_CASE ("strip: a cut between two frames looked at is placed on its first frame by halving (§47)")
{
    std::vector<strip::Signature> frames;

    for (int frame = 0; frame < 16; ++frame)
        frames.push_back (frame < 11 ? solid (200, 50, 50) : solid (50, 50, 200));

    int asked = 0;
    const auto first = strip::firstFrameOfShot (0, 15, [&] (int frame) { ++asked; return frames[static_cast<std::size_t> (frame)]; });

    CHECK (first == 11);
    CHECK (asked <= 8);
}

TEST_CASE ("strip: pictures at the start of every shot and every so often between, never too many (§47)")
{
    const std::vector<strip::Cut> cuts { { 2.0, 0.5, false }, { 6.0, 0.4, true } };
    const auto times = strip::thumbnailTimes (10.0, cuts);

    REQUIRE_FALSE (times.empty());
    CHECK (times.front() == doctest::Approx (0.0));
    CHECK (std::find_if (times.begin(), times.end(), [] (double t) { return std::abs (t - 2.05) < 1.0e-9; }) != times.end());
    CHECK (std::find_if (times.begin(), times.end(), [] (double t) { return std::abs (t - 6.5) < 1.0e-9; }) != times.end());
    CHECK (std::is_sorted (times.begin(), times.end()));

    //  Three hours: fewer than the limit.
    CHECK (strip::thumbnailTimes (3 * 3600.0, {}).size() <= strip::maxThumbnails);
    CHECK (strip::thumbnailTimes (0.0, cuts).empty());

    CHECK (strip::thumbnailHeightFor (1920, 1080) == 45);
    CHECK (strip::thumbnailHeightFor (16, 8) == 40);
}

TEST_CASE ("strip: kept as bytes and read back the same, and refused when a byte is wrong (§47)")
{
    strip::MovieStrip made;
    made.duration = 12.5;
    made.width = 1920;
    made.height = 1080;
    made.cuts = { { 2.0, 0.6, false }, { 7.25, 0.35, true } };

    strip::Thumbnail picture;
    picture.seconds = 2.05;
    picture.width = 2;
    picture.height = 1;
    picture.rgb = { 1, 2, 3, 4, 5, 6 };
    made.thumbnails = { picture };

    const auto bytes = strip::encode (made);

    strip::MovieStrip read;
    REQUIRE (strip::decode (bytes.data(), bytes.size(), read));
    CHECK (read.duration == doctest::Approx (made.duration));
    CHECK (read.width == made.width);
    CHECK (read.height == made.height);
    REQUIRE (read.cuts.size() == 2);
    CHECK (read.cuts[1].seconds == doctest::Approx (7.25));
    CHECK (read.cuts[1].strength == doctest::Approx (0.35));
    CHECK (read.cuts[1].gradual);
    REQUIRE (read.thumbnails.size() == 1);
    CHECK (read.thumbnails[0].seconds == doctest::Approx (2.05));
    CHECK (read.thumbnails[0].rgb == picture.rgb);
    CHECK (read.thumbnailAt (5.0) == &read.thumbnails.front());
    CHECK (read.thumbnailAt (0.0) == &read.thumbnails.front());

    auto broken = bytes;
    broken[20] ^= 0x01u;
    strip::MovieStrip untouched;
    CHECK_FALSE (strip::decode (broken.data(), broken.size(), untouched));
    CHECK (untouched.cuts.empty());
    CHECK_FALSE (strip::decode (bytes.data(), 3, untouched));
}

TEST_CASE ("strip: a HAP movie of three shots read from its file - its cuts on their frames, a picture of each (§47)")
{
    using namespace wfg::testing::hapmovie;

    auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getNonexistentChildFile ("wfg-strip", {}, false);
    REQUIRE (folder.createDirectory());

    //  Ten frames a second: red for 1.3 s, blue for 1.0 s, green for 1.2 s.
    std::vector<Bytes> frames;

    for (int frame = 0; frame < 35; ++frame)
        frames.push_back (section (0xAB, solidDxt1 (16, 8, frame < 13 ? 0xE01010u : frame < 23 ? 0x1010E0u : 0x10C020u)));

    const auto movie = writeMovie (folder, "shots.mov", hapMovie (16, 8, frames, 10));

    strip::MovieStrip found;
    std::string why;
    REQUIRE_MESSAGE (strip::analyseHap (movie.getFullPathName().toStdString(), found, why), why);

    CHECK (found.duration == doctest::Approx (3.5));
    CHECK (found.width == 16);
    CHECK (found.height == 8);
    REQUIRE (found.cuts.size() == 2);
    CHECK (found.cuts[0].seconds == doctest::Approx (1.3));
    CHECK (found.cuts[1].seconds == doctest::Approx (2.3));

    REQUIRE (found.thumbnails.size() >= 3);
    CHECK (found.thumbnails.front().width == strip::thumbnailWidth);
    CHECK (found.thumbnails.front().height == 40);

    //  The first picture is red, the one at the second shot blue.
    CHECK (found.thumbnails.front().rgb[0] > 150);
    const auto* blue = found.thumbnailAt (1.5);
    REQUIRE (blue != nullptr);
    CHECK (blue->rgb[2] > 150);
    CHECK (blue->rgb[0] < 100);

    folder.deleteRecursively();
}
