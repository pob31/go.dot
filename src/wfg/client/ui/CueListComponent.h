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
    The cue list: the show as rows, and the pointer at the one GO will fire.

    A juce::ListBox, so only the rows on screen are painted however long the
    show is, and the rows themselves come from model::ShowModel - which is
    rebuilt when the show changes and not when the operator moves (M0). This
    class does the other half: it draws, and it repaints as little as it can.

    THE TWO RATES, which is the whole performance design and is one line:
    `updateContent()` at show-change rate, `repaintRow()` at tick rate,
    `repaint()` never. A GO moves the standby twenty times in a chain and
    nothing is rebuilt; what changes is two rows' decoration, and only those
    two are asked to paint again.

    COLOUR IS NEVER THE SOLE CARRIER (PRD §4.8). The standby row carries a
    pointer glyph and its number is drawn in the standby colour; a disabled cue
    is dimmed AND its name is struck; a group says its mode in words. Somebody
    reading this list on a projector with the colour wrong still knows where GO
    will go.

    WHAT THIS DOES NOT DO YET: selection, dragging, editing a cell. Those are
    M5 and after, and each wants a round of the author's eye before it is
    built. What it does carry is the standby arrows, because a pointer nobody
    can move is a pointer nobody can judge.
*/

#include <wfg/client/model/ShowModel.h>
#include <wfg/client/model/Theme.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <string>

namespace wfg::client::ui
{
    class CueListComponent final : public juce::Component,
                                   private juce::ListBoxModel
    {
    public:
        struct Actions
        {
            std::function<void()> standbyNext;
            std::function<void()> standbyPrevious;
            std::function<void (const std::string&)> park;
            std::function<void()> go;

            /** A sentence for the reader, when a gesture is declined before it is sent. */
            std::function<void (const juce::String&)> say;

            /** Opens or shuts a section, by the key its head carries. */
            std::function<void (const std::string&)> fold;

            /** Which cue the inspector should be about. Empty when none is picked. */
            std::function<void (const std::string&)> pick;
        };

        CueListComponent (const model::Theme& theme, Actions actions);

        /*  The rows, where the pointer is, and which cue is picked. Cheap
            when none of the three has moved. */
        void show (const model::ShowModel& model, const std::string& standbyId,
                   const std::string& pickedId);

        void applyTheme (const model::Theme& theme);

        void paint (juce::Graphics& g) override;
        void resized() override;
        bool keyPressed (const juce::KeyPress& key) override;

    private:
        int getNumRows() override;
        void paintListBoxItem (int row, juce::Graphics& g, int width, int height,
                               bool rowIsSelected) override;
        void listBoxItemClicked (int row, const juce::MouseEvent& event) override;
        void backgroundClicked (const juce::MouseEvent& event) override;
        void paintBand (const model::Row& entry, int row, juce::Graphics& g, int width, int height);

        Actions actions;
        model::Theme theme;
        juce::ListBox list { "cues", this };

        /*  A COPY, not a pointer into the model: the model is rebuilt whole
            when the show moves, and a view holding references into the vector
            it replaces is the oldest bug in this shape. Copying five hundred
            rows happens at show-change rate, which is when somebody typed - so
            the cost is paid where nobody can feel it. */
        std::vector<model::Row> rows;
        std::string standby;
        int standbyRow = -1;
        std::string picked;
        int pickedRow = -1;
        std::size_t drawnWalk = 0;
        std::string drawnList;

        int rowHeight() const noexcept;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CueListComponent)
    };
}
