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

#include <string>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
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

        bool present() const noexcept { return ! sendId.empty(); }
    };

    /*  Every mix channel of the show, in output-list order, joined with this
        cue's sends. An empty answer means the show declares no mix channels at
        all, which the panel says in a sentence rather than by drawing nothing.
    */
    std::vector<SendStrip> readSends (const tree::TreeSnapshot&, const std::string& cueId);
}
