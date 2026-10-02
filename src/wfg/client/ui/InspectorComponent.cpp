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

#include <algorithm>
#include <utility>

namespace wfg::client::ui
{
    /*  A DRAWER'S HEAD, drawn rather than made of a label and a button: one
        thing to press, carrying three tellings of one state - the twist's
        direction, the rows under it or not, and while shut how many there are
        (§4.8: a shape and a number, never a colour alone). The icon says which
        drawer it is before the word does. */
    class InspectorComponent::DrawerHead final : public juce::Component,
                                                 public juce::SettableTooltipClient
    {
    public:
        explicit DrawerHead (const model::Theme& themeToUse) : look (themeToUse)
        {
            setWantsKeyboardFocus (false);
            setMouseClickGrabsKeyboardFocus (false);
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
        }

        void set (const juce::String& wordToUse, model::Icon iconToUse, const std::string& accentToUse,
                  bool shutToUse, int countToUse, bool idleToUse)
        {
            if (wordToUse == word && iconToUse == icon && accentToUse == accent && shutToUse == shut
                  && countToUse == count && idleToUse == idle)
                return;

            word = wordToUse;
            icon = iconToUse;
            accent = accentToUse;
            shut = shutToUse;
            count = countToUse;
            idle = idleToUse;

            setTooltip (shut ? "Opens this drawer" : "Shuts this drawer");
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            auto area = getLocalBounds();
            const auto height = area.getHeight();

            //  A band, and a rule along its top: a drawer reads as the front of something.
            g.setColour (Look::colour (look, "panel-high").withAlpha (over ? 0.95f : 0.6f));
            g.fillRect (area);
            g.setColour (Look::colour (look, "rule"));
            g.fillRect (area.removeFromTop (1));

            area.removeFromLeft (height / 5);

            /*  THE TWIST, DRAWN: right when shut, down when open, as every
                other thing in this window that opens does. */
            {
                const auto twist = area.removeFromLeft (height * 2 / 3).toFloat();
                const auto side = static_cast<float> (height) * 0.2f;
                const auto c = twist.getCentre();

                juce::Path triangle;

                if (shut)
                    triangle.addTriangle (c.x - side * 0.6f, c.y - side, c.x - side * 0.6f, c.y + side,
                                          c.x + side * 0.9f, c.y);
                else
                    triangle.addTriangle (c.x - side, c.y - side * 0.6f, c.x + side, c.y - side * 0.6f,
                                          c.x, c.y + side * 0.9f);

                g.setColour (Look::colour (look, "ink-dim"));
                g.fillPath (triangle);
            }

            const auto iconSide = static_cast<float> (height) * 0.62f;
            icons::draw (g, icon, area.removeFromLeft (height).toFloat().withSizeKeepingCentre (iconSide, iconSide),
                         Look::colour (look, accent.c_str()));

            area.removeFromLeft (height / 6);

            const auto font = Look::font (look, 11.5f);
            const auto text = word.toUpperCase();

            g.setFont (font);
            g.setColour (Look::colour (look, idle ? "ink-off" : "ink-dim"));
            g.drawText (text, area, juce::Justification::centredLeft, true);

            /*  SHUT, IT SAYS HOW MUCH IS IN IT - and when none of it means
                anything for this cue, says that too, which is why it shut. */
            if (shut && count > 0)
            {
                area.removeFromLeft (juce::GlyphArrangement::getStringWidthInt (font, text) + height / 3);

                g.setFont (Look::font (look, 11.0f));
                g.setColour (Look::colour (look, "ink-off"));
                g.drawText (juce::String (count) + (count == 1 ? " row" : " rows")
                              + (idle ? juce::String (", not for this cue") : juce::String()),
                            area, juce::Justification::centredLeft, true);
            }
        }

        void mouseEnter (const juce::MouseEvent&) override { over = true; repaint(); }
        void mouseExit (const juce::MouseEvent&) override { over = false; repaint(); }


    private:
        const model::Theme& look;
        juce::String word;
        model::Icon icon = model::Icon::none;
        std::string accent = "ink-faint";
        bool shut = false, idle = false, over = false;
        int count = 0;
    };

    /*  ONE ROW OF THE PANEL: a name, and whatever the node says its value
        should be edited with. A heading carries no field and draws itself. */
    struct InspectorComponent::Line
    {
        model::Field field;
        bool isHeading = false;
        bool isDetail = false;
        juce::String headingText;

        /*  WHICH DRAWER THE LINE IS IN - its block's heading, or "details" -
            and, for a heading, the drawer's own head. */
        std::string drawer;
        std::unique_ptr<DrawerHead> head;

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

        /*  The direct-out menu's other half: a door to the send mixer, beside
            the output the cue lands on rather than on a row of its own
            (author, 2026-09-22: "add a button next to this drop down menu").
            The two belong together - where a cue goes, and how much of it goes
            everywhere else, are one question asked twice. */
        juce::TextButton sends { "Sends" };

        /*  A DOOR RATHER THAN A DECISION: the button that opens the panel at
            the foot on this cue. It spans the row instead of sitting in the
            value column, because everything in that column is a number or a
            word the document keeps and this is neither. */
        juce::TextButton opener;

