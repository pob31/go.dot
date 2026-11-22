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

#include <wfg/client/ui/DynPanelComponent.h>

#include <wfg/client/model/Dyn.h>
#include <wfg/client/model/FadeMix.h>
#include <wfg/client/ui/Look.h>
#include <wfg/engine/cue/FadeMoves.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>
#include <vector>

namespace wfg::client::ui
{
    namespace
    {
        enum class Group { compressor, gate, key };

        //  The reduction the bar is drawn against: twenty-four decibels down is its foot.
        constexpr double meterFloorDb = -24.0;
    }

    //==============================================================================
    struct DynPanelComponent::Knob
    {
        std::string row;
        Group group;
        juce::Label name;
        juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
        juce::ToggleButton moves;   ///< on a fade only: whether it moves this number (BIF)
    };

    DynPanelComponent::DynPanelComponent (const model::Theme& themeToUse, Actions actionsToUse)
        : theme (themeToUse), actions (std::move (actionsToUse))
    {
        struct Spec
        {
            const char* row;
            const char* label;
            Group group;
            double low, high, middle;
            int decimals;
            const char* unit;
        };

        //  Ranges are the table's; the middle of each knob's travel where the ear works most.
        static const Spec specs[] = {
            { "dynThreshold",     "Threshold", Group::compressor, -60.0, 0.0, -20.0, 1, " dB" },
            { "dynRatio",         "Ratio",     Group::compressor, 1.0, 100.0, 4.0, 1, ":1" },
            { "dynAttack",        "Attack",    Group::compressor, 0.05, 200.0, 10.0, 2, " ms" },
            { "dynRelease",       "Release",   Group::compressor, 5.0, 2000.0, 150.0, 0, " ms" },
            { "dynKnee",          "Knee",      Group::compressor, 0.0, 24.0, 6.0, 1, " dB" },
            { "dynMakeup",        "Make-up",   Group::compressor, -24.0, 24.0, 0.0, 1, " dB" },
            { "dynGateThreshold", "Threshold", Group::gate, -90.0, 0.0, -45.0, 1, " dB" },
            { "dynGateRange",     "Range",     Group::gate, -80.0, 0.0, -40.0, 1, " dB" },
            { "dynGateAttack",    "Attack",    Group::gate, 0.05, 200.0, 5.0, 2, " ms" },
            { "dynGateHold",      "Hold",      Group::gate, 0.0, 500.0, 50.0, 0, " ms" },
            { "dynGateRelease",   "Release",   Group::gate, 5.0, 2000.0, 150.0, 0, " ms" },
            { "dynKeyLowCut",     "High-pass", Group::key, 20.0, 2000.0, 200.0, 0, " Hz" },
            { "dynKeyHighCut",    "Low-pass",  Group::key, 1000.0, 20000.0, 5000.0, 0, " Hz" },
        };

        for (const auto& spec : specs)
        {
            auto knob = std::make_unique<Knob>();
            knob->row = spec.row;
            knob->group = spec.group;
            knob->name.setText (spec.label, juce::dontSendNotification);
            knob->name.setJustificationType (juce::Justification::centred);
            knob->slider.setRange (spec.low, spec.high, 0.0);

            if (spec.middle > spec.low && spec.middle < spec.high)
                knob->slider.setSkewFactorFromMidPoint (spec.middle);

            knob->slider.setNumDecimalPlacesToDisplay (spec.decimals);
            knob->slider.setTextValueSuffix (spec.unit);
            knob->slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 18);
            knob->slider.setWantsKeyboardFocus (false);

            const auto row = knob->row;
            const auto decimals = spec.decimals;
            auto* slider = &knob->slider;
            knob->slider.onValueChange = [this, row, decimals, slider]
            {
                if (! updating)
                    write (row, juce::String (slider->getValue(), std::max (decimals, 2)).toStdString());
            };

            /*  ON A FADE, A TICK BOX ABOVE THE KNOB (§141, BIF), as the EQ
                panel's beside its numbers: ticked, the fade moves the row to
                the knob; cleared, it leaves the row alone. Ticking one it did
                not move starts it at what the target holds now. */
            knob->moves.setWantsKeyboardFocus (false);
            knob->moves.setTooltip ("Whether the fade moves " + juce::String (spec.label).toLowerCase()
                                    + " - clear it to leave the number alone");
            knob->moves.onClick = [this, row, decimals, slider, moves = &knob->moves]
            {
                if (updating)
                    return;

                if (moves->getToggleState())
                    write (row, juce::String (slider->getValue(), std::max (decimals, 2)).toStdString());
                else
                    write (row, {});
            };

            addAndMakeVisible (knob->name);
            addAndMakeVisible (knob->slider);
            addChildComponent (knob->moves);
            knobs.push_back (std::move (knob));
        }

