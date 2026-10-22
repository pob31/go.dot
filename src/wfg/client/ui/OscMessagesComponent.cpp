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

#include <wfg/client/ui/OscMessagesComponent.h>

#include <wfg/client/ui/Look.h>

#include <algorithm>
#include <cstdlib>
#include <cstddef>
#include <string>
#include <utility>

namespace wfg::client::ui
{
    /*  One value of a message: its type, what it says, whether a curve moves
        it, and the cross that takes it away. */
    struct OscMessagesComponent::Cell
    {
        juce::ComboBox type;
        juce::Label value;
        juce::ToggleButton curve;
        juce::TextButton drop { "x" };
    };

    struct OscMessagesComponent::Row
    {
        std::size_t at = 0;
        model::OscMessageRow message;

        juce::Label number, address;
        juce::TextButton pick;
        std::vector<std::unique_ptr<Cell>> cells;
        juce::TextButton addArg { "+" }, drop { "x" };
    };

    namespace
    {
        constexpr int gap = 4;
        constexpr int numberWidth = 22, addressWidth = 210;
        constexpr int typeWidth = 70, valueWidth = 66, curveWidth = 26, dropWidth = 20;

        //  How narrow and how wide a hand may drag them (§98): an address keeps its arrow, a box a digit or two.
        constexpr int addressLeast = 80, addressMost = 800, valueLeast = 30, valueMost = 400;

        //  How near an edge the pointer must be to take it, either side.
        constexpr int edgeReach = 3;

        int scaled (int base, const model::Theme& theme)
        {
            return juce::roundToInt (static_cast<float> (base) * theme.type);
        }

        int kept (const OscMessagesComponent::Actions& actions, const char* column, int own, int least, int most)
        {
            if (actions.widthOf)
                if (const auto width = actions.widthOf (column); width.has_value())
                    return juce::jlimit (least, most, *width);

            return own;
        }

        /*  THE TYPE MENU'S ITEMS, numbered from one in `argumentTypes` order. */
        void fillTypes (juce::ComboBox& menu)
        {
            const auto& types = model::argumentTypes();

            for (std::size_t n = 0; n < types.size(); ++n)
                menu.addItem (juce::String (types[n].second), static_cast<int> (n) + 1);
        }

        int itemFor (char tag)
        {
            const auto& types = model::argumentTypes();

            for (std::size_t n = 0; n < types.size(); ++n)
                if (types[n].first == tag)
                    return static_cast<int> (n) + 1;

            return 0;
        }

        char tagFor (int item)
        {
            const auto& types = model::argumentTypes();
            const auto at = static_cast<std::size_t> (item - 1);
            return item >= 1 && at < types.size() ? types[at].first : '\0';
        }
    }

    OscMessagesComponent::OscMessagesComponent (const model::Theme& themeToUse, Actions actionsToUse)
        : theme (themeToUse), actions (std::move (actionsToUse))
    {
        addressBase = kept (actions, addressColumn, addressWidth, addressLeast, addressMost);
        valueBase = kept (actions, valueColumn, valueWidth, valueLeast, valueMost);

        content.painter = [this] (juce::Graphics& g)
        {
            const auto row = juce::roundToInt (theme.row * theme.type);
            g.setColour (Look::colour (theme, "rule").withAlpha (0.5f));

            for (const auto& line : rows)
                if (line->at > 0 && line->at < starts.size())
                    g.fillRect (0, static_cast<int> (starts[line->at]) * row, content.getWidth(), 1);
        };

        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, true);
        addAndMakeVisible (viewport);

        /*  THE ROWS IN VIEW, made again when it scrolls past them (namespace
            draft §88): only those have components. */
        viewport.moved = [this]
        {
            //  The head goes across with the rows, its words and edges with it.
            repaint (getLocalBounds().removeFromTop (juce::roundToInt (theme.row * theme.type)));

            if (window().first != first)
            {
                rebuild();
                return;
            }
        };

