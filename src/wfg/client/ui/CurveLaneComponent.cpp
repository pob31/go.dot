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

#include <wfg/client/ui/CurveLaneComponent.h>

#include <wfg/client/model/Surfaces.h>
#include <wfg/client/ui/Look.h>
#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace wfg::client::ui
{
    namespace
    {
        constexpr int grabRadius = 7;
        constexpr int axisWidth = 54;
        constexpr int rulerHeight = 16;

        bool samePoints (const std::vector<model::LanePoint>& a, const std::vector<model::LanePoint>& b)
        {
            if (a.size() != b.size())
                return false;

            for (std::size_t n = 0; n < a.size(); ++n)
                if (std::abs (a[n].seconds - b[n].seconds) > 1.0e-6 || std::abs (a[n].levelDb - b[n].levelDb) > 1.0e-6)
                    return false;

            return true;
        }

        juce::String numberText (double value)
        {
            return juce::String (osc::formatDouble (std::round (value * 1000.0) / 1000.0));
        }

        /*  THE RECORDER'S THREE WORDS, the waveform editor's (§34): at rest,
            with this cue armed, and recording. */
        const char* const recAtRest    = model::laneRecorderName;
        const char* const recReady     = "\xe2\x97\x8f Rec";
        const char* const recRecording = "\xe2\x96\xa0 Stop";

        /*  The puck's axes in the engine's words, `none` first: tx, ty, tz the
            three pushes, rx, ry, rz the three twists. */
        const char* const puckAxes[] { "none", "tx", "ty", "tz", "rx", "ry", "rz" };
    }

    CurveLaneComponent::CurveLaneComponent (const model::Theme& themeToUse, Actions actionsToUse)
        : theme (themeToUse), actions (std::move (actionsToUse))
    {
        curveMenu.setTooltip ("Which curve is drawn and edited; the cue's others are faint behind it");
        curveMenu.onChange = [this]
        {
            pickCurve (static_cast<std::size_t> (std::max (0, curveMenu.getSelectedId() - 1)));
        };
        addAndMakeVisible (curveMenu);

        /*  THE PICKED POINT, TYPED: its second and its value, as the level
            lane's head row has them. A box that will not parse is put back. */
        for (auto* box : { &pointAt, &pointValue })
        {
            box->setEditable (true, true, false);
            addAndMakeVisible (*box);
        }

        pointAt.setTooltip ("The picked point's second, on the cue's own time");
        pointValue.setTooltip ("The picked point's value");

        const auto typed = [this]
        {
            const auto* drawn = curve();

            if (drawn == nullptr || pickedPoint == none)
                return;

            const auto points = pointsOf (*drawn);

            if (pickedPoint >= points.size())
                return;

            const auto at = osc::parseDouble (pointAt.getText().toStdString());
            const auto value = osc::parseDouble (pointValue.getText().toStdString());

            if (! at.has_value() || ! value.has_value() || *at < 0.0)
            {
                showPicked();

                if (actions.say)
                    actions.say ("A point is a second and a value, both numbers");

                return;
            }

            write (*drawn, model::withCurvePoint (points, pickedPoint, *at, *value, drawn->axis));
        };

        pointAt.onTextChange = typed;
        pointValue.onTextChange = typed;

        playButton.setTooltip ("Plays the cue from its start");
        playButton.onClick = [this]
        {
            if (actions.play)
                actions.play (reading.cueId);
        };
        addAndMakeVisible (playButton);

        stopButton.setTooltip ("Stops the cue where it is");
        stopButton.onClick = [this]
        {
            if (running && actions.stop)
                actions.stop (runId);
        };
        addAndMakeVisible (stopButton);

        /*  THE RECORDER (O.9): what a click does is decided by what the tree
            says, read again at every click - at rest it arms this cue, armed it
            starts a pass where the playhead is, recording it ends the pass. */
        recordAll.setWantsKeyboardFocus (false);
        recordAll.onClick = [this]
        {
            if (reading.recording)
            {
                if (actions.stopPass)
                    actions.stopPass();
            }
            else if (reading.armedHere)
            {
                if (actions.record)
                    actions.record (running ? position : 0.0);
            }
            else if (actions.arm)
            {
                actions.arm (reading.cueId);
            }
        };
        addAndMakeVisible (recordAll);

        /*  THE PICKED CURVE'S REC: armed or not, before a pass or during one.
            On a cue nobody armed it arms the cue first. */
        curveRec.setWantsKeyboardFocus (false);
        curveRec.setTooltip ("Whether a pass writes this curve: from the first value the device reports for it,"
                             " or a hand moves it, until the pass stops");
        curveRec.onClick = [this]
        {
            const auto* drawn = curve();

            if (drawn == nullptr)
                return;

            if (! reading.armedHere && actions.arm)
                actions.arm (reading.cueId);

            if (actions.rec)
                actions.rec (drawn->id, ! (reading.armedHere && drawn->armed));
        };
        addAndMakeVisible (curveRec);

        freeArming.setButtonText (juce::String::fromUTF8 ("\xe2\x9c\x95"));
        freeArming.setTooltip ("Lets go of this cue: no curve of it is recorded");
        freeArming.setWantsKeyboardFocus (false);
        freeArming.onClick = [this]
        {
            if (actions.free)
                actions.free();
        };
        addChildComponent (freeArming);

        puckAxis.setTooltip ("Which of the SpaceMouse's axes moves this curve in a pass: tx, ty, tz its three pushes,"
                             " rx, ry, rz its three twists, or none");

        for (int at = 0; at < 7; ++at)
            puckAxis.addItem (puckAxes[at], at + 1);

        puckAxis.onChange = [this]
        {
            const auto* drawn = curve();
            const auto at = puckAxis.getSelectedId() - 1;

            if (drawn != nullptr && at >= 0 && at < 7 && drawn->puckAxis != puckAxes[at] && actions.set)
                actions.set (drawn->rowAddress ("axis"), puckAxes[at]);
        };
        addAndMakeVisible (puckAxis);

        puckSpeed.setEditable (true, true, false);
        puckSpeed.setTooltip ("How fast a full push moves this curve, in its own units a second");
        puckSpeed.onTextChange = [this]
        {
            const auto* drawn = curve();
            const auto speed = osc::parseDouble (puckSpeed.getText().toStdString());

            if (drawn == nullptr)
                return;

            if (! speed.has_value() || *speed < 0.0)
            {
                puckSpeed.setText (numberText (drawn->puckSpeed), juce::dontSendNotification);

                if (actions.say)
                    actions.say ("A speed is a number, nought or more");

                return;
            }

            if (actions.set)
                actions.set (drawn->rowAddress ("speed"), osc::formatDouble (*speed));
        };
        addAndMakeVisible (puckSpeed);

        puckInvert.setWantsKeyboardFocus (false);
        puckInvert.setTooltip ("Turns the push round: pushing away moves the curve down");
        puckInvert.onClick = [this]
        {
            const auto* drawn = curve();

            if (drawn != nullptr && actions.set)
                actions.set (drawn->rowAddress ("invert"), drawn->puckInvert ? "false" : "true");
        };
        addAndMakeVisible (puckInvert);

        showRecording();
    }

    CurveLaneComponent::~CurveLaneComponent() = default;

    void CurveLaneComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;

        for (auto* box : { &pointAt, &pointValue, &puckSpeed })
        {
            box->setFont (Look::font (theme, 12.0f));
            box->setColour (juce::Label::textColourId, Look::colour (theme, "ink"));
            box->setColour (juce::Label::backgroundColourId, Look::colour (theme, "panel-in"));
        }

        repaint();
    }

    const std::vector<model::LanePoint>* CurveLaneComponent::trailOf (const std::string& curveId) const
    {
        const auto found = trails.find (curveId);
        return found != trails.end() && ! found->second.empty() ? &found->second.back() : nullptr;
    }

    void CurveLaneComponent::showRecording()
    {
        const auto word = reading.recording ? recRecording : reading.armedHere ? recReady : recAtRest;
        recordAll.setButtonText (juce::String::fromUTF8 (word));
        recordAll.setTooltip (reading.recording
                                ? juce::String ("Ends the pass: every armed curve keeps what it rode")
                              : reading.armedHere
                                ? juce::String ("Plays the cue from the playhead and writes every armed curve from its"
                                                " first value heard or ridden")
                                : juce::String::fromUTF8 (recAtRest)
                                    + ": arm this cue, so its curves can be written from what the device reports");
        recordAll.setToggleState (reading.recording, juce::dontSendNotification);

        const auto* drawn = curve();

        recordAll.setEnabled (reading.recording || (! reading.locked && ! reading.curves.empty()));
        curveRec.setEnabled (! reading.locked && drawn != nullptr);
        curveRec.setToggleState (drawn != nullptr && reading.armedHere && drawn->armed, juce::dontSendNotification);
        freeArming.setVisible (reading.armedHere && ! reading.recording);

        //  The picked curve's puck, as the tree has it.
        const auto puckEditable = ! reading.locked && drawn != nullptr;
        auto axisAt = 0;

        for (int at = 0; at < 7; ++at)
            if (drawn != nullptr && drawn->puckAxis == puckAxes[at])
                axisAt = at;

        puckAxis.setSelectedId (axisAt + 1, juce::dontSendNotification);
        puckAxis.setEnabled (puckEditable);
        puckSpeed.setEditable (puckEditable, puckEditable, false);

        if (! puckSpeed.isBeingEdited())
            puckSpeed.setText (drawn != nullptr ? numberText (drawn->puckSpeed) : juce::String(), juce::dontSendNotification);

        puckInvert.setEnabled (puckEditable);
        puckInvert.setToggleState (drawn != nullptr && drawn->puckInvert, juce::dontSendNotification);
    }

    const model::CurveView* CurveLaneComponent::curve() const
    {
        return picked < reading.curves.size() ? &reading.curves[picked] : nullptr;
    }

    std::vector<model::LanePoint> CurveLaneComponent::pointsOf (const model::CurveView& drawn) const
    {
        return held.has_value() && heldCurve == drawn.id ? *held : drawn.points;
    }

    void CurveLaneComponent::write (const model::CurveView& drawn, const std::vector<model::LanePoint>& points)
    {
        held = points;
        beforeHeld = drawn.points;
        heldCurve = drawn.id;
        heldPasses = 0;

        if (actions.set)
            actions.set (drawn.pointsAddress(), model::writeCurve (points, model::stepFor (drawn.axis)));

        repaint();
    }

    void CurveLaneComponent::show (const model::OscCurvesReading& readingToUse, bool isRunning,
                                   double at, const std::string& run)
    {
        reading = readingToUse;
        running = isRunning;
        position = at;
        runId = run;

        /*  THE PICKED CURVE KEPT BY IDENTIFIER, so a curve added or taken away
            above it does not move the menu to another one. */
        const auto found = std::find_if (reading.curves.begin(), reading.curves.end(),
                                         [this] (const model::CurveView& view) { return view.id == pickedId; });

        picked = found != reading.curves.end() ? static_cast<std::size_t> (found - reading.curves.begin()) : 0;
        pickedId = picked < reading.curves.size() ? reading.curves[picked].id : std::string {};

        /*  A WRITE LANDED when the curve has moved off what it was before the
            drag - or after half a second regardless, so a refusal does not
            leave a drawing the show does not hold. */
        if (held.has_value())
        {
            const auto landed = std::find_if (reading.curves.begin(), reading.curves.end(),
                                              [this] (const model::CurveView& view) { return view.id == heldCurve; });

            if (landed == reading.curves.end() || ! samePoints (landed->points, beforeHeld) || ++heldPasses > 25)
                held.reset();
        }

        juce::StringArray labels;

        for (const auto& view : reading.curves)
            labels.add (juce::String (view.label));

        juce::StringArray shown;

        for (int n = 0; n < curveMenu.getNumItems(); ++n)
            shown.add (curveMenu.getItemText (n));

        if (labels != shown)
        {
            curveMenu.clear (juce::dontSendNotification);

            for (int n = 0; n < labels.size(); ++n)
                curveMenu.addItem (labels[n], n + 1);
        }

        curveMenu.setSelectedId (static_cast<int> (picked) + 1, juce::dontSendNotification);

        /*  THE TRAILS: begun again when a pass begins, a sample a pass while it
            runs - broken where the clock goes back, a loop's wrap. */
        if (reading.recording && ! wasRecording)
            trails.clear();

        wasRecording = reading.recording;

        if (reading.recording && running)
            for (const auto& view : reading.curves)
            {
                if (! view.armed || ! view.ride.has_value())
                    continue;

                auto& runsOf = trails[view.id];

                if (runsOf.empty() || (! runsOf.back().empty() && position < runsOf.back().back().seconds - 1.0e-6))
                    runsOf.emplace_back();

                auto& trail = runsOf.back();

                if (! trail.empty() && std::abs (trail.back().seconds - position) < 1.0e-6)
                    trail.back().levelDb = *view.ride;
                else
                    trail.push_back ({ position, *view.ride });
            }

        showRecording();

        const auto editable = ! reading.locked && curve() != nullptr;
        pointAt.setEditable (editable, editable, false);
        pointValue.setEditable (editable, editable, false);
        stopButton.setEnabled (running);

        if (curve() == nullptr || pickedPoint >= pointsOf (*curve()).size())
            pickedPoint = none;

        showPicked();
        repaint();
    }

    void CurveLaneComponent::pickCurve (std::size_t index)
    {
        if (index >= reading.curves.size())
            return;

        picked = index;
        pickedId = reading.curves[index].id;
        pickedPoint = none;
        curveMenu.setSelectedId (static_cast<int> (index) + 1, juce::dontSendNotification);
        showPicked();
        repaint();
    }

    void CurveLaneComponent::showPicked()
    {
        const auto* drawn = curve();

        if (drawn == nullptr || pickedPoint == none)
        {
            if (! pointAt.isBeingEdited())
                pointAt.setText ({}, juce::dontSendNotification);

            if (! pointValue.isBeingEdited())
                pointValue.setText ({}, juce::dontSendNotification);

            return;
        }

        const auto points = pointsOf (*drawn);

        if (pickedPoint >= points.size())
            return;

        if (! pointAt.isBeingEdited())
            pointAt.setText (numberText (points[pickedPoint].seconds), juce::dontSendNotification);

        if (! pointValue.isBeingEdited())
            pointValue.setText (numberText (points[pickedPoint].levelDb), juce::dontSendNotification);
    }

    //==============================================================================
    juce::Rectangle<int> CurveLaneComponent::headArea() const
    {
        return getLocalBounds().removeFromTop (juce::roundToInt (theme.row * theme.type));
    }

    juce::Rectangle<int> CurveLaneComponent::pictureArea() const
    {
        auto area = getLocalBounds();
        area.removeFromTop (juce::roundToInt (theme.row * theme.type));
        area.removeFromBottom (rulerHeight);
        area.removeFromLeft (axisWidth);
        return area.reduced (4, 4);
    }

    juce::Rectangle<int> CurveLaneComponent::rulerArea() const
    {
        auto area = getLocalBounds();
        auto ruler = area.removeFromBottom (rulerHeight);
        ruler.removeFromLeft (axisWidth);
        return ruler.reduced (4, 0);
    }

    double CurveLaneComponent::secondsAt (int x) const
    {
        const auto area = pictureArea();
        const auto share = area.getWidth() > 0 ? static_cast<double> (x - area.getX()) / area.getWidth() : 0.0;
        return std::max (0.0, share * reading.drawn);
    }

    int CurveLaneComponent::xFor (double seconds) const
    {
        const auto area = pictureArea();
        return area.getX() + juce::roundToInt (seconds / std::max (reading.drawn, 1.0e-6) * area.getWidth());
    }

    double CurveLaneComponent::heightAt (int y) const
    {
        const auto area = pictureArea();
        return area.getHeight() > 0 ? static_cast<double> (area.getBottom() - y) / area.getHeight() : 0.0;
    }

    int CurveLaneComponent::yFor (double height) const
    {
        const auto area = pictureArea();
        return area.getBottom() - juce::roundToInt (height * area.getHeight());
    }

    juce::Point<int> CurveLaneComponent::positionAt (double seconds, double value) const
    {
        const auto* drawn = curve();
        return { xFor (seconds), yFor (drawn != nullptr ? model::heightOnAxis (value, drawn->axis) : 0.5) };
    }

    juce::Point<int> CurveLaneComponent::pointPosition (std::size_t point) const
    {
        const auto* drawn = curve();

        if (drawn == nullptr)
            return {};

        const auto points = pointsOf (*drawn);
        return point < points.size() ? positionAt (points[point].seconds, points[point].levelDb) : juce::Point<int> {};
    }

    //==============================================================================
    void CurveLaneComponent::resized()
    {
        auto head = headArea().reduced (2, 3);

        recordAll.setBounds (head.removeFromRight (70));
        head.removeFromRight (2);
        freeArming.setBounds (head.removeFromRight (24));
        head.removeFromRight (6);
        stopButton.setBounds (head.removeFromRight (54));
        head.removeFromRight (4);
        playButton.setBounds (head.removeFromRight (54));
        head.removeFromRight (8);

        curveMenu.setBounds (head.removeFromLeft (150));
        head.removeFromLeft (4);
        curveRec.setBounds (head.removeFromLeft (40));
        head.removeFromLeft (8);
        pointAt.setBounds (head.removeFromLeft (70));
        head.removeFromLeft (4);
        pointValue.setBounds (head.removeFromLeft (90));
        head.removeFromLeft (12);
        puckAxis.setBounds (head.removeFromLeft (70));
        head.removeFromLeft (4);
        puckSpeed.setBounds (head.removeFromLeft (52));
        head.removeFromLeft (4);
        puckInvert.setBounds (head.removeFromLeft (40));
    }

    void CurveLaneComponent::paint (juce::Graphics& g)
    {
        const auto picture = pictureArea();

        g.setColour (Look::colour (theme, "panel-in"));
        g.fillRect (picture.expanded (4, 4));

        const auto* drawn = curve();

        if (drawn == nullptr)
        {
            g.setColour (Look::colour (theme, "ink-dim"));
            g.setFont (Look::font (theme, 12.0f));
            g.drawText ("No curve yet: put one on a value with ~ in the table", picture,
                        juce::Justification::centred, true);
            return;
        }

        //  The axis: its two ends, in the value's own units.
        g.setColour (Look::colour (theme, "ink-faint"));
        g.setFont (Look::font (theme, 11.0f));

        const auto axisColumn = juce::Rectangle<int> (0, picture.getY() - 6, axisWidth - 6, picture.getHeight() + 12);
        g.drawText (numberText (drawn->axis.high), axisColumn.withHeight (12), juce::Justification::centredRight, false);
        g.drawText (numberText (drawn->axis.low), axisColumn.withTrimmedTop (axisColumn.getHeight() - 12),
                    juce::Justification::centredRight, false);

        //  The ruler, a figure a second - or a tenth, or ten, as the length asks.
        const auto ruler = rulerArea();
        const auto every = reading.drawn > 60.0 ? 10.0 : reading.drawn > 6.0 ? 1.0 : 0.1;

        for (auto at = 0.0; at <= reading.drawn + 1.0e-9; at += every)
        {
            const auto x = xFor (at);
            g.setColour (Look::colour (theme, "rule"));
            g.drawVerticalLine (x, static_cast<float> (picture.getY()), static_cast<float> (picture.getBottom()));

            if (every >= 1.0 || std::fmod (std::round (at * 10.0), 5.0) < 0.5)
            {
                g.setColour (Look::colour (theme, "ink-faint"));
                g.drawText (numberText (at), juce::Rectangle<int> (x - 20, ruler.getY(), 40, ruler.getHeight()),
                            juce::Justification::centred, false);
            }
        }

        //  Where the cue ends, when that is before the edge of what is drawn.
        if (reading.duration > 0.0 && reading.duration < reading.drawn)
        {
            g.setColour (Look::colour (theme, "ink-dim"));
            g.drawVerticalLine (xFor (reading.duration), static_cast<float> (picture.getY()),
                                static_cast<float> (picture.getBottom()));
        }

        //  A curve as a path: straight between points, held beyond them.
        const auto pathOf = [this] (const std::vector<model::LanePoint>& points, const model::CurveAxis& axis,
                                    double written)
        {
            juce::Path path;
            const auto start = points.empty() ? written : points.front().levelDb;
            path.startNewSubPath (static_cast<float> (xFor (0.0)), static_cast<float> (yFor (model::heightOnAxis (start, axis))));

            for (const auto& point : points)
                path.lineTo (static_cast<float> (xFor (point.seconds)),
                             static_cast<float> (yFor (model::heightOnAxis (point.levelDb, axis))));

            const auto end = points.empty() ? written : points.back().levelDb;
            path.lineTo (static_cast<float> (xFor (reading.drawn)), static_cast<float> (yFor (model::heightOnAxis (end, axis))));
            return path;
        };

        //  THE CUE'S OTHER CURVES, faint behind, each on its own axis (YT).
        for (std::size_t n = 0; n < reading.curves.size(); ++n)
        {
            if (n == picked)
                continue;

            const auto& other = reading.curves[n];
            g.setColour (Look::colour (theme, "ink-faint").withAlpha (0.5f));
            g.strokePath (pathOf (other.points, other.axis, other.written), juce::PathStrokeType (1.0f));
        }

        const auto points = pointsOf (*drawn);

        g.setColour (Look::colour (theme, "ink"));
        g.strokePath (pathOf (points, drawn->axis, drawn->written), juce::PathStrokeType (1.6f));

        //  Its points, the picked one filled and the others hollow (4.8: by shape, not colour alone).
        for (std::size_t n = 0; n < points.size(); ++n)
        {
            const auto at = positionAt (points[n].seconds, points[n].levelDb).toFloat();
            const auto dot = juce::Rectangle<float> (8.0f, 8.0f).withCentre (at);

            if (n == pickedPoint)
                g.fillEllipse (dot);
            else
                g.drawEllipse (dot.reduced (0.5f), 1.4f);
        }

        /*  WHAT THE PASS HAS RIDDEN on the picked curve, over it, as the level
            lane's trail is drawn: wider than the curve and outlined, so it
            reads apart from it without colour alone (4.8). */
        if (const auto found = trails.find (drawn->id); found != trails.end())
        {
            juce::Path line;

            for (const auto& trail : found->second)
                for (std::size_t n = 0; n < trail.size(); ++n)
                {
                    const auto at = positionAt (trail[n].seconds, trail[n].levelDb).toFloat();

                    if (n == 0)
                        line.startNewSubPath (at);
                    else
                        line.lineTo (at);
                }

            g.saveState();
            g.reduceClipRegion (picture.expanded (4, 4));
            g.setColour (juce::Colours::black.withAlpha (0.8f));
            g.strokePath (line, juce::PathStrokeType (3.5f));
            g.setColour (Look::colour (theme, "failed"));
            g.strokePath (line, juce::PathStrokeType (2.0f));
            g.restoreState();
        }

        //  The playhead, while the cue plays.
        if (running)
        {
            g.setColour (Look::colour (theme, "go"));
            g.drawVerticalLine (xFor (position), static_cast<float> (picture.getY() - 4),
                                static_cast<float> (picture.getBottom() + 4));
        }
    }

    //==============================================================================
    void CurveLaneComponent::mouseDown (const juce::MouseEvent& event)
    {
        const auto* drawn = curve();

        if (drawn == nullptr)
            return;

        /*  ON THE RULER, WHILE IT PLAYS: the clock moved there. */
        if (rulerArea().contains (event.getPosition()))
        {
            if (running && actions.seek)
                actions.seek (runId, secondsAt (event.x));

            return;
        }

        if (! pictureArea().expanded (grabRadius).contains (event.getPosition()))
            return;

        const auto points = pointsOf (*drawn);
        const auto area = pictureArea();
        const auto secondsTolerance = grabRadius * reading.drawn / std::max (1, area.getWidth());
        const auto heightTolerance = static_cast<double> (grabRadius) / std::max (1, area.getHeight());

        pickedPoint = model::nearestCurvePoint (points, secondsAt (event.x), heightAt (event.y), drawn->axis,
                                                secondsTolerance, heightTolerance);
        grabbed = reading.locked ? none : pickedPoint;

        if (pickedPoint != none && grabbed != none)
        {
            held = points;
            beforeHeld = drawn->points;
            heldCurve = drawn->id;
            heldPasses = 0;
        }

        showPicked();
        repaint();
    }

    void CurveLaneComponent::mouseDrag (const juce::MouseEvent& event)
    {
        const auto* drawn = curve();

        if (drawn == nullptr || grabbed == none || ! held.has_value())
            return;

        held = model::withCurvePoint (*held, grabbed, secondsAt (event.x),
                                      model::valueOnAxis (heightAt (event.y), drawn->axis), drawn->axis);
        showPicked();
        repaint();
    }

    void CurveLaneComponent::mouseUp (const juce::MouseEvent&)
    {
        const auto* drawn = curve();

        /*  ONE WRITE A GESTURE, when the hand lets go - and none for a press
            that moved nothing. */
        if (drawn != nullptr && grabbed != none && held.has_value() && ! samePoints (*held, drawn->points))
            write (*drawn, *held);
        else if (held.has_value() && samePoints (*held, beforeHeld))
            held.reset();

        grabbed = none;
    }

    void CurveLaneComponent::mouseDoubleClick (const juce::MouseEvent& event)
    {
        const auto* drawn = curve();

        if (drawn == nullptr || reading.locked || ! pictureArea().expanded (grabRadius).contains (event.getPosition()))
            return;

        const auto points = drawn->points;
        const auto area = pictureArea();
        const auto secondsTolerance = grabRadius * reading.drawn / std::max (1, area.getWidth());
        const auto heightTolerance = static_cast<double> (grabRadius) / std::max (1, area.getHeight());
        const auto onPoint = model::nearestCurvePoint (points, secondsAt (event.x), heightAt (event.y), drawn->axis,
                                                       secondsTolerance, heightTolerance);

        /*  ON A POINT: taken away. ELSEWHERE: one more, on the line the curve
            already draws there, so adding it changes nothing until it moves. */
        if (onPoint != none)
        {
            pickedPoint = none;
            write (*drawn, model::removeLanePoint (points, onPoint));
            return;
        }

        if (const auto added = model::insertCurvePoint (points, secondsAt (event.x), drawn->written))
        {
            write (*drawn, *added);

            for (std::size_t n = 0; n < added->size(); ++n)
                if (std::abs ((*added)[n].seconds - secondsAt (event.x)) < 1.0e-9)
                    pickedPoint = n;

            showPicked();
        }
    }
}
