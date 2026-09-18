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
    The undo history as a vertical stack with a pointer in it, and OK and
    Cancel under it (author, 2026-09-18: "an undo/redo history ... opens in
    place of the Inspector and shows a diff overlay on the cues as the user
    drags a pointer. This is only applied with an OK button or dismissed with
    a Cancel button. Both will also close the panel.").

    IT STANDS WHERE THE INSPECTOR STANDS. The stack is the engine's two lists
    of names, newest at the top: what Redo would put back, dim; the pointer,
    which is where the show stands now; what Undo would unmake; and the show
    as opened at the bottom. A click or a drag over a row moves the show to
    stand after that transaction - which is that many `undo` or `redo`
    records, sent when the hand settles - and the list beside shows what
    standing there changed against the picture taken when the panel opened.
    OK keeps where it stands; Cancel goes back to where it was opened. Both
    close it.
*/

#include <wfg/client/model/Theme.h>
#include <wfg/client/model/UndoHistory.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <string>
#include <vector>

namespace wfg::client::ui
{
    class UndoPanelComponent final : public juce::Component
    {
    public:
        struct Actions
        {
            /** Stand after that many transactions: the window sends the undos or redos. */
            std::function<void (int index)> moveTo;

            std::function<void()> ok;
            std::function<void()> cancel;
        };

        UndoPanelComponent (const model::Theme& theme, Actions actions);

        /** Every pass: the stack, what standing here changed, and where the panel was opened. */
        void show (const model::UndoReading& reading, const model::Diff& diff, int openedAt);

        void applyTheme (const model::Theme& theme);

        void paint (juce::Graphics& g) override;
        void resized() override;

    private:
        class Stack final : public juce::Component
        {
        public:
            explicit Stack (UndoPanelComponent& ownerToUse) : owner (ownerToUse) {}

            void paint (juce::Graphics& g) override;
            void mouseDown (const juce::MouseEvent& event) override;
            void mouseDrag (const juce::MouseEvent& event) override;
            void mouseUp (const juce::MouseEvent& event) override;

        private:
            UndoPanelComponent& owner;
        };

        int rowHeight() const noexcept;
        int stackHeight() const noexcept;
        void paintStack (juce::Graphics& g, int width);
        void pointedAt (int y, bool settled);
        void layOutStack();

        Actions actions;
        model::Theme theme;
        model::UndoReading reading;
        std::vector<model::Standing> stack;
        model::Diff changes;
        int openedAt = 0;

        /*  WHERE THE HAND IS POINTING while it drags, or -1: drawn at once,
            sent when it settles - every row passed would otherwise be a
            flurry of undos and redos the log would keep. */
        int pointing = -1;

        std::string drawnKey;

        juce::Label heading;
        juce::Label summary;
        juce::TextButton okButton { "OK" };
        juce::TextButton cancelButton { "Cancel" };
        juce::Viewport viewport;
        Stack stackView { *this };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UndoPanelComponent)
    };
}
