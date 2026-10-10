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

#include <wfg/engine/cue/PictureSpec.h>
#include <wfg/engine/cue/PlayedMedia.h>

#include <wfg/engine/audio/MediaInfo.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/video/Grade.h>
#include <wfg/engine/video/Mask.h>
#include <wfg/engine/video/VideoRamp.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace wfg::cue
{
    namespace
    {
        //  The numbers of a list row, written as words: a curve's pairs, a mask's corners.
        std::vector<double> numbersIn (const std::string& text)
        {
            std::vector<double> out;
            std::size_t at = 0;

            while (at < text.size())
            {
                const auto start = text.find_first_not_of (' ', at);

                if (start == std::string::npos)
                    break;

                const auto end = text.find (' ', start);
                const auto word = text.substr (start, end == std::string::npos ? std::string::npos : end - start);

                if (const auto value = osc::parseDouble (word); value.has_value())
                    out.push_back (*value);

                at = end == std::string::npos ? text.size() : end;
            }

            return out;
        }

        bool same (double a, double b) noexcept
        {
            return std::bit_cast<std::uint64_t> (a) == std::bit_cast<std::uint64_t> (b);
        }

        bool same (float a, float b) noexcept
        {
            return std::bit_cast<std::uint32_t> (a) == std::bit_cast<std::uint32_t> (b);
        }
    }

    video::LayerSpec pictureSpecOf (const doc::ShowDocument& document, const std::string& cueId,
                                    const std::string& mediaFolder)
    {
        return pictureSpecOf (document, cueId, mediaFolder, nullptr);
    }

    video::LayerSpec pictureSpecOf (const doc::ShowDocument& document, const std::string& cueId,
                                    const std::string& mediaFolder, const PlayedMedia* played)
    {
        video::LayerSpec spec;

        if (cueId.empty())
            return spec;

        const auto base = "/godot/cue/" + cueId + "/";

        const auto textOf = [&document, &base] (const char* name)
        {
            return document.getAttribute (base + name).value_or (std::string {});
        };

        const auto numberOf = [&textOf] (const char* name)
        {
            return osc::parseDouble (textOf (name)).value_or (0.0);
        };

        spec.canvas = textOf ("canvas");
        spec.layer = static_cast<int> (std::lround (numberOf ("layer")));
        spec.source = textOf ("source");
        spec.blend = textOf ("blend");

        const auto paint = textOf ("paint");
        spec.paint = video::paintFromText (paint.data(), paint.size());

        /*  A PICTURE'S FILE AS A WHOLE PATH, resolved as a sound's is (VX); a
            capture's video input and the insert it goes through, by identifier
            (§44, YC, YE). */
        if (spec.source == "movie" && played != nullptr)
        {
            /*  A MOVIE BEING EDITED plays its render (§55.5): nothing while
                the render is not there, which the caller has refused already. */
            if (! played->name.empty())
                spec.file = audio::resolveMediaPath (mediaFolder, played->name);
        }
        else if (spec.source == "picture" || spec.source == "movie")
        {
            if (const auto named = textOf ("file"); ! named.empty())
                spec.file = audio::resolveMediaPath (mediaFolder, named);
        }

        if (spec.source == "capture")
            spec.input = textOf ("videoInput");

        spec.insert = textOf ("videoInsert");

        //  The geometry as the cue is written (§36.3).
        spec.fit = textOf ("fit");
        spec.scale = numberOf ("scale");
        spec.offsetX = numberOf ("offsetX");
        spec.offsetY = numberOf ("offsetY");
        spec.rotation = numberOf ("rotation");
        spec.flipH = textOf ("flipH") == "true";
        spec.flipV = textOf ("flipV") == "true";

        //  The grade (§36, VP, VU), its curves baked here so the far side reads tables (VM).
        spec.grade.contrast = numberOf ("contrast");
        spec.grade.saturation = numberOf ("saturation");
        spec.grade.gamma = numberOf ("gamma");
        spec.grade.hue = numberOf ("hue");

        const auto curveOf = [&textOf] (const char* row)
        {
            return video::curveFrom (numbersIn (textOf (row)));
        };

        video::bakeCurves (spec.grade, curveOf ("curveLuma"),
                           { curveOf ("curveRed"), curveOf ("curveGreen"), curveOf ("curveBlue") });

        //  A mask's outline (UY, VF): x, y pairs, at most `maxPoints`.
        const auto corners = numbersIn (textOf ("shape"));
        const auto count = std::min<std::size_t> (corners.size() / 2, video::mask::maxPoints);

        for (std::size_t n = 0; n < count; ++n)
        {
            spec.shape.x[n] = static_cast<float> (std::clamp (corners[2 * n], -1.0, 2.0));
            spec.shape.y[n] = static_cast<float> (std::clamp (corners[2 * n + 1], -1.0, 2.0));
        }

        spec.shape.count = static_cast<int> (count);
        spec.shape.feather = static_cast<float> (std::max (0.0, numberOf ("feather")));
        spec.shape.invert = textOf ("invert") == "true";

        return spec;
    }

    bool sameLook (const video::LayerSpec& a, const video::LayerSpec& b) noexcept
    {
        if (a.blend != b.blend || a.paint != b.paint || a.fit != b.fit
              || a.flipH != b.flipH || a.flipV != b.flipV)
            return false;

        if (! same (a.scale, b.scale) || ! same (a.offsetX, b.offsetX) || ! same (a.offsetY, b.offsetY)
              || ! same (a.rotation, b.rotation))
            return false;

        if (! same (a.grade.contrast, b.grade.contrast) || ! same (a.grade.saturation, b.grade.saturation)
              || ! same (a.grade.gamma, b.grade.gamma) || ! same (a.grade.hue, b.grade.hue)
              || a.grade.hasCurves != b.grade.hasCurves || a.grade.tables != b.grade.tables)
            return false;

        if (a.shape.count != b.shape.count || a.shape.invert != b.shape.invert
              || ! same (a.shape.feather, b.shape.feather))
            return false;

        return std::memcmp (a.shape.x, b.shape.x, sizeof (a.shape.x)) == 0
            && std::memcmp (a.shape.y, b.shape.y, sizeof (a.shape.y)) == 0;
    }

    void takeLook (video::LayerSpec& into, const video::LayerSpec& from) noexcept
    {
        into.blend = from.blend;
        into.paint = from.paint;
        into.fit = from.fit;
        into.scale = from.scale;
        into.offsetX = from.offsetX;
        into.offsetY = from.offsetY;
        into.rotation = from.rotation;
        into.flipH = from.flipH;
        into.flipV = from.flipV;
        into.grade = from.grade;
        into.shape = from.shape;
    }
}
