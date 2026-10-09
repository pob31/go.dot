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

#include <wfg/engine/video/render/Painter.h>

#include <wfg/engine/video/Compositor.h>
#include <wfg/engine/video/Geometry.h>
#include <wfg/engine/video/render/Gpu.h>

#include <sokol/sokol_gfx.h>
#include <wfg/engine/video/render/shaders/video.glsl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <set>
#include <tuple>

namespace wfg::video::render
{
    namespace
    {
        enum class Program : int { fill = 0, picture, movie, movieQ, mask, warp, count };

        /*  HOW A DRAW IS LAID ON WHAT IS UNDER IT: the layers' four (VD) on
            premultiplied colour - the compositor's formulas as equations - and
            none, for a pass that covers everything. */
        enum class Lay : int { none = 0, normal, add, screen, multiply };

        Lay layOf (region::Blend blend) noexcept
        {
            switch (blend)
            {
                case region::Blend::add:      return Lay::add;
                case region::Blend::screen:   return Lay::screen;
                case region::Blend::multiply: return Lay::multiply;
                case region::Blend::normal:   break;
            }

            return Lay::normal;
        }

        sg_blend_state blendOf (Lay lay) noexcept
        {
            sg_blend_state state {};
            state.enabled = lay != Lay::none;

            switch (lay)
            {
                case Lay::add:      state.src_factor_rgb = SG_BLENDFACTOR_ONE;       state.dst_factor_rgb = SG_BLENDFACTOR_ONE; break;
                case Lay::screen:   state.src_factor_rgb = SG_BLENDFACTOR_ONE;       state.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_COLOR; break;
                case Lay::multiply: state.src_factor_rgb = SG_BLENDFACTOR_DST_COLOR; state.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA; break;
                case Lay::normal:   state.src_factor_rgb = SG_BLENDFACTOR_ONE;       state.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA; break;
                case Lay::none:     break;
            }

            /*  ALPHA LAID NORMALLY whatever the colour does: Direct3D 11 takes
                no colour factor for alpha (screen's and multiply's would leave
                the pipeline unmade and the layer undrawn), and nothing reads a
                canvas's alpha - the warp takes its colour. */
            state.src_factor_alpha = SG_BLENDFACTOR_ONE;
            state.dst_factor_alpha = lay == Lay::add ? SG_BLENDFACTOR_ONE : SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            return state;
        }

        constexpr int meshSteps = 64;

        /*  A QUAD'S UNIFORMS: four corners as the target's coordinates, -1..1
            with y up, and the texel each shows - bottom-left, bottom-right,
            top-left, top-right. */
        video_corners_t quadFor (const double x[4], const double y[4], const double u[4], const double v[4]) noexcept
        {
            video_corners_t quad {};

            for (int n = 0; n < 2; ++n)
            {
                quad.corner_xy01[2 * n] = static_cast<float> (x[n]);
                quad.corner_xy01[2 * n + 1] = static_cast<float> (y[n]);
                quad.corner_xy23[2 * n] = static_cast<float> (x[n + 2]);
                quad.corner_xy23[2 * n + 1] = static_cast<float> (y[n + 2]);
                quad.corner_uv01[2 * n] = static_cast<float> (u[n]);
                quad.corner_uv01[2 * n + 1] = static_cast<float> (v[n]);
                quad.corner_uv23[2 * n] = static_cast<float> (u[n + 2]);
                quad.corner_uv23[2 * n + 1] = static_cast<float> (v[n + 2]);
            }

            return quad;
        }

        /*  A LAYER'S QUAD (the OpenGL renderer's drawQuad): its four corners on
            the canvas, with the texture's - `uMax` across, and `tBottom`, `tTop`
            where the picture's bottom and top rows are in its texture. */
        video_corners_t quadOf (const Placement& place, double uMax, double tBottom, double tTop) noexcept
        {
            constexpr double corners[4][2] { { -1.0, -1.0 }, { 1.0, -1.0 }, { -1.0, 1.0 }, { 1.0, 1.0 } };
            double x[4], y[4], u[4], v[4];

            for (int n = 0; n < 4; ++n)
            {
                double cx = 0.0, cy = 0.0;
                place.toCanvas (corners[n][0], corners[n][1], cx, cy);

                const auto across = (place.flipH ? -corners[n][0] : corners[n][0]) * 0.5 + 0.5;
                const auto up = (place.flipV ? -corners[n][1] : corners[n][1]) * 0.5 + 0.5;

                x[n] = cx / (0.5 * place.canvasWidth);
                y[n] = cy / (0.5 * place.canvasHeight);
                u[n] = across * uMax;
                v[n] = tBottom + up * (tTop - tBottom);
            }

            return quadFor (x, y, u, v);
        }

        /*  A rectangle of the target, -1..1 with y up, showing nothing in
            particular: a fill's, the test pattern's bars. */
        video_corners_t rectangle (double left, double bottom, double right, double top) noexcept
        {
            const double x[4] { left, right, left, right };
            const double y[4] { bottom, bottom, top, top };
            const double u[4] { 0.0, 1.0, 0.0, 1.0 };
            const double v[4] { 0.0, 0.0, 1.0, 1.0 };
            return quadFor (x, y, u, v);
        }

        sg_range rangeOf (const void* data, std::size_t size) noexcept
        {
            sg_range range {};
            range.ptr = data;
            range.size = size;
            return range;
        }

        template <typename Uniforms>
        void apply (int slot, const Uniforms& uniforms) noexcept
        {
            sg_apply_uniforms (slot, rangeOf (&uniforms, sizeof (uniforms)));
        }

        void release (sg_view& view) noexcept
        {
            if (view.id != SG_INVALID_ID)
                sg_destroy_view (view);

            view = {};
        }

        void release (sg_image& image) noexcept
        {
            if (image.id != SG_INVALID_ID)
                sg_destroy_image (image);

            image = {};
        }

        sg_view textureView (sg_image image) noexcept
        {
            sg_view_desc desc {};
            desc.texture.image = image;
            return sg_make_view (desc);
        }
    }

    //==============================================================================
    struct Painter::Impl
    {
        explicit Impl (const Sources& sourcesToRead) : sources (sourcesToRead) {}

