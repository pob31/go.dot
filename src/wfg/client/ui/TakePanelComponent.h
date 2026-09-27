/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/*
    A SAMPLING CHANNEL'S TAKE, AT THE FOOT (Phase 9c, stage 9c.4, namespace
    draft §19.7): the picture of the take and every layer on it, growing as it
    records; the loop between its in and out, whose edges a hand drags; the
    playhead going round; and the presses - Rec, Loop, Overdub, Undo, Clear -
    as the named commands they are, so the D700's Rec and a transport cue and
    this panel are the same five verbs.

    THE PICTURE IS THE RECORDER'S, NOT THIS COMPONENT'S. Its peaks come through
    the take pictures' door (audio/TakePictures.h), a column per so many
    chunks, the take first and each layer after it; the state, the length, the
    layers, the points and the playhead are the channel's rows
    (`model::readTake`). Nothing here measures anything.

    LAYERS ARE STACKED, the take at the bottom of each column and each layer
    on top of it, because what a loop sounds like is their sum - and a layer
    being laid is drawn in the live colour. Colour never says it alone (PRD
    §4.8): the sentence over the picture counts the layers.

    A DRAGGED EDGE IS THE TAKE'S DOOR: one `node.set` on the channel's
    `loopIn` or `loopOut` each time the second it stands on changes by a
    millisecond - logged, replayed, never a step of the history, allowed under
    the lock, because the points belong to tonight's take (decision CQ). A
    press on an edge also puts it on the master dial (rule BQ, amended).
*/

#include <wfg/client/model/Foot.h>
#include <wfg/client/model/Theme.h>
#include <wfg/engine/audio/TakePictures.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <string>

namespace wfg::client::ui
{
    class TakePanelComponent final : public juce::Component
    {
    public:
        struct Actions
        {
            /** One `node.set`: a loop point dragged. */
            std::function<void (const std::string& address, const std::string& text)> set;

            /** `take.<verb> <channel>`: record, loop, overdub, undo or clear. */
            std::function<void (const std::string& verb, const std::string& channelId)> press;

            /** A press on an edge: that point on the surfaces' master dial. */
            std::function<void (const std::string& address)> dial;

            /** A sentence in the panel's head, or nothing to clear it. */
            std::function<void (const juce::String&)> say;
        };

        TakePanelComponent (const model::Theme&, Actions);

        void applyTheme (const model::Theme&);

        /** The reading for this pass, and the pictures taken with it. */
        void show (const model::FootReading&, std::shared_ptr<const audio::TakePictureSet>);

        void paint (juce::Graphics&) override;
        void resized() override;

        void mouseMove (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;

    private:
        enum class Edge { none, in, out };

        juce::Rectangle<int> wordsArea() const;
        juce::Rectangle<int> pictureArea() const;

        double secondsAt (int x) const;
        int xOf (double seconds) const;
        double pointOf (Edge) const;
        Edge edgeAt (juce::Point<int>) const;

        void paintPicture (juce::Graphics&, juce::Rectangle<int>) const;
        void paintLoop (juce::Graphics&, juce::Rectangle<int>) const;
        void updateButtons();
        void pressed (const char* verb);

        model::Theme theme;
        Actions actions;

        model::TakeReading take;
        std::shared_ptr<const audio::TakePictureSet> pictures;

        juce::TextButton rec { "Rec" }, loop { "Loop" }, overdub { "Overdub" },
                         undo { "Undo" }, clear { "Clear" };

        Edge hover = Edge::none;
        Edge grabbed = Edge::none;
        double sentSeconds = -1.0;
    };
}
