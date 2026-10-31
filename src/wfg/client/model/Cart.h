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
    A cart shown as pads (namespace draft §125, BEZ; MT.6).

    THE TABLET'S CART TAB, IN THE WINDOW (§74, AQQ; go.dot-remote's
    model/Cart.kt): a cue at the cart's top level is a pad of its own - a
    sound, an OSC message, a script, a group that is a macro - and a sampler
    group in the cart is its members, each a pad, under the group's name.
    The deck's own walk (`DeckBridge::padsOf`), so a pad here, a key on a
    Stream Deck and a pad on the tablet are the same cue in the same place.

    A HAND DOWN PRESSES (`cue.press`, at the velocity of where it landed: the
    top of a pad a hard hit, as on the desk's pads), up lets go
    (`cue.release`), so a hold member plays while it is held and anything
    else is fired by the press. The standby never moves; under the lock the
    pads still play.

    GOING is a run of the cue playing, waiting out its pre-wait, stopping or
    in its post-wait - not a sampler member only armed and waiting for a
    press.

    std only, like the rest of model/.
*/

#include <string>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    struct CartPad
    {
        std::string cue;
        std::string number;
        std::string name;
        std::string kind;
        std::string mode;

        /** The cue's own colour, `#rrggbb`, or empty: a tab at the pad's left, never the only telling. */
        std::string colour;

        /** Whether a run of it is going now. */
        bool going = false;

        /** The sampler group it is a member of, by name; empty for a pad at the cart's top level. */
        std::string bank;

        bool operator== (const CartPad&) const = default;
    };

    /** A cart's pads, in its order; none for a list that is not a cart. */
    std::vector<CartPad> readCartPads (const tree::TreeSnapshot& snapshot, const std::string& cartId);

    /** The velocity a press `height` up a pad carries (nought the bottom, one the top): 1 to 127. */
    int padVelocity (double height) noexcept;

    /*  WHAT THE SWITCH OVER A CART SAYS (§125, BEZ): the view it goes to, so
        the words say where the window is as well as where it goes. */
    std::string cartViewWords (bool showingPads);
}
