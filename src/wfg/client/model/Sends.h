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
    How loud one cue arrives at each of the show's mix channels.

    A STRIP PER MIX CHANNEL AND NOT PER SEND, which is the whole shape of it.
    The document holds a `Send` only where somebody has set one, so a cue that
    feeds two mixes has two children and says nothing about the other six - but
    a mixer that drew two faders would be a mixer you cannot raise a third send
    on. So the strips come from the RIG, every mix channel the show declares, in
    the order the output list puts them; the document says which of them are at
    a level and which are at nothing, and raising one that is at nothing is what
    creates the `Send`.

    That is the same reading the console's own strip takes of a desk: the
    channels are what the desk has, and what is up is what somebody pushed.
*/

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    /*  ONE PICKED CUE'S PART OF A STRIP, when the mixer stands over several
        cues at once (namespace draft §30.11). */
    struct SendShare
    {
        std::string cueId;

        /** Empty when this cue sends nothing into the mix yet. */
        std::string sendId;

        double levelDb = -120.0;
        bool on = true;

        bool present() const noexcept { return ! sendId.empty(); }
    };

    /** One mix channel, and what this cue sends into it. */
    struct SendStrip
    {
        std::string busId;
        std::string name;

        /** "Mono", "Stereo", or a count - `OutputRow::widthWord`'s answer. */
        std::string widthWord;

        /** The interface channels it occupies, one-based: "3-4". */
        std::string channelWord;

        /*  The `Send` object, when there is one. Empty means the cue sends
            nothing here yet, and the first move of this fader makes one. */
        std::string sendId;

        /*  How loud, in dB. Silence when there is no send at all, which is
            the same number the document would hold for one at silence - and
            the two are drawn the same way, because they sound the same. What
            differs is only whether there is an object to delete. */
        double levelDb = -120.0;

        /*  Whether the send is in the mix (send/on, 2026-09-25): off keeps its
            level and contributes nothing. */
        bool on = true;

        /*  Whether a locked show is riding it live - its level or switch held,
            or the whole send made under the lock - and not saved until kept. */
        bool live = false;

        bool present() const noexcept { return ! sendId.empty(); }

        /*  OVER SEVERAL PICKED CUES (namespace draft §30.11): every cue's send
            into this mix, in the reading's order - the first is the lead, whose
            values are the ones above, drawn as the fader's cap - and how they
            spread, drawn as a band behind it. Empty over one cue. */
        std::vector<SendShare> each;

        /*  The quietest and the loudest of the cues that have a send here -
            the lead's level, when none has. */
        double lowestDb = -120.0;
        double loudestDb = -120.0;

        /** How many of the picked cues send here at all, and how many of those are on. */
        std::size_t having() const noexcept;
        std::size_t onCount() const noexcept;

        /** Whether the cues that send here do so at different levels. */
        bool mixed() const noexcept;
    };

    /*  Every mix channel of the show, in output-list order, joined with this
        cue's sends. An empty answer means the show declares no mix channels at
        all, which the panel says in a sentence rather than by drawing nothing.
    */
    std::vector<SendStrip> readSends (const tree::TreeSnapshot&, const std::string& cueId);

    /*  AND JOINED WITH SEVERAL CUES' (namespace draft §30.11): the strips of
        the first of `cueIds` - the lead - each carrying every cue's send in
        `each`, in the order given, and the spread of their levels. One scan of
        the tree, however many cues. */
    std::vector<SendStrip> readSendsMany (const tree::TreeSnapshot&, const std::vector<std::string>& cueIds);

    /*  A FADER DRAGGED OVER SEVERAL CUES (namespace draft §30.11, the author's
        RA: "the same number of decibels for all, keeping their differences"):
        each address in `held` - with its level when the hand went down - moved
        by `decibels`, kept between silence and the loudest a level takes, and
        rounded to the tenth every level here is. Measured from the grab and
        not from the last frame, so a level held at the bottom by the drag
        comes back to its own place when the hand goes back up. */
    std::vector<std::pair<std::string, std::string>> levelsMovedBy (const std::vector<std::pair<std::string, double>>& held,
                                                                    double decibels);

    /*  AND A NUMBER TYPED, or the strip reset: every one of `addresses` to the
        one level, clamped and rounded the same way. */
    std::vector<std::pair<std::string, std::string>> levelsSetTo (const std::vector<std::string>& addresses, double decibels);

    /*  THE NUMBER A LEVEL IS WRITTEN AS: clamped between silence and the
        loudest, to the tenth of a decibel, in the locale-free spelling. */
    std::string levelText (double decibels);
}
