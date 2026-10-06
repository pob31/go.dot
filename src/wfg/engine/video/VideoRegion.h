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

#pragma once

/*
    WHAT THE ENGINE AND THE RENDERER SAY TO EACH OTHER (Phase 8a, namespace
    draft 35.4, decisions UV and VC).

    One memory-mapped file, laid out by the engine and read by both as the
    structure below - the editor helper's region in the same discipline: every
    field a lock-free atomic, or a byte array published under a sequence the
    reader checks. It outlives the renderer: a renderer that crashes and is
    started again reads the same file, and draws again what was up (VC),
    because the ENGINE HOLDS THE WHOLE SCENE here and the renderer nothing of
    its own.

    ENGINE TO RENDERER, all written on the tick thread, never waiting:
      - the CLOCK: a sample of Go.dot's and the moment it was read, a pair the
        renderer turns into "which sample is this frame shown at";
      - the CONFIGURATION: the canvases and the outputs, as the show says them;
      - the LAYERS: one slot per video run, what it is, and a ring of the
        points its opacity goes through (video::Point), and when it goes.

    RENDERER TO ENGINE: ready or failed-and-why, a heartbeat, the displays this
    machine has (VL), and per output whether it is bound and how its frames
    are going - and, for a renderer started with `--no-window`, the colour at
    the middle of each canvas, which is how CI looks at a picture with no
    screen.

    NAMES NO JUCE TYPE, so a test lays one out in a vector and drives both ends
    with no file and no process. Numbers cross as numbers; text only where a
    name is the thing (an identifier, a display's name) - never a document's
    number to parse (VM).
*/

