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
            if (reading.cueKind != "media" && ! reading.movie)
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
        /*  THE KEYS ONCE CLICKED (namespace draft §55.9, AEF): x and
            Backspace are this panel's; every other key goes on up to the
            window - Space to GO, the arrows to the list, Esc to panic. */
        setWantsKeyboardFocus (true);
        setMouseClickGrabsKeyboardFocus (true);

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

            //  A sound, or a movie (§47, AAC): what has a playhead to play from.
            if ((reading.cueKind != "media" && ! reading.movie) || actions.play == nullptr)
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

        /*  A MOVIE'S CUTS, SHOWN AND SNAPPED TO (§47.10): ticks, so whether
            each is on is a shape in the box and not a colour (§4.8). */
        showCuts.setTooltip ("Mark where the movie changes shot: a line with a triangle at its head,"
                             " dashed for a dissolve");
        snapCuts.setTooltip ("The playhead, and an in or out point, dragged near a scene change land on it"
                             " - Alt lets go. Only while the scene changes are shown");

        for (auto* toggle : { &showCuts, &snapCuts })
        {
            toggle->setWantsKeyboardFocus (false);
            toggle->setToggleState (true, juce::dontSendNotification);
            toggle->onClick = [this] { sayCuts(); repaint(); };
            addChildComponent (*toggle);
        }

        /*  THE SECTIONS ROW (namespace draft §55): a sound cut into pieces at
            the playhead, the pieces dragged into an order, a join's crossfade
            dragged, each piece at a trim, the edit frozen to a file the cue
            plays. Every button says what it does (§4.8) and is greyed when
            the edit is not this panel's to make. */
        splitButton.setButtonText ("Split");
        splitButton.setTooltip ("Splits the sound - or the movie - at the playhead into two sections");
        removeButton.setButtonText ("Remove");
        removeButton.setTooltip ("Removes the picked section: what sat on it goes with it, the rest closes up");
        joinButton.setButtonText ("Join");
        joinButton.setTooltip ("Joins the picked section with the next, when the two are still one in the file");
        freezeButton.setButtonText ("Freeze");
        freezeButton.setTooltip ("Freezes the edit: the render is bounced to a new file the cue plays");

        for (auto* button : { &splitButton, &removeButton, &joinButton, &freezeButton })
        {
            button->setWantsKeyboardFocus (false);
            addChildComponent (*button);
        }

        splitButton.onClick = [this] { splitHere(); };

        removeButton.onClick = [this]
        {
            if (! reading.editable || actions.removeSection == nullptr)
                return refuseEdit();

            //  The button closes up, as its words say (AEE); Backspace leaves silence.
            if (pickedSection < reading.sections.size())
                actions.removeSection (reading.sections[pickedSection].id, false);
        };

        joinButton.onClick = [this]
        {
            if (! reading.editable || actions.joinSection == nullptr)
                return refuseEdit();

            if (pickedSection + 1 >= reading.sections.size())
                return;

            if (! model::continuousJoin (reading.sections[pickedSection], reading.sections[pickedSection + 1]))
                return tell ("these two are no longer one in the file - only a cut can be taken back");

            actions.joinSection (reading.sections[pickedSection].id);
        };

        freezeButton.onClick = [this]
        {
            if (reading.locked)
                return refuseEdit();

            if (reading.frozen)
            {
                if (actions.unfreezeEdit)
                    actions.unfreezeEdit (reading.subject.objectId);

                return;
            }

            if (reading.sections.empty())
                return tell ("Split at the playhead to start an edit.");

            if (reading.render.state != "done")
                return tell ("the edit is still being rendered - Freeze once it is");

            if (actions.freezeEdit)
                actions.freezeEdit (reading.subject.objectId);
        };

        /*  THE PICKED SECTION'S TRIM, TYPED, by the level box's rules: a
            number that will not parse is put back rather than written. */
        trimBox.setEditable (true, true, false);
        trimBox.setJustificationType (juce::Justification::centredRight);
        trimBox.setTooltip ("The picked section's trim, in dB: nought is the material as recorded");
        addChildComponent (trimBox);
        trimBox.onTextChange = [this]
        {
            if (pickedSection >= reading.sections.size())
                return;

            const auto trim = model::trimFrom (trimBox.getText().toStdString());

            if (! trim.has_value())
            {
                tell ("that is not a trim: -6, +3, silence");
                showPickedSection();
                return;
            }

            if (! reading.editable)
            {
                showPickedSection();
                return refuseEdit();
            }

            if (actions.set)
                actions.set ("/godot/section/" + reading.sections[pickedSection].id + "/trim",
                             osc::formatDouble (*trim));
        };

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

    /*  THE CUTS' TICKS ARE A MOVIE'S (§47.10), and the snap is greyed while
        the marks are hidden - it would pull the hand to something unseen. */
    void WaveformEditorComponent::sayCuts()
    {
        const auto offered = reading.movie && reading.notice.empty();

        snapCuts.setEnabled (showCuts.getToggleState());

        if (showCuts.isVisible() != offered)
        {
            showCuts.setVisible (offered);
            snapCuts.setVisible (offered);
            resized();
        }
    }

    int WaveformEditorComponent::cutsWidth() const
    {
        return showCuts.isVisible() ? showCuts.getWidth() + snapCuts.getWidth() : 0;
    }

    bool WaveformEditorComponent::cutsSnap() const
    {
        return reading.movie && strip != nullptr && showCuts.getToggleState() && snapCuts.getToggleState();
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
        transport.setEnabled (sounding || reading.cueKind == "media" || reading.movie);
    }

    std::string WaveformEditorComponent::heldEdgeWord() const
    {
        if (! edgeHeld.has_value())
            return {};

        switch (grabbed.handle)
        {
            case model::Handle::in:     return "in point";
            case model::Handle::out:    return "out point";
            case model::Handle::slice:  return "slice";
            case model::Handle::none:   break;
        }

        return {};
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

        for (auto* toggle : { &showCuts, &snapCuts })
        {
            toggle->setColour (juce::ToggleButton::textColourId, Look::colour (theme, "ink-dim"));
            toggle->setColour (juce::ToggleButton::tickColourId, Look::colour (theme, "picked"));
            toggle->setColour (juce::ToggleButton::tickDisabledColourId, Look::colour (theme, "ink-off"));
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

        //  A movie's strip, when the analyser has found it (§47, AAI).
        {
            std::shared_ptr<const video::strip::MovieStrip> found;

            if (reading.movie && media != nullptr)
                if (const auto record = media->find (reading.file); record != media->end())
                    found = record->second.strip;

            if (found != strip)
            {
                strip = std::move (found);
                stripImages.clear();
            }
        }

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
        sayCuts();

        /*  THE SECTIONS (namespace draft §55): another cue lets the pick go;
            a section gone lets it go; what the buttons may do is read anew. */
        if (sectionsCue != reading.subject.objectId)
        {
            if (curveTarget.has_value())
                stopTimer();

            curveTarget.reset();
            curvePreview.clear();
            sectionsCue = reading.subject.objectId;
            pickedSection = draggedSection = noSection;
            sectionMoved = false;
            selection.reset();
            gripHeld = gripHover = {};
            slidSection = noSection;
            slidePreview.clear();
            pickWhenCut.reset();
            markerHeld.clear();
            markerHover.clear();
            markerSent.clear();
            menuMarker.clear();
        }

        /*  THE PIECE TRIM CUT OUT (namespace draft §81, ATX), picked once its
            cut has come back - so its trim box is the one shown. Within half a
            frame of where the selection began, which is where a movie's cut
            lands; given up after a second of passes, as a refusal never comes. */
        if (pickWhenCut.has_value())
        {
            const auto starts = model::sectionStarts (reading.sections);
            const auto slack = std::max (0.002, reading.frameRate > 0.0 ? 0.51 / reading.frameRate : 0.0);

            for (std::size_t k = 0; k < starts.size(); ++k)
                if (std::abs (starts[k] - *pickWhenCut) < slack)
                {
                    pickedSection = k;
                    pickWhenCut.reset();
                    break;
                }

            if (pickWhenCut.has_value() && ++pickWaited > 25)
                pickWhenCut.reset();
        }

        if (! markerSent.empty())
        {
            const auto found = std::find_if (reading.markers.begin(), reading.markers.end(),
                                             [this] (const model::MarkerRow& marker) { return marker.id == markerSent; });

            if (found == reading.markers.end() || std::abs (found->at - markerSentAt) < 1.0e-6 || ++markerSentPasses > 25)
                markerSent.clear();
        }

        if (pickedSection >= reading.sections.size())
            pickedSection = noSection;

        sayEdit();
        showPickedSection();

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
        area.removeFromTop (headArea().getHeight() + buttonsArea().getHeight() + sectionsArea().getHeight());
        area.removeFromBottom (rulerArea().getHeight());
        return area.reduced (2, 4);
    }

    double WaveformEditorComponent::headSeconds() const
    {
        return reading.running ? reading.position : point;
    }

    void WaveformEditorComponent::moveHeadTo (int x, bool letGo, bool snap)
    {
        const auto bar = barArea();
        auto seconds = secondsAt (x);

        /*  ONTO A MOVIE'S CUT when it is near one (§47.10), so the head sits
            on a shot's first frame - the monitor shows it - as an edge does;
            and onto a marker (namespace draft §81), which is a point somebody
            wanted to come back to. */
        if (snap)
        {
            auto targets = markerSeconds();

            if (cutsSnap())
                for (const auto& cut : cutsShown())
                    targets.push_back (cut.seconds);

            if (! targets.empty())
                seconds = model::snapTo (seconds, targets, toleranceSeconds());
        }

        point = std::min (std::max (seconds, 0.0),
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

        //  THE EDIT AS THE HAND HAS IT (§55.13): a slide or a handle drawn waveform and all.
        const auto& shownSections = sectionsShown();

        /*  A MOVIE DRAWS THE SOUND LOCKED TO IT (namespace draft §47, AAC):
            the same seconds, since the sound is the movie's own, taken out
            over the same span. */
        const auto& drawnFile = reading.movie ? reading.soundFile : reading.file;

        /*  THE EDIT IS PART OF THE KEY (namespace draft §55): the bar draws the
            edited timeline, each section's stretch of the file's own picture
            laid where the section is, so a cut or a move redraws it. */
        std::string editKey;

        /*  NOT A FROZEN ONE (the author, 2026-10-17: "Freezing the file doesn't
            show the frozen audio's waveform and spectral colours"): its file is
            the bounce, already the edited timeline, and is drawn whole. Its
            sections are kept for Unfreeze, and read at their source seconds they
            laid the bounce's picture in the wrong places. */
        if (editShown() && ! reading.frozen)
            for (const auto& section : shownSections)
                editKey += section.id + ":" + osc::formatDouble (section.in) + "-" + osc::formatDouble (section.out)
                         + "@" + osc::formatDouble (section.gap) + "*" + osc::formatDouble (section.trimDb) + ";";

        if (barsFile == drawnFile && barsWidth == bar.getWidth() && barsEdit == editKey
              && juce::approximatelyEqual (barsFrom, view.from)
              && juce::approximatelyEqual (barsTo, view.to))
            return bars;

        bars.clear();
        barsFile = drawnFile;
        barsEdit = editKey;
        barsWidth = bar.getWidth();
        barsFrom = view.from;
        barsTo = view.to;

        /*  NOT ANALYSED YET IS NOT CACHED (the run pane's rule): a file the
            analyser has not reached - a bounce Freeze just wrote - would
            otherwise stay blank until a zoom or a resize asked again. */
        if (media == nullptr || drawnFile.empty() || bar.getWidth() <= 0)
        {
            barsFile.clear();
            return bars;
        }

        const auto& analysed = *media;
        const auto found = analysed.find (drawnFile);

        if (found == analysed.end() || found->second.pyramid == nullptr)
        {
            barsFile.clear();
            return bars;
        }

        if (editKey.empty())
        {
            bars = model::waveform (*found->second.pyramid, found->second.peaks.get(), bar.getWidth(),
                                    view.from, view.to);
            return bars;
        }

        /*  SECTION BY SECTION: the part of each that meets the view, taken
            from the file at the section's own seconds and laid at its place
            on the timeline; a gap between is left silent. */
        bars.assign (static_cast<std::size_t> (bar.getWidth()), model::Column {});
        const auto starts = model::sectionStarts (shownSections);

        for (std::size_t k = 0; k < shownSections.size(); ++k)
        {
            const auto& section = shownSections[k];
            const auto from = juce::jmax (starts[k], view.from);
            const auto to = juce::jmin (starts[k] + section.length(), view.to);

            if (! (to > from))
                continue;

            const auto x0 = juce::jlimit (0, bar.getWidth(), juce::roundToInt (view.xForSeconds (from, bar.getWidth())));
            const auto x1 = juce::jlimit (0, bar.getWidth(), juce::roundToInt (view.xForSeconds (to, bar.getWidth())));

            if (x1 <= x0)
                continue;

            auto piece = model::waveform (*found->second.pyramid, found->second.peaks.get(), x1 - x0,
                                          section.in + (from - starts[k]), section.in + (to - starts[k]));

            /*  AT ITS TRIM (55.9): a section turned down looks turned down. */
            const auto gain = std::min (4.0, std::pow (10.0, section.trimDb / 20.0));

            for (auto& column : piece)
            {
                column.peak = std::min (1.0, column.peak * gain);
                column.low = std::max (-1.0, column.low * gain);
                column.high = std::min (1.0, column.high * gain);
            }

            for (std::size_t i = 0; i < piece.size() && x0 + static_cast<int> (i) < bar.getWidth(); ++i)
                bars[static_cast<std::size_t> (x0) + i] = piece[i];
        }

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

        if (editShown())
            paintGrips (g, bar);
        paintRuler (g, rulerArea());
        paintHead (g, headArea());

        if (editShown())
            paintSections (g, sectionsArea());
    }

    void WaveformEditorComponent::paintStrip (juce::Graphics& g, juce::Rectangle<int> pictures)
    {
        if (strip == nullptr || strip->thumbnails.empty() || pictures.getHeight() <= 0)
            return;

        /*  PICTURES SIDE BY SIDE, each at the bar's height in the movie's
            shape, each the picture nearest at or before the second its left
            edge stands on - a waveform's columns, a picture wide. */
        const auto& first = strip->thumbnails.front();
        const auto aspect = first.height > 0 ? static_cast<double> (first.width) / first.height : 16.0 / 9.0;
        const auto wide = std::max (8, static_cast<int> (std::lround (pictures.getHeight() * aspect)));

        /*  KEPT INSIDE THE STRIP AND THE MOVIE (the author, 2026-10-09: "The
            thumbnails on a resized time line tend to over flow on the loop/section
            list to the right"): a picture is scaled to cover its slot in the
            movie's shape, so the last one, and any slot narrower than a picture,
            ran on past its edge - over the next, and past the bar onto the range
            table. Each is clipped to its slot, and the strip ends where the
            movie does. */
        /*  THROUGH THE EDIT (namespace draft §55.5, ADV): the bar draws the
            edited timeline, so the strip ends where the edit does and each slot
            shows the file's picture at the second its section maps to. */
        const auto edited = editShown() && ! sectionsShown().empty();
        const auto length = edited ? model::editedLength (sectionsShown()) : strip->duration;
        const auto movieEnd = length > 0.0 && view.span() > 0.0
                                ? pictures.getX() + static_cast<int> (std::ceil (view.xForSeconds (length, pictures.getWidth())))
                                : pictures.getRight();
        const auto stripEnd = std::clamp (movieEnd, pictures.getX(), pictures.getRight());

        juce::Graphics::ScopedSaveState keepInside (g);
        g.reduceClipRegion (pictures.withRight (stripEnd));
        g.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);

        for (auto x = pictures.getX(); x < stripEnd; x += wide)
        {
            auto fileSecond = secondsAt (x);

            if (edited)
            {
                const auto place = model::placeOf (sectionsShown(), fileSecond);

                if (! place.has_value())
                    continue;

                fileSecond = place->fileSecond;
            }

            const auto* thumbnail = strip->thumbnailAt (fileSecond);

            if (thumbnail == nullptr || thumbnail->width <= 0 || thumbnail->height <= 0)
                continue;

            const auto index = static_cast<std::size_t> (thumbnail - strip->thumbnails.data());
            auto& image = stripImages[index];

            if (! image.isValid())
            {
                image = juce::Image (juce::Image::RGB, thumbnail->width, thumbnail->height, false, juce::SoftwareImageType());
                juce::Image::BitmapData pixels (image, juce::Image::BitmapData::writeOnly);

                for (int row = 0; row < thumbnail->height; ++row)
                    for (int column = 0; column < thumbnail->width; ++column)
                    {
                        const auto* rgb = thumbnail->rgb.data() + 3 * (row * thumbnail->width + column);
                        pixels.setPixelColour (column, row, juce::Colour (rgb[0], rgb[1], rgb[2]));
                    }
            }

            const auto slot = juce::Rectangle<int> (x, pictures.getY(), std::min (wide, stripEnd - x), pictures.getHeight());

            juce::Graphics::ScopedSaveState keepToSlot (g);
            g.reduceClipRegion (slot);
            g.drawImage (image, slot.withWidth (wide).toFloat(), juce::RectanglePlacement::fillDestination);
        }
    }

    std::vector<video::strip::Cut> WaveformEditorComponent::cutsShown() const
    {
        std::vector<video::strip::Cut> out;

        if (strip == nullptr)
            return out;

        if (! editShown() || sectionsShown().empty())
            return strip->cuts;

        /*  THROUGH THE EDIT (namespace draft §55.5): each cut of the file laid
            where the section holding it now is - more than once when the
            material is - and the model's rule says where. */
        for (const auto& cut : strip->cuts)
            for (const auto seconds : model::cutsOnTimeline (sectionsShown(), { cut.seconds }))
            {
                auto moved = cut;
                moved.seconds = seconds;
                out.push_back (moved);
            }

        std::sort (out.begin(), out.end(),
                   [] (const video::strip::Cut& a, const video::strip::Cut& b) { return a.seconds < b.seconds; });
        return out;
    }

    void WaveformEditorComponent::paintCuts (juce::Graphics& g, juce::Rectangle<int> bar)
    {
        if (strip == nullptr || ! showCuts.getToggleState() || ! (view.span() > 0.0))
            return;

        /*  A CUT AS A SHAPE, not a colour alone (§4.8): a line down the bar
            and a triangle at its head - solid for a cut, dashed for a dissolve. */
        const auto ink = Look::colour (theme, "ink");

        for (const auto& cut : cutsShown())
        {
            if (cut.seconds < view.from || cut.seconds > view.to)
                continue;

            const auto x = static_cast<float> (bar.getX()) + static_cast<float> (view.xForSeconds (cut.seconds, bar.getWidth()));

            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.drawLine (x + 1.0f, static_cast<float> (bar.getY()), x + 1.0f, static_cast<float> (bar.getBottom()), 1.0f);
            g.setColour (ink);

            if (cut.gradual)
            {
                const float dashes[] { 4.0f, 3.0f };
                g.drawDashedLine (juce::Line<float> (x, static_cast<float> (bar.getY()), x, static_cast<float> (bar.getBottom())),
                                  dashes, 2, 1.0f);
            }
            else
            {
                g.drawLine (x, static_cast<float> (bar.getY()), x, static_cast<float> (bar.getBottom()), 1.0f);
            }

            juce::Path head;
            head.addTriangle (x - 4.0f, static_cast<float> (bar.getY()), x + 4.0f, static_cast<float> (bar.getY()),
                              x, static_cast<float> (bar.getY()) + 6.0f);
            g.fillPath (head);
        }
    }

    void WaveformEditorComponent::paintBar (juce::Graphics& g, juce::Rectangle<int> bar)
    {
        /*  A MOVIE (namespace draft §47, AAI): its pictures along it, its sound
            in a band below them, its cuts marked over both. */
        if (reading.movie && strip != nullptr && ! strip->thumbnails.empty())
        {
            const auto& sound = columns();
            auto pictures = bar;
            const auto band = sound.empty() ? juce::Rectangle<int>() : pictures.removeFromBottom (bar.getHeight() / 4);

            paintStrip (g, pictures);

            if (! band.isEmpty())
            {
                const auto middle = band.getCentreY();
                const auto half = band.getHeight() / 2.0;
                const auto count = static_cast<int> (sound.size());

                g.setColour (juce::Colours::black);
                g.fillRect (band);

                for (auto at = 0; at < count; ++at)
                {
                    const auto& column = sound[static_cast<std::size_t> (at)];
                    g.setColour (juce::Colour::fromHSV (static_cast<float> (column.hue / 360.0),
                                                        static_cast<float> (column.saturation),
                                                        static_cast<float> (0.25 + column.lightness * 0.6), 1.0f));

                    const auto top = middle - juce::roundToInt (column.high * half);
                    const auto bottom = middle - juce::roundToInt (column.low * half);
                    g.fillRect (band.getX() + at, juce::jmin (top, bottom), 1, juce::jmax (1, std::abs (bottom - top)));
                }
            }

            paintCuts (g, bar);
            return;
        }

        const auto& drawn = columns();

        if (drawn.empty())
        {
            /*  A MOVIE'S PICTURE HAS NO WAVEFORM, and is not waiting for one
                (author, 2026-10-07: "make it plain obvious that for files that
                don't have anything to show ... Empty is fine or something that
                doesn't suggest that it's being processed"). Its ranges and its
                ruler are what the panel is for; the bar stays empty. */
            if (reading.cueKind != "media")
                return;

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

        paintMarkers (g, bar);

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

        //  A movie has no level lane: its sound's is on the sound's own line.
        if (reading.movie || bar.getWidth() <= 1 || ! (view.span() > 0.0))
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
        auto area = head.withTrimmedLeft (head.getHeight() * 2 + recWidth() + pickWidth() + 8)
                        .withTrimmedRight (cutsWidth());

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

        /*  THE RENDER OF AN OPEN EDIT says where it is, ahead of the row's
            instructions (namespace draft §55). */
        const auto render = editShown() ? juce::String (model::renderWords (reading.render)) : juce::String();
        //  And the menus (namespace draft §81), said where the hand looks first.
        const auto instructions = (reading.running ? "playing - drag the ruler to move the playhead, right-click for more"
                                                   : "drag the ruler to place the playhead, right-click for more") + lanes;

        g.drawText (render.isNotEmpty() ? render + "  " + juce::String::fromUTF8 ("\xc2\xb7") + "  " + instructions
                                        : instructions,
                    area, juce::Justification::centredLeft, true);
    }


    //==============================================================================
    /*  THE HANDLES ON THE BAR (namespace draft §55.9). */

    const std::vector<model::SectionRow>& WaveformEditorComponent::sectionsShown() const
    {
        if (slidSection != noSection && slideMoved && ! slidePreview.empty())
            return slidePreview;

        if (gripHeld.grip != model::Grip::none && gripMoved && ! gripPreview.empty())
            return gripPreview;

        if (curveTarget.has_value() && ! curvePreview.empty())
            return curvePreview;

        return reading.sections;
    }

    double WaveformEditorComponent::sourceLength() const
    {
        /*  THE FILE'S OWN LENGTH, for an edge pulled past the last section's
            end: the analyser's record, the edit's file while it is open. */
        if (media != nullptr)
        {
            const auto& named = reading.editSource.empty() ? reading.file : reading.editSource;

            if (const auto found = media->find (named); found != media->end())
                return found->second.seconds;
        }

        return 0.0;
    }

    juce::Point<float> WaveformEditorComponent::gripPosition (model::Grip grip, std::size_t index) const
    {
        const auto bar = barArea();
        const auto& sections = sectionsShown();

        if (index >= sections.size() || bar.isEmpty())
            return {};

        const auto starts = model::sectionStarts (sections);
        const auto band = static_cast<float> (model::gripBand (bar.getHeight()));
        const auto xOf = [this, &bar] (double seconds)
        {
            return static_cast<float> (bar.getX()) + static_cast<float> (view.xForSeconds (seconds, bar.getWidth()));
        };

        switch (grip)
        {
            case model::Grip::volume:
                return { xOf (starts[index] + sections[index].length() / 2.0),
                         static_cast<float> (bar.getBottom())
                           - static_cast<float> (model::laneHeightFor (sections[index].trimDb) * bar.getHeight()) };
            case model::Grip::fadeIn:
                return { xOf (model::fadeHandleSeconds (sections, index, true)), static_cast<float> (bar.getY()) + band / 2.0f };
            case model::Grip::fadeOut:
                return { xOf (model::fadeHandleSeconds (sections, index, false)), static_cast<float> (bar.getY()) + band / 2.0f };
            case model::Grip::edgeIn:
                return { xOf (starts[index]), static_cast<float> (bar.getBottom()) - band / 2.0f };
            case model::Grip::edgeOut:
                return { xOf (starts[index] + sections[index].length()), static_cast<float> (bar.getBottom()) - band / 2.0f };
            case model::Grip::none:
                break;
        }

        return {};
    }

    void WaveformEditorComponent::paintGrips (juce::Graphics& g, juce::Rectangle<int> bar)
    {
        const auto& sections = sectionsShown();

        if (sections.empty() || bar.getWidth() <= 1 || ! (view.span() > 0.0))
        {
            /*  NO EDIT YET: a selection may still be dragged out, to split. */
            if (selection.has_value())
            {
                const auto x0 = static_cast<float> (bar.getX()) + static_cast<float> (view.xForSeconds (selection->first, bar.getWidth()));
                const auto x1 = static_cast<float> (bar.getX()) + static_cast<float> (view.xForSeconds (selection->second, bar.getWidth()));
                g.setColour (Look::colour (theme, "picked").withAlpha (0.18f));
                g.fillRect (juce::Rectangle<float> (x0, static_cast<float> (bar.getY()), std::max (1.0f, x1 - x0),
                                                    static_cast<float> (bar.getHeight())));
                g.setColour (Look::colour (theme, "picked"));
                g.fillRect (juce::Rectangle<float> (x0, static_cast<float> (bar.getY()), 1.0f, static_cast<float> (bar.getHeight())));
                g.fillRect (juce::Rectangle<float> (x1, static_cast<float> (bar.getY()), 1.0f, static_cast<float> (bar.getHeight())));
            }

            return;
        }

        const auto starts = model::sectionStarts (sections);
        const auto heard = model::heardFades (sections);
        const auto stored = model::clampFades (sections);
        const auto top = static_cast<float> (bar.getY());
        const auto bottom = static_cast<float> (bar.getBottom());
        const auto height = bottom - top;
        const auto xOf = [this, &bar] (double seconds)
        {
            return static_cast<float> (bar.getX()) + static_cast<float> (view.xForSeconds (seconds, bar.getWidth()));
        };
        const auto live = reading.editable;
        const auto ink = Look::colour (theme, live ? "ink" : "ink-off");
        const auto accent = Look::colour (theme, live ? "standby" : "ink-off");

        juce::Graphics::ScopedSaveState keepInside (g);
        g.reduceClipRegion (bar);

        //  SILENCE: the gaps shaded, as no sound is there (55.9).
        {
            auto end = 0.0;

            for (std::size_t k = 0; k < sections.size(); ++k)
            {
                if (starts[k] > end + 1.0e-6)
                {
                    g.setColour (juce::Colours::black.withAlpha (0.35f));
                    g.fillRect (juce::Rectangle<float> (xOf (end), top, xOf (starts[k]) - xOf (end), height));
                }

                end = starts[k] + sections[k].length();
            }
        }

        //  THE SELECTION, over the bar's whole height, its two ends marked.
        if (selection.has_value())
        {
            const auto x0 = xOf (selection->first);
            const auto x1 = xOf (selection->second);
            g.setColour (Look::colour (theme, "picked").withAlpha (0.18f));
            g.fillRect (juce::Rectangle<float> (x0, top, std::max (1.0f, x1 - x0), height));
            g.setColour (Look::colour (theme, "picked"));
            g.fillRect (juce::Rectangle<float> (x0, top, 1.0f, height));
            g.fillRect (juce::Rectangle<float> (x1, top, 1.0f, height));
        }

        const auto band = static_cast<float> (model::gripBand (bar.getHeight()));
        const auto square = [&g] (juce::Point<float> at, float size, bool filled)
        {
            const auto box = juce::Rectangle<float> (size, size).withCentre (at);

            if (filled)
                g.fillRect (box);
            else
                g.drawRect (box, 1.0f);
        };

        for (std::size_t k = 0; k < sections.size(); ++k)
        {
            const auto left = xOf (starts[k]);
            const auto right = xOf (starts[k] + sections[k].length());

            if (right < static_cast<float> (bar.getX()) || left > static_cast<float> (bar.getRight()))
                continue;

            //  THE SECTION'S EDGES, and the picked one outlined twice (a shape, §4.8).
            g.setColour (ink.withAlpha (0.5f));
            g.fillRect (juce::Rectangle<float> (left, top, 1.0f, height));
            g.fillRect (juce::Rectangle<float> (right - 1.0f, top, 1.0f, height));

            if (k == pickedSection)
            {
                g.setColour (Look::colour (theme, "picked"));
                g.drawRect (juce::Rectangle<float> (left, top, right - left, height), 2.0f);
                g.drawRect (juce::Rectangle<float> (left + 4.0f, top + 4.0f, std::max (0.0f, right - left - 8.0f), height - 8.0f), 1.0f);
            }

            /*  THE FADES DRAWN AS THEIR CURVES, gain from the foot to the top:
                a sound's equal power, a picture's straight, bent by the curve
                (AEB); a join still one in the file dotted, as it plays plain. */
            for (const auto inSide : { true, false })
            {
                const auto length = inSide ? stored[k].fadeIn : stored[k].fadeOut;
                const auto centred = inSide ? heard[k].inCentred : heard[k].outCentred;
                const auto plain = inSide ? heard[k].plainIn : heard[k].plainOut;
                const auto curve = inSide ? heard[k].inCurve : heard[k].outCurve;

                if (! (length > 0.0))
                    continue;

                const auto edge = inSide ? starts[k] : starts[k] + sections[k].length();
                const auto from = inSide ? (centred ? edge - length / 2.0 : edge) : (centred ? edge - length / 2.0 : edge - length);
                juce::Path shape;

                for (int step = 0; step <= 24; ++step)
                {
                    const auto progress = static_cast<double> (step) / 24.0;
                    const auto gain = model::fadeGain (inSide ? progress : 1.0 - progress, curve, reading.movie);
                    const auto at = juce::Point<float> (xOf (from + progress * length),
                                                        bottom - static_cast<float> (gain) * height);

                    if (step == 0)
                        shape.startNewSubPath (at);
                    else
                        shape.lineTo (at);
                }

                g.setColour (accent.withAlpha (plain ? 0.35f : 0.9f));

                if (plain)
                {
                    const float dashes[] { 3.0f, 3.0f };
                    juce::Path dashed;
                    juce::PathStrokeType (1.0f).createDashedStroke (dashed, shape, dashes, 2);
                    g.fillPath (dashed);
                }
                else
                {
                    g.strokePath (shape, juce::PathStrokeType (1.5f));
                }
            }

            //  THE HANDLES: the fades' at the top, the edges' at the foot, the volume in the middle.
            const auto hot = [this, k] (model::Grip grip)
            {
                return (gripHover.grip == grip && gripHover.index == k) || (gripHeld.grip == grip && gripHeld.index == k);
            };

            for (const auto grip : { model::Grip::fadeIn, model::Grip::fadeOut })
            {
                g.setColour (hot (grip) ? Look::colour (theme, "picked") : accent);
                square (gripPosition (grip, k), hot (grip) ? 9.0f : 7.0f, true);
            }

            if (! model::isJoin (sections, k) || k == 0)
            {
                g.setColour (hot (model::Grip::edgeIn) ? Look::colour (theme, "picked") : ink);
                square (gripPosition (model::Grip::edgeIn, k), hot (model::Grip::edgeIn) ? 9.0f : 7.0f, true);
            }
            else
            {
                //  AT A JOIN THE TWO EDGES ARE ONE: the cut, rolled (AEC).
                g.setColour (hot (model::Grip::edgeIn) ? Look::colour (theme, "picked") : ink);
                const auto at = gripPosition (model::Grip::edgeIn, k);
                juce::Path diamond;
                diamond.addQuadrilateral (at.x - 5.0f, at.y, at.x, at.y - 5.0f, at.x + 5.0f, at.y, at.x, at.y + 5.0f);
                g.fillPath (diamond);
            }

            if (k + 1 >= sections.size() || ! model::isJoin (sections, k + 1))
            {
                g.setColour (hot (model::Grip::edgeOut) ? Look::colour (theme, "picked") : ink);
                square (gripPosition (model::Grip::edgeOut, k), hot (model::Grip::edgeOut) ? 9.0f : 7.0f, true);
            }

            if (reading.cueKind == "media" || ! reading.soundFile.empty())
            {
                const auto at = gripPosition (model::Grip::volume, k);
                g.setColour (hot (model::Grip::volume) ? Look::colour (theme, "picked") : ink);
                square (at, hot (model::Grip::volume) ? 10.0f : 8.0f, false);
                g.drawHorizontalLine (juce::roundToInt (at.y), at.x - 10.0f, at.x + 10.0f);
            }
        }

        juce::ignoreUnused (band);
    }

    void WaveformEditorComponent::timerCallback()
    {
        stopTimer();
        flushCurve();
    }

    void WaveformEditorComponent::flushCurve()
    {
        stopTimer();

        if (curveTarget.has_value() && curveTarget->first < reading.sections.size() && actions.curveSection != nullptr)
        {
            const auto& section = reading.sections[curveTarget->first];
            const auto now = curveTarget->second ? section.fadeInCurve : section.fadeOutCurve;

            if (std::abs (now - curveHeld) >= 0.005)
                actions.curveSection (section.id, curveTarget->second, curveHeld, curveAlone);
        }

        curveTarget.reset();
        curvePreview.clear();
        tell ({});
        repaint();
    }

    void WaveformEditorComponent::splitHere()
    {
        if (! reading.editable || actions.splitSection == nullptr)
            return refuseEdit();

        if (reading.sections.empty() && ! (reading.fileLength > 0.0))
            return tell ("the file's length is not known yet");

        /*  On a cut, at the top or at the end there is nothing to divide, and in
            silence nothing at all: the engine would refuse, and the word is
            better here. */
        const auto at = headSeconds();
        auto onACut = at <= 0.001 || at >= reading.fileLength - 0.001;

        for (const auto start : model::sectionStarts (reading.sections))
            onACut = onACut || std::abs (at - start) <= 0.001;

        if (onACut)
            return tell ("there is already a cut here");

        if (! reading.sections.empty() && ! model::sectionAt (reading.sections, at).has_value())
            return tell ("the playhead is over silence - nothing to split there");

        actions.splitSection (reading.subject.objectId, at);
    }

    bool WaveformEditorComponent::keyPressed (const juce::KeyPress& key)
    {
        if (! editShown())
            return false;

        const auto mods = key.getModifiers();

        //  THE MENUS' KEYS - Ctrl/Cmd with anything - are the window's.
        if (mods.isCommandDown() || mods.isCtrlDown() || mods.isAltDown())
            return false;

        const auto code = key.getKeyCode();

        /*  x SPLITS (namespace draft §55.9): at both ends of the selection in one
            step, else at the playhead. */
        if (code == 'x' || code == 'X')
        {
            if (selection.has_value())
            {
                if (! reading.editable || actions.splitSpan == nullptr)
                    refuseEdit();
                else
                    actions.splitSpan (reading.subject.objectId, selection->first, selection->second);
            }
            else
            {
                splitHere();
            }

            return true;
        }

        /*  BACKSPACE DELETES (AEE): the selection if there is one, else the
            picked section - leaving silence, or closing up with Shift. */
        if (code == juce::KeyPress::backspaceKey || code == juce::KeyPress::deleteKey)
        {
            const auto ripple = mods.isShiftDown();

            if (! reading.editable)
            {
                refuseEdit();
                return true;
            }

            if (selection.has_value())
            {
                if (actions.deleteSpan != nullptr)
                    actions.deleteSpan (reading.subject.objectId, selection->first, selection->second, ripple);

                selection.reset();
                repaint();
                return true;
            }

            if (pickedSection < reading.sections.size() && actions.removeSection != nullptr)
            {
                actions.removeSection (reading.sections[pickedSection].id, ! ripple);
                pickedSection = noSection;
                showPickedSection();
                sayEdit();
                return true;
            }

            tell ("pick a section in the lower half, or drag a selection in the top half, to delete it");
            return true;
        }

        return false;
    }

    //==============================================================================
    /*  THE SECTIONS ROW (namespace draft §55). */

    bool WaveformEditorComponent::editShown() const
    {
        //  A sound's, and a movie's (§55.5) - every movie, greyed with its words when it is not HAP.
        return (reading.cueKind == "media" || reading.movie) && reading.notice.empty();
    }

    juce::Rectangle<int> WaveformEditorComponent::buttonsArea() const
    {
        if (! editShown())
            return {};

        auto area = pictureArea();
        area.removeFromTop (headArea().getHeight());
        return area.removeFromTop (juce::roundToInt (theme.row * theme.type * 0.8));
    }

    juce::Rectangle<int> WaveformEditorComponent::sectionsArea() const
    {
        if (! editShown())
            return {};

        auto area = pictureArea();
        area.removeFromTop (headArea().getHeight() + buttonsArea().getHeight());
        return area.removeFromTop (juce::roundToInt (theme.row * theme.type * 0.8)).reduced (2, 0);
    }

    std::vector<model::SectionLayout> WaveformEditorComponent::sectionLayout() const
    {
        return model::layoutSections (reading.sections, view, barArea().getWidth());
    }

    double WaveformEditorComponent::joinSecondsOf (std::size_t index) const
    {
        const auto starts = model::sectionStarts (reading.sections);
        return index < starts.size() ? starts[index] : 0.0;
    }

    void WaveformEditorComponent::tell (const juce::String& sentence)
    {
        if (actions.say != nullptr)
            actions.say (sentence);
    }

    /*  WHY THE EDIT IS NOT THIS PANEL'S TO MAKE, in the words that say what to
        do about it. */
    void WaveformEditorComponent::refuseEdit()
    {
        if (reading.locked)
            tell ("The show is locked.");
        else if (reading.frozen)
            tell ("This edit is frozen - Unfreeze to change it.");
        else if (! reading.editWords.empty())
            tell (juce::String::fromUTF8 (reading.editWords.c_str()));   // a movie not read yet, or not HAP (§55.5)
        else if (reading.cueKind == "media")
            tell ("This sound follows its movie: edit the movie.");
    }

    /*  WHAT THE ROW'S BUTTONS MAY DO, read off the reading every pass: Split
        while the edit is live, Remove with a section picked, Join when the
        picked one and the next are still one in the file, Freeze once the
        render is there - and Unfreeze in its place while frozen. */
    void WaveformEditorComponent::sayEdit()
    {
        const auto shown = editShown();
        const auto editable = reading.editable;
        const auto picked = pickedSection < reading.sections.size();

        splitButton.setEnabled (shown && editable);
        removeButton.setEnabled (shown && editable && picked);
        joinButton.setEnabled (shown && editable && picked && pickedSection + 1 < reading.sections.size()
                                 && model::continuousJoin (reading.sections[pickedSection], reading.sections[pickedSection + 1]));

        freezeButton.setButtonText (reading.frozen ? "Unfreeze" : "Freeze");
        freezeButton.setTooltip (reading.frozen
                                   ? (reading.movie ? "Unfreezes the edit: the movie and its sound play their files again and the sections are live"
                                                    : "Unfreezes the edit: the cue plays its file again and the sections are live")
                                   : (reading.movie && ! reading.soundFile.empty()
                                        ? "Freezes the edit: the render is bounced to a new file the cue plays, and its sound's with it"
                                        : "Freezes the edit: the render is bounced to a new file the cue plays"));
        freezeButton.setEnabled (shown && ! reading.locked && reading.editWords.empty()
                                   && (reading.frozen || (! reading.sections.empty() && reading.render.state == "done")));

        if (splitButton.isVisible() != shown)
        {
            for (auto* button : { &splitButton, &removeButton, &joinButton, &freezeButton })
                button->setVisible (shown);

            resized();
        }
    }

    void WaveformEditorComponent::showPickedSection()
    {
        //  A movie's trim is its locked sound's (55.5): no sound, no box.
        const auto visible = editShown() && pickedSection < reading.sections.size()
                               && (reading.cueKind == "media" || ! reading.soundFile.empty());
        trimBox.setVisible (visible);

        if (! visible)
            return;

        trimBox.setEditable (reading.editable, reading.editable, false);

        if (! trimBox.isBeingEdited())
            trimBox.setText (juce::String (model::trimText (reading.sections[pickedSection].trimDb)), juce::dontSendNotification);
    }

    void WaveformEditorComponent::paintSections (juce::Graphics& g, juce::Rectangle<int> row)
    {
        const auto bar = barArea();
        g.setFont (Look::font (theme, 11.0f));

        if (reading.sections.empty())
        {
            /*  NO EDIT: one block, the whole file, and the words that start one -
                or, on a movie that is not HAP or not read yet, the words that
                say what to do first (§55.5, ADX). */
            const auto whole = juce::Rectangle<float> (static_cast<float> (bar.getX()), static_cast<float> (row.getY() + 2),
                                                       static_cast<float> (bar.getWidth()), static_cast<float> (row.getHeight() - 4));
            g.setColour (Look::colour (theme, "rule"));
            g.drawRoundedRectangle (whole, 3.0f, 1.0f);
            g.setColour (Look::colour (theme, "ink-off"));
            g.drawFittedText (reading.editWords.empty() ? juce::String ("the whole file - Split at the playhead to start an edit")
                                                        : juce::String::fromUTF8 (reading.editWords.c_str()),
                              whole.reduced (6.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft, 1);
            return;
        }

        const auto layout = sectionLayout();
        const auto pixelsPerSecond = view.span() > 0.0 ? bar.getWidth() / view.span() : 0.0;

        for (const auto& block : layout)
        {
            const auto& section = reading.sections[block.index];
            const auto left = static_cast<float> (bar.getX()) + static_cast<float> (block.x0);
            const auto right = static_cast<float> (bar.getX()) + static_cast<float> (block.x1);
            const auto rect = juce::Rectangle<float> (left, static_cast<float> (row.getY() + 2),
                                                      right - left, static_cast<float> (row.getHeight() - 4))
                                .getIntersection (row.toFloat());
            const auto picked = block.index == pickedSection;
            const auto dragged = block.index == draggedSection && sectionMoved;

            g.setColour (Look::colour (theme, "panel-in").withAlpha (dragged ? 0.4f : 1.0f));
            g.fillRoundedRectangle (rect, 3.0f);
            g.setColour (Look::colour (theme, picked ? "picked" : "rule"));
            g.drawRoundedRectangle (rect, 3.0f, picked ? 2.0f : 1.0f);

            /*  PICKED IS A SHAPE: a second outline inside the first, and a
                triangle under the number (§4.8). */
            if (picked)
            {
                g.drawRoundedRectangle (rect.reduced (3.0f), 2.0f, 1.0f);
                juce::Path mark;
                mark.addTriangle (rect.getX() + 6.0f, rect.getBottom() - 1.0f,
                                  rect.getX() + 14.0f, rect.getBottom() - 1.0f,
                                  rect.getX() + 10.0f, rect.getBottom() - 6.0f);
                g.fillPath (mark);
            }

            auto label = juce::String::fromUTF8 (model::sectionLabel (section, block.index).c_str());

            if (std::abs (section.trimDb) >= 0.05)
                label += "  " + juce::String (model::trimText (section.trimDb));

            if (reading.frozen)
                label += "  frozen";

            g.setColour (Look::colour (theme, reading.frozen || ! reading.editable ? "ink-off" : "ink"));
            g.drawFittedText (label, rect.reduced (18.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft, 1);

            /*  THE JOIN INTO THIS SECTION: a line down the bar where the cut is,
                dotted while the two are still one in the file and solid once they
                are not. Its fades are the bar's top handles' (55.9). */
            if (block.join)
            {
                const auto& before = reading.sections[block.index - 1];
                const auto x = juce::roundToInt (left);

                if (model::continuousJoin (before, section))
                {
                    g.setColour (Look::colour (theme, "ink-off"));

                    for (auto y = row.getY(); y < row.getBottom(); y += 4)
                        g.fillRect (x, y, 1, 2);
                }
                else
                {
                    g.setColour (Look::colour (theme, "standby"));
                    g.fillRect (x, row.getY(), 1, row.getHeight());
                }
            }
        }

        /*  WHERE A DRAGGED BLOCK WILL LAND: a mark at the place among the
            sections, and the block's ghost under the hand. */
        if (draggedSection != noSection && sectionMoved && draggedSection < reading.sections.size())
        {
            const auto slot = model::dropSlotFor (reading.sections, view, bar.getWidth(), draggedSection,
                                                  static_cast<double> (dragX - bar.getX()));

            if (slot >= 0)
            {
                /*  The mark stands at the left edge of the section that will
                    follow the dropped one, or at the end of the last. */
                std::vector<std::size_t> others;

                for (std::size_t i = 0; i < reading.sections.size(); ++i)
                    if (i != draggedSection)
                        others.push_back (i);

                const auto starts = model::sectionStarts (reading.sections);
                const auto at = static_cast<std::size_t> (slot) < others.size()
                                  ? starts[others[static_cast<std::size_t> (slot)]]
                                  : starts.back() + reading.sections.back().length();
                const auto x = bar.getX() + juce::roundToInt (view.xForSeconds (at, bar.getWidth()));

                g.setColour (Look::colour (theme, "picked"));
                g.fillRect (x - 1, row.getY(), 3, row.getHeight());
            }

            const auto& dragged = reading.sections[draggedSection];
            const auto wide = juce::jmax (24.0f, static_cast<float> (dragged.length() * pixelsPerSecond));
            const auto ghost = juce::Rectangle<float> (static_cast<float> (dragX) - wide / 2.0f, static_cast<float> (row.getY() + 2),
                                                       wide, static_cast<float> (row.getHeight() - 4));
            g.setColour (Look::colour (theme, "picked").withAlpha (0.35f));
            g.fillRoundedRectangle (ghost, 3.0f);
            g.setColour (Look::colour (theme, "picked"));
            g.drawRoundedRectangle (ghost, 3.0f, 1.0f);
        }
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

        /*  A MOVIE'S TWO TICKS AT THE RIGHT, as wide as their words, from
            what the buttons on the left have left over. */
        if (showCuts.isVisible())
            for (auto* toggle : { &snapCuts, &showCuts })
            {
                toggle->setSize (0, head.getHeight());
                toggle->changeWidthToFitText();
                toggle->setBounds (head.removeFromRight (juce::jmin (toggle->getWidth(), head.getWidth())));
            }

        /*  THE SECTIONS' BUTTONS on their own row under the head (namespace
            draft §55): the three verbs at the left, the trim box and Freeze
            at the right; the blocks on the row below, over the bar's seconds. */
        if (auto buttons = buttonsArea(); ! buttons.isEmpty())
        {
            const auto wide = juce::jmax (48, buttons.getHeight() * 2);
            splitButton.setBounds (buttons.removeFromLeft (wide).reduced (2, 1));
            removeButton.setBounds (buttons.removeFromLeft (wide + 14).reduced (2, 1));
            joinButton.setBounds (buttons.removeFromLeft (wide).reduced (2, 1));
            freezeButton.setBounds (buttons.removeFromRight (wide + 22).reduced (2, 1));
            trimBox.setBounds (buttons.removeFromRight (64).reduced (2, 2));
        }

        auto boxes = pointBoxes();
        pointLevel.setBounds (boxes.removeFromRight (64));
        boxes.removeFromRight (6);
        pointAt.setBounds (boxes.removeFromRight (64));

        repaint();
    }

    //==============================================================================
    /*  THE MARKERS (namespace draft §81, AUA): a diamond at the top of the bar,
        a dashed line down it and the name beside - a shape and a word, never a
        colour alone (§4.8). Held by the diamond, which a drag moves (one write
        on release) and a right click gives its menu; the line is for the eye. */
    juce::Rectangle<int> WaveformEditorComponent::flagBand() const
    {
        const auto bar = barArea();
        return bar.withHeight (juce::jmin (12, bar.getHeight()));
    }

    std::vector<double> WaveformEditorComponent::markerSeconds() const
    {
        std::vector<double> out;

        for (const auto& marker : reading.markers)
            out.push_back (marker.id == markerHeld ? markerValue
                           : marker.id == markerSent ? markerSentAt
                                                     : marker.at);

        return out;
    }

    std::string WaveformEditorComponent::markerUnder (juce::Point<int> at) const
    {
        if (! flagBand().expanded (0, 2).contains (at) || ! (view.span() > 0.0))
            return {};

        const auto bar = barArea();
        const auto seconds = markerSeconds();
        std::string nearest;
        auto best = grabRadius + 1;

        for (std::size_t k = 0; k < reading.markers.size() && k < seconds.size(); ++k)
        {
            const auto x = bar.getX() + juce::roundToInt (view.xForSeconds (seconds[k], bar.getWidth()));

            if (const auto off = std::abs (x - at.x); off < best)
            {
                best = off;
                nearest = reading.markers[k].id;
            }
        }

        return nearest;
    }

    std::optional<juce::Point<int>> WaveformEditorComponent::markerFlag (const std::string& markerId) const
    {
        const auto bar = barArea();
        const auto seconds = markerSeconds();

        for (std::size_t k = 0; k < reading.markers.size() && k < seconds.size(); ++k)
            if (reading.markers[k].id == markerId && seconds[k] >= view.from && seconds[k] <= view.to)
                return juce::Point<int> (bar.getX() + juce::roundToInt (view.xForSeconds (seconds[k], bar.getWidth())),
                                         flagBand().getCentreY());

        return std::nullopt;
    }

    void WaveformEditorComponent::paintMarkers (juce::Graphics& g, juce::Rectangle<int> bar)
    {
        if (reading.markers.empty() || ! (view.span() > 0.0))
            return;

        const auto seconds = markerSeconds();
        std::vector<std::pair<double, const model::MarkerRow*>> shown;

        for (std::size_t k = 0; k < reading.markers.size() && k < seconds.size(); ++k)
            shown.emplace_back (seconds[k], &reading.markers[k]);

        std::sort (shown.begin(), shown.end(),
                   [] (const auto& a, const auto& b) { return a.first < b.first; });

        g.setFont (Look::font (theme, 11.0f));
        const auto top = static_cast<float> (bar.getY());
        const auto bottom = static_cast<float> (bar.getBottom());

        for (std::size_t k = 0; k < shown.size(); ++k)
        {
            const auto second = shown[k].first;
            const auto* marker = shown[k].second;

            if (second < view.from || second > view.to)
                continue;

            const auto x = static_cast<float> (bar.getX()) + static_cast<float> (view.xForSeconds (second, bar.getWidth()));
            const auto hot = marker->id == markerHeld || marker->id == markerHover;
            const float dashes[] { 3.0f, 3.0f };

            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.drawLine (x + 1.0f, top, x + 1.0f, bottom, 1.0f);
            g.setColour (Look::colour (theme, hot ? "ink" : "ink-dim"));
            g.drawDashedLine (juce::Line<float> (x, top, x, bottom), dashes, 2, hot ? 1.5f : 1.0f);

            juce::Path diamond;
            const auto middle = top + 5.0f;
            diamond.addQuadrilateral (x, middle - 5.0f, x + 5.0f, middle, x, middle + 5.0f, x - 5.0f, middle);
            g.setColour (juce::Colours::black);
            g.strokePath (diamond, juce::PathStrokeType (2.0f));
            g.setColour (Look::colour (theme, "ink"));
            g.fillPath (diamond);

            /*  ITS NAME, on the line under a range's own, as far as the next
                marker or the end of the bar leaves room for. */
            const auto next = k + 1 < shown.size()
                                ? bar.getX() + juce::roundToInt (view.xForSeconds (shown[k + 1].first, bar.getWidth()))
                                : bar.getRight();
            const auto room = next - juce::roundToInt (x) - 10;

            if (room > 24 && ! marker->name.empty())
            {
                g.setColour (Look::colour (theme, hot ? "ink" : "ink-dim"));
                g.drawText (juce::String::fromUTF8 (marker->name.c_str()),
                            juce::Rectangle<int> (juce::roundToInt (x) + 7, bar.getY() + 12, room, 13),
                            juce::Justification::centredLeft, true);
            }
        }
    }

    /*  THE MARKS A DOUBLE CLICK SELECTS BETWEEN (namespace draft §81, AUB):
        markers, scene changes while they are shown, every range's edges, the
        edit's cuts, where a cue with no ranges starts, and the material's ends. */
    std::vector<double> WaveformEditorComponent::marksNow() const
    {
        std::vector<double> cuts;

        if (strip != nullptr && showCuts.getToggleState())
            for (const auto& cut : cutsShown())
                cuts.push_back (cut.seconds);

        auto markers = reading.markers;
        const auto seconds = markerSeconds();

        for (std::size_t k = 0; k < markers.size() && k < seconds.size(); ++k)
            markers[k].at = seconds[k];

        return model::marksOf (markers, reading.ranges,
                               editShown() ? reading.sections : std::vector<model::SectionRow> {},
                               cuts, reading.startOffset, reading.fileLength);
    }

    bool WaveformEditorComponent::selectIntervalAt (int x, bool extend)
    {
        const auto interval = model::intervalAround (marksNow(), secondsAt (x));

        if (! interval.has_value())
        {
            tell ("there is no mark on either side of here to select between");
            return false;
        }

        selection = extend && selection.has_value() ? model::spanOver (*selection, *interval) : *interval;
        tell ("selection " + clockText (selection->first) + juce::String::fromUTF8 (" \xe2\x80\x93 ")
                + clockText (selection->second) + " - right-click it for its menu, Shift+click to take in the next");
        repaint();
        return true;
    }

    //==============================================================================
    /*  THE RIGHT-CLICK MENUS (the author, 2026-10-18; namespace draft §81). The
        words of the playhead's and the selection's lines are the author's; a
        marker's two are mine. */
    juce::String WaveformEditorComponent::wordsFor (MenuLine line)
    {
        switch (line)
        {
            case MenuLine::setToStart:     return "Set to start";
            case MenuLine::setToEnd:       return "Set to end";
            case MenuLine::addMarker:      return "Add marker";
            case MenuLine::addSlice:       return "Add new slice";
            case MenuLine::split:          return "Split";
            case MenuLine::setStartAndEnd: return "Set start and end";
            case MenuLine::makeSlices:     return juce::String::fromUTF8 ("Make new slices\xe2\x80\xa6");
            case MenuLine::trim:           return "Trim";
            case MenuLine::remove:         return "Remove";
            case MenuLine::renameMarker:   return juce::String::fromUTF8 ("Rename marker\xe2\x80\xa6");
            case MenuLine::removeMarker:   return "Remove marker";
        }

        return {};
    }

    std::vector<WaveformEditorComponent::MenuLine> WaveformEditorComponent::menuFor (juce::Point<int> at)
    {
        if (! reading.notice.empty() || reading.subject.objectId.empty())
            return {};

        menuMarker = markerUnder (at);

        if (! menuMarker.empty())
            return { MenuLine::renameMarker, MenuLine::removeMarker };

        const auto seconds = secondsAt (at.x);

        if (selection.has_value() && seconds >= selection->first && seconds <= selection->second)
            return { MenuLine::setStartAndEnd, MenuLine::makeSlices, MenuLine::trim, MenuLine::remove };

        /*  THE PLAYHEAD COMES TO THE CLICK (ATV), as a left click there would
            bring it - and lets a selection it is outside go, as that click
            does - unless it is already under the pointer. */
        const auto bar = barArea();
        const auto headX = bar.getX() + juce::roundToInt (view.xForSeconds (headSeconds(), bar.getWidth()));

        if (std::abs (headX - at.x) > grabRadius)
        {
            selection.reset();
            sentSeek = -1.0;
            moveHeadTo (at.x, true, true);
        }

        return { MenuLine::setToStart, MenuLine::setToEnd, MenuLine::addMarker, MenuLine::addSlice, MenuLine::split };
    }

    void WaveformEditorComponent::openMenuAt (juce::Point<int> at)
    {
        const auto lines = menuFor (at);

        if (lines.empty())
            return;

        juce::PopupMenu menu;

        for (const auto line : lines)
            menu.addItem (static_cast<int> (line) + 1, wordsFor (line));

        menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition(),
                            [safe = juce::Component::SafePointer<WaveformEditorComponent> (this)] (int chosen)
                            {
                                if (safe != nullptr && chosen > 0)
                                    safe->choose (static_cast<MenuLine> (chosen - 1));
                            });
    }

    bool WaveformEditorComponent::rangesRefused()
    {
        if (! reading.locked)
            return false;

        tell ("The show is locked.");
        return true;
    }

    void WaveformEditorComponent::choose (MenuLine line)
    {
        const auto cueId = reading.subject.objectId;

        if (cueId.empty())
            return;

        const auto head = headSeconds();
        const auto noSelection = [this] { tell ("drag a selection in the top half, or double-click between two marks, first"); };

        switch (line)
        {
            /*  WHERE THE CUE STARTS AND ENDS (ATW): cut to the point with ranges
                (ATT), the offset and the first range without. */
            case MenuLine::setToStart:
            case MenuLine::setToEnd:
            case MenuLine::setStartAndEnd:
            {
                if (rangesRefused())
                    return;

                std::optional<double> start, end;

                if (line == MenuLine::setStartAndEnd)
                {
                    if (! selection.has_value())
                        return noSelection();

                    start = selection->first;
                    end = selection->second;
                }
                else if (line == MenuLine::setToStart)
                    start = head;
                else
                    end = head;

                if (const auto why = model::spanRefusal (reading.ranges, reading.startOffset, start, end); ! why.empty())
                    return tell (said (why.c_str()));

                if (actions.spanRanges != nullptr)
                    actions.spanRanges (cueId, start.value_or (-1.0), end.value_or (-1.0));

                tell (start && end ? "the cue plays from " + clockText (*start) + " to " + clockText (*end)
                      : start ? "the cue starts at " + clockText (*start)
                              : "the cue ends at " + clockText (*end));
                return;
            }

            case MenuLine::addMarker:
                if (rangesRefused())
                    return;

                if (actions.createMarker != nullptr)
                    actions.createMarker (cueId, head, model::nextMarkerName (reading.markers));

                tell ("a marker at " + clockText (head) + " - drag its diamond to move it, right-click it to rename it");
                return;

            case MenuLine::addSlice:
                addSliceHere();
                return;

            case MenuLine::split:
                splitHere();
                return;

            case MenuLine::makeSlices:
                if (! selection.has_value())
                    return noSelection();

                if (rangesRefused())
                    return;

                askSliceCount();
                return;

            /*  TRIM: the edit cut at both ends of the selection, and the piece
                between picked once the cut comes back, its trim box ready. */
            case MenuLine::trim:
                if (! selection.has_value())
                    return noSelection();

                if (! reading.editable || actions.splitSpan == nullptr)
                    return refuseEdit();

                pickWhenCut = selection->first;
                pickWaited = 0;
                actions.splitSpan (cueId, selection->first, selection->second);
                return;

            /*  REMOVE CLOSES THE GAP (the author's pick, ATU): what follows
                moves earlier, as Shift+Backspace does. */
            case MenuLine::remove:
                if (! selection.has_value())
                    return noSelection();

                if (! reading.editable || actions.deleteSpan == nullptr)
                    return refuseEdit();

                actions.deleteSpan (cueId, selection->first, selection->second, true);
                selection.reset();
                repaint();
                return;

            case MenuLine::renameMarker:
                if (rangesRefused())
                    return;

                askMarkerName();
                return;

            case MenuLine::removeMarker:
                if (rangesRefused())
                    return;

                if (! menuMarker.empty() && actions.removeMarker != nullptr)
                    actions.removeMarker (menuMarker);

                menuMarker.clear();
                return;
        }
    }

    /*  ADD NEW SLICE: the table's +, at the playhead (ATX) - and on a cue with
        no ranges, the whole of it cut there into two slices rather than one
        range over the file. */
    void WaveformEditorComponent::addSliceHere()
    {
        if (rangesRefused())
            return;

        const auto at = headSeconds();
        const auto& cueId = reading.subject.objectId;

        if (reading.ranges.empty())
        {
            if (! (reading.fileLength > 0.0))
                return tell ("the length of this file is not known yet");

            if (! (at > reading.startOffset + 0.001) || ! (at < reading.fileLength - 0.001))
                return tell ("the playhead is where the cue starts or ends - move it into the sound to cut there");

            if (actions.splitWhole != nullptr)
                actions.splitWhole (cueId, at, reading.fileLength);

            return tell ("two slices, cut at " + clockText (at));
        }

        const auto wanted = model::addAt (reading.ranges, at, reading.fileLength);

        switch (wanted.kind)
        {
            case model::RangeAdd::Kind::split:
                if (actions.splitRange != nullptr)
                    actions.splitRange (cueId, wanted.at);

                tell ("a new slice from " + clockText (wanted.at));
                break;

            case model::RangeAdd::Kind::create:
                if (actions.createRange != nullptr)
                    actions.createRange (cueId, wanted.in, wanted.out);

                tell ("a new slice from " + clockText (wanted.in) + " to " + clockText (wanted.out));
                break;

            case model::RangeAdd::Kind::nothing:
                tell (said (wanted.why.c_str()));
                break;
        }
    }

    /*  MAKE NEW SLICES asks how many (ATY), saying how many ranges this show
        has room for on one cue before it must be reopened. */
    void WaveformEditorComponent::askSliceCount()
    {
        if (! selection.has_value())
            return;

        const auto span = *selection;
        auto words = "Cut " + clockText (span.first) + juce::String::fromUTF8 (" \xe2\x80\x93 ")
                       + clockText (span.second) + " into equal slices.";

        if (reading.launcherSlots > 0)
            words << "\n\nThis show was opened with room for " << reading.launcherSlots
                  << " slices on one cue; this one has " << static_cast<int> (reading.ranges.size())
                  << ". More will play once the show is reopened.";

        auto* ask = new juce::AlertWindow ("Make new slices", words, juce::MessageBoxIconType::NoIcon, this);
        ask->addTextEditor ("count", "2", "How many");
        ask->addButton ("Make", 1, juce::KeyPress (juce::KeyPress::returnKey));
        ask->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        ask->enterModalState (true, juce::ModalCallbackFunction::create (
            [safe = juce::Component::SafePointer<WaveformEditorComponent> (this), ask, span] (int result)
            {
                if (safe == nullptr || result != 1)
                    return;

                const auto typed = ask->getTextEditorContents ("count").trim();
                safe->selection = span;
                safe->makeSlices (typed.isNotEmpty() && typed.containsOnly ("0123456789") && typed.length() < 4
                                    ? typed.getIntValue() : 0);
            }), true);
    }

    void WaveformEditorComponent::makeSlices (int count)
    {
        if (! selection.has_value())
            return tell ("drag a selection in the top half, or double-click between two marks, first");

        if (rangesRefused())
            return;

        const auto from = selection->first;
        const auto to = selection->second;

        if (count < 1 || count > 64)
            return tell ("type a number of slices from 1 to 64");

        if ((to - from) / count < 0.01)
            return tell ("slices that many would each be under a hundredth of a second");

        if (reading.ranges.empty() && ! (reading.fileLength > 0.0))
            return tell ("the length of this file is not known yet");

        if (actions.sliceSpan != nullptr)
            actions.sliceSpan (reading.subject.objectId, from, to, count, reading.fileLength);

        const auto after = model::rangesAfterSlicing (reading.ranges, from, to, count);

        if (reading.launcherSlots > 0 && after > reading.launcherSlots)
            return tell (juce::String (after) + " slices on this cue, more than the " + juce::String (reading.launcherSlots)
                           + " this show was opened with room for - reopen the show to play them all");

        tell (juce::String (count) + (count == 1 ? " slice" : " slices") + " from " + clockText (from) + " to "
                + clockText (to));
    }

    void WaveformEditorComponent::askMarkerName()
    {
        const auto markerId = menuMarker;
        const auto found = std::find_if (reading.markers.begin(), reading.markers.end(),
                                         [&markerId] (const model::MarkerRow& marker) { return marker.id == markerId; });

        if (found == reading.markers.end())
            return;

        auto* ask = new juce::AlertWindow ("Rename the marker", {}, juce::MessageBoxIconType::NoIcon, this);
        ask->addTextEditor ("name", juce::String::fromUTF8 (found->name.c_str()), "Name");
        ask->addButton ("Rename", 1, juce::KeyPress (juce::KeyPress::returnKey));
        ask->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        ask->enterModalState (true, juce::ModalCallbackFunction::create (
            [safe = juce::Component::SafePointer<WaveformEditorComponent> (this), ask, markerId] (int result)
            {
                if (safe == nullptr || result != 1)
                    return;

                safe->menuMarker = markerId;
                safe->renameMarker (ask->getTextEditorContents ("name").trim().toStdString());
            }), true);
    }

    void WaveformEditorComponent::renameMarker (const std::string& name)
    {
        if (menuMarker.empty() || name.empty() || rangesRefused())
            return;

        if (actions.set != nullptr)
            actions.set ("/godot/marker/" + menuMarker + "/name", name);
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

        /*  A MARKER'S DIAMOND (namespace draft §81), at the very top of the bar:
            dragged sideways, right-clicked for its menu. */
        if (const auto underFlag = markerUnder (event.getPosition()); underFlag != markerHover)
        {
            markerHover = underFlag;
            repaint();
        }

        if (! markerHover.empty())
        {
            setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
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

        /*  A SECTION'S HANDLE (namespace draft §55.9): its cursor says which
            way it goes. */
        if (editShown() && barArea().contains (event.getPosition()))
        {
            const auto bar = barArea();
            const auto found = model::hitGrip (sectionsShown(), view, bar.getWidth(), bar.getHeight(),
                                               event.x - bar.getX(), event.y - bar.getY(), grabRadius,
                                               reading.cueKind == "media" || ! reading.soundFile.empty());

            if (! (found == gripHover))
            {
                gripHover = found;
                repaint();
            }

            if (gripHover.grip != model::Grip::none)
            {
                setMouseCursor (gripHover.grip == model::Grip::volume ? juce::MouseCursor::UpDownResizeCursor
                                                                     : juce::MouseCursor::LeftRightResizeCursor);
                return;
            }
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
        rightPress = false;
        pressTookSelection = false;

        /*  A RIGHT PRESS (namespace draft §81, AUC): on the bar it pans while it
            drags, as it always did; let go where it was pressed, it opens the
            menu of what is under it - over the ruler and the row above too. */
        if (event.mods.isPopupMenu())
        {
            const auto where = event.getPosition();

            if (rulerArea().contains (where) || barArea().contains (where) || sectionsArea().contains (where))
            {
                rightPress = true;
                rightPressX = event.x;
                panning = barArea().contains (where);
                panFrom = event.x;
            }

            return;
        }

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
            moveHeadTo (event.x, false, ! event.mods.isAltDown());
            return;
        }

        /*  THE SECTIONS ROW (namespace draft §55): a press picks the block under
            it, and a drag selects time, as the top half of the bar does (the
            author, 2026-10-17: "The time strip above the waveform and top half
            of the waveform should be for range"). It reordered, with the block
            drawn well away from the hand; the lower half of the bar carries a
            section now, past its neighbours too. */
        if (const auto row = sectionsArea(); ! row.isEmpty() && row.contains (event.getPosition()))
        {
            const auto hit = model::hitSection (sectionLayout(), static_cast<double> (event.x - barArea().getX()),
                                                static_cast<double> (grabRadius));

            if (hit.hit == model::SectionHit::block || hit.hit == model::SectionHit::join)
            {
                pickedSection = hit.index;
                showPickedSection();
                sayEdit();
            }

            /*  SHIFT TAKES IN ANOTHER INTERVAL (namespace draft §81, AUB). */
            if (event.mods.isShiftDown() && event.getNumberOfClicks() == 1)
            {
                pressTookSelection = selectIntervalAt (event.x, true);
                return;
            }

            selecting = true;
            selectingFromRow = true;
            selectPressX = event.x;
            selectFrom = secondsAt (event.x);

            if (! event.mods.isAltDown())
            {
                std::vector<double> targets { headSeconds() };

                for (const auto start : model::sectionStarts (reading.sections))
                    targets.push_back (start);

                for (const auto marked : markerSeconds())
                    targets.push_back (marked);

                selectFrom = model::snapTo (selectFrom, targets, toleranceSeconds());
            }

            repaint();
            return;
        }

        if (! barArea().contains (event.getPosition()))
            return;

        if (event.mods.isMiddleButtonDown())
        {
            panning = true;
            panFrom = event.x;
            return;
        }

        /*  A MARKER, held by its diamond (namespace draft §81). */
        if (const auto underFlag = markerUnder (event.getPosition()); ! underFlag.empty())
        {
            if (rangesRefused())
                return;

            const auto seconds = markerSeconds();

            for (std::size_t k = 0; k < reading.markers.size() && k < seconds.size(); ++k)
                if (reading.markers[k].id == underFlag)
                    markerValue = seconds[k];

            markerHeld = underFlag;
            markerMoved = false;
            repaint();
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

        /*  A SECTION'S HANDLE (namespace draft §55.9, ADY): the volume, a fade's
            length, an edge - held, drawn as the hand moves it, written once on
            release. */
        if (editShown())
        {
            const auto bar = barArea();
            const auto found = model::hitGrip (sectionsShown(), view, bar.getWidth(), bar.getHeight(),
                                               event.x - bar.getX(), event.y - bar.getY(), grabRadius,
                                               reading.cueKind == "media" || ! reading.soundFile.empty());

            if (found.grip != model::Grip::none)
            {
                if (! reading.editable)
                    return refuseEdit();

                gripHeld = found;
                gripPreview = reading.sections;
                gripMoved = false;
                pickedSection = found.index;
                showPickedSection();
                sayEdit();
                repaint();
                return;
            }
        }

        grabbed = model::hitTest (reading.ranges, secondsAt (event.x), toleranceSeconds());

        if (grabbed.handle != model::Handle::none)
            return;

        /*  THE TWO HALVES (namespace draft §55.9, ADZ): the top selects time - a
            press that does not drag puts the playhead there - and the lower
            half picks the section under it, a drag moving it into a new place;
            over silence, or where there is no edit to show, a press pans. */
        if (editShown())
        {
            const auto bar = barArea();

            if (event.y < bar.getCentreY())
            {
                if (event.mods.isShiftDown() && event.getNumberOfClicks() == 1)
                {
                    pressTookSelection = selectIntervalAt (event.x, true);
                    return;
                }

                selecting = true;
                selectingFromRow = false;
                selectPressX = event.x;
                selectFrom = secondsAt (event.x);

                if (! event.mods.isAltDown())
                {
                    std::vector<double> targets { headSeconds() };

                    for (const auto start : model::sectionStarts (reading.sections))
                        targets.push_back (start);

                    for (const auto marked : markerSeconds())
                        targets.push_back (marked);

                    selectFrom = model::snapTo (selectFrom, targets, toleranceSeconds());
                }

                return;
            }

            /*  THE LOWER HALF PICKS AND CARRIES (§55.13; 2026-10-17): a press
                picks the section under it, a drag slides it along the timeline
                into the room beside it, and carried past a neighbour it takes
                that place in the order. */
            if (const auto under = model::sectionAt (reading.sections, secondsAt (event.x)))
            {
                pickedSection = *under;
                slidSection = *under;
                slideFrom = model::sectionStarts (reading.sections)[*under];
                slideValue = 0.0;   // the seconds slid (§82), nought until it moves
                slidePress = secondsAt (event.x);
                slideMoved = false;
                slideSlot = -1;
                slidePreview.clear();
                lowerPress = true;
                pressX = dragX = event.x;
                selection.reset();
                showPickedSection();
                sayEdit();
                repaint();
                return;
            }

            pickedSection = noSection;
            showPickedSection();
            sayEdit();
            repaint();
        }

        /*  A PRESS ON NOTHING PANS, which is the gesture people try first on a
            picture that is wider than its window. Grabbing an edge wins, so a
            hand aiming at a handle never scrolls the view out from under it. */
        panning = true;
        panFrom = event.x;
    }

    void WaveformEditorComponent::mouseDrag (const juce::MouseEvent& event)
    {
        /*  A MARKER CARRIED (namespace draft §81): snapped, unless Alt, to the
            other marks and the playhead; written once, on release. */
        if (! markerHeld.empty())
        {
            const auto end = reading.fileLength > 0.0 ? reading.fileLength : secondsAt (barArea().getRight());
            auto seconds = std::clamp (secondsAt (event.x), 0.0, std::max (0.0, end));

            if (! event.mods.isAltDown())
            {
                auto others = reading.markers;
                others.erase (std::remove_if (others.begin(), others.end(),
                                              [this] (const model::MarkerRow& marker) { return marker.id == markerHeld; }),
                              others.end());

                std::vector<double> cuts;

                if (cutsSnap())
                    for (const auto& cut : cutsShown())
                        cuts.push_back (cut.seconds);

                auto targets = model::marksOf (others, reading.ranges,
                                               editShown() ? reading.sections : std::vector<model::SectionRow> {},
                                               cuts, reading.startOffset, reading.fileLength);
                targets.push_back (headSeconds());
                seconds = model::snapTo (seconds, targets, toleranceSeconds());
            }

            markerValue = seconds;
            markerMoved = true;
            tell ("marker at " + clockText (seconds) + " - Alt lets it go of the marks");
            repaint();
            return;
        }

        /*  A HANDLE HELD (namespace draft §55.9): the edit as the drag would
            leave it, drawn, and said; Shift moves a fade alone (AED). */
        if (gripHeld.grip != model::Grip::none && gripHeld.index < reading.sections.size())
        {
            const auto index = gripHeld.index;
            const auto seconds = secondsAt (event.x);
            const auto name = juce::String (static_cast<int> (index) + 1);
            gripMoved = true;

            switch (gripHeld.grip)
            {
                case model::Grip::volume:
                {
                    auto level = std::clamp (model::laneLevelForHeight (heightAt (event.y)), -120.0, 12.0);

                    if (! event.mods.isAltDown()
                          && std::abs (heightAt (event.y) - model::laneHeightFor (0.0)) < toleranceHeight() * 0.5)
                        level = 0.0;

                    gripValue = std::round (level * 10.0) / 10.0;
                    gripPreview = reading.sections;
                    gripPreview[index].trimDb = gripValue;
                    tell ("section " + name + " at " + juce::String (model::trimText (gripValue)));
                    break;
                }

                case model::Grip::fadeIn:
                case model::Grip::fadeOut:
                {
                    const auto inSide = gripHeld.grip == model::Grip::fadeIn;
                    gripAlone = event.mods.isShiftDown();
                    gripValue = model::fadeFromHandle (reading.sections, index, inSide, seconds);
                    gripPreview = model::withFade (reading.sections, index, inSide, gripValue, gripAlone);
                    gripValue = inSide ? gripPreview[index].fadeIn : gripPreview[index].fadeOut;
                    tell (juce::String (inSide ? "fade in " : "fade out ")
                            + juce::String (juce::roundToInt (gripValue * 1000.0)) + " ms on section " + name
                            + (gripAlone ? " alone" : " - Shift moves this side alone"));
                    break;
                }

                case model::Grip::edgeIn:
                case model::Grip::edgeOut:
                {
                    const auto inSide = gripHeld.grip == model::Grip::edgeIn;
                    gripValue = model::edgeFromHandle (reading.sections, index, inSide, seconds, sourceLength());
                    gripPreview = model::withEdge (reading.sections, index, inSide, gripValue);
                    tell (juce::String (inSide ? "section " + name + " begins at " : "section " + name + " ends at ")
                            + clockText (gripValue) + " of the file");
                    break;
                }

                case model::Grip::none:
                    break;
            }

            repaint();
            return;
        }

        /*  A SECTION SLID (§55.13): where it would begin, snapped by its start
            or its end to the other edges, the playhead and a movie's cuts unless
            Alt, held between its neighbours - and the edit so, drawn. */
        if (slidSection != noSection && slidSection < reading.sections.size())
        {
            if (std::abs (event.x - pressX) <= 3 && ! slideMoved)
                return;

            slideMoved = true;
            const auto name = juce::String (static_cast<int> (slidSection) + 1);

            /*  WITH CTRL/⌘, ANOTHER PLACE IN THE ORDER (the author, 2026-10-18;
                namespace draft §82, AUE): past a neighbour's middle it takes that
                place, as a plain drag did the evening before. */
            if (event.mods.isCommandDown())
            {
                const auto bar = barArea();
                slideSlot = model::dropSlotFor (reading.sections, view, bar.getWidth(), slidSection,
                                                static_cast<double> (event.x - bar.getX()));
                slideValue = 0.0;
                slidePreview = slideSlot >= 0 ? model::withOrder (reading.sections, slidSection, slideSlot)
                                              : reading.sections;
                tell (slideSlot >= 0 ? "section " + name + " to place " + juce::String (slideSlot + 1) + " of "
                                         + juce::String (static_cast<int> (reading.sections.size()))
                                     : "section " + name + " - past a neighbour's middle it takes that place");
                repaint();
                return;
            }

            /*  A PLAIN DRAG SLIDES IT, EVERYTHING AFTER IT WITH IT (the author,
                2026-10-18: "When moving an object left ... it should slide beneath
                the previous one, shortening the whole clip ... the left segment
                pushes back the right following segments"; namespace draft §82,
                AUD): to the right silence opens before it; to the left the silence
                closes, then it goes beneath the one before, which stays on top.
                Snapped by its start or its end to what stays where it is - the
                sections before it, the playhead, a movie's cuts, the markers -
                unless Alt. */
            slideSlot = -1;
            const auto length = reading.sections[slidSection].length();
            auto start = slideFrom + (secondsAt (event.x) - slidePress);

            if (! event.mods.isAltDown())
            {
                std::vector<double> targets { headSeconds() };
                const auto starts = model::sectionStarts (reading.sections);

                for (std::size_t k = 0; k < slidSection && k < starts.size(); ++k)
                {
                    targets.push_back (starts[k]);
                    targets.push_back (starts[k] + reading.sections[k].length());
                }

                if (cutsSnap())
                    for (const auto& cut : cutsShown())
                        targets.push_back (cut.seconds);

                for (const auto marked : markerSeconds())
                    targets.push_back (marked);

                const auto byStart = model::snapTo (start, targets, toleranceSeconds());
                const auto byEnd = model::snapTo (start + length, targets, toleranceSeconds()) - length;

                if (std::abs (byStart - start) > 1.0e-9)
                    start = byStart;
                else if (std::abs (byEnd - start) > 1.0e-9)
                    start = byEnd;
            }

            slideValue = start - slideFrom;
            slidePreview = model::withSlide (reading.sections, slidSection, slideValue);

            const auto& shown = slidePreview[slidSection];
            tell (shown.under > 0.0
                    ? "section " + name + ": " + clockText (shown.under) + " of it beneath section "
                        + juce::String (static_cast<int> (slidSection)) + ", the cue that much shorter"
                    : "section " + name + " from " + clockText (model::sectionStarts (slidePreview)[slidSection])
                        + ", what follows with it - Ctrl/Cmd to reorder");
            repaint();
            return;
        }

        //  THE SELECTION, dragged out across the top half (55.9).
        if (selecting)
        {
            auto now = secondsAt (event.x);

            if (! event.mods.isAltDown())
            {
                std::vector<double> targets { headSeconds() };
                const auto starts = model::sectionStarts (reading.sections);

                for (std::size_t k = 0; k < starts.size(); ++k)
                {
                    targets.push_back (starts[k]);
                    targets.push_back (starts[k] + reading.sections[k].length());
                }

                if (cutsSnap())
                    for (const auto& cut : cutsShown())
                        targets.push_back (cut.seconds);

                for (const auto marked : markerSeconds())
                    targets.push_back (marked);

                now = model::snapTo (now, targets, toleranceSeconds());
            }

            if (std::abs (event.x - selectPressX) > 3)
            {
                selection = std::make_pair (std::min (selectFrom, now), std::max (selectFrom, now));
                tell ("selection " + clockText (selection->first) + juce::String::fromUTF8 (" \xe2\x80\x93 ")
                        + clockText (selection->second) + " - x splits at both ends, Backspace deletes,"
                        " Shift+Backspace closes up");
                repaint();
            }

            return;
        }

        /*  A BLOCK DRAGGED to a new place: a ghost under the hand and a mark
            where it lands; nothing written until release. */
        if (draggedSection != noSection)
        {
            dragX = event.x;

            if (std::abs (dragX - pressX) > 4)
                sectionMoved = true;

            repaint();
            return;
        }

        if (onRuler)
        {
            moveHeadTo (event.x, false, ! event.mods.isAltDown());
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
        {
            auto targets = model::snapTargets (reading.ranges, grabbed.rangeId, reading.fileLength);

            /*  AND A MOVIE'S CUTS (§47, AAI): an in point on a shot's first
                frame - while they are shown and snapped to (§47.10). */
            if (cutsSnap())
                for (const auto& cut : cutsShown())
                    targets.push_back (cut.seconds);

            //  And a marker (namespace draft §81): a slice cut where somebody marked.
            for (const auto marked : markerSeconds())
                targets.push_back (marked);

            seconds = model::snapTo (seconds, targets, toleranceSeconds());
        }

        const auto writes = model::dragTo (grabbed, seconds, reading.ranges, reading.fileLength);

        for (const auto& write : writes)
            actions.set (model::rangeAddress (write.rangeId, write.attribute),
                         osc::formatDouble (write.seconds));

        //  Where the edge is now, for the monitor's tile of a movie (§47, AAH).
        if (! writes.empty())
            edgeHeld = writes.front().seconds;

        if (! writes.empty() && actions.say != nullptr)
            actions.say (juce::String (grabbed.handle == model::Handle::slice
                                         ? "loop point at " : "edge at ")
                           + clockText (writes.front().seconds));
    }

    void WaveformEditorComponent::mouseDoubleClick (const juce::MouseEvent& event)
    {
        /*  ON A MARKER'S DIAMOND, its name (namespace draft §81). */
        if (const auto underFlag = markerUnder (event.getPosition()); ! underFlag.empty())
        {
            markerHeld.clear();
            menuMarker = underFlag;

            if (! rangesRefused())
                askMarkerName();

            return;
        }

        /*  ON THE ROW ABOVE, WHAT LIES BETWEEN TWO MARKS IS SELECTED (the
            author, 2026-10-18; namespace draft §81, AUB) - the row is for time
            now (ATF), and framing a section is a wheel's work. */
        if (const auto row = sectionsArea(); ! row.isEmpty() && row.contains (event.getPosition()))
        {
            selecting = false;
            selectIntervalAt (event.x, event.mods.isShiftDown());
            return;
        }

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

        /*  AND IN THE TOP HALF, the same (AUB): the lower half and the ruler
            keep the way back to the whole file. */
        if (editShown() && barArea().contains (event.getPosition()) && event.y < barArea().getCentreY())
        {
            selecting = false;
            grabbed = {};
            panning = false;
            selectIntervalAt (event.x, event.mods.isShiftDown());
            return;
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
        /*  A RIGHT CLICK THAT DID NOT DRAG opens its menu (namespace draft §81). */
        if (rightPress)
        {
            rightPress = false;
            panning = false;

            if (std::abs (event.x - rightPressX) <= 3)
                openMenuAt (event.getPosition());

            return;
        }

        if (pressTookSelection)
        {
            pressTookSelection = false;
            return;
        }

        //  A MARKER'S ONE WRITE (namespace draft §81), and only if it moved.
        if (! markerHeld.empty())
        {
            const auto id = markerHeld;
            markerHeld.clear();

            if (markerMoved && actions.set != nullptr)
            {
                markerSent = id;
                markerSentAt = markerValue;
                markerSentPasses = 0;
                actions.set ("/godot/marker/" + id + "/at", osc::formatDouble (markerValue));
            }

            markerMoved = false;
            tell ({});
            repaint();
            return;
        }

        /*  A HANDLE'S ONE WRITE (namespace draft §55.9). */
        if (gripHeld.grip != model::Grip::none)
        {
            const auto released = gripHeld;
            gripHeld = {};

            if (gripMoved && released.index < reading.sections.size())
            {
                const auto& section = reading.sections[released.index];

                switch (released.grip)
                {
                    case model::Grip::volume:
                        if (actions.set != nullptr && std::abs (gripValue - section.trimDb) >= 0.05)
                            actions.set ("/godot/section/" + section.id + "/trim", osc::formatDouble (gripValue));
                        break;

                    case model::Grip::fadeIn:
                    case model::Grip::fadeOut:
                    {
                        const auto inSide = released.grip == model::Grip::fadeIn;

                        if (actions.fadeSection != nullptr
                              && std::abs (gripValue - (inSide ? section.fadeIn : section.fadeOut)) >= 0.0005)
                            actions.fadeSection (section.id, inSide, gripValue, gripAlone);
                        break;
                    }

                    case model::Grip::edgeIn:
                    case model::Grip::edgeOut:
                    {
                        const auto inSide = released.grip == model::Grip::edgeIn;

                        if (actions.edgeSection != nullptr
                              && std::abs (gripValue - (inSide ? section.in : section.out)) >= 0.0005)
                            actions.edgeSection (section.id, inSide, gripValue);
                        break;
                    }

                    case model::Grip::none:
                        break;
                }
            }

            gripMoved = false;
            tell ({});
            repaint();
            return;
        }

        //  THE SLIDE'S ONE WRITE (§55.13).
        if (slidSection != noSection)
        {
            const auto index = slidSection;
            slidSection = noSection;

            if (slideMoved && index < reading.sections.size())
            {
                if (! reading.editable)
                    refuseEdit();
                else if (slideSlot >= 0)
                {
                    if (actions.moveSection != nullptr)
                        actions.moveSection (reading.sections[index].id, slideSlot);
                }
                else if (std::abs (slideValue) >= 0.0005 && actions.slideSection != nullptr)
                    actions.slideSection (reading.sections[index].id, slideValue);   // §82: by seconds, what follows with it
            }

            slideMoved = false;
            slideSlot = -1;
            slidePreview.clear();
            lowerPress = false;
            tell ({});
            repaint();
            return;
        }

        /*  A PRESS IN THE TOP HALF THAT DID NOT DRAG puts the playhead there and
            lets the selection go; one that did keeps it (55.9). */
        if (selecting)
        {
            selecting = false;

            //  On the row above, a click picked its block and leaves the playhead be.
            //  A double click's release keeps the interval it selected (§81).
            if (std::abs (event.x - selectPressX) <= 3 && event.getNumberOfClicks() < 2)
            {
                selection.reset();

                if (! selectingFromRow)
                    moveHeadTo (event.x, true, ! event.mods.isAltDown());

                tell ({});
            }

            selectingFromRow = false;
            repaint();
            return;
        }

        /*  THE BLOCK'S ONE MOVE (namespace draft §55), from the row or the lower half. */
        lowerPress = false;
        if (draggedSection != noSection)
        {
            const auto index = draggedSection;
            draggedSection = noSection;

            if (sectionMoved && index < reading.sections.size())
            {
                if (! reading.editable)
                    refuseEdit();
                else if (const auto slot = model::dropSlotFor (reading.sections, view, barArea().getWidth(), index,
                                                               static_cast<double> (event.x - barArea().getX()));
                         slot >= 0 && actions.moveSection != nullptr)
                    actions.moveSection (reading.sections[index].id, slot);
            }

            sectionMoved = false;
            repaint();
            return;
        }

        if (onRuler)
        {
            moveHeadTo (event.x, true, ! event.mods.isAltDown());
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
        edgeHeld.reset();
        panning = false;

        if (actions.say != nullptr)
            actions.say ({});

        repaint();
    }

    void WaveformEditorComponent::mouseExit (const juce::MouseEvent&)
    {
        if (hover.handle != model::Handle::none || hoverPoint != noPoint || ! markerHover.empty())
        {
            hover = {};
            hoverPoint = noPoint;
            markerHover.clear();
            repaint();
        }
    }

    void WaveformEditorComponent::mouseWheelMove (const juce::MouseEvent& event,
                                                  const juce::MouseWheelDetails& wheel)
    {
        if (! (reading.fileLength > 0.0))
            return;

        const auto bar = barArea();

        /*  OVER A FADE THE WHEEL BENDS ITS CURVE (namespace draft §55.9, AEB):
            a tenth a notch, both sides of a join unless Shift, written once the
            wheel has been still a moment - one step to undo, not one a notch. */
        if (editShown() && bar.contains (event.getPosition()) && ! juce::approximatelyEqual (wheel.deltaY, 0.0f)
              && std::abs (wheel.deltaY) >= std::abs (wheel.deltaX))
        {
            const auto slack = toleranceSeconds();

            if (const auto fade = model::fadeAt (reading.sections, secondsAt (event.x), slack))
            {
                if (! reading.editable)
                    return refuseEdit();

                const auto alone = event.mods.isShiftDown();

                if (curveTarget != fade || curveAlone != alone)
                {
                    flushCurve();
                    curveTarget = fade;
                    curveAlone = alone;
                    const auto& section = reading.sections[fade->first];
                    curveHeld = fade->second ? section.fadeInCurve : section.fadeOutCurve;
                }

                const auto step = wheel.isSmooth ? static_cast<double> (wheel.deltaY) : (wheel.deltaY > 0.0f ? 0.1 : -0.1);
                curveHeld = std::clamp (std::round ((curveHeld + step) * 100.0) / 100.0, -1.0, 1.0);
                curvePreview = model::withCurve (reading.sections, fade->first, fade->second, curveHeld, curveAlone);
                tell (juce::String (fade->second ? "fade in " : "fade out ") + "curve "
                        + juce::String (curveHeld, 2) + " on section " + juce::String (static_cast<int> (fade->first) + 1)
                        + (curveAlone ? " alone" : " - Shift turns this side alone"));
                startTimer (400);
                repaint();
                return;
            }
        }

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
