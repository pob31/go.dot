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

/*  THE RENDERER ON EACH SYSTEM'S OWN GRAPHICS (namespace draft §44, R.1): the
    pixels the GPU draws, read back and held to the reference compositor
    (Compositor.h) - so the rules of the picture stay written once, in
    arithmetic, and the shaders are checked against them rather than trusted.

    Drawn on the system's software rasteriser - WARP on Windows, llvmpipe on
    Linux - so CI's machines, which have no graphics card, draw what a booth's
    would. macOS has none: there the cases run where a Metal device is, and say
    so where it is not. On Windows a device is required, since WARP always is.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/video/Compositor.h>
#include <wfg/engine/video/Grade.h>
#include <wfg/engine/video/Mapping.h>
#include <wfg/engine/video/VideoRegion.h>
#include <wfg/engine/video/render/Gpu.h>
#include <wfg/engine/video/render/Painter.h>

#include <juce_cryptography/juce_cryptography.h>
#include <juce_graphics/juce_graphics.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace wfg;

namespace
{
    namespace region = video::region;

    /*  THE DEVICE FOR ONE CASE, let go at its end - or none, said, where the
        system has nothing to draw with. */
    struct Device
    {
        Device()
        {
            /*  WFG_GPU_HARDWARE=1 draws on the machine's own card instead -
                how a booth's driver is checked against the reference. */
            video::gpu::OpenOptions options;
            options.software = juce::SystemStats::getEnvironmentVariable ("WFG_GPU_HARDWARE", {}) != "1";
            open = video::gpu::open (options, why);

            if (open)
                MESSAGE ("drawing on " << video::gpu::describe());

           #if JUCE_WINDOWS
            REQUIRE_MESSAGE (open, why);
           #else
            if (! open)
                MESSAGE ("no device to draw on here (" << why << "): the GPU's pixels are not checked on this machine");
           #endif
        }

        ~Device()
        {
            video::gpu::close();
        }

        bool open = false;
        std::string why;
    };

    /*  A TEST'S PICTURES AND FRAMES, made up, and the same pictures as the
        reference compositor reads them - nearest pixel, straight colour, as
        the renderer's own sampler does. */
    struct Pictures final : video::render::Sources, video::PictureSampler
    {
        juce::Image picture (const std::string& path, std::uint64_t& version) const override
        {
            const auto found = images.find (path);
            version = 1;
            return found != images.end() ? found->second : juce::Image {};
        }

        std::shared_ptr<const video::render::MovieFrame> movieFrame (const std::string& path, double) const override
        {
            const auto found = frames.find (path);
            return found != frames.end() ? found->second : nullptr;
        }

        bool sizeOf (const std::string& path, int& width, int& height) const override
        {
            const auto found = images.find (path);

            if (found == images.end())
                return false;

            width = found->second.getWidth();
            height = found->second.getHeight();
            return true;
        }

        bool colourAt (const std::string& path, double u, double v,
                       double& red, double& green, double& blue, double& alpha) const override
        {
            const auto found = images.find (path);

            if (found == images.end())
                return false;

            const auto& image = found->second;
            const auto x = std::clamp (static_cast<int> (u * image.getWidth()), 0, image.getWidth() - 1);
            const auto y = std::clamp (static_cast<int> ((1.0 - v) * image.getHeight()), 0, image.getHeight() - 1);
            const auto colour = image.getPixelAt (x, y);
            red = colour.getRed();
            green = colour.getGreen();
            blue = colour.getBlue();
            alpha = colour.getFloatAlpha();
            return true;
        }

        std::map<std::string, juce::Image> images;
        std::map<std::string, std::shared_ptr<const video::render::MovieFrame>> frames;
    };

    region::LayerReading layerOf (const std::string& id, region::Source source, double opacity, std::uint64_t order)
    {
        region::LayerReading layer;
        layer.id = id;
        layer.canvas = "C1";
        layer.order = order;
        layer.source = source;
        layer.rings[static_cast<int> (video::Property::opacity)].count = 1;
        layer.rings[static_cast<int> (video::Property::opacity)].points[0] = { 0, opacity };
        return layer;
    }

    /*  A GENTLE PICTURE: a gradient, so the GPU's smooth sampling and the
        reference's nearest pixel differ by less than a step anywhere - and a
        see-through corner. */
    juce::Image gradientPicture (int width, int height)
    {
        juce::Image image (juce::Image::ARGB, width, height, true, juce::SoftwareImageType());

        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                const auto alpha = x < width / 4 && y < height / 4 ? 0.5f : 1.0f;
                image.setPixelAt (x, y, juce::Colour (static_cast<std::uint8_t> (60 + 3 * x),
                                                      static_cast<std::uint8_t> (200 - 2 * y),
                                                      static_cast<std::uint8_t> (90 + x + y), alpha));
            }

        return image;
    }

    struct Comparison
    {
        int pixels = 0;
        int far = 0;            ///< pixels off by more than the tolerance
        double worst = 0.0;
        double mean = 0.0;
    };

    /*  THE GPU'S PICTURE (floats 0..1, rows from the top) against what
        `expected` says each pixel's middle is, in 0..255. */
    template <typename Expected>
    Comparison compare (const std::vector<float>& rgba, int width, int height, double tolerance, Expected&& expected)
    {
        Comparison result;
        double total = 0.0;

        for (int row = 0; row < height; ++row)
            for (int column = 0; column < width; ++column)
            {
                const auto want = expected (column, row);
                const auto* got = rgba.data() + 4 * (static_cast<std::size_t> (row) * static_cast<std::size_t> (width)
                                                     + static_cast<std::size_t> (column));
                double off = 0.0;

                for (int channel = 0; channel < 3; ++channel)
                {
                    const auto wanted = static_cast<double> ((want >> (16 - 8 * channel)) & 0xffu);
                    off = std::max (off, std::abs (static_cast<double> (got[channel]) * 255.0 - wanted));
                }

                ++result.pixels;
                total += off;
                result.worst = std::max (result.worst, off);

                if (off > tolerance)
                    ++result.far;
            }

        result.mean = total / std::max (1, result.pixels);
        return result;
    }

    /*  `layers` composited on the GPU and by the reference, pixel by pixel. */
    Comparison drawAndCompare (const Pictures& pictures, const std::vector<region::LayerReading>& layers, double level)
    {
        constexpr int width = 160, height = 90;
        constexpr std::int64_t sample = 1000;

        region::ConfigReading config;
        config.canvases.push_back ({ "C1", width, height });

        std::vector<float> rgba;
        int readWidth = 0, readHeight = 0;

        {
            video::render::Painter painter (pictures);
            std::string why;
            REQUIRE_MESSAGE (painter.make (why), why);

            painter.beginFrame (config, layers, [level] (const std::string&) { return level; });
            CHECK (painter.canvas ("C1", sample).id != SG_INVALID_ID);
            CHECK (painter.canvas ("nowhere", sample).id == SG_INVALID_ID);
            sg_commit();

            REQUIRE (video::gpu::readBack (painter.canvasImage ("C1"), rgba, readWidth, readHeight));
            painter.endFrame();
        }

        REQUIRE (readWidth == width);
        REQUIRE (readHeight == height);

        std::vector<const region::LayerReading*> stack;

        for (const auto& layer : layers)
            stack.push_back (&layer);

        return compare (rgba, width, height, 6.0, [&] (int column, int row)
        {
            const auto x = (column + 0.5) - width / 2.0;
            const auto y = height / 2.0 - (row + 0.5);
            return video::scaledColour (video::colourAt (stack, sample, width, height, x, y, &pictures), level);
        });
    }

    std::uint32_t rgbOf (const float* pixel) noexcept
    {
        const auto channel = [] (float value)
        {
            return static_cast<std::uint32_t> (std::clamp (std::lround (value * 255.0f), 0L, 255L));
        };

        return (channel (pixel[0]) << 16) | (channel (pixel[1]) << 8) | channel (pixel[2]);
    }
}

