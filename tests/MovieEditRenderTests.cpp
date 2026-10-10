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

/*  THE RENDER OF A MOVIE'S EDIT (namespace draft §55.5, ADU, ADV): a HAP
    movie's sections laid on its own frame grid as a new HAP movie - every
    frame outside a dissolve the source's bytes, a dissolve a linear blend
    centred on the join from material beyond the edges, black beyond the file.

    The movies are the tests' own: three seconds at ten frames a second, red
    then green then blue, 16 by 8 - DXT1 from the fixture writer, Hap Alpha and
    Hap Q through Go.dot's own encoder and writer.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "HapMovieWriter.h"
#include "TestSupport.h"

#include <wfg/engine/document/MediaEdit.h>
#include <wfg/engine/video/Hap.h>
#include <wfg/engine/video/HapEncoder.h>
#include <wfg/engine/video/Movie.h>
#include <wfg/engine/video/MovieEditRender.h>
#include <wfg/engine/video/MovieWriter.h>

#include <juce_core/juce_core.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace wfg;
using namespace wfg::testing::hapmovie;

namespace
{
    constexpr int width = 16, height = 8;
    constexpr std::uint32_t colours[3] { 0xFF0000, 0x00FF00, 0x0000FF };   // a second each at 10 fps

    juce::File scratchFolder (const char* stem)
    {
        const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile (juce::String (stem) + "-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));
        REQUIRE (folder.createDirectory());
        return folder;
    }

    /*  THIRTY DXT1 FRAMES from the fixture writer, under any codec box. */
    std::string writeFixtureMovie (const juce::File& folder, const char* name, const char* codec = "Hap1")
    {
        std::vector<Bytes> frames;

        for (int n = 0; n < 30; ++n)
            frames.push_back (section (0xAB, solidDxt1 (width, height, colours[n / 10])));

        return writeMovie (folder, name, hapMovie (width, height, frames, 10, codec)).getFullPathName().toStdString();
    }

    /*  THIRTY FRAMES THROUGH GO.DOT'S ENCODER AND WRITER, in any texture. */
    std::string writeEncodedMovie (const juce::File& folder, const char* name, video::hap::Texture texture, const char* codec)
    {
        const auto path = folder.getChildFile (name).getFullPathName().toStdString();
        video::movie::MovieWriter writer;
        std::string why;
        REQUIRE_MESSAGE (writer.open (path, codec, width, height, 10, 1, why), why);

        for (int n = 0; n < 30; ++n)
        {
            std::vector<std::uint8_t> rgba (static_cast<std::size_t> (width * height * 4));
            const auto colour = colours[n / 10];

            for (std::size_t p = 0; p < rgba.size(); p += 4)
            {
                rgba[p] = static_cast<std::uint8_t> (colour >> 16);
                rgba[p + 1] = static_cast<std::uint8_t> ((colour >> 8) & 0xffu);
                rgba[p + 2] = static_cast<std::uint8_t> (colour & 0xffu);
                rgba[p + 3] = 255;
            }

            std::vector<std::uint8_t> blocks, frame, scratch;
            video::hap::encodeTexture (texture, rgba.data(), width, height, static_cast<std::size_t> (width) * 4, blocks);
            video::hap::packFrame (texture, blocks, frame, scratch);
            REQUIRE (writer.write (frame.data(), frame.size()));
        }

        REQUIRE_MESSAGE (writer.finish (why), why);
        return path;
    }

    using Frames = std::vector<std::vector<std::uint8_t>>;

    Frames framesOf (const std::string& path, video::movie::Info& info)
    {
        video::movie::MovieFile file;
        std::string why;
        REQUIRE_MESSAGE (file.open (path, why), why);
        info = file.info();

        Frames out (info.frames.size());

        for (std::size_t n = 0; n < out.size(); ++n)
            REQUIRE (file.readFrame (static_cast<int> (n), out[n]));

        return out;
    }

    struct Rgba
    {
        double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
    };

    Rgba pixelOf (const std::vector<std::uint8_t>& frameBytes, int x = 5, int y = 3)
    {
        auto texture = video::hap::Texture::none;
        std::vector<std::uint8_t> blocks;
        REQUIRE (video::hap::unpack (frameBytes.data(), frameBytes.size(), texture, blocks));

        Rgba p;
        REQUIRE (video::hap::pixelAt (texture, blocks, width, height, x, y, p.r, p.g, p.b, p.a));
        return p;
    }

    /*  A pixel within `within` of a colour on each channel: the encoders
        quantise, and a blend of two solid colours comes back a shade off. */
    void near (const Rgba& got, double r, double g, double b, double within = 10.0)
    {
        INFO ("got " << got.r << " " << got.g << " " << got.b << ", wanted " << r << " " << g << " " << b);
        CHECK (std::abs (got.r - r) <= within);
        CHECK (std::abs (got.g - g) <= within);
        CHECK (std::abs (got.b - b) <= within);
    }

    /*  A section as a test spells it: no fade and no gap unless said (55.9). */
    doc::Section sec (double in, double out, double fadeIn = 0.0, double fadeOut = 0.0, double gap = 0.0)
    {
        doc::Section s;
        s.id = "S" + std::to_string (static_cast<int> (in * 100)) + "T" + std::to_string (static_cast<int> (out * 100));
        s.in = in;
        s.out = out;
        s.fadeIn = fadeIn;
        s.fadeOut = fadeOut;
        s.gap = gap;
        return s;
    }
}

