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

/*  WHAT IS WORKED OUT ONCE FROM A REPLY rather than read off it: the indexes
    the rows ask for, added to the tree object as methods so that every view
    keeps asking `tree.triggersOf` and `tree.overlaps` - and so that
    scripts/measure-console-render.py, which wraps them there, keeps finding
    them. Imported by app.js for this effect before any view can ask. */

import { tree } from "../plumbing/tree.js";

Object.assign(tree, {
  /*  A cue's triggers, found by asking every trigger which cue it belongs to.
      There is no `triggerOrder` node to read: a trigger says where it sits and
      the containment is not copied anywhere, so this is the only answer that
      cannot go stale.

      ASKED ONCE PER POLL, AND NOT ONCE PER ROW. The asking used to happen on
      every call - a walk over every address in the tree with a regular
      expression - and every cue row calls this, so each render cost the rows
      times the addresses: five hundred cues against the thousands of
      addresses five hundred cues publish, to find a handful of triggers. In
      a browser that took longer than the hundred milliseconds between one
      poll and the next, so the page spent its whole time rendering and the
      wheel and the clicks waited behind it. Now the first question after a
      poll reads every trigger's answer in one pass and files it under the
      cue it names, and every question after that is a lookup. It is still
      each trigger saying where it sits - the index is only those answers,
      filed by cue - and it is filed in its turn under the reply it was read
      from, so a new reply is a new index and no index outlives its reply.

      IN A WEAKMAP KEYED BY THE REPLY, not in a property that remembers which
      reply the index came from. Such a property keeps that reply alive, and
      on a poll that draws no row - an empty list, no list at all - nothing
      asks for the index again, so the whole previous tree, every node of it,
      would stay in memory beside the current one. A WeakMap answers the same
      question and still lets the old reply go.

      What comes back is the index's own list: read it, do not change it. */
  triggerIndexes: new WeakMap(),

  triggersOf(cueId) {
    let index = this.triggerIndexes.get(this.at);

    if (!index) {
      index = new Map();

      for (const address of Object.keys(this.at)) {
        if (!address.startsWith("/godot/trigger/")) continue;

        const match = /^\/godot\/trigger\/([0-9A-Z]+)\/cue$/.exec(address);

        if (!match) continue;

        const cue = this.get(address, "");

        if (!index.has(cue)) index.set(cue, []);
        index.get(cue).push(match[1]);
      }

      this.triggerIndexes.set(this.at, index);
    }

    return index.get(cueId) || [];
  },

  /*  WHICH CUES THE ENGINE SAYS COULD BE FIGHTING OVER A SLOT.

      Read off every declared slot's `overlaps`, which is the edit-time
      analysis of PRD §3.9c published as cue pairs. Built once per render and
      handed to the rows rather than asked per row: a show has hundreds of rows
      and a handful of slots.

      A WARNING AND NEVER AN ERROR. The analysis is conservative by design - it
      reports what it cannot prove impossible - so a bed looping for ever beside
      a second cue on one input is a pair that is correct, unavoidable and
      completely fine. The row says so in a word, never in colour alone (§4.8),
      and `shared` on either destination is how a designer says they meant it. */
  overlaps() {
    const found = new Map();

    for (const id of this.ids("/godot/slot/order")) {
      const pairs = this.ids("/godot/slot/" + id + "/overlaps");
      const name = this.slot(id, "name", "") || id;

      for (let n = 0; n + 1 < pairs.length; n += 2) {
        for (const [cue, other] of [[pairs[n], pairs[n + 1]], [pairs[n + 1], pairs[n]]]) {
          if (!found.has(cue)) found.set(cue, []);
          found.get(cue).push({ slot: name, other: other });
        }
      }
    }

    return found;
  },
});