        ~Impl()
        {
            if (! gpu::isOpen())
                return;

            for (auto& [key, held] : pictures)       { release (held.view); release (held.image); }
            for (auto& [key, held] : movies)         { release (held.view); release (held.image); }
            for (auto& [key, held] : tables)         { release (held.view); release (held.image); }
            for (auto& [key, held] : targets)        { release (held.texture); release (held.colour); release (held.image); }
            for (auto& [key, held] : meshes)         if (held.buffer.id != SG_INVALID_ID) sg_destroy_buffer (held.buffer);
            for (auto& [key, made] : pipelines)      sg_destroy_pipeline (made);

            release (identityTableView);
            release (identityTable);

            if (meshIndices.id != SG_INVALID_ID)  sg_destroy_buffer (meshIndices);
            if (linear.id != SG_INVALID_ID)       sg_destroy_sampler (linear);
            if (nearest.id != SG_INVALID_ID)      sg_destroy_sampler (nearest);

            for (auto& shader : shaders)
                if (shader.id != SG_INVALID_ID)
                    sg_destroy_shader (shader);
        }

        //==============================================================================
        bool make (std::string& why)
        {
            const auto backend = sg_query_backend();
            const sg_shader_desc* descs[] { video_fill_shader_desc (backend), video_picture_shader_desc (backend),
                                            video_movie_shader_desc (backend), video_movie_q_shader_desc (backend),
                                            video_mask_shader_desc (backend), video_warp_shader_desc (backend) };
            const char* names[] { "fill", "picture", "movie", "movie_q", "mask", "warp" };

            for (std::size_t n = 0; n < shaders.size(); ++n)
            {
                if (descs[n] == nullptr)
                {
                    why = "no shader for " + gpu::describe();
                    return false;
                }

                shaders[n] = sg_make_shader (descs[n]);

                if (sg_query_shader_state (shaders[n]) != SG_RESOURCESTATE_VALID)
                {
                    why = "the " + std::string (names[n]) + " shader would not compile on " + gpu::describe() + ": "
                        + gpu::lastMessage();
                    return false;
                }
            }

            sg_sampler_desc smooth {};
            smooth.min_filter = SG_FILTER_LINEAR;
            smooth.mag_filter = SG_FILTER_LINEAR;
            smooth.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
            smooth.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
            linear = sg_make_sampler (smooth);

            sg_sampler_desc exact {};
            exact.min_filter = SG_FILTER_NEAREST;
            exact.mag_filter = SG_FILTER_NEAREST;
            exact.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
            exact.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
            nearest = sg_make_sampler (exact);

            /*  THE TABLES A GRADE WITHOUT CURVES BINDS: the shader declares
                them whether it reads them or not. */
            std::uint8_t identity[256 * 4];

            for (std::size_t step = 0; step < 256; ++step)
                for (std::size_t channel = 0; channel < 4; ++channel)
                    identity[step * 4 + channel] = channel == 3 ? 255 : static_cast<std::uint8_t> (step);

            identityTable = makeTable (identity);
            identityTableView = textureView (identityTable);

            /*  THE MESH'S TRIANGLES, two a cell of a 64 by 64 grid, shared by
                every warp. */
            std::vector<std::uint32_t> indices;
            constexpr auto across = static_cast<std::uint32_t> (meshSteps) + 1u;

            for (std::uint32_t row = 0; row + 1u < across; ++row)
                for (std::uint32_t column = 0; column + 1u < across; ++column)
                {
                    const auto at = row * across + column;
                    indices.insert (indices.end(), { at, at + 1u, at + across, at + 1u, at + across + 1u, at + across });
                }

            sg_buffer_desc indexDesc {};
            indexDesc.usage.index_buffer = true;
            indexDesc.data = rangeOf (indices.data(), indices.size() * sizeof (std::uint32_t));
            meshIndices = sg_make_buffer (indexDesc);

            originTopLeft = gpu::originTopLeft();
            dxtReady = sg_query_pixelformat (SG_PIXELFORMAT_BC1_RGBA).sample && sg_query_pixelformat (SG_PIXELFORMAT_BC3_RGBA).sample;

            /*  A CANVAS IS SIXTEEN BITS A CHANNEL, 0..1: as fine as a slow fade
                to black needs before the dither takes it to eight, and clamped
                at white as a display is - so an added light saturates where the
                compositor says it does (VD). Half-floats, which do not clamp,
                only on a device that will not blend the other. */
            const auto unorm = sg_query_pixelformat (SG_PIXELFORMAT_RGBA16);
            canvasFormat = unorm.render && unorm.blend ? SG_PIXELFORMAT_RGBA16 : SG_PIXELFORMAT_RGBA16F;
            bgraReady = sg_query_pixelformat (SG_PIXELFORMAT_BGRA8).sample;
            return true;
        }

        sg_image makeTable (const std::uint8_t* texels)
        {
            sg_image_desc desc {};
            desc.width = 256;
            desc.height = 1;
            desc.pixel_format = SG_PIXELFORMAT_RGBA8;
            desc.data.mip_levels[0] = rangeOf (texels, 256 * 4);
            return sg_make_image (desc);
        }

        //==============================================================================
        /*  A PIPELINE: one program, one way of laying, one target format -
            made the first time it is asked for. */
        sg_pipeline pipeline (Program program, Lay lay, sg_pixel_format format)
        {
            const auto key = std::make_tuple (static_cast<int> (program), static_cast<int> (lay), static_cast<int> (format));

            if (const auto found = pipelines.find (key); found != pipelines.end())
                return found->second;

            sg_pipeline_desc desc {};
            desc.shader = shaders[static_cast<std::size_t> (program)];
            desc.colors[0].pixel_format = format;
            desc.colors[0].blend = blendOf (lay);
            desc.depth.pixel_format = SG_PIXELFORMAT_NONE;
            desc.sample_count = 1;
            desc.cull_mode = SG_CULLMODE_NONE;

            if (program == Program::warp)
            {
                desc.layout.attrs[ATTR_video_warp_position].format = SG_VERTEXFORMAT_FLOAT2;
                desc.layout.attrs[ATTR_video_warp_texel].format = SG_VERTEXFORMAT_FLOAT2;
                desc.index_type = SG_INDEXTYPE_UINT32;
                desc.primitive_type = SG_PRIMITIVETYPE_TRIANGLES;
            }
            else
            {
                //  A quad with no buffer: four vertices, a strip.
                desc.primitive_type = SG_PRIMITIVETYPE_TRIANGLE_STRIP;
            }

            const auto made = sg_make_pipeline (desc);
            pipelines[key] = made;
            return made;
        }

        //==============================================================================
        /*  AN OFFSCREEN PICTURE under `key`: its image, the view a pass draws
            into and the view a shader reads - made again at a new size. */
        struct Offscreen
        {
            sg_image image {};
            sg_view colour {};
            sg_view texture {};
            int width = 0;
            int height = 0;
            sg_pixel_format format = SG_PIXELFORMAT_NONE;
        };

