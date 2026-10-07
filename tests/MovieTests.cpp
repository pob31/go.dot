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

#include <wfg/engine/video/Ffmpeg.h>
#include <wfg/engine/video/Hap.h>
#include <wfg/engine/video/HapEncoder.h>
#include <wfg/engine/video/Movie.h>
#include <wfg/engine/video/MovieWriter.h>
#include <wfg/engine/video/PipedChild.h>
#include <wfg/engine/video/Snappy.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
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

//==============================================================================
/*  A REAL ENCODER'S FILES (namespace draft 37.4 owed it): HAP movies FFmpeg's
    own encoder wrote - Hap, Hap Alpha, Hap Q, chunked and uncompressed - read
    by Go.dot's reader and decoded on the CPU, against FFmpeg's decode of the
    same frame. Skipped unless WFG_HAP_DIR names a folder of `<name>.mov` with,
    beside each, `<name>.rgba`: frame 30 as FFmpeg decodes it, raw RGBA. */
TEST_CASE ("movie: a real encoder's HAP files read, frame for frame as FFmpeg decodes them")
{
    const auto folder = juce::SystemStats::getEnvironmentVariable ("WFG_HAP_DIR", {});

    if (folder.isEmpty())
        return;

    const auto movies = juce::File (folder).findChildFiles (juce::File::findFiles, false, "*.mov");
    REQUIRE_FALSE (movies.isEmpty());

    for (const auto& file : movies)
    {
        CAPTURE (file.getFileName().toStdString());

        juce::MemoryBlock reference;
        REQUIRE (file.withFileExtension ("rgba").loadFileAsData (reference));

        video::movie::MovieFile movie;
        std::string why;
        REQUIRE (movie.open (file.getFullPathName().toStdString(), why));
        CHECK (movie.info().isHap());

        const auto width = movie.info().width;
        const auto height = movie.info().height;
        REQUIRE (reference.getSize() == static_cast<std::size_t> (width * height * 4));
        REQUIRE (movie.info().frames.size() > 30);

        std::vector<std::uint8_t> bytes, texture;
        REQUIRE (movie.readFrame (30, bytes));

        auto format = video::hap::Texture::none;
        REQUIRE (video::hap::unpack (bytes.data(), bytes.size(), format, texture));

        const auto* expected = static_cast<const std::uint8_t*> (reference.getData());
        double worst = 0.0, total = 0.0;

        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
                REQUIRE (video::hap::pixelAt (format, texture, width, height, x, y, r, g, b, a));

                const auto* want = expected + (static_cast<std::size_t> (y) * static_cast<std::size_t> (width)
                                               + static_cast<std::size_t> (x)) * 4;
                const double got[4] { r, g, b, a * 255.0 };

                for (int c = 0; c < 4; ++c)
                {
                    const auto off = std::abs (got[c] - static_cast<double> (want[c]));
                    worst = std::max (worst, off);
                    total += off;
                }
            }

        const auto mean = total / (static_cast<double> (width) * static_cast<double> (height) * 4.0);
        MESSAGE (file.getFileName() << ": mean " << mean << ", worst " << worst << " of 255");
        CHECK (mean < 1.0);
        CHECK (worst < 8.0);
    }
}

//==============================================================================
/*  FFMPEG, ASKED WHAT A FILE HOLDS (namespace draft 37.6, F.1): ffprobe's
    JSON read - the first video stream, not a cover picture; whether it has
    alpha and sound; a rate written as a fraction; a length from the format
    when the stream has none. */
