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

#include <wfg/client/ui/FxPanelComponent.h>

#include <wfg/client/model/Eq.h>
#include <wfg/client/model/Fx.h>
#include <wfg/client/ui/Look.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace wfg::client::ui
{
    namespace
    {
        constexpr int boxWidth = 190;
        constexpr int linkWidth = 34;     // the arrow between two boxes
        constexpr int endWidth = 40;      // "file" and "out"
        constexpr int margin = 8;

        /** A frequency as a person reads it: 80 Hz, 1.2 kHz. */
        juce::String hertzText (double frequency)
        {
            if (frequency >= 1000.0)
                return juce::String (frequency / 1000.0, frequency >= 10000.0 ? 0 : 1) + " kHz";

            return juce::String (juce::roundToInt (frequency)) + " Hz";
        }

        /*  WHAT THE EQ IS DOING, in a line: the box is too small for the
            curve, and "flat" or "high-pass 80 Hz, 2 bands" answers the one
            question a glance at a chain asks - is it shaping this cue. */
        juce::String eqSummary (const audio::EqSettings& s)
        {
            juce::StringArray parts;

            if (s.hpf)
                parts.add ("high-pass " + hertzText (s.hpfFreq));

            auto bands = 0;

            for (int band = 0; band < audio::EqSettings::numBands; ++band)
                if (audio::EqSettings::bandIsActive (s.band[band]))
                    ++bands;

            if (bands > 0)
                parts.add (juce::String (bands) + (bands == 1 ? " band" : " bands"));

            if (s.lpf)
                parts.add ("low-pass " + hertzText (s.lpfFreq));

            return parts.isEmpty() ? juce::String ("flat") : parts.joinIntoString (", ");
        }

        const char* stateColour (const std::string& state)
        {
            if (state == "failed" || state == "missing")
                return "failed";

            if (state == "loading")
                return "waiting";

            return "ink-dim";
        }
    }

    //==============================================================================
    /*  ONE LINK OF THE CHAIN: the EQ's box, or one plugin's. Each draws its
        own words and holds its own two controls, and knows nothing of the
        others - the canvas decides where it goes and draws the arrows. */
    struct FxPanelComponent::Box final : public juce::Component
    {
        Box (FxPanelComponent& ownerToUse, bool isEqToUse)
            : owner (ownerToUse), isEq (isEqToUse)
        {
            inSwitch.setButtonText ("in");
            inSwitch.setWantsKeyboardFocus (false);
            door.setWantsKeyboardFocus (false);
            addAndMakeVisible (inSwitch);
            addAndMakeVisible (door);

            if (isEq)
            {
                door.setButtonText ("Open");
                inSwitch.setTooltip ("Whether this cue's EQ is in its signal");
                door.setTooltip ("Show this cue's EQ in the panel");

                inSwitch.onClick = [this]
                {
                    if (owner.actions.set)
                        owner.actions.set (model::eqAddress (owner.reading.subject.objectId, "eqOn"),
                                           inSwitch.getToggleState() ? "true" : "false");
                };

                door.onClick = [this]
                {
                    if (owner.actions.openEq)
                        owner.actions.openEq (owner.reading.subject.objectId);
                };
            }
            else
            {
                door.setButtonText ("Edit...");

                inSwitch.onClick = [this] { owner.switchPlugin (strip, inSwitch.getToggleState()); };

                door.onClick = [this]
                {
                    if (owner.actions.edit)
                        owner.actions.edit (owner.reading.subject.objectId, strip.pluginId);
                };
            }
        }

        /** Whether this link is in the cue's signal, as the switch should show it. */
        bool isIn() const
        {
            if (isEq)
                return owner.reading.eq.settings.on;

            return strip.present() ? strip.enabled : owner.pending (strip.pluginId);
        }

        void refresh()
        {
            inSwitch.setToggleState (isIn(), juce::dontSendNotification);

            if (! isEq)
            {
                inSwitch.setTooltip (strip.present()
                                       ? "Whether " + juce::String (strip.name) + " is in this cue's signal"
                                       : "Put " + juce::String (strip.name) + " in this cue's signal "
                                         "- the first switch-in makes the insert");
                door.setTooltip ("Open " + juce::String (strip.name) + "'s own window for this cue");
            }

            repaint();
        }

        juce::Rectangle<int> inner() const
        {
            return getLocalBounds().reduced (owner.scaled (8), owner.scaled (6));
        }

        void resized() override
        {
            auto area = inner();
            area.removeFromTop (owner.scaled (20));                          // the name
            inSwitch.setBounds (area.removeFromTop (owner.scaled (22)).withWidth (owner.scaled (70)));
            door.setBounds (area.removeFromBottom (owner.scaled (24)).withWidth (owner.scaled (90)));
        }

        void paint (juce::Graphics& g) override
        {
            const auto in = isIn();
            const auto bounds = getLocalBounds().toFloat().reduced (0.5f);

            /*  IN IS A RAISED BOX WITH A BRIGHTER EDGE, out is a recessed one
                with the rule's - and the switch's tick says it in the one
                place that is not a shade. */
            g.setColour (Look::colour (owner.theme, in ? "panel-high" : "panel-in"));
            g.fillRoundedRectangle (bounds, 4.0f);
            g.setColour (Look::colour (owner.theme, in ? "ink-faint" : "rule"));
            g.drawRoundedRectangle (bounds, 4.0f, in ? 1.5f : 1.0f);

            auto area = inner();

            g.setColour (Look::colour (owner.theme, in ? "ink" : "ink-dim"));
            g.setFont (Look::font (owner.theme, 14.0f));
            g.drawFittedText (isEq ? juce::String ("EQ")
                                   : juce::String (strip.index + 1) + ". " + juce::String (strip.name),
                              area.removeFromTop (owner.scaled (20)), juce::Justification::centredLeft, 1);

            area.removeFromTop (owner.scaled (22));                          // the switch
            area.removeFromBottom (owner.scaled (28));                       // the door
            area.removeFromTop (owner.scaled (4));

            /*  THE WORDS, under the switch: what it is doing, then anything
                the client knows of the plugin's own window. Clipped to the
                box rather than trusted to be short (a `problem` is the
                engine's sentence, and a client never hands an unbounded
                engine string to a fixed-size control). */
            const auto line = owner.scaled (16);
            g.setFont (Look::font (owner.theme, 12.0f));

            if (isEq)
            {
                g.setColour (Look::colour (owner.theme, "ink-dim"));
                g.drawFittedText (in ? eqSummary (owner.reading.eq.settings)
                                     : juce::String ("out: the file is not shaped"),
                                  area.removeFromTop (line * 2), juce::Justification::topLeft, 2);
                return;
            }

            /*  AS MANY LINES AS THE SENTENCE NEEDS, up to three: "loaded" is
                one line and the next words sit right under it, while a long
                `problem` gets the room it takes and is cut after that. */
            const auto sentence = juce::String (model::stateSentence (strip));
            const auto font = Look::font (owner.theme, 12.0f);
            const auto width = juce::jmax (1.0f, static_cast<float> (area.getWidth()));
            const auto lines = juce::jlimit (1, 3, static_cast<int> (std::ceil (
                                   juce::GlyphArrangement::getStringWidth (font, sentence) * 1.1f / width)));

            g.setColour (Look::colour (owner.theme, stateColour (strip.state)));
            g.drawFittedText (sentence, area.removeFromTop (line * lines), juce::Justification::topLeft, lines);

            if (const auto late = model::latencyWords (strip); ! late.empty() && area.getHeight() >= line)
            {
                g.setColour (Look::colour (owner.theme, "ink-faint"));
                g.drawFittedText (juce::String (late), area.removeFromTop (line),
                                  juce::Justification::topLeft, 1);
            }

            if (const auto words = owner.editorWords.find (strip.pluginId);
                words != owner.editorWords.end() && ! words->second.empty() && area.getHeight() >= line)
            {
                g.setColour (Look::colour (owner.theme, "ink-faint"));
                g.drawFittedText (juce::String (words->second), area, juce::Justification::topLeft,
                                  juce::jmax (1, area.getHeight() / line));
            }
        }

        FxPanelComponent& owner;
        const bool isEq;
        model::FxStrip strip;

        juce::ToggleButton inSwitch;
        juce::TextButton door;
    };

    //==============================================================================
    /*  WHAT THE BOXES SIT ON, and the one thing no box can draw: the arrows
        between them, from "file" to "out", which is what makes a row of boxes
        read as a signal path rather than as a list. */
    struct FxPanelComponent::Canvas final : public juce::Component
    {
        explicit Canvas (FxPanelComponent& ownerToUse) : owner (ownerToUse)
        {
            setInterceptsMouseClicks (false, true);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Look::colour (owner.theme, "panel"));

            if (owner.boxes.empty())
                return;

            const auto middle = static_cast<float> (getHeight()) / 2.0f;
            const auto ink = Look::colour (owner.theme, "ink-faint");

            const auto arrow = [&] (int fromX, int toX)
            {
                const auto x0 = static_cast<float> (fromX) + 4.0f;
                const auto x1 = static_cast<float> (toX) - 4.0f;

                if (x1 <= x0)
                    return;

                g.setColour (ink);
                g.drawLine (x0, middle, x1 - 5.0f, middle, 1.5f);

                juce::Path head;
                head.addTriangle (x1, middle, x1 - 7.0f, middle - 4.0f, x1 - 7.0f, middle + 4.0f);
                g.fillPath (head);
            };

            g.setFont (Look::font (owner.theme, 12.0f));

            //  "file", then an arrow into the first box.
            const auto first = owner.boxes.front()->getBounds();
            const auto fileArea = juce::Rectangle<int> (owner.scaled (margin), 0, owner.scaled (endWidth), getHeight());
            g.setColour (Look::colour (owner.theme, "ink-dim"));
            g.drawText ("file", fileArea, juce::Justification::centred, false);
            arrow (fileArea.getRight(), first.getX());

            for (std::size_t at = 1; at < owner.boxes.size(); ++at)
                arrow (owner.boxes[at - 1]->getRight(), owner.boxes[at]->getX());

            //  An arrow out of the last box, and "out".
            const auto last = owner.boxes.back()->getBounds();
            const auto outArea = juce::Rectangle<int> (last.getRight() + owner.scaled (linkWidth), 0,
                                                       owner.scaled (endWidth), getHeight());
            arrow (last.getRight(), outArea.getX());
            g.setColour (Look::colour (owner.theme, "ink-dim"));
            g.drawText ("out", outArea, juce::Justification::centred, false);

            /*  A SHOW WITH NO PLUGINS STILL HAS A CHAIN - the EQ is on every
                voice - so the boxes are drawn and the sentence that says how
                to add a plugin goes after them rather than instead of them. */
            /*  AND WHAT THE INSERTS DO TO THE CUE (2026-09-26): how wide it
                comes out, how late - where no sentence about the set is due. */
            const auto said = owner.reading.fx.notice.empty() ? model::chainWords (owner.reading.fx)
                                                               : owner.reading.fx.notice;

            if (! said.empty())
            {
                g.setColour (Look::colour (owner.theme, "ink-dim"));
                g.setFont (Look::font (owner.theme, 13.0f));
                g.drawFittedText (juce::String (said),
                                  juce::Rectangle<int> (outArea.getRight() + owner.scaled (16), 0,
                                                        juce::jmax (0, getWidth() - outArea.getRight()
                                                                         - owner.scaled (24)),
                                                        getHeight()),
                                  juce::Justification::centredLeft, 3);
            }
        }

        FxPanelComponent& owner;
    };

    //==============================================================================
    FxPanelComponent::FxPanelComponent (const model::Theme& themeToUse, Actions actionsToUse)
        : theme (themeToUse), actions (std::move (actionsToUse))
    {
        canvas = std::make_unique<Canvas> (*this);

        viewport.setViewedComponent (canvas.get(), false);
        viewport.setScrollBarsShown (false, true);
        viewport.setWantsKeyboardFocus (false);
        addAndMakeVisible (viewport);
    }

    FxPanelComponent::~FxPanelComponent()
    {
        viewport.setViewedComponent (nullptr, false);
    }

    int FxPanelComponent::scaled (int base) const
    {
        return juce::roundToInt (static_cast<double> (base) * theme.type);
    }

    void FxPanelComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;
        layOut();
        repaint();
    }

    //==============================================================================
    std::string FxPanelComponent::shapeOf() const
    {
        std::string out = reading.subject.objectId + "!" + (reading.fx.present ? "1" : "0");

        for (const auto& strip : reading.fx.strips)
            out += "|" + strip.pluginId;

        return out;
    }

    void FxPanelComponent::show (const model::FootReading& readingToUse)
    {
        reading = readingToUse;

        /*  A DIFFERENT CUE FORGETS WHAT WAS ASKED OF THE LAST ONE: a create
            still in flight belongs to the cue it was pressed on, and must
            not light a switch on this one. */
        if (reading.subject.objectId != creatingFor)
        {
            creating.clear();
            creatingFor = reading.subject.objectId;
        }

        //  And an entry the tree now shows as present needs no more waiting for.
        for (const auto& strip : reading.fx.strips)
            if (strip.present())
                creating.erase (strip.pluginId);

        if (const auto drawn = shapeOf(); drawn != shape)
        {
            shape = drawn;
            rebuild();
        }
        else
        {
            refresh();
        }
    }

    void FxPanelComponent::setEditorWords (std::map<std::string, std::string> words)
    {
        if (words == editorWords)
            return;

        editorWords = std::move (words);

        for (auto& box : boxes)
            box->repaint();
    }

    void FxPanelComponent::rebuild()
    {
        boxes.clear();

        /*  NO CHAIN, NO CANVAS: the sentence saying why is painted by the
            panel itself, and a canvas left on top would cover it. */
        viewport.setVisible (reading.fx.present);

        if (reading.fx.present)
        {
            boxes.push_back (std::make_unique<Box> (*this, true));

            for (const auto& strip : reading.fx.strips)
            {
                auto box = std::make_unique<Box> (*this, false);
                box->strip = strip;
                boxes.push_back (std::move (box));
            }

            for (auto& box : boxes)
                canvas->addAndMakeVisible (*box);
        }

        layOut();
        refresh();
    }

    void FxPanelComponent::refresh()
    {
        /*  THE BOXES ARE THE SAME ONES, so each takes its entry's new reading
            - the order is the set's, which is what `shape` compared. */
        for (std::size_t at = 1; at < boxes.size() && at - 1 < reading.fx.strips.size(); ++at)
            boxes[at]->strip = reading.fx.strips[at - 1];

        for (auto& box : boxes)
            box->refresh();

        canvas->repaint();
        repaint();
    }

    //==============================================================================
    bool FxPanelComponent::pending (const std::string& pluginId) const
    {
        const auto found = creating.find (pluginId);

        return found != creating.end()
                 && juce::Time::getMillisecondCounter() - found->second < pendingMs;
    }

    void FxPanelComponent::switchPlugin (const model::FxStrip& strip, bool on)
    {
        const auto& cueId = reading.subject.objectId;

        if (cueId.empty())
            return;

        if (strip.present())
        {
            if (actions.set)
                actions.set (model::fxAddress (strip.fxId, "enabled"), on ? "true" : "false");

            return;
        }

        /*  NOT THERE YET: the first switch-in is the create and nothing else.
            A second press while it is in flight sends nothing - `fx.create`
            on an entry the cue already has is refused - and a switch pressed
            off before the tree caught up simply shows the truth on the next
            pass. */
        if (! on || pending (strip.pluginId))
        {
            for (auto& box : boxes)
                box->refresh();

            return;
        }

        creating[strip.pluginId] = juce::Time::getMillisecondCounter();

        if (actions.createFx)
            actions.createFx (cueId, strip.pluginId);
    }

    //==============================================================================
    int FxPanelComponent::wantedWidth() const
    {
        if (boxes.empty())
            return 0;

        const auto count = static_cast<int> (boxes.size());

        /*  file, then an arrow and a box per link, then an arrow and out -
            and room after it for the sentence a show with no plugins says. */
        return scaled (margin) + scaled (endWidth)
               + count * (scaled (linkWidth) + scaled (boxWidth))
               + scaled (linkWidth) + scaled (endWidth) + scaled (margin)
               + (reading.fx.notice.empty() ? 0 : scaled (360));
    }

    void FxPanelComponent::layOut()
    {
        /*  AS WIDE AS THE CHAIN, and at least as wide as the panel; as tall as
            the viewport shows, asked twice because the sideways scrollbar
            appearing is what decides how tall that is (the surface panel's
            idiom). */
        canvas->setSize (juce::jmax (1, wantedWidth()), juce::jmax (1, viewport.getHeight()));
        canvas->setSize (juce::jmax (wantedWidth(), viewport.getMaximumVisibleWidth()),
                         juce::jmax (1, viewport.getMaximumVisibleHeight()));

        auto x = scaled (margin) + scaled (endWidth);
        const auto height = juce::jmax (0, canvas->getHeight() - 2 * scaled (4));

        for (auto& box : boxes)
        {
            x += scaled (linkWidth);
            box->setBounds (x, scaled (4), scaled (boxWidth), height);
            x += scaled (boxWidth);
        }

        canvas->repaint();
    }

    void FxPanelComponent::resized()
    {
        viewport.setBounds (getLocalBounds());
        layOut();
    }

    void FxPanelComponent::paint (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "panel"));

        if (reading.fx.present)
            return;

        /*  NOT A MEDIA CUE: no chain at all, and the sentence says why rather
            than the panel going blank (the EQ panel's rule). */
        g.setColour (Look::colour (theme, "ink-dim"));
        g.setFont (Look::font (theme, 13.0f));
        g.drawFittedText (juce::String (reading.fx.notice.empty() ? reading.notice : reading.fx.notice),
                          getLocalBounds().reduced (scaled (12), scaled (6)),
                          juce::Justification::centredLeft, 3);
    }
}
