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
    Several fingers on one control surface (namespace draft §125, BFE, MT.1).

    A POINTER IS ITS KIND AND ITS NUMBER. JUCE numbers the mouse 0 and the
    first finger 0 too, so a map keyed by the number alone takes the mouse for
    a finger - the mistake WFS-DIY's map made first. `pointerKey` folds the
    kind in: the mouse is 0, a finger 1000 and up, a pen 2000 and up.

    MOUSE AND FINGERS TAKE TURNS ON A CONTROL (WFS-DIY e14b297d, kept as its
    rules). Both reach the same component independently, so without a rule a
    mouse can take a fader from under a finger, or a finger land on a strip
    the mouse is dragging:

      - while a finger is down, a mouse (or pen) press does nothing here;
      - while the mouse button is down, a landing finger is ignored;
      - a press ignored at its start stays ignored until it ends, so an
        ignored press leaves no state behind and its lift lets go of nothing.

    Fingers among themselves never refuse each other here: whether a second
    finger may take a control another holds is the control's own rule (BFA,
    one control one hand), not a turn.

    A LIFT THAT NEVER CAME. A finger that leaves the window, a touch screen
    unplugged, a window that lost the pointer mid-press: JUCE may never say
    the finger went up. `sweep` is asked at every press with the number of
    pointers JUCE says are dragging; when that is one - the press now - every
    pointer still listed is stale, and the ones that held something are
    answered so their owner lets go of it (a release, a `node.release`)
    rather than only forgetting them.

    std only, like the rest of model/.
*/

#include <set>
#include <vector>

namespace wfg::client::model
{
    enum class PointerKind
    {
        mouse,
        touch,
        pen
    };

    /** The key a pointer is held under: its kind times a thousand, plus its index. */
    int pointerKey (PointerKind kind, int index) noexcept;

    /** The kind a key was made from. */
    PointerKind kindOfPointer (int key) noexcept;

    /** Whether a key is a finger's. */
    bool isFinger (int key) noexcept;

    /** The mouse's key, for a gesture a test or a key makes on the mouse's behalf. */
    inline constexpr int mousePointer = 0;

    /** The first finger's. */
    inline constexpr int firstFinger = 1000;

    class PointerTurns
    {
    public:
        /*  A press: answers whether this pointer may act. A pointer refused
            here is remembered as refused until its lift. A press of a key
            already down (a repeat JUCE should not send) answers as before. */
        bool down (int key);

        /** Whether a pointer took its press and has not lifted. */
        bool owns (int key) const;

        /*  A lift: answers whether the pointer had taken its press, and
            forgets it either way. */
        bool up (int key);

        /*  Before a press: `dragging` is how many pointers JUCE says are down
            now, the new one among them. At one or fewer, everything still
            listed is stale and is forgotten; answers the stale pointers that
            had taken their press, so their owner lets go of what they held. */
        std::vector<int> sweep (int dragging);

        /** Every pointer that took its press and is still down. */
        std::vector<int> held() const;

        /** Whether any finger is down and acting. */
        bool fingerDown() const;

        /** Whether a mouse or pen is down and acting. */
        bool mouseDown() const;

        /** Forget everything, saying which had taken their press. */
        std::vector<int> clear();

    private:
        std::set<int> taken;
        std::set<int> refused;
    };
}
