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
    WHICH STEP OF UNDO AN APPLIED COMMAND GOES ON - the one rule the
    transaction hook applies, written once (namespace draft §14.9; §58, RD.3
    and RD.6).

    `wfg serve` and `wfg replay` each install `Engine::setBeforeApply`, and
    until RD.6 each spelled the rule out in its own lambda: a replay that
    decided differently from the session it reproduces diverges only where
    coalescing mattered, which is a log of thousands of drags and a failure
    nobody would think to look for. A backup following its main is a live
    replay, and the two machines must decide alike record for record, so the
    rule is a function both hooks call, and so does every test that wants the
    serve's undo.

    In order, the first that fits:

      - A RIDE IS NOT AN EDIT: a fader on a live row writes fifty times a
        second and none of it is a decision about the show (`isLiveWrite`,
        `isLiveEdit`): no step at all.
      - WHAT IS THIS MACHINE'S (`pair::isMachineLocal`, RD.3): no step, no
        drag ended, nothing landing on the step still open
        (`ShowDocument::beginMachineEdit`).
      - THE ENGINE'S OWN RECORD (origin `engine`, RD.6): a step only if it
        writes the show, opened at that first write
        (`ShowDocument::beginEngineEdit`) - so a sound reported ready on one
        machine and a tick later on the other ends nobody's drag on either.
      - A HOLD IS NOT AN EDIT EITHER (namespace draft §125, BFC): `node.touch`,
        `node.release` and `node.releaseAll` change who holds a node and no row
        of the show, so they open no step (`ShowDocument::noteHold`). A hold
        still breaks a run of plain writes, as it always did; inside a run of
        held writes it leaves the run alone, and the origin's last let-go ends
        it - so two hands riding two faders at once are one step.
      - Everything else, a transaction of its own or the open one joined
        (`ShowDocument::beginTransaction`), a write HELD when its origin holds
        every address it writes.

    The touch table is the one the session's `node.touch` commands fill; it
    changes only through logged records, so a replay holding its own table
    decides exactly as the session did. Null - a rig with no table - leaves
    holds as the transactions they were before §125.
*/

#include <cstdint>
#include <vector>

#include <wfg/engine/osc/OscValue.h>

namespace wfg
{
    struct Command;
    struct Event;

    namespace doc
    {
        class ShowDocument;
    }

    namespace tree
    {
        class TouchTable;
    }
}

namespace wfg::cue
{
    class LiveEdits;

    /*  Tick thread, from the before-apply hook: `args` as coerced, the ones the
        handler is about to see. `live` may be null for a session with no live
        layer, which then has no live edits to tell apart. `touches` is read
        BEFORE the command applies, so a `node.release` still finds what it
        lets go of. */
    void beginUndoStep (doc::ShowDocument& document, const LiveEdits* live, const Command& command,
                        const Event& submitted, const std::vector<osc::Value>& args, std::int64_t tick,
                        const tree::TouchTable* touches = nullptr);
}