TEST_CASE ("video: the shader header was made from the shader source")
{
    /*  scripts/generate-shaders.py stamps the header with the SHA-256 of the
        source it translated: a shader edited and not translated again would
        otherwise build and draw the old one. */
    const juce::File root { juce::String (std::string (WFG_REPO_ROOT)) };
    const auto shaders = root.getChildFile ("src/wfg/engine/video/render/shaders");
    const auto source = shaders.getChildFile ("video.glsl").loadFileAsString().replace ("\r\n", "\n");
    const auto header = shaders.getChildFile ("video.glsl.h").loadFileAsString();

    REQUIRE (source.isNotEmpty());
    const auto bytes = source.toStdString();
    const auto hash = juce::SHA256 (bytes.data(), bytes.size()).toHexString().toStdString();
    const auto stamp = header.upToFirstOccurrenceOf ("\n", false, false).trim().toStdString();

    CHECK (stamp == "// source-sha256: " + hash);
}

TEST_CASE ("video gpu: the GPU composites a canvas as the reference compositor says (R.1)")
{
    Device device;

    if (! device.open)
        return;

    Pictures pictures;
    pictures.images["picture.png"] = gradientPicture (40, 30);

    std::vector<region::LayerReading> layers;

    //  A ground.
    auto ground = layerOf ("L1", region::Source::fill, 1.0, 1);
    ground.paint = 0x204080u;
    layers.push_back (ground);

    //  A panel, scaled, moved and turned, see-through.
    auto panel = layerOf ("L2", region::Source::fill, 0.6, 2);
    panel.paint = 0xC03010u;
    panel.scale = 50.0;
    panel.offsetX = 20.0;
    panel.offsetY = -10.0;
    panel.rotation = 30.0;
    layers.push_back (panel);

    //  A picture, fitted, scaled, turned, screened, graded.
    auto shown = layerOf ("L3", region::Source::picture, 0.8, 3);
    shown.file = "picture.png";
    shown.scale = 80.0;
    shown.rotation = -15.0;
    shown.blend = region::Blend::screen;
    shown.grade.contrast = 120.0;
    shown.grade.saturation = 80.0;
    shown.grade.gamma = 1.2;
    shown.grade.hue = 20.0;
    layers.push_back (shown);

    //  A feathered triangle, added.
    auto triangle = layerOf ("L4", region::Source::mask, 0.5, 4);
    triangle.paint = 0x00FF00u;
    triangle.blend = region::Blend::add;
    triangle.shape.count = 3;
    triangle.shape.x[0] = 0.1f; triangle.shape.y[0] = 0.2f;
    triangle.shape.x[1] = 0.6f; triangle.shape.y[1] = 0.9f;
    triangle.shape.x[2] = 0.3f; triangle.shape.y[2] = 0.8f;
    triangle.shape.feather = 4.0f;
    layers.push_back (triangle);

    //  And one multiplied over all of it, half the canvas, flipped.
    auto gel = layerOf ("L5", region::Source::fill, 0.7, 5);
    gel.paint = 0xFFC080u;
    gel.blend = region::Blend::multiply;
    gel.fit = region::Fit::stretch;
    gel.scale = 60.0;
    gel.flipH = true;
    layers.push_back (gel);

    /*  EACH LAYER ALONE, then all of them: a fault is named by the layer
        that has it. */
    std::vector<std::pair<std::string, std::vector<region::LayerReading>>> scenes;

    //  Each over the ground: a blend over black would hide a layer not drawn.
    for (const auto& layer : layers)
        scenes.push_back ({ layer.id, layer.id == ground.id ? std::vector<region::LayerReading> { layer }
                                                            : std::vector<region::LayerReading> { ground, layer } });

    //  The picture plain - laid normally, ungraded - and graded alone.
    auto plain = shown;
    plain.blend = region::Blend::normal;
    plain.grade = video::Grade {};
    scenes.push_back ({ "the picture, plain", { ground, plain } });

    auto graded = shown;
    graded.blend = region::Blend::normal;
    scenes.push_back ({ "the picture, graded", { ground, graded } });

    scenes.push_back ({ "all, at a level of 0.9", layers });

    for (const auto& [name, scene] : scenes)
    {
        const auto level = scene.size() > 2 ? 0.9 : 1.0;
        const auto result = drawAndCompare (pictures, scene, level);

        /*  EDGES ASIDE: a pixel whose middle lies on a shape's very edge may
            fall either side on either; everywhere else the two agree within a
            few steps of 255 - the GPU's sampling is smooth where the
            reference's is the nearest pixel. */
        INFO (name << ": worst " << result.worst << ", mean " << result.mean << ", " << result.far
              << " of " << result.pixels << " pixels off by more than 6 on " << video::gpu::describe());
        CHECK (result.far * 100 <= result.pixels);
        CHECK (result.mean < 1.5);
    }
}

