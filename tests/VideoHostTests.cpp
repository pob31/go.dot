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

/*  PHASE 8a'S RENDERER AND WHAT IT IS TOLD (namespace draft 35.4, 35.7).

    Three parts with no screen and no process: the region's protocol laid out
    in plain memory - a layer shown, pointed, removed and let go, the
    configuration written and read back, text cut to its field; the clock
    estimate on a made-up clock - jitter averaged out, a drift followed, a jump
    taken as a new start; and the compositor's arithmetic - the stack's order
    and the blend in display space.

    And the whole loop, for real: the host starts a renderer with no window -
    this test binary, under the verb the engine uses - which reads the scene
    off the region at the clock the host writes, and says what colour the
    middle of a canvas is. Asked to leave, it is started again, and draws the
    same picture again from the same region (VC).
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "HapMovieWriter.h"
#include "TestSupport.h"

#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/video/Compositor.h>
#include <wfg/engine/video/Geometry.h>
#include <wfg/engine/video/RegionSink.h>
#include <wfg/engine/video/VideoClock.h>
#include <wfg/engine/video/VideoHost.h>
#include <wfg/engine/video/VideoRamp.h>
#include <wfg/engine/video/VideoRegion.h>

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

using namespace wfg;

namespace
{
    /*  A region in plain memory, zeroed and stamped as the host stamps one. */
    struct Memory
    {
        Memory() : bytes (video::region::regionBytes(), 0)
        {
            region = new (bytes.data()) video::region::Region {};
            video::region::stamp (*region);
        }

        std::vector<char> bytes;
        video::region::Region* region = nullptr;
    };

    std::vector<video::region::LayerReading> layersOf (const video::region::Region& r)
    {
        std::vector<video::region::LayerReading> out;

        for (const auto& slot : r.layers)
        {
            video::region::LayerReading layer;

            if (video::region::readLayer (slot, layer))
                out.push_back (std::move (layer));
        }

        return out;
    }

    video::LayerSpec fill (const char* id, const char* canvas, int layer, std::uint64_t order, std::uint32_t paint)
    {
        video::LayerSpec spec;
        spec.id = id;
        spec.canvas = canvas;
        spec.layer = layer;
        spec.order = order;
        spec.source = "fill";
        spec.paint = paint;
        return spec;
    }
}

//==============================================================================
TEST_CASE ("video region: a layer shown, pointed, removed and let go, as the renderer reads it")
{
    Memory memory;
    auto& r = *memory.region;
    REQUIRE (video::region::looksValid (r));

    video::RegionSink sink { r };

    sink.show (fill ("RUN00001", "CANVAS01", 2, 7, 0x2040A0));
    sink.opacity ("RUN00001", { 1000, 0.0 });
    sink.opacity ("RUN00001", { 49000, 1.0 });

    auto layers = layersOf (r);
    REQUIRE (layers.size() == 1);
    CHECK (layers[0].id == "RUN00001");
    CHECK (layers[0].canvas == "CANVAS01");
    CHECK (layers[0].layer == 2);
    CHECK (layers[0].order == 7u);
    CHECK (layers[0].source == video::region::Source::fill);
    CHECK (layers[0].paint == 0x2040A0u);
    REQUIRE (layers[0].ring (video::Property::opacity).count == 2);
    CHECK (layers[0].ring (video::Property::opacity).points[1].sample == 49000);
    CHECK (layers[0].removeAt == video::region::notRemoved);
    CHECK (video::opacityOf (layers[0], 25000) == doctest::Approx (0.5));

    /*  A POINT FOR A LAYER NOBODY SHOWED goes nowhere. */
    sink.opacity ("NOBODY01", { 0, 1.0 });
    CHECK (layersOf (r).size() == 1);

    /*  REMOVED AT A SAMPLE: still there until then, gone from it, and held
        until the clock has passed it by the margin. */
    sink.remove ("RUN00001", 90000);
    layers = layersOf (r);
    REQUIRE (layers.size() == 1);
    CHECK (layers[0].removeAt == 90000);
    CHECK (video::opacityOf (layers[0], 89999) == doctest::Approx (1.0));
    CHECK (video::opacityOf (layers[0], 90000) == doctest::Approx (0.0));

    sink.release (100000, 48000);
    CHECK (layersOf (r).size() == 1);
    sink.release (140000, 48000);
    CHECK (layersOf (r).empty());
    CHECK (sink.held() == 0);

    /*  REMOVED NOW, and a clear: both let go at once. */
    sink.show (fill ("RUN00002", "CANVAS01", 0, 8, 0xFFFFFF));
    sink.show (fill ("RUN00003", "CANVAS01", 0, 9, 0x000000));
    sink.remove ("RUN00002", -1);
    CHECK (layersOf (r).size() == 1);
    sink.clear();
    CHECK (layersOf (r).empty());

    /*  THE RING HOLDS THE LAST POINTS, oldest first. */
    sink.show (fill ("RUN00004", "CANVAS01", 0, 10, 0xFFFFFF));

    for (int n = 0; n < 300; ++n)
        sink.opacity ("RUN00004", { n * 960, 1.0 });

    layers = layersOf (r);
    REQUIRE (layers.size() == 1);
    const auto& opacities = layers[0].ring (video::Property::opacity);
    CHECK (opacities.count == video::region::pointsPerLayer);
    CHECK (opacities.points[0].sample == (300 - static_cast<int> (video::region::pointsPerLayer)) * 960);
    CHECK (opacities.points[video::region::pointsPerLayer - 1].sample == 299 * 960);

    /*  A RING PER VALUE: three hundred points of a scale fade push out no
        opacity point - the picture stays where its own last point left it. */
    sink.show (fill ("RUN00005", "CANVAS01", 0, 11, 0xFFFFFF));
    sink.opacity ("RUN00005", { 0, 0.75 });

    for (int n = 0; n < 300; ++n)
        sink.move ("RUN00005", video::Property::scale, { n * 960, 100.0 + n });

    for (const auto& layer : layersOf (r))
        if (layer.id == "RUN00005")
        {
            CHECK (layer.ring (video::Property::opacity).count == 1);
            CHECK (video::opacityOf (layer, 1000000) == doctest::Approx (0.75));
            CHECK (video::valueOf (layer, video::Property::scale, 299 * 960, 100.0) == doctest::Approx (399.0));
        }
}

