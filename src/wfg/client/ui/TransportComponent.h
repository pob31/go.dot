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
    The transport: the strip the operator's eye rests on between cues.

    Which show and whether the file holds it, where the clock is, which cue GO
    would fire, what undo would take back, whether the audio is running, and
    the last thing the engine refused - with the gestures that answer each:
    GO, save, revert, undo, redo, the lock, and the recovery offer.

    It is handed a TransportReading and never a snapshot: the model decides
    what the words are, this decides where they go. A label's setText repaints
    only when the text changed, so at twenty-five passes a second the only
    thing that redraws is the tick.

    COLOUR IS NEVER THE SOLE CARRIER (PRD §4.8). The dirty dot comes with the
    words "unsaved changes", the standby's yellow with "standby in <list>", a
    refusal's red with "error:", the lock's state with the word "locked" - and
    every disabled button has a sentence beside it saying which thing is
    unavailable, because a greyed control reports only THAT something is.

    WHAT THIS DOES NOT OFFER. While the show is locked there is no save
    gesture here - not the button and not the accelerator (§9, decision W, as
    reaffirmed 2026-09-17). The engine would accept one; this client does not
    ask, because usually nobody saves a show mid-performance.
*/

#include <wfg/client/model/Theme.h>
#include <wfg/client/model/Transport.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace wfg::client::ui
{
    class TransportComponent final : public juce::Component
    {
    public:
        /*  One per gesture, each ending in exactly one engine command - which
            is what makes §4.11 structural here rather than aspirational: a
            button that wanted two would have to be two buttons, or one
            command that does not exist yet. */
        struct Actions
        {
            std::function<void()> go;

            /*  PANIC, and Esc: one press is §4.4's graceful abort, a second
                within the window is the immediate one. Which of the two is
                the window's to read (model/Panic.h); this only says a press
                happened. */
            std::function<void()> panic;
            std::function<void()> undo;
            std::function<void()> redo;
            std::function<void()> save;
            std::function<void()> revert;
            std::function<void()> recover;
            std::function<void()> discardRecovery;
            std::function<void (bool)> setLocked;
            std::function<void()> reloadTheme;
        };

        TransportComponent (const model::Theme& theme, Actions actions);

        /** The reading to show. Cheap when nothing changed. */
        void show (const model::TransportReading& reading);

        /** Fonts and colours from the theme; the window's look-and-feel carries the rest. */
        void applyTheme (const model::Theme& theme);

        /** A sentence for the foot - a theme file's refusal, mostly. Empty clears it. */
        void setNotice (const juce::String& notice);

        /** Revert's question, asked before the revert; the button and the menu both come here. */
        void askThenRevert();

        /*  How tall this wants to be, which changes by three rows and a half
            when the recovery banner appears. The shell asks rather than
            assuming, and `onHeightChanged` tells it when to ask again. */
        int preferredHeight() const noexcept;

        std::function<void()> onHeightChanged;

        void paint (juce::Graphics& g) override;
        void resized() override;
        bool keyPressed (const juce::KeyPress& key) override;

    private:
        Actions actions;
        model::Theme theme;
        model::TransportReading last;
        bool shownOnce = false;
        bool bannerShowing = false;

        /*  The saved/unsaved line and the undo/redo line both went on the
            author's word (2026-09-18), for the headroom and because a dimmed
            button and a tooltip say the same things where somebody is already
            looking. `statusLabel` carries only the lock word now. */
        juce::Label showLabel, tickLabel, clockLabel, rateLabel,
                    listLabel, standbyLabel, notesLabel, statusLabel, errorLabel, noticeLabel;
        /*  GO and PANIC, and the banner's two. Save, revert, undo, redo and
            the lock left for the menu (author, 2026-09-18: "we can remove the
            redundant buttons"); their keys stay, and ask the reading. */
        juce::TextButton goButton { "GO" }, panicButton { "PANIC" },
                        recoverButton { "recover" }, discardButton { "discard" };

        int rowHeight() const noexcept;
        juce::Rectangle<int> bannerArea() const;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransportComponent)
    };
}
