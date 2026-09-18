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
    class RunPaneComponent final : public juce::Component
    {
    public:
        struct Actions
        {
            std::function<void (const std::string&)> kill;
        };

        RunPaneComponent (const model::Theme& theme, Actions actions);

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

        void paint (juce::Graphics& g) override;
        void resized() override;

    private:
        /*  The rows' surface: paints each row at its own top and height by
            asking its owner, and hands clicks back the same way. */
        class Canvas final : public juce::Component
        {
        public:
            explicit Canvas (RunPaneComponent& ownerToUse) : owner (ownerToUse) {}

            void paint (juce::Graphics& g) override;
            void mouseUp (const juce::MouseEvent& event) override;

        private:
            RunPaneComponent& owner;
        };

        void paintRow (int index, juce::Graphics& g, int width, int height);
        void clicked (const juce::MouseEvent& event);

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
        bool paintWaveform (const model::RunRow& entry, juce::Graphics& g,
                            juce::Rectangle<int> strip);
        void paintCountdown (const model::RunRow& entry, juce::Graphics& g,
                             juce::Rectangle<int> strip, bool layered);
        void paintCursor (const model::RunRow& entry, juce::Graphics& g,
                          juce::Rectangle<int> strip, juce::Colour tint, bool layered);
        const std::vector<model::Column>& columnsFor (const std::string& file, int width);

        Actions actions;
        model::Theme theme;
        juce::Viewport viewport;
        Canvas canvas { *this };
        std::vector<model::RunRow> rows;

        std::shared_ptr<const audio::MediaRecords> media;
        std::map<std::string, std::vector<model::Column>> bars;
        int barsWidth = 0;

        int rowHeight() const noexcept;
        int stripHeight() const noexcept;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RunPaneComponent)
    };
}
