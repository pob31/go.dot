/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#include <wfg/client/ui/WaveformEditorComponent.h>

#include <wfg/client/model/Fader.h>
#include <wfg/client/model/Text.h>
#include <wfg/client/ui/Look.h>
#include <wfg/engine/audio/MediaInfo.h>
#include <wfg/engine/audio/Timbre.h>
#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace wfg::client::ui
{
    namespace
    {
        /*  How near the pointer has to be to an edge to take hold of it, in
            pixels. Generous, because an in-point is one pixel wide and a hand
            on a trackpad is not. */
        constexpr int grabRadius = 7;

        /*  THE LANE RECORDER'S THREE WORDS: at rest, with the faders flipped to
            this cue, and recording. QY's four were the author's (2026-10-05);
            the flip took the touch away (2026-10-06, §34), and these are the
            implementer's, which the author kept when asked. The first is the
            model's, so the sentences that report a pass open with the same
            name. */
        const char* const recAtRest    = model::laneRecorderName;
        const char* const recReady     = "\xe2\x97\x8f Rec";
        const char* const recRecording = "\xe2\x96\xa0 Stop";

        juce::String said (const char* utf8)
        {
            return juce::String::fromUTF8 (utf8);
        }

        /*  What the head row says of a media cue's speed: nothing when it plays
            at one in varispeed, its speed and its mode otherwise. */
        juce::String speedWords (const model::FootReading& reading)
        {
            if (reading.cueKind != "media")
                return {};

            const auto speed = model::speedText (reading.rate);
            const auto stretched = reading.rateMode == "timestretch";

            if (speed.empty() && ! stretched)
                return {};

            return juce::String (juce::CharPointer_UTF8 ((speed.empty() ? std::string ("\xc3\x97" "1") : speed).c_str()))
                     + (stretched ? " timestretch" : " varispeed");
        }

        /** A time, as a ruler writes it: 1.5 s, or 1:04.2 once there are minutes. */
        juce::String clockText (double seconds)
        {
            const auto whole = static_cast<int> (std::floor (std::abs (seconds)));
            const auto tenths = static_cast<int> (std::floor ((std::abs (seconds) - whole) * 10.0));
            const auto sign = seconds < 0.0 ? "-" : "";

            if (whole < 60)
                return juce::String (sign) + juce::String (whole) + "." + juce::String (tenths);

            return juce::String (sign) + juce::String (whole / 60) + ":"
                     + juce::String (whole % 60).paddedLeft ('0', 2) + "."
                     + juce::String (tenths);
        }

        /*  A step that gives a readable number of marks across the window, out
            of the divisions a person actually counts in: tenths, halves,
            seconds, then the usual clock steps. */
        double rulerStep (double span)
        {
            static const double steps[] { 0.01, 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0,
                                          10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0 };

            for (const auto step : steps)
                if (span / step <= 12.0)
                    return step;

            return 900.0;
        }
    }

    WaveformEditorComponent::WaveformEditorComponent (const model::Theme& themeToUse,
                                                      Actions actionsToUse)
        : theme (themeToUse), actions (std::move (actionsToUse))
    {
        setWantsKeyboardFocus (false);

        RangeTableComponent::Actions typed;
        typed.set = actions.set;
        typed.say = actions.say;
        typed.createRange = actions.createRange;
        typed.removeRange = actions.removeRange;
        typed.splitRange = actions.splitRange;

        table = std::make_unique<RangeTableComponent> (theme, std::move (typed));
        addAndMakeVisible (*table);

        /*  PLAY AND PAUSE, so a region can be HEARD where it is being placed
            (author, 2026-09-21: *"we need to play/pause the clip for
            testing"*). It fires the cue itself rather than auditioning the
            file on the side: what somebody is checking is what the show will
            do, and a preview through a different path would answer a question
            nobody asked.

            PAUSE IS A STOP THAT REMEMBERS WHERE IT WAS. The engine has no
            per-run pause - `audio.connection` pauses the whole graph for a
            lost interface and that is a different thing - so this kills the
            run and keeps its position as the point, and play starts again
            from there. The difference from a true pause is audible only in
            the tail, which a media cue being auditioned does not have. */
        transport.setWantsKeyboardFocus (false);
        transport.onClick = [this]
        {
            /*  A PASS IS ENDED, NOT KILLED: a kill drops the ride (§20.9, DM),
                and the transport's stop is a hand that has heard enough. */
            if (laneIsMine() && reading.laneRecord.recording)
            {
                if (actions.laneStop)
                    actions.laneStop();

                return;
            }

            if (reading.running && ! reading.runId.empty())
            {
                point = reading.position;
                askedToPlay = false;

                if (actions.stop)
                    actions.stop (reading.runId);

                return;
            }

            if (reading.cueKind != "media" || actions.play == nullptr)
                return;

            askedToPlay = true;
            playingRun = reading.runId;
            actions.play (reading.subject.objectId);
        };

        addAndMakeVisible (transport);
        sayWhichWayTheTransportGoes();

        /*  THE LANES' REC (§34): what a click does is decided by what the tree
            says the faders are doing, read again at every click - never by
            what the button last showed. At rest it flips the faders to this
            cue; flipped, it starts a pass; recording, it ends it. */
        rec.setWantsKeyboardFocus (false);
        rec.onClick = [this]
        {
            const auto& lane = reading.laneRecord;
            const auto cue = reading.subject.objectId;

            if (laneIsMine() && lane.recording)
            {
                if (actions.laneStop)
                    actions.laneStop();
            }
            else if (laneIsMine())
            {
                if (actions.laneRecord)
                    actions.laneRecord (headSeconds());
            }
            else if (actions.laneArm)
            {
                actions.laneArm (cue);
            }
        };

        addAndMakeVisible (rec);

        freeFader.setButtonText (juce::String::fromUTF8 ("\xe2\x9c\x95"));
        freeFader.setTooltip ("Flip the faders back to what they rode");
        freeFader.setWantsKeyboardFocus (false);
        freeFader.onClick = [this]
        {
            if (actions.laneFree)
                actions.laneFree();
        };

        addChildComponent (freeFader);
        sayWhatRecDoes();

        lanePick.setWantsKeyboardFocus (false);
        lanePick.onClick = [this] { pickLane(); };
        addChildComponent (lanePick);

        /*  THE PICKED POINT, TYPED. A number that will not parse is put back
            to what the lane says rather than written as nought - a slip of the
            keyboard must not move a level - and a number that parses is held
            between the point's neighbours as a drag would be. */
        const auto box = [this] (juce::Label& label, const char* tip)
        {
            label.setEditable (true, true, false);
            label.setJustificationType (juce::Justification::centredRight);
            label.setTooltip (tip);
            addChildComponent (label);
        };

        box (pointAt, "Where this level point is in the file, in seconds or minutes:seconds");
        box (pointLevel, "The level at this point, in dB: nought is the cue as written");

        pointAt.onTextChange = [this]
        {
            const auto points = lane();

            if (pickedPoint >= points.size())
                return;

            const auto asked = model::timeFrom (pointAt.getText().toStdString());

            if (! asked.has_value())
            {
                showPicked();

                if (actions.say != nullptr)
                    actions.say ("that is not a time: seconds, or minutes:seconds");

                return;
            }

            sendLane (model::withLanePoint (points, pickedPoint, *asked,
                                            points[pickedPoint].levelDb, reading.fileLength));
        };

        pointLevel.onTextChange = [this]
        {
            const auto points = lane();

            if (pickedPoint >= points.size())
                return;

            const auto asked = model::levelFrom (pointLevel.getText().toStdString());

            if (! asked.has_value())
            {
                showPicked();

                if (actions.say != nullptr)
                    actions.say ("that is not a level: decibels from -120 to 12, or silence");

                return;
            }

            sendLane (model::withLanePoint (points, pickedPoint, points[pickedPoint].seconds,
                                            *asked, reading.fileLength));
        };

        applyTheme (theme);
    }

    /*  WHAT THE BUTTON DOES NEXT, on the button - a triangle to start it and
        two bars to hold it. A SHAPE and not a colour (4.8), and the tooltip
        says it in words for anyone who reads the glyph the other way. */
    bool WaveformEditorComponent::laneIsMine() const
    {
        return ! reading.laneRecord.cue.empty() && reading.laneRecord.cue == reading.subject.objectId;
    }

    /*  THE LANE DRAWN, AS THE FLIPPED FADERS NAME IT (§34): `level`, or the
        picked send's mix. */
    std::string WaveformEditorComponent::pickedKey() const
    {
        if (sendPicked())
            for (const auto& send : reading.sendLanes)
                if (send.sendId == pickedSend)
                    return send.busId;

        return "level";
    }

    /*  WHERE THE DRAWN LANE'S FADER IS, on the lane's own axis, while this
        cue's pass runs: the ride is the number as heard (UK), and the lane is
        an offset on the written one, so the written number comes off it. */
    std::optional<double> WaveformEditorComponent::drawnRide() const
    {
        if (! (laneIsMine() && reading.laneRecord.recording))
            return std::nullopt;

        const auto key = pickedKey();
        const auto* fader = reading.laneRecord.faderOf (key);

        if (fader == nullptr || ! fader->hasRide)
            return std::nullopt;

        auto written = reading.cueLevel;

        if (key != "level")
            for (const auto& send : reading.sendLanes)
                if (send.busId == key)
                    written = send.writtenDb;

        return fader->rideDb - written;
    }

    int WaveformEditorComponent::recWidth() const
    {
        const auto height = headArea().getHeight();
        return recWide + (freeFader.isVisible() ? height : 0);
    }

    /*  WIDE ENOUGH FOR THE LONGEST OF ITS FOUR WORDS (QY), in the font the
        button draws them in, and a row's height of margin - so "Touch a
        fader…" is never cut to "Touch a f…" while "■ Stop" sits in a button
        twice its size. Measured on every layout, the font following the row. */
    void WaveformEditorComponent::measureRec()
    {
        const auto height = headArea().getHeight();
        const auto font = rec.getLookAndFeel().getTextButtonFont (rec, height);
        auto widest = 0;

        for (const auto* label : { recAtRest, recReady, recRecording })
            widest = juce::jmax (widest, juce::GlyphArrangement::getStringWidthInt (font, said (label)));

        recWide = juce::jmax (height * 3, widest + height);
    }

    int WaveformEditorComponent::pickWidth() const
    {
        return lanePick.isVisible() ? 120 : 0;
    }

    const std::vector<model::LanePoint>& WaveformEditorComponent::shownLane() const
    {
        if (sendPicked())
            for (const auto& send : reading.sendLanes)
                if (send.sendId == pickedSend)
                    return send.points;

        return reading.lane;
    }

    std::string WaveformEditorComponent::shownLaneAddress() const
    {
        return sendPicked() ? model::sendLaneAddress (pickedSend)
                            : model::laneAddress (reading.subject.objectId);
    }

    /*  THE PICKER'S MENU: *Level*, then a line per send, the one drawn
        ticked. Picking lets go of anything the hand was holding, which belongs
        to the lane that was drawn. */
    void WaveformEditorComponent::pickLane()
    {
        juce::PopupMenu menu;
        menu.addItem (1, "Level", true, ! sendPicked());

        for (std::size_t at = 0; at < reading.sendLanes.size(); ++at)
        {
            const auto& send = reading.sendLanes[at];
            menu.addItem (static_cast<int> (at) + 2, "Send to " + juce::String (send.busName), true,
                          send.sendId == pickedSend);
        }

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&lanePick),
                            [safe = juce::Component::SafePointer<WaveformEditorComponent> (this)] (int chosen)
                            {
                                if (safe != nullptr && chosen > 0)
                                    safe->pickLaneAt (chosen == 1 ? -1 : chosen - 2);
                            });
    }

    void WaveformEditorComponent::pickLaneAt (int sendIndex)
    {
        const auto at = static_cast<std::size_t> (sendIndex);

        pickedSend = sendIndex < 0 || at >= reading.sendLanes.size() ? std::string {}
                                                                      : reading.sendLanes[at].sendId;
        held.reset();
        grabbedPoint = hoverPoint = pickedPoint = noPoint;

        sayWhichLane();
        sayWhatRecDoes();
        showPicked();
        repaint();
    }

    /*  THE PICKER SAYS WHICH LANE IS DRAWN, in words (§4.8), and is there only
        for a media cue that sends somewhere: with no send, the level is the
        only lane there is. */
    void WaveformEditorComponent::sayWhichLane()
    {
        const auto offered = reading.cueKind == "media" && ! reading.sendLanes.empty()
                               && reading.notice.empty();

        juce::String word = "Level";

        for (const auto& send : reading.sendLanes)
            if (send.sendId == pickedSend)
                word = "Send to " + juce::String (send.busName);

        lanePick.setButtonText (word + " " + juce::String::fromUTF8 ("\xe2\x96\xbe"));
        lanePick.setTooltip ("Which lane is drawn over the waveform: the level, or one send's");

        if (lanePick.isVisible() != offered)
        {
            lanePick.setVisible (offered);
            resized();
        }
    }

    /*  WHAT THE LANE'S REC DOES NEXT, on the button in words (§4.8), the
        author's (2026-10-05, QY): "Level autom." to arm this cue's lane;
        "Touch a fader…" while it waits for one, a click cancelling; "● Rec
        level" to start a pass once a fader is taken; "■ Stop" to end it. The
        tooltip and the head row's line name the same four, so what the
        button says and what the row says are one vocabulary. The ✕ is up
        while this cue's lane has a fader and no pass runs. Nothing under the
        lock but the stop - a lane is the show's. */
    void WaveformEditorComponent::sayWhatRecDoes()
    {
        const auto& lane = reading.laneRecord;
        const auto mine = laneIsMine();
        const auto recording = mine && lane.recording;

        if (recording)
        {
            rec.setButtonText (said (recRecording));
            rec.setTooltip (said (recRecording) + ": end the pass and write every lane its armed faders rode"
                              " - the faders stay on this cue");
        }
        else if (mine)
        {
            rec.setButtonText (said (recReady));
            rec.setTooltip (said (recReady) + ": play the cue from the playhead and record each fader whose REC"
                              " is lit, from its first touch, held until " + said (recRecording));
        }
        else
        {
            rec.setButtonText (said (recAtRest));
            rec.setTooltip (said (recAtRest) + ": put this cue's level and sends on the faders of every surface"
                              " - then a fader's REC arms its lane");
        }

        rec.setToggleState (mine, juce::dontSendNotification);
        rec.setColour (juce::TextButton::buttonOnColourId,
                       Look::colour (theme, recording ? "failed" : "standby"));
        rec.setEnabled (recording || (reading.cueKind == "media" && ! reading.locked && reading.notice.empty()));

        const auto freeable = mine && ! recording;

        if (freeFader.isVisible() != freeable)
        {
            freeFader.setVisible (freeable);
            resized();
        }
    }

    void WaveformEditorComponent::sayWhichWayTheTransportGoes()
    {
        const auto sounding = reading.running && ! reading.runId.empty();

        transport.setButtonText (juce::String::fromUTF8 (sounding ? "\xe2\x96\x8e\xe2\x96\x8e"
                                                                  : "\xe2\x96\xb6"));
        transport.setTooltip (sounding ? "Stop, and keep the playhead where it is"
                                       : "Play this cue from the playhead");
        transport.setEnabled (sounding || reading.cueKind == "media");
    }

    void WaveformEditorComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;

        if (table != nullptr)
            table->applyTheme (theme);

        for (auto* label : { &pointAt, &pointLevel })
        {
            label->setFont (Look::font (theme, 12.0f));
            label->setColour (juce::Label::textColourId, Look::colour (theme, "ink"));
            label->setColour (juce::Label::backgroundColourId, Look::colour (theme, "panel-in"));
        }

        repaint();
    }

    void WaveformEditorComponent::show (const model::FootReading& readingToUse,
                                        std::shared_ptr<const audio::MediaRecords> mediaToUse)
    {
        const auto wasFile = reading.file;
        const auto wasRanges = reading.ranges.size();

        reading = readingToUse;
        media = std::move (mediaToUse);

        /*  A NEW FILE IS A NEW VIEW. Keeping the old window would open the next
            cue zoomed into a second of it that means nothing there, which is
            the panel showing a reading about one file over the picture of
            another. */
        if (viewFile != reading.file)
        {
            viewFile = reading.file;
            view.reset (reading.fileLength);
            hover = {};
            grabbed = {};
            held.reset();
            grabbedPoint = hoverPoint = pickedPoint = noPoint;
        }

        /*  A PICKED SEND THAT HAS GONE - another cue, or the send deleted -
            gives the level lane back, and whatever the hand held with it. */
        if (sendPicked()
              && std::none_of (reading.sendLanes.begin(), reading.sendLanes.end(),
                               [this] (const model::SendLaneReading& send) { return send.sendId == pickedSend; }))
        {
            pickedSend.clear();
            held.reset();
            grabbedPoint = hoverPoint = pickedPoint = noPoint;
        }

        sayWhichLane();

        /*  THE HAND'S COPY OF THE LANE LETS GO once the reading moves from
            where it was when the write went - or after a second of passes,
            for a write the engine refused and which will never come round. */
        if (held.has_value() && grabbedPoint == noPoint
              && (model::writeLane (shownLane()) != laneBeforeSend || ++sentPasses > 25))
            held.reset();

        if (pickedPoint >= lane().size())
            pickedPoint = noPoint;
        else if (! juce::approximatelyEqual (view.length, reading.fileLength))
        {
            /*  The length arrives late for a file imported in this session, so
                a view built against nought is rebuilt when it turns up. */
            view.reset (reading.fileLength);
        }

        /*  THE TABLE IS HANDED THE SAME READING, never its own: two halves of
            one editor drawn from two moments would show an out-point in the
            box that the bar has already moved. */
        /*  THE SEEK THAT WAS WAITING FOR A RUN TO EXIST. Play was asked for
            from a point somewhere in the file; `cue.fire` made a run and it
            has just turned up, so this is where it is told to start there. */
        if (askedToPlay && reading.running && ! reading.runId.empty()
              && reading.runId != playingRun)
        {
            askedToPlay = false;
            playingRun = reading.runId;

            if (actions.seek != nullptr && point > 0.0)
                actions.seek (reading.runId, point);
        }

        if (! reading.running)
            playingRun.clear();

        sayWhichWayTheTransportGoes();
        sayWhatRecDoes();

        /*  THE RIDE OF THE LANE DRAWN, a point a pass while this cue records
            - the file's second and its fader's level on the lane's own axis -
            and nothing once the pass is over: the lane it wrote is in the
            reading by then. A lane its REC leaves alone follows its curve, and
            draws a trail along it. Another lane picked starts the trail again. */
        if (const auto ride = drawnRide(); ride.has_value() && reading.running)
        {
            if (trailKey != pickedKey())
                trail.clear();

            trailKey = pickedKey();
            trail.push_back ({ reading.position, *ride });
        }
        else if (! (laneIsMine() && reading.laneRecord.recording))
        {
            trail.clear();
        }

        /*  AND THE VIEW FOLLOWS THE PASS (namespace draft §30.4): a 647-second
            file drawn whole is a pixel and a half a second, and a ride drawn
            at that scale is a smudge at the playhead - the author's points
            that "all stayed together at the initial time". So while this cue's
            lane records, the window keeps the playhead in sight no wider than
            a minute (RG), or as close as the hand had zoomed, paging as it
            goes. */
        if (laneIsMine() && reading.laneRecord.recording && reading.running)
            view.follow (reading.position, followSeconds);

        /*  WHEN THE PASS HAS ENDED, WHAT IT WROTE IS PUT ON SCREEN AND SAID -
            the points and the seconds they span, framed - and a pass that
            wrote nothing says why rather than ending in silence. Once per
            pass, by the tick it ended on; a pass that ended before this editor
            opened, or on another cue, is not said here. */
        if (const auto& pass = reading.laneRecord.pass; ! passSeenSet)
        {
            passSeen = pass.tick;
            passSeenSet = true;
        }
        else if (pass.tick != passSeen)
        {
            passSeen = pass.tick;

            if (pass.tick >= 0 && pass.cue == reading.subject.objectId)
            {
                if (pass.how == "kept" && pass.spans)
                    view.frame (pass.from, pass.to);

                if (actions.say != nullptr)
                    actions.say (said (model::lanePassWords (pass).c_str()));
            }
        }

        if (table != nullptr)
        {
            table->setVisible (reading.notice.empty());
            table->show (reading);
            table->setPlayhead (headSeconds());
        }

        if (wasFile != reading.file || wasRanges != reading.ranges.size())
            resized();   // the table's width can change with what is in it

        showPicked();
        repaint();       // the playhead moves every pass; the bar is cached below
    }

    std::vector<model::LanePoint> WaveformEditorComponent::lane() const
    {
        return held.has_value() ? *held : shownLane();
    }

    double WaveformEditorComponent::heightAt (int y) const
    {
        const auto bar = barArea();

        if (bar.getHeight() <= 0)
            return 0.0;

        return static_cast<double> (bar.getBottom() - y) / static_cast<double> (bar.getHeight());
    }

    float WaveformEditorComponent::yForLevel (double levelDb) const
    {
        const auto bar = barArea();

        return static_cast<float> (bar.getBottom())
                 - static_cast<float> (model::laneHeightFor (levelDb) * bar.getHeight());
    }

    juce::Point<float> WaveformEditorComponent::pointPosition (const model::LanePoint& lanePoint) const
    {
        const auto bar = barArea();

        return { static_cast<float> (bar.getX())
                   + static_cast<float> (view.xForSeconds (lanePoint.seconds, bar.getWidth())),
                 yForLevel (lanePoint.levelDb) };
    }

    double WaveformEditorComponent::toleranceHeight() const
    {
        const auto bar = barArea();
        return bar.getHeight() > 0 ? static_cast<double> (grabRadius) / bar.getHeight() : 0.0;
    }

    std::size_t WaveformEditorComponent::laneHit (juce::Point<int> at) const
    {
        if (reading.locked || ! barArea().contains (at))
            return noPoint;

        return model::nearestLanePoint (lane(), secondsAt (at.x), heightAt (at.y),
                                        toleranceSeconds(), toleranceHeight());
    }

    void WaveformEditorComponent::sendLane (const std::vector<model::LanePoint>& points)
    {
        /*  REFUSED HERE rather than at the door, in words, for the shapes the
            door would refuse - which the verbs in `model/Lane` never make, so
            this is the backstop and not the rule. */
        if (const auto why = model::whyNotALane (points); ! why.empty())
        {
            if (actions.say != nullptr)
                actions.say (juce::String (why));

            return;
        }

        const auto text = model::writeLane (points);

        /*  NOTHING TO SEND when the lane is what the reading already says - a
            point typed back to the number it had. */
        if (text == model::writeLane (shownLane()))
        {
            held.reset();
            showPicked();
            repaint();
            return;
        }

        held = points;
        laneBeforeSend = model::writeLane (shownLane());
        sentPasses = 0;

        if (actions.set != nullptr)
            actions.set (shownLaneAddress(), text);

        showPicked();
        repaint();
    }

    /*  WHERE THE TWO NUMBERS GO: the right of the transport's row, clear of
        the clock and of the words, and only while a point is picked. */
    juce::Rectangle<int> WaveformEditorComponent::pointBoxes() const
    {
        auto head = headArea();
        return head.removeFromRight (juce::jmin (230, head.getWidth() / 2)).reduced (0, 2);
    }

    void WaveformEditorComponent::showPicked()
    {
        const auto points = lane();
        const auto visible = pickedPoint < points.size() && reading.notice.empty();

        pointAt.setVisible (visible);
        pointLevel.setVisible (visible);

        if (! visible)
            return;

        pointAt.setEditable (! reading.locked, ! reading.locked, false);
        pointLevel.setEditable (! reading.locked, ! reading.locked, false);

        /*  NOT UNDER A HAND THAT IS TYPING: the next pass would put the old
            number back into the box somebody is halfway through. */
        const auto& shown = points[pickedPoint];

        if (! pointAt.isBeingEdited())
            pointAt.setText (juce::String (model::timeText (shown.seconds)), juce::dontSendNotification);

        if (! pointLevel.isBeingEdited())
            pointLevel.setText (juce::String (model::faderText (shown.levelDb)) + " dB",
                                juce::dontSendNotification);
    }

    void WaveformEditorComponent::setRightColumn (int width, int gap)
    {
        if (columnWidth == width && columnGap == gap)
            return;

        columnWidth = width;
        columnGap = gap;
        resized();
    }

    int WaveformEditorComponent::tableWidth() const
    {
        if (table == nullptr || ! table->isVisible())
            return 0;

        //  Never more than half: the picture is what this panel is for.
        return juce::jmin (columnWidth > 0 ? columnWidth : table->wantedWidth(), getWidth() / 2);
    }

    juce::Rectangle<int> WaveformEditorComponent::pictureArea() const
    {
        auto area = getLocalBounds();
        area.removeFromRight (tableWidth() + columnGap);
        return area;
    }

    juce::Rectangle<int> WaveformEditorComponent::rulerArea() const
    {
        const auto row = juce::roundToInt (theme.row * theme.type * 0.7);
        return pictureArea().removeFromBottom (row);
    }

    juce::Rectangle<int> WaveformEditorComponent::headArea() const
    {
        const auto row = juce::roundToInt (theme.row * theme.type * 0.8);
        return pictureArea().removeFromTop (row);
    }

    juce::Rectangle<int> WaveformEditorComponent::barArea() const
    {
        auto area = pictureArea();
        area.removeFromTop (headArea().getHeight());
        area.removeFromBottom (rulerArea().getHeight());
        return area.reduced (2, 4);
    }

    double WaveformEditorComponent::headSeconds() const
    {
        return reading.running ? reading.position : point;
    }

    void WaveformEditorComponent::moveHeadTo (int x, bool letGo)
    {
        const auto bar = barArea();

        point = std::min (std::max (secondsAt (x), 0.0),
                          reading.fileLength > 0.0 ? reading.fileLength : secondsAt (bar.getRight()));

        if (table != nullptr)
            table->setPlayhead (headSeconds());

        repaint();

        if (! reading.running || reading.runId.empty() || actions.seek == nullptr)
            return;

        /*  ONE PER POSITION THE HAND SETTLES ON, and one when it lets go. A
            record per pixel would be a hundred seeks a second down the log and
            a hundred voices stopped and re-asked. */
        const auto now = juce::Time::getMillisecondCounterHiRes();

        if (! letGo && (now - sentAtMs < 200.0
                          || juce::approximatelyEqual (point, sentSeek)))
        {
            return;
        }

        sentSeek = point;
        sentAtMs = now;
        actions.seek (reading.runId, point);
    }

    double WaveformEditorComponent::secondsAt (int x) const
    {
        const auto bar = barArea();
        return view.secondsForX (x - bar.getX(), bar.getWidth());
    }

    double WaveformEditorComponent::toleranceSeconds() const
    {
        const auto bar = barArea();

        if (bar.getWidth() <= 0 || ! (view.span() > 0.0))
            return 0.0;

        return view.span() * grabRadius / bar.getWidth();
    }

    const std::vector<model::Column>& WaveformEditorComponent::columns()
    {
        const auto bar = barArea();

        if (barsFile == reading.file && barsWidth == bar.getWidth()
              && juce::approximatelyEqual (barsFrom, view.from)
              && juce::approximatelyEqual (barsTo, view.to))
            return bars;

        bars.clear();
        barsFile = reading.file;
        barsWidth = bar.getWidth();
        barsFrom = view.from;
        barsTo = view.to;

        if (media == nullptr || reading.file.empty() || bar.getWidth() <= 0)
            return bars;

        const auto& analysed = *media;
        const auto found = analysed.find (reading.file);

        if (found == analysed.end() || found->second.pyramid == nullptr)
            return bars;

        bars = model::waveform (*found->second.pyramid, found->second.peaks.get(), bar.getWidth(),
                                view.from, view.to);
        return bars;
    }

    void WaveformEditorComponent::paint (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "panel-runs"));

        if (! reading.notice.empty())
        {
            g.setColour (Look::colour (theme, "ink-off"));
            g.setFont (Look::font (theme, 13.0f));
            g.drawFittedText (juce::String (reading.notice), pictureArea().reduced (12, 6),
                              juce::Justification::centred, 3);
            return;
        }

        const auto bar = barArea();

        paintBar (g, bar);
        paintRanges (g, bar);
        paintLane (g, bar);
        paintTrail (g, bar);
        paintRuler (g, rulerArea());
        paintHead (g, headArea());
    }

    void WaveformEditorComponent::paintBar (juce::Graphics& g, juce::Rectangle<int> bar)
    {
        const auto& drawn = columns();

        if (drawn.empty())
        {
            /*  NOT ANALYSED YET is a different thing from silence, and the
                engine's own answer for it is no shape at all (§3.30). */
            g.setColour (Look::colour (theme, "ink-off"));
            g.setFont (Look::font (theme, 12.0f));
            g.drawFittedText ("waiting for the analysis of this file", bar,
                              juce::Justification::centred, 1);
            return;
        }

        const auto middle = bar.getCentreY();
        const auto half = bar.getHeight() / 2.0;
        const auto count = static_cast<int> (drawn.size());

        /*  The running pane's picture, given room: one line per column from
            its lowest sample to its highest - the wave's own shape where the
            finer level is there (2026-09-25: "more precise in level, not
            colour, when zooming in"), the peak mirrored about the middle where
            it is not - coloured by what that slice sounds like. A pixel at
            least, so silence is a line and not a gap. */
        for (auto at = 0; at < count; ++at)
        {
            const auto& column = drawn[static_cast<std::size_t> (at)];

            g.setColour (juce::Colour::fromHSV (static_cast<float> (column.hue / 360.0),
                                                static_cast<float> (column.saturation),
                                                static_cast<float> (0.25 + column.lightness * 0.6),
                                                1.0f));

            const auto top = middle - juce::roundToInt (column.high * half);
            const auto bottom = middle - juce::roundToInt (column.low * half);

            g.fillRect (bar.getX() + at, juce::jmin (top, bottom), 1, juce::jmax (1, std::abs (bottom - top)));
        }
    }

    void WaveformEditorComponent::paintRanges (juce::Graphics& g, juce::Rectangle<int> bar)
    {
        const auto xOf = [this, bar] (double seconds)
        {
            return bar.getX() + juce::roundToInt (view.xForSeconds (seconds, bar.getWidth()));
        };

        /*  THE START OFFSET, where a cue with no ranges begins. Drawn even when
            it is nought, because "the top of the file" is a decision somebody
            can move and a marker that appears only once it is non-zero is one
            nobody discovers. */
        if (reading.ranges.empty())
        {
            const auto x = xOf (reading.startOffset);

            if (bar.contains (x, bar.getCentreY()))
            {
                g.setColour (Look::colour (theme, "standby"));
                g.fillRect (x, bar.getY(), 2, bar.getHeight());
            }
        }

        for (std::size_t at = 0; at < reading.ranges.size(); ++at)
        {
            const auto& range = reading.ranges[at];
            const auto a = xOf (range.in);
            const auto b = xOf (range.out);

            if (b < bar.getX() || a > bar.getRight())
                continue;

            const auto band = juce::Rectangle<int> (a, bar.getY(), juce::jmax (1, b - a),
                                                    bar.getHeight()).getIntersection (bar);

            //  A wash, so the picture under it still reads.
            g.setColour (Look::colour (theme, "picked").withAlpha (0.12f));
            g.fillRect (band);

            /*  THE JOIN IS DRAWN ONCE AND DIFFERENTLY. Where one range ends and
                the next begins, the two edges are the same instant and moving
                one alone would tear the loop - so it is one mark, in the colour
                of the thing it is, and it is grabbed as one. */
            const auto joined = at + 1 < reading.ranges.size()
                                  && model::sameInstant (range.out, reading.ranges[at + 1].in);

            const auto edge = [&] (int x, const char* token, int thickness)
            {
                if (x < bar.getX() - 2 || x > bar.getRight() + 2)
                    return;

                g.setColour (Look::colour (theme, token));
                g.fillRect (x - thickness / 2, bar.getY(), thickness, bar.getHeight());
            };

            const auto hot = [this] (const model::Hit& h, const std::string& id, model::Handle kind)
            {
                return h.handle == kind && h.rangeId == id;
            };

            const auto live = grabbed.handle != model::Handle::none ? grabbed : hover;

            edge (a, hot (live, range.id, model::Handle::in) ? "ink" : "live", 2);

            if (joined)
                edge (b, live.handle == model::Handle::slice && live.rangeId == range.id
                           ? "ink" : "standby", 3);
            else
                edge (b, hot (live, range.id, model::Handle::out) ? "ink" : "stopping", 2);

            //  The name and the loop count, where there is room for them.
            if (band.getWidth() > 46)
            {
                g.setColour (Look::colour (theme, "ink-dim"));
                g.setFont (Look::font (theme, 11.0f));

                const auto label = juce::String (range.name.empty()
                                                   ? "range " + std::to_string (range.index + 1)
                                                   : range.name)
                                     + (range.loops == 1 ? juce::String()
                                        : range.loops == 0
                                          ? " " + juce::String::fromUTF8 ("\xe2\x88\x9e")
                                          : " x" + juce::String (range.loops));

                g.drawText (label, band.reduced (4, 2), juce::Justification::topLeft, true);
            }
        }

        /*  AND THE PLAYHEAD. Where the sound IS when something is sounding
            this cue, and where it WOULD START otherwise - two different claims,
            so two different marks: solid ink for the one that is real, and a
            thinner line in the standby colour for the one that is an
            intention. Black either side of either, so it reads on any colour
            the analysis produced - the running pane's own trick. */
        {
            const auto x = xOf (headSeconds());

            if (x >= bar.getX() && x <= bar.getRight())
            {
                g.setColour (juce::Colours::black);
                g.fillRect (x - 2, bar.getY(), 4, bar.getHeight());
                g.setColour (Look::colour (theme, reading.running ? "ink" : "standby"));
                g.fillRect (x - 1, bar.getY(), reading.running ? 2 : 1, bar.getHeight());
            }
        }
    }

    /*  THE LEVEL LANE OVER THE WAVEFORM (namespace draft §20.5, decision CY):
        a line across the bar, a dot at each point, and at the playhead a dot
        where the lane is now. On the fader's throw (DD), and sampled a column
        at a time, so a segment straight in dB bends where the throw bends and
        the picture is exactly what will be heard. Black under ink, the
        playhead's trick, so it reads on whatever colour the analysis drew.

        A LANE NOBODY HAS DRAWN is a dim line at unity - where the cue is as
        written - and it is the line a double click adds the first point to,
        so the gesture is discoverable without a word of instruction. */
    void WaveformEditorComponent::paintLane (juce::Graphics& g, juce::Rectangle<int> bar)
    {
        const auto points = lane();

        if (bar.getWidth() <= 1 || ! (view.span() > 0.0))
            return;

        juce::Path line;

        for (auto x = bar.getX(); ; x = juce::jmin (x + 2, bar.getRight()))
        {
            const auto y = yForLevel (model::laneLevelAt (points, secondsAt (x)));

            if (x == bar.getX())
                line.startNewSubPath (static_cast<float> (x), y);
            else
                line.lineTo (static_cast<float> (x), y);

            if (x >= bar.getRight())
                break;
        }

        if (points.empty())
        {
            g.setColour (Look::colour (theme, "ink-off").withAlpha (0.7f));
            g.strokePath (line, juce::PathStrokeType (1.0f));
            return;
        }

        g.setColour (juce::Colours::black.withAlpha (0.8f));
        g.strokePath (line, juce::PathStrokeType (3.0f));
        g.setColour (Look::colour (theme, "ink"));
        g.strokePath (line, juce::PathStrokeType (1.5f));

        /*  THE POINTS. The one under a hand is larger, and the picked one -
            whose numbers are in the head row - is ringed: a SHAPE, not only a
            colour (§4.8). */
        for (std::size_t at = 0; at < points.size(); ++at)
        {
            const auto drawnAt = pointPosition (points[at]);
            const auto x = drawnAt.x;
            const auto y = drawnAt.y;

            if (x < static_cast<float> (bar.getX() - 6) || x > static_cast<float> (bar.getRight() + 6))
                continue;
            const auto live = at == grabbedPoint || at == hoverPoint;
            const auto radius = live ? 5.5f : 4.0f;

            g.setColour (juce::Colours::black);
            g.fillEllipse (x - radius - 1.0f, y - radius - 1.0f, 2.0f * (radius + 1.0f), 2.0f * (radius + 1.0f));
            g.setColour (Look::colour (theme, live ? "live" : "ink"));
            g.fillEllipse (x - radius, y - radius, 2.0f * radius, 2.0f * radius);

            if (at == pickedPoint)
            {
                g.setColour (Look::colour (theme, "picked"));
                g.drawEllipse (x - radius - 3.0f, y - radius - 3.0f,
                               2.0f * (radius + 3.0f), 2.0f * (radius + 3.0f), 1.5f);
            }
        }

        //  Where the lane is now, while the cue sounds.
        if (reading.running)
        {
            const auto x = static_cast<float> (bar.getX())
                             + static_cast<float> (view.xForSeconds (reading.position, bar.getWidth()));

            if (x >= static_cast<float> (bar.getX()) && x <= static_cast<float> (bar.getRight()))
            {
                /*  WHILE A PASS RECORDS, WHERE THE FADER IS: what is heard is
                    the hand's, not the lane's. */
                const auto ride = drawnRide();
                const auto riding = ride.has_value();
                const auto y = yForLevel (riding ? *ride : model::laneLevelAt (points, reading.position));

                g.setColour (juce::Colours::black);
                g.fillEllipse (x - 3.5f, y - 3.5f, 7.0f, 7.0f);
                g.setColour (Look::colour (theme, riding ? "failed" : "standby"));
                g.fillEllipse (x - 2.5f, y - 2.5f, 5.0f, 5.0f);
            }
        }
    }

    /*  THE RIDE AS IT IS HEARD (§20.9): a line in the recording's colour over
        the lane, broken wherever the second falls back - a loop's wrap - so a
        second pass over a stretch draws over the first rather than across. */
    void WaveformEditorComponent::paintTrail (juce::Graphics& g, juce::Rectangle<int> bar)
    {
        if (trail.size() < 2)
            return;

        juce::Path line;
        auto previous = -1.0;

        for (const auto& ridden : trail)
        {
            const auto at = pointPosition (ridden);

            if (ridden.seconds < previous || previous < 0.0)
                line.startNewSubPath (at);
            else
                line.lineTo (at);

            previous = ridden.seconds;
        }

        g.saveState();
        g.reduceClipRegion (bar);
        g.setColour (juce::Colours::black.withAlpha (0.8f));
        g.strokePath (line, juce::PathStrokeType (3.5f));
        g.setColour (Look::colour (theme, "failed"));
        g.strokePath (line, juce::PathStrokeType (2.0f));
        g.restoreState();
    }

    /*  THE TRANSPORT'S OWN ROW: the button, and beside it where the head is
        and what it is doing. The clock is always written out, because the
        mark on the picture answers "roughly where" and placing an in-point
        wants "exactly when". */
    void WaveformEditorComponent::paintHead (juce::Graphics& g, juce::Rectangle<int> head)
    {
        auto area = head.withTrimmedLeft (head.getHeight() * 2 + recWidth() + pickWidth() + 8);

        g.setFont (Look::font (theme, 12.0f));
        g.setColour (Look::colour (theme, reading.running ? "ink" : "ink-dim"));
        g.drawText (clockText (headSeconds()), area.removeFromLeft (70),
                    juce::Justification::centredLeft, false);

        g.setColour (Look::colour (theme, "ink-off"));
        g.setFont (Look::font (theme, 11.0f));

        /*  THE SPEED AND ITS MODE (namespace draft §22.7), beside the clock -
            which counts the file's seconds, not the room's - whenever either
            is not the plain one: "×0.5 varispeed", "×1 timestretch". */
        if (const auto speed = speedWords (reading); speed.isNotEmpty())
        {
            g.setColour (Look::colour (theme, "ink-dim"));
            g.drawText (speed, area.removeFromLeft (juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), speed) + 12),
                        juce::Justification::centredLeft, false);
            g.setColour (Look::colour (theme, "ink-off"));
        }

        /*  THE PICKED POINT'S NUMBERS take the right of the row, captioned
            in words so two bare numbers are never left to explain themselves,
            and the row's instructions give way to them: a hand that has picked
            a point has found the lane already. */
        if (pointAt.isVisible())
        {
            auto boxes = pointBoxes();

            g.drawText (sendPicked() ? "send point at" : "level point at",
                        boxes.removeFromLeft (boxes.getWidth() - 2 * 64 - 6).withTrimmedRight (4),
                        juce::Justification::centredRight, true);
            return;
        }

        /*  THE FADERS FLIPPED TO THIS CUE say so here, in place of the row's
            instructions: which lanes are armed and what a pass is doing - in
            the button's own words, so the line names the button that does the
            next thing (§34). */
        const auto& laneRecord = reading.laneRecord;

        if (laneIsMine())
        {
            juce::String armed;

            for (const auto& name : laneRecord.armedNames())
                armed += (armed.isEmpty() ? "" : ", ") + juce::String::fromUTF8 (name.c_str());

            g.setColour (Look::colour (theme, laneRecord.recording ? "failed" : "ink-dim"));
            g.drawText (laneRecord.recording
                          ? (armed.isEmpty() ? juce::String ("playing back - a fader's REC joins the pass")
                                             : "recording " + armed + ", each from its first touch - a fader's"
                                                 " REC joins or leaves - " + said (recRecording) + " ends the pass")
                          : (armed.isEmpty() ? "the faders show this cue - a fader's REC arms its lane, then "
                                                 + said (recReady)
                                             : "REC on " + armed + " - " + said (recReady)
                                                 + " plays the cue from the playhead and records each from its"
                                                   " first touch"),
                        area, juce::Justification::centredLeft, true);
            return;
        }

        const auto lanes = reading.cueKind == "media" && ! reading.locked
                             ? juce::String ("  ") + juce::String::fromUTF8 ("\xc2\xb7")
                                 + (sendPicked() ? "  double-click the send's line to add a point"
                                                 : "  double-click the level line to add a point")
                             : juce::String();

        g.drawText ((reading.running ? "playing - drag the ruler to move the playhead"
                                      : "drag the ruler to place the playhead") + lanes,
                    area, juce::Justification::centredLeft, true);
    }

    void WaveformEditorComponent::paintRuler (juce::Graphics& g, juce::Rectangle<int> ruler)
    {
        g.setColour (Look::colour (theme, "rule"));
        g.fillRect (ruler.getX(), ruler.getY(), ruler.getWidth(), 1);

        if (! (view.span() > 0.0) || ruler.getWidth() <= 0)
            return;

        const auto step = rulerStep (view.span());
        const auto first = std::ceil (view.from / step) * step;

        g.setFont (Look::font (theme, 11.0f));

        for (auto seconds = first; seconds <= view.to; seconds += step)
        {
            const auto x = barArea().getX()
                             + juce::roundToInt (view.xForSeconds (seconds, barArea().getWidth()));

            g.setColour (Look::colour (theme, "rule"));
            g.fillRect (x, ruler.getY(), 1, 4);

            /*  TO THE RIGHT OF ITS MARK, AND TO THE LEFT AT THE END (author,
                2026-09-21: *"the last unit can be shift on the other side so
                it doesn't overflow in the range table"*). A figure is written
                after its tick everywhere it fits; the last one would run off
                the picture and under the numbers beside it, so it is written
                before its tick instead. The mark is what carries the instant
                either way - the text only names it. */
            const auto figure = clockText (seconds);
            const auto wide = juce::jmax (30, juce::GlyphArrangement::getStringWidthInt (
                                                  g.getCurrentFont(), figure) + 6);

            const auto after = x + 3 + wide <= ruler.getRight();

            g.setColour (Look::colour (theme, "ink-off"));
            g.drawText (figure,
                        juce::Rectangle<int> (after ? x + 3 : x - 3 - wide, ruler.getY(),
                                              wide, ruler.getHeight()),
                        after ? juce::Justification::centredLeft
                              : juce::Justification::centredRight,
                        false);
        }

        /*  AND WHETHER THIS IS THE WHOLE FILE OR A WINDOW INTO IT, said in
            words at the right: a zoomed bar and a whole one look identical
            otherwise, and somebody who has lost track of where they are should
            not have to count ruler marks to find out. */
        if (! view.isWholeThing())
        {
            g.setColour (Look::colour (theme, "ink-off"));
            g.drawText (clockText (view.from) + " to " + clockText (view.to)
                          + "  " + juce::String::fromUTF8 ("\xc2\xb7") + "  double-click shows all",
                        ruler.reduced (6, 0), juce::Justification::centredRight, true);
        }
    }

    void WaveformEditorComponent::resized()
    {
        if (table != nullptr)
            table->setBounds (getLocalBounds().removeFromRight (tableWidth()));

        barsFrom = barsTo = 0.0;   // the window moved with the width

        barsWidth = 0;   // the bucketing is per width

        auto head = headArea();
        measureRec();
        transport.setBounds (head.removeFromLeft (head.getHeight() * 2).reduced (2, 1));
        rec.setBounds (head.removeFromLeft (recWide).reduced (2, 1));

        if (freeFader.isVisible())
            freeFader.setBounds (head.removeFromLeft (head.getHeight()).reduced (2, 1));

        if (lanePick.isVisible())
            lanePick.setBounds (head.removeFromLeft (pickWidth()).reduced (2, 1));

        auto boxes = pointBoxes();
        pointLevel.setBounds (boxes.removeFromRight (64));
        boxes.removeFromRight (6);
        pointAt.setBounds (boxes.removeFromRight (64));

        repaint();
    }

    void WaveformEditorComponent::mouseMove (const juce::MouseEvent& event)
    {
        if (rulerArea().contains (event.getPosition()))
        {
            setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);

            if (hover.handle != model::Handle::none)
            {
                hover = {};
                repaint();
            }

            return;
        }

        /*  A LEVEL POINT IS FOUND FIRST: it is a dot on an edge's line more
            often than not, and a slice's edge has the whole height of the bar
            to be grabbed by where a point has only itself. */
        if (const auto under = laneHit (event.getPosition()); under != hoverPoint)
        {
            hoverPoint = under;
            repaint();
        }

        if (hoverPoint != noPoint)
        {
            setMouseCursor (juce::MouseCursor::DraggingHandCursor);

            if (hover.handle != model::Handle::none)
                hover = {};

            return;
        }

        if (reading.ranges.empty() || ! barArea().contains (event.getPosition()))
        {
            setMouseCursor (juce::MouseCursor::NormalCursor);

            if (hover.handle != model::Handle::none)
            {
                hover = {};
                repaint();
            }

            return;
        }

        const auto found = model::hitTest (reading.ranges, secondsAt (event.x), toleranceSeconds());

        if (found.handle != hover.handle || found.rangeId != hover.rangeId)
        {
            hover = found;
            setMouseCursor (hover.handle == model::Handle::none
                              ? juce::MouseCursor::NormalCursor
                              : juce::MouseCursor::LeftRightResizeCursor);
            repaint();
        }
    }

    void WaveformEditorComponent::mouseDown (const juce::MouseEvent& event)
    {
        grabbed = {};
        panning = false;
        onRuler = false;

        /*  THE RULER IS THE PLAYHEAD'S STRIP (author, 2026-09-21: *"the
            playhead can be controlled in the time ruler"*). A press there puts
            the head where the pointer is and the drag carries it, which is
            what a time ruler does everywhere - and it keeps the BAR free for
            the in and out points, so neither gesture has to be aimed around
            the other. */
        if (rulerArea().contains (event.getPosition()))
        {
            onRuler = true;
            sentSeek = -1.0;
            moveHeadTo (event.x, false);
            return;
        }

        if (! barArea().contains (event.getPosition()))
            return;

        if (event.mods.isPopupMenu() || event.mods.isMiddleButtonDown())
        {
            panning = true;
            panFrom = event.x;
            return;
        }

        /*  A LEVEL POINT BEFORE A SLICE'S EDGE (§20.5), and the press picks
            it - its two numbers come up in the head row - whether or not the
            hand goes on to drag it. */
        grabbedPoint = laneHit (event.getPosition());

        if (grabbedPoint != noPoint)
        {
            pickedPoint = grabbedPoint;
            held = lane();
            pointMoved = false;
            showPicked();
            repaint();
            return;
        }

        grabbed = model::hitTest (reading.ranges, secondsAt (event.x), toleranceSeconds());

        /*  A PRESS ON NOTHING PANS, which is the gesture people try first on a
            picture that is wider than its window. Grabbing an edge wins, so a
            hand aiming at a handle never scrolls the view out from under it. */
        if (grabbed.handle == model::Handle::none)
        {
            panning = true;
            panFrom = event.x;
        }
    }

    void WaveformEditorComponent::mouseDrag (const juce::MouseEvent& event)
    {
        if (onRuler)
        {
            moveHeadTo (event.x, false);
            return;
        }

        if (panning)
        {
            view.panBy (panFrom - event.x, barArea().getWidth());
            panFrom = event.x;
            repaint();
            return;
        }

        if (grabbedPoint != noPoint && held.has_value())
        {
            auto seconds = secondsAt (event.x);
            auto level = model::laneLevelForHeight (heightAt (event.y));

            /*  SNAPPED unless Alt says otherwise, as an edge is: its second to
                a slice's edges and the file's ends, its level to unity - the
                cue as written, the one level a hand most often means. */
            if (! event.mods.isAltDown())
            {
                seconds = model::snapTo (seconds, model::snapTargets (reading.ranges, {}, reading.fileLength),
                                         toleranceSeconds());

                if (std::abs (heightAt (event.y) - model::laneHeightFor (0.0)) < toleranceHeight() * 0.5)
                    level = 0.0;
            }

            held = model::withLanePoint (*held, grabbedPoint, seconds, level, reading.fileLength);
            pointMoved = true;

            const auto& moved = (*held)[grabbedPoint];

            if (actions.say != nullptr)
                actions.say ("level point at " + clockText (moved.seconds) + ", "
                               + juce::String (model::faderText (moved.levelDb)) + " dB");

            showPicked();
            repaint();
            return;
        }

        if (grabbed.handle == model::Handle::none || actions.set == nullptr)
            return;

        auto seconds = secondsAt (event.x);

        /*  SNAPPED TO WHAT IS ALREADY THERE unless the hand says otherwise -
            the other edges, both ends of the file - because a designer placing
            a loop point almost always means "where that one is". Alt lets go
            of the magnet, which is the same key the cue list uses to mean
            "not the ordinary reading of this drag". */
        if (! event.mods.isAltDown())
            seconds = model::snapTo (seconds,
                                     model::snapTargets (reading.ranges, grabbed.rangeId,
                                                         reading.fileLength),
                                     toleranceSeconds());

        const auto writes = model::dragTo (grabbed, seconds, reading.ranges, reading.fileLength);

        for (const auto& write : writes)
            actions.set (model::rangeAddress (write.rangeId, write.attribute),
                         osc::formatDouble (write.seconds));

        if (! writes.empty() && actions.say != nullptr)
            actions.say (juce::String (grabbed.handle == model::Handle::slice
                                         ? "loop point at " : "edge at ")
                           + clockText (writes.front().seconds));
    }

    void WaveformEditorComponent::mouseDoubleClick (const juce::MouseEvent& event)
    {
        /*  ON THE LANE, A DOUBLE CLICK DRAWS (§20.5): on a point it takes the
            point away, and on the line it adds one there - on the line, so the
            level does not move until somebody moves the point. The fade
            editor's two gestures, in the same places. */
        if (reading.cueKind == "media" && ! reading.locked && barArea().contains (event.getPosition()))
        {
            const auto points = lane();

            if (const auto hit = laneHit (event.getPosition()); hit != noPoint)
            {
                grabbedPoint = hoverPoint = pickedPoint = noPoint;
                sendLane (model::removeLanePoint (points, hit));
                return;
            }

            const auto seconds = secondsAt (event.x);

            if (model::onLaneLine (points, seconds, heightAt (event.y), toleranceHeight()))
            {
                if (const auto added = model::insertLanePoint (points, seconds, reading.fileLength))
                {
                    for (std::size_t at = 0; at < added->size(); ++at)
                        if (std::abs ((*added)[at].seconds - seconds) < 1.0e-9)
                            pickedPoint = at;

                    panning = false;
                    sendLane (*added);
                }

                return;
            }
        }

        /*  BACK TO THE WHOLE FILE, which is the way out of a zoom that has got
            away from somebody. Deliberately NOT Escape: that key is PANIC in
            this window (PRD 4.4), and a panel that quietly took it for its own
            would swallow the one key an operator reaches for when a show is
            going wrong. */
        view.reset (reading.fileLength);
        grabbed = {};
        panning = false;
        repaint();
    }

    void WaveformEditorComponent::mouseUp (const juce::MouseEvent& event)
    {
        if (onRuler)
        {
            moveHeadTo (event.x, true);
            onRuler = false;
        }

        /*  THE DRAG'S ONE WRITE, and only if the point moved: a press that
            only picked a point has decided nothing. */
        if (grabbedPoint != noPoint)
        {
            grabbedPoint = noPoint;

            if (pointMoved && held.has_value())
                sendLane (*held);
            else
                held.reset();

            pointMoved = false;
        }

        grabbed = {};
        panning = false;

        if (actions.say != nullptr)
            actions.say ({});

        repaint();
    }

    void WaveformEditorComponent::mouseExit (const juce::MouseEvent&)
    {
        if (hover.handle != model::Handle::none || hoverPoint != noPoint)
        {
            hover = {};
            hoverPoint = noPoint;
            repaint();
        }
    }

    void WaveformEditorComponent::mouseWheelMove (const juce::MouseEvent& event,
                                                  const juce::MouseWheelDetails& wheel)
    {
        if (! (reading.fileLength > 0.0))
            return;

        const auto bar = barArea();

        /*  TWO AXES, TWO JOBS (author, 2026-09-21: *"vertical scroll = zoom,
            horizontal scroll = scroll back and forth the view of the
            waveform"*). A trackpad sends both at once, so the larger movement
            wins rather than the picture doing both things at a slant. */
        if (std::abs (wheel.deltaX) > std::abs (wheel.deltaY))
        {
            /*  A WHEEL'S WORTH OF TRAVEL IS A QUARTER OF THE WINDOW, so the
                picture moves by an amount somebody can follow at any zoom -
                a fixed number of pixels would crawl when zoomed out and fly
                when zoomed in. */
            view.panBy (juce::roundToInt (-wheel.deltaX * bar.getWidth() * 0.25),
                        bar.getWidth());
            repaint();
            return;
        }

        if (juce::approximatelyEqual (wheel.deltaY, 0.0f))
            return;

        /*  ABOUT THE POINTER, so the second under it stays under it: that is
            what makes a wheel feel like magnifying the picture rather than
            scrolling it. `model/View` holds the arithmetic and the test. */
        view.zoomAbout (event.x - bar.getX(), bar.getWidth(),
                        wheel.deltaY > 0 ? 0.85 : 1.0 / 0.85);
        repaint();
    }
}
