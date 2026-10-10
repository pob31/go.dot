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

#include <wfg/client/model/Text.h>
#include <wfg/engine/Engine.h>
#include <wfg/engine/audio/MediaAnalyser.h>
#include <wfg/engine/audio/MediaInfo.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/cue/DcaTable.h>
#include <wfg/engine/cue/ListState.h>
#include <wfg/engine/cue/LiveRows.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/tree/TreeCommands.h>
#include <wfg/engine/tree/Touches.h>
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/video/Compositor.h>
#include <wfg/engine/video/Displays.h>
#include <wfg/engine/video/Ffmpeg.h>
#include <wfg/engine/video/Geometry.h>
#include <wfg/engine/video/Mapping.h>
#include <wfg/engine/video/Movie.h>
#include <wfg/engine/video/MovieEditRender.h>
#include <wfg/engine/video/PipedChild.h>
#include <wfg/engine/video/RegionSink.h>
#include <wfg/engine/video/Strip.h>
#include <wfg/engine/video/VideoClock.h>
#include <wfg/engine/video/VideoHost.h>
#include <wfg/engine/video/VideoRamp.h>
#include <wfg/engine/video/VideoRegion.h>

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
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
    std::vector<video::region::OutputReading> outputs { { "OUTPUT01", "CANVAS01", "Face", "EPSON PJ", "\\\\?\\DISPLAY#EPS", true, false, {}, {}, {} } };

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

TEST_CASE ("video region: what to read ahead and what the renderer holds of it, both ways, cut to the region's room")
{
    Memory memory;
    auto& r = *memory.region;

    /*  FORTY NAMED, THIRTY-TWO KEPT, in their order - a movie with its second
        and its way (namespace draft §48). */
    std::vector<video::Preload> items;

    for (int n = 0; n < 40; ++n)
        items.push_back ({ "/show/media/still" + std::to_string (n) + ".png" });

    items[1] = { "/show/media/clip.mov", true, 2.5, -1 };

    video::region::writePrepared (r, items);

    std::uint32_t under = 0;
    const auto read = video::region::readPrepared (r, &under);
    REQUIRE (read.size() == static_cast<std::size_t> (video::region::maxPrepared));
    CHECK (under == r.preparedSeq.load());
    CHECK (read[0].path == "/show/media/still0.png");
    CHECK_FALSE (read[0].movie);
    CHECK (read[1].movie);
    CHECK (read[1].seconds == doctest::Approx (2.5));
    CHECK (read[1].direction == -1);
    CHECK (read.back().path == "/show/media/still31.png");

    /*  THE ANSWER, whole, naming the list it answers; a problem longer than
        its field is cut. */
    const std::string longProblem (400, 'x');
    video::region::writeHeld (r, under, { { "/show/media/still0.png", video::region::HeldState::ready },
                                          { "/show/media/clip.mov", video::region::HeldState::failed, longProblem } });

    std::vector<video::region::HeldReading> held;
    std::uint32_t answers = 0;
    REQUIRE (video::region::readHeld (r, held, answers));
    CHECK (answers == under);
    REQUIRE (held.size() == 2);
    CHECK (held[0].state == video::region::HeldState::ready);
    CHECK (held[1].state == video::region::HeldState::failed);
    CHECK (held[1].problem.size() == static_cast<std::size_t> (video::region::textChars - 1));

    /*  A LIST WRITTEN SINCE is a question the answer above is not to. */
    video::region::writePrepared (r, { { "/show/media/other.png" } });
    CHECK (r.preparedSeq.load() != answers);
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

        /*  THE PROJECTORS PUT AWAY (§39, the author's request of 2026-10-08):
            at once, through an edit of the show, the renderer saying why its
            output has no window - and never under the lock, which shows
            every projector whatever was asked. Asked still, they go away
            again at the unlock. */
        {
            const auto said = [&host]
            {
                const auto readouts = host.readouts();
                const auto* output = readouts.output ("VD000021");
                return output != nullptr ? output->problem : std::string {};
            };

            video::region::ConfigReading config;
            host.hideProjectors (true);
            REQUIRE (video::region::readConfig (r, config));
            CHECK (config.outputs.front().hidden);
            CHECK (host.projectorsHidden());
            CHECK (tickUntil (host, clock, [&said] { return said() == "put away while the show is unlocked"; }));

            REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/name", "Face right").ok);
            host.configure (document);
            REQUIRE (video::region::readConfig (r, config));
            CHECK (config.outputs.front().hidden);

            REQUIRE (document.setAttribute ("/godot/document/locked", "true").ok);
            host.configure (document);
            REQUIRE (video::region::readConfig (r, config));
            CHECK_FALSE (config.outputs.front().hidden);
            CHECK (host.projectorsHidden());
            CHECK (tickUntil (host, clock, [&said] { return said() != "put away while the show is unlocked"; }));

            host.hideProjectors (true);
            REQUIRE (video::region::readConfig (r, config));
            CHECK_FALSE (config.outputs.front().hidden);

            REQUIRE (document.setAttribute ("/godot/document/locked", "false").ok);
            host.configure (document);
            REQUIRE (video::region::readConfig (r, config));
            CHECK (config.outputs.front().hidden);

            host.hideProjectors (false);
            REQUIRE (video::region::readConfig (r, config));
            CHECK_FALSE (config.outputs.front().hidden);
            CHECK_FALSE (host.projectorsHidden());
            CHECK (tickUntil (host, clock, [&said] { return said() != "put away while the show is unlocked"; }));
        }

        /*  A FILL OF #2040A0 PUT UP NOW, on the fixture's canvas - the first in
            the configuration - and the renderer says the middle of it is that
            colour, at a sample after the point. */
        const auto at = clock.now();
        host.sink().show (fill ("RUN00001", "VD000011", 0, 1, 0x2040A0));
        host.sink().opacity ("RUN00001", { at, 1.0 });

        std::int64_t seen = -1;
        CHECK (tickUntil (host, clock, [&] { return probeOf (r, 0, seen) == 0x2040A0u && seen >= at; }));

        /*  WHAT IT SHOWS, AS ONE COLOUR (namespace draft §38, WR): the layer's
            and the canvas's, read back by the engine by their names. */
        CHECK (tickUntil (host, clock, [&host]
                          {
                              const auto said = host.readouts();
                              const auto has = [] (const auto& tints, const char* id, std::uint32_t rgb)
                              {
                                  return std::find (tints.begin(), tints.end(),
                                                    std::pair<std::string, std::uint32_t> { id, rgb }) != tints.end();
                              };

                              return has (said.layerTints, "RUN00001", 0x2040A0u)
                                       && has (said.canvasTints, "VD000011", 0x2040A0u);
                          }));

        /*  A CANVAS AT HALF (WT): the whole composite towards black. */
        host.sink().canvasLevels ({ { "VD000011", 0.5 } });
        CHECK (tickUntil (host, clock, [&] { return probeOf (r, 0, seen) == 0x102050u; }));
        host.sink().canvasLevels ({ { "VD000011", 1.0 } });
        CHECK (tickUntil (host, clock, [&] { return probeOf (r, 0, seen) == 0x2040A0u; }));

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

    /*  COUNTED FROM THE FIRST FRAME DRAWN, not from the window: the graphics
        card may be waking from its sleep (a laptop's second card takes two
        seconds), which is no frame late. */
    const auto framesOf = [&host]
    {
        const auto said = host.readouts();
        const auto* output = said.output ("VD000021");
        return output != nullptr ? output->framesPresented : std::uint64_t {};
    };

    const auto bound = juce::Time::getMillisecondCounterHiRes();
    REQUIRE (tickUntil (host, clock, [&framesOf] { return framesOf() > 0; }, 10000));
    MESSAGE ("the first frame " << juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - bound)
                                << " ms after the window was made");

    const auto first = framesOf();
    const auto at = clock.now();
    host.sink().show (fill ("RUN00001", "VD000011", 0, 1, 0x2040A0));
    host.sink().opacity ("RUN00001", { at, 0.0 });
    host.sink().opacity ("RUN00001", { at + 48000, 1.0 });

    tickUntil (host, clock, [] { return false; }, 4000);

    const auto said = host.readouts();
    const auto* output = said.output ("VD000021");
    REQUIRE (output != nullptr);

    MESSAGE ("frames " << output->framesPresented - first << " in four seconds, late " << output->framesLate
                       << ", jitter " << output->jitterMs << " ms");
    CHECK (output->framesPresented - first > 100u);

    host.sink().remove ("RUN00001", clock.now() + 48000);
    tickUntil (host, clock, [] { return false; }, 1500);
}

//==============================================================================
/*  A PROJECTOR'S WINDOW NEVER COVERS ITS DISPLAY EXACTLY (§39, the author's
    report of 2026-10-08): the driver takes such a window for a full-screen
    program and every screen goes black as it switches. It reaches one pixel
    past an edge no other display touches. */
