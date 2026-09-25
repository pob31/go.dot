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

#include <wfg/client/ui/EqPanelComponent.h>

#include <wfg/client/ui/Look.h>
#include <wfg/engine/osc/OscValue.h>

#include <spatcore/ui/TypedValue.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace wfg::client::ui
{
    namespace
    {
        int scaled (int base, const model::Theme& theme)
        {
            return juce::roundToInt (static_cast<float> (base) * theme.type);
        }

        constexpr double lowestHz = 20.0;
        constexpr double highestHz = 20000.0;

        constexpr int curvePoints = 240;

        /** A frequency as a person reads it: 80 Hz, 1.2 kHz. */
        juce::String hertzText (double frequency)
        {
            if (frequency >= 1000.0)
                return juce::String (frequency / 1000.0, frequency >= 10000.0 ? 0 : 1) + " kHz";

            return juce::String (juce::roundToInt (frequency)) + " Hz";
        }

        std::string numberText (double value, int decimals)
        {
            const auto factor = std::pow (10.0, decimals);
            return osc::formatDouble (std::round (value * factor) / factor);
        }
    }

    //==============================================================================
    /*  A NUMBER BOX FOR ONE ROW: what it says is the row's value, and typing
        into it is the same write a drag makes. An editable label, as the send
        mixer's value boxes are, because that is the one control a test can
        drive without inventing a mouse event. */
    struct EqPanelComponent::Box
    {
        Box (EqPanelComponent& ownerToUse, std::string rowToUse, int decimalsToUse,
             juce::String unitToUse)
            : owner (ownerToUse), row (std::move (rowToUse)), decimals (decimalsToUse),
              unit (std::move (unitToUse))
        {
            value.setEditable (true, false, false);
            value.setJustificationType (juce::Justification::centredRight);
            value.setTooltip (juce::String (row) + (unit.isNotEmpty() ? ", in " + unit : ""));

            value.onTextChange = [this]
            {
                /*  READ THE WAY A PERSON TYPES IT, which is spatcore's typed
                    reader and the one WFS-DIY's fields use: "2.5 kHz" in a
                    frequency box is 2500, "-3 dB" is -3, a comma is a decimal
                    point. A text with no number in it puts the box back
                    rather than writing nought - nought is a real gain. */
                const auto typed = spatcore::ui::typed::number (value.getText());

                if (typed.has_value())
                    owner.writeNumber (row, static_cast<double> (*typed), decimals);
                else
                    owner.refreshControls();
            };
        }

        double current() const
        {
            const auto& s = owner.shown();

            if (row == "eqHpfFreq") return s.hpfFreq;
            if (row == "eqLpfFreq") return s.lpfFreq;

            for (int band = 0; band < audio::EqSettings::numBands; ++band)
            {
                if (row == model::eqBandRow (band, "Freq")) return s.band[band].freq;
                if (row == model::eqBandRow (band, "Gain")) return s.band[band].gain;
                if (row == model::eqBandRow (band, "Q"))    return s.band[band].q;
            }

            return 0.0;
        }

        EqPanelComponent& owner;
        std::string row;
        int decimals;
        juce::String unit;
        juce::Label value;
    };

    //==============================================================================
    EqPanelComponent::EqPanelComponent (const model::Theme& themeToUse, Actions actionsToUse)
        : theme (themeToUse), actions (std::move (actionsToUse))
    {
        onToggle.setButtonText ("EQ in");
        hpfToggle.setButtonText ("High-pass");
        lpfToggle.setButtonText ("Low-pass");
        flat.setButtonText ("Flat");

        onToggle.setTooltip ("Whether the cue's EQ is in the signal at all");
        hpfToggle.setTooltip ("Whether the high-pass is in");
        lpfToggle.setTooltip ("Whether the low-pass is in");
        flat.setTooltip ("Every band to nought and both filters out - one step to undo");

        for (auto* toggle : { &onToggle, &hpfToggle, &lpfToggle })
            toggle->setWantsKeyboardFocus (false);

        flat.setWantsKeyboardFocus (false);

        onToggle.onClick = [this] { writeFlag ("eqOn", onToggle.getToggleState()); };
        hpfToggle.onClick = [this] { writeFlag ("eqHpf", hpfToggle.getToggleState()); };
        lpfToggle.onClick = [this] { writeFlag ("eqLpf", lpfToggle.getToggleState()); };

        flat.onClick = [this]
        {
            if (actions.reset && ! reading.subject.objectId.empty())
                actions.reset (reading.subject.objectId);
        };

        /*  THE TWO SHAPE MENUS, on the bands the table lets be shelves: the
            low band a peak or a low shelf, the high band a peak or a high
            shelf. The words are the rows' own. */
        lowShape.addItem ("peak", 1);
        lowShape.addItem ("low shelf", 2);
        highShape.addItem ("peak", 1);
        highShape.addItem ("high shelf", 2);

        lowShape.onChange = [this]
        {
            write ("eqB1Shape", lowShape.getSelectedId() == 2 ? "lowShelf" : "peak");
        };

        highShape.onChange = [this]
        {
            write ("eqB4Shape", highShape.getSelectedId() == 2 ? "highShelf" : "peak");
        };

        applyTheme (theme);
    }

    EqPanelComponent::~EqPanelComponent() = default;

    void EqPanelComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;

        for (auto& box : boxes)
        {
            box->value.setFont (Look::font (theme, 12.0f));
            box->value.setColour (juce::Label::textColourId, Look::colour (theme, "ink"));
            box->value.setColour (juce::Label::backgroundColourId, Look::colour (theme, "panel-in"));
        }

        repaint();
    }

    //==============================================================================
    void EqPanelComponent::show (const model::FootReading& readingToUse)
    {
        reading = readingToUse;

        if (reading.subject.objectId != builtFor || (reading.eq.present == boxes.empty()))
            rebuildControls();
        else
            refreshControls();

        repaint();
    }

    const audio::EqSettings& EqPanelComponent::shown() const noexcept
    {
        return dragging ? held : reading.eq.settings;
    }

    void EqPanelComponent::rebuildControls()
    {
        builtFor = reading.subject.objectId;

        boxes.clear();
        removeAllChildren();

        if (! reading.eq.present)
            return;

        addAndMakeVisible (onToggle);
        addAndMakeVisible (hpfToggle);
        addAndMakeVisible (lpfToggle);
        addAndMakeVisible (lowShape);
        addAndMakeVisible (highShape);
        addAndMakeVisible (flat);

        /*  ONE BOX PER NUMBER, in the order the column draws them: the two
            filters' frequencies, then frequency, gain and width for each band. */
        boxes.push_back (std::make_unique<Box> (*this, "eqHpfFreq", 0, "Hz"));
        boxes.push_back (std::make_unique<Box> (*this, "eqLpfFreq", 0, "Hz"));

        for (int band = 0; band < audio::EqSettings::numBands; ++band)
        {
            boxes.push_back (std::make_unique<Box> (*this, model::eqBandRow (band, "Freq"), 0, "Hz"));
            boxes.push_back (std::make_unique<Box> (*this, model::eqBandRow (band, "Gain"), 1, "dB"));
            boxes.push_back (std::make_unique<Box> (*this, model::eqBandRow (band, "Q"), 2, ""));
        }

        for (auto& box : boxes)
            addAndMakeVisible (box->value);

        applyTheme (theme);
        refreshControls();
        resized();
    }

    void EqPanelComponent::refreshControls()
    {
        if (! reading.eq.present)
            return;

        const auto& s = shown();

        onToggle.setToggleState (s.on, juce::dontSendNotification);
        hpfToggle.setToggleState (s.hpf, juce::dontSendNotification);
        lpfToggle.setToggleState (s.lpf, juce::dontSendNotification);

        lowShape.setSelectedId (s.band[0].shape == audio::EqSettings::Shape::lowShelf ? 2 : 1,
                                juce::dontSendNotification);
        highShape.setSelectedId (s.band[3].shape == audio::EqSettings::Shape::highShelf ? 2 : 1,
                                 juce::dontSendNotification);

        for (auto& box : boxes)
        {
            if (box->value.isBeingEdited())
                continue;

            box->value.setText (juce::String (numberText (box->current(), box->decimals)),
                                juce::dontSendNotification);
        }
    }

    //==============================================================================
    void EqPanelComponent::write (const std::string& row, const std::string& text)
    {
        if (actions.set && ! reading.subject.objectId.empty())
            actions.set (model::eqAddress (reading.subject.objectId, row), text);
    }

    void EqPanelComponent::writeNumber (const std::string& row, double value, int decimals)
    {
        /*  CLAMPED TO WHAT THE ROW TAKES, before sending rather than after:
            the door would refuse a gain of forty and say so, but a handle
            dragged off the top of the field should stop at the top. */
        auto bounded = value;

        if (row.find ("Gain") != std::string::npos)
            bounded = std::clamp (value, -rangeDb, rangeDb);
        else if (row.find ("Freq") != std::string::npos)
            bounded = std::clamp (value, lowestHz, highestHz);
        else if (row.find ('Q') != std::string::npos)
            bounded = std::clamp (value, 0.1, 10.0);

        if (row == "eqHpfFreq")
            bounded = std::clamp (bounded, 20.0, 2000.0);
        else if (row == "eqLpfFreq")
            bounded = std::clamp (bounded, 1000.0, 20000.0);

        write (row, numberText (bounded, decimals));
    }

    void EqPanelComponent::writeFlag (const std::string& row, bool on)
    {
        write (row, on ? "true" : "false");
    }

    //==============================================================================
    juce::Rectangle<int> EqPanelComponent::columnArea() const
    {
        /*  THE NUMBERS ON THE RIGHT, a column wide enough for a shape menu
            and three boxes; the field takes what is left. */
        const auto width = std::min (getWidth() / 2, scaled (330, theme));
        return getLocalBounds().removeFromRight (width).reduced (scaled (6, theme), 0);
    }

    juce::Rectangle<int> EqPanelComponent::fieldArea() const
    {
        auto area = getLocalBounds().withTrimmedRight (columnArea().getWidth() + scaled (12, theme));
        area.removeFromLeft (scaled (34, theme));     // the decibels, up the side
        area.removeFromBottom (scaled (16, theme));   // the hertz, along the bottom
        return area.reduced (scaled (4, theme));
    }

    double EqPanelComponent::xForFrequency (double frequency) const
    {
        const auto field = fieldArea();
        const auto fraction = (std::log2 (std::clamp (frequency, lowestHz, highestHz)) - std::log2 (lowestHz))
                              / (std::log2 (highestHz) - std::log2 (lowestHz));
        return field.getX() + fraction * field.getWidth();
    }

    double EqPanelComponent::frequencyForX (double x) const
    {
        const auto field = fieldArea();

        if (field.getWidth() <= 0)
            return 1000.0;

        const auto fraction = std::clamp ((x - field.getX()) / field.getWidth(), 0.0, 1.0);
        return std::exp2 (std::log2 (lowestHz) + fraction * (std::log2 (highestHz) - std::log2 (lowestHz)));
    }

    double EqPanelComponent::yForDb (double db) const
    {
        const auto field = fieldArea();
        const auto fraction = (std::clamp (db, -rangeDb, rangeDb) + rangeDb) / (2.0 * rangeDb);
        return field.getBottom() - fraction * field.getHeight();
    }

    double EqPanelComponent::dbForY (double y) const
    {
        const auto field = fieldArea();

        if (field.getHeight() <= 0)
            return 0.0;

        const auto fraction = std::clamp ((field.getBottom() - y) / field.getHeight(), 0.0, 1.0);
        return -rangeDb + fraction * 2.0 * rangeDb;
    }

    juce::Point<float> EqPanelComponent::placeOf (int handle, const audio::EqSettings& s) const
    {
        /*  A FILTER'S HANDLE SITS AT ITS CORNER, three decibels down, which
            is where the filter is and the one place on its slope a hand can
            name. */
        if (handle == hpfHandle)
            return { static_cast<float> (xForFrequency (s.hpfFreq)), static_cast<float> (yForDb (-3.0)) };

        if (handle == lpfHandle)
            return { static_cast<float> (xForFrequency (s.lpfFreq)), static_cast<float> (yForDb (-3.0)) };

        const auto& band = s.band[std::clamp (handle, 0, audio::EqSettings::numBands - 1)];
        return { static_cast<float> (xForFrequency (band.freq)), static_cast<float> (yForDb (band.gain)) };
    }

    int EqPanelComponent::handleAt (juce::Point<float> where) const
    {
        if (! reading.eq.present)
            return noHandle;

        const auto reach = static_cast<float> (scaled (9, theme));
        const auto& s = shown();
        auto best = noHandle;
        auto nearest = reach * reach;

        for (int handle = 0; handle < 6; ++handle)
        {
            const auto place = placeOf (handle, s);
            const auto distance = place.getDistanceSquaredFrom (where);

            if (distance <= nearest)
            {
                nearest = distance;
                best = handle;
            }
        }

        return best;
    }

    //==============================================================================
    void EqPanelComponent::mouseDown (const juce::MouseEvent& event)
    {
        dragged = handleAt (event.position);

        if (dragged == noHandle)
            return;

        held = reading.eq.settings;
        dragging = true;
        dragFrom = event.position;
    }

    void EqPanelComponent::mouseDrag (const juce::MouseEvent& event)
    {
        if (! dragging || dragged == noHandle)
            return;

        /*  MEASURED FROM WHERE THE HAND WENT DOWN rather than from where the
            pointer is, so a press does not jump the handle to the pointer;
            shift divides the movement by ten, the fine drag every other drag
            in this window has. */
        const auto scale = event.mods.isShiftDown() ? 0.1f : 1.0f;
        const auto origin = placeOf (dragged, reading.eq.settings);
        const auto moved = (event.position - dragFrom) * scale;
        const auto target = origin + moved;

        const auto frequency = frequencyForX (target.x);

        if (dragged == hpfHandle)
        {
            held.hpfFreq = static_cast<float> (std::clamp (frequency, 20.0, 2000.0));
            writeNumber ("eqHpfFreq", held.hpfFreq, 0);
        }
        else if (dragged == lpfHandle)
        {
            held.lpfFreq = static_cast<float> (std::clamp (frequency, 1000.0, 20000.0));
            writeNumber ("eqLpfFreq", held.lpfFreq, 0);
        }
        else
        {
            auto& band = held.band[dragged];
            band.freq = static_cast<float> (frequency);
            band.gain = static_cast<float> (dbForY (target.y));
            writeNumber (model::eqBandRow (dragged, "Freq"), band.freq, 0);
            writeNumber (model::eqBandRow (dragged, "Gain"), band.gain, 1);
        }

        refreshControls();
        repaint();
    }

    void EqPanelComponent::mouseUp (const juce::MouseEvent&)
    {
        /*  AND THE READING TAKES OVER AGAIN: by now what was asked for has
            been applied and published, and if it has not, the next pass
            corrects the handle rather than this holding a number the
            document never accepted. */
        dragging = false;
        dragged = noHandle;
        refreshControls();
        repaint();
    }

    void EqPanelComponent::mouseDoubleClick (const juce::MouseEvent& event)
    {
        const auto handle = handleAt (event.position);

        /*  A BAND TAKEN OUT is a band at nought: what the row rests at, and
            what the page's rotary click will do. A filter has its switch. */
        if (handle >= 0 && handle < audio::EqSettings::numBands)
            writeNumber (model::eqBandRow (handle, "Gain"), 0.0, 1);
    }

    void EqPanelComponent::mouseWheelMove (const juce::MouseEvent& event,
                                          const juce::MouseWheelDetails& wheel)
    {
        const auto handle = handleAt (event.position);

        if (handle < 0 || handle >= audio::EqSettings::numBands)
            return;

        const auto clicks = juce::roundToInt (wheel.deltaY * 10.0f);

        if (clicks == 0)
            return;

        /*  THE WIDTH, on the wheel over a band: a tenth narrower or wider a
            click, on a log scale, because Q is read that way. */
        const auto q = shown().band[handle].q * std::pow (event.mods.isShiftDown() ? 1.01 : 1.1, clicks);
        writeNumber (model::eqBandRow (handle, "Q"), q, 2);
    }

    void EqPanelComponent::mouseMove (const juce::MouseEvent& event)
    {
        const auto over = handleAt (event.position);

        if (over != hovered)
        {
            hovered = over;
            setMouseCursor (over == noHandle ? juce::MouseCursor::NormalCursor
                                             : juce::MouseCursor::DraggingHandCursor);
            repaint();
        }
    }

    //==============================================================================
    void EqPanelComponent::resized()
    {
        if (! reading.eq.present)
            return;

        auto column = columnArea();
        const auto row = scaled (22, theme);
        const auto gapPx = scaled (3, theme);

        /*  THE COLUMN, top to bottom: the EQ's own switch and Flat; the two
            filters, each a switch and a frequency; then a row per band with
            its shape (or its name) and its three numbers. */
        auto line = column.removeFromTop (row);
        onToggle.setBounds (line.removeFromLeft (line.getWidth() * 2 / 3));
        flat.setBounds (line.reduced (gapPx, 1));
        column.removeFromTop (gapPx);

        const auto numberWidth = scaled (62, theme);

        line = column.removeFromTop (row);
        hpfToggle.setBounds (line.removeFromLeft (line.getWidth() - numberWidth));
        if (boxes.size() > 0) boxes[0]->value.setBounds (line.reduced (gapPx, 1));
        column.removeFromTop (gapPx);

        line = column.removeFromTop (row);
        lpfToggle.setBounds (line.removeFromLeft (line.getWidth() - numberWidth));
        if (boxes.size() > 1) boxes[1]->value.setBounds (line.reduced (gapPx, 1));
        column.removeFromTop (gapPx);

        for (int band = 0; band < audio::EqSettings::numBands; ++band)
        {
            line = column.removeFromTop (row);
            auto head = line.removeFromLeft (line.getWidth() - 3 * numberWidth);

            if (band == 0)
                lowShape.setBounds (head.reduced (gapPx, 1));
            else if (band == audio::EqSettings::numBands - 1)
                highShape.setBounds (head.reduced (gapPx, 1));

            for (int which = 0; which < 3; ++which)
            {
                const auto at = static_cast<std::size_t> (2 + band * 3 + which);

                if (at < boxes.size())
                    boxes[at]->value.setBounds (line.removeFromLeft (numberWidth).reduced (gapPx, 1));
            }

            column.removeFromTop (gapPx);
        }
    }

    void EqPanelComponent::paint (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "panel"));

        if (! reading.eq.present)
        {
            g.setColour (Look::colour (theme, "ink-dim"));
            g.setFont (Look::font (theme, 13.0f));
            g.drawFittedText (juce::String (reading.eq.notice.empty() ? reading.notice
                                                                        : reading.eq.notice),
                              getLocalBounds().reduced (scaled (12, theme), scaled (6, theme)),
                              juce::Justification::centredLeft, 3);
            return;
        }

        const auto field = fieldArea();

        g.setColour (Look::colour (theme, "panel-in"));
        g.fillRect (field);

        paintGrid (g, field);
        paintCurve (g, field);
        paintHandles (g, field);

        /*  THE NAMES OF THE BANDS, beside their rows in the column, where the
            shape menu is not: two and three are always peaks and say so. */
        auto column = columnArea();
        const auto row = scaled (22, theme);
        const auto gapPx = scaled (3, theme);
        column.removeFromTop (3 * (row + gapPx));

        g.setColour (Look::colour (theme, "ink-dim"));
        g.setFont (Look::font (theme, 12.0f));

        for (int band = 0; band < audio::EqSettings::numBands; ++band)
        {
            auto line = column.removeFromTop (row);
            auto head = line.removeFromLeft (line.getWidth() - 3 * scaled (62, theme));

            if (band == 1 || band == 2)
                g.drawFittedText ("Band " + juce::String (band + 1) + ", peak",
                                  head.reduced (gapPx, 0), juce::Justification::centredLeft, 1);

            column.removeFromTop (gapPx);
        }
    }

    void EqPanelComponent::paintGrid (juce::Graphics& g, juce::Rectangle<int> field)
    {
        g.setFont (Look::font (theme, 10.0f));

        /*  THE HERTZ ALONG THE BOTTOM, at the places a frequency axis is read
            by, and the decibels up the side - in words, both, because a grid
            with no numbers is a picture that cannot be read across a booth. */
        for (const auto frequency : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        {
            const auto x = juce::roundToInt (xForFrequency (frequency));
            g.setColour (Look::colour (theme, "rule"));
            g.drawVerticalLine (x, static_cast<float> (field.getY()), static_cast<float> (field.getBottom()));
            g.setColour (Look::colour (theme, "ink-faint"));
            g.drawFittedText (hertzText (frequency), x - 22, field.getBottom() + 1, 44, scaled (14, theme),
                              juce::Justification::centred, 1);
        }

        for (const auto db : { -24.0, -12.0, 0.0, 12.0, 24.0 })
        {
            const auto y = juce::roundToInt (yForDb (db));
            const auto zero = std::abs (db) < 0.5;
            g.setColour (Look::colour (theme, zero ? "ink-off" : "rule"));
            g.drawHorizontalLine (y, static_cast<float> (field.getX()), static_cast<float> (field.getRight()));
            g.setColour (Look::colour (theme, "ink-faint"));
            g.drawFittedText ((db > 0.0 ? "+" : "") + juce::String (juce::roundToInt (db)),
                              0, y - 7, field.getX() - 4, 14, juce::Justification::centredRight, 1);
        }
    }

    void EqPanelComponent::paintCurve (juce::Graphics& g, juce::Rectangle<int> field)
    {
        const auto& s = shown();
        const auto points = model::eqCurve (s, pictureRate, curvePoints);

        if (points.empty())
            return;

        juce::Path curve;
        auto first = true;

        for (const auto& point : points)
        {
            const auto x = static_cast<float> (xForFrequency (point.frequency));
            const auto y = static_cast<float> (yForDb (point.db));

            if (first)
                curve.startNewSubPath (x, y);
            else
                curve.lineTo (x, y);

            first = false;
        }

        /*  DIMMED WHEN THE EQ IS OUT: the shape is still there to be read and
            put back, and the dimming says it is not being heard - never the
            only carrier, since the switch says so in words. */
        g.setColour (Look::colour (theme, s.on ? "accent" : "ink-off"));
        g.strokePath (curve, juce::PathStrokeType (2.0f));

        juce::ignoreUnused (field);
    }

    void EqPanelComponent::paintHandles (juce::Graphics& g, juce::Rectangle<int> field)
    {
        const auto& s = shown();
        const auto radius = static_cast<float> (scaled (6, theme));

        for (int handle = 0; handle < 6; ++handle)
        {
            const auto isFilter = handle >= hpfHandle;
            const auto in = isFilter ? (handle == hpfHandle ? s.hpf : s.lpf)
                                     : audio::EqSettings::bandIsActive (s.band[handle]);

            const auto place = placeOf (handle, s);
            const auto box = juce::Rectangle<float> (place.x - radius, place.y - radius,
                                                     2.0f * radius, 2.0f * radius);

            g.setColour (Look::colour (theme, handle == hovered || handle == dragged ? "picked"
                                              : in ? "accent" : "ink-off"));

            /*  A BAND IS ROUND AND A FILTER IS SQUARE - two shapes, §4.8 -
                and one that is out is hollow rather than a different colour. */
            if (isFilter)
            {
                if (in) g.fillRect (box); else g.drawRect (box, 1.5f);
            }
            else
            {
                if (in) g.fillEllipse (box); else g.drawEllipse (box, 1.5f);
            }

            //  The band's number beside it, so a handle is never only a dot.
            g.setColour (Look::colour (theme, "ink"));
            g.setFont (Look::font (theme, 10.0f));
            g.drawFittedText (isFilter ? juce::String (handle == hpfHandle ? "HP" : "LP")
                                       : juce::String (handle + 1),
                              juce::roundToInt (place.x) + juce::roundToInt (radius) + 2,
                              juce::roundToInt (place.y) - 7, 20, 14,
                              juce::Justification::centredLeft, 1);
        }

        juce::ignoreUnused (field);
    }
}
