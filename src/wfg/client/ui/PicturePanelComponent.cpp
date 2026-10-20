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

#include <wfg/client/ui/PicturePanelComponent.h>

#include <wfg/client/ui/Look.h>
#include <wfg/client/model/FadeMix.h>
#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <cmath>

namespace wfg::client::ui
{
    namespace
    {
        constexpr const char* fitWords[3] { "fit", "fill", "stretch" };

        /*  WHAT A FADE MOVES ON A PICTURE (namespace draft §94), by row, in the
            order the panel lists them; `fadeTicks` follows it. */
        constexpr const char* fadeRowNames[7] { "opacity", "scale", "offsetX", "offsetY", "rotation", "rate", "blur" };
        constexpr const char* curveRows[4] { "curveLuma", "curveRed", "curveGreen", "curveBlue" };
        constexpr const char* curveWords[4] { "L", "R", "G", "B" };

        /*  THE KEYS BY NAME (namespace draft 61, AKB): the rows' words and the
            menu's, the latter the implementer's, for the author (AKS). */
        constexpr const char* keyRows[4] { "none", "luma", "matte", "colour" };
        constexpr const char* keyWords[4] { "No key", "Brightness", "Brightness as a shape", "A colour (green screen)" };

        /*  A TEXT'S ALIGNMENTS (61, AKJ): the rows' words and the buttons'. */
        constexpr const char* alignRows[3] { "left", "centre", "right" };
        constexpr const char* alignWords[3] { "Left", "Centre", "Right" };
        constexpr const char* alignVRows[3] { "top", "middle", "bottom" };
        constexpr const char* alignVWords[3] { "At the top", "In the middle", "At the bottom" };

        //  The font menu's first line: the font a cue with none is set in.
        constexpr int defaultFontId = 1;
        constexpr int missingFontId = 1000000;

        //  The show's own families (§62), and the three things the menu does with fonts.
        constexpr int carriedFontId = 100000;
        constexpr int carryFontId = 2000001;
        constexpr int addFontFileId = 2000002;
        constexpr int removeFontId = 2000003;

        //  A number as a row takes it: four decimals at most, in no locale's spelling.
        std::string numberText (double value)
        {
            return osc::formatDouble (std::round (value * 10000.0) / 10000.0);
        }

        juce::Colour paintColour (const std::string& paint)
        {
            if (paint.size() == 7 && paint[0] == '#')
                return juce::Colour (0xff000000u | static_cast<juce::uint32> (juce::String (paint.substr (1)).getHexValue32()));

            return juce::Colours::black;
        }

        //  A colour picked, as the row writes it.
        std::string paintText (juce::Colour colour)
        {
            return "#" + colour.toDisplayString (false).toUpperCase().toStdString();
        }
    }

    //==========================================================================
    /*  THE CANVAS, AND THE FRAME ON IT. */
    class PicturePanelComponent::View final : public juce::Component
    {
    public:
        explicit View (PicturePanelComponent& ownerToUse) : owner (ownerToUse)
        {
            setWantsKeyboardFocus (true);
            setMouseClickGrabsKeyboardFocus (true);
        }

        juce::Image picture;

        void paint (juce::Graphics& g) override
        {
            const auto& r = drawn();
            const auto canvas = canvasRect();

            g.setColour (juce::Colours::black);
            g.fillRect (canvas);

            //  A blur cue's picture too: its canvas beneath it, blurred, under its outline (61).
            if (picture.isValid() && (! r.masked() || r.blurred()))
                g.drawImage (picture, canvas, juce::RectanglePlacement::centred);

            g.setColour (Look::colour (owner.theme, "rule"));
            g.drawRect (canvas, 1.0f);

            //  Holding the arrow keys: an outline round the whole view, a shape and not a colour alone (§4.8).
            if (hasKeyboardFocus (false))
            {
                g.setColour (Look::colour (owner.theme, "accent"));
                g.drawRect (getLocalBounds().toFloat().reduced (1.0f), 1.0f);
            }

            if (r.cueId.empty())
                return;

            if (r.masked())
            {
                paintOutline (g, r);
                return;
            }

            //  The frame, where the projector draws it.
            const auto corners = model::frameCorners (r);
            juce::Path frame;

            for (std::size_t at = 0; at < corners.size(); ++at)
            {
                const auto point = toView (corners[at]);

                if (at == 0)
                    frame.startNewSubPath (point);
                else
                    frame.lineTo (point);
            }

            frame.closeSubPath();

            const auto ink = Look::colour (owner.theme, "kind-video");
            g.setColour (ink.withAlpha (picture.isValid() ? 0.08f : 0.25f));
            g.fillPath (frame);
            g.setColour (ink);
            g.strokePath (frame, juce::PathStrokeType (1.5f));

            for (const auto& corner : corners)
            {
                const auto at = toView (corner);
                g.setColour (juce::Colours::black);
                g.fillRect (juce::Rectangle<float> (handle, handle).withCentre (at));
                g.setColour (ink);
                g.drawRect (juce::Rectangle<float> (handle, handle).withCentre (at), 1.5f);
            }

            //  The turn's handle, a round one above the top edge's middle.
            const auto top = turnHandle (r);
            const auto topMiddle = toView ({ (corners[2].x + corners[3].x) / 2.0, (corners[2].y + corners[3].y) / 2.0 });
            g.drawLine (juce::Line<float> (topMiddle, top), 1.0f);
            g.setColour (juce::Colours::black);
            g.fillEllipse (juce::Rectangle<float> (handle + 2.0f, handle + 2.0f).withCentre (top));
            g.setColour (ink);
            g.drawEllipse (juce::Rectangle<float> (handle + 2.0f, handle + 2.0f).withCentre (top), 1.5f);

            //  The middle, crossed.
            const auto middle = toView (model::frameMiddle (r));
            g.drawLine (middle.x - 5.0f, middle.y, middle.x + 5.0f, middle.y, 1.0f);
            g.drawLine (middle.x, middle.y - 5.0f, middle.x, middle.y + 5.0f, 1.0f);
        }

        void mouseDown (const juce::MouseEvent& event) override
        {
            const auto& r = owner.reading;

            if (r.cueId.empty() || r.locked)
                return;

            hand = r;
            start = r;
            grabbed = Grab::none;
            from = toCanvas (event.position);

            //  A mask's outline is the cue's: a fade does not move it (§94).
            if (r.masked() && r.onFade())
                return;

            if (r.masked())
            {
                const auto point = toMask (event.position);
                const auto near = model::maskPointNear (r.shape, point.x, point.y, maskRadius());

                if (event.mods.isPopupMenu())
                {
                    if (near >= 0)
                        owner.write ("shape", model::maskText (model::maskWithPointRemoved (r.shape, near)));

                    return;
                }

                if (near >= 0)
                {
                    grabbed = Grab::maskPoint;
                    maskIndex = near;
                }

                return;
            }

            if (event.position.getDistanceFrom (turnHandle (r)) <= handle)
            {
                grabbed = Grab::turn;
                return;
            }

            const auto corners = model::frameCorners (r);

            for (const auto& corner : corners)
                if (event.position.getDistanceFrom (toView (corner)) <= handle)
                {
                    grabbed = Grab::corner;
                    from = corner;
                    return;
                }

            if (insideFrame (event.position))
                grabbed = Grab::move;
        }

        void mouseDrag (const juce::MouseEvent& event) override
        {
            if (grabbed == Grab::none)
                return;

            /*  FROM WHERE THE HAND TOOK IT: the document catching up mid-drag
                must not count the travel so far a second time. */
            const auto& r = start;
            const auto to = toCanvas (event.position);

            switch (grabbed)
            {
                case Grab::move:
                {
                    const auto [x, y] = model::offsetsMoved (r, r.offsetX, r.offsetY, from, to);
                    hand.offsetX = x;
                    hand.offsetY = y;
                    owner.hold ("offsetX", x);
                    owner.hold ("offsetY", y);

                    owner.writeMany ({ { owner.addressOf ("offsetX"), numberText (x) },
                                       { owner.addressOf ("offsetY"), numberText (y) } });
                    break;
                }

                case Grab::corner:
                    hand.scale = model::scaleDragged (r, r.scale, from, to);
                    owner.hold ("scale", hand.scale);
                    owner.write ("scale", numberText (hand.scale));
                    break;

                case Grab::turn:
                    hand.rotation = model::rotationDragged (r, r.rotation, from, to, event.mods.isShiftDown());
                    owner.hold ("rotation", hand.rotation);
                    owner.write ("rotation", numberText (hand.rotation));
                    break;

                case Grab::maskPoint:
                {
                    const auto point = toMask (event.position);
                    hand.shape = model::maskWithPointMoved (r.shape, maskIndex, point.x, point.y);
                    owner.write ("shape", model::maskText (hand.shape));
                    break;
                }

                case Grab::none:
                    break;
            }

            dragging = true;
            repaint();
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            dragging = false;
            grabbed = Grab::none;
            repaint();
        }

        void mouseDoubleClick (const juce::MouseEvent& event) override
        {
            const auto& r = owner.reading;

            if (r.cueId.empty() || r.locked || ! r.masked() || r.onFade())
                return;

            //  A CORNER ADDED on the edge nearest; a mask with fewer than three starts as a triangle there.
            const auto point = toMask (event.position);
            auto shape = r.shape;

            if (shape.size() < 3)
                shape = { { point.x - 0.1, point.y + 0.1 }, { point.x + 0.1, point.y + 0.1 }, { point.x, point.y - 0.1 } };
            else
                shape = model::maskWithPointAdded (shape, point.x, point.y);

            owner.write ("shape", model::maskText (shape));
        }

