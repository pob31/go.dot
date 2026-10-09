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
    A VIDEO CUE'S PICTURE AS ITS PANEL DRAWS IT (namespace draft §47, AAG).

    The author, 2026-10-09: "Could we have another foot panel with all colour
    and geometry adjustments rather listing them in the inspector." The panel
    draws the cue's canvas with the picture's frame on it - where it lies, how
    large, how turned - its mask's outline, its four curves; a hand drags them.
    What a drag MEANS is here, as numbers, so a test can drag a pointer and
    read where the picture would go:

      - THE FRAME is `video::Placement`'s, the projector's own arithmetic, so
        the outline the hand drags is where the picture is drawn;
      - A MOVE is the pointer's travel in the canvas's pixels, as percent of
        its width and height (VQ), up being up (VT);
      - A CORNER scales about the picture's middle, by how much further from
        it the pointer is than where the corner was taken;
      - THE TURN is the pointer's angle about the middle, clockwise (VT),
        fifteen degrees a step with Shift held;
      - A MASK'S OUTLINE is points 0..1 of the canvas from its top-left (VF),
        a point added on the edge nearest the press;
      - A CURVE is (in, out) pairs 0..1 (§36.6): its ends stay at in 0 and 1.

    std only, as every model file is.
*/

#include <wfg/engine/video/Geometry.h>

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    /** A point on the canvas, in its pixels from its middle, y up - `Placement`'s space. */
    struct CanvasPoint
    {
        double x = 0.0;
        double y = 0.0;
    };

    /** A mask's corner: 0..1 of the canvas from its top-left (VF). */
    struct MaskPoint
    {
        double x = 0.0;
        double y = 0.0;
    };

    /** A curve's point: what comes in, what goes out, each 0..1. */
    using CurvePoints = std::vector<std::pair<double, double>>;

    struct PictureReading
    {
        std::string cueId;
        std::string source;             ///< fill, mask, picture, movie, capture
        std::string file;               ///< as the document names it: its size is the media table's
        std::string canvas;

        double canvasWidth = 1920.0;
        double canvasHeight = 1080.0;

        /*  The picture's own size; nought until the analyser has read it, when
            the frame is drawn the canvas's shape. Filled by the panel from the
            media table, which the tree does not carry. */
        double pictureWidth = 0.0;
        double pictureHeight = 0.0;

        std::string fit = "fit";
        double scale = 100.0;
        double offsetX = 0.0;
        double offsetY = 0.0;
        double rotation = 0.0;
        bool flipH = false;
        bool flipV = false;

        double contrast = 100.0;
        double saturation = 100.0;
        double gamma = 1.0;
        double hue = 0.0;

        /*  The curves: the luminosity's, then red, green and blue. Empty is
            the straight line, which changes nothing. */
        std::array<CurvePoints, 4> curves;

        std::vector<MaskPoint> shape;
        double feather = 0.0;
        bool invert = false;

        std::string paint;              ///< #RRGGBB

        bool locked = false;

        /*  WHAT THIS SOURCE HAS: every source a place on its canvas (VW); a
            picture, a movie and a capture a grade; a fill and a mask a colour;
            a mask an outline. */
        bool graded() const noexcept  { return source == "picture" || source == "movie" || source == "capture"; }
        bool painted() const noexcept { return source == "fill" || source == "mask"; }
        bool masked() const noexcept  { return source == "mask"; }
    };

    /*  THE ROWS EACH HALF OF THE PANEL EDITS, by name: what leaves the
        inspector for it. */
    const std::vector<std::string>& pictureRows();

    /** One cue's picture, read from the tree; empty `cueId` for a cue that is not a video cue. */
    PictureReading readPicture (const tree::TreeSnapshot& snapshot, const std::string& cueId);

    /** The projector's arithmetic, for this reading. */
    video::Placement placementOf (const PictureReading& reading);

    /*  THE FRAME'S CORNERS on the canvas - bottom-left, bottom-right,
        top-right, top-left, as drawn before any flip - and its middle. */
    std::array<CanvasPoint, 4> frameCorners (const PictureReading& reading);
    CanvasPoint frameMiddle (const PictureReading& reading);

    /*  A MOVE: the offsets, in % of the canvas, after the pointer travelled
        from `from` to `to` with the picture at `startX`, `startY`. */
    std::pair<double, double> offsetsMoved (const PictureReading& reading, double startX, double startY,
                                            CanvasPoint from, CanvasPoint to);

    /*  A CORNER DRAGGED: the scale, in %, that puts the corner taken at `from`
        where the pointer is, about the picture's middle; 0..1000. */
    double scaleDragged (const PictureReading& reading, double startScale, CanvasPoint from, CanvasPoint to);

    /*  THE TURN: degrees clockwise, the pointer's angle about the middle since
        it was taken at `from`; fifteen degrees a step when `snapped`. */
    double rotationDragged (const PictureReading& reading, double startRotation, CanvasPoint from, CanvasPoint to,
                            bool snapped);

    /*  AN ARROW KEY ON THE FRAME: a tenth of a percent, or one with Shift. */
    double nudged (double value, int direction, bool coarse);

    /*  A MASK'S OUTLINE: the point within `radius` of (x, y), or -1; one added
        on the edge nearest (x, y); one moved, held to -1..2 as the engine
        holds it; one taken away, never below three. */
    int maskPointNear (const std::vector<MaskPoint>& shape, double x, double y, double radius);
    std::vector<MaskPoint> maskWithPointAdded (const std::vector<MaskPoint>& shape, double x, double y);
    std::vector<MaskPoint> maskWithPointMoved (const std::vector<MaskPoint>& shape, int index, double x, double y);
    std::vector<MaskPoint> maskWithPointRemoved (const std::vector<MaskPoint>& shape, int index);

    /*  A CURVE: the straight line it starts as when somebody first touches it;
        the point within `radius` of (in, out), or -1; one added, kept in
        order; one moved - the ends only up and down; one taken away, never an
        end. */
    CurvePoints curveOrLine (const CurvePoints& points);
    int curvePointNear (const CurvePoints& points, double in, double out, double radius);
    CurvePoints curveWithPointAdded (const CurvePoints& points, double in, double out);
    CurvePoints curveWithPointMoved (const CurvePoints& points, int index, double in, double out);
    CurvePoints curveWithPointRemoved (const CurvePoints& points, int index);

    /*  AS A ROW IS WRITTEN: numbers in words, four decimals at most, in no
        locale's spelling - what `node.set` takes for a list row. */
    std::string maskText (const std::vector<MaskPoint>& shape);
    std::string curveText (const CurvePoints& points);
}
