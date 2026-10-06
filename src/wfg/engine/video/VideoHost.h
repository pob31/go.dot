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
    THE ENGINE'S END OF THE RENDERER (Phase 8a, namespace draft 35.4, decisions
    UV and VC).

    It makes the region and holds it for the engine's whole life - the scene
    lives there, not in the renderer - and it keeps a renderer running while
    the show has an output: started on a thread of its own, never the tick
    thread, whose GO must not wait for a process to be made (PRD §4.1); watched
    by its heartbeat; started again, a little later each time it fails, when
    it stops answering or goes away. A renderer started again reads the same
    region and draws again what was up.

    THE TICK THREAD'S PART is three stores into the region and nothing that
    waits: the clock pair, the configuration when the show's canvases or
    outputs changed, and the layers, through `sink()`.

    WHAT THE RENDERER FOUND - the displays, which outputs are bound, their
    frames - is copied out by the host's thread, and `readouts()` hands a copy
    to whoever asks: the parameter tree, on the tick thread, never waiting on
    the host's thread for longer than that copy takes.
*/

#include <wfg/engine/video/VideoRegion.h>
#include <wfg/engine/video/VideoSink.h>

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace wfg::doc { class ShowDocument; }

namespace wfg::video
{
    struct HostSpec
    {
        /** Where the region's file is made. */
        std::string workFolder;

        /** What to run, empty for this program, and the words before its own. */
        std::string executable;
        std::vector<std::string> leadingArgs { "video-render" };

        /** The renderer with no window: CI's, and a machine with no screen. */
        bool headless = false;
    };

    struct Readouts
    {
        /** stopped, starting, running or failed (`videoOutputs/renderer`). */
        std::string renderer = "stopped";
        std::string rendererProblem;

        struct DisplayEntry
        {
            std::string name;
            std::string id;
        };

        std::vector<DisplayEntry> displays;

        struct OutputEntry
        {
            std::string id;
            bool bound = false;
            std::string problem;
            std::uint64_t framesPresented = 0;
            std::uint64_t framesLate = 0;
            double jitterMs = 0.0;
        };

        std::vector<OutputEntry> outputs;

        const OutputEntry* output (const std::string& id) const noexcept
        {
            for (const auto& entry : outputs)
                if (entry.id == id)
                    return &entry;

            return nullptr;
        }
    };

    class VideoHost
    {
    public:
        explicit VideoHost (HostSpec spec);
        ~VideoHost();

        VideoHost (const VideoHost&) = delete;
        VideoHost& operator= (const VideoHost&) = delete;

        /** Whether the region was made; false says why in `readouts()`. */
        bool isOpen() const noexcept;

        /** The Runner's picture side. Tick thread. */
        Sink& sink() noexcept;

        /*  The clock pair - Go.dot's sample now and the machine's steady time -
            and the layers whose removal has passed let go. Tick thread, once a
            tick; three stores and a loop over sixty-four slots. */
        void tick (std::int64_t sampleNow, int sampleRate) noexcept;

        /*  THE SHOW'S CANVASES AND OUTPUTS, read off the document and written
            into the region only when they changed. Tick thread, when the show
            changed. A renderer is kept running while the show has an output
            switched on. */
        void configure (const doc::ShowDocument& document);

        /*  AN OUTPUT'S TEST PATTERN, on or off (`videoOutput.identify`):
            tonight's, held here and never in the show. Tick thread. */
        void identify (const std::string& outputId, bool on);
        bool identifying (const std::string& outputId) const;

        /** What the renderer found, as of the host thread's last look. Any thread. */
        Readouts readouts() const;

        /** The region itself, for a test that reads what the renderer wrote. */
        region::Region* regionForTests() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
