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
#include <wfg/client/ui/NewCueBarComponent.h>
#include <wfg/client/ui/RunPaneComponent.h>
#include <wfg/client/ui/TransportComponent.h>

#include <juce_gui_basics/juce_gui_basics.h>

namespace wfg::client::ui
{
    /*  A DragAndDropContainer as well, because a row dragged in the cue list
        needs an ancestor to carry it: JUCE looks up from the list for the
        nearest one, and this is the nearest thing that holds every pane. */
    class Shell final : public juce::Component,
                        public juce::DragAndDropContainer
    {
    public:
        Shell (const model::Theme& theme,
               TransportComponent::Actions transportActions,
               CueListComponent::Actions listActions,
               RunPaneComponent::Actions runActions,
               InspectorComponent::Actions inspectorActions,
               NewCueBarComponent::Actions newCueActions);

        void applyTheme (const model::Theme& theme);

        void resized() override;
        bool keyPressed (const juce::KeyPress& key) override;

        TransportComponent transport;
        CueListComponent cues;
        RunPaneComponent runs;
        InspectorComponent inspector;
        NewCueBarComponent newCues;

        /*  THE NEW-CUE ROW STANDS WHILE THE SHOW MAY BE EDITED and goes when
            it is locked (author, 2026-09-18: "Lock makes them disappear"). A
            button that would only be refused is not offered, and a show in
            show mode reads as one: the list takes the row back. */
        void setEditing (bool editable);

        /*  THE INSPECTOR STANDS DOWN WHEN NOTHING IS PICKED, which is the
            arrangement the author settled on the page: the two list panes have
            the width to themselves until somebody asks about one cue. */
        void setInspecting (bool showing);

    private:
        bool inspecting = false;
        bool editing = true;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Shell)
    };
}
