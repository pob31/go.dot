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
*/

#include <wfg/client/model/OscMessages.h>
#include <wfg/client/model/Theme.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
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
        };

        OscMessagesComponent (const model::Theme&, Actions);
        ~OscMessagesComponent() override;

        void applyTheme (const model::Theme&);

        /** The reading for this pass. Cheap when nothing came or went. */
        void show (const model::OscMessagesReading&);

        void paint (juce::Graphics&) override;
        void resized() override;

        /*  For a test: the row of a message (its own first) and a value's
            controls in it, or nullptr. */
        juce::Label* addressBox (std::size_t row);
        juce::Label* valueBox (std::size_t row, std::size_t arg);
        juce::ComboBox* typeMenu (std::size_t row, std::size_t arg);
        juce::ToggleButton* curveSwitch (std::size_t row, std::size_t arg);
        juce::Button* removeValue (std::size_t row, std::size_t arg);
        juce::Button* addValue (std::size_t row);
        juce::Button* removeMessage (std::size_t row);
        juce::Button* addMessage() { return &add; }

    private:
        struct Cell;
        struct Row;

        void rebuild();
        void refresh();
        void layOut();
        std::string shapeOf (const model::OscMessagesReading&) const;
        void wire (Row&);
        void wireCell (Row&, Cell&, std::size_t arg);

        model::Theme theme;
        Actions actions;
        model::OscMessagesReading reading;
        std::string drawnShape;

        juce::Viewport viewport;
        juce::Component content;
        juce::TextButton add { "+ message" };
        std::vector<std::unique_ptr<Row>> rows;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OscMessagesComponent)
    };
}