TEST_CASE ("movie: ffprobe's answer read - the picture, its alpha, its sound, its rate and length")
{
    const auto movie = video::ffmpeg::parseProbe (R"({
        "streams": [
            { "codec_type": "video", "codec_name": "mjpeg", "width": 600, "height": 600,
              "avg_frame_rate": "0/0", "disposition": { "attached_pic": 1 } },
            { "codec_type": "video", "codec_name": "prores", "width": 1920, "height": 1080,
              "pix_fmt": "yuva444p12le", "avg_frame_rate": "30000/1001", "duration": "12.512500",
              "disposition": { "attached_pic": 0 } },
            { "codec_type": "audio", "codec_name": "pcm_s24le", "channels": 2, "sample_rate": "48000" }
        ],
        "format": { "duration": "12.600000" } })");

    REQUIRE (movie.ok);
    CHECK (movie.codec == "prores");
    CHECK (movie.width == 1920);
    CHECK (movie.height == 1080);
    CHECK (movie.frameRate == doctest::Approx (29.97).epsilon (0.001));
    CHECK (movie.duration == doctest::Approx (12.5125));
    CHECK (movie.alpha);
    CHECK (movie.sound);
    CHECK (movie.soundChannels == 2);
    CHECK (movie.soundRate == 48000);
    CHECK_FALSE (movie.isHap());

    const auto plain = video::ffmpeg::parseProbe (R"({ "streams": [
        { "codec_type": "video", "codec_name": "h264", "width": 1280, "height": 720,
          "pix_fmt": "yuv420p", "avg_frame_rate": "25/1" } ],
        "format": { "duration": "4.000000" } })");

    REQUIRE (plain.ok);
    CHECK_FALSE (plain.alpha);
    CHECK_FALSE (plain.sound);
    CHECK (plain.duration == doctest::Approx (4.0));

    CHECK_FALSE (video::ffmpeg::parseProbe (R"({ "streams": [ { "codec_type": "audio", "channels": 2 } ] })").ok);
    CHECK_FALSE (video::ffmpeg::parseProbe ("not json").ok);
}

/*  AND ASKED FOR REAL, where FFmpeg is on this machine: skipped where it is
    not, which is CI until F.7 ships it. A movie FFmpeg makes, then asks. */
TEST_CASE ("movie: FFmpeg, where it is found, describes a movie it made")
{
    const auto tools = video::ffmpeg::find();

    if (! tools.found())
        return;

    const auto folder = juce::File::createTempFile ("ffmpeg");
    REQUIRE (folder.createDirectory());
    const auto movie = folder.getChildFile ("made.mov");

    video::PipedChild maker;
    REQUIRE (maker.start ({ tools.ffmpeg, "-nostdin", "-v", "error", "-y",
                            "-f", "lavfi", "-i", "testsrc2=size=320x240:rate=25:duration=1",
                            "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=1",
                            "-c:v", "mpeg4", "-c:a", "pcm_s16le", movie.getFullPathName().toStdString() }));
    maker.readAll();
    REQUIRE (maker.wait (30000) == 0);

    const auto probed = video::ffmpeg::probe (tools, movie.getFullPathName().toStdString());
    REQUIRE (probed.ok);
    CHECK (probed.codec == "mpeg4");
    CHECK (probed.width == 320);
    CHECK (probed.height == 240);
    CHECK (probed.frameRate == doctest::Approx (25.0));
    CHECK (probed.duration == doctest::Approx (1.0).epsilon (0.05));
    CHECK (probed.sound);

    const auto missing = video::ffmpeg::probe (tools, folder.getChildFile ("none.mov").getFullPathName().toStdString());
    CHECK_FALSE (missing.ok);

    folder.deleteRecursively();
}

//==============================================================================
/*  GO.DOT'S OWN HAP, WRITTEN (namespace draft 37.6, F.2, WK). */
namespace
{
    /*  A PICTURE WITH SOMETHING IN IT: smooth ramps, a hard edge, a ring of
        colour and a soft alpha - what a frame of a show is made of. */
    std::vector<std::uint8_t> testPicture (int width, int height)
    {
        std::vector<std::uint8_t> rgba (static_cast<std::size_t> (width * height * 4));

        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                auto* at = rgba.data() + (static_cast<std::size_t> (y * width + x)) * 4;
                const auto dx = x - width / 2.0, dy = y - height / 2.0;
                const auto ring = std::sqrt (dx * dx + dy * dy) / (width / 2.0);

                at[0] = static_cast<std::uint8_t> (255 * x / std::max (1, width - 1));
                at[1] = static_cast<std::uint8_t> (255 * y / std::max (1, height - 1));
                at[2] = static_cast<std::uint8_t> (x < width / 3 ? 40.0 : 128.0 + 127.0 * std::sin (ring * 6.0));
                at[3] = static_cast<std::uint8_t> (std::clamp (255.0 * (1.2 - ring), 0.0, 255.0));
            }

