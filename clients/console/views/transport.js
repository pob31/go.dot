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

/*  THE TRANSPORT'S BUTTONS: GO, the lock, revert, and every button that names
    its command in its own `data-cmd`. Wired when this module is loaded. */

import { tree } from "../plumbing/tree.js";
import { command, send } from "../plumbing/link.js";
import { gesture } from "../gestures/table.js";
import { wantsTheKeys } from "../gestures/keys.js";
import { el } from "./common.js";

el("go").addEventListener("click", () => gesture("go", []));

/*  WHERE THE KEYBOARD IS, IN WORDS.

    A box with the focus keeps the arrows and the accelerators - stepping a
    number is what a number box is for - so while one has them the hint beside
    GO would otherwise be telling an operator that ctrl-Z undoes, when ctrl-Z
    is the browser's typing-undo and the arrows are moving a pre-wait rather
    than the standby. Didi's time columns put three such boxes on every row, so
    this is now one click away at all times.

    SAID, AND NOT COLOURED (§4.8). The focus ring on a box is a pixel of hue on
    a control the pointer is already resting on; the sentence is what an
    operator reads. Space is still GO from a number box (gestures/keys.js), and
    the sentence says that too, because the one thing nobody may be made to
    wonder about is whether GO will fire.

    ON THE DOCUMENT, because focus moves between elements this module does not
    own; `focusout` runs before the next element has it, so the answer is asked
    for after the browser has settled. */
const hint = el("hint");
const HINT_KEYS = hint ? hint.textContent : "";
const HINT_TYPING = "typing in a box · space still GO · esc gives the keyboard back";

function sayWhereTheKeysAre() {
  if (! hint) return;

  const typing = wantsTheKeys(document.activeElement);

  hint.dataset.typing = typing ? "yes" : "no";

  const said = typing ? HINT_TYPING : HINT_KEYS;

  if (hint.textContent !== said) hint.textContent = said;
}

document.addEventListener("focusin", sayWhereTheKeysAre);
document.addEventListener("focusout", () => setTimeout(sayWhereTheKeysAre, 0));

/*  THE LOCK IS A NODE, NOT A COMMAND (namespace draft §14.7): the toggle is a
    `node.set` on /godot/document/locked like any other write, carrying the
    boolean it wants rather than a word, and the engine's answer comes back the
    way every answer does - as the reading the next poll brings. Which way to
    flip is decided from what the engine last said, not from what this button
    last did, so two pages in two hands cannot argue it into the wrong state. */
el("lock").addEventListener("click", () => {
  const now = tree.get("/godot/document/locked", null);
  const isLocked = now === true || now === "true";

  send("/godot/document/locked", [{ tag: isLocked ? "F" : "T" }]);
});

/*  REVERT ASKS FIRST AND THE DELETE DOES NOT, and that is not an inconsistency.
    PR 5.4 took the confirm off `object.delete` because undo had arrived: a
    gesture ctrl/⌘-Z takes back does not need a dialogue, and a dialogue on a
    gesture that can be taken back teaches an operator to dismiss dialogues.
    Revert is the case that argument does not reach. It reloads the show from
    disk and CLEARS THE UNDO HISTORY as part of the same command (§14.10), so
    afterwards there is nothing on the stack to press ctrl-Z on - and what goes
    is not one edit but everything since the last save, in one press, with
    nowhere left to fetch it from. Undo is the whole of why the delete stopped
    asking, and undo is exactly what revert takes away, so this is where the
    page asks. The question says what will be lost rather than asking whether
    anybody is sure, because "are you sure?" is a question nobody reads.

    Discard, in the recovery banner, is as final and does not ask twice: the
    banner it sits in is already the question, written out in full, and a
    dialogue over a paragraph somebody has just read is that question again. */
el("revert").addEventListener("click", () => {
  const show = tree.get("/godot/document/name", "") || "the show";

  if (window.confirm("Revert " + show + " to what was last saved?\n\n" +
                     "Everything changed since then is thrown away, and undo cannot " +
                     "bring it back: reverting clears the undo history as well.")) {
    gesture("revert", []);
  }
});

for (const button of document.querySelectorAll("#transport [data-cmd], #recovery [data-cmd], #pane-gogo header [data-cmd]")) {
  button.addEventListener("click", () => command(button.dataset.cmd));
}
