/*
    Go.dot - show control for sound, light and video
    Copyright (C) 2026 Pierre-Olivier Boulant

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once

#include <wfg/client/model/Theme.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace wfg::client::ui
{
    /*  A SEND'S PLACE ON A BAR (namespace draft §113, BCJ): a lean - a mono
        cue's pan, a stereo cue's balance - drawn from the centre, or a width
        drawn from the left, with its reading in the middle: "L 30", "C",
        "R 50", "100 %". The same bar in the send mixer, where it is the send's
        own, and in a fade's mixer, where it is where the fade takes it. A drag
        or a click sets it where the hand is; a double click puts it back to
        its rest - the centre, the full width - as the D700's press does; the
        wheel nudges it. Dim when it is not the one heard (a send switched out,
        a fade that leaves it alone). */
    class PlaceBar final : public juce::Component,
                           public juce::SettableTooltipClient
    {
    public:
        enum class Kind { lean, width };

        PlaceBar (const model::Theme&, Kind);

        void applyTheme (const model::Theme&);

        /** What the bar shows, from the reading: never calls back. */
        void setValue (double value);
        double value() const noexcept { return shown; }

        /** Drawn in the accent, or dim. */
        void setLit (bool lit);

        /** The hand: each value it asks for, rounded to the hundredth. */
        std::function<void (double)> onChange;

        /** The reading as the bar writes it: "L 30", "C", "R 100"; "75 %". */
        static juce::String textFor (Kind, double value);

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    private:
        double rest() const noexcept { return kind == Kind::lean ? 0.0 : 1.0; }
        double lowest() const noexcept { return kind == Kind::lean ? -1.0 : 0.0; }
        double valueAt (float x) const;
        void ask (double value);

        model::Theme theme;
        Kind kind;
        double shown = 0.0;
        bool lit = true;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlaceBar)
    };
}