        Offscreen& offscreen (const std::string& key, int width, int height, sg_pixel_format format)
        {
            auto& held = targets[key];

            if (held.image.id != SG_INVALID_ID && held.width == width && held.height == height && held.format == format)
                return held;

            release (held.texture);
            release (held.colour);
            release (held.image);

            sg_image_desc desc {};
            desc.usage.color_attachment = true;
            desc.width = width;
            desc.height = height;
            desc.pixel_format = format;
            desc.sample_count = 1;
            held.image = sg_make_image (desc);

            sg_view_desc colourDesc {};
            colourDesc.color_attachment.image = held.image;
            held.colour = sg_make_view (colourDesc);
            held.texture = textureView (held.image);
            held.width = width;
            held.height = height;
            held.format = format;
            return held;
        }

        static sg_pass passInto (sg_view colour)
        {
            sg_pass pass {};
            pass.attachments.colors[0] = colour;
            pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
            pass.action.colors[0].clear_value = { 0.0f, 0.0f, 0.0f, 1.0f };
            return pass;
        }

        static sg_pass passInto (const Target& into)
        {
            if (into.colour.id != SG_INVALID_ID)
                return passInto (into.colour);

            sg_pass pass {};
            pass.swapchain = into.swapchain;
            pass.swapchain.width = into.width;
            pass.swapchain.height = into.height;
            pass.swapchain.color_format = into.format;
            pass.swapchain.depth_format = SG_PIXELFORMAT_NONE;
            pass.swapchain.sample_count = 1;
            pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
            pass.action.colors[0].clear_value = { 0.0f, 0.0f, 0.0f, 1.0f };
            return pass;
        }

        //==============================================================================
        /*  A PICTURE ON THE DEVICE, made or made again when the store has a
            newer reading: its rows as they are, BGRA where the device reads it
            (Direct3D, Metal), turned to RGBA where it does not (OpenGL). */
        struct HeldPicture
        {
            sg_image image {};
            sg_view view {};
            std::uint64_t version = 0;
            int width = 0;
            int height = 0;
            bool used = false;
        };

        HeldPicture* picture (const std::string& path)
        {
            std::uint64_t version = 0;
            const auto image = sources.picture (path, version);

            if (! image.isValid() || image.getFormat() != juce::Image::ARGB)
                return nullptr;

            auto& held = pictures[path];

            if (held.image.id == SG_INVALID_ID || held.version != version)
            {
                release (held.view);
                release (held.image);

                const auto width = image.getWidth();
                const auto height = image.getHeight();
                const juce::Image::BitmapData bitmap (image, juce::Image::BitmapData::readOnly);
                std::vector<std::uint8_t> texels (static_cast<std::size_t> (width) * static_cast<std::size_t> (height) * 4);

                for (int y = 0; y < height; ++y)
                {
                    const auto* row = bitmap.getLinePointer (y);
                    auto* out = texels.data() + static_cast<std::size_t> (y) * static_cast<std::size_t> (width) * 4;

                    if (bgraReady)
                    {
                        std::memcpy (out, row, static_cast<std::size_t> (width) * 4);
                        continue;
                    }

                    for (int x = 0; x < width; ++x)
                    {
                        const auto* bgra = row + 4 * x;
                        out[4 * x + 0] = bgra[2];
                        out[4 * x + 1] = bgra[1];
                        out[4 * x + 2] = bgra[0];
                        out[4 * x + 3] = bgra[3];
                    }
                }

                sg_image_desc desc {};
                desc.width = width;
                desc.height = height;
                desc.pixel_format = bgraReady ? SG_PIXELFORMAT_BGRA8 : SG_PIXELFORMAT_RGBA8;
                desc.data.mip_levels[0] = rangeOf (texels.data(), texels.size());
                held.image = sg_make_image (desc);
                held.view = textureView (held.image);
                held.version = version;
                held.width = width;
                held.height = height;
            }

            held.used = true;
            return &held;
        }

        /*  THE READ-AHEAD'S STILLS (namespace draft §48, AAT): kept, and one
            more put on the device a frame, within the budget. */
        void keepPictures (const std::vector<std::string>& paths, std::size_t budget)
        {
            settled.clear();
            std::size_t heldBytes = 0;
            auto uploaded = false;

            for (const auto& path : paths)
            {
                std::uint64_t version = 0;
                const auto image = sources.picture (path, version);

                //  Not read yet: nothing to put on the device.
                if (! image.isValid() || image.getFormat() != juce::Image::ARGB)
                    continue;

                const auto bytes = static_cast<std::size_t> (image.getWidth()) * static_cast<std::size_t> (image.getHeight()) * 4;

                //  Past the budget: drawn, it is uploaded then - settled as read.
                if (heldBytes + bytes > budget)
                {
                    settled.insert (path);
                    continue;
                }

                if (const auto found = pictures.find (path);
                    found != pictures.end() && found->second.image.id != SG_INVALID_ID && found->second.version == version)
                {
                    found->second.used = true;
                    heldBytes += bytes;
                    settled.insert (path);
                    continue;
                }

                if (uploaded)
                    continue;

                if (picture (path) != nullptr)
                {
                    uploaded = true;
                    ++uploadedAhead;
                    heldBytes += bytes;
                    settled.insert (path);
                }
            }
        }

        std::set<std::string> settled;
        std::uint64_t uploadedAhead = 0;

        /*  A MOVIE'S FRAME ON THE DEVICE, uploaded still compressed when it is
            not the one already there - one upload a frame of the movie, however
            many outputs show it. */
        struct HeldMovie
        {
            sg_image image {};
            sg_view view {};
            int index = -1;
            int width = 0;
            int height = 0;
            int paddedWidth = 0;
            int paddedHeight = 0;
            bool raw = false;
            bool q = false;
            bool used = false;
        };

        HeldMovie* movie (const std::string& path, const MovieFrame& frame)
        {
            auto& held = movies[path];
            held.used = true;

            const auto raw = ! frame.rgba.empty();

            if (held.image.id != SG_INVALID_ID && held.index == frame.index && held.width == frame.width && held.raw == raw)
                return &held;

            if (! raw && ! dxtReady)
                return nullptr;

            release (held.view);
            release (held.image);

            const auto paddedWidth = raw ? frame.width : (frame.width + 3) / 4 * 4;
            const auto paddedHeight = raw ? frame.height : (frame.height + 3) / 4 * 4;
            const auto format = raw ? SG_PIXELFORMAT_RGBA8
                                    : (frame.texture == hap::Texture::dxt1 ? SG_PIXELFORMAT_BC1_RGBA : SG_PIXELFORMAT_BC3_RGBA);
            const auto& bytes = raw ? frame.rgba : frame.blocks;
            const auto expected = static_cast<std::size_t> (sg_query_surface_pitch (format, paddedWidth, paddedHeight, 1));

            if (bytes.size() < expected)
                return nullptr;

            sg_image_desc desc {};
            desc.width = paddedWidth;
            desc.height = paddedHeight;
            desc.pixel_format = format;
            desc.data.mip_levels[0] = rangeOf (bytes.data(), expected);
            held.image = sg_make_image (desc);
            held.view = textureView (held.image);
            held.index = frame.index;
            held.width = frame.width;
            held.height = frame.height;
            held.paddedWidth = paddedWidth;
            held.paddedHeight = paddedHeight;
            held.raw = raw;
            held.q = ! raw && frame.texture == hap::Texture::ycocgDxt5;
            return &held;
        }

