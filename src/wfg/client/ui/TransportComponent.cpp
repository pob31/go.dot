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

#include <wfg/client/ui/TransportComponent.h>

#include <wfg/client/ui/Look.h>

#include <string>
#include <utility>

namespace wfg::client::ui
{
    namespace
    {
        juce::String text (const std::string& s) { return juce::String (s); }

        /*  WHAT THE BANNER SAYS, and it is the question itself rather than a
            heading over two buttons - which is why neither button asks again
            (the page makes the same argument for its own banner). Shorter than
            the page's: a window has less room than a tab, and the sentences
            that survive are the ones that say what each button DOES. */
        constexpr const char* recoveryText =
            "Unsaved work was found beside this show. An earlier session ended before anybody "
            "saved it, and the engine kept it in the bundle's recovery folder rather than in the "
            "show file.\n"
            "Recover puts that work on screen, changes made here included, and leaves it unsaved. "
            "Discard deletes it for good and changes nothing on screen. Until one is chosen both "
            "are kept, and saving the show does not count as an answer.";
    }

    TransportComponent::TransportComponent (const model::Theme& themeToUse, Actions actionsToUse)
        : actions (std::move (actionsToUse)), theme (themeToUse)
    {
        for (auto* label : { &showLabel, &tickLabel, &clockLabel, &rateLabel,
                             &listLabel, &standbyLabel, &notesLabel, &statusLabel, &errorLabel,
                             &noticeLabel })
        {
            label->setJustificationType (juce::Justification::centredLeft);
            label->setMinimumHorizontalScale (1.0f);
            addAndMakeVisible (label);
        }

        for (auto* label : { &tickLabel, &clockLabel, &rateLabel })
            label->setJustificationType (juce::Justification::centredRight);

        /*  NO BUTTON TAKES THE KEYBOARD, so Space stays this component's. A
            focused button would turn Space into a second route to whichever
            one the focus happened to be resting on - which on this strip could
            be `revert`. */
        struct { juce::TextButton* button; std::function<void()>* action; } wiring[]
        {
            { &goButton,      &actions.go },
            { &panicButton,   &actions.panic },
            { &recoverButton, &actions.recover },
            { &discardButton, &actions.discardRecovery },
        };

        for (auto& [button, action] : wiring)
        {
            button->setWantsKeyboardFocus (false);
            button->onClick = [action] { if (*action) (*action)(); };
            addAndMakeVisible (button);
        }

        /*  THE TWO PRESSED WITHOUT LOOKING ARE READ FROM FURTHEST AWAY
            (author, 2026-09-18): GO at twice the button type, PANIC at one and
            a half. */
        goButton.getProperties().set (Look::fontScale(), 2.0);
        panicButton.getProperties().set (Look::fontScale(), 1.5);

        setWantsKeyboardFocus (true);
        applyTheme (theme);
    }

    int TransportComponent::rowHeight() const noexcept
    {
        return juce::roundToInt (theme.row * theme.type);
    }

    void TransportComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;

        const auto ink = Look::colour (theme, "ink");
        const auto dim = Look::colour (theme, "ink-dim");
        const auto faint = Look::colour (theme, "ink-faint");

        showLabel.setFont (Look::font (theme, 16.0f));
        showLabel.setColour (juce::Label::textColourId, ink);

        for (auto* label : { &tickLabel, &clockLabel, &rateLabel })
        {
            label->setFont (Look::font (theme, 13.0f));
            label->setColour (juce::Label::textColourId, dim);
        }

        listLabel.setFont (Look::font (theme, 13.0f));
        listLabel.setColour (juce::Label::textColourId, faint);

        standbyLabel.setFont (Look::font (theme, 22.0f));
        standbyLabel.setColour (juce::Label::textColourId, Look::colour (theme, "standby"));

        /*  THE NOTES BESIDE THE NAME, in the reading colour and not the
            standby's: what to look out for before GO, wrapped over the two
            rows the name has, since this is the field that is written long. */
        notesLabel.setFont (Look::font (theme, 13.0f));
        notesLabel.setColour (juce::Label::textColourId, Look::colour (theme, "ink-dim"));
        notesLabel.setJustificationType (juce::Justification::topLeft);
        notesLabel.setMinimumHorizontalScale (1.0f);

