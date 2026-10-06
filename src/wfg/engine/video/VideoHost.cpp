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

#include <wfg/engine/video/VideoHost.h>

#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/plugin/ChildLaunch.h>
#include <wfg/engine/plugin/ProcessUtil.h>
#include <wfg/engine/video/RegionSink.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <new>
#include <thread>
#include <utility>

namespace wfg::video
{
    namespace
    {
        std::int64_t steadyNanos() noexcept
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds> (
                       std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        bool sameCanvases (const std::vector<region::CanvasReading>& a, const std::vector<region::CanvasReading>& b)
        {
            return std::equal (a.begin(), a.end(), b.begin(), b.end(),
                               [] (const region::CanvasReading& x, const region::CanvasReading& y)
                               { return x.id == y.id && x.width == y.width && x.height == y.height; });
        }

        bool sameOutputs (const std::vector<region::OutputReading>& a, const std::vector<region::OutputReading>& b)
        {
            return std::equal (a.begin(), a.end(), b.begin(), b.end(),
                               [] (const region::OutputReading& x, const region::OutputReading& y)
                               {
                                   return x.id == y.id && x.canvas == y.canvas && x.name == y.name
                                       && x.display == y.display && x.displayId == y.displayId
                                       && x.enabled == y.enabled && x.testPattern == y.testPattern;
                               });
        }
    }

    struct VideoHost::Impl
    {
        explicit Impl (HostSpec specToUse) : spec (std::move (specToUse))
        {
            open = makeRegion();

            if (open)
                host = std::thread ([this] { watch(); });
        }

        ~Impl()
        {
            stopping.store (true, std::memory_order_release);

            if (host.joinable())
                host.join();

            stopChild();
            sink.reset();
            r = nullptr;
            mapping.reset();
            regionFile.deleteFile();
        }

        //==============================================================================
        bool makeRegion()
        {
            static std::atomic<int> opened { 0 };

            const juce::File folder { juce::String::fromUTF8 (spec.workFolder.c_str()) };
            folder.createDirectory();

            regionFile = folder.getChildFile ("video-" + juce::String (plugin::process::currentId()) + "-"
                                              + juce::String (++opened) + ".shm");

            const auto bytes = region::regionBytes();

            {
                juce::FileOutputStream out (regionFile);

                if (! out.openedOk())
                {
                    setStatus ("failed", "could not create " + regionFile.getFullPathName().toStdString());
                    return false;
                }

                out.setPosition (0);
                out.truncate();
                const std::vector<char> zeros (bytes, 0);
                out.write (zeros.data(), zeros.size());
            }

            mapping = std::make_unique<juce::MemoryMappedFile> (regionFile, juce::MemoryMappedFile::readWrite, false);

            if (mapping->getData() == nullptr || mapping->getSize() < bytes)
            {
                setStatus ("failed", "could not map " + regionFile.getFullPathName().toStdString());
                mapping.reset();
                regionFile.deleteFile();
                return false;
            }

            r = new (mapping->getData()) region::Region {};
            region::stamp (*r);
            sink = std::make_unique<RegionSink> (*r);
            return true;
        }

        //==============================================================================
        /*  THE HOST'S THREAD, twenty times a second: a renderer kept running
            while one is wanted, and what it found copied out. */
        void watch()
        {
            while (! stopping.load (std::memory_order_acquire))
            {
                manage();
                copyReadouts();
                std::this_thread::sleep_for (std::chrono::milliseconds (50));
            }
        }

        void manage()
        {
            const auto now = std::chrono::steady_clock::now();
            const auto wanted = wantRenderer.load (std::memory_order_acquire);
            const auto running = child != nullptr && child->isRunning();

            if (! wanted)
            {
                if (child != nullptr)
                    stopChild();

                setStatus ("stopped", {});
                failures = 0;
                return;
            }

            if (! running)
            {
                /*  GONE WITHOUT BEING ASKED: a crash, or a driver that took it
                    down. Said, and started again a little later each time -
                    half a second, then one, two, four, at most ten. */
                if (child != nullptr)
                {
                    child.reset();
                    ++failures;
                    setStatus ("failed", r->failed.load (std::memory_order_acquire) != 0
                                           ? region::readText (r->problem)
                                           : std::string ("the renderer stopped; starting it again"));
                    nextLaunch = now + backoff();
                }

                if (now >= nextLaunch)
                    launch();

                return;
            }

            if (r->ready.load (std::memory_order_acquire) != 0)
            {
                /*  ALIVE IS A HEARTBEAT THAT MOVES. One that has not for three
                    seconds is a renderer hung inside a driver: ended, and
                    started again. */
                const auto beat = r->heartbeat.load (std::memory_order_acquire);

                if (beat != lastBeat)
                {
                    lastBeat = beat;
                    lastBeatAt = now;
                    failures = 0;
                    setStatus ("running", {});
                }
                else if (now - lastBeatAt > std::chrono::seconds (3))
                {
                    child->kill();
                    child.reset();
                    ++failures;
                    setStatus ("failed", "the renderer stopped answering; starting it again");
                    nextLaunch = now + backoff();
                }
            }
            else if (now - launchedAt > std::chrono::seconds (15))
            {
                child->kill();
                child.reset();
                ++failures;
                setStatus ("failed", "the renderer did not come up; starting it again");
                nextLaunch = now + backoff();
            }
        }

