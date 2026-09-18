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

#include <wfg/client/ui/InspectorComponent.h>

#include <wfg/client/ui/Look.h>

#include <wfg/engine/osc/OscValue.h>

#include <utility>

namespace wfg::client::ui
{
    /*  ONE ROW OF THE PANEL: a name, and whatever the node says its value
        should be edited with. A heading carries no field and draws itself. */
    struct InspectorComponent::Line
    {
        model::Field field;
        bool isHeading = false;
        bool isDetail = false;
        juce::String headingText;

        juce::Label name;
        juce::Label box;                    ///< editable in place, for text and numbers
        juce::ToggleButton toggle;          ///< a `T` row is a switch, not a word to type
        juce::ComboBox choice;              ///< a closed set of values is a choice, not typing

        /*  THE LOOP CONTROL, which is three questions and one integer: does it
            repeat at all, for ever or a set number of times, and how many.
            Written as the author asked for it (2026-09-18): "First [x]Loop if
            checked [x] infinite if unchecked [123] loops". */
        juce::ToggleButton repeats { "loop" }, forever { "for ever" };

        /** The file control's other half: the box takes a name, this goes looking. */
        juce::TextButton browse { "..." };
    };

    namespace
    {
        /*  ONE INTEGER, THREE QUESTIONS, and the mapping written out once so
            that reading it and writing it cannot drift:

                0   for ever      the ambience bed
                1   no repeat     plays its round and is done
                N   N rounds

            The engine's own word for zero is "for ever", which is what makes
            `loops` unlike every other count in the show and is exactly what a
            bare box could not say. */
        constexpr int loopsForever = 0;
        constexpr int loopsOnce = 1;

        int loopsFrom (const std::string& value)
        {
            const auto number = osc::parseDouble (value);
            return number.has_value() ? static_cast<int> (*number) : loopsOnce;
        }
    }

    InspectorComponent::InspectorComponent (const model::Theme& themeToUse, Actions actionsToUse)
        : actions (std::move (actionsToUse)), theme (themeToUse)
    {
        heading.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (heading);

        /*  THE FOLD THE AUTHOR ASKED FOR, on the page in these words: "we could
            hide the internal stuff like the various UIDs, hash and other things
            that are not really necessary for the user". */
        /*  A WAY OUT (author, 2026-09-18: "we also need a way to deselect and
            close the Inspector. There can be a [x] close box icon at the top of
            the inspector"). It clears the SELECTION rather than hiding a pane,
            which is the honest thing: this panel is here because a cue is
            picked, so the way to be rid of it is to pick nothing - and the two
            list panes take the width back by themselves. */
        closeButton.setWantsKeyboardFocus (false);
        closeButton.onClick = [this] { if (actions.close) actions.close(); };
        addAndMakeVisible (closeButton);

        detailsButton.setWantsKeyboardFocus (false);
        detailsButton.getProperties().set (Look::glyphButton(), true);
        detailsButton.onClick = [this]
        {
            detailsOpen = ! detailsOpen;
            sayWhetherDetailsAreOpen();
            layOut();
        };
        sayWhetherDetailsAreOpen();
        /*  THE FOLD'S HEAD LIVES AMONG THE LINES, not at the foot of the
            pane: the author found it "sitting at the bottom of the window"
            (2026-09-18), a screen away from the fields it folds. It stands
            where the details begin, so opening it puts them right under it. */
        content.addAndMakeVisible (detailsButton);

        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);

