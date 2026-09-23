/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/*
    THE PANEL AT THE FOOT OF THE WINDOW: a host, and one editor at a time.

    The author's shape for it (2026-09-21): *"the panel at the bottom opens on
    specific things not tabbed or cramming all at once"*. So there is no tab
    bar here and no view that shows everything about a cue. Something names a
    SUBJECT - the waveform of this cue, later its send levels, a fade's curve -
    and the panel opens on that, with a line saying what it is showing and for
    which cue, and a way to shut it.

    ADDING AN EDITOR IS ADDING AN EDITOR. `model::Subject::Kind` grows one
    enumerator, `build()` grows one branch, and nothing else here moves. That
    is deliberate rather than tidy-minded: the author has named what is coming
    - a VST interface for processing media files, the slots of the effects
    channels, state-machine processing channels, *"and so on"* - and a host
    that had to be rewritten for each would be a host that stopped being
    rewritten and started being worked around.

    ITS HEIGHT IS DRAGGED from the line along its top, and it is the window's
    to remember: `Shell` owns the number, because the panel is one of the
    things that shares the window's height and it should not be the one
    deciding how much of it to take.
*/

#include <wfg/client/model/Foot.h>
#include <wfg/client/model/Theme.h>
#include <wfg/client/ui/CurveEditorComponent.h>
#include <wfg/client/ui/EqPanelComponent.h>
#include <wfg/client/ui/SendMixerComponent.h>
#include <wfg/client/ui/TimelineComponent.h>
#include <wfg/client/ui/WaveformEditorComponent.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <string>

namespace wfg::client::ui
{
    class FootPanelComponent final : public juce::Component
    {
    public:
        struct Actions
        {
            std::function<void (const std::string& address, const std::string& text)> set;

            /** `range.create`, asked for by the editor the panel is showing. */
            std::function<void (const std::string& cueId, double in, double out)> createRange;

            /** `object.delete` on one of a cue's children. */
            std::function<void (const std::string& objectId)> removeObject;

            /** `range.split` at the playhead. */
            std::function<void (const std::string& cueId, double at)> splitRange;

            /** `send.create`, when a silent fader in the mixer is raised. */
            std::function<void (const std::string& cueId, const std::string& busId)> createSend;

            /** `eq.reset` on a media cue, from the EQ panel's Flat button. */
            std::function<void (const std::string& cueId)> resetEq;

            /** Point the panel at another group, from the timeline's own gestures. */
            std::function<void (const std::string& groupId)> openTimelineOn;

            /** The transport of whatever the panel is showing: fire, kill, seek. */
            std::function<void (const std::string& cueId)> play;
            std::function<void (const std::string& runId)> stop;
            std::function<void (const std::string& runId, double seconds)> seek;

            std::function<void()> close;

            /** The height changed by a drag on the top edge, in pixels. */
            std::function<void (int)> resizeBy;
        };

        FootPanelComponent (const model::Theme&, Actions);

        void applyTheme (const model::Theme&);

        /*  WHERE THE RIGHT-HAND COLUMN BEGINS AND HOW MUCH AIR IS BEFORE IT,
            told by the window so the panel's own split lines up with the one
            between the panes above it. The panel does not work it out: which
            column is where is the window's arrangement and not this one's. */
        void setRightColumn (int width, int gap);

        /** What it should be showing; `Kind::none` shuts it. */
        void open (const model::Subject&);
        const model::Subject& subject() const noexcept { return showing; }

        void show (const model::FootReading&, std::shared_ptr<const audio::MediaRecords>);

        void paint (juce::Graphics&) override;
        void resized() override;

        /** The line along the top, which is the grip as well as the edge. */
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseMove (const juce::MouseEvent&) override;

        static int gripHeight() noexcept { return 5; }

    private:
        void build();
        bool overGrip (juce::Point<int>) const;

        model::Theme theme;
        Actions actions;

        model::Subject showing;
        juce::String title, note;

        std::unique_ptr<WaveformEditorComponent> waveform;
        std::unique_ptr<SendMixerComponent> sends;
        std::unique_ptr<TimelineComponent> timeline;
        std::unique_ptr<CurveEditorComponent> curve;
        std::unique_ptr<EqPanelComponent> eq;
        juce::TextButton shut { "x" };
        int columnWidth = 0, columnGap = 0;

        bool dragging = false;
        int dragFrom = 0;
    };
}
