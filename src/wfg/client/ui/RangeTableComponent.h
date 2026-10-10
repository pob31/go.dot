/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/*
    THE NUMBERS BESIDE THE PICTURE: every range of one media cue, with its in
    and out points as times, how long it is, how many times it repeats, and the
    one button that gives the next range this same length.

    The author asked for it on 2026-09-21, in these words: *"With the in out
    times for all slices (including if there's a single slice). It would be
    great to be able copy a duration from one slice to the next so the next out
    point is at the same time from the previous. Also show the repeat and
    infinite/number of repeats."*

    WHY A TABLE AND NOT JUST THE BAR. A dragged edge is how a loop point is
    FOUND, and a typed number is how it is FIXED - at 4.000 rather than at
    3.997, because the next one has to start where this one ends and the ear
    hears the difference a drag cannot reach. The two are the same three
    numbers seen twice, and a change in either shows in the other on the next
    pass, because both are drawn from the one reading the window took.

    "INCLUDING IF THERE'S A SINGLE SLICE" is the part worth saying out loud.
    With one range there is no join to drag and nothing on the bar says where
    it starts and ends except two thin lines; the table is then the only place
    those two times are legible, so it is drawn whenever the panel is - never
    only once a second range makes it look like a list.

    IT WRITES WHAT THE MODEL NAMED and judges nothing: `model/Ranges` turns a
    typed time into a number or refuses it, and says what copying a length
    would write. What is here is columns, focus, and turning a press into the
    `node.set` or the command the model asked for.
*/

#include <wfg/client/model/Foot.h>
#include <wfg/client/model/Ranges.h>
#include <wfg/client/model/Theme.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wfg::client::ui
{
    class RangeTableComponent final : public juce::Component
    {
    public:
        struct Actions
        {
            /** One `node.set`: an in-point, an out-point, a name, a loop count. */
            std::function<void (const std::string& address, const std::string& text)> set;

            /** `range.create` on this cue, over that span. */
            std::function<void (const std::string& cueId, double in, double out)> createRange;

            /** `range.split` on this cue, at the second the playhead is standing on. */
            std::function<void (const std::string& cueId, double at)> splitRange;

            /** `object.delete` on one range. */
            std::function<void (const std::string& rangeId)> removeRange;

            /** A sentence in the panel's head, or nothing to clear it. */
            std::function<void (const juce::String&)> say;
        };

        RangeTableComponent (const model::Theme&, Actions);
        ~RangeTableComponent() override;

        void applyTheme (const model::Theme&);

        /** The reading for this pass. Cheap when the list has not changed shape. */
        void show (const model::FootReading&);

        /*  WHERE THE PLAYHEAD IS STANDING, which is what the plus acts on: it
            cuts the range under the head rather than appending at the end. The
            editor beside this owns the head - it is drawn on the picture and
            placed in the ruler - so it says, rather than this reading it. */
        void setPlayhead (double seconds);

        void paint (juce::Graphics&) override;
        void resized() override;

        /*  How wide this wants to be, when nobody has said. The panel above
            usually has: the table lines up with the pane boundary the window
            already draws (author, 2026-09-21: *"align and pad the waveform and
            range table with the separation between the inspector and the
            running cues"*), so this is the fallback and the minimum rather
            than the answer. */
        int wantedWidth() const;

        /*  THE BUTTON AT THE FOOT OF THE LIST (namespace draft §54), public so
            a test can find and press it. */
        juce::TextButton& restButton() noexcept { return rest; }

    private:
        struct Row;

        void rebuild();
        void refresh();
        void sayWhatThePlusWouldDo();
        void sayWhatTheRestWouldDo();
        void layOut();

        /** What the rows are, as one string: a different one is a different table. */
        std::string shapeOf() const;

        /*  THE WHOLE FILE, READY (author, 2026-09-30: "Could we have the
            complete range already there by default. It makes looping a media
            much easier this way"). A cue with no ranges shows one standing
            row - nought to the end of the file, once through - that the show
            does not hold: it is what the cue plays, drawn as the range it
            would be. Changing anything on it is the decision, and makes it:
            `range.create` over the whole file, then the change, written to the
            range once the tree has it.

            NOT MADE BY ITSELF, which is the other way this could have gone and
            the one §4.10 rules out: a range written on import would carry the
            file's length as its out-point, a fact about today's file that
            would quietly cut short a longer one put in its place. */
        struct Pending
        {
            std::string cueId, attribute, value;
        };

        std::optional<Pending> pending;
        bool standing() const noexcept;

        model::Theme theme;
        Actions actions;

        model::FootReading reading;
        double playhead = 0.0;
        std::string drawnShape;
        std::string drawnCue;

        juce::Viewport viewport;
        juce::Component content;
        juce::Label heads;
        juce::TextButton add { "+ range" };

        /*  THE REST OF THE FILE, GIVEN BACK (namespace draft §54, the author's,
            2026-10-10: "Sometimes when working on loops the end of the media
            becomes unaccessible ... a button at the end of the list if the end
            marker is not at the end to make a new section from the position of
            the end until the end of the media file"). Under the last row, and
            there only while the furthest out point stops short of the file's
            end: `range.create` from that out point to the end, so the old end
            becomes the join between two ranges and the file's end is reachable
            again. `model::nextRange` says where. */
        juce::TextButton rest;

        std::vector<std::unique_ptr<Row>> rows;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RangeTableComponent)
    };
}
