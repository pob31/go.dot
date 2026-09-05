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
    Who is holding which node, and what that stops the engine sending them.

    THE PROBLEM THIS SOLVES is the one every control surface has. Somebody has a
    fader under their finger; the engine, meanwhile, keeps publishing that
    node's value to everyone listening. The value it publishes was written by
    that same finger a few milliseconds ago, so it arrives just late enough to
    fight the hand holding it, and the fader jumps. PRD §3.16 calls touch state
    "required from day one" for exactly this reason.

    THE RULE, and it is short: while an origin is TOUCHING a node, that origin
    receives no pushes for it. Everyone else still does - the point is not to
    freeze the value, it is to stop telling the person who is setting it what
    they just set. On RELEASE the current value is pushed once, so the surface
    ends up agreeing with the engine even if it drifted while held. A
    disconnect releases everything that origin held, because a surface that
    vanished cannot release anything itself and its touches would otherwise
    silence that node for a client id nobody will ever use again.

    ECHO SUPPRESSION IS THE SAME IDEA one step smaller (§3.16): the origin that
    caused a change is not told about it either, touch or no touch. Both live in
    shouldPush() so there is one place that decides what goes out.

    AN ORIGIN IS A STRING, and its shape is the transport's: `ws:<ip>:<port>`,
    `udp:<ip>:<port>`, `cli`, `replay`. It travels ON THE EVENT rather than in a
    thread-local, because the event crosses a queue between the thread that
    received it and the tick thread that applies it, and a thread-local cannot
    survive that hop. spatcore's OriginTagScope has the same problem for the
    same reason.

    QUESTION D IN THE NAMESPACE DRAFT is this vocabulary, and it is still open.
    What is built here is the recommendation: node.touch and node.release per
    origin, pushes suppressed to the toucher, one push on release, release on
    disconnect. No surface exists yet to disagree with it.

    THREADING: none of its own. The tick thread owns this, like the rest of the
    model.
*/

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace wfg::tree
{
    class TouchTable
    {
    public:
        /** False when that origin was already holding it, which is not an error
            but is worth the caller knowing. */
        bool touch (const std::string& origin, const std::string& address);

        /** False when that origin was not holding it. */
        bool release (const std::string& origin, const std::string& address);

        /*  Everything that origin held, released at once. Returns the addresses
            so the caller can push each one's current value, which is what a
            release means - see the note on disconnects above. */
        std::vector<std::string> releaseAll (const std::string& origin);

        bool isHeld (const std::string& origin, const std::string& address) const;

        /** Every origin currently holding this node. */
        std::vector<std::string> holdersOf (const std::string& address) const;

        /** Everything this origin is holding, in address order. */
        std::vector<std::string> heldBy (const std::string& origin) const;

        /** How many (origin, node) pairs are held. */
        std::size_t size() const noexcept;

        bool empty() const noexcept { return size() == 0; }

    private:
        std::map<std::string, std::set<std::string>> byOrigin;
    };

    //==============================================================================
    /*  Whether a value change on `address` should be sent to `toOrigin`.

        Two reasons not to, and they are different: `causedBy` is echo
        suppression - do not tell someone what they just did - and the touch
        table is the fader-fight above. An empty `causedBy` means the change had
        no origin outside the engine (a cue fired, a curve moved), in which case
        everyone who is not touching it hears about it. */
    bool shouldPush (const TouchTable& touches,
                     const std::string& toOrigin,
                     const std::string& address,
                     const std::string& causedBy);
}
