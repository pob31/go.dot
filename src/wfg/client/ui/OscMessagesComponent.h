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
    AN OSC CUE'S MESSAGES, AS A TABLE (namespace draft 45, O.5): one row a
    message - the cue's own first - its address, and each of its values as a
    type and a box, with a switch putting a curve on a number and a cross
    taking a value away; a plus adds a value, the row's cross takes the message
    away, and the head's plus adds one more message after the last.

    IT WRITES WHAT THE MODEL NAMED: `model/OscMessages` turns a typed value or
    a changed type into the message's `value` row, or refuses it; what is here
    is boxes, focus and turning a press into the `node.set` or the command.
    The table is rebuilt when a message, a value or a curve comes or goes, and
    only retyped when a value moves - never under a box somebody is typing in.

    ITS COLUMNS ARE DRAGGED WIDER OR NARROWER (namespace draft §98): the edge
    after the address in the head, and the edge after the values, which widens
    every value's box alike. A double click on an edge puts the column back to
    its own width. Kept on this computer, never in the show.

    A MESSAGE'S VALUES ONE UNDER ANOTHER (namespace draft §103): each value a
    line of its own, under the first, so the table is as wide as one value
    however many a message holds; and the numbers' column as wide as the
    cue's last number needs.
*/

#include <wfg/client/model/DeviceTree.h>
#include <wfg/client/model/OscMessages.h>
#include <wfg/client/model/Theme.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wfg::client::ui
{
    class OscMessagesComponent final : public juce::Component
    {
    public:
        struct Actions
        {
            /** One `node.set`: an address, or a message's whole value list. */
            std::function<void (const std::string& address, const std::string& text)> set;

            /** `node.setMany`: a value list and the curves whose values moved down one. */
            std::function<void (const std::vector<std::pair<std::string, std::string>>&)> setMany;

            /** `message.create` on the cue, with an address and a value list. */
            std::function<void (const std::string& cueId, const std::string& address,
                                const std::string& value)> createMessage;

            /** `message.promote`: the message made the cue's own, the first taken away. */
            std::function<void (const std::string& messageId)> promoteMessage;

            /** `curve.create` on one value of a message, under the cue or the message. */
            std::function<void (const std::string& parentId, int arg)> createCurve;

            /** `object.delete`: a message, or a curve. */
            std::function<void (const std::string& id)> removeObject;

            /** A sentence in the panel's head, or nothing to clear it. */
            std::function<void (const juce::String&)> say;

            /*  THE DEVICE'S TREE AS A MENU beside a row's address (namespace
                draft §56, AEP): the window builds it from the snapshot it holds
                and writes the pick to `addressRow`, under `under`. */
            std::function<void (const std::string& addressRow, const std::string& current,
                                juce::Component& under)> chooseAddress;

            /*  COLUMN WIDTHS KEPT ON THIS COMPUTER (namespace draft §98), in the
                units the type size scales: the width a column was last dragged
                to, or nothing; and a width just dragged to keep, nought to
                forget it. */
            std::function<std::optional<int> (const std::string& column)> widthOf;
            std::function<void (const std::string& column, int width)> keepWidth;
        };

        /*  The names its two columns are kept under. */
        static constexpr const char* addressColumn = "osc.address";
        static constexpr const char* valueColumn = "osc.value";

        OscMessagesComponent (const model::Theme&, Actions);
        ~OscMessagesComponent() override;

        void applyTheme (const model::Theme&);

        /*  The reading for this pass: nothing at all when it is the object shown
            last pass, cheap when nothing came or went. */
        void show (std::shared_ptr<const model::OscMessagesReading>);
        void show (const model::OscMessagesReading&);

        /*  For a test: how many rows have components now - those in view and a
            few either side, never the cue's every message (namespace draft §88). */
        std::size_t rowsMade() const noexcept { return rows.size(); }

        void paint (juce::Graphics&) override;
        void resized() override;

        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        /*  For a test: where the edge after the address (0) or after the values
            (1) is in the head, as the component sees it, or -1. */
        int columnEdge (std::size_t edge) const;

        /*  For a test: the row of a message (its own first) and a value's
            controls in it, or nullptr. */
        juce::Label* addressBox (std::size_t row);
        juce::Label* valueBox (std::size_t row, std::size_t arg);
        juce::ComboBox* typeMenu (std::size_t row, std::size_t arg);
        juce::ToggleButton* curveSwitch (std::size_t row, std::size_t arg);
        juce::Button* removeValue (std::size_t row, std::size_t arg);
        juce::Button* addValue (std::size_t row);
        juce::Button* removeMessage (std::size_t row);
        juce::Button* pickAddress (std::size_t row);

        /*  A DEVICE'S TREE AS ONE NESTED POPUP: a submenu per part that holds
            others, an item per node, `current` ticked. Each item's id is one
            more than its index in `addresses`, which this fills. */
        static juce::PopupMenu menuOf (const std::vector<model::TreeMenuItem>& items, const std::string& current,
                                       std::vector<std::string>& addresses);
        juce::Button* addMessage() { return &add; }

    private:
        struct Cell;
        struct Row;

        void rebuild();
        void refresh();
        void layOut();

        /*  The line each message starts on, a value a line, and one past the
            last: counted again when a value comes or goes. */
        void countLines();
        std::vector<std::size_t> starts { 0 };

        /*  The rows in view, and a few either side: [first, first + rows). */
        std::pair<std::size_t, std::size_t> window() const;
        Row* rowAt (std::size_t at) const;
        const model::OscMessagesReading& reading() const noexcept { return *shown; }
        std::string shapeOf (const model::OscMessagesReading&) const;
        void wire (Row&);
        void wireCell (Row&, Cell&, std::size_t arg);

        /*  The column widths, as drawn: the parts of a value scaled one by one,
            as the rows take them. */
        int cellPixels() const;
        int numberPixels = 0;  // the cue's last number and a little, as last laid out
        bool valued = false;   // some message has a value, so the values have an edge
        int edgeNear (int x) const;
        int headScroll() const;

        model::Theme theme;
        Actions actions;
        std::shared_ptr<const model::OscMessagesReading> shown = std::make_shared<const model::OscMessagesReading>();
        std::string drawnShape;

        /*  A VIEWPORT THAT SAYS WHEN IT SCROLLED, so the rows in view can be
            made for where it now looks. */
        struct Port final : juce::Viewport
        {
            std::function<void()> moved;
            void visibleAreaChanged (const juce::Rectangle<int>&) override { if (moved) moved(); }
        };

        /*  THE ROWS' SHEET, with a faint rule above each message, so the
            values under one read as its own. */
        struct Sheet final : juce::Component
        {
            std::function<void (juce::Graphics&)> painter;
            void paint (juce::Graphics& g) override { if (painter) painter (g); }
        };

        Port viewport;
        std::size_t first = 0;
        Sheet content;
        juce::TextButton add { "+ message" };
        std::vector<std::unique_ptr<Row>> rows;

        int addressBase = 0, valueBase = 0;

        /*  A drag on an edge: which (-1 none, 0 the address, 1 the values),
            where the hand took it and how wide the column was then. */
        int dragging = -1, dragFrom = 0, dragStartBase = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OscMessagesComponent)
    };
}
