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
    The virtual surface: every strip the show declares, drawn as a desk the
    mouse can play (PRD §3.17, author's decision AB of 2026-09-23).

    THE WHOLE DESK AND NOT ONE CUE, which is why it is a window of its own and
    not a subject of the foot (plan decision 11): the foot follows the pick,
    and a desk has to stay where it is while the operator picks cues. One
    column per strip, surfaces left to right in the order the show declares
    them and each surface's name over its strips - the order a sampler group
    fills them in, so the third member of a bank is the third column.

    A COLUMN IS WHAT A D700 STRIP IS, drawn: the strip's number and what is on
    it, the engine's word for what it is doing with a colour beside it (never
    instead of it, §4.8), a fader where the strip has one, and a pad.

    THE FADER IS RIDDEN THE WAY A MOTOR FADER IS - `node.touch` on the strip's
    `target` when the hand goes down, `node.set` as it moves, `node.release`
    when it comes up - so the touch table gates the mouse exactly as it gates
    the hardware, and a fader-start works from here too (namespace draft
    §16.5, §16.7). THE PAD IS PRESSED: `strip.press` with a velocity from where
    on the pad the click landed, `strip.release` when the button comes up, and
    the keys 1 to 8 are the first eight columns at velocity 100.

    NOT AN EDITOR. A ride and a press are not decisions about the show, so the
    panel keeps working under the lock; nothing here opens an undo step.

    It reads only what `Client.cpp`'s timer hands it - the one call site that
    takes a snapshot (`check-client-boundary.py`, rule c).
*/

