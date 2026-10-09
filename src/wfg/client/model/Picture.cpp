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

#include <wfg/client/model/Picture.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace wfg::client::model
{
    namespace
    {
        constexpr double pi = 3.14159265358979323846;

        double numberAt (const tree::TreeSnapshot& snapshot, const std::string& address, double otherwise)
        {
            return osc::parseDouble (text (snapshot, address)).value_or (otherwise);
        }

        //  A list row's numbers, which `text` answers empty for (four numbers have no single text).
        std::vector<double> numbersAt (const tree::TreeSnapshot& snapshot, const std::string& address)
        {
            std::vector<double> out;

            if (const auto* node = snapshot.find (address))
                for (const auto& value : node->values)
                    if (value.isNumber())
                        out.push_back (value.asDouble());

            return out;
        }

        CurvePoints pairsOf (const std::vector<double>& numbers)
        {
            CurvePoints out;

            for (std::size_t at = 0; at + 1 < numbers.size(); at += 2)
                out.emplace_back (std::clamp (numbers[at], 0.0, 1.0), std::clamp (numbers[at + 1], 0.0, 1.0));

            std::stable_sort (out.begin(), out.end(),
                              [] (const auto& a, const auto& b) { return a.first < b.first; });
            return out;
        }

        double distance (double ax, double ay, double bx, double by) noexcept
        {
            return std::hypot (ax - bx, ay - by);
        }

        //  A number as a row takes it: four decimals at most, the zeros at the end left off.
        std::string shortNumber (double value)
        {
            auto rounded = std::round (value * 10000.0) / 10000.0;

            if (std::abs (rounded) < 0.00005)
                rounded = 0.0;

            return osc::formatDouble (rounded);
        }
    }

    const std::vector<std::string>& pictureRows()
    {
        static const std::vector<std::string> rows {
            "fit", "scale", "offsetX", "offsetY", "rotation", "flipH", "flipV",
            "contrast", "saturation", "gamma", "hue",
            "curveLuma", "curveRed", "curveGreen", "curveBlue",
            "shape", "feather", "invert", "paint" };

        return rows;
    }

    PictureReading readPicture (const tree::TreeSnapshot& snapshot, const std::string& cueId)
    {
        PictureReading out;

        if (cueId.empty())
            return out;

        const auto base = "/godot/cue/" + cueId + "/";

        if (text (snapshot, base + "kind") != "video")
            return out;

        out.cueId = cueId;
        out.source = text (snapshot, base + "source");
        out.file = text (snapshot, base + "file");
        out.canvas = text (snapshot, base + "canvas");

        if (! out.canvas.empty())
        {
            out.canvasWidth = std::max (16.0, numberAt (snapshot, "/godot/canvas/" + out.canvas + "/width", 1920.0));
            out.canvasHeight = std::max (16.0, numberAt (snapshot, "/godot/canvas/" + out.canvas + "/height", 1080.0));
        }

        out.fit = text (snapshot, base + "fit");

        if (out.fit.empty())
            out.fit = "fit";

        out.scale = numberAt (snapshot, base + "scale", 100.0);
        out.offsetX = numberAt (snapshot, base + "offsetX", 0.0);
        out.offsetY = numberAt (snapshot, base + "offsetY", 0.0);
        out.rotation = numberAt (snapshot, base + "rotation", 0.0);
        out.flipH = isYes (flag (snapshot, base + "flipH"));
        out.flipV = isYes (flag (snapshot, base + "flipV"));

        out.contrast = numberAt (snapshot, base + "contrast", 100.0);
        out.saturation = numberAt (snapshot, base + "saturation", 100.0);
        out.gamma = numberAt (snapshot, base + "gamma", 1.0);
        out.hue = numberAt (snapshot, base + "hue", 0.0);

        const char* curveRows[] { "curveLuma", "curveRed", "curveGreen", "curveBlue" };

        for (std::size_t at = 0; at < out.curves.size(); ++at)
            out.curves[at] = pairsOf (numbersAt (snapshot, base + curveRows[at]));

        const auto corners = numbersAt (snapshot, base + "shape");

        for (std::size_t at = 0; at + 1 < corners.size(); at += 2)
            out.shape.push_back ({ corners[at], corners[at + 1] });

        out.feather = numberAt (snapshot, base + "feather", 0.0);
        out.invert = isYes (flag (snapshot, base + "invert"));
        out.paint = text (snapshot, base + "paint");
        out.locked = isYes (flag (snapshot, "/godot/document/locked"));

        return out;
    }

    video::Placement placementOf (const PictureReading& reading)
    {
        video::Placement placement;
        placement.canvasWidth = reading.canvasWidth;
        placement.canvasHeight = reading.canvasHeight;

        //  A fill, a mask, or a picture whose size is not read yet: the canvas's shape.
        const auto known = reading.pictureWidth > 0.0 && reading.pictureHeight > 0.0 && ! reading.painted();
        placement.pictureWidth = known ? reading.pictureWidth : reading.canvasWidth;
        placement.pictureHeight = known ? reading.pictureHeight : reading.canvasHeight;

        placement.fit = reading.fit == "fill" ? 1 : reading.fit == "stretch" ? 2 : 0;
        placement.scale = reading.scale;
        placement.offsetX = reading.offsetX;
        placement.offsetY = reading.offsetY;
        placement.rotation = reading.rotation;
        placement.flipH = reading.flipH;
        placement.flipV = reading.flipV;
        return placement;
    }

    std::array<CanvasPoint, 4> frameCorners (const PictureReading& reading)
    {
        const auto placement = placementOf (reading);
        std::array<CanvasPoint, 4> corners;
        const double us[] { -1.0, 1.0, 1.0, -1.0 };
        const double vs[] { -1.0, -1.0, 1.0, 1.0 };

        for (std::size_t at = 0; at < corners.size(); ++at)
            placement.toCanvas (us[at], vs[at], corners[at].x, corners[at].y);

        return corners;
    }

    CanvasPoint frameMiddle (const PictureReading& reading)
    {
        return { reading.offsetX / 100.0 * reading.canvasWidth, reading.offsetY / 100.0 * reading.canvasHeight };
    }

    std::pair<double, double> offsetsMoved (const PictureReading& reading, double startX, double startY,
                                            CanvasPoint from, CanvasPoint to)
    {
        const auto x = startX + (to.x - from.x) / reading.canvasWidth * 100.0;
        const auto y = startY + (to.y - from.y) / reading.canvasHeight * 100.0;

        return { std::clamp (x, -1000.0, 1000.0), std::clamp (y, -1000.0, 1000.0) };
    }

    double scaleDragged (const PictureReading& reading, double startScale, CanvasPoint from, CanvasPoint to)
    {
        const auto middle = frameMiddle (reading);
        const auto was = distance (from.x, from.y, middle.x, middle.y);

        if (! (was > 0.0))
            return startScale;

        return std::clamp (startScale * distance (to.x, to.y, middle.x, middle.y) / was, 0.0, 1000.0);
    }

    double rotationDragged (const PictureReading& reading, double startRotation, CanvasPoint from, CanvasPoint to,
                            bool snapped)
    {
        const auto middle = frameMiddle (reading);
        const auto angleOf = [&middle] (CanvasPoint point)
        {
            return std::atan2 (point.y - middle.y, point.x - middle.x);
        };

        //  CLOCKWISE (VT): with y up, an angle that grows is a turn anticlockwise.
        auto turned = -(angleOf (to) - angleOf (from)) * 180.0 / pi;

        while (turned > 180.0)
            turned -= 360.0;

        while (turned < -180.0)
            turned += 360.0;

        auto rotation = startRotation + turned;

        if (snapped)
            rotation = std::round (rotation / 15.0) * 15.0;

        return std::clamp (rotation, -3600.0, 3600.0);
    }

    double nudged (double value, int direction, bool coarse)
    {
        const auto step = coarse ? 1.0 : 0.1;
        return std::clamp (std::round ((value + step * static_cast<double> (direction)) * 10.0) / 10.0, -1000.0, 1000.0);
    }

    int maskPointNear (const std::vector<MaskPoint>& shape, double x, double y, double radius)
    {
        auto found = -1;
        auto nearest = radius;

        for (std::size_t at = 0; at < shape.size(); ++at)
        {
            const auto gap = distance (shape[at].x, shape[at].y, x, y);

            if (gap <= nearest)
            {
                nearest = gap;
                found = static_cast<int> (at);
            }
        }

        return found;
    }

    std::vector<MaskPoint> maskWithPointAdded (const std::vector<MaskPoint>& shape, double x, double y)
    {
        auto out = shape;
        const MaskPoint added { std::clamp (x, -1.0, 2.0), std::clamp (y, -1.0, 2.0) };

        if (out.size() < 2)
        {
            out.push_back (added);
            return out;
        }

        //  On the edge nearest the press: after the corner that edge starts at.
        auto best = out.size() - 1;
        auto bestGap = std::numeric_limits<double>::max();

        for (std::size_t at = 0; at < out.size(); ++at)
        {
            const auto& a = out[at];
            const auto& b = out[(at + 1) % out.size()];
            const auto dx = b.x - a.x, dy = b.y - a.y;
            const auto length = dx * dx + dy * dy;
            const auto t = length > 0.0 ? std::clamp (((x - a.x) * dx + (y - a.y) * dy) / length, 0.0, 1.0) : 0.0;
            const auto gap = distance (a.x + t * dx, a.y + t * dy, x, y);

            if (gap < bestGap)
            {
                bestGap = gap;
                best = at;
            }
        }

        out.insert (out.begin() + static_cast<std::ptrdiff_t> (best + 1), added);
        return out;
    }

    std::vector<MaskPoint> maskWithPointMoved (const std::vector<MaskPoint>& shape, int index, double x, double y)
    {
        auto out = shape;

        if (index >= 0 && static_cast<std::size_t> (index) < out.size())
            out[static_cast<std::size_t> (index)] = { std::clamp (x, -1.0, 2.0), std::clamp (y, -1.0, 2.0) };

        return out;
    }

    std::vector<MaskPoint> maskWithPointRemoved (const std::vector<MaskPoint>& shape, int index)
    {
        auto out = shape;

        if (out.size() > 3 && index >= 0 && static_cast<std::size_t> (index) < out.size())
            out.erase (out.begin() + index);

        return out;
    }

    CurvePoints curveOrLine (const CurvePoints& points)
    {
        if (points.empty())
            return { { 0.0, 0.0 }, { 1.0, 1.0 } };

        return points;
    }

    int curvePointNear (const CurvePoints& points, double in, double out, double radius)
    {
        auto found = -1;
        auto nearest = radius;

        for (std::size_t at = 0; at < points.size(); ++at)
        {
            const auto gap = distance (points[at].first, points[at].second, in, out);

            if (gap <= nearest)
            {
                nearest = gap;
                found = static_cast<int> (at);
            }
        }

        return found;
    }

    CurvePoints curveWithPointAdded (const CurvePoints& points, double in, double out)
    {
        auto result = curveOrLine (points);
        const std::pair<double, double> added { std::clamp (in, 0.0, 1.0), std::clamp (out, 0.0, 1.0) };

        const auto place = std::find_if (result.begin(), result.end(),
                                         [&added] (const auto& point) { return point.first > added.first; });
        result.insert (place, added);
        return result;
    }

    CurvePoints curveWithPointMoved (const CurvePoints& points, int index, double in, double out)
    {
        auto result = curveOrLine (points);

        if (index < 0 || static_cast<std::size_t> (index) >= result.size())
            return result;

        const auto at = static_cast<std::size_t> (index);
        auto& point = result[at];
        point.second = std::clamp (out, 0.0, 1.0);

        //  AN END ONLY UP AND DOWN; a point between, no further than its neighbours.
        if (at > 0 && at + 1 < result.size())
            point.first = std::clamp (in, result[at - 1].first, result[at + 1].first);

        return result;
    }

    CurvePoints curveWithPointRemoved (const CurvePoints& points, int index)
    {
        auto result = curveOrLine (points);

        if (index > 0 && static_cast<std::size_t> (index) + 1 < result.size())
            result.erase (result.begin() + index);

        return result;
    }

    std::string maskText (const std::vector<MaskPoint>& shape)
    {
        std::string out;

        for (const auto& point : shape)
            out += (out.empty() ? "" : " ") + shortNumber (point.x) + " " + shortNumber (point.y);

        return out;
    }

    std::string curveText (const CurvePoints& points)
    {
        std::string out;

        for (const auto& [in, value] : points)
            out += (out.empty() ? "" : " ") + shortNumber (in) + " " + shortNumber (value);

        return out;
    }
}
