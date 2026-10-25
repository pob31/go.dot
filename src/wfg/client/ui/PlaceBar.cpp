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

#include <wfg/client/ui/PlaceBar.h>

#include <wfg/client/ui/Look.h>

#include <algorithm>
#include <cmath>

namespace wfg::client::ui
{
    PlaceBar::PlaceBar (const model::Theme& themeToUse, Kind kindToUse)
        : theme (themeToUse), kind (kindToUse), shown (kindToUse == Kind::lean ? 0.0 : 1.0)
    {
        setWantsKeyboardFocus (false);
        setTooltip (kind == Kind::lean
                      ? "Where the cue sits in the mix: a mono cue's pan, a stereo cue's balance. Drag, or double-click for the centre."
                      : "How wide a stereo cue arrives: 100 % as the file is, 0 % mono. Drag, or double-click for 100 %.");
    }

    void PlaceBar::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;
        repaint();
    }

    void PlaceBar::setValue (double value)
    {
        const auto next = std::clamp (value, lowest(), 1.0);

        if (std::abs (next - shown) < 1.0e-9)
            return;

        shown = next;
        repaint();
    }

    void PlaceBar::setLit (bool litToUse)
    {
        if (lit == litToUse)
            return;

        lit = litToUse;
        repaint();
    }

    juce::String PlaceBar::textFor (Kind kind, double value)
    {
        if (kind == Kind::width)
            return juce::String (juce::roundToInt (std::clamp (value, 0.0, 1.0) * 100.0)) + " %";

        const auto percent = juce::roundToInt (std::abs (value) * 100.0);

        if (percent == 0)
            return "C";

        return (value < 0.0 ? "L " : "R ") + juce::String (percent);
    }

    double PlaceBar::valueAt (float x) const
    {
        const auto width = static_cast<float> (std::max (1, getWidth() - 2));
        const auto fraction = std::clamp ((x - 1.0f) / width, 0.0f, 1.0f);
        const auto value = lowest() + static_cast<double> (fraction) * (1.0 - lowest());
        return std::round (value * 100.0) / 100.0;
    }

    void PlaceBar::ask (double value)
    {
        const auto next = std::clamp (std::round (value * 100.0) / 100.0, lowest(), 1.0);
        shown = next;
        repaint();

        if (onChange)
            onChange (next);
    }

    void PlaceBar::mouseDown (const juce::MouseEvent& event)
    {
        ask (valueAt (event.position.x));
    }

    void PlaceBar::mouseDrag (const juce::MouseEvent& event)
    {
        ask (valueAt (event.position.x));
    }

    void PlaceBar::mouseDoubleClick (const juce::MouseEvent&)
    {
        ask (rest());
    }

    void PlaceBar::mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
    {
        const auto clicks = juce::roundToInt (wheel.deltaY * 10.0f);

        if (clicks == 0)
            return;

        ask (shown + clicks * (event.mods.isShiftDown() ? 0.002 : 0.02));
    }

    void PlaceBar::paint (juce::Graphics& g)
    {
        const auto area = getLocalBounds();
        const auto track = area.reduced (1);

        g.setColour (Look::colour (theme, "panel-in"));
        g.fillRect (track);

        const auto fraction = (shown - lowest()) / (1.0 - lowest());
        const auto at = track.getX() + juce::roundToInt (fraction * track.getWidth());
        const auto middle = track.getCentreX();

        //  THE FILL: from the centre for a lean, from the left for a width.
        g.setColour (Look::colour (theme, lit ? "accent" : "ink-off").withAlpha (0.55f));

        if (kind == Kind::lean)
            g.fillRect (juce::Rectangle<int> (std::min (at, middle), track.getY(), std::max (1, std::abs (at - middle)), track.getHeight()));
        else
            g.fillRect (juce::Rectangle<int> (track.getX(), track.getY(), std::max (1, at - track.getX()), track.getHeight()));

        //  The centre, and the hand's mark.
        g.setColour (Look::colour (theme, "rule"));

        if (kind == Kind::lean)
            g.fillRect (juce::Rectangle<int> (middle, track.getY(), 1, track.getHeight()));

        g.setColour (Look::colour (theme, lit ? "accent" : "ink-off"));
        g.fillRect (juce::Rectangle<int> (std::clamp (at - 1, track.getX(), track.getRight() - 2), track.getY(), 2, track.getHeight()));

        g.setColour (Look::colour (theme, lit ? "ink" : "ink-dim"));
        g.setFont (Look::font (theme, 11.0f));
        g.drawFittedText (textFor (kind, shown), track, juce::Justification::centred, 1);

        g.setColour (Look::colour (theme, "rule"));
        g.drawRect (area, 1);
    }
}
