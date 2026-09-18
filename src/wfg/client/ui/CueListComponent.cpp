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

    void CueListComponent::show (const model::ShowModel& model, const std::string& standbyId,
                                 const std::string& pickedId)
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
        const auto structureMoved = model.rebuilds() != drawnWalk || model.list() != drawnList;

        if (structureMoved)
        {
            rows = model.rows();
            drawnWalk = model.rebuilds();
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
            if (standbyRow >= 0 && list.getHeight() > 0)
                list.scrollToEnsureRowIsOnscreen (standbyRow);
        }

        /*  AND WHICH CUE IS PICKED, which moves as often as somebody clicks and
            costs the same two rows. THE STANDBY AND THE SELECTION ARE DIFFERENT
            THINGS - where GO will act, against what the inspector is about -
            and §4.8 wants them told apart by more than a hue: one is a mark in
            the gutter and a bar down the left edge, the other is a wash across
            the row. */
        if (pickedId != picked || structureMoved)
        {
            const auto wasAt = pickedRow;

            picked = pickedId;
            pickedRow = model.indexOf (pickedId);

            if (! structureMoved)
            {
                if (wasAt >= 0)      list.repaintRow (wasAt);
                if (pickedRow >= 0)  list.repaintRow (pickedRow);
            }
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

        if (entry.rowKind == model::RowKind::band)
        {
            paintBand (entry, row, g, width, height);
            return;
        }

        const auto isStandby = row == standbyRow;
        const auto isPicked = row == pickedRow;

        const auto ink = Look::colour (theme, entry.enabled ? "ink" : "ink-off");
        const auto faint = Look::colour (theme, "ink-faint");
        const auto standbyColour = Look::colour (theme, "standby");

        /*  A SECTION READS AS A BAND AND ANYTHING INSIDE SOMETHING READS AS
            RECESSED - the page's `.row[data-in]` ground, which is the other
            half of what makes a container legible there. Two different
            questions drawn two different ways: a header is not a member, and
            drawing them alike is what made the page's own header lines
            ambiguous until they got a frame. */
        g.fillAll (entry.section != model::Section::member || entry.depth > 0
                     ? Look::colour (theme, "panel-in")
                     : Look::colour (theme, row % 2 == 0 ? "panel" : "panel-high"));

        if (isPicked)
        {
            g.setColour (Look::colour (theme, "picked").withAlpha (0.16f));
            g.fillRect (0, 0, width, height);
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
            the last row inside turns it right and stops it. Three rows drawing
            one shape, which holds together only because all three measure the
            rail from the same left edge. */
        const auto indent = unit * 2;
        const auto railsFrom = area.getX();

        const auto railX = [railsFrom, indent] (int level)
        { return railsFrom + (level - 1) * indent + indent / 2; };

        const auto middle = height / 2;

        g.setColour (Look::colour (theme, "rule"));

        for (int level = 1; level <= entry.depth; ++level)
        {
            /*  Does this level's run end on this row? The walk lays a
                container's rows out contiguously, so the next row shallower
                than the level is where that level closes. */
            const auto next = static_cast<std::size_t> (row) + 1;
            const auto closes = next >= rows.size() || rows[next].depth < level;

            g.fillRect (railX (level), 0, 1, closes ? middle : height);

            if (closes)
                g.fillRect (railX (level), middle, indent / 2, 1);
        }

        /*  And a container opens its children's rail under itself, so the eye
            can follow one line from the group to the last thing inside it. */
        if (entry.isGroup)
            g.fillRect (railX (entry.depth + 1), middle, 1, height - middle);

        area.removeFromLeft (entry.depth * indent);

        /*  A GROUP SAYS SO WITH A SHAPE as well as with its mode in words and
            its rail (§4.8): three tellings, not one, and none of them colour. */
        auto markCell = area.removeFromLeft (indent);
        g.setColour (faint);
        g.setFont (Look::font (theme, 13.0f));
        /*  A GROUP'S TWIST POINTS DOWN WHEN IT IS OPEN AND RIGHT WHEN IT IS
            SHUT, which is the one convention a file tree has taught everybody
            already - and the same shape a section's band uses, so the two kinds
            of container fold the same way. */
        g.drawText (entry.isGroup
                      ? juce::String (juce::CharPointer_UTF8 (entry.shut ? "\xe2\x96\xb8"
                                                                        : "\xe2\x96\xbe"))
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

    void CueListComponent::paintBand (const model::Row& entry, int row, juce::Graphics& g,
                                      int width, int height)
    {
        /*  A SECTION'S HEAD: the word, how many lines it holds, and a twist -
            three tellings, and not one of them a colour (§4.8). The page's
            band, in a list box rather than a stylesheet.

            THE FRAME IS ITS TOP EDGE. The head draws the rule over the word and
            the corner the rail comes down from; the rows inside draw that rail;
            the last of them turns it right. A shut section keeps the rule -
            that is what parts it from the rows above, open or shut - and loses
            the rail, which would otherwise hang off the bottom with nothing to
            enclose. */
        const auto unit = juce::roundToInt (theme.type * 7.0);
        const auto pad = unit / 2;
        const auto indent = unit * 2;

        g.fillAll (Look::colour (theme, "panel-in"));

        auto area = juce::Rectangle<int> (0, 0, width, height).reduced (pad, 0);
        area.removeFromLeft (unit * 2 + numberChars * unit);

        const auto left = area.getX() + entry.depth * indent;

        g.setColour (Look::colour (theme, "rule"));
        g.fillRect (left, 0, width - left - pad, 1);

        if (! entry.shut)
            g.fillRect (left, 0, 1, height);

        auto text = area.withTrimmedLeft (entry.depth * indent + pad);

        /*  THE TWIST IS A SHAPE: pointing down when the section is open and
            right when it is shut, which is the one convention every file tree
            has taught everybody already. */
        auto twist = text.removeFromLeft (indent);
        g.setColour (Look::colour (theme, "ink-dim"));
        g.setFont (Look::font (theme, 11.0f));
        g.drawText (juce::String (juce::CharPointer_UTF8 (entry.shut ? "\xe2\x96\xb8" : "\xe2\x96\xbe")),
                    twist, juce::Justification::centredLeft, false);

        auto word = juce::String (entry.name).toUpperCase();
        g.setColour (Look::colour (theme, "ink-dim"));
        g.setFont (Look::font (theme, 10.0f));
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
            if (actions.pick)
                actions.pick (entry.id);

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

    void CueListComponent::backgroundClicked (const juce::MouseEvent&)
    {
        /*  EMPTY SPACE MEANS NOTHING IS PICKED (author, 2026-09-18: "clicking
            on empty space in the cuelist could also close the inspector"). The
            close box says the same thing at the other end of the window; this
            is the gesture somebody makes without thinking about it, which is
            the one worth having. */
        if (actions.pick)
            actions.pick ({});
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