        applyTheme (theme);
    }

    InspectorComponent::~InspectorComponent() = default;

    /*  A TOGGLE THAT DOES NOT SAY WHICH WAY IT IS SET is a control somebody has
        to press to find out (author, 2026-09-18: "the details toggle doesn't
        show different states"). It said `details` open and `details` shut, so
        the only way to read it was to look at whether any details were there -
        which is exactly what somebody is pressing it to change.

        THE SAME TWIST THE BANDS USE, pointing down when the fold is open and
        right when it is shut: the window has one convention for a thing that
        opens, and a panel that invented a second would be two conventions for
        one idea. A SHAPE and not a colour (§4.8), like theirs. */
    void InspectorComponent::sayWhetherDetailsAreOpen()
    {
        /*  IT POINTS THE WAY THE PANEL OPENS, and this fold is at the FOOT of
            the panel - so open is UP, towards the rows it revealed, and shut is
            down (author, 2026-09-18: "details is at the bottom and should be
            pointing up when expanded").

            The bands in the cue list point the other way for the same reason:
            they head their section, so their rows appear BELOW them. One rule,
            two directions, and the rule is where the content goes. */
        detailsButton.setButtonText (juce::String (juce::CharPointer_UTF8 (detailsOpen ? "\xe2\x96\xb4"
                                                                                      : "\xe2\x96\xbe"))
                                       + "  details");
    }

    int InspectorComponent::rowHeight() const noexcept
    {
        return juce::roundToInt (theme.row * theme.type);
    }

    void InspectorComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;

        heading.setFont (Look::font (theme, 14.0f));
        heading.setColour (juce::Label::textColourId, Look::colour (theme, "ink"));

        viewport.setColour (juce::ScrollBar::thumbColourId, Look::colour (theme, "rule"));

        for (auto& line : lines)
        {
            line->name.setFont (Look::font (theme, 12.0f));
            line->name.setColour (juce::Label::textColourId,
                                  Look::colour (theme, line->isDetail ? "ink-off" : "ink-faint"));

            line->box.setFont (Look::font (theme, 12.0f));
            line->box.setColour (juce::Label::textColourId,
                                 Look::colour (theme, line->isDetail ? "ink-off" : "ink"));
            line->box.setColour (juce::Label::backgroundColourId,
                                 line->field.writable ? Look::colour (theme, "panel-in")
                                                      : juce::Colours::transparentBlack);
        }

        resized();
        repaint();
    }

    void InspectorComponent::show (const model::Inspection& inspection)
    {
        /*  REBUILT ONLY WHEN THE CUE OR ITS SHAPE CHANGES. A value moving is a
            `setText` on a line that already exists; a different cue is a
            different panel. Counting the fields catches the case that matters
            in between - a kind changing under the same identifier, which adds
            and removes rows. */
        auto fieldCount = inspection.details.size();

        for (const auto& block : inspection.blocks)
            fieldCount += block.fields.size();

        if (inspection.cueId != drawnCue || fieldCount != drawnFields)
        {
            rebuild (inspection);
            return;
        }

        //  The same panel: only the values can have moved.
        std::size_t at = 0;

        const auto update = [this, &at] (const model::Field& field)
        {
            while (at < lines.size() && lines[at]->isHeading)
                ++at;

            if (at >= lines.size())
                return;

            auto& line = *lines[at++];

            /*  NEVER WHILE SOMEBODY IS TYPING IN IT. A poll that overwrote a
                half-typed value would lose exactly the keystrokes somebody was
                in the middle of, which is the one thing a panel like this must
                not do. */
            if (line.box.isBeingEdited())
                return;

            switch (line.field.control)
            {
                case model::Control::toggle:
                    line.toggle.setToggleState (field.value == "true", juce::dontSendNotification);
                    break;

                case model::Control::choice:
                    line.choice.setText (field.value, juce::dontSendNotification);
                    break;

                case model::Control::loopCount:
                {
                    /*  THE THREE TOGETHER, and only when the engine's answer
                        differs from what they already say - otherwise a poll
                        twenty-five times a second would fight a hand that had
                        just unticked a box. */
                    const auto count = loopsFrom (field.value);

                    if (count != loopsFrom (line.field.value))
                    {
                        line.repeats.setToggleState (count != loopsOnce, juce::dontSendNotification);
                        line.forever.setToggleState (count == loopsForever, juce::dontSendNotification);
                        line.forever.setEnabled (count != loopsOnce);

                        if (count > loopsOnce)
                            line.box.setText (juce::String (count), juce::dontSendNotification);

                        line.box.setVisible (count > loopsOnce);
                    }

                    break;
                }

                case model::Control::file:
                case model::Control::text:
                default:
                    line.box.setText (field.value, juce::dontSendNotification);
                    break;
            }

            line.field.value = field.value;
        };

        for (const auto& block : inspection.blocks)
            for (const auto& field : block.fields)
                update (field);

        for (const auto& field : inspection.details)
            update (field);
    }

    void InspectorComponent::rebuild (const model::Inspection& inspection)
    {
        lines.clear();
        content.removeAllChildren();

        drawnCue = inspection.cueId;
        drawnFields = inspection.details.size();

        for (const auto& block : inspection.blocks)
            drawnFields += block.fields.size();

        heading.setText (inspection.cueId.empty()
                           ? juce::String ("nothing picked")
                           : juce::String (inspection.cueName.empty() ? inspection.cueId
                                                                      : inspection.cueName)
                               + "   " + juce::String (inspection.kind),
                         juce::dontSendNotification);

        detailsButton.setVisible (! inspection.details.empty());

        const auto addLine = [this] (const model::Field& field, bool detail)
        {
            auto line = std::make_unique<Line>();
            line->field = field;
            line->isDetail = detail;

            line->name.setText (juce::String (field.label.empty() ? field.name : field.label),
                                juce::dontSendNotification);
            line->name.setTooltip (juce::String (field.description));
            content.addAndMakeVisible (line->name);

            if (field.boolean && field.writable)
            {
                line->toggle.setToggleState (field.value == "true", juce::dontSendNotification);
                line->toggle.setWantsKeyboardFocus (false);

                const auto address = field.address;
                auto* raw = line.get();

                line->toggle.onClick = [this, address, raw]
                {
                    if (actions.set)
                        actions.set (address, raw->toggle.getToggleState() ? "true" : "false");
                };

                content.addAndMakeVisible (line->toggle);
            }
            else if (! field.options.empty() && field.writable)
            {
                /*  A CLOSED SET IS A CHOICE AND NOT TYPING. The tree publishes
                    the legal values, so the window offers exactly those and a
                    typo becomes impossible rather than refused. */
                auto at = 1;

                for (const auto& option : field.options)
                    line->choice.addItem (juce::String (option), at++);

                line->choice.setText (juce::String (field.value), juce::dontSendNotification);
                line->choice.setWantsKeyboardFocus (false);

                const auto address = field.address;
                auto* raw = line.get();

                line->choice.onChange = [this, address, raw]
                {
                    if (actions.set)
                        actions.set (address, raw->choice.getText().toStdString());
                };

                content.addAndMakeVisible (line->choice);
            }
            else if (field.control == model::Control::loopCount)
            {
                const auto count = loopsFrom (field.value);

                line->repeats.setToggleState (count != loopsOnce, juce::dontSendNotification);
                line->forever.setToggleState (count == loopsForever, juce::dontSendNotification);
                line->box.setText (juce::String (count > loopsOnce ? count : 2),
                                   juce::dontSendNotification);
                line->box.setEditable (true, true, false);

                for (auto* button : { &line->repeats, &line->forever })
                    button->setWantsKeyboardFocus (false);

                const auto address = field.address;
                auto* raw = line.get();

                /*  WHAT THE THREE SAY TOGETHER, in one place so that no pair of
                    them can mean something the third contradicts: not looping
                    is one round, looping for ever is nought, and looping a
                    number of times is that number - never less than two, since
                    "loop once" is what the unchecked box already says. */
                const auto commit = [this, address, raw]
                {
                    const auto typed = juce::jmax (2, raw->box.getText().getIntValue());

                    /*  `wanted` and not `count`, which is the value this row
                        was DRAWN with and is still in scope: GCC's -Wshadow is
                        right that two of them one inside the other is a
                        reader's trap, whatever the compiler this box has says. */
                    const auto wanted = ! raw->repeats.getToggleState() ? loopsOnce
                                      : raw->forever.getToggleState()   ? loopsForever
                                                                        : typed;

                    raw->forever.setEnabled (raw->repeats.getToggleState());
                    raw->box.setVisible (raw->repeats.getToggleState()
                                           && ! raw->forever.getToggleState());

                    if (actions.set)
                        actions.set (address, std::to_string (wanted));
                };

                line->repeats.onClick = commit;
                line->forever.onClick = commit;
                line->box.onTextChange = commit;

                line->forever.setEnabled (line->repeats.getToggleState());

                content.addAndMakeVisible (line->repeats);
                content.addAndMakeVisible (line->forever);
                content.addAndMakeVisible (line->box);
            }
            else
            {
                line->box.setText (juce::String (field.value), juce::dontSendNotification);

                /*  AND A WAY TO GO LOOKING, beside the box and never instead of
                    it. `JUCE_MODAL_LOOPS_PERMITTED` is 0 here, so the chooser
                    is launched and answered later; the window owns it, because
                    a panel rebuilt while a dialogue is open would take its
                    owner with it. */
                if (field.control == model::Control::file && field.writable)
                {
                    const auto id = drawnCue;   // set above, and what this panel is about

                    line->browse.setWantsKeyboardFocus (false);
                    line->browse.setTooltip ("Choose the media this cue plays");
                    line->browse.onClick = [this, id]
                    {
                        if (actions.chooseFile)
                            actions.chooseFile (id);
                    };

                    content.addAndMakeVisible (line->browse);
                }

                /*  ONE CLICK, NOT TWO. This wanted a double-click at first, and
                    the author met that the way anybody would: they picked a
                    group, typed a loop count into it, pressed Return and
                    watched the show loop for ever - because no editor had ever
                    opened and nothing was sent. The log said so plainly: not
                    one `node.set` for that field, while the `selection` row
                    beside it worked, because a closed set of values is a
                    dropdown and a dropdown opens on one click.

                    A PANEL WHOSE FIELDS NEED A DIFFERENT NUMBER OF CLICKS
                    DEPENDING ON THEIR TYPE is a panel nobody can learn. The
                    page's are live inputs; so are these. */
                line->box.setEditable (field.writable, field.writable, false);
                line->box.setTooltip (juce::String (field.description)
                                        + (field.unit.empty() ? juce::String()
                                                              : "  (" + juce::String (field.unit) + ")"));

                if (field.writable)
                {
                    const auto address = field.address;
                    auto* raw = line.get();

                    /*  ON COMMIT, NOT ON EVERY KEYSTROKE: `onTextChange` fires
                        when the editor is dismissed, which is Return or the
                        focus leaving - so a cue is not renamed letter by
                        letter down the log. */
                    line->box.onTextChange = [this, address, raw]
                    {
                        if (actions.set)
                            actions.set (address, raw->box.getText().toStdString());
                    };
                }

                content.addAndMakeVisible (line->box);
            }

            lines.push_back (std::move (line));
        };

        for (const auto& block : inspection.blocks)
        {
            auto head = std::make_unique<Line>();
            head->isHeading = true;
            head->headingText = juce::String (block.heading).toUpperCase();
            head->name.setText (head->headingText, juce::dontSendNotification);
            content.addAndMakeVisible (head->name);
            lines.push_back (std::move (head));

            for (const auto& field : block.fields)
                addLine (field, false);
        }

        for (const auto& field : inspection.details)
            addLine (field, true);

        applyTheme (theme);
        layOut();
    }

    void InspectorComponent::layOut()
    {
        const auto row = rowHeight();
        const auto pad = row / 3;
        const auto width = juce::jmax (120, viewport.getWidth() - 16);
        const auto nameWidth = juce::jmax (60, width * 2 / 5);

        auto y = pad;
        auto detailsPlaced = false;

        for (auto& line : lines)
        {
            /*  The details button heads the first detail line, open or shut,
                so it is always just under the last ordinary field. */
            if (line->isDetail && ! detailsPlaced && detailsButton.isVisible())
            {
                detailsButton.setBounds (pad, y + pad / 2, juce::jmin (width, row * 4), row);
                y += row + pad;
                detailsPlaced = true;
            }

            const auto hidden = line->isDetail && ! detailsOpen;

            line->name.setVisible (! hidden);
            /*  ASKED OF THE CONTROL AND NOT OF THE OTHER COMPONENTS' VISIBILITY,
                which is what this used to do and was a pass behind: each row
                knows which control it is, so each says so directly. */
            line->box.setVisible (! hidden && ! line->isHeading
                                    && line->field.control != model::Control::toggle
                                    && line->field.control != model::Control::choice);
            line->toggle.setVisible (! hidden && line->toggle.getParentComponent() != nullptr
                                       && line->field.control == model::Control::toggle);
            line->choice.setVisible (! hidden && line->choice.getParentComponent() != nullptr
                                       && line->field.control == model::Control::choice);
            line->repeats.setVisible (! hidden
                                        && line->field.control == model::Control::loopCount);
            line->forever.setVisible (! hidden
                                        && line->field.control == model::Control::loopCount);
            line->browse.setVisible (! hidden && line->browse.getParentComponent() != nullptr
                                       && line->field.control == model::Control::file);

            if (hidden)
                continue;

            if (line->isHeading)
            {
                line->name.setBounds (pad, y + pad, width, row);
                y += row + pad;
                continue;
            }

            line->name.setBounds (pad, y, nameWidth, row);

            auto boxArea = juce::Rectangle<int> (pad + nameWidth, y,
                                                 width - nameWidth - pad, row).reduced (1);

            if (line->field.control == model::Control::loopCount)
            {
                /*  Loop, for ever, and how many - in the order they are asked,
                    with the count last because it is the one that only
                    sometimes applies and is hidden when it does not. */
                const auto third = boxArea.getWidth() / 3;

                line->repeats.setBounds (boxArea.removeFromLeft (third));
                line->forever.setBounds (boxArea.removeFromLeft (third));
                line->box.setBounds (boxArea);
                line->box.setVisible (line->repeats.getToggleState()
                                        && ! line->forever.getToggleState());
            }
            else
            {
                if (line->browse.isVisible())
                    line->browse.setBounds (boxArea.removeFromRight (juce::jmin (row + row / 2,
                                                                                 boxArea.getWidth() / 3)));

                line->box.setBounds (boxArea);
                line->toggle.setBounds (boxArea);
                line->choice.setBounds (boxArea);
            }

            y += row;
        }

        content.setSize (juce::jmax (width, viewport.getWidth()), y + pad);
    }

    void InspectorComponent::paint (juce::Graphics& g)
    {
        /*  ITS OWN GROUND, which is the page's `--panel-inspect`: the pane an
            operator is reading about ONE thing should not look like the pane
            listing all of them. */
        g.fillAll (Look::colour (theme, "panel-inspect"));

        g.setColour (Look::colour (theme, "rule"));
        g.fillRect (0, 0, 1, getHeight());
    }

    void InspectorComponent::resized()
    {
        const auto row = rowHeight();

        auto area = getLocalBounds().reduced (row / 3, 0);

        auto top = area.removeFromTop (row + row / 3).withTrimmedTop (row / 3);
        closeButton.setBounds (top.removeFromRight (row).reduced (2));
        heading.setBounds (top);

        viewport.setBounds (area);
        layOut();
    }
}
