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
        constexpr int hideWidth = 104;    // the Hide unused plugins switch, two lines

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
        /*  THREE KINDS OF LINK: the EQ every voice carries, an entry of the set
            or of the channel's chain, and on a sampling channel the recorder
            (Phase 9c), which is always in and so has no switch. And a fourth
            that is no link yet: the place at the chain's end where a plugin is
            added from here (§105), with Load now while the graph lags. */
        enum class Kind { eq, plugin, recorder, add };

        Box (FxPanelComponent& ownerToUse, Kind kindToUse)
            : owner (ownerToUse), kind (kindToUse), isEq (kindToUse == Kind::eq)
        {
            inSwitch.setButtonText ("in");
            inSwitch.setWantsKeyboardFocus (false);
            door.setWantsKeyboardFocus (false);
            addAndMakeVisible (inSwitch);
            addAndMakeVisible (door);

            if (kind == Kind::add)
            {
                inSwitch.setVisible (false);
                door.setButtonText ("Add plugin...");
                door.onClick = [this] { owner.chooseToAdd (door); };

                load.setButtonText ("Load now");
                load.setWantsKeyboardFocus (false);
                load.onClick = [this]
                {
                    if (owner.actions.load && owner.reading.fx.busy.empty() && ! owner.reading.fx.locked)
                        owner.actions.load();
                };
                addChildComponent (load);
            }
            else if (kind == Kind::recorder)
            {
                inSwitch.setVisible (false);
                door.setButtonText ("Open");
                door.setTooltip ("Show the take this channel records in the panel");

                door.onClick = [this]
                {
                    if (owner.actions.openTake)
                        owner.actions.openTake (owner.reading.subject.objectId);
                };
            }
            else if (isEq)
            {
                door.setButtonText ("Open");
                inSwitch.setTooltip ("Whether this cue's EQ is in its signal");
                door.setTooltip ("Show this cue's EQ in the panel");

                inSwitch.onClick = [this]
                {
                    if (owner.actions.set && ! owner.onFade())
                        owner.actions.set (owner.reading.rowAt ("eqOn"),
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

                /*  EDIT SWITCHES IT IN (namespace draft §105, the author: "engage
                    the effects at the same time rather than punish the user to
                    close the popup to enable it and then click edit"). An insert
                    the cue has not got is made by the window itself, which waits
                    for it; one switched out is switched in here. Never on a
                    fade, which switches nothing (§94). */
                door.onClick = [this]
                {
                    if (! owner.onFade() && strip.present() && ! strip.enabled)
                        owner.switchPlugin (strip, true);

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

            if (kind == Kind::recorder)
                return true;

            return strip.present() ? strip.enabled : owner.pending (strip.pluginId);
        }

        void refresh()
        {
            if (kind == Kind::add)
            {
                const auto& fx = owner.reading.fx;

                door.setEnabled (! fx.locked);
                door.setTooltip (fx.locked ? juce::String ("The show is locked: unlock it to add a plugin")
                                           : fx.live || owner.reading.cueKind == "aux"
                                               ? juce::String ("Add a plugin at the end of this channel's chain,"
                                                               " switched in on this cue")
                                               : juce::String ("Add a plugin to the show's set, switched in on"
                                                               " this cue"));

                //  LOAD NOW WHILE THE GRAPH LAGS THE SHOW, lit only when it can be (the Rack tab's rule).
                load.setVisible (fx.graphChanged);
                load.setEnabled (fx.busy.empty() && ! fx.locked);
                load.setTooltip (! fx.busy.empty() ? juce::String (fx.busy) + ": Load now waits for nothing to sound."
                                                     " Stop it first - Esc."
                                                   : juce::String ("Rebuild the audio graph with the plugins as they"
                                                                   " stand. Only while nothing plays; the sound stops"
                                                                   " for a moment."));
                repaint();
                return;
            }

            inSwitch.setToggleState (isIn(), juce::dontSendNotification);

            if (kind == Kind::plugin)
            {
                inSwitch.setTooltip (strip.present()
                                       ? "Whether " + juce::String (strip.name) + " is in this cue's signal"
                                       : "Put " + juce::String (strip.name) + " in this cue's signal "
                                         "- the first switch-in makes the insert");
                door.setTooltip (isIn() ? "Open " + juce::String (strip.name) + "'s own window for this cue"
                                        : "Switch " + juce::String (strip.name) + " in on this cue and open its own window");
            }

            /*  ON A FADE the switch is the target's, shown and not thrown, and
                the window is the fade's (namespace draft §94). */
            const auto onFade = owner.onFade();
            inSwitch.setEnabled (! onFade);

            if (onFade && kind != Kind::recorder)
            {
                inSwitch.setTooltip ("A fade does not switch anything in or out: it moves numbers");
                door.setTooltip (isEq ? juce::String ("The target's EQ, for the bands this fade moves")
                                      : "Open " + juce::String (strip.name) + "'s own window on this fade: "
                                        "what you turn there, the fade moves");
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

            if (kind == Kind::add)
            {
                door.setBounds (area.removeFromTop (owner.scaled (24)).withWidth (owner.scaled (110)));
                load.setBounds (area.removeFromBottom (owner.scaled (24)).withWidth (owner.scaled (90)));
                return;
            }
            inSwitch.setBounds (area.removeFromTop (owner.scaled (22)).withWidth (owner.scaled (70)));
            door.setBounds (area.removeFromBottom (owner.scaled (24)).withWidth (owner.scaled (90)));
        }

        void paint (juce::Graphics& g) override
        {
            const auto bounds = getLocalBounds().toFloat().reduced (0.5f);

            /*  NOT A LINK YET: a dashed outline on the panel's own ground, and
                - while the graph lags the show - why Load now is or is not lit,
                in words. */
            if (kind == Kind::add)
            {
                juce::Path outline;
                outline.addRoundedRectangle (bounds, 4.0f);
                juce::Path dashed;
                const float dashes[] = { 4.0f, 3.0f };
                juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);
                g.setColour (Look::colour (owner.theme, "rule"));
                g.fillPath (dashed);

                auto area = inner();
                g.setColour (Look::colour (owner.theme, "ink-dim"));
                g.setFont (Look::font (owner.theme, 14.0f));
                g.drawFittedText ("+", area.removeFromTop (owner.scaled (20)), juce::Justification::centredLeft, 1);

                area.removeFromTop (owner.scaled (28));                      // Add plugin...
                area.removeFromBottom (owner.scaled (28));                   // Load now
                const auto& fx = owner.reading.fx;

                if (fx.graphChanged)
                {
                    g.setFont (Look::font (owner.theme, 12.0f));
                    g.setColour (Look::colour (owner.theme, fx.busy.empty() ? "ink-dim" : "waiting"));
                    g.drawFittedText (fx.busy.empty() ? juce::String ("Not in the audio yet: Load now builds it in.")
                                                      : juce::String (fx.busy) + ": Load now waits for"
                                                        " nothing to sound.",
                                      area, juce::Justification::topLeft, juce::jmax (1, area.getHeight() / owner.scaled (16)));
                }

                return;
            }

            const auto in = isIn();

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
                                   : kind == Kind::recorder ? juce::String::fromUTF8 ("\xe2\x97\x8f Take")
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

            /*  WHAT THE FADE MOVES HERE, first and in words (§94): the accent
                where it moves something, never the colour alone. */
            if (owner.onFade() && kind != Kind::recorder)
            {
                const auto count = owner.reading.eq.fadeMoves.size();
                const auto said = isEq ? (count == 0 ? std::string ("moves nothing")
                                                     : "moves " + std::to_string (count)
                                                         + (count == 1 ? " number" : " numbers"))
                                       : model::fadeMoveWords (strip);
                const auto moving = isEq ? count > 0 : ! strip.fadeMoves.empty();

                g.setColour (Look::colour (owner.theme, moving ? "accent" : "ink-off"));
                g.drawFittedText (juce::String (said), area.removeFromTop (line * 2), juce::Justification::topLeft, 2);
            }

            if (isEq)
            {
                g.setColour (Look::colour (owner.theme, "ink-dim"));
                g.drawFittedText (in ? eqSummary (owner.reading.eq.settings)
                                     : juce::String ("out: the file is not shaped"),
                                  area.removeFromTop (line * 2), juce::Justification::topLeft, 2);
                return;
            }

            /*  THE RECORDER SAYS WHAT ITS TAKE IS DOING, in the channel's own
                word, and what the sides mean: before it is printed, after it
                is heard. */
            if (kind == Kind::recorder)
            {
                const auto& state = owner.reading.fx.takeState;

                g.setColour (Look::colour (owner.theme, "ink-dim"));
                g.drawFittedText (juce::String ("take: ") + juce::String (state.empty() ? "empty" : state),
                                  area.removeFromTop (line), juce::Justification::topLeft, 1);
                g.setColour (Look::colour (owner.theme, "ink-faint"));
                g.drawFittedText ("printed before it, heard after", area.removeFromTop (line * 2),
                                  juce::Justification::topLeft, 2);
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
        const Kind kind;
        const bool isEq;
        model::FxStrip strip;

        juce::ToggleButton inSwitch;
        juce::TextButton door;
        juce::TextButton load;      // the add box's Load now (§105)
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

            //  "file" - or a mic cue's input (Phase 9b) - then an arrow into the first box.
            const auto first = owner.boxes.front()->getBounds();
            const auto fileArea = juce::Rectangle<int> (owner.scaled (margin), 0, owner.sourceWidth(), getHeight());
            g.setColour (Look::colour (owner.theme, "ink-dim"));
            g.drawText (juce::String (owner.reading.fx.source), fileArea, juce::Justification::centred, false);
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
            auto said = owner.reading.fx.notice.empty() ? model::chainWords (owner.reading.fx)
                                                         : owner.reading.fx.notice;

            //  And what Hide unused plugins left out (§105), so an empty-looking chain is not a mystery.
            if (owner.hiddenCount > 0)
                said += (said.empty() ? "" : "\n") + std::to_string (owner.hiddenCount)
                          + (owner.hiddenCount == 1 ? " unused plugin hidden" : " unused plugins hidden");

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

        hideUnused.setButtonText ("Hide unused plugins");
        hideUnused.setTooltip ("Leave out of this cue's chain every plugin it has not switched in."
                               " Kept with the cue.");
        hideUnused.setWantsKeyboardFocus (false);
        hideUnused.onClick = [this]
        {
            if (actions.set && offersHide())
                actions.set ("/godot/cue/" + reading.subject.objectId + "/fxHideUnused",
                             hideUnused.getToggleState() ? "true" : "false");
        };
        canvas->addChildComponent (hideUnused);

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
        std::string out = reading.subject.objectId + "!" + (reading.fx.present ? "1" : "0")
                            + (reading.fx.recorder ? "!take" : "") + (hiding() ? "!hide" : "")
                            + (offersHide() ? "!offer" : "") + (offersAdd() ? "!add" : "");

        /*  Hiding, whether the cue has an entry changes which boxes there are;
            a pending first switch-in counts as having it. */
        for (const auto& strip : reading.fx.strips)
            out += "|" + strip.pluginId + ":" + strip.side
                     + (hiding() && (strip.present() || pending (strip.pluginId)) ? "+" : "");

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

        /*  LOAD AT ONCE IF QUIET (the author's choice, §105): the first pass
            that shows the graph lagging after an add from here loads it when
            nothing sounds; when something does, Load now stays lit for a
            hand instead, saying what it waits for. Asked once per add. */
        if (loadAskedAt != 0)
        {
            if (juce::Time::getMillisecondCounter() - loadAskedAt > loadWaitMs)
            {
                loadAskedAt = 0;
            }
            else if (reading.fx.graphChanged)
            {
                loadAskedAt = 0;

                if (reading.fx.busy.empty() && ! reading.fx.locked && actions.load)
                    actions.load();
            }
        }

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

    //==============================================================================
    /*  THIS MACHINE'S PLUGINS, as a menu under the button: the Rack tab's -
        the scan's list, grouped by maker when it is long, which is how
        somebody looks for one. */
    void FxPanelComponent::chooseToAdd (juce::Component& under)
    {
        if (reading.fx.locked || ! offersAdd())
            return;

        const auto& known = reading.fx.known;
        juce::PopupMenu menu;

        if (known.empty())
            menu.addItem (-1, "Nothing scanned yet: scan in Show settings, Plugins.", false);
        else if (known.size() <= 24)
        {
            for (std::size_t at = 0; at < known.size(); ++at)
                menu.addItem (static_cast<int> (at) + 1,
                              juce::String (known[at].name) + "  (" + juce::String (known[at].format) + ")");
        }
        else
        {
            std::map<std::string, juce::PopupMenu> makers;

            for (std::size_t at = 0; at < known.size(); ++at)
                makers[known[at].manufacturer.empty() ? std::string ("Unknown maker") : known[at].manufacturer]
                    .addItem (static_cast<int> (at) + 1,
                              juce::String (known[at].name) + "  (" + juce::String (known[at].format) + ")");

            for (auto& [maker, items] : makers)
                menu.addSubMenu (juce::String (maker), items);
        }

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&under),
                            [safe = juce::Component::SafePointer<FxPanelComponent> (this),
                             cueId = reading.subject.objectId, offered = known] (int chosen)
                            {
                                if (safe == nullptr || chosen <= 0 || chosen > static_cast<int> (offered.size()))
                                    return;

                                safe->add (cueId, offered[static_cast<std::size_t> (chosen - 1)]);
                            });
    }

    void FxPanelComponent::add (const std::string& cueId, const model::KnownPluginRow& plugin)
    {
        //  The menu answers later: the pick may have moved on, and then nothing is added.
        if (cueId != reading.subject.objectId || ! offersAdd() || reading.fx.locked || ! actions.addPlugin)
            return;

        actions.addPlugin (cueId, plugin);
        loadAskedAt = std::max<std::uint32_t> (1, juce::Time::getMillisecondCounter());
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
        hiddenCount = 0;

        /*  NO CHAIN, NO CANVAS: the sentence saying why is painted by the
            panel itself, and a canvas left on top would cover it. */
        viewport.setVisible (reading.fx.present);

        if (reading.fx.present)
        {
            const auto plugin = [this] (const model::FxStrip& strip)
            {
                if (hiding() && ! strip.present() && ! pending (strip.pluginId))
                {
                    ++hiddenCount;
                    return;
                }

                auto box = std::make_unique<Box> (*this, Box::Kind::plugin);
                box->strip = strip;
                boxes.push_back (std::move (box));
            };

            /*  A SAMPLING CHANNEL (Phase 9c, namespace draft §19.2): the
                plugins before the recorder, the recorder, the EQ, then the
                plugins after the player - the order its track is built in.
                Anywhere else the EQ, then every plugin. */
            if (reading.fx.recorder)
            {
                for (const auto& strip : reading.fx.strips)
                    if (strip.side == "before")
                        plugin (strip);

                boxes.push_back (std::make_unique<Box> (*this, Box::Kind::recorder));
            }

            boxes.push_back (std::make_unique<Box> (*this, Box::Kind::eq));

            for (const auto& strip : reading.fx.strips)
                if (! reading.fx.recorder || strip.side != "before")
                    plugin (strip);

            //  WHERE THE NEXT ONE GOES, at the end (§105): not on a fade, which adds nothing.
            if (offersAdd())
                boxes.push_back (std::make_unique<Box> (*this, Box::Kind::add));

            for (auto& box : boxes)
                canvas->addAndMakeVisible (*box);
        }

        layOut();
        refresh();
    }

    void FxPanelComponent::refresh()
    {
        /*  THE BOXES ARE THE SAME ONES, so each takes its entry's new reading,
            found by the entry: the shape - entries, sides and the recorder -
            is what `shape` compared, and a sampling channel draws them in
            another order than the set's. */
        for (auto& box : boxes)
            if (box->kind == Box::Kind::plugin)
                for (const auto& strip : reading.fx.strips)
                    if (strip.pluginId == box->strip.pluginId)
                        box->strip = strip;

        for (auto& box : boxes)
            box->refresh();

        hideUnused.setVisible (reading.fx.present && offersHide());
        hideUnused.setToggleState (hiding(), juce::dontSendNotification);

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

        //  A fade's chain is shown, not switched (§94).
        if (cueId.empty() || onFade())
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
        return scaled (margin) + sourceWidth()
               + count * (scaled (linkWidth) + scaled (boxWidth))
               + scaled (linkWidth) + scaled (endWidth) + scaled (margin)
               + (reading.fx.notice.empty() && model::chainWords (reading.fx).empty() && hiddenCount == 0
                    ? 0 : scaled (360));
    }

    int FxPanelComponent::sourceWidth() const
    {
        const auto said = juce::String (reading.fx.source);
        const auto wide = juce::GlyphArrangement::getStringWidthInt (Look::font (theme, 12.0f), said) + scaled (8);

        //  Room above "file" for the cue's Hide unused plugins switch, on two lines (§105).
        return juce::jmax (scaled (offersHide() ? hideWidth : endWidth), wide);
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

        auto x = scaled (margin) + sourceWidth();
        const auto height = juce::jmax (0, canvas->getHeight() - 2 * scaled (4));

        hideUnused.setBounds (scaled (margin), scaled (4), sourceWidth(), scaled (40));

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
