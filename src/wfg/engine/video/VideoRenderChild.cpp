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

#include <wfg/engine/video/VideoRenderChild.h>

#include <wfg/engine/plugin/ProcessUtil.h>
#include <wfg/engine/video/Compositor.h>
#include <wfg/engine/video/Displays.h>
#include <wfg/engine/video/VideoClock.h>
#include <wfg/engine/video/VideoRegion.h>

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_opengl/juce_opengl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#if JUCE_MAC
/*  Declared here as PluginEditorChild.cpp declares it, and for its reason:
    runDispatchLoop is [NSApp run], and there is no NSApp unless it is made. */
namespace juce { void initialiseNSApplication(); }
#endif

namespace wfg::video
{
    namespace
    {
        std::string optionFrom (const std::vector<std::string>& args, const char* name)
        {
            const std::string prefix = std::string (name) + "=";

            for (const auto& arg : args)
                if (arg.rfind (prefix, 0) == 0)
                    return arg.substr (prefix.size());

            return {};
        }

        bool hasFlag (const std::vector<std::string>& args, const char* flag)
        {
            return std::find (args.begin(), args.end(), std::string (flag)) != args.end();
        }

        std::int64_t steadyNanos() noexcept
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds> (
                       std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        /*  EVERY LAYER THE ENGINE HOLDS, copied out of the region: what a
            frame is drawn from. */
        std::vector<region::LayerReading> readLayers (const region::Region& r)
        {
            std::vector<region::LayerReading> out;

            for (const auto& slot : r.layers)
            {
                if (slot.used.load (std::memory_order_acquire) == 0)
                    continue;

                region::LayerReading layer;

                if (region::readLayer (slot, layer))
                    out.push_back (std::move (layer));
            }

            return out;
        }

        /*  THE CLOCK AS ONE READER SEES IT: fed from the region whenever the
            engine has written a new pair, read at whatever moment is asked. */
        struct ClockReader
        {
            std::int64_t sampleAt (const region::Region& r, std::int64_t nanos)
            {
                const auto pair = region::readClock (r);

                if (pair.nanos != lastNanos)
                {
                    estimate.feed (pair.sample, pair.nanos, pair.sampleRate);
                    lastNanos = pair.nanos;
                }

                return estimate.sampleAt (nanos);
            }

            ClockEstimate estimate;
            std::int64_t lastNanos = 0;
        };

        //==============================================================================
        /*  ONE OUTPUT: a borderless window covering its display, drawn by
            OpenGL every vsync. */
        class OutputWindow final : public juce::Component, private juce::OpenGLRenderer
        {
        public:
            OutputWindow (region::Region& regionToRead, std::string outputIdToShow, int stateSlot,
                          const DisplayInfo& display)
                : r (regionToRead), outputId (std::move (outputIdToShow)), slot (stateSlot),
                  periodNanos (1.0e9 / std::max (24.0, static_cast<double> (display.refreshHz)))
            {
                setOpaque (true);
                setWantsKeyboardFocus (false);
                setMouseCursor (juce::MouseCursor::NoCursor);
                setBounds (display.x, display.y, display.width, display.height);

                context.setRenderer (this);
                context.setOpenGLVersionRequired (juce::OpenGLContext::openGL3_2);
                context.setComponentPaintingEnabled (false);
                context.setContinuousRepainting (true);
                context.setSwapInterval (1);
                context.attachTo (*this);

                /*  NEVER THE FOCUS, never a button on the task bar: the show is
                    run from Go.dot's own window, and a projector's window that
                    took a key press would take the GO it was meant for. */
                addToDesktop (juce::ComponentPeer::windowIsTemporary
                                | juce::ComponentPeer::windowIgnoresKeyPresses);
                setAlwaysOnTop (true);
                setVisible (true);
            }

            ~OutputWindow() override
            {
                context.detach();
            }

            void paint (juce::Graphics& g) override
            {
                g.fillAll (juce::Colours::black);
            }

        private:
            void newOpenGLContextCreated() override
            {
                using namespace juce::gl;

                program = std::make_unique<juce::OpenGLShaderProgram> (context);

                const auto ok = program->addVertexShader (juce::OpenGLHelpers::translateVertexShaderToV3 (
                                    "attribute vec2 position;\n"
                                    "void main() { gl_Position = vec4 (position, 0.0, 1.0); }\n"))
                             && program->addFragmentShader (juce::OpenGLHelpers::translateFragmentShaderToV3 (
                                    "uniform vec4 colour;\n"
                                    "void main() { gl_FragColor = colour; }\n"))
                             && program->link();

                if (! ok)
                {
                    program.reset();
                    return;
                }

                colour = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*program, "colour");
                position = juce::OpenGLShaderProgram::Attribute (*program, "position").attributeID;

                /*  THE WHOLE CANVAS AS TWO TRIANGLES: a fill covers all of it. */
                const GLfloat corners[] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };

                glGenVertexArrays (1, &vertexArray);
                glBindVertexArray (vertexArray);
                glGenBuffers (1, &vertexBuffer);
                glBindBuffer (GL_ARRAY_BUFFER, vertexBuffer);
                glBufferData (GL_ARRAY_BUFFER, sizeof (corners), corners, GL_STATIC_DRAW);
                glEnableVertexAttribArray (position);
                glVertexAttribPointer (position, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
                glBindVertexArray (0);
            }

            void renderOpenGL() override
            {
                using namespace juce::gl;

                const auto began = steadyNanos();
                countFrame (began);

                /*  THE SAMPLE THIS FRAME IS SEEN AT: the next vsync, a frame
                    period on. */
                const auto sample = clock.sampleAt (r, began + static_cast<std::int64_t> (periodNanos));

                const auto scale = context.getRenderingScale();
                const auto width = juce::roundToInt (getWidth() * scale);
                const auto height = juce::roundToInt (getHeight() * scale);

                glViewport (0, 0, width, height);
                glDisable (GL_SCISSOR_TEST);
                glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
                glClear (GL_COLOR_BUFFER_BIT);

                region::ConfigReading config;
                region::readConfig (r, config);

                std::string canvas;
                bool testPattern = false;

                for (const auto& output : config.outputs)
                    if (output.id == outputId)
                    {
                        canvas = output.canvas;
                        testPattern = output.testPattern;
                    }

                if (program != nullptr && sample >= 0 && ! canvas.empty())
                {
                    const auto layers = readLayers (r);
                    const auto stack = stackOf (layers, canvas);

                    glEnable (GL_BLEND);
                    glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                    program->use();
                    glBindVertexArray (vertexArray);

                    /*  THE COMPOSITOR'S ORDER AND ITS BLEND (Compositor.h), on the
                        GPU: bottom first, each laid over what is under it. */
                    for (const auto* layer : stack)
                    {
                        if (layer->source != region::Source::fill)
                            continue;

                        const auto a = opacityOf (*layer, sample);

                        if (! (a > 0.0))
                            continue;

                        colour->set (static_cast<GLfloat> ((layer->paint >> 16) & 0xffu) / 255.0f,
                                     static_cast<GLfloat> ((layer->paint >> 8) & 0xffu) / 255.0f,
                                     static_cast<GLfloat> (layer->paint & 0xffu) / 255.0f,
                                     static_cast<GLfloat> (a));
                        glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
                    }

                    glBindVertexArray (0);
                    glDisable (GL_BLEND);
                }

                if (testPattern)
                    drawTestPattern (width, height);
            }

            void openGLContextClosing() override
            {
                using namespace juce::gl;

                if (vertexBuffer != 0)
                    glDeleteBuffers (1, &vertexBuffer);

                if (vertexArray != 0)
                    glDeleteVertexArrays (1, &vertexArray);

                vertexBuffer = 0;
                vertexArray = 0;
                colour.reset();
                program.reset();
            }

            /*  WHICH PROJECTOR IS WHICH: a white frame round the edge and a
                cross through the middle, drawn by clearing thin rectangles -
                no text, no geometry. The output's name is said on the Video
                tab beside the switch that turns this on. */
            static void drawTestPattern (int width, int height)
            {
                using namespace juce::gl;

                const auto line = std::max (2, std::min (width, height) / 200);
                const auto bar = [] (int x, int y, int w, int h)
                {
                    glScissor (x, y, w, h);
                    glClear (GL_COLOR_BUFFER_BIT);
                };

                glEnable (GL_SCISSOR_TEST);
                glClearColor (1.0f, 1.0f, 1.0f, 1.0f);
                bar (0, 0, width, line);
                bar (0, height - line, width, line);
                bar (0, 0, line, height);
                bar (width - line, 0, line, height);
                bar (0, height / 2 - line / 2, width, line);
                bar (width / 2 - line / 2, 0, line, height);
                glDisable (GL_SCISSOR_TEST);
            }

            /*  THE OUTPUT'S FRAMES AS NUMBERS (§35.7): every frame counted, a
                frame begun more than half a period late counted late, and how
                far each strays from its period, smoothed. */
            void countFrame (std::int64_t began)
            {
                auto& state = r.outputs[static_cast<std::size_t> (slot)];

                if (lastFrame > 0)
                {
                    const auto interval = static_cast<double> (began - lastFrame);

                    if (interval > 1.5 * periodNanos)
                        state.framesLate.fetch_add (1, std::memory_order_relaxed);

                    jitterMs = jitterMs * 0.95 + 0.05 * std::abs (interval - periodNanos) * 1.0e-6;
                    state.jitterMs.store (static_cast<float> (jitterMs), std::memory_order_relaxed);
                }

                lastFrame = began;
                state.framesPresented.fetch_add (1, std::memory_order_relaxed);
            }

            region::Region& r;
            std::string outputId;
            int slot = 0;
            double periodNanos = 1.0e9 / 60.0;

            juce::OpenGLContext context;
            std::unique_ptr<juce::OpenGLShaderProgram> program;
            std::unique_ptr<juce::OpenGLShaderProgram::Uniform> colour;
            GLuint position = 0;
            GLuint vertexArray = 0;
            GLuint vertexBuffer = 0;

            ClockReader clock;
            std::int64_t lastFrame = 0;
            double jitterMs = 0.0;

            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OutputWindow)
        };

