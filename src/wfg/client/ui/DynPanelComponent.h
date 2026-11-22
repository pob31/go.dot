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

#pragma once

/*
    A sound's own dynamics stage at the foot (namespace draft §141, SC.8): the
    compressor, the gate and the filter on what they hear, what the stage
    listens to, and what it is taking off.

    ACROSS THE TOP: the stage's switch, the detector (peak or RMS, the author's
    question of 2026-11-22), the Key menu and Reset - one command, one undo
    step, the key kept. BELOW, three groups of knobs, each with its own switch
    where it has one, and every number written beside its knob in its unit.
    AT THE RIGHT, THE REDUCTION: a bar down from nought and its number in
    decibels - never the colour alone (PRD §4.8) - or a dash while nothing of
    the cue is sounding.

    EVERY TURN IS A `node.set` on the cue's own row: a decision the show keeps,
    coalesced by the engine as a drag goes, heard at once on a sounding cue.
*/

#include <wfg/client/model/Foot.h>
#include <wfg/client/model/Theme.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wfg::client::ui
{
    class DynPanelComponent final : public juce::Component
    {
    public:
        struct Actions
        {
            /** One `node.set` on one of the stage's rows. */
            std::function<void (const std::string& address, const std::string& text)> set;

            /** `dyn.reset` on the cue. */
            std::function<void (const std::string& cueId)> reset;

            /** A sentence for the panel's head. */
            std::function<void (const juce::String&)> say;
        };

        DynPanelComponent (const model::Theme&, Actions);
        ~DynPanelComponent() override;

        void applyTheme (const model::Theme&);

        /** The reading for this pass. */
        void show (const model::FootReading&);

        void paint (juce::Graphics&) override;
        void resized() override;

        /*  FOR A TEST, without inventing a mouse: the knob of a row, by the
            row's name ("dynThreshold"), or null; the switches; the menus; and
            what the meter says in words. */
        juce::Slider* knobFor (const std::string& row);
        juce::ToggleButton& stageSwitch() noexcept      { return inSwitch; }
        juce::ToggleButton& compressorSwitch() noexcept { return compSwitch; }
        juce::ToggleButton& gateSwitch() noexcept       { return gateSwitchButton; }
        juce::ComboBox& detectorMenu() noexcept         { return detector; }
        juce::ComboBox& keyMenuBox() noexcept           { return keyMenu; }
        juce::TextButton& resetButton() noexcept        { return reset; }
        juce::String meterWords() const;

    private:
        struct Knob;

        int scaled (int base) const;
        void write (const std::string& row, const std::string& text);
        void refreshKey();

        model::Theme theme;
        Actions actions;
        model::FootReading reading;

        juce::ToggleButton inSwitch, compSwitch, gateSwitchButton;
        juce::ComboBox detector, keyMenu;
        juce::TextButton reset;
        std::vector<std::string> keyValues;

        std::vector<std::unique_ptr<Knob>> knobs;
        juce::Rectangle<int> compArea, gateArea, keyArea, meterArea, keyWordArea;

        bool updating = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DynPanelComponent)
    };
}
