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

#include <wfg/client/ui/CartPadsComponent.h>

#include <wfg/client/model/Gestures.h>
#include <wfg/client/model/Icons.h>
#include <wfg/client/ui/Icons.h>
#include <wfg/client/ui/Look.h>
#include <wfg/client/ui/PointerKeys.h>

#include <algorithm>
#include <optional>

namespace wfg::client::ui
{
    namespace
    {
        //  The tablet's pad, 150 by 112, and the gap between pads.
        constexpr int padWidth = 150;
        constexpr int padHeight = 112;
        constexpr int padGap = 10;

        /*  "#rrggbb" and nothing else: anything else is no colour at all,
            never a black nobody chose. */
        std::optional<juce::Colour> colourOf (const std::string& text)
        {
            const juce::String colour (text);

            if (colour.length() != 7 || ! colour.startsWithChar ('#')
                  || ! colour.substring (1).containsOnly ("0123456789abcdefABCDEF"))
                return std::nullopt;

            return juce::Colour::fromString ("ff" + colour.substring (1));
        }
    }

    //==============================================================================
    CartPadsComponent::CartPadsComponent (const model::Theme& themeToUse, std::function<void (Event)> sendToUse)
        : theme (themeToUse), send (std::move (sendToUse))
    {
        viewport.setViewedComponent (&canvas, false);
        viewport.setScrollBarsShown (true, false);

        /*  A FINGER ON A PAD PLAYS IT and does not scroll the cart: JUCE's
            viewport scrolls under any drag that is not a mouse's. The scroll
            bar and the wheel still scroll. */
        viewport.setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::never);
        viewport.setWantsKeyboardFocus (false);
        canvas.setWantsKeyboardFocus (false);
        addAndMakeVisible (viewport);
    }

    CartPadsComponent::~CartPadsComponent()
    {
        //  A pad still held lets go: its lift will never reach a view that is gone.
        releaseEverything();
    }

    void CartPadsComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;
        layOut();
        repaint();
    }

    int CartPadsComponent::scaled (int base) const
    {
        return juce::roundToInt (static_cast<double> (base) * theme.type);
    }

    void CartPadsComponent::show (const std::vector<model::CartPad>& padsNow, const std::string& sayNow)
    {
        if (padsNow == pads && sayNow == said)
            return;

        const auto shape = padsNow.size() != pads.size();
        pads = padsNow;
        said = sayNow;

        if (shape)
            layOut();

        canvas.repaint();
    }

    //==============================================================================
    void CartPadsComponent::resized()
    {
        viewport.setBounds (getLocalBounds());
        layOut();
    }

    void CartPadsComponent::layOut()
    {
        /*  IN ROWS THAT WRAP, as the tablet's: as many pads a row as the width
            takes, at least one, and as tall as the rows need. */
        const auto width = juce::jmax (1, viewport.getMaximumVisibleWidth());
        const auto gap = scaled (padGap);
        const auto padW = scaled (padWidth);
        const auto padH = scaled (padHeight);
        const auto across = juce::jmax (1, (width - gap) / (padW + gap));

        places.clear();

        for (std::size_t at = 0; at < pads.size(); ++at)
        {
            const auto column = static_cast<int> (at) % across;
            const auto row = static_cast<int> (at) / across;
            places.emplace_back (gap + column * (padW + gap), gap + row * (padH + gap), padW, padH);
        }

        const auto rows = (static_cast<int> (pads.size()) + across - 1) / across;
        const auto tall = gap + rows * (padH + gap);

        canvas.setSize (width, juce::jmax (tall, viewport.getMaximumVisibleHeight()));
    }

    juce::Rectangle<int> CartPadsComponent::padBounds (std::size_t index) const
    {
        return index < places.size() ? places[index] : juce::Rectangle<int> {};
    }

    std::size_t CartPadsComponent::padAt (juce::Point<int> where) const
    {
        for (std::size_t at = 0; at < places.size(); ++at)
            if (places[at].contains (where))
                return at;

        return pads.size();
    }

    bool CartPadsComponent::held (const std::string& cue) const
    {
        return std::any_of (holding.begin(), holding.end(), [&cue] (const auto& entry) { return entry.second == cue; });
    }

    //==============================================================================
    void CartPadsComponent::Canvas::paint (juce::Graphics& g)
    {
        owner.paintPads (g);
    }

    void CartPadsComponent::Canvas::mouseDown (const juce::MouseEvent& event)
    {
        owner.pointerDown (pointerKeyOf (event.source), event.position, draggingPointers());
    }

    void CartPadsComponent::Canvas::mouseUp (const juce::MouseEvent& event)
    {
        owner.pointerUp (pointerKeyOf (event.source));
    }

    void CartPadsComponent::pointerDown (int pointer, juce::Point<float> at, int dragging)
    {
        for (const auto stale : turns.sweep (dragging))
            if (const auto found = holding.find (stale); found != holding.end())
            {
                const auto cue = found->second;
                holding.erase (found);

                if (send)
                    send (gesture::releaseCue (cue));
            }

        if (! turns.down (pointer))
            return;

        const auto index = padAt (at.roundToInt());

        //  ONE PAD, ONE HAND (BFA): a pad another hand holds is not struck again under it.
        if (index >= pads.size() || held (pads[index].cue))
            return;

        /*  HOW FAR UP THE PAD, from its bottom edge: the top is a hard hit, as
            on the desk's pads and the tablet's. */
        const auto& place = places[index];
        const auto height = static_cast<double> ((static_cast<float> (place.getBottom()) - at.y)
                                                 / static_cast<float> (juce::jmax (1, place.getHeight())));

        holding[pointer] = pads[index].cue;

        if (send)
            send (gesture::pressCue (pads[index].cue, model::padVelocity (height)));

        canvas.repaint();
    }

    void CartPadsComponent::pointerUp (int pointer)
    {
        turns.up (pointer);

        const auto found = holding.find (pointer);

        if (found == holding.end())
            return;

        const auto cue = found->second;
        holding.erase (found);

        if (send)
            send (gesture::releaseCue (cue));

        canvas.repaint();
    }

    void CartPadsComponent::releaseEverything()
    {
        const auto was = holding;
        holding.clear();
        turns.clear();

        if (send)
            for (const auto& entry : was)
                send (gesture::releaseCue (entry.second));

        canvas.repaint();
    }

    //==============================================================================
    void CartPadsComponent::paint (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "ground"));
    }

    void CartPadsComponent::paintPads (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "ground"));

        //  An empty cart says so rather than showing an empty field.
        if (pads.empty())
        {
            g.setColour (Look::colour (theme, "ink-faint"));
            g.setFont (Look::font (theme, 14.0f));
            g.drawFittedText (juce::String::fromUTF8 ((said.empty() ? std::string ("Nothing in this cart yet.") : said).c_str()),
                              canvas.getLocalBounds().reduced (scaled (24)), juce::Justification::centredTop, 3);
            return;
        }

        for (std::size_t at = 0; at < pads.size(); ++at)
            paintPad (g, at);
    }

    void CartPadsComponent::paintPad (juce::Graphics& g, std::size_t index)
    {
        const auto& pad = pads[index];
        const auto area = places[index].toFloat();
        const auto lit = held (pad.cue) || pad.going;

        g.setColour (Look::colour (theme, lit ? "picked" : "panel-high"));
        g.fillRoundedRectangle (area, 8.0f);
        g.setColour (Look::colour (theme, lit ? "ink" : "rule"));
        g.drawRoundedRectangle (area.reduced (1.0f), 8.0f, lit ? 2.5f : 1.0f);

        //  The cue's own colour, as a tab at the left - never the only telling.
        if (const auto colour = colourOf (pad.colour))
        {
            g.setColour (*colour);
            g.fillRoundedRectangle (area.withWidth (static_cast<float> (scaled (6))), 4.0f);
        }

        auto inside = places[index].reduced (scaled (8)).withTrimmedLeft (scaled (6));
        const auto ink = Look::colour (theme, lit ? "ground" : "ink");
        const auto faint = Look::colour (theme, lit ? "ground" : "ink-faint");

        auto head = inside.removeFromTop (scaled (18));
        icons::draw (g, model::iconFor (pad.kind, pad.mode), head.removeFromLeft (scaled (18)).toFloat(), faint);
        head.removeFromLeft (scaled (6));
        g.setColour (faint);
        g.setFont (Look::font (theme, 13.0f));
        g.drawText (juce::String::fromUTF8 (pad.number.c_str()), head, juce::Justification::centredLeft, true);

        //  "going" at the foot while lit - the words, not the colour, carry it (§4.8).
        if (lit)
        {
            g.setColour (ink);
            g.setFont (Look::font (theme, 12.0f).boldened());
            g.drawText ("going", inside.removeFromBottom (scaled (15)), juce::Justification::centredLeft, true);
        }

        if (! pad.bank.empty())
        {
            g.setColour (faint);
            g.setFont (Look::font (theme, 12.0f));
            g.drawText (juce::String::fromUTF8 (pad.bank.c_str()), inside.removeFromBottom (scaled (15)),
                        juce::Justification::centredLeft, true);
        }

        inside.removeFromTop (scaled (4));
        g.setColour (ink);
        g.setFont (Look::font (theme, 16.0f).boldened());
        g.drawFittedText (juce::String::fromUTF8 (pad.name.c_str()), inside, juce::Justification::topLeft, 2, 0.8f);
    }
}