        /*  ONE MORE MESSAGE, after the last: to the address the last one is
            at, with its values, which is the commonest next message there is -
            the same kind of thing on the next channel, the next object. */
        add.setWantsKeyboardFocus (false);
        add.setTooltip ("Adds a message after the last, copying its address and its values");
        add.onClick = [this]
        {
            if (reading().rows.empty() || reading().rows.front().address.empty())
            {
                if (actions.say)
                    actions.say ("Give the cue an address first: it is the first message");

                return;
            }

            const auto& last = reading().rows.back();

            if (actions.createMessage)
                actions.createMessage (reading().cueId, last.address, last.value);
        };
        addAndMakeVisible (add);
    }

    OscMessagesComponent::~OscMessagesComponent() = default;

    void OscMessagesComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;
        viewport.setColour (juce::ScrollBar::thumbColourId, Look::colour (theme, "rule"));

        for (auto& row : rows)
        {
            row->number.setFont (Look::font (theme, 11.0f));
            row->number.setColour (juce::Label::textColourId, Look::colour (theme, "ink-dim"));

            for (auto* label : { &row->address })
            {
                label->setFont (Look::font (theme, 12.0f));
                label->setColour (juce::Label::textColourId, Look::colour (theme, "ink"));
                label->setColour (juce::Label::backgroundColourId, Look::colour (theme, "panel-in"));
            }

            for (auto& cell : row->cells)
            {
                cell->value.setFont (Look::font (theme, 12.0f));
                cell->value.setColour (juce::Label::textColourId, Look::colour (theme, "ink"));
                cell->value.setColour (juce::Label::backgroundColourId, Look::colour (theme, "panel-in"));
            }
        }

        repaint();
    }

    std::string OscMessagesComponent::shapeOf (const model::OscMessagesReading& r) const
    {
        /*  WHAT COMES AND GOES, and not what the values say: a message, a
            value, a type, a curve. A value retyped in a box is a refresh, so
            the box keeps its focus. */
        std::string out = r.cueId + "|" + (r.locked ? "locked|" : "");

        for (const auto& row : r.rows)
        {
            out += row.id + ":";

            for (const auto& arg : row.args)
                out += std::string (1, arg.tag) + arg.curveId + ",";

            out += row.spells ? ";" : "?;";
        }

        return out;
    }

    void OscMessagesComponent::show (const model::OscMessagesReading& readingToUse)
    {
        show (std::make_shared<const model::OscMessagesReading> (readingToUse));
    }

    void OscMessagesComponent::show (std::shared_ptr<const model::OscMessagesReading> readingToUse)
    {
        //  The same object as last pass: nothing came, went or moved.
        if (readingToUse == nullptr || readingToUse == shown)
            return;

        shown = std::move (readingToUse);

        const auto shape = shapeOf (reading());

        if (shape != drawnShape)
        {
            drawnShape = shape;
            countLines();
            rebuild();
            return;
        }

        refresh();
    }

    void OscMessagesComponent::wireCell (Row& row, Cell& cell, std::size_t arg)
    {
        auto* raw = &row;
        auto* box = &cell;

        fillTypes (cell.type);
        cell.type.setTooltip ("The type of this value");
        cell.type.onChange = [this, raw, box, arg]
        {
            const auto tag = tagFor (box->type.getSelectedId());
            const auto& now = raw->message.args[arg];

            if (tag == '\0' || tag == now.tag)
                return;

            /*  A CURVE MOVES A NUMBER, and a word has nowhere between two
                others to be: its curve comes off first. */
            if (! now.curveId.empty() && ! (tag == 'f' || tag == 'd' || tag == 'i' || tag == 'h'))
            {
                box->type.setSelectedId (itemFor (now.tag), juce::dontSendNotification);

                if (actions.say)
                    actions.say ("Take the curve off this value first: a curve moves a number");

                return;
            }

            if (const auto list = model::retyped (raw->message.value, arg, tag); list.has_value() && actions.set)
                actions.set (raw->message.valueRow(), *list);
        };

        cell.value.setEditable (true, true, false);
        cell.value.setTooltip ("What this value says");
        cell.value.onTextChange = [this, raw, box, arg]
        {
            const auto& now = raw->message.args[arg];
            const auto typed = box->value.getText().toStdString();
            const auto atom = model::atomFor (now.tag, typed);
            const auto list = atom.has_value() ? model::withArgument (raw->message.value, arg, *atom)
                                               : std::nullopt;

            /*  A BOX THAT WILL NOT PARSE IS PUT BACK, as a range's time is: a
                slip of the keyboard must not send a desk a nought. */
            if (! list.has_value())
            {
                box->value.setText (juce::String (now.payload), juce::dontSendNotification);

                if (actions.say)
                    actions.say ("\"" + juce::String (typed) + "\" is not a value of that type");

                return;
            }

            if (actions.set)
                actions.set (raw->message.valueRow(), *list);
        };

        cell.curve.setButtonText ("~");
        cell.curve.onClick = [this, raw, box, arg]
        {
            const auto& now = raw->message.args[arg];

            if (now.curveId.empty())
            {
                if (actions.createCurve)
                    actions.createCurve (raw->message.parentId (reading().cueId), static_cast<int> (arg));
            }
            else if (actions.removeObject)
            {
                actions.removeObject (now.curveId);
            }

            //  The table says what the document holds, once it holds it.
            box->curve.setToggleState (! now.curveId.empty(), juce::dontSendNotification);
        };

        cell.drop.setWantsKeyboardFocus (false);
        cell.drop.onClick = [this, raw, arg]
        {
            const auto& now = raw->message.args[arg];

            if (! now.curveId.empty())
            {
                if (actions.say)
                    actions.say ("Take the curve off this value first");

                return;
            }

            const auto list = model::withoutArgument (raw->message.value, arg);

            if (! list.has_value())
                return;

            /*  THE CURVES ON THE VALUES AFTER IT MOVE DOWN ONE with them, in
                the same set of writes: a curve rides its value, not its place. */
            std::vector<std::pair<std::string, std::string>> writes { { raw->message.valueRow(), *list } };

            for (std::size_t later = arg + 1; later < raw->message.args.size(); ++later)
                if (const auto& curve = raw->message.args[later].curveId; ! curve.empty())
                    writes.push_back ({ "/godot/curve/" + curve + "/arg", std::to_string (later - 1) });

            if (writes.size() == 1 && actions.set)
                actions.set (writes.front().first, writes.front().second);
            else if (actions.setMany)
                actions.setMany (writes);
        };

        content.addAndMakeVisible (cell.type);
        content.addAndMakeVisible (cell.value);
        content.addAndMakeVisible (cell.curve);
        content.addAndMakeVisible (cell.drop);
    }

    void OscMessagesComponent::wire (Row& row)
    {
        auto* raw = &row;

        row.number.setText (juce::String (static_cast<int> (row.at) + 1), juce::dontSendNotification);
        row.number.setJustificationType (juce::Justification::centredRight);
        content.addAndMakeVisible (row.number);

        row.address.setEditable (true, true, false);
        row.address.setTooltip (row.at == 0 ? "The address of the cue's own message - its first"
                                            : "The address this message writes, under the cue's device");
        row.address.onTextChange = [this, raw]
        {
            if (actions.set)
                actions.set (raw->message.addressRow(), raw->address.getText().toStdString());
        };
        content.addAndMakeVisible (row.address);

        /*  THE DEVICE'S TREE, part by part, as one nested menu (namespace draft
            §56, AEP): a pick writes the row's address as typing does. */
        row.pick.setButtonText (juce::String::fromUTF8 ("\xe2\x96\xbe"));
        row.pick.setWantsKeyboardFocus (false);
        row.pick.setTooltip ("Pick this message's address from the device's own list");
        row.pick.onClick = [this, raw]
        {
            if (actions.chooseAddress)
                actions.chooseAddress (raw->message.addressRow(), raw->message.address, raw->pick);
        };
        content.addAndMakeVisible (row.pick);

        for (std::size_t arg = 0; arg < row.message.args.size(); ++arg)
        {
            row.cells.push_back (std::make_unique<Cell>());
            wireCell (row, *row.cells.back(), arg);
        }

        /*  A VALUE MORE, a float at nought - the commonest next value there is. */
        row.addArg.setWantsKeyboardFocus (false);
        row.addArg.setTooltip ("Adds a value at the end of this message");
        row.addArg.onClick = [this, raw]
        {
            if (const auto list = model::withArgumentAppended (raw->message.value, "f:0"); list.has_value() && actions.set)
                actions.set (raw->message.valueRow(), *list);
        };
        content.addAndMakeVisible (row.addArg);

        /*  THE FIRST MESSAGE IS THE CUE'S OWN, and has no element to delete:
            taking it away makes the second the cue's own. Alone, it stays. */
        row.drop.setWantsKeyboardFocus (false);
        row.drop.onClick = [this, raw]
        {
            if (raw->at > 0)
            {
                if (actions.removeObject)
                    actions.removeObject (raw->message.id);

                return;
            }

            if (reading().rows.size() > 1 && actions.promoteMessage)
                actions.promoteMessage (reading().rows[1].id);
        };
        content.addAndMakeVisible (row.drop);
    }

    std::pair<std::size_t, std::size_t> OscMessagesComponent::window() const
    {
        /*  WHAT IS IN VIEW, AND EIGHT ROWS EITHER SIDE (namespace draft §88): a cue
            of 5,300 messages had nine components a message, all of them
            refreshed every pass and taken down one by one; now some forty rows
            have components, whatever the cue holds. Thirty-two at the least,
            so a short cue - or a panel not yet laid out - has all of its own. */
        constexpr std::size_t margin = 8, least = 32;

        const auto row = std::max (1, juce::roundToInt (theme.row * theme.type));
        const auto total = reading().rows.size();

        //  The message a line is one of: a message is as many lines as it has values (§103).
        const auto messageAt = [this, total] (std::size_t line)
        {
            const auto past = std::upper_bound (starts.begin(), starts.end(), line);
            const auto at = static_cast<std::size_t> (past - starts.begin());
            return std::min (total, at > 0 ? at - 1 : 0);
        };

        const auto topLine = static_cast<std::size_t> (std::max (0, viewport.getViewPositionY() / row));
        const auto seenLines = static_cast<std::size_t> (std::max (0, viewport.getHeight() / row)) + 1;
        const auto top = messageAt (topLine);
        const auto seen = messageAt (topLine + seenLines) - top + 1;
        const auto count = std::max (least, seen + margin * 2);

        auto from = top > margin ? top - margin : 0;

        if (from + count > total)
            from = total > count ? total - count : 0;

        return { from, std::min (count, total - std::min (total, from)) };
    }

    void OscMessagesComponent::countLines()
    {
        starts.assign (1, 0);
        starts.reserve (reading().rows.size() + 1);
        valued = false;

        for (const auto& message : reading().rows)
        {
            starts.push_back (starts.back() + std::max<std::size_t> (1, message.args.size()));
            valued = valued || ! message.args.empty();
        }
    }

    OscMessagesComponent::Row* OscMessagesComponent::rowAt (std::size_t at) const
    {
        if (at < first || at - first >= rows.size())
            return nullptr;

        return rows[at - first].get();
    }

    void OscMessagesComponent::rebuild()
    {
        /*  TAKEN OFF THE CONTENT BEFORE THEY GO, so no row's destructor looks
            for itself among the content's children. */
        content.removeAllChildren();
        rows.clear();

        const auto [from, count] = window();
        first = from;

        for (std::size_t at = first; at < first + count; ++at)
        {
            auto row = std::make_unique<Row>();
            row->at = at;
            row->message = reading().rows[at];
            wire (*row);
            rows.push_back (std::move (row));
        }

        applyTheme (theme);
        refresh();
        layOut();
    }

    void OscMessagesComponent::refresh()
    {
        const auto editable = ! reading().locked;

        add.setEnabled (editable);

        for (auto& pooled : rows)
        {
            auto& row = *pooled;
            const auto at = row.at;

            if (at >= reading().rows.size())
                continue;

            row.message = reading().rows[at];

            if (! row.address.isBeingEdited())
                row.address.setText (juce::String (row.message.address), juce::dontSendNotification);

            row.address.setEditable (editable, editable, false);
            row.pick.setEnabled (editable);
            row.addArg.setEnabled (editable && row.message.spells);
            row.drop.setEnabled (editable && (at > 0 || reading().rows.size() > 1));
            row.drop.setTooltip (at > 0 ? "Removes this message"
                                        : reading().rows.size() > 1 ? "Removes the first message: the second becomes the cue's own"
                                                                    : "A cue keeps its own message");

            for (std::size_t arg = 0; arg < row.cells.size() && arg < row.message.args.size(); ++arg)
            {
                auto& cell = *row.cells[arg];
                const auto& now = row.message.args[arg];

                cell.type.setSelectedId (itemFor (now.tag), juce::dontSendNotification);
                cell.type.setEnabled (editable);

                if (! cell.value.isBeingEdited())
                    cell.value.setText (juce::String (now.payload), juce::dontSendNotification);

                //  The four types that carry nothing have no box to type in.
                const auto carries = now.tag != 'T' && now.tag != 'F' && now.tag != 'N' && now.tag != 'I';
                cell.value.setEditable (editable && carries, editable && carries, false);
                cell.value.setAlpha (carries ? 1.0f : 0.4f);

                cell.curve.setToggleState (! now.curveId.empty(), juce::dontSendNotification);
                cell.curve.setEnabled (editable && now.number);
                cell.curve.setTooltip (! now.number ? "Only a number can follow a curve"
                                       : now.curveId.empty() ? "Puts a curve on this value, over the cue's own time"
                                                             : "Takes this value's curve off");

                cell.drop.setEnabled (editable && now.curveId.empty());
                cell.drop.setTooltip (now.curveId.empty() ? "Takes this value away"
                                                          : "Take the curve off this value first");
            }
        }

        repaint();
    }

    void OscMessagesComponent::resized()
    {
        layOut();
    }

    void OscMessagesComponent::layOut()
    {
        const auto row = juce::roundToInt (theme.row * theme.type);
        auto area = getLocalBounds();
        auto head = area.removeFromTop (row);

        add.setBounds (head.removeFromRight (scaled (90, theme)).reduced (1, 2));
        viewport.setBounds (area);

        /*  THE NUMBERS' COLUMN AS WIDE AS THE CUE'S LAST NUMBER (§103): a
            whole WFS-DIY dump is thousands of messages, and "6012" showed "60". */
        const auto digits = static_cast<int> (std::to_string (std::max<std::size_t> (1, reading().rows.size())).size());
        numberPixels = std::max (scaled (numberWidth, theme),
                                 juce::GlyphArrangement::getStringWidthInt (Look::font (theme, 11.0f),
                                                                            juce::String::repeatedString ("0", digits))
                                   + scaled (gap, theme) * 2);

        //  As wide as one value, however many a message has: they go one under another.
        const auto inner = std::max (viewport.getWidth() - scaled (14, theme),
                                     numberPixels + scaled (addressBase, theme)
                                       + scaled (gap, theme) * 3 + scaled (dropWidth, theme) * 2 + cellPixels());

        content.setSize (inner, std::max (row, static_cast<int> (starts.back()) * row));

        for (auto& line : rows)
        {
            //  Each row where its message sits among all of the cue's, a line a value.
            const auto y = static_cast<int> (starts[line->at]) * row;
            const auto lineAt = [y, row, inner] (std::size_t n)
            {
                return juce::Rectangle<int> (0, y + static_cast<int> (n) * row, inner, row);
            };

            auto strip = lineAt (0);
            const auto take = [this] (juce::Rectangle<int>& from, int pixels)
            {
                auto cut = from.removeFromLeft (pixels);
                from.removeFromLeft (scaled (gap, theme));
                return cut;
            };

            line->number.setBounds (take (strip, numberPixels));

            auto addressCell = take (strip, scaled (addressBase, theme));
            line->pick.setBounds (addressCell.removeFromRight (scaled (dropWidth, theme)).reduced (1, 2));
            line->address.setBounds (addressCell.reduced (0, 1));

            line->drop.setBounds (strip.removeFromRight (scaled (dropWidth, theme)).reduced (1, 2));

            //  The values one under another, each where the first is, the plus after the last.
            const auto valuesFrom = strip.getX();
            auto last = strip;

            for (std::size_t n = 0; n < line->cells.size(); ++n)
            {
                auto& cell = *line->cells[n];
                last = n == 0 ? strip : lineAt (n).withLeft (valuesFrom).withRight (strip.getRight());

                cell.type.setBounds (take (last, scaled (typeWidth, theme)).reduced (0, 1));
                cell.value.setBounds (take (last, scaled (valueBase, theme)).reduced (0, 1));
                cell.curve.setBounds (take (last, scaled (curveWidth, theme)));
                cell.drop.setBounds (take (last, scaled (dropWidth, theme)).reduced (1, 2));
            }

            line->addArg.setBounds (take (last, scaled (dropWidth, theme)).reduced (1, 2));
        }

        content.repaint();
    }

    void OscMessagesComponent::paint (juce::Graphics& g)
    {
        const auto row = juce::roundToInt (theme.row * theme.type);
        auto head = getLocalBounds().removeFromTop (row);

        g.setColour (Look::colour (theme, "ink-faint"));
        g.setFont (Look::font (theme, 11.0f));

        //  Laid as the rows are, and moved across with them when they scroll.
        auto strip = head.withX (head.getX() - headScroll()).withWidth (std::max (head.getWidth(), content.getWidth()));
        strip.removeFromLeft (numberPixels + scaled (gap, theme));
        g.drawText ("address", strip.removeFromLeft (scaled (addressBase, theme) + scaled (gap, theme)),
                    juce::Justification::centredLeft, true);
        g.drawText ("values - type, value, curve", strip.removeFromLeft (cellPixels() - scaled (gap, theme)),
                    juce::Justification::centredLeft, true);

        g.setColour (Look::colour (theme, "rule"));
        g.fillRect (head.getX(), head.getBottom() - 1, head.getWidth(), 1);

        /*  THE EDGES A HAND DRAGS (§98): after the address, and after the
            values - a short rule in the head, the cursor saying the rest. */
        for (std::size_t edge = 0; edge <= (valued ? 1u : 0u); ++edge)
            if (const auto x = columnEdge (edge); x >= 0 && x < getWidth())
                g.fillRect (x, head.getY() + 4, 1, head.getHeight() - 8);

        if (reading().rows.empty())
        {
            g.setColour (Look::colour (theme, "ink-dim"));
            g.drawText ("Not an OSC cue", getLocalBounds().withTrimmedTop (row), juce::Justification::centred, false);
        }
    }

    int OscMessagesComponent::cellPixels() const
    {
        return scaled (typeWidth, theme) + scaled (valueBase, theme) + scaled (curveWidth, theme)
             + scaled (dropWidth, theme) + scaled (gap, theme) * 4;
    }

    int OscMessagesComponent::headScroll() const
    {
        return viewport.getViewPositionX();
    }

    int OscMessagesComponent::columnEdge (std::size_t edge) const
    {
        //  The address has an edge always; the values only where some message has one.
        if (edge > (valued ? 1u : 0u))
            return -1;

        const auto pastAddress = numberPixels + scaled (gap, theme) + scaled (addressBase, theme);
        const auto halfGap = scaled (gap, theme) / 2;
        const auto x = edge == 0 ? pastAddress + halfGap
                                 : pastAddress + scaled (gap, theme) + cellPixels() - halfGap;
        return x - headScroll();
    }

    int OscMessagesComponent::edgeNear (int x) const
    {
        const auto reach = std::max (edgeReach, scaled (edgeReach, theme));

        for (std::size_t edge = 0; edge <= 1; ++edge)
            if (const auto at = columnEdge (edge); at >= 0 && std::abs (at - x) <= reach)
                return static_cast<int> (edge);

        return -1;
    }

    void OscMessagesComponent::mouseMove (const juce::MouseEvent& event)
    {
        const auto inHead = event.y < juce::roundToInt (theme.row * theme.type);
        setMouseCursor (inHead && edgeNear (event.x) >= 0 ? juce::MouseCursor::LeftRightResizeCursor
                                                          : juce::MouseCursor::NormalCursor);
    }

    void OscMessagesComponent::mouseExit (const juce::MouseEvent&)
    {
        if (dragging < 0)
            setMouseCursor (juce::MouseCursor::NormalCursor);
    }

    void OscMessagesComponent::mouseDown (const juce::MouseEvent& event)
    {
        dragging = event.y < juce::roundToInt (theme.row * theme.type) ? edgeNear (event.x) : -1;
        dragFrom = event.x;
        dragStartBase = dragging == 0 ? addressBase : valueBase;
    }

    void OscMessagesComponent::mouseDrag (const juce::MouseEvent& event)
    {
        if (dragging < 0)
            return;

        //  THE EDGE FOLLOWS THE HAND: the values are one under another (§103), so one box's worth.
        const auto travel = static_cast<float> (event.x - dragFrom) / theme.type;
        const auto wanted = dragStartBase + juce::roundToInt (travel);

        auto& base = dragging == 0 ? addressBase : valueBase;
        const auto now = dragging == 0 ? juce::jlimit (addressLeast, addressMost, wanted)
                                       : juce::jlimit (valueLeast, valueMost, wanted);

        if (now == base)
            return;

        base = now;
        layOut();
        repaint();
    }

    void OscMessagesComponent::mouseUp (const juce::MouseEvent&)
    {
        if (dragging < 0)
            return;

        const auto* column = dragging == 0 ? addressColumn : valueColumn;
        const auto now = dragging == 0 ? addressBase : valueBase;
        dragging = -1;

        if (now != dragStartBase && actions.keepWidth)
            actions.keepWidth (column, now);
    }

    void OscMessagesComponent::mouseDoubleClick (const juce::MouseEvent& event)
    {
        //  A DOUBLE CLICK ON AN EDGE puts the column back to its own width, and forgets the one kept.
        const auto edge = event.y < juce::roundToInt (theme.row * theme.type) ? edgeNear (event.x) : -1;

        if (edge < 0)
            return;

        dragging = -1;

        if (edge == 0)
            addressBase = addressWidth;
        else
            valueBase = valueWidth;

        if (actions.keepWidth)
            actions.keepWidth (edge == 0 ? addressColumn : valueColumn, 0);

        layOut();
        repaint();
    }

    juce::Label* OscMessagesComponent::addressBox (std::size_t row)
    {
        auto* line = rowAt (row);
        return line != nullptr ? &line->address : nullptr;
    }

    juce::Label* OscMessagesComponent::valueBox (std::size_t row, std::size_t arg)
    {
        auto* line = rowAt (row);
        return line != nullptr && arg < line->cells.size() ? &line->cells[arg]->value : nullptr;
    }

    juce::ComboBox* OscMessagesComponent::typeMenu (std::size_t row, std::size_t arg)
    {
        auto* line = rowAt (row);
        return line != nullptr && arg < line->cells.size() ? &line->cells[arg]->type : nullptr;
    }

    juce::ToggleButton* OscMessagesComponent::curveSwitch (std::size_t row, std::size_t arg)
    {
        auto* line = rowAt (row);
        return line != nullptr && arg < line->cells.size() ? &line->cells[arg]->curve : nullptr;
    }

    juce::Button* OscMessagesComponent::removeValue (std::size_t row, std::size_t arg)
    {
        auto* line = rowAt (row);
        return line != nullptr && arg < line->cells.size() ? &line->cells[arg]->drop : nullptr;
    }

    juce::Button* OscMessagesComponent::addValue (std::size_t row)
    {
        auto* line = rowAt (row);
        return line != nullptr ? &line->addArg : nullptr;
    }

    juce::Button* OscMessagesComponent::removeMessage (std::size_t row)
    {
        auto* line = rowAt (row);
        return line != nullptr ? &line->drop : nullptr;
    }

    juce::Button* OscMessagesComponent::pickAddress (std::size_t row)
    {
        auto* line = rowAt (row);
        return line != nullptr ? &line->pick : nullptr;
    }

    juce::PopupMenu OscMessagesComponent::menuOf (const std::vector<model::TreeMenuItem>& items,
                                                  const std::string& current, std::vector<std::string>& addresses)
    {
        juce::PopupMenu menu;

        for (const auto& item : items)
        {
            if (! item.children.empty())
            {
                menu.addSubMenu (juce::String (item.label), menuOf (item.children, current, addresses));
                continue;
            }

            addresses.push_back (item.address);
            menu.addItem (static_cast<int> (addresses.size()), juce::String (item.label), true, item.address == current);
        }

        return menu;
    }
}
