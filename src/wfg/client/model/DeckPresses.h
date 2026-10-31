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
    The Stream Deck's picture played by hand, a key per finger (namespace draft
    §125, BFA, MT.5).

    A KEY OF THE PICTURE IS PRESSED AND LET GO as the deck's own key would be
    (§95, §74 AQU): a cue's key is `cue.press` then `cue.release`, a learnt
    key `trigger.fire` then `trigger.release`, and the show's keys - GO, Esc,
    Doh!, Pause or Play, the standby arrows - a single command on the press.

    EVERY POINTER HOLDS ITS OWN KEY. Two fingers on two cue keys are two
    presses, and each lift lets go of its own: the picture once kept one
    pressed cue, so a second finger took it over and the first key's clip
    sounded for ever. A key another pointer is holding is not pressed again.
    The mouse and the fingers take turns (`PointerTurns`), as on the desk.

    std only, like the rest of model/.
*/

#include <wfg/client/model/Pointers.h>
#include <wfg/client/model/Surfaces.h>
#include <wfg/engine/command/Event.h>

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace wfg::client::model
{
    class DeckPresses
    {
    public:
        /*  `pointer` lands on the key at `index`, which reads `key`: the
            commands to send. `dragging` is how many pointers JUCE says are
            down, this one among them - one sweeps a lift that never came,
            whose release comes first. */
        std::vector<Event> press (int pointer, std::size_t index, const DeckKeyRow& key, int dragging = 1);

        /** `pointer` lifts: the release of what it held, if anything. */
        std::vector<Event> release (int pointer);

        /** Every key let go: the panel is going, or its picture changed under the hands. */
        std::vector<Event> releaseAll();

        /** Whether a key is held, by any pointer. */
        bool held (std::size_t index) const;

    private:
        struct Held
        {
            std::size_t index = 0;
            std::string cue;
            std::string trigger;
        };

        static void letGo (const Held& down, std::vector<Event>& out);

        std::map<int, Held> holding;
        PointerTurns turns;
    };
}
