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
    The vertical list a new-cue button opens (the author, 2026-09-27), drawn
    from the lines model/NewCueMenus.h decided: headings, lines to click,
    greyed sentences and rules, under the button that opened it.

    It answers with the line clicked - which choice, and whether it makes the
    group around the picked cues - and says nothing when the list is closed
    without a click. What the line makes is the window's to send.
*/

#include <wfg/client/model/NewCueMenus.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <utility>
#include <vector>

namespace wfg::client::ui
{
    /*  The PopupMenu for `lines`, as JUCE is handed it: item identifiers
        encode (choice, wrap) so the answer can be read back. Separate from
        showing it so a test can look at what would be shown. */
    juce::PopupMenu newCueMenu (const std::vector<model::MenuLine>& lines);

    /*  Shown under `anchor`, asynchronously - JUCE_MODAL_LOOPS_PERMITTED is
        off - and `chosen` is called with the line clicked, never when the list
        is closed without one, nor when the anchor has gone by then. */
    void showNewCueMenu (juce::Component& anchor, const std::vector<model::MenuLine>& lines,
                         std::function<void (int choice, bool wrap)> chosen);

    /** The line a PopupMenu answer names: (choice, wrap), or choice -1 for none. */
    std::pair<int, bool> choiceOfMenuItem (int menuItemId);
}