        /*  THE TALL BOX for a field written at length - notes - several
            lines, wrapped, Return starting a new one, committed when the
            focus leaves (author, 2026-09-18: "the edit field in the
            inspector is too limited for this"). */
        juce::TextEditor editor;
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

        /*  THE FOLD'S HEAD LIVES AMONG THE LINES, not at the foot of the
            pane: the author found it "sitting at the bottom of the window"
            (2026-09-18), a screen away from the fields it folds. It stands
            where the details begin, so opening it puts them right under it.
            Since 2026-09-30 it is one drawer among the others, drawn as they
            are and twisting the way they do. */
        detailsHead = std::make_unique<DrawerHead> (theme);
        content.addAndMakeVisible (*detailsHead);

        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);

        /*  EVERY PRESS ON A LINE IS HEARD HERE TOO, before the box takes it
            to edit: a click or a touch on a number is what puts it on the
            master dial (mouseDown, below). */
        content.addMouseListener (this, true);

        applyTheme (theme);
    }

    juce::String InspectorComponent::nameOf (const model::Field& field) const
    {
        const auto name = juce::String (field.label.empty() ? field.name : field.label);

        if (dialed.empty() || field.address != dialed)
            return name;

        return juce::String (juce::CharPointer_UTF8 ("\xe2\x97\x89 ")) + name;
    }

    void InspectorComponent::markDial()
    {
        for (auto& line : lines)
        {
            if (line->isHeading)
                continue;

            const auto on = ! dialed.empty() && line->field.address == dialed;
            line->name.setText (nameOf (line->field), juce::dontSendNotification);
            line->box.setColour (juce::Label::outlineColourId,
                                 on ? Look::colour (theme, "picked") : juce::Colours::transparentBlack);
        }
    }

    void InspectorComponent::showDial (const std::string& address)
    {
        if (address == dialed)
            return;

        dialed = address;
        markDial();
    }

    /*  A PRESS ON A LINE'S NAME OR ITS VALUE puts that number on the master
        dial (author, 2026-09-26: "any click or touch"), and anything else here
        is left to the control it landed on. Only a number the dial can turn
        is sent; the window decides nothing else about it. */
    void InspectorComponent::mouseDown (const juce::MouseEvent& event)
    {
        pressedOn (event.eventComponent);
    }

    void InspectorComponent::pressedOn (const juce::Component* hit)
    {
        if (hit == nullptr)
            return;

        /*  A PRESS ON A DRAWER'S HEAD opens or shuts it - heard here, with
            every other press on the panel, rather than by the head itself,
            so a test can press one as it presses a number. */
        if (hit == detailsHead.get())
        {
            toggleDrawer ("details");
            return;
        }

        for (auto& line : lines)
            if (line->head != nullptr && hit == line->head.get())
            {
                toggleDrawer (line->drawer);
                return;
            }

        for (auto& line : lines)
        {
            if (hit != &line->name && hit != &line->box && ! line->box.isParentOf (hit))
                continue;

            if (actions.dial && model::mayDial (line->field))
                actions.dial (line->field.address);

            return;
        }
    }

    InspectorComponent::~InspectorComponent() = default;

    int InspectorComponent::rowHeight() const noexcept
    {
        return juce::roundToInt (theme.row * theme.type);
    }

    //==========================================================================
    //  The drawers
    bool InspectorComponent::isShut (const std::string& drawer) const
    {
        if (const auto chosen = drawerChosen.find (drawer); chosen != drawerChosen.end())
            return chosen->second;

        //  The details start shut, as the fold always did.
        if (drawer == "details")
            return true;

        /*  A DRAWER NOBODY HAS TOUCHED IS OPEN, unless not one of its rows
            means anything for this cue - greyed, every one - when it starts
            shut and says so on its head. Opening it by hand keeps it open. */
        auto rows = 0, idle = 0;

        for (const auto& line : lines)
            if (! line->isHeading && line->drawer == drawer)
            {
                ++rows;

                if (! line->field.applies)
                    ++idle;
            }

        return rows > 0 && idle == rows;
    }

    void InspectorComponent::toggleDrawer (const std::string& drawer)
    {
        drawerChosen[drawer] = ! isShut (drawer);
        layOut();
    }

    std::string InspectorComponent::drawerStates() const
    {
        std::string out;

        for (const auto& line : lines)
        {
            if (line->isHeading)
                out += line->drawer + (isShut (line->drawer) ? "=shut;" : "=open;");
            else if (! line->field.applies)
                out += "~";     // a row that went grey can change what a shut head says
        }

        return out + (isShut ("details") ? "details=shut" : "details=open");
    }

    //==========================================================================
    //  The panel bar
    int InspectorComponent::panelBarHeight() const noexcept
    {
        const auto row = rowHeight();
        return panelButtons.empty() ? 0 : row + row / 3;
    }

    void InspectorComponent::rebuildPanels (const std::vector<model::Field>& panels)
    {
        std::vector<std::string> words;

        for (const auto& panel : panels)
            words.push_back (panel.value);

        if (words == panelWords)
            return;

        for (auto& button : panelButtons)
            removeChildComponent (button.get());

        panelButtons.clear();
        panelWords = words;

        for (const auto& panel : panels)
        {
            /*  THE WORD IS THE LABEL'S FIRST: "Waveform, in and out points"
                reads "Waveform" on the button and in full on the tooltip. */
            const auto label = juce::String (panel.label);
            const auto word = label.upToFirstOccurrenceOf (",", false, false).trim();

            auto button = std::make_unique<IconButton> (model::iconForPanel (panel.value), word);
            button->setTooltip ("Opens at the foot of the window, on this cue: " + label
                                  + ". Pressed again, shuts it.");

            //  The subject is the button's; which cue it opens on is read when pressed.
            button->onClick = [this, subject = panel.value]
            {
                if (actions.openPanel)
                    actions.openPanel (drawnCue, subject);
            };

            addAndMakeVisible (*button);
            panelButtons.push_back (std::move (button));
        }

        applyTheme (theme);
    }

    void InspectorComponent::lightPanels()
    {
        for (std::size_t at = 0; at < panelButtons.size() && at < panelWords.size(); ++at)
            panelButtons[at]->setToggleState (! footWord.empty() && panelWords[at] == footWord
                                                && ! drawnCue.empty() && footCue == drawnCue,
                                              juce::dontSendNotification);
    }

    void InspectorComponent::showFoot (const std::string& subject, const std::string& cueId)
    {
        if (subject == footWord && cueId == footCue)
            return;

        footWord = subject;
        footCue = cueId;
        lightPanels();
    }

    //==========================================================================
    //  The head
    void InspectorComponent::readHead (const model::Inspection& inspection)
    {
        const auto valueOf = [&inspection] (const char* name)
        {
            for (const auto& block : inspection.blocks)
                for (const auto& field : block.fields)
                    if (field.name == name)
                        return field.mixed ? std::string {} : field.value;

            return std::string {};
        };

        const auto icon = model::iconFor (inspection.kind, valueOf ("mode"), valueOf ("verb"));
        const auto accent = model::accentFor (inspection.kind);
        const auto colour = valueOf ("colour");

        if (icon == headIcon && accent == headAccent && colour == headColour)
            return;

        headIcon = icon;
        headAccent = accent;
        headColour = colour;
        repaint (headIconBox.expanded (8));
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

        /*  THE PANEL BAR IN THE CUE'S ACCENT, so the bar over a media cue and
            the bar over a fade are told apart at the same glance the icon in
            the head is. */
        for (auto& button : panelButtons)
        {
            button->setColours (Look::colour (theme, "ink"), Look::colour (theme, "panel-high"),
                                Look::colour (theme, headAccent.empty() ? "ink-faint" : headAccent.c_str()));
            button->setTextHeight (Look::font (theme, 12.0f).getHeight());
        }

        resized();
        repaint();
    }

    void InspectorComponent::show (const model::Inspection& inspection)
    {
        /*  THE HEAD AND THE PANEL BAR FIRST, whichever way the rows go: both
            are about which cue this is, not about its rows. */
        const auto accentWas = headAccent;
        readHead (inspection);
        rebuildPanels (inspection.panels);

        if (headAccent != accentWas)
            applyTheme (theme);

        /*  REBUILT ONLY WHEN THE CUE OR ITS SHAPE CHANGES. A value moving is a
            `setText` on a line that already exists; a different cue is a
            different panel. Counting the fields catches the case that matters
            in between - a kind changing under the same identifier, which adds
            and removes rows. */
        if (shapeOf (inspection) != drawnShape)
        {
            rebuild (inspection);
            lightPanels();
            return;
        }

        /*  THE SAME PANEL, POINTED SOMEWHERE ELSE. Everything a row writes is
            addressed, and the address is the cue's - so the identifier moves
            here, before the values do, and the buttons read it when pressed. */
        drawnCue = inspection.cueId;
        lightPanels();

        heading.setText (headingFor (inspection), juce::dontSendNotification);

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
                    /*  A WORDED CHOICE CAN CHANGE ITS WORDS while the cue stays
                        picked - Doh!'s first item says what the device says
                        now - so its menu is compared and refilled as the
                        show's menus below are, and selected by key. */
                    if (! field.choices.empty())
                    {
                        if (line.field.choices != field.choices)
                        {
                            line.choice.clear (juce::dontSendNotification);

                            auto item = 1;

                            for (const auto& choice : field.choices)
                                line.choice.addItem (juce::String (choice.second), item++);
                        }

                        line.choice.setSelectedId (idForChoice (field), juce::dontSendNotification);
                        break;
                    }

                    line.choice.setText (shown (field), juce::dontSendNotification);
                    break;

                case model::Control::busRef:
                case model::Control::deviceRef:
                case model::Control::portRef:
                case model::Control::dcaRef:
                case model::Control::stripRef:
                case model::Control::inputRef:
                case model::Control::channelRef:
                    /*  THE ITEMS THEMSELVES CAN HAVE MOVED, which no other
                        control here has to think about: a `choice`'s options
                        come from the parameter table and are fixed for the
                        life of the program, while these come from the SHOW -
                        an output renamed, added, deleted, or its mark changing
                        from free to taken as a cue is moved; a DCA made or
                        renamed in the Surfaces tab while this cue stays picked.
                        The field count does not change when any of that
                        happens, so the panel is not rebuilt and the menu would
                        go on offering last week's names.

                        Compared rather than repopulated blindly, because
                        refilling a ComboBox twenty-five times a second would
                        shut it under anybody trying to use it. */
                    if (line.field.choices != field.choices)
                    {
                        line.field.choices = field.choices;
                        line.choice.clear (juce::dontSendNotification);

                        //  Named `item` and not `at`: this panel already has one.
                        auto item = 1;

                        for (const auto& choice : field.choices)
                            line.choice.addItem (juce::String (choice.second), item++);
                    }

                    line.choice.setSelectedId (idForChoice (field), juce::dontSendNotification);
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

                case model::Control::longText:
                    /*  NOT WHILE SOMEBODY IS TYPING IN IT: the poll arrives up
                        to twenty-five times a second, and a note half-written
                        put back to what the tree says is the one thing this
                        panel must not do. */
                    if (! line.editor.hasKeyboardFocus (true))
                        line.editor.setText (shown (field), juce::dontSendNotification);

                    line.box.setText (shown (field), juce::dontSendNotification);
                    break;

                case model::Control::opener:
                    //  Nothing to poll: it holds no value and writes none.
                    break;

                case model::Control::file:
                case model::Control::cueRef:
                case model::Control::text:
                default:
                    line.box.setText (shown (field), juce::dontSendNotification);
                    break;
            }

            /*  THE WHOLE ROW AND NOT ONLY ITS VALUE, now that a line outlives
                the cue it was built for: the address is what a commit writes
                to, `choices` is the menu of a rig that may have changed, and
                `applies` is whether this cue is one the row means anything
                for. Keeping the value and dropping the rest would be a panel
                that showed one cue and wrote to another. */
            const auto wasControl = line.field.control;

            line.field = field;
            line.field.control = wasControl;

            /*  AND ITS NAME, which until 2026-09-22 nothing here touched
                because a row's name could not change: `shapeOf` keys on it,
                so a different name meant a different panel and a rebuild.

                A MIDI cue broke that. What its two payload rows are CALLED now
                follows the message type - note and velocity, controller and
                value, program - which is a value, and a value is exactly what
                this refill exists to carry. Without this line the panel showed
                the first cue's words over the second cue's numbers: pick two
                program changes and a note-on and the note-on's labels stuck to
                everything after it (author, looking at it). */
            line.name.setText (nameOf (field), juce::dontSendNotification);
            line.name.setTooltip (juce::String (field.description));
            line.box.setColour (juce::Label::outlineColourId,
                                ! dialed.empty() && field.address == dialed
                                    ? Look::colour (theme, "picked") : juce::Colours::transparentBlack);

            /*  AND WHETHER THE ROW MEANS ANYTHING FOR THIS CUE. Set in the
                layout as well, which is where it was set alone - and a refill
                does not resize, so a panel that only swapped values left a
                dead row live and a live row dead. */
            line.toggle.setEnabled (field.applies);
            line.choice.setEnabled (field.applies);
            line.box.setEnabled (field.applies);
        };

        for (const auto& block : inspection.blocks)
            for (const auto& field : block.fields)
                update (field);

        for (const auto& field : inspection.details)
            update (field);

        /*  A ROW GOING GREY CAN SHUT A DRAWER NOBODY TOUCHED, or open one:
            picking a sampler member after a cue outside any group keeps the
            panel and changes what its sampler rows mean. Relaid only then. */
        if (drawerStates() != laidDrawers)
            layOut();
    }

    void InspectorComponent::commitField (const model::Field& field, const std::string& text)
    {
        if (! actions.set)
            return;

        if (field.addresses.empty())
        {
            actions.set (field.address, text);
            return;
        }

        for (const auto& address : field.addresses)
            actions.set (address, text);
    }

    void InspectorComponent::commitCueRef (const model::Field& field, const std::string& text)
    {
        if (! actions.setCueRef)
            return;

        if (field.addresses.empty())
        {
            actions.setCueRef (field.address, text);
            return;
        }

        for (const auto& address : field.addresses)
            actions.setCueRef (address, text);
    }

    juce::String InspectorComponent::shown (const model::Field& field)
    {
        /*  "(mixed)" IN THE BOX rather than an empty one: an empty box says
            "nothing", and what is true is that the cues say different things.
            Typing over it writes the one value to all of them. */
        return field.mixed ? juce::String ("(mixed)") : juce::String (field.value);
    }

    int InspectorComponent::idForChoice (const model::Field& field)
    {
        /*  CUES THAT DISAGREE SELECT NOTHING, which the comment at the foot
            always said and the loop below could not do: a mixed field's value
            is empty, and empty is exactly the key "(none)" has - so eight cues
            on three DCAs read as eight cues on none, and picking "(none)" to
            clear them all sent nothing, the menu thinking it was already
            there. Asked first, before any key can match. */
        if (field.mixed)
            return 0;

        for (std::size_t at = 0; at < field.choices.size(); ++at)
            if (field.choices[at].first == field.value)
                return static_cast<int> (at) + 1;

        /*  NOTHING SELECTED rather than the first item, which would be this
            panel quietly telling somebody their cue plays out of an output
            they never chose. A mixed selection lands here too, and an empty
            menu is the honest drawing of "they do not agree". */
        return 0;
    }

    juce::String InspectorComponent::headingFor (const model::Inspection& inspection)
    {
        if (inspection.cueId.empty())
            return "nothing picked";

        return juce::String (inspection.cueName.empty() ? inspection.cueId : inspection.cueName)
                 + "   " + juce::String (inspection.kind);
    }

    std::string InspectorComponent::shapeOf (const model::Inspection& inspection)
    {
        /*  WHAT WOULD HAVE TO BE BUILT DIFFERENTLY, and nothing else: a row's
            name, which control draws it, and whether it can be written. A
            value is not in it, and neither is the cue - those are what the
            refill is for. */
        std::string out;

        const auto add = [&out] (const model::Field& field)
        {
            out += field.name + ":" + std::to_string (static_cast<int> (field.control))
                     + (field.writable ? "w;" : "r;");
        };

        for (const auto& block : inspection.blocks)
        {
            out += "[" + block.heading + "]";

            for (const auto& field : block.fields)
                add (field);
        }

        out += "|";

        for (const auto& field : inspection.details)
            add (field);

        return out;
    }

    void InspectorComponent::rebuild (const model::Inspection& inspection)
    {
        lines.clear();
        content.removeAllChildren();

        /*  The fold's head lives among the lines (above), so clearing them
            took it too - and it vanished the first time a cue was picked
            (author, 2026-09-18: "the detail button has disappeared"). Put
            back before the lines are, so it is there for `layOut` to place. */
        content.addAndMakeVisible (*detailsHead);

        drawnCue = inspection.cueId;
        drawnShape = shapeOf (inspection);
        drawnKind = inspection.kind;

        heading.setText (headingFor (inspection),
                         juce::dontSendNotification);

        detailsHead->setVisible (! inspection.details.empty());

        const auto addLine = [this] (const model::Field& field, bool detail, const std::string& drawer)
        {
            auto line = std::make_unique<Line>();
            line->field = field;
            line->isDetail = detail;
            line->drawer = drawer;

            line->name.setText (nameOf (field), juce::dontSendNotification);
            line->name.setTooltip (juce::String (field.description));
            content.addAndMakeVisible (line->name);

            if (field.control == model::Control::opener)
            {
                /*  THE WHOLE ROW IS THE BUTTON, and the name label steps aside
                    - there is no value to line up against, and a label plus a
                    button reading "Open" would be two things to read where one
                    will do. */
                line->name.setVisible (false);

                line->opener.setButtonText (juce::String (field.label));
                line->opener.setWantsKeyboardFocus (false);
                line->opener.setTooltip ("Opens at the foot of the window, on this cue");

                /*  THE SUBJECT IS REMEMBERED AND THE CUE IS NOT. Which panel
                    this row opens is a property of the row and cannot change
                    while the row exists; WHICH cue it opens on is whatever is
                    picked now, because two cues of one kind share these lines. */
                const auto subject = field.value;

                line->opener.onClick = [this, subject]
                {
                    if (actions.openPanel)
                        actions.openPanel (drawnCue, subject);
                };

                content.addAndMakeVisible (line->opener);
            }
            else if (field.boolean && field.writable)
            {
                line->toggle.setToggleState (field.value == "true", juce::dontSendNotification);
                line->toggle.setWantsKeyboardFocus (false);

                auto* raw = line.get();

                line->toggle.onClick = [this, raw]
                {
                    commitField (raw->field, raw->toggle.getToggleState() ? "true" : "false");
                };

                content.addAndMakeVisible (line->toggle);
            }
            else if (! field.options.empty() && field.writable && ! field.choices.empty())
            {
                /*  A CLOSED SET THE MODEL PUTS IN WORDS (Doh!'s row, PRD
                    §3.32, 2026-10-01): the menu reads the words and writes their
                    keys, which are still exactly the values the tree declares -
                    the device menus' way of committing, by position. */
                auto at = 1;

                for (const auto& choice : field.choices)
                    line->choice.addItem (juce::String (choice.second), at++);

                line->choice.setSelectedId (idForChoice (field), juce::dontSendNotification);
                line->choice.setWantsKeyboardFocus (false);

                auto* raw = line.get();

                line->choice.onChange = [this, raw]
                {
                    const auto at_ = raw->choice.getSelectedId() - 1;

                    if (at_ >= 0 && at_ < static_cast<int> (raw->field.choices.size()))
                        commitField (raw->field,
                                     raw->field.choices[static_cast<std::size_t> (at_)].first);
                };

                content.addAndMakeVisible (line->choice);
            }
            else if (! field.options.empty() && field.writable)
            {
                /*  A CLOSED SET IS A CHOICE AND NOT TYPING. The tree publishes
                    the legal values, so the window offers exactly those and a
                    typo becomes impossible rather than refused. */
                auto at = 1;

                for (const auto& option : field.options)
                    line->choice.addItem (juce::String (option), at++);

                line->choice.setText (shown (field), juce::dontSendNotification);
                line->choice.setWantsKeyboardFocus (false);

                auto* raw = line.get();

                line->choice.onChange = [this, raw]
                {
                    commitField (raw->field, raw->choice.getText().toStdString());
                };

                content.addAndMakeVisible (line->choice);
            }
            else if ((field.control == model::Control::busRef
                        || field.control == model::Control::deviceRef
                        || field.control == model::Control::portRef
                        || field.control == model::Control::dcaRef
                        || field.control == model::Control::stripRef
                        || field.control == model::Control::inputRef
                        || field.control == model::Control::channelRef)
                       && field.writable)
            {
                /*  A MENU THE SHOW WROTE, not one the parameter table
                    declares: the legal values are the outputs of THIS rig, so
                    they arrive with the reading rather than with the schema.
                    The item's index is the position in `choices`, which is how
                    the identifier is found again on the way back - the name is
                    what a designer reads and never what gets written. A DCA's
                    menu is the same menu over a different list, and commits
                    the same way: the identifier, to the row it is drawn on,
                    once per picked cue. */
                auto at = 1;

                for (const auto& choice : field.choices)
                    line->choice.addItem (juce::String (choice.second), at++);

                line->choice.setSelectedId (idForChoice (field), juce::dontSendNotification);
                line->choice.setWantsKeyboardFocus (false);

                auto* raw = line.get();

                line->choice.onChange = [this, raw]
                {
                    const auto at_ = raw->choice.getSelectedId() - 1;

                    if (at_ >= 0 && at_ < static_cast<int> (raw->field.choices.size()))
                        commitField (raw->field,
                                     raw->field.choices[static_cast<std::size_t> (at_)].first);
                };

                content.addAndMakeVisible (line->choice);

                /*  AND THE DOOR TO THE MIX BESIDE IT - for an OUTPUT only.

                    A DEVICE HAS NO SEND LEVELS. What a network cue writes is
                    one value at one address; a button opening a mixer on it
                    would be a door onto an empty room. The two menus share
                    everything above this line because both are lists the SHOW
                    wrote rather than lists the parameter table declares, and
                    they part company here. */
                if (field.control == model::Control::busRef)
                {
                    /*  It opens on the cue this panel is about, through the
                        same door the waveform opener uses, so the panel at the
                        foot has exactly one way in. */
                    /*  READ WHEN PRESSED AND NOT REMEMBERED. A line now
                        outlives the cue it was built for - two cues of one
                        kind share a panel - so a button holding the identifier
                        it was made with would open the panel on whichever cue
                        happened to be picked when the row was first drawn. */
                    line->sends.setWantsKeyboardFocus (false);
                    line->sends.setTooltip ("Send levels from this cue into the show's mix channels");
                    line->sends.onClick = [this] { if (actions.openPanel) actions.openPanel (drawnCue, "sends"); };

                    content.addAndMakeVisible (line->sends);
                }
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

                auto* raw = line.get();

                /*  WHAT THE THREE SAY TOGETHER, in one place so that no pair of
                    them can mean something the third contradicts: not looping
                    is one round, looping for ever is nought, and looping a
                    number of times is that number - never less than two, since
                    "loop once" is what the unchecked box already says. */
                const auto commit = [this, raw]
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

                    commitField (raw->field, std::to_string (wanted));
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
                line->box.setText (shown (field), juce::dontSendNotification);

                if (field.control == model::Control::longText && field.writable)
                {
                    auto* raw = line.get();

                    line->editor.setMultiLine (true, true);
                    line->editor.setReturnKeyStartsNewLine (true);
                    line->editor.setScrollbarsShown (true);
                    line->editor.setText (shown (field), juce::dontSendNotification);
                    line->editor.setTooltip (juce::String (field.description));

                    /*  COMMITTED WHEN THE FOCUS LEAVES, which is when a note is
                        finished: a note is prose, and a write per line would be
                        a record per sentence down the log. Leaving "(mixed)"
                        as it was writes nothing. */
                    line->editor.onFocusLost = [this, raw]
                    {
                        const auto text = raw->editor.getText().toStdString();

                        if (raw->field.mixed && text == "(mixed)")
                            return;

                        if (text != raw->field.value)
                            commitField (raw->field, text);
                    };

                    content.addAndMakeVisible (line->editor);
                }

                /*  AND A WAY TO GO LOOKING, beside the box and never instead of
                    it. `JUCE_MODAL_LOOPS_PERMITTED` is 0 here, so the chooser
                    is launched and answered later; the window owns it, because
                    a panel rebuilt while a dialogue is open would take its
                    owner with it. */
                if (field.control == model::Control::file && field.writable)
                {
                    //  Read when pressed, for the reason the sends button is.
                    line->browse.setWantsKeyboardFocus (false);
                    line->browse.setTooltip ("Choose the media this cue plays");
                    line->browse.onClick = [this]
                    {
                        if (actions.chooseFile)
                            actions.chooseFile (drawnCue);
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
                                                              : "  (" + juce::String (field.unit) + ")")
                                        + (field.control == model::Control::cueRef
                                             ? "  Type a cue's number, name or identifier, "
                                               "or drag a cue onto this one in the list."
                                             : ""));

                if (field.writable)
                {
                    const auto namesACue = field.control == model::Control::cueRef;
                    auto* raw = line.get();

                    /*  ON COMMIT, NOT ON EVERY KEYSTROKE: `onTextChange` fires
                        when the editor is dismissed, which is Return or the
                        focus leaving - so a cue is not renamed letter by
                        letter down the log.

                        A FIELD THAT NAMES A CUE goes by the other door, where
                        what was typed is resolved to an identifier first. */
                    line->box.onTextChange = [this, raw, namesACue]
                    {
                        const auto text = raw->box.getText().toStdString();

                        //  Leaving "(mixed)" as it was is not a decision, and writes nothing.
                        if (raw->field.mixed && text == "(mixed)")
                            return;

                        if (namesACue)
                            commitCueRef (raw->field, text);
                        else
                            commitField (raw->field, text);
                    };
                }

                content.addAndMakeVisible (line->box);
            }

            lines.push_back (std::move (line));
        };

        for (const auto& block : inspection.blocks)
        {
            /*  EVERY BLOCK IS A DRAWER, headed by what opens and shuts it. The
                head is remembered by the block's heading, so the drawer a hand
                shut stays shut on the next cue that has one. */
            auto head = std::make_unique<Line>();
            head->isHeading = true;
            head->headingText = juce::String (block.heading);
            head->drawer = block.heading;
            head->head = std::make_unique<DrawerHead> (theme);
            content.addAndMakeVisible (*head->head);
            lines.push_back (std::move (head));

            for (const auto& field : block.fields)
                addLine (field, false, block.heading);
        }

        for (const auto& field : inspection.details)
            addLine (field, true, "details");

        applyTheme (theme);
        layOut();
    }

    void InspectorComponent::layOut()
    {
        const auto row = rowHeight();
        const auto pad = row / 3;
        const auto width = juce::jmax (120, viewport.getWidth() - 16);
        const auto nameWidth = juce::jmax (60, width * 2 / 5);

        auto y = pad / 2;
        auto detailsPlaced = false;
        auto shut = false;

        /*  A DRAWER'S HEAD across the whole width, with a little air above it,
            saying whether it is shut, how many rows it holds, and whether any
            of them means anything for this cue. */
        const auto placeHead = [this, &y, &shut, row, pad, width] (DrawerHead& head, const std::string& drawer)
        {
            auto rowsIn = 0, idle = 0;

            for (const auto& other : lines)
                if (! other->isHeading && other->drawer == drawer)
                {
                    ++rowsIn;

                    if (! other->field.applies)
                        ++idle;
                }

            shut = isShut (drawer);

            const auto accent = drawer == "what it does" ? model::accentFor (drawnKind) : std::string ("ink-faint");
            const auto mode = [this]
            {
                for (const auto& other : lines)
                    if (other->field.name == "mode")
                        return other->field.value;

                return std::string {};
            }();

            head.set (juce::String (drawer), model::iconForDrawer (drawer, drawnKind, mode), accent,
                      shut, rowsIn, rowsIn > 0 && idle == rowsIn);

            const auto gap = pad / 2;
            head.setBounds (0, y + gap, width + pad, row);
            y += gap + row + (shut ? 0 : pad / 2);
        };

        for (auto& line : lines)
        {
            /*  The details' head leads the first detail line, open or shut,
                so it is always just under the last ordinary drawer. */
            if (line->isDetail && ! detailsPlaced && detailsHead->isVisible())
            {
                placeHead (*detailsHead, "details");
                detailsPlaced = true;
            }

            if (line->isHeading && line->head != nullptr)
            {
                placeHead (*line->head, line->drawer);
                continue;
            }

            const auto hidden = shut;

            line->name.setVisible (! hidden);
            /*  ASKED OF THE CONTROL AND NOT OF THE OTHER COMPONENTS' VISIBILITY,
                which is what this used to do and was a pass behind: each row
                knows which control it is, so each says so directly. */
            /*  EVERY MENU, and not the first two. The device and port menus
                were built in `rebuild` and then hidden here, because this list
                stopped at the output's: a row drew its name and nothing beside
                it, and a menu nobody could see was the one control on the line.
                The DCA menu would have joined them. */
            const auto isMenu = line->field.control == model::Control::choice
                                  || line->field.control == model::Control::busRef
                                  || line->field.control == model::Control::deviceRef
                                  || line->field.control == model::Control::portRef
                                  || line->field.control == model::Control::dcaRef
                                  || line->field.control == model::Control::stripRef
                                  || line->field.control == model::Control::inputRef
                                  || line->field.control == model::Control::channelRef;

            line->box.setVisible (! hidden && ! line->isHeading
                                    && line->field.control != model::Control::toggle
                                    && ! isMenu);
            line->toggle.setVisible (! hidden && line->toggle.getParentComponent() != nullptr
                                       && line->field.control == model::Control::toggle);
            line->choice.setVisible (! hidden && line->choice.getParentComponent() != nullptr
                                       && isMenu);
            line->sends.setVisible (! hidden && line->sends.getParentComponent() != nullptr
                                      && line->field.control == model::Control::busRef);

            /*  GREYED WHERE THE ROW IS LEGAL BUT MEANS NOTHING FOR THIS CUE -
                a fold on a mono file. Still drawn, still labelled: an absence
                reads as "this program cannot do that", which is a different
                and wrong sentence. */
            line->toggle.setEnabled (line->field.applies);
            line->choice.setEnabled (line->field.applies);

            /*  THE BOX TOO, which it never was: a greyed row whose control is
                a text field - a MIDI cue's velocity under a program change -
                stayed white and typeable, so the one kind of row this rule
                exists for was the one it did not reach. */
            line->box.setEnabled (line->field.applies);
            line->repeats.setVisible (! hidden
                                        && line->field.control == model::Control::loopCount);
            line->forever.setVisible (! hidden
                                        && line->field.control == model::Control::loopCount);
            line->browse.setVisible (! hidden && line->browse.getParentComponent() != nullptr
                                       && line->field.control == model::Control::file);

            const auto opens = line->field.control == model::Control::opener;

            line->opener.setVisible (! hidden && opens);

            if (opens)
            {
                line->name.setVisible (false);
                line->box.setVisible (false);
            }

            /*  The tall box stands in for the one-line box when it exists. */
            const auto tall = line->field.control == model::Control::longText
                                && line->editor.getParentComponent() != nullptr;

            line->editor.setVisible (! hidden && tall);

            if (tall)
                line->box.setVisible (false);

            if (hidden)
                continue;

            if (opens)
            {
                line->opener.setBounds (pad, y + 1, width - pad, row - 2);
                y += row;
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

                /*  Taken off the right of the value column, as the file row's
                    browse button is, so a menu and its door share one line and
                    the panel keeps one column of values. */
                if (line->sends.isVisible())
                    line->sends.setBounds (boxArea.removeFromRight (juce::jmin (row * 3,
                                                                                boxArea.getWidth() / 2)));

                line->box.setBounds (boxArea);
                line->toggle.setBounds (boxArea);
                line->choice.setBounds (boxArea);

                //  Three rows for a note, the name staying on the first.
                if (line->editor.isVisible())
                {
                    line->editor.setBounds (boxArea.withHeight (row * 3 - 2));
                    y += row * 2;
                }
            }

            y += row;
        }

        content.setSize (juce::jmax (width, viewport.getWidth()), y + pad);
        laidDrawers = drawerStates();
    }

    void InspectorComponent::paint (juce::Graphics& g)
    {
        /*  ITS OWN GROUND, which is the page's `--panel-inspect`: the pane an
            operator is reading about ONE thing should not look like the pane
            listing all of them. */
        g.fillAll (Look::colour (theme, "panel-inspect"));

        g.setColour (Look::colour (theme, "rule"));
        g.fillRect (0, 0, 1, getHeight());

        /*  THE CUE'S ICON BEFORE ITS NAME, in its kind's accent - the same
            picture its row wears in the list, so the eye carries one across
            to the other - and its own colour as a tab before that. */
        if (headIcon != model::Icon::none)
            icons::draw (g, headIcon, headIconBox.toFloat().reduced (2.0f),
                         Look::colour (theme, headAccent.c_str()));

        if (const auto own = model::colourFromHex (headColour); own.has_value())
        {
            const auto tab = headIconBox.withX (headIconBox.getX() - 7).withWidth (4)
                                 .withSizeKeepingCentre (4, headIconBox.getHeight() * 3 / 4);

            g.setColour (juce::Colour (static_cast<juce::uint32> (*own)));
            g.fillRoundedRectangle (tab.toFloat(), 2.0f);
        }
    }

    void InspectorComponent::resized()
    {
        const auto row = rowHeight();

        auto area = getLocalBounds().reduced (row / 3, 0);

        auto top = area.removeFromTop (row + row / 3).withTrimmedTop (row / 3);
        closeButton.setBounds (top.removeFromRight (row).reduced (2));
        headIconBox = top.removeFromLeft (row).reduced (2);
        top.removeFromLeft (row / 6);
        heading.setBounds (top);

        /*  THE PANEL BAR, UNDER THE NAME AND OVER THE ROWS - where the eye
            lands first, and out of the scrolling so it is there whichever
            drawer somebody has scrolled to. Three ways to lay it, tried in
            order: equal widths that all carry their words; each as wide as its
            word, the room left shared out; and, where not even that fits,
            equal widths with the pictures alone - every button dropping its
            word together, so the bar never reads half one way and half the
            other. Capped so a wide inspector does not stretch them into slabs. */
        if (! panelButtons.empty())
        {
            auto bar = area.removeFromTop (panelBarHeight()).reduced (0, row / 6);
            const auto count = static_cast<int> (panelButtons.size());
            const auto gap = row / 4;
            const auto room = bar.getWidth() - gap * (count - 1);

            std::vector<int> wanted;
            auto widest = 0, total = 0;

            for (auto& button : panelButtons)
            {
                wanted.push_back (button->idealWidth (bar.getHeight()));
                widest = juce::jmax (widest, wanted.back());
                total += wanted.back();
            }

            std::vector<int> widths;
            auto wordsFit = true;

            if (widest * count <= room)
                widths.assign (panelButtons.size(), juce::jmin (row * 4, room / juce::jmax (1, count)));
            else if (total <= room)
                for (const auto one : wanted)
                    widths.push_back (one + (room - total) / count);
            else
            {
                wordsFit = false;
                widths.assign (panelButtons.size(), room / juce::jmax (1, count));
            }

            for (std::size_t at = 0; at < panelButtons.size(); ++at)
            {
                panelButtons[at]->setWordShown (wordsFit);
                panelButtons[at]->setBounds (bar.removeFromLeft (widths[at]));
                bar.removeFromLeft (gap);
            }
        }

        viewport.setBounds (area);
        layOut();
    }
}
