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
#include <wfg/engine/video/Ffmpeg.h>
#include <wfg/engine/video/Hap.h>
#include <wfg/engine/video/Movie.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/video/PipedChild.h>
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

        //==============================================================================
        /*  THE MOVIES THE RENDERER IS PLAYING (namespace draft 37): each file
            opened once, on a thread of its own, and its frames read and
            unpacked a few ahead of where each playhead is - a disk read and a
            copy each, which is what HAP is for. A frame is handed out whole and
            never changed, so a window's drawing thread holds it without a lock
            while it uploads it. */
        struct MovieFrame
        {
            hap::Texture texture = hap::Texture::none;
            std::vector<std::uint8_t> blocks;
            int width = 0;
            int height = 0;
            int index = -1;

            /*  A PREVIEW'S FRAME (namespace draft 37.5, WF): straight RGBA, rows
                from the top, as FFmpeg decoded it - for a movie that is not HAP
                and is played until it is converted. Empty for a HAP frame. */
            std::vector<std::uint8_t> rgba;

            bool drawable() const noexcept  { return texture != hap::Texture::none || ! rgba.empty(); }
        };

        class MovieStore final : private juce::Thread
        {
        public:
            MovieStore() : juce::Thread ("video movies")  { startThread(); }
            ~MovieStore() override                         { stopThread (4000); }

            /*  Each movie wanted now, and the frame its playhead is on. */
            void want (const std::map<std::string, int>& frames)
            {
                {
                    const std::lock_guard<std::mutex> hold (lock);
                    wanted = frames;

                    for (auto at = held.begin(); at != held.end();)
                        at = wanted.count (at->first) > 0 ? std::next (at) : held.erase (at);
                }

                notify();
            }

            /*  The frame showing at `seconds`, by the file's own index: -1
                before the file is open. */
            int frameAt (const std::string& path, double seconds) const
            {
                const std::lock_guard<std::mutex> hold (lock);
                const auto found = held.find (path);
                return found != held.end() && found->second.open ? found->second.info.frameAt (seconds) : -1;
            }

            bool sizeOf (const std::string& path, int& width, int& height) const
            {
                const std::lock_guard<std::mutex> hold (lock);
                const auto found = held.find (path);

                if (found == held.end() || ! found->second.open)
                    return false;

                width = found->second.info.width;
                height = found->second.info.height;
                return true;
            }

            /*  Frame `index` if it has been read, else the nearest read before
                it - a frame late rather than a black one. */
            std::shared_ptr<const MovieFrame> frame (const std::string& path, int index) const
            {
                const std::lock_guard<std::mutex> hold (lock);
                const auto found = held.find (path);

                if (found == held.end() || found->second.frames.empty())
                    return {};

                auto at = found->second.frames.upper_bound (index);

                if (at == found->second.frames.begin())
                    return {};

                return std::prev (at)->second;
            }

        private:
            struct Held
            {
                bool open = false;
                bool failed = false;
                movie::Info info;
                std::map<int, std::shared_ptr<const MovieFrame>> frames;
            };

            /*  A MOVIE THAT IS NOT HAP, PLAYED AS A PREVIEW (namespace draft
                37.5, WF; 37.6, F.6): FFmpeg decodes it to raw RGBA down a pipe,
                at most 1280 wide, at its own rate from where the playhead is;
                a jump back, or one more than two seconds ahead, starts it again
                from there. Slower to seek than HAP, which is what a preview is. */
            struct Preview
            {
                ffmpeg::Tools tools;
                std::string path;
                int width = 0, height = 0;
                int rateOver = 25, rateUnder = 1;
                std::unique_ptr<PipedChild> decoder;
                int next = -1;          ///< the frame the decoder sends next
                bool ended = false;

                double rate() const noexcept  { return static_cast<double> (rateOver) / std::max (1, rateUnder); }

                void startAt (int index)
                {
                    decoder = std::make_unique<PipedChild>();
                    const auto seconds = static_cast<double> (std::max (0, index)) / rate();

                    if (! decoder->start ({ tools.ffmpeg, "-nostdin", "-v", "error", "-ss", osc::formatDouble (seconds),
                                            "-i", path, "-map", "0:v:0", "-an", "-sn",
                                            "-vf", "scale=" + std::to_string (width) + ":" + std::to_string (height),
                                            "-fps_mode", "cfr", "-r", std::to_string (rateOver) + "/" + std::to_string (rateUnder),
                                            "-f", "rawvideo", "-pix_fmt", "rgba", "-" }))
                        decoder.reset();

                    next = std::max (0, index);
                    ended = decoder == nullptr;
                }
            };

            /*  OPENED AS A PREVIEW: its size scaled for a preview, its frames
                as the rate and the length make them. False when FFmpeg is not
                here or cannot read it. */
            static bool openPreview (const std::string& path, Preview& preview, movie::Info& info)
            {
                preview.tools = ffmpeg::find();

                if (! preview.tools.found())
                    return false;

                const auto probed = ffmpeg::probe (preview.tools, path);

                if (! probed.ok || ! (probed.duration > 0.0) || probed.width <= 0 || probed.height <= 0)
                    return false;

                const auto scale = std::min (1.0, 1280.0 / probed.width);
                preview.path = path;
                preview.width = std::max (2, static_cast<int> (std::lround (probed.width * scale / 2.0)) * 2);
                preview.height = std::max (2, static_cast<int> (std::lround (probed.height * scale / 2.0)) * 2);
                preview.rateOver = probed.rateOver > 0 ? probed.rateOver : 25;
                preview.rateUnder = probed.rateOver > 0 ? std::max (1, probed.rateUnder) : 1;

                info = {};
                info.codec = "preview";
                info.width = preview.width;
                info.height = preview.height;
                info.duration = probed.duration;

                const auto count = std::max (1, static_cast<int> (std::floor (probed.duration * preview.rate())));

                for (int n = 0; n < count; ++n)
                    info.frames.push_back ({ 0, 0, static_cast<double> (n) / preview.rate() });

                return true;
            }

            /*  THE PREVIEW'S FRAMES from the playhead's and three after, read
                from the decoder; started again where the playhead jumped. */
            bool feedPreview (const std::string& path, Preview& preview, int current, int count)
            {
                const auto rateNow = preview.rate();

                if (preview.decoder == nullptr || current < preview.next - 1
                      || current > preview.next + static_cast<int> (rateNow * 2.0))
                    preview.startAt (current);

                auto worked = false;
                const auto bytes = static_cast<std::size_t> (preview.width) * static_cast<std::size_t> (preview.height) * 4;

                while (! preview.ended && preview.decoder != nullptr && preview.next <= current + 3
                       && preview.next < count && ! threadShouldExit())
                {
                    auto frame = std::make_shared<MovieFrame>();
                    frame->index = preview.next;
                    frame->width = preview.width;
                    frame->height = preview.height;
                    frame->rgba.resize (bytes);

                    if (! preview.decoder->readExactly (frame->rgba.data(), bytes))
                    {
                        preview.ended = true;
                        break;
                    }

                    ++preview.next;
                    worked = true;

                    const std::lock_guard<std::mutex> hold (lock);
                    auto& entry = held[path];
                    entry.frames[frame->index] = frame;

                    for (auto at = entry.frames.begin(); at != entry.frames.end();)
                        at = at->first < current - 1 || at->first > current + 8 ? entry.frames.erase (at) : std::next (at);
                }

                return worked;
            }

            void run() override
            {
                std::map<std::string, std::unique_ptr<movie::MovieFile>> files;
                std::map<std::string, Preview> previews;
                std::vector<std::uint8_t> bytes;

                while (! threadShouldExit())
                {
                    std::map<std::string, int> now;

                    {
                        const std::lock_guard<std::mutex> hold (lock);
                        now = wanted;
                    }

                    for (auto at = files.begin(); at != files.end();)
                        at = now.count (at->first) > 0 ? std::next (at) : files.erase (at);

                    for (auto at = previews.begin(); at != previews.end();)
                        at = now.count (at->first) > 0 ? std::next (at) : previews.erase (at);

                    bool worked = false;

                    for (const auto& [path, current] : now)
                    {
                        auto& file = files[path];

                        if (file == nullptr)
                        {
                            file = std::make_unique<movie::MovieFile>();
                            std::string why;
                            auto opened = file->open (path, why) && file->info().isHap();
                            movie::Info info = opened ? file->info() : movie::Info {};

                            /*  NOT HAP: a preview, where FFmpeg is here. */
                            if (! opened)
                            {
                                Preview preview;

                                if (openPreview (path, preview, info))
                                {
                                    opened = true;
                                    previews[path] = std::move (preview);
                                }
                            }

                            const std::lock_guard<std::mutex> hold (lock);
                            auto& entry = held[path];
                            entry.open = opened;
                            entry.failed = ! opened;

                            if (opened)
                                entry.info = info;
                        }

                        if (const auto preview = previews.find (path); preview != previews.end())
                        {
                            int count = 0;

                            {
                                const std::lock_guard<std::mutex> hold (lock);
                                count = static_cast<int> (held[path].info.frames.size());
                            }

                            worked = feedPreview (path, preview->second, std::clamp (current, 0, std::max (0, count - 1)), count)
                                  || worked;
                            continue;
                        }

                        if (file->info().frames.empty())
                            continue;

                        /*  THE FRAME THE PLAYHEAD IS ON AND THREE AFTER, wrapping
                            round the end as a loop does; anything well behind
                            let go. */
                        const auto count = static_cast<int> (file->info().frames.size());
                        const auto first = std::clamp (current, 0, count - 1);

                        for (int ahead = 0; ahead < 4 && ! threadShouldExit(); ++ahead)
                        {
                            const auto index = (first + ahead) % count;

                            {
                                const std::lock_guard<std::mutex> hold (lock);
                                const auto found = held.find (path);

                                if (found == held.end() || found->second.frames.count (index) > 0)
                                    continue;
                            }

                            auto frame = std::make_shared<MovieFrame>();
                            frame->index = index;
                            frame->width = file->info().width;
                            frame->height = file->info().height;

                            if (! file->readFrame (index, bytes)
                                  || ! hap::unpack (bytes.data(), bytes.size(), frame->texture, frame->blocks))
                                frame->texture = hap::Texture::none;

                            const std::lock_guard<std::mutex> hold (lock);
                            auto& entry = held[path];
                            entry.frames[index] = std::move (frame);
                            worked = true;

                            for (auto at = entry.frames.begin(); at != entry.frames.end();)
                            {
                                const auto distance = (at->first - first + count) % count;
                                at = distance > 8 ? entry.frames.erase (at) : std::next (at);
                            }
                        }
                    }

                    if (! worked)
                        wait (5);
                }
            }

            mutable std::mutex lock;
            std::map<std::string, int> wanted;
            std::map<std::string, Held> held;
        };

        /*  THE CPU'S VIEW OF THE STORES (Compositor.h): a picture's size and
            its colour at one point, nearest pixel, straight colour - and a
            movie's, from the frame its playhead is on. */
        struct StoreSampler final : PictureSampler
        {
            StoreSampler (const PictureStore& storeToRead, const MovieStore& moviesToRead)
                : store (storeToRead), movies (moviesToRead) {}

            bool movieSizeOf (const std::string& path, int& width, int& height) const override
            {
                return movies.sizeOf (path, width, height);
            }

            bool movieColourAt (const std::string& path, double seconds, double u, double v,
                                double& red, double& green, double& blue, double& alpha) const override
            {
                const auto frame = movies.frame (path, movies.frameAt (path, seconds));

                if (frame == nullptr || ! frame->drawable())
                    return false;

                /*  A PREVIEW'S FRAME: straight RGBA, rows from the top. */
                if (! frame->rgba.empty())
                {
                    const auto px = std::clamp (static_cast<int> (u * frame->width), 0, frame->width - 1);
                    const auto py = std::clamp (static_cast<int> ((1.0 - v) * frame->height), 0, frame->height - 1);
                    const auto* at = frame->rgba.data() + (static_cast<std::size_t> (py) * static_cast<std::size_t> (frame->width)
                                                           + static_cast<std::size_t> (px)) * 4;
                    red = at[0];
                    green = at[1];
                    blue = at[2];
                    alpha = at[3] / 255.0;
                    return true;
                }

                const auto padded = (frame->width + 3) / 4 * 4;
                const auto x = std::clamp (static_cast<int> (u * frame->width), 0, frame->width - 1);
                const auto y = std::clamp (static_cast<int> ((1.0 - v) * frame->height), 0, frame->height - 1);

                return hap::pixelAt (frame->texture, frame->blocks, padded, (frame->height + 3) / 4 * 4,
                                     x, y, red, green, blue, alpha);
            }

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
            const MovieStore& movies;
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

        /*  THE GRADE IN GLSL (Grade.h, line for line): gamma, contrast about
            mid-grey, the hue turned about the grey axis, the saturation, then
            the baked tables - one texel a step, red, green and blue. */
        constexpr const char* gradeGlsl =
            "uniform float gradeGamma;\n"
            "uniform float gradeContrast;\n"
            "uniform float gradeSaturation;\n"
            "uniform vec2 gradeHue;\n"
            "uniform float gradeCurves;\n"
            "uniform sampler2D gradeTables;\n"
            "vec3 grade (vec3 c) {\n"
            "  c = pow (clamp (c, 0.0, 1.0), vec3 (1.0 / gradeGamma));\n"
            "  c = (c - 0.5) * gradeContrast + 0.5;\n"
            "  float luma = dot (c, vec3 (0.2126, 0.7152, 0.0722));\n"
            "  vec3 colour = c - luma;\n"
            "  vec3 axis = vec3 (0.57735026918963);\n"
            "  colour = colour * gradeHue.x + cross (axis, colour) * gradeHue.y + axis * dot (axis, colour) * (1.0 - gradeHue.x);\n"
            "  c = clamp (luma + colour * gradeSaturation, 0.0, 1.0);\n"
            "  if (gradeCurves > 0.5) {\n"
            "    vec3 at = (c * 255.0 + 0.5) / 256.0;\n"
            "    c = vec3 (texture2D (gradeTables, vec2 (at.r, 0.5)).r,\n"
            "              texture2D (gradeTables, vec2 (at.g, 0.5)).g,\n"
            "              texture2D (gradeTables, vec2 (at.b, 0.5)).b);\n"
            "  }\n"
            "  return c;\n"
            "}\n";

        //==============================================================================
        /*  ONE OUTPUT: a borderless window covering its display, drawn by
            OpenGL every vsync. The canvas fills the window; each layer is a
            quad placed by the shared geometry (Geometry.h) and blended over
            what is under it, premultiplied, which is the compositor's
            `under * (1 - a) + colour * a` (VD). */
        class OutputWindow final : public juce::Component, private juce::OpenGLRenderer
        {
        public:
            OutputWindow (region::Region& regionToRead, const PictureStore& picturesToDraw, const MovieStore& moviesToDraw,
                          std::string outputIdToShow, int stateSlot, const DisplayInfo& display)
                : r (regionToRead), pictures (picturesToDraw), movies (moviesToDraw), outputId (std::move (outputIdToShow)), slot (stateSlot),
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
                                           juce::String (gradeGlsl) +
                                           "uniform sampler2D picture;\n"
                                           "uniform float opacity;\n"
                                           "varying vec2 at;\n"
                                           "void main() {\n"
                                           "  vec4 p = texture2D (picture, at);\n"
                                           "  vec3 rgb = p.a > 0.0 ? grade (p.rgb / p.a) : vec3 (0.0);\n"
                                           "  gl_FragColor = vec4 (rgb * p.a, p.a) * opacity;\n"
                                           "}\n"))
                                    && pictureProgram->link();

                /*  A MOVIE'S TWO: DXT's colour premultiplied by its own alpha -
                    HAP's is straight - and Hap Q's scaled YCoCg turned back to
                    RGB as the HAP shader does (Hap.h). */
                movieProgram = std::make_unique<juce::OpenGLShaderProgram> (context);
                movieQProgram = std::make_unique<juce::OpenGLShaderProgram> (context);

                const auto movieOk = movieProgram->addVertexShader (vertex)
                                  && movieProgram->addFragmentShader (juce::OpenGLHelpers::translateFragmentShaderToV3 (
                                         juce::String (gradeGlsl) +
                                         "uniform sampler2D picture;\n"
                                         "uniform float opacity;\n"
                                         "varying vec2 at;\n"
                                         "void main() { vec4 c = texture2D (picture, at); gl_FragColor = vec4 (grade (c.rgb) * c.a, c.a) * opacity; }\n"))
                                  && movieProgram->link();

                const auto movieQOk = movieQProgram->addVertexShader (vertex)
                                   && movieQProgram->addFragmentShader (juce::OpenGLHelpers::translateFragmentShaderToV3 (
                                          juce::String (gradeGlsl) +
                                          "uniform sampler2D picture;\n"
                                          "uniform float opacity;\n"
                                          "varying vec2 at;\n"
                                          "void main() {\n"
                                          "  vec4 q = texture2D (picture, at);\n"
                                          "  float scale = (q.b * (255.0 / 8.0)) + 1.0;\n"
                                          "  float co = (q.r - 0.50196078431373) / scale;\n"
                                          "  float cg = (q.g - 0.50196078431373) / scale;\n"
                                          "  vec3 rgb = vec3 (q.a + co - cg, q.a + cg, q.a - co - cg);\n"
                                          "  gl_FragColor = vec4 (grade (clamp (rgb, 0.0, 1.0)), 1.0) * opacity;\n"
                                          "}\n"))
                                   && movieQProgram->link();

                /*  EACH PROGRAM FAILS ALONE: a driver that will not compile the
                    picture's shader still draws the fills, and says nothing
                    worse than a picture not shown. Without the fill's there is
                    nothing to draw with at all. */
                if (! fillOk)
                {
                    fillProgram.reset();
                    pictureProgram.reset();
                    movieProgram.reset();
                    movieQProgram.reset();
                    return;
                }

                if (! pictureOk)  pictureProgram.reset();
                if (! movieOk)    movieProgram.reset();
                if (! movieQOk)   movieQProgram.reset();

                /*  A MASK'S (Mask.h, line for line): its shape filled even-odd
                    and feathered, over the canvas-sized quad the geometry put. */
                maskProgram = std::make_unique<juce::OpenGLShaderProgram> (context);

                const auto maskOk = maskProgram->addVertexShader (vertex)
                                 && maskProgram->addFragmentShader (juce::OpenGLHelpers::translateFragmentShaderToV3 (
                                        "uniform vec4 colour;\n"
                                        "uniform vec2 canvas;\n"
                                        "uniform float feather;\n"
                                        "uniform float invert;\n"
                                        "uniform int count;\n"
                                        "uniform vec2 corners[64];\n"
                                        "varying vec2 at;\n"
                                        "void main() {\n"
                                        "  vec2 p = vec2 (at.x * canvas.x, (1.0 - at.y) * canvas.y);\n"
                                        "  bool inside = false;\n"
                                        "  float nearest = 1.0e30;\n"
                                        "  int previous = count - 1;\n"
                                        "  for (int n = 0; n < 64; ++n) {\n"
                                        "    if (n >= count) break;\n"
                                        "    vec2 a = corners[previous] * canvas;\n"
                                        "    vec2 b = corners[n] * canvas;\n"
                                        "    if (((b.y > p.y) != (a.y > p.y)) && (p.x < (a.x - b.x) * (p.y - b.y) / (a.y - b.y) + b.x)) inside = ! inside;\n"
                                        "    vec2 e = a - b;\n"
                                        "    float span = dot (e, e);\n"
                                        "    float t = span > 0.0 ? clamp (dot (p - b, e) / span, 0.0, 1.0) : 0.0;\n"
                                        "    nearest = min (nearest, length (p - (b + t * e)));\n"
                                        "    previous = n;\n"
                                        "  }\n"
                                        "  float cover = count < 3 ? 0.0 : (feather > 0.0 ? clamp (0.5 + (inside ? nearest : -nearest) / feather, 0.0, 1.0)\n"
                                        "                                                  : (inside ? 1.0 : 0.0));\n"
                                        "  if (invert > 0.5) cover = 1.0 - cover;\n"
                                        "  float a = colour.a * cover;\n"
                                        "  gl_FragColor = vec4 (colour.rgb * a, a);\n"
                                        "}\n"))
                                 && maskProgram->link();

                if (! maskOk)
                    maskProgram.reset();

                /*  THE WARP'S (Mapping.h): the offscreen canvas onto the display
                    through the mesh, the output's CDL on the way, and a dither
                    from half-float down to the display's eight bits, so a slow
                    fade to black does not band. */
                warpProgram = std::make_unique<juce::OpenGLShaderProgram> (context);

                const auto warpOk = warpProgram->addVertexShader (vertex)
                                 && warpProgram->addFragmentShader (juce::OpenGLHelpers::translateFragmentShaderToV3 (
                                        "uniform sampler2D canvas;\n"
                                        "uniform vec3 slope;\n"
                                        "uniform vec3 offset;\n"
                                        "uniform vec3 power;\n"
                                        "uniform float saturation;\n"
                                        "varying vec2 at;\n"
                                        "float noise (vec2 p) { return fract (sin (dot (p, vec2 (12.9898, 78.233))) * 43758.5453); }\n"
                                        "void main() {\n"
                                        "  vec3 c = texture2D (canvas, at).rgb;\n"
                                        "  c = pow (clamp (c * slope + offset, 0.0, 1.0), power);\n"
                                        "  float luma = dot (c, vec3 (0.2126, 0.7152, 0.0722));\n"
                                        "  c = clamp (luma + saturation * (c - luma), 0.0, 1.0);\n"
                                        "  c += (noise (gl_FragCoord.xy) - 0.5) / 255.0;\n"
                                        "  gl_FragColor = vec4 (c, 1.0);\n"
                                        "}\n"))
                                 && warpProgram->link();

                if (! warpOk)
                    warpProgram.reset();
                else
                    for (const auto* name : { "canvas", "slope", "offset", "power", "saturation" })
                        warpUniforms[name] = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*warpProgram, name);

                if (maskProgram != nullptr)
                    for (const auto* name : { "colour", "canvas", "feather", "invert", "count", "corners" })
                        maskUniforms[name] = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*maskProgram, name);

                if (pictureProgram != nullptr)
                    pictureGrade.make (*pictureProgram);

                if (movieProgram != nullptr)
                {
                    movieGrade.make (*movieProgram);
                    movieUniform = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*movieProgram, "picture");
                    movieOpacity = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*movieProgram, "opacity");
                }

                if (movieQProgram != nullptr)
                {
                    movieQGrade.make (*movieQProgram);
                    movieQUniform = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*movieQProgram, "picture");
                    movieQOpacity = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*movieQProgram, "opacity");
                }

                colour = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*fillProgram, "colour");

                if (pictureProgram != nullptr)
                {
                    pictureUniform = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*pictureProgram, "picture");
                    opacityUniform = std::make_unique<juce::OpenGLShaderProgram::Uniform> (*pictureProgram, "opacity");
                }

                glGenVertexArrays (1, &vertexArray);
                glGenBuffers (1, &vertexBuffer);
                glBindVertexArray (vertexArray);
                glBindBuffer (GL_ARRAY_BUFFER, vertexBuffer);
                glBufferData (GL_ARRAY_BUFFER, sizeof (GLfloat) * 16, nullptr, GL_DYNAMIC_DRAW);

                /*  THE SAME TWO ATTRIBUTES IN BOTH PROGRAMS, bound by the
                    location each program gave them. */
                for (auto* program : { fillProgram.get(), pictureProgram.get(), movieProgram.get(), movieQProgram.get(),
                                       maskProgram.get() })
                {
                    if (program == nullptr)
                        continue;

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
                drawQuad (place, uMax, vMin, 1.0);
            }

            /*  `tBottom` and `tTop`: where the picture's bottom and top rows are
                in the texture - a picture's flipped on loading, a movie's
                blocks laid top row first (§37). */
            void drawQuad (const Placement& place, double uMax, double tBottom, double tTop)
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
                    vertices[n * 4 + 3] = static_cast<GLfloat> (tBottom + v * (tTop - tBottom));
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
                const region::OutputReading* mine = nullptr;

                for (const auto& output : config.outputs)
                    if (output.id == outputId)
                    {
                        canvasId = output.canvas;
                        testPattern = output.testPattern;
                        mine = &output;
                    }

                const auto* canvas = canvasIn (config, canvasId);

                /*  A MAPPED OUTPUT draws its canvas offscreen first, at the
                    canvas's own size, and bends it onto the display after; one
                    nobody mapped draws straight onto the display, as ever. */
                const auto mapped = mine != nullptr && canvas != nullptr && fillProgram != nullptr && warpProgram != nullptr
                                      && (! mine->mesh.isIdentity() || ! mine->cdl.isIdentity())
                                      && bindCanvasTarget (std::max (1, canvas->width), std::max (1, canvas->height));

                for (auto& held : textures)
                    held.second.used = false;

                if (fillProgram != nullptr && sample >= 0 && canvas != nullptr)
                {
                    const auto canvasWidth = static_cast<double> (std::max (1, canvas->width));
                    const auto canvasHeight = static_cast<double> (std::max (1, canvas->height));
                    const auto layers = readLayers (r);

                    glEnable (GL_BLEND);
                    glBindVertexArray (vertexArray);

                    /*  THE COMPOSITOR'S ORDER AND ITS BLEND (Compositor.h), on the
                        GPU: bottom first, each laid over what is under it. */
                    for (const auto* layer : stackOf (layers, canvasId))
                    {
                        const auto a = opacityOf (*layer, sample);

                        if (! (a > 0.0))
                            continue;

                        /*  THE LAYER'S BLEND (VD), on premultiplied colour - the
                            compositor's formulas (Compositor.h) as equations. */
                        switch (layer->blend)
                        {
                            case region::Blend::add:      glBlendFunc (GL_ONE, GL_ONE); break;
                            case region::Blend::screen:   glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_COLOR); break;
                            case region::Blend::multiply: glBlendFunc (GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA); break;
                            case region::Blend::normal:   glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
                        }

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
                            if (pictureProgram == nullptr)
                                continue;

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
                            setGrade (pictureGrade, *layer);
                            drawQuad (placementOf (*layer, sample, canvasWidth, canvasHeight,
                                                   static_cast<double> (held.width), static_cast<double> (held.height)),
                                      uMax, vMin);
                            texture->unbind();
                        }
                        else if (layer->source == region::Source::movie)
                        {
                            drawMovie (*layer, sample, canvasWidth, canvasHeight, a);
                        }
                        else if (layer->source == region::Source::mask && maskProgram != nullptr)
                        {
                            GLfloat corners[2 * mask::maxPoints] {};

                            for (int n = 0; n < layer->shape.count; ++n)
                            {
                                corners[2 * n] = layer->shape.x[n];
                                corners[2 * n + 1] = layer->shape.y[n];
                            }

                            maskProgram->use();
                            maskUniforms["colour"]->set (static_cast<GLfloat> ((layer->paint >> 16) & 0xffu) / 255.0f,
                                                         static_cast<GLfloat> ((layer->paint >> 8) & 0xffu) / 255.0f,
                                                         static_cast<GLfloat> (layer->paint & 0xffu) / 255.0f,
                                                         static_cast<GLfloat> (a));
                            maskUniforms["canvas"]->set (static_cast<GLfloat> (canvasWidth), static_cast<GLfloat> (canvasHeight));
                            maskUniforms["feather"]->set (layer->shape.feather);
                            maskUniforms["invert"]->set (layer->shape.invert ? 1.0f : 0.0f);
                            maskUniforms["count"]->set (static_cast<GLint> (layer->shape.count));
                            glUniform2fv (maskUniforms["corners"]->uniformID, mask::maxPoints, corners);
                            drawQuad (placementOf (*layer, sample, canvasWidth, canvasHeight, canvasWidth, canvasHeight),
                                      1.0, 0.0);
                        }
                    }

                    /*  THE CANVAS'S LEVEL (namespace draft §38, WT): the whole
                        composite taken towards black, once, over every layer -
                        so two layers stacked go down together as one picture,
                        never each turning see-through over the other. Before
                        the mapping and the test pattern, which are the
                        output's and not the canvas's. */
                    if (const auto level = region::canvasLevelOf (r, canvasId); level < 1.0)
                    {
                        fillProgram->use();
                        glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                        colour->set (0.0f, 0.0f, 0.0f, static_cast<GLfloat> (1.0 - level));

                        Placement whole;
                        whole.canvasWidth = canvasWidth;
                        whole.canvasHeight = canvasHeight;
                        whole.pictureWidth = canvasWidth;
                        whole.pictureHeight = canvasHeight;
                        whole.fit = static_cast<int> (region::Fit::stretch);
                        drawQuad (whole, 1.0, 0.0);
                    }

                    glBindVertexArray (0);
                    glDisable (GL_BLEND);
                }

                if (mapped)
                    warpOnto (width, height, *mine);

                /*  A PICTURE NO LAYER DREW THIS FRAME is let go from this
                    context; the store keeps it while it is wanted. */
                for (auto at = textures.begin(); at != textures.end();)
                    at = at->second.used ? std::next (at) : textures.erase (at);

                for (auto at = gradeTables.begin(); at != gradeTables.end();)
                {
                    if (at->second.used)
                    {
                        at->second.used = false;
                        ++at;
                        continue;
                    }

                    juce::gl::glDeleteTextures (1, &at->second.id);
                    at = gradeTables.erase (at);
                }

                for (auto at = movieTextures.begin(); at != movieTextures.end();)
                {
                    if (at->second.used)
                    {
                        ++at;
                        continue;
                    }

                    juce::gl::glDeleteTextures (1, &at->second.id);
                    at = movieTextures.erase (at);
                }

                if (testPattern)
                    drawTestPattern (width, height);
            }

            /*  THE OFFSCREEN CANVAS: a half-float texture of the canvas's size
                and its framebuffer, made or made again when the size changes,
                bound, and cleared to black. False when the driver will not
                give one - and the output is then drawn unmapped. */
            bool bindCanvasTarget (int canvasWidth, int canvasHeight)
            {
                using namespace juce::gl;

                if (canvasFrame == 0 || targetWidth != canvasWidth || targetHeight != canvasHeight)
                {
                    releaseCanvasTarget();

                    glGenTextures (1, &canvasTexture);
                    glBindTexture (GL_TEXTURE_2D, canvasTexture);
                    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA16F, canvasWidth, canvasHeight, 0, GL_RGBA, GL_FLOAT, nullptr);
                    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    glBindTexture (GL_TEXTURE_2D, 0);

                    glGenFramebuffers (1, &canvasFrame);
                    glBindFramebuffer (GL_FRAMEBUFFER, canvasFrame);
                    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, canvasTexture, 0);

                    const auto complete = glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
                    glBindFramebuffer (GL_FRAMEBUFFER, context.getFrameBufferID());

                    if (! complete)
                    {
                        releaseCanvasTarget();
                        return false;
                    }

                    targetWidth = canvasWidth;
                    targetHeight = canvasHeight;
                }

                glBindFramebuffer (GL_FRAMEBUFFER, canvasFrame);
                glViewport (0, 0, canvasWidth, canvasHeight);
                glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
                glClear (GL_COLOR_BUFFER_BIT);
                return true;
            }

            void releaseCanvasTarget()
            {
                using namespace juce::gl;

                if (canvasFrame != 0)
                    glDeleteFramebuffers (1, &canvasFrame);

                if (canvasTexture != 0)
                    glDeleteTextures (1, &canvasTexture);

                canvasFrame = 0;
                canvasTexture = 0;
                targetWidth = targetHeight = 0;
            }

            /*  THE CANVAS ONTO THE DISPLAY THROUGH THE MESH: a grid of 64 by 64
                quads, each corner where the mesh sends that point of the canvas
                (Mapping.h), made again when the mesh changes. */
            void warpOnto (int width, int height, const region::OutputReading& output)
            {
                using namespace juce::gl;

                constexpr int steps = 64;

                glBindFramebuffer (GL_FRAMEBUFFER, context.getFrameBufferID());
                glViewport (0, 0, width, height);
                glDisable (GL_BLEND);
                glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
                glClear (GL_COLOR_BUFFER_BIT);

                if (warpArray == 0)
                {
                    glGenVertexArrays (1, &warpArray);
                    glGenBuffers (1, &warpBuffer);
                    glGenBuffers (1, &warpIndices);
                    glBindVertexArray (warpArray);
                    glBindBuffer (GL_ARRAY_BUFFER, warpBuffer);
                    glBufferData (GL_ARRAY_BUFFER, sizeof (GLfloat) * 4 * (steps + 1) * (steps + 1), nullptr, GL_DYNAMIC_DRAW);

                    const auto position = juce::OpenGLShaderProgram::Attribute (*warpProgram, "position").attributeID;
                    const auto texel = juce::OpenGLShaderProgram::Attribute (*warpProgram, "texel").attributeID;
                    glEnableVertexAttribArray (position);
                    glVertexAttribPointer (position, 2, GL_FLOAT, GL_FALSE, sizeof (GLfloat) * 4, nullptr);
                    glEnableVertexAttribArray (texel);
                    glVertexAttribPointer (texel, 2, GL_FLOAT, GL_FALSE, sizeof (GLfloat) * 4,
                                           reinterpret_cast<const void*> (sizeof (GLfloat) * 2));

                    std::vector<GLuint> indices;

                    constexpr GLuint across = static_cast<GLuint> (steps) + 1u;

                    for (GLuint row = 0; row + 1u < across; ++row)
                        for (GLuint column = 0; column + 1u < across; ++column)
                        {
                            const auto at = row * across + column;
                            indices.insert (indices.end(), { at, at + 1u, at + across, at + 1u, at + across + 1u, at + across });
                        }

                    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, warpIndices);
                    glBufferData (GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr> (indices.size() * sizeof (GLuint)),
                                  indices.data(), GL_STATIC_DRAW);
                    glBindVertexArray (0);
                    meshDrawn = Mesh {};
                }

                if (meshDrawn.columns != output.mesh.columns || meshDrawn.rows != output.mesh.rows
                      || meshDrawn.x != output.mesh.x || meshDrawn.y != output.mesh.y)
                {
                    std::vector<GLfloat> vertices;
                    vertices.reserve (4 * (steps + 1) * (steps + 1));

                    for (int row = 0; row <= steps; ++row)
                        for (int column = 0; column <= steps; ++column)
                        {
                            const auto s = static_cast<double> (column) / steps;
                            const auto t = static_cast<double> (row) / steps;
                            double x = 0.0, y = 0.0;
                            meshAt (output.mesh, s, t, x, y);

                            vertices.push_back (static_cast<GLfloat> (2.0 * x - 1.0));
                            vertices.push_back (static_cast<GLfloat> (1.0 - 2.0 * y));
                            vertices.push_back (static_cast<GLfloat> (s));
                            vertices.push_back (static_cast<GLfloat> (1.0 - t));
                        }

                    glBindBuffer (GL_ARRAY_BUFFER, warpBuffer);
                    glBufferSubData (GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr> (vertices.size() * sizeof (GLfloat)), vertices.data());
                    meshDrawn = output.mesh;
                }

                const auto& cdl = output.cdl;
                warpProgram->use();
                glActiveTexture (GL_TEXTURE0);
                glBindTexture (GL_TEXTURE_2D, canvasTexture);
                warpUniforms["canvas"]->set (0);
                warpUniforms["slope"]->set (static_cast<GLfloat> (cdl.slope[0]), static_cast<GLfloat> (cdl.slope[1]), static_cast<GLfloat> (cdl.slope[2]));
                warpUniforms["offset"]->set (static_cast<GLfloat> (cdl.offset[0]), static_cast<GLfloat> (cdl.offset[1]), static_cast<GLfloat> (cdl.offset[2]));
                warpUniforms["power"]->set (static_cast<GLfloat> (cdl.power[0]), static_cast<GLfloat> (cdl.power[1]), static_cast<GLfloat> (cdl.power[2]));
                warpUniforms["saturation"]->set (static_cast<GLfloat> (cdl.saturation));

                glBindVertexArray (warpArray);
                glDrawElements (GL_TRIANGLES, steps * steps * 6, GL_UNSIGNED_INT, nullptr);
                glBindVertexArray (0);
                glBindTexture (GL_TEXTURE_2D, 0);
            }

            /*  A PROGRAM'S GRADE UNIFORMS, and setting them for one layer: its
                numbers, and its baked tables on texture unit one - made once a
                layer, since a layer's grade does not move (VV). */
            struct GradeUniforms
            {
                void make (juce::OpenGLShaderProgram& program)
                {
                    gamma = std::make_unique<juce::OpenGLShaderProgram::Uniform> (program, "gradeGamma");
                    contrast = std::make_unique<juce::OpenGLShaderProgram::Uniform> (program, "gradeContrast");
                    saturation = std::make_unique<juce::OpenGLShaderProgram::Uniform> (program, "gradeSaturation");
                    hue = std::make_unique<juce::OpenGLShaderProgram::Uniform> (program, "gradeHue");
                    curves = std::make_unique<juce::OpenGLShaderProgram::Uniform> (program, "gradeCurves");
                    tables = std::make_unique<juce::OpenGLShaderProgram::Uniform> (program, "gradeTables");
                }

                void reset()
                {
                    gamma.reset(); contrast.reset(); saturation.reset(); hue.reset(); curves.reset(); tables.reset();
                }

                std::unique_ptr<juce::OpenGLShaderProgram::Uniform> gamma, contrast, saturation, hue, curves, tables;
            };

            void setGrade (GradeUniforms& uniforms, const region::LayerReading& layer)
            {
                using namespace juce::gl;

                if (uniforms.gamma == nullptr)
                    return;

                const auto& grade = layer.grade;
                const auto turn = grade.hue * 3.14159265358979323846 / 180.0;

                uniforms.gamma->set (static_cast<GLfloat> (std::max (0.01, grade.gamma)));
                uniforms.contrast->set (static_cast<GLfloat> (grade.contrast / 100.0));
                uniforms.saturation->set (static_cast<GLfloat> (grade.saturation / 100.0));
                uniforms.hue->set (static_cast<GLfloat> (std::cos (turn)), static_cast<GLfloat> (std::sin (turn)));
                uniforms.curves->set (grade.hasCurves ? 1.0f : 0.0f);
                uniforms.tables->set (1);

                if (! grade.hasCurves)
                    return;

                auto& held = gradeTables[layer.id];
                held.used = true;

                if (held.id == 0)
                {
                    std::uint8_t texels[256 * 3];

                    for (std::size_t step = 0; step < 256; ++step)
                        for (std::size_t channel = 0; channel < 3; ++channel)
                            texels[step * 3 + channel] = grade.tables[channel][step];

                    glGenTextures (1, &held.id);
                    glBindTexture (GL_TEXTURE_2D, held.id);
                    glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
                    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGB8, 256, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, texels);
                    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                }

                glActiveTexture (GL_TEXTURE1);
                glBindTexture (GL_TEXTURE_2D, held.id);
                glActiveTexture (GL_TEXTURE0);
            }

            /*  A MOVIE'S FRAME AT THIS SAMPLE, uploaded still compressed when it
                is not the one this context holds, and drawn by the picture's
                quad. The frame a little late rather than none while the next is
                read. */
            void drawMovie (const region::LayerReading& layer, std::int64_t sample,
                            double canvasWidth, double canvasHeight, double a)
            {
                using namespace juce::gl;

                const auto seconds = valueOf (layer, Property::time, sample, 0.0);
                const auto frame = movies.frame (layer.file, movies.frameAt (layer.file, seconds));

                if (frame == nullptr || ! frame->drawable() || frame->width <= 0)
                    return;

                auto& held = movieTextures[layer.file];
                held.used = true;

                const auto raw = ! frame->rgba.empty();
                const auto paddedWidth = raw ? frame->width : (frame->width + 3) / 4 * 4;
                const auto paddedHeight = raw ? frame->height : (frame->height + 3) / 4 * 4;

                if (held.id == 0)
                    glGenTextures (1, &held.id);

                glActiveTexture (GL_TEXTURE0);
                glBindTexture (GL_TEXTURE_2D, held.id);

                if (held.index != frame->index || held.width != frame->width)
                {
                    if (raw)
                    {
                        /*  A PREVIEW'S FRAME, as it is: RGBA, a row at a time. */
                        glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
                        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, frame->width, frame->height, 0,
                                      GL_RGBA, GL_UNSIGNED_BYTE, frame->rgba.data());
                    }
                    else
                    {
                        const auto format = frame->texture == hap::Texture::dxt1 ? GL_COMPRESSED_RGB_S3TC_DXT1_EXT
                                                                                 : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;

                        glCompressedTexImage2D (GL_TEXTURE_2D, 0, format, paddedWidth, paddedHeight, 0,
                                                static_cast<GLsizei> (frame->blocks.size()), frame->blocks.data());
                    }

                    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    held.index = frame->index;
                    held.width = frame->width;
                }

                const auto q = ! raw && frame->texture == hap::Texture::ycocgDxt5;

                if ((q ? movieQProgram : movieProgram) == nullptr)
                {
                    glBindTexture (GL_TEXTURE_2D, 0);
                    return;
                }

                (q ? movieQProgram : movieProgram)->use();
                (q ? movieQUniform : movieUniform)->set (0);
                (q ? movieQOpacity : movieOpacity)->set (static_cast<GLfloat> (a));
                setGrade (q ? movieQGrade : movieGrade, layer);

                /*  THE BLOCKS RUN FROM THE TOP ROW: the picture's top is the
                    texture's first row, its bottom `height` rows down. */
                drawQuad (placementOf (layer, sample, canvasWidth, canvasHeight,
                                       static_cast<double> (frame->width), static_cast<double> (frame->height)),
                          static_cast<double> (frame->width) / paddedWidth,
                          static_cast<double> (frame->height) / paddedHeight, 0.0);

                glBindTexture (GL_TEXTURE_2D, 0);
            }

            void openGLContextClosing() override
            {
                using namespace juce::gl;

                textures.clear();

                for (auto& held : movieTextures)
                    if (held.second.id != 0)
                        glDeleteTextures (1, &held.second.id);

                movieTextures.clear();
                maskUniforms.clear();
                maskProgram.reset();
                releaseCanvasTarget();

                if (warpArray != 0)
                {
                    glDeleteBuffers (1, &warpBuffer);
                    glDeleteBuffers (1, &warpIndices);
                    glDeleteVertexArrays (1, &warpArray);
                }

                warpArray = warpBuffer = warpIndices = 0;
                warpUniforms.clear();
                warpProgram.reset();

                for (auto& held : gradeTables)
                    if (held.second.id != 0)
                        glDeleteTextures (1, &held.second.id);

                gradeTables.clear();
                pictureGrade.reset();
                movieGrade.reset();
                movieQGrade.reset();
                movieUniform.reset();
                movieOpacity.reset();
                movieQUniform.reset();
                movieQOpacity.reset();
                movieProgram.reset();
                movieQProgram.reset();

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
            const MovieStore& movies;
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

            std::unique_ptr<juce::OpenGLShaderProgram> movieProgram, movieQProgram;
            std::unique_ptr<juce::OpenGLShaderProgram::Uniform> movieUniform, movieOpacity, movieQUniform, movieQOpacity;

            struct MovieTexture
            {
                GLuint id = 0;
                int index = -1;
                int width = 0;
                bool used = false;
            };

            std::map<std::string, MovieTexture> movieTextures;

            GradeUniforms pictureGrade, movieGrade, movieQGrade;

            std::unique_ptr<juce::OpenGLShaderProgram> maskProgram;
            std::map<std::string, std::unique_ptr<juce::OpenGLShaderProgram::Uniform>> maskUniforms;

            std::unique_ptr<juce::OpenGLShaderProgram> warpProgram;
            std::map<std::string, std::unique_ptr<juce::OpenGLShaderProgram::Uniform>> warpUniforms;
            GLuint canvasFrame = 0, canvasTexture = 0;
            int targetWidth = 0, targetHeight = 0;
            GLuint warpArray = 0, warpBuffer = 0, warpIndices = 0;
            Mesh meshDrawn;

            struct GradeTable
            {
                GLuint id = 0;
                bool used = false;
            };

            std::map<std::string, GradeTable> gradeTables;

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
                : r (regionToServe), parentPid (parentPidToWatch), windowed (windowedToUse), sampler (pictures, movies)
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

                std::map<std::string, int> moviesWanted;
                const auto now = clock.sampleAt (r, steadyNanos());

                for (const auto& layer : layers)
                {
                    if (layer.source == region::Source::picture && ! layer.file.empty())
                        wanted.insert (layer.file);

                    /*  A MOVIE AND THE FRAME ITS PLAYHEAD IS ON NOW - the store
                        reads from there on. */
                    if (layer.source == region::Source::movie && ! layer.file.empty())
                        moviesWanted[layer.file] = std::max (0, movies.frameAt (layer.file,
                                                                                 valueOf (layer, Property::time, std::max<std::int64_t> (now, 0), 0.0)));
                }

                movies.want (moviesWanted);

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

                /*  WHAT EACH PICTURE AND EACH CANVAS SHOWS, as one colour, a
                    tenth of a second apart (namespace draft §38, WR). */
                if (++tintTicks >= 10)
                {
                    tintTicks = 0;
                    writeTints (config, layers);
                }
            }

            /*  THE TINTS, through the compositor the probe uses, from the
                pictures and frames the stores already hold: each layer alone
                over black, and each canvas whole with its level. */
            void writeTints (const region::ConfigReading& config, const std::vector<region::LayerReading>& layers)
            {
                const auto now = clock.sampleAt (r, steadyNanos());

                if (now < 0)
                    return;

                std::size_t at = 0;

                for (const auto& layer : layers)
                {
                    if (at >= static_cast<std::size_t> (region::maxLayers))
                        break;

                    const auto* canvas = canvasIn (config, layer.canvas);

                    if (canvas == nullptr)
                        continue;

                    const std::vector<const region::LayerReading*> alone { &layer };
                    region::writeTint (r.layerTints[at++], layer.id,
                                       tintOf (alone, now, static_cast<double> (std::max (1, canvas->width)),
                                               static_cast<double> (std::max (1, canvas->height)), &sampler));
                }

                for (; at < static_cast<std::size_t> (region::maxLayers); ++at)
                    if (r.layerTints[at].id[0] != 0)
                        region::writeTint (r.layerTints[at], {}, 0);

                const auto count = std::min<std::size_t> (config.canvases.size(), region::maxCanvases);

                for (std::size_t n = 0; n < static_cast<std::size_t> (region::maxCanvases); ++n)
                {
                    if (n >= count)
                    {
                        if (r.canvasTints[n].id[0] != 0)
                            region::writeTint (r.canvasTints[n], {}, 0);

                        continue;
                    }

                    const auto& canvas = config.canvases[n];
                    region::writeTint (r.canvasTints[n], canvas.id,
                                       scaledColour (tintOf (stackOf (layers, canvas.id), now,
                                                             static_cast<double> (std::max (1, canvas.width)),
                                                             static_cast<double> (std::max (1, canvas.height)), &sampler),
                                                     region::canvasLevelOf (r, canvas.id)));
                }
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
                        windows.push_back (std::make_unique<OutputWindow> (r, pictures, movies, output.id, static_cast<int> (n),
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
                    r.probe[n].store (scaledColour (colourAt (stackOf (layers, canvas.id), now,
                                                              static_cast<double> (std::max (1, canvas.width)),
                                                              static_cast<double> (std::max (1, canvas.height)),
                                                              0.0, 0.0, &sampler),
                                                    region::canvasLevelOf (r, canvas.id)),
                                      std::memory_order_relaxed);
                }

                r.probeSample.store (now, std::memory_order_relaxed);
                region::endWrite (r.probeSeq);

            }

            region::Region& r;
            std::int64_t parentPid = 0;
            bool windowed = true;
            std::int64_t ticks = 0;
            int tintTicks = 0;
            std::uint32_t boundSeq = 0xffffffffu;

            std::vector<DisplayInfo> displays;
            PictureStore pictures;
            MovieStore movies;
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
