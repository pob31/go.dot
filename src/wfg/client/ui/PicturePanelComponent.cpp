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
#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <cmath>

namespace wfg::client::ui
{
    namespace
    {
        constexpr const char* fitWords[3] { "fit", "fill", "stretch" };
        constexpr const char* curveRows[4] { "curveLuma", "curveRed", "curveGreen", "curveBlue" };
        constexpr const char* curveWords[4] { "L", "R", "G", "B" };

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

            if (picture.isValid() && ! r.masked())
                g.drawImage (picture, canvas, juce::RectanglePlacement::centred);

            g.setColour (Look::colour (owner.theme, "rule"));
            g.drawRect (canvas, 1.0f);

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
            grabbed = Grab::none;
            from = toCanvas (event.position);

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

            const auto& r = owner.reading;
            const auto to = toCanvas (event.position);
            const auto base = "/godot/cue/" + r.cueId + "/";

            switch (grabbed)
            {
                case Grab::move:
                {
                    const auto [x, y] = model::offsetsMoved (r, r.offsetX, r.offsetY, from, to);
                    hand.offsetX = x;
                    hand.offsetY = y;

                    if (owner.actions.setMany)
                        owner.actions.setMany ({ { base + "offsetX", numberText (x) },
                                                 { base + "offsetY", numberText (y) } });
                    break;
                }

                case Grab::corner:
                    hand.scale = model::scaleDragged (r, r.scale, from, to);
                    owner.write ("scale", numberText (hand.scale));
                    break;

                case Grab::turn:
                    hand.rotation = model::rotationDragged (r, r.rotation, from, to, event.mods.isShiftDown());
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

            if (r.cueId.empty() || r.locked || ! r.masked())
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

            const auto coarse = key.getModifiers().isShiftDown();
            const auto code = key.getKeyCode();

            if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
            {
                owner.write ("offsetX", numberText (model::nudged (r.offsetX, code == juce::KeyPress::leftKey ? -1 : 1, coarse)));
                return true;
            }

            if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey)
            {
                owner.write ("offsetY", numberText (model::nudged (r.offsetY, code == juce::KeyPress::upKey ? 1 : -1, coarse)));
                return true;
            }

            return false;
        }

        void readingChanged()
        {
            if (! dragging)
                hand = owner.reading;

            repaint();
        }

    private:
        enum class Grab { none, move, corner, turn, maskPoint };

        PicturePanelComponent& owner;
        model::PictureReading hand;
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
                g.drawFittedText ("No outline yet: double-click to start one", canvasRect().toNearestInt(),
                                  juce::Justification::centred, 2);
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

        build (scale, scaleLabel, "size", "scale", 0.0, 1000.0, 100.0, false, " %", 1);
        build (offsetX, offsetXLabel, "across", "offsetX", -1000.0, 1000.0, 0.0, true, " %", 1);
        build (offsetY, offsetYLabel, "up", "offsetY", -1000.0, 1000.0, 0.0, true, " %", 1);
        build (rotation, rotationLabel, "turn", "rotation", -3600.0, 3600.0, 0.0, true, " deg", 1);
        build (contrast, contrastLabel, "contrast", "contrast", 0.0, 400.0, 100.0, false, " %", 0);
        build (saturation, saturationLabel, "saturation", "saturation", 0.0, 400.0, 100.0, false, " %", 0);
        build (gamma, gammaLabel, "gamma", "gamma", 0.1, 10.0, 1.0, false, "", 2);
        build (hue, hueLabel, "hue", "hue", -180.0, 180.0, 0.0, true, " deg", 0);
        build (feather, featherLabel, "feather", "feather", 0.0, 400.0, 20.0, false, " px", 0);

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
        paintSwatch.onClick = [this] { choosePaint(); };
        addAndMakeVisible (paintSwatch);
        addAndMakeVisible (paintLabel);

