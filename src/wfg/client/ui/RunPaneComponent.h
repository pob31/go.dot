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
    The running pane: what the show is doing, now.

    Beside the cue list, which is what the show WILL do. The two answer
    different questions and the page keeps them apart for that reason - Didi is
    the document, Gogo is the present tense - and so does this.

    IT REBUILDS EVERY PASS, unlike the cue list. Runs appear and end on any
    tick and there is no revision to key them on, because a run is not a
    decision anybody recorded (§4.10). A dozen rows twenty-five times a second
    is nothing; the same treatment on five hundred cue rows would be the whole
    budget, which is why only one of these two panes has a cache.

    A KILL IS A CLICK ON THE ROW'S RIGHT EDGE, where the cross is drawn - the
    mirror of the cue list's park gutter on the left, and for the same reason:
    a gesture that acts on the whole row is one nobody can aim.
*/

#include <wfg/client/model/RunModel.h>
#include <wfg/client/model/Theme.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <string>
#include <vector>

namespace wfg::client::ui
{
    class RunPaneComponent final : public juce::Component,
                                   private juce::ListBoxModel
    {
    public:
        struct Actions
        {
            std::function<void (const std::string&)> kill;
        };

        RunPaneComponent (const model::Theme& theme, Actions actions);

        /** The runs this pass found. Cheap when they are the ones already drawn. */
        void show (std::vector<model::RunRow> runs);

        void applyTheme (const model::Theme& theme);

        void paint (juce::Graphics& g) override;
        void resized() override;

    private:
        int getNumRows() override;
        void paintListBoxItem (int row, juce::Graphics& g, int width, int height,
                               bool rowIsSelected) override;
        void listBoxItemClicked (int row, const juce::MouseEvent& event) override;

        Actions actions;
        model::Theme theme;
        juce::ListBox list { "runs", this };
        std::vector<model::RunRow> rows;

        int rowHeight() const noexcept;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RunPaneComponent)
    };
}