TEST_CASE ("video gpu: an output warps and calibrates the canvas once composited, and lays its zones (R.1)")
{
    Device device;

    if (! device.open)
        return;

    Pictures pictures;

    auto red = layerOf ("L1", region::Source::fill, 1.0, 1);
    red.paint = 0xC04020u;

    auto blue = layerOf ("L2", region::Source::fill, 1.0, 2);
    blue.canvas = "C2";
    blue.paint = 0x2060E0u;

    constexpr int width = 120, height = 60;

    region::ConfigReading config;
    config.canvases.push_back ({ "C1", 64, 36 });
    config.canvases.push_back ({ "C2", 64, 36 });

    /*  THE OUTPUT: its canvas squeezed into the middle half across by its
        mesh, and calibrated - red's slope halved. */
    region::OutputReading output;
    output.id = "O1";
    output.canvas = "C1";
    output.mesh = video::Mesh::identity();
    output.mesh.x = { 0.25f, 0.75f, 0.25f, 0.75f };
    output.cdl.slope[0] = 0.5;

    /*  A SECOND OUTPUT with a zone: C2 over the left half, on top of C1. */
    region::OutputReading zoned;
    zoned.id = "O2";
    zoned.canvas = "C1";
    zoned.mesh = video::Mesh::identity();
    region::ZoneReading zone;
    zone.canvas = "C2";
    zone.mesh = video::Mesh::identity();
    zone.mesh.x = { 0.0f, 0.5f, 0.0f, 0.5f };
    zoned.zones.push_back (zone);

    std::vector<float> squeezed, laid;
    int w = 0, h = 0;

    {
        video::render::Painter painter (pictures);
        std::string why;
        REQUIRE_MESSAGE (painter.make (why), why);

        painter.beginFrame (config, { red, blue }, {});
        painter.drawOutput (output, 100, painter.offscreenTarget ("O1", width, height, SG_PIXELFORMAT_RGBA8));
        painter.drawOutput (zoned, 100, painter.offscreenTarget ("O2", width, height, SG_PIXELFORMAT_RGBA8));
        sg_commit();

        REQUIRE (video::gpu::readBack (painter.offscreenImage ("O1"), squeezed, w, h));
        REQUIRE (video::gpu::readBack (painter.offscreenImage ("O2"), laid, w, h));
        painter.endFrame();
    }

    REQUIRE (w == width);
    REQUIRE (h == height);

    const auto at = [stride = static_cast<std::size_t> (w)] (const std::vector<float>& rgba, int column, int row)
    {
        return rgbOf (rgba.data() + 4 * (static_cast<std::size_t> (row) * stride + static_cast<std::size_t> (column)));
    };

    const auto near = [] (std::uint32_t got, std::uint32_t want)
    {
        for (int shift : { 16, 8, 0 })
            if (std::abs (static_cast<int> ((got >> shift) & 0xffu) - static_cast<int> ((want >> shift) & 0xffu)) > 2)
                return false;

        return true;
    };

    //  Outside the middle half, nothing; inside it, the canvas, its red halved.
    CHECK (at (squeezed, 5, height / 2) == 0u);
    CHECK (at (squeezed, width - 5, height / 2) == 0u);
    INFO ("the middle reads " << at (squeezed, width / 2, height / 2));
    CHECK (near (at (squeezed, width / 2, height / 2), 0x604020u));

    //  The zone covers the left half; its own canvas shows on the right.
    CHECK (near (at (laid, 10, height / 2), 0x2060E0u));
    CHECK (near (at (laid, width - 10, height / 2), 0xC04020u));
}

