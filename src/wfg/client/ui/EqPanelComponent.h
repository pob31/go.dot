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
    A media cue's EQ, drawn as a response somebody can drag (Phase 9a).

    THE PICTURE IS THE SOUND. The curve is `model::eqCurve`, which is the DSP's
    own maths, so what the hand shapes here is what the voice plays - and every
    write goes to the cue's own rows, through `node.set`, so a drag is a
    decision the show keeps and Undo takes back as one step. The rotary pages
    on the D700 will write the same addresses (namespace draft §17.8).

    FREQUENCY ACROSS, GAIN UP, and both axes say what they are in words - the
    hertz along the bottom, the decibels up the side - because §4.8's rule
    applies to a field as much as to a colour. Four handles for the bands, two
    for the filters, and the number of every one of them always drawn in the
    column beside the field: a handle's position is a colour-like fact, and a
    band read across a booth is read by its number.

    WHAT A GESTURE DOES: drag a band's handle to move its frequency and gain,
    turn the wheel over it for its width, double-click it to take the band out
    (a gain of nought is a band that is not there); drag a filter's handle for
    where it turns over; a switch for each filter and one for the EQ itself; a
    shape menu on the two bands that may be shelves; a box for every number;
    and Flat, which is one command and one undo step.

    AND THE WIDTH BY PINCHING (author, 2026-09-25: "Touch gestures on the EQ
    are not working"): two fingers on a touch screen, a trackpad's magnify, or
    a Windows touchpad's pinch, on the band being edited - the one the last
    hand took, drawn ringed, in its own colour as every handle is (spatcore's
    EQ, which the author works with). Closing the fingers narrows the band
    (`model::pinchedQ` and its neighbours say how).

    WHILE A HAND IS DOWN THE HAND IS DRAWN, and the document catches up
    underneath it - the send mixer's rule, for its reason: the round trip
    through the tick thread is a pass long, and a handle that waited for it
    would lag the pointer.
*/

#include <wfg/client/model/Eq.h>
#include <wfg/client/model/Foot.h>
#include <wfg/client/model/Theme.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace wfg::client::ui
{
    class EqPanelComponent final : public juce::Component
    {
    public:
        struct Actions
        {
            /** One `node.set` on one of the cue's EQ rows. */
            std::function<void (const std::string& address, const std::string& text)> set;

            /** `eq.reset` on the cue: every row back, one transaction. */
            std::function<void (const std::string& cueId)> reset;

            /** A sentence for the panel's head. */
            std::function<void (const juce::String&)> say;
        };

        EqPanelComponent (const model::Theme&, Actions);
        ~EqPanelComponent() override;

        void applyTheme (const model::Theme&);

        /** The reading for this pass. Cheap when the cue has not changed. */
        void show (const model::FootReading&);

        void paint (juce::Graphics&) override;
        void resized() override;

        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;

        /*  THE DRAG, as positions in this component: what the three mouse
            handlers above forward to, and what a test drives without
            inventing a mouse event (the rule tests/RunPaneUiTests.cpp keeps).
            `fine` is shift's tenth of the movement. */
        void beginDrag (juce::Point<float> at);
        void dragTo (juce::Point<float> at, bool fine);
        void endDrag();

        /** Where a handle is drawn now: bands 0 to 3, the high-pass 4, the low-pass 5. */
        juce::Point<float> handlePosition (int handle) const;

        /*  THE FINGERS, by the pointer's own index: what the three mouse
            handlers forward to, and what a test drives. One finger drags as
            above; a SECOND one down makes the two a pinch on the band nearest
            their middle, or on the one being edited, and ends the drag where
            it stands. Lifting either ends the pinch, and the finger left
            drags nothing. */
        void fingerDown (int finger, juce::Point<float> at);
        void fingerMoved (int finger, juce::Point<float> at, bool fine);
        void fingerUp (int finger);

        /*  THE HANDLE BEING EDITED, drawn ringed (author: "having a circle
            around the one being edited"): the last one a hand took, kept once
            it lets go, since the wheel and a pinch act on it. A press on the
            empty field lets it go. -1 is none. */
        int editedHandle() const noexcept { return editing; }

        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
        void mouseMagnify (const juce::MouseEvent&, float scale) override;
        void mouseMove (const juce::MouseEvent&) override;

        /** The sample rate the picture is drawn for. */
        static constexpr double pictureRate = 48000.0;

        /** The field's range, in dB: what a band may be asked for. */
        static constexpr double rangeDb = 24.0;

    private:
        /*  A HANDLE is one of the six things a hand can take hold of: the four
            bands, then the high-pass and the low-pass. */
        static constexpr int hpfHandle = 4;
        static constexpr int lpfHandle = 5;
        static constexpr int noHandle = -1;

        struct Box;

        juce::Rectangle<int> fieldArea() const;
        juce::Rectangle<int> columnArea() const;

        double xForFrequency (double frequency) const;
        double frequencyForX (double x) const;
        double yForDb (double db) const;
        double dbForY (double y) const;

        juce::Point<float> placeOf (int handle, const audio::EqSettings&) const;
        int handleAt (juce::Point<float>) const;

        /*  WHERE A HANDLE'S MARK STANDS IN THE COLUMN, at the left of its row
            (author, 2026-09-25: "Show colours in the side panel with the
            parameters"): the high-pass's and the low-pass's rows, then the
            bands'. The layout and the painting both take it from here. */
        juce::Rectangle<int> markArea (int handle) const;

        /** Whether a handle's band or filter is in: a mark is filled when it is, hollow when not. */
        static bool handleIsIn (int handle, const audio::EqSettings&) noexcept;

        /*  The band a width gesture acts on: the one under the pointer, else
            the one being edited; none over a filter, which has no width. */
        int bandFor (juce::Point<float>) const;

        /*  The band two fingers act on: the nearest to their middle within
            reach - spatcore's rule - else the one being edited. */
        int bandForPinch (juce::Point<float> middle) const;

        void beginPinch();

        /*  A TURN OF A BAND'S WIDTH, from the hand's own last Q while the
            turns keep coming: a wheel, a touchpad's pinch and a magnify all
            send faster than a tick publishes, and a turn taken from the
            published Q would be taken from the same one twice and lost. */
        double qToTurn (int band) const;
        void turnTo (int band, double q);

        /** What is drawn: the hand's copy while one is down, else the reading. */
        const audio::EqSettings& shown() const noexcept;

        void write (const std::string& row, const std::string& text);
        void writeNumber (const std::string& row, double value, int decimals);
        void writeFlag (const std::string& row, bool on);

        void rebuildControls();
        void refreshControls();

        void paintGrid (juce::Graphics&, juce::Rectangle<int> field);
        void paintCurve (juce::Graphics&, juce::Rectangle<int> field);
        void paintHandles (juce::Graphics&, juce::Rectangle<int> field);

        model::Theme theme;
        Actions actions;

        model::FootReading reading;

        /** The cue the controls were built for; a different one rebuilds them. */
        std::string builtFor;

        audio::EqSettings held;
        bool dragging = false;
        int dragged = noHandle;
        int hovered = noHandle;
        juce::Point<float> dragFrom;

        /*  WHERE THE HANDLE WAS WHEN THE HAND WENT DOWN, kept beside where the
            pointer was: a drag is the one plus the pointer's movement since
            the other, and never the published value plus it (2026-09-25). */
        juce::Point<float> handleFrom;
        bool dragFine = false;

        int editing = noHandle;

        std::map<int, juce::Point<float>> fingers;
        bool pinching = false;
        double pinchFrom = 0.0;     // the distance between the two when the second landed
        double pinchQ = 0.0;        // and the band's Q then

        int turning = noHandle;
        double turningQ = 0.0;
        juce::uint32 turnedAt = 0;

        juce::ToggleButton onToggle, hpfToggle, lpfToggle;
        juce::ComboBox lowShape, highShape;
        juce::TextButton flat;
        std::vector<std::unique_ptr<Box>> boxes;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqPanelComponent)
    };
}
