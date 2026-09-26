/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#include <wfg/client/ui/FootPanelComponent.h>

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

        if (fx != nullptr)
            fx->applyTheme (theme);

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
        fx.reset();

        switch (showing.kind)
        {
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
                                   std::shared_ptr<const audio::MediaRecords> media)
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

            case model::Subject::Kind::none:
                break;
        }

        if (! reading.cueName.empty())
            wanted += "  " + juce::String::fromUTF8 ("\xe2\x80\x94") + "  "
                        + juce::String (reading.cueName);

        if (wanted != title)
        {
            title = wanted;
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

        if (fx != nullptr)
            fx->show (reading);
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

        g.setColour (Look::colour (theme, "ink-dim"));
        g.setFont (Look::font (theme, 13.0f));
        g.drawText (title, head, juce::Justification::centredLeft, true);

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

        if (fx != nullptr)
            fx->setBounds (area.withTrimmedTop (2).withTrimmedBottom (2));
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
