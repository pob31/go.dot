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
#include <wfg/engine/video/SendNames.h>

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

        bool sameInserts (const std::vector<region::InsertReading>& a, const std::vector<region::InsertReading>& b)
        {
            return std::equal (a.begin(), a.end(), b.begin(), b.end(),
                               [] (const region::InsertReading& x, const region::InsertReading& y)
                               {
                                   return x.id == y.id && x.kind == y.kind && x.sendName == y.sendName
                                       && x.returnSender == y.returnSender;
                               });
        }

        bool sameInputs (const std::vector<region::InputReading>& a, const std::vector<region::InputReading>& b)
        {
            return std::equal (a.begin(), a.end(), b.begin(), b.end(),
                               [] (const region::InputReading& x, const region::InputReading& y)
                               {
                                   return x.id == y.id && x.kind == y.kind && x.sender == y.sender && x.enabled == y.enabled;
                               });
        }

        bool sameOutputs (const std::vector<region::OutputReading>& a, const std::vector<region::OutputReading>& b)
        {
            return std::equal (a.begin(), a.end(), b.begin(), b.end(),
                               [] (const region::OutputReading& x, const region::OutputReading& y)
                               {
                                   const auto sameCdl = [] (const Cdl& one, const Cdl& other)
                                   {
                                       for (int n = 0; n < 3; ++n)
                                           if (std::abs (one.slope[n] - other.slope[n]) > 1e-12 || std::abs (one.offset[n] - other.offset[n]) > 1e-12
                                                 || std::abs (one.power[n] - other.power[n]) > 1e-12)
                                               return false;

                                       return std::abs (one.saturation - other.saturation) < 1e-12;
                                   };

                                   const auto sameZones = std::equal (x.zones.begin(), x.zones.end(), y.zones.begin(), y.zones.end(),
                                                                      [] (const region::ZoneReading& one, const region::ZoneReading& other)
                                                                      {
                                                                          return one.canvas == other.canvas && one.blend == other.blend
                                                                              && std::abs (one.opacity - other.opacity) < 1e-9
                                                                              && one.mesh.columns == other.mesh.columns
                                                                              && one.mesh.rows == other.mesh.rows
                                                                              && one.mesh.x == other.mesh.x && one.mesh.y == other.mesh.y;
                                                                      });

                                   return x.id == y.id && x.canvas == y.canvas && x.name == y.name
                                       && x.display == y.display && x.displayId == y.displayId
                                       && x.enabled == y.enabled && x.testPattern == y.testPattern && x.hidden == y.hidden
                                       && x.kind == y.kind && x.sendName == y.sendName
                                       && std::abs (x.frameRate - y.frameRate) < 1e-9
                                       && x.mesh.columns == y.mesh.columns && x.mesh.rows == y.mesh.rows
                                       && x.mesh.x == y.mesh.x && x.mesh.y == y.mesh.y && sameCdl (x.cdl, y.cdl)
                                       && sameZones;
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
            /*  AND WHILE A MONITOR WATCHES (namespace draft §47, AAH): the
                picked cue's tile is drawn by the renderer, with no output
                switched on as with one - no window is made for no output. */
            const auto wanted = wantRenderer.load (std::memory_order_acquire)
                                  || monitoring.load (std::memory_order_acquire);
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

            /*  THE OLD OPENGL RENDERER, asked for by name while the new one
                (namespace draft §44) is being proved on the projectors. */
            if (juce::SystemStats::getEnvironmentVariable ("WFG_VIDEO_RENDERER", {}) == "gl")
                command.push_back ("--renderer=gl");

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

            std::vector<Readouts::InputEntry> foundInputs;

            for (auto& state : r->inputs)
            {
                Readouts::InputEntry entry;

                region::readConsistent (state.seq, [&]
                {
                    entry.id = region::readText (state.inputId);
                    entry.connected = state.connected.load (std::memory_order_relaxed) != 0;
                    entry.width = state.width.load (std::memory_order_relaxed);
                    entry.height = state.height.load (std::memory_order_relaxed);
                    entry.frameRate = static_cast<double> (state.frameRate.load (std::memory_order_relaxed));
                    entry.problem = region::readText (state.problem);
                });

                if (! entry.id.empty())
                    foundInputs.push_back (std::move (entry));
            }

            auto offered = region::readAvailable (*r);

            /*  WHAT IS HELD OF THE READ-AHEAD (§48), and whether it answers the
                list written last. */
            std::vector<region::HeldReading> foundHeld;
            std::uint32_t answers = 0;
            const auto heldRead = region::readHeld (*r, foundHeld, answers);
            const auto heldCurrent = heldRead && answers == r->preparedSeq.load (std::memory_order_acquire);

            std::vector<Readouts::InsertEntry> foundInserts;

            for (auto& state : r->inserts)
            {
                Readouts::InsertEntry entry;

                region::readConsistent (state.seq, [&]
                {
                    entry.id = region::readText (state.insertId);
                    entry.connected = state.connected.load (std::memory_order_relaxed) != 0;
                    entry.frameRate = static_cast<double> (state.frameRate.load (std::memory_order_relaxed));
                    entry.returnAge = static_cast<double> (state.returnAge.load (std::memory_order_relaxed));
                    entry.problem = region::readText (state.problem);
                });

                if (! entry.id.empty())
                    foundInserts.push_back (std::move (entry));
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
                latest.inputs = std::move (foundInputs);
                latest.available = std::move (offered);
                latest.inserts = std::move (foundInserts);
                latest.layerTints = region::readTints (r->layerTints);
                latest.canvasTints = region::readTints (r->canvasTints);

                if (heldRead)
                {
                    latest.held = std::move (foundHeld);
                    latest.heldCurrent = heldCurrent;
                }
            }
            else
            {
                latest.outputs.clear();
                latest.inputs.clear();
                latest.available.clear();
                latest.inserts.clear();
                latest.layerTints.clear();
                latest.canvasTints.clear();
                latest.held.clear();
                latest.heldCurrent = false;
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
        std::vector<region::InputReading> inputs;
        std::vector<region::InsertReading> inserts;
        bool configured = false;
        std::set<std::string> identified;
        bool hideProjectors = false;
        bool locked = false;    // the show's lock, as `configure` last read it

        std::atomic<bool> wantRenderer { false };
        std::atomic<bool> monitoring { false };
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
            void move (const std::string&, Property, const Point&) override {}
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
        impl->locked = document.isLocked();

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
            entry.hidden = impl->hideProjectors && ! impl->locked;

            /*  A SENDER (§44, YA): what it sends under - Go.dot and the
                output's name, when the show names nothing - and how often. */
            entry.kind = region::outputKindFrom (text (base + "kind"));
            entry.sendName = text (base + "sendName");

            entry.sendName = outputSendName (entry.sendName, entry.name, id);

            if (const auto rate = osc::parseDouble (text (base + "frameRate")); rate.has_value() && *rate > 0.0)
                entry.frameRate = std::clamp (*rate, 1.0, 240.0);

            /*  ITS MAPPING: the mesh as the show says it - a grid the wrong
                size for its columns and rows is the identity - and the CDL. */
            const auto numbers = [&text] (const std::string& address)
            {
                std::vector<double> out;

                for (const auto& word : juce::StringArray::fromTokens (juce::String (text (address)), " ", ""))
                    if (const auto value = osc::parseDouble (word.toStdString()); value.has_value())
                        out.push_back (*value);

                return out;
            };

            entry.mesh.columns = std::clamp (whole (base + "meshColumns", 2), 2, Mesh::maxPoints);
            entry.mesh.rows = std::clamp (whole (base + "meshRows", 2), 2, Mesh::maxPoints);

            const auto points = numbers (base + "mesh");

            if (points.size() == static_cast<std::size_t> (2 * entry.mesh.columns * entry.mesh.rows))
                for (std::size_t n = 0; n + 1 < points.size(); n += 2)
                {
                    entry.mesh.x.push_back (static_cast<float> (points[n]));
                    entry.mesh.y.push_back (static_cast<float> (points[n + 1]));
                }
            else
                entry.mesh = Mesh::identity (entry.mesh.columns, entry.mesh.rows);

            entry.cdl = Cdl::from (numbers (base + "cdl"));

            /*  ITS ZONES (namespace draft 40, WY), bottom first: each a further
                canvas through its own warp, by its blend and its opacity. */
            for (const auto& child : output)
            {
                const auto zoneId = child["id"].toString().toStdString();

                if (! child.hasType ("Zone") || zoneId.empty())
                    continue;

                const auto at = "/godot/zone/" + zoneId + "/";
                region::ZoneReading zone;
                zone.canvas = text (at + "canvas");
                zone.blend = region::blendFrom (text (at + "blend"));
                zone.opacity = std::clamp (osc::parseDouble (text (at + "opacity")).value_or (100.0) / 100.0, 0.0, 1.0);
                zone.mesh.columns = std::clamp (whole (at + "meshColumns", 2), 2, Mesh::maxPoints);
                zone.mesh.rows = std::clamp (whole (at + "meshRows", 2), 2, Mesh::maxPoints);

                const auto zonePoints = numbers (at + "mesh");

                if (zonePoints.size() == static_cast<std::size_t> (2 * zone.mesh.columns * zone.mesh.rows))
                    for (std::size_t n = 0; n + 1 < zonePoints.size(); n += 2)
                    {
                        zone.mesh.x.push_back (static_cast<float> (zonePoints[n]));
                        zone.mesh.y.push_back (static_cast<float> (zonePoints[n + 1]));
                    }
                else
                    zone.mesh = Mesh::identity (zone.mesh.columns, zone.mesh.rows);

                entry.zones.push_back (std::move (zone));
            }

            anyEnabled = anyEnabled || entry.enabled;
            outputs.push_back (std::move (entry));
        }

        /*  THE VIDEO INPUTS (namespace draft §44, YB): what each takes in and
            from whom. One taken in keeps the renderer running, as an output
            does - it is what finds what other programs offer, too. */
        std::vector<region::InputReading> inputs;

        for (const auto& input : root.getChildWithName ("VideoInputs"))
        {
            const auto id = input["id"].toString().toStdString();

            if (id.empty())
                continue;

            const auto base = "/godot/videoInput/" + id + "/";
            region::InputReading entry;
            entry.id = id;
            entry.kind = region::outputKindFrom (text (base + "kind"));

            //  An unknown word is NDI, the default, never a display.
            if (entry.kind == region::OutputKind::display)
                entry.kind = region::OutputKind::ndi;

            entry.sender = text (base + "sender");
            entry.enabled = text (base + "enabled") != "false";
            anyEnabled = anyEnabled || entry.enabled;
            inputs.push_back (std::move (entry));
        }

        /*  THE VIDEO INSERTS (§44, YE): how each sends and takes back, under
            which names - the send name Go.dot - insert and the insert's name
            when the show names none. */
        std::vector<region::InsertReading> inserts;

        for (const auto& insert : root.getChildWithName ("VideoInserts"))
        {
            const auto id = insert["id"].toString().toStdString();

            if (id.empty())
                continue;

            const auto base = "/godot/videoInsert/" + id + "/";
            region::InsertReading entry;
            entry.id = id;
            entry.kind = region::outputKindFrom (text (base + "kind"));

            if (entry.kind == region::OutputKind::display)
                entry.kind = region::OutputKind::spout;

            entry.sendName = insertSendName (text (base + "sendName"), text (base + "name"), id);
            entry.returnSender = text (base + "returnSender");
            inserts.push_back (std::move (entry));

            /*  AND IT KEEPS THE RENDERER RUNNING (§47, AAK), as an input does:
                its send is up from the moment it is declared, so the other
                program can be patched to it before any cue goes through. */
            anyEnabled = true;
        }

        if (! impl->configured || ! sameCanvases (canvases, impl->canvases) || ! sameOutputs (outputs, impl->outputs)
              || ! sameInputs (inputs, impl->inputs) || ! sameInserts (inserts, impl->inserts))
        {
            region::writeConfig (*impl->r, canvases, outputs, inputs, inserts);
            impl->canvases = std::move (canvases);
            impl->outputs = std::move (outputs);
            impl->inputs = std::move (inputs);
            impl->inserts = std::move (inserts);
            impl->configured = true;
        }

        impl->wantRenderer.store (anyEnabled, std::memory_order_release);
    }

    void VideoHost::setMonitoring (bool wanted) noexcept
    {
        impl->monitoring.store (wanted, std::memory_order_release);

        if (impl->r != nullptr)
            impl->r->previewWanted.store (wanted ? 1u : 0u, std::memory_order_release);
    }

    void VideoHost::showTile (const LayerSpec& spec, double seconds, double opacity)
    {
        if (impl->r == nullptr)
            return;

        auto& slot = impl->r->tileLayer;

        region::beginWrite (slot.seq);
        writeLayerSpec (slot, spec);

        for (auto& ring : slot.rings)
            ring.written.store (0, std::memory_order_relaxed);

        //  ONE POINT EACH: its opacity, and a movie's second - held from there on.
        const auto put = [&slot] (Property property, double value)
        {
            auto& ring = slot.rings[static_cast<std::size_t> (property)];
            ring.points[0].sample.store (0, std::memory_order_relaxed);
            ring.points[0].value.store (value, std::memory_order_relaxed);
            ring.written.store (1, std::memory_order_relaxed);
        };

        put (Property::opacity, std::clamp (opacity, 0.0, 1.0));
        put (Property::time, std::max (0.0, seconds));
        slot.removeAt.store (region::notRemoved, std::memory_order_relaxed);
        region::endWrite (slot.seq);

        impl->r->tileSerial.fetch_add (1, std::memory_order_acq_rel);
        impl->r->tileWanted.store (1, std::memory_order_release);
    }

    void VideoHost::hideTile()
    {
        if (impl->r != nullptr)
            impl->r->tileWanted.store (0, std::memory_order_release);
    }

    VideoHost::CueTile VideoHost::cueTile() const
    {
        CueTile out;

        if (impl->r == nullptr || impl->r->tileWanted.load (std::memory_order_acquire) == 0)
            return out;

        const auto& tile = impl->r->tile;

        const auto read = region::readConsistent (tile.seq, [&]
        {
            out.serial = tile.serial.load (std::memory_order_relaxed);
            out.width = std::clamp (static_cast<int> (tile.width.load (std::memory_order_relaxed)), 0, region::tileWidth);
            out.height = std::clamp (static_cast<int> (tile.height.load (std::memory_order_relaxed)), 0, region::tileHeight);
            out.rgb.assign (tile.rgb, tile.rgb + 3 * out.width * out.height);
        });

        if (! read)
            return {};

        return out;
    }

    std::vector<VideoHost::CanvasPicture> VideoHost::canvasPictures() const
    {
        std::vector<CanvasPicture> out;

        if (impl->r == nullptr)
            return out;

        for (const auto& slot : impl->r->previews)
        {
            CanvasPicture picture;

            const auto read = region::readConsistent (slot.seq, [&]
            {
                picture.canvasId = region::readText (slot.id);
                picture.width = std::clamp (static_cast<int> (slot.width.load (std::memory_order_relaxed)), 0, region::previewWidth);
                picture.height = std::clamp (static_cast<int> (slot.height.load (std::memory_order_relaxed)), 0, region::previewHeight);
                picture.sample = slot.sample.load (std::memory_order_relaxed);
                picture.rgb.assign (slot.rgb, slot.rgb + 3 * picture.width * picture.height);
            });

            if (read && ! picture.canvasId.empty() && picture.width > 0 && picture.height > 0)
                out.push_back (std::move (picture));
        }

        return out;
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

        region::writeConfig (*impl->r, impl->canvases, impl->outputs, impl->inputs, impl->inserts);
    }

    bool VideoHost::identifying (const std::string& outputId) const
    {
        return impl->identified.count (outputId) > 0;
    }

    void VideoHost::hideProjectors (bool on)
    {
        impl->hideProjectors = on;

        if (impl->r == nullptr || ! impl->configured)
            return;

        //  At once, as the pattern is; the lock overrules it (§39).
        for (auto& output : impl->outputs)
            output.hidden = on && ! impl->locked;

        region::writeConfig (*impl->r, impl->canvases, impl->outputs, impl->inputs, impl->inserts);
    }

    bool VideoHost::projectorsHidden() const noexcept
    {
        return impl->hideProjectors;
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