        /*  A LAYER'S CURVES, baked on its side and uploaded here (VV): made
            again when the bytes change - a playing cue's curves edited (§47,
            AAE) - and never otherwise. */
        struct HeldTable
        {
            sg_image image {};
            sg_view view {};
            std::array<std::uint8_t, 256 * 4> texels {};
            bool used = false;
        };

        sg_view tableFor (const region::LayerReading& layer)
        {
            if (! layer.grade.hasCurves)
                return identityTableView;

            auto& held = tables[layer.id];
            held.used = true;

            std::array<std::uint8_t, 256 * 4> texels {};

            for (std::size_t step = 0; step < 256; ++step)
            {
                for (std::size_t channel = 0; channel < 3; ++channel)
                    texels[step * 4 + channel] = layer.grade.tables[channel][step];

                texels[step * 4 + 3] = 255;
            }

            if (held.image.id == SG_INVALID_ID || texels != held.texels)
            {
                release (held.view);
                release (held.image);
                held.image = makeTable (texels.data());
                held.view = textureView (held.image);
                held.texels = texels;
            }

            return held.view;
        }

        /*  NO GRADE, at an opacity: what came back through an insert, its
            grade already in it, drawn opaque as a capture is. */
        static video_grade_t neutralGrade (double opacity) noexcept
        {
            video_grade_t uniforms {};
            uniforms.grade_a[0] = 1.0f;
            uniforms.grade_a[1] = 1.0f;
            uniforms.grade_a[2] = 1.0f;
            uniforms.grade_b[0] = 1.0f;
            uniforms.grade_b[2] = static_cast<float> (opacity);
            uniforms.grade_b[3] = 1.0f;
            return uniforms;
        }

        /*  A LAYER'S PICTURE'S OWN SIZE: a picture's, a movie frame's, an
            input's - and a fill's or a mask's, its canvas's. What an insert
            sends it at, and where black stands for it. */
        void sourceSizeOf (const region::LayerReading& layer, std::int64_t sample, int& width, int& height)
        {
            width = 0;
            height = 0;

            if (layer.source == region::Source::picture)
            {
                if (const auto* held = picture (layer.file))
                {
                    width = held->width;
                    height = held->height;
                }
            }
            else if (layer.source == region::Source::movie)
            {
                if (const auto frame = sources.movieFrame (layer.file, valueOf (layer, Property::time, sample, 0.0)))
                {
                    width = frame->width;
                    height = frame->height;
                }
            }
            else if (layer.source == region::Source::capture)
            {
                if (const auto found = inputs.find (layer.input); found != inputs.end())
                {
                    width = found->second.width;
                    height = found->second.height;
                }
            }

            if (width <= 0 || height <= 0)
                for (const auto& canvasReading : config.canvases)
                    if (canvasReading.id == layer.canvas)
                    {
                        width = std::max (1, canvasReading.width);
                        height = std::max (1, canvasReading.height);
                    }

            width = std::max (1, width);
            height = std::max (1, height);
        }

        /*  THE CUE'S PICTURE ALONE (YF), filling `width` by `height`: its
            source, graded, at full opacity - no geometry, no blend. */
        bool drawAlone (const region::LayerReading& layer, std::int64_t sample, Offscreen& into)
        {
            const auto format = into.format;
            const auto width = static_cast<double> (into.width);
            const auto height = static_cast<double> (into.height);

            Placement whole;
            whole.canvasWidth = whole.pictureWidth = width;
            whole.canvasHeight = whole.pictureHeight = height;
            whole.fit = static_cast<int> (region::Fit::stretch);

            sg_begin_pass (passInto (into.colour));
            auto drawn = true;

            if (layer.source == region::Source::fill || layer.source == region::Source::mask)
            {
                const auto colour = [&layer] (float* into4)
                {
                    into4[0] = static_cast<float> ((layer.paint >> 16) & 0xffu) / 255.0f;
                    into4[1] = static_cast<float> ((layer.paint >> 8) & 0xffu) / 255.0f;
                    into4[2] = static_cast<float> (layer.paint & 0xffu) / 255.0f;
                    into4[3] = 1.0f;
                };

                if (layer.source == region::Source::fill)
                {
                    sg_apply_pipeline (pipeline (Program::fill, Lay::none, format));
                    apply (UB_video_corners, quadOf (whole, 1.0, 0.0, 1.0));
                    video_fill_t fill {};
                    colour (fill.colour);
                    apply (UB_video_fill, fill);
                }
                else
                {
                    sg_apply_pipeline (pipeline (Program::mask, Lay::normal, format));
                    apply (UB_video_corners, quadOf (whole, 1.0, 0.0, 1.0));
                    video_mask_t uniforms {};
                    colour (uniforms.colour);
                    uniforms.shape[0] = static_cast<float> (width);
                    uniforms.shape[1] = static_cast<float> (height);
                    uniforms.shape[2] = layer.shape.feather;
                    uniforms.shape[3] = layer.shape.invert ? 1.0f : 0.0f;
                    uniforms.count[0] = static_cast<float> (layer.shape.count);

                    for (int n = 0; n < layer.shape.count && n < mask::maxPoints; ++n)
                    {
                        uniforms.corners[n / 2][(n % 2) * 2] = layer.shape.x[n];
                        uniforms.corners[n / 2][(n % 2) * 2 + 1] = layer.shape.y[n];
                    }

                    apply (UB_video_mask, uniforms);
                }

                sg_draw (0, 4, 1);
            }
            else if (layer.source == region::Source::picture)
            {
                auto* held = picture (layer.file);
                drawn = held != nullptr;

                if (drawn)
                {
                    sg_apply_pipeline (pipeline (Program::picture, Lay::normal, format));
                    bindPicture (held->view, layer);
                    apply (UB_video_corners, quadOf (whole, 1.0, 1.0, 0.0));
                    apply (UB_video_grade, gradeOf (layer, 1.0));
                    sg_draw (0, 4, 1);
                }
            }
            else if (layer.source == region::Source::movie)
            {
                const auto frame = sources.movieFrame (layer.file, valueOf (layer, Property::time, sample, 0.0));
                auto* held = frame != nullptr && frame->drawable() ? movie (layer.file, *frame) : nullptr;
                drawn = held != nullptr;

                if (drawn)
                {
                    sg_apply_pipeline (pipeline (held->q ? Program::movieQ : Program::movie, Lay::normal, format));
                    bindPicture (held->view, layer);
                    apply (UB_video_corners, quadOf (whole, static_cast<double> (held->width) / held->paddedWidth,
                                                  static_cast<double> (held->height) / held->paddedHeight, 0.0));
                    apply (UB_video_grade, gradeOf (layer, 1.0));
                    sg_draw (0, 4, 1);
                }
            }
            else if (layer.source == region::Source::capture)
            {
                const auto found = inputs.find (layer.input);
                drawn = found != inputs.end() && found->second.view.id != SG_INVALID_ID;

                if (drawn)
                {
                    sg_apply_pipeline (pipeline (Program::movie, Lay::normal, format));
                    bindPicture (found->second.view, layer);
                    apply (UB_video_corners, quadOf (whole, 1.0, 1.0, 0.0));
                    apply (UB_video_grade, gradeOf (layer, 1.0, true));
                    sg_draw (0, 4, 1);
                }
            }

            sg_end_pass();
            return drawn;
        }

