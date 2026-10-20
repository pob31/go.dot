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
#include <wfg/engine/cue/FadeMoves.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/TreeSnapshot.h>
#include <wfg/engine/video/Blanks.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
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
            "shape", "feather", "invert", "paint",
            "useAlpha", "key", "keyColour", "keyLevel", "keySoftness", "keyInvert", "keySpill",
            "words", "font", "fontSize", "bold", "italic", "align", "alignV", "lineSpacing", "wrap",
            "outlineColour", "outline", "outlineBlur", "blur" };

        return rows;
    }

    namespace
    {
        std::vector<std::string> tabbed (const std::string& line)
        {
            std::vector<std::string> parts { std::string {} };
            for (const char c : line)
            {
                if (c == '\t')
                    parts.emplace_back();
                else
                    parts.back() += c;
            }
            return parts;
        }
    }

    std::vector<CarriedFamily> carriedFamilies (const std::string& listing)
    {
        std::vector<CarriedFamily> families;
        std::string line;
        for (std::size_t at = 0; at <= listing.size(); ++at)
        {
            if (at < listing.size() && listing[at] != '\n')
            {
                line += listing[at];
                continue;
            }
            if (! line.empty())
            {
                auto parts = tabbed (line);
                parts.resize (4);
                if (! parts[0].empty())
                    families.push_back ({ parts[0], parts[1], parts[2] == "around", parts[3] == "restricted" });
            }
            line.clear();
        }
        return families;
    }

    FontsJob fontsJobOf (const std::string& text)
    {
        FontsJob job;
        if (text.empty())
            return job;

        auto parts = tabbed (text);
        parts.resize (6);
        job.number = static_cast<unsigned> (std::strtoul (parts[0].c_str(), nullptr, 10));
        job.state = parts[1];
        job.verb = parts[2];
        job.subject = parts[3];
        job.result = parts[4];
        job.detail = parts[5];
        return job;
    }

    std::string fontsJobWords (const FontsJob& job)
    {
        //  A file by its name, a family as it is.
        auto subject = job.subject;
        if (job.verb == "add")
            if (const auto slash = subject.find_last_of ("/\\"); slash != std::string::npos)
                subject = subject.substr (slash + 1);

        if (job.state == "working")
            return (job.verb == "remove" ? "Taking " + subject + " out of the show..." : "Carrying " + subject + " in the show...");

        if (job.result == "carried")     return (job.verb == "add" ? job.detail : subject) + " carried in the show";
        if (job.result == "removed")     return subject + " taken out of the show";
        if (job.result == "restricted")  return subject + ": its licence restricts carrying it";
        if (job.result == "not-a-font")  return "Not carried: " + (job.detail.empty() ? subject : job.detail) + " is not a font Go.dot reads";
        if (job.result == "not-installed") return "Not carried: " + subject + " is not installed on the engine's machine";
        if (job.result == "not-carried") return subject + " is not in the show's fonts";
        if (job.result == "no-folder")   return "The show has no folder yet: save it, then carry the font";
        if (job.result == "failed")      return "Not done: " + job.detail;
        return {};
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
        out.blur = numberAt (snapshot, base + "blur", 20.0);

        out.useAlpha = flag (snapshot, base + "useAlpha") != Flag::no;
        out.inputAlpha = isYes (flag (snapshot, "/godot/videoInput/" + text (snapshot, base + "videoInput") + "/alpha"));
        out.key = text (snapshot, base + "key");

        if (out.key.empty())
            out.key = "none";

        out.keyColour = text (snapshot, base + "keyColour");

        if (out.keyColour.empty())
            out.keyColour = "#00FF00";

        out.keyLevel = numberAt (snapshot, base + "keyLevel", 50.0);
        out.keySoftness = numberAt (snapshot, base + "keySoftness", 100.0);
        out.keySpill = numberAt (snapshot, base + "keySpill", 50.0);
        out.keyInvert = isYes (flag (snapshot, base + "keyInvert"));

        out.words = text (snapshot, base + "words");
        out.font = text (snapshot, base + "font");
        out.fontSize = numberAt (snapshot, base + "fontSize", 8.0);
        out.bold = isYes (flag (snapshot, base + "bold"));
        out.italic = isYes (flag (snapshot, base + "italic"));
        out.align = text (snapshot, base + "align");
        out.alignV = text (snapshot, base + "alignV");

        if (out.align.empty())
            out.align = "centre";

        if (out.alignV.empty())
            out.alignV = "middle";

        out.lineSpacing = numberAt (snapshot, base + "lineSpacing", 100.0);
        out.wrap = numberAt (snapshot, base + "wrap", 90.0);
        out.outlineColour = text (snapshot, base + "outlineColour");

        if (out.outlineColour.empty())
            out.outlineColour = "#000000";

        out.outline = numberAt (snapshot, base + "outline", 0.0);
        out.outlineBlur = numberAt (snapshot, base + "outlineBlur", 0.0);

        //  ITS FIELDS (61, AKM): each blank, its value as the engine publishes it.
        if (out.source == "text")
            for (const auto& name : video::blanksIn (out.words))
            {
                const auto address = "/godot/text/" + name;
                out.fields.push_back ({ name, text (snapshot, address), address });
            }

        out.carried = carriedFamilies (text (snapshot, "/godot/engine/fonts"));
        out.fontsJob = text (snapshot, "/godot/engine/fontsJob");

        out.locked = isYes (flag (snapshot, "/godot/document/locked"));
        out.opacity = numberAt (snapshot, base + "opacity", 100.0);
        out.rate = numberAt (snapshot, base + "rate", 1.0);

        return out;
    }

    std::vector<std::string> PictureReading::fadeRows() const
    {
        std::vector<std::string> out { "opacity" };

        if (! blurred())
            for (const auto* row : { "scale", "offsetX", "offsetY", "rotation" })
                out.emplace_back (row);

        if (source == "movie")
            out.emplace_back ("rate");

        if (blurred())
            out.emplace_back ("blur");

        return out;
    }

    PictureReading readFadePicture (const tree::TreeSnapshot& snapshot, const std::string& fadeId,
                                    std::string& notice)
    {
        const auto fade = "/godot/cue/" + fadeId + "/";

        if (text (snapshot, fade + "kind") != "fade")
        {
            notice = "Only a video cue, or a fade aimed at one, has a picture to place.";
            return {};
        }

        const auto targetId = text (snapshot, fade + "target");

        if (! text (snapshot, fade + "dca").empty() || ! text (snapshot, fade + "aux").empty()
              || text (snapshot, "/godot/cue/" + targetId + "/kind") != "video")
        {
            notice = "This fade is not aimed at a video cue: only a fade on a video cue moves a picture.";
            return {};
        }

        auto out = readPicture (snapshot, targetId);
        out.fadeId = fadeId;

        /*  THE FADE'S PAIRS OVER THE TARGET'S NUMBERS, those this source can
            be moved by: where the fade arrives. */
        const auto movable = out.fadeRows();

        for (const auto& [row, value] : cue::parseMoveList (text (snapshot, fade + "video")))
        {
            if (std::find (movable.begin(), movable.end(), row) == movable.end())
                continue;

            out.fadeMoves[row] = value;

            if (row == "opacity")        out.opacity = value;
            else if (row == "scale")     out.scale = value;
            else if (row == "offsetX")   out.offsetX = value;
            else if (row == "offsetY")   out.offsetY = value;
            else if (row == "rotation")  out.rotation = value;
            else if (row == "rate")      out.rate = value;
            else if (row == "blur")      out.blur = value;
        }

        return out;
    }

    std::vector<std::pair<std::string, std::string>> keyChoice (const PictureReading& reading, const std::string& key)
    {
        const auto base = "/godot/cue/" + reading.cueId + "/";
        std::vector<std::pair<std::string, std::string>> rows { { base + "key", key } };

        const auto untouched = std::abs (reading.keyLevel - 50.0) < 1.0e-9 && std::abs (reading.keySoftness - 100.0) < 1.0e-9;

        if (key == "colour" && reading.key != "colour" && untouched)
        {
            rows.emplace_back (base + "keyLevel", "40");
            rows.emplace_back (base + "keySoftness", "10");
        }

        return rows;
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
        return stepped (value, 0.1, direction, coarse, -1000.0, 1000.0);
    }

    double stepped (double value, double step, int direction, bool coarse, double low, double high)
    {
        if (! (step > 0.0))
            return std::clamp (value, low, high);

        const auto moved = value + step * (coarse ? 10.0 : 1.0) * static_cast<double> (direction);

        //  On the step's grid, by its whole inverse when it is a fraction: 0.1 is not exact, 10 is.
        const auto onGrid = step < 1.0 ? std::round (moved * std::round (1.0 / step)) / std::round (1.0 / step)
                                       : std::round (moved / step) * step;

        return std::clamp (onGrid, low, high);
    }

    void HeldNumbers::send (const std::string& row, double value, double nowMs)
    {
        held[row] = { value, nowMs };
    }

    double HeldNumbers::valueOf (const std::string& row, double documentValue, double nowMs)
    {
        const auto found = held.find (row);

        if (found == held.end())
            return documentValue;

        //  Caught up (a row is written to four decimals), or given up waiting.
        if (std::abs (documentValue - found->second.value) < 1.0e-4 || nowMs - found->second.atMs > patienceMs)
        {
            held.erase (found);
            return documentValue;
        }

        return found->second.value;
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
