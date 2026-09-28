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
    WHAT A LOCKED SHOW RODE LIVE, AND WHAT TO DO WITH IT (author, 2026-09-25).

    Under the show lock a cue's EQ and sends are ridden like faders - heard at
    once and written to nothing. The author's decision: they last until the
    show is unlocked, and then the window asks whether to keep them in the show
    or let them go. This is that question: one row across the window, under
    the transport, shown whenever something rides live.

    TWO STATES, ONE ROW. While the show is locked it only says how many changes
    are riding and that they are not saved - nothing can be kept while the
    show refuses edits. Once unlocked it offers Keep in the show (`live.keep`,
    one undo step) and Discard (`live.drop`, the cues back to their saved
    sound), and stays until one is pressed.

    It only says and asks; the window decides when it is shown.
*/

#include <wfg/client/model/Theme.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <utility>

namespace wfg::client::ui
{
    class LiveBarComponent final : public juce::Component
    {
    public:
        struct Actions
        {
            std::function<void()> keep;
            std::function<void()> drop;
        };

        LiveBarComponent (const model::Theme& theme, Actions actions);

        void applyTheme (const model::Theme& theme);

        /** What the two buttons send, handed in once the window has built its gestures. */
        void setActions (Actions actionsToUse) { actions = std::move (actionsToUse); }

        /*  How many changes ride live, and whether the show is locked. Answers
            whether the bar has anything to say - nought changes, it has not. */
        bool setLive (int count, bool locked);

        int preferredHeight() const noexcept;

        void paint (juce::Graphics& g) override;
        void resized() override;

    private:
        Actions actions;
        model::Theme theme;

        juce::TextButton keep, drop;
        juce::String said;
        int shownCount = -1;
        bool shownLocked = false;

        int rowHeight() const noexcept;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LiveBarComponent)
    };
}
