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
    Load to time: the history as a vertical stack, the aim as a number, and
    the answer as words (PRD §3.13; author, 2026-09-18).

    IT STANDS WHERE THE INSPECTOR STANDS, for as long as the operator is
    looking before they leap, and only a GO takes it down: "The panel is only
    closed when the user triggers Go." Everything it shows is the engine's -
    the steps, the aim, the solve - read once a pass through model/LoadToTime;
    everything it sends is one of two gestures, `list.aim` on every change and
    `list.loadToTime` on the one button.

    THE STACK IS NEWEST AT THE TOP, as the node spells it: the last thing that
    happened is the first thing read. The aimed cue's own step is lit; the
    steps past the instant are dim, since a load would take them back; and
    the pointer - the aim itself - is drawn as a line between the steps at the
    moment it names, with its offset beside it.
*/

#include <wfg/client/model/LoadToTime.h>
#include <wfg/client/model/Theme.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <string>

namespace wfg::client::ui
{
    class HistoryPanelComponent final : public juce::Component
    {
    public:
        struct Actions
        {
            /** Point at a cue, that many seconds in (-1 for before it). */
            std::function<void (const std::string& cueId, double offset)> aim;

            /** Make the aim true. */
            std::function<void()> load;
        };

        HistoryPanelComponent (const model::Theme& theme, Actions actions);

        /** Every pass: the reading. Cheap when nothing moved. */
        void show (const model::LoadToTimeReading& reading);

        void applyTheme (const model::Theme& theme);

        void paint (juce::Graphics& g) override;
        void resized() override;

    private:
        class Stack final : public juce::Component
        {
        public:
            explicit Stack (HistoryPanelComponent& ownerToUse) : owner (ownerToUse) {}

            void paint (juce::Graphics& g) override;
            void mouseUp (const juce::MouseEvent& event) override;

        private:
            HistoryPanelComponent& owner;
        };

        int rowHeight() const noexcept;
        int stackHeight() const noexcept;
        void paintStack (juce::Graphics& g, int width);
        void stackClicked (int y);
        void commitOffset();
        void layOutStack();

        Actions actions;
        model::Theme theme;
        model::LoadToTimeReading reading;

        /*  What the stack was last drawn from, so a pass that moved nothing
            repaints nothing: the steps, the aim, and the instant. */
        std::string drawnKey;

        juce::Label heading;
        juce::Label aimLabel;
        juce::Label answer;
        juce::TextEditor offsetBox;
        juce::TextButton beforeButton { "before" };
        juce::TextButton loadButton { "Load" };
        juce::Viewport viewport;
        Stack stack { *this };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HistoryPanelComponent)
    };
}
