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
#include <wfg/client/ui/FootPanelComponent.h>
#include <wfg/client/ui/HistoryPanelComponent.h>
#include <wfg/client/ui/InspectorComponent.h>
#include <wfg/client/ui/LiveBarComponent.h>
#include <wfg/client/ui/NewCueBarComponent.h>
#include <wfg/client/ui/RunPaneComponent.h>
#include <wfg/client/ui/TransportComponent.h>
#include <wfg/client/ui/UndoPanelComponent.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace wfg::client::ui
{
    /*  A DragAndDropContainer as well, because a row dragged in the cue list
        needs an ancestor to carry it: JUCE looks up from the list for the
        nearest one, and this is the nearest thing that holds every pane. */
    class Shell final : public juce::Component,
                        public juce::DragAndDropContainer
    {
    public:
        Shell (const model::Theme& themeToUse,
               TransportComponent::Actions transportActions,
               CueListComponent::Actions listActions,
               RunPaneComponent::Actions runActions,
               InspectorComponent::Actions inspectorActions,
               NewCueBarComponent::Actions newCueActions,
               HistoryPanelComponent::Actions historyActions,
               UndoPanelComponent::Actions undoActions,
               FootPanelComponent::Actions footActions);

        void applyTheme (const model::Theme& theme);

        void resized() override;
        bool keyPressed (const juce::KeyPress& key) override;

        /*  THE MENU'S OWN KEYS, asked last: what neither pane claims is
            offered to the window that owns the menu, so ctrl/⌘-N, -O and
            -shift-S reach the items they are printed beside. */
        std::function<bool (const juce::KeyPress&)> menuKeys;

        TransportComponent transport;
        FootPanelComponent foot;
        CueListComponent cues;
        RunPaneComponent runs;
        InspectorComponent inspector;
        NewCueBarComponent newCues;
        HistoryPanelComponent history;
        UndoPanelComponent undoPanel;

        /*  WHAT A LOCKED SHOW RODE LIVE (2026-09-25): a row under the transport
            while something rides, asking once the show is unlocked whether to
            keep it or discard it. Hidden while nothing does. */
        LiveBarComponent liveBar;
        void setLive (int count, bool locked);

        /*  THE NEW-CUE ROW STANDS WHILE THE SHOW MAY BE EDITED and goes when
            it is locked (author, 2026-09-18: "Lock makes them disappear"). A
            button that would only be refused is not offered, and a show in
            show mode reads as one: the list takes the row back. */
        void setEditing (bool editable);

        /*  THE PANEL AT THE FOOT, which is open when it has a subject and shut
            when it has none - there is no separate flag, because two ways of
            saying the same thing is how a window comes to be showing a panel
            about nothing. Its height is kept here rather than in the panel:
            how much of the window it may take is the window's question. */
        void setFoot (const model::Subject&);
        const model::Subject& footSubject() const noexcept { return foot.subject(); }
        void growFoot (int pixels);

        /*  ONE SLOT BESIDE THE LIST, THREE THINGS THAT CAN STAND IN IT: nothing,
            which is the arrangement the author settled on the page - the two
            list panes have the width to themselves until somebody asks about
            one cue; the inspector, when something is picked; or the load to
            time panel, which takes the inspector's place for as long as the
            operator is looking before they leap (author, 2026-09-18: "The
            Inspector panel turns into a history vertical stack"). */
        enum class Panel { none, inspector, history, undo };

        void setPanel (Panel showing);
        Panel panel() const noexcept { return shown; }

    private:
        model::Theme theme;
        Panel shown = Panel::none;
        bool editing = true;

        /*  How tall the foot is, in pixels, and how tall it may be. The floor
            is a bar with a ruler under it and no less; the ceiling is half the
            window, because a panel that could take all of it would leave
            somebody with no cue list and no obvious way back. */
        int footHeight = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Shell)
    };
}
