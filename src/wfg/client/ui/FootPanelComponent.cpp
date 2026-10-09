/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#include <wfg/client/ui/FootPanelComponent.h>
#include <wfg/client/ui/Icons.h>

#include <wfg/client/ui/Look.h>

#include <memory>
#include <string>
#include <utility>

namespace wfg::client::ui
{
    FootPanelComponent::FootPanelComponent (const model::Theme& themeToUse, Actions actionsToUse)
        : theme (themeToUse), actions (std::move (actionsToUse))
    {
        addAndMakeVisible (shut);
        shut.setTooltip ("Close the panel");
        shut.onClick = [this] { if (actions.close) actions.close(); };

        addChildComponent (copyButton);
        addChildComponent (pasteButton);
        copyButton.setTooltip ("Copy this part of the cue");
        pasteButton.setTooltip ("Nothing copied to paste here");
        pasteButton.setEnabled (false);

        copyButton.onClick = [this]
        {
            if (const auto part = model::partForPanel (showing.kind); ! part.empty() && actions.copyPart)
                actions.copyPart (part);
        };

        pasteButton.onClick = [this]
        {
            if (const auto part = model::partForPanel (showing.kind); ! part.empty() && actions.pastePart)
                actions.pastePart (part);
        };

        setInterceptsMouseClicks (true, true);
    }

    void FootPanelComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;

        if (waveform != nullptr)
            waveform->applyTheme (theme);

        if (sends != nullptr)
            sends->applyTheme (theme);

        if (timeline != nullptr)
            timeline->applyTheme (theme);

        if (curve != nullptr)
            curve->applyTheme (theme);

        if (eq != nullptr)
            eq->applyTheme (theme);

        if (fadeMixer != nullptr)
            fadeMixer->applyTheme (theme);

        if (picture != nullptr)
            picture->applyTheme (theme);

        if (messages != nullptr)
            messages->applyTheme (theme);

        if (curves != nullptr)
            curves->applyTheme (theme);

        if (fx != nullptr)
            fx->applyTheme (theme);

        if (takePanel != nullptr)
            takePanel->applyTheme (theme);

        for (auto* button : { &copyButton, &pasteButton })
        {
            button->setColours (Look::colour (theme, "ink"), Look::colour (theme, "panel-high"),
                                Look::colour (theme, "ink-faint"));
            button->setTextHeight (Look::font (theme, 12.0f).getHeight());
        }