TEST_CASE ("video gpu: a HAP frame is drawn still compressed, one upload for every output (R.1)")
{
    Device device;

    if (! device.open)
        return;

    /*  AN 8 BY 8 DXT1 FRAME of pure orange-red, 565: both block colours the
        same, every index nought. */
    auto frame = std::make_shared<video::render::MovieFrame>();
    frame->texture = video::hap::Texture::dxt1;
    frame->width = 8;
    frame->height = 8;
    frame->index = 0;

    const std::uint16_t colour565 = static_cast<std::uint16_t> ((31u << 11) | (16u << 5) | 0u);

    for (int block = 0; block < 4; ++block)
    {
        const std::uint8_t bytes[8] { static_cast<std::uint8_t> (colour565 & 0xffu), static_cast<std::uint8_t> (colour565 >> 8),
                                      static_cast<std::uint8_t> (colour565 & 0xffu), static_cast<std::uint8_t> (colour565 >> 8),
                                      0, 0, 0, 0 };
        frame->blocks.insert (frame->blocks.end(), bytes, bytes + 8);
    }

    Pictures pictures;
    pictures.frames["movie.mov"] = frame;

    auto shown = layerOf ("L1", region::Source::movie, 1.0, 1);
    shown.file = "movie.mov";
    shown.fit = region::Fit::stretch;

    region::ConfigReading config;
    config.canvases.push_back ({ "C1", 32, 32 });

    std::vector<float> rgba;
    int w = 0, h = 0;

    {
        video::render::Painter painter (pictures);
        std::string why;
        REQUIRE_MESSAGE (painter.make (why), why);

        painter.beginFrame (config, { shown }, {});
        painter.canvas ("C1", 100);
        sg_commit();

        REQUIRE (video::gpu::readBack (painter.canvasImage ("C1"), rgba, w, h));
        painter.endFrame();
    }

    //  565's 16 of 63 green is 65 of 255.
    const auto middle = rgbOf (rgba.data() + 4 * (static_cast<std::size_t> (h / 2) * static_cast<std::size_t> (w)
                                                  + static_cast<std::size_t> (w / 2)));
    INFO ("the middle reads " << middle);
    CHECK (((middle >> 16) & 0xffu) >= 253u);
    CHECK (std::abs (static_cast<int> ((middle >> 8) & 0xffu) - 65) <= 2);
    CHECK ((middle & 0xffu) <= 2u);
}