        const auto toggle = [this] (juce::ToggleButton& button, const char* words, const char* row)
        {
            button.setButtonText (words);
            button.setWantsKeyboardFocus (false);
            button.onClick = [this, &button, row]
            {
                if (! updating)
                    write (row, button.getToggleState() ? "true" : "false");
            };
            addAndMakeVisible (button);
        };

        toggle (inSwitch, "Dynamics in", "dynOn");
        toggle (compSwitch, "Compressor", "dynCompOn");
        toggle (gateSwitchButton, "Gate", "dynGateOn");

        //  PEAK OR RMS (the author, 2026-11-22), for both the compressor and the gate.
        detector.addItem ("Peak", 1);
        detector.addItem ("RMS", 2);
        detector.setWantsKeyboardFocus (false);
        detector.setTooltip ("What the detector reads: each sample's peak, or the power over ten milliseconds");
        detector.onChange = [this]
        {
            if (! updating)
                write ("dynDetector", detector.getSelectedId() == 2 ? "rms" : "peak");
        };
        addAndMakeVisible (detector);

        //  WHAT IT LISTENS TO (§141, BHL, BHU).
        keyMenu.setWantsKeyboardFocus (false);
        keyMenu.setTooltip ("What the stage listens to: this cue before or after its level, an input, an aux,"
                            " or an output - an output heard a block late, after the whole show is mixed");
        keyMenu.onChange = [this]
        {
            const auto picked = keyMenu.getSelectedId() - 1;

            if (updating || picked < 0 || picked >= static_cast<int> (keyValues.size()))
                return;

            const auto& value = keyValues[static_cast<std::size_t> (picked)];

            if (value != reading.dyn.key)
                write ("dynKey", value);
        };
        addAndMakeVisible (keyMenu);

        reset.setButtonText ("Reset");
        reset.setWantsKeyboardFocus (false);
        reset.setTooltip ("Every number back to rest and the stage out - one step to undo; the key stays");
        reset.onClick = [this]
        {
            if (actions.reset && reading.dyn.present)
                actions.reset (reading.subject.objectId);
        };
        addAndMakeVisible (reset);

