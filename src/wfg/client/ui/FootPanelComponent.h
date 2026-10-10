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
#include <wfg/client/model/Icons.h>
#include <wfg/client/model/Theme.h>
#include <wfg/client/ui/CurveEditorComponent.h>
#include <wfg/client/ui/EqPanelComponent.h>
#include <wfg/client/ui/FadeMixerComponent.h>
#include <wfg/client/ui/CurveLaneComponent.h>
#include <wfg/client/ui/OscMessagesComponent.h>
#include <wfg/client/ui/PatchCanvasComponent.h>
#include <wfg/client/ui/PicturePanelComponent.h>
#include <wfg/client/ui/FxPanelComponent.h>
#include <wfg/client/ui/Icons.h>
#include <wfg/client/ui/SendMixerComponent.h>
#include <wfg/client/ui/TakePanelComponent.h>
#include <wfg/client/ui/TimelineComponent.h>
#include <wfg/client/ui/WaveformEditorComponent.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wfg::client::ui
{
    class FootPanelComponent final : public juce::Component
    {
    public:
        struct Actions
        {
            std::function<void (const std::string& address, const std::string& text)> set;

            /*  SEVERAL VALUES AS ONE GESTURE (namespace draft §30.11): the send
                mixer and the EQ over several picked cues, one `node.setMany` a
                frame. */
            std::function<void (const std::vector<std::pair<std::string, std::string>>& writes)> setMany;

            /** `range.create`, asked for by the editor the panel is showing. */
            std::function<void (const std::string& cueId, double in, double out)> createRange;

            /** `object.delete` on one of a cue's children. */
            std::function<void (const std::string& objectId)> removeObject;

            /*  A process cue's patch opened in plugdata or Pd, and Pd offered
                where neither is on the machine (namespace draft §51, PC.7). */
            std::function<void (const std::string& cueId)> editPatch;
            std::function<void()> getPd;

            /*  Atoms for a name a running patch hears (PC.8). */
            std::function<void (const std::string& runId, const std::string& name, const process::Atoms&)> playPatch;

            /** `range.split` at the playhead. */
            std::function<void (const std::string& cueId, double at)> splitRange;

            /*  A SOUND'S EDIT (namespace draft §55): the waveform's sections row. */
            std::function<void (const std::string& cueId, double at)> splitSection;
            std::function<void (const std::string& sectionId)> joinSection;
            std::function<void (const std::string& sectionId, int index)> moveSection;
            std::function<void (const std::string& sectionId, bool leaveGap)> removeSection;
            std::function<void (const std::string& sectionId, bool inSide, double seconds)> edgeSection;
            std::function<void (const std::string& sectionId, bool inSide, double seconds, bool alone)> fadeSection;
            std::function<void (const std::string& sectionId, bool inSide, double curve, bool alone)> curveSection;
            std::function<void (const std::string& cueId, double from, double to)> splitSpan;
            std::function<void (const std::string& cueId, double from, double to, bool ripple)> deleteSpan;
            std::function<void (const std::string& cueId)> freezeEdit;
            std::function<void (const std::string& cueId)> unfreezeEdit;

            /*  AN OSC CUE'S MESSAGES AND CURVES (namespace draft 45): one more
                message, the second made the cue's own, a curve on a value. */
            std::function<void (const std::string& cueId, const std::string& address,
                                const std::string& value)> createMessage;
            std::function<void (const std::string& messageId)> promoteMessage;
            std::function<void (const std::string& parentId, int arg)> createCurve;

            /*  A MESSAGE'S ADDRESS PICKED FROM ITS DEVICE'S TREE (namespace draft
                §56, AEP): the window holds the snapshot the menu is built from. */
            std::function<void (const std::string& addressRow, const std::string& current,
                                juce::Component& under)> chooseAddress;

            /*  RECORDING AN OSC CUE'S CURVES (O.9): the cue armed or let go of,
                a curve armed or not, a pass from a second, the pass ended. */
            std::function<void (const std::string& cueId)> curveArm;
            std::function<void()> curveFree;
            std::function<void (const std::string& curveId, bool on)> curveRec;
            std::function<void (double fromSeconds)> curveRecord;
            std::function<void()> curveStop;

            /** `send.create`, when a silent fader in the mixer is raised. */
            std::function<void (const std::string& cueId, const std::string& busId, double level)> createSend;

            /** `eq.reset` on a media cue, from the EQ panel's Flat button. */
            std::function<void (const std::string& cueId)> resetEq;

            /** `fx.create`, when an entry of the set is first switched in on a cue. */
            std::function<void (const std::string& cueId, const std::string& pluginId)> createFx;

            /** Open the plugin's own window for this cue, from the chain's Edit... */
            std::function<void (const std::string& cueId, const std::string& pluginId)> editPlugin;

            /** Point the panel at another group, from the timeline's own gestures. */
            std::function<void (const std::string& groupId)> openTimelineOn;

            /** Show a cue's EQ, from the chain's EQ box. */
            std::function<void (const std::string& cueId)> openEqOn;

            /** Show another subject, from a fade's mixer: its EQ, its curve (§26). */
            std::function<void (const model::Subject&)> openSubject;

            /** Show the take a mic cue's channel records, from the chain's recorder (Phase 9c). */
            std::function<void (const std::string& cueId)> openTakeOn;

            /** The transport of whatever the panel is showing: fire, kill, seek. */
            std::function<void (const std::string& cueId)> play;
            std::function<void (const std::string& runId)> stop;
            std::function<void (const std::string& runId, double seconds)> seek;

            /*  RECORDING THE LANE FROM A FADER (namespace draft §20.9): arm this
                cue's lane (empty cancels), let the fader go, start a pass from
                a second of the file, end it. */
            std::function<void (const std::string& cueId)> laneArm;
            std::function<void()> laneFree;
            std::function<void (double fromSeconds)> laneRecord;
            std::function<void()> laneStop;

            /*  COPY AND PASTE OF THE PART THE PANEL SHOWS (namespace draft
                §38): its EQ, its sends, its chain, its time and loops - the
                part's word, `model::partForPanel`'s. */
            std::function<void (const std::string& part)> copyPart;
            std::function<void (const std::string& part)> pastePart;

            std::function<void()> close;

            /** The height changed by a drag on the top edge, in pixels. */
            std::function<void (int)> resizeBy;

            /*  A CLICK OR A TOUCH ON A NUMBER in the EQ or the send mixer: that
                number on the surfaces' master dial (2026-09-26). */
            std::function<void (const std::string& address)> dial;

            /** `take.<verb> <channel>`, from the take panel's buttons (Phase 9c). */
            std::function<void (const std::string& verb, const std::string& channelId)> pressTake;

            /** `take.keep`, from the take panel's Keep and Keep as cue (Phase 9c). */
            std::function<void (const std::string& channelId, bool asCue, const std::string& afterCue)> keepTake;
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

        /*  The reading for this pass, and the doors' tables taken with it:
            the analyser's, and the takes' pictures - null unless the take
            panel is the one open, since nothing else draws them. */
        void show (const model::FootReading&, std::shared_ptr<const audio::MediaRecords>,
                   std::shared_ptr<const audio::TakePictureSet> takes = {});

        /** What the client knows of each plugin's own window, for the chain to say. */
        void setEditorWords (std::map<std::string, std::string>);

        /*  The EQ band a surface's rotary last turned, ringed on the EQ panel
            while it is the one open (2026-09-25); -1 lets it go. */
        void showEditedEqHandle (int handle);

        /*  THE MONITOR'S TILE OF THE PICKED CUE (namespace draft §47, AAH): the
            strip's held edge and playhead, which second the tile is of; and the
            tile itself, drawn under the picture panel's frame. */
        std::optional<double> heldEdge() const;
        std::string heldEdgeWord() const;
        std::optional<double> stripPlayhead() const;
        void setCuePicture (const juce::Image& cuePicture);

        /** The number the master dial turns, marked in whichever panel draws it. */
        void showDial (const std::string& address);

        /*  WHETHER PASTE HAS ANYTHING TO PUT DOWN HERE, told by the window each
            pass, and the sentence its tooltip says - which cue's part, onto
            how many. The window knows the clipboard and the pick; the panel
            only draws the answer. */
        void setPasteable (bool pasteable, const juce::String& why);

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

        /*  THE TITLE'S PICTURES (2026-09-30): what the panel is, and what kind
            of cue it is about in that kind's accent - the same two pictures
            the inspector's panel bar and the cue's row wear. */
        juce::String titleCue;
        model::Icon titleIcon = model::Icon::none, cueIcon = model::Icon::none;
        std::string cueAccent = "ink-faint";

        std::unique_ptr<WaveformEditorComponent> waveform;
        std::unique_ptr<SendMixerComponent> sends;
        std::unique_ptr<TimelineComponent> timeline;
        std::unique_ptr<CurveEditorComponent> curve;
        std::unique_ptr<EqPanelComponent> eq;
        std::unique_ptr<FadeMixerComponent> fadeMixer;
        std::unique_ptr<OscMessagesComponent> messages;
        std::unique_ptr<CurveLaneComponent> curves;

        std::string dialed;
        std::unique_ptr<FxPanelComponent> fx;
        std::unique_ptr<TakePanelComponent> takePanel;
        std::unique_ptr<PicturePanelComponent> picture;
        std::unique_ptr<PatchCanvasComponent> patchCanvas;
        std::map<std::string, std::string> editorWords;
        juce::TextButton shut { "x" };

        /*  COPY AND PASTE in the head, before the close button, shown while the
            panel is on a part of a cue (namespace draft §38). A picture and a
            word each, so neither is told by colour; the word goes when the
            head is narrow and stays the tooltip. */
        IconButton copyButton { model::Icon::copy, "Copy" };
        IconButton pasteButton { model::Icon::paste, "Paste" };
        int headButtons = 0;   ///< how much of the head they take, for `paint`
        int columnWidth = 0, columnGap = 0;

        bool dragging = false;
        int dragFrom = 0;
    };
}