//==============================================================================
TEST_CASE ("movie edit render: a cut and a reorder are the source's own bytes, on its own grid")
{
    const auto folder = scratchFolder ("godot-medit");
    const auto source = writeFixtureMovie (folder, "three.mov");
    const auto target = folder.getChildFile ("render.mov").getFullPathName().toStdString();

    video::movie::Info sourceInfo;
    const auto sourceFrames = framesOf (source, sourceInfo);
    REQUIRE (sourceFrames.size() == 30u);

    /*  THE LAST SECOND THEN THE FIRST, with no dissolve. */
    std::vector<int> progress;
    const auto result = video::movie::renderMovieEdit (source, { sec (2.0, 3.0), sec (0.0, 1.0) }, target, nullptr,
                                                       [&progress] (int percent) { progress.push_back (percent); });
    REQUIRE_MESSAGE (result.ok, result.problem);
    CHECK (result.seconds == doctest::Approx (2.0));
    CHECK (result.frames == 20);
    CHECK (result.frameRate == doctest::Approx (10.0));
    CHECK_FALSE (result.resampled);
    REQUIRE_FALSE (progress.empty());
    CHECK (progress.back() == 100);
    CHECK_FALSE (folder.getChildFile ("render.mov.part").exists());

    video::movie::Info info;
    const auto frames = framesOf (target, info);
    CHECK (info.codec == "Hap1");
    CHECK (info.width == width);
    CHECK (info.height == height);
    CHECK (info.timeScale == 10u);
    CHECK (info.frameDuration == 1u);
    CHECK (info.duration == doctest::Approx (2.0));
    REQUIRE (frames.size() == 20u);

    for (std::size_t k = 0; k < 20; ++k)
    {
        const auto from = k < 10 ? 20 + k : k - 10;
        INFO ("frame " << k << " from source frame " << from);
        CHECK (frames[k] == sourceFrames[from]);
    }

    /*  A SECTION ALONE with no fade: the frames are the source's. */
    const auto one = video::movie::renderMovieEdit (source, { sec (1.0, 2.0) }, target);
    REQUIRE_MESSAGE (one.ok, one.problem);
    const auto alone = framesOf (target, info);
    REQUIRE (alone.size() == 10u);

    for (std::size_t k = 0; k < 10; ++k)
        CHECK (alone[k] == sourceFrames[10 + k]);

    folder.deleteRecursively();
}