        applyTheme (theme);
    }

    DynPanelComponent::~DynPanelComponent() = default;

    int DynPanelComponent::scaled (int base) const
    {
        return juce::roundToInt (static_cast<double> (base) * theme.type);
    }

    void DynPanelComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;

        //  The theme's own colours on every control, as the EQ panel puts them on its boxes.
        for (auto& knob : knobs)
        {
            knob->name.setFont (Look::font (theme, 12.0f));
            knob->name.setColour (juce::Label::textColourId, Look::colour (theme, "ink-dim"));
            knob->slider.setColour (juce::Slider::textBoxTextColourId, Look::colour (theme, "ink"));
            knob->slider.setColour (juce::Slider::textBoxBackgroundColourId, Look::colour (theme, "panel-in"));
            knob->slider.setColour (juce::Slider::textBoxOutlineColourId, Look::colour (theme, "rule"));
            knob->slider.setColour (juce::Slider::rotarySliderFillColourId, Look::colour (theme, "ink-dim"));
            knob->slider.setColour (juce::Slider::rotarySliderOutlineColourId, Look::colour (theme, "panel-in"));
            knob->slider.setColour (juce::Slider::thumbColourId, Look::colour (theme, "ink"));
        }

        for (auto* button : { &inSwitch, &compSwitch, &gateSwitchButton })
        {
            button->setColour (juce::ToggleButton::textColourId, Look::colour (theme, "ink"));
            button->setColour (juce::ToggleButton::tickColourId, Look::colour (theme, "ink"));
            button->setColour (juce::ToggleButton::tickDisabledColourId, Look::colour (theme, "ink-dim"));
        }

        resized();
        repaint();
    }

    bool DynPanelComponent::onFade() const noexcept
    {
        return ! reading.dyn.fadeId.empty();
    }

    void DynPanelComponent::write (const std::string& row, const std::string& text)
    {
        if (! actions.set || ! reading.dyn.present)
            return;

        /*  ON A FADE, THROUGH THE FADE'S DOOR (§141, BIF): the number becomes
            one of what the fade moves, and an empty text takes it out - never
            the target's own row. A switch, the detector or the key is not a
            fade's to move, and says so. */
        if (onFade())
        {
            if (! cue::isMovableDynRow (row))
            {
                if (actions.say)
                    actions.say ("A fade moves numbers, not switches - set those on the cue itself.");

                return;
            }

            actions.set (model::fadeMoveAddress (reading.dyn.fadeId, "dyn/" + row), text);
            return;
        }

        actions.set (model::dynRowAt (reading.dyn, row), text);
    }

    juce::Slider* DynPanelComponent::knobFor (const std::string& row)
    {
        for (auto& knob : knobs)
            if (knob->row == row)
                return &knob->slider;

        return nullptr;
    }

    juce::ToggleButton* DynPanelComponent::movesFor (const std::string& row)
    {
        for (auto& knob : knobs)
            if (knob->row == row)
                return &knob->moves;

        return nullptr;
    }

    juce::String DynPanelComponent::meterWords() const
    {
        if (! reading.dyn.present || ! reading.dyn.running || ! reading.dyn.settings.on)
            return juce::String::fromUTF8 ("\xe2\x80\x93");

        return juce::String (std::abs (reading.dyn.reductionDb), 1) + " dB";
    }

    //==============================================================================
    void DynPanelComponent::show (const model::FootReading& readingToUse)
    {
        reading = readingToUse;
        const auto& dyn = reading.dyn;
        const auto& s = dyn.settings;

        updating = true;

        for (auto* child : getChildren())
            child->setVisible (dyn.present);

        //  A FADE MOVES NUMBERS, NOT SWITCHES (BIF): on a fade they show the target's and take no hand.
        for (auto* control : std::initializer_list<juce::Component*> { &inSwitch, &compSwitch, &gateSwitchButton,
                                                                       &detector, &keyMenu, &reset })
            control->setEnabled (! onFade());

        inSwitch.setToggleState (s.on, juce::dontSendNotification);
        compSwitch.setToggleState (s.compOn, juce::dontSendNotification);
        gateSwitchButton.setToggleState (s.gateOn, juce::dontSendNotification);
        detector.setSelectedId (s.rms ? 2 : 1, juce::dontSendNotification);

        for (auto& knob : knobs)
        {
            //  A knob in a hand is the hand's until it lets go, the send mixer's rule.
            if (! knob->slider.isMouseButtonDown())
                knob->slider.setValue (model::dynNumberOf (s, knob->row), juce::dontSendNotification);

            /*  ON A FADE, WHAT IT MOVES IS LIT, as the EQ panel's: the name in the
                accent and its box ticked - the tick the carrier, the colour the
                help (§4.8). What it leaves alone is the target's, dimmed. */
            knob->moves.setVisible (dyn.present && onFade());

            if (onFade())
            {
                const auto moved = dyn.fadeMoves.count (knob->row) > 0;
                knob->moves.setToggleState (moved, juce::dontSendNotification);
                knob->name.setColour (juce::Label::textColourId, Look::colour (theme, moved ? "accent" : "ink-off"));
                knob->slider.setAlpha (moved ? 1.0f : 0.5f);
                continue;
            }

            knob->name.setColour (juce::Label::textColourId, Look::colour (theme, "ink-dim"));

            const auto live = knob->group == Group::compressor ? s.compOn
                            : knob->group == Group::gate      ? s.gateOn
                                                              : true;
            knob->slider.setAlpha (s.on && live ? 1.0f : 0.5f);
        }

        refreshKey();
        updating = false;
        repaint (meterArea);
    }

    void DynPanelComponent::refreshKey()
    {
        const auto& dyn = reading.dyn;
        std::vector<std::string> values;
        std::vector<std::string> labels;

        for (const auto& choice : dyn.keyChoices)
        {
            values.push_back (choice.value);
            labels.push_back (choice.label);
        }

        //  A key naming what the show has not got stays listed and picked (BHV).
        if (! model::keyIsThere (dyn.keyChoices, dyn.key))
        {
            values.push_back (dyn.key);
            labels.push_back (model::dynKeyWords (dyn, dyn.key));
        }

        if (values != keyValues || keyMenu.getNumItems() != static_cast<int> (labels.size()))
        {
            keyMenu.clear (juce::dontSendNotification);

            for (std::size_t i = 0; i < labels.size(); ++i)
                keyMenu.addItem (juce::String::fromUTF8 (labels[i].c_str()), static_cast<int> (i) + 1);

            keyValues = values;
        }

        const auto at = std::find (keyValues.begin(), keyValues.end(), dyn.key) - keyValues.begin();
        keyMenu.setSelectedId (static_cast<int> (at) + 1, juce::dontSendNotification);
    }

    //==============================================================================
    void DynPanelComponent::resized()
    {
        auto area = getLocalBounds().reduced (scaled (8), scaled (6));

        auto top = area.removeFromTop (scaled (24));
        inSwitch.setBounds (top.removeFromLeft (scaled (120)));
        top.removeFromLeft (scaled (8));
        detector.setBounds (top.removeFromLeft (scaled (80)));
        top.removeFromLeft (scaled (8));
        reset.setBounds (top.removeFromRight (scaled (70)));
        top.removeFromRight (scaled (8));
        keyWordArea = top.removeFromLeft (scaled (32));
        keyMenu.setBounds (top.removeFromLeft (std::min (top.getWidth(), scaled (260))));
        liveWordArea = top.reduced (scaled (8), 0);

        area.removeFromTop (scaled (6));
        meterArea = area.removeFromRight (scaled (70));
        area.removeFromRight (scaled (8));

        //  Six knobs, five, two: the width shared by how many each group holds.
        const auto unit = area.getWidth() / 13;
        compArea = area.removeFromLeft (unit * 6);
        gateArea = area.removeFromLeft (unit * 5);
        keyArea = area;

        const auto layGroup = [this] (juce::Rectangle<int> box, Group group, juce::ToggleButton* button)
        {
            auto head = box.removeFromTop (scaled (22));

            if (button != nullptr)
                button->setBounds (head.removeFromLeft (scaled (120)));

            std::vector<Knob*> members;

            for (auto& knob : knobs)
                if (knob->group == group)
                    members.push_back (knob.get());

            if (members.empty())
                return;

            const auto width = box.getWidth() / static_cast<int> (members.size());

            for (auto* knob : members)
            {
                auto cell = box.removeFromLeft (width).reduced (scaled (2), 0);
                auto label = cell.removeFromTop (scaled (16));
                knob->moves.setBounds (label.removeFromLeft (scaled (18)));   // shown on a fade only
                knob->name.setBounds (label);
                knob->slider.setBounds (cell);
            }
        };

        layGroup (compArea, Group::compressor, &compSwitch);
        layGroup (gateArea, Group::gate, &gateSwitchButton);
        layGroup (keyArea, Group::key, nullptr);
    }

    void DynPanelComponent::paint (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "panel"));

        if (! reading.dyn.present)
        {
            g.setColour (Look::colour (theme, "ink-dim"));
            g.drawFittedText (juce::String::fromUTF8 (reading.dyn.notice.c_str()), getLocalBounds().reduced (scaled (12)),
                              juce::Justification::centred, 2);
            return;
        }

        //  The groups' frames, and the key filter's name where the others have a switch.
        g.setColour (Look::colour (theme, "rule"));

        for (const auto& box : { compArea, gateArea, keyArea })
            g.drawRoundedRectangle (box.toFloat().reduced (1.0f), 4.0f, 1.0f);

        g.setColour (Look::colour (theme, "ink-dim"));
        g.drawText ("Key", keyWordArea, juce::Justification::centredLeft);
        g.drawText ("Key filter", keyArea.withHeight (scaled (22)).reduced (scaled (6), 0),
                    juce::Justification::centredLeft);

        /*  RIDING LIVE ON A LOCKED SHOW (§141, BIE): heard, and not saved until
            the window's bar keeps it - said in words, as the EQ panel says it. */
        if (! reading.dyn.live.empty())
            g.drawText ("live, not saved", liveWordArea, juce::Justification::centredRight, true);

        //  THE REDUCTION: a bar down from nought, and its number - never the colour alone.
        auto meter = meterArea;
        g.drawText ("Reduction", meter.removeFromTop (scaled (16)), juce::Justification::centred);
        const auto words = meter.removeFromBottom (scaled (20));
        const auto bar = meter.reduced (scaled (20), scaled (2));

        g.setColour (Look::colour (theme, "panel-in"));
        g.fillRect (bar);
        g.setColour (Look::colour (theme, "rule"));
        g.drawRect (bar);

        if (reading.dyn.running && reading.dyn.settings.on)
        {
            const auto depth = std::clamp (reading.dyn.reductionDb / meterFloorDb, 0.0, 1.0);
            const auto filled = bar.withHeight (juce::roundToInt (bar.getHeight() * depth));
            g.setColour (Look::colour (theme, "live"));
            g.fillRect (filled);
        }

        g.setColour (Look::colour (theme, "ink"));
        g.drawText (meterWords(), words, juce::Justification::centred);
    }
}