        //==============================================================================
        /*  THE RENDERER'S MESSAGE THREAD, a hundred times a second: alive, the
            parent still there, the displays and the configuration followed,
            and - with no window - the probes. */
        class Session final : private juce::Timer
        {
        public:
            Session (region::Region& regionToServe, std::int64_t parentPidToWatch, bool windowedToUse)
                : r (regionToServe), parentPid (parentPidToWatch), windowed (windowedToUse)
            {
                followDisplays (true);
                startTimer (10);
            }

            ~Session() override
            {
                stopTimer();
                windows.clear();
            }

        private:
            void timerCallback() override
            {
                ++ticks;
                r.heartbeat.fetch_add (1, std::memory_order_release);

                const auto orphaned = parentPid > 0 && ticks % 100 == 0 && ! plugin::process::isAlive (parentPid);

                if (orphaned || r.shouldExit.load (std::memory_order_acquire) != 0)
                {
                    juce::MessageManager::getInstance()->stopDispatchLoop();
                    return;
                }

                const auto displaysMoved = ticks % 100 == 0 && followDisplays (false);

                region::ConfigReading config;

                if (region::readConfig (r, config) && (config.seq != boundSeq || displaysMoved))
                {
                    boundSeq = config.seq;
                    bind (config);
                }

                if (! windowed)
                    probe (config);
            }

