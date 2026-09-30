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
    THE ICONS, DRAWN: lines and a few filled shapes on a 24-unit grid, scaled
    to whatever square they are given and stroked in whatever colour.

    Drawn rather than typed, because a symbol from a font is whatever the
    machine's fonts make of it and the author asked for "not the standard
    emoticons" (2026-09-30) - which is what a good many arrows and shapes turn
    into on Windows. A path is the same picture on every machine and at every
    size the theme's type scale asks for.

    ONE LINE WEIGHT FOR THE WHOLE SET, so a row of marks reads as one family
    rather than as clip art gathered from several places; it never goes
    thinner than a pixel, where a line starts to shimmer.
*/

#include <wfg/client/model/Icons.h>
#include <wfg/client/model/Theme.h>

#include <juce_gui_basics/juce_gui_basics.h>

namespace wfg::client::ui
{
    namespace icons
    {
        /** `icon` in `colour`, as large as fits square and centred in `area`. */
        void draw (juce::Graphics& g, model::Icon icon, juce::Rectangle<float> area, juce::Colour colour);

        /*  A MARK: its icon, a square as tall as the area, and its few
            characters after it in `font`. Drawn from the area's left; answers
            how wide it was, so marks can be laid one after another. */
        int drawMark (juce::Graphics& g, const model::Mark& mark, juce::Rectangle<int> area,
                      juce::Colour colour, const juce::Font& font);

        /** How wide `drawMark` would be, at `height`. */
        int markWidth (const model::Mark& mark, int height, const juce::Font& font);
    }

    /*  A BUTTON THAT IS AN ICON AND A WORD, and may stay pressed.

        Made for the panels an inspector opens at the foot (author,
        2026-09-30: "the toggles for the foot panels in the inspector should be
        at the top to make opening the panel really quick"): each is a shape
        and a word, and it is LIT while its panel is open on this cue - lit in
        three ways, a wash, a bar along its foot and a brighter word, so it is
        not the colour alone that says so (§4.8).

        The accent is a theme colour handed in, so the button draws in the
        show's palette without reading the theme itself. */
    class IconButton final : public juce::Button
    {
    public:
        IconButton (model::Icon icon, const juce::String& word);

        void setIcon (model::Icon icon);
        void setColours (juce::Colour ink, juce::Colour ground, juce::Colour accent);
        void setTextHeight (float height);

        /*  WHETHER THE WORD IS DRAWN. A narrow panel draws the icon alone and
            keeps the word as the button's text and tooltip, so it is still
            there for anybody who hovers - and for a test that reads it. */
        void setWordShown (bool shown);

        /** Wide enough for the icon and the word, at `height`. */
        int idealWidth (int height) const;

    protected:
        void paintButton (juce::Graphics& g, bool over, bool down) override;

    private:
        model::Icon icon;
        juce::Colour ink { juce::Colours::white }, ground { juce::Colours::black },
                     accent { juce::Colours::skyblue };
        float textHeight = 14.0f;
        bool wordShown = true;
    };
}