        std::chrono::milliseconds backoff() const
        {
            const auto steps = std::min (failures, 5);
            return std::chrono::milliseconds (std::min (10000, 500 << std::max (0, steps - 1)));
        }

        void launch()
        {
            r->ready.store (0, std::memory_order_relaxed);
            r->failed.store (0, std::memory_order_relaxed);
            r->shouldExit.store (0, std::memory_order_release);

            std::vector<std::string> command;
            command.push_back (spec.executable.empty()
                                   ? juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                                         .getFullPathName().toStdString()
                                   : spec.executable);

            for (const auto& word : spec.leadingArgs)
                command.push_back (word);

            command.push_back ("--region=" + regionFile.getFullPathName().toStdString());
            command.push_back ("--parent-pid=" + std::to_string (plugin::process::currentId()));

            if (spec.headless)
                command.push_back ("--no-window");

            child = std::make_unique<plugin::ChildLaunch>();

            if (! child->start (command))
            {
                child.reset();
                ++failures;
                setStatus ("failed", "could not start the renderer " + command.front());
                nextLaunch = std::chrono::steady_clock::now() + backoff();
                return;
            }

            launchedAt = std::chrono::steady_clock::now();
            lastBeat = r->heartbeat.load (std::memory_order_acquire);
            lastBeatAt = launchedAt;
            setStatus ("starting", {});
        }

        void stopChild()
        {
            if (child == nullptr)
                return;

            if (r != nullptr)
                r->shouldExit.store (1, std::memory_order_release);

            for (int waited = 0; waited < 25 && child->isRunning(); ++waited)
                std::this_thread::sleep_for (std::chrono::milliseconds (20));

            if (child->isRunning())
                child->kill();

            child.reset();
        }

        //==============================================================================
        void setStatus (std::string renderer, std::string problem)
        {
            const std::lock_guard<std::mutex> hold (lock);
            latest.renderer = std::move (renderer);
            latest.rendererProblem = std::move (problem);
        }

        /*  WHAT THE RENDERER WROTE, copied under each block's sequence. */
        void copyReadouts()
        {
            std::vector<Readouts::DisplayEntry> foundDisplays;

            region::readConsistent (r->displaysSeq, [&]
            {
                foundDisplays.clear();
                const auto count = std::min<std::uint32_t> (r->displayCount.load (std::memory_order_relaxed),
                                                            region::maxDisplays);

                for (std::uint32_t n = 0; n < count; ++n)
                    foundDisplays.push_back ({ region::readText (r->displays[n].name), region::readText (r->displays[n].id) });
            });

            std::vector<Readouts::OutputEntry> foundOutputs;

            for (auto& state : r->outputs)
            {
                Readouts::OutputEntry entry;

                region::readConsistent (state.seq, [&]
                {
                    entry.id = region::readText (state.outputId);
                    entry.bound = state.bound.load (std::memory_order_relaxed) != 0;
                    entry.problem = region::readText (state.problem);
                });

                if (entry.id.empty())
                    continue;

                entry.framesPresented = state.framesPresented.load (std::memory_order_relaxed);
                entry.framesLate = state.framesLate.load (std::memory_order_relaxed);
                entry.jitterMs = static_cast<double> (state.jitterMs.load (std::memory_order_relaxed));
                foundOutputs.push_back (std::move (entry));
            }

            /*  A RENDERER NOT RUNNING HAS FOUND NOTHING: its last words are not
                tonight's. */
            const auto running = child != nullptr && child->isRunning()
                                   && r->ready.load (std::memory_order_acquire) != 0;

            const std::lock_guard<std::mutex> hold (lock);

            if (running)
            {
                latest.displays = std::move (foundDisplays);
                latest.outputs = std::move (foundOutputs);
            }
            else
            {
                latest.outputs.clear();
            }
        }

        //==============================================================================
        HostSpec spec;
        bool open = false;

        juce::File regionFile;
        std::unique_ptr<juce::MemoryMappedFile> mapping;
        region::Region* r = nullptr;
        std::unique_ptr<RegionSink> sink;

