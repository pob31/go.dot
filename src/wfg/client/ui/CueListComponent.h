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

#include <wfg/client/model/Reorder.h>
#include <wfg/client/model/ShowModel.h>
#include <wfg/client/model/Theme.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <string>
#include <vector>

namespace wfg::client::ui
{
    class CueListComponent final : public juce::Component,
                                   public juce::FileDragAndDropTarget,
                                   public juce::DragAndDropTarget,
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

            /*  A CLICK ON A ROW'S BODY, with what the hand held: shift extends
                from the anchor, ctrl/⌘ toggles, neither picks this one alone
                (model/Selection.h). Empty id with neither means nothing. */
            std::function<void (const std::string&, bool extend, bool toggle)> pick;

            /*  MEDIA ARRIVING FROM OUTSIDE (decision Y): files dropped to be
                made into cues at a member position, or one file dropped onto
                a cue that should name it instead. The window does the copying
                and the commands; this pane only says where the hand let go.

                `index` is a MEMBER position in `parent`, which is the index a
                create speaks in, or **-1 for the end** - which this pane
                cannot name itself, having only the rows it drew. */
            std::function<void (const std::string& parent, int index,
                                const juce::StringArray& files)> importMedia;
            std::function<void (const std::string& cueId,
                                const juce::String& file)> linkMedia;

            /*  A ROW DRAGGED IN THE LIST (model/Reorder.h): moved after or
                into another, or dropped on a fade or a stop to become what it
                aims at. This pane says what the hand did; the window sends
                the one command each means. */
            std::function<void (const std::string& id, const std::string& parent, int index)> move;
            std::function<void (const std::string& aimedCue, const std::string& atCue)> setTarget;

            /** Deletes what is picked: ctrl/⌘-Backspace. One `object.delete` each; undo brings them back. */
            std::function<void()> removeChosen;

            /** Picks every cue the list draws: ctrl/⌘-A. */
            std::function<void()> pickAll;
        };

        CueListComponent (const model::Theme& theme, Actions actions);

        /*  The rows, where the pointer is, and which cues are picked. Cheap
            when none of the three has moved. */
        void show (const model::ShowModel& model, const std::string& standbyId,
                   const std::vector<std::string>& chosenIds);

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

        bool isInterestedInFileDrag (const juce::StringArray& files) override;
        void fileDragEnter (const juce::StringArray& files, int x, int y) override;
        void fileDragMove (const juce::StringArray& files, int x, int y) override;
        void fileDragExit (const juce::StringArray& files) override;
        void filesDropped (const juce::StringArray& files, int x, int y) override;

        /*  THE ROW DRAG. The list starts it (`getDragSourceDescription` names
            the cue), the shell carries it, and this pane is where it lands:
            the same rows, the same feedback shapes a dropped file uses - a
            line under for "after", the whole row lit for "on". */
        juce::var getDragSourceDescription (const juce::SparseSet<int>& rowsToDescribe) override;
        bool isInterestedInDragSource (const SourceDetails& details) override;
        void itemDragEnter (const SourceDetails& details) override;
        void itemDragMove (const SourceDetails& details) override;
        void itemDragExit (const SourceDetails& details) override;
        void itemDropped (const SourceDetails& details) override;

        /** What letting go of the dragged row at this point would do. */
        model::Drop dropAt (const SourceDetails& details, int& rowOut) const;
        const model::Row* rowById (const std::string& id) const;

        int rowUnder (int y) const;

        /*  WHERE THE RAILS STAND, asked in one place because four kinds of row
            draw the same shape and the shape only holds if they agree: a
            contained row draws the rule down its left, a container starts that
            rule under itself, the last row inside turns it right, and a
            section's band carries it through. They disagreed once - a band sat
            one indent right of the rows it headed, because a section's members
            share their band's depth and the band was measuring as though they
            were one deeper (author, 2026-09-18: "the expanded bracket is not
            always well aligned"). */
        int railsOrigin() const noexcept;
        int railAt (int level) const noexcept;
        void paintRails (const model::Row& entry, int row, juce::Graphics& g, int height);
        void paintBand (const model::Row& entry, int row, juce::Graphics& g, int width, int height);

        /*  THE COLUMN LABELS, drawn by this component and not by the list, so
            they stay put while the rows scroll under them (author,
            2026-09-18: "we're missing Pre/Duration/Post for the column labels
            at the top of the cuelist. These should stay visible when
            scrolling"). Laid out from the same three widths the rows use, so
            a label and its column cannot come apart. */
        void paintHeadings (juce::Graphics& g, juce::Rectangle<int> area);
        int headingHeight() const noexcept;

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
        std::vector<std::string> chosen;     ///< the picked cues, as the selection holds them
        bool isChosen (const std::string& id) const;
        int dropRow = -1;            ///< the row a file drag is over, or -1
        bool dropWouldInsert = false;   ///< whether letting go really inserts after that row
        bool dropWouldLink = false;  ///< whether letting go there names a cue's file, or lands ON the row
        std::size_t drawnWalk = 0;
        std::string drawnList;

        int rowHeight() const noexcept;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CueListComponent)
    };
}
