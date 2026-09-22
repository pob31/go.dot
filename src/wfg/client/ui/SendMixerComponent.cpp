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

#include <wfg/client/ui/SendMixerComponent.h>

#include <wfg/client/model/Fader.h>
#include <wfg/client/ui/Look.h>
#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace wfg::client::ui
{
    namespace
    {
        constexpr int stripWidth = 68;
        constexpr int gap = 6;

        int scaled (int base, const model::Theme& theme)
        {
            return juce::roundToInt (static_cast<float> (base) * theme.type);
        }
    }

    //==============================================================================
    /*  ONE STRIP: a name, a throw, a number, and - where the document holds a
        `Send` - a cross to take it away. The master strip is the same object
        with no bus and no cross, because the two behave identically under the
        hand and only differ in what they write. */
    struct SendMixerComponent::Strip final : public juce::Component
    {
        /*  `at` IS THE MIXER'S INDEX AND NOT THE SEND'S: nought is the
            master and the sends run from one. One numbering for the whole
            component, because two - a strip index and a send index, differing
            by one - is the arithmetic that eventually writes a level to the
            wrong destination. `send()` is the one place that converts. */
        Strip (SendMixerComponent& ownerToUse, std::size_t atToUse)
            : owner (ownerToUse), at (atToUse), isMaster (atToUse == 0)
        {
            value.setEditable (false, true, false);
            value.setJustificationType (juce::Justification::centred);
            addAndMakeVisible (value);

            value.onTextChange = [this]
            {
                /*  TYPED IS AS GOOD AS DRAGGED, and it goes through the same
                    parse the rest of this client uses so that a French locale
                    reads a comma. A refusal leaves the number where it was
                    rather than writing nought, which is what `value_or` would
                    have done to a mistyped level. */
                const auto typed = value.getText().trim();

                if (typed.equalsIgnoreCase ("-inf") || typed.equalsIgnoreCase ("inf"))
                {
                    owner.levelWanted (at, model::silenceDb);
                    return;
                }

                if (const auto parsed = osc::parseDouble (typed.toStdString()))
                    owner.levelWanted (at, *parsed);
                else
                    owner.refresh();
            };

            if (! isMaster)
            {
                drop.setButtonText ("x");
                drop.setWantsKeyboardFocus (false);
                drop.setTooltip ("Takes this send away. The mix channel stays; this cue stops "
                                 "arriving at it.");
                drop.onClick = [this] { owner.removeAt (at); };
            }
        }

        const model::SendStrip& send() const { return owner.reading.sends[at - 1]; }

        /*  THE CROSS ONLY WHERE THERE IS SOMETHING TO TAKE AWAY: a strip at
            silence with no `Send` behind it has nothing to delete, and a cross
            on it would look like an offer to remove the mix channel, which is
            the Outputs tab's business and not this panel's. */
        void showCross (bool wanted)
        {
            if (wanted == (drop.getParentComponent() != nullptr))
                return;

            if (wanted)
                addAndMakeVisible (drop);
            else
                removeChildComponent (&drop);

            resized();
        }

        /** Where the throw is drawn, which is what a drag is measured against. */
        juce::Rectangle<int> throwArea() const
        {
            auto area = getLocalBounds().reduced (scaled (gap, owner.theme), 0);
            area.removeFromTop (scaled (26, owner.theme));    // the name and its width word
            area.removeFromBottom (scaled (isMaster ? 20 : 40, owner.theme));  // value, and the cross
            return area;
        }

        /*  WHERE THE FADER IS DRAWN, which is the HAND while a hand is on it
            and the document the rest of the time.

            A drag is a conversation with a round trip in it: the level goes
            out as a command, the tick thread applies it, and the reading comes
            back a pass later. Drawn from the reading throughout, the cap
            lags the pointer - and on the first move of a SILENT strip it does
            worse than lag, because what comes back first is the new send's
            own default of nought. That is the jump the author saw. So while
            the hand is down the strip draws what the hand asked for, and the
            document catches up underneath it. */
        double levelHere() const
        {
            if (dragging)
                return shown;

            return isMaster ? owner.reading.cueLevel : send().levelDb;
        }

        void mouseDown (const juce::MouseEvent& event) override
        {
            held = levelHere();
            shown = held;
            dragging = true;
            dragFrom = event.position.y;
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            /*  AND THE READING TAKES OVER AGAIN. By now the level that was
                asked for has been applied and published; if it has not, the
                next pass corrects the cap rather than this holding a number
                the document never accepted. */
            dragging = false;
            owner.refresh();
        }

        void mouseDrag (const juce::MouseEvent& event) override
        {
            const auto area = throwArea();

            if (area.getHeight() <= 0)
                return;

            /*  MEASURED FROM WHERE THE HAND WENT DOWN rather than from where
                the pointer is, so a press does not jump the fader to the
                pointer. Shift divides the movement by ten, which is the
                fine-drag every other drag in this window has. */
            const auto moved = (dragFrom - event.position.y)
                                 / static_cast<float> (area.getHeight());
            const auto scale = event.mods.isShiftDown() ? 0.1f : 1.0f;

            shown = model::dbForFraction (model::fractionForDb (held)
                                            + static_cast<double> (moved * scale));

            owner.levelWanted (at, shown);
            owner.refresh();
        }

        void mouseDoubleClick (const juce::MouseEvent&) override
        {
            //  Unity, which is where a strip is when nobody has decided otherwise.
            owner.levelWanted (at, 0.0);
        }

        void mouseWheelMove (const juce::MouseEvent& event,
                             const juce::MouseWheelDetails& wheel) override
        {
            const auto clicks = juce::roundToInt (wheel.deltaY * 10.0f);

            if (clicks != 0)
                owner.levelWanted (at, model::stepDb (levelHere(), clicks,
                                                      event.mods.isShiftDown()));
        }

        void paint (juce::Graphics& g) override
        {
            /*  Named for what it is rather than `theme`, which is the
                owner's member and which a local of that name hides - GCC's
                -Wshadow and MSVC's C4458 both say so, and this file is inside
                the class that declares it. */
            const auto& look = owner.theme;
            const auto area = getLocalBounds();

            g.setColour (Look::colour (look, isMaster ? "panel-in" : "panel"));
            g.fillRect (area);

            //  The name, and under it what the output is - mono, stereo, its channels.
            auto head = area.reduced (scaled (gap, look), 0)
                            .withHeight (scaled (26, look));

            g.setColour (Look::colour (look, "ink"));
            g.setFont (Look::font (look, 12.0f));
            g.drawFittedText (isMaster ? juce::String ("Cue level")
                                       : juce::String (send().name),
                              head.removeFromTop (scaled (14, look)), juce::Justification::centred, 1);

            g.setColour (Look::colour (look, "ink-faint"));
            g.setFont (Look::font (look, 10.0f));
            g.drawFittedText (isMaster ? juce::String ("a DCA over everything below")
                                       : juce::String (send().widthWord + " " + send().channelWord),
                              head, juce::Justification::centred, 1);

            //  The throw: a groove, a mark at unity, and the cap where the level is.
            const auto track = throwArea();
            const auto middle = track.getCentreX();

            g.setColour (Look::colour (look, "rule"));
            g.fillRect (juce::Rectangle<int> (middle - 1, track.getY(), 2, track.getHeight()));

            const auto placeOf = [&track] (double decibels)
            {
                return track.getBottom()
                         - juce::roundToInt (model::fractionForDb (decibels)
                                               * static_cast<double> (track.getHeight()));
            };

            /*  UNITY IS MARKED, because it is the one place on the throw that
                means something on its own and the taper does not put it where
                a ruler would. */
            g.setColour (Look::colour (look, "ink-off"));
            g.drawHorizontalLine (placeOf (0.0), static_cast<float> (track.getX()),
                                  static_cast<float> (track.getRight()));

            const auto level = levelHere();
            const auto cap = placeOf (level);

            /*  A SILENT STRIP WITH NOTHING BEHIND IT is drawn dimmer, and the
                word under it says which it is - a send at silence and no send
                at all sound the same, and what differs is only whether there
                is an object to delete. */
            const auto live = isMaster || send().present();

            g.setColour (Look::colour (look, live ? "accent" : "ink-off"));
            g.fillRect (juce::Rectangle<int> (track.getX(), cap - 3, track.getWidth(), 6));
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (scaled (gap, owner.theme), 0);

            /*  Asked of the component rather than of the strip's kind: only
                a strip with a send to remove has one, and `throwArea` takes
                the same row back whether it is there or not so the throws all
                end at the same height. */
            if (drop.getParentComponent() != nullptr)
                drop.setBounds (area.removeFromBottom (scaled (18, owner.theme)));

            value.setBounds (area.removeFromBottom (scaled (18, owner.theme)));
        }

        SendMixerComponent& owner;
        std::size_t at;
        bool isMaster;

        juce::Label value;
        juce::TextButton drop;

        double held = 0.0;      ///< where the level was when the hand went down
        double shown = 0.0;     ///< where the hand has asked for it to be
        bool dragging = false;
        float dragFrom = 0.0f;
    };

    //==============================================================================
    SendMixerComponent::SendMixerComponent (const model::Theme& themeToUse, Actions actionsToUse)
        : theme (themeToUse), actions (std::move (actionsToUse))
    {
        applyTheme (theme);
    }

    SendMixerComponent::~SendMixerComponent() = default;

    void SendMixerComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;

        for (auto& strip : strips)
        {
            strip->value.setFont (Look::font (theme, 12.0f));
            strip->value.setColour (juce::Label::textColourId, Look::colour (theme, "ink"));
            strip->value.setColour (juce::Label::backgroundColourId,
                                    Look::colour (theme, "panel-in"));
        }

        repaint();
    }

    std::string SendMixerComponent::shapeOf() const
    {
        /*  THE IDENTITY OF THE DESK AND NOT ITS LEVELS. Rebuilt when a mix
            channel appears or goes, or when a send is made or deleted -
            retyped when a fader moves. Rebuilding on a level would take the
            focus out of the box somebody is typing a number into, twenty-five
            times a second. */
        /*  THE MIX CHANNELS AND NOT THE SENDS. A send appearing or going is
            NOT a change of shape: the strip was already there, drawn at
            silence, and the only visible difference is whether it has a cross.
            Rebuilding on it would destroy the very component the hand is
            dragging, mid-drag - which is exactly what raising a silent fader
            does, so the first move of every new send killed itself. */
        std::string out = reading.subject.objectId + "!" + reading.notice;

        for (const auto& strip : reading.sends)
            out += "|" + strip.busId;

        return out;
    }

    void SendMixerComponent::show (const model::FootReading& readingToUse)
    {
        const auto was = shapeOf();
        reading = readingToUse;

        if (shapeOf() != was)
            rebuild();
        else
            refresh();

        /*  AND THE LEVEL THE HAND WAS ASKING FOR, now that the object it
            needed exists. The `send.create` went out on the drag; the level
            goes out here, on the first pass where the tree has the new send in
            it - the same two-step the importer uses to make a cue and then
            fill it in. Outside the rebuild branch, because a send arriving is
            no longer a change of shape. */
        if (! awaitingBus.empty())
            for (std::size_t at = 0; at < reading.sends.size(); ++at)
                if (reading.sends[at].busId == awaitingBus && reading.sends[at].present())
                {
                    const auto wanted = awaitingLevel;
                    awaitingBus.clear();
                    levelWanted (at + 1, wanted);
                    break;
                }
    }

    void SendMixerComponent::rebuild()
    {
        strips.clear();
        removeAllChildren();

        /*  NOTHING TO DRAW MEANS A SENTENCE AND NOT AN EMPTY DESK. The
            reading has already worked out which of the three it is; here it
            is only a matter of not drawing strips over the top of it. */
        if (! reading.notice.empty())
            return;

        //  The master first, then one per mix channel in the output list's order.
        strips.push_back (std::make_unique<Strip> (*this, 0u));

        for (std::size_t at = 0; at < reading.sends.size(); ++at)
            strips.push_back (std::make_unique<Strip> (*this, at + 1));

        for (auto& strip : strips)
            addAndMakeVisible (*strip);

        applyTheme (theme);
        refresh();
        resized();
    }

    void SendMixerComponent::refresh()
    {
        for (auto& strip : strips)
        {
            /*  THE CROSS FOLLOWS THE SEND rather than the strip, and it is
                here rather than in the constructor because a send arriving is
                no longer a rebuild: there is something to delete now, so there
                is a cross, and the strip it belongs to has not moved. */
            strip->showCross (! strip->isMaster && strip->send().present());

            if (strip->value.isBeingEdited())
                continue;

            strip->value.setText (model::faderText (strip->levelHere()),
                                  juce::dontSendNotification);
            strip->repaint();
        }
    }

    void SendMixerComponent::levelWanted (std::size_t at, double decibels)
    {
        const auto level = std::clamp (decibels, model::silenceDb, model::loudestDb);

        //  Nought is the master strip; the sends are one along from it.
        if (at == 0)
        {
            if (actions.set)
                actions.set ("/godot/cue/" + reading.subject.objectId + "/level",
                             osc::formatDouble (std::round (level * 10.0) / 10.0));

            return;
        }

        const auto which = at - 1;

        if (which >= reading.sends.size())
            return;

        const auto& strip = reading.sends[which];

        /*  RAISING A STRIP THAT IS NOT THERE YET MAKES IT. The document holds
            a `Send` only where somebody set one, so the first move of a silent
            fader is two writes: the object, then the level once the tree has
            published it. Held here rather than sent blind, because the
            identifier the level has to be addressed to does not exist yet. */
        if (! strip.present())
        {
            if (actions.createSend)
            {
                /*  ONCE PER DRAG, not once per mouse-move. A drag sends fifty
                    of these a second and the document refuses every duplicate
                    after the first - correctly, but it would put fifty
                    refusals in the log for one gesture. The level keeps
                    catching up; only the making of the object is once. */
                const auto asked = awaitingBus == strip.busId;

                awaitingBus = strip.busId;
                awaitingLevel = level;

                if (! asked)
                    actions.createSend (reading.subject.objectId, strip.busId);
            }

            return;
        }

        if (actions.set)
            actions.set ("/godot/send/" + strip.sendId + "/level",
                         osc::formatDouble (std::round (level * 10.0) / 10.0));
    }

    void SendMixerComponent::removeAt (std::size_t at)
    {
        if (at == 0)
            return;                 // the master is not a send and has no cross

        const auto which = at - 1;

        if (which < reading.sends.size() && reading.sends[which].present() && actions.removeSend)
            actions.removeSend (reading.sends[which].sendId);
    }

    void SendMixerComponent::paint (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "panel"));

        if (! reading.notice.empty())
        {
            g.setColour (Look::colour (theme, "ink-dim"));
            g.setFont (Look::font (theme, 13.0f));
            g.drawFittedText (juce::String (reading.notice),
                              getLocalBounds().reduced (scaled (12, theme), scaled (6, theme)),
                              juce::Justification::centred, 3);
            return;
        }

        /*  A RULE BETWEEN THE MASTER AND THE SENDS, because they are different
            kinds of number: one is what the cue is, the others are where it
            also goes. */
        if (! strips.empty())
        {
            const auto x = strips.front()->getRight() + scaled (gap, theme) / 2;

            g.setColour (Look::colour (theme, "rule"));
            g.fillRect (juce::Rectangle<int> (x, 0, 1, getHeight()));
        }
    }

    void SendMixerComponent::resized()
    {
        auto area = getLocalBounds().reduced (scaled (gap, theme));
        const auto width = scaled (stripWidth, theme);

        for (auto& strip : strips)
        {
            strip->setBounds (area.removeFromLeft (width));
            area.removeFromLeft (scaled (gap, theme));
        }
    }
}
