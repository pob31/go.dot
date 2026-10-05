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
    One cue's level, and how much of it arrives at each of the show's mixes.

    THE MASTER IS ON THE LEFT AND IT IS NOT A NEW NUMBER. It is `media/level`,
    the same row the inspector shows and the same one a fade writes over while
    it runs - and what makes the whole picture true is where it sits in the
    signal: `CueMatrix` sums every coefficient and multiplies the sum by that
    level, once. So the master is a DCA over the direct out and every send
    alike, a fade on the cue moves them all together, and this panel is drawing
    the graph rather than an interpretation of it (author, 2026-09-22: "there is
    a general level for the file and a send level for each mix channel. The
    fades operate as a DCA on top of this").

    A STRIP FOR EVERY MIX THE SHOW DECLARES, not one per send the cue happens to
    hold - see `model/Sends`. A strip at silence has no `Send` object behind it,
    and the first drag on it makes one; that is the only asymmetry, and it shows
    as the cross appearing rather than as a fader that will not move.

    THE NUMBER IS ALWAYS DRAWN. §4.8: colour is never the sole carrier, and a
    fader's position is a colour-like fact - a strip read across a booth at a
    glance is read by its number.
*/

#include <wfg/client/model/Foot.h>
#include <wfg/client/model/Sends.h>
#include <wfg/client/model/Theme.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace wfg::client::ui
{
    class SendMixerComponent final : public juce::Component
    {
    public:
        struct Actions
        {
            /** One `node.set`: the cue's level, or one send's. */
            std::function<void (const std::string& address, const std::string& text)> set;

            /*  SEVERAL AT ONCE (namespace draft §30.11): one gesture over the
                picked cues - a frame of a drag, a number typed, a switch - as
                one `node.setMany`, one step to undo. */
            std::function<void (const std::vector<std::pair<std::string, std::string>>& writes)> setMany;

            /** `send.create` on this cue into that mix, when a silent strip is raised. */
            std::function<void (const std::string& cueId, const std::string& busId, double level)> createSend;

            /** `object.delete` on one send, from its cross. */
            std::function<void (const std::string& sendId)> removeSend;

            /** A sentence for the panel's head. */
            std::function<void (const juce::String&)> say;

            /** A click or a touch on a strip: its level on the master dial (2026-09-26). */
            std::function<void (const std::string& address)> dial;
        };

        SendMixerComponent (const model::Theme&, Actions);
        ~SendMixerComponent() override;

        void applyTheme (const model::Theme&);

        /** The reading for this pass. Cheap when the strips have not changed shape. */
        void show (const model::FootReading&);

        void paint (juce::Graphics&) override;
        void resized() override;

        /*  THE NUMBER THE MASTER DIAL TURNS (2026-09-26): a strip whose level
            it is wears a dial before its name. */
        void showDial (const std::string& address);

    private:
        struct Strip;

        void rebuild();
        void refresh();

        /** The identity of the desk, for deciding rebuild against retype. */
        std::string shapeOf() const;

        /** `object.delete` on the send a strip stands for. */
        void removeAt (std::size_t at);

        /*  WHAT A DRAG ON A STRIP WRITES, and the one place that decides
            whether it has to make the object first. A silent strip with no
            `Send` behind it sends `send.create` and remembers what level the
            hand was asking for; the level itself is written on the next pass,
            once the tree has published the new object - the same two-step the
            importer uses when it makes a cue and then fills it in. */
        void levelWanted (std::size_t at, double decibels);

        /** A send's switch, by the mixer's index (2026-09-25). */
        void switchAt (std::size_t at, bool on);

        /*  OVER SEVERAL PICKED CUES (namespace draft §30.11, the author's RA).
            `levelsAt` is every picked cue's level at a strip - its address and
            where it stands - for the cues that have one there: every cue's own
            level on the master, the sends that exist on a mix. `missingAt` is
            the cues with no send into that mix. */
        bool many() const noexcept { return reading.many(); }
        std::vector<std::pair<std::string, double>> levelsAt (std::size_t at) const;
        std::vector<std::string> missingAt (std::size_t at) const;

        /*  A DRAG OR A TURN: every level in `from` moved by the same number of
            decibels, each kept in its range - their differences kept. */
        void levelsMoved (const std::vector<std::pair<std::string, double>>& from, double decibels);

        /*  A NUMBER TYPED, OR THE STRIP RESET: every picked cue to the one
            level - and a cue with no send into the mix is given one, born at
            that level (`send.create`, one each, once per gesture). */
        void levelsSet (std::size_t at, double decibels);

        model::Theme theme;
        Actions actions;

        model::FootReading reading;

        /*  The strip a `send.create` is outstanding for, and what to write
            when it arrives. Empty when nothing is pending. */
        std::string awaitingBus;
        double awaitingLevel = 0.0;

        std::vector<std::unique_ptr<Strip>> strips;

        /*  The sends a gesture over several cues has asked to be made, as
            "cue bus", so a drag asks for each once - cleared when the hand
            goes down again. */
        std::set<std::string> askedSends;

        std::string dialed;

        /** The address of a strip's level - the cue's own for the first - or empty. */
        std::string levelAddressAt (std::size_t at) const;
        void dialAt (std::size_t at);

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SendMixerComponent)
    };
}