TEST_CASE ("video gpu: a Hap Q frame turned back from YCoCg, and a preview's straight RGBA (R.1)")
{
    Device device;

    if (! device.open)
        return;

    /*  AN 8 BY 8 HAP Q FRAME: every block's alpha (Y) 128, its colour 565
        (16, 32, 0) - which decodes to Co 132, Cg 130 and no scale - so the
        shader's arithmetic gives (130, 130, 122). */
    auto q = std::make_shared<video::render::MovieFrame>();
    q->texture = video::hap::Texture::ycocgDxt5;
    q->width = 8;
    q->height = 8;
    q->index = 0;

    const std::uint16_t colour565 = static_cast<std::uint16_t> ((16u << 11) | (32u << 5));

    for (int block = 0; block < 4; ++block)
    {
        const std::uint8_t bytes[16] { 128, 128, 0, 0, 0, 0, 0, 0,
                                       static_cast<std::uint8_t> (colour565 & 0xffu), static_cast<std::uint8_t> (colour565 >> 8),
                                       static_cast<std::uint8_t> (colour565 & 0xffu), static_cast<std::uint8_t> (colour565 >> 8),
                                       0, 0, 0, 0 };
        q->blocks.insert (q->blocks.end(), bytes, bytes + 16);
    }

    //  A preview's frame: 6 by 4, straight RGBA, every pixel (200, 100, 50, 255).
    auto preview = std::make_shared<video::render::MovieFrame>();
    preview->width = 6;
    preview->height = 4;
    preview->index = 0;

    for (int n = 0; n < 6 * 4; ++n)
        preview->rgba.insert (preview->rgba.end(), { 200, 100, 50, 255 });

    Pictures pictures;
    pictures.frames["q.mov"] = q;
    pictures.frames["preview.mp4"] = preview;

    auto left = layerOf ("L1", region::Source::movie, 1.0, 1);
    left.file = "q.mov";
    left.canvas = "C1";
    left.fit = region::Fit::stretch;

    auto right = layerOf ("L2", region::Source::movie, 1.0, 1);
    right.file = "preview.mp4";
    right.canvas = "C2";
    right.fit = region::Fit::stretch;

    region::ConfigReading config;
    config.canvases.push_back ({ "C1", 16, 16 });
    config.canvases.push_back ({ "C2", 16, 16 });

    std::vector<float> fromQ, fromPreview;
    int w = 0, h = 0;

    {
        video::render::Painter painter (pictures);
        std::string why;
        REQUIRE_MESSAGE (painter.make (why), why);

        painter.beginFrame (config, { left, right }, {});
        painter.canvas ("C1", 100);
        painter.canvas ("C2", 100);
        sg_commit();

        REQUIRE (video::gpu::readBack (painter.canvasImage ("C1"), fromQ, w, h));
        REQUIRE (video::gpu::readBack (painter.canvasImage ("C2"), fromPreview, w, h));
        painter.endFrame();
    }

    const auto middleOf = [&w, &h] (const std::vector<float>& rgba)
    {
        return rgbOf (rgba.data() + 4 * (static_cast<std::size_t> (h / 2) * static_cast<std::size_t> (w)
                                         + static_cast<std::size_t> (w / 2)));
    };

    const auto near = [] (std::uint32_t got, std::uint32_t want)
    {
        for (int shift : { 16, 8, 0 })
            if (std::abs (static_cast<int> ((got >> shift) & 0xffu) - static_cast<int> ((want >> shift) & 0xffu)) > 2)
                return false;

        return true;
    };

    INFO ("Hap Q reads " << middleOf (fromQ) << ", the preview " << middleOf (fromPreview));
    CHECK (near (middleOf (fromQ), 0x82827Au));
    CHECK (near (middleOf (fromPreview), 0xC86432u));
}
