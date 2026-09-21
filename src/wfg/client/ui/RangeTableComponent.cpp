/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#include <wfg/client/ui/RangeTableComponent.h>

#include <wfg/client/ui/Look.h>
#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace wfg::client::ui
{
    /*  ONE RANGE, ACROSS. The order of the columns is the order the questions
        are asked about a region: what it is called, where it starts, where it
        stops, how long that makes it, and how many times it goes round. */
    struct RangeTableComponent::Row
    {
        model::RangeRow range;
        std::size_t at = 0;

        juce::Label name, in, out, length, count;

        /*  FOR EVER IS NOT A NUMBER, which is what makes a loop count unlike
            every other count in the show: nought means it never stops. A box
            on its own could not say that, so the box says how many and this
            says whether "how many" is a question at all. */
        juce::ToggleButton forever;

        juce::TextButton copy, drop { "x" };
    };

    namespace
    {
        constexpr int loopsForever = 0;

        /*  THE COLUMNS, AT A SCALE OF ONE, in the order they are drawn. Kept
            here rather than spread through the layout so that the heading
            painted above the list and the boxes inside it cannot drift apart:
            both ask this for their rectangles. */
        struct Columns
        {
            juce::Rectangle<int> name, in, out, length, copy, forever, count, drop;
        };

        constexpr int gap = 4;
        constexpr int nameWidth = 76, timeWidth = 62, lengthWidth = 58;
        constexpr int copyWidth = 24, foreverWidth = 30, countWidth = 40, dropWidth = 22;

        int scaled (int base, const model::Theme& theme)
        {
            return juce::roundToInt (static_cast<float> (base) * theme.type);
        }

        Columns columnsOf (juce::Rectangle<int> row, const model::Theme& theme)
        {
            const auto take = [&row, &theme] (int base)
            {
                auto cut = row.removeFromLeft (scaled (base, theme));
                row.removeFromLeft (scaled (gap, theme));
                return cut;
            };

            Columns out;
            out.name = take (nameWidth);
            out.in = take (timeWidth);
            out.out = take (timeWidth);
            out.length = take (lengthWidth);
            out.copy = take (copyWidth);
            out.forever = take (foreverWidth);
            out.count = take (countWidth);
            out.drop = row.removeFromLeft (scaled (dropWidth, theme));

            return out;
        }

        int totalWidth (const model::Theme& theme)
        {
            return scaled (nameWidth + timeWidth * 2 + lengthWidth + copyWidth
                             + foreverWidth + countWidth + dropWidth + gap * 7,
                           theme);
        }
    }

    RangeTableComponent::RangeTableComponent (const model::Theme& themeToUse, Actions actionsToUse)
        : theme (themeToUse), actions (std::move (actionsToUse))
    {
        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);

        /*  A CUE WITH NO RANGES PLAYS THE WHOLE FILE, which is a perfectly
            good thing for it to do - so this is an offer and never a
            correction. It is also the only way to make the FIRST range from
            here, and a table that could edit regions but not make one would
            send somebody back to a menu for the one gesture it exists for. */
        /*  IN THE HEAD, OVER THE COLUMN THAT REMOVES ONE (author, 2026-09-21:
            *"put the add range in the head of the table above the [X] to
            remove the ranges"*). It was a button across the foot of the panel
            and it overlapped the ruler's last figure; here it is a glyph in
            the one column that is about adding and taking away, and the
            picture gets its whole width back. */
        add.setButtonText ("+");
        add.setWantsKeyboardFocus (false);
        add.setTooltip ("Adds a range after the last one, to the end of the file");
        add.onClick = [this]
        {
            const auto wanted = model::addAt (reading.ranges, playhead, reading.fileLength);

            switch (wanted.kind)
            {
                case model::RangeAdd::Kind::split:
                    if (actions.splitRange)
                        actions.splitRange (reading.subject.objectId, wanted.at);

                    break;

                case model::RangeAdd::Kind::create:
                    if (actions.createRange)
                        actions.createRange (reading.subject.objectId, wanted.in, wanted.out);

                    break;

                case model::RangeAdd::Kind::nothing:
                    if (actions.say)
                        actions.say (juce::String (wanted.why));

                    break;
            }
        };

        addAndMakeVisible (add);

        applyTheme (theme);
    }

    RangeTableComponent::~RangeTableComponent() = default;

    int RangeTableComponent::wantedWidth() const
    {
        //  Plus the scrollbar, which is inside the viewport and not over the last column.
        return totalWidth (theme) + scaled (14, theme);
    }

    void RangeTableComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;

        viewport.setColour (juce::ScrollBar::thumbColourId, Look::colour (theme, "rule"));

        for (auto& row : rows)
        {
            for (auto* label : { &row->name, &row->in, &row->out, &row->count })
            {
                label->setFont (Look::font (theme, 12.0f));
                label->setColour (juce::Label::textColourId, Look::colour (theme, "ink"));
                label->setColour (juce::Label::backgroundColourId,
                                  Look::colour (theme, "panel-in"));
            }

            row->length.setFont (Look::font (theme, 12.0f));
            row->length.setColour (juce::Label::textColourId, Look::colour (theme, "ink-dim"));
        }

        repaint();
    }

    std::string RangeTableComponent::shapeOf() const
    {
        /*  THE IDENTITY OF THE LIST AND NOT ITS VALUES: a table is rebuilt when
            a range appears, goes or changes places, and merely retyped when a
            number moves. Rebuilding on a number would take the focus out of
            the box somebody is typing into, twenty-five times a second. */
        std::string out;

        for (const auto& range : reading.ranges)
            out += range.id + ";";

        return out;
    }

    void RangeTableComponent::setPlayhead (double seconds)
    {
        if (juce::approximatelyEqual (playhead, seconds))
            return;

        playhead = seconds;
        sayWhatThePlusWouldDo();
    }

    /*  WHAT PRESSING IT WOULD DO, ON IT: the plus cuts where the head is, and
        where the head is changes as a cue plays - so the button says whether
        there is a cut to be made there before somebody presses it, rather than
        refusing afterwards. §4.8: the word does the telling, and the greying
        only agrees with it. */
    void RangeTableComponent::sayWhatThePlusWouldDo()
    {
        const auto wanted = model::addAt (reading.ranges, playhead, reading.fileLength);

        add.setEnabled (wanted.kind != model::RangeAdd::Kind::nothing);

        switch (wanted.kind)
        {
            case model::RangeAdd::Kind::split:
                add.setTooltip ("Cuts this range in two at "
                                  + juce::String (model::timeText (wanted.at)));
                break;

            case model::RangeAdd::Kind::create:
                add.setTooltip ("Makes a range from " + juce::String (model::timeText (wanted.in))
                                  + " to " + juce::String (model::timeText (wanted.out)));
                break;

            case model::RangeAdd::Kind::nothing:
                add.setTooltip (juce::String (wanted.why));
                break;
        }
    }

    void RangeTableComponent::show (const model::FootReading& readingToUse)
    {
        reading = readingToUse;

        const auto shape = shapeOf();

        if (shape != drawnShape || reading.subject.objectId != drawnCue)
        {
            drawnShape = shape;
            drawnCue = reading.subject.objectId;
            rebuild();
            return;
        }

        refresh();
    }

    void RangeTableComponent::rebuild()
    {
        rows.clear();
        content.removeAllChildren();

        for (std::size_t at = 0; at < reading.ranges.size(); ++at)
        {
            auto row = std::make_unique<Row>();
            row->range = reading.ranges[at];
            row->at = at;

            auto* raw = row.get();

            const auto write = [this, raw] (const char* attribute, const std::string& value)
            {
                if (actions.set)
                    actions.set (model::rangeAddress (raw->range.id, attribute), value);
            };

            //  The name, which is what the bar writes across the band as well.
            row->name.setEditable (true, true, false);
            row->name.setTooltip ("What this range is called");
            row->name.onTextChange = [raw, write]
            {
                write ("name", raw->name.getText().toStdString());
            };

            /*  IN AND OUT AS TIMES. A cell that will not parse is put back to
                what the document says rather than written as nought: a slip of
                the keyboard must not move a cue point, and the number is still
                there to try again from. */
            const auto time = [this, raw, write] (juce::Label* box, bool isIn)
            {
                box->setEditable (true, true, false);
                box->onTextChange = [this, raw, box, isIn, write]
                {
                    const auto typed = model::timeFrom (box->getText().toStdString());

                    if (! typed.has_value())
                    {
                        box->setText (juce::String (model::timeText (isIn ? raw->range.in
                                                                          : raw->range.out)),
                                      juce::dontSendNotification);

                        if (actions.say)
                            actions.say ("that is not a time: seconds, or minutes:seconds");

                        return;
                    }

                    write (isIn ? "in" : "out", osc::formatDouble (*typed));
                };
            };

            time (&row->in, true);
            time (&row->out, false);

            row->in.setTooltip ("Where this range starts, in seconds or minutes:seconds");
            row->out.setTooltip ("Where it stops");

            row->length.setJustificationType (juce::Justification::centredRight);
            row->length.setTooltip ("How long it is");

            /*  THE BUTTON THE AUTHOR ASKED FOR. It writes the NEXT range's
                out-point, leaving its in-point alone, so a join survives and
                pressing it down the table builds a run of equal slices. */
            row->copy.setButtonText (juce::String::fromUTF8 ("\xe2\x86\x92"));
            row->copy.setWantsKeyboardFocus (false);
            row->copy.setTooltip ("Give the next range this same length");
            row->copy.onClick = [this, raw]
            {
                const auto writes = model::copyLengthToNext (reading.ranges, raw->at,
                                                             reading.fileLength);

                if (writes.empty())
                {
                    if (actions.say)
                        actions.say ("the next range would run past the end of the file");

                    return;
                }

                for (const auto& one : writes)
                    if (actions.set)
                        actions.set (model::rangeAddress (one.rangeId, one.attribute),
                                     osc::formatDouble (one.seconds));

                if (actions.say)
                    actions.say ("next range set to " + juce::String (model::timeText (raw->range.length())));
            };

            row->forever.setButtonText (juce::String::fromUTF8 ("\xe2\x88\x9e"));
            row->forever.setWantsKeyboardFocus (false);
            row->forever.setTooltip ("Repeat for ever, until something stops it");

            row->count.setEditable (true, true, false);
            row->count.setJustificationType (juce::Justification::centredRight);
            row->count.setTooltip ("How many times it plays: 1 is once through");

            /*  THE TWO TOGETHER, in one place so that neither can say something
                the other contradicts: ticked is nought and means for ever,
                unticked is the number in the box and never less than one. */
            const auto commitLoops = [raw, write]
            {
                const auto typed = juce::jmax (1, raw->count.getText().getIntValue());
                const auto wanted = raw->forever.getToggleState() ? loopsForever : typed;

                raw->count.setVisible (! raw->forever.getToggleState());
                write ("loops", std::to_string (wanted));
            };

            row->forever.onClick = commitLoops;
            row->count.onTextChange = commitLoops;

            row->drop.setWantsKeyboardFocus (false);
            row->drop.setTooltip ("Removes this range");
            row->drop.onClick = [this, raw]
            {
                if (actions.removeRange)
                    actions.removeRange (raw->range.id);
            };

            juce::Component* children[] { &row->name, &row->in, &row->out, &row->length,
                                          &row->copy, &row->forever, &row->count, &row->drop };

            for (auto* child : children)
                content.addAndMakeVisible (*child);

            rows.push_back (std::move (row));
        }

        applyTheme (theme);
        refresh();
        layOut();
    }

    void RangeTableComponent::refresh()
    {
        for (std::size_t at = 0; at < rows.size() && at < reading.ranges.size(); ++at)
        {
            auto& row = *rows[at];
            const auto& range = reading.ranges[at];

            row.range = range;
            row.at = at;

            /*  NEVER OVER A HAND THAT IS TYPING. The pass arrives twenty-five
                times a second and the half-typed number in a box is the one
                thing this table must not take back. */
            if (! row.name.isBeingEdited())
                row.name.setText (juce::String (range.name.empty()
                                                  ? "range " + std::to_string (range.index + 1)
                                                  : range.name),
                                  juce::dontSendNotification);

            if (! row.in.isBeingEdited())
                row.in.setText (juce::String (model::timeText (range.in)),
                                juce::dontSendNotification);

            if (! row.out.isBeingEdited())
                row.out.setText (juce::String (model::timeText (range.out)),
                                 juce::dontSendNotification);

            row.length.setText (juce::String (model::timeText (range.length())),
                                juce::dontSendNotification);

            const auto goesOnForEver = range.loops == loopsForever;

            row.forever.setToggleState (goesOnForEver, juce::dontSendNotification);
            row.count.setVisible (! goesOnForEver);

            if (! row.count.isBeingEdited())
                row.count.setText (juce::String (goesOnForEver ? 1 : range.loops),
                                   juce::dontSendNotification);

            //  The last range has nothing to copy its length TO.
            row.copy.setEnabled (at + 1 < rows.size());

            /*  AND THE LAST ONE LEFT CANNOT BE REMOVED (author, 2026-09-21:
                *"there can't [be] less than one range"*). A cue that has said
                which region of its file it plays has said something, and
                taking the last one away would quietly turn it back into a cue
                that plays the whole recording - a decision, made by a cross
                that looks like a tidy-up. The way back is deliberate: drag the
                one range to cover what is wanted, or say so in the boxes. */
            row.drop.setEnabled (rows.size() > 1);
            row.drop.setTooltip (rows.size() > 1
                                   ? "Removes this range"
                                   : "A cue with ranges keeps at least one");
        }

        sayWhatThePlusWouldDo();
    }

    void RangeTableComponent::resized()
    {
        layOut();
    }

    void RangeTableComponent::layOut()
    {
        const auto row = juce::roundToInt (theme.row * theme.type);
        auto area = getLocalBounds();

        auto head = area.removeFromTop (row);        // the column names, painted below

        viewport.setBounds (area);

        const auto inner = juce::jmax (totalWidth (theme),
                                       viewport.getWidth() - scaled (14, theme));

        //  The plus sits in the head, in the column the crosses are in.
        add.setBounds (columnsOf (head.withWidth (inner), theme).drop.reduced (1, 2));

        content.setSize (inner, juce::jmax (row, static_cast<int> (rows.size()) * row));

        auto y = 0;

        for (auto& line : rows)
        {
            const auto columns = columnsOf (juce::Rectangle<int> (0, y, inner, row), theme);

            line->name.setBounds (columns.name.reduced (0, 1));
            line->in.setBounds (columns.in.reduced (0, 1));
            line->out.setBounds (columns.out.reduced (0, 1));
            line->length.setBounds (columns.length);
            line->copy.setBounds (columns.copy.reduced (1, 2));
            line->forever.setBounds (columns.forever);
            line->count.setBounds (columns.count.reduced (0, 1));
            line->drop.setBounds (columns.drop.reduced (1, 2));

            y += row;
        }
    }

    void RangeTableComponent::paint (juce::Graphics& g)
    {
        const auto row = juce::roundToInt (theme.row * theme.type);
        auto area = getLocalBounds();
        auto head = area.removeFromTop (row);

        /*  THE COLUMN NAMES, over the boxes they belong to. Drawn rather than
            laid out as labels so that the heading and the rows ask the same
            function for their rectangles and cannot come apart. */
        const auto inner = juce::jmax (totalWidth (theme),
                                       viewport.getWidth() - scaled (14, theme));
        const auto columns = columnsOf (juce::Rectangle<int> (0, head.getY(), inner, row), theme);

        g.setColour (Look::colour (theme, "ink-faint"));
        g.setFont (Look::font (theme, 11.0f));

        g.drawText ("range", columns.name, juce::Justification::centredLeft, false);
        g.drawText ("in", columns.in, juce::Justification::centredLeft, false);
        g.drawText ("out", columns.out, juce::Justification::centredLeft, false);
        g.drawText ("length", columns.length, juce::Justification::centredRight, false);
        g.drawText ("repeats", columns.forever.withWidth (columns.count.getRight()
                                                            - columns.forever.getX()),
                    juce::Justification::centredLeft, false);

        g.setColour (Look::colour (theme, "rule"));
        g.fillRect (head.getX(), head.getBottom() - 1, head.getWidth(), 1);

        /*  AND WHEN THERE ARE NONE, what that means rather than an empty box.
            A media cue with no ranges is not unfinished: it plays its file
            from the start offset to the end, which is what most cues do. */
        if (rows.empty())
        {
            g.setColour (Look::colour (theme, "ink-off"));
            g.setFont (Look::font (theme, 12.0f));
            g.drawFittedText ("no ranges: the whole file plays, from "
                                + juce::String (model::timeText (reading.startOffset))
                                + " - the + above makes one",
                              area.reduced (6, 4), juce::Justification::centredLeft, 2);
        }
    }
}