TEST_CASE ("video displays: a projector's window reaches a pixel past an edge nothing else is on")
{
    const auto at = [] (int x, int y, int width, int height)
    {
        video::DisplayInfo display;
        display.physicalX = x;
        display.physicalY = y;
        display.physicalWidth = width;
        display.physicalHeight = height;
        return display;
    };

    const auto is = [] (const video::Overhang& overhang, int left, int top, int right, int bottom)
    {
        return overhang.left == left && overhang.top == top && overhang.right == right && overhang.bottom == bottom;
    };

    //  The author's two: a laptop and a screen to its left, set a little higher.
    const auto laptop = at (0, 0, 2560, 1440);
    const auto screen = at (-2560, -169, 2560, 1440);
    CHECK (is (video::overhangAmong (screen, { laptop, screen }), 0, 0, 0, 1));
    CHECK (is (video::overhangAmong (laptop, { laptop, screen }), 0, 0, 0, 1));

    //  One display under another: the upper reaches right, the lower below.
    const auto upper = at (0, 0, 1920, 1080);
    const auto lower = at (0, 1080, 1920, 1080);
    CHECK (is (video::overhangAmong (upper, { upper, lower }), 0, 0, 1, 0));
    CHECK (is (video::overhangAmong (lower, { upper, lower }), 0, 0, 0, 1));

    //  Under and right taken: above.
    const auto right = at (1920, 1080, 1920, 1080);
    CHECK (is (video::overhangAmong (lower, { lower, right, at (0, 2160, 1920, 1080) }), 0, 1, 0, 0));

    //  Walled in on every side: below all the same, over a neighbour's top row.
    const auto middle = at (0, 0, 100, 100);
    const std::vector<video::DisplayInfo> wall { middle, at (0, 100, 100, 100), at (100, 0, 100, 100),
                                                 at (0, -100, 100, 100), at (-100, 0, 100, 100) };
    CHECK (is (video::overhangAmong (middle, wall), 0, 0, 0, 1));

    //  No pixels known: nothing.
    CHECK (is (video::overhangAmong (video::DisplayInfo {}, { laptop }), 0, 0, 0, 0));
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
        host.sink().prepare ({ { png.getFullPathName().toStdString() } });

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

namespace
{
    /*  A STILL THE SIZE OF A PROJECTOR'S PICTURE, noise all over - a real
        decode - with a block of `centre` in its middle, where the probe looks. */
    juce::File noiseStill (const juce::File& folder, const juce::String& name, std::uint32_t centre, int width, int height)
    {
        juce::Image image (juce::Image::RGB, width, height, false, juce::SoftwareImageType());
        juce::Random random (static_cast<juce::int64> (centre) + width);

        {
            const juce::Image::BitmapData pixels (image, juce::Image::BitmapData::writeOnly);

            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    pixels.setPixelColour (x, y, juce::Colour (static_cast<juce::uint32> (random.nextInt()) | 0xFF000000u));
        }

        {
            juce::Graphics g (image);
            g.setColour (juce::Colour (centre | 0xFF000000u));
            g.fillRect (width / 2 - 64, height / 2 - 64, 128, 128);
        }

        const auto file = folder.getChildFile (name);
        juce::FileOutputStream out (file);
        REQUIRE (out.openedOk());
        REQUIRE (juce::PNGImageFormat().writeImageToStream (image, out));
        return file;
    }

    /*  WHAT CI HOLDS A PICTURE READ AHEAD TO: seen within a quarter of a
        second of its sample. The guarantee is that it was ready before it was
        shown, which each case waits for; this bound only says it then came up.
        Sixty milliseconds held on this machine and on Windows and Linux CI, and
        failed on the hosted macOS runner at 63 to 79 ms (CI run 37911881791),
        whose timer noise is larger than the effect - a still cold was seen
        sooner than one read ahead there. The numbers themselves are M54's, from
        the bench and the MESSAGE lines. */
    constexpr std::int64_t seenSoonEnough = 48 * 250;

    /*  FROM THE SAMPLE A LAYER IS SHOWN AT TO THE FIRST PROBE THAT SEES IT, in
        samples at 48 kHz - asked every millisecond, so the answer is the
        renderer's and not this loop's. -1 if it never comes. */
    std::int64_t lagToFirstSight (video::VideoHost& host, const TestClock& clock, const video::region::Region& r,
                                  std::int64_t shownAt, std::uint32_t colour)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds (10);
        auto lastTick = std::chrono::steady_clock::now();

        while (std::chrono::steady_clock::now() < until)
        {
            if (std::chrono::steady_clock::now() - lastTick >= std::chrono::milliseconds (20))
            {
                host.tick (clock.now(), 48000);
                lastTick = std::chrono::steady_clock::now();
            }

            std::int64_t sample = -1;

            if (probeOf (r, 0, sample) == colour && sample >= shownAt)
                return sample - shownAt;

            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }

        return -1;
    }
}

TEST_CASE ("video host: a renderer with no window reads ahead what it is named, says what it holds, and shows a still read ahead at once")
{
    juce::TemporaryFile work;
    const auto folder = work.getFile().getSiblingFile ("godot-video-ahead-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));
    folder.createDirectory();

    /*  M54a's stills (namespace draft §48): a projector's size, or 4K on the
        bench. */
    const auto bench = juce::SystemStats::getEnvironmentVariable ("WFG_VIDEO_BENCH", {}).isNotEmpty();
    const auto width = bench ? 3840 : 1920;
    const auto height = bench ? 2160 : 1080;
    const auto ahead = noiseStill (folder, "ahead.png", 0x00FF00, width, height);
    const auto cold = noiseStill (folder, "cold.png", 0xFF00FF, width, height);
    const auto missing = folder.getChildFile ("missing.png");

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

        /*  NAMED, THEN HELD - and the one that is not there said so, in words. */
        host.sink().prepare ({ { ahead.getFullPathName().toStdString() }, { missing.getFullPathName().toStdString() } });

        REQUIRE (tickUntil (host, clock, [&host]
                            {
                                const auto readouts = host.readouts();
                                return readouts.heldCurrent && readouts.held.size() == 2
                                         && readouts.held[0].state == video::region::HeldState::ready
                                         && readouts.held[1].state == video::region::HeldState::failed;
                            }));

        CHECK (host.readouts().held[1].problem == "not found");

        /*  AND THE ROWS SAY SO (§48, AAR), through the tree as a window reads
            it: the still held ready reads armed, the one the renderer could not
            find partial, and why. */
        {
            Engine engine;
            cue::RunTable runs;
            cue::ListState lists;
            lists.setAhead ({ { "VD000002", "picture", ahead.getFullPathName().toStdString() },
                              { "VD000003", "picture", missing.getFullPathName().toStdString() } });

            tree::MountTable mounts;
            tree::ParameterTree parameters { document, engine.commands(), mounts, runs };
            parameters.setListState (&lists);
            parameters.setVideo (&host);
            parameters.markStale();

            tree::EngineState state;
            const auto snapshot = parameters.publish (0, state);
            REQUIRE (snapshot != nullptr);
            CHECK (client::model::text (*snapshot, "/godot/cue/VD000002/prepare") == "armed");
            CHECK (client::model::text (*snapshot, "/godot/cue/VD000003/prepare") == "partial");
            CHECK (client::model::text (*snapshot, "/godot/cue/VD000003/prepareError") == "media-missing");
        }

        /*  SHOWN, READ AHEAD: seen on the renderer's first pass at or after its
            sample. Then one never named, read only once it is on a layer. */
        const auto shownAt = clock.now() + 960;
        video::LayerSpec layer;
        layer.id = "RUN00001";
        layer.canvas = "VD000011";
        layer.order = 1;
        layer.source = "picture";
        layer.file = ahead.getFullPathName().toStdString();
        host.sink().show (layer);
        host.sink().opacity ("RUN00001", { shownAt, 1.0 });

        const auto aheadLag = lagToFirstSight (host, clock, r, shownAt, 0x00FF00);

        const auto coldAt = clock.now() + 960;
        layer.id = "RUN00002";
        layer.order = 2;
        layer.file = cold.getFullPathName().toStdString();
        host.sink().show (layer);
        host.sink().opacity ("RUN00002", { coldAt, 1.0 });

        const auto coldLag = lagToFirstSight (host, clock, r, coldAt, 0xFF00FF);

        MESSAGE ("M54a, a ", width, "x", height, " still: read ahead ", static_cast<double> (aheadLag) / 48.0,
                 " ms after its sample; cold ", static_cast<double> (coldLag) / 48.0, " ms");

        /*  A still read ahead is never waited for by a decode; the bound is CI's
            (see `seenSoonEnough`). The cold one is only reported. */
        REQUIRE (aheadLag >= 0);
        CHECK (aheadLag < seenSoonEnough);
        CHECK (coldLag >= 0);
    }

    folder.deleteRecursively();
}

//==============================================================================
namespace
{
    /*  A PLAYER WHOSE CLOCK IS THE TEST'S (M55b): the Runner places a picture
        on the samples the renderer is told - the machine's own time, as
        `TestClock` keeps it - and nothing sounds. */
    struct ClockPlayer final : cue::Player
    {
        explicit ClockPlayer (const TestClock& clockToRead) : clock (clockToRead) {}

