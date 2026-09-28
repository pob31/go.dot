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

#include <wfg/client/ui/Shell.h>

#include <wfg/client/ui/Look.h>

#include <utility>

namespace wfg::client::ui
{
    Shell::Shell (const model::Theme& themeToUse,
                  TransportComponent::Actions transportActions,
                  CueListComponent::Actions listActions,
                  RunPaneComponent::Actions runActions,
                  InspectorComponent::Actions inspectorActions,
                  NewCueBarComponent::Actions newCueActions,
                  HistoryPanelComponent::Actions historyActions,
                  UndoPanelComponent::Actions undoActions,
                  FootPanelComponent::Actions footActions)
        : transport (themeToUse, std::move (transportActions)),
          foot (themeToUse, std::move (footActions)),
          cues (themeToUse, std::move (listActions)),
          runs (themeToUse, std::move (runActions)),
          inspector (themeToUse, std::move (inspectorActions)),
          newCues (themeToUse, std::move (newCueActions)),
          history (themeToUse, std::move (historyActions)),
          undoPanel (themeToUse, std::move (undoActions)),
          liveBar (themeToUse, {}),
          theme (themeToUse)
    {
        /*  NEITHER PANE TAKES THE FOCUS: it rests here, and `keyPressed` below
            offers each key to both. */
        transport.setWantsKeyboardFocus (false);
        cues.setWantsKeyboardFocus (false);

        /*  The transport grows a row when the recovery banner appears, so the
            layout is asked for again rather than computed once. */
        transport.onHeightChanged = [this] { resized(); };

        addAndMakeVisible (transport);
        addAndMakeVisible (newCues);
        addAndMakeVisible (cues);
        addAndMakeVisible (runs);

        inspector.setVisible (false);
        addChildComponent (inspector);
        history.setVisible (false);
        addChildComponent (history);
        undoPanel.setVisible (false);
        addChildComponent (undoPanel);
        foot.setVisible (false);
        addChildComponent (foot);
        liveBar.setVisible (false);
        addChildComponent (liveBar);

        setWantsKeyboardFocus (true);
    }

