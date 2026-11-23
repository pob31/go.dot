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
    step, the key kept. BELOW, TWO GRAPHS (the author, 2026-11-23: "2 graphs
    instead of the large dials"): the compressor's and the gate's, each what
    comes out for what goes in, in decibels both ways, drawn by the voice's
    own law. A handle on each threshold, dragged across; the compressor's
    ratio by its handle at the right, dragged up and down, its knee by the
    wheel; the gate's range by its handle under the threshold. Beside each
    graph, its numbers in bars - every one of them written in its unit, typed
    into or dragged - and the key filter's two after them.
    AT THE RIGHT, THE REDUCTION: a bar down from nought and its number in
    decibels - never the colour alone (PRD §4.8) - or a dash while nothing of
    the cue is sounding.

    EVERY TURN IS A `node.set` on the cue's own row: a decision the show keeps,
    coalesced by the engine as a drag goes, heard at once on a sounding cue.

    ON A FADE (§141, BIF) it draws the target's stage with the fade's numbers
    over it, a tick box before each number: ticked, the fade moves that number
    to the bar; cleared, it leaves it alone. A graph's handle is a write to
    its number, so dragging one on a fade ticks it. Every write goes through the fade's
    door (`moves/dyn/<row>`), never the target's row; the switches, the
    detector, the key and Reset show the target's and take no hand.
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

        /*  FOR A TEST, without inventing a mouse: the number bar of a row, by
            the row's name ("dynThreshold"), or null; the switches; the menus;
            what the meter says in words; and the two graphs. */
        juce::Slider* knobFor (const std::string& row);
        juce::ToggleButton* movesFor (const std::string& row);
        juce::ToggleButton& stageSwitch() noexcept      { return inSwitch; }
        juce::ToggleButton& compressorSwitch() noexcept { return compSwitch; }
        juce::ToggleButton& gateSwitch() noexcept       { return gateSwitchButton; }
        juce::ComboBox& detectorMenu() noexcept         { return detector; }
        juce::ComboBox& keyMenuBox() noexcept           { return keyMenu; }
        juce::TextButton& resetButton() noexcept        { return reset; }
        juce::String meterWords() const;
        juce::Component& compressorGraph() noexcept;
        juce::Component& gateGraph() noexcept;

    private:
        struct Knob;
        struct Graph;

        int scaled (int base) const;
        void write (const std::string& row, const std::string& text);
        bool onFade() const noexcept;
        double numberOf (const std::string& row);
        void turn (const std::string& row, double value);
        void refreshKey();

        model::Theme theme;
        Actions actions;
        model::FootReading reading;

        juce::ToggleButton inSwitch, compSwitch, gateSwitchButton;
        juce::ComboBox detector, keyMenu;
        juce::TextButton reset;
        std::vector<std::string> keyValues;

        std::vector<std::unique_ptr<Knob>> knobs;
        std::unique_ptr<Graph> compGraph, gateGraph_;
        juce::Rectangle<int> compArea, gateArea, keyArea, meterArea, keyWordArea, liveWordArea;

        /*  The row a graph's handle is holding, which the document does not
            overwrite until the hand lets go - the send mixer's rule. */
        std::string heldRow;

        bool updating = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DynPanelComponent)
    };
}
