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
    The running pane: what the show is doing, now.

    Beside the cue list, which is what the show WILL do. The two answer
    different questions and the page keeps them apart for that reason - Didi is
    the document, Gogo is the present tense - and so does this.

    IT REBUILDS EVERY PASS, unlike the cue list. Runs appear and end on any
    tick and there is no revision to key them on, because a run is not a
    decision anybody recorded (§4.10). A dozen rows twenty-five times a second
    is nothing; the same treatment on five hundred cue rows would be the whole
    budget, which is why only one of these two panes has a cache.

    A KILL IS A CLICK ON THE ROW'S RIGHT EDGE, where the cross is drawn - the
    mirror of the cue list's park gutter on the left, and for the same reason:
    a gesture that acts on the whole row is one nobody can aim.
*/

#include <wfg/client/model/RunModel.h>
#include <wfg/client/model/Scrub.h>
#include <wfg/client/model/Waveform.h>
#include <wfg/engine/audio/MediaInfo.h>
#include <wfg/client/model/Theme.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <string>
#include <map>
#include <memory>
#include <vector>

namespace wfg::client::ui
{
    /*  NOT A ListBox ANY MORE, and for one reason: a ListBox has one row
        height for every row, and this pane wants two. A run with a waveform
        is tall so the picture can be read; a wait, a fade, an OSC cue is one
        line (author, 2026-09-18: "only the active cues with the waveform
        should be taller, not the OSC, MIDI, fade cues - these can stay low
        profile"). So the rows are painted by hand on a canvas inside a
        viewport, each at its own height, which is cheap for the dozen rows a
        busy pane holds and repaints whole every pass by design. */
    class RunPaneComponent final : public juce::Component, private juce::ListBoxModel
    {
    public:
        struct Actions
        {
            std::function<void (const std::string&)> kill;

            /** A scrub settling on a second of a run: one per position the hand rests at, one on release. */
            std::function<void (const std::string&, double)> seek;
            std::function<void (const std::string&)> inspectError;

            /*  A click on a media run's name (author, 2026-09-25): the cue a
                surface's rotaries edit on their EQ and Send pages - the cue's
                identifier, or empty when it was already the one. */
            std::function<void (const std::string&)> aim;
        };

        RunPaneComponent (const model::Theme& theme, Actions actions);

        /*  A CLICK AT A POINT OF THE ROWS, as the mouse's release makes one when
            no scrub was taken: the cross kills, a media run's name line aims
            the surfaces' rotaries. Public so a test clicks where a hand would. */
        void clickAt (int x, int y);

        /*  The runs this pass found, and the analyser's table to draw their
            waveforms from. Cheap when they are the ones already drawn.

            THE TABLE IS HANDED IN RATHER THAN LOOKED UP: it is an immutable
            snapshot the analyser thread published, so holding one for a pass
            costs a pointer copy and cannot tear - the same bargain the
            parameter tree's snapshot makes. Null is ordinary and means no
            analysis yet, which is drawn as no bar. */
        void show (std::vector<model::RunRow> runs,
                   std::shared_ptr<const audio::MediaRecords> media);

        void applyTheme (const model::Theme& theme);
        void setEditing (bool editable) { editing = editable; errorList.repaint(); }

        void paint (juce::Graphics& g) override;
        void resized() override;

    private:
        int getNumRows() override;
        juce::Component* refreshComponentForRow (int row, bool selected, juce::Component* existing) override;
        void dismissError (int row);
        class ErrorControls final : public juce::Component
        {
        public:
            explicit ErrorControls (RunPaneComponent& pane) : owner (pane)
            {
                setInterceptsMouseClicks (false, true);
                addAndMakeVisible (close);
                close.setWantsKeyboardFocus (false);
                close.setTooltip ("Dismiss this error");
                close.onClick = [this] { owner.dismissError (row); };
            }
            void resized() override { close.setBounds (getLocalBounds().removeFromRight (34).reduced (3)); }
            int row = -1;
            juce::TextButton close { "×" };
        private:
            RunPaneComponent& owner;
        };
        void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
        void listBoxItemClicked (int row, const juce::MouseEvent&) override;
        void returnKeyPressed (int row) override;
        juce::String getNameForRow (int row) override;
        juce::String getTooltipForRow (int row) override { return getNameForRow (row); }
        void updateErrors();
        void inspectError (int row);
        model::CueErrorLog errorLog;
        juce::TextButton errorToggle { "> Errors (0)" }, clearErrors { "Clear" };
        juce::ListBox errorList { "Cue errors", this };
        bool editing = true;
        /*  The rows' surface: paints each row at its own top and height by
            asking its owner, and hands clicks back the same way. */
        class Canvas final : public juce::Component
        {
        public:
            explicit Canvas (RunPaneComponent& ownerToUse) : owner (ownerToUse) {}

            void paint (juce::Graphics& g) override;
            void mouseDown (const juce::MouseEvent& event) override;
            void mouseDrag (const juce::MouseEvent& event) override;
            void mouseUp (const juce::MouseEvent& event) override;
            void mouseMove (const juce::MouseEvent& event) override;
            void mouseExit (const juce::MouseEvent& event) override;