        resized();
        repaint();
    }

    void FootPanelComponent::setRightColumn (int width, int gap)
    {
        if (columnWidth == width && columnGap == gap)
            return;

        columnWidth = width;
        columnGap = gap;
        resized();
    }

    void FootPanelComponent::open (const model::Subject& wanted)
    {
        if (showing == wanted)
            return;

        showing = wanted;
        build();

        const auto partShown = ! model::partForPanel (showing.kind).empty();
        copyButton.setVisible (partShown);
        pasteButton.setVisible (partShown);

        resized();
        repaint();
    }

    void FootPanelComponent::build()
    {
        /*  ONE EDITOR AT A TIME, built when the subject changes and destroyed
            with it. A kind that is not showing holds no component, so a panel
            on a waveform is not also carrying a fader bank nobody asked for. */
        waveform.reset();
        sends.reset();
        timeline.reset();
        curve.reset();
        eq.reset();
        fadeMixer.reset();
        messages.reset();
        curves.reset();
        fx.reset();
        takePanel.reset();
        picture.reset();

        switch (showing.kind)
        {
            case model::Subject::Kind::picture:
            {
                /*  A VIDEO CUE'S PICTURE (namespace draft §47, AAG): its place
                    on its canvas, its colour, its mask - dragged and typed. */
                PicturePanelComponent::Actions placing;
                placing.set = actions.set;
                placing.setMany = actions.setMany;
                placing.say = [this] (const juce::String& sentence)
                {
                    note = sentence;
                    repaint();
                };

                picture = std::make_unique<PicturePanelComponent> (theme, std::move (placing));
                addAndMakeVisible (*picture);
                break;
            }

            case model::Subject::Kind::messages:
            {
                /*  AN OSC CUE'S MESSAGES (namespace draft 45, O.5): a table of
                    them, the cue's own first, every value with its type and a
                    switch putting a curve on a number. */
                OscMessagesComponent::Actions editing;
                editing.set = actions.set;
                editing.setMany = actions.setMany;
                editing.createMessage = actions.createMessage;
                editing.promoteMessage = actions.promoteMessage;
                editing.createCurve = actions.createCurve;
                editing.removeObject = actions.removeObject;
                editing.say = [this] (const juce::String& sentence)
                {
                    note = sentence;
                    repaint();
                };

                messages = std::make_unique<OscMessagesComponent> (theme, std::move (editing));
                addAndMakeVisible (*messages);

                /*  AND ITS CURVES BESIDE IT (O.7), one at a time with the
                    cue's others faint behind (YT), with the cue's transport. */
                CurveLaneComponent::Actions drawing;
                drawing.set = actions.set;
                drawing.play = actions.play;
                drawing.stop = actions.stop;
                drawing.seek = actions.seek;
                drawing.say = [this] (const juce::String& sentence)
                {
                    note = sentence;
                    repaint();
                };
                drawing.arm = actions.curveArm;
                drawing.free = actions.curveFree;
                drawing.rec = actions.curveRec;
                drawing.record = actions.curveRecord;
                drawing.stopPass = actions.curveStop;

                curves = std::make_unique<CurveLaneComponent> (theme, std::move (drawing));
                addAndMakeVisible (*curves);
                break;
            }

            case model::Subject::Kind::fade:
            {
                /*  WHAT A FADE MOVES (namespace draft §26, PG): its doors open
                    the EQ and the curve in this same foot, on the fade, and a
                    plugin's own window on the fade. */
                FadeMixerComponent::Actions fading;
                fading.set = actions.set;
                fading.openEq = [this] (const std::string& fadeId)
                {
                    if (actions.openSubject)
                        actions.openSubject ({ model::Subject::Kind::eq, fadeId });
                };
                fading.openCurve = [this] (const std::string& fadeId)
                {
                    if (actions.openSubject)
                        actions.openSubject ({ model::Subject::Kind::curve, fadeId });
                };
                fading.editPlugin = actions.editPlugin;
                fading.say = [this] (const juce::String& sentence)
                {
                    note = sentence;
                    repaint();
                };

                fadeMixer = std::make_unique<FadeMixerComponent> (theme, std::move (fading));
                addAndMakeVisible (*fadeMixer);
                break;
            }

            case model::Subject::Kind::take:
            {
                /*  THE TAKE (Phase 9c): its presses are the take verbs, a
                    dragged edge is the take's door, and a press on an edge
                    puts it on the master dial. */
                TakePanelComponent::Actions taking;
                taking.set = actions.set;
                taking.press = actions.pressTake;
                taking.keep = actions.keepTake;
                taking.dial = actions.dial;
                taking.say = [this] (const juce::String& sentence)
                {
                    note = sentence;
                    repaint();
                };

                takePanel = std::make_unique<TakePanelComponent> (theme, std::move (taking));
                addAndMakeVisible (*takePanel);
                break;
            }

            case model::Subject::Kind::fx:
            {
                /*  THE CHAIN (author, 2026-09-25). Its EQ box opens the EQ in
                    this same foot, on the same cue - one editor at a time,
                    so asking for the EQ is asking the host for another
                    subject, never a second panel. */
                FxPanelComponent::Actions chaining;
                chaining.set = actions.set;
                chaining.createFx = actions.createFx;
                chaining.edit = [this] (const std::string& cueId, const std::string& pluginId)
                {
                    if (actions.editPlugin)
                        actions.editPlugin (cueId, pluginId);
                    else
                    {
                        note = "The plugin's own window is not built yet.";
                        repaint();
                    }
                };
                chaining.openEq = [this] (const std::string& cueId)
                {
                    /*  ON THE NEXT MESSAGE, not inside the click: opening
                        another subject destroys this chain, and the button
                        that asked is one of its children. */
                    juce::MessageManager::callAsync ([self = juce::Component::SafePointer<FootPanelComponent> (this),
                                                      cueId]
                    {
                        if (self != nullptr && self->actions.openEqOn)
                            self->actions.openEqOn (cueId);
                    });
                };
                chaining.openTake = [this] (const std::string& cueId)
                {
                    //  On the next message, for the EQ box's reason.
                    juce::MessageManager::callAsync ([self = juce::Component::SafePointer<FootPanelComponent> (this),
                                                      cueId]
                    {
                        if (self != nullptr && self->actions.openTakeOn)
                            self->actions.openTakeOn (cueId);
                    });
                };
                chaining.say = [this] (const juce::String& sentence)
                {
                    note = sentence;
                    repaint();
                };

                fx = std::make_unique<FxPanelComponent> (theme, std::move (chaining));
                fx->setEditorWords (editorWords);
                addAndMakeVisible (*fx);
                break;
            }

            case model::Subject::Kind::eq:
            {
                EqPanelComponent::Actions shaping;
                shaping.set = actions.set;
                shaping.setMany = actions.setMany;
                shaping.reset = actions.resetEq;
                shaping.say = [this] (const juce::String& sentence)
                {
                    note = sentence;
                    repaint();
                };

                shaping.dial = actions.dial;

                eq = std::make_unique<EqPanelComponent> (theme, std::move (shaping));
                eq->showDial (dialed);
                addAndMakeVisible (*eq);
                break;
            }

            case model::Subject::Kind::curve:
            {
                CurveEditorComponent::Actions drawing;
                drawing.set = actions.set;
                drawing.say = [this] (const juce::String& sentence)
                {
                    note = sentence;
                    repaint();
                };

                curve = std::make_unique<CurveEditorComponent> (theme, std::move (drawing));
                addAndMakeVisible (*curve);
                break;
            }

            case model::Subject::Kind::timeline:
            {
                TimelineComponent::Actions arranging;
                arranging.set = actions.set;
                arranging.openOn = actions.openTimelineOn;
                arranging.say = [this] (const juce::String& sentence)
                {
                    note = sentence;
                    repaint();
                };

                timeline = std::make_unique<TimelineComponent> (theme, std::move (arranging));
                addAndMakeVisible (*timeline);
                break;
            }

            case model::Subject::Kind::sends:
            {
                SendMixerComponent::Actions mixing;
                mixing.set = actions.set;
                mixing.setMany = actions.setMany;
                mixing.createSend = actions.createSend;
                mixing.removeSend = actions.removeObject;
                mixing.say = [this] (const juce::String& sentence)
                {
                    note = sentence;
                    repaint();
                };

                mixing.dial = actions.dial;

                sends = std::make_unique<SendMixerComponent> (theme, std::move (mixing));
                sends->showDial (dialed);
                addAndMakeVisible (*sends);
                break;
            }

            case model::Subject::Kind::waveform:
            {
                WaveformEditorComponent::Actions editing;
                editing.set = actions.set;
                editing.createRange = actions.createRange;
                editing.removeRange = actions.removeObject;
                editing.splitRange = actions.splitRange;
                editing.play = actions.play;
                editing.stop = actions.stop;
                editing.seek = actions.seek;
                editing.laneArm = actions.laneArm;
                editing.laneFree = actions.laneFree;
                editing.laneRecord = actions.laneRecord;
                editing.laneStop = actions.laneStop;
                editing.say = [this] (const juce::String& sentence)
                {
                    note = sentence;
                    repaint();
                };

                waveform = std::make_unique<WaveformEditorComponent> (theme, std::move (editing));
                addAndMakeVisible (*waveform);
                break;
            }

            case model::Subject::Kind::none:
                break;
        }
    }

    void FootPanelComponent::showEditedEqHandle (int handle)
    {
        if (eq != nullptr)
            eq->setEditedHandle (handle);
    }

    void FootPanelComponent::showDial (const std::string& address)
    {
        dialed = address;

        if (eq != nullptr)
            eq->showDial (address);

        if (sends != nullptr)
            sends->showDial (address);
    }

    void FootPanelComponent::show (const model::FootReading& reading,
                                   std::shared_ptr<const audio::MediaRecords> media,
                                   std::shared_ptr<const audio::TakePictureSet> takes)
    {
        /*  WHAT IT IS SHOWING AND WHAT THAT THING IS CALLED. Said in the title
            rather than left to the drawing, because a panel that opens on one
            of several subjects has to answer "which" before it answers
            anything else. */
        auto wanted = juce::String();

        switch (showing.kind)
        {
            case model::Subject::Kind::waveform:
                wanted = "Waveform";
                break;

            case model::Subject::Kind::sends:
                wanted = "Send levels";
                break;

            case model::Subject::Kind::timeline:
                wanted = "Timeline";
                break;

            case model::Subject::Kind::curve:
                wanted = "Fade curve";
                break;

            case model::Subject::Kind::eq:
                wanted = "EQ";
                break;

            case model::Subject::Kind::fx:
                wanted = "FX";
                break;

            case model::Subject::Kind::take:
                wanted = "Take";
                break;

            case model::Subject::Kind::fade:
                wanted = "Fade";
                break;

            case model::Subject::Kind::messages:
                wanted = "Messages";
                break;

            case model::Subject::Kind::picture:
                wanted = "Picture";
                break;

            case model::Subject::Kind::none:
                break;
        }

        /*  THE CUE BY NAME, AFTER A PICTURE OF ITS KIND (2026-09-30), rather
            than joined to the panel's word by a dash: two pictures and two
            words, each saying one thing. */
        const auto wantedCue = juce::String (reading.cueName);
        const auto wantedIcon = model::iconForPanel (model::wordFor (showing.kind));
        const auto wantedCueIcon = reading.cueName.empty() ? model::Icon::none : model::iconFor (reading.cueKind);
        const auto wantedAccent = model::accentFor (reading.cueKind);

        if (wanted != title || wantedCue != titleCue || wantedIcon != titleIcon
              || wantedCueIcon != cueIcon || wantedAccent != cueAccent)
        {
            title = wanted;
            titleCue = wantedCue;
            titleIcon = wantedIcon;
            cueIcon = wantedCueIcon;
            cueAccent = wantedAccent;
            repaint();
        }

        if (waveform != nullptr)
            waveform->show (reading, std::move (media));

        /*  NOT HIDDEN WHEN THERE IS NOTHING TO DRAW: the mixer says why
            itself, as the waveform editor does. A panel that went blank would
            leave somebody wondering whether the show has no mixes, whether
            this cue plays nothing, or whether the window has stopped. */
        if (sends != nullptr)
            sends->show (reading);

        if (timeline != nullptr)
            timeline->show (reading);

        if (curve != nullptr)
            curve->show (reading);

        if (eq != nullptr)
            eq->show (reading);

        if (fadeMixer != nullptr)
            fadeMixer->show (reading);

        if (messages != nullptr)
            messages->show (reading.oscMessages);

        if (curves != nullptr)
            curves->show (reading.oscCurves, reading.running, reading.position, reading.runId);

        if (fx != nullptr)
            fx->show (reading);

        if (takePanel != nullptr)
            takePanel->show (reading, std::move (takes));

        if (picture != nullptr)
            picture->show (reading, media);
    }

    void FootPanelComponent::setPasteable (bool pasteable, const juce::String& why)
    {
        if (pasteButton.isEnabled() != pasteable)
            pasteButton.setEnabled (pasteable);

        if (pasteButton.getTooltip() != why)
            pasteButton.setTooltip (why);
    }

    void FootPanelComponent::setEditorWords (std::map<std::string, std::string> words)
    {
        /*  KEPT HERE AS WELL, so a chain built after the words arrived - the
            panel reopened, or pointed at another cue - starts with them
            rather than blank for a pass. */
        editorWords = std::move (words);

        if (fx != nullptr)
            fx->setEditorWords (editorWords);
    }

    bool FootPanelComponent::overGrip (juce::Point<int> where) const
    {
        return where.y <= gripHeight();
    }

    void FootPanelComponent::paint (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "panel"));

        /*  THE LINE ALONG THE TOP IS THE EDGE AND THE GRIP, one thing, because
            the edge is where a hand goes to change a height and a separate
            handle would be a second thing to find. */
        g.setColour (Look::colour (theme, dragging ? "picked" : "rule"));
        g.fillRect (0, 0, getWidth(), dragging ? 2 : 1);

        const auto row = juce::roundToInt (theme.row * theme.type);
        auto head = juce::Rectangle<int> (0, gripHeight(), getWidth(), row).reduced (10, 0);

        head.removeFromRight (row);   // the close button's place
        head.removeFromRight (headButtons);

        /*  WHAT IT IS, THEN WHOSE: the panel's picture and word, and the cue's
            kind as a picture in its accent before the cue's name. */
        const auto iconSide = static_cast<float> (row) * 0.58f;
        const auto font = Look::font (theme, 13.0f);
        const auto ink = Look::colour (theme, "ink-dim");

        icons::draw (g, titleIcon, head.removeFromLeft (row).toFloat().withSizeKeepingCentre (iconSide, iconSide),
                     ink);
        head.removeFromLeft (row / 8);

        g.setColour (ink);
        g.setFont (font);
        g.drawText (title, head, juce::Justification::centredLeft, true);

        if (titleCue.isNotEmpty())
        {
            auto after = head.withTrimmedLeft (juce::GlyphArrangement::getStringWidthInt (font, title) + row / 2);

            icons::draw (g, cueIcon, after.removeFromLeft (row).toFloat().withSizeKeepingCentre (iconSide, iconSide),
                         Look::colour (theme, cueAccent.c_str()));
            after.removeFromLeft (row / 8);

            g.setColour (Look::colour (theme, "ink"));
            g.drawText (titleCue, after, juce::Justification::centredLeft, true);
        }

        if (note.isNotEmpty())
        {
            g.setColour (Look::colour (theme, "ink-off"));
            g.setFont (Look::font (theme, 12.0f));
            g.drawText (note, head, juce::Justification::centredRight, true);
        }
    }

    void FootPanelComponent::resized()
    {
        auto area = getLocalBounds();
        area.removeFromTop (gripHeight());

        const auto row = juce::roundToInt (theme.row * theme.type);
        auto head = area.removeFromTop (row);

        shut.setBounds (head.removeFromRight (row + 10).reduced (6, 3));

        /*  PASTE NEAREST THE CLOSE BUTTON AND COPY BEFORE IT, as they read: a
            word each while the head has room for both beside the title, the
            pictures alone when it has not. */
        headButtons = 0;

        if (copyButton.isVisible())
        {
            const auto height = row - 6;
            const auto roomy = head.getWidth() > 520;

            for (auto* button : { &pasteButton, &copyButton })
            {
                button->setWordShown (roomy);
                const auto width = roomy ? button->idealWidth (height) : height + 6;
                button->setBounds (head.removeFromRight (width + 4).withTrimmedLeft (4).withSizeKeepingCentre (width, height));
                headButtons += width + 4;
            }
        }

        if (waveform != nullptr)
        {
            /*  NO INSET OF ITS OWN ACROSS. The picture's left edge is the cue
                list's left edge and the table's left edge is the running
                pane's, which is what makes the foot read as being under the
                window rather than beside it. */
            waveform->setRightColumn (columnWidth, columnGap);
            waveform->setBounds (area.withTrimmedTop (2).withTrimmedBottom (2));
        }

        if (sends != nullptr)
            sends->setBounds (area.withTrimmedTop (2).withTrimmedBottom (2));

        if (timeline != nullptr)
            timeline->setBounds (area.withTrimmedTop (2).withTrimmedBottom (2));

        if (curve != nullptr)
            curve->setBounds (area.withTrimmedTop (2).withTrimmedBottom (2));

        if (eq != nullptr)
            eq->setBounds (area.withTrimmedTop (2).withTrimmedBottom (2));

        if (fadeMixer != nullptr)
            fadeMixer->setBounds (area.withTrimmedTop (2).withTrimmedBottom (2));

        if (picture != nullptr)
            picture->setBounds (area.withTrimmedTop (2).withTrimmedBottom (2));

        /*  THE TABLE ON THE LEFT AND THE CURVES ON THE RIGHT, the table as
            wide as the inspector and the cue list's own column would allow it
            and never more than half. */
        if (messages != nullptr)
        {
            auto inner = area.withTrimmedTop (2).withTrimmedBottom (2);

            if (curves != nullptr)
            {
                const auto tableWidth = juce::jmin (inner.getWidth() / 2, juce::jmax (420, inner.getWidth() * 2 / 5));
                messages->setBounds (inner.removeFromLeft (tableWidth));
                inner.removeFromLeft (6);
                curves->setBounds (inner);
            }
            else
            {
                messages->setBounds (inner);
            }
        }

        if (fx != nullptr)
            fx->setBounds (area.withTrimmedTop (2).withTrimmedBottom (2));

        if (takePanel != nullptr)
            takePanel->setBounds (area.withTrimmedTop (2).withTrimmedBottom (2));
    }

    void FootPanelComponent::mouseMove (const juce::MouseEvent& event)
    {
        setMouseCursor (overGrip (event.getPosition()) ? juce::MouseCursor::UpDownResizeCursor
                                                       : juce::MouseCursor::NormalCursor);
    }

    void FootPanelComponent::mouseDown (const juce::MouseEvent& event)
    {
        dragging = overGrip (event.getPosition());
        dragFrom = event.getScreenY();

        if (dragging)
            repaint();
    }

    void FootPanelComponent::mouseDrag (const juce::MouseEvent& event)
    {
        if (! dragging || actions.resizeBy == nullptr)
            return;

        /*  UPWARDS MAKES IT TALLER, which is the direction the edge moves. The
            Shell clamps: how much of the window this may take is the window's
            question and not the panel's. */
        const auto moved = dragFrom - event.getScreenY();

        if (moved != 0)
        {
            actions.resizeBy (moved);
            dragFrom = event.getScreenY();
        }
    }
}
