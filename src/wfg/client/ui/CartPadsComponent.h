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
    A cart as pads, in the cue list's place (namespace draft §125, BEZ; MT.6).

    THE TABLET'S CART TAB IN THE WINDOW: each cue of the cart a pad - its
    colour as a tab at the left, its kind's icon, its number, its name, the
    bank it belongs to - in rows that wrap, scrolled when there are more than
    fit. Lit while a hand is on it or the cue is going, and the word "going"
    says so (PRD §4.8, colour never the only telling).

    A HAND PER FINGER (BFA): every pointer presses its own pad, `cue.press`
    at the velocity of where it landed, and its lift lets go of that pad
    alone (`cue.release`) - so two fingers play two hold clips and each stops
    with its own finger. A pad another hand holds is not struck again. The
    mouse and the fingers take turns. Under the lock the pads still play: a
    press is not an edit.

    It reads only what `Client.cpp`'s timer hands it.
*/

#include <wfg/client/model/Cart.h>
#include <wfg/client/model/Pointers.h>
#include <wfg/client/model/Theme.h>
#include <wfg/engine/command/Event.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace wfg::client::ui
{
    class CartPadsComponent final : public juce::Component
    {
    public:
        /*  Every press and release leaves through `send`, as an Event built by
            a gesture (model/Gestures.h), origin `window`. */
        CartPadsComponent (const model::Theme& themeToUse, std::function<void (Event)> sendToUse);
        ~CartPadsComponent() override;

        void applyTheme (const model::Theme& themeToUse);

        /** Where presses go, given by the window once it has a door (the Shell is made first). */
        void setSend (std::function<void (Event)> sendToUse) { send = std::move (sendToUse); }

        /** The cart's pads this pass. Cheap when nothing changed. */
        void show (const std::vector<model::CartPad>& padsNow, const std::string& sayNow = {});

        std::size_t padCount() const noexcept { return pads.size(); }

        /*  A POINTER ON THE PADS, as the canvas's mouse handlers report it and
            as a test drives a finger (§125, BFE): `at` in the pads' own
            coordinates (the canvas's, before any scrolling), `dragging` how
            many pointers JUCE says are down with this one. */
        void pointerDown (int pointer, juce::Point<float> at, int dragging = 1);
        void pointerUp (int pointer);

        /** Where a pad is, in the canvas's coordinates. */
        juce::Rectangle<int> padBounds (std::size_t index) const;

        /** Every hand let go: the view going, or another list drawn. */
        void releaseEverything();

        void paint (juce::Graphics& g) override;
        void resized() override;

    private:
        class Canvas final : public juce::Component
        {
        public:
            explicit Canvas (CartPadsComponent& ownerToUse) : owner (ownerToUse) {}

            void paint (juce::Graphics& g) override;
            void mouseDown (const juce::MouseEvent& event) override;
            void mouseUp (const juce::MouseEvent& event) override;

        private:
            CartPadsComponent& owner;
        };

        void layOut();
        void paintPads (juce::Graphics& g);
        void paintPad (juce::Graphics& g, std::size_t index);
        std::size_t padAt (juce::Point<int> where) const;
        int scaled (int base) const;
        bool held (const std::string& cue) const;

        model::Theme theme;
        std::function<void (Event)> send;

        juce::Viewport viewport;
        Canvas canvas { *this };

        std::vector<model::CartPad> pads;
        std::vector<juce::Rectangle<int>> places;
        std::string said;

        //  The cue each pointer is holding down (§125, BFA).
        std::map<int, std::string> holding;
        model::PointerTurns turns;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CartPadsComponent)
    };
}
