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
    THE PICTURE SIDE, AS THE CUE LAYER SEES IT (Phase 8a, namespace draft 35).

    The audio `Player`'s twin, and shaped like it on purpose: it names no JUCE
    type and no graphics one, so the command layer and the tests include it
    freely, and a null implementation is a complete one - a show replayed with
    no sink still creates its runs, moves its standby and writes the same log;
    it just shows nothing.

    WHAT CROSSES IS A LAYER AND WHAT MOVES ON IT, placed in time. A video run is
    one layer on one canvas; the Runner says what it is when it comes up - its
    source, its picture and the geometry it is written with (§36) - and then
    only where a moving value goes: its opacity, its scale, its offset, its
    turn. A point is "straight from wherever the last point of this value left
    it, to here at this sample", and two at one sample are a step -
    `Player::placeRate`'s shape exactly. The Runner places each point a launch
    horizon ahead, as it places a launch, so the picture side always knows what
    the next frames show before it has to show them, and a fade is drawn on the
    frame it is due and never in fifty steps a second (§35.4). A value with no
    point stays as the layer was written.

    Tick thread, all of it, and never blocking: an implementation writes into
    a region another process reads, or into a vector a test reads.
*/

#include <wfg/engine/video/Grade.h>
#include <wfg/engine/video/Mask.h>

#include <cstdint>
#include <string>
#include <vector>

namespace wfg::video
{
    /*  What moves on a layer: the opacity since V.1, and the geometry a fade
        moves since §36 (VS). The numbers are the region's too. */
    enum class Property : std::uint32_t
    {
        opacity = 0,    ///< 0..1 - the row's % over a hundred
        scale = 1,      ///< % of the fitted size
        offsetX = 2,    ///< % of the canvas's width, right positive
        offsetY = 3,    ///< % of the canvas's height, up positive
        rotation = 4,   ///< degrees, clockwise
        time = 5,       ///< a movie's playhead: seconds of the file (namespace draft 37, VZ)
        dca = 6         ///< 0..1, what the DCAs above the cue leave of its opacity (37.5, WE)
    };

    constexpr int propertyCount = 7;

    /*  One layer: what a video run puts on a canvas. A VALUE, carrying no
        document reference, for `ArmRequest`'s reason - it is read on the other
        side of a boundary. */
    struct LayerSpec
    {
        std::string id;             ///< the run's identifier: one layer per run
        std::string canvas;         ///< the canvas it lies on, by identifier
        int layer = 0;              ///< higher is on top (VI)
        std::uint64_t order = 0;    ///< which came up later, for two on one layer
        std::string source;         ///< fill, mask, picture or movie
        std::string blend = "normal";   ///< normal, add, screen or multiply (VD)

        /*  The fill's or the mask's colour as 0xRRGGBB - the cue's `paint`,
            parsed once here so nothing on the far side reads document text
            (VM). */
        std::uint32_t paint = 0x000000;

        /*  A picture's file, as a whole path the engine resolved (VX), and how
            it meets the canvas (VO). */
        std::string file;
        std::string fit = "fit";

        /*  The geometry as the cue is written (§36.3): where each moving value
            starts, and the flips. */
        double scale = 100.0;
        double offsetX = 0.0;
        double offsetY = 0.0;
        double rotation = 0.0;
        bool flipH = false;
        bool flipV = false;

        /*  ITS GRADE (§36, VP, VU): a picture's and a movie's, the curves
            already baked. Does not move (VV). */
        Grade grade;

        /*  A MASK'S OUTLINE, its feather and whether it is turned inside out
            (UY, VF), parsed once here. */
        mask::Shape shape;
    };

    /*  Where a value is at one sample of Go.dot's own clock. A sample below
        nought is "now": a show with no clock to place it on (§35.4). */
    struct Point
    {
        std::int64_t sample = -1;
        double value = 0.0;         ///< whichever property it is
    };

    class Sink
    {
    public:
        virtual ~Sink() = default;

        /*  A layer comes up, or one already up is told what it is now. Its
            opacity is nought until a point says otherwise, so a layer placed
            and then pointed at is never seen for a frame at full. */
        virtual void show (const LayerSpec&) = 0;

        /** From the last point of this value the layer holds, straight to this one. */
        virtual void move (const std::string& id, Property, const Point&) = 0;

        /** The opacity's, which every layer has. */
        void opacity (const std::string& id, const Point& point)  { move (id, Property::opacity, point); }

        /*  The layer goes, at a sample - the end of a fade-out, placed with
            its last point, so the picture is gone on the frame it reaches
            nought and not a tick later. */
        virtual void remove (const std::string& id, std::int64_t sample) = 0;

        /*  Every layer gone, now: the double Esc (PRD §4.4). The outputs stay
            open and black - what Go.dot originates is the picture, not the
            projector. */
        virtual void clear() = 0;

        /*  THE PICTURES TO HAVE READY, as whole paths: the standby's, read
            before GO so GO only shows them (VX). Replaces the last list. A sink
            that reads no picture has nothing to do. */
        virtual void prepare (const std::vector<std::string>&) {}
    };
}
