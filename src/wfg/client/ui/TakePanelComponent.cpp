/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#include <wfg/client/ui/TakePanelComponent.h>

#include <wfg/client/model/Take.h>
#include <wfg/client/ui/Look.h>
#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <cmath>

namespace wfg::client::ui
{
    namespace
    {
        /** How near an edge a press takes it, in pixels either side. */
        constexpr int edgeReach = 6;
    }

    TakePanelComponent::TakePanelComponent (const model::Theme& themeToUse, Actions actionsToUse)
        : theme (themeToUse), actions (std::move (actionsToUse))
    {
        for (auto* button : { &rec, &loop, &overdub, &undo, &clear })
            addAndMakeVisible (*button);

        /*  THE FIVE PRESSES, each the command of its name on the channel -
            what the D700's Rec and a transport cue send too, so the log says
            the same thing whichever hand it was. */
        rec.onClick = [this] { pressed ("record"); };
        loop.onClick = [this] { pressed ("loop"); };
        overdub.onClick = [this] { pressed ("overdub"); };
        undo.onClick = [this] { pressed ("undo"); };
        clear.onClick = [this] { pressed ("clear"); };

        updateButtons();
    }

    void TakePanelComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;
        resized();
        repaint();
    }

    void TakePanelComponent::show (const model::FootReading& reading,
                                   std::shared_ptr<const audio::TakePictureSet> set)
    {
        take = reading.take;
        pictures = std::move (set);

        if (! take.present || take.state == "empty" || take.state == "recording")
            grabbed = hover = Edge::none;

        updateButtons();
        repaint();
    }

    void TakePanelComponent::updateButtons()
    {
        /*  REC, LOOP AND A LAYER RECORD A CUE'S INPUT, so they want a mic cue
            sounding on the channel - the engine's own rule, offered only where
            it would be applied, and the sentence says why when it is not. */
        const auto sounds = take.present && take.channelSounds;
        const auto& state = take.state;

        rec.setEnabled (sounds);
        loop.setEnabled (sounds && (state == "recording" || state == "overdubbing" || state == "held"));
        overdub.setEnabled (sounds && state != "empty");
        undo.setEnabled (take.present && (take.layers > 0 || state == "recording" || state == "overdubbing"));
        clear.setEnabled (take.present && take.hasTake());
    }

    void TakePanelComponent::pressed (const char* verb)
    {
        if (take.present && actions.press != nullptr)
            actions.press (verb, take.channelId);
    }

    //==============================================================================
    void TakePanelComponent::resized()
    {
        const auto row = juce::roundToInt (theme.row * theme.type);
        auto strip = getLocalBounds().removeFromTop (row).reduced (10, 3);
        const auto width = juce::roundToInt (58.0 * theme.type);
        const auto gap = juce::roundToInt (4.0 * theme.type);

        for (auto* button : { &rec, &loop, &overdub, &undo, &clear })
        {
            button->setBounds (strip.removeFromLeft (width));
            strip.removeFromLeft (gap);
        }
    }

    juce::Rectangle<int> TakePanelComponent::wordsArea() const
    {
        const auto row = juce::roundToInt (theme.row * theme.type);
        auto strip = getLocalBounds().removeFromTop (row).reduced (10, 0);
        strip.setLeft (clear.getRight() + juce::roundToInt (12.0 * theme.type));
        return strip;
    }

    juce::Rectangle<int> TakePanelComponent::pictureArea() const
    {
        const auto row = juce::roundToInt (theme.row * theme.type);
        return getLocalBounds().withTrimmedTop (row + 4)
                               .withTrimmedBottom (juce::roundToInt (16.0 * theme.type))
                               .reduced (10, 2);
    }

    double TakePanelComponent::secondsAt (int x) const
    {
        const auto area = pictureArea();
        const auto span = take.span();

        if (span <= 0.0 || area.getWidth() <= 0)
            return 0.0;

        return juce::jlimit (0.0, span, static_cast<double> (x - area.getX()) / area.getWidth() * span);
    }

    int TakePanelComponent::xOf (double seconds) const
    {
        const auto area = pictureArea();
        const auto span = take.span();

        if (span <= 0.0)
            return area.getX();

        return area.getX() + juce::roundToInt (juce::jlimit (0.0, span, seconds) / span * area.getWidth());
    }

    double TakePanelComponent::pointOf (Edge edge) const
    {
        /*  WHERE THE HAND HAS IT, while it is held: the tree says where the
            door put it a tick later, and an edge that trailed the pointer by a
            pass would feel like a rubber band. */
        if (edge == grabbed && sentSeconds >= 0.0)
            return sentSeconds;

        return edge == Edge::in ? take.loopIn : take.loopOut;
    }

    TakePanelComponent::Edge TakePanelComponent::edgeAt (juce::Point<int> where) const
    {
        /*  ONLY A CLOSED TAKE HAS A LOOP to move the edges of; a recording is
            still finding its length. */
        if (! take.present || take.state == "empty" || take.state == "recording" || take.span() <= 0.0
              || ! pictureArea().expanded (edgeReach, 0).contains (where))
            return Edge::none;

        const auto toIn = std::abs (where.x - xOf (take.loopIn));
        const auto toOut = std::abs (where.x - xOf (take.loopOut));

        if (std::min (toIn, toOut) > edgeReach)
            return Edge::none;

        return toIn < toOut ? Edge::in : Edge::out;
    }

    //==============================================================================
    void TakePanelComponent::paint (juce::Graphics& g)
    {
        /*  WHAT IT IS DOING, IN WORDS FIRST: the state, the length and the
            layers, and why Rec is not offered when it is not. */
        auto said = model::takePanelWords (take);

        if (const auto why = model::takePressWhy (take); ! why.empty())
            said += "  " + why;

        g.setColour (Look::colour (theme, "ink-dim"));
        g.setFont (Look::font (theme, 13.0f));
        g.drawFittedText (juce::String::fromUTF8 (said.c_str()), wordsArea(),
                          juce::Justification::centredLeft, 2);

        if (! take.present)
            return;

        const auto area = pictureArea();

        g.setColour (Look::colour (theme, "panel-in"));
        g.fillRect (area);

        paintLoop (g, area);
        paintPicture (g, area);

        //  The time along the bottom: nought, and what the picture spans.
        const auto ruler = juce::Rectangle<int> (area.getX(), area.getBottom() + 2, area.getWidth(),
                                                 juce::roundToInt (14.0 * theme.type));
        g.setColour (Look::colour (theme, "ink-off"));
        g.setFont (Look::font (theme, 11.0f));
        g.drawText ("0 s", ruler, juce::Justification::centredLeft, false);
        g.drawText (juce::String (model::tenthsOfSeconds (take.span())), ruler,
                    juce::Justification::centredRight, false);
    }

    void TakePanelComponent::paintLoop (juce::Graphics& g, juce::Rectangle<int> area) const
    {
        if (take.state == "empty" || take.state == "recording" || take.span() <= 0.0)
            return;

        const auto x0 = xOf (pointOf (Edge::in)), x1 = xOf (pointOf (Edge::out));

        //  The loop: shaded, under the peaks.
        g.setColour (Look::colour (theme, "picked").withAlpha (0.16f));
        g.fillRect (juce::Rectangle<int> (x0, area.getY(), juce::jmax (1, x1 - x0), area.getHeight()));
    }

    void TakePanelComponent::paintPicture (juce::Graphics& g, juce::Rectangle<int> area) const
    {
        const auto* picture = pictures != nullptr ? pictures->of (take.channelId) : nullptr;
        const auto span = take.span();

        if (picture != nullptr && picture->bins > 0 && picture->sampleRate > 0 && span > 0.0)
        {
            const auto middle = area.getCentreY();
            const auto half = area.getHeight() / 2.0;
            const auto secondsPerBin = static_cast<double> (picture->binSamples) / picture->sampleRate;
            const auto laying = take.state == "overdubbing" ? picture->slots - 1 : -1;

            const auto takeInk = Look::colour (theme, "ink-dim");
            const auto layerInk = Look::colour (theme, "picked");
            const auto layingInk = Look::colour (theme, "live");

            /*  A COLUMN OF PIXELS AT A TIME: the loudest of the bins under it
                in each slot, stacked - the take at the bottom, each layer on
                it - and mirrored about the middle. */
            for (int x = 0; x < area.getWidth(); ++x)
            {
                const auto from = static_cast<double> (x) / area.getWidth() * span;
                const auto to = static_cast<double> (x + 1) / area.getWidth() * span;
                const auto first = static_cast<int> (std::floor (from / secondsPerBin));

                if (first >= picture->bins)
                    break;

                const auto last = juce::jlimit (first, picture->bins - 1,
                                                static_cast<int> (std::ceil (to / secondsPerBin)) - 1);
                auto stacked = 0.0;

                for (int slot = 0; slot < picture->slots; ++slot)
                {
                    auto loudest = 0.0f;

                    for (int bin = first; bin <= last; ++bin)
                        loudest = std::max (loudest, picture->peak (slot, bin));

                    if (loudest <= 0.0f)
                        continue;

                    const auto under = stacked;
                    stacked = std::min (1.0, stacked + static_cast<double> (loudest));

                    const auto low = juce::roundToInt (under * half);
                    const auto high = juce::roundToInt (stacked * half);
                    const auto tall = juce::jmax (1, high - low);

                    g.setColour (slot == 0 ? takeInk : slot == laying ? layingInk : layerInk);
                    g.fillRect (area.getX() + x, middle - high, 1, tall);
                    g.fillRect (area.getX() + x, middle + low, 1, tall);
                }
            }

            //  WHERE A RECORDING HAS GOT TO: its growing edge, in the live colour.
            if (take.state == "recording")
            {
                g.setColour (layingInk);
                g.fillRect (xOf (picture->seconds()), area.getY(), 2, area.getHeight());
            }
        }
        else
        {
            g.setColour (Look::colour (theme, "ink-off"));
            g.setFont (Look::font (theme, 12.0f));
            g.drawFittedText (take.state == "empty" ? "no take yet" : "the take's picture is not here yet",
                              area, juce::Justification::centred, 1);
        }

        if (take.state == "empty" || take.state == "recording" || span <= 0.0)
            return;

        /*  THE EDGES, over the peaks: a line each, brighter under the hand,
            with where it stands in words beside it. */
        g.setFont (Look::font (theme, 11.0f));

        for (const auto edge : { Edge::in, Edge::out })
        {
            const auto seconds = pointOf (edge);
            const auto x = xOf (seconds);
            const auto lit = edge == grabbed || (grabbed == Edge::none && edge == hover);

            g.setColour (Look::colour (theme, lit ? "picked" : "ink"));
            g.fillRect (x - (lit ? 1 : 0), area.getY(), lit ? 3 : 2, area.getHeight());

            const auto label = juce::String (edge == Edge::in ? "in " : "out ")
                               + juce::String (model::tenthsOfSeconds (seconds));
            const auto labelWidth = juce::roundToInt (64.0 * theme.type);
            const auto box = edge == Edge::in
                                 ? juce::Rectangle<int> (x + 4, area.getY() + 2, labelWidth, 14)
                                 : juce::Rectangle<int> (x - 4 - labelWidth, area.getY() + 2, labelWidth, 14);

            g.drawText (label, box, edge == Edge::in ? juce::Justification::centredLeft
                                                     : juce::Justification::centredRight, false);
        }

        //  THE PLAYHEAD, going round.
        if (take.state == "looping" || take.state == "overdubbing")
        {
            g.setColour (Look::colour (theme, "standby"));
            g.fillRect (xOf (take.playhead), area.getY(), 2, area.getHeight());
        }
    }

    //==============================================================================
    void TakePanelComponent::mouseMove (const juce::MouseEvent& event)
    {
        const auto now = edgeAt (event.getPosition());

        setMouseCursor (now != Edge::none ? juce::MouseCursor::LeftRightResizeCursor
                                          : juce::MouseCursor::NormalCursor);

        if (now != hover)
        {
            hover = now;
            repaint();
        }
    }

    void TakePanelComponent::mouseExit (const juce::MouseEvent&)
    {
        if (hover != Edge::none)
        {
            hover = Edge::none;
            repaint();
        }
    }

    void TakePanelComponent::mouseDown (const juce::MouseEvent& event)
    {
        grabbed = edgeAt (event.getPosition());
        sentSeconds = -1.0;

        /*  A PRESS ON AN EDGE PUTS IT ON THE MASTER DIAL too (rule BQ,
            amended by §19.7): the point is a number a hand may write. */
        if (grabbed != Edge::none && actions.dial != nullptr)
            actions.dial (model::loopPointAddress (take.channelId, grabbed == Edge::in));
    }

    void TakePanelComponent::mouseDrag (const juce::MouseEvent& event)
    {
        if (grabbed == Edge::none || actions.set == nullptr)
            return;

        /*  ONE WRITE PER MILLISECOND MOVED, not one per pixel event: the
            door clamps it to the take and keeps the two a loop apart. */
        const auto seconds = std::round (secondsAt (event.x) * 1000.0) / 1000.0;

        if (std::abs (seconds - sentSeconds) < 0.0005)
            return;

        sentSeconds = seconds;
        actions.set (model::loopPointAddress (take.channelId, grabbed == Edge::in), osc::formatDouble (seconds));

        if (actions.say != nullptr)
            actions.say (juce::String (grabbed == Edge::in ? "in at " : "out at ")
                           + juce::String (model::tenthsOfSeconds (seconds)));
    }

    void TakePanelComponent::mouseUp (const juce::MouseEvent&)
    {
        grabbed = Edge::none;
        repaint();
    }
}