TEST_CASE ("movie edit render: a dissolve blends from the edges, centred on the join, and the rest is the source's bytes")
{
    const auto folder = scratchFolder ("godot-medit");
    const auto source = writeFixtureMovie (folder, "three.mov");
    const auto target = folder.getChildFile ("render.mov").getFullPathName().toStdString();

    video::movie::Info sourceInfo;
    const auto sourceFrames = framesOf (source, sourceInfo);

    /*  [0,1] THEN [2,3] with four frames of dissolve: the window is [0.8, 1.2),
        frames 8 to 11, judged at 0.85, 0.95, 1.05 and 1.15. The outgoing
        picture goes on past its out point into the file's second second -
        green - and the incoming begins before its in point, still green. */
    const auto result = video::movie::renderMovieEdit (source, { sec (0.0, 1.0, 0.0, 0.4), sec (2.0, 3.0, 0.4) }, target);
    REQUIRE_MESSAGE (result.ok, result.problem);

    video::movie::Info info;
    const auto frames = framesOf (target, info);
    REQUIRE (frames.size() == 20u);

    for (std::size_t k = 0; k < 8; ++k)
        CHECK (frames[k] == sourceFrames[k]);

    for (std::size_t k = 12; k < 20; ++k)
        CHECK (frames[k] == sourceFrames[k + 10]);

    near (pixelOf (frames[8]), 255 * 0.875, 255 * 0.125, 0);     // red 7/8 (0.85 s), green 1/8 (1.85 s)
    near (pixelOf (frames[9]), 255 * 0.625, 255 * 0.375, 0);     // red 5/8 (0.95 s), green 3/8 (1.95 s)
    near (pixelOf (frames[10]), 0, 255 * 0.375, 255 * 0.625);    // green 3/8 (1.05 s), blue 5/8 (2.05 s)
    near (pixelOf (frames[11]), 0, 255 * 0.125, 255 * 0.875);    // green 1/8 (1.15 s), blue 7/8 (2.15 s)

    /*  [2,3] THEN [1,2]: the outgoing runs off the end of the file into
        black (3.05 s and 3.15 s), the incoming begins before its in point
        in the file's first second, red. */
    const auto edges = video::movie::renderMovieEdit (source, { sec (2.0, 3.0, 0.0, 0.4), sec (1.0, 2.0, 0.4) }, target);
    REQUIRE_MESSAGE (edges.ok, edges.problem);
    const auto atEdges = framesOf (target, info);
    REQUIRE (atEdges.size() == 20u);

    near (pixelOf (atEdges[8]), 255 * 0.125, 0, 255 * 0.875);    // blue 7/8, red 1/8
    near (pixelOf (atEdges[9]), 255 * 0.375, 0, 255 * 0.625);
    near (pixelOf (atEdges[10]), 0, 255 * 0.625, 0);             // black 3/8, green 5/8
    near (pixelOf (atEdges[11]), 0, 255 * 0.875, 0);
    CHECK (atEdges[7] == sourceFrames[27]);
    CHECK (atEdges[12] == sourceFrames[12]);

    /*  A CROSSFADE INTO THE FILE'S FIRST SECOND is clamped to nothing, as
        the sound's is (there is nothing before the start to fade from): a
        hard cut, every frame the source's. */
    const auto atStart = video::movie::renderMovieEdit (source, { sec (2.0, 3.0), sec (0.0, 1.0, 0.4) }, target);
    REQUIRE_MESSAGE (atStart.ok, atStart.problem);
    const auto hardCut = framesOf (target, info);
    REQUIRE (hardCut.size() == 20u);

    for (std::size_t k = 0; k < 20; ++k)
        CHECK (hardCut[k] == sourceFrames[k < 10 ? 20 + k : k - 10]);

    /*  A JOIN STILL ONE IN THE FILE plays plain, whatever its crossfade. */
    const auto plain = video::movie::renderMovieEdit (source, { sec (0.0, 1.0), sec (1.0, 3.0, 0.6) }, target);
    REQUIRE_MESSAGE (plain.ok, plain.problem);
    const auto continuous = framesOf (target, info);
    REQUIRE (continuous.size() == 30u);

    for (std::size_t k = 0; k < 30; ++k)
        CHECK (continuous[k] == sourceFrames[k]);

    folder.deleteRecursively();
}

TEST_CASE ("movie edit render: a gap is black, and a fade at a free edge fades from black and to it (55.9, AEG)")
{
    const auto folder = scratchFolder ("godot-medit");
    const auto source = writeFixtureMovie (folder, "three.mov");
    const auto target = folder.getChildFile ("render.mov").getFullPathName().toStdString();

    video::movie::Info sourceInfo;
    const auto sourceFrames = framesOf (source, sourceInfo);

    /*  The red second fading in over 200 ms and out over 400; half a second of
        black; the blue second fading in over 400 ms. Twenty-five frames. */
    const auto result = video::movie::renderMovieEdit (source, { sec (0.0, 1.0, 0.2, 0.4), sec (2.0, 3.0, 0.4, 0.0, 0.5) },
                                                       target);
    REQUIRE_MESSAGE (result.ok, result.problem);

    video::movie::Info info;
    const auto frames = framesOf (target, info);
    REQUIRE (frames.size() == 25u);

    near (pixelOf (frames[0]), 255 * 0.25, 0, 0);       // 0.05 s of 0.2
    near (pixelOf (frames[1]), 255 * 0.75, 0, 0);
    CHECK (frames[3] == sourceFrames[3]);
    near (pixelOf (frames[7]), 255 * 0.625, 0, 0);      // 0.75 s: 0.15 into a fade out of 0.4
    near (pixelOf (frames[12]), 0, 0, 0);               // the gap
    near (pixelOf (frames[16]), 0, 0, 255 * 0.375);     // 1.65 s: 0.15 into a fade in of 0.4
    CHECK (frames[20] == sourceFrames[25]);             // 2.05 s: the blue second at 2.55 of the file
}

