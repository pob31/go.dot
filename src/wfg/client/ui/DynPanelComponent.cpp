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

        //  The ratio the gate's expander works at (CueDyn, SC.6): a hundred to one.
        constexpr double gateRatio = 100.0;

        /*  THE COMPRESSOR'S LAW, spatcore's DynamicsModule's to the letter: a
            gain of (1/R - 1) dB a decibel over the threshold, the knee a
            quadratic between, then the make-up. What comes out for `in`. */
        double compressorOut (double in, double threshold, double ratio, double knee, double makeup)
        {
            const auto slope = 1.0 / std::max (ratio, 1.0) - 1.0;
            const auto over = in - threshold;
            const auto half = 0.5 * knee;
            auto gain = 0.0;

            if (over >= half)
                gain = slope * over;
            else if (over > -half)
            {
                const auto t = over + half;
                gain = slope * t * t / (2.0 * knee);
            }

            return in + gain + makeup;
        }

        //  THE GATE'S: (R - 1) dB down a decibel under the threshold, floored by the range.
        double gateOut (double in, double threshold, double range)
        {
            const auto over = in - threshold;
            return over >= 0.0 ? in : in + std::max (range, (gateRatio - 1.0) * over);
        }
    }

    //==============================================================================
    struct DynPanelComponent::Knob
    {
        std::string row;
        Group group;
        juce::Label name;
        juce::Slider slider { juce::Slider::LinearBar, juce::Slider::TextBoxLeft };
        juce::ToggleButton moves;   ///< on a fade only: whether it moves this number (BIF)
    };

    //==============================================================================
    /*  ONE GRAPH: what comes out across what goes in, both in decibels, the
        line where nothing is done drawn faint under the law. Two handles -
        round on the threshold, square on the other (ratio or range), two
        shapes as well as two words (PRD §4.8). */
    struct DynPanelComponent::Graph final : public juce::Component, public juce::SettableTooltipClient
    {
        Graph (DynPanelComponent& ownerToUse, Group groupToUse)
            : owner (ownerToUse), group (groupToUse)
        {
            setTooltip (group == Group::compressor
                          ? "Drag the round handle across for the threshold, the square one up and down for"
                            " the ratio; the wheel over the graph widens or narrows the knee"
                          : "Drag the round handle across for the threshold, the square one up and down for"
                            " how far the gate takes the sound down");
        }

        bool isCompressor() const noexcept { return group == Group::compressor; }

        //  The decibels the graph spans, across and up.
        double inLow() const noexcept  { return isCompressor() ? -60.0 : -90.0; }
        double outLow() const noexcept { return inLow(); }
        double outHigh() const noexcept { return isCompressor() ? 12.0 : 0.0; }

        juce::Rectangle<float> field() const
        {
            const auto left = static_cast<float> (owner.scaled (28));
            const auto bottom = static_cast<float> (owner.scaled (14));
            return getLocalBounds().toFloat().withTrimmedLeft (left).withTrimmedBottom (bottom).reduced (2.0f, 4.0f);
        }

        float xFor (double db) const
        {
            const auto f = field();
            return f.getX() + static_cast<float> ((db - inLow()) / (0.0 - inLow())) * f.getWidth();
        }

        float yFor (double db) const
        {
            const auto f = field();
            return f.getBottom() - static_cast<float> ((db - outLow()) / (outHigh() - outLow())) * f.getHeight();
        }

        double dbAtX (float x) const
        {
            const auto f = field();
            return inLow() + (0.0 - inLow()) * static_cast<double> ((x - f.getX()) / f.getWidth());
        }

        double dbAtY (float y) const
        {
            const auto f = field();
            return outLow() + (outHigh() - outLow()) * static_cast<double> ((f.getBottom() - y) / f.getHeight());
        }

        double out (double in)
        {
            if (isCompressor())
                return compressorOut (in, owner.numberOf ("dynThreshold"), owner.numberOf ("dynRatio"),
                                      owner.numberOf ("dynKnee"), owner.numberOf ("dynMakeup"));

            return gateOut (in, owner.numberOf ("dynGateThreshold"), owner.numberOf ("dynGateRange"));
        }

        const char* thresholdRow() const noexcept { return isCompressor() ? "dynThreshold" : "dynGateThreshold"; }
        const char* otherRow() const noexcept     { return isCompressor() ? "dynRatio" : "dynGateRange"; }

        /*  WHERE THE HANDLES ARE: the threshold on the line; the compressor's
            ratio at the right, where the loudest sound comes out; the gate's
            range under its threshold, where the floor is. Kept inside the
            field, so a handle pushed past its edge is still there to take. */
        juce::Point<float> placeOf (int handle)
        {
            const auto threshold = owner.numberOf (thresholdRow());
            const auto f = field();
            juce::Point<float> at;

            if (handle == 0)
                at = { xFor (threshold), yFor (out (threshold)) };
            else if (isCompressor())
                at = { xFor (0.0), yFor (out (0.0)) };
            else
                at = { xFor (threshold), yFor (threshold + owner.numberOf ("dynGateRange")) };

            return { std::clamp (at.x, f.getX(), f.getRight()), std::clamp (at.y, f.getY(), f.getBottom()) };
        }

        int handleAt (juce::Point<float> point)
        {
            const auto reach = static_cast<float> (owner.scaled (10));
            auto best = -1;
            auto bestDistance = reach;

            for (int handle = 0; handle < 2; ++handle)
            {
                const auto distance = placeOf (handle).getDistanceFrom (point);

                if (distance <= bestDistance)
                {
                    best = handle;
                    bestDistance = distance;
                }
            }

            return best;
        }

        void paint (juce::Graphics& g) override
        {
            const auto& look = owner.theme;
            const auto f = field();
            const auto& s = owner.reading.dyn.settings;
            const auto live = s.on && (isCompressor() ? s.compOn : s.gateOn);

            g.setColour (Look::colour (look, "panel-in"));
            g.fillRect (f);

            /*  THE DECIBELS ON BOTH AXES, in words: a graph with no numbers is
                a picture that cannot be read across a booth. */
            g.setFont (Look::font (look, 10.0f));
            const auto step = isCompressor() ? 12.0 : 15.0;

            for (auto db = inLow(); db <= 0.0 + 1.0e-6; db += step)
            {
                const auto x = xFor (db);
                g.setColour (Look::colour (look, "rule"));
                g.drawVerticalLine (juce::roundToInt (x), f.getY(), f.getBottom());
                g.setColour (Look::colour (look, "ink-faint"));
                g.drawText (juce::String (juce::roundToInt (db)),
                            juce::Rectangle<float> (x - 18.0f, f.getBottom() + 1.0f, 36.0f,
                                                    static_cast<float> (owner.scaled (12))),
                            juce::Justification::centred, false);
            }

            for (auto db = outLow(); db <= outHigh() + 1.0e-6; db += step)
            {
                const auto y = yFor (db);
                g.setColour (Look::colour (look, std::abs (db) < 0.5 ? "ink-off" : "rule"));
                g.drawHorizontalLine (juce::roundToInt (y), f.getX(), f.getRight());
                g.setColour (Look::colour (look, "ink-faint"));
                g.drawText ((db > 0.0 ? "+" : "") + juce::String (juce::roundToInt (db)),
                            juce::Rectangle<float> (0.0f, y - 7.0f, f.getX() - 3.0f, 14.0f),
                            juce::Justification::centredRight, false);
            }

            g.saveState();
            g.reduceClipRegion (f.toNearestInt());

            //  WHERE NOTHING IS DONE, the line out equals in - faint, under the law.
            g.setColour (Look::colour (look, "ink-off"));
            g.drawLine (xFor (inLow()), yFor (inLow()), xFor (0.0), yFor (0.0), 1.0f);

            juce::Path law;
            const auto points = std::max (2, juce::roundToInt (f.getWidth()));

            for (int at = 0; at <= points; ++at)
            {
                const auto in = inLow() + (0.0 - inLow()) * at / points;
                const auto x = xFor (in);
                const auto y = yFor (out (in));

                if (at == 0)
                    law.startNewSubPath (x, y);
                else
                    law.lineTo (x, y);
            }

            /*  DIMMED WHEN IT IS OUT, as the EQ's curve: the shape is still there
                to be read and put back, and the switch says so in words. */
            g.setColour (Look::colour (look, live ? "accent" : "ink-off"));
            g.strokePath (law, juce::PathStrokeType (2.0f));
            g.restoreState();

            const auto radius = static_cast<float> (owner.scaled (6));
            static const char* const words[2][2] = { { "threshold", "ratio" }, { "threshold", "range" } };

            for (int handle = 0; handle < 2; ++handle)
            {
                const auto place = placeOf (handle);
                const auto box = juce::Rectangle<float> (place.x - radius, place.y - radius, 2.0f * radius, 2.0f * radius);
                auto colour = Look::colour (look, "ink");

                if (handle == hovered || handle == dragged)
                    colour = Look::colour (look, "picked");

                g.setColour (colour);

                if (handle == 0)
                    g.fillEllipse (box);
                else
                    g.fillRect (box);

                //  ITS NAME BESIDE IT, on the side with room - a handle is never only a dot.
                g.setColour (Look::colour (look, "ink-dim"));
                const auto word = juce::String (words[isCompressor() ? 0 : 1][handle]);
                const auto wide = static_cast<float> (owner.scaled (60));
                const auto toLeft = place.x + radius + wide > f.getRight();
                const auto lift = toLeft ? radius + 7.0f : 0.0f;   // over the line, not on it
                const auto area = toLeft ? juce::Rectangle<float> (place.x - wide, place.y - 7.0f - lift, wide - 2.0f, 14.0f)
                                         : juce::Rectangle<float> (place.x + radius + 2.0f, place.y - 7.0f, wide, 14.0f);
                g.drawText (word, area, toLeft ? juce::Justification::centredRight : juce::Justification::centredLeft, false);
            }
        }

        void mouseMove (const juce::MouseEvent& e) override
        {
            const auto now = handleAt (e.position);

            if (now != hovered)
            {
                hovered = now;
                setMouseCursor (now < 0 ? juce::MouseCursor::NormalCursor
                                        : now == 0 ? juce::MouseCursor::LeftRightResizeCursor
                                                   : juce::MouseCursor::UpDownResizeCursor);
                repaint();
            }
        }

        void mouseExit (const juce::MouseEvent&) override
        {
            hovered = -1;
            repaint();
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            dragged = handleAt (e.position);

            if (dragged >= 0)
                owner.heldRow = dragged == 0 ? thresholdRow() : otherRow();

            repaint();
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (dragged < 0)
                return;

            if (dragged == 0)
            {
                owner.turn (thresholdRow(), dbAtX (e.position.x));
            }
            else if (isCompressor())
            {
                /*  THE RATIO FROM WHERE THE LOUDEST SOUND COMES OUT: at nought
                    in, T + (0 - T)/R + make-up out, turned round for R. */
                const auto threshold = owner.numberOf ("dynThreshold");
                const auto above = dbAtY (e.position.y) - owner.numberOf ("dynMakeup") - threshold;

                if (threshold < -0.5)
                    owner.turn ("dynRatio", above <= -threshold / 100.0 ? 100.0 : -threshold / above);
            }
            else
            {
                owner.turn ("dynGateRange", dbAtY (e.position.y) - owner.numberOf ("dynGateThreshold"));
            }

            repaint();
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            dragged = -1;
            owner.heldRow.clear();
            repaint();
        }

        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
        {
            //  THE KNEE BY THE WHEEL, a decibel a notch - the EQ's width by its wheel.
            if (! isCompressor() || juce::exactlyEqual (wheel.deltaY, 0.0f))
            {
                Component::mouseWheelMove (e, wheel);
                return;
            }

            owner.turn ("dynKnee", owner.numberOf ("dynKnee") + (wheel.deltaY > 0.0f ? 1.0 : -1.0));
            repaint();
        }

        DynPanelComponent& owner;
        Group group;
        int hovered = -1, dragged = -1;
    };

    //==============================================================================
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

        //  Ranges are the table's; the middle of each bar's travel where the ear works most.
        static const Spec specs[] = {
            { "dynThreshold",     "Threshold", Group::compressor, -60.0, 0.0, -20.0, 1, " dB" },
            { "dynRatio",         "Ratio",     Group::compressor, 1.0, 100.0, 4.0, 1, ":1" },
            { "dynKnee",          "Knee",      Group::compressor, 0.0, 24.0, 6.0, 1, " dB" },
            { "dynMakeup",        "Make-up",   Group::compressor, -24.0, 24.0, 0.0, 1, " dB" },
            { "dynAttack",        "Attack",    Group::compressor, 0.05, 200.0, 10.0, 2, " ms" },
            { "dynRelease",       "Release",   Group::compressor, 5.0, 2000.0, 150.0, 0, " ms" },
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
            knob->name.setJustificationType (juce::Justification::centredLeft);
            knob->slider.setRange (spec.low, spec.high, 0.0);

            if (spec.middle > spec.low && spec.middle < spec.high)
                knob->slider.setSkewFactorFromMidPoint (spec.middle);

            /*  A BAR, DRAGGED FROM WHERE IT IS rather than jumping to the
                pointer, and typed into with a click - its number written
                inside it, in its unit. */
            knob->slider.setSliderSnapsToMousePosition (false);
            knob->slider.setNumDecimalPlacesToDisplay (spec.decimals);
            knob->slider.setTextValueSuffix (spec.unit);
            knob->slider.setWantsKeyboardFocus (false);

            const auto row = knob->row;
            const auto decimals = spec.decimals;
            auto* slider = &knob->slider;
            knob->slider.onValueChange = [this, row, decimals, slider]
            {
                if (! updating)
                    write (row, juce::String (slider->getValue(), std::max (decimals, 2)).toStdString());
            };

            /*  ON A FADE, A TICK BOX BEFORE THE NUMBER (§141, BIF), as the EQ
                panel's beside its numbers: ticked, the fade moves the row to
                the number; cleared, it leaves the row alone. Ticking one it
                did not move starts it at what the target holds now. */
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

        compGraph = std::make_unique<Graph> (*this, Group::compressor);
        gateGraph_ = std::make_unique<Graph> (*this, Group::gate);
        addAndMakeVisible (*compGraph);
        addAndMakeVisible (*gateGraph_);

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
            knob->slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
            knob->slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            knob->slider.setColour (juce::Slider::backgroundColourId, Look::colour (theme, "panel-in"));
            knob->slider.setColour (juce::Slider::trackColourId, Look::colour (theme, "rule"));
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

    /*  WHAT THE GRAPHS DRAW is what the bars say - the hand's number while a
        hand is on one, the document's otherwise. */
    double DynPanelComponent::numberOf (const std::string& row)
    {
        if (auto* slider = knobFor (row))
            return slider->getValue();

        return 0.0;
    }

    //  A graph's handle is a turn of its number's bar: one write, the bar's own.
    void DynPanelComponent::turn (const std::string& row, double value)
    {
        if (auto* slider = knobFor (row))
        {
            slider->setValue (value, juce::sendNotificationSync);
            compGraph->repaint();
            gateGraph_->repaint();
        }
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

    juce::Component& DynPanelComponent::compressorGraph() noexcept
    {
        return *compGraph;
    }

    juce::Component& DynPanelComponent::gateGraph() noexcept
    {
        return *gateGraph_;
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
            //  A number in a hand - its bar's or its graph's handle's - is the hand's until it lets go.
            if (! knob->slider.isMouseButtonDown() && knob->row != heldRow)
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
        compGraph->repaint();
        gateGraph_->repaint();
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

        /*  THE KEY FILTER'S TWO NUMBERS IN A NARROW COLUMN, and the rest
            shared by the two groups: each a graph and the column of its
            numbers beside it. */
        keyArea = area.removeFromRight (std::min (area.getWidth() / 5, scaled (190)));
        area.removeFromRight (scaled (6));
        const auto half = (area.getWidth() - scaled (6)) / 2;
        compArea = area.removeFromLeft (half);
        area.removeFromLeft (scaled (6));
        gateArea = area;

        const auto layNumbers = [this] (juce::Rectangle<int> column, Group group)
        {
            std::vector<Knob*> members;

            for (auto& knob : knobs)
                if (knob->group == group)
                    members.push_back (knob.get());

            if (members.empty())
                return;

            //  Six rows for the compressor; the gate's five and the filter's two as tall as those.
            const auto rowHeight = std::min (column.getHeight() / 6, scaled (26));

            for (auto* knob : members)
            {
                auto line = column.removeFromTop (rowHeight).reduced (0, scaled (2));
                knob->moves.setBounds (line.removeFromLeft (scaled (20)));   // shown on a fade only
                knob->name.setBounds (line.removeFromLeft (scaled (72)));
                knob->slider.setBounds (line);
                knob->slider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, line.getWidth(), line.getHeight());
            }
        };

        const auto layGroup = [this, &layNumbers] (juce::Rectangle<int> box, Group group,
                                                   juce::ToggleButton& button, Graph& graph)
        {
            auto inner = box.reduced (scaled (6), scaled (4));
            button.setBounds (inner.removeFromTop (scaled (22)).removeFromLeft (scaled (130)));
            inner.removeFromTop (scaled (2));

            //  The numbers' column a fixed width, the graph what is left - as wide as it is tall, or wider.
            auto numbers = inner.removeFromRight (std::min (scaled (230), inner.getWidth() / 2));
            inner.removeFromRight (scaled (8));
            graph.setBounds (inner);
            layNumbers (numbers, group);
        };

        layGroup (compArea, Group::compressor, compSwitch, *compGraph);
        layGroup (gateArea, Group::gate, gateSwitchButton, *gateGraph_);

        auto keyInner = keyArea.reduced (scaled (6), scaled (4));
        keyInner.removeFromTop (scaled (24));
        layNumbers (keyInner, Group::key);
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

        /*  EVERY NUMBER'S BOX, under its bar: the bar fills only as far as
            its number, and one at its foot would otherwise be no box at all. */
        for (const auto& knob : knobs)
        {
            g.setColour (Look::colour (theme, "panel-in").withMultipliedAlpha (knob->slider.getAlpha()));
            g.fillRect (knob->slider.getBounds());
        }

        //  The groups' frames, and the key filter's name where the others have a switch.
        g.setColour (Look::colour (theme, "rule"));

        for (const auto& box : { compArea, gateArea, keyArea })
            g.drawRoundedRectangle (box.toFloat().reduced (1.0f), 4.0f, 1.0f);

        g.setColour (Look::colour (theme, "ink-dim"));
        g.setFont (Look::font (theme, 13.0f));
        g.drawText ("Key", keyWordArea, juce::Justification::centredLeft);
        g.drawText ("Key filter", keyArea.reduced (scaled (8), scaled (4)).withHeight (scaled (22)),
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
