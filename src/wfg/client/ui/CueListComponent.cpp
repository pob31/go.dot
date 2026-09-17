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

#include <wfg/client/ui/Look.h>

#include <utility>

namespace wfg::client::ui
{
    namespace
    {
        /*  THE COLUMNS, as fractions of the width left after the number and
            the times, which are fixed. The author asked for preWait, duration
            and postWait as columns on the page and they are the same three
            here, in the same order - "something logical time wise", their
            words, and the order somebody works in rather than the alphabet. */
        constexpr int numberChars = 6;
        constexpr int timeChars = 6;
        constexpr int kindChars = 8;

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

        setWantsKeyboardFocus (true);
        applyTheme (theme);
    }

    int CueListComponent::rowHeight() const noexcept
    {
        return juce::roundToInt (theme.row * theme.type);
    }

    void CueListComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;

        list.setRowHeight (rowHeight());
        list.setColour (juce::ListBox::backgroundColourId, Look::colour (theme, "panel"));
        list.setColour (juce::ListBox::outlineColourId, Look::colour (theme, "rule"));

        resized();
        repaint();
    }

    void CueListComponent::show (const model::ShowModel& model, const std::string& standbyId)
    {
        /*  THE STRUCTURE, at show-change rate. `builtAt` is the revision the
            model walked; while it and the list are the same, the rows are the
            same objects and `updateContent` - which relays out every row -
            has nothing to do. */
        const auto structureMoved = model.builtAt() != drawnAt || model.list() != drawnList;

        if (structureMoved)
        {
            rows = model.rows();
            drawnAt = model.builtAt();
            drawnList = model.list();
            list.updateContent();
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
            if (standbyRow >= 0)
                list.scrollToEnsureRowIsOnscreen (standbyRow);
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
        const auto isStandby = row == standbyRow;

        const auto ink = Look::colour (theme, entry.enabled ? "ink" : "ink-off");
        const auto faint = Look::colour (theme, "ink-faint");
        const auto standbyColour = Look::colour (theme, "standby");

        /*  A SECTION READS AS A BAND, and a member of a group reads as indented
            under it: two different questions, drawn two different ways, because
            a header IS not a member and drawing them alike is what made the
            page's own header lines ambiguous until they got a frame. */
        g.fillAll (entry.section != model::Section::member
                     ? Look::colour (theme, "panel-in")
                     : Look::colour (theme, row % 2 == 0 ? "panel" : "panel-high"));

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

        area.removeFromLeft (entry.depth * unit * 2);

        /*  THE POINTER IS A GLYPH AS WELL AS A COLOUR (§4.8), and a group is a
            triangle, so neither of the two things this column says is said by
            colour alone. */
        auto markCell = area.removeFromLeft (unit * 2);
        g.setColour (isStandby ? standbyColour : faint);
        g.setFont (Look::font (theme, 13.0f));
        g.drawText (isStandby ? juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xb6"))
                              : entry.isGroup ? juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xbe"))
                                              : juce::String(),
                    markCell, juce::Justification::centredLeft, false);

        g.setColour (ink);
        g.setFont (Look::font (theme, entry.isGroup ? 14.0f : 13.0f));

        const auto word = sectionWord (entry.section);
        const auto name = entry.name.empty() ? juce::String ("(unnamed)") : juce::String (entry.name);

        g.drawText (word.isEmpty() ? name : name + "   " + word,
                    area, juce::Justification::centredLeft, true);

        //  A rule under every row, as the page draws one.
        g.setColour (Look::colour (theme, "rule").withAlpha (0.5f));
        g.fillRect (0, height - 1, width, 1);
    }

    void CueListComponent::listBoxItemClicked (int row, const juce::MouseEvent&)
    {
        /*  A CLICK PARKS THE POINTER, and at M3 that is the only way into a
            list whose standby is clear: `standby.next` stays put from nowhere,
            which is the engine's decision and a reasonable one - an arrow key
            should not invent a starting point. Selection, which is what a
            click will ALSO mean once the inspector exists, is M5's; when it
            arrives this becomes a click on the row's left edge, as the page
            already draws it. */
        if (row >= 0 && row < static_cast<int> (rows.size()) && actions.park)
            actions.park (rows[static_cast<std::size_t> (row)].id);
    }

    void CueListComponent::paint (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "panel"));
    }

    void CueListComponent::resized()
    {
        list.setBounds (getLocalBounds());
    }

    bool CueListComponent::keyPressed (const juce::KeyPress& key)
    {
        /*  THE ARROWS MOVE THE POINTER, not a selection: this list has no
            selection yet (M5), and the thing the author needs to judge at M3
            is whether the standby is unmistakable - which cannot be judged
            without moving it. Down is next, up is previous, exactly as the
            page binds them and as gestures/commands.json records. */
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

        return false;
    }
}