        static video_grade_t gradeOf (const region::LayerReading& layer, double opacity, bool opaque = false) noexcept
        {
            const auto& grade = layer.grade;
            const auto turn = grade.hue * 3.14159265358979323846 / 180.0;

            video_grade_t uniforms {};
            uniforms.grade_a[0] = static_cast<float> (std::max (0.01, grade.gamma));
            uniforms.grade_a[1] = static_cast<float> (grade.contrast / 100.0);
            uniforms.grade_a[2] = static_cast<float> (grade.saturation / 100.0);
            uniforms.grade_a[3] = grade.hasCurves ? 1.0f : 0.0f;
            uniforms.grade_b[0] = static_cast<float> (std::cos (turn));
            uniforms.grade_b[1] = static_cast<float> (std::sin (turn));
            uniforms.grade_b[2] = static_cast<float> (opacity);
            uniforms.grade_b[3] = opaque ? 1.0f : 0.0f;
            return uniforms;
        }

        //==============================================================================
        /*  ONE CANVAS'S LAYERS, bottom first, into its own picture. */
        void composite (const std::string& canvasId, const region::CanvasReading& canvas, std::int64_t sample, Offscreen& into)
        {
            const auto canvasWidth = static_cast<double> (std::max (1, canvas.width));
            const auto canvasHeight = static_cast<double> (std::max (1, canvas.height));
            const auto format = canvasFormat;

            sg_begin_pass (passInto (into.colour));

            for (const auto* layer : sample < 0 ? std::vector<const region::LayerReading*> {} : stackOf (layers, canvasId))
            {
                const auto a = opacityOf (*layer, sample);

                if (! (a > 0.0))
                    continue;

                const auto lay = layOf (layer->blend);

                if (! layer->insert.empty())
                {
                    /*  THROUGH AN INSERT (§44, YE): what came back, placed,
                        faded and blended as the cue says - its grade already in
                        it (YF). Black where it would be while nothing comes
                        back, or while a later cue holds the insert (YG). */
                    const auto returned = returns.find (layer->insert);
                    const auto holder = holders.find (layer->insert);
                    const auto holds = holder != holders.end() && holder->second == layer;

                    if (holds && returned != returns.end() && returned->second.view.id != SG_INVALID_ID
                          && returned->second.width > 0 && returned->second.height > 0)
                    {
                        const auto& back = returned->second;
                        sg_apply_pipeline (pipeline (Program::movie, lay, format));
                        sg_bindings bindings {};
                        bindings.views[VIEW_video_picture] = back.view;
                        bindings.samplers[SMP_video_picture_smp] = linear;
                        bindings.views[VIEW_video_grade_tables] = identityTableView;
                        bindings.samplers[SMP_video_grade_smp] = nearest;
                        sg_apply_bindings (bindings);
                        apply (UB_video_corners, quadOf (placementOf (*layer, sample, canvasWidth, canvasHeight,
                                                                   static_cast<double> (back.width), static_cast<double> (back.height)),
                                                      1.0, 1.0, 0.0));
                        apply (UB_video_grade, neutralGrade (a));
                        sg_draw (0, 4, 1);
                    }
                    else
                    {
                        int w = 0, h = 0;
                        sourceSizeOf (*layer, sample, w, h);
                        sg_apply_pipeline (pipeline (Program::fill, lay, format));
                        apply (UB_video_corners, quadOf (placementOf (*layer, sample, canvasWidth, canvasHeight,
                                                                   static_cast<double> (w), static_cast<double> (h)),
                                                      1.0, 0.0, 1.0));
                        video_fill_t black {};
                        black.colour[3] = static_cast<float> (a);
                        apply (UB_video_fill, black);
                        sg_draw (0, 4, 1);
                    }
                }
                else if (layer->source == region::Source::fill)
                {
                    sg_apply_pipeline (pipeline (Program::fill, lay, format));
                    apply (UB_video_corners, quadOf (placementOf (*layer, sample, canvasWidth, canvasHeight, canvasWidth, canvasHeight),
                                                  1.0, 0.0, 1.0));
                    video_fill_t fill {};
                    fill.colour[0] = static_cast<float> ((layer->paint >> 16) & 0xffu) / 255.0f;
                    fill.colour[1] = static_cast<float> ((layer->paint >> 8) & 0xffu) / 255.0f;
                    fill.colour[2] = static_cast<float> (layer->paint & 0xffu) / 255.0f;
                    fill.colour[3] = static_cast<float> (a);
                    apply (UB_video_fill, fill);
                    sg_draw (0, 4, 1);
                }
                else if (layer->source == region::Source::picture)
                {
                    auto* held = picture (layer->file);

                    if (held == nullptr)
                        continue;

                    /*  ITS ROWS FROM THE TOP: the picture's bottom is the
                        texture's last row. */
                    sg_apply_pipeline (pipeline (Program::picture, lay, format));
                    bindPicture (held->view, *layer);
                    apply (UB_video_corners, quadOf (placementOf (*layer, sample, canvasWidth, canvasHeight,
                                                               static_cast<double> (held->width), static_cast<double> (held->height)),
                                                  1.0, 1.0, 0.0));
                    apply (UB_video_grade, gradeOf (*layer, a));
                    sg_draw (0, 4, 1);
                }
                else if (layer->source == region::Source::movie)
                {
                    const auto seconds = valueOf (*layer, Property::time, sample, 0.0);
                    const auto frame = sources.movieFrame (layer->file, seconds);

                    if (frame == nullptr || ! frame->drawable() || frame->width <= 0 || frame->height <= 0)
                        continue;

                    auto* held = movie (layer->file, *frame);

                    if (held == nullptr)
                        continue;

                    sg_apply_pipeline (pipeline (held->q ? Program::movieQ : Program::movie, lay, format));
                    bindPicture (held->view, *layer);

                    /*  THE BLOCKS RUN FROM THE TOP ROW: the picture's top is
                        the texture's first row, its bottom `height` rows down. */
                    apply (UB_video_corners, quadOf (placementOf (*layer, sample, canvasWidth, canvasHeight,
                                                               static_cast<double> (held->width), static_cast<double> (held->height)),
                                                  static_cast<double> (held->width) / held->paddedWidth,
                                                  static_cast<double> (held->height) / held->paddedHeight, 0.0));
                    apply (UB_video_grade, gradeOf (*layer, a));
                    sg_draw (0, 4, 1);
                }
                else if (layer->source == region::Source::capture)
                {
                    /*  A CAPTURE (§44, YC): its input's newest picture, rows
                        from the top, drawn as a movie's frame is but opaque -
                        a sender's fourth byte is often anything. Nothing while
                        none has arrived. */
                    const auto found = inputs.find (layer->input);

                    if (found == inputs.end() || found->second.view.id == SG_INVALID_ID
                          || found->second.width <= 0 || found->second.height <= 0)
                        continue;

                    const auto& in = found->second;
                    sg_apply_pipeline (pipeline (Program::movie, lay, format));
                    bindPicture (in.view, *layer);
                    apply (UB_video_corners, quadOf (placementOf (*layer, sample, canvasWidth, canvasHeight,
                                                               static_cast<double> (in.width), static_cast<double> (in.height)),
                                                  1.0, 1.0, 0.0));
                    apply (UB_video_grade, gradeOf (*layer, a, true));
                    sg_draw (0, 4, 1);
                }
                else if (layer->source == region::Source::mask)
                {
                    sg_apply_pipeline (pipeline (Program::mask, lay, format));
                    apply (UB_video_corners, quadOf (placementOf (*layer, sample, canvasWidth, canvasHeight, canvasWidth, canvasHeight),
                                                  1.0, 0.0, 1.0));

                    video_mask_t uniforms {};
                    uniforms.colour[0] = static_cast<float> ((layer->paint >> 16) & 0xffu) / 255.0f;
                    uniforms.colour[1] = static_cast<float> ((layer->paint >> 8) & 0xffu) / 255.0f;
                    uniforms.colour[2] = static_cast<float> (layer->paint & 0xffu) / 255.0f;
                    uniforms.colour[3] = static_cast<float> (a);
                    uniforms.shape[0] = static_cast<float> (canvasWidth);
                    uniforms.shape[1] = static_cast<float> (canvasHeight);
                    uniforms.shape[2] = layer->shape.feather;
                    uniforms.shape[3] = layer->shape.invert ? 1.0f : 0.0f;
                    uniforms.count[0] = static_cast<float> (layer->shape.count);

                    for (int n = 0; n < layer->shape.count && n < mask::maxPoints; ++n)
                    {
                        uniforms.corners[n / 2][(n % 2) * 2] = layer->shape.x[n];
                        uniforms.corners[n / 2][(n % 2) * 2 + 1] = layer->shape.y[n];
                    }

                    apply (UB_video_mask, uniforms);
                    sg_draw (0, 4, 1);
                }
            }

            /*  THE CANVAS'S LEVEL (namespace draft §38, WT): the whole
                composite taken towards black, once, over every layer. */
            if (const auto level = levelOf ? levelOf (canvasId) : 1.0; level < 1.0)
            {
                sg_apply_pipeline (pipeline (Program::fill, Lay::normal, format));
                apply (UB_video_corners, rectangle (-1.0, -1.0, 1.0, 1.0));
                video_fill_t fill {};
                fill.colour[3] = static_cast<float> (1.0 - level);
                apply (UB_video_fill, fill);
                sg_draw (0, 4, 1);
            }

            sg_end_pass();
        }

