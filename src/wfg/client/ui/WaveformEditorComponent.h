/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/*
    A MEDIA CUE'S FILE, drawn wide and editable: the coloured waveform, its
    ranges, and the in-points, out-points and loop slices somebody places on it.

    The author asked for this on 2026-09-21: *"coloured zoomable waveform to
    place the in and out points and slice separator for loops"*. The running
    pane has drawn a coloured bar since `b87bef7`, but it is a READOUT the
    width of a strip and spans the whole file; this is the same picture given
    room, a zoom, and handles.

    THE COLOURS ARE THE ENGINE'S and not this component's. §3.30's analysis
    gives every slice of every file a hue from its spectral centroid, a
    saturation from its flatness and a peak that is the shape; `model/Waveform`
    buckets that into one column per pixel and this draws it. There is no
    signal processing here and there must never be any - a window that decided
    for itself what a file looks like would be a second answer to a question
    the engine has already answered.

    AND THE ARITHMETIC IS THE MODEL'S. Where a window is looking (`model/View`),
    which edge the pointer is over and what dragging it writes
    (`model/Ranges`) are all pure and tested with no window. What is left here
    is hit radius, colour, and turning a gesture into the `node.set` the model
    named - which is the same division the cue list's drag already uses.
*/

#include <wfg/client/model/Foot.h>
#include <wfg/client/model/Lane.h>
#include <wfg/client/model/Ranges.h>
#include <wfg/client/model/Theme.h>
#include <wfg/client/model/View.h>
#include <wfg/client/model/Waveform.h>
#include <wfg/client/ui/RangeTableComponent.h>
#include <wfg/engine/audio/MediaInfo.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wfg::client::ui
{
    class WaveformEditorComponent final : public juce::Component
    {
    public:
        struct Actions
        {
            /** One `node.set` per write the drag implied. */
            std::function<void (const std::string& address, const std::string& text)> set;

            /** A sentence under the bar, or nothing to clear it. */
            std::function<void (const juce::String&)> say;

            /** `range.create` on this cue, over that span, from the table's `+`. */
            std::function<void (const std::string& cueId, double in, double out)> createRange;

            /** `object.delete` on one range, from the table's cross. */
            std::function<void (const std::string& rangeId)> removeRange;

            /** `range.split` at the playhead, from the table's plus. */
            std::function<void (const std::string& cueId, double at)> splitRange;

            /*  THE THREE THE TRANSPORT NEEDS. `play` fires this cue - the real
                one, through its own routing - `stop` kills the run it made,
                and `seek` moves that run to a second of its file. All three
                are named commands the surface could send too (4.11); nothing
                here is a private channel to the audio. */
            std::function<void (const std::string& cueId)> play;
            std::function<void (const std::string& runId)> stop;
            std::function<void (const std::string& runId, double seconds)> seek;
        };

        WaveformEditorComponent (const model::Theme&, Actions);

        void applyTheme (const model::Theme&);

        /*  WHERE THE TABLE'S COLUMN BEGINS, so the split under the window
            lines up with the split across it: the same boundary the panes
            above already draw between the inspector and the running cues, and
            the same air either side of it. Nought leaves the table asking for
            the width it wants. */
        void setRightColumn (int width, int gap);

        /** The reading for this pass, and the analyser's table the columns come from. */
        void show (const model::FootReading&, std::shared_ptr<const audio::MediaRecords>);

        void paint (juce::Graphics&) override;
        void resized() override;

        /*  WHERE A LEVEL POINT IS DRAWN, in this component's pixels - the one
            place that says so, which the painter draws at and a test aims its
            clicks with, so the two cannot disagree about where a point is. A
            point at nought on a lane nobody has drawn is on the unity line. */
        juce::Point<float> pointPosition (const model::LanePoint&) const;

        void mouseMove (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    private:
        /*  WHAT IS LEFT FOR THE PICTURE once the table has its column. The bar
            and the ruler are both cut from this and never from the whole
            component, or a drag aimed at the last second of the file would
            land under the numbers. */
        juce::Rectangle<int> pictureArea() const;
        int tableWidth() const;

        juce::Rectangle<int> headArea() const;
        juce::Rectangle<int> barArea() const;
        juce::Rectangle<int> rulerArea() const;

        /** Where the head is: the run's position when one is sounding, else the point. */
        double headSeconds() const;

        void moveHeadTo (int x, bool letGo);
        void sayWhichWayTheTransportGoes();

        double secondsAt (int x) const;
        double toleranceSeconds() const;

        const std::vector<model::Column>& columns();
        void paintHead (juce::Graphics&, juce::Rectangle<int>);
        void paintBar (juce::Graphics&, juce::Rectangle<int>);
        void paintRanges (juce::Graphics&, juce::Rectangle<int>);
        void paintLane (juce::Graphics&, juce::Rectangle<int>);
        void paintRuler (juce::Graphics&, juce::Rectangle<int>);

        /*  THE LEVEL LANE (namespace draft §20.5): the points as the hand has
            them while a gesture runs, else as the reading has them; where a
            height of the bar is on the fader's throw and back; which point is
            under the pointer; and the one write a gesture sends. */
        std::vector<model::LanePoint> lane() const;
        double heightAt (int y) const;
        float yForLevel (double levelDb) const;
        double toleranceHeight() const;
        std::size_t laneHit (juce::Point<int>) const;
        void sendLane (const std::vector<model::LanePoint>&);
        void showPicked();
        juce::Rectangle<int> pointBoxes() const;

        model::Theme theme;
        Actions actions;

        /*  THE SAME THREE NUMBERS, TYPED. Owned here rather than beside this
            in the panel because they are one subject - "the waveform, in and
            out points" - and a host that had to lay out two halves of one
            editor would be a host that knew what a waveform is. */
        std::unique_ptr<RangeTableComponent> table;

        model::FootReading reading;
        std::shared_ptr<const audio::MediaRecords> media;

        model::View view;
        std::string viewFile;   ///< which file `view` was reset for

        /*  The columns for the window as it stands, kept until the window or
            the width moves. Keyed on all three because any of them changes
            what the picture is. */
        std::vector<model::Column> bars;
        std::string barsFile;
        int barsWidth = 0;
        double barsFrom = 0.0, barsTo = 0.0;

        int columnWidth = 0, columnGap = 0;

        /*  THE POINT: where playing would start, and where the head sits when
            nothing of this cue is sounding. Placed by a press in the ruler,
            which is the strip a time ruler is for (author, 2026-09-21: *"the
            playhead can be controlled in the time ruler"*), and kept across
            passes so a chosen start survives a stop.

            `askedToPlay` is the other half of a play from somewhere other than
            the top: `cue.fire` makes a run and the run turns up on a later
            pass, so the seek is sent when it does. The same "find what it
            made" shape the importer uses, and for the same reason - the
            engine draws the identifiers, not the window. */
        double point = 0.0;
        bool askedToPlay = false;
        std::string playingRun;

        /*  ONE SEEK PER POSITION THE HAND SETTLES ON, not one per pixel: a
            drag makes a hundred events a second and each `run.seek` stops and
            re-asks a voice. The same rule `model::Scrub` states for the
            running pane's strip, kept by hand here because the gesture is a
            different one - a wide, zoomable ruler at 1:1 rather than a thin
            strip with gearing. */
        double sentSeek = -1.0;
        double sentAtMs = 0.0;

        juce::TextButton transport;

        model::Hit hover;
        model::Hit grabbed;

        /*  THE LANE AS THE HAND HAS IT. A drag moves a copy and sends the
            whole list ONCE, on release (the fade editor's rule: a write per
            mouse move would be a hundred undo steps a second), and the copy
            is drawn until the reading says the same - so the point does not
            jump back to where it was for the few passes the write takes to
            come round. It lets go the moment the reading MOVES - to what was
            sent, or to somebody else's edit, which is newer than the hand's -
            and `sentPasses` gives up on a write a refusal means never comes. */
        std::optional<std::vector<model::LanePoint>> held;
        std::string laneBeforeSend;
        int sentPasses = 0;

        static constexpr std::size_t noPoint = static_cast<std::size_t> (-1);

        std::size_t grabbedPoint = noPoint;
        std::size_t hoverPoint = noPoint;
        std::size_t pickedPoint = noPoint;   ///< whose numbers the head row's boxes show
        bool pointMoved = false;

        /*  THE PICKED POINT'S TWO NUMBERS, typed (PRD §3.10: "breakpoint lists
            with numeric entry, not only draggable pixels"): where it is in the
            file, and the offset it asks for there. */
        juce::Label pointAt;
        juce::Label pointLevel;
        bool panning = false;
        bool onRuler = false;    ///< the press started in the time ruler, so it moves the head
        int panFrom = 0;
    };
}
