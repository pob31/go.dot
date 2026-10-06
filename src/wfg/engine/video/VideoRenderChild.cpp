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
#include <mutex>
#include <set>
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
        /*  THE PICTURES THE RENDERER HAS READ (§36, VX): decoded on a thread of
            their own - never a window's drawing thread, whose next frame must
            not wait for a disk - and held while somebody wants them: a layer
            showing one, or the engine's standby list. A file that will not read
            is remembered as such, and its layer stays black. */
        class PictureStore final : private juce::Thread
        {
        public:
            PictureStore() : juce::Thread ("video pictures")  { startThread(); }
            ~PictureStore() override                            { stopThread (4000); }

            /*  The paths wanted now; any other picture is let go. */
            void want (const std::set<std::string>& paths)
            {
                {
                    const std::lock_guard<std::mutex> hold (lock);
                    wanted = paths;

                    for (auto at = held.begin(); at != held.end();)
                        at = wanted.count (at->first) > 0 ? std::next (at) : held.erase (at);
                }

                notify();
            }

            /*  The picture at `path`, or an invalid image while it is being
                read - and its version, which moves when it is read again. */
            juce::Image get (const std::string& path, std::uint64_t& version) const
            {
                const std::lock_guard<std::mutex> hold (lock);
                const auto found = held.find (path);

                if (found == held.end())
                    return {};

                version = found->second.version;
                return found->second.image;
            }

        private:
            struct Held
            {
                juce::Image image;
                std::uint64_t version = 0;
                bool failed = false;
            };

            void run() override
            {
                while (! threadShouldExit())
                {
                    std::string next;

                    {
                        const std::lock_guard<std::mutex> hold (lock);

                        for (const auto& path : wanted)
                            if (held.count (path) == 0)
                            {
                                next = path;
                                break;
                            }
                    }

                    if (next.empty())
                    {
                        wait (100);
                        continue;
                    }

                    /*  READ OUTSIDE THE LOCK: a large picture takes a while, and
                        the windows ask for the others meanwhile. */
                    auto image = juce::ImageFileFormat::loadFrom (juce::File (juce::String::fromUTF8 (next.c_str())));

                    /*  HELD AS A SOFTWARE IMAGE, in ARGB: JUCE 8's own type on
                        Windows lives on the GPU (Direct2D), and is read back
                        from a thread like this one, or a window's drawing
                        thread, at its peril. */
                    if (image.isValid())
                        image = juce::SoftwareImageType().convert (image.convertedToFormat (juce::Image::ARGB));

                    const std::lock_guard<std::mutex> hold (lock);

                    if (wanted.count (next) > 0)
                        held[next] = Held { image, ++versions, ! image.isValid() };
                }
            }

            mutable std::mutex lock;
            std::set<std::string> wanted;
            std::map<std::string, Held> held;
            std::uint64_t versions = 0;
        };

        /*  THE CPU'S VIEW OF THE STORE (Compositor.h): a picture's size and
            its colour at one point, nearest pixel, straight colour. */
        struct StoreSampler final : PictureSampler
        {
            explicit StoreSampler (const PictureStore& storeToRead) : store (storeToRead) {}

            bool sizeOf (const std::string& path, int& width, int& height) const override
            {
                std::uint64_t version = 0;
                const auto image = store.get (path, version);

                if (! image.isValid())
                    return false;

                width = image.getWidth();
                height = image.getHeight();
                return true;
            }

            bool colourAt (const std::string& path, double u, double v,
                           double& red, double& green, double& blue, double& alpha) const override
            {
                std::uint64_t version = 0;
                const auto image = store.get (path, version);

                if (! image.isValid())
                    return false;

                /*  (0, 0) is the picture's bottom-left; an image's rows run
                    down from its top. */
                const auto x = std::clamp (static_cast<int> (u * image.getWidth()), 0, image.getWidth() - 1);
                const auto y = std::clamp (static_cast<int> ((1.0 - v) * image.getHeight()), 0, image.getHeight() - 1);
                const auto colour = image.getPixelAt (x, y);

                red = colour.getRed();
                green = colour.getGreen();
                blue = colour.getBlue();
                alpha = colour.getFloatAlpha();
                return true;
            }

            const PictureStore& store;
        };

        /*  The canvases the configuration holds, by identifier, with their
            sizes. */
        const region::CanvasReading* canvasIn (const region::ConfigReading& config, const std::string& id)
        {
            for (const auto& canvas : config.canvases)
                if (canvas.id == id)
                    return &canvas;

            return nullptr;
        }

        //==============================================================================
        /*  ONE OUTPUT: a borderless window covering its display, drawn by
            OpenGL every vsync. The canvas fills the window; each layer is a
            quad placed by the shared geometry (Geometry.h) and blended over
            what is under it, premultiplied, which is the compositor's
            `under * (1 - a) + colour * a` (VD). */
        class OutputWindow final : public juce::Component, private juce::OpenGLRenderer
        {
        public:
            OutputWindow (region::Region& regionToRead, const PictureStore& picturesToDraw,
                          std::string outputIdToShow, int stateSlot, const DisplayInfo& display)
                : r (regionToRead), pictures (picturesToDraw), outputId (std::move (outputIdToShow)), slot (stateSlot),
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

                /*  TWO PROGRAMS: a colour, for a fill; a picture, for a picture -
                    each premultiplied by the layer's opacity. */
                const auto vertex = juce::OpenGLHelpers::translateVertexShaderToV3 (
                    "attribute vec2 position;\n"
                    "attribute vec2 texel;\n"
                    "varying vec2 at;\n"
                    "void main() { at = texel; gl_Position = vec4 (position, 0.0, 1.0); }\n");

                fillProgram = std::make_unique<juce::OpenGLShaderProgram> (context);
                pictureProgram = std::make_unique<juce::OpenGLShaderProgram> (context);

                const auto fillOk = fillProgram->addVertexShader (vertex)
                                 && fillProgram->addFragmentShader (juce::OpenGLHelpers::translateFragmentShaderToV3 (
                                        "uniform vec4 colour;\n"
                                        "varying vec2 at;\n"
                                        "void main() { gl_FragColor = vec4 (colour.rgb * colour.a, colour.a); }\n"))
                                 && fillProgram->link();

                const auto pictureOk = pictureProgram->addVertexShader (vertex)
                                    && pictureProgram->addFragmentShader (juce::OpenGLHelpers::translateFragmentShaderToV3 (
                                           "uniform sampler2D picture;\n"
                                           "uniform float opacity;\n"
                                           "varying vec2 at;\n"
                                           "void main() { gl_FragColor = texture2D (picture, at) * opacity; }\n"))
                                    && pictureProgram->link();

                if (! fillOk || ! pictureOk)
                {
                    fillProgram.reset();
                    pictureProgram.reset();
                    return;
                }

                colour = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*fillProgram, "colour");
                pictureUniform = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*pictureProgram, "picture");
                opacityUniform = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*pictureProgram, "opacity");

                glGenVertexArrays (1, &vertexArray);
                glGenBuffers (1, &vertexBuffer);
                glBindVertexArray (vertexArray);
                glBindBuffer (GL_ARRAY_BUFFER, vertexBuffer);
                glBufferData (GL_ARRAY_BUFFER, sizeof (GLfloat) * 16, nullptr, GL_DYNAMIC_DRAW);

                /*  THE SAME TWO ATTRIBUTES IN BOTH PROGRAMS, bound by the
                    location each program gave them. */
                for (auto* program : { fillProgram.get(), pictureProgram.get() })
                {
                    const auto position = juce::OpenGLShaderProgram::Attribute (*program, "position").attributeID;
                    const auto texel = juce::OpenGLShaderProgram::Attribute (*program, "texel").attributeID;

                    glEnableVertexAttribArray (position);
                    glVertexAttribPointer (position, 2, GL_FLOAT, GL_FALSE, sizeof (GLfloat) * 4, nullptr);
                    glEnableVertexAttribArray (texel);
                    glVertexAttribPointer (texel, 2, GL_FLOAT, GL_FALSE, sizeof (GLfloat) * 4,
                                           reinterpret_cast<const void*> (sizeof (GLfloat) * 2));
                }

                glBindVertexArray (0);
            }

            /*  A LAYER'S QUAD: its four corners on the canvas, as the window's
                coordinates, with the texture's corners - `uMax` and `vMin`
                when the texture is larger than the picture it holds. */
            void drawQuad (const Placement& place, double uMax, double vMin)
            {
                using namespace juce::gl;

                GLfloat vertices[16];
                const double corners[4][2] { { -1.0, -1.0 }, { 1.0, -1.0 }, { -1.0, 1.0 }, { 1.0, 1.0 } };

                for (int n = 0; n < 4; ++n)
                {
                    double x = 0.0, y = 0.0;
                    place.toCanvas (corners[n][0], corners[n][1], x, y);

                    const auto u = (place.flipH ? -corners[n][0] : corners[n][0]) * 0.5 + 0.5;
                    const auto v = (place.flipV ? -corners[n][1] : corners[n][1]) * 0.5 + 0.5;

                    vertices[n * 4 + 0] = static_cast<GLfloat> (x / (0.5 * place.canvasWidth));
                    vertices[n * 4 + 1] = static_cast<GLfloat> (y / (0.5 * place.canvasHeight));
                    vertices[n * 4 + 2] = static_cast<GLfloat> (u * uMax);
                    vertices[n * 4 + 3] = static_cast<GLfloat> (vMin + v * (1.0 - vMin));
                }

                glBindBuffer (GL_ARRAY_BUFFER, vertexBuffer);
                glBufferSubData (GL_ARRAY_BUFFER, 0, sizeof (vertices), vertices);
                glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
            }

            /*  The texture for a picture, made or made again on this window's
                context when the store has a newer reading. */
            juce::OpenGLTexture* textureFor (const std::string& path)
            {
                std::uint64_t version = 0;
                const auto image = pictures.get (path, version);

                if (! image.isValid())
                    return nullptr;

                auto& held = textures[path];

                if (held.texture == nullptr || held.version != version)
                {
                    held.texture = std::make_unique<juce::OpenGLTexture>();
                    held.texture->loadImage (image);
                    held.version = version;
                    held.width = image.getWidth();
                    held.height = image.getHeight();
                }

                held.used = true;
                return held.texture.get();
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

                std::string canvasId;
                bool testPattern = false;

                for (const auto& output : config.outputs)
                    if (output.id == outputId)
                    {
                        canvasId = output.canvas;
                        testPattern = output.testPattern;
                    }

                const auto* canvas = canvasIn (config, canvasId);

                for (auto& held : textures)
                    held.second.used = false;

                if (fillProgram != nullptr && sample >= 0 && canvas != nullptr)
                {
                    const auto canvasWidth = static_cast<double> (std::max (1, canvas->width));
                    const auto canvasHeight = static_cast<double> (std::max (1, canvas->height));
                    const auto layers = readLayers (r);

                    glEnable (GL_BLEND);
                    glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                    glBindVertexArray (vertexArray);

                    /*  THE COMPOSITOR'S ORDER AND ITS BLEND (Compositor.h), on the
                        GPU: bottom first, each laid over what is under it. */
                    for (const auto* layer : stackOf (layers, canvasId))
                    {
                        const auto a = opacityOf (*layer, sample);

                        if (! (a > 0.0))
                            continue;

                        if (layer->source == region::Source::fill)
                        {
                            fillProgram->use();
                            colour->set (static_cast<GLfloat> ((layer->paint >> 16) & 0xffu) / 255.0f,
                                         static_cast<GLfloat> ((layer->paint >> 8) & 0xffu) / 255.0f,
                                         static_cast<GLfloat> (layer->paint & 0xffu) / 255.0f,
                                         static_cast<GLfloat> (a));
                            drawQuad (placementOf (*layer, sample, canvasWidth, canvasHeight, canvasWidth, canvasHeight),
                                      1.0, 0.0);
                        }
                        else if (layer->source == region::Source::picture)
                        {
                            auto* texture = textureFor (layer->file);

                            if (texture == nullptr)
                                continue;

                            const auto& held = textures[layer->file];
                            const auto uMax = static_cast<double> (held.width) / std::max (1, texture->getWidth());
                            const auto vMin = 1.0 - static_cast<double> (held.height) / std::max (1, texture->getHeight());

                            pictureProgram->use();
                            glActiveTexture (GL_TEXTURE0);
                            texture->bind();
                            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                            pictureUniform->set (0);
                            opacityUniform->set (static_cast<GLfloat> (a));
                            drawQuad (placementOf (*layer, sample, canvasWidth, canvasHeight,
                                                   static_cast<double> (held.width), static_cast<double> (held.height)),
                                      uMax, vMin);
                            texture->unbind();
                        }
                    }

                    glBindVertexArray (0);
                    glDisable (GL_BLEND);
                }

                /*  A PICTURE NO LAYER DREW THIS FRAME is let go from this
                    context; the store keeps it while it is wanted. */
                for (auto at = textures.begin(); at != textures.end();)
                    at = at->second.used ? std::next (at) : textures.erase (at);

                if (testPattern)
                    drawTestPattern (width, height);
            }

            void openGLContextClosing() override
            {
                using namespace juce::gl;

                textures.clear();

                if (vertexBuffer != 0)
                    glDeleteBuffers (1, &vertexBuffer);

                if (vertexArray != 0)
                    glDeleteVertexArrays (1, &vertexArray);

                vertexBuffer = 0;
                vertexArray = 0;
                colour.reset();
                pictureUniform.reset();
                opacityUniform.reset();
                fillProgram.reset();
                pictureProgram.reset();
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
            const PictureStore& pictures;
            std::string outputId;
            int slot = 0;
            double periodNanos = 1.0e9 / 60.0;

            juce::OpenGLContext context;
            std::unique_ptr<juce::OpenGLShaderProgram> fillProgram, pictureProgram;
            std::unique_ptr<juce::OpenGLShaderProgram::Uniform> colour, pictureUniform, opacityUniform;
            GLuint vertexArray = 0;
            GLuint vertexBuffer = 0;

            struct HeldTexture
            {
                std::unique_ptr<juce::OpenGLTexture> texture;
                std::uint64_t version = 0;
                int width = 0;
                int height = 0;
                bool used = false;
            };

            std::map<std::string, HeldTexture> textures;

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
                : r (regionToServe), parentPid (parentPidToWatch), windowed (windowedToUse), sampler (pictures)
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

                /*  THE PICTURES WANTED: every layer's, and the standby's the
                    engine named - read before GO, so GO only shows them (VX). */
                const auto layers = readLayers (r);
                auto wanted = std::set<std::string> {};

                for (const auto& layer : layers)
                    if (layer.source == region::Source::picture && ! layer.file.empty())
                        wanted.insert (layer.file);

                for (const auto& path : region::readPrepared (r))
                    if (! path.empty())
                        wanted.insert (path);

                if (wanted != lastWanted)
                {
                    lastWanted = wanted;
                    pictures.want (wanted);
                }

                if (! windowed)
                    probe (config, layers);
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
                        windows.push_back (std::make_unique<OutputWindow> (r, pictures, output.id, static_cast<int> (n),
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
            void probe (const region::ConfigReading& config, const std::vector<region::LayerReading>& layers)
            {
                const auto now = clock.sampleAt (r, steadyNanos());

                if (now < 0)
                    return;

                const auto count = std::min<std::size_t> (config.canvases.size(), region::maxCanvases);

                region::beginWrite (r.probeSeq);

                for (std::size_t n = 0; n < count; ++n)
                {
                    const auto& canvas = config.canvases[n];
                    r.probe[n].store (colourAt (stackOf (layers, canvas.id), now,
                                                static_cast<double> (std::max (1, canvas.width)),
                                                static_cast<double> (std::max (1, canvas.height)),
                                                0.0, 0.0, &sampler),
                                      std::memory_order_relaxed);
                }

                r.probeSample.store (now, std::memory_order_relaxed);
                region::endWrite (r.probeSeq);

            }

            region::Region& r;
            std::int64_t parentPid = 0;
            bool windowed = true;
            std::int64_t ticks = 0;
            std::uint32_t boundSeq = 0xffffffffu;

            std::vector<DisplayInfo> displays;
            PictureStore pictures;
            StoreSampler sampler;
            std::set<std::string> lastWanted;
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
