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
    The Theme, as JUCE wants it: a LookAndFeel whose colours are the tokens.

    Nothing is drawn differently from LookAndFeel_V4 yet. What this class does
    is translate - a token name into a colour id, a 0xAARRGGBB into a
    juce::Colour, the type scale into a font - so that no component reads the
    Theme's map itself and a token renamed is renamed in one place.
*/

#include <wfg/client/model/Theme.h>

#include <juce_gui_basics/juce_gui_basics.h>

namespace wfg::client::ui
{
    class Look final : public juce::LookAndFeel_V4
    {
    public:
        explicit Look (const model::Theme& theme);

        /** Re-reads every colour. Called at start and on F5. */
        void apply (const model::Theme& theme);

        /** A theme colour as JUCE draws it. */
        static juce::Colour colour (const model::Theme& theme, const char* name);

        /** A font at `height` pixels before the theme's type scale is applied. */
        static juce::Font font (const model::Theme& theme, float height);

        /*  A BUTTON THAT BEGINS WITH A SHAPE draws the shape large and the
            word after it at a word's size (author, 2026-09-18: "only the
            triangle needed to be enlarged, the text itself in its previous
            size was correct"). A button draws one string in one font, so this
            is the look-and-feel drawing it in two. Marked with this property
            rather than by its name, so the rule is a thing a button declares
            about itself and this class does not have to know which buttons
            exist. */
        static const juce::Identifier& glyphButton();

        void drawButtonText (juce::Graphics& g, juce::TextButton& button,
                             bool shouldDrawButtonAsHighlighted,
                             bool shouldDrawButtonAsDown) override;

    private:
        /*  The type scale, kept because a look-and-feel is asked for a font
            long after the theme that set it has gone out of scope. Everything
            else this class needs it reads once, in `apply`. */
        float type = 1.0f;
    };
}
