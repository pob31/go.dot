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
    The transport: the strip the operator's eye rests on between cues.

    Which show, whether it is saved, where the clock is, which list is focused
    and which cue GO would fire, whether the audio is running, and the last
    thing the engine refused - one label each, a GO button, and Space.

    It is handed a TransportReading and never a snapshot: the model decides
    what the words are, this decides where they go. A label's setText repaints
    only when the text changed, so at 25 passes a second the only thing that
    redraws is the tick.

    Colour is never the sole carrier (PRD §4.8): the dirty dot comes with the
    word "unsaved", the standby's yellow with the word "standby", and a
    refusal's red with "error:".
*/

#include <wfg/client/model/Theme.h>
#include <wfg/client/model/Transport.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace wfg::client::ui
{
    class TransportComponent final : public juce::Component
    {
    public:
        struct Actions
        {
            std::function<void()> go;            ///< the button, and Space
            std::function<void()> reloadTheme;   ///< F5
        };

        TransportComponent (const model::Theme& theme, Actions actions);

        /** The reading to show. Cheap when nothing changed. */
        void show (const model::TransportReading& reading);

        /** Fonts and colours from the theme; the window's look-and-feel carries the rest. */
        void applyTheme (const model::Theme& theme);

        /** A sentence for the bottom line - a theme file's refusal, mostly. Empty clears it. */
        void setNotice (const juce::String& notice);

        void paint (juce::Graphics& g) override;
        void resized() override;
        bool keyPressed (const juce::KeyPress& key) override;

    private:
        Actions actions;
        model::Theme theme;
        model::TransportReading last;
        bool shownOnce = false;

        juce::Label showLabel, tickLabel, clockLabel, rateLabel,
                    listLabel, standbyLabel, statusLabel, errorLabel, noticeLabel;
        juce::TextButton goButton { "GO" };

        int rowHeight() const noexcept;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransportComponent)
    };
}
