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
    What the window holds: the transport across the top, the cue list under it.

    Two panes today. The arrangement the author settled on the page - the
    inspector between the panes when something is picked, the running pane
    beside the list - arrives as those views do, and this is where it will be
    decided, because a layout is one object's job or it is nobody's.

    IT ALSO OWNS THE KEYBOARD, and that is deliberate. Neither pane takes focus
    of its own: focus rests here and the keys are offered to the list first and
    the transport second, so Space reaches GO wherever the pointer happens to
    be and ctrl/⌘-S reaches the save whether or not the list was last clicked.
    A window where the shortcut works only when the right pane is focused is a
    window an operator learns to distrust in the dark.
*/

#include <wfg/client/model/Theme.h>
#include <wfg/client/ui/CueListComponent.h>
#include <wfg/client/ui/InspectorComponent.h>
#include <wfg/client/ui/RunPaneComponent.h>
#include <wfg/client/ui/TransportComponent.h>

#include <juce_gui_basics/juce_gui_basics.h>

namespace wfg::client::ui
{
    class Shell final : public juce::Component
    {
    public:
        Shell (const model::Theme& theme,
               TransportComponent::Actions transportActions,
               CueListComponent::Actions listActions,
               RunPaneComponent::Actions runActions,
               InspectorComponent::Actions inspectorActions);

        void applyTheme (const model::Theme& theme);

        void resized() override;
        bool keyPressed (const juce::KeyPress& key) override;

        TransportComponent transport;
        CueListComponent cues;
        RunPaneComponent runs;
        InspectorComponent inspector;

        /*  THE INSPECTOR STANDS DOWN WHEN NOTHING IS PICKED, which is the
            arrangement the author settled on the page: the two list panes have
            the width to themselves until somebody asks about one cue. */
        void setInspecting (bool showing);

    private:
        bool inspecting = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Shell)
    };
}
