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

#include <wfg/client/ui/TimelineComponent.h>

#include <wfg/client/ui/Look.h>
#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace wfg::client::ui
{
    namespace
    {
        constexpr int rulerHeight = 20;
        constexpr int rowGap = 3;

        /** How near, in pixels, a shift-drag has to be before it lines up. */
        constexpr double snapPixels = 9.0;

        int scaled (int base, const model::Theme& theme)
        {
            return juce::roundToInt (static_cast<float> (base) * theme.type);
        }

        /** Seconds as a ruler says them: "0", "2.5", "1:04". */
        std::string tickText (double seconds)
        {
            if (seconds < 60.0)
                return osc::formatDouble (std::round (seconds * 10.0) / 10.0);

            const auto minutes = static_cast<int> (seconds / 60.0);
            const auto rest = std::round ((seconds - minutes * 60.0) * 10.0) / 10.0;

            auto out = std::to_string (minutes) + ":";

            if (rest < 10.0)
                out += "0";

            return out + osc::formatDouble (rest);
        }
    }

    //==============================================================================
    TimelineComponent::TimelineComponent (const model::Theme& themeToUse, Actions actionsToUse)
        : theme (themeToUse), actions (std::move (actionsToUse))
    {
        setWantsKeyboardFocus (false);
    }

    void TimelineComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;
        repaint();
    }

    void TimelineComponent::show (const model::FootReading& readingToUse)
    {
        const auto was = reading.timeline.groupId;
        const auto span = reading.timeline.span;

        reading = readingToUse;

        /*  THE WHOLE GROUP WHEN IT IS A NEW ONE, and the window left alone
            otherwise: a pass arriving twenty-five times a second must not undo
            a zoom somebody set, and a member dragged past the old edge must
            not jump the view out from under the hand either. What DOES move
            the window is the group changing, or the show growing past it. */
        if (was != reading.timeline.groupId)
        {
            view.reset (reading.timeline.span);
        }
        else if (! juce::approximatelyEqual (span, reading.timeline.span))
        {
            /*  The group has grown or shrunk. Somebody looking at the whole of
                it goes on looking at the whole of it; somebody zoomed in keeps
                their window, clamped into what is now there. */
            const auto wasWhole = view.isWholeThing();

            view.length = reading.timeline.span;

            if (wasWhole)
                view.reset (reading.timeline.span);
            else
                view.clamp();
        }

        repaint();
    }

    //==============================================================================
    juce::Rectangle<int> TimelineComponent::rulerArea() const
    {
        return getLocalBounds().withHeight (scaled (rulerHeight, theme));
    }

    juce::Rectangle<int> TimelineComponent::barsArea() const
    {
        return getLocalBounds().withTrimmedTop (scaled (rulerHeight, theme));
    }

    juce::Rectangle<int> TimelineComponent::rowFor (std::size_t at) const
    {
        const auto area = barsArea();
        const auto count = static_cast<int> (reading.timeline.bars.size());

        if (count <= 0)
            return {};

        /*  EVERY MEMBER GETS A ROW and the rows share what there is, so a
            group of three is read at a glance and a group of twenty still
            fits without a scrollbar to get lost in. */
        const auto height = std::max (scaled (10, theme), area.getHeight() / count);

        return { area.getX(), area.getY() + static_cast<int> (at) * height,
                 area.getWidth(), height };
    }

    std::size_t TimelineComponent::barAt (juce::Point<int> where) const
    {
        for (std::size_t at = 0; at < reading.timeline.bars.size(); ++at)
            if (rowFor (at).contains (where))
                return at;

        return static_cast<std::size_t> (-1);
    }

    //==============================================================================
    void TimelineComponent::mouseDown (const juce::MouseEvent& event)
    {
        dragging = static_cast<std::size_t> (-1);

        if (! reading.timeline.draggable)
        {
            if (actions.say && ! reading.timeline.bars.empty())
                actions.say ("A sequence runs its members one after another, so where they sit "
                             "is worked out rather than chosen. Make the group a timeline to "
                             "arrange them.");

            return;
        }

        const auto at = barAt (event.getPosition());

        if (at >= reading.timeline.bars.size())
            return;

        dragging = at;
        heldStart = reading.timeline.bars[at].at;
        shownStart = heldStart;
        grabbedAt = view.secondsForX (event.position.x, getWidth());
    }

    void TimelineComponent::mouseDrag (const juce::MouseEvent& event)
    {
        if (dragging >= reading.timeline.bars.size())
            return;

        const auto& bar = reading.timeline.bars[dragging];
        const auto moved = view.secondsForX (event.position.x, getWidth()) - grabbedAt;

        auto wanted = model::preWaitFor (heldStart + moved);
        auto said = std::string ("at ") + tickText (wanted);

        /*  SHIFT LINES IT UP, and the sentence says what with. A plain drag is
            free to the pixel, which is what somebody wants while they are
            still deciding roughly where a thing goes. */
        if (event.mods.isShiftDown())
        {
            const auto tolerance = view.span() > 0.0
                                     ? snapPixels * view.span() / std::max (1, getWidth())
                                     : 0.0;

            const auto targets = model::snapTargets (reading.timeline.bars, bar.id);

            if (const auto found = model::snapTo (wanted, bar.length, bar.lengthKnown,
                                                  targets, tolerance))
            {
                wanted = found->at;
                said = found->said;
            }
            else
            {
                said += " - nothing near to line up with";
            }
        }

        shownStart = wanted;

        if (actions.say)
            actions.say (juce::String (bar.name.empty() ? bar.id : bar.name) + ": " + said);

        repaint();
    }

    void TimelineComponent::mouseUp (const juce::MouseEvent&)
    {
        if (dragging >= reading.timeline.bars.size())
            return;

        const auto& bar = reading.timeline.bars[dragging];

        /*  WRITTEN ONCE, WHEN THE HAND LETS GO. A pre-wait is a decision and
            not a performance: writing it per mouse-move would put a hundred
            undo steps behind one gesture, where the engine's coalescing window
            is about a value being ridden rather than a bar being placed. */
        if (actions.set && ! juce::approximatelyEqual (shownStart, heldStart))
            actions.set ("/godot/cue/" + bar.id + "/preWait",
                         osc::formatDouble (std::round (shownStart * 1000.0) / 1000.0));

        dragging = static_cast<std::size_t> (-1);
        repaint();
    }

    void TimelineComponent::mouseDoubleClick (const juce::MouseEvent& event)
    {
        /*  DOWN INTO A NESTED GROUP, and up to this one's own parent from the
            ruler. A group's bar is the only thing on this panel that stands
            for a container, so it is the only thing worth opening. */
        const auto at = barAt (event.getPosition());

        if (at < reading.timeline.bars.size() && reading.timeline.bars[at].isGroup)
        {
            if (actions.openOn)
                actions.openOn (reading.timeline.bars[at].id);

            return;
        }

        if (rulerArea().contains (event.getPosition()) && ! reading.timeline.parent.empty())
            if (actions.openOn)
                actions.openOn (reading.timeline.parent);
    }

    void TimelineComponent::mouseWheelMove (const juce::MouseEvent& event,
                                            const juce::MouseWheelDetails& wheel)
    {
        /*  The waveform editor's own rule, so the two panels feel the same:
            across is a pan, up and down is a zoom about the pointer. */
        if (std::abs (wheel.deltaX) > std::abs (wheel.deltaY))
        {
            view.panBy (juce::roundToInt (-wheel.deltaX * getWidth() * 0.25), getWidth());
            repaint();
            return;
        }

        if (! juce::approximatelyEqual (wheel.deltaY, 0.0f))
        {
            view.zoomAbout (event.position.x, getWidth(), wheel.deltaY > 0.0f ? 0.85 : 1.0 / 0.85);
            repaint();
        }
    }

    //==============================================================================
    void TimelineComponent::paintRuler (juce::Graphics& g)
    {
        const auto area = rulerArea();

        g.setColour (Look::colour (theme, "panel-in"));
        g.fillRect (area);

        if (! (view.span() > 0.0))
            return;

        /*  A ROUND NUMBER OF SECONDS BETWEEN MARKS, chosen so there are
            roughly six of them however far in somebody has zoomed. */
        const auto wanted = view.span() / 6.0;
        const auto step = [wanted]
        {
            for (const auto candidate : { 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0,
                                          60.0, 120.0, 300.0, 600.0 })
                if (candidate >= wanted)
                    return candidate;

            return 900.0;
        }();

        g.setFont (Look::font (theme, 10.0f));

        for (auto second = std::floor (view.from / step) * step; second <= view.to; second += step)
        {
            if (second < 0.0)
                continue;

            const auto x = juce::roundToInt (view.xForSeconds (second, getWidth()));

            g.setColour (Look::colour (theme, "rule"));
            g.drawVerticalLine (x, static_cast<float> (area.getBottom() - scaled (5, theme)),
                                static_cast<float> (area.getBottom()));

            g.setColour (Look::colour (theme, "ink-faint"));
            g.drawText (juce::String (tickText (second)),
                        juce::Rectangle<int> (x + 2, area.getY(), scaled (48, theme),
                                              area.getHeight() - scaled (4, theme)),
                        juce::Justification::centredLeft, false);
        }
    }

    void TimelineComponent::paintBar (juce::Graphics& g, const model::Bar& bar,
                                      juce::Rectangle<int> row, bool held)
    {
        const auto at = held ? shownStart : bar.at;

        const auto left = juce::roundToInt (view.xForSeconds (at, getWidth()));
        const auto body = row.reduced (0, scaled (rowGap, theme));

        /*  A COLOUR THE CUE DECLARES, and the panel's accent where it declares
            none. §4.8: never the sole carrier, so the name is written on the
            bar and the times are in the head. */
        auto tone = Look::colour (theme, held ? "accent" : "rule");

        /*  "#rrggbb" AND NOTHING ELSE, which is the one spelling `Theme`'s own
            parser accepts and the one the page writes. Asked rather than handed
            to `Colour::fromString`, which answers BLACK for anything it cannot
            read - so a colour with a typo in it would paint the bar the same as
            the panel's ground and look like a bar that had vanished. */
        if (bar.colour.size() == 7 && bar.colour.front() == '#'
              && juce::String (bar.colour).substring (1).containsOnly ("0123456789abcdefABCDEF"))
            tone = juce::Colour::fromString ("ff" + juce::String (bar.colour).substring (1));

        if (held)
            tone = tone.brighter (0.3f);

        if (bar.lengthKnown && bar.length > 0.0)
        {
            const auto right = juce::roundToInt (view.xForSeconds (at + bar.length, getWidth()));

            g.setColour (tone.withAlpha (held ? 0.95f : 0.75f));
            g.fillRect (juce::Rectangle<int> (left, body.getY(),
                                              std::max (2, right - left), body.getHeight()));
        }
        else
        {
            /*  A START WITH NO END, drawn as a flag rather than as a bar of no
                width: "I do not know how long this is" is a different fact
                from "this takes no time", and a designer reads the difference.
                A group is always this, because a group's length is the
                engine's own arithmetic and reaches no client. */
            g.setColour (tone.withAlpha (held ? 0.95f : 0.75f));
            g.fillRect (juce::Rectangle<int> (left, body.getY(), scaled (3, theme), body.getHeight()));

            g.setColour (Look::colour (theme, "ink-off"));
            g.drawHorizontalLine (body.getCentreY(), static_cast<float> (left + scaled (3, theme)),
                                  static_cast<float> (left + scaled (26, theme)));
        }

        g.setColour (Look::colour (theme, "ink"));
        g.setFont (Look::font (theme, 11.0f));
        g.drawText (juce::String (bar.name.empty() ? bar.id : bar.name),
                    juce::Rectangle<int> (left + scaled (5, theme), body.getY(),
                                          std::max (scaled (60, theme), getWidth() - left),
                                          body.getHeight()),
                    juce::Justification::centredLeft, true);
    }

    void TimelineComponent::paint (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "panel"));

        const auto& timeline = reading.timeline;

        if (! timeline.notice.empty())
        {
            g.setColour (Look::colour (theme, "ink-dim"));
            g.setFont (Look::font (theme, 13.0f));
            g.drawFittedText (juce::String (timeline.notice),
                              getLocalBounds().reduced (scaled (12, theme), scaled (6, theme)),
                              juce::Justification::centred, 3);
            return;
        }

        paintRuler (g);

        //  The group's own entry, which every offset is measured from.
        if (view.from <= 0.0 && view.to >= 0.0)
        {
            g.setColour (Look::colour (theme, "ink-off"));
            g.drawVerticalLine (juce::roundToInt (view.xForSeconds (0.0, getWidth())),
                                static_cast<float> (barsArea().getY()),
                                static_cast<float> (getBottom()));
        }

        for (std::size_t at = 0; at < timeline.bars.size(); ++at)
            paintBar (g, timeline.bars[at], rowFor (at), at == dragging);

        /*  AND WHY NOTHING MOVES, where nothing can. Said on the panel rather
            than only when somebody tries, because a bar that does not budge
            reads as a fault long before anybody thinks to look for a reason. */
        if (! timeline.draggable)
        {
            g.setColour (Look::colour (theme, "ink-faint"));
            g.setFont (Look::font (theme, 11.0f));
            g.drawText ("a sequence: each member follows the one before it",
                        rulerArea().reduced (scaled (6, theme), 0),
                        juce::Justification::centredRight, true);
        }
        else if (! timeline.parent.empty())
        {
            //  Said rather than left to be discovered, which is 4.8's rule for form too.
            g.setColour (Look::colour (theme, "ink-faint"));
            g.setFont (Look::font (theme, 11.0f));
            g.drawText ("double-click a group to go into it, or the ruler to go up",
                        rulerArea().reduced (scaled (6, theme), 0),
                        juce::Justification::centredRight, true);
        }
    }

    void TimelineComponent::resized()
    {
        repaint();
    }
}