TEST_CASE ("movie edit render: Hap Alpha and Hap Q keep their texture, and beyond the file Hap Alpha is clear")
{
    const auto folder = scratchFolder ("godot-medit");
    const auto target = folder.getChildFile ("render.mov").getFullPathName().toStdString();

    for (const auto& [texture, codec] : { std::pair { video::hap::Texture::dxt5, "Hap5" },
                                          std::pair { video::hap::Texture::ycocgDxt5, "HapY" } })
    {
        INFO (codec);
        const auto source = writeEncodedMovie (folder, (std::string (codec) + ".mov").c_str(), texture, codec);

        video::movie::Info sourceInfo;
        const auto sourceFrames = framesOf (source, sourceInfo);
        REQUIRE (sourceInfo.codec == codec);

        const auto result = video::movie::renderMovieEdit (source, { sec (2.0, 3.0, 0.0, 0.4), sec (1.0, 2.0, 0.4) }, target);
        REQUIRE_MESSAGE (result.ok, result.problem);

        video::movie::Info info;
        const auto frames = framesOf (target, info);
        CHECK (info.codec == codec);
        REQUIRE (frames.size() == 20u);

        for (std::size_t k = 0; k < 8; ++k)
            CHECK (frames[k] == sourceFrames[20 + k]);

        for (std::size_t k = 12; k < 20; ++k)
            CHECK (frames[k] == sourceFrames[k]);

        //  Hap Q's chroma lands within its own tolerance; Hap Alpha's is DXT5's.
        const auto within = texture == video::hap::Texture::ycocgDxt5 ? 16.0 : 10.0;
        near (pixelOf (frames[9]), 255 * 0.375, 0, 255 * 0.625, within);
        near (pixelOf (frames[10]), 0, 255 * 0.625, 0, within);

        if (texture == video::hap::Texture::dxt5)
        {
            /*  Black beyond the file is CLEAR where the codec carries alpha:
                the outgoing at 3.05 s and 3.15 s is past the end; at 2.95 s it
                is still blue, and solid. */
            CHECK (pixelOf (frames[9]).a == doctest::Approx (1.0));
            CHECK (pixelOf (frames[10]).a == doctest::Approx (0.625).epsilon (0.08));
            CHECK (pixelOf (frames[11]).a == doctest::Approx (0.875).epsilon (0.08));
            CHECK (pixelOf (frames[7]).a == doctest::Approx (1.0));
        }
    }

    folder.deleteRecursively();
}

TEST_CASE ("movie edit render: a variable-rate source lands on its dominant grid and says so")
{
    const auto folder = scratchFolder ("godot-medit");
    const auto target = folder.getChildFile ("render.mov").getFullPathName().toStdString();

    /*  SIX FRAMES ON A SCALE OF 600: four of 24 ticks, two of 20 - a 25 fps
        grid with a faster tail, 136 ticks long. The edit is the whole file,
        which is no edit for a sound; here it is rendered since the test
        asks, onto six frames of 24 ticks. */
    std::vector<Bytes> frames;

    for (int n = 0; n < 6; ++n)
        frames.push_back (section (0xAB, solidDxt1 (width, height, colours[n % 3])));

    const auto source = writeMovie (folder, "uneven.mov", hapMovieTimed (width, height, frames, 600, { { 4, 24 }, { 2, 20 } }))
                          .getFullPathName().toStdString();

    const auto result = video::movie::renderMovieEdit (source, { sec (0.0, 136.0 / 600.0) }, target);
    REQUIRE_MESSAGE (result.ok, result.problem);
    CHECK (result.resampled);
    CHECK (result.frameRate == doctest::Approx (25.0));
    CHECK (result.frames == 6);
    CHECK (result.seconds == doctest::Approx (6.0 * 24.0 / 600.0));

    video::movie::Info info;
    const auto rendered = framesOf (target, info);
    REQUIRE (rendered.size() == 6u);
    CHECK (info.constantRate);
    CHECK (info.timeScale == 600u);
    CHECK (info.frameDuration == 24u);

    folder.deleteRecursively();
}

