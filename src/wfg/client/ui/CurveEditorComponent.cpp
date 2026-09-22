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

#include <wfg/client/ui/CurveEditorComponent.h>

#include <wfg/client/ui/Look.h>
#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace wfg::client::ui
{
    namespace
    {
        constexpr int axisLeft = 44;
        constexpr int axisFoot = 18;

        /** How near a pointer has to be, as a fraction of each axis. */
        constexpr double nearT = 0.04;
        constexpr double nearLevel = 0.06;

        int scaled (int base, const model::Theme& theme)
        {
            return juce::roundToInt (static_cast<float> (base) * theme.type);
        }

        std::string oneDecimal (double value)
        {
            return osc::formatDouble (std::round (value * 10.0) / 10.0);
        }
    }

    //==============================================================================
    CurveEditorComponent::CurveEditorComponent (const model::Theme& themeToUse, Actions actionsToUse)
        : theme (themeToUse), actions (std::move (actionsToUse))
    {
    }

    void CurveEditorComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;
        repaint();
    }

    void CurveEditorComponent::show (const model::FootReading& readingToUse)
    {
        reading = readingToUse;

        /*  THE HAND KEEPS ITS OWN COPY WHILE IT IS MOVING ONE, and takes the
            document's again the moment it lets go. A drag redrawn from the
            reading would lag the pointer by a pass, and on the first move of a
            worded fade it would do worse - what comes back first is the three
            points the insert made, not where the hand is. */
        if (dragging >= held.size())
            held = reading.curve.points;

        repaint();
    }

    //==============================================================================
    juce::Rectangle<int> CurveEditorComponent::fieldArea() const
    {
        return getLocalBounds()
                 .withTrimmedLeft (scaled (axisLeft, theme))
                 .withTrimmedBottom (scaled (axisFoot, theme))
                 .reduced (scaled (6, theme), scaled (8, theme));
    }

    double CurveEditorComponent::tForX (double x) const
    {
        const auto area = fieldArea();

        if (area.getWidth() <= 0)
            return 0.0;

        return std::clamp ((x - area.getX()) / static_cast<double> (area.getWidth()), 0.0, 1.0);
    }

    double CurveEditorComponent::levelForY (double y) const
    {
        const auto area = fieldArea();

        if (area.getHeight() <= 0)
            return 0.0;

        const auto share = std::clamp ((area.getBottom() - y)
                                         / static_cast<double> (area.getHeight()), 0.0, 1.0);

        return model::quietestDb + share * (model::loudestFadeDb - model::quietestDb);
    }

    double CurveEditorComponent::xForT (double t) const
    {
        const auto area = fieldArea();
        return area.getX() + t * area.getWidth();
    }

    double CurveEditorComponent::yForLevel (double levelDb) const
    {
        const auto area = fieldArea();
        const auto span = model::loudestFadeDb - model::quietestDb;

        if (! (span > 0.0))
            return area.getBottom();

        const auto share = (std::clamp (levelDb, model::quietestDb, model::loudestFadeDb)
                              - model::quietestDb) / span;

        return area.getBottom() - share * area.getHeight();
    }

    //==============================================================================
    void CurveEditorComponent::commit (const std::vector<model::CurvePoint>& points)
    {
        /*  JUDGED BEFORE IT IS SENT, by the same rules the engine's door judges
            by. A refusal that never leaves the window is one nobody has to
            read, and the alternative is a `node.set` the engine turns down for
            a reason that arrives somewhere else entirely. */
        if (const auto why = model::whyNotACurve (points); ! why.empty())
        {
            if (actions.say)
                actions.say (juce::String (why));

            return;
        }

        if (actions.set)
            actions.set ("/godot/cue/" + reading.curve.cueId + "/points",
                         model::writePoints (points));
    }

    void CurveEditorComponent::mouseDown (const juce::MouseEvent& event)
    {
        dragging = static_cast<std::size_t> (-1);

        if (! reading.curve.notice.empty())
            return;

        held = reading.curve.points;

        dragging = model::nearestPoint (held, tForX (event.position.x),
                                        levelForY (event.position.y), nearT,
                                        nearLevel * (model::loudestFadeDb - model::quietestDb));
    }

    void CurveEditorComponent::mouseDrag (const juce::MouseEvent& event)
    {
        if (dragging >= held.size())
            return;

        held[dragging] = model::dragTo (held, dragging, tForX (event.position.x),
                                        levelForY (event.position.y));

        if (actions.say)
            actions.say (juce::String (oneDecimal (held[dragging].t * reading.curve.seconds))
                           + " s at " + juce::String (oneDecimal (held[dragging].levelDb)) + " dB");

        repaint();
    }

    void CurveEditorComponent::mouseUp (const juce::MouseEvent&)
    {
        if (dragging >= held.size())
            return;

        /*  WRITTEN WHEN THE HAND LETS GO. A drawn shape is a decision, not a
            value being ridden: writing it per mouse-move would put a hundred
            undo steps behind one gesture. */
        const auto wanted = held;
        dragging = static_cast<std::size_t> (-1);

        commit (wanted);
    }

    void CurveEditorComponent::mouseDoubleClick (const juce::MouseEvent& event)
    {
        if (! reading.curve.notice.empty())
            return;

        const auto points = reading.curve.points;
        const auto t = tForX (event.position.x);

        //  ON A BREAKPOINT, it goes; anywhere else, one arrives.
        const auto at = model::nearestPoint (points, t, levelForY (event.position.y),
                                             nearT,
                                             nearLevel * (model::loudestFadeDb - model::quietestDb));

        if (at < points.size())
        {
            if (const auto fewer = model::removeAt (points, at))
                commit (*fewer);
            else if (actions.say)
                actions.say ("The two ends stay: a curve has to cover the whole fade.");

            return;
        }

        /*  A WORDED FADE BECOMES A DRAWN ONE, shaped like the word it replaces
            at its ends so nothing jumps: it leaves from wherever the run is,
            which is nought as a starting point here, and arrives at the level
            the fade already declares. */
        if (const auto more = model::insertAt (points, t, 0.0, reading.curve.levelDb))
            commit (*more);
        else if (actions.say)
            actions.say ("There is already a breakpoint at that moment.");
    }

    //==============================================================================
    void CurveEditorComponent::paintGrid (juce::Graphics& g)
    {
        const auto area = fieldArea();

        g.setColour (Look::colour (theme, "panel-in"));
        g.fillRect (area);

        g.setFont (Look::font (theme, 10.0f));

        //  LEVELS UP THE SIDE, in words: 4.8's rule applies to a field too.
        for (const auto decibels : { 12.0, 0.0, -12.0, -24.0, -48.0, -120.0 })
        {
            const auto y = juce::roundToInt (yForLevel (decibels));

            g.setColour (Look::colour (theme, juce::approximatelyEqual (decibels, 0.0)
                                                ? "ink-off" : "rule"));
            g.drawHorizontalLine (y, static_cast<float> (area.getX()),
                                  static_cast<float> (area.getRight()));

            g.setColour (Look::colour (theme, "ink-faint"));
            g.drawText (juce::String (osc::formatDouble (decibels)),
                        juce::Rectangle<int> (0, y - scaled (7, theme),
                                              scaled (axisLeft - 6, theme), scaled (14, theme)),
                        juce::Justification::centredRight, false);
        }

        //  And the fade's own duration across the foot, so the fraction is never alone.
        for (const auto share : { 0.0, 0.25, 0.5, 0.75, 1.0 })
        {
            const auto x = juce::roundToInt (xForT (share));

            g.setColour (Look::colour (theme, "rule"));
            g.drawVerticalLine (x, static_cast<float> (area.getY()),
                                static_cast<float> (area.getBottom()));

            g.setColour (Look::colour (theme, "ink-faint"));
            g.drawText (juce::String (oneDecimal (share * reading.curve.seconds)) + " s",
                        juce::Rectangle<int> (x - scaled (24, theme), area.getBottom() + 2,
                                              scaled (48, theme), scaled (axisFoot - 2, theme)),
                        juce::Justification::centred, false);
        }
    }

    void CurveEditorComponent::paint (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "panel"));

        if (! reading.curve.notice.empty())
        {
            g.setColour (Look::colour (theme, "ink-dim"));
            g.setFont (Look::font (theme, 13.0f));
            g.drawFittedText (juce::String (reading.curve.notice),
                              getLocalBounds().reduced (scaled (12, theme), scaled (6, theme)),
                              juce::Justification::centred, 3);
            return;
        }

        paintGrid (g);

        const auto& points = dragging < held.size() ? held : reading.curve.points;

        if (points.empty())
        {
            /*  A FADE THAT HAS NOT BEEN DRAWN ON plays its `curve` word, and
                saying which word is more use than an empty field. */
            g.setColour (Look::colour (theme, "ink-faint"));
            g.setFont (Look::font (theme, 12.0f));
            g.drawFittedText ("This fade uses its \"" + juce::String (reading.curve.curve)
                                + "\" curve.\nDouble-click to draw one instead.",
                              fieldArea().reduced (scaled (10, theme)),
                              juce::Justification::centred, 2);
            return;
        }

        juce::Path line;

        for (std::size_t at = 0; at < points.size(); ++at)
        {
            const auto x = static_cast<float> (xForT (points[at].t));
            const auto y = static_cast<float> (yForLevel (points[at].levelDb));

            if (at == 0)
                line.startNewSubPath (x, y);
            else
                line.lineTo (x, y);
        }

        g.setColour (Look::colour (theme, "accent"));
        g.strokePath (line, juce::PathStrokeType (2.0f));

        for (std::size_t at = 0; at < points.size(); ++at)
        {
            const auto x = static_cast<float> (xForT (points[at].t));
            const auto y = static_cast<float> (yForLevel (points[at].levelDb));
            const auto size = static_cast<float> (scaled (at == dragging ? 9 : 7, theme));

            g.setColour (Look::colour (theme, at == dragging ? "ink" : "accent"));
            g.fillEllipse (x - size * 0.5f, y - size * 0.5f, size, size);
        }
    }
}
