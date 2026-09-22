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
    The shape a fade takes, drawn as breakpoints somebody can drag.

    THE FIRST VIEW THAT COULD NEVER LIVE ON THE PAGE. A fade's level, duration
    and curve word are three rows in the inspector and always were; the SHAPE is
    not a row, and until there is somewhere to draw it a drawn fade is a list of
    numbers nobody can read. That is what this is for.

    TIME ACROSS, LEVEL UP, and both axes say what they are in words rather than
    only by position - §4.8's rule applies to a field as much as to a colour.
    Time is a fraction of the fade's own duration, so the drawing means the same
    thing when somebody changes how long the fade takes; the seconds are written
    on the axis so the fraction is never the only thing said.

    WHAT A GESTURE DOES: drag a breakpoint to move it, double-click the field to
    put one where the line already is - so adding one changes the shape not at
    all until it is moved, which is what makes it safe to do while listening -
    and double-click a breakpoint to take it away. The two ends move only in
    level: a curve that started after nought would leave a stretch of the fade
    it says nothing about, which the engine's door refuses.

    IT REFUSES WHAT THE DOOR WOULD REFUSE, before sending rather than after.
    `model/Curve` holds the same rules `doc::readFadePoints` judges by, and the
    test that keeps the two honest asserts every string this sends against the
    real one.
*/

#include <wfg/client/model/Curve.h>
#include <wfg/client/model/Foot.h>
#include <wfg/client/model/Theme.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace wfg::client::ui
{
    class CurveEditorComponent final : public juce::Component
    {
    public:
        struct Actions
        {
            /** One `node.set` on the fade's `points`. */
            std::function<void (const std::string& address, const std::string& text)> set;

            /** A sentence for the panel's head. */
            std::function<void (const juce::String&)> say;
        };

        CurveEditorComponent (const model::Theme&, Actions);

        void applyTheme (const model::Theme&);
        void show (const model::FootReading&);

        void paint (juce::Graphics&) override;

        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

    private:
        /** The field the curve is drawn in, inside the axis labels. */
        juce::Rectangle<int> fieldArea() const;

        double tForX (double x) const;
        double levelForY (double y) const;
        double xForT (double t) const;
        double yForLevel (double levelDb) const;

        /** What the window would send, judged first. */
        void commit (const std::vector<model::CurvePoint>&);

        void paintGrid (juce::Graphics&);

        model::Theme theme;
        Actions actions;

        model::FootReading reading;

        /*  The breakpoints as the HAND has them, which is what is drawn while
            one is being moved: the document catches up a pass later, and a
            curve that waited for it would lag the pointer. */
        std::vector<model::CurvePoint> held;
        std::size_t dragging = static_cast<std::size_t> (-1);

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CurveEditorComponent)
    };
}
