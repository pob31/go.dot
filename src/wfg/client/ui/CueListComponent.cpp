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

#include <wfg/client/ui/CueListComponent.h>

#include <wfg/client/model/LoadToTime.h>

#include <wfg/client/ui/Look.h>

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <utility>

namespace wfg::client::ui
{
    void CueListComponent::revealCue (const std::string& id)
    {
        for (std::size_t index = 0; index < rows.size(); ++index)
            if (rows[index].id == id)
            { list.scrollToEnsureRowIsOnscreen (static_cast<int> (index)); return; }
    }
    namespace
    {
        /*  THE COLUMNS, as fractions of the width left after the number and
            the times, which are fixed. The author asked for preWait, duration
            and postWait as columns on the page and they are the same three
            here, in the same order - "something logical time wise", their
            words, and the order somebody works in rather than the alphabet. */
        constexpr int numberChars = 6;

        /*  TWICE THE SIZE THEY WERE (author, 2026-09-18: "you can enlarge these
            triangles two fold"). A twist is the one control on a row and it was
            drawn at the size of a word beside it. The glyph's ink is about half
            its em, so a cell one indent wide still holds it - and the number
            lives here rather than at three call sites so the three twists in
            this window cannot come to differ. */
        constexpr float twistHeight = 22.0f;
        constexpr int timeChars = 6;
        constexpr int kindChars = 8;

        /*  WHETHER LETTING GO ON THIS ROW REALLY DOES INSERT AFTER IT. Only a
            member can be pointed at: `cue.create` speaks in member positions,
            and a group's header and footer are separate orders that a role
            decides rather than places an index can reach. Asked in one spot
            because the answer has to be the same in the drawing and in the
            dropping, and drawing a promise the drop then breaks is the exact
            failure the feedback exists to prevent. */
        bool insertAfter (const model::Row& row)
        {
            /*  ANY CUE ROW THAT IS ITS OWN, since 2026-09-18: a header, footer
                or persistent row is in a section the tree now names, so a
                drop after it has a container to go into. A derived header
                line is a reading of a cue elsewhere and is not a place. */
            return row.rowKind == model::RowKind::cue && ! row.derived;
        }

        /*  HOW A GROUP BEHAVES, AS SHAPES. Two questions and two marks,
            drawn only when the answer is not the default - a group that plays
            its members once, in order, is the ordinary case and says nothing.

                ↻   it loops, and the number of rounds beside it
                ∞   it loops for ever, which is what nought rounds means
                ⇄   its members are shuffled rather than played in order

            Shapes and not colours (§4.8), and every one of them is a word in
            the inspector as well. */
        juce::String behaviourOf (const model::Row& row)
        {
            juce::String marks;

            if (row.loops == "0")
                marks << juce::String (juce::CharPointer_UTF8 ("\xe2\x86\xbb\xe2\x80\x89\xe2\x88\x9e"));
            else if (! row.loops.empty() && row.loops != "1")
                marks << juce::String (juce::CharPointer_UTF8 ("\xe2\x86\xbb\xe2\x80\x89"))
                      << juce::String (row.loops);

            if (row.selection == "shuffle")
                marks << (marks.isEmpty() ? "" : "  ")
                      << juce::String (juce::CharPointer_UTF8 ("\xe2\x87\x84"));

            /*  ∥  a TIMELINE: its members start together, each after its own
                pre-wait, so their order on screen is not their order in time.
                Added when the author reordered one and read the result as a
                fault (2026-09-18); the word is in the inspector's `mode`. */
            if (row.mode == "timeline")
                marks << (marks.isEmpty() ? "" : "  ")
                      << juce::String (juce::CharPointer_UTF8 ("\xe2\x88\xa5"));

            /*  pads  a SAMPLER group (PRD §3.27): GO arms its members onto
                strips and a hand plays them, in any order, any number of
                times, so the order on screen is not an order at all. A WORD
                where the timeline has a shape, because there is no shape that
                already means "played from a surface" - and colour is never the
                one carrier (§4.8). */
            if (row.mode == "sampler")
                marks << (marks.isEmpty() ? "" : "  ") << "pads";

            return marks;
        }

        juce::String sectionWord (model::Section section)
        {
            switch (section)
            {
                case model::Section::persistent: return "persistent";
                case model::Section::header:     return "header";
                case model::Section::footer:     return "footer";
                case model::Section::member:     break;
            }

            return {};
        }
    }

    CueListComponent::CueListComponent (const model::Theme& themeToUse, Actions actionsToUse)
        : actions (std::move (actionsToUse)), theme (themeToUse)
    {
        list.setRowHeight (rowHeight());
        list.setWantsKeyboardFocus (false);     // the arrows are this component's
        list.getViewport()->setScrollBarsShown (true, false);
        addAndMakeVisible (list);

        /*  THE CELL EDITOR rides on the list's own scrolling surface, so a
            box opened over a row stays over it when the list scrolls. Enter
            commits, Esc cancels - both inside the box, so neither reaches the
            shell and Esc is never PANIC - and the focus leaving commits too,
            which is what a click elsewhere does. */
        editor.setSelectAllWhenFocused (true);
        editor.setMultiLine (false);
        editor.onReturnKey = [this] { commitEdit(); };
        editor.onEscapeKey = [this] { cancelEdit(); };
        /*  A FOCUS LOSS THAT ARRIVES LATE IS NOT ONE. The editor's callback is
            posted, not called: it lands on the next pass of the message
            loop. An arrow moving the edit hides the box (posting the loss),
            opens it again on the next row with the focus, and only then is
            the stale loss delivered - and it used to commit and shut the new
            box, which is why the arrows seemed to do nothing (traced
            2026-09-18). A loss reported while the box has the focus is
            that stale one, and is ignored. */
        editor.onFocusLost = [this]
        {
            if (editing() && ! editor.hasKeyboardFocus (true))
                commitEdit();
        };

        if (auto* surface = list.getViewport()->getViewedComponent())
            surface->addChildComponent (editor);

        setWantsKeyboardFocus (true);
        applyTheme (theme);
    }

    int CueListComponent::rowHeight() const noexcept
    {
        return juce::roundToInt (theme.row * theme.type);
    }

    /*  THE LEFT EDGE EVERY RAIL IS MEASURED FROM: past the padding, the park
        gutter and the number column, which is where a row's own indented part
        begins. Both painters removed exactly these three by hand and got the
        same answer; asking once is what stops the next edit to either from
        being the one that separates them. */
    int CueListComponent::railsOrigin() const noexcept
    {
        const auto unit = juce::roundToInt (theme.type * 7.0);

        return unit / 2 + unit * 2 + numberChars * unit;
    }

    /** Where the rail of the Nth enclosing container stands. Level 1 is the outermost. */
    int CueListComponent::railAt (int level) const noexcept
    {
        const auto indent = juce::roundToInt (theme.type * 7.0) * 2;

        return railsOrigin() + (level - 1) * indent + indent / 2;
    }

    /*  THE RAILS OF WHATEVER HOLDS THIS ROW, and it is asked of the row's
        DEPTH alone - so a cue, a group and a section's band all draw the same
        line in the same place, which is the whole of what makes the bracket
        read as one shape rather than as several that nearly line up. */
    void CueListComponent::paintRails (const model::Row& entry, int row, juce::Graphics& g,
                                       int height)
    {
        const auto indent = juce::roundToInt (theme.type * 7.0) * 2;
        const auto middle = height / 2;

        g.setColour (Look::colour (theme, "rule"));

        for (int level = 1; level <= entry.depth; ++level)
        {
            /*  Does this level's run end on this row? The walk lays a
                container's rows out contiguously, so the next row shallower
                than the level is where that level closes. */
            const auto next = static_cast<std::size_t> (row) + 1;
            const auto closes = next >= rows.size() || rows[next].depth < level;

            g.fillRect (railAt (level), 0, 1, closes ? middle : height);

            if (closes)
                g.fillRect (railAt (level), middle, indent / 2, 1);
        }
    }

    void CueListComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;

        list.setRowHeight (rowHeight());
        list.setColour (juce::ListBox::backgroundColourId, Look::colour (theme, "panel-cue"));
        list.setColour (juce::ListBox::outlineColourId, Look::colour (theme, "rule"));