            /*  THE DISPLAYS, published when they change: at the start, and
                when one is plugged in or taken away. True when they moved. */
            bool followDisplays (bool force)
            {
                auto now = windowed ? listDisplays() : std::vector<DisplayInfo> {};

                const auto same = ! force && now.size() == displays.size()
                                    && std::equal (now.begin(), now.end(), displays.begin(),
                                                   [] (const DisplayInfo& a, const DisplayInfo& b)
                                                   {
                                                       return a.id == b.id && a.name == b.name && a.x == b.x
                                                           && a.y == b.y && a.width == b.width && a.height == b.height;
                                                   });

                if (same)
                    return false;

                displays = std::move (now);

                region::beginWrite (r.displaysSeq);
                const auto count = std::min<std::size_t> (displays.size(), region::maxDisplays);

                for (std::size_t n = 0; n < count; ++n)
                {
                    auto& into = r.displays[n];
                    region::writeText (into.name, displays[n].name);
                    region::writeText (into.id, displays[n].id);
                    into.x.store (displays[n].x, std::memory_order_relaxed);
                    into.y.store (displays[n].y, std::memory_order_relaxed);
                    into.width.store (displays[n].width, std::memory_order_relaxed);
                    into.height.store (displays[n].height, std::memory_order_relaxed);
                    into.refreshHz.store (displays[n].refreshHz, std::memory_order_relaxed);
                }

                r.displayCount.store (static_cast<std::uint32_t> (count), std::memory_order_relaxed);
                region::endWrite (r.displaysSeq);
                return true;
            }