#include <wfg/engine/video/VideoSink.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::video::region
{
    /** "GotV", read as a little-endian word. */
    constexpr std::uint32_t magic = 0x56746f47u;

    /** Bumped whenever the structure below changes shape. */
    constexpr std::uint32_t version = 1;

    constexpr int idChars = 16;
    constexpr int nameChars = 160;
    constexpr int textChars = 256;

    constexpr int maxCanvases = 16;
    constexpr int maxOutputs = 16;
    constexpr int maxDisplays = 16;
    constexpr int maxLayers = 64;

    /** A layer's opacity points, at most, held at once; a power of two. */
    constexpr std::uint32_t pointsPerLayer = 128;

    /** "Not going": a layer whose removal has not been placed. */
    constexpr std::int64_t notRemoved = std::numeric_limits<std::int64_t>::max();

    enum class Source : std::uint32_t { fill = 0, mask = 1, picture = 2 };

    inline Source sourceFrom (std::string_view word) noexcept
    {
        if (word == "mask")    return Source::mask;
        if (word == "picture") return Source::picture;
        return Source::fill;
    }

    //==============================================================================
    /*  THE CLOCK (§35.4): where Go.dot's samples were when they were read, and
        when - nanoseconds of the machine's steady clock, which every process
        on it shares. Written as a pair under `seq` (odd while being written),
        so a reader never takes a sample from one write and a time from the
        next. */
    struct Clock
    {
        std::atomic<std::uint32_t> seq;
        std::atomic<std::int64_t> sample;
        std::atomic<std::int64_t> nanos;
        std::atomic<std::int32_t> sampleRate;
    };

    struct Canvas
    {
        char id[idChars];
        std::atomic<std::int32_t> width;
        std::atomic<std::int32_t> height;
    };

    struct Output
    {
        char id[idChars];
        char canvas[idChars];
        char name[nameChars];
        char display[nameChars];
        char displayId[nameChars];
        std::atomic<std::uint32_t> enabled;
        std::atomic<std::uint32_t> testPattern;
    };

    /*  THE SHOW'S CANVASES AND OUTPUTS, rewritten whole when the show changes,
        under `seq` (odd while being written). */
    struct Config
    {
        std::atomic<std::uint32_t> seq;
        std::atomic<std::uint32_t> canvasCount;
        Canvas canvases[maxCanvases];
        std::atomic<std::uint32_t> outputCount;
        Output outputs[maxOutputs];
    };

    struct PointSlot
    {
        std::atomic<std::int64_t> sample;
        std::atomic<double> opacity;
    };

    /*  ONE VIDEO RUN. What it is - written under `seq` (odd while the engine
        writes it) when it comes up - and then only appended to: a point at a
        time, published by `pointsWritten` with release, and its removal. A
        slot is `used` from its show to the engine's letting it go, which is
        after its removal has passed. */
    struct Layer
    {
        std::atomic<std::uint32_t> seq;
        std::atomic<std::uint32_t> used;
        char id[idChars];
        char canvas[idChars];
        std::atomic<std::int32_t> layer;
        std::atomic<std::uint64_t> order;
        std::atomic<std::uint32_t> source;
        std::atomic<std::uint32_t> paint;

        std::atomic<std::uint32_t> pointsWritten;
        PointSlot points[pointsPerLayer];
        std::atomic<std::int64_t> removeAt;
    };

    struct Display
    {
        char name[nameChars];
        char id[nameChars];
        std::atomic<std::int32_t> x;
        std::atomic<std::int32_t> y;
        std::atomic<std::int32_t> width;
        std::atomic<std::int32_t> height;
        std::atomic<float> refreshHz;
    };

    struct OutputState
    {
        std::atomic<std::uint32_t> seq;
        char outputId[idChars];
        std::atomic<std::uint32_t> bound;
        char problem[textChars];
        std::atomic<std::uint64_t> framesPresented;
        std::atomic<std::uint64_t> framesLate;
        std::atomic<float> jitterMs;
    };

    struct Region
    {
        std::atomic<std::uint32_t> magic;
        std::atomic<std::uint32_t> version;
        std::atomic<std::uint32_t> layoutHash;

        //==============================================================================
        /** Engine to renderer: 1, leave. */
        std::atomic<std::uint32_t> shouldExit;

        Clock clock;
        Config config;
        Layer layers[maxLayers];

        //==============================================================================
        /** Renderer to engine: up, its windows made for the outputs it could bind. */
        std::atomic<std::uint32_t> ready;

        /** Renderer to engine: it could not come up, and `problem` says why. */
        std::atomic<std::uint32_t> failed;
        char problem[textChars];

        /** Renderer to engine: counted up while it is alive and answering. */
        std::atomic<std::uint64_t> heartbeat;

        /*  The displays this machine has, by the names the system gives them,
            under `displaysSeq` (odd while being written). */
        std::atomic<std::uint32_t> displaysSeq;
        std::atomic<std::uint32_t> displayCount;
        Display displays[maxDisplays];

        /*  Per output, in the configuration's order, each under its own `seq`:
            the output it is about (by identifier, since the configuration may
            have moved on since), bound or why not, and its frames. */
        OutputState outputs[maxOutputs];

        /*  `--no-window` only: the colour at the middle of each canvas, as
            0xRRGGBB, in the configuration's order, and the sample it is for -
            what a test asks instead of looking at a screen. */
        std::atomic<std::uint32_t> probeSeq;
        std::atomic<std::int64_t> probeSample;
        std::atomic<std::uint32_t> probe[maxCanvases];
    };

    static_assert (std::atomic<std::uint32_t>::is_always_lock_free
                     && std::atomic<std::int32_t>::is_always_lock_free
                     && std::atomic<std::int64_t>::is_always_lock_free
                     && std::atomic<std::uint64_t>::is_always_lock_free
                     && std::atomic<float>::is_always_lock_free
                     && std::atomic<double>::is_always_lock_free,
                   "two processes share these: a locking atomic would lock nothing across them");

    static_assert ((pointsPerLayer & (pointsPerLayer - 1)) == 0, "the ring wraps with a mask");

    constexpr std::size_t regionBytes() noexcept
    {
        constexpr std::size_t page = 4096;
        return (sizeof (Region) + page - 1) / page * page;
    }

    /** FNV-1a over everything the layout depends on. */
    constexpr std::uint32_t layoutHash() noexcept
    {
        std::uint32_t hash = 2166136261u;

        const auto mix = [&hash] (std::uint32_t value)
        {
            for (int i = 0; i < 4; ++i)
            {
                hash ^= (value >> (8 * i)) & 0xffu;
                hash *= 16777619u;
            }
        };

        mix (version);
        mix (static_cast<std::uint32_t> (sizeof (Region)));
        mix (static_cast<std::uint32_t> (sizeof (Layer)));
        mix (static_cast<std::uint32_t> (sizeof (Config)));
        mix (static_cast<std::uint32_t> (sizeof (Display)));
        mix (static_cast<std::uint32_t> (sizeof (OutputState)));
        mix (static_cast<std::uint32_t> (maxLayers));
        mix (pointsPerLayer);
        return hash;
    }

    inline bool looksValid (const Region& r) noexcept
    {
        return r.magic.load (std::memory_order_relaxed) == magic
            && r.version.load (std::memory_order_relaxed) == version
            && r.layoutHash.load (std::memory_order_relaxed) == layoutHash();
    }

    /*  Stamps a freshly zeroed region as this build's, the hash last with
        release, so a reader that sees the hash sees the rest. */
    inline void stamp (Region& r) noexcept
    {
        r.magic.store (magic, std::memory_order_relaxed);
        r.version.store (version, std::memory_order_relaxed);

        for (auto& layer : r.layers)
            layer.removeAt.store (notRemoved, std::memory_order_relaxed);

        r.layoutHash.store (layoutHash(), std::memory_order_release);
    }

    //==============================================================================
    /*  TEXT IN A FIXED FIELD, cut to fit and NUL-ended, and read back up to the
        NUL. A name longer than the field is cut, never overrun. */
    inline void writeText (char* into, std::size_t size, std::string_view text) noexcept
    {
        const auto n = std::min (text.size(), size - 1);
        std::memcpy (into, text.data(), n);
        std::memset (into + n, 0, size - n);
    }

    inline std::string readText (const char* from, std::size_t size)
    {
        const auto* end = static_cast<const char*> (std::memchr (from, 0, size));
        return std::string (from, end != nullptr ? static_cast<std::size_t> (end - from) : size);
    }

    template <std::size_t N>
    void writeText (char (&into)[N], std::string_view text) noexcept   { writeText (into, N, text); }

    template <std::size_t N>
    std::string readText (const char (&from)[N])                        { return readText (from, N); }

    /*  A SEQUENCE LOCK'S TWO ENDS. The writer makes the count odd, writes,
        makes it even; a reader copies between two reads of an even, equal
        count, or tries again. The writer never waits for a reader. */
    inline void beginWrite (std::atomic<std::uint32_t>& seq) noexcept
    {
        seq.store (seq.load (std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_release);
    }

    inline void endWrite (std::atomic<std::uint32_t>& seq) noexcept
    {
        seq.store (seq.load (std::memory_order_relaxed) + 1, std::memory_order_release);
    }

    template <typename Copy>
    bool readConsistent (const std::atomic<std::uint32_t>& seq, Copy copy, int attempts = 64)
    {
        for (int n = 0; n < attempts; ++n)
        {
            const auto before = seq.load (std::memory_order_acquire);

            if ((before & 1u) != 0)
                continue;

            copy();
            std::atomic_thread_fence (std::memory_order_acquire);

            if (seq.load (std::memory_order_relaxed) == before)
                return true;
        }

        return false;
    }

    //==============================================================================
    /*  THE CLOCK PAIR, the engine's side and the renderer's. */
    inline void writeClock (Region& r, std::int64_t sample, std::int64_t nanos, int sampleRate) noexcept
    {
        beginWrite (r.clock.seq);
        r.clock.sample.store (sample, std::memory_order_relaxed);
        r.clock.nanos.store (nanos, std::memory_order_relaxed);
        r.clock.sampleRate.store (sampleRate, std::memory_order_relaxed);
        endWrite (r.clock.seq);
    }

    struct ClockReading
    {
        std::int64_t sample = -1;
        std::int64_t nanos = 0;
        int sampleRate = 0;
    };

    inline ClockReading readClock (const Region& r)
    {
        ClockReading out;

        readConsistent (r.clock.seq, [&]
        {
            out.sample = r.clock.sample.load (std::memory_order_relaxed);
            out.nanos = r.clock.nanos.load (std::memory_order_relaxed);
            out.sampleRate = r.clock.sampleRate.load (std::memory_order_relaxed);
        });

        return out;
    }

    //==============================================================================
    /*  A LAYER AS THE RENDERER COPIES IT: what it is, its points in the order
        they were placed - the last `pointsPerLayer` of them - and when it goes. */
    struct LayerReading
    {
        std::string id;
        std::string canvas;
        int layer = 0;
        std::uint64_t order = 0;
        Source source = Source::fill;
        std::uint32_t paint = 0;
        std::int64_t removeAt = notRemoved;
        std::uint32_t pointCount = 0;
        Point points[pointsPerLayer];
    };

    /*  False for a free slot, or one the engine was rewriting all the while. */
    inline bool readLayer (const Layer& slot, LayerReading& out)
    {
        bool used = false;

        const auto consistent = readConsistent (slot.seq, [&]
        {
            used = slot.used.load (std::memory_order_relaxed) != 0;

            if (! used)
                return;

            out.id = readText (slot.id);
            out.canvas = readText (slot.canvas);
            out.layer = slot.layer.load (std::memory_order_relaxed);
            out.order = slot.order.load (std::memory_order_relaxed);
            out.source = static_cast<Source> (slot.source.load (std::memory_order_relaxed));
            out.paint = slot.paint.load (std::memory_order_relaxed);
        });

        if (! consistent || ! used)
            return false;

        /*  THE POINTS, AFTER WHAT IT IS: appended under no lock, each one
            whole before the count that publishes it. The last ring's worth,
            oldest first. */
        const auto written = slot.pointsWritten.load (std::memory_order_acquire);
        const auto count = std::min (written, pointsPerLayer);
        const auto first = written - count;

        for (std::uint32_t n = 0; n < count; ++n)
        {
            const auto& point = slot.points[(first + n) & (pointsPerLayer - 1)];
            out.points[n].sample = point.sample.load (std::memory_order_relaxed);
            out.points[n].opacity = point.opacity.load (std::memory_order_relaxed);
        }

        out.pointCount = count;
        out.removeAt = slot.removeAt.load (std::memory_order_acquire);
        return true;
    }

    //==============================================================================
    /*  THE CONFIGURATION AS THE RENDERER COPIES IT. */
    struct CanvasReading
    {
        std::string id;
        int width = 1920;
        int height = 1080;
    };

    struct OutputReading
    {
        std::string id;
        std::string canvas;
        std::string name;
        std::string display;
        std::string displayId;
        bool enabled = true;
        bool testPattern = false;
    };

    struct ConfigReading
    {
        std::uint32_t seq = 0;
        std::vector<CanvasReading> canvases;
        std::vector<OutputReading> outputs;
    };

    inline bool readConfig (const Region& r, ConfigReading& out)
    {
        return readConsistent (r.config.seq, [&]
        {
            out.seq = r.config.seq.load (std::memory_order_relaxed);
            out.canvases.clear();
            out.outputs.clear();

            const auto canvases = std::min<std::uint32_t> (r.config.canvasCount.load (std::memory_order_relaxed),
                                                           maxCanvases);

            for (std::uint32_t n = 0; n < canvases; ++n)
            {
                const auto& c = r.config.canvases[n];
                out.canvases.push_back ({ readText (c.id), c.width.load (std::memory_order_relaxed),
                                          c.height.load (std::memory_order_relaxed) });
            }

            const auto outputs = std::min<std::uint32_t> (r.config.outputCount.load (std::memory_order_relaxed),
                                                          maxOutputs);

            for (std::uint32_t n = 0; n < outputs; ++n)
            {
                const auto& o = r.config.outputs[n];
                out.outputs.push_back ({ readText (o.id), readText (o.canvas), readText (o.name),
                                         readText (o.display), readText (o.displayId),
                                         o.enabled.load (std::memory_order_relaxed) != 0,
                                         o.testPattern.load (std::memory_order_relaxed) != 0 });
            }
        });
    }

    /*  THE ENGINE'S SIDE: the whole configuration, rewritten under the lock. */
    inline void writeConfig (Region& r, const std::vector<CanvasReading>& canvases,
                             const std::vector<OutputReading>& outputs) noexcept
    {
        beginWrite (r.config.seq);

        const auto canvasCount = std::min<std::size_t> (canvases.size(), maxCanvases);

        for (std::size_t n = 0; n < canvasCount; ++n)
        {
            auto& c = r.config.canvases[n];
            writeText (c.id, canvases[n].id);
            c.width.store (canvases[n].width, std::memory_order_relaxed);
            c.height.store (canvases[n].height, std::memory_order_relaxed);
        }

        r.config.canvasCount.store (static_cast<std::uint32_t> (canvasCount), std::memory_order_relaxed);

        const auto outputCount = std::min<std::size_t> (outputs.size(), maxOutputs);

        for (std::size_t n = 0; n < outputCount; ++n)
        {
            auto& o = r.config.outputs[n];
            writeText (o.id, outputs[n].id);
            writeText (o.canvas, outputs[n].canvas);
            writeText (o.name, outputs[n].name);
            writeText (o.display, outputs[n].display);
            writeText (o.displayId, outputs[n].displayId);
            o.enabled.store (outputs[n].enabled ? 1u : 0u, std::memory_order_relaxed);
            o.testPattern.store (outputs[n].testPattern ? 1u : 0u, std::memory_order_relaxed);
        }

        r.config.outputCount.store (static_cast<std::uint32_t> (outputCount), std::memory_order_relaxed);
        endWrite (r.config.seq);
    }
}