        resized();
        repaint();
    }

    bool CueListComponent::isChosen (const std::string& id) const
    {
        return ! id.empty() && std::find (chosen.begin(), chosen.end(), id) != chosen.end();
    }

    //==========================================================================
    //  Editing in place
    void CueListComponent::setEditable (bool editableToUse)
    {
        editable = editableToUse;

        if (! editable && editing())
            cancelEdit();
    }

    CueListComponent::Cells CueListComponent::cellsFor (const model::Row& entry, int width, int height) const
    {
        /*  THE SAME CARVING THE PAINTER DOES, in the same order, so the box
            lands on the words: the gutter, the three times from the right,
            the kind, the number from the left, the depth's indent and the
            group's mark, and the name is what is left. */
        const auto unit = juce::roundToInt (theme.type * 7.0);
        const auto pad = unit / 2;
        const auto indent = unit * 2;

        auto area = juce::Rectangle<int> (0, 0, width, height).reduced (pad, 0);
        area.removeFromLeft (unit * 2);

        Cells cells;
        cells.postWait = area.removeFromRight (timeChars * unit);
        cells.duration = area.removeFromRight (timeChars * unit);
        cells.preWait = area.removeFromRight (timeChars * unit);
        area.removeFromRight (kindChars * unit);
        cells.number = area.removeFromLeft (numberChars * unit);
        area.removeFromLeft (entry.depth * indent);
        area.removeFromLeft (indent);
        cells.name = area;
        return cells;
    }

    juce::Rectangle<int> CueListComponent::rectOf (const Cells& cells, model::EditCell cell) const
    {
        switch (cell)
        {
            case model::EditCell::number:   return cells.number;
            case model::EditCell::name:     return cells.name;
            case model::EditCell::preWait:  return cells.preWait;
            case model::EditCell::duration: return cells.duration;
            case model::EditCell::postWait: return cells.postWait;
            case model::EditCell::none:     break;
        }

        return {};
    }

    model::EditCell CueListComponent::cellAt (const model::Row& entry, int x, int width, int height) const
    {
        const auto cells = cellsFor (entry, width, height);

        for (const auto cell : { model::EditCell::number, model::EditCell::name, model::EditCell::preWait,
                                 model::EditCell::duration, model::EditCell::postWait })
            if (rectOf (cells, cell).getHorizontalRange().contains (x))
                return cell;

        return model::EditCell::none;
    }

    void CueListComponent::listBoxItemDoubleClicked (int row, const juce::MouseEvent& event)
    {
        /*  The list box's own count, kept for the case where nothing moved
            between the clicks; `listBoxItemClicked` counts across a relayout. */
        if (! editable || row < 0 || row >= static_cast<int> (rows.size()))
            return;

        const auto& entry = rows[static_cast<std::size_t> (row)];

        if (entry.rowKind != model::RowKind::cue || entry.derived)
            return;

        //  The first click's cell when it was this cue's, for the same reason as above.
        const auto rowWidth = event.eventComponent != nullptr ? event.eventComponent->getWidth() : list.getWidth();
        const auto aimed = lastClickId == entry.id && lastClickCell != model::EditCell::none
                             ? lastClickCell
                             : cellAt (entry, event.x, rowWidth, rowHeight());

        openCell (row, aimed);
    }

    void CueListComponent::openCell (int row, model::EditCell cell)
    {
        if (row < 0 || row >= static_cast<int> (rows.size()) || cell == model::EditCell::none)
            return;

        const auto& entry = rows[static_cast<std::size_t> (row)];

        //  Already open on this very cell: a second count of the same clicks.
        if (editing() && editRow == row && editCell == cell)
            return;

        /*  A COLUMN THAT IS NOT THIS CUE'S TO WRITE says so rather than opening
            a box that would be refused: a media cue's duration is its file's. */
        if (model::editAttributeFor (cell, entry.kind).empty())
        {
            if (actions.say)
                actions.say (cell == model::EditCell::duration
                               ? (entry.kind == "media" ? juce::String ("a media cue's duration is its file's")
                                                        : juce::String ("a ") + entry.kind + " has no duration to set")
                               : juce::String ("not a value this cue has"));
            return;
        }

        beginEdit (row, cell);
    }

    void CueListComponent::beginEdit (int row, model::EditCell cell)
    {
        if (editing())
            commitEdit();

        const auto& entry = rows[static_cast<std::size_t> (row)];
        const auto attribute = model::editAttributeFor (cell, entry.kind);

        if (attribute.empty())
            return;

        editRow = row;
        editCell = cell;
        editId = entry.id;
        editAttribute = attribute;

        const auto text = cell == model::EditCell::number   ? entry.number
                        : cell == model::EditCell::name     ? entry.name
                        : cell == model::EditCell::preWait  ? entry.preWait
                        : cell == model::EditCell::duration ? entry.duration
                                                            : entry.postWait;

        editor.setFont (Look::font (theme, 13.0f));
        editor.setColour (juce::TextEditor::backgroundColourId, Look::colour (theme, "panel-in"));
        editor.setColour (juce::TextEditor::textColourId, Look::colour (theme, "ink"));
        editor.setColour (juce::TextEditor::outlineColourId, Look::colour (theme, "picked"));
        editor.setColour (juce::TextEditor::focusedOutlineColourId, Look::colour (theme, "picked"));
        editor.setJustification (cell == model::EditCell::name || cell == model::EditCell::number
                                   ? juce::Justification::centredLeft : juce::Justification::centredRight);
        editor.setText (juce::String (text), juce::dontSendNotification);

        placeEditor();
        editor.setVisible (true);
        editor.toFront (true);      // above the row components made after it
        editor.selectAll();

        if (actions.editingBegan)
            actions.editingBegan();
    }

    void CueListComponent::placeEditor()
    {
        if (! editing())
            return;

        //  Relative to the list's scrolling surface, which is where the box lives.
        const auto rowArea = list.getRowPosition (editRow, false);
        const auto cells = cellsFor (rows[static_cast<std::size_t> (editRow)], rowArea.getWidth(), rowArea.getHeight());
        editor.setBounds (rectOf (cells, editCell).translated (rowArea.getX(), rowArea.getY()).reduced (0, 1));
    }

    void CueListComponent::commitEdit()
    {
        if (! editing())
            return;

        const auto id = editId;
        const auto attribute = editAttribute;
        const auto text = editor.getText().toStdString();
        const auto row = editRow;

        editRow = -1;
        editCell = model::EditCell::none;
        editor.setVisible (false);

        /*  Written only when it changed: leaving a box as it was is not a
            decision, and a `node.set` that says nothing new is a record for
            the undo stack to hold for nobody. */
        const auto& entry = rows[static_cast<std::size_t> (juce::jlimit (0, static_cast<int> (rows.size()) - 1, row))];
        const auto was = attribute == "number"   ? entry.number
                       : attribute == "name"     ? entry.name
                       : attribute == "preWait"  ? entry.preWait
                       : attribute == "duration" ? entry.duration
                                                 : entry.postWait;

        if (text != was && actions.setValue)
            actions.setValue ("/godot/cue/" + id + "/" + attribute, text);

        grabKeyboardFocus();   // the arrows and Space are the shell's again
    }

    void CueListComponent::cancelEdit()
    {
        editRow = -1;
        editCell = model::EditCell::none;
        editor.setVisible (false);
        grabKeyboardFocus();
    }

    void CueListComponent::moveEdit (int rowStep, int cellStep)
    {
        if (! editing())
            return;

        auto row = editRow;
        auto cell = editCell;

        commitEdit();

        /*  ALONG THE ROW: the next cell this cue may write, skipping one it
            may not (a memo's duration). DOWN THE LIST: the next cue row,
            skipping bands and derived lines, in the same column. */
        static constexpr model::EditCell order[] { model::EditCell::number, model::EditCell::name,
                                                   model::EditCell::preWait, model::EditCell::duration,
                                                   model::EditCell::postWait };

        if (cellStep != 0)
        {
            auto at = 0;

            for (; at < 5; ++at)
                if (order[at] == cell)
                    break;

            const auto& entry = rows[static_cast<std::size_t> (row)];

            for (at += cellStep; at >= 0 && at < 5; at += cellStep)
            {
                if (! model::editAttributeFor (order[at], entry.kind).empty())
                {
                    beginEdit (row, order[at]);
                    return;
                }
            }

            return;
        }

        for (row += rowStep; row >= 0 && row < static_cast<int> (rows.size()); row += rowStep)
        {
            const auto& entry = rows[static_cast<std::size_t> (row)];

            if (entry.rowKind != model::RowKind::cue || entry.derived)
                continue;

            if (model::editAttributeFor (cell, entry.kind).empty())
                continue;

            list.scrollToEnsureRowIsOnscreen (row);
            beginEdit (row, cell);
            return;
        }
    }

    bool CueListComponent::CellEditor::keyPressed (const juce::KeyPress& key)
    {
        /*  THE ARROWS MOVE BETWEEN CELLS, committing as they go (author,
            2026-09-18: "arrows allow to navigate in neighbouring fields while
            validating any edits"); Tab and shift-Tab go along the row too. */
        if (key == juce::KeyPress (juce::KeyPress::upKey))    { owner.moveEdit (-1, 0); return true; }
        if (key == juce::KeyPress (juce::KeyPress::downKey))  { owner.moveEdit (+1, 0); return true; }
        if (key == juce::KeyPress (juce::KeyPress::leftKey))  { owner.moveEdit (0, -1); return true; }
        if (key == juce::KeyPress (juce::KeyPress::rightKey)) { owner.moveEdit (0, +1); return true; }
        if (key == juce::KeyPress (juce::KeyPress::tabKey))   { owner.moveEdit (0, +1); return true; }
        if (key == juce::KeyPress (juce::KeyPress::tabKey, juce::ModifierKeys::shiftModifier, 0))
                                                              { owner.moveEdit (0, -1); return true; }

        return juce::TextEditor::keyPressed (key);
    }

    void CueListComponent::show (const model::ShowModel& model, const std::string& standbyId,
                                 const std::vector<std::string>& chosenIds)
    {
        /*  THE STRUCTURE, at show-change rate: while the rows are the same
            objects, `updateContent` - which relays out every one of them - has
            nothing to do.

            KEYED ON THE WALK AND NOT ON THE REVISION, which is a fix rather
            than a tidy. `builtAt()` is the SHOW's revision, and a fold does not
            move it - nor should it, since collapsing a section is not a change
            to the show. So a folded section rebuilt the model and this view
            never noticed: the bands drew their twist the other way round and
            nothing else happened (author, 2026-09-18: "the containers for the
            groups, headers and footers don't collapse. They're always
            expanded").

            `rebuilds()` counts walks, so it moves for every reason the rows
            can change - an edit, a different list, a fold - and asks this view
            one question instead of three. */
        const auto structureMoved = model.rebuilds() != drawnWalk || model.list() != drawnList
                                 || stepsVersion != drawnSteps;

        if (structureMoved)
        {
            rows = model.rows();
            drawnWalk = model.rebuilds();
            drawnList = model.list();
            drawnSteps = stepsVersion;

            /*  THE STEPS GO UNDER THE AIMED CUE'S OWN ROW - after its members
                and bands when it is a group, so they read as what happened
                after the scene fired rather than as members of it. */
            if (! steps.empty() && ! stepsUnder.empty())
            {
                auto at = rows.size();

                for (std::size_t n = 0; n < rows.size(); ++n)
                    if (rows[n].rowKind == model::RowKind::cue && rows[n].id == stepsUnder
                         && ! rows[n].derived)
                    {
                        at = n + 1;

                        while (at < rows.size() && rows[at].depth > rows[n].depth
                                 && rows[at].rowKind != model::RowKind::step)
                            ++at;

                        break;
                    }

                if (at <= rows.size())
                    rows.insert (rows.begin() + static_cast<long> (at), steps.begin(), steps.end());
            }

            list.updateContent();

            /*  AND EVERY ROW IS ASKED TO PAINT, because `updateContent` alone
                repaints only the rows whose INDEX or selection changed - and
                a move keeps both: the same number of rows, at the same
                indices, with different cues in them. The author dragged a cue
                and saw nothing until a click on another row repainted that
                one (2026-09-18: "the display only updates after clicking on
                another cue"). A whole repaint here is at show-change rate,
                which is when somebody edited; the rule against `repaint()`
                is for the tick-rate branch below. */
            list.repaint();

            /*  A BOX OPEN OVER A ROW FOLLOWS ITS CUE through a rebuild, and
                shuts if the cue is gone. */
            if (editing())
            {
                const auto at = model.indexOf (editId);

                if (at < 0)
                    cancelEdit();
                else
                {
                    editRow = at;
                    placeEditor();
                }
            }
        }

        /*  AND THE POINTER, at tick rate: two rows change decoration and two
            rows are asked to paint. Not `repaint()`, which on a five-hundred
            row list at twenty-five passes a second is the whole budget spent
            on a triangle that moved. */
        if (standbyId != standby || structureMoved)
        {
            const auto wasAt = standbyRow;

            standby = standbyId;
            standbyRow = model.indexOf (standbyId);

            if (! structureMoved)
            {
                if (wasAt >= 0)        list.repaintRow (wasAt);
                if (standbyRow >= 0)   list.repaintRow (standbyRow);
            }

            /*  AND IT IS BROUGHT INTO VIEW, because a pointer that has moved
                somewhere the operator cannot see is worse than no pointer: the
                list scrolls itself only when the standby moves, never while
                somebody is reading. */
            if (standbyRow >= 0 && list.getHeight() > 0)
                list.scrollToEnsureRowIsOnscreen (standbyRow);
        }

        /*  AND WHICH CUE IS PICKED, which moves as often as somebody clicks and
            costs the same two rows. THE STANDBY AND THE SELECTION ARE DIFFERENT
            THINGS - where GO will act, against what the inspector is about -
            and §4.8 wants them told apart by more than a hue: one is a mark in
            the gutter and a bar down the left edge, the other is a wash across
            the row. */
        if (chosenIds != chosen)
        {
            /*  The rows that were picked and the rows that are, each asked
                to paint; a selection is a handful of rows and a shift-click
                over fifty is still fifty repaints at a hand's rate. */
            for (const auto& id : chosen)
                if (const auto at = model.indexOf (id); at >= 0 && ! structureMoved)
                    list.repaintRow (at);

            chosen = chosenIds;

            for (const auto& id : chosen)
                if (const auto at = model.indexOf (id); at >= 0 && ! structureMoved)
                    list.repaintRow (at);
        }
    }

    int CueListComponent::getNumRows()
    {
        return static_cast<int> (rows.size());
    }

    void CueListComponent::paintListBoxItem (int row, juce::Graphics& g,
                                             int width, int height, bool)
    {
        if (row < 0 || row >= static_cast<int> (rows.size()))
            return;

        const auto& entry = rows[static_cast<std::size_t> (row)];

        if (entry.rowKind == model::RowKind::step)
        {
            paintStep (entry, g, width, height);
            return;
        }

        if (entry.rowKind == model::RowKind::band)
        {
            paintBand (entry, row, g, width, height);

            //  A drop INTO the section lights its band, as a drop onto a group lights the group.
            if (row == dropRow && dropWouldLink)
            {
                const auto tone = Look::colour (theme, dropTone.c_str());

                g.setColour (tone.withAlpha (0.22f));
                g.fillRect (0, 0, width, height);
                g.setColour (tone);
                g.drawRect (0, 0, width, height, 1);
            }

            return;
        }

        const auto isStandby = row == standbyRow;
        const auto isPicked = isChosen (rows[static_cast<std::size_t> (row)].id);

        const auto ink = Look::colour (theme, entry.enabled ? "ink" : "ink-off");
        const auto faint = Look::colour (theme, "ink-faint");
        const auto standbyColour = Look::colour (theme, "standby");

        /*  A SECTION READS AS A BAND AND ANYTHING INSIDE SOMETHING READS AS
            RECESSED - the page's `.row[data-in]` ground, which is the other
            half of what makes a container legible there. Two different
            questions drawn two different ways: a header is not a member, and
            drawing them alike is what made the page's own header lines
            ambiguous until they got a frame. */
        /*  THREE GROUNDS AND NOT A STRIPE. A header, a footer or a
            persistent cue is a different KIND of row and says so in its own
            tone; anything inside a container is recessed; everything else is
            the panel.

            NOTHING ALTERNATES (author, 2026-09-18: "the lines for each group
            can stay the same colour and not alternating"). A zebra is a way of
            following a row across a wide table, and it was fighting the two
            distinctions above - which carry meaning, where the stripe carried
            only parity. */
        g.fillAll (Look::colour (theme, entry.section != model::Section::member
                                          ? "panel-section-cue" : "panel-cue"));

        if (isPicked)
        {
            g.setColour (Look::colour (theme, "picked").withAlpha (0.16f));
            g.fillRect (0, 0, width, height);
        }

        /*  THE DIFF, while the undo panel is up: a cue that reads differently
            from when it opened is washed and marked with a delta, one that
            was not there with a plus - a shape beside the colour (§4.8), in
            the gutter where nothing else is drawn but the park. */
        const auto changed = std::find (changedIds.begin(), changedIds.end(), entry.id) != changedIds.end();
        const auto added = ! changed && std::find (addedIds.begin(), addedIds.end(), entry.id) != addedIds.end();

        if (changed || added)
        {
            const auto mark = Look::colour (theme, added ? "live" : "waiting");
            g.setColour (mark.withAlpha (0.14f));
            g.fillRect (0, 0, width, height);
            g.setColour (mark);
            g.setFont (Look::font (theme, 13.0f));
            g.drawText (juce::String (juce::CharPointer_UTF8 (added ? "+" : "\xce\x94")),
                        juce::Rectangle<int> (juce::roundToInt (theme.type * 7.0) / 2, 0,
                                              juce::roundToInt (theme.type * 7.0) * 2, height),
                        juce::Justification::centred, false);
        }

        /*  WHERE A FILE WOULD LAND, SAID WHILE THE HAND IS STILL IN THE AIR.
            Two answers and two shapes: letting go ON a media cue names that
            cue's file, so the whole row lights; letting go on a member makes
            new cues after it, so a line is drawn under it. Guessing afterwards
            which of the two happened is the thing this is here to prevent.

            AND A THIRD CASE DRAWN AS NEITHER. A header, a footer, a persistent
            cue and a band take the files to the end of their container's
            members, because that is the only place a create can put them, so
            no line is drawn under a row the cue will not appear under. */
        if (row == dropRow)
        {
            /*  AND THE TONE SAYS WHICH OF THE FOUR (author, 2026-09-21): into a
                group, aimed at a fade, prepared by a group's header, or into a
                footer. The sentence under the list says the same thing in
                words, so the colour is the fast half of a pair and not the
                only carrier (§4.8). */
            const auto tone = Look::colour (theme, dropTone.c_str());

            g.setColour (tone);

            if (dropWouldLink)
            {
                g.setColour (tone.withAlpha (0.22f));
                g.fillRect (0, 0, width, height);
                g.setColour (tone);
                g.drawRect (0, 0, width, height, 1);
            }
            else if (dropWouldInsert)
            {
                g.fillRect (0, height - 2, width, 2);
            }
        }

        if (isStandby)
        {
            g.setColour (standbyColour.withAlpha (0.16f));
            g.fillRect (0, 0, width, height);
            g.setColour (standbyColour);
            g.fillRect (0, 0, 3, height);
        }

        const auto unit = juce::roundToInt (theme.type * 7.0);
        const auto pad = unit / 2;

        auto area = juce::Rectangle<int> (0, 0, width, height).reduced (pad, 0);

        /*  THE GUTTER, WHICH IS THE ONLY PLACE A CLICK PARKS (author,
            2026-09-18: "so far selection on the full line sets the stand-by and
            not the far left of each row"). The page has had it this way since
            Didi was drawn - `commands.json` says park is "a row's left edge" -
            because the rest of the row is going to mean SELECT the moment the
            inspector exists, and a gesture that has to be taken back from the
            whole row later is one people will have learned by then.

            The pointer's own mark lives here too, at the far left where the
            eye runs down looking for it, rather than beside the name where it
            moved with the indent. */
        const auto gutter = area.removeFromLeft (unit * 2);

        if (isStandby)
        {
            g.setColour (standbyColour);
            g.setFont (Look::font (theme, 13.0f));
            g.drawText (juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xb6")),
                        gutter, juce::Justification::centred, false);
        }

        //  The times, on the right, in the order somebody works in.
        g.setFont (Look::font (theme, 12.0f));

        for (const auto* when : { &entry.postWait, &entry.duration, &entry.preWait })
        {
            auto cell = area.removeFromRight (timeChars * unit);
            g.setColour (faint);
            g.drawText (*when, cell.reduced (pad / 2, 0), juce::Justification::centredRight, false);
        }

        auto kindCell = area.removeFromRight (kindChars * unit);
        g.setColour (faint);
        g.drawText (entry.isGroup && ! entry.mode.empty() ? entry.mode : entry.kind,
                    kindCell, juce::Justification::centredRight, true);

        //  The number, then the name, indented by how deep the cue sits.
        auto numberCell = area.removeFromLeft (numberChars * unit);
        g.setColour (isStandby ? standbyColour : faint);
        g.setFont (Look::font (theme, 12.0f));
        g.drawText (entry.number, numberCell, juce::Justification::centredLeft, false);

        /*  WHAT HOLDS WHAT, DRAWN AS THE PAGE DRAWS IT (author, 2026-09-18:
            "containers are not as clear as on the webview"). Indentation alone
            says a row is further right; it does not say what it is inside.

            THE FRAME IS A RAIL AND TWO CORNERS AND NEVER A BOX - styles.css's
            own words, and for the same reason here as there: this is one flat
            list of rows and there is nothing around a group to put a border
            on. So each contained row draws the same one-pixel rule down its
            left; the container's own row starts that rule under itself; and
            the last row inside turns it right and stops it. Rows of four kinds
            drawing one shape, which holds together only because all of them
            measure the rail from `railsOrigin()` rather than each from its own
            arithmetic. */
        const auto indent = unit * 2;
        const auto middle = height / 2;

        paintRails (entry, row, g, height);

        /*  And a container opens its children's rail under itself, so the eye
            can follow one line from the group to the last thing inside it. */
        if (entry.isGroup)
        {
            g.setColour (Look::colour (theme, "rule"));
            g.fillRect (railAt (entry.depth + 1), middle, 1, height - middle);
        }

        area.removeFromLeft (entry.depth * indent);

        /*  A GROUP SAYS SO WITH A SHAPE as well as with its mode in words and
            its rail (§4.8): three tellings, not one, and none of them colour. */
        auto markCell = area.removeFromLeft (indent);
        g.setColour (faint);
        g.setFont (Look::font (theme, twistHeight));
        /*  A GROUP'S TWIST POINTS DOWN WHEN IT IS OPEN AND RIGHT WHEN IT IS
            SHUT, which is the one convention a file tree has taught everybody
            already - and the same shape a section's band uses, so the two kinds
            of container fold the same way. */
        /*  CENTRED, so the tip of the triangle stands on the rail its
            children come down (author, 2026-09-18: "the expanded bracket could
            have the vertical line aligned with the tip of the triangle when
            it's pointing down"). The cell is exactly one indent wide and its
            centre IS `railAt (depth + 1)`, so centring the glyph is the whole
            of the alignment - no second arithmetic to keep in step. */
        g.drawText (entry.isGroup
                      ? juce::String (juce::CharPointer_UTF8 (entry.shut ? "\xe2\x96\xb8"
                                                                        : "\xe2\x96\xbe"))
                      : juce::String(),
                    markCell, juce::Justification::centred, false);

        /*  A GROUP'S NAME IS THE ONE AN EYE RUNS DOWN LOOKING FOR, so it is
            larger and brighter than its members' and carries its behaviour
            beside it as SHAPES (author, 2026-09-18: "we could have a style for
            group label so they are easy to read and they can have icons
            showing their behaviour, loop, sequential/random").

            THE MARKS ARE NOT THE ONLY TELLING (§4.8): the mode is already a
            word in the kind column, and every one of these is in the inspector
            in full. What they buy is a group whose behaviour can be read
            without picking it. */
        /*  A DERIVED HEADER LINE READS AS A READING: dimmed, italic, and the
            word "preset" after it rather than "header", because the cue is
            not in the header - the header will prepare it, which is what the
            page's italics say too. */
        g.setColour (entry.derived ? Look::colour (theme, "ink-dim") : ink);
        g.setFont (entry.derived ? Look::font (theme, 13.0f).italicised()
                                 : Look::font (theme, entry.isGroup ? 14.0f : 13.0f));

        const auto word = entry.derived ? juce::String ("preset") : sectionWord (entry.section);
        const auto name = entry.name.empty() ? juce::String ("(unnamed)") : juce::String (entry.name);

        g.drawText (word.isEmpty() ? name : name + "   " + word,
                    area, juce::Justification::centredLeft, true);

        if (entry.isGroup)
        {
            const auto marks = behaviourOf (entry);

            if (! marks.isEmpty())
            {
                const auto used = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(),
                                                                            name) + unit;

                g.setColour (faint);
                g.setFont (Look::font (theme, 12.0f));
                g.drawText (marks, area.withTrimmedLeft (juce::jmin (used, area.getWidth())),
                            juce::Justification::centredLeft, false);
            }
        }

        //  A rule under every row, as the page draws one.
        g.setColour (Look::colour (theme, "rule").withAlpha (0.5f));
        g.fillRect (0, height - 1, width, 1);
    }

    void CueListComponent::paintBand (const model::Row& entry, int row, juce::Graphics& g,
                                      int width, int height)
    {
        /*  A SECTION'S HEAD: the word, how many lines it holds, and a twist -
            three tellings, and not one of them a colour (§4.8). The page's
            band, in a list box rather than a stylesheet.

            THE FRAME IS ITS TOP EDGE, and the rule runs from the rail of
            whatever holds the section so that the two meet rather than nearly
            meet. A SECTION ADDS NO INDENT: its rows carry its own depth, not
            one more, because a footer is not further inside its group than the
            group's members are - so the band draws exactly the rails one of
            those rows draws, through itself, and has no line of its own.

            IT USED TO HAVE ONE, at its own depth times the indent, which is
            one half-indent right of where those rows put theirs: the bracket
            around a footer stood a few pixels off the rail it was supposed to
            continue, and did so at every depth but the outermost - where there
            is no rail to disagree with (author, 2026-09-18: "the expanded
            bracket is not always well aligned"). */
        const auto unit = juce::roundToInt (theme.type * 7.0);
        const auto pad = unit / 2;
        const auto indent = unit * 2;

        g.fillAll (Look::colour (theme, "panel-section"));

        auto area = juce::Rectangle<int> (0, 0, width, height).reduced (pad, 0);
        area.removeFromLeft (unit * 2 + numberChars * unit);

        paintRails (entry, row, g, height);

        /*  A RAIL AND A CORNER, which is the shape every container in this
            list is drawn with. The corner is where the section's own rail
            stands - the one its rows come down, since they sit one level in -
            so the top edge starts exactly there and the rail drops from it.

            FULL HEIGHT AND NOT FROM THE MIDDLE, which is what a group's row
            does: a group's members begin BELOW it, so its rail starts halfway
            down, but a band IS the top of its section and the frame has to
            read as starting at its own top edge. Half a rail and a nearly
            invisible fill is what made these disappear. */
        const auto corner = railAt (entry.depth + 1);

        g.setColour (Look::colour (theme, "rule"));
        g.fillRect (corner, 0, width - corner - pad, 1);

        if (! entry.shut)
            g.fillRect (corner, 0, 1, height);

        auto text = area.withTrimmedLeft (entry.depth * indent);

        /*  THE TWIST IS A SHAPE: pointing down when the section is open and
            right when it is shut, which is the one convention every file tree
            has taught everybody already. */
        /*  Centred on the rail, for the reason a cue row's twist is - and on
            its own patch of ground, so the rail it stands on runs behind it
            rather than through the glyph. */
        auto twist = text.removeFromLeft (indent);

        g.setColour (Look::colour (theme, "panel-section"));
        g.fillRect (twist.reduced (0, 1));

        g.setColour (Look::colour (theme, "ink-dim"));
        g.setFont (Look::font (theme, twistHeight));
        g.drawText (juce::String (juce::CharPointer_UTF8 (entry.shut ? "\xe2\x96\xb8" : "\xe2\x96\xbe")),
                    twist, juce::Justification::centred, false);

        text.removeFromLeft (pad);

        //  The word reads at a glance or the band is a stripe nobody can name.
        auto word = juce::String (entry.name).toUpperCase();
        g.setColour (Look::colour (theme, "ink"));
        g.setFont (Look::font (theme, 11.0f));
        const auto wordWidth = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), word) + pad;
        g.drawText (word, text.removeFromLeft (wordWidth), juce::Justification::centredLeft, false);

        /*  AND THE COUNT SAYS HOW MUCH: how many lines the section holds, or
            how many are hidden while it is shut. The dimmer grey, which is the
            only thing in a head that ranks the two. */
        g.setColour (Look::colour (theme, "ink-faint"));
        g.drawText (juce::String (static_cast<int> (entry.count))
                      + (entry.shut ? " hidden" : ""),
                    text, juce::Justification::centredLeft, false);

        juce::ignoreUnused (row);
    }

    void CueListComponent::listBoxItemClicked (int row, const juce::MouseEvent& event)
    {
        /*  A CLICK IN THE GUTTER PARKS THE POINTER, and only there. At M3 this
            is the only way into a list whose standby is clear - `standby.next`
            stays put from nowhere, which is the engine's decision and a
            reasonable one, since an arrow should not invent a starting point.

            THE REST OF THE ROW IS LEFT ALONE ON PURPOSE. It will mean SELECT
            when the inspector arrives, and a gesture taken back from the whole
            row later is one somebody will have learned by then; the page has
            parked from the left edge since Didi was drawn, and
            `commands.json` says so in as many words. */
        if (row < 0 || row >= static_cast<int> (rows.size()))
            return;

        const auto& entry = rows[static_cast<std::size_t> (row)];

        /*  A BAND IS ALL TARGET. The head is small and the twist smaller, and
            a word missed by a pixel should still open the section - which is
            the page's own reasoning about its `[data-fold]` head. */
        if (entry.rowKind == model::RowKind::band)
        {
            if (actions.fold)
                actions.fold (entry.bandKey);

            return;
        }

        /*  A STEP ROW RE-AIMS at its moment; the pointer's own row is where
            the aim already is and takes nothing. */
        if (entry.rowKind == model::RowKind::step)
        {
            if (! entry.pointer && actions.reaim)
                actions.reaim (entry.offset);

            return;
        }

        const auto unit = juce::roundToInt (theme.type * 7.0);

        /*  A GROUP'S TWIST FOLDS IT, and only the twist: the rest of the row
            picks like any other, because a group is a cue somebody can inspect
            as well as a container they can shut. */
        if (entry.isGroup)
        {
            const auto indent = unit * 2;
            const auto twistFrom = unit / 2 + unit * 2 + numberChars * unit + entry.depth * indent;

            if (event.x >= twistFrom && event.x < twistFrom + indent)
            {
                if (actions.fold)
                    actions.fold (entry.bandKey);

                return;
            }
        }

        /*  THE GUTTER PARKS AND THE ROW PICKS, which is the two meanings a
            click has to carry and the reason the whole row could not be one of
            them. The page splits them the same way, and it is why the row body
            was left inert until there was an inspector for it to speak to. */
        if (event.x > unit * 2 + unit / 2)
        {
            /*  WITH WHAT THE HAND HELD: shift extends from the anchor, ctrl/⌘
                toggles, and the selection model reads them (model/Selection.h).
                `isCommandDown` is ctrl here and ⌘ on the Mac, which is the
                key each platform's lists use. */
            /*  THE DOUBLE-CLICK IS COUNTED HERE, NOT BY THE ROW: the first click
                on an unpicked cue opens the inspector, which narrows the list
                and relays its rows out between the two clicks, so the second
                lands on a fresh row component and the list box's own count
                starts again (author, 2026-09-18: "difficult to edit other
                cues in the cue list than the first one" - the first was
                already picked, and nothing moved). Two plain clicks on the
                same cell inside the system's double-click time open it. */
            /*  AND THE CELL IS THE FIRST CLICK'S. The inspector opening on that
                click narrows the list, and the columns - carved from the right
                - shift under a pointer that has not moved; the second click
                then reads as another column, or none. What the hand aimed at
                is what it aimed at first. */
            /*  The row component's own width, not the list's: the list is a
                scrollbar wider, and the cells are carved from the right. */
            const auto rowWidth = event.eventComponent != nullptr ? event.eventComponent->getWidth() : list.getWidth();
            const auto now = juce::Time::getMillisecondCounter();
            const auto cell = cellAt (entry, event.x, rowWidth, rowHeight());
            const auto plain = ! event.mods.isShiftDown() && ! event.mods.isCommandDown();
            const auto second = plain && lastClickId == entry.id
                             && now - lastClickAt <= static_cast<juce::uint32> (juce::MouseEvent::getDoubleClickTimeout());
            const auto aimed = second ? lastClickCell : cell;

            lastClickId = entry.id;
            lastClickCell = aimed;
            lastClickAt = now;

            if (second && editable && ! entry.derived && aimed != model::EditCell::none)
            {
                lastClickAt = 0;    // a third click is a first again
                openCell (row, aimed);
                return;
            }

            if (actions.pick)
                actions.pick (entry.id, event.mods.isShiftDown(), event.mods.isCommandDown());

            return;
        }

        /*  AND A ROW THAT CANNOT TAKE THE POINTER SAYS SO rather than being
            sent and refused. The engine answers `standby.set` on a header, a
            footer or a persistent cue with `not-a-stop`, which is right -
            those run with their group or from the top of the show, and none is
            a place anybody waits. What was wrong was this client offering the
            gesture anyway: the first thing the author did with the cue list
            was click two such rows and get `error: 5411 26 window not-a-stop
            standby.set` where an answer should have been.

            A CLIENT THAT KNOWS THE RULE ASKS IT FIRST. `Row::mayPark` is that
            rule, in the model where a test can reach it, and the sentence
            below says which of the three reasons applies - because "nothing
            happened" and "this is not that kind of row" look identical from a
            chair. */
        if (! entry.mayPark())
        {
            if (actions.say)
            {
                const auto why = entry.section == model::Section::persistent
                                   ? "runs from the moment the show starts"
                                   : entry.section == model::Section::header
                                       ? "runs before its group, with it"
                                       : "runs after its group, with it";

                actions.say (juce::String (entry.name.empty() ? entry.id : entry.name)
                               + " " + why + ", so the pointer cannot stand there");
            }

            return;
        }

        if (actions.park)
            actions.park (entry.id);
    }

    //==========================================================================
    /*  MEDIA ARRIVING FROM OUTSIDE THE WINDOW, which is decision Y and the one
        thing the page cannot be given later: a browser is handed a dropped
        file's NAME and BYTES and never its path, so it can only offer to copy
        one in, while this is handed the path and can do either.

        TWO GESTURES, TOLD APART BY WHAT IS UNDER THE POINTER. On a media cue,
        letting go NAMES that cue's file - which is what somebody means when
        they drag a replacement onto a cue that already has one. Anywhere else,
        it MAKES cues, one per file, after whatever row the hand was over. The
        difference is drawn while the drag is in the air rather than explained
        afterwards. */
    int CueListComponent::rowUnder (int y) const
    {
        const auto inList = y - list.getY() + list.getViewport()->getViewPositionY();
        const auto at = inList / juce::jmax (1, rowHeight());

        return at >= 0 && at < static_cast<int> (rows.size()) ? at : -1;
    }

    bool CueListComponent::isInterestedInFileDrag (const juce::StringArray& files)
    {
        /*  ASKED OF THE SAME FORMAT READERS THE ENGINE USES, so the window
            cannot come to accept a file the show would then fail on: both
            sides are `registerBasicFormats`, and a format added to one is
            added to the other. */
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        for (const auto& path : files)
            if (formats.findFormatForFileExtension (juce::File (path).getFileExtension()) != nullptr)
                return true;

        return false;
    }

    void CueListComponent::fileDragEnter (const juce::StringArray& files, int x, int y)
    {
        fileDragMove (files, x, y);
    }

    void CueListComponent::fileDragMove (const juce::StringArray& files, int, int y)
    {
        const auto was = dropRow;
        const auto wasLink = dropWouldLink;

        dropRow = rowUnder (y);
        dropWouldLink = false;
        dropWouldInsert = false;
        dropTone = "drop-into";

        if (dropRow >= 0)
        {
            const auto& entry = rows[static_cast<std::size_t> (dropRow)];

            /*  ONE FILE ONTO ONE MEDIA CUE NAMES IT. More than one could not,
                and a cue of another kind has no file to name. */
            dropWouldLink = files.size() == 1
                         && entry.rowKind == model::RowKind::cue
                         && entry.kind == "media";

            //  Drawn only where letting go really does insert there.
            dropWouldInsert = ! dropWouldLink && insertAfter (entry);
        }

        if (dropRow != was || dropWouldLink != wasLink)
        {
            if (was >= 0)      list.repaintRow (was);
            if (dropRow >= 0)  list.repaintRow (dropRow);
        }
    }

    void CueListComponent::fileDragExit (const juce::StringArray&)
    {
        const auto was = dropRow;

        dropRow = -1;
        dropWouldLink = false;
        dropWouldInsert = false;
        dropTone = "drop-into";

        if (was >= 0)
            list.repaintRow (was);
    }

    void CueListComponent::filesDropped (const juce::StringArray& files, int, int y)
    {
        const auto at = rowUnder (y);

        dropRow = -1;
        dropWouldLink = false;
        dropWouldInsert = false;
        dropTone = "drop-into";
        repaint();

        if (files.isEmpty())
            return;

        if (at >= 0)
        {
            const auto& entry = rows[static_cast<std::size_t> (at)];

            if (files.size() == 1 && entry.rowKind == model::RowKind::cue
                  && entry.kind == "media")
            {
                if (actions.linkMedia)
                    actions.linkMedia (entry.id, files[0]);

                return;
            }

            /*  AFTER THE ROW THE HAND WAS OVER, in that row's own container,
                by its MEMBER index - which is the only index a create speaks
                in, and the reason the other rows cannot be pointed at.

                A HEADER, A FOOTER, A PERSISTENT CUE AND A BAND ALL ANSWER THE
                END INSTEAD. `cue.create` puts a cue among its parent's
                MEMBERS; the header and footer of a group are separate orders
                that a role decides, not positions an index can reach. So a
                drop on one of those rows says which container was meant and
                nothing about where, and the end of its members is the honest
                reading of that. The line under the row is not drawn for them,
                so nothing is promised that will not happen. */
            if (actions.importMedia)
                actions.importMedia (entry.parent, insertAfter (entry) ? entry.indexInParent + 1 : -1,
                                     files);

            return;
        }

        /*  AND NOTHING UNDER THE POINTER MEANS THE END OF THE LIST, which is
            where a drop into empty space obviously belongs. It says -1 rather
            than a row count standing in for the end: these rows are what is
            DRAWN - bands, and the members of every open group - so their
            number is not the list's member count and would name a position
            inside it. The window holds the tree and can name the end exactly;
            this does not have to guess. */
        if (actions.importMedia)
            actions.importMedia (drawnList, -1, files);
    }

    //==========================================================================
    /*  THE ROW DRAG (model/Reorder.h). The description is the cue's identifier
        and nothing else: a drag that carried a row index would name a row
        that may have moved by the time it lands, and a drag that carried a
        name would name two cues. */
    juce::var CueListComponent::getDragSourceDescription (const juce::SparseSet<int>& rowsToDescribe)
    {
        if (rowsToDescribe.isEmpty())
            return {};

        const auto at = rowsToDescribe[0];

        if (at < 0 || at >= static_cast<int> (rows.size()))
            return {};

        const auto& entry = rows[static_cast<std::size_t> (at)];

        //  Only a cue can be picked up. A band is a heading, not a thing.
        if (entry.rowKind != model::RowKind::cue || entry.id.empty())
            return {};

        /*  A DERIVED HEADER LINE IS THE MARK, NOT THE CUE: dragging it moves
            or clears the preset (model/Reorder.h), so it says so. */
        if (entry.derived)
            return juce::var (juce::String (presetLinePrefix) + juce::String (entry.id));

        return juce::var (juce::String (entry.id));
    }

    bool CueListComponent::isPresetLine (const SourceDetails& details)
    {
        return details.description.isString()
            && details.description.toString().startsWith (presetLinePrefix);
    }

    std::string CueListComponent::draggedIdOf (const SourceDetails& details)
    {
        const auto text = details.description.toString();
        return (isPresetLine (details) ? text.fromFirstOccurrenceOf (presetLinePrefix, false, false) : text)
                 .toStdString();
    }

    const model::Row* CueListComponent::rowById (const std::string& id) const
    {
        /*  THE CUE'S OWN ROW FIRST: a derived header line carries the same id
            and is drawn before it, and a drag or a drop reasoning from the
            line's place would get the cue's container wrong. The line itself
            answers only when the cue's own row is folded away. */
        for (const auto& row : rows)
            if (row.rowKind == model::RowKind::cue && row.id == id && ! row.derived)
                return &row;

        for (const auto& row : rows)
            if (row.rowKind == model::RowKind::cue && row.id == id)
                return &row;

        return nullptr;
    }

    bool CueListComponent::isInterestedInDragSource (const SourceDetails& details)
    {
        //  One of this list's own rows, and still one of them.
        return details.description.isString()
            && rowById (draggedIdOf (details)) != nullptr;
    }

    model::Drop CueListComponent::dropAt (const SourceDetails& details, int& rowOut) const
    {
        rowOut = rowUnder (details.localPosition.y);

        const auto draggedId = draggedIdOf (details);

        /*  A preset line dragged: the mark moves or goes, and letting go on
            no row at all is one of the answers. */
        if (isPresetLine (details))
        {
            const auto* own = rowById (draggedId);
            return model::presetLineDropFor (rowOut >= 0 ? &rows[static_cast<std::size_t> (rowOut)] : nullptr,
                                             draggedId, own != nullptr ? own->preset : std::string {}, rows);
        }

        const auto* dragged = rowById (draggedId);

        if (rowOut < 0 || dragged == nullptr)
            return {};

        /*  How far down the row the pointer is, which is what tells "on"
            from "after": the same arithmetic `rowUnder` uses, kept beside it. */
        const auto inList = details.localPosition.y - list.getY()
                              + list.getViewport()->getViewPositionY();
        const auto height = juce::jmax (1, rowHeight());
        const auto fraction = static_cast<double> (inList % height) / static_cast<double> (height);

        /*  ALT HELD MEANS THE PRESET, not a move (author, 2026-09-18: "drag
            and drop with alt onto a group label adds this cue to the header").
            Read from the keyboard's current state, since a drag carries no
            modifiers of its own. */
        const auto mods = juce::ModifierKeys::getCurrentModifiers();

        //  Shift with alt on a group title: into its footer (made first if it has none).
        if (mods.isAltDown() && mods.isShiftDown())
            return model::footerDropFor (rows[static_cast<std::size_t> (rowOut)], *dragged);

        if (mods.isAltDown())
            return model::presetDropFor (rows[static_cast<std::size_t> (rowOut)], *dragged, rows);

        if (rows[static_cast<std::size_t> (rowOut)].rowKind == model::RowKind::step)
            return {};

        return model::dropFor (rows[static_cast<std::size_t> (rowOut)], *dragged, fraction);
    }

    void CueListComponent::itemDragEnter (const SourceDetails& details)
    {
        /*  Thirty times a second, which is cheap - the body is a hit test
            over rows already in memory - and only for as long as a hand is
            over this list holding something. */
        lastMods = juce::ModifierKeys::getCurrentModifiers();
        startTimerHz (30);
        itemDragMove (details);
    }

    void CueListComponent::timerCallback()
    {
        const auto now = juce::ModifierKeys::getCurrentModifiers();

        if (now == lastMods)
            return;

        lastMods = now;

        if (lastDrag.sourceComponent != nullptr)
            itemDragMove (lastDrag);
    }

    void CueListComponent::itemDragMove (const SourceDetails& details)
    {
        const auto was = dropRow;
        const auto wasLink = dropWouldLink;
        const auto wasInsert = dropWouldInsert;
        const auto wasTone = dropTone;

        //  Kept so the timer can ask the same question again without the hand moving.
        lastDrag = details;

        /*  THE BANDS FIRST, because they change what is under the pointer:
            hovering a group's name grows its missing header and footer, and
            the hit test below has to see them. */
        {
            const auto over = rowUnder (details.localPosition.y);
            const auto* row = over >= 0 && static_cast<std::size_t> (over) < rows.size()
                                ? &rows[static_cast<std::size_t> (over)] : nullptr;

            /*  STICKY WHILE THE HAND IS ON ONE OF THE BANDS ITSELF, or they
                would vanish the moment somebody moved towards them. */
            const auto keep = row != nullptr && row->rowKind == model::RowKind::band
                                && row->parent == bandsFor;

            if (! keep)
                showEmptyBands (row != nullptr && row->isGroup ? row->id : std::string {});
        }

        auto at = -1;
        const auto drop = dropAt (details, at);

        /*  THE SAME TWO SHAPES A FILE GETS: a line under the row for "after",
            the whole row lit for "on" - into a group, or aimed at a fade. */
        dropRow = drop.kind == model::DropKind::none || drop.kind == model::DropKind::clearPreset ? -1 : at;
        dropWouldInsert = drop.kind == model::DropKind::after;
        dropWouldLink = drop.kind == model::DropKind::into || drop.kind == model::DropKind::target
                     || drop.kind == model::DropKind::preset || drop.kind == model::DropKind::footer
                     || drop.kind == model::DropKind::header;
        dropTone = model::dropTone (drop.kind);

        //  Out of the header: said even over nothing, since that is where the hand is.
        if (drop.kind == model::DropKind::clearPreset && actions.say)
            actions.say (juce::String (model::describe (drop, model::Row {}, false)));

        if (dropRow != was || dropWouldLink != wasLink || dropWouldInsert != wasInsert
              || dropTone != wasTone)
        {
            if (was >= 0)      list.repaintRow (was);
            if (dropRow >= 0)  list.repaintRow (dropRow);

            if (actions.say && dropRow >= 0)
            {
                /*  Whether the cue would land in a TIMELINE group, whose order
                    on screen is not its order in time - said now, because the
                    author reordered one and read the result as a fault. */
                const auto* container = rowById (drop.container);
                const auto intoTimeline = container != nullptr && container->mode == "timeline";

                actions.say (juce::String (model::describe (drop, rows[static_cast<std::size_t> (at)],
                                                            intoTimeline)));
            }
        }
    }

    /*  THE BANDS A GROUP WOULD HAVE, shown while a drag is over its name.

        AN EMPTY SECTION DRAWS NO BAND (`ShowModel`: "an empty header is not a
        thing an operator needs told about"), which is right when nobody is
        holding a cue and leaves nothing to aim at when somebody is. So the two
        lines appear for the group under the hand and go when the hand does.

        THE GROUP WHOSE TITLE ROW IS UNDER THE POINTER, which is what makes
        nesting answer itself: a title row belongs to exactly one group, so
        there is never a question of which group's header is meant. To reach an
        outer group's header from inside a nested one, hover the outer group's
        own name - the same gesture, one row up.

        AND THE ROWS BELOW THE TITLE MOVE, which is why it is the TITLE row
        rather than any row of the group: nothing above the insertion shifts,
        so the pointer stays on the thing it was pointing at. */
    void CueListComponent::showEmptyBands (const std::string& groupId)
    {
        if (groupId == bandsFor)
            return;

        bandsFor = groupId;

        //  Whatever was spliced in last time goes with the group it was for.
        rows.erase (std::remove_if (rows.begin(), rows.end(),
                                    [] (const model::Row& row)
                                    { return row.rowKind == model::RowKind::band && row.sectionId.empty()
                                              && row.count == 0; }),
                    rows.end());

        if (! bandsFor.empty())
        {
            const auto* group = rowById (bandsFor);

            if (group == nullptr || ! group->isGroup)
            {
                bandsFor.clear();
            }
            else
            {
                /*  ONLY THE SECTIONS THAT ARE NOT THERE. One that already has
                    cues in it has a band of its own, drawn by the model, and a
                    second would be two answers to one question. */
                const auto has = [this] (model::Section which)
                {
                    for (const auto& row : rows)
                        if (row.rowKind == model::RowKind::band && row.parent == bandsFor
                              && row.section == which)
                            return true;

                    return false;
                };

                const auto make = [this, group] (model::Section which, const char* word)
                {
                    model::Row band;
                    band.rowKind = model::RowKind::band;
                    band.section = which;
                    band.depth = group->depth + 1;
                    band.parent = bandsFor;
                    band.name = word;
                    band.count = 0;         //  what marks it as one of ours
                    band.bandKey = bandsFor + "/" + word;
                    return band;
                };

                auto at = std::find_if (rows.begin(), rows.end(),
                                        [this] (const model::Row& row) { return row.id == bandsFor; });

                if (at != rows.end())
                {
                    const auto depth = at->depth;
                    auto after = at + 1;

                    //  The footer goes past everything the group contains.
                    while (after != rows.end() && after->depth > depth)
                        ++after;

                    if (! has (model::Section::footer))
                        after = rows.insert (after, make (model::Section::footer, "Footer"));

                    if (! has (model::Section::header))
                    {
                        at = std::find_if (rows.begin(), rows.end(),
                                           [this] (const model::Row& row) { return row.id == bandsFor; });
                        rows.insert (at + 1, make (model::Section::header, "Header"));
                    }
                }
            }
        }

        list.updateContent();
        list.repaint();
    }

    void CueListComponent::itemDragExit (const SourceDetails&)
    {
        const auto was = dropRow;

        stopTimer();
        lastDrag = SourceDetails { juce::var(), nullptr, {} };
        showEmptyBands ({});

        dropRow = -1;
        dropWouldLink = false;
        dropWouldInsert = false;
        dropTone = "drop-into";

        if (was >= 0)
            list.repaintRow (was);

        if (actions.say)
            actions.say ({});
    }

    void CueListComponent::itemDropped (const SourceDetails& details)
    {
        auto at = -1;
        const auto drop = dropAt (details, at);
        const auto dragged = draggedIdOf (details);

        stopTimer();
        lastDrag = SourceDetails { juce::var(), nullptr, {} };

        dropRow = -1;
        dropWouldLink = false;
        dropWouldInsert = false;
        dropTone = "drop-into";

        //  The bands were the hand's, and the hand has gone.
        showEmptyBands ({});

        repaint();

        if (actions.say)
            actions.say ({});

        switch (drop.kind)
        {
            case model::DropKind::none:
                return;

            case model::DropKind::after:
            case model::DropKind::into:
                if (actions.move)
                    actions.move (dragged, drop.container, drop.index);
                return;

            case model::DropKind::target:
                if (actions.setTarget)
                    actions.setTarget (drop.cueId, dragged);
                return;

            case model::DropKind::preset:
                if (actions.setPreset)
                    actions.setPreset (dragged, drop.cueId);
                return;

            case model::DropKind::footer:
                if (actions.moveToFooter)
                    actions.moveToFooter (dragged, drop.cueId);
                return;

            case model::DropKind::header:
                if (actions.moveToHeader)
                    actions.moveToHeader (dragged, drop.cueId);
                return;

            case model::DropKind::clearPreset:
                if (actions.setPreset)
                    actions.setPreset (dragged, {});
                return;
        }
    }

    void CueListComponent::backgroundClicked (const juce::MouseEvent&)
    {
        /*  EMPTY SPACE MEANS NOTHING IS PICKED (author, 2026-09-18: "clicking
            on empty space in the cuelist could also close the inspector"). The
            close box says the same thing at the other end of the window; this
            is the gesture somebody makes without thinking about it, which is
            the one worth having. */
        if (actions.pick)
            actions.pick ({}, false, false);
    }

    int CueListComponent::headingHeight() const noexcept
    {
        return juce::roundToInt (rowHeight() * 0.7);
    }

    void CueListComponent::paintHeadings (juce::Graphics& g, juce::Rectangle<int> area)
    {
        const auto unit = juce::roundToInt (theme.type * 7.0);
        const auto pad = unit / 2;

        g.setColour (Look::colour (theme, "panel-high"));
        g.fillRect (area);

        g.setColour (Look::colour (theme, "rule"));
        g.fillRect (area.getX(), area.getBottom() - 1, area.getWidth(), 1);

        auto row = area.reduced (pad, 0);
        row.removeFromLeft (unit * 2);        // the park gutter, which has no label

        g.setColour (Look::colour (theme, "ink-off"));
        g.setFont (Look::font (theme, 10.0f));

        /*  THE SAME THREE WIDTHS AND THE SAME ORDER the rows take them off in,
            which is what keeps a label over its own column: post, duration,
            pre from the right, then the kind. */
        /*  FITTED AND NOT CLIPPED. A heading has to fit its column or it is
            not a heading: "DURATION" came out as "DURATIO" at the width the
            time columns are, and a word with its last letter missing reads as
            a fault rather than as a label (author's screenshot, 2026-09-18).
            Squeezed rather than shortened, because the author asked for these
            words and the abbreviation would be mine. */
        for (const auto* label : { "POST", "DURATION", "PRE" })
        {
            auto cell = row.removeFromRight (timeChars * unit).reduced (pad / 2, 0);
            g.drawFittedText (label, cell, juce::Justification::centredRight, 1, 0.6f);
        }

        g.drawFittedText ("KIND", row.removeFromRight (kindChars * unit),
                          juce::Justification::centredRight, 1, 0.6f);

        g.drawFittedText ("CUE", row.removeFromLeft (numberChars * unit),
                          juce::Justification::centredLeft, 1, 0.6f);
    }

    void CueListComponent::setSteps (const std::string& underCue, std::vector<model::Row> stepsToShow)
    {
        const auto same = underCue == stepsUnder && stepsToShow.size() == steps.size()
                       && std::equal (stepsToShow.begin(), stepsToShow.end(), steps.begin(),
                                      [] (const model::Row& a, const model::Row& b)
                                      {
                                          return a.id == b.id && a.pointer == b.pointer
                                              && a.undone == b.undone && a.name == b.name
                                              && std::abs (a.offset - b.offset) < 1.0e-9;
                                      });

        if (same)
            return;

        stepsUnder = underCue;
        steps = std::move (stepsToShow);
        ++stepsVersion;
    }

    void CueListComponent::setDiff (std::vector<std::string> changed, std::vector<std::string> added)
    {
        if (changed == changedIds && added == addedIds)
            return;

        changedIds = std::move (changed);
        addedIds = std::move (added);
        list.repaint();
    }

    void CueListComponent::paintStep (const model::Row& entry, juce::Graphics& g,
                                      int width, int height)
    {
        /*  A READING UNDER A CUE: recessed like anything inside something,
            a step's offset into the cue where a number would be, then its
            name and how it was fired; the pointer in the picked colour with a
            line across, since it is the one row that is a position and not
            an event. Past the instant, a step is dim: a load would take it
            back. */
        const auto unit = juce::roundToInt (theme.type * 7.0);
        const auto indent = unit * 2;
        const auto picked = Look::colour (theme, "picked");

        g.fillAll (Look::colour (theme, "panel-in"));

        auto area = juce::Rectangle<int> (0, 0, width, height).reduced (unit / 2, 0);
        area.removeFromLeft (unit * 2 + entry.depth * indent);

        if (entry.pointer)
        {
            g.setColour (picked);
            g.fillRect (area.getX(), height / 2 - 1, area.getWidth(), 2);
            g.setFont (Look::font (theme, 12.0f));

            const auto words = juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xb6 ")) + "aim  "
                                 + juce::String (entry.name);
            const auto box = area.removeFromLeft (juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), words) + unit);
            g.setColour (Look::colour (theme, "panel-in"));
            g.fillRect (box);
            g.setColour (picked);
            g.drawText (words, box, juce::Justification::centredLeft, false);
            return;
        }

        const auto ink = Look::colour (theme, entry.undone ? "ink-off" : "ink-dim");
        const auto faint = Look::colour (theme, entry.undone ? "ink-off" : "ink-faint");

        g.setColour (faint);
        g.setFont (Look::font (theme, 12.0f));
        g.drawText (juce::String (juce::CharPointer_UTF8 ("\xe2\x86\xb3 ")) + juce::String (model::offsetText (entry.offset)),
                    area.removeFromLeft (unit * 9), juce::Justification::centredLeft, false);

        g.setColour (faint);
        g.drawText (juce::String (entry.number) + (entry.undone ? "  (a load undoes this)" : ""),
                    area.removeFromRight (unit * 16), juce::Justification::centredRight, false);

        g.setColour (ink);
        g.setFont (Look::font (theme, 13.0f).italicised());
        g.drawText (juce::String (entry.name), area, juce::Justification::centredLeft, true);

        g.setColour (Look::colour (theme, "rule").withAlpha (0.3f));
        g.fillRect (0, height - 1, width, 1);
    }

    void CueListComponent::paint (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "panel-cue"));
        paintHeadings (g, getLocalBounds().removeFromTop (headingHeight()));
    }

    void CueListComponent::resized()
    {
        auto area = getLocalBounds();
        area.removeFromTop (headingHeight());   // the labels, which never scroll
        list.setBounds (area);
        placeEditor();
    }

    bool CueListComponent::keyPressed (const juce::KeyPress& key)
    {
        /*  THE ARROWS MOVE THE POINTER, not a selection: this list has no
            selection yet (M5), and the thing the author needs to judge at M3
            is whether the standby is unmistakable - which cannot be judged
            without moving it. Down is next, up is previous, exactly as the
            page binds them and as gestures/commands.json records. */
        /*  CTRL/⌘-ARROWS MOVE THE PRESET, plain arrows the pointer (author,
            2026-09-18: "ctrl+upArrow and downArrow, since this way we can
            move the preload/preset up or down nested groups"). Asked before
            the plain arrows, since a plain KeyPress compare ignores nothing. */
        if (key == juce::KeyPress (juce::KeyPress::upKey, juce::ModifierKeys::commandModifier, 0))
        {
            if (actions.presetStep) actions.presetStep (+1);
            return true;
        }

        if (key == juce::KeyPress (juce::KeyPress::downKey, juce::ModifierKeys::commandModifier, 0))
        {
            if (actions.presetStep) actions.presetStep (-1);
            return true;
        }

        if (key == juce::KeyPress (juce::KeyPress::downKey))
        {
            if (actions.standbyNext) actions.standbyNext();
            return true;
        }

        if (key == juce::KeyPress (juce::KeyPress::upKey))
        {
            if (actions.standbyPrevious) actions.standbyPrevious();
            return true;
        }

        if (key == juce::KeyPress (juce::KeyPress::spaceKey))
        {
            if (actions.go) actions.go();
            return true;
        }

        /*  CTRL/⌘-BACKSPACE DELETES THE PICKED CUE (author, 2026-09-18). With
            the modifier, so a Backspace meant for a field that has just lost
            the focus does not take a cue with it; and only when something is
            picked, since a delete aimed at nothing is nothing. Undo is one
            keystroke, so it does not ask. */
        if (key == juce::KeyPress (juce::KeyPress::backspaceKey,
                                   juce::ModifierKeys::commandModifier, 0))
        {
            if (! chosen.empty() && actions.removeChosen)
                actions.removeChosen();

            return true;
        }

        //  Ctrl/⌘-A picks every cue the list draws.
        if (key == juce::KeyPress ('a', juce::ModifierKeys::commandModifier, 0))
        {
            if (actions.pickAll)
                actions.pickAll();

            return true;
        }

        return false;
    }
}
