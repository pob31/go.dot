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

    WHAT IS FOLDED IS NO LONGER HERE. Folding and the inspector's arrangement
    moved to model/remember.js when they gained a memory that survives a reload;
    they are the same kind of thing as this file's subject - the reader's own
    view, never the document's - but they are written down and this is not. What
    is picked is deliberately NOT remembered: it is a question somebody is asking
    right now, and a page that reopened three days later still pointing at a cue
    would be asserting an interest nobody has. */

/*  In an object rather than a variable of its own, because it is written from
    more than one module - a click picks, a new show clears - and an imported
    binding is one no importer may assign to. */
const selection = {
  picked: null,

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

export { selection };