        /*  PANIC IN THE REFUSAL'S RED, and the word on it carries the meaning
            (§4.8): the colour is for the hand that already knows. The tooltip
            says both levels, because the second is the one nobody reads about
            first. */
        panicButton.setColour (juce::TextButton::buttonColourId, Look::colour (theme, "failed"));
        panicButton.setTooltip ("Esc: every cue stops and the footers run. "
                                "Esc again within a second: everything is dropped, no footers.");
        goButton.setTooltip ("Space: fires the standby cue");

        statusLabel.setFont (Look::font (theme, 13.0f));
        statusLabel.setColour (juce::Label::textColourId, dim);

        errorLabel.setFont (Look::font (theme, 13.0f));
        errorLabel.setColour (juce::Label::textColourId, Look::colour (theme, "failed"));

        noticeLabel.setFont (Look::font (theme, 13.0f));
        noticeLabel.setColour (juce::Label::textColourId, Look::colour (theme, "waiting"));

        resized();
        repaint();
    }

    void TransportComponent::show (const model::TransportReading& reading)
    {
        if (shownOnce && reading == last)
            return;

        const auto wasShowing = bannerShowing;
        bannerShowing = model::isYes (reading.recovery);

        showLabel.setText (text (reading.show)
                             + (reading.dirty == model::Flag::yes ? "   ● unsaved" : ""),
                           juce::dontSendNotification);
        tickLabel.setText ("tick " + text (reading.tick), juce::dontSendNotification);
        clockLabel.setText (text (reading.clock), juce::dontSendNotification);
        rateLabel.setText (text (reading.rate), juce::dontSendNotification);
        listLabel.setText (reading.listId.empty() ? juce::String ("no list")
                                                  : "standby in " + text (reading.listName),
                           juce::dontSendNotification);
        standbyLabel.setText (text (reading.standbyLine()), juce::dontSendNotification);
        notesLabel.setText (text (reading.standbyNotes), juce::dontSendNotification);

        statusLabel.setText (text (reading.lockLine()), juce::dontSendNotification);

        /*  THE WRITER'S OWN SENTENCE COMES FIRST when there is one: a write
            that failed belongs to a command that was APPLIED, so it is not
            `lastError` and the operator needs it more - the show they think is
            on disk is not. */
        errorLabel.setText (! reading.writeError.empty() ? "write failed: " + text (reading.writeError)
                                                        : text (reading.errorLine()),
                            juce::dontSendNotification);

        /*  The whole record is still one hover away: what the strip drops is
            the tick and the sequence, which an operator who just pressed the
            thing does not need and a reader of the log does. */
        errorLabel.setTooltip (text (reading.lastError));

        /*  WARNINGS SIT ON THE FOOT LINE, where a theme's refusal also lands:
            both are things the engine or the file said about this session that
            no other line has a place for. A notice set by hand wins until the
            next reading changes them, which is the same rule the page's strip
            follows for its own hint. */
        if (reading.warningLine() != last.warningLine() || ! shownOnce)
            noticeLabel.setText (text (reading.warningLine()), juce::dontSendNotification);

        last = reading;
        shownOnce = true;

        if (bannerShowing != wasShowing)
        {
            recoverButton.setVisible (bannerShowing);
            discardButton.setVisible (bannerShowing);

            /*  THE SHELL FIRST: this component's height changes with the
                banner, so laying itself out before its parent has resized it
                would lay out against the old height. */
            if (onHeightChanged)
                onHeightChanged();

            resized();
            repaint();
        }
    }

    void TransportComponent::setNotice (const juce::String& notice)
    {
        /*  CLIPPED, WHATEVER THE CALLER THINKS IT IS SENDING. This label is one
            row high, and laying a quarter of a megabyte of text into one row
            is work without end - which is not a guess: the window spun exactly
            there the first time a show with eighteen hundred warnings was
            opened in it. The model summarises before this is reached
            (TransportReading::warningLine), and this is the second wall, for
            the day a sentence arrives from somewhere that has not thought
            about it. */
        constexpr int longest = 300;

        noticeLabel.setText (notice.length() > longest
                               ? notice.substring (0, longest) + "..."
                               : notice,
                             juce::dontSendNotification);
    }

    int TransportComponent::preferredHeight() const noexcept
    {
        const auto row = rowHeight();

        /*  The rows `resized` lays out, counted in the same order: the padding,
            the title line, the buttons, the sentences, the banner when there is
            one, the standby, the status and the foot. Written as the sum rather
            than measured afterwards, so the two cannot drift without this
            number visibly changing. */
        const auto rows = row                       // padding, top and bottom
                        + row                       // show, tick, clock, rate
                        + row / 2
                        + (bannerShowing ? row * 3 + row / 2 : 0)
                        + row / 2
                        + row * 2                   // the standby, and GO
                        + row / 2
                        + row                       // the lock word and the error
                        + row;                      // the notice

        return rows;
    }

    juce::Rectangle<int> TransportComponent::bannerArea() const
    {
        if (! bannerShowing)
            return {};

        const auto row = rowHeight();
        const auto pad = row / 2;

        //  Under the title line and its half-row of air, as `resized` places it.
        return { pad, pad + row * 2, getWidth() - 2 * pad, row * 3 };
    }

    void TransportComponent::paint (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "ground"));

        const auto row = rowHeight();
        const auto pad = row / 2;

        g.setColour (Look::colour (theme, "rule"));
        g.fillRect (pad, pad + row, getWidth() - 2 * pad, 1);

        if (const auto banner = bannerArea(); ! banner.isEmpty())
        {
            /*  AMBER, AND THE WORDS SAY IT TOO (§4.8): work found beside the
                show is the same amber the standby wears, because both are
                "something is waiting for you" rather than "something is
                wrong". */
            g.setColour (Look::colour (theme, "panel-high"));
            g.fillRect (banner);

            g.setColour (Look::colour (theme, "standby"));
            g.fillRect (banner.getX(), banner.getY(), 2, banner.getHeight());

            g.setColour (Look::colour (theme, "ink-dim"));
            g.setFont (Look::font (theme, 12.0f));
            g.drawFittedText (recoveryText,
                              banner.reduced (pad, pad / 2).withTrimmedRight (row * 7),
                              juce::Justification::topLeft, 6, 1.0f);
        }
    }

    void TransportComponent::resized()
    {
        const auto row = rowHeight();
        const auto pad = row / 2;

        auto area = getLocalBounds().reduced (pad);

        auto top = area.removeFromTop (row);
        rateLabel.setBounds (top.removeFromRight (row * 5));
        clockLabel.setBounds (top.removeFromRight (row * 3));
        tickLabel.setBounds (top.removeFromRight (row * 4));
        showLabel.setBounds (top);

        /*  The row of show-wide buttons that stood here went to the menu
            (2026-09-18); the half-row of air it left stays, so the banner and
            GO keep their distance from the title line. */
        area.removeFromTop (row / 2);

        if (bannerShowing)
        {
            auto banner = area.removeFromTop (row * 3 + row / 2).withTrimmedTop (row / 2);
            auto choice = banner.removeFromRight (row * 7).reduced (pad, pad);
            recoverButton.setBounds (choice.removeFromTop (row).reduced (1));
            choice.removeFromTop (pad / 2);
            discardButton.setBounds (choice.removeFromTop (row).reduced (1));
        }

        area.removeFromTop (row / 2);

        /*  GO ON THE LEFT AND THE CUE IT WILL FIRE BESIDE IT (author,
            2026-09-18: "the Go button should be on the left with the standby
            cue name to its right"). The hand goes to one place and the eye
            reads outward from it, which is the order the two things are used
            in - rather than reading a name and then travelling back across the
            window to the button. */
        auto middle = area.removeFromTop (row * 2);
        goButton.setBounds (middle.removeFromLeft (row * 3).reduced (2));
        middle.removeFromLeft (pad);

        /*  PANIC AT THE FAR RIGHT OF THE SAME ROW, as far from GO as the row
            allows: the two are the same height because both are pressed
            without looking, and apart because one of them must never be hit
            by a hand reaching for the other. */
        panicButton.setBounds (middle.removeFromRight (row * 3).reduced (2));
        middle.removeFromRight (pad);

        /*  THE NAME ON THE LEFT HALF, THE NOTES ON THE RIGHT: the eye reads
            outward from GO through the name into what to look out for. The
            notes take what the name does not need, and both rows of it. */
        auto names = middle.removeFromLeft (juce::jmax (row * 8, middle.getWidth() / 2));
        listLabel.setBounds (names.removeFromTop (row * 3 / 4));
        standbyLabel.setBounds (names);
        middle.removeFromLeft (pad);
        notesLabel.setBounds (middle);

        area.removeFromTop (row / 2);

        auto bottom = area.removeFromTop (row);
        statusLabel.setBounds (bottom.removeFromLeft (row * 4));
        errorLabel.setBounds (bottom);

        noticeLabel.setBounds (area.removeFromTop (row));
    }

    void TransportComponent::askThenRevert()
    {
        /*  REVERT ASKS AND THE OTHERS DO NOT, and that is not an
            inconsistency - it is the page's argument, which holds here for the
            same reason. Undo is why a delete stopped asking; revert is exactly
            what takes undo away. It reloads the show from disk and clears the
            history in the same command, so afterwards there is nothing to
            press ctrl-Z on, and what goes is not one edit but everything since
            the last save. The question says what will be lost rather than
            asking whether anybody is sure, because "are you sure?" is a
            question nobody reads.

            Discard, in the banner, is as final and does not ask twice: the
            banner it sits in is already the question, written out. */
        const auto show = last.show.empty() ? juce::String ("the show") : juce::String (last.show);

        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                        .withIconType (juce::MessageBoxIconType::WarningIcon)
                                        .withTitle ("Revert " + show + " to what was last saved?")
                                        .withMessage ("Everything changed since then is thrown away, "
                                                      "and undo cannot bring it back: reverting "
                                                      "clears the undo history as well.")
                                        .withButton ("Revert")
                                        .withButton ("Keep editing")
                                        .withAssociatedComponent (this),
                                      [safe = juce::Component::SafePointer<TransportComponent> (this)]
                                      (int result)
                                      {
                                          // Two buttons: the first answers 1 (AlertWindow::show's table).
                                          if (result == 1 && safe != nullptr && safe->actions.revert)
                                              safe->actions.revert();
                                      });
    }

    bool TransportComponent::keyPressed (const juce::KeyPress& key)
    {
        if (key == juce::KeyPress (juce::KeyPress::spaceKey))
        {
            if (actions.go) actions.go();
            return true;
        }

        if (key == juce::KeyPress (juce::KeyPress::F5Key))
        {
            if (actions.reloadTheme) actions.reloadTheme();
            return true;
        }

        /*  ESC IS PANIC (PRD §4.4), and a second Esc is the second level -
            the window reads the timing. Unmodified only: an Esc with a
            modifier held is not the key the law names. */
        if (key == juce::KeyPress (juce::KeyPress::escapeKey))
        {
            if (actions.panic) actions.panic();
            return true;
        }

        /*  THE ACCELERATORS FOLLOW THEIR BUTTONS, every one of them: a key
            that fired a gesture the strip is not offering would be a second
            route to something this client has decided not to ask for - which
            is exactly what "show mode does not offer a save" must not mean in
            practice. So each one asks the reading what its menu item asks,
            and that keeps the two in step with no second rule to maintain. */
        const auto mod = juce::ModifierKeys::commandModifier;

        if (key == juce::KeyPress ('z', mod, 0))
        {
            if (model::isYes (last.canUndo) && actions.undo) actions.undo();
            return true;
        }

        if (key == juce::KeyPress ('z', mod | juce::ModifierKeys::shiftModifier, 0))
        {
            if (model::isYes (last.canRedo) && actions.redo) actions.redo();
            return true;
        }

        if (key == juce::KeyPress ('s', mod, 0))
        {
            if (last.mayOfferSave() && last.hasSomethingToSave() && actions.save) actions.save();
            return true;
        }

        return false;
    }
}
