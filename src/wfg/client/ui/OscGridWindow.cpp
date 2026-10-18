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

#include <wfg/client/ui/OscGridWindow.h>

#include <wfg/client/ui/Look.h>

#include <algorithm>
#include <utility>

namespace wfg::client::ui
{
    namespace
    {
        //  Where a cell is: its table, its row and its column.
        struct CellAt
        {
            std::size_t table = 0, row = 0, column = 0;
            bool found = false;
        };

        /*  THE BOX A CELL IS TYPED IN: Enter writes, Esc takes it back - inside
            the box, so the window's Esc, which is PANIC, never hears it. */
        class CellBox final : public juce::TextEditor
        {
        public:
            std::function<void()> onCommit, onCancel;

            bool keyPressed (const juce::KeyPress& key) override
            {
                if (key == juce::KeyPress (juce::KeyPress::escapeKey))
                {
                    if (onCancel)
                        onCancel();

                    return true;
                }

                if (key == juce::KeyPress (juce::KeyPress::returnKey))
                {
                    if (onCommit)
                        onCommit();

                    return true;
                }

                return juce::TextEditor::keyPressed (key);
            }
        };
    }

    //==========================================================================
    class OscGridWindow::Content final : public juce::Component
    {
    public:
        Content (const model::Theme& themeToUse, Actions actionsToUse)
            : theme (themeToUse), actions (std::move (actionsToUse)), grid (*this)
        {
            status.setColour (juce::Label::textColourId, Look::colour (theme, "ink-dim"));
            status.setFont (Look::font (theme, 13.0f));
            addAndMakeVisible (status);

            viewport.setViewedComponent (&grid, false);
            viewport.setScrollBarsShown (true, true);
            addAndMakeVisible (viewport);

            box.setFont (Look::font (theme, 13.0f));
            box.setColour (juce::TextEditor::backgroundColourId, Look::colour (theme, "panel-high"));
            box.setColour (juce::TextEditor::textColourId, Look::colour (theme, "ink"));
            box.setColour (juce::TextEditor::outlineColourId, Look::colour (theme, "standby"));
            box.onCommit = [this] { commit(); };
            box.onCancel = [this] { closeBox(); };
            box.onFocusLost = [this] { commit(); };
        }

        void show (const model::OscMessagesReading& readingToUse, const std::string& nameToUse)
        {
            //  Nothing moved: nothing to do. A cell being typed in is never redrawn under the hand.
            if (same (readingToUse) && nameToUse == cueName)
                return;

            if (editing.found && readingToUse.cueId == reading.cueId)
                return;

            closeBox();
            reading = readingToUse;
            cueName = nameToUse;
            built = model::gridOf (reading);
            grid.relayout();
            sayStatus();
        }

        const std::string& cueId() const noexcept { return reading.cueId; }
        const model::OscGrid& gridShown() const noexcept { return built; }

        juce::Rectangle<int> cellBounds (std::size_t table, std::size_t row, std::size_t column) const
        {
            return grid.cellBounds (table, row, column);
        }

        bool typeInto (std::size_t table, std::size_t row, std::size_t column, const juce::String& text)
        {
            if (! openBox ({ table, row, column, true }))
                return false;

            box.setText (text, false);
            commit();
            return true;
        }