        private:
            RunPaneComponent& owner;
        };

        void paintRow (int index, juce::Graphics& g, int width, int height);
        void clicked (const juce::MouseEvent& event);

        /*  SCRUBBING (author, 2026-09-18: "I'd like to be able to scrub
            active cues and groups"). A press on a sounding media run's strip,
            or on a running scene's row, takes the head; the drag moves it -
            1:1 inside the strip, finer above and below, sliding on at an edge
            of the window - and the arithmetic is `model::Scrub`'s, so it is
            tested with numbers. The pane sends `run.seek` for each position
            the hand settles on and once when it lets go, and draws the ghost
            head with its clock and gearing until then. The engine's own head
            keeps drawing where the sound actually is. */
        void pressed (const juce::MouseEvent& event);
        void dragged (const juce::MouseEvent& event);
        void released (const juce::MouseEvent& event);
        bool scrubbable (const model::RunRow& entry) const;
        double secondsPerPixel (const model::RunRow& entry, int stripWidth) const;
        double extentOf (const model::RunRow& entry) const;
        void sendScrub (bool letGo);
        void endScrub();
        void paintScrub (const model::RunRow& entry, juce::Graphics& g,
                         juce::Rectangle<int> strip);

        /** Where a row's strip is, in the row's own coordinates: the painter and the hit test agree. */
        juce::Rectangle<int> stripFor (const model::RunRow& entry, int width, int height) const;

        /*  THE CROSS SAYS WHAT IT WOULD STOP BEFORE IT IS PRESSED (author,
            2026-09-18: "hovering over the X of a group should highlight the
            cues that will be stopped if clicked"). A kill takes a run and
            everything under it, so the pointer resting on a cross marks that
            run's row and every descendant's, and the marking is client state
            that never reaches the engine. */
        void hovered (const juce::MouseEvent& event);
        void unhovered();
        bool wouldStop (const model::RunRow& entry) const;
        bool overCross (int x) const;

        /** A row's height: the words, plus a band for a waveform when it has one. */
        int heightOf (const model::RunRow& entry) const;
        int topOf (int index) const;
        int rowAt (int y) const;
        void layOutRows();

        /*  UNDER THE WORDS, A PICTURE OF WHAT IS SOUNDING (author,
            2026-09-18). A media run gets its file's waveform with a playhead
            on it; a run in a pre-wait or a post-wait gets a bar that EMPTIES
            right to left, which is a countdown and not a progress bar - what
            somebody watching a wait wants is how long until it fires.

            THE COLUMNS ARE CACHED, keyed by the file and thrown away when the
            pane's width changes. Recomputing them is cheap, but this pane
            repaints whole at twenty-five passes a second by design, and M25's
            lesson was that the cost nobody measures is the one that bites: a
            bar is the same picture every pass until the bar itself moves. */
        void paintStrip (const model::RunRow& entry, juce::Graphics& g,
                         juce::Rectangle<int> strip, juce::Colour tint, bool layered);

        /*  Whether this run has a picture to draw, which is what decides
            between a band of its own and a mark behind the words. */
        bool hasWaveform (const model::RunRow& entry) const;

        /*  HOW LONG THE FILE IS, for the playhead: the tree's `duration` when
            the show knew the file at opening, else the analyser's own
            measurement - a file imported this session has no duration in the
            tree until the show is next opened, and the author's first long
            track showed no head at all for that reason (2026-09-18). */
        double lengthOf (const model::RunRow& entry) const;

        /** Where the head sits over the stretch this cue plays, in [0, 1]. */
        double throughOf (const model::RunRow& entry) const;
        bool paintWaveform (const model::RunRow& entry, juce::Graphics& g,
                            juce::Rectangle<int> strip);
        void paintCountdown (const model::RunRow& entry, juce::Graphics& g,
                             juce::Rectangle<int> strip, bool layered);
        void paintCursor (const model::RunRow& entry, juce::Graphics& g,
                          juce::Rectangle<int> strip, juce::Colour tint, bool layered);
        const std::vector<model::Column>& columnsFor (const std::string& file, int width,
                                                      double from, double to);

        Actions actions;
        model::Theme theme;
        juce::Viewport viewport;
        Canvas canvas { *this };
        std::vector<model::RunRow> rows;

        /** The run whose cross the pointer rests on; empty when none. */
        std::string hoverKill;

        /** The run whose strip the pointer rests on and could scrub; empty when none. */
        std::string hoverScrub;

        model::Scrub scrub;
        std::string scrubRun;
        juce::Rectangle<int> scrubStrip;   ///< on the canvas
        double scrubDistance = 0.0;
        int scrubPush = 0;                 ///< -1, 0 or +1: which edge the pointer is against

        std::shared_ptr<const audio::MediaRecords> media;
        /*  KEYED ON THE WINDOW AS WELL AS THE FILE. Two cues can play two
            stretches of one recording, and since 2026-09-21 the strip draws
            the stretch rather than the whole thing - so the file's name alone
            no longer says which picture this is. */
        std::map<std::string, std::vector<model::Column>> bars;
        int barsWidth = 0;

        int rowHeight() const noexcept;
        int stripHeight() const noexcept;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RunPaneComponent)
    };
}