        bool keyPressed (const juce::KeyPress& key) override
        {
            const auto& r = owner.reading;

            if (r.cueId.empty() || r.locked || r.masked())
                return false;

            const auto mods = key.getModifiers();
            const auto code = key.getKeyCode();

            if (mods.isCommandDown() || mods.isCtrlDown() || mods.isAltDown())
                return false;

            /*  FROM WHAT THE HAND LAST SENT (2026-10-20), not from the document
                still catching up: a key held down moves steadily. */
            const auto nudge = [this, &mods] (const char* row, PicturePanelComponent::NumberSlider& slider,
                                              double documentValue, int direction)
            {
                if (! owner.writable (row))
                    return;

                const auto next = model::nudged (owner.shown (row, documentValue), direction, mods.isShiftDown());
                owner.hold (row, next);
                slider.setValue (next, juce::dontSendNotification);
                owner.write (row, numberText (next));
            };

            if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
            {
                nudge ("offsetX", owner.offsetX, r.offsetX, code == juce::KeyPress::leftKey ? -1 : 1);
                return true;
            }

            if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey)
            {
                nudge ("offsetY", owner.offsetY, r.offsetY, code == juce::KeyPress::upKey ? 1 : -1);
                return true;
            }

            return false;
        }

        //  Which of the canvas and the numbers holds the arrow keys is drawn (2026-10-20).
        void focusGained (FocusChangeType) override { repaint(); }
        void focusLost (FocusChangeType) override { repaint(); }

        void readingChanged()
        {
            if (! dragging)
                hand = owner.reading;

            repaint();
        }

    private:
        enum class Grab { none, move, corner, turn, maskPoint };

        PicturePanelComponent& owner;
        model::PictureReading hand, start;
        Grab grabbed = Grab::none;
        model::CanvasPoint from;
        int maskIndex = -1;
        bool dragging = false;
        static constexpr float handle = 9.0f;

        //  The hand's numbers while it is down, the document's otherwise.
        const model::PictureReading& drawn() const noexcept { return dragging ? hand : owner.reading; }

        juce::Rectangle<float> canvasRect() const
        {
            const auto& r = owner.reading;
            const auto area = getLocalBounds().toFloat().reduced (24.0f);
            const auto aspect = r.canvasHeight > 0.0 ? r.canvasWidth / r.canvasHeight : 16.0 / 9.0;
            auto width = area.getWidth();
            auto height = static_cast<float> (width / aspect);

            if (height > area.getHeight())
            {
                height = area.getHeight();
                width = static_cast<float> (height * aspect);
            }

            return juce::Rectangle<float> (width, height).withCentre (area.getCentre());
        }

        //  The canvas's pixels, from its middle with y up, to the view and back.
        juce::Point<float> toView (model::CanvasPoint point) const
        {
            const auto canvas = canvasRect();
            const auto& r = owner.reading;
            const auto x = canvas.getCentreX() + static_cast<float> (point.x / r.canvasWidth) * canvas.getWidth();
            const auto y = canvas.getCentreY() - static_cast<float> (point.y / r.canvasHeight) * canvas.getHeight();
            return { x, y };
        }

        model::CanvasPoint toCanvas (juce::Point<float> at) const
        {
            const auto canvas = canvasRect();
            const auto& r = owner.reading;

            if (canvas.isEmpty())
                return {};

            return { static_cast<double> ((at.x - canvas.getCentreX()) / canvas.getWidth()) * r.canvasWidth,
                     static_cast<double> ((canvas.getCentreY() - at.y) / canvas.getHeight()) * r.canvasHeight };
        }

        //  A mask's corners: 0..1 of the canvas from its top-left (VF).
        model::MaskPoint toMask (juce::Point<float> at) const
        {
            const auto canvas = canvasRect();

            if (canvas.isEmpty())
                return {};

            return { static_cast<double> ((at.x - canvas.getX()) / canvas.getWidth()),
                     static_cast<double> ((at.y - canvas.getY()) / canvas.getHeight()) };
        }

        juce::Point<float> fromMask (const model::MaskPoint& point) const
        {
            const auto canvas = canvasRect();
            return { canvas.getX() + static_cast<float> (point.x) * canvas.getWidth(),
                     canvas.getY() + static_cast<float> (point.y) * canvas.getHeight() };
        }

        double maskRadius() const
        {
            const auto canvas = canvasRect();
            return canvas.getWidth() > 0.0f ? static_cast<double> (handle / canvas.getWidth()) : 0.02;
        }

        juce::Point<float> turnHandle (const model::PictureReading& r) const
        {
            const auto corners = model::frameCorners (r);
            const auto top = toView ({ (corners[2].x + corners[3].x) / 2.0, (corners[2].y + corners[3].y) / 2.0 });
            const auto middle = toView (model::frameMiddle (r));
            auto outwards = top - middle;
            const auto length = outwards.getDistanceFromOrigin();

            if (length < 1.0f)
                return top.translated (0.0f, -24.0f);

            outwards = outwards * (24.0f / length);
            return top + outwards;
        }

        bool insideFrame (juce::Point<float> at) const
        {
            const auto corners = model::frameCorners (owner.reading);
            juce::Path frame;
            frame.startNewSubPath (toView (corners[0]));

            for (std::size_t n = 1; n < corners.size(); ++n)
                frame.lineTo (toView (corners[n]));

            frame.closeSubPath();
            return frame.contains (at);
        }

