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
    AN OSC CUE'S MESSAGES AS A GRID, IN A WINDOW OF ITS OWN (the author,
    2026-10-17, his picks: its own window, cells typed in; namespace draft §80).

    A table a family - `/wfs/input` - a row a parameter, a column a channel the
    cue holds and no other, the recorded value in each cell the cue has one
    for (`model/OscGrid`). A double click on a filled cell opens it for typing;
    Enter writes the message's value row, one `node.set` and one step of Undo,
    Esc inside the box takes it back - so PANIC never hears it - and Esc
    anywhere else is PANIC, as in every window of the show. An empty cell stays
    empty: the grid shows what was recorded and adds nothing (the author: "Don't
    add columns"). Under the lock it is read only, and says so.

    It shows whichever cue it was handed, every pass; the window follows the
    pick of an OSC cue while it is open.
*/

#include <wfg/client/model/OscGrid.h>
#include <wfg/client/model/OscMessages.h>
#include <wfg/client/model/Theme.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <string>

namespace wfg::client::ui
{
    class OscGridWindow final : public juce::DocumentWindow
    {
    public:
        struct Actions
        {
            /** One `node.set`: a message's value row and its new list. */
            std::function<void (const std::string& address, const std::string& text)> set;

            /** Esc, which is PANIC in every window of the show. */
            std::function<void()> panic;
        };

        OscGridWindow (const model::Theme& theme, Actions actions);
        ~OscGridWindow() override;

        /*  THE CUE TO SHOW, and its name, as the last pass read it. Cheap when
            nothing moved; never redrawn under a cell being typed in. */
        void show (const model::OscMessagesReading& reading, const std::string& cueName);

        void open();
        void closeButtonPressed() override;
        bool keyPressed (const juce::KeyPress& key) override;

        /** The cue it shows, empty before the first. */
        const std::string& cueId() const noexcept;

        //  For a test: the grid as drawn, a cell's place, a cell opened and typed into.
        const model::OscGrid& gridForTest() const;
        juce::Rectangle<int> cellBoundsForTest (std::size_t table, std::size_t row, std::size_t column) const;
        bool typeIntoCellForTest (std::size_t table, std::size_t row, std::size_t column, const juce::String& text);
        juce::String statusForTest() const;

    private:
        class Content;
        std::unique_ptr<Content> content;
        std::function<void()> panicAction;
    };
}
