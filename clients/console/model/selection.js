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

/*  WHAT THE READER HAS PICKED, which is the page's and never the engine's
    (§14.1): the inspected cue - NOT the standby (§3.5), which is the show's and
    lives in a node.

    IT IS TWO THINGS AND NOT ONE, since the author asked for batch editing
    (2026-09-16: "multiple selection for batch editing does not work").
    `picked` is the ANCHOR - one id or null - and `chosen` is the whole SET.
    The anchor is not simply the first of the set: it is the row the reader's
    hand is on, the one a range is measured from, the one the aim names and the
    one the single-cue gestures around this page mean. The set is what a batch
    edit writes to. An ordinary click makes them agree, which is why a dozen
    places could read `picked` alone for as long as picking meant one row, and
    why they still can.

    WHAT IS FOLDED IS NO LONGER HERE. Folding and the inspector's arrangement
    moved to model/remember.js when they gained a memory that survives a reload;
    they are the same kind of thing as this file's subject - the reader's own
    view, never the document's - but they are written down and this is not. What
    is picked is deliberately NOT remembered: it is a question somebody is asking
    right now, and a page that reopened three days later still pointing at a cue
    would be asserting an interest nobody has. */

/*  THE ANCHOR ITSELF, kept in the module rather than on the object below,
    because `selection.picked` is a pair of accessors over it.

    THE ACCESSORS ARE HOW THE PAIR CANNOT COME APART. Every writer inside this
    file goes through the little functions at the foot, but `picked` has been
    assignable from anywhere since before there was a set to keep beside it -
    app.js clears it when a different show arrives - and an assignment that left
    `chosen` pointing at cues of the document being left would hand the
    inspector a batch out of a show nobody is looking at any more. So the
    setter re-derives the set: writing an id means that one row and no other,
    which is exactly what such a write has always meant. Anything that wants
    more than one row says so by calling one of the functions.

    An empty string lands as `null`, so that "nothing is picked" has ONE
    spelling. The page tests this value for truth in a dozen places and a `""`
    would read as unpicked there while still being a string here, which is the
    kind of difference that shows up months later as a row that cannot be
    deselected. */
let anchor = null;

/*  In an object rather than a variable of its own, because it is written from
    more than one module - a click picks, a new show clears - and an imported
    binding is one no importer may assign to. */
const selection = {
  get picked() { return anchor; },

  set picked(id) {
    anchor = id || null;
    selection.chosen = anchor ? [anchor] : [];
  },

  /*  EVERY ROW THE READER HAS CHOSEN, in the order the rows are drawn: the
      order the list runs in, which is the order they are being looked at in and
      the only one a reader could predict. It holds the anchor whenever there is
      one, it is empty whenever there is not, and it holds exactly one id for an
      ordinary click - so a view that wants the set can render `chosen` without
      first asking whether this is one of those days.

      DRAWN ORDER IS NOT SOMETHING THIS FILE CAN SEE. It knows nothing of the
      page: the order arrives with the ids, from the handler that read it off
      the pane (gestures/clicks.js), and this file's job is to keep what it was
      given rather than to sort ids it has no basis for sorting. */
  chosen: [],

  /*  WHERE THE PAGE HAS JUST BEEN SENT, while it is still worth saying so.

      `null`, or `{ key, until, scrolled }`: the reconciler key of the row a
      reveal was aimed at, the moment the highlight stops being drawn, and
      whether the list has already been scrolled to it.

      A reveal is how the two views of one cue are joined up (author,
      2026-09-16: "I could get the focus of a header item with the actual cue"):
      a derived line in a header and the member's own row are the same object
      drawn twice, and clicking either goes to the other. Arriving at a row in a
      list of five hundred is no use if the row does not say it is the one that
      was asked for, so it is marked for about a second and then is not.

      IN THE MARKUP AND NOT ON THE ELEMENT. views/didi.js draws the mark as an
      attribute on the row it emits, because the reconciler copies a fresh
      element's attributes onto the live one and removes any the fresh one
      lacks: a class added to a live element after a render is wiped by the next
      poll, a tenth of a second later. `scrolled` is here for the same kind of
      reason - the scroll must happen once, when the row first exists, and not
      again on every poll for as long as the mark is up. */
  reveal: null,
};