        int trackCount() const override                      { return 2; }
        std::int64_t samplesElapsed() const override         { return clock.now(); }
        int blockSize() const override                       { return 128; }
        int channelsPerTrack() const override                { return 2; }
        int slotCount() const override                       { return 1; }
        int sampleRate() const override                      { return 48000; }
        void requestArm (const cue::ArmRequest&) override    {}
        bool launchAtSample (int, int, std::int64_t) override { return true; }
        bool stop (int) override                             { return true; }
        bool stopAtSample (int, int, std::int64_t) override  { return true; }
        void setLevelDb (int, double) override               {}
        void setRouting (int, const std::vector<cue::Coefficient>&) override {}
        bool isPlaying (int) const override                  { return false; }
        bool isArmReady (int) const override                 { return false; }

        const TestClock& clock;
    };

    /*  THE RENDERER'S SINK, and the sample each layer was first put up on:
        what a press asked for, which the lag is measured from. */
    struct FirstSample final : video::Sink
    {
        explicit FirstSample (video::Sink& inner) : to (inner) {}

        void show (const video::LayerSpec& spec) override                  { to.show (spec); }
        void restate (const video::LayerSpec& spec) override               { to.restate (spec); }
        void remove (const std::string& id, std::int64_t sample) override  { to.remove (id, sample); }
        void clear() override                                              { to.clear(); }
        void prepare (const std::vector<video::Preload>& items) override   { to.prepare (items); }

        void canvasLevels (const std::vector<std::pair<std::string, double>>& levels) override
        {
            to.canvasLevels (levels);
        }

        void move (const std::string& id, video::Property property, const video::Point& point) override
        {
            if (property == video::Property::opacity && first.count (id) == 0)
                first[id] = point.sample;

            to.move (id, property, point);
        }

        video::Sink& to;
        std::map<std::string, std::int64_t> first;
    };
}

