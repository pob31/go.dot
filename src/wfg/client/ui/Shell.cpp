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
    Shell::Shell (const model::Theme& theme,
                  TransportComponent::Actions transportActions,
                  CueListComponent::Actions listActions)
        : transport (theme, std::move (transportActions)),
          cues (theme, std::move (listActions))
    {
        /*  NEITHER PANE TAKES THE FOCUS: it rests here, and `keyPressed` below
            offers each key to both. */
        transport.setWantsKeyboardFocus (false);
        cues.setWantsKeyboardFocus (false);

        /*  The transport grows a row when the recovery banner appears, so the
            layout is asked for again rather than computed once. */
        transport.onHeightChanged = [this] { resized(); };

        addAndMakeVisible (transport);
        addAndMakeVisible (cues);

        setWantsKeyboardFocus (true);
    }

    void Shell::applyTheme (const model::Theme& theme)
    {
        transport.applyTheme (theme);
        cues.applyTheme (theme);
        resized();
        repaint();
    }

    void Shell::resized()
    {
        auto area = getLocalBounds();

        transport.setBounds (area.removeFromTop (juce::jmin (transport.preferredHeight(),
                                                             area.getHeight())));
        cues.setBounds (area);
    }

    bool Shell::keyPressed (const juce::KeyPress& key)
    {
        /*  THE LIST FIRST, because its keys are the ones an operator presses
            without looking - the arrows and Space - and the transport's are
            the ones they press deliberately. Neither set overlaps except
            Space, which both would send to GO. */
        return cues.keyPressed (key) || transport.keyPressed (key);
    }
}