/*  ONE ROW, AND NOTHING ELSE: the ordinary click, and what every gesture that
    lands somewhere definite does - a step clicked in the aim's history, a row
    reached from a tendril. The set becomes that row alone, so the inspector
    that was showing nine cues shows the one under the pointer. */
function pickOne(id) {
  selection.picked = id;
}

/*  AND THIS ONE TOO, OR NO LONGER: ctrl/⌘-click, which is a TOGGLE because the
    gesture has to be its own undo. Somebody assembling eight cues out of
    thirty will put a wrong one in, and a modifier-click that could only ever
    add would make them start the whole set again.

    THE ROW JUST TOUCHED BECOMES THE ANCHOR when it is added, because it is
    where the reader's hand is: the next shift-click ranges from there, which is
    how a set gets built as "these three, and then everything down to here".

    WHEN THE ANCHOR ITSELF IS LET GO the anchor cannot simply go with it - the
    set would then hold rows with nothing inspecting them and no row to range
    from. It lands on the nearest row still chosen, the one that took the
    removed one's place in the order, so a shift-click straight afterwards
    measures from where the hand already was rather than from the far end of
    the set. Letting go of the last one is the only case that leaves nothing
    picked, and that is right: the set is empty, so there is nothing to anchor.

    `drawn` is every id the pane is showing, in the order it shows them, and it
    is optional: given, it is what puts a newly added row in its place among the
    others rather than at the end; left out, the row goes to the end, which is
    the only order this file could invent on its own. The handler that has the
    pane in front of it passes it (gestures/clicks.js). */
function pickAlso(id, drawn) {
  if (!id) return;

  const at = selection.chosen.indexOf(id);

  if (at >= 0) {
    const rest = selection.chosen.filter((other) => other !== id);

    selection.chosen = rest;

    if (anchor === id) anchor = rest.length ? rest[Math.min(at, rest.length - 1)] : null;

    return;
  }

  const order = Array.isArray(drawn) ? drawn : [];
  const added = selection.chosen.concat([id]);

  /*  Sorted by where the rows ARE, and only when the caller said where that
      is. An id the pane is not currently showing has no place in that order -
      a member inside two folded groups, chosen and then folded away - so it
      keeps the end of the list rather than being sorted to the front by an
      index of -1. */
  selection.chosen = order.indexOf(id) >= 0
    ? added.sort((one, other) => {
        const here = order.indexOf(one);
        const there = order.indexOf(other);

        return (here < 0 ? order.length : here) - (there < 0 ? order.length : there);
      })
    : added;

  anchor = id;
}

/*  EVERYTHING FROM THERE TO HERE: shift-click, handed the ids it is to hold,
    already in the order the reader sees them in. The set becomes exactly those
    and the ANCHOR DOES NOT MOVE - a range is measured from somewhere, and an
    operator who shift-clicks twice to correct the far end expects the near end
    to stay where they put it.

    It moves only when the range no longer contains it, which happens when a
    range is asked for with nothing picked or with an anchor that has since gone
    out of the document: then the first row of the range is as good a place to
    stand as any, and an anchor outside the set would be an inspector pointing
    at a cue the reader did not choose. */
function pickThrough(ids) {
  const wanted = Array.isArray(ids) ? ids : [];
  const set = [];

  for (const id of wanted) if (id && set.indexOf(id) < 0) set.push(id);

  selection.chosen = set;

  if (!set.length) anchor = null;
  else if (set.indexOf(anchor) < 0) anchor = set[0];
}

/*  NOTHING AT ALL, which is a gesture of its own and not an accident: the
    author asked for it with the page open (2026-09-16, "clicking in the
    background or top of Didi should deselect the selected cues and close the
    inspector"). There is no command sent and nothing told to the engine - what
    is being looked at is the page's alone (§14.1) - and the inspector shuts
    because views/strip.js reads this to decide whether the pane is there at
    all.

    The reveal is left where it is. It says where the page was last SENT, which
    is a fact about the last second and not about what is picked; it expires on
    its own. */
function pickNothing() {
  selection.picked = null;
}

export { selection, pickAlso, pickNothing, pickOne, pickThrough };