TEST_CASE ("video region: the configuration written whole and read back, and text cut to its field")
{
    Memory memory;
    auto& r = *memory.region;

    std::vector<video::region::CanvasReading> canvases { { "CANVAS01", 3840, 2160 }, { "CANVAS02", 1920, 1080 } };
    std::vector<video::region::OutputReading> outputs { { "OUTPUT01", "CANVAS01", "Face", "EPSON PJ", "\\\\?\\DISPLAY#EPS", true, false } };

    video::region::writeConfig (r, canvases, outputs);

    video::region::ConfigReading read;
    REQUIRE (video::region::readConfig (r, read));
    REQUIRE (read.canvases.size() == 2);
    CHECK (read.canvases[0].width == 3840);
    CHECK (read.canvases[1].id == "CANVAS02");
    REQUIRE (read.outputs.size() == 1);
    CHECK (read.outputs[0].display == "EPSON PJ");
    CHECK (read.outputs[0].canvas == "CANVAS01");
    CHECK (read.outputs[0].enabled);

    /*  A NAME LONGER THAN ITS FIELD is cut, never run over. */
    char field[8];
    video::region::writeText (field, "a much longer name");
    CHECK (video::region::readText (field) == "a much ");

    /*  And the clock pair, read whole. */
    video::region::writeClock (r, 123456, 987654321, 48000);
    const auto clock = video::region::readClock (r);
    CHECK (clock.sample == 123456);
    CHECK (clock.nanos == 987654321);
    CHECK (clock.sampleRate == 48000);
}