        void bindPicture (sg_view view, const region::LayerReading& layer)
        {
            sg_bindings bindings {};
            bindings.views[VIEW_video_picture] = view;
            bindings.samplers[SMP_video_picture_smp] = linear;
            bindings.views[VIEW_video_grade_tables] = tableFor (layer);
            bindings.samplers[SMP_video_grade_smp] = nearest;
            sg_apply_bindings (bindings);
        }

        //==============================================================================
        /*  A PICTURE THROUGH A MESH: the 65 by 65 points where the mesh sends
            each point of the grid (Mapping.h), made again only when the mesh
            changes - one geometry a pass, under `key`. */
        struct HeldMesh
        {
            sg_buffer buffer {};
            Mesh drawn;
            bool used = false;
        };

        sg_buffer meshFor (const std::string& key, const Mesh& mesh)
        {
            auto& held = meshes[key];
            held.used = true;

            if (held.buffer.id != SG_INVALID_ID && held.drawn.columns == mesh.columns && held.drawn.rows == mesh.rows
                  && held.drawn.x == mesh.x && held.drawn.y == mesh.y)
                return held.buffer;

            if (held.buffer.id != SG_INVALID_ID)
                sg_destroy_buffer (held.buffer);

            std::vector<float> vertices;
            vertices.reserve (static_cast<std::size_t> (4 * (meshSteps + 1) * (meshSteps + 1)));

            for (int row = 0; row <= meshSteps; ++row)
                for (int column = 0; column <= meshSteps; ++column)
                {
                    const auto s = static_cast<double> (column) / meshSteps;
                    const auto t = static_cast<double> (row) / meshSteps;
                    double x = 0.0, y = 0.0;
                    meshAt (mesh, s, t, x, y);

                    //  Where it lands, -1..1 with y up; which point of the picture, t down.
                    vertices.push_back (static_cast<float> (2.0 * x - 1.0));
                    vertices.push_back (static_cast<float> (1.0 - 2.0 * y));
                    vertices.push_back (static_cast<float> (s));
                    vertices.push_back (static_cast<float> (t));
                }

            sg_buffer_desc desc {};
            desc.usage.vertex_buffer = true;
            desc.data = rangeOf (vertices.data(), vertices.size() * sizeof (float));
            held.buffer = sg_make_buffer (desc);
            held.drawn = mesh;
            return held.buffer;
        }

