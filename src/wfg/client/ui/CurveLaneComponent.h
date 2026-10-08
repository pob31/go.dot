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
    AN OSC CUE'S CURVES, DRAWN AND EDITED (namespace draft 45, O.7): the level
    lane's editor on the cue's own time and a straight axis - one curve at a
    time, picked from a menu, the cue's others faint behind it (YT, the
    author's pick); a point dragged, added with a double click on the line,
    taken away with a double click on it, or typed in the head's two boxes;
    the playhead while the cue plays, and a click on the ruler to move it.

    AND RECORDED (O.9): the waveform editor's three words on one button - the
    cue armed, a pass started where the playhead is, the pass ended - and a
    REC for the picked curve. While a pass runs, each armed curve's ride is
    drawn as a trail over it, from what the tree says it rides each pass.

    A drag is drawn from a copy held here and written once, when the hand lets
    go - one `node.set` of the whole curve, one step of undo - as the level
    lane's is. The waveform editor is left as it was: this is its gestures, not
    its code, over `model/OscCurves`.
*/

#include <wfg/client/model/OscCurves.h>
#include <wfg/client/model/Theme.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstddef>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wfg::client::ui
{
    class CurveLaneComponent final : public juce::Component
    {
    public:
        struct Actions
        {
            /** One `node.set`: a curve's points, whole. */
            std::function<void (const std::string& address, const std::string& text)> set;

            /** The cue fired, its run stopped, its run's clock moved. */
            std::function<void (const std::string& cueId)> play;
            std::function<void (const std::string& runId)> stop;
            std::function<void (const std::string& runId, double seconds)> seek;

            /** A sentence in the panel's head. */
            std::function<void (const juce::String&)> say;

            /*  RECORDING (O.9): the cue armed, let go of, a curve armed or not,
                a pass from a second of the cue's clock, the pass ended. */
            std::function<void (const std::string& cueId)> arm;
            std::function<void()> free;
            std::function<void (const std::string& curveId, bool on)> rec;
            std::function<void (double fromSeconds)> record;
            std::function<void()> stopPass;
        };

        CurveLaneComponent (const model::Theme&, Actions);
        ~CurveLaneComponent() override;

        void applyTheme (const model::Theme&);

        /** The reading for this pass, and the cue's playhead when it plays. */
        void show (const model::OscCurvesReading&, bool running, double position, const std::string& runId);

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        //  For a test.
        juce::Point<int> pointPosition (std::size_t point) const;
        juce::Point<int> positionAt (double seconds, double value) const;
        void pickCurve (std::size_t index);
        juce::Label& pointAtBox() noexcept { return pointAt; }
        juce::Label& pointValueBox() noexcept { return pointValue; }
        std::size_t pickedCurve() const noexcept { return picked; }
        juce::Button& recordButton() noexcept { return recordAll; }
        juce::Button& curveRecButton() noexcept { return curveRec; }
        juce::Button& freeArmingButton() noexcept { return freeArming; }
        juce::ComboBox& puckAxisMenu() noexcept { return puckAxis; }
        juce::Label& puckSpeedBox() noexcept { return puckSpeed; }
        juce::Button& puckInvertButton() noexcept { return puckInvert; }
        const std::vector<model::LanePoint>* trailOf (const std::string& curveId) const;

    private:
        static constexpr std::size_t none = std::numeric_limits<std::size_t>::max();

        const model::CurveView* curve() const;
        std::vector<model::LanePoint> pointsOf (const model::CurveView&) const;
        void write (const model::CurveView&, const std::vector<model::LanePoint>&);
        void showPicked();
        void showRecording();

        juce::Rectangle<int> headArea() const;
        juce::Rectangle<int> pictureArea() const;
        juce::Rectangle<int> rulerArea() const;
        double secondsAt (int x) const;
        int xFor (double seconds) const;
        double heightAt (int y) const;
        int yFor (double height) const;

        model::Theme theme;
        Actions actions;
        model::OscCurvesReading reading;
        bool running = false;
        double position = 0.0;
        std::string runId;

        std::size_t picked = 0;
        std::string pickedId;
        std::size_t grabbed = none, pickedPoint = none;

        /*  WHAT A DRAG IS DRAWING, until the tree has it: the curve it is on,
            the points it draws, and the points before it - the reading moving
            off those is the write landing. */
        std::optional<std::vector<model::LanePoint>> held;
        std::vector<model::LanePoint> beforeHeld;
        std::string heldCurve;
        int heldPasses = 0;

        /*  WHAT A PASS HAS RIDDEN SO FAR, by curve: the clock's second and
            the ride the tree said at it, a run broken where a loop goes round.
            Begun again when a pass begins. */
        std::map<std::string, std::vector<std::vector<model::LanePoint>>> trails;
        bool wasRecording = false;

        juce::ComboBox curveMenu;
        juce::Label pointAt, pointValue;
        juce::TextButton playButton { "Play" }, stopButton { "Stop" };
        juce::TextButton recordAll, curveRec { "REC" }, freeArming;

        /*  THE PICKED CURVE'S PUCK (O.11): which axis moves it, how fast, and
            turned round or not - each one `node.set` of its row. */
        juce::ComboBox puckAxis;
        juce::Label puckSpeed;
        juce::TextButton puckInvert { "Inv" };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CurveLaneComponent)
    };
}