    void Shell::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;
        transport.applyTheme (theme);
        newCues.applyTheme (theme);
        cues.applyTheme (theme);
        runs.applyTheme (theme);
        inspector.applyTheme (theme);
        history.applyTheme (theme);
        undoPanel.applyTheme (theme);
        foot.applyTheme (theme);
        liveBar.applyTheme (theme);
        resized();
        repaint();
    }

    void Shell::resized()
    {
        auto area = getLocalBounds();

        transport.setBounds (area.removeFromTop (juce::jmin (transport.preferredHeight(),
                                                             area.getHeight())));

        //  What a locked show rode live, under the transport and across the window, while it has anything.
        if (liveBar.isVisible())
            liveBar.setBounds (area.removeFromTop (juce::jmin (liveBar.preferredHeight(), area.getHeight())));

        /*  THE FOOT SPANS THE WHOLE WIDTH, under all three panes and not under
            the cue list alone (author, 2026-09-21: it is where a timeline, a
            waveform or a video goes, and those are wide). Cut here, before the
            columns are worked out, so the panes above simply have less height
            and none of them has to know the panel exists.

            Its height is clamped to what the window can spare: a floor of four
            rows, which is a bar with a ruler under it and no less, and never
            more than half the window, because a panel that could take all of
            it would leave somebody with no cue list and no obvious way back. */
        if (foot.subject().isOpen())
        {
            const auto row = juce::roundToInt (theme.row * theme.type);
            const auto floorHeight = row * 4;
            const auto ceiling = juce::jmax (floorHeight, area.getHeight() / 2);

            /*  NINE ROWS TO BEGIN WITH, which is a bar with a ruler under it
                AND the table beside it showing its heading and half a dozen
                ranges. Seven was the height before the numbers arrived, and it
                opened on a table that could show two of them. */
            if (footHeight <= 0)
                footHeight = juce::jmin (ceiling, row * 9);

            footHeight = juce::jlimit (floorHeight, ceiling, footHeight);
            foot.setBounds (area.removeFromBottom (juce::jmin (footHeight, area.getHeight())));
        }

        /*  WHAT THE SHOW WILL DO ON THE LEFT, WHAT IT IS DOING ON THE RIGHT.
            The page keeps Didi and Gogo apart for the same reason and the
            author reads them that way; the inspector arrives between them
            when something is picked, which is the arrangement they settled on
            the page (§14.3) and the space this split leaves room for. */
        /*  A LITTLE AIR BETWEEN THE PANES (author, 2026-09-18: "can we pad a
            little between the different panels?"): a third of a row, in the
            ground colour, so each pane reads as its own surface. */
        const auto gap = juce::roundToInt (theme.row * theme.type / 3.0);

        /*  THE LIST KEEPS A WIDTH IT CAN BE READ AND EDITED AT. Driving
            double-clicks from a script found the failing ones failing on
            geometry (2026-09-18): with the running pane at two fifths and the
            inspector at two fifths of the rest, the list in a 1105-pixel window
            was 380 pixels wide, and after the times and the kind were carved
            from the right the name of an indented cue was twelve pixels of
            column - a double-click on the name hit the kind. The running pane
            takes a third, the inspector a bounded share, and the list is never
            squeezed under `listFloor` while the window can give it that. */
        const auto total = area.getWidth();
        const auto listFloor = juce::roundToInt (theme.row * theme.type * 16);   // ~520 px at the default type
        const auto runsWidth = juce::jlimit (juce::jmin (220, total), juce::jmax (220, total - listFloor),
                                             total / 3);

        /*  AND THE FOOT IS TOLD WHERE THAT BOUNDARY IS, so its own split - the
            picture on the left, the numbers on the right - falls on the same
            line as the one between the inspector and the running cues (author,
            2026-09-21: *"align and pad the waveform and range table with the
            separation between the inspector and the running cues"*). One line
            down the window rather than two a few pixels apart. */
        foot.setRightColumn (runsWidth, gap);

        runs.setBounds (area.removeFromRight (runsWidth));
        area.removeFromRight (gap);

        /*  AND THE INSPECTOR BETWEEN THEM, only when something is picked -
            the arrangement the author chose on the page (`3115b10`), where
            the two list panes shrink to make room rather than a third pane
            standing empty whenever nobody is asking about a cue. */
        /*  THE NEW-CUE ROW FIRST, across the whole of what is left, and only
            while the show may be edited: a row that never moves is the point
            of it (author, 2026-09-18: "leave the strip with the new cue
            buttons on the same width and start the inspector panel beneath
            it so the buttons stay in place"). So it is cut before the
            inspector takes its column, and the inspector opens under it. */
        if (editing)
            newCues.setBounds (area.removeFromTop (juce::jmin (newCues.preferredHeight(),
                                                               area.getHeight())));

        if (shown != Panel::none)
        {
            const auto inspectorWidth = juce::jlimit (juce::jmin (240, area.getWidth()),
                                                      juce::jmax (240, area.getWidth() - listFloor),
                                                      area.getWidth() / 3);
            const auto slot = area.removeFromRight (inspectorWidth);

            if (shown == Panel::inspector)
                inspector.setBounds (slot);
            else if (shown == Panel::history)
                history.setBounds (slot);
            else
                undoPanel.setBounds (slot);

            area.removeFromRight (gap);
        }

        cues.setBounds (area);
    }

    void Shell::setLive (int count, bool locked)
    {
        const auto wanted = liveBar.setLive (count, locked);

        if (wanted != liveBar.isVisible())
        {
            liveBar.setVisible (wanted);
            resized();
        }
    }

    void Shell::setFoot (const model::Subject& wanted)
    {
        const auto was = foot.subject().isOpen();

        foot.open (wanted);
        foot.setVisible (wanted.isOpen());

        if (was != wanted.isOpen())
            resized();
    }

    void Shell::growFoot (int pixels)
    {
        if (! foot.subject().isOpen() || pixels == 0)
            return;

        footHeight = juce::jmax (0, footHeight + pixels);
        resized();
    }

    void Shell::setEditing (bool editable)
    {
        if (editable == editing)
            return;

        editing = editable;
        newCues.setVisible (editable);
        cues.setEditable (editable);
        runs.setEditing (editable);
        resized();
    }

    void Shell::setPanel (Panel showing)
    {
        if (showing == shown)
            return;

        shown = showing;
        inspector.setVisible (shown == Panel::inspector);
        history.setVisible (shown == Panel::history);
        undoPanel.setVisible (shown == Panel::undo);
        resized();
    }

    bool Shell::keyPressed (const juce::KeyPress& key)
    {
        /*  THE LIST FIRST, because its keys are the ones an operator presses
            without looking - the arrows and Space - and the transport's are
            the ones they press deliberately. Neither set overlaps except
            Space, which both would send to GO. */
        return cues.keyPressed (key) || transport.keyPressed (key)
            || (menuKeys != nullptr && menuKeys (key));
    }
}