        /*  THE WARP'S DRAW, in a pass already begun: `texture` through `mesh`,
            calibrated by `cdl` or laid by `opacity`. */
        void warp (const std::string& meshKey, const Mesh& mesh, sg_view texture, sg_pixel_format format, Lay lay,
                   const Cdl* cdl, double opacity)
        {
            sg_apply_pipeline (pipeline (Program::warp, lay, format));

            sg_bindings bindings {};
            bindings.vertex_buffers[0] = meshFor (meshKey, mesh);
            bindings.index_buffer = meshIndices;
            bindings.views[VIEW_video_canvas] = texture;
            bindings.samplers[SMP_video_canvas_smp] = linear;
            sg_apply_bindings (bindings);

            video_warp_t uniforms {};

            if (cdl != nullptr)
                for (int n = 0; n < 3; ++n)
                {
                    uniforms.slope[n] = static_cast<float> (cdl->slope[n]);
                    uniforms.offset[n] = static_cast<float> (cdl->offset[n]);
                    uniforms.power[n] = static_cast<float> (cdl->power[n]);
                }

            uniforms.misc[0] = cdl != nullptr ? static_cast<float> (cdl->saturation) : 1.0f;
            uniforms.misc[1] = static_cast<float> (opacity);
            uniforms.misc[2] = cdl != nullptr ? 1.0f : 0.0f;
            uniforms.misc[3] = originTopLeft ? 0.0f : 1.0f;
            apply (UB_video_warp, uniforms);
            sg_draw (0, meshSteps * meshSteps * 6, 1);
        }

        /*  WHICH PROJECTOR IS WHICH: a white frame round the edge and a cross
            through the middle - thin fills, in the display's own pixels. */
        void testPattern (int width, int height, sg_pixel_format format)
        {
            const auto line = std::max (2, std::min (width, height) / 200);
            const auto bar = [&] (int x, int y, int w, int h)
            {
                const auto ndcX = [width] (int px) { return 2.0 * px / std::max (1, width) - 1.0; };
                const auto ndcY = [height] (int py) { return 2.0 * py / std::max (1, height) - 1.0; };
                apply (UB_video_corners, rectangle (ndcX (x), ndcY (y), ndcX (x + w), ndcY (y + h)));
                video_fill_t fill {};
                fill.colour[0] = fill.colour[1] = fill.colour[2] = fill.colour[3] = 1.0f;
                apply (UB_video_fill, fill);
                sg_draw (0, 4, 1);
            };

            sg_apply_pipeline (pipeline (Program::fill, Lay::none, format));
            bar (0, 0, width, line);
            bar (0, height - line, width, line);
            bar (0, 0, line, height);
            bar (width - line, 0, line, height);
            bar (0, height / 2 - line / 2, width, line);
            bar (width / 2 - line / 2, 0, line, height);
        }

        //==============================================================================
        sg_view canvas (const std::string& id, std::int64_t sample)
        {
            const region::CanvasReading* reading = nullptr;

            for (const auto& canvasReading : config.canvases)
                if (canvasReading.id == id)
                    reading = &canvasReading;

            if (reading == nullptr)
                return {};

            auto& held = offscreen ("canvas:" + id, std::max (1, reading->width), std::max (1, reading->height),
                                    canvasFormat);

            if (const auto drawn = drawnAt.find (id); drawn == drawnAt.end() || drawn->second != sample)
            {
                composite (id, *reading, sample, held);
                drawnAt[id] = sample;
            }

            usedTargets.insert ("canvas:" + id);
            return held.texture;
        }

        void drawOutput (const region::OutputReading& output, std::int64_t sample, const Target& into)
        {
            const auto width = std::max (1, into.viewWidth);
            const auto height = std::max (1, into.viewHeight);

            //  A zoned output's own canvas and its zones, bottom first (§40, WY).
            struct Pass
            {
                sg_view texture;
                Lay lay;
                double opacity;
                const Mesh* mesh;
            };

            std::vector<Pass> passes;

            if (const auto own = canvas (output.canvas, sample); own.id != SG_INVALID_ID)
                passes.push_back ({ own, Lay::normal, 1.0, &output.mesh });

            for (const auto& zone : output.zones)
                if (zone.opacity > 0.0)
                    if (const auto texture = canvas (zone.canvas, sample); texture.id != SG_INVALID_ID)
                        passes.push_back ({ texture, layOf (zone.blend), zone.opacity, &zone.mesh });

            sg_view calibrated {};
            const Mesh* calibratedMesh = &output.mesh;

            /*  ZONES are laid on an offscreen picture of the whole output, in
                display space, and that calibrated once (XF). */
            if (! output.zones.empty())
            {
                auto& whole = offscreen ("output:" + output.id, width, height, canvasFormat);
                usedTargets.insert ("output:" + output.id);
                sg_begin_pass (passInto (whole.colour));

                for (std::size_t n = 0; n < passes.size(); ++n)
                    warp (output.id + "#" + std::to_string (n), *passes[n].mesh, passes[n].texture, canvasFormat,
                          passes[n].lay, nullptr, passes[n].opacity);

                sg_end_pass();
                calibrated = whole.texture;
                calibratedMesh = &identityMesh;
            }
            else if (! passes.empty())
            {
                calibrated = passes.front().texture;
            }

            sg_begin_pass (passInto (into));
            sg_apply_viewport (into.viewX, into.viewY, width, height, true);

            if (calibrated.id != SG_INVALID_ID)
                warp (output.id + "#out", *calibratedMesh, calibrated, into.format, Lay::none, &output.cdl, 1.0);

            if (output.testPattern)
                testPattern (width, height, into.format);

            sg_end_pass();
        }

        void endFrame()
        {
            const auto sweep = [] (auto& held, auto&& letGo)
            {
                for (auto at = held.begin(); at != held.end();)
                {
                    if (at->second.used)
                    {
                        at->second.used = false;
                        ++at;
                        continue;
                    }

                    letGo (at->second);
                    at = held.erase (at);
                }
            };

            sweep (pictures, [] (HeldPicture& held) { release (held.view); release (held.image); });
            sweep (movies, [] (HeldMovie& held) { release (held.view); release (held.image); });
            sweep (tables, [] (HeldTable& held) { release (held.view); release (held.image); });
            sweep (meshes, [] (HeldMesh& held) { if (held.buffer.id != SG_INVALID_ID) sg_destroy_buffer (held.buffer); });

            /*  A canvas or a zoned output's picture is kept while it is drawn,
                and a picture made for a test or a sender while it is asked for. */
            for (auto at = targets.begin(); at != targets.end();)
            {
                const auto ownKey = at->first.rfind ("own:", 0) == 0;

                if (ownKey || usedTargets.count (at->first) > 0)
                {
                    ++at;
                    continue;
                }

                release (at->second.texture);
                release (at->second.colour);
                release (at->second.image);
                at = targets.erase (at);
            }

            usedTargets.clear();
        }