#include <wfg/client/model/Surfaces.h>
#include <wfg/client/model/Theme.h>
#include <wfg/engine/command/Event.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::ui
{
    class SurfacePanelComponent final : public juce::Component
    {
    public:
        /*  Every write leaves through `send`, as an Event built by a gesture
            (model/Gestures.h), origin `window`. */
        SurfacePanelComponent (const model::Theme& themeToUse, std::function<void (Event)> sendToUse);
        ~SurfacePanelComponent() override;

        /*  The surfaces and strips this pass found. Also the clock the fader
            writes ride: a value dragged since the last pass goes out here, so
            a moving fader sends at most one `node.set` a pass. */
        void show (const std::vector<model::SurfaceRow>& surfacesNow,
                   const std::vector<model::StripRow>& stripsNow);

        /** How many columns are drawn: every strip of every surface. */
        std::size_t columnCount() const noexcept { return strips.size(); }

        /** The width every column needs, band gaps included, before any scrolling. */
        int wantedWidth() const;

        /*  THE GESTURES, which the mouse and the keys call and a test calls
            directly. A column is counted from nought, in the order drawn.

            `height` is how far up the pad the press landed, nought at the
            bottom edge and one at the top: the top is a hard hit (127) and the
            bottom a soft one (1). `fraction` is a place on the fader's throw,
            nought at the bottom, on `model/Fader.h`'s curve; the first drag of
            a fader touches it, and `endFader` sends whatever the last pass has
            not yet sent and then lets go. */
        void pressPad (std::size_t column, double height);
        void releasePad (std::size_t column);
        void dragFader (std::size_t column, double fraction);
        void endFader (std::size_t column);

        /*  EVERY HAND OFF AT ONCE: the window closing, or losing the keyboard
            while a key was holding a pad down - whose key-up would otherwise
            never arrive, leaving a hold clip sounding for ever. */
        void releaseEverything();

        void paint (juce::Graphics& g) override;
        void resized() override;
        bool keyPressed (const juce::KeyPress& key) override;
        bool keyStateChanged (bool isKeyDown) override;
        void focusLost (FocusChangeType) override;

    private:
        /*  The columns' surface, inside a viewport: a desk wider than the
            window scrolls sideways rather than squeezing its strips. */
        class Canvas final : public juce::Component
        {
        public:
            explicit Canvas (SurfacePanelComponent& ownerToUse) : owner (ownerToUse) {}

            void paint (juce::Graphics& g) override;
            void mouseDown (const juce::MouseEvent& event) override;
            void mouseDrag (const juce::MouseEvent& event) override;
            void mouseUp (const juce::MouseEvent& event) override;

        private:
            SurfacePanelComponent& owner;
        };

        /*  A surface's name over its strips: the first column it covers and
            how many. */
        struct Band
        {
            std::string label;
            std::size_t first = 0;
            std::size_t count = 0;
        };

        /*  WHERE EACH PART OF A COLUMN IS, in the canvas's coordinates. One
            function for the painter and the hit test, so a click cannot land
            somewhere the eye says is another part. */
        struct Parts
        {
            juce::Rectangle<int> column, number, label, word, fader, value, pad;
        };

        /*  THE FADER A HAND IS ON: the address it touched - kept for the
            whole ride, so the touch and the release always name the same node
            even if the strip changes hands under the hand - where it was when
            the hand went down, and where the hand wants it now. */
        struct Grab
        {
            std::string strip;
            std::string target;
            double heldDb = 0.0;
            double wantedDb = 0.0;
            bool unsent = false;
            float fromY = 0.0f;
        };

        /*  A PAD HELD DOWN, by the mouse or by a key: which strip, and whether
            letting go is a `strip.release` - a dca strip's pad resets its trim
            and has nothing to let go of. */
        struct Down
        {
            std::string strip;
            bool letGo = false;
            int keyCode = 0;
        };

        Parts partsOf (std::size_t column) const;
        juce::Rectangle<int> throwOf (const Parts& parts) const;
        std::size_t columnAt (int x) const;
        std::size_t columnOf (const std::string& stripId) const;
        int scaled (int base) const;
        void layOut();

        void paintCanvas (juce::Graphics& g);
        void paintColumn (juce::Graphics& g, std::size_t column);
        juce::Colour wordColour (const std::string& word) const;
        juce::Colour swatchOf (const model::StripRow& strip) const;

        /** Where the fader is drawn: the hand's value while a hand is on it, else the engine's. */
        double levelShown (const model::StripRow& strip) const;

        void pressed (const juce::MouseEvent& event);
        void dragged (const juce::MouseEvent& event);
        void released (const juce::MouseEvent& event);

        /*  One press, however it was made: `strip.press` on a sampler strip,
            and a reset to unity on a dca strip. Answers what the matching
            release has to do. */
        std::optional<Down> press (std::size_t column, int velocity);
        void letGoOf (const Down& down);
        bool takeFader (std::size_t column);
        void letGoOfFader();
        void flush();

        model::Theme theme;
        std::function<void (Event)> send;

        juce::Viewport viewport;
        Canvas canvas { *this };

        std::vector<model::StripRow> strips;
        std::vector<Band> bands;
        std::vector<int> columnX;
        std::string shape;

        std::optional<Grab> grab;
        std::optional<Down> mousePad;
        std::map<std::size_t, Down> keysDown;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SurfacePanelComponent)
    };

    /*  THE WINDOW THE PANEL LIVES IN, opened from Show > Surfaces... and kept
        once made, as the settings window is. Refreshed from the one timer pass
        with the one snapshot; hidden, it reads nothing. Esc is PANIC here as it
        is in every window of this client (PRD §4.4): a window that swallowed
        it would be the one place the first level of stop did not reach. */
    class SurfaceWindow final : public juce::DocumentWindow
    {
    public:
        SurfaceWindow (const model::Theme& themeToUse, std::function<void (Event)> sendToUse,
                       std::function<void()> panicToUse = {});
        ~SurfaceWindow() override;

        void refresh (const tree::TreeSnapshot& snapshot);
        void closeButtonPressed() override;
        bool keyPressed (const juce::KeyPress& key) override;
        bool keyStateChanged (bool isKeyDown) override;
        void activeWindowStatusChanged() override;

    private:
        std::unique_ptr<SurfacePanelComponent> panel;
        std::function<void()> panic;
    };
}