        std::vector<region::CanvasReading> canvases;
        std::vector<region::OutputReading> outputs;
        bool configured = false;
        std::set<std::string> identified;

        std::atomic<bool> wantRenderer { false };
        std::atomic<bool> stopping { false };
        std::thread host;

        std::unique_ptr<plugin::ChildLaunch> child;
        int failures = 0;
        std::chrono::steady_clock::time_point nextLaunch {};
        std::chrono::steady_clock::time_point launchedAt {};
        std::chrono::steady_clock::time_point lastBeatAt {};
        std::uint64_t lastBeat = 0;

        mutable std::mutex lock;
        Readouts latest;
    };

    //==============================================================================
    VideoHost::VideoHost (HostSpec spec) : impl (std::make_unique<Impl> (std::move (spec))) {}
    VideoHost::~VideoHost() = default;

    bool VideoHost::isOpen() const noexcept
    {
        return impl->open;
    }

    Sink& VideoHost::sink() noexcept
    {
        /*  A host whose region could not be made still answers, with a sink
            that writes nowhere: the show runs with no picture and says why. */
        struct Nowhere final : Sink
        {
            void show (const LayerSpec&) override {}
            void opacity (const std::string&, const Point&) override {}
            void remove (const std::string&, std::int64_t) override {}
            void clear() override {}
        };

        static Nowhere nowhere;

        if (impl->sink == nullptr)
            return nowhere;

        return *impl->sink;
    }

    void VideoHost::tick (std::int64_t sampleNow, int sampleRate) noexcept
    {
        if (impl->r == nullptr || sampleNow < 0)
            return;

        region::writeClock (*impl->r, sampleNow, steadyNanos(), sampleRate);

        /*  A SLOT IS LET GO a second after its removal was due: long enough
            for a renderer a frame or two behind to have drawn it gone. */
        impl->sink->release (sampleNow, std::max (sampleRate, 1));
    }

    void VideoHost::configure (const doc::ShowDocument& document)
    {
        if (impl->r == nullptr)
            return;

        std::vector<region::CanvasReading> canvases;
        std::vector<region::OutputReading> outputs;

        const auto text = [&document] (const std::string& address)
        {
            return document.getAttribute (address).value_or (std::string {});
        };

        const auto whole = [&text] (const std::string& address, int fallback)
        {
            const auto value = osc::parseDouble (text (address));
            return value.has_value() ? static_cast<int> (*value) : fallback;
        };

        const auto root = document.root();

        for (const auto& canvas : root.getChildWithName ("Canvases"))
        {
            const auto id = canvas["id"].toString().toStdString();

            if (id.empty())
                continue;

            const auto base = "/godot/canvas/" + id + "/";
            canvases.push_back ({ id, whole (base + "width", 1920), whole (base + "height", 1080) });
        }

        bool anyEnabled = false;

        for (const auto& output : root.getChildWithName ("VideoOutputs"))
        {
            const auto id = output["id"].toString().toStdString();

            if (id.empty())
                continue;

            const auto base = "/godot/videoOutput/" + id + "/";
            region::OutputReading entry;
            entry.id = id;
            entry.canvas = text (base + "canvas");
            entry.name = text (base + "name");
            entry.display = text (base + "display");
            entry.displayId = text (base + "displayId");
            entry.enabled = text (base + "enabled") != "false";
            entry.testPattern = impl->identified.count (id) > 0;

            anyEnabled = anyEnabled || entry.enabled;
            outputs.push_back (std::move (entry));
        }

        if (! impl->configured || ! sameCanvases (canvases, impl->canvases) || ! sameOutputs (outputs, impl->outputs))
        {
            region::writeConfig (*impl->r, canvases, outputs);
            impl->canvases = std::move (canvases);
            impl->outputs = std::move (outputs);
            impl->configured = true;
        }

        impl->wantRenderer.store (anyEnabled, std::memory_order_release);
    }

    void VideoHost::identify (const std::string& outputId, bool on)
    {
        if (on)
            impl->identified.insert (outputId);
        else
            impl->identified.erase (outputId);

        if (impl->r == nullptr || ! impl->configured)
            return;

        /*  THE CONFIGURATION AS IT STANDS, with the pattern switched: written
            at once, so the wall shows it on the next frame. */
        for (auto& output : impl->outputs)
            output.testPattern = impl->identified.count (output.id) > 0;

        region::writeConfig (*impl->r, impl->canvases, impl->outputs);
    }

    bool VideoHost::identifying (const std::string& outputId) const
    {
        return impl->identified.count (outputId) > 0;
    }

    Readouts VideoHost::readouts() const
    {
        const std::lock_guard<std::mutex> hold (impl->lock);
        return impl->latest;
    }

    region::Region* VideoHost::regionForTests() noexcept
    {
        return impl->r;
    }
}
