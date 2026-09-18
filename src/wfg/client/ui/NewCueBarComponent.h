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
    One row of buttons, one per kind of cue, in a place that does not move.

    The author's word (2026-09-18): a stable UI for making cues, that the lock
    makes disappear, and that helps getting started - an empty show has a
    button to press before it has a row to drop a file on. It stands over the
    cue list and is the same row whatever is picked; what changes with the
    pick is WHERE the cue lands (model/NewCue.h), and the tooltip says so.

    The buttons name the kind and nothing else. Which cue is picked, which list
    has focus and whether the show is locked are the window's to know; this
    only says which button was pressed.
*/

#include <wfg/client/model/Theme.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wfg::client::ui
{
    class NewCueBarComponent final : public juce::Component
    {
    public:
        struct Actions
        {
            /** A cue of this kind, where the window decides it goes. */
            std::function<void (const std::string& kind)> create;
        };

        NewCueBarComponent (const model::Theme& theme, Actions actions);

        void applyTheme (const model::Theme& theme);

        /** The tooltip every button carries: where the next cue will land, in words. */
        void setDestination (const juce::String& sentence);

        int preferredHeight() const noexcept;

        void paint (juce::Graphics& g) override;
        void resized() override;

    private:
        Actions actions;
        model::Theme theme;
        std::vector<std::unique_ptr<juce::TextButton>> buttons;
        juce::String destination;

        int rowHeight() const noexcept;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NewCueBarComponent)
    };
}