        //==============================================================================
        const Sources& sources;
        region::ConfigReading config;
        std::vector<region::LayerReading> layers;
        std::function<double (const std::string&)> levelOf;
        std::map<std::string, std::int64_t> drawnAt;
        std::set<std::string> usedTargets;

        std::array<sg_shader, static_cast<std::size_t> (Program::count)> shaders {};
        std::map<std::tuple<int, int, int>, sg_pipeline> pipelines;
        sg_sampler linear {}, nearest {};
        sg_image identityTable {};
        sg_view identityTableView {};
        sg_buffer meshIndices {};
        const Mesh identityMesh = Mesh::identity();

        std::map<std::string, Offscreen> targets;
        std::map<std::string, HeldPicture> pictures;
        std::map<std::string, HeldMovie> movies;
        std::map<std::string, HeldTable> tables;
        std::map<std::string, HeldMesh> meshes;

        struct InputPicture
        {
            sg_view view {};
            int width = 0;
            int height = 0;
        };

        std::map<std::string, InputPicture> inputs;
        std::map<std::string, InputPicture> returns;

        /*  WHICH CUE HOLDS EACH INSERT this frame (YG): the latest brought up
            of those going through it - and how many go through it. */
        std::map<std::string, const region::LayerReading*> holders;
        std::map<std::string, int> users;

        bool originTopLeft = true;
        sg_pixel_format canvasFormat = SG_PIXELFORMAT_RGBA16;
        bool dxtReady = false;
        bool bgraReady = false;
    };

    //==============================================================================
    Painter::Painter (const Sources& sourcesToRead) : impl (std::make_unique<Impl> (sourcesToRead)) {}
    Painter::~Painter() = default;

    bool Painter::make (std::string& why)
    {
        return impl->make (why);
    }

    void Painter::setInputPicture (const std::string& inputId, sg_view picture, int width, int height)
    {
        if (picture.id == SG_INVALID_ID)
        {
            impl->inputs.erase (inputId);
            return;
        }

        impl->inputs[inputId] = { picture, width, height };
    }

    void Painter::beginFrame (const region::ConfigReading& config, std::vector<region::LayerReading> layers,
                              std::function<double (const std::string&)> levelOf)
    {
        impl->config = config;
        impl->layers = std::move (layers);
        impl->levelOf = std::move (levelOf);
        impl->drawnAt.clear();

        //  Of the cues through each insert, the later brought up holds it (YG).
        impl->holders.clear();
        impl->users.clear();

        for (const auto& layer : impl->layers)
        {
            if (layer.insert.empty())
                continue;

            ++impl->users[layer.insert];
            auto& holder = impl->holders[layer.insert];

            if (holder == nullptr || layer.order > holder->order)
                holder = &layer;
        }
    }

    void Painter::setInsertReturn (const std::string& insertId, sg_view picture, int width, int height)
    {
        if (picture.id == SG_INVALID_ID)
        {
            impl->returns.erase (insertId);
            return;
        }

        impl->returns[insertId] = { picture, width, height };
    }

    bool Painter::drawInsertPicture (const std::string& insertId, std::int64_t sample, sg_pixel_format format)
    {
        const auto holder = impl->holders.find (insertId);

        if (holder == impl->holders.end() || holder->second == nullptr)
            return false;

        int width = 0, height = 0;
        impl->sourceSizeOf (*holder->second, sample, width, height);
        auto& into = impl->offscreen ("own:insert:" + insertId, width, height, format);
        return impl->drawAlone (*holder->second, sample, into);
    }

    bool Painter::drawInsertBlack (const std::string& insertId, sg_pixel_format format)
    {
        //  The size it last sent, else 1920 by 1080.
        auto width = 1920, height = 1080;

        if (const auto found = impl->targets.find ("own:insert:" + insertId); found != impl->targets.end())
        {
            width = found->second.width;
            height = found->second.height;
        }

        auto& into = impl->offscreen ("own:insert:" + insertId, std::max (16, width), std::max (16, height), format);
        sg_begin_pass (Impl::passInto (into.colour));
        sg_end_pass();
        return true;
    }

    sg_image Painter::insertImage (const std::string& insertId) const
    {
        const auto found = impl->targets.find ("own:insert:" + insertId);
        return found != impl->targets.end() ? found->second.image : sg_image {};
    }

    int Painter::insertUsers (const std::string& insertId) const
    {
        const auto found = impl->users.find (insertId);
        return found != impl->users.end() ? found->second : 0;
    }

    sg_view Painter::canvas (const std::string& id, std::int64_t sample)
    {
        return impl->canvas (id, sample);
    }

    void Painter::drawOutput (const region::OutputReading& output, std::int64_t sample, const Target& into)
    {
        impl->drawOutput (output, sample, into);
    }

    void Painter::keepPictures (const std::vector<std::string>& paths, std::size_t budgetBytes)
    {
        impl->keepPictures (paths, budgetBytes);
    }

    bool Painter::settledAhead (const std::string& path) const
    {
        return impl->settled.count (path) > 0;
    }

    std::uint64_t Painter::uploadsAhead() const noexcept
    {
        return impl->uploadedAhead;
    }

    void Painter::endFrame()
    {
        impl->endFrame();
    }

    sg_image Painter::canvasImage (const std::string& id) const
    {
        const auto found = impl->targets.find ("canvas:" + id);
        return found != impl->targets.end() ? found->second.image : sg_image {};
    }

    Target Painter::offscreenTarget (const std::string& key, int width, int height, sg_pixel_format format)
    {
        auto& held = impl->offscreen ("own:" + key, std::max (1, width), std::max (1, height), format);

        Target target;
        target.colour = held.colour;
        target.format = format;
        target.width = target.viewWidth = held.width;
        target.height = target.viewHeight = held.height;
        return target;
    }

    void Painter::releaseOffscreen (const std::string& key)
    {
        const auto found = impl->targets.find ("own:" + key);

        if (found == impl->targets.end())
            return;

        release (found->second.texture);
        release (found->second.colour);
        release (found->second.image);
        impl->targets.erase (found);
    }

    sg_image Painter::offscreenImage (const std::string& key) const
    {
        const auto found = impl->targets.find ("own:" + key);
        return found != impl->targets.end() ? found->second.image : sg_image {};
    }
}
