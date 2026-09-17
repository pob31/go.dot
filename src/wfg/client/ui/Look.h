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
    };
}
