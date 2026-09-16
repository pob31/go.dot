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

/*  WHAT A NODE SAYS AS A CONTROL SHOWS IT, and the refresh that keeps a live
    control true without ever writing over somebody's hands.

    Shared by the inspector and by Didi's time columns, which is the whole
    reason it is a file: the same attribute is now on screen in two places at
    once - `preWait` in a row and `preWait` in the inspector - and two copies of
    "what this value looks like" would eventually disagree about one of them. */

import { tree } from "../plumbing/tree.js";

/*  A LIST NODE - a route's `gains`, a fade's `points` - carries one type tag
    per element, so its TYPE is as long as its value (§14.6). */
function isList(node) {
  if (typeof node.TYPE === "string") return node.TYPE.length !== 1;

  /*  AN EMPTY LIST IS SERVED WITH NO TYPE AT ALL, there being no tag to give
      zero values, and with no CONTENTS either - which is what tells it from a
      container. */
  return !node.CONTENTS && node.ACCESS !== undefined;
}

/*  `element` is the control the value is going into, when there is one: a field
    may ask for a rule of its own, and one does. */
function shownValue(node, element) {
  if (isList(node)) return Array.isArray(node.VALUE) ? node.VALUE.join(" ") : "";

  const value = Array.isArray(node.VALUE) && node.VALUE.length ? node.VALUE[0] : "";

  /*  A ZERO SHOWN AS AN EMPTY BOX, which only Didi's time columns ask for.
      Almost every cue waits for nothing before it and holds nothing after it,
      so a column of noughts down a hundred rows would be the loudest thing in
      the cue list and would say nothing. The box still tells anybody who looks
      what empty means: the column's head names the attribute, and the box's
      own title is the row's sentence from the parameter table. A placeholder
      nought was tried first and read as a value - a grey column of them, which
      is the noise this rule exists to remove. Marked on the element rather
      than decided here, so the inspector's fields keep showing the value the
      engine holds, whatever it is. */
  if (element && element.dataset && element.dataset.blankZero === "yes"
        && value !== "" && Number(value) === 0)
    return "";

  return value;
}

/*  THE OTHER HALF OF THE BLANK-NOUGHT RULE, and it belongs beside the first.

    A box that shows a nought as empty teaches exactly one gesture - clear it,
    to mean none - and that gesture used to send an empty string. The engine
    refuses one: `parseValue` for a number answers `type-mismatch` on text it
    cannot read, so the page taught a gesture and then unsaid it, with a red
    banner about a box somebody had emptied on purpose.

    AN UNREADABLE BOX IS NOT AN EMPTY ONE. A number box holding `1e` or `-`
    reports its value as "" as well - the HTML value sanitiser keeps the raw
    text and says `badInput` - and that is not a nought anybody typed. It is
    sent as it stands and refused as it deserves, which is the honest answer
    and the one a red banner is for. */
function commitText(element) {
  if (element.type === "checkbox") return element.checked ? "true" : "false";

  const unreadable = !!(element.validity && element.validity.badInput);

  if (element.value === "" && ! unreadable
        && element.dataset && element.dataset.blankZero === "yes")
    return "0";

  return element.value;
}

/*  Values only, and only where nobody is working: never into a field that has
    the focus, and never over an edit that has not been committed. */
function refreshFields(pane) {
  for (const input of pane.querySelectorAll("[data-set]")) {
    if (input === document.activeElement || input.dataset.dirty === "yes") continue;

    const node = tree.node(input.dataset.set);
    if (!node) continue;

    /*  An empty list is a value - the curve was cleared - where an empty
        scalar is a node with nothing to say yet. */
    if (!isList(node) && (!Array.isArray(node.VALUE) || !node.VALUE.length)) continue;

    const value = shownValue(node, input);

    /*  A COMMIT IS NOT INSTANT, and the reply in flight when it was made says
        the old value. Without this, every edit flickered back to what it had
        been for the one frame between the write and the tree that carries it -
        which reads exactly like an edit that did not take.

        BOUNDED, because an edit that did not take looks the same from here. A
        refusal never changes the tree, so the wait is given a second and a
        half - fifteen polls - and then the box goes back to what the engine
        says, which is the honest answer and the one the red banner explains. */
    if (input.dataset.sent !== undefined) {
      const agreed = String(value) === input.dataset.sent;
      const waited = Date.now() - Number(input.dataset.sentAt || 0) > 1500;

      if (! agreed && ! waited) continue;

      delete input.dataset.sent;
      delete input.dataset.sentAt;
    }

    if (input.type === "checkbox") input.checked = value === true;
    else if (String(input.value) !== String(value)) input.value = value;
  }
}

export { isList, shownValue, commitText, refreshFields };