TEST_CASE ("video clock: jitter averaged out, a drift followed, a jump taken as a new start")
{
    /*  A CLOCK READ BY A THREAD THAT WAKES WHEN IT WAKES, off a counter that
        moves a block at a time: each pair up to a 256-sample block late and up
        to two milliseconds off its time. The estimate stays within a fraction
        of a block of the truth. */
    video::ClockEstimate estimate;

    constexpr int rate = 48000;
    const auto truth = [] (std::int64_t nanos, double ratio)
    {
        return static_cast<std::int64_t> (static_cast<double> (nanos) * 1.0e-9 * rate * ratio);
    };

    std::uint32_t seed = 12345;
    const auto random = [&seed] { seed = seed * 1664525u + 1013904223u; return static_cast<double> (seed >> 8) / 16777216.0; };

    std::int64_t nanos = 1'000'000'000;

    for (int tick = 0; tick < 2000; ++tick, nanos += 20'000'000)
    {
        const auto readAt = nanos + static_cast<std::int64_t> (random() * 2.0e6);
        const auto counter = truth (readAt, 1.0) / 256 * 256;
        estimate.feed (counter, readAt, rate);
    }

    const auto error = std::abs (static_cast<double> (estimate.sampleAt (nanos) - truth (nanos, 1.0)));
    CHECK (error < 256.0);

    /*  A SECOND CRYSTAL 100 ppm FAST: followed, not slid away from. */
    video::ClockEstimate drifting;

    for (std::int64_t at = 0; at < 120'000'000'000LL; at += 20'000'000)
        drifting.feed (truth (at, 1.0001), at, rate);

    CHECK (std::abs (static_cast<double> (drifting.sampleAt (120'000'000'000LL) - truth (120'000'000'000LL, 1.0001))) < 64.0);

    /*  A JUMP OF A SECOND is not noise: the line starts again from it. */
    drifting.feed (truth (120'020'000'000LL, 1.0001) + rate, 120'020'000'000LL, rate);
    CHECK (std::abs (static_cast<double> (drifting.sampleAt (120'020'000'000LL)
                                          - (truth (120'020'000'000LL, 1.0001) + rate))) < 2.0);

    /*  Before any pair, nothing. */
    CHECK (video::ClockEstimate {}.sampleAt (0) == -1);
}

TEST_CASE ("video compositor: the stack in its order, each layer laid over in display space")
{
    Memory memory;
    video::RegionSink sink { *memory.region };

    /*  WHITE AT HALF OVER BLACK: the middle grey of the numbers the colour is
        written in, 128 - display space, as VD says. */
    sink.show (fill ("RUN00001", "C1", 0, 1, 0xFFFFFF));
    sink.opacity ("RUN00001", { 0, 0.5 });

    auto layers = layersOf (*memory.region);
    CHECK (video::fillsAt (video::stackOf (layers, "C1"), 100) == 0x808080u);

    /*  HIGHER IS ON TOP, whatever came up first; and of two on one layer the
        later is on top (VI). */
    sink.show (fill ("RUN00002", "C1", 5, 2, 0xFF0000));
    sink.opacity ("RUN00002", { 0, 1.0 });
    sink.show (fill ("RUN00003", "C1", 1, 3, 0x00FF00));
    sink.opacity ("RUN00003", { 0, 1.0 });

    layers = layersOf (*memory.region);
    CHECK (video::fillsAt (video::stackOf (layers, "C1"), 100) == 0xFF0000u);

    sink.show (fill ("RUN00004", "C1", 5, 4, 0x0000FF));
    sink.opacity ("RUN00004", { 0, 1.0 });
    layers = layersOf (*memory.region);
    CHECK (video::fillsAt (video::stackOf (layers, "C1"), 100) == 0x0000FFu);

    /*  ANOTHER CANVAS is another stack, and an empty one is black. */
    CHECK (video::fillsAt (video::stackOf (layers, "C2"), 100) == 0x000000u);

    /*  Before a layer's first point it is not there. */
    sink.show (fill ("RUN00005", "C2", 0, 5, 0xFFFFFF));
    sink.opacity ("RUN00005", { 1000, 1.0 });
    layers = layersOf (*memory.region);
    CHECK (video::fillsAt (video::stackOf (layers, "C2"), 999) == 0x000000u);
    CHECK (video::fillsAt (video::stackOf (layers, "C2"), 1000) == 0xFFFFFFu);
}

//==============================================================================
namespace
{
    juce::File videoBundle()
    {
        const juce::File folder { juce::String (std::string (WFG_TEST_FIXTURES_DIR)) + "/bundles/video" };

        REQUIRE_MESSAGE (folder.isDirectory(), "missing fixture bundle: " << folder.getFullPathName());
        return folder;
    }

    /*  THE ENGINE'S TICK, played by the test: Go.dot's samples as the machine's
        own time has gone by since the start, at 48 kHz, written fifty times a
        second as the tick thread writes them. */
    struct TestClock
    {
        std::int64_t now() const
        {
            const auto elapsed = std::chrono::steady_clock::now() - began;
            return std::chrono::duration_cast<std::chrono::microseconds> (elapsed).count() * 48 / 1000;
        }

        std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();
    };

    /*  Ticks the host until `done`, or gives up. */
    template <typename Done>
    bool tickUntil (video::VideoHost& host, const TestClock& clock, Done done, int milliseconds = 20000)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds (milliseconds);

        while (std::chrono::steady_clock::now() < until)
        {
            host.tick (clock.now(), 48000);

            if (done())
                return true;

            std::this_thread::sleep_for (std::chrono::milliseconds (20));
        }

        return done();
    }

    std::uint32_t probeOf (const video::region::Region& r, int canvas, std::int64_t& sample)
    {
        std::uint32_t colour = 0;

        video::region::readConsistent (r.probeSeq, [&]
        {
            colour = r.probe[canvas].load (std::memory_order_relaxed);
            sample = r.probeSample.load (std::memory_order_relaxed);
        });

        return colour;
    }
}

TEST_CASE ("video host: a renderer with no window draws the scene the region holds, and draws it again when started again")
{
    juce::TemporaryFile work;
    const auto folder = work.getFile().getSiblingFile ("godot-video-test-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));

    video::HostSpec spec;
    spec.workFolder = folder.getFullPathName().toStdString();
    spec.executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName().toStdString();
    spec.leadingArgs = { "video-render" };
    spec.headless = true;

    doc::ShowDocument document;
    REQUIRE (doc::Bundle::open (videoBundle(), document).ok);

    TestClock clock;

    {
        video::VideoHost host { spec };
        REQUIRE (host.isOpen());

        /*  NO OUTPUT, NO RENDERER: the fixture has one, switched on, so one
            is started. */
        host.configure (document);
        REQUIRE (tickUntil (host, clock, [&host] { return host.readouts().renderer == "running"; }));

        auto& r = *host.regionForTests();

        /*  IDENTIFY reaches the region at once, outlives an edit of the show -
            it is tonight's, held by the host - and goes when switched off. */
        {
            host.identify ("VD000021", true);
            video::region::ConfigReading config;
            REQUIRE (video::region::readConfig (r, config));
            REQUIRE (config.outputs.size() == 1);
            CHECK (config.outputs.front().testPattern);
            CHECK (host.identifying ("VD000021"));

            REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/name", "Face left").ok);
            host.configure (document);
            REQUIRE (video::region::readConfig (r, config));
            CHECK (config.outputs.front().name == "Face left");
            CHECK (config.outputs.front().testPattern);

            host.identify ("VD000021", false);
            REQUIRE (video::region::readConfig (r, config));
            CHECK_FALSE (config.outputs.front().testPattern);
        }

        /*  A FILL OF #2040A0 PUT UP NOW, on the fixture's canvas - the first in
            the configuration - and the renderer says the middle of it is that
            colour, at a sample after the point. */
        const auto at = clock.now();
        host.sink().show (fill ("RUN00001", "VD000011", 0, 1, 0x2040A0));
        host.sink().opacity ("RUN00001", { at, 1.0 });

        std::int64_t seen = -1;
        CHECK (tickUntil (host, clock, [&] { return probeOf (r, 0, seen) == 0x2040A0u && seen >= at; }));

        /*  THE OUTPUT SAYS WHY IT IS NOT BOUND: the renderer has no window. */
        CHECK (tickUntil (host, clock, [&host]
                          {
                              const auto said = host.readouts();
                              const auto* output = said.output ("VD000021");
                              return output != nullptr && ! output->bound
                                       && output->problem.find ("no window") != std::string::npos;
                          }));

        /*  ASKED TO LEAVE - as a crash would leave it gone - it is started
            again, and the same picture is drawn again from the same region:
            the engine held the scene, not the renderer (VC). */
        r.shouldExit.store (1, std::memory_order_release);
        REQUIRE (tickUntil (host, clock, [&host] { return host.readouts().renderer == "failed"; }));

        r.probe[0].store (0, std::memory_order_relaxed);
        CHECK (tickUntil (host, clock, [&host] { return host.readouts().renderer == "running"; }));
        CHECK (tickUntil (host, clock, [&] { return probeOf (r, 0, seen) == 0x2040A0u; }));

        /*  AND BLACK AGAIN WHEN THE LAYER GOES. */
        host.sink().remove ("RUN00001", clock.now());
        CHECK (tickUntil (host, clock, [&] { return probeOf (r, 0, seen) == 0x000000u; }));
    }

    folder.deleteRecursively();
}

//==============================================================================
/*  ON THE BENCH ONLY (§35.7): a real window, on a real display, drawn by
    OpenGL - what CI has no screen for. Skipped unless WFG_VIDEO_BENCH is set.
    It lists the displays the renderer finds; with WFG_VIDEO_BENCH_DISPLAY set
    to one of their names it puts the fixture's output on that display, brings
    a fill up over a second and holds it three seconds, and says how the frames
    went - the numbers the author reads, rather than a judgement by eye. */
TEST_CASE ("video bench: a real window on a real display, its frames counted")
{
    if (juce::SystemStats::getEnvironmentVariable ("WFG_VIDEO_BENCH", {}).isEmpty())
        return;

    const auto chosen = juce::SystemStats::getEnvironmentVariable ("WFG_VIDEO_BENCH_DISPLAY", {}).toStdString();

    juce::TemporaryFile work;
    const auto folder = work.getFile().getSiblingFile ("godot-video-bench");

    video::HostSpec spec;
    spec.workFolder = folder.getFullPathName().toStdString();
    spec.executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName().toStdString();
    spec.leadingArgs = { "video-render" };

    doc::ShowDocument document;
    REQUIRE (doc::Bundle::open (videoBundle(), document).ok);

    if (! chosen.empty())
        REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/display", chosen).ok);

    TestClock clock;
    video::VideoHost host { spec };
    host.configure (document);

    REQUIRE (tickUntil (host, clock, [&host] { return host.readouts().renderer == "running"; }));
    tickUntil (host, clock, [&host] { return ! host.readouts().displays.empty(); }, 3000);

    for (const auto& display : host.readouts().displays)
        MESSAGE ("display: " << display.name << "  [" << display.id << "]");

    if (chosen.empty())
        return;

    REQUIRE (tickUntil (host, clock, [&host]
                        {
                            const auto said = host.readouts();
                            const auto* output = said.output ("VD000021");
                            return output != nullptr && output->bound;
                        }, 5000));

    const auto at = clock.now();
    host.sink().show (fill ("RUN00001", "VD000011", 0, 1, 0x2040A0));
    host.sink().opacity ("RUN00001", { at, 0.0 });
    host.sink().opacity ("RUN00001", { at + 48000, 1.0 });

    tickUntil (host, clock, [] { return false; }, 4000);

    const auto said = host.readouts();
    const auto* output = said.output ("VD000021");
    REQUIRE (output != nullptr);

    MESSAGE ("frames " << output->framesPresented << ", late " << output->framesLate
                       << ", jitter " << output->jitterMs << " ms");
    CHECK (output->framesPresented > 100u);

    host.sink().remove ("RUN00001", clock.now() + 48000);
    tickUntil (host, clock, [] { return false; }, 1500);
}

//==============================================================================
/*  PICTURES AND THEIR GEOMETRY (namespace draft 36): where a picture lies -
    fitted, scaled, turned, moved, flipped - and what the canvas shows there,
    worked out by the same function the GPU draws by. */
namespace
{
    /*  A PICTURE OF THE TEST'S OWN, 400 by 200: its left half red, its right
        half blue, with a green square at its very middle. */
    struct TwoHalves final : video::PictureSampler
    {
        bool sizeOf (const std::string& path, int& width, int& height) const override
        {
            if (path != "two-halves.png")
                return false;

            width = 400;
            height = 200;
            return true;
        }

        bool colourAt (const std::string& path, double u, double v,
                       double& red, double& green, double& blue, double& alpha) const override
        {
            if (path != "two-halves.png")
                return false;

            red = green = blue = 0.0;
            alpha = 1.0;

            if (std::abs (u - 0.5) < 0.02 && std::abs (v - 0.5) < 0.04)
                green = 255.0;
            else if (u < 0.5)
                red = 255.0;
            else
                blue = 255.0;

            return true;
        }
    };

    video::LayerSpec picture (const char* id, std::uint64_t order)
    {
        video::LayerSpec spec;
        spec.id = id;
        spec.canvas = "C1";
        spec.order = order;
        spec.source = "picture";
        spec.file = "two-halves.png";
        return spec;
    }
}

TEST_CASE ("video geometry: fit, fill and stretch; scale, offset, a clockwise turn and the flips")
{
    video::Placement place;
    place.canvasWidth = 1920.0;
    place.canvasHeight = 1080.0;
    place.pictureWidth = 1000.0;
    place.pictureHeight = 1000.0;

    double halfWidth = 0.0, halfHeight = 0.0;

    /*  FIT: the whole square, its shape kept - as high as the canvas. FILL:
        the canvas covered - as wide. STRETCH: the canvas exactly (VO). */
    place.fit = 0;
    place.halfSize (halfWidth, halfHeight);
    CHECK (halfWidth == doctest::Approx (540.0));
    CHECK (halfHeight == doctest::Approx (540.0));

    place.fit = 1;
    place.halfSize (halfWidth, halfHeight);
    CHECK (halfWidth == doctest::Approx (960.0));
    CHECK (halfHeight == doctest::Approx (960.0));

    place.fit = 2;
    place.halfSize (halfWidth, halfHeight);
    CHECK (halfWidth == doctest::Approx (960.0));
    CHECK (halfHeight == doctest::Approx (540.0));

    /*  SCALE in % of the fitted size, OFFSET in % of the canvas, up is up
        (VQ, VT). */
    place.fit = 0;
    place.scale = 50.0;
    place.offsetX = 25.0;
    place.offsetY = 10.0;

    double x = 0.0, y = 0.0;
    place.toCanvas (1.0, 1.0, x, y);
    CHECK (x == doctest::Approx (270.0 + 480.0));
    CHECK (y == doctest::Approx (270.0 + 108.0));

    /*  A QUARTER TURN CLOCKWISE takes the picture's top edge to its right. */
    place.scale = 100.0;
    place.offsetX = 0.0;
    place.offsetY = 0.0;
    place.rotation = 90.0;
    place.toCanvas (0.0, 1.0, x, y);
    CHECK (x == doctest::Approx (540.0));
    CHECK (y == doctest::Approx (0.0).epsilon (1e-9));

    /*  AND BACK: every point of the canvas to the texture, the flips mirroring
        the texture and not the place. */
    double u = 0.0, v = 0.0;
    REQUIRE (place.toTexture (540.0 - 1.0, 0.0, u, v));
    CHECK (u == doctest::Approx (0.5).epsilon (0.01));
    CHECK (v > 0.99);

    place.flipV = true;
    REQUIRE (place.toTexture (540.0 - 1.0, 0.0, u, v));
    CHECK (v < 0.01);

    CHECK_FALSE (place.toTexture (900.0, 0.0, u, v));
}

TEST_CASE ("video compositor: a picture sampled where it lies, and a fill scaled to a panel")
{
    Memory memory;
    video::RegionSink sink { *memory.region };
    const TwoHalves sampler;

    /*  FIT ON A 1920 BY 1080 CANVAS: 400 by 200 becomes 1920 by 960; its
        middle is green, left of it red, right blue, above it black (bars). */
    sink.show (picture ("RUN00001", 1));
    sink.opacity ("RUN00001", { 0, 1.0 });

    auto layers = layersOf (*memory.region);
    auto stack = video::stackOf (layers, "C1");

    CHECK (video::colourAt (stack, 10, 1920.0, 1080.0, 0.0, 0.0, &sampler) == 0x00FF00u);
    CHECK (video::colourAt (stack, 10, 1920.0, 1080.0, -400.0, 0.0, &sampler) == 0xFF0000u);
    CHECK (video::colourAt (stack, 10, 1920.0, 1080.0, 400.0, 0.0, &sampler) == 0x0000FFu);
    CHECK (video::colourAt (stack, 10, 1920.0, 1080.0, 0.0, 500.0, &sampler) == 0x000000u);

    /*  MOVED A QUARTER OF THE CANVAS LEFT by a fade's points: the middle of
        the canvas now shows the picture's right half (VS - the value read off
        its own points). */
    sink.move ("RUN00001", video::Property::offsetX, { 100, 0.0 });
    sink.move ("RUN00001", video::Property::offsetX, { 200, -25.0 });

    layers = layersOf (*memory.region);
    stack = video::stackOf (layers, "C1");
    CHECK (video::colourAt (stack, 150, 1920.0, 1080.0, 0.0, 0.0, &sampler) == 0x0000FFu);
    CHECK (video::colourAt (stack, 250, 1920.0, 1080.0, 0.0, 0.0, &sampler) == 0x0000FFu);
    CHECK (video::colourAt (stack, 50, 1920.0, 1080.0, 0.0, 0.0, &sampler) == 0x00FF00u);

    /*  FLIPPED, the halves trade places. */
    auto flipped = picture ("RUN00002", 2);
    flipped.flipH = true;
    sink.clear();
    sink.show (flipped);
    sink.opacity ("RUN00002", { 0, 1.0 });

    layers = layersOf (*memory.region);
    stack = video::stackOf (layers, "C1");
    CHECK (video::colourAt (stack, 10, 1920.0, 1080.0, -400.0, 0.0, &sampler) == 0x0000FFu);

    /*  A FILL AT HALF ITS SCALE IS A PANEL (VW): the middle painted, a corner
        not. A picture not read yet shows nothing. */
    video::LayerSpec panel;
    panel.id = "RUN00003";
    panel.canvas = "C2";
    panel.order = 3;
    panel.source = "fill";
    panel.paint = 0xFFFFFF;
    panel.scale = 50.0;
    sink.show (panel);
    sink.opacity ("RUN00003", { 0, 1.0 });

    layers = layersOf (*memory.region);
    stack = video::stackOf (layers, "C2");
    CHECK (video::colourAt (stack, 10, 1920.0, 1080.0, 0.0, 0.0, nullptr) == 0xFFFFFFu);
    CHECK (video::colourAt (stack, 10, 1920.0, 1080.0, 700.0, 400.0, nullptr) == 0x000000u);

    auto unread = picture ("RUN00004", 4);
    unread.file = "nowhere.png";
    sink.show (unread);
    sink.opacity ("RUN00004", { 0, 1.0 });
    layers = layersOf (*memory.region);
    /*  The flipped picture under it still shows its middle. */
    CHECK (video::colourAt (video::stackOf (layers, "C1"), 10, 1920.0, 1080.0, 0.0, 0.0, &sampler) == 0x00FF00u);
}

TEST_CASE ("video host: a picture read off the disk by a renderer with no window, and shown where its geometry puts it")
{
    juce::TemporaryFile work;
    const auto folder = work.getFile().getSiblingFile ("godot-video-picture-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));
    folder.createDirectory();

    /*  A REAL PNG: left half red, right half blue. */
    const auto png = folder.getChildFile ("halves.png");
    {
        juce::Image image (juce::Image::ARGB, 64, 32, true, juce::SoftwareImageType());
        juce::Graphics g (image);
        g.fillAll (juce::Colours::red);
        g.setColour (juce::Colour (0xFF0000FF));
        g.fillRect (32, 0, 32, 32);

        juce::FileOutputStream out (png);
        REQUIRE (out.openedOk());
        REQUIRE (juce::PNGImageFormat().writeImageToStream (image, out));
    }

    video::HostSpec spec;
    spec.workFolder = folder.getFullPathName().toStdString();
    spec.executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName().toStdString();
    spec.leadingArgs = { "video-render" };
    spec.headless = true;

    doc::ShowDocument document;
    REQUIRE (doc::Bundle::open (videoBundle(), document).ok);

    TestClock clock;

    {
        video::VideoHost host { spec };
        host.configure (document);
        REQUIRE (tickUntil (host, clock, [&host] { return host.readouts().renderer == "running"; }));

        auto& r = *host.regionForTests();

        /*  READ AHEAD, then shown: the middle of the canvas is the picture's
            middle - and a quarter canvas to the left, its right half. */
        host.sink().prepare ({ png.getFullPathName().toStdString() });

        video::LayerSpec layer;
        layer.id = "RUN00001";
        layer.canvas = "VD000011";
        layer.order = 1;
        layer.source = "picture";
        layer.file = png.getFullPathName().toStdString();
        layer.offsetX = -10.0;

        const auto at = clock.now();
        host.sink().show (layer);
        host.sink().opacity ("RUN00001", { at, 1.0 });

        std::int64_t seen = -1;
        CHECK (tickUntil (host, clock, [&] { return probeOf (r, 0, seen) == 0x0000FFu; }));

        host.sink().move ("RUN00001", video::Property::offsetX, { clock.now(), 10.0 });
        CHECK (tickUntil (host, clock, [&] { return probeOf (r, 0, seen) == 0xFF0000u; }));
    }

    folder.deleteRecursively();
}

TEST_CASE ("video host: a HAP movie read by a renderer with no window, its frame chosen by the playhead's points")
{
    using namespace wfg::testing::hapmovie;

    juce::TemporaryFile work;
    const auto folder = work.getFile().getSiblingFile ("godot-video-movie-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));
    folder.createDirectory();

    /*  THREE FRAMES A SECOND APART: red, blue, green - one plain, one Snappy. */
    std::vector<Bytes> frames;
    frames.push_back (section (0xAB, solidDxt1 (16, 8, 0xFF0000)));
    frames.push_back (section (0xBB, snappyLiterals (solidDxt1 (16, 8, 0x0000FF))));
    frames.push_back (section (0xAB, solidDxt1 (16, 8, 0x00FF00)));
    const auto movie = writeMovie (folder, "three.mov", hapMovie (16, 8, frames, 1));

    video::HostSpec spec;
    spec.workFolder = folder.getFullPathName().toStdString();
    spec.executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName().toStdString();
    spec.leadingArgs = { "video-render" };
    spec.headless = true;

    doc::ShowDocument document;
    REQUIRE (doc::Bundle::open (videoBundle(), document).ok);

    TestClock clock;

    {
        video::VideoHost host { spec };
        host.configure (document);
        REQUIRE (tickUntil (host, clock, [&host] { return host.readouts().renderer == "running"; }));

        auto& r = *host.regionForTests();

        video::LayerSpec layer;
        layer.id = "RUN00001";
        layer.canvas = "VD000011";
        layer.order = 1;
        layer.source = "movie";
        layer.file = movie.getFullPathName().toStdString();

        host.sink().show (layer);
        host.sink().opacity ("RUN00001", { clock.now(), 1.0 });
        host.sink().move ("RUN00001", video::Property::time, { clock.now(), 0.2 });

        std::int64_t seen = -1;
        CHECK (tickUntil (host, clock, [&] { return probeOf (r, 0, seen) == 0xFF0000u; }));

        /*  THE PLAYHEAD MOVED to a second and a half: the second frame. */
        host.sink().move ("RUN00001", video::Property::time, { clock.now(), 1.5 });
        CHECK (tickUntil (host, clock, [&] { return probeOf (r, 0, seen) == 0x0000FFu; }));

        host.sink().move ("RUN00001", video::Property::time, { clock.now(), 2.5 });
        CHECK (tickUntil (host, clock, [&] { return probeOf (r, 0, seen) == 0x00FF00u; }));
    }

    folder.deleteRecursively();
}

//==============================================================================
TEST_CASE ("video grade: contrast, saturation, gamma, hue and the curves, in their order")
{
    video::Grade grade;
    video::bakeCurves (grade, {}, {});
    CHECK (grade.isIdentity());

    double r = 0.8, g = 0.4, b = 0.2;
    video::applyGrade (grade, r, g, b);
    CHECK (r == doctest::Approx (0.8));
    CHECK (g == doctest::Approx (0.4));
    CHECK (b == doctest::Approx (0.2));

    /*  NO SATURATION IS GREY at the colour's luma; NO CONTRAST is mid-grey. */
    grade.saturation = 0.0;
    r = 1.0; g = 0.0; b = 0.0;
    video::applyGrade (grade, r, g, b);
    CHECK (r == doctest::Approx (0.2126));
    CHECK (g == doctest::Approx (0.2126));

    grade.saturation = 100.0;
    grade.contrast = 0.0;
    r = 0.9; g = 0.1; b = 0.3;
    video::applyGrade (grade, r, g, b);
    CHECK (r == doctest::Approx (0.5));
    CHECK (b == doctest::Approx (0.5));

    /*  GAMMA TWO lifts a quarter to a half; the ends stay. */
    grade.contrast = 100.0;
    grade.gamma = 2.0;
    r = 0.25; g = 0.0; b = 1.0;
    video::applyGrade (grade, r, g, b);
    CHECK (r == doctest::Approx (0.5));
    CHECK (g == doctest::Approx (0.0));
    CHECK (b == doctest::Approx (1.0));

    /*  A THIRD OF A TURN takes a grey-balanced red round to green. */
    grade.gamma = 1.0;
    grade.hue = 120.0;
    r = 1.0; g = 0.0; b = 0.0;
    video::applyGrade (grade, r, g, b);
    CHECK (g > r);
    CHECK (g > b);

    /*  THE CURVES: the luminosity curve inverts, the red curve then halves -
        black in comes out red at half. */
    grade.hue = 0.0;
    video::bakeCurves (grade, video::curveFrom ({ 0.0, 1.0, 1.0, 0.0 }),
                       { video::curveFrom ({ 0.0, 0.0, 1.0, 0.5 }), {}, {} });
    CHECK (grade.hasCurves);
    r = 0.0; g = 0.0; b = 1.0;
    video::applyGrade (grade, r, g, b);
    CHECK (r == doctest::Approx (0.5).epsilon (0.01));
    CHECK (g == doctest::Approx (1.0));
    CHECK (b == doctest::Approx (0.0));
}

TEST_CASE ("video compositor: a picture's grade applied where it is sampled, and never a fill's")
{
    Memory memory;
    video::RegionSink sink { *memory.region };
    const TwoHalves sampler;

    auto layer = picture ("RUN00001", 1);
    layer.grade.saturation = 0.0;
    video::bakeCurves (layer.grade, {}, {});
    sink.show (layer);
    sink.opacity ("RUN00001", { 0, 1.0 });

    auto layers = layersOf (*memory.region);
    REQUIRE (layers.size() == 1);
    CHECK (layers[0].grade.saturation == doctest::Approx (0.0));

    /*  THE RED HALF, IN BLACK AND WHITE: grey at red's luma. */
    const auto grey = video::colourAt (video::stackOf (layers, "C1"), 10, 1920.0, 1080.0, -400.0, 0.0, &sampler);
    CHECK ((grey >> 16) == ((grey >> 8) & 0xffu));
    CHECK ((grey >> 16) == doctest::Approx (54.0).epsilon (0.05));

    video::LayerSpec fill;
    fill.id = "RUN00002";
    fill.canvas = "C2";
    fill.order = 2;
    fill.source = "fill";
    fill.paint = 0xFF0000;
    fill.grade.saturation = 0.0;
    sink.show (fill);
    sink.opacity ("RUN00002", { 0, 1.0 });

    layers = layersOf (*memory.region);
    CHECK (video::colourAt (video::stackOf (layers, "C2"), 10, 1920.0, 1080.0, 0.0, 0.0, nullptr) == 0xFF0000u);
}
