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

#include <juce_audio_formats/juce_audio_formats.h>

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
            return row.rowKind == model::RowKind::cue
                && row.section == model::Section::member;
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
            g.setColour (Look::colour (theme, "live"));

            if (dropWouldLink)
            {
                g.setColour (Look::colour (theme, "live").withAlpha (0.22f));
                g.fillRect (0, 0, width, height);
                g.setColour (Look::colour (theme, "live"));
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
        g.setColour (ink);
        g.setFont (Look::font (theme, entry.isGroup ? 14.0f : 13.0f));

        const auto word = sectionWord (entry.section);
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

        if (was >= 0)
            list.repaintRow (was);
    }

    void CueListComponent::filesDropped (const juce::StringArray& files, int, int y)
    {
        const auto at = rowUnder (y);

        dropRow = -1;
        dropWouldLink = false;
        dropWouldInsert = false;
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
