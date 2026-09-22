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
    A group's members as bars on a time axis, arranged by dragging them.

    ONE ROW PER MEMBER, IN DOCUMENT ORDER, and the order is left alone by every
    gesture here: this panel answers WHEN, and the cue list answers in what
    order they are written. A timeline group's order on screen is not its order
    in time (§3.6), which is exactly the confusion this is for - the list says
    one thing, this says the other, and neither is wrong.

    A DRAG WRITES ONE NUMBER. In a timeline group a member's `preWait` IS its
    offset from the group's entry, so moving a bar is one `node.set` and nothing
    else moves. That is the whole reason this panel can exist without a model of
    the schedule: there is no cascade to recompute and nothing to keep in step.

    SHIFT SNAPS (author, 2026-09-22: *"you can snap starts together, start and
    end, end and end with drag+shift modifier"*). A plain drag is free to the
    pixel. Shift offers the group's entry and both edges of every other bar, and
    the head says in words what it lined up with - a snap nobody can read is a
    snap nobody trusts.

    WHAT IT DOES NOT DO. It does not move a bar in a SEQUENCE group, where a
    member's position is arithmetic over everything above it rather than a
    decision; the bars are drawn and the head says why they are still. And it
    draws no playhead yet: a group run's position is a thing the running pane
    knows and this does not ask for.
*/

#include <wfg/client/model/Foot.h>
#include <wfg/client/model/Theme.h>
#include <wfg/client/model/Timeline.h>
#include <wfg/client/model/View.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <string>
#include <vector>

namespace wfg::client::ui
{
    class TimelineComponent final : public juce::Component
    {
    public:
        struct Actions
        {
            /** One `node.set`: the pre-wait a dragged bar settled on. */
            std::function<void (const std::string& address, const std::string& text)> set;

            /*  Point the panel at another group: down into a nested one on a
                double click, or up to the one this is inside of. A timeline is
                about a CONTAINER, so moving between containers is a gesture it
                has to have - otherwise arranging a nested scene means going
                back to the cue list to find it. */
            std::function<void (const std::string& groupId)> openOn;

            /** A sentence for the panel's head: what a drag would do, or did. */
            std::function<void (const juce::String&)> say;
        };

        TimelineComponent (const model::Theme&, Actions);

        void applyTheme (const model::Theme&);

        /** The reading for this pass. */
        void show (const model::FootReading&);

        void paint (juce::Graphics&) override;
        void resized() override;

        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

    private:
        /** Where the bars are drawn, which is everything under the ruler. */
        juce::Rectangle<int> barsArea() const;
        juce::Rectangle<int> rulerArea() const;

        /** The row one bar occupies, by its place in the reading. */
        juce::Rectangle<int> rowFor (std::size_t at) const;

        /** Which bar is under a point, or npos. */
        std::size_t barAt (juce::Point<int>) const;

        void paintRuler (juce::Graphics&);
        void paintBar (juce::Graphics&, const model::Bar&, juce::Rectangle<int> row, bool held);

        model::Theme theme;
        Actions actions;

        model::FootReading reading;
        model::View view;

        /*  The bar being dragged, where it started, and where the hand has
            asked for it to go - drawn from the hand so the bar does not wait
            for the document to agree, exactly as a send fader does. */
        std::size_t dragging = static_cast<std::size_t> (-1);
        double grabbedAt = 0.0;
        double heldStart = 0.0;
        double shownStart = 0.0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimelineComponent)
    };
}