        juce::String statusText() const { return status.getText(); }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Look::colour (theme, "panel"));
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (8);
            status.setBounds (area.removeFromTop (juce::roundToInt (24.0 * theme.type)));
            area.removeFromTop (4);
            viewport.setBounds (area);
            grid.relayout();
        }

    private:
        //======================================================================
        class GridView final : public juce::Component
        {
        public:
            explicit GridView (Content& ownerToUse) : owner (ownerToUse) {}

            int labelWidth() const  { return juce::roundToInt (190.0 * owner.theme.type); }
            int cellWidth() const   { return juce::roundToInt (76.0 * owner.theme.type); }
            int rowHeight() const   { return juce::roundToInt (22.0 * owner.theme.type); }
            int titleHeight() const { return juce::roundToInt (28.0 * owner.theme.type); }

            /*  Each table: its title, its head of channels, its rows - one after
                another down the window, a gap between. */
            int tableTop (std::size_t table) const
            {
                int y = 0;

                for (std::size_t t = 0; t < table && t < owner.built.tables.size(); ++t)
                    y += titleHeight() + rowHeight() * (1 + static_cast<int> (owner.built.tables[t].parameters.size()))
                       + rowHeight();

                return y;
            }

            juce::Rectangle<int> cellBounds (std::size_t table, std::size_t row, std::size_t column) const
            {
                if (table >= owner.built.tables.size())
                    return {};

                const auto top = tableTop (table) + titleHeight() + rowHeight() * (1 + static_cast<int> (row));
                return { labelWidth() + cellWidth() * static_cast<int> (column), top, cellWidth(), rowHeight() };
            }

            CellAt cellAt (juce::Point<int> at) const
            {
                for (std::size_t t = 0; t < owner.built.tables.size(); ++t)
                {
                    const auto& table = owner.built.tables[t];

                    for (std::size_t r = 0; r < table.parameters.size(); ++r)
                        for (std::size_t c = 0; c < table.columns.size(); ++c)
                            if (cellBounds (t, r, c).contains (at))
                                return { t, r, c, true };
                }

                return {};
            }

            void relayout()
            {
                int widest = 0;

                for (const auto& table : owner.built.tables)
                    widest = std::max (widest, static_cast<int> (table.columns.size()));

                const auto width = std::max (owner.viewport.getMaximumVisibleWidth(),
                                             labelWidth() + cellWidth() * widest + 8);
                const auto height = std::max (rowHeight(), tableTop (owner.built.tables.size()));
                setSize (width, height);
                repaint();
            }

            void paint (juce::Graphics& g) override
            {
                const auto& look = owner.theme;
                g.fillAll (Look::colour (look, "panel-cue"));

                const auto font = Look::font (look, 13.0f);
                const auto rule = Look::colour (look, "rule");

                for (std::size_t t = 0; t < owner.built.tables.size(); ++t)
                {
                    const auto& table = owner.built.tables[t];
                    const auto top = tableTop (t);
                    const auto right = labelWidth() + cellWidth() * static_cast<int> (table.columns.size());

                    /*  ONLY WHAT IS TO BE PAINTED (namespace draft §88): a cue of
                        seventy inputs is a table of thousands of cells, and a
                        repaint of the corner a cell is typed in drew all of them. */
                    if (! g.clipRegionIntersects (juce::Rectangle<int> (0, top, getWidth(), tableTop (t + 1) - top)))
                        continue;

                    //  The family, as its addresses spell it.
                    g.setColour (Look::colour (look, "ink"));
                    g.setFont (font.boldened());
                    g.drawText (juce::String::fromUTF8 (table.family.c_str()),
                                juce::Rectangle<int> (6, top, getWidth() - 12, titleHeight()),
                                juce::Justification::centredLeft, true);

                    //  The channels this cue holds, and no others.
                    const auto headTop = top + titleHeight();
                    g.setColour (Look::colour (look, "panel-high"));
                    g.fillRect (0, headTop, right, rowHeight());
                    g.setFont (font);
                    g.setColour (Look::colour (look, "ink-dim"));

                    for (std::size_t c = 0; c < table.columns.size(); ++c)
                        g.drawText (juce::String::fromUTF8 (table.columns[c].c_str()),
                                    juce::Rectangle<int> (labelWidth() + cellWidth() * static_cast<int> (c), headTop,
                                                          cellWidth(), rowHeight()),
                                    juce::Justification::centred, true);

                    for (std::size_t r = 0; r < table.parameters.size(); ++r)
                    {
                        const auto y = headTop + rowHeight() * (1 + static_cast<int> (r));

                        if (! g.clipRegionIntersects (juce::Rectangle<int> (0, y, right, rowHeight())))
                            continue;

                        g.setColour (Look::colour (look, "ink-dim"));
                        g.drawText (juce::String::fromUTF8 (table.parameters[r].c_str()),
                                    juce::Rectangle<int> (6, y, labelWidth() - 10, rowHeight()),
                                    juce::Justification::centredLeft, true);

                        for (std::size_t c = 0; c < table.columns.size(); ++c)
                        {
                            const auto at = table.cells[r][c];
                            const auto cell = cellBounds (t, r, c);

                            if (at < 0 || static_cast<std::size_t> (at) >= owner.reading.rows.size())
                                continue;

                            g.setColour (Look::colour (look, "panel-inspect"));
                            g.fillRect (cell.reduced (1));
                            g.setColour (Look::colour (look, "ink"));
                            g.drawText (juce::String::fromUTF8 (model::cellText (owner.reading.rows[static_cast<std::size_t> (at)]).c_str()),
                                        cell.reduced (4, 0), juce::Justification::centred, true);
                        }

                        g.setColour (rule.withAlpha (0.5f));
                        g.drawHorizontalLine (y, 0.0f, static_cast<float> (right));
                    }

                    //  The column rules, so a value is read against its channel.
                    g.setColour (rule.withAlpha (0.5f));

                    for (std::size_t c = 0; c <= table.columns.size(); ++c)
                    {
                        const auto x = labelWidth() + cellWidth() * static_cast<int> (c);
                        g.drawVerticalLine (x, static_cast<float> (headTop),
                                            static_cast<float> (headTop + rowHeight() * (1 + static_cast<int> (table.parameters.size()))));
                    }
                }

                if (owner.built.empty())
                {
                    g.setColour (Look::colour (look, "ink-off"));
                    g.setFont (font);
                    g.drawText ("No message of this cue has a channel in its address.", getLocalBounds().reduced (8),
                                juce::Justification::topLeft, true);
                }
            }

            void mouseDoubleClick (const juce::MouseEvent& event) override
            {
                owner.openBox (cellAt (event.getPosition()));
            }

        private:
            Content& owner;
        };

        //======================================================================
        bool same (const model::OscMessagesReading& other) const
        {
            if (other.cueId != reading.cueId || other.locked != reading.locked || other.rows.size() != reading.rows.size())
                return false;

            for (std::size_t at = 0; at < other.rows.size(); ++at)
                if (other.rows[at].address != reading.rows[at].address || other.rows[at].value != reading.rows[at].value)
                    return false;

            return true;
        }

        void sayStatus()
        {
            juce::String words = juce::String::fromUTF8 (cueName.c_str());
            std::size_t cells = 0;

            for (const auto& table : built.tables)
                for (const auto& row : table.cells)
                    for (const auto at : row)
                        if (at >= 0)
                            ++cells;

            words << "  -  " << static_cast<int> (cells) << (cells == 1 ? " value" : " values");

            if (built.outside > 0)
                words << ", and " << static_cast<int> (built.outside)
                      << (built.outside == 1 ? " message" : " messages") << " with no channel, in the Messages panel";

            words << (reading.locked ? "  -  the show is locked: read only"
                                     : "  -  double-click a value to change it");
            status.setText (words, juce::dontSendNotification);
        }

        bool openBox (CellAt at)
        {
            if (! at.found || at.table >= built.tables.size())
                return false;

            const auto& table = built.tables[at.table];

            if (at.row >= table.cells.size() || at.column >= table.cells[at.row].size())
                return false;

            const auto message = table.cells[at.row][at.column];

            //  An empty cell stays empty: the grid adds nothing (the author: "Don't add columns").
            if (message < 0 || static_cast<std::size_t> (message) >= reading.rows.size())
                return false;

            if (reading.locked)
            {
                status.setText ("The show is locked: unlock it to change a value.", juce::dontSendNotification);
                return false;
            }

            closeBox();
            editing = at;
            editingRow = static_cast<std::size_t> (message);

            grid.addAndMakeVisible (box);
            box.setBounds (grid.cellBounds (at.table, at.row, at.column));
            box.setText (juce::String::fromUTF8 (model::cellText (reading.rows[editingRow]).c_str()), false);
            box.selectAll();
            box.grabKeyboardFocus();
            return true;
        }

        void commit()
        {
            if (! editing.found || editingRow >= reading.rows.size())
                return;

            const auto& row = reading.rows[editingRow];
            const auto typed = box.getText().toStdString();
            const auto unchanged = typed == model::cellText (row);
            const auto list = model::cellEdit (row, typed);

            if (unchanged)
            {
                closeBox();
                return;
            }

            if (! list.has_value())
            {
                status.setText ("\"" + box.getText() + "\" is not what " + juce::String::fromUTF8 (row.address.c_str())
                                  + " takes - its values' types are kept", juce::dontSendNotification);
                closeBox();
                return;
            }

            if (actions.set)
                actions.set (row.valueRow(), *list);

            closeBox();
        }

        void closeBox()
        {
            if (! editing.found)
                return;

            editing = {};
            box.onFocusLost = nullptr;
            grid.removeChildComponent (&box);
            box.onFocusLost = [this] { commit(); };
        }

        model::Theme theme;
        Actions actions;
        model::OscMessagesReading reading;
        std::string cueName;
        model::OscGrid built;

        juce::Label status;
        juce::Viewport viewport;
        GridView grid;
        CellBox box;
        CellAt editing;
        std::size_t editingRow = 0;
    };

    //==========================================================================
    OscGridWindow::OscGridWindow (const model::Theme& theme, Actions actions)
        : DocumentWindow ("Messages as a grid", Look::colour (theme, "ground"), juce::DocumentWindow::closeButton),
          panicAction (actions.panic)
    {
        content = std::make_unique<Content> (theme, std::move (actions));
        setUsingNativeTitleBar (true);
        setResizable (true, false);
        setResizeLimits (420, 240, 4000, 3000);
        setContentNonOwned (content.get(), true);
        centreWithSize (1000, 560);
    }

    OscGridWindow::~OscGridWindow()
    {
        clearContentComponent();
    }

    void OscGridWindow::show (const model::OscMessagesReading& reading, const std::string& cueName)
    {
        content->show (reading, cueName);
        setName ("Messages as a grid - " + juce::String::fromUTF8 (cueName.c_str()));
    }

    void OscGridWindow::open()
    {
        setVisible (true);
        toFront (true);
    }

    void OscGridWindow::closeButtonPressed()
    {
        setVisible (false);
    }

    bool OscGridWindow::keyPressed (const juce::KeyPress& key)
    {
        //  ESC IS PANIC HERE TOO (§4.4): a box being typed in has taken it before it gets here.
        if (key != juce::KeyPress (juce::KeyPress::escapeKey))
            return false;

        if (panicAction)
            panicAction();

        return true;
    }

    const std::string& OscGridWindow::cueId() const noexcept
    {
        return content->cueId();
    }

    const model::OscGrid& OscGridWindow::gridForTest() const
    {
        return content->gridShown();
    }

    juce::Rectangle<int> OscGridWindow::cellBoundsForTest (std::size_t table, std::size_t row, std::size_t column) const
    {
        return content->cellBounds (table, row, column);
    }

    bool OscGridWindow::typeIntoCellForTest (std::size_t table, std::size_t row, std::size_t column, const juce::String& text)
    {
        return content->typeInto (table, row, column, text);
    }

    juce::String OscGridWindow::statusForTest() const
    {
        return content->statusText();
    }
}
