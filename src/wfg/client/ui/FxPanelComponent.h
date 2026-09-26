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
    A media cue's signal chain, drawn at the foot the way the sound goes
    through it (Phase 9a; the author's design, 2026-09-25).

    FILE, EQ, THE SHOW'S PLUGINS, OUT - left to right, one box a link. That is
    the chain every voice carries (namespace draft §17): Go.dot's own EQ first,
    then the plugin set in `plugins/order`, then the output. The panel draws
    the whole set whether this cue uses an entry or not, because a panel that
    drew only the inserts the cue already has would be a panel nobody could
    switch a second one in on (Sends.h's argument).

    WHAT A BOX HOLDS: its name; a switch that puts it in this cue's signal;
    what became of it tonight, in words; and a door. The EQ's door opens the
    EQ panel in this same foot. A plugin's door is Edit..., which opens the
    plugin's OWN window (author, 2026-09-25: "show the chain, bypass switch
    and open the native plugin UI as a popup"), so there are no sliders here:
    the plugin draws its controls better than a generic list ever could.

    THE FIRST SWITCH-IN MAKES THE INSERT. A cue holds an `Fx` only for an entry
    somebody switched in, so pressing the switch on one it has not got sends
    `fx.create` - whose `enabled` defaults to true, so creating is switching
    in - and nothing else; every press after that writes `enabled`. The press
    is remembered for a moment so the switch does not flicker off while the
    tree catches up, and forgotten when the cue changes, so a slow create can
    never land on the next cue picked.

    COLOUR IS NEVER THE ONLY CARRIER (§4.8): in or out is the switch's tick
    and the box's frame, and what became of a plugin is a word and a sentence
    - "missing: ... - this cue plays it dry" - before it is a colour.

    AND NOTHING HERE TAKES THE KEYBOARD, so the space bar still GOes while a
    hand is in the chain.
*/

#include <wfg/client/model/Foot.h>
#include <wfg/client/model/Theme.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace wfg::client::ui
{
    class FxPanelComponent final : public juce::Component
    {
    public:
        struct Actions
        {
            /** One `node.set`: the EQ's switch, or an insert's. */
            std::function<void (const std::string& address, const std::string& text)> set;

            /** `fx.create` on this cue for that entry of the set: the first switch-in. */
            std::function<void (const std::string& cueId, const std::string& pluginId)> createFx;

            /** Show this cue's EQ in the foot instead of the chain. */
            std::function<void (const std::string& cueId)> openEq;

            /** Open the plugin's own window for this cue. */
            std::function<void (const std::string& cueId, const std::string& pluginId)> edit;

            /** A sentence for the panel's head. */
            std::function<void (const juce::String&)> say;
        };

        FxPanelComponent (const model::Theme&, Actions);
        ~FxPanelComponent() override;

        void applyTheme (const model::Theme&);

        /** The reading for this pass. Cheap when the chain has not changed shape. */
        void show (const model::FootReading&);

        /*  WHAT THE WINDOW KNOWS OF EACH PLUGIN'S OWN WINDOW, in words, by
            entry: "opening...", "open", "closed unexpectedly - Edit... opens
            it again". Told by the client, which owns those windows; a box with
            nothing to say draws nothing. */
        void setEditorWords (std::map<std::string, std::string>);

        void paint (juce::Graphics&) override;
        void resized() override;

        /** How long a first switch-in is trusted before the tree must agree. */
        static constexpr std::uint32_t pendingMs = 2000;

    private:
        struct Box;
        struct Canvas;

        void rebuild();
        void refresh();
        void layOut();

        /** The identity of the chain, for deciding rebuild against refresh. */
        std::string shapeOf() const;

        /** The switch on a plugin's box was pressed, and now says `on`. */
        void switchPlugin (const model::FxStrip&, bool on);

        /** Whether a first switch-in is outstanding for this entry. */
        bool pending (const std::string& pluginId) const;

        int scaled (int base) const;
        int wantedWidth() const;

        /*  How wide the chain's source is drawn: "file", or a mic cue's input
            by name (Phase 9b) - never narrower than "file" was. */
        int sourceWidth() const;

        model::Theme theme;
        Actions actions;

        model::FootReading reading;
        std::string shape;

        /*  The entries a `fx.create` has gone out for, and when - on this cue
            only: the map is emptied whenever the cue changes. */
        std::map<std::string, std::uint32_t> creating;
        std::string creatingFor;

        std::map<std::string, std::string> editorWords;

        juce::Viewport viewport;
        std::unique_ptr<Canvas> canvas;
        std::vector<std::unique_ptr<Box>> boxes;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxPanelComponent)
    };
}