        return rgba;
    }

    /*  HOW FAR A TEXTURE DECODES FROM THE PICTURE: the mean and the worst,
        over the colour (and the alpha, `withAlpha`). */
    std::pair<double, double> errorOf (video::hap::Texture texture, const std::vector<std::uint8_t>& blocks,
                                       const std::vector<std::uint8_t>& rgba, int width, int height, bool withAlpha)
    {
        double total = 0.0, worst = 0.0;
        const auto channels = withAlpha ? 4 : 3;

        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
                REQUIRE (video::hap::pixelAt (texture, blocks, width, height, x, y, r, g, b, a));

                const auto* want = rgba.data() + static_cast<std::size_t> (y * width + x) * 4;
                const double got[4] { r, g, b, a * 255.0 };

                for (int c = 0; c < channels; ++c)
                {
                    const auto off = std::abs (got[c] - static_cast<double> (want[c]));
                    total += off;
                    worst = std::max (worst, off);
                }
            }

        return { total / (static_cast<double> (width * height) * channels), worst };
    }
}

TEST_CASE ("movie: Snappy compresses to what it decompresses from")
{
    juce::Random random (37);

    std::vector<std::vector<std::uint8_t>> inputs;
    inputs.push_back ({});
    inputs.push_back ({ 7 });
    inputs.push_back (std::vector<std::uint8_t> (200000, 0x42));

    std::vector<std::uint8_t> noise (70001);
    for (auto& byte : noise) byte = static_cast<std::uint8_t> (random.nextInt (256));
    inputs.push_back (noise);

    //  Runs and repeats at every distance, across fragment edges.
    std::vector<std::uint8_t> patterned;
    for (int n = 0; n < 300000; ++n)
        patterned.push_back (static_cast<std::uint8_t> ((n % 7) * 13 + ((n / 3000) % 5) + (random.nextInt (50) == 0 ? 1 : 0)));
    inputs.push_back (patterned);

    for (const auto& input : inputs)
    {
        CAPTURE (input.size());

        std::vector<std::uint8_t> packed, unpacked;
        video::snappy::compress (input.data(), input.size(), packed);
        REQUIRE (video::snappy::decompress (packed.data(), packed.size(), unpacked));
        CHECK (unpacked == input);
    }

    std::vector<std::uint8_t> packed;
    video::snappy::compress (inputs[2].data(), inputs[2].size(), packed);
    CHECK (packed.size() < inputs[2].size() / 20);
}

TEST_CASE ("movie: Go.dot's HAP encoder - Hap, Hap Alpha and Hap Q come back close to the picture")
{
    constexpr int width = 130, height = 70;     // not a multiple of four: edge blocks repeat the edge
    const auto picture = testPicture (width, height);

    for (const auto texture : { video::hap::Texture::dxt1, video::hap::Texture::dxt5, video::hap::Texture::ycocgDxt5 })
    {
        CAPTURE (static_cast<int> (texture));

        std::vector<std::uint8_t> blocks;
        video::hap::encodeTexture (texture, picture.data(), width, height, static_cast<std::size_t> (width) * 4, blocks);
        CHECK (blocks.size() == static_cast<std::size_t> (((width + 3) / 4) * ((height + 3) / 4)) * video::hap::bytesPerBlock (texture));

        const auto [mean, worst] = errorOf (texture, blocks, picture, width, height, texture == video::hap::Texture::dxt5);
        MESSAGE ("texture " << static_cast<int> (texture) << ": mean " << mean << ", worst " << worst << " of 255");
        CHECK (mean < 4.0);
        CHECK (worst < 48.0);

        /*  AND AS A FRAME: one section, packed, and unpacked to the same blocks. */
        std::vector<std::uint8_t> frame, scratch, back;
        video::hap::packFrame (texture, blocks, frame, scratch);

        auto format = video::hap::Texture::none;
        REQUIRE (video::hap::unpack (frame.data(), frame.size(), format, back));
        CHECK (format == texture);
        CHECK (back == blocks);
    }

    /*  ONE COLOUR IS EXACT, as far as 5:6:5 goes: pure white, black, a primary. */
    for (const std::uint32_t rgb : { 0xffffffu, 0x000000u, 0xff0000u })
    {
        std::vector<std::uint8_t> flat (16 * 4);

        for (std::size_t p = 0; p < 16; ++p)
        {
            flat[p * 4] = static_cast<std::uint8_t> (rgb >> 16);
            flat[p * 4 + 1] = static_cast<std::uint8_t> (rgb >> 8);
            flat[p * 4 + 2] = static_cast<std::uint8_t> (rgb);
            flat[p * 4 + 3] = 255;
        }

        std::vector<std::uint8_t> blocks;
        video::hap::encodeTexture (video::hap::Texture::dxt1, flat.data(), 4, 4, 16, blocks);
        CHECK (errorOf (video::hap::Texture::dxt1, blocks, flat, 4, 4, false).second < 0.5);
    }
}