        void paintOutline (juce::Graphics& g, const model::PictureReading& r) const
        {
            const auto ink = Look::colour (owner.theme, "kind-video");

            if (r.shape.empty())
            {
                g.setColour (Look::colour (owner.theme, "ink-dim"));
                g.setFont (Look::font (owner.theme, 12.0f));
                g.drawFittedText (r.blurred() ? "Blurs the whole canvas: double-click to draw an outline"
                                              : "No outline yet: double-click to start one",
                                  canvasRect().toNearestInt(), juce::Justification::centred, 2);
                return;
            }

            juce::Path outline;

            for (std::size_t at = 0; at < r.shape.size(); ++at)
            {
                const auto point = fromMask (r.shape[at]);

                if (at == 0)
                    outline.startNewSubPath (point);
                else
                    outline.lineTo (point);
            }

            outline.closeSubPath();

            //  A mask's outline in its colour; a blur cue's has none, only a faint wash over the picture (61).
            if (r.blurred())
                g.setColour (ink.withAlpha (r.invert ? 0.05f : 0.12f));
            else
                g.setColour (paintColour (r.paint).withAlpha (r.invert ? 0.15f : 0.45f));

            g.fillPath (outline);
            g.setColour (ink);
            g.strokePath (outline, juce::PathStrokeType (1.5f));

            for (const auto& corner : r.shape)
            {
                const auto at = fromMask (corner);
                g.setColour (juce::Colours::black);
                g.fillEllipse (juce::Rectangle<float> (handle, handle).withCentre (at));
                g.setColour (ink);
                g.drawEllipse (juce::Rectangle<float> (handle, handle).withCentre (at), 1.5f);
            }
        }
    };

    //==========================================================================
    /*  THE FOUR CURVES, one picked by its letter, the others faint behind. */
    class PicturePanelComponent::Curves final : public juce::Component
    {
    public:
        explicit Curves (PicturePanelComponent& ownerToUse) : owner (ownerToUse) {}

        void paint (juce::Graphics& g) override
        {
            const auto box = square();
            g.setColour (juce::Colours::black);
            g.fillRect (box);
            g.setColour (Look::colour (owner.theme, "rule"));
            g.drawRect (box, 1.0f);

            for (int step = 1; step < 4; ++step)
            {
                const auto x = box.getX() + box.getWidth() * static_cast<float> (step) / 4.0f;
                const auto y = box.getY() + box.getHeight() * static_cast<float> (step) / 4.0f;
                g.setColour (Look::colour (owner.theme, "rule").withAlpha (0.4f));
                g.drawVerticalLine (juce::roundToInt (x), box.getY(), box.getBottom());
                g.drawHorizontalLine (juce::roundToInt (y), box.getX(), box.getRight());
            }

            for (int which = 0; which < 4; ++which)
                if (which != owner.curvePicked)
                    paintCurve (g, which, false);

            paintCurve (g, owner.curvePicked, true);
        }

        void mouseDown (const juce::MouseEvent& event) override
        {
            const auto& r = owner.reading;

            if (r.cueId.empty() || r.locked || ! r.graded())
                return;

            const auto points = model::curveOrLine (r.curves[static_cast<std::size_t> (owner.curvePicked)]);
            const auto [in, out] = toCurve (event.position);
            const auto near = model::curvePointNear (points, in, out, radius());

            if (event.mods.isPopupMenu())
            {
                if (near >= 0)
                    owner.writeCurve (owner.curvePicked, model::curveWithPointRemoved (points, near));

                held = -1;
                return;
            }

            if (near >= 0)
            {
                held = near;
                hand = points;
                return;
            }

            hand = model::curveWithPointAdded (points, in, out);
            held = model::curvePointNear (hand, in, out, 1.0e-9);
            owner.writeCurve (owner.curvePicked, hand);
        }

        void mouseDrag (const juce::MouseEvent& event) override
        {
            if (held < 0)
                return;

            const auto [in, out] = toCurve (event.position);
            hand = model::curveWithPointMoved (hand, held, in, out);
            owner.writeCurve (owner.curvePicked, hand);
            repaint();
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            held = -1;
            repaint();
        }

    private:
        PicturePanelComponent& owner;
        model::CurvePoints hand;
        int held = -1;

        juce::Rectangle<float> square() const
        {
            const auto area = getLocalBounds().toFloat().reduced (6.0f);
            const auto side = std::min (area.getWidth(), area.getHeight());
            return juce::Rectangle<float> (side, side).withCentre (area.getCentre());
        }

        std::pair<double, double> toCurve (juce::Point<float> at) const
        {
            const auto box = square();

            if (box.isEmpty())
                return { 0.0, 0.0 };

            return { std::clamp (static_cast<double> ((at.x - box.getX()) / box.getWidth()), 0.0, 1.0),
                     std::clamp (static_cast<double> ((box.getBottom() - at.y) / box.getHeight()), 0.0, 1.0) };
        }

        juce::Point<float> fromCurve (double in, double out) const
        {
            const auto box = square();
            return { box.getX() + static_cast<float> (in) * box.getWidth(),
                     box.getBottom() - static_cast<float> (out) * box.getHeight() };
        }

        double radius() const
        {
            const auto box = square();
            return box.getWidth() > 0.0f ? 8.0 / static_cast<double> (box.getWidth()) : 0.03;
        }

        juce::Colour colourOf (int which) const
        {
            switch (which)
            {
                case 1:  return juce::Colour (0xffe05050);
                case 2:  return juce::Colour (0xff50c060);
                case 3:  return juce::Colour (0xff5080f0);
                default: return Look::colour (owner.theme, "ink");
            }
        }

        void paintCurve (juce::Graphics& g, int which, bool picked) const
        {
            const auto& stored = owner.reading.curves[static_cast<std::size_t> (which)];
            const auto& points = picked && held >= 0 ? hand : model::curveOrLine (stored);

            if (! picked && stored.empty())
                return;

            juce::Path line;
            line.startNewSubPath (fromCurve (0.0, points.front().second));

            for (const auto& [in, out] : points)
                line.lineTo (fromCurve (in, out));

            line.lineTo (fromCurve (1.0, points.back().second));

            g.setColour (colourOf (which).withAlpha (picked ? 1.0f : 0.35f));
            g.strokePath (line, juce::PathStrokeType (picked ? 2.0f : 1.0f));

            if (! picked)
                return;

            for (const auto& [in, out] : points)
            {
                const auto at = fromCurve (in, out);
                g.setColour (juce::Colours::black);
                g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre (at));
                g.setColour (colourOf (which));
                g.drawEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre (at), 1.5f);
            }
        }
    };

    //==========================================================================
    PicturePanelComponent::PicturePanelComponent (const model::Theme& themeToUse, Actions actionsToUse)
        : theme (themeToUse), actions (std::move (actionsToUse))
    {
        view = std::make_unique<View> (*this);
        curves = std::make_unique<Curves> (*this);
        addAndMakeVisible (*view);
        addAndMakeVisible (*curves);

        for (int at = 0; at < 3; ++at)
        {
            auto& button = fitButtons[at];
            button.setButtonText (fitWords[at]);
            button.setClickingTogglesState (false);
            button.setWantsKeyboardFocus (false);
            button.setTooltip (at == 0 ? "The whole picture inside the canvas, its shape kept"
                             : at == 1 ? "The canvas covered, its shape kept"
                                       : "The canvas covered, its shape not kept");
            button.onClick = [this, at] { write ("fit", fitWords[at]); };
            addAndMakeVisible (button);
        }

        for (auto* toggle : { &flipH, &flipV, &invert })
        {
            toggle->setWantsKeyboardFocus (false);
            addAndMakeVisible (*toggle);
        }

        flipH.onClick = [this] { write ("flipH", flipH.getToggleState() ? "true" : "false"); };
        flipV.onClick = [this] { write ("flipV", flipV.getToggleState() ? "true" : "false"); };
        invert.onClick = [this] { write ("invert", invert.getToggleState() ? "true" : "false"); };

        build (scale, scaleLabel, "size", "scale", 0.0, 1000.0, 100.0, false, " %", 1, 0.1);
        build (offsetX, offsetXLabel, "across", "offsetX", -1000.0, 1000.0, 0.0, true, " %", 1, 0.1);
        build (offsetY, offsetYLabel, "up", "offsetY", -1000.0, 1000.0, 0.0, true, " %", 1, 0.1);
        build (rotation, rotationLabel, "turn", "rotation", -3600.0, 3600.0, 0.0, true, " deg", 1, 0.1);
        build (contrast, contrastLabel, "contrast", "contrast", 0.0, 400.0, 100.0, false, " %", 0, 1.0);
        build (saturation, saturationLabel, "saturation", "saturation", 0.0, 400.0, 100.0, false, " %", 0, 1.0);
        build (gamma, gammaLabel, "gamma", "gamma", 0.1, 10.0, 1.0, false, "", 2, 0.01);
        build (hue, hueLabel, "hue", "hue", -180.0, 180.0, 0.0, true, " deg", 0, 1.0);
        build (feather, featherLabel, "feather", "feather", 0.0, 400.0, 20.0, false, " px", 0, 1.0);

        for (int at = 0; at < 4; ++at)
        {
            auto& button = curvePicks[at];
            button.setButtonText (curveWords[at]);
            button.setWantsKeyboardFocus (false);
            button.setTooltip (at == 0 ? "The luminosity's curve" : at == 1 ? "Red's curve" : at == 2 ? "Green's curve" : "Blue's curve");
            button.onClick = [this, at]
            {
                curvePicked = at;
                refresh();
                curves->repaint();
            };
            addAndMakeVisible (button);
        }

        curveReset.setWantsKeyboardFocus (false);
        curveReset.setTooltip ("This curve back to the straight line, which changes nothing");
        curveReset.onClick = [this] { write (curveRows[curvePicked], ""); };
        addAndMakeVisible (curveReset);

        paintSwatch.setWantsKeyboardFocus (false);
        paintSwatch.setTooltip ("The colour of the fill or the mask");
        paintSwatch.onClick = [this] { chooseColour ("paint", reading.paint, paintSwatch); };
        addAndMakeVisible (paintSwatch);
        addAndMakeVisible (paintLabel);

        //  THE GRADE OR THE KEY (namespace draft 61).
        for (int at = 0; at < 2; ++at)
        {
            auto& button = colourPages[at];
            button.setWantsKeyboardFocus (false);
            button.setTooltip (at == 0 ? "Contrast, saturation, gamma, hue and the curves"
                                       : "What makes it see-through: its own alpha, its brightness or a colour");
            button.onClick = [this, at]
            {
                colourPage = at;
                refresh();
            };
            addAndMakeVisible (button);
        }

        useAlpha.setWantsKeyboardFocus (false);
        useAlpha.onClick = [this] { write ("useAlpha", useAlpha.getToggleState() ? "true" : "false"); };
        keyInvert.setWantsKeyboardFocus (false);
        keyInvert.setTooltip ("Keep what the key would clear");
        keyInvert.onClick = [this] { write ("keyInvert", keyInvert.getToggleState() ? "true" : "false"); };
        addAndMakeVisible (useAlpha);
        addAndMakeVisible (keyInvert);

        for (int at = 0; at < 4; ++at)
            keyMode.addItem (keyWords[at], at + 1);

        keyMode.setWantsKeyboardFocus (false);
        keyMode.onChange = [this]
        {
            const auto picked = keyMode.getSelectedId() - 1;

            if (picked >= 0 && picked < 4 && reading.key != keyRows[picked])
                writeMany (model::keyChoice (reading, keyRows[picked]));
        };
        addAndMakeVisible (keyMode);

        build (keyLevel, keyLevelLabel, "cut at", "keyLevel", 0.0, 100.0, 50.0, false, " %", 0, 1.0);
        build (keySoftness, keySoftnessLabel, "softness", "keySoftness", 0.0, 100.0, 50.0, false, " %", 0, 1.0);
        build (keySpill, keySpillLabel, "spill", "keySpill", 0.0, 100.0, 50.0, false, " %", 0, 1.0);
        keyLevel.setTooltip ("Where the cut falls: the brightness, or how far from the screen's colour, at which a point is half solid");
        keySoftness.setTooltip ("How gradual the cut is: nought a hard edge");
        keySpill.setTooltip ("How much of the screen's colour is taken off what is kept - the green on hair and shoulders");

        /*  ONE SWATCH, two colours: a colour key's screen, a shape's paint. */
        keyColourSwatch.setWantsKeyboardFocus (false);
        keyColourSwatch.onClick = [this]
        {
            if (reading.key == "matte")
                chooseColour ("paint", reading.paint, keyColourSwatch);
            else
                chooseColour ("keyColour", reading.keyColour, keyColourSwatch);
        };
        addAndMakeVisible (keyColourSwatch);
        addAndMakeVisible (keyColourLabel);

        /*  A TEXT CUE'S WORDS (61): typed here, written when the hand leaves
            them, as the inspector's long rows are; a line break is a line. */
        wordsBox.setMultiLine (true, true);
        wordsBox.setReturnKeyStartsNewLine (true);
        wordsBox.setTooltip ("The words; written when you leave the box");
        wordsBox.onFocusLost = [this] { writeWords(); };
        wordsBox.onEscapeKey = [this]
        {
            wordsBox.setText (juce::String::fromUTF8 (reading.words.c_str()), false);
            unfocusAllComponents();
        };
        addAndMakeVisible (wordsBox);

        fontMenu.setWantsKeyboardFocus (false);
        fontMenu.setTooltip ("The font: one the show carries, or one this machine has");
        fontMenu.onChange = [this]
        {
            const auto id = fontMenu.getSelectedId();

            if (id == defaultFontId)
                write ("font", "");
            else if (id >= carriedFontId && id - carriedFontId < static_cast<int> (reading.carried.size()))
                write ("font", reading.carried[static_cast<std::size_t> (id - carriedFontId)].family);
            else if (id == carryFontId)
            {
                askFontJob ("carry", reading.font, false);
                refreshFonts();
            }
            else if (id == addFontFileId)
            {
                refreshFonts();
                fontChooser = std::make_unique<juce::FileChooser> ("Add a font file to the show", juce::File(),
                                                                   "*.ttf;*.otf;*.ttc;*.otc");
                fontChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                          [safe = juce::Component::SafePointer<PicturePanelComponent> (this)] (const juce::FileChooser& chooser)
                                          {
                                              const auto file = chooser.getResult();
                                              if (safe != nullptr && file.existsAsFile())
                                                  safe->askFontJob ("add", file.getFullPathName().toStdString(), false);
                                          });
            }
            else if (id == removeFontId)
            {
                const auto family = reading.font;
                refreshFonts();
                juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                                .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                                .withTitle ("Take " + juce::String::fromUTF8 (family.c_str()) + " out of the show?")
                                                .withMessage ("Its files are deleted from the show's fonts folder. Its text cues"
                                                              " then draw in the font of that name each machine has, or in the"
                                                              " system's sans-serif where there is none.")
                                                .withButton ("Take it out")
                                                .withButton ("Keep it")
                                                .withAssociatedComponent (this),
                                              [safe = juce::Component::SafePointer<PicturePanelComponent> (this), family] (int result)
                                              {
                                                  if (result == 1 && safe != nullptr)
                                                      safe->askFontJob ("remove", family, false);
                                              });
            }
            else if (id > defaultFontId && id - 2 < fonts.size())
                write ("font", fonts[id - 2].toStdString());
        };
        addAndMakeVisible (fontMenu);
        fonts = juce::Font::findAllTypefaceNames();
        fonts.sortNatural();

        for (auto* toggle : { &bold, &italic })
        {
            toggle->setWantsKeyboardFocus (false);
            addAndMakeVisible (*toggle);
        }

        bold.onClick = [this] { write ("bold", bold.getToggleState() ? "true" : "false"); };
        italic.onClick = [this] { write ("italic", italic.getToggleState() ? "true" : "false"); };

        for (int at = 0; at < 3; ++at)
        {
            alignButtons[at].setButtonText (alignWords[at]);
            alignButtons[at].setWantsKeyboardFocus (false);
            alignButtons[at].setTooltip ("Each line to the " + juce::String (alignRows[at]) + " of the box the words wrap in");
            alignButtons[at].onClick = [this, at] { write ("align", alignRows[at]); };
            addAndMakeVisible (alignButtons[at]);
            alignVMenu.addItem (alignVWords[at], at + 1);
        }

        alignVMenu.setWantsKeyboardFocus (false);
        alignVMenu.setTooltip ("Where the block of lines lies on the canvas");
        alignVMenu.onChange = [this]
        {
            const auto picked = alignVMenu.getSelectedId() - 1;

            if (picked >= 0 && picked < 3)
                write ("alignV", alignVRows[picked]);
        };
        addAndMakeVisible (alignVMenu);

        build (fontSize, fontSizeLabel, "letters", "fontSize", 0.1, 100.0, 8.0, false, " %", 1, 0.1);
        build (lineSpacing, lineSpacingLabel, "spacing", "lineSpacing", 50.0, 400.0, 100.0, false, " %", 0, 1.0);
        build (wrap, wrapLabel, "wrap at", "wrap", 0.0, 100.0, 50.0, false, " %", 0, 1.0);
        build (outlineWidth, outlineLabel, "outline", "outline", 0.0, 200.0, 8.0, false, " px", 1, 0.1);
        build (outlineBlur, outlineBlurLabel, "glow", "outlineBlur", 0.0, 200.0, 20.0, false, " px", 1, 0.1);
        fontSize.setTooltip ("How tall the letters are, in % of the canvas's height");
        wrap.setTooltip ("How wide the box the words wrap in is, in % of the canvas's width; nought never wraps");
        outlineBlur.setTooltip ("How far the outline is blurred into a glow");

        letterSwatch.setWantsKeyboardFocus (false);
        letterSwatch.setTooltip ("The letters' colour");
        letterSwatch.onClick = [this] { chooseColour ("paint", reading.paint, letterSwatch); };
        addAndMakeVisible (letterSwatch);

        outlineSwatch.setWantsKeyboardFocus (false);
        outlineSwatch.setTooltip ("The outline's colour");
        outlineSwatch.onClick = [this] { chooseColour ("outlineColour", reading.outlineColour, outlineSwatch); };
        addAndMakeVisible (outlineSwatch);

        /*  A BLUR CUE'S AMOUNT AND ITS OUTLINE OR NONE (61, AKN). */
        build (blurAmount, blurAmountLabel, "blur", "blur", 0.0, 500.0, 40.0, false, " px", 0, 1.0);
        blurAmount.setTooltip ("How far it blurs, a radius in canvas pixels; a fade moves it");

        for (auto* button : { &wholeCanvas, &aShape })
        {
            button->setWantsKeyboardFocus (false);
            addAndMakeVisible (*button);
        }

        wholeCanvas.setTooltip ("Blur everything beneath it, no outline");
        aShape.setTooltip ("Blur inside an outline, a square to start from");
        wholeCanvas.onClick = [this] { write ("shape", ""); };
        aShape.onClick = [this] { write ("shape", "0.25 0.25 0.75 0.25 0.75 0.75 0.25 0.75"); };

        /*  ITS FIELDS (61, AKM), a page of the column picked beside the font. */
        fieldsPick.setWantsKeyboardFocus (false);
        fieldsPick.setTooltip ("The blanks in the words, {name}, and what fills them tonight");
        fieldsPick.onClick = [this]
        {
            fieldsPage = ! fieldsPage;
            refresh();
        };
        addAndMakeVisible (fieldsPick);

        noFields.setText ("No fields: write {name} in the words, and send /godot/text/name to fill it",
                          juce::dontSendNotification);
        noFields.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (noFields);

        /*  ON A FADE (namespace draft §94): how solid and a movie's speed, and
            the tick beside each value saying whether the fade moves it. */
        build (opacity, opacityLabel, "opacity", "opacity", 0.0, 100.0, 50.0, false, " %", 0, 1.0);
        build (speed, speedLabel, "speed", "rate", -20.0, 20.0, 1.0, true, "", 3, 0.01);
        opacity.setTooltip ("How solid the fade leaves it, in %");
        speed.setTooltip ("The movie's speed the fade takes it to: one is the file's own");

        for (int at = 0; at < 7; ++at)
        {
            auto& tick = fadeTicks[at];
            tick.setWantsKeyboardFocus (false);
            tick.onClick = [this, at] { tickFade (at, fadeTicks[at].getToggleState()); };
            addAndMakeVisible (tick);
        }

        fadeWords.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (fadeWords);

        applyTheme (theme);
    }

    PicturePanelComponent::~PicturePanelComponent() = default;

    void PicturePanelComponent::build (NumberSlider& slider, juce::Label& label, const char* words, const char* row,
                                       double low, double high, double mid, bool symmetric, const char* suffix,
                                       int decimals, double step)
    {
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 80, 22);
        slider.setRange (low, high, 0.0);
        slider.setNumDecimalPlacesToDisplay (decimals);
        slider.setTextValueSuffix (suffix);

        /*  THE ARROW KEYS STEP THE NUMBER CLICKED (2026-10-20): from what the
            hand last sent, not from the document still catching up, so a key
            held down climbs steadily. */
        slider.setWantsKeyboardFocus (true);
        slider.onFocusChange = [this] { repaint(); };
        slider.onArrow = [this, &slider, row, step, low, high] (const juce::KeyPress& key)
        {
            const auto code = key.getKeyCode();
            const auto up = code == juce::KeyPress::rightKey || code == juce::KeyPress::upKey;
            const auto down = code == juce::KeyPress::leftKey || code == juce::KeyPress::downKey;
            const auto mods = key.getModifiers();

            if (! (up || down) || mods.isCommandDown() || mods.isCtrlDown() || mods.isAltDown())
                return false;

            //  No cue, or under the lock: the arrows go on to the window, as from the canvas.
            if (reading.cueId.empty() || reading.locked)
                return false;

            if (! writable (row))
                return true;

            const auto next = model::stepped (slider.getValue(), step, up ? 1 : -1, mods.isShiftDown(), low, high);
            hold (row, next);
            slider.setValue (next, juce::dontSendNotification);
            write (row, numberText (next));
            return true;
        };
        numbers.emplace_back (&slider, &label);

        //  FINE WHERE IT MATTERS: about the middle for an offset and a turn, low down for a size.
        if (symmetric)
            slider.setSkewFactor (0.4, true);
        else
            slider.setSkewFactorFromMidPoint (mid);

        slider.setDoubleClickReturnValue (true, mid);
        //  A hand's change only: the document's own is set without a notification.
        slider.onValueChange = [this, &slider, row]
        {
            if (writable (row))
                hold (row, slider.getValue());

            write (row, numberText (slider.getValue()));
        };

        label.setText (words, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (slider);
        addAndMakeVisible (label);
    }

    std::string PicturePanelComponent::addressOf (const char* row) const
    {
        if (reading.onFade())
            return model::fadeMoveAddress (reading.fadeId, std::string ("video/") + row);

        return "/godot/cue/" + reading.cueId + "/" + row;
    }

    bool PicturePanelComponent::writable (const char* row) const
    {
        if (reading.cueId.empty() || reading.locked || actions.set == nullptr)
            return false;

        /*  ON A FADE only what it moves, through its door: a number sets the
            pair, and so ticks it (§94). */
        if (reading.onFade())
        {
            const auto movable = reading.fadeRows();
            return std::find (movable.begin(), movable.end(), std::string (row)) != movable.end();
        }

        return true;
    }

    void PicturePanelComponent::write (const char* row, const std::string& text)
    {
        if (writable (row))
            actions.set (addressOf (row), text);
    }

    void PicturePanelComponent::hold (const char* row, double value)
    {
        heldNumbers.send (row, value, juce::Time::getMillisecondCounterHiRes());
    }

    double PicturePanelComponent::shown (const char* row, double documentValue)
    {
        return heldNumbers.valueOf (row, documentValue, juce::Time::getMillisecondCounterHiRes());
    }

    void PicturePanelComponent::writeMany (const std::vector<std::pair<std::string, std::string>>& rows)
    {
        if (reading.cueId.empty() || reading.locked || rows.empty())
            return;

        //  A fade's door takes one pair a write.
        if (rows.size() == 1 || actions.setMany == nullptr || reading.onFade())
        {
            if (actions.set != nullptr)
                for (const auto& [address, text] : rows)
                    actions.set (address, text);

            return;
        }

        actions.setMany (rows);
    }

    juce::Slider* PicturePanelComponent::fadeSlider (int at)
    {
        juce::Slider* sliders[7] { &opacity, &scale, &offsetX, &offsetY, &rotation, &speed, &blurAmount };
        return at >= 0 && at < 7 ? sliders[at] : nullptr;
    }

    juce::Label* PicturePanelComponent::fadeLabel (int at)
    {
        juce::Label* labels[7] { &opacityLabel, &scaleLabel, &offsetXLabel, &offsetYLabel, &rotationLabel,
                                 &speedLabel, &blurAmountLabel };
        return at >= 0 && at < 7 ? labels[at] : nullptr;
    }

    void PicturePanelComponent::tickFade (int at, bool moves)
    {
        /*  TICKED, IT IS MOVED TO WHERE IT STANDS - nothing seen changes until
            the number is moved; CLEARED, the pair goes (§26, PF; §94). */
        if (at < 0 || at >= 7 || ! reading.onFade() || reading.locked || actions.set == nullptr)
            return;

        if (moves)
            write (fadeRowNames[at], numberText (fadeSlider (at)->getValue()));
        else
            actions.set (addressOf (fadeRowNames[at]), {});
    }

    /*  A FIELD'S VALUE SENT (61, AKM): at once, and under the lock too - a
        field is tonight's, never the show's, so the lock that keeps the show
        from changing has nothing to keep here. */
    void PicturePanelComponent::sendField (FieldRow& row)
    {
        const auto typed = row.value.getText().toStdString();

        for (const auto& field : reading.fields)
            if (field.name == row.name && field.value == typed)
                return;

        if (actions.set != nullptr)
            actions.set (row.address, typed);
    }

    /*  THE ROWS, one a blank, made again only when the blanks are others;
        a value under a hand is the hand's. */
    void PicturePanelComponent::refreshFields()
    {
        auto same = fieldRows.size() == reading.fields.size();

        for (std::size_t at = 0; same && at < fieldRows.size(); ++at)
            same = fieldRows[at]->name == reading.fields[at].name;

        if (! same)
        {
            for (auto& row : fieldRows)
            {
                removeChildComponent (&row->label);
                removeChildComponent (&row->value);
                removeChildComponent (&row->copy);
            }

            fieldRows.clear();

            for (const auto& field : reading.fields)
            {
                auto row = std::make_unique<FieldRow>();
                auto* made = row.get();
                made->name = field.name;
                made->address = field.address;
                made->label.setText (juce::String::fromUTF8 (field.name.c_str()), juce::dontSendNotification);
                made->label.setTooltip (juce::String::fromUTF8 (field.address.c_str()));
                made->label.setColour (juce::Label::textColourId, Look::colour (theme, "ink-dim"));
                made->label.setFont (Look::font (theme, 12.0f));
                made->value.setTooltip ("What {" + juce::String::fromUTF8 (field.name.c_str())
                                          + "} shows tonight; sent when you press Return or leave the box");
                made->value.onReturnKey = [this, made] { sendField (*made); };
                made->value.onFocusLost = [this, made] { sendField (*made); };
                made->copy.setWantsKeyboardFocus (false);
                made->copy.setTooltip ("Copy " + juce::String::fromUTF8 (field.address.c_str()));
                made->copy.onClick = [made]
                {
                    juce::SystemClipboard::copyTextToClipboard (juce::String::fromUTF8 (made->address.c_str()));
                };
                addChildComponent (made->label);
                addChildComponent (made->value);
                addChildComponent (made->copy);
                fieldRows.push_back (std::move (row));
            }
        }

        for (std::size_t at = 0; at < fieldRows.size(); ++at)
            if (! fieldRows[at]->value.hasKeyboardFocus (true))
                fieldRows[at]->value.setText (juce::String::fromUTF8 (reading.fields[at].value.c_str()), false);
    }

    void PicturePanelComponent::writeWords()
    {
        const auto typed = wordsBox.getText().toStdString();

        if (typed != reading.words)
            write ("words", typed);
    }

    /*  THE FONT MENU: the default first, then the families the show carries
        (§62), which draw the same on every machine, then this machine's others,
        and the cue's own last, said as missing, when neither has it (61, AKL) -
        kept in the menu so it is not lost by looking. Under them, what can be
        done with fonts: carry the cue's installed font in the show, add a font
        file, take the cue's font out of the show - each offered only where it
        means something, never under the lock. */
    void PicturePanelComponent::refreshFonts()
    {
        fontsFilled = true;
        fontShown = reading.font;
        carriedShown.clear();
        for (const auto& carried : reading.carried)
            carriedShown += carried.family + "\t" + carried.styles + "\n";

        fontMenu.clear (juce::dontSendNotification);
        fontMenu.addItem ("System sans-serif", defaultFontId);

        const auto own = juce::String::fromUTF8 (reading.font.c_str());
        int selected = own.isEmpty() ? defaultFontId : 0;

        if (! reading.carried.empty())
        {
            fontMenu.addSectionHeading ("In this show");
            for (std::size_t at = 0; at < reading.carried.size(); ++at)
            {
                const auto& carried = reading.carried[at];
                const auto name = juce::String::fromUTF8 (carried.family.c_str());
                fontMenu.addItem (name, carriedFontId + static_cast<int> (at));
                if (selected == 0 && name.equalsIgnoreCase (own))
                    selected = carriedFontId + static_cast<int> (at);
            }
            fontMenu.addSectionHeading ("On this machine");
        }

        for (int at = 0; at < fonts.size(); ++at)
            if (! carries (fonts[at].toStdString()))
            {
                fontMenu.addItem (fonts[at], at + 2);
                if (selected == 0 && fonts[at].equalsIgnoreCase (own))
                    selected = at + 2;
            }

        if (selected == 0)
        {
            fontMenu.addItem (own + " (not in the show, not on this machine)", missingFontId);
            selected = missingFontId;
        }

        const auto installedHere = fonts.contains (own, true);
        const auto carriedHere = carries (reading.font);
        const auto ownCarried = std::any_of (reading.carried.begin(), reading.carried.end(), [&own] (const model::CarriedFamily& c)
        {
            return ! c.around && juce::String::fromUTF8 (c.family.c_str()).equalsIgnoreCase (own);
        });

        fontMenu.addSeparator();
        fontMenu.addItem ("Carry it in the show", carryFontId);
        fontMenu.addItem ("Add a font file...", addFontFileId);
        fontMenu.addItem ("Remove it from the show", removeFontId);
        fontMenu.setItemEnabled (carryFontId, ! reading.locked && installedHere && ! carriedHere);
        fontMenu.setItemEnabled (addFontFileId, ! reading.locked);
        fontMenu.setItemEnabled (removeFontId, ! reading.locked && ownCarried);

        fontMenu.setSelectedId (selected, juce::dontSendNotification);
    }

    bool PicturePanelComponent::carries (const std::string& family) const
    {
        const auto name = juce::String::fromUTF8 (family.c_str());
        return ! family.empty()
            && std::any_of (reading.carried.begin(), reading.carried.end(), [&name] (const model::CarriedFamily& c)
               {
                   return juce::String::fromUTF8 (c.family.c_str()).equalsIgnoreCase (name);
               });
    }

    /*  A fonts.* COMMAND ASKED (§62, ALA), and the job it becomes followed in
        the panel's head: the engine does it on a thread of its own and says
        what it did at /godot/engine/fontsJob. */
    void PicturePanelComponent::askFontJob (const std::string& verb, const std::string& subject, bool restricted)
    {
        if (subject.empty())
            return;

        fontJobWaiting = subject;
        fontJobSeen = model::fontsJobOf (reading.fontsJob).number;

        if (verb == "carry" && actions.carryFont)
            actions.carryFont (subject, restricted);
        else if (verb == "add" && actions.addFontFile)
            actions.addFontFile (subject, restricted);
        else if (verb == "remove" && actions.removeFont)
            actions.removeFont (subject);
    }

    void PicturePanelComponent::followFontJob()
    {
        if (fontJobWaiting.empty())
            return;

        const auto job = model::fontsJobOf (reading.fontsJob);
        if (job.number <= fontJobSeen || job.subject != fontJobWaiting)
            return;

        if (actions.say)
            actions.say (juce::String::fromUTF8 (model::fontsJobWords (job).c_str()));

        if (job.state != "done")
            return;

        fontJobWaiting.clear();

        /*  A LICENCE THAT RESTRICTS CARRYING IT (ALB): asked, and carried only
            on a yes - the same job asked again, saying so. */
        if (job.result == "restricted")
        {
            const auto verb = job.verb;
            const auto subject = job.subject;
            juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                            .withIconType (juce::MessageBoxIconType::WarningIcon)
                                            .withTitle ("Carry it all the same?")
                                            .withMessage (juce::String::fromUTF8 (job.detail.c_str())
                                                          + ": the font's licence restricts carrying it in a show. A show given"
                                                            " to somebody else takes its fonts with it.")
                                            .withButton ("Carry it")
                                            .withButton ("Leave it")
                                            .withAssociatedComponent (this),
                                          [safe = juce::Component::SafePointer<PicturePanelComponent> (this), verb, subject] (int result)
                                          {
                                              if (result == 1 && safe != nullptr)
                                                  safe->askFontJob (verb, subject, true);
                                          });
        }
    }

    void PicturePanelComponent::writeCurve (int which, const model::CurvePoints& points)
    {
        write (curveRows[which], model::curveText (points));
    }

    void PicturePanelComponent::show (const model::FootReading& footReading,
                                      std::shared_ptr<const audio::MediaRecords> media)
    {
        auto next = footReading.picture;

        //  THE PICTURE'S OWN SIZE, from the media table: what its frame's shape is when it is fitted.
        if (media != nullptr && ! next.file.empty())
            if (const auto found = media->find (next.file); found != media->end())
            {
                next.pictureWidth = static_cast<double> (found->second.width);
                next.pictureHeight = static_cast<double> (found->second.height);
            }

        //  Another cue, or the same through another fade: nothing the hand sent is held for it.
        if (const auto whose = next.cueId + "/" + next.fadeId; whose != heldFor)
        {
            heldNumbers.clear();
            heldFor = whose;
        }

        reading = std::move (next);
        refresh();
        view->readingChanged();
        curves->repaint();
    }

    void PicturePanelComponent::setPicture (const juce::Image& picture)
    {
        view->picture = picture;
        view->repaint();
    }

    void PicturePanelComponent::refresh()
    {
        /*  A NUMBER UNDER A HAND IS THE HAND'S: the document's catches up
            behind it, and is not pushed back into a slider being dragged. */
        const auto setValue = [this] (juce::Slider& slider, const char* row, double value)
        {
            //  And a number the hand has sent is the hand's until the document has it.
            value = shown (row, value);

            if (! slider.isMouseButtonDown() && ! juce::exactlyEqual (slider.getValue(), value))
                slider.setValue (value, juce::dontSendNotification);
        };

        setValue (scale, "scale", reading.scale);
        setValue (offsetX, "offsetX", reading.offsetX);
        setValue (offsetY, "offsetY", reading.offsetY);
        setValue (rotation, "rotation", reading.rotation);
        setValue (contrast, "contrast", reading.contrast);
        setValue (saturation, "saturation", reading.saturation);
        setValue (gamma, "gamma", reading.gamma);
        setValue (hue, "hue", reading.hue);
        setValue (feather, "feather", reading.feather);
        setValue (keyLevel, "keyLevel", reading.keyLevel);
        setValue (keySoftness, "keySoftness", reading.keySoftness);
        setValue (keySpill, "keySpill", reading.keySpill);
        setValue (fontSize, "fontSize", reading.fontSize);
        setValue (lineSpacing, "lineSpacing", reading.lineSpacing);
        setValue (wrap, "wrap", reading.wrap);
        setValue (outlineWidth, "outline", reading.outline);
        setValue (outlineBlur, "outlineBlur", reading.outlineBlur);
        setValue (blurAmount, "blur", reading.blur);
        setValue (opacity, "opacity", reading.opacity);
        setValue (speed, "rate", reading.rate);
        wholeCanvas.setToggleState (reading.shape.size() < 3, juce::dontSendNotification);
        aShape.setToggleState (reading.shape.size() >= 3, juce::dontSendNotification);

        //  THE WORDS UNDER A HAND ARE THE HAND'S, as a slider's number is.
        if (! wordsBox.hasKeyboardFocus (true))
            wordsBox.setText (juce::String::fromUTF8 (reading.words.c_str()), false);

        bold.setToggleState (reading.bold, juce::dontSendNotification);
        italic.setToggleState (reading.italic, juce::dontSendNotification);

        for (int at = 0; at < 3; ++at)
        {
            alignButtons[at].setToggleState (reading.align == alignRows[at], juce::dontSendNotification);

            if (reading.alignV == alignVRows[at])
                alignVMenu.setSelectedId (at + 1, juce::dontSendNotification);
        }

        letterSwatch.setColour (juce::TextButton::buttonColourId, paintColour (reading.paint));
        outlineSwatch.setColour (juce::TextButton::buttonColourId, paintColour (reading.outlineColour));

        //  The menu made again only when the cue's font, or the show's fonts, are others: never under an open menu.
        std::string carriedNow;
        for (const auto& carried : reading.carried)
            carriedNow += carried.family + "\t" + carried.styles + "\n";

        if (reading.texted() && (! fontsFilled || reading.font != fontShown || carriedNow != carriedShown))
            refreshFonts();

        followFontJob();

        flipH.setToggleState (reading.flipH, juce::dontSendNotification);
        flipV.setToggleState (reading.flipV, juce::dontSendNotification);
        invert.setToggleState (reading.invert, juce::dontSendNotification);
        useAlpha.setToggleState (reading.useAlpha, juce::dontSendNotification);
        keyInvert.setToggleState (reading.keyInvert, juce::dontSendNotification);

        for (int at = 0; at < 4; ++at)
            if (reading.key == keyRows[at])
                keyMode.setSelectedId (at + 1, juce::dontSendNotification);

        /*  THE PICK SAYS A KEY IS ON IN WORDS, never by a colour alone (§4.8),
            so a cue keyed and left on the Grade page is not keyed unseen. */
        colourPages[0].setButtonText ("Grade");
        colourPages[1].setButtonText (reading.keyed() ? "Key (on)" : "Key");

        for (int at = 0; at < 2; ++at)
            colourPages[at].setToggleState (at == colourPage, juce::dontSendNotification);

        /*  A CAPTURE'S ALPHA only when its input sends one (AKF): the switch is
            the cue's, and says why it changes nothing. */
        const auto inputSaysNone = reading.source == "capture" && ! reading.inputAlpha;
        useAlpha.setTooltip (inputSaysNone ? "Its video input sends no alpha: say so in the Video tab, then this reads it"
                                           : "Where the picture says it is see-through, it is");

        const auto shape = reading.key == "matte";
        keyColourSwatch.setColour (juce::TextButton::buttonColourId,
                                   paintColour (shape ? reading.paint : reading.keyColour));
        keyColourSwatch.setTooltip (shape ? "The colour the shape is painted in" : "The screen's colour, which is cleared");
        keyColourLabel.setText (shape ? "colour " + juce::String (reading.paint.empty() ? std::string ("#000000") : reading.paint)
                                      : "screen " + juce::String (reading.keyColour),
                                juce::dontSendNotification);

        for (int at = 0; at < 3; ++at)
            fitButtons[at].setToggleState (reading.fit == fitWords[at], juce::dontSendNotification);

        for (int at = 0; at < 4; ++at)
            curvePicks[at].setToggleState (at == curvePicked, juce::dontSendNotification);

        paintSwatch.setColour (juce::TextButton::buttonColourId, paintColour (reading.paint));
        paintLabel.setText (reading.paint.empty() ? juce::String ("#000000") : juce::String (reading.paint),
                            juce::dontSendNotification);

        //  WHAT THIS SOURCE HAS: the rest is not drawn, rather than drawn and dead.
        const auto graded = reading.graded();
        const auto painted = reading.painted();
        const auto masked = reading.masked();

        const auto keying = keyPage();

        for (auto* part : std::initializer_list<juce::Component*> { &contrast, &contrastLabel, &saturation, &saturationLabel,
                                                                    &gamma, &gammaLabel, &hue, &hueLabel, curves.get(),
                                                                    &curveReset, &curvePicks[0], &curvePicks[1],
                                                                    &curvePicks[2], &curvePicks[3] })
            part->setVisible (graded && ! keying);

        for (auto* part : std::initializer_list<juce::Component*> { &colourPages[0], &colourPages[1] })
            part->setVisible (reading.keyable());

        //  THE KEY'S PAGE: what the key in use reads, and nothing it does not.
        const auto keyOn = reading.key != "none";
        const auto colourKey = reading.key == "colour";

        for (auto* part : std::initializer_list<juce::Component*> { &useAlpha, &keyMode })
            part->setVisible (keying);

        for (auto* part : std::initializer_list<juce::Component*> { &keyLevel, &keyLevelLabel, &keySoftness,
                                                                    &keySoftnessLabel, &keyInvert })
            part->setVisible (keying && keyOn);

        for (auto* part : std::initializer_list<juce::Component*> { &keyColourSwatch, &keyColourLabel })
            part->setVisible (keying && (colourKey || shape));

        for (auto* part : std::initializer_list<juce::Component*> { &keySpill, &keySpillLabel })
            part->setVisible (keying && colourKey);

        //  A TEXT CUE'S PAGE (61): its words where the fit is, its style beside them.
        const auto texted = reading.texted();

        for (auto* part : std::initializer_list<juce::Component*> { &wordsBox, &fontMenu, &bold, &italic, &fontSize,
                                                                    &fontSizeLabel, &lineSpacing, &lineSpacingLabel,
                                                                    &wrap, &wrapLabel, &outlineWidth, &outlineLabel,
                                                                    &outlineBlur, &outlineBlurLabel, &letterSwatch,
                                                                    &outlineSwatch, &alignButtons[0], &alignButtons[1],
                                                                    &alignButtons[2], &alignVMenu })
            part->setVisible (texted);

        for (auto& button : fitButtons)
            button.setVisible (! texted && ! reading.blurred());

        for (auto* part : std::initializer_list<juce::Component*> { &blurAmount, &blurAmountLabel, &wholeCanvas, &aShape })
            part->setVisible (reading.blurred());

        //  THE STYLE OR THE FIELDS, picked beside the font (61).
        refreshFields();
        fieldsPick.setButtonText (fieldsPage ? juce::String ("Style")
                                             : "Fields (" + juce::String (static_cast<int> (reading.fields.size())) + ")");
        fieldsPick.setVisible (texted);

        if (texted && fieldsPage)
            for (auto* part : std::initializer_list<juce::Component*> { &fontSize, &fontSizeLabel, &bold, &italic,
                                                                        &alignVMenu, &alignButtons[0], &alignButtons[1],
                                                                        &alignButtons[2], &lineSpacing, &lineSpacingLabel,
                                                                        &wrap, &wrapLabel, &outlineWidth, &outlineLabel,
                                                                        &outlineBlur, &outlineBlurLabel, &letterSwatch,
                                                                        &outlineSwatch })
                part->setVisible (false);

        for (auto& row : fieldRows)
            for (auto* part : std::initializer_list<juce::Component*> { &row->label, &row->value, &row->copy })
                part->setVisible (texted && fieldsPage);

        noFields.setVisible (texted && fieldsPage && fieldRows.empty());

        paintSwatch.setVisible (painted);
        paintLabel.setVisible (painted);

        for (auto* part : std::initializer_list<juce::Component*> { &feather, &featherLabel, &invert })
            part->setVisible (masked);

        //  UNDER THE LOCK it shows, and changes nothing.
        const auto open = ! reading.locked && ! reading.cueId.empty();

        for (auto* part : std::initializer_list<juce::Component*> { &scale, &offsetX, &offsetY, &rotation, &contrast,
                                                                    &saturation, &gamma, &hue, &feather, &flipH, &flipV,
                                                                    &invert, &paintSwatch, &curveReset,
                                                                    &fitButtons[0], &fitButtons[1], &fitButtons[2],
                                                                    &keyMode, &keyLevel, &keySoftness, &keySpill,
                                                                    &keyInvert, &keyColourSwatch, &fontMenu, &bold,
                                                                    &italic, &fontSize, &lineSpacing, &wrap, &outlineWidth,
                                                                    &outlineBlur, &letterSwatch, &outlineSwatch,
                                                                    &alignButtons[0], &alignButtons[1], &alignButtons[2],
                                                                    &alignVMenu, &blurAmount, &wholeCanvas, &aShape })
            part->setEnabled (open);

        wordsBox.setReadOnly (! open);

        useAlpha.setEnabled (open && ! inputSaysNone);

        /*  ON A FADE (namespace draft §94): only what a fade moves, each with
            its tick and its word - "moves" or "stays" - and the number in the
            accent where it moves, never the colour alone (§4.8). The rest is
            the cue's, and not drawn here. */
        for (auto* part : std::initializer_list<juce::Component*> { &opacity, &opacityLabel, &speed, &speedLabel,
                                                                    &fadeWords })
            part->setVisible (false);

        //  What every source has, shown again after a fade's panel hid it.
        for (auto* part : std::initializer_list<juce::Component*> { &scale, &scaleLabel, &offsetX, &offsetXLabel,
                                                                    &offsetY, &offsetYLabel, &rotation, &rotationLabel,
                                                                    &flipH, &flipV })
            part->setVisible (true);

        for (auto& tick : fadeTicks)
            tick.setVisible (false);

        for (int at = 0; at < 7; ++at)
            fadeSlider (at)->removeColour (juce::Slider::textBoxTextColourId);

        if (reading.onFade())
        {
            for (auto* child : getChildren())
                if (child != view.get())
                    child->setVisible (false);

            const auto movable = reading.fadeRows();

            for (int at = 0; at < 7; ++at)
            {
                const auto row = std::string (fadeRowNames[at]);

                if (std::find (movable.begin(), movable.end(), row) == movable.end())
                    continue;

                const auto moves = reading.fadeMoves.count (row) > 0;
                auto& tick = fadeTicks[at];
                tick.setVisible (true);
                tick.setEnabled (open);
                tick.setButtonText (moves ? "moves" : "stays");
                tick.setToggleState (moves, juce::dontSendNotification);
                tick.setTooltip (moves ? "The fade moves this - clear to leave it alone"
                                       : "The fade leaves this alone - tick, or move it, to move it");

                auto* slider = fadeSlider (at);
                slider->setVisible (true);
                slider->setEnabled (open);
                slider->setColour (juce::Slider::textBoxTextColourId, Look::colour (theme, moves ? "accent" : "ink-off"));
                fadeLabel (at)->setVisible (true);
            }

            fadeWords.setText (reading.blurred()
                                 ? "Where the fade takes this blur. Its outline is the cue's."
                                 : "Where the fade takes the picture: drag the frame, or the numbers. "
                                   "Its look, key and words are the cue's.",
                               juce::dontSendNotification);
            fadeWords.setVisible (true);
        }

        resized();
    }

    void PicturePanelComponent::chooseColour (const char* row, const std::string& current, juce::Component& from)
    {
        if (reading.cueId.empty() || reading.locked)
            return;

        /*  A SWATCH THAT WRITES AS IT IS MOVED: each change a `node.set` on the
            one row - the paint, or a key's screen colour (namespace draft 61) -
            which the document folds into one undo step. */
        class Picker final : public juce::ColourSelector, private juce::Timer
        {
        public:
            Picker (PicturePanelComponent& ownerToUse, const char* rowToWrite, juce::Colour start)
                : juce::ColourSelector (juce::ColourSelector::showColourAtTop | juce::ColourSelector::showSliders
                                        | juce::ColourSelector::showColourspace),
                  owner (&ownerToUse), row (rowToWrite), written (start)
            {
                setCurrentColour (start, juce::dontSendNotification);
                setSize (300, 320);
                startTimerHz (20);
            }

            ~Picker() override { stopTimer(); }

        private:
            juce::Component::SafePointer<PicturePanelComponent> owner;
            const char* row;
            juce::Colour written;

            //  The colour looked at twenty times a second, and written when it moved.
            void timerCallback() override
            {
                const auto now = getCurrentColour();

                if (owner == nullptr || now == written)
                    return;

                written = now;
                owner->write (row, paintText (now));
            }
        };

        juce::CallOutBox::launchAsynchronously (std::make_unique<Picker> (*this, row, paintColour (current)),
                                                from.getScreenBounds(), nullptr);
    }

    void PicturePanelComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;

        for (auto* label : { &scaleLabel, &offsetXLabel, &offsetYLabel, &rotationLabel, &contrastLabel,
                             &saturationLabel, &gammaLabel, &hueLabel, &featherLabel, &paintLabel,
                             &keyLevelLabel, &keySoftnessLabel, &keySpillLabel, &keyColourLabel,
                             &fontSizeLabel, &lineSpacingLabel, &wrapLabel, &outlineLabel, &outlineBlurLabel,
                             &blurAmountLabel, &opacityLabel, &speedLabel, &fadeWords })
        {
            label->setColour (juce::Label::textColourId, Look::colour (theme, "ink-dim"));
            label->setFont (Look::font (theme, 12.0f));
        }

        repaint();
    }

    juce::Rectangle<int> PicturePanelComponent::controlsArea() const
    {
        auto area = getLocalBounds();
        return area.removeFromRight (juce::jmax (360, area.getWidth() * 11 / 20));
    }

    void PicturePanelComponent::resized()
    {
        auto area = getLocalBounds();
        auto controls = area.removeFromRight (juce::jmax (360, area.getWidth() * 11 / 20));
        view->setBounds (area);

        const auto row = 26;

        /*  ON A FADE (§94): the sentence, then a line a value - its tick, its
            word, its number. */
        if (reading.onFade())
        {
            auto column = controls.reduced (8, 6);
            fadeWords.setBounds (column.removeFromTop (row * 2));

            for (int at = 0; at < 7; ++at)
            {
                if (! fadeTicks[at].isVisible())
                    continue;

                auto line = column.removeFromTop (row);
                fadeTicks[at].setBounds (line.removeFromLeft (76));
                fadeLabel (at)->setBounds (line.removeFromLeft (64));
                fadeSlider (at)->setBounds (line);
            }

            return;
        }

        auto placeColumn = controls.removeFromLeft (reading.graded() || reading.texted() || reading.blurred()
                                                        ? controls.getWidth() / 2 : controls.getWidth())
                                   .reduced (8, 6);
        auto colourColumn = controls.reduced (8, 6);

        //  WHERE IT LIES: the fit - a text cue's words instead (61) - the flips, then its numbers.
        {
            if (reading.texted())
            {
                wordsBox.setBounds (placeColumn.removeFromTop (row * 3).reduced (2, 2));
            }
            else if (reading.blurred())
            {
                //  A blur cue has no fit: its frame is the canvas, as a mask's is.
            }
            else
            {
                auto fits = placeColumn.removeFromTop (row);
                const auto width = fits.getWidth() / 3;

                for (auto& button : fitButtons)
                    button.setBounds (fits.removeFromLeft (width).reduced (2, 1));
            }

            auto flips = placeColumn.removeFromTop (row);
            flipH.setBounds (flips.removeFromLeft (flips.getWidth() / 2));
            flipV.setBounds (flips);

            for (auto [slider, label] : { std::pair { &scale, &scaleLabel }, std::pair { &offsetX, &offsetXLabel },
                                          std::pair { &offsetY, &offsetYLabel }, std::pair { &rotation, &rotationLabel } })
            {
                auto line = placeColumn.removeFromTop (row);
                label->setBounds (line.removeFromLeft (64));
                slider->setBounds (line);
            }

            //  A fill's and a mask's colour; a mask's feather and inside-out.
            if (reading.painted())
            {
                auto line = placeColumn.removeFromTop (row);
                paintSwatch.setBounds (line.removeFromLeft (64).reduced (2, 2));
                paintLabel.setBounds (line);
            }

            if (reading.masked() && ! reading.blurred())
            {
                auto line = placeColumn.removeFromTop (row);
                featherLabel.setBounds (line.removeFromLeft (64));
                feather.setBounds (line);
                invert.setBounds (placeColumn.removeFromTop (row));
            }
        }

        //  THE GRADE OR THE KEY, picked above the column (namespace draft 61).
        if (reading.keyable())
        {
            auto picks = colourColumn.removeFromTop (row);
            const auto half = picks.getWidth() / 2;
            colourPages[0].setBounds (picks.removeFromLeft (half).reduced (2, 1));
            colourPages[1].setBounds (picks.reduced (2, 1));
        }

        /*  A BLUR CUE (61, AKN): how far, the whole canvas or an outline, and
            the outline's feather and inside-out. */
        if (reading.blurred())
        {
            auto amount = colourColumn.removeFromTop (row);
            blurAmountLabel.setBounds (amount.removeFromLeft (72));
            blurAmount.setBounds (amount);

            auto choice = colourColumn.removeFromTop (row);
            const auto half = choice.getWidth() / 2;
            wholeCanvas.setBounds (choice.removeFromLeft (half).reduced (2, 1));
            aShape.setBounds (choice.reduced (2, 1));

            auto line = colourColumn.removeFromTop (row);
            featherLabel.setBounds (line.removeFromLeft (72));
            feather.setBounds (line);
            invert.setBounds (colourColumn.removeFromTop (row));
            return;
        }

        //  A TEXT CUE'S STYLE (61): the font, the letters, the alignments, the box, the outline.
        if (reading.texted())
        {
            auto first = colourColumn.removeFromTop (row);
            fieldsPick.setBounds (first.removeFromRight (first.getWidth() / 3).reduced (2, 2));
            fontMenu.setBounds (first.reduced (2, 2));

            /*  ITS FIELDS: a row each - the name, the value, Copy. */
            if (fieldsPage)
            {
                if (fieldRows.empty())
                    noFields.setBounds (colourColumn.removeFromTop (row * 2));

                for (auto& field : fieldRows)
                {
                    auto line = colourColumn.removeFromTop (row);
                    field->label.setBounds (line.removeFromLeft (72));
                    field->copy.setBounds (line.removeFromRight (52).reduced (2, 2));
                    field->value.setBounds (line.reduced (2, 2));
                }

                return;
            }

            const auto sliderLine = [&] (juce::Slider& slider, juce::Label& label)
            {
                auto line = colourColumn.removeFromTop (row);
                label.setBounds (line.removeFromLeft (72));
                slider.setBounds (line);
            };

            //  The letters' colour a swatch before their size, as the outline's is.
            auto letters = colourColumn.removeFromTop (row);
            letterSwatch.setBounds (letters.removeFromLeft (24).reduced (2, 3));
            fontSizeLabel.setBounds (letters.removeFromLeft (48));
            fontSize.setBounds (letters);

            auto styles = colourColumn.removeFromTop (row);
            const auto quarter = styles.getWidth() / 4;
            bold.setBounds (styles.removeFromLeft (quarter));
            italic.setBounds (styles.removeFromLeft (quarter));
            alignVMenu.setBounds (styles.reduced (2, 2));

            auto aligns = colourColumn.removeFromTop (row);
            const auto width = aligns.getWidth() / 3;

            for (auto& button : alignButtons)
                button.setBounds (aligns.removeFromLeft (width).reduced (1, 1));

            sliderLine (lineSpacing, lineSpacingLabel);
            sliderLine (wrap, wrapLabel);

            auto outlined = colourColumn.removeFromTop (row);
            outlineSwatch.setBounds (outlined.removeFromLeft (24).reduced (2, 3));
            outlineLabel.setBounds (outlined.removeFromLeft (48));
            outlineWidth.setBounds (outlined);

            sliderLine (outlineBlur, outlineBlurLabel);
        }

        //  THE KEY: its alpha and its turn, the key, then what that key reads.
        else if (keyPage())
        {
            auto toggles = colourColumn.removeFromTop (row);
            useAlpha.setBounds (toggles.removeFromLeft (toggles.getWidth() / 2));
            keyInvert.setBounds (toggles);
            keyMode.setBounds (colourColumn.removeFromTop (row).reduced (2, 2));

            for (auto [slider, label] : { std::pair { &keyLevel, &keyLevelLabel }, std::pair { &keySoftness, &keySoftnessLabel } })
            {
                auto line = colourColumn.removeFromTop (row);
                label->setBounds (line.removeFromLeft (72));
                slider->setBounds (line);
            }

            if (reading.key == "colour" || reading.key == "matte")
            {
                auto line = colourColumn.removeFromTop (row);
                keyColourSwatch.setBounds (line.removeFromLeft (72).reduced (2, 2));
                keyColourLabel.setBounds (line);
            }

            if (reading.key == "colour")
            {
                auto line = colourColumn.removeFromTop (row);
                keySpillLabel.setBounds (line.removeFromLeft (72));
                keySpill.setBounds (line);
            }
        }

        //  ITS COLOUR: the four numbers, then the curves with their letters.
        else if (reading.graded())
        {
            for (auto [slider, label] : { std::pair { &contrast, &contrastLabel }, std::pair { &saturation, &saturationLabel },
                                          std::pair { &gamma, &gammaLabel }, std::pair { &hue, &hueLabel } })
            {
                auto line = colourColumn.removeFromTop (row);
                label->setBounds (line.removeFromLeft (72));
                slider->setBounds (line);
            }

            auto picks = colourColumn.removeFromTop (row);
            curveReset.setBounds (picks.removeFromRight (80).reduced (2, 1));

            for (auto& button : curvePicks)
                button.setBounds (picks.removeFromLeft (32).reduced (2, 1));

            curves->setBounds (colourColumn);
        }
    }

    void PicturePanelComponent::paint (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "panel-runs"));
        g.setColour (Look::colour (theme, "rule"));
        g.drawVerticalLine (controlsArea().getX(), 0.0f, static_cast<float> (getHeight()));
    }

    void PicturePanelComponent::paintOverChildren (juce::Graphics& g)
    {
        //  THE NUMBER HOLDING THE ARROW KEYS, outlined with its words (2026-10-20; a shape, §4.8).
        for (const auto& [slider, label] : numbers)
            if (slider->isVisible() && slider->hasKeyboardFocus (true))
            {
                g.setColour (Look::colour (theme, "accent"));
                g.drawRoundedRectangle (slider->getBounds().getUnion (label->getBounds()).toFloat().expanded (1.0f),
                                        3.0f, 1.0f);
            }
    }
}