        applyTheme (theme);
    }

    PicturePanelComponent::~PicturePanelComponent() = default;

    void PicturePanelComponent::build (juce::Slider& slider, juce::Label& label, const char* words, const char* row,
                                       double low, double high, double mid, bool symmetric, const char* suffix,
                                       int decimals)
    {
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 80, 22);
        slider.setRange (low, high, 0.0);
        slider.setNumDecimalPlacesToDisplay (decimals);
        slider.setTextValueSuffix (suffix);
        slider.setWantsKeyboardFocus (false);

        //  FINE WHERE IT MATTERS: about the middle for an offset and a turn, low down for a size.
        if (symmetric)
            slider.setSkewFactor (0.4, true);
        else
            slider.setSkewFactorFromMidPoint (mid);

        slider.setDoubleClickReturnValue (true, mid);
        //  A hand's change only: the document's own is set without a notification.
        slider.onValueChange = [this, &slider, row] { write (row, numberText (slider.getValue())); };

        label.setText (words, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (slider);
        addAndMakeVisible (label);
    }

    void PicturePanelComponent::write (const char* row, const std::string& text)
    {
        if (reading.cueId.empty() || reading.locked || actions.set == nullptr)
            return;

        actions.set ("/godot/cue/" + reading.cueId + "/" + row, text);
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
        const auto setValue = [] (juce::Slider& slider, double value)
        {
            if (! slider.isMouseButtonDown() && ! juce::exactlyEqual (slider.getValue(), value))
                slider.setValue (value, juce::dontSendNotification);
        };

        setValue (scale, reading.scale);
        setValue (offsetX, reading.offsetX);
        setValue (offsetY, reading.offsetY);
        setValue (rotation, reading.rotation);
        setValue (contrast, reading.contrast);
        setValue (saturation, reading.saturation);
        setValue (gamma, reading.gamma);
        setValue (hue, reading.hue);
        setValue (feather, reading.feather);

        flipH.setToggleState (reading.flipH, juce::dontSendNotification);
        flipV.setToggleState (reading.flipV, juce::dontSendNotification);
        invert.setToggleState (reading.invert, juce::dontSendNotification);

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

        for (auto* part : std::initializer_list<juce::Component*> { &contrast, &contrastLabel, &saturation, &saturationLabel,
                                                                    &gamma, &gammaLabel, &hue, &hueLabel, curves.get(),
                                                                    &curveReset, &curvePicks[0], &curvePicks[1],
                                                                    &curvePicks[2], &curvePicks[3] })
            part->setVisible (graded);

        paintSwatch.setVisible (painted);
        paintLabel.setVisible (painted);

        for (auto* part : std::initializer_list<juce::Component*> { &feather, &featherLabel, &invert })
            part->setVisible (masked);

        //  UNDER THE LOCK it shows, and changes nothing.
        const auto open = ! reading.locked && ! reading.cueId.empty();

        for (auto* part : std::initializer_list<juce::Component*> { &scale, &offsetX, &offsetY, &rotation, &contrast,
                                                                    &saturation, &gamma, &hue, &feather, &flipH, &flipV,
                                                                    &invert, &paintSwatch, &curveReset,
                                                                    &fitButtons[0], &fitButtons[1], &fitButtons[2] })
            part->setEnabled (open);

        resized();
    }

    void PicturePanelComponent::choosePaint()
    {
        if (reading.cueId.empty() || reading.locked)
            return;

        /*  A SWATCH THAT WRITES AS IT IS MOVED: each change a `node.set` on the
            one row, which the document folds into one undo step. */
        class Picker final : public juce::ColourSelector, private juce::Timer
        {
        public:
            Picker (PicturePanelComponent& ownerToUse, juce::Colour start)
                : juce::ColourSelector (juce::ColourSelector::showColourAtTop | juce::ColourSelector::showSliders
                                        | juce::ColourSelector::showColourspace),
                  owner (&ownerToUse), written (start)
            {
                setCurrentColour (start, juce::dontSendNotification);
                setSize (300, 320);
                startTimerHz (20);
            }

            ~Picker() override { stopTimer(); }

        private:
            juce::Component::SafePointer<PicturePanelComponent> owner;
            juce::Colour written;

            //  The colour looked at twenty times a second, and written when it moved.
            void timerCallback() override
            {
                const auto now = getCurrentColour();

                if (owner == nullptr || now == written)
                    return;

                written = now;
                owner->write ("paint", paintText (now));
            }
        };

        juce::CallOutBox::launchAsynchronously (std::make_unique<Picker> (*this, paintColour (reading.paint)),
                                                paintSwatch.getScreenBounds(), nullptr);
    }

    void PicturePanelComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;

        for (auto* label : { &scaleLabel, &offsetXLabel, &offsetYLabel, &rotationLabel, &contrastLabel,
                             &saturationLabel, &gammaLabel, &hueLabel, &featherLabel, &paintLabel })
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
        auto placeColumn = controls.removeFromLeft (reading.graded() ? controls.getWidth() / 2 : controls.getWidth())
                                   .reduced (8, 6);
        auto colourColumn = controls.reduced (8, 6);

        //  WHERE IT LIES: the fit, the flips, then its numbers.
        {
            auto fits = placeColumn.removeFromTop (row);
            const auto width = fits.getWidth() / 3;

            for (auto& button : fitButtons)
                button.setBounds (fits.removeFromLeft (width).reduced (2, 1));

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

            if (reading.masked())
            {
                auto line = placeColumn.removeFromTop (row);
                featherLabel.setBounds (line.removeFromLeft (64));
                feather.setBounds (line);
                invert.setBounds (placeColumn.removeFromTop (row));
            }
        }

        //  ITS COLOUR: the four numbers, then the curves with their letters.
        if (reading.graded())
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
}