TEST_CASE ("movie edit render: stopped it leaves no file; a movie that is not HAP, or no movie, is refused in words")
{
    const auto folder = scratchFolder ("godot-medit");
    const auto target = folder.getChildFile ("render.mov").getFullPathName().toStdString();
    const auto source = writeFixtureMovie (folder, "three.mov");

    std::atomic<bool> stop { true };
    const auto stopped = video::movie::renderMovieEdit (source, { sec (2.0, 3.0), sec (0.0, 1.0) }, target, &stop);
    CHECK_FALSE (stopped.ok);
    CHECK (stopped.problem == "stopped");
    CHECK_FALSE (folder.getChildFile ("render.mov").exists());
    CHECK_FALSE (folder.getChildFile ("render.mov.part").exists());

    const auto preview = writeFixtureMovie (folder, "preview.mov", "avc1");
    const auto refused = video::movie::renderMovieEdit (preview, { sec (0.0, 1.0) }, target);
    CHECK_FALSE (refused.ok);
    CHECK (refused.problem == video::movie::convertFirst);
    CHECK (refused.problem.find ("Show > Convert the movie to HAP") != std::string::npos);

    const auto junk = writeMovie (folder, "junk.mov", Bytes (64, 0x41)).getFullPathName().toStdString();
    CHECK (video::movie::renderMovieEdit (junk, { sec (0.0, 1.0) }, target).problem.find ("could not be read") != std::string::npos);

    CHECK (video::movie::renderMovieEdit (source, {}, target).problem == "there are no sections to render");
    CHECK_FALSE (video::movie::renderMovieEdit (source, { sec (2.0, 1.0) }, target).problem.empty());
    CHECK_FALSE (folder.getChildFile ("render.mov").exists());

    folder.deleteRecursively();
}

TEST_CASE ("hap: a whole frame decoded agrees with the pixel reader, texel for texel, in every texture")
{
    constexpr int w = 20, h = 12;   // not a multiple of four down, so the edge blocks are read too
    std::vector<std::uint8_t> picture (static_cast<std::size_t> (w * h * 4));

    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            auto* at = picture.data() + static_cast<std::size_t> (y * w + x) * 4;
            at[0] = static_cast<std::uint8_t> (255 * x / (w - 1));
            at[1] = static_cast<std::uint8_t> (255 * y / (h - 1));
            at[2] = static_cast<std::uint8_t> ((x * 37 + y * 11) % 256);
            at[3] = static_cast<std::uint8_t> (x < w / 2 ? 255 : 64 + y * 10);
        }

    for (const auto texture : { video::hap::Texture::dxt1, video::hap::Texture::dxt5, video::hap::Texture::ycocgDxt5 })
    {
        INFO ("texture " << static_cast<int> (texture));
        std::vector<std::uint8_t> blocks, rgba;
        video::hap::encodeTexture (texture, picture.data(), w, h, static_cast<std::size_t> (w) * 4, blocks);
        REQUIRE (video::hap::decodeTexture (texture, blocks, w, h, rgba));
        REQUIRE (rgba.size() == picture.size());

        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
                REQUIRE (video::hap::pixelAt (texture, blocks, w, h, x, y, r, g, b, a));
                const auto* got = rgba.data() + static_cast<std::size_t> (y * w + x) * 4;
                CHECK (got[0] == static_cast<std::uint8_t> (std::lround (r)));
                CHECK (got[1] == static_cast<std::uint8_t> (std::lround (g)));
                CHECK (got[2] == static_cast<std::uint8_t> (std::lround (b)));
                CHECK (got[3] == static_cast<std::uint8_t> (std::lround (a * 255.0)));
            }

        //  And the threaded encoder makes the same blocks as the plain one.
        std::vector<std::uint8_t> threaded;
        video::hap::encodeTextureThreaded (texture, picture.data(), w, h, static_cast<std::size_t> (w) * 4, threaded, 3);
        CHECK (threaded == blocks);
    }

    //  Out of its blocks, nothing.
    std::vector<std::uint8_t> rgba;
    CHECK_FALSE (video::hap::decodeTexture (video::hap::Texture::dxt1, std::vector<std::uint8_t> (8, 0), 8, 8, rgba));
    CHECK_FALSE (video::hap::decodeTexture (video::hap::Texture::none, std::vector<std::uint8_t> (64, 0), 8, 8, rgba));
}