            /*  EVERY OUTPUT TO ITS DISPLAY, or a sentence why not - and its
                window made again. Rare: the show's outputs changed, or a
                display came or went. */
            void bind (const region::ConfigReading& config)
            {
                windows.clear();

                const auto count = std::min<std::size_t> (config.outputs.size(), region::maxOutputs);

                for (std::size_t n = 0; n < count; ++n)
                {
                    const auto& output = config.outputs[n];
                    auto& state = r.outputs[n];

                    std::string why;
                    int display = -1;

                    if (! output.enabled)
                        why = "switched off";
                    else if (! windowed)
                        why = "the renderer has no window (--no-window)";
                    else
                        display = findDisplay (displays, output.display, output.displayId, why);

                    region::beginWrite (state.seq);
                    region::writeText (state.outputId, output.id);
                    state.bound.store (display >= 0 ? 1u : 0u, std::memory_order_relaxed);
                    region::writeText (state.problem, display >= 0 ? std::string {} : why);
                    region::endWrite (state.seq);

                    if (display >= 0)
                        windows.push_back (std::make_unique<OutputWindow> (r, output.id, static_cast<int> (n),
                                                                           displays[static_cast<std::size_t> (display)]));
                }

                for (std::size_t n = count; n < static_cast<std::size_t> (region::maxOutputs); ++n)
                {
                    auto& state = r.outputs[n];
                    region::beginWrite (state.seq);
                    region::writeText (state.outputId, std::string {});
                    state.bound.store (0, std::memory_order_relaxed);
                    region::endWrite (state.seq);
                }
            }

            /*  WITH NO WINDOW, WHAT EACH CANVAS WOULD SHOW, at the middle, now -
                through the same compositor the windows are held to. */
            void probe (const region::ConfigReading& config)
            {
                const auto now = clock.sampleAt (r, steadyNanos());

                if (now < 0)
                    return;

                const auto layers = readLayers (r);
                const auto count = std::min<std::size_t> (config.canvases.size(), region::maxCanvases);

                region::beginWrite (r.probeSeq);

                for (std::size_t n = 0; n < count; ++n)
                    r.probe[n].store (fillsAt (stackOf (layers, config.canvases[n].id), now), std::memory_order_relaxed);

                r.probeSample.store (now, std::memory_order_relaxed);
                region::endWrite (r.probeSeq);
            }

            region::Region& r;
            std::int64_t parentPid = 0;
            bool windowed = true;
            std::int64_t ticks = 0;
            std::uint32_t boundSeq = 0xffffffffu;

            std::vector<DisplayInfo> displays;
            std::vector<std::unique_ptr<OutputWindow>> windows;
            ClockReader clock;
        };
    }

    //==============================================================================
    int runVideoRender (const std::vector<std::string>& args)
    {
        const auto regionPath = optionFrom (args, "--region");
        const auto parentPid = static_cast<std::int64_t> (std::atoll (optionFrom (args, "--parent-pid").c_str()));
        const auto windowed = ! hasFlag (args, "--no-window");

        if (regionPath.empty())
        {
            std::fprintf (stderr, "wfg video-render: --region=<path> is required\n");
            return 2;
        }

        const juce::File regionFile { juce::String (regionPath) };
        juce::MemoryMappedFile mapping (regionFile, juce::MemoryMappedFile::readWrite, false);

        if (mapping.getData() == nullptr || mapping.getSize() < region::regionBytes())
        {
            std::fprintf (stderr, "wfg video-render: could not map %s\n", regionPath.c_str());
            return 3;
        }

        auto& r = *static_cast<region::Region*> (mapping.getData());

        if (! region::looksValid (r))
        {
            std::fprintf (stderr, "wfg video-render: the region at %s is not laid out as this build expects\n",
                          regionPath.c_str());
            return 3;
        }

        /*  JUCE up for the renderer's whole life, on this thread, which is
            therefore the message thread: where the windows live and where the
            session's passes run. Each window draws on its context's own. */
        juce::ScopedJuceInitialiser_GUI juceForTheRenderer;

       #if JUCE_MAC
        juce::initialiseNSApplication();

        /*  NO DOCK ICON: the outputs are part of Go.dot, not a second
            application somebody could switch to. */
        juce::Process::setDockIconVisible (false);
       #endif

        {
            Session session (r, parentPid, windowed);
            r.ready.store (1, std::memory_order_release);
            juce::MessageManager::getInstance()->runDispatchLoop();
        }

        return 0;
    }

    bool runVideoRenderIfAsked (int argc, char** argv, int& exitCode)
    {
        if (argc < 2 || std::strcmp (argv[1], videoRenderVerb) != 0)
            return false;

        std::vector<std::string> args;

        for (int i = 2; i < argc; ++i)
            args.emplace_back (argv[i]);

        exitCode = runVideoRender (args);
        return true;
    }
}