TEST_CASE ("video host: M55b, a still on an armed bank's strip is seen at once when pressed, one never read only once it is read (§49)")
{
    juce::TemporaryFile work;
    const auto folder = work.getFile().getSiblingFile ("godot-video-bank-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));
    folder.createDirectory();

    const auto bench = juce::SystemStats::getEnvironmentVariable ("WFG_VIDEO_BENCH", {}).isNotEmpty();
    const auto width = bench ? 3840 : 1920;
    const auto height = bench ? 2160 : 1080;
    const auto ahead = noiseStill (folder, "member.png", 0x00FF00, width, height);
    const auto cold = noiseStill (folder, "cold.png", 0xFF00FF, width, height);

    video::HostSpec spec;
    spec.workFolder = folder.getFullPathName().toStdString();
    spec.executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName().toStdString();
    spec.leadingArgs = { "video-render" };
    spec.headless = true;

    /*  THE VIDEO FIXTURE, and a bank last in its list: one still on the first
        strip of a virtual panel. And a still before the bank, fired by name and
        read only then - GO leaves the standby past the bank, on nothing. */
    doc::ShowDocument document;
    REQUIRE (doc::Bundle::open (videoBundle(), document).ok);

    std::vector<std::string> strips;
    REQUIRE (document.createSurface ("virtual", "Panel", {}, {}, strips).ok);

    const auto elsewhere = document.createCue ("VD000001", 2, "video", "Cold", {},
                                               { { "source", "picture" }, { "file", cold.getFullPathName().toStdString() },
                                                 { "canvas", "VD000011" } });
    REQUIRE (elsewhere.ok);

    const auto bank = document.createCue ("VD000001", 3, "group", "Bank");
    REQUIRE (bank.ok);
    REQUIRE (document.setAttribute ("/godot/cue/" + bank.id + "/mode", "sampler").ok);

    const auto member = document.createCue (bank.id, 0, "video", "Member", {},
                                            { { "source", "picture" }, { "file", ahead.getFullPathName().toStdString() },
                                              { "canvas", "VD000011" } });
    REQUIRE (member.ok);
    REQUIRE (document.setAttribute ("/godot/list/goDebounce", "0").ok);

    Engine engine;
    engine.log().openInMemory ({});
    cue::RunTable runs;
    cue::DcaTable dcas;
    tree::TouchTable touches;
    doc::IdRegistry runIds { doc::IdRegistry::withSeed (55) };
    cue::Focus focus;
    cue::Runner runner { document, runs, runIds, focus };

    doc::registerDocumentCommands (engine.commands(), document, {}, cue::liveWriteFor (runs, dcas, document));
    cue::registerCueCommands (engine.commands(), document, focus);
    cue::registerRunCommands (engine.commands(), runs);
    cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);
    tree::registerTreeCommands (engine.commands(), touches);

    TestClock clock;
    ClockPlayer audio { clock };
    runner.setPlayer (&audio);
    runner.setSamplesPerTick (960);
    runner.setDcas (&dcas);
    runner.setTouches (&touches);

    {
        video::VideoHost host { spec };
        host.configure (document);

        FirstSample sink { host.sink() };
        runner.setVideo (&sink);

        std::int64_t tick = 1;

        /*  THE ENGINE AND THE RENDERER, ticked together on the machine's clock
            until `done`. */
        const auto run = [&] (auto done)
        {
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds (20);

            while (std::chrono::steady_clock::now() < until)
            {
                runner.beforeTick (engine, tick);
                engine.processTick (tick++);
                host.tick (clock.now(), 48000);

                if (done())
                    return true;

                std::this_thread::sleep_for (std::chrono::milliseconds (20));
            }

            return done();
        };

        REQUIRE (run ([&host] { return host.readouts().renderer == "running"; }));
        auto& r = *host.regionForTests();

        //  GO ARMS THE BANK, and its still is read ahead before any hand moves (ABF).
        REQUIRE (document.setAttribute (cue::standbyAddressOf ("VD000001"), bank.id).ok);
        REQUIRE (engine.submit ("cli", "go", {}));

        const auto memberPath = ahead.getFullPathName().toStdString();

        REQUIRE (run ([&host, &memberPath]
                      {
                          const auto readouts = host.readouts();
                          const auto* held = readouts.heldOf (memberPath);
                          return held != nullptr && held->state == video::region::HeldState::ready;
                      }));

        //  PRESSED: up a horizon on, and seen on the renderer's first pass from there.
        const auto* armed = runs.liveRunOf (member.id);
        REQUIRE (armed != nullptr);
        const auto memberRun = armed->id;

        REQUIRE (engine.submit ("window", "strip.press", { osc::Value::string (strips[0]) }));
        REQUIRE (run ([&sink, &memberRun] { return sink.first.count (memberRun) > 0; }));

        const auto aheadLag = lagToFirstSight (host, clock, r, sink.first[memberRun], 0x00FF00);

        //  FIRED BY NAME, never read: seen once it is.
        REQUIRE (engine.submit ("window", "cue.fire", { osc::Value::string (elsewhere.id) }));
        REQUIRE (run ([&runs, &sink, &elsewhere]
                      {
                          const auto* fired = runs.liveRunOf (elsewhere.id);
                          return fired != nullptr && sink.first.count (fired->id) > 0;
                      }));

        const auto coldLag = lagToFirstSight (host, clock, r, sink.first[runs.liveRunOf (elsewhere.id)->id], 0xFF00FF);

        MESSAGE ("M55b, a ", width, "x", height, " still on a strip: read ahead with its bank ", static_cast<double> (aheadLag) / 48.0,
                 " ms after its sample; cold ", static_cast<double> (coldLag) / 48.0, " ms");

        REQUIRE (aheadLag >= 0);
        CHECK (aheadLag < seenSoonEnough);
        CHECK (coldLag >= 0);

        runner.setVideo (nullptr);
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

TEST_CASE ("video host: a HAP movie named to read ahead is opened and read from the second GO starts it at, beside one a layer plays")
{
    using namespace wfg::testing::hapmovie;

    juce::TemporaryFile work;
    const auto folder = work.getFile().getSiblingFile ("godot-video-movie-ahead-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));
    folder.createDirectory();

    std::vector<Bytes> frames;
    frames.push_back (section (0xAB, solidDxt1 (16, 8, 0xFF0000)));
    frames.push_back (section (0xAB, solidDxt1 (16, 8, 0x0000FF)));
    frames.push_back (section (0xAB, solidDxt1 (16, 8, 0x00FF00)));
    const auto movie = writeMovie (folder, "three.mov", hapMovie (16, 8, frames, 1));
    const auto path = movie.getFullPathName().toStdString();

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

        const auto readyAt = [&host] (std::size_t n)
        {
            const auto readouts = host.readouts();
            return readouts.heldCurrent && readouts.held.size() > n
                     && readouts.held[n].state == video::region::HeldState::ready;
        };

        /*  NAMED AT A SECOND AND A HALF: open, the frame there and those after
            it read, and ready - before any layer plays it. */
        host.sink().prepare ({ { path, true, 1.5, 1 } });
        REQUIRE (tickUntil (host, clock, [&] { return readyAt (0); }));

        /*  A LAYER PLAYING ITS START WHILE IT IS NAMED AT TWO AND A HALF: both
            heads held - the red under the layer, and still ready for GO. */
        video::LayerSpec layer;
        layer.id = "RUN00001";
        layer.canvas = "VD000011";
        layer.order = 1;
        layer.source = "movie";
        layer.file = path;

        host.sink().show (layer);
        host.sink().opacity ("RUN00001", { clock.now(), 1.0 });
        host.sink().move ("RUN00001", video::Property::time, { clock.now(), 0.2 });

        std::int64_t seen = -1;
        REQUIRE (tickUntil (host, clock, [&] { return probeOf (r, 0, seen) == 0xFF0000u; }));

        host.sink().prepare ({ { path, true, 2.5, 1 } });
        REQUIRE (tickUntil (host, clock, [&] { return readyAt (0); }));
        CHECK (probeOf (r, 0, seen) == 0xFF0000u);

        /*  AND A SECOND LAYER FROM THERE shows green on its first pass. */
        const auto shownAt = clock.now() + 960;
        layer.id = "RUN00002";
        layer.order = 2;
        host.sink().show (layer);
        host.sink().opacity ("RUN00002", { shownAt, 1.0 });
        host.sink().move ("RUN00002", video::Property::time, { shownAt, 2.5 });

        const auto lag = lagToFirstSight (host, clock, r, shownAt, 0x00FF00);
        REQUIRE (lag >= 0);
        CHECK (lag < seenSoonEnough);

        /*  A MOVIE THAT IS NOT THERE is said so. */
        host.sink().prepare ({ { folder.getChildFile ("gone.mov").getFullPathName().toStdString(), true, 0.0, 1 } });
        REQUIRE (tickUntil (host, clock, [&host]
                            {
                                const auto readouts = host.readouts();
                                return readouts.heldCurrent && readouts.held.size() == 1
                                         && readouts.held[0].state == video::region::HeldState::failed;
                            }));
        CHECK (host.readouts().held[0].problem == "not found");
    }

    folder.deleteRecursively();
}

TEST_CASE ("video host: M54a, a projector-sized HAP movie read ahead is seen at once, and one never named only once it is read")
{
    using namespace wfg::testing::hapmovie;

    juce::TemporaryFile work;
    const auto folder = work.getFile().getSiblingFile ("godot-video-m54-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));
    folder.createDirectory();

    /*  TEN FRAMES OF 1920 BY 1080 at 25 a second - a megabyte each, as a DXT1
        HAP frame of that size is - all one colour, a different file each. */
    const auto movieOf = [&folder] (const char* name, std::uint32_t colour)
    {
        std::vector<Bytes> frames;

        for (int n = 0; n < 10; ++n)
            frames.push_back (section (0xAB, solidDxt1 (1920, 1080, colour)));

        return writeMovie (folder, name, hapMovie (1920, 1080, frames, 25));
    };

    const auto ahead = movieOf ("ahead.mov", 0x00FF00);
    const auto cold = movieOf ("cold.mov", 0xFF00FF);

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

        host.sink().prepare ({ { ahead.getFullPathName().toStdString(), true, 0.0, 1 } });
        REQUIRE (tickUntil (host, clock, [&host]
                            {
                                const auto readouts = host.readouts();
                                return readouts.heldCurrent && readouts.held.size() == 1
                                         && readouts.held[0].state == video::region::HeldState::ready;
                            }));

        const auto showAt = [&] (const char* id, std::uint64_t order, const juce::File& file)
        {
            const auto at = clock.now() + 960;
            video::LayerSpec layer;
            layer.id = id;
            layer.canvas = "VD000011";
            layer.order = order;
            layer.source = "movie";
            layer.file = file.getFullPathName().toStdString();
            host.sink().show (layer);
            host.sink().opacity (id, { at, 1.0 });
            host.sink().move (id, video::Property::time, { at, 0.0 });
            return at;
        };

        const auto aheadAt = showAt ("RUN00001", 1, ahead);
        const auto aheadLag = lagToFirstSight (host, clock, r, aheadAt, 0x00FF00);

        const auto coldAt = showAt ("RUN00002", 2, cold);
        const auto coldLag = lagToFirstSight (host, clock, r, coldAt, 0xFF00FF);

        MESSAGE ("M54a, a 1920x1080 HAP movie: read ahead ", static_cast<double> (aheadLag) / 48.0,
                 " ms after its sample; cold ", static_cast<double> (coldLag) / 48.0, " ms");

        REQUIRE (aheadLag >= 0);
        CHECK (aheadLag < seenSoonEnough);
        CHECK (coldLag >= 0);
    }

    folder.deleteRecursively();
}

/*  A MOVIE THAT IS NOT HAP, PLAYED AS A PREVIEW (namespace draft 37.5, WF;
    37.6, F.6): an MPEG-4 movie FFmpeg makes - a second red, a second blue, a
    second green - read by a renderer with no window through FFmpeg, its frame
    chosen by the playhead, forwards and back. Skipped where FFmpeg is not. */
TEST_CASE ("video host: a movie that is not HAP plays as a preview through FFmpeg, its frame chosen by the playhead")
{
    const auto tools = video::ffmpeg::find();

    if (! tools.found())
        return;

    juce::TemporaryFile work;
    const auto folder = work.getFile().getSiblingFile ("godot-video-preview-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));
    folder.createDirectory();
    const auto movie = folder.getChildFile ("three.mp4");

    video::PipedChild maker;
    REQUIRE (maker.start ({ tools.ffmpeg, "-nostdin", "-v", "error", "-y",
                            "-f", "lavfi", "-i", "color=c=red:s=64x32:r=25:d=1",
                            "-f", "lavfi", "-i", "color=c=blue:s=64x32:r=25:d=1",
                            "-f", "lavfi", "-i", "color=c=lime:s=64x32:r=25:d=1",
                            "-filter_complex", "[0][1][2]concat=n=3:v=1:a=0",
                            "-c:v", "mpeg4", "-q:v", "1", movie.getFullPathName().toStdString() }));
    maker.readAll();
    REQUIRE (maker.wait (60000) == 0);

    CHECK (video::movie::durationOf (movie.getFullPathName().toStdString()) == doctest::Approx (3.0).epsilon (0.05));

    video::HostSpec spec;
    spec.workFolder = folder.getFullPathName().toStdString();
    spec.executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName().toStdString();
    spec.leadingArgs = { "video-render" };
    spec.headless = true;

    doc::ShowDocument document;
    REQUIRE (doc::Bundle::open (videoBundle(), document).ok);

    TestClock clock;

    const auto is = [] (std::uint32_t colour, int channel)
    {
        const int parts[3] { static_cast<int> ((colour >> 16) & 0xffu), static_cast<int> ((colour >> 8) & 0xffu),
                             static_cast<int> (colour & 0xffu) };

        for (int c = 0; c < 3; ++c)
            if (c == channel ? parts[c] < 180 : parts[c] > 80)
                return false;

        return true;
    };

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
        CHECK (tickUntil (host, clock, [&] { return is (probeOf (r, 0, seen), 0); }));

        host.sink().move ("RUN00001", video::Property::time, { clock.now(), 2.5 });
        CHECK (tickUntil (host, clock, [&] { return is (probeOf (r, 0, seen), 1); }));

        /*  AND BACK: the decoder started again from there. */
        host.sink().move ("RUN00001", video::Property::time, { clock.now(), 1.5 });
        CHECK (tickUntil (host, clock, [&] { return is (probeOf (r, 0, seen), 2); }));

        /*  NAMED TO READ AHEAD while no layer plays it (namespace draft §48,
            AAQ): probed, its decoder started at the second GO will start it
            at, and ready - then a layer from there is blue at once. */
        host.sink().remove ("RUN00001", clock.now());
        host.sink().prepare ({ { movie.getFullPathName().toStdString(), true, 1.5, 1 } });

        REQUIRE (tickUntil (host, clock, [&host]
                            {
                                const auto readouts = host.readouts();
                                return readouts.heldCurrent && readouts.held.size() == 1
                                         && readouts.held[0].state == video::region::HeldState::ready;
                            }));

        layer.id = "RUN00002";
        host.sink().show (layer);
        host.sink().opacity ("RUN00002", { clock.now(), 1.0 });
        host.sink().move ("RUN00002", video::Property::time, { clock.now(), 1.5 });
        CHECK (tickUntil (host, clock, [&] { return is (probeOf (r, 0, seen), 2); }, 2000));
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

TEST_CASE ("video compositor: normal, add, screen and multiply, in display space")
{
    /*  GREY (128) UNDER, AND A HALF-RED LAYER OVER IT AT FULL OPACITY, each
        blend on its own canvas - the numbers the GPU's equations give. */
    Memory memory;
    video::RegionSink sink { *memory.region };

    const auto put = [&sink] (const char* id, const char* canvas, std::uint64_t order, std::uint32_t paint,
                              const char* blend)
    {
        video::LayerSpec spec;
        spec.id = id;
        spec.canvas = canvas;
        spec.order = order;
        spec.source = "fill";
        spec.paint = paint;
        spec.blend = blend;
        sink.show (spec);
        sink.opacity (id, { 0, 1.0 });
    };

    const char* blends[] { "normal", "add", "screen", "multiply" };
    const char* canvases[] { "N", "A", "S", "M" };

    for (int n = 0; n < 4; ++n)
    {
        put ((std::string ("GREY000") + std::to_string (n)).c_str(), canvases[n], 1, 0x808080, "normal");
        put ((std::string ("OVER000") + std::to_string (n)).c_str(), canvases[n], 2, 0x800000, blends[n]);
    }

    const auto layers = layersOf (*memory.region);
    const auto at = [&layers] (const char* canvas) { return video::fillsAt (video::stackOf (layers, canvas), 10); };

    CHECK (at ("N") == 0x800000u);      // normal: covered
    CHECK (at ("A") == 0xFF8080u);      // add: 128 + 128, clamped
    CHECK (at ("S") == 0xC08080u);      // screen: 1 - (1 - .5)(1 - .5) = .75
    CHECK (at ("M") == 0x400000u);      // multiply: .5 x .5, and nought where red has none
}

TEST_CASE ("video mask: a shape filled even-odd, its edge feathered, turned inside out")
{
    video::mask::Shape square;
    const float xs[] { 0.25f, 0.75f, 0.75f, 0.25f }, ys[] { 0.25f, 0.25f, 0.75f, 0.75f };

    for (int n = 0; n < 4; ++n)
    {
        square.x[n] = xs[n];
        square.y[n] = ys[n];
    }

    square.count = 4;

    /*  (u, v) as a texture is addressed, v from the bottom. */
    CHECK (video::mask::coverAt (square, 0.5, 0.5, 1920.0, 1080.0) == doctest::Approx (1.0));
    CHECK (video::mask::coverAt (square, 0.1, 0.5, 1920.0, 1080.0) == doctest::Approx (0.0));
    CHECK (video::mask::coverAt (square, 0.5, 0.9, 1920.0, 1080.0) == doctest::Approx (0.0));

    /*  FEATHERED 100 PIXELS: half at the edge, full fifty inside it. */
    square.feather = 100.0f;
    CHECK (video::mask::coverAt (square, 0.25, 0.5, 1920.0, 1080.0) == doctest::Approx (0.5).epsilon (0.01));
    CHECK (video::mask::coverAt (square, (480.0 + 50.0) / 1920.0, 0.5, 1920.0, 1080.0) == doctest::Approx (1.0));
    CHECK (video::mask::coverAt (square, (480.0 - 25.0) / 1920.0, 0.5, 1920.0, 1080.0) == doctest::Approx (0.25).epsilon (0.01));

    square.feather = 0.0f;
    square.invert = true;
    CHECK (video::mask::coverAt (square, 0.5, 0.5, 1920.0, 1080.0) == doctest::Approx (0.0));
    CHECK (video::mask::coverAt (square, 0.1, 0.5, 1920.0, 1080.0) == doctest::Approx (1.0));

    /*  NO SHAPE covers nothing - everything, inverted. */
    video::mask::Shape none;
    CHECK (video::mask::coverAt (none, 0.5, 0.5, 1920.0, 1080.0) == doctest::Approx (0.0));
    none.invert = true;
    CHECK (video::mask::coverAt (none, 0.5, 0.5, 1920.0, 1080.0) == doctest::Approx (1.0));
}

TEST_CASE ("video compositor: a black mask laid over a white fill blacks out its shape and nothing else")
{
    Memory memory;
    video::RegionSink sink { *memory.region };

    video::LayerSpec white;
    white.id = "RUN00001";
    white.canvas = "C1";
    white.order = 1;
    white.source = "fill";
    white.paint = 0xFFFFFF;
    sink.show (white);
    sink.opacity ("RUN00001", { 0, 1.0 });

    video::LayerSpec door;
    door.id = "RUN00002";
    door.canvas = "C1";
    door.layer = 5;
    door.order = 2;
    door.source = "mask";
    door.paint = 0x000000;

    const float xs[] { 0.4f, 0.6f, 0.6f, 0.4f }, ys[] { 0.2f, 0.2f, 1.0f, 1.0f };

    for (int n = 0; n < 4; ++n)
    {
        door.shape.x[n] = xs[n];
        door.shape.y[n] = ys[n];
    }

    door.shape.count = 4;
    sink.show (door);
    sink.opacity ("RUN00002", { 0, 1.0 });

    const auto layers = layersOf (*memory.region);
    const auto stack = video::stackOf (layers, "C1");

    /*  THE DOOR'S MIDDLE IS BLACK; beside it, and above its top, still white.
        (x, y) from the canvas's middle, y up. */
    CHECK (video::colourAt (stack, 10, 1920.0, 1080.0, 0.0, -200.0, nullptr) == 0x000000u);
    CHECK (video::colourAt (stack, 10, 1920.0, 1080.0, 400.0, -200.0, nullptr) == 0xFFFFFFu);
    CHECK (video::colourAt (stack, 10, 1920.0, 1080.0, 0.0, 400.0, nullptr) == 0xFFFFFFu);
}

//==============================================================================
TEST_CASE ("video mapping: the mesh nobody moved fills the display, a corner pinned moves, the surface runs through its points")
{
    /*  THE IDENTITY, at every size. */
    for (const auto& size : { std::pair { 2, 2 }, std::pair { 3, 3 }, std::pair { 5, 4 } })
    {
        const auto mesh = video::Mesh::identity (size.first, size.second);
        CHECK (mesh.isIdentity());

        for (const auto& [s, t] : { std::pair { 0.0, 0.0 }, std::pair { 0.37, 0.81 }, std::pair { 1.0, 0.5 } })
        {
            double x = 0.0, y = 0.0;
            video::meshAt (mesh, s, t, x, y);
            CHECK (x == doctest::Approx (s));
            CHECK (y == doctest::Approx (t));
        }
    }

    /*  A KEYSTONE: the top-right corner pulled in. The corner lands where it
        was put; the opposite one does not move. */
    auto keystone = video::Mesh::identity();
    keystone.x[1] = 0.8f;
    keystone.y[1] = 0.1f;
    CHECK_FALSE (keystone.isIdentity());

    double x = 0.0, y = 0.0;
    video::meshAt (keystone, 1.0, 0.0, x, y);
    CHECK (x == doctest::Approx (0.8));
    CHECK (y == doctest::Approx (0.1));

    video::meshAt (keystone, 0.0, 1.0, x, y);
    CHECK (x == doctest::Approx (0.0));
    CHECK (y == doctest::Approx (1.0));

    /*  A 3 x 3 GRID with its middle point raised: the surface passes through
        it, and moves smoothly on either side. */
    auto bowed = video::Mesh::identity (3, 3);
    bowed.y[4] = 0.4f;

    video::meshAt (bowed, 0.5, 0.5, x, y);
    CHECK (x == doctest::Approx (0.5));
    CHECK (y == doctest::Approx (0.4));

    double yLeft = 0.0, yRight = 0.0;
    video::meshAt (bowed, 0.45, 0.5, x, yLeft);
    video::meshAt (bowed, 0.55, 0.5, x, yRight);
    CHECK (yLeft == doctest::Approx (yRight));
    CHECK (yLeft > 0.4);
    CHECK (yLeft < 0.5);

    /*  A GRID THE WRONG SIZE is the identity. */
    video::Mesh broken;
    broken.columns = 3;
    broken.rows = 3;
    broken.x = { 0.0f };
    broken.y = { 0.0f };
    video::meshAt (broken, 0.3, 0.6, x, y);
    CHECK (x == doctest::Approx (0.3));
    CHECK (y == doctest::Approx (0.6));
}

TEST_CASE ("video mapping: an output's ASC CDL, and the identity when it is not set")
{
    const auto none = video::Cdl::from ({});
    CHECK (none.isIdentity());

    /*  SLOPE HALF ON RED, OFFSET A TENTH ON GREEN, POWER TWO ON BLUE. */
    const auto cdl = video::Cdl::from ({ 0.5, 1.0, 1.0, 0.0, 0.1, 0.0, 1.0, 1.0, 2.0, 1.0 });
    CHECK_FALSE (cdl.isIdentity());

    double r = 0.8, g = 0.5, b = 0.5;
    video::applyCdl (cdl, r, g, b);
    CHECK (r == doctest::Approx (0.4));
    CHECK (g == doctest::Approx (0.6));
    CHECK (b == doctest::Approx (0.25));

    /*  SATURATION NOUGHT is grey at luma. */
    const auto grey = video::Cdl::from ({ 1, 1, 1, 0, 0, 0, 1, 1, 1, 0 });
    r = 1.0; g = 0.0; b = 0.0;
    video::applyCdl (grey, r, g, b);
    CHECK (r == doctest::Approx (0.2126));
    CHECK (g == doctest::Approx (0.2126));
}

TEST_CASE ("video host: an output's mesh and CDL reach the region with its configuration")
{
    juce::TemporaryFile work;
    const auto folder = work.getFile().getSiblingFile ("godot-video-mapping-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));

    video::HostSpec spec;
    spec.workFolder = folder.getFullPathName().toStdString();
    spec.executable = "no renderer is started by this test";

    doc::ShowDocument document;
    REQUIRE (doc::Bundle::open (videoBundle(), document).ok);
    REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/mesh", "0 0 0.9 0.1 0 1 1 1").ok);
    REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/cdl", "1 1 1 0 0 0 1 1 1 0.5").ok);
    REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/enabled", "false").ok);

    {
        video::VideoHost host { spec };
        host.configure (document);

        video::region::ConfigReading config;
        REQUIRE (video::region::readConfig (*host.regionForTests(), config));
        REQUIRE (config.outputs.size() == 1);

        const auto& output = config.outputs.front();
        CHECK (output.mesh.columns == 2);
        CHECK (output.mesh.rows == 2);
        REQUIRE (output.mesh.x.size() == 4);
        CHECK (output.mesh.x[1] == doctest::Approx (0.9f));
        CHECK (output.mesh.y[1] == doctest::Approx (0.1f));
        CHECK_FALSE (output.mesh.isIdentity());
        CHECK (output.cdl.saturation == doctest::Approx (0.5));

        /*  A MESH THE WRONG SIZE for its grid is the identity. */
        REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/mesh", "0 0 1 1").ok);
        host.configure (document);
        REQUIRE (video::region::readConfig (*host.regionForTests(), config));
        CHECK (config.outputs.front().mesh.isIdentity());
    }

    folder.deleteRecursively();
}

//==============================================================================
TEST_CASE ("video region: a canvas's level reaches the renderer by the canvas's name")
{
    /*  Namespace draft §38, WT: the engine writes every canvas's level, the
        renderer reads one by name each frame - 1 when no slot names it. */
    Memory memory;
    auto& region = *memory.region;
    video::RegionSink writer { region };

    CHECK (video::region::canvasLevelOf (region, "VD000011") == doctest::Approx (1.0));
    writer.canvasLevels ({ { "VD000011", 0.25 }, { "VD000012", 0.75 } });
    CHECK (video::region::canvasLevelOf (region, "VD000011") == doctest::Approx (0.25));
    CHECK (video::region::canvasLevelOf (region, "VD000012") == doctest::Approx (0.75));
    CHECK (video::region::canvasLevelOf (region, "VD0000ZZ") == doctest::Approx (1.0));
    writer.canvasLevels ({ { "VD000012", 0.5 } });
    CHECK (video::region::canvasLevelOf (region, "VD000011") == doctest::Approx (1.0));
}

TEST_CASE ("video region: the renderer's tints reach the engine by name, and a slot let go says nothing")
{
    Memory memory;
    auto& region = *memory.region;

    CHECK (video::region::readTints (region.layerTints).empty());

    video::region::writeTint (region.layerTints[0], "RUN00001", 0x2040A0u);
    video::region::writeTint (region.layerTints[1], "RUN00002", 0xFFFFFFu);
    video::region::writeTint (region.canvasTints[0], "CANVAS01", 0x101010u);

    const auto layers = video::region::readTints (region.layerTints);
    REQUIRE (layers.size() == 2);
    CHECK (layers[0] == std::pair<std::string, std::uint32_t> { "RUN00001", 0x2040A0u });
    CHECK (video::region::readTints (region.canvasTints).size() == 1);

    video::region::writeTint (region.layerTints[1], {}, 0);
    CHECK (video::region::readTints (region.layerTints).size() == 1);
}

//==============================================================================
/*  NAMESPACE DRAFT 40 (WY): several canvases on one output, each through a
    warp of its own; and the canvases drawn small for the video monitor. */
TEST_CASE ("video host: an output's zones reach the region bottom first, by their canvas, blend, opacity and warp (§40)")
{
    juce::TemporaryFile work;
    const auto folder = work.getFile().getSiblingFile ("godot-video-zones-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));

    video::HostSpec spec;
    spec.workFolder = folder.getFullPathName().toStdString();
    spec.executable = "no renderer is started by this test";

    doc::ShowDocument document;
    REQUIRE (doc::Bundle::open (videoBundle(), document).ok);
    REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/enabled", "false").ok);

    const auto left = document.createZone ("VD000021", "VD000011");
    const auto right = document.createZone ("VD000021", {});
    REQUIRE (left.ok);
    REQUIRE (right.ok);

    //  Not on a canvas, nor on anything but an output.
    CHECK_FALSE (document.createZone ("VD000011", {}).ok);
    CHECK_FALSE (document.createZone ("VD000021", "VD000021").ok);

    REQUIRE (document.setAttribute ("/godot/zone/" + left.id + "/blend", "add").ok);
    REQUIRE (document.setAttribute ("/godot/zone/" + left.id + "/opacity", "50").ok);
    REQUIRE (document.setAttribute ("/godot/zone/" + left.id + "/mesh", "0 0 0.5 0 0 1 0.5 1").ok);

    {
        video::VideoHost host { spec };
        host.configure (document);

        video::region::ConfigReading config;
        REQUIRE (video::region::readConfig (*host.regionForTests(), config));
        REQUIRE (config.outputs.size() == 1);

        const auto& zones = config.outputs.front().zones;
        REQUIRE (zones.size() == 2);
        CHECK (zones[0].canvas == "VD000011");
        CHECK (zones[0].blend == video::region::Blend::add);
        CHECK (zones[0].opacity == doctest::Approx (0.5));
        REQUIRE (zones[0].mesh.x.size() == 4);
        CHECK (zones[0].mesh.x[1] == doctest::Approx (0.5f));
        CHECK_FALSE (zones[0].mesh.isIdentity());

        //  A new zone is the whole output, normal and solid, until moved.
        CHECK (zones[1].canvas.empty());
        CHECK (zones[1].blend == video::region::Blend::normal);
        CHECK (zones[1].opacity == doctest::Approx (1.0));
        CHECK (zones[1].mesh.isIdentity());

        //  Taken away, it goes from the region with the next configuration.
        REQUIRE (document.remove (right.id).ok);
        host.configure (document);
        REQUIRE (video::region::readConfig (*host.regionForTests(), config));
        CHECK (config.outputs.front().zones.size() == 1);
    }

    folder.deleteRecursively();
}

TEST_CASE ("video host: while a monitor watches, a renderer with no window draws every canvas small, its shape kept (§40)")
{
    juce::TemporaryFile work;
    const auto folder = work.getFile().getSiblingFile ("godot-video-monitor-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));

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
        host.configure (document);
        REQUIRE (tickUntil (host, clock, [&host] { return host.readouts().renderer == "running"; }));

        const auto at = clock.now();
        host.sink().show (fill ("RUN00001", "VD000011", 0, 1, 0x2040A0));
        host.sink().opacity ("RUN00001", { at, 1.0 });

        //  Nobody watching: nothing is drawn for anybody.
        for (int n = 0; n < 30; ++n)
            tickUntil (host, clock, [] { return false; }, 10);

        CHECK (host.canvasPictures().empty());

        host.setMonitoring (true);

        std::vector<video::VideoHost::CanvasPicture> pictures;
        CHECK (tickUntil (host, clock, [&]
                          {
                              pictures = host.canvasPictures();
                              return ! pictures.empty() && pictures.front().rgb.size() >= 3
                                       && pictures.front().rgb[0] == 0x20 && pictures.front().rgb[1] == 0x40
                                       && pictures.front().rgb[2] == 0xA0;
                          }));

        REQUIRE_FALSE (pictures.empty());
        CHECK (pictures.front().canvasId == "VD000011");
        CHECK (pictures.front().width <= video::region::previewWidth);
        CHECK (pictures.front().height <= video::region::previewHeight);
        CHECK (pictures.front().rgb.size() == static_cast<std::size_t> (3 * pictures.front().width * pictures.front().height));

        host.setMonitoring (false);
    }

    folder.deleteRecursively();
}

#if JUCE_WINDOWS
//==============================================================================
/*  AN OUTPUT SENT OVER SPOUT, THE WHOLE WAY (namespace draft §44, YA; N.1): the
    show sets the fixture's output to spout under a name; the host writes it to
    the region; a renderer with no window draws it on its render thread, on
    WARP where the machine has no graphics card, and Spout shares it; this
    process - as another program would - finds it by that name and reads the
    fill the engine put up. Spout's headers last: they bring windowsx.h's
    macros. */
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#include <spout/SpoutDX.h>

TEST_CASE ("video host: an output set to spout is sent by a renderer with no window, and another program reads it (N.1)")
{
    juce::TemporaryFile work;
    const auto folder = work.getFile().getSiblingFile ("godot-video-spout-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));

    video::HostSpec spec;
    spec.workFolder = folder.getFullPathName().toStdString();
    spec.executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName().toStdString();
    spec.leadingArgs = { "video-render" };
    spec.headless = true;

    doc::ShowDocument document;
    REQUIRE (doc::Bundle::open (videoBundle(), document).ok);

    const auto name = "Go.dot spout test " + std::to_string (GetCurrentProcessId());
    REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/kind", "spout").ok);
    REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/sendName", name).ok);
    REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/frameRate", "30").ok);

    /*  BOTH ON WARP: Spout shares a picture only between programs on the
        same graphics card, and this one's receiver is made on WARP. */
    SetEnvironmentVariableA ("WFG_VIDEO_SOFTWARE", "1");

    TestClock clock;
    video::VideoHost host { spec };
    host.configure (document);

    REQUIRE (tickUntil (host, clock, [&host] { return host.readouts().renderer == "running"; }));
    SetEnvironmentVariableA ("WFG_VIDEO_SOFTWARE", nullptr);

    REQUIRE (tickUntil (host, clock, [&host]
                        {
                            const auto said = host.readouts();
                            const auto* output = said.output ("VD000021");
                            return output != nullptr && output->bound && output->framesPresented > 3;
                        }, 15000));

    host.sink().show (fill ("RUN00001", "VD000011", 0, 1, 0x50A020));
    host.sink().opacity ("RUN00001", { clock.now() - 48000, 1.0 });

    /*  ANOTHER PROGRAM: its own device, the sender found by its name. */
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    REQUIRE (SUCCEEDED (D3D11CreateDevice (nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                           D3D11_SDK_VERSION, &device, nullptr, &context)));

    std::uint32_t received = 0xFFFFFFFFu;

    {
        spoutDX receiver;
        REQUIRE (receiver.OpenDirectX11 (device));
        receiver.SetReceiverName (name.c_str());

        tickUntil (host, clock, [&]
        {
            if (! receiver.ReceiveTexture() || receiver.IsUpdated())
                return false;

            auto* texture = receiver.GetSenderTexture();

            if (texture == nullptr)
                return false;

            D3D11_TEXTURE2D_DESC desc {};
            texture->GetDesc (&desc);
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;

            ID3D11Texture2D* staging = nullptr;

            if (FAILED (device->CreateTexture2D (&desc, nullptr, &staging)))
                return false;

            context->CopyResource (staging, texture);
            D3D11_MAPPED_SUBRESOURCE mapped {};

            if (SUCCEEDED (context->Map (staging, 0, D3D11_MAP_READ, 0, &mapped)))
            {
                const auto* pixel = static_cast<const std::uint8_t*> (mapped.pData) + (desc.Height / 2) * mapped.RowPitch + 4 * (desc.Width / 2);
                const auto bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
                received = (static_cast<std::uint32_t> (pixel[bgra ? 2 : 0]) << 16) | (static_cast<std::uint32_t> (pixel[1]) << 8)
                         | static_cast<std::uint32_t> (pixel[bgra ? 0 : 2]);
                context->Unmap (staging, 0);
            }

            staging->Release();

            //  The fill, once it has reached the picture sent.
            return std::abs (static_cast<int> ((received >> 8) & 0xffu) - 0xA0) <= 2;
        }, 10000);

        receiver.ReleaseReceiver();
        receiver.CloseDirectX11();
    }

    context->Release();
    device->Release();

    INFO ("received " << received);
    CHECK (std::abs (static_cast<int> ((received >> 16) & 0xffu) - 0x50) <= 2);
    CHECK (std::abs (static_cast<int> ((received >> 8) & 0xffu) - 0xA0) <= 2);
    CHECK (std::abs (static_cast<int> (received & 0xffu) - 0x20) <= 2);

    const auto said = host.readouts();
    const auto* output = said.output ("VD000021");
    REQUIRE (output != nullptr);
    CHECK (output->bound);
    CHECK (output->problem.empty());
}
/*  AN INSERT'S SEND, BEFORE ANY CUE (namespace draft §47, AAK): declared with
    nothing named to come back from and no output switched on, its send is up -
    black - for another program to find and patch to, and stays up: it used to
    be made again every two seconds while no return was named, and Spout listed
    it only once a cue had gone through. */
TEST_CASE ("video host: an insert's send is there before any cue goes through it, black, and stays with nothing named to come back (§47)")
{
    juce::TemporaryFile work;
    const auto folder = work.getFile().getSiblingFile ("godot-video-insert-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));

    video::HostSpec spec;
    spec.workFolder = folder.getFullPathName().toStdString();
    spec.executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName().toStdString();
    spec.leadingArgs = { "video-render" };
    spec.headless = true;

    doc::ShowDocument document;
    REQUIRE (doc::Bundle::open (videoBundle(), document).ok);
    REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/enabled", "false").ok);

    const auto name = "Go.dot insert test " + std::to_string (GetCurrentProcessId()) + " (send)";
    REQUIRE (document.createVideoInsert ("Spout insert 1", "spout", name, {}).ok);

    SetEnvironmentVariableA ("WFG_VIDEO_SOFTWARE", "1");

    TestClock clock;
    video::VideoHost host { spec };
    host.configure (document);

    //  No output on: the insert alone keeps the renderer running.
    REQUIRE (tickUntil (host, clock, [&host] { return host.readouts().renderer == "running"; }));
    SetEnvironmentVariableA ("WFG_VIDEO_SOFTWARE", nullptr);

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    REQUIRE (SUCCEEDED (D3D11CreateDevice (nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                           D3D11_SDK_VERSION, &device, nullptr, &context)));

    {
        spoutDX receiver;
        REQUIRE (receiver.OpenDirectX11 (device));
        receiver.SetReceiverName (name.c_str());

        //  Found, with no cue through it.
        CHECK (tickUntil (host, clock, [&] { return receiver.ReceiveTexture() && receiver.GetSenderTexture() != nullptr; }, 10000));

        //  And still there five seconds on, whatever the missing return does.
        auto lost = 0;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds (5);

        while (std::chrono::steady_clock::now() < until)
        {
            host.tick (clock.now(), 48000);

            if (! receiver.ReceiveTexture())
                ++lost;

            std::this_thread::sleep_for (std::chrono::milliseconds (100));
        }

        CHECK (lost == 0);

        receiver.ReleaseReceiver();
        receiver.CloseDirectX11();
    }

    context->Release();
    device->Release();

    //  The state says which side is missing, in words.
    CHECK (tickUntil (host, clock, [&host]
                      {
                          const auto said = host.readouts();
                          const auto* insert = said.insert (said.inserts.empty() ? std::string {} : said.inserts.front().id);
                          return insert != nullptr && insert->problem == "nothing named to come back from";
                      }, 5000));

    folder.deleteRecursively();
}
#endif

TEST_CASE ("video host: a layer restated takes its new look and keeps its points, its place and its removal (§47)")
{
    /*  A PLAYING CUE EDITED (namespace draft §47, AAE): `restate` writes the
        look and nothing else - where the layer is in its fades and when it
        goes are as they were, which `show` would have started again. */
    Memory memory;
    auto& r = *memory.region;
    video::RegionSink sink { r };

    auto spec = fill ("RUN00001", "CANVAS01", 2, 7, 0x2040A0);
    sink.show (spec);
    sink.opacity ("RUN00001", { 1000, 0.0 });
    sink.opacity ("RUN00001", { 49000, 1.0 });
    sink.move ("RUN00001", video::Property::scale, { 2000, 150.0 });
    sink.remove ("RUN00001", 90000);

    spec.paint = 0xFF0000;
    spec.blend = "screen";
    spec.offsetX = 25.0;
    spec.grade.hue = 30.0;
    spec.shape.count = 3;
    spec.shape.x[1] = 0.5f;
    sink.restate (spec);

    const auto layers = layersOf (r);
    REQUIRE (layers.size() == 1);
    CHECK (layers[0].paint == 0xFF0000u);
    CHECK (layers[0].blend == video::region::Blend::screen);
    CHECK (layers[0].offsetX == doctest::Approx (25.0));
    CHECK (layers[0].grade.hue == doctest::Approx (30.0));
    CHECK (layers[0].shape.count == 3);
    CHECK (layers[0].shape.x[1] == doctest::Approx (0.5f));

    //  Its points, its place in the stack, its removal: untouched.
    CHECK (layers[0].ring (video::Property::opacity).count == 2);
    CHECK (layers[0].ring (video::Property::scale).count == 1);
    CHECK (layers[0].layer == 2);
    CHECK (layers[0].order == 7u);
    CHECK (layers[0].removeAt == 90000);

    //  And a layer nobody showed is restated nowhere.
    auto other = fill ("NOBODY01", "CANVAS01", 1, 1, 0x000000);
    sink.restate (other);
    CHECK (layersOf (r).size() == 1);
}

TEST_CASE ("video host: the analyser reads a picture's size and a movie's, for the picture panel's frame (§47)")
{
    /*  THE PICTURE PANEL (namespace draft §47, AAG) draws a fitted picture's
        frame in its own shape: the analyser, which every file a video cue
        names now reaches, reads a still's size by decoding it and a movie's
        from its index. */
    auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getNonexistentChildFile ("wfg-picture-sizes", {}, false);
    REQUIRE (folder.createDirectory());

    {
        juce::Image still (juce::Image::RGB, 40, 30, true, juce::SoftwareImageType());
        juce::FileOutputStream out (folder.getChildFile ("logo.png"));
        REQUIRE (out.openedOk());
        REQUIRE (juce::PNGImageFormat().writeImageToStream (still, out));
    }

    using namespace wfg::testing::hapmovie;
    writeMovie (folder, "clip.mov", hapMovie (16, 8, { section (0xAB, solidDxt1 (16, 8, 0xFF8000)) }, 25));

    doc::ShowDocument document;
    const auto mediaFolder = folder.getFullPathName().toStdString();
    audio::MediaInfo info { document, mediaFolder };
    audio::MediaAnalyser analyser { info, mediaFolder };

    CHECK (analyser.queue ("logo.png"));
    CHECK (analyser.queue ("clip.mov"));
    REQUIRE (analyser.start());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (60);

    while (analyser.outstanding() > 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (10));

    const auto records = info.snapshot();

    REQUIRE (records->count ("logo.png") == 1u);
    CHECK (records->at ("logo.png").width == 40);
    CHECK (records->at ("logo.png").height == 30);
    CHECK (records->at ("logo.png").pyramid == nullptr);      // a picture has no waveform

    REQUIRE (records->count ("clip.mov") == 1u);
    CHECK (records->at ("clip.mov").width == 16);
    CHECK (records->at ("clip.mov").height == 8);
    CHECK (records->at ("clip.mov").seconds > 0.0);

    /*  AND ITS STRIP (§47, AAI): found, published on its record, and kept
        beside the analysis of sounds to be read back. */
    REQUIRE (records->at ("clip.mov").strip != nullptr);
    CHECK (records->at ("clip.mov").strip->thumbnails.size() == 1u);
    CHECK (folder.getChildFile (".timbre").findChildFiles (juce::File::findFiles, false, "*.tms").size() == 1);

    analyser.stop();
    folder.deleteRecursively();
}

TEST_CASE ("video host: while the monitor watches, the picked cue is drawn alone in its canvas's shape, with no output switched on (§47)")
{
    /*  THE PICKED CUE'S TILE (namespace draft §47, AAH): the cue as it would
        look on its canvas, playing or not - here a red fill at half its size,
        so the middle is red and the corners black. A show with no output
        switched on has no renderer, until a monitor watches. */
    juce::TemporaryFile work;
    const auto folder = work.getFile().getSiblingFile ("godot-video-tile-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));

    video::HostSpec spec;
    spec.workFolder = folder.getFullPathName().toStdString();
    spec.executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName().toStdString();
    spec.leadingArgs = { "video-render" };
    spec.headless = true;

    doc::ShowDocument document;
    REQUIRE (doc::Bundle::open (videoBundle(), document).ok);
    REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/enabled", "false").ok);

    TestClock clock;

    {
        video::VideoHost host { spec };
        REQUIRE (host.isOpen());
        host.configure (document);

        //  No output on and nobody watching: no renderer.
        tickUntil (host, clock, [] { return false; }, 300);
        CHECK (host.readouts().renderer != "running");

        host.setMonitoring (true);
        REQUIRE (tickUntil (host, clock, [&host] { return host.readouts().renderer == "running"; }));

        auto look = fill ("VD000002", "VD000011", 0, 1, 0xC03010);
        look.scale = 50.0;
        host.showTile (look, 0.0, 1.0);

        const auto pixel = [] (const video::VideoHost::CueTile& tile, int x, int y)
        {
            const auto* at = tile.rgb.data() + 3 * (y * tile.width + x);
            return (static_cast<std::uint32_t> (at[0]) << 16) | (static_cast<std::uint32_t> (at[1]) << 8) | at[2];
        };

        video::VideoHost::CueTile tile;
        CHECK (tickUntil (host, clock, [&]
                          {
                              tile = host.cueTile();
                              return tile.width > 0 && tile.height > 0
                                       && pixel (tile, tile.width / 2, tile.height / 2) == 0xC03010u;
                          }));

        REQUIRE (tile.width > 0);
        CHECK (tile.width == video::region::tileWidth);       // the canvas's shape, 16 by 9
        CHECK (tile.height == video::region::tileHeight);
        CHECK (pixel (tile, 2, 2) == 0u);
        CHECK (pixel (tile, tile.width - 3, tile.height - 3) == 0u);

        //  Let go: nothing to read.
        host.hideTile();
        CHECK (host.cueTile().width == 0);

        host.setMonitoring (false);
    }

    folder.deleteRecursively();
}

TEST_CASE ("video host: the picked movie's tile shows the frame at the second asked, while the same movie plays another (§47)")
{
    /*  A MOVIE'S TILE (namespace draft §47, AAH): the frame under an in point
        being dragged, say, while the movie plays somewhere else on the canvas -
        the tile reads the file through a store of its own, so neither pulls the
        other's frame away. Red, blue, green, a second each. */
    using namespace wfg::testing::hapmovie;

    juce::TemporaryFile work;
    const auto folder = work.getFile().getSiblingFile ("godot-video-tile-movie-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));
    folder.createDirectory();

    std::vector<Bytes> frames;
    frames.push_back (section (0xAB, solidDxt1 (16, 8, 0xFF0000)));
    frames.push_back (section (0xAB, solidDxt1 (16, 8, 0x0000FF)));
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
        host.setMonitoring (true);

        auto& r = *host.regionForTests();

        video::LayerSpec layer;
        layer.id = "RUN00001";
        layer.canvas = "VD000011";
        layer.order = 1;
        layer.source = "movie";
        layer.file = movie.getFullPathName().toStdString();

        //  Playing at its first frame.
        host.sink().show (layer);
        host.sink().opacity ("RUN00001", { clock.now(), 1.0 });
        host.sink().move ("RUN00001", video::Property::time, { clock.now(), 0.2 });

        std::int64_t seen = -1;
        CHECK (tickUntil (host, clock, [&] { return probeOf (r, 0, seen) == 0xFF0000u; }));

        //  Its tile asked at a second and a half: the second frame, and the canvas still on the first.
        auto look = layer;
        look.id = "VD000060";
        host.showTile (look, 1.5, 1.0);

        video::VideoHost::CueTile tile;
        CHECK (tickUntil (host, clock, [&]
                          {
                              tile = host.cueTile();

                              if (tile.width <= 0 || tile.height <= 0)
                                  return false;

                              const auto* at = tile.rgb.data() + 3 * ((tile.height / 2) * tile.width + tile.width / 2);
                              return at[0] == 0x00 && at[1] == 0x00 && at[2] == 0xFF;
                          }));

        CHECK (tickUntil (host, clock, [&] { return probeOf (r, 0, seen) == 0xFF0000u; }));

        host.hideTile();
        host.setMonitoring (false);
    }

    folder.deleteRecursively();
}

//==============================================================================
/*  A MOVIE'S EDIT, RENDERED AND PLAYED (namespace draft §55.5): the render is
    a HAP movie like any other to the renderer with no window, its shots in
    the edit's order and its dissolve between them. */

TEST_CASE ("video host: a movie's edit rendered is played by a renderer with no window, the shots in their new order and the dissolve between them (§55.5)")
{
    using namespace wfg::testing::hapmovie;

    juce::TemporaryFile work;
    const auto folder = work.getFile().getSiblingFile ("godot-video-edit-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));
    folder.createDirectory();

    /*  THREE SECONDS AT TEN A SECOND: red, green, blue. */
    std::vector<Bytes> frames;
    const std::uint32_t colours[3] { 0xFF0000, 0x00FF00, 0x0000FF };

    for (int n = 0; n < 30; ++n)
        frames.push_back (section (0xAB, solidDxt1 (16, 8, colours[n / 10])));

    const auto movie = writeMovie (folder, "three.mov", hapMovie (16, 8, frames, 10));

    /*  THE BLUE SECOND THEN THE GREEN, four frames of dissolve between. */
    doc::Section blue, green;
    blue.id = "B";  blue.in = 2.0;  blue.out = 3.0;
    blue.fadeIn = 0.0; blue.fadeOut = 0.4;
    green.id = "G"; green.in = 1.0; green.out = 2.0; green.fadeIn = 0.4; green.fadeOut = 0.0;

    const auto render = folder.getChildFile ("edit.mov");
    const auto result = video::movie::renderMovieEdit (movie.getFullPathName().toStdString(), { blue, green },
                                                       render.getFullPathName().toStdString());
    REQUIRE_MESSAGE (result.ok, result.problem);

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
        layer.file = render.getFullPathName().toStdString();

        host.sink().show (layer);
        host.sink().opacity ("RUN00001", { clock.now(), 1.0 });
        host.sink().move ("RUN00001", video::Property::time, { clock.now(), 0.5 });

        std::int64_t seen = -1;
        CHECK (tickUntil (host, clock, [&] { return probeOf (r, 0, seen) == 0x0000FFu; }));

        host.sink().move ("RUN00001", video::Property::time, { clock.now(), 1.5 });
        CHECK (tickUntil (host, clock, [&] { return probeOf (r, 0, seen) == 0x00FF00u; }));

        /*  INSIDE THE DISSOLVE, at 0.95 s: blue five eighths, and the red the
            incoming green's section runs back into three eighths. */
        const auto closeTo = [] (std::uint32_t colour, int wantRed, int wantGreen, int wantBlue, int within)
        {
            return std::abs (static_cast<int> ((colour >> 16) & 0xffu) - wantRed) <= within
                && std::abs (static_cast<int> ((colour >> 8) & 0xffu) - wantGreen) <= within
                && std::abs (static_cast<int> (colour & 0xffu) - wantBlue) <= within;
        };

        host.sink().move ("RUN00001", video::Property::time, { clock.now(), 0.95 });
        CHECK (tickUntil (host, clock, [&] { return closeTo (probeOf (r, 0, seen), 96, 0, 159, 12); }));
    }

    folder.deleteRecursively();
}
