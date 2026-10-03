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

#include <wfg/client/ui/FadeMixerComponent.h>

#include <wfg/client/model/Fader.h>
#include <wfg/client/ui/Look.h>
#include <wfg/engine/osc/OscValue.h>

#include <spatcore/ui/TypedValue.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace wfg::client::ui
{
    namespace
    {
        constexpr int stripWidth = 68;
        constexpr int gap = 6;
        constexpr int columnWidth = 300;

        int scaled (int base, const model::Theme& theme)
        {
            return juce::roundToInt (static_cast<float> (base) * theme.type);
        }

        bool isSpeed (const model::FadeStrip& strip) { return strip.kind == "speed"; }

        double fractionOf (const model::FadeStrip& strip, double value)
        {
            return isSpeed (strip) ? model::fractionForSpeed (value) : model::fractionForDb (value);
        }

        double valueAt (const model::FadeStrip& strip, double fraction)
        {
            return isSpeed (strip) ? model::speedForFraction (fraction) : model::dbForFraction (fraction);
        }

        /** Within the row's range, and rounded as a hand's number is. */
        double bounded (const model::FadeStrip& strip, double value)
        {
            if (isSpeed (strip))
                return std::clamp (value, 0.0, 20.0);

            return std::round (std::clamp (value, model::silenceDb, model::loudestDb) * 10.0) / 10.0;
        }
    }

    //==============================================================================
    /*  ONE STRIP: a name, a throw, a number, and the tick box with its word. */
    struct FadeMixerComponent::Strip final : public juce::Component
    {
        Strip (FadeMixerComponent& ownerToUse, std::size_t atToUse)
            : owner (ownerToUse), at (atToUse)
        {
            value.setEditable (false, true, false);
            value.setJustificationType (juce::Justification::centred);
            addAndMakeVisible (value);

            value.onTextChange = [this]
            {
                const auto typed = value.getText().trim();

                if (! isSpeed (strip()) && (typed.equalsIgnoreCase ("-inf") || typed.equalsIgnoreCase ("inf")))
                {
                    owner.valueWanted (at, model::silenceDb);
                    return;
                }

                //  "x0.5" and "0,5" as well as "0.5" for a speed; "-6 dB" for a level.
                const auto cleaned = typed.trimCharactersAtStart ("xX");

                if (const auto parsed = spatcore::ui::typed::number (cleaned))
                    owner.valueWanted (at, static_cast<double> (*parsed));
                else
                    owner.refresh();
            };

            moves.setWantsKeyboardFocus (false);
            moves.onClick = [this] { owner.tickWanted (at, moves.getToggleState()); };
            addAndMakeVisible (moves);
        }

        const model::FadeStrip& strip() const { return owner.reading.fadeMix.strips[at]; }

        juce::Rectangle<int> throwArea() const
        {
            auto area = getLocalBounds().reduced (scaled (gap, owner.theme), 0);
            area.removeFromTop (scaled (26, owner.theme));      // the name and the word under it
            area.removeFromBottom (scaled (40, owner.theme));   // the number, and the tick box
            return area;
        }

        double here() const
        {
            return dragging ? shown : strip().value;
        }

        void mouseDown (const juce::MouseEvent& event) override
        {
            if (event.eventComponent != this)
                return;

            held = here();
            shown = held;
            dragging = true;
            dragFrom = event.position.y;
        }

        void mouseUp (const juce::MouseEvent& event) override
        {
            if (event.eventComponent != this)
                return;

            dragging = false;
            owner.refresh();
        }

        void mouseDrag (const juce::MouseEvent& event) override
        {
            if (event.eventComponent != this)
                return;

            const auto area = throwArea();

            if (area.getHeight() <= 0)
                return;

            const auto moved = (dragFrom - event.position.y) / static_cast<float> (area.getHeight());
            const auto scale = event.mods.isShiftDown() ? 0.1f : 1.0f;

            shown = bounded (strip(), valueAt (strip(), fractionOf (strip(), held)
                                                        + static_cast<double> (moved * scale)));

            owner.valueWanted (at, shown);
            repaint();
        }

        void mouseDoubleClick (const juce::MouseEvent& event) override
        {
            if (event.eventComponent != this)
                return;

            //  Unity for a level, the file's own speed for a speed.
            owner.valueWanted (at, isSpeed (strip()) ? 1.0 : 0.0);
        }

        void mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override
        {
            if (event.eventComponent != this)
                return;

            const auto clicks = juce::roundToInt (wheel.deltaY * 10.0f);

            if (clicks == 0)
                return;

            if (isSpeed (strip()))
                owner.valueWanted (at, bounded (strip(), valueAt (strip(), fractionOf (strip(), here())
                                                                            + clicks * (event.mods.isShiftDown() ? 0.002 : 0.02))));
            else
                owner.valueWanted (at, model::stepDb (here(), clicks, event.mods.isShiftDown()));
        }

        void paint (juce::Graphics& g) override
        {
            const auto& look = owner.theme;
            const auto& s = strip();
            const auto area = getLocalBounds();

            g.setColour (Look::colour (look, s.kind == "send" ? "panel" : "panel-in"));
            g.fillRect (area);

            auto head = area.reduced (scaled (gap, look), 0).withHeight (scaled (26, look));

            g.setColour (Look::colour (look, s.moved ? "ink" : "ink-dim"));
            g.setFont (Look::font (look, 12.0f));
            g.drawFittedText (juce::String (s.name), head.removeFromTop (scaled (14, look)),
                              juce::Justification::centred, 1);

            g.setColour (Look::colour (look, "ink-faint"));
            g.setFont (Look::font (look, 10.0f));
            g.drawFittedText (juce::String (s.under), head, juce::Justification::centred, 1);

            const auto track = throwArea();
            const auto middle = track.getCentreX();

            g.setColour (Look::colour (look, "rule"));
            g.fillRect (juce::Rectangle<int> (middle - 1, track.getY(), 2, track.getHeight()));

            const auto placeOf = [&track, &s] (double v)
            {
                return track.getBottom() - juce::roundToInt (fractionOf (s, v) * static_cast<double> (track.getHeight()));
            };

            //  Unity, or the file's own speed: the one place that means something on its own.
            g.setColour (Look::colour (look, "ink-off"));
            g.drawHorizontalLine (placeOf (isSpeed (s) ? 1.0 : 0.0), static_cast<float> (track.getX()),
                                  static_cast<float> (track.getRight()));

            /*  THE CAP: the accent where the fade moves it, dim where it stays,
                and a hollow frame for the second so the two differ in shape
                as well as in colour (§4.8). */
            const auto cap = juce::Rectangle<int> (track.getX(), placeOf (here()) - 3, track.getWidth(), 6);
            const auto lit = s.moved || dragging;

            if (lit)
            {
                g.setColour (Look::colour (look, "accent"));
                g.fillRect (cap);
            }
            else
            {
                g.setColour (Look::colour (look, "ink-off"));
                g.drawRect (cap, 1);
            }
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (scaled (gap, owner.theme), 0);
            moves.setBounds (area.removeFromBottom (scaled (18, owner.theme)));
            area.removeFromBottom (scaled (4, owner.theme));
            value.setBounds (area.removeFromBottom (scaled (18, owner.theme)));
        }

        FadeMixerComponent& owner;
        std::size_t at;

        juce::Label value;
        juce::ToggleButton moves;

        double held = 0.0, shown = 0.0;
        bool dragging = false;
        float dragFrom = 0.0f;
    };

    //==============================================================================
    /*  ONE EQ NUMBER OR PLUGIN VALUE THE FADE MOVES: ticked, since it is in
        the list; clearing the tick takes it out. */
    struct FadeMixerComponent::MoveRow final : public juce::Component
    {
        MoveRow (FadeMixerComponent& ownerToUse, std::size_t atToUse)
            : owner (ownerToUse), at (atToUse)
        {
            tick.setWantsKeyboardFocus (false);
            tick.setToggleState (true, juce::dontSendNotification);
            tick.setTooltip ("Clear to stop the fade moving this");
            tick.onClick = [this]
            {
                if (! tick.getToggleState())
                    owner.untickMove (at);
            };
            addAndMakeVisible (tick);
        }

        void paint (juce::Graphics& g) override
        {
            const auto& row = owner.reading.fadeMix.moves[at];
            auto area = getLocalBounds().withTrimmedLeft (getHeight() + scaled (4, owner.theme));

            g.setFont (Look::font (owner.theme, 12.0f));
            g.setColour (Look::colour (owner.theme, "accent"));
            g.drawText (juce::String (row.valueText), area.removeFromRight (scaled (70, owner.theme)),
                        juce::Justification::centredRight, true);

            g.setColour (Look::colour (owner.theme, "ink"));
            g.drawFittedText (juce::String (row.label), area, juce::Justification::centredLeft, 1);
        }

        void resized() override
        {
            tick.setBounds (getLocalBounds().removeFromLeft (getHeight()));
        }

        FadeMixerComponent& owner;
        std::size_t at;
        juce::ToggleButton tick;
    };

    //==============================================================================
    FadeMixerComponent::FadeMixerComponent (const model::Theme& themeToUse, Actions actionsToUse)
        : theme (themeToUse), actions (std::move (actionsToUse))
    {
        applyTheme (theme);
    }

    FadeMixerComponent::~FadeMixerComponent() = default;

    void FadeMixerComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;

        for (auto& strip : strips)
        {
            strip->value.setFont (Look::font (theme, 12.0f));
            strip->value.setColour (juce::Label::backgroundColourId, Look::colour (theme, "panel-in"));
        }

        refresh();
        repaint();
    }

    std::string FadeMixerComponent::shapeOf() const
    {
        /*  THE IDENTITY OF THE DESK, NOT ITS NUMBERS: rebuilt when a strip, a
            moved row or a door appears or goes, retyped when a value moves -
            so the box somebody is typing in keeps the focus. */
        const auto& mix = reading.fadeMix;
        std::string out = reading.subject.objectId + "!" + reading.notice + "!" + mix.targetId + "!" + mix.dcaId
                            + (mix.hasEq ? "!eq" : "");

        for (const auto& strip : mix.strips)
            out += "|" + strip.kind + ":" + strip.busId;

        for (const auto& row : mix.moves)
            out += "|m:" + row.entry;

        for (const auto& insert : mix.inserts)
            out += "|i:" + insert.pluginId;

        return out;
    }

    void FadeMixerComponent::show (const model::FootReading& readingToUse)
    {
        const auto was = shapeOf();
        reading = readingToUse;

        if (shapeOf() != was || (strips.empty() && rows.empty() && doors.empty()))
            rebuild();
        else
            refresh();
    }

    void FadeMixerComponent::rebuild()
    {
        strips.clear();
        rows.clear();
        doors.clear();
        removeAllChildren();

        const auto& mix = reading.fadeMix;

        if (! mix.present)
        {
            repaint();
            return;
        }

        for (std::size_t at = 0; at < mix.strips.size(); ++at)
        {
            strips.push_back (std::make_unique<Strip> (*this, at));
            addAndMakeVisible (*strips.back());
        }

        const auto fadeId = mix.fadeId;

        const auto door = [this] (const juce::String& words, const juce::String& tip, std::function<void()> then)
        {
            auto button = std::make_unique<juce::TextButton> (words);
            button->setWantsKeyboardFocus (false);
            button->setTooltip (tip);

            /*  ON THE NEXT MESSAGE, not inside the click: opening another
                subject destroys this mixer, and the button is one of its
                children (the chain's EQ box's reason). */
            button->onClick = [self = juce::Component::SafePointer<FadeMixerComponent> (this), then]
            {
                juce::MessageManager::callAsync ([self, then]
                {
                    if (self != nullptr)
                        then();
                });
            };

            addAndMakeVisible (*button);
            doors.push_back (std::move (button));
        };

        if (mix.hasEq)
            door ("EQ...", "The target's EQ, for the bands this fade moves",
                  [this, fadeId] { if (actions.openEq) actions.openEq (fadeId); });

        for (const auto& insert : mix.inserts)
        {
            const auto pluginId = insert.pluginId;
            door (juce::String (insert.name) + "...",
                  "Opens " + juce::String (insert.name) + "'s own window on this fade: what you turn there, the fade moves",
                  [this, fadeId, pluginId] { if (actions.editPlugin) actions.editPlugin (fadeId, pluginId); });
        }

        door ("Curve...", "The shape the fade takes - drawn points shape the level",
              [this, fadeId] { if (actions.openCurve) actions.openCurve (fadeId); });

        for (std::size_t at = 0; at < mix.moves.size(); ++at)
        {
            rows.push_back (std::make_unique<MoveRow> (*this, at));
            addAndMakeVisible (*rows.back());
        }

        applyTheme (theme);
        resized();
    }

    std::string FadeMixerComponent::valueText (const model::FadeStrip& strip, double value)
    {
        //  A point and not the locale's comma: the canonical formatter, to a thousandth.
        if (isSpeed (strip))
            return "x" + osc::formatDouble (std::round (value * 1000.0) / 1000.0);

        return model::faderText (value);
    }

    void FadeMixerComponent::refresh()
    {
        for (auto& strip : strips)
        {
            const auto& s = strip->strip();
            strip->moves.setButtonText (s.moved ? "moves" : "stays");
            strip->moves.setToggleState (s.moved, juce::dontSendNotification);
            strip->moves.setTooltip (s.moved ? "The fade moves this - clear to leave it alone"
                                             : "The fade leaves this alone - tick, or move the strip, to move it");
            strip->value.setColour (juce::Label::textColourId, Look::colour (theme, s.moved ? "accent" : "ink-off"));

            if (! strip->value.isBeingEdited())
                strip->value.setText (valueText (s, strip->here()), juce::dontSendNotification);

            strip->repaint();
        }

        for (auto& row : rows)
            row->repaint();
    }

    //==============================================================================
    void FadeMixerComponent::valueWanted (std::size_t at, double value)
    {
        const auto& mix = reading.fadeMix;

        if (at >= mix.strips.size() || ! actions.set)
            return;

        const auto& strip = mix.strips[at];
        const auto text = osc::formatDouble (bounded (strip, value));

        /*  MOVING A STRIP TICKS IT (the author: "Changing a parameter
            highlights the parameter"): the switch first for the level and the
            speed, so the value never lands on a fade that is not moving it. A
            send is ticked by being in the list, which the write itself does. */
        if (strip.kind != "send" && ! strip.moved)
            actions.set (strip.switchAddress, "true");

        actions.set (strip.address, text);
    }

    void FadeMixerComponent::tickWanted (std::size_t at, bool moves)
    {
        const auto& mix = reading.fadeMix;

        if (at >= mix.strips.size() || ! actions.set)
            return;

        const auto& strip = mix.strips[at];

        if (strip.kind == "send")
        {
            /*  TICKED, IT IS MOVED TO WHERE IT STANDS - nothing heard changes
                until the strip is moved; CLEARED, the entry goes (PF). */
            actions.set (strip.address, moves ? osc::formatDouble (bounded (strip, strip.value)) : std::string());
            return;
        }

        /*  THE LEVEL AND THE SPEED: ticked, from where the target stands, so a
            tick alone never makes a fade to silence nobody asked for. */
        if (moves)
            actions.set (strip.address, osc::formatDouble (bounded (strip, strip.value)));

        actions.set (strip.switchAddress, moves ? "true" : "false");
    }

    void FadeMixerComponent::untickMove (std::size_t at)
    {
        const auto& mix = reading.fadeMix;

        if (at < mix.moves.size() && actions.set)
            actions.set (mix.moves[at].address, {});
    }

    //==============================================================================
    void FadeMixerComponent::paint (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "panel"));

        const auto& mix = reading.fadeMix;

        if (! mix.present)
        {
            g.setColour (Look::colour (theme, "ink-dim"));
            g.setFont (Look::font (theme, 13.0f));
            g.drawFittedText (juce::String (mix.notice.empty() ? reading.notice : mix.notice),
                              getLocalBounds().reduced (scaled (12, theme), scaled (6, theme)),
                              juce::Justification::centred, 3);
            return;
        }

        /*  WHAT IT IS AIMED AT, over the doors, and a rule between the level
            and speed and the sends - different kinds of number, as the send
            mixer draws them. */
        auto column = getLocalBounds().removeFromRight (scaled (columnWidth, theme)).reduced (scaled (gap, theme));

        g.setColour (Look::colour (theme, "ink-dim"));
        g.setFont (Look::font (theme, 12.0f));
        g.drawFittedText ("Moves " + juce::String (mix.targetName)
                            + (mix.targetKind == "dca" ? juce::String (" - the motorised fader follows") : juce::String()),
                          column.removeFromTop (scaled (18, theme)), juce::Justification::centredLeft, 1);

        if (rows.empty() && mix.hasEq)
        {
            g.setColour (Look::colour (theme, "ink-off"));
            g.drawFittedText ("Moves no EQ number or plugin value - open EQ... or a plugin to set one.",
                              column.withTrimmedTop (scaled (34, theme)).withHeight (scaled (36, theme)),
                              juce::Justification::topLeft, 2);
        }

        for (std::size_t at = 1; at < strips.size(); ++at)
            if (strips[at]->strip().kind == "send" && strips[at - 1]->strip().kind != "send")
            {
                const auto x = strips[at]->getX() - scaled (gap, theme) / 2;
                g.setColour (Look::colour (theme, "rule"));
                g.fillRect (juce::Rectangle<int> (x, 0, 1, getHeight()));
            }
    }

    void FadeMixerComponent::resized()
    {
        auto area = getLocalBounds().reduced (scaled (gap, theme));
        auto column = area.removeFromRight (scaled (columnWidth, theme));
        const auto width = scaled (stripWidth, theme);

        for (auto& strip : strips)
        {
            strip->setBounds (area.removeFromLeft (width));
            area.removeFromLeft (scaled (gap, theme));
        }

        column.removeFromTop (scaled (20, theme));     // what it moves, in words

        auto buttons = column.removeFromTop (scaled (24, theme));
        const auto doorWidth = doors.empty() ? 0 : buttons.getWidth() / static_cast<int> (doors.size());

        for (auto& door : doors)
            door->setBounds (buttons.removeFromLeft (std::max (doorWidth, scaled (60, theme))).reduced (2, 1));

        column.removeFromTop (scaled (8, theme));

        for (auto& row : rows)
            row->setBounds (column.removeFromTop (scaled (20, theme)));
    }
}