TEST_CASE ("movie: a movie Go.dot writes is read back frame for frame")
{
    constexpr int width = 64, height = 48;
    const auto folder = juce::File::createTempFile ("hapwrite");
    REQUIRE (folder.createDirectory());
    const auto path = folder.getChildFile ("written.mov").getFullPathName().toStdString();

    std::vector<std::vector<std::uint8_t>> frames;
    video::movie::MovieWriter writer;
    std::string why;
    REQUIRE (writer.open (path, "Hap1", width, height, 30000, 1001, why));

    for (int n = 0; n < 5; ++n)
    {
        auto picture = testPicture (width, height);

        for (std::size_t p = 0; p < picture.size(); p += 4)
            picture[p] = static_cast<std::uint8_t> (picture[p] + n * 40);

        std::vector<std::uint8_t> blocks, frame, scratch;
        video::hap::encodeTexture (video::hap::Texture::dxt1, picture.data(), width, height, width * 4u, blocks);
        video::hap::packFrame (video::hap::Texture::dxt1, blocks, frame, scratch);
        REQUIRE (writer.write (frame.data(), frame.size()));
        frames.push_back (frame);
    }

    REQUIRE (writer.finish (why));

    video::movie::MovieFile movie;
    REQUIRE (movie.open (path, why));
    CHECK (movie.info().codec == "Hap1");
    CHECK (movie.info().width == width);
    CHECK (movie.info().height == height);
    REQUIRE (movie.info().frames.size() == 5);
    CHECK (movie.info().duration == doctest::Approx (5.0 * 1001.0 / 30000.0));
    CHECK (movie.info().frameRate() == doctest::Approx (29.97).epsilon (0.001));

    for (int n = 0; n < 5; ++n)
    {
        std::vector<std::uint8_t> bytes;
        REQUIRE (movie.readFrame (n, bytes));
        CHECK (bytes == frames[static_cast<std::size_t> (n)]);
    }

    /*  AND FFMPEG READS IT TOO, where it is found: the file is QuickTime,
        not only what Go.dot's reader forgives. */
    if (const auto tools = video::ffmpeg::find(); tools.found())
    {
        const auto probed = video::ffmpeg::probe (tools, path);
        REQUIRE (probed.ok);
        CHECK (probed.isHap());
        CHECK (probed.width == width);
        CHECK (probed.height == height);
        CHECK (probed.frameRate == doctest::Approx (29.97).epsilon (0.001));

        video::PipedChild decode;
        REQUIRE (decode.start ({ tools.ffmpeg, "-nostdin", "-v", "error", "-i", path,
                                 "-f", "rawvideo", "-pix_fmt", "rgba", "-" }));
        const auto raw = decode.readAll();
        CHECK (decode.wait (30000) == 0);
        REQUIRE (raw.size() == static_cast<std::size_t> (5 * width * height * 4));

        //  Frame 2 as FFmpeg decodes it, against Go.dot's decode of the same.
        std::vector<std::uint8_t> bytes, blocks;
        REQUIRE (movie.readFrame (2, bytes));
        auto format = video::hap::Texture::none;
        REQUIRE (video::hap::unpack (bytes.data(), bytes.size(), format, blocks));

        const std::vector<std::uint8_t> theirs (raw.begin() + 2 * width * height * 4, raw.begin() + 3 * width * height * 4);
        CHECK (errorOf (format, blocks, theirs, width, height, false).first < 1.0);
    }

    folder.deleteRecursively();
}
