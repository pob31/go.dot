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
#include <array>
#include <cmath>
#include <iterator>
#include <string>

namespace wfg::client::ui
{
    namespace
    {
        int scaled (int base, const model::Theme& theme)
        {
            return juce::roundToInt (static_cast<float> (base) * theme.type);
        }

        /*  A COLOUR EACH (author, 2026-09-25: "Having different colours on
            each handle like on the EQ of the spatcore library really helps"):
            spatcore's first six, in the order the handles stand along the
            field by default - the high-pass red, the four bands orange to
            blue, the low-pass purple - as the author's six-band EQs there run
            from a low cut in red to a high cut in purple. Tokens, by handle
            index. The colour is the fast half: the shape (a band round, a
            filter square), the fill (in or out) and the name beside each
            still say it all (§4.8). */
        constexpr std::array<const char*, 6> handleTokens { { "eq-1", "eq-2", "eq-3", "eq-4",
                                                              "eq-hp", "eq-lp" } };

        /*  How long a turn of the wheel waits for the next before the
            published Q is believed again. */
        constexpr juce::uint32 turnPatienceMs = 400;

        //  The width a handle's mark takes at the left of its row in the column.
        constexpr int markWidth = 16;

        /*  A HANDLE'S SHAPE, on the field and in the column alike: a band
            round and a filter square, hollow when it is out (§4.8). */
        void drawHandleShape (juce::Graphics& g, juce::Rectangle<float> box, bool filter, bool in)
        {
            if (filter)
            {
                if (in) g.fillRect (box); else g.drawRect (box, 1.5f);
            }
            else
            {
                if (in) g.fillEllipse (box); else g.drawEllipse (box, 1.5f);
            }
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

        for (int band = 0; band < audio::EqSettings::numBands; ++band)
        {
            auto& toggle = bandToggles[static_cast<std::size_t> (band)];
            toggle.setWantsKeyboardFocus (false);
            toggle.setTooltip ("Whether band " + juce::String (band + 1) + " is in - off keeps its numbers");
            toggle.onClick = [this, band]
            {
                writeFlag (model::eqBandRow (band, "On"),
                           bandToggles[static_cast<std::size_t> (band)].getToggleState());
            };
        }

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

    void EqPanelComponent::setEditedHandle (int handle)
    {
        if (handle == editing)
            return;

        editing = handle;
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

        for (auto& toggle : bandToggles)
            addAndMakeVisible (toggle);

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

        for (int band = 0; band < audio::EqSettings::numBands; ++band)
            bandToggles[static_cast<std::size_t> (band)].setToggleState (s.band[band].on,
                                                                          juce::dontSendNotification);

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
            bounded = std::clamp (value, model::eqQLowest, model::eqQHighest);

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
        /*  A FINGER THAT NEVER SAID IT LIFTED - the panel hidden under it - is
            forgotten when this is the only one down, or the next single press
            would be read as the second finger of a pinch. */
        if (juce::Desktop::getInstance().getNumDraggingMouseSources() <= 1)
        {
            fingers.clear();
            pinching = false;
        }

        fingerDown (event.source.getIndex(), event.position);
    }

    void EqPanelComponent::mouseDrag (const juce::MouseEvent& event)
    {
        fingerMoved (event.source.getIndex(), event.position, event.mods.isShiftDown());
    }

    void EqPanelComponent::fingerDown (int finger, juce::Point<float> at)
    {
        fingers[finger] = at;

        if (fingers.size() == 2)
            beginPinch();
        else if (fingers.size() == 1)
            beginDrag (at);

        //  A third finger changes nothing.
    }

    void EqPanelComponent::fingerMoved (int finger, juce::Point<float> at, bool fine)
    {
        if (const auto found = fingers.find (finger); found != fingers.end())
            found->second = at;

        if (! pinching)
        {
            dragTo (at, fine);
            return;
        }

        if (fingers.size() < 2 || editing == noHandle)
            return;

        const auto first = fingers.begin()->second;
        const auto second = std::next (fingers.begin())->second;

        turnTo (editing, model::pinchedQ (pinchQ, pinchFrom,
                                          static_cast<double> (first.getDistanceFrom (second))));
    }

    void EqPanelComponent::fingerUp (int finger)
    {
        fingers.erase (finger);

        if (fingers.size() < 2)
            pinching = false;

        if (fingers.empty())
            endDrag();
    }

    void EqPanelComponent::beginPinch()
    {
        const auto first = fingers.begin()->second;
        const auto second = std::next (fingers.begin())->second;
        const auto band = bandForPinch ((first + second) * 0.5f);

        //  The drag the first finger began ends where it stands: its writes stay.
        dragging = false;
        dragged = noHandle;
        pinching = band != noHandle;

        if (pinching)
        {
            editing = band;
            pinchFrom = static_cast<double> (first.getDistanceFrom (second));
            pinchQ = qToTurn (band);
        }

        refreshControls();
        repaint();
    }

    int EqPanelComponent::bandFor (juce::Point<float> at) const
    {
        const auto under = handleAt (at);

        if (under != noHandle)
            return under < audio::EqSettings::numBands ? under : noHandle;

        return editing >= 0 && editing < audio::EqSettings::numBands ? editing : noHandle;
    }

    int EqPanelComponent::bandForPinch (juce::Point<float> middle) const
    {
        const auto reach = static_cast<float> (scaled (150, theme));
        const auto& s = shown();
        auto best = noHandle;
        auto nearest = reach * reach;

        for (int band = 0; band < audio::EqSettings::numBands; ++band)
        {
            const auto distance = placeOf (band, s).getDistanceSquaredFrom (middle);

            if (distance <= nearest)
            {
                nearest = distance;
                best = band;
            }
        }

        if (best != noHandle)
            return best;

        return editing >= 0 && editing < audio::EqSettings::numBands ? editing : noHandle;
    }

    double EqPanelComponent::qToTurn (int band) const
    {
        const auto continuing = band == turning
                             && juce::Time::getMillisecondCounter() - turnedAt < turnPatienceMs;

        return continuing ? turningQ : static_cast<double> (shown().band[band].q);
    }

    void EqPanelComponent::turnTo (int band, double q)
    {
        turning = band;
        turningQ = q;
        turnedAt = juce::Time::getMillisecondCounter();
        editing = band;

        writeNumber (model::eqBandRow (band, "Q"), q, 2);
        refreshControls();
        repaint();
    }

    void EqPanelComponent::beginDrag (juce::Point<float> at)
    {
        dragged = handleAt (at);

        //  The ring goes to the handle taken, and from the field when the press found none.
        editing = dragged;
        repaint();

        if (dragged == noHandle)
            return;

        held = reading.eq.settings;
        dragging = true;
        dragFrom = at;
        handleFrom = placeOf (dragged, held);
        dragFine = false;
    }

    juce::Point<float> EqPanelComponent::handlePosition (int handle) const
    {
        return placeOf (handle, shown());
    }

    void EqPanelComponent::dragTo (juce::Point<float> at, bool fine)
    {
        if (! dragging || dragged == noHandle)
            return;

        /*  MEASURED FROM WHERE THE HAND WENT DOWN - the pointer AND the handle,
            both kept from the press. It used to take the handle from the
            reading, which this drag's own writes move on every pass: the whole
            movement so far was added again twenty-five times a second and the
            point ran away from the hand (author, 2026-09-25: "The Eq points
            move in very large increments when using the mouse on the graph.
            Same with touch."). The send mixer's faders always did it this way.

            Shift divides the movement by ten, the fine drag every other drag
            in this window has; pressing or letting go of it mid-drag starts
            again from where the handle is, so the change of speed is not a
            jump. */
        const auto scale = fine ? 0.1f : 1.0f;

        if (fine != dragFine)
        {
            handleFrom = placeOf (dragged, held);
            dragFrom = at;
            dragFine = fine;
        }

        const auto target = handleFrom + (at - dragFrom) * scale;

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

    void EqPanelComponent::mouseUp (const juce::MouseEvent& event)
    {
        fingerUp (event.source.getIndex());
    }

    void EqPanelComponent::endDrag()
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
        /*  THE WIDTH, on the wheel over a band or with one being edited, on a
            log scale, because Q is read that way. CTRL HELD IS A PINCH: a
            Windows touchpad sends its pinch as the wheel with ctrl, and the
            fingers closing must narrow the band (author, 2026-09-25: "Pinch
            widens and this feels reversed") - `model::turnedQ` turns the
            sense round for it. */
        const auto band = bandFor (event.position);

        if (band == noHandle || juce::exactlyEqual (wheel.deltaY, 0.0f))
            return;

        turnTo (band, model::turnedQ (qToTurn (band), static_cast<double> (wheel.deltaY),
                                      event.mods.isCtrlDown(), event.mods.isShiftDown()));
    }

    void EqPanelComponent::mouseMagnify (const juce::MouseEvent& event, float scale)
    {
        //  A trackpad's own pinch: fingers spreading are a scale above one, and widen the band.
        const auto band = bandFor (event.position);

        if (band != noHandle)
            turnTo (band, model::magnifiedQ (qToTurn (band), static_cast<double> (scale)));
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

        //  Each handle's row gives its mark the left of it (`markArea`).
        const auto mark = scaled (markWidth, theme);

        line = column.removeFromTop (row);
        line.removeFromLeft (mark);
        hpfToggle.setBounds (line.removeFromLeft (line.getWidth() - numberWidth));
        if (boxes.size() > 0) boxes[0]->value.setBounds (line.reduced (gapPx, 1));
        column.removeFromTop (gapPx);

        line = column.removeFromTop (row);
        line.removeFromLeft (mark);
        lpfToggle.setBounds (line.removeFromLeft (line.getWidth() - numberWidth));
        if (boxes.size() > 1) boxes[1]->value.setBounds (line.reduced (gapPx, 1));
        column.removeFromTop (gapPx);

        for (int band = 0; band < audio::EqSettings::numBands; ++band)
        {
            line = column.removeFromTop (row);
            line.removeFromLeft (mark);
            auto head = line.removeFromLeft (line.getWidth() - 3 * numberWidth);

            //  The band's switch first, then its shape or its name.
            bandToggles[static_cast<std::size_t> (band)].setBounds (head.removeFromLeft (row));

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

        /*  RIDING LIVE ON A LOCKED SHOW (2026-09-25): heard, and not saved
            until the window's bar keeps it - said in words over the field. */
        if (! reading.eq.live.empty())
        {
            g.setColour (Look::colour (theme, "ink-dim"));
            g.setFont (Look::font (theme, 12.0f));
            g.drawText ("live, not saved", field.reduced (scaled (6, theme), scaled (4, theme)),
                        juce::Justification::topRight, true);
        }

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
            line.removeFromLeft (scaled (markWidth, theme));
            auto head = line.removeFromLeft (line.getWidth() - 3 * scaled (62, theme));
            head.removeFromLeft (row);    // the band's switch

            if (band == 1 || band == 2)
                g.drawFittedText ("Band " + juce::String (band + 1) + ", peak",
                                  head.reduced (gapPx, 0), juce::Justification::centredLeft, 1);

            column.removeFromTop (gapPx);
        }

        /*  AND EACH HANDLE'S MARK BESIDE ITS ROW, as it is drawn on the field
            - its colour, its shape, filled or hollow, and ringed while it is
            the one being edited - so the row and the handle find each other. */
        const auto& s = shown();
        const auto radius = static_cast<float> (scaled (5, theme));

        for (int handle = 0; handle < 6; ++handle)
        {
            const auto centre = markArea (handle).toFloat().getCentre();
            const auto box = juce::Rectangle<float> (2.0f * radius, 2.0f * radius).withCentre (centre);

            g.setColour (Look::colour (theme, handleTokens[static_cast<std::size_t> (handle)]));
            drawHandleShape (g, box, handle >= hpfHandle, handleIsIn (handle, s));

            if (handle == editing)
            {
                g.setColour (Look::colour (theme, "ink"));
                g.drawEllipse (box.expanded (static_cast<float> (scaled (3, theme))),
                               static_cast<float> (scaled (1, theme)));
            }
        }
    }

    juce::Rectangle<int> EqPanelComponent::markArea (int handle) const
    {
        auto column = columnArea();
        const auto row = scaled (22, theme);
        const auto gapPx = scaled (3, theme);

        //  The EQ's own row first, then the high-pass, the low-pass and the four bands.
        const auto index = handle == hpfHandle ? 1 : handle == lpfHandle ? 2 : 3 + handle;
        column.removeFromTop (index * (row + gapPx));

        return column.removeFromTop (row).removeFromLeft (scaled (markWidth, theme));
    }

    bool EqPanelComponent::handleIsIn (int handle, const audio::EqSettings& s) noexcept
    {
        if (handle == hpfHandle)
            return s.hpf;

        if (handle == lpfHandle)
            return s.lpf;

        return audio::EqSettings::bandIsActive (s.band[std::clamp (handle, 0, audio::EqSettings::numBands - 1)]);
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
            const auto in = handleIsIn (handle, s);

            const auto place = placeOf (handle, s);
            const auto box = juce::Rectangle<float> (place.x - radius, place.y - radius,
                                                     2.0f * radius, 2.0f * radius);

            /*  ITS OWN COLOUR, in or out, brighter under the pointer or the
                hand (`handleTokens`). */
            auto colour = Look::colour (theme, handleTokens[static_cast<std::size_t> (handle)]);

            if (handle == hovered || handle == dragged)
                colour = colour.brighter (0.4f);

            g.setColour (colour);

            /*  A BAND IS ROUND AND A FILTER IS SQUARE - two shapes, §4.8 -
                and one that is out is hollow rather than a different colour. */
            drawHandleShape (g, box, isFilter, in);

            /*  THE ONE BEING EDITED, RINGED in the ink (author, 2026-09-25:
                "And having a circle around the one being edited too"): what
                the wheel and a pinch will narrow or widen. */
            const auto ring = radius + static_cast<float> (scaled (4, theme));
            const auto ringed = handle == editing;

            if (ringed)
            {
                g.setColour (Look::colour (theme, "ink"));
                g.drawEllipse (place.x - ring, place.y - ring, 2.0f * ring, 2.0f * ring,
                               static_cast<float> (scaled (2, theme)));
            }

            //  The band's number beside it - outside the ring - so a handle is never only a dot.
            g.setColour (Look::colour (theme, "ink"));
            g.setFont (Look::font (theme, 10.0f));
            g.drawFittedText (isFilter ? juce::String (handle == hpfHandle ? "HP" : "LP")
                                       : juce::String (handle + 1),
                              juce::roundToInt (place.x + (ringed ? ring + 1.0f : radius)) + 2,
                              juce::roundToInt (place.y) - 7, 20, 14,
                              juce::Justification::centredLeft, 1);
        }

        juce::ignoreUnused (field);
    }
}
