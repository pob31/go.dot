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

/*  THE INSPECTOR'S FIELDS, and the controls a pointer leaves holding the
    keyboard: what commits, what Escape takes back, and when a menu or the
    slider lets go. Wired when this module is loaded; `pointer` is shared with
    gestures/keys.js, which reads and clears it. */

import { setNode } from "../plumbing/link.js";
import { commitText } from "../views/values.js";
import { aimHere } from "../views/aim.js";

document.addEventListener("input", (event) => {
  /*  The slider re-aims as it moves - asking is free and the answer is what it
      is for. */
  if (event.target && event.target.id === "aim-offset") { aimHere(); return; }

  const target = event.target;

  if (target && target.dataset && target.dataset.set && target.type !== "checkbox") {
    target.dataset.dirty = "yes";
  }
});

/*  WHAT A FIELD SAID WHEN IT WAS ENTERED, so that Escape can put it back. Taken
    on the way in rather than when the typing starts, because by the first
    `input` the first keystroke is already in the value. `refreshFields` never
    writes into the field with the focus, so what is recorded here stays what
    the field said until the focus leaves it. */
document.addEventListener("focusin", (event) => {
  const target = event.target;

  if (target && target.dataset && target.dataset.set && target.type !== "checkbox") {
    target.dataset.before = target.value;
  }
});

/*  AND A BOX THE BROWSER CANNOT READ IS LET GO OF WHEN THE HAND LEAVES IT.

    A number box holding `1e` or `-` has an empty value and raw text the
    sanitiser kept, so no `change` is fired when the focus goes: the field
    stayed marked as holding an uncommitted edit for ever, and `refreshFields`
    skips exactly those - so that one box never took another value from the
    engine again, silently, for the rest of the session. The mark goes here,
    and the text nobody can read goes with it, which lets the next reply put
    the engine's own value back. */
document.addEventListener("focusout", (event) => {
  const target = event.target;

  if (! target || ! target.dataset || ! target.dataset.set || target.type === "checkbox") return;

  target.dataset.dirty = "";

  if (target.validity && target.validity.badInput) target.value = "";
});

/*  A MENU PICKED WITH THE MOUSE LETS GO OF THE KEYBOARD once it has been
    picked from, for the slider's reason and the transport's. A select keeps
    the focus after a click, and while it has it the arrows change its option
    - and a closed menu commits every change - so the key an operator meant
    for the standby would quietly rewrite a trigger's type. The press is what
    tells a menu picked with the mouse from one reached with Tab, and the one
    reached with Tab keeps its arrows, because that is how it is worked from
    the keyboard. The mark is dropped on any key and when the menu loses the
    focus, so a menu opened with the mouse and closed without a choice does
    not throw away a later choice made from the keyboard. */

/*  Whether the key being pressed steps the aim slider, whose `change` is
    then no release (the change listener says so). */
const pointer = {
  menu: null,                      // the menu a pointer last pressed, until it is picked from
  aimKeyStep: false,
};
const SLIDER_STEPS = new Set(["ArrowLeft", "ArrowRight", "PageUp", "PageDown", "Home", "End"]);

document.addEventListener("pointerdown", (event) => {
  const target = event.target;

  pointer.menu = target && target.closest ? target.closest("select") : null;
  pointer.aimKeyStep = false;
});

document.addEventListener("focusout", (event) => {
  if (event.target === pointer.menu) pointer.menu = null;
});

/*  A FIELD COMMITS ON `change` - blur or Enter for a box, the moment of
    choosing for a menu - and not on every keystroke. A cue named "Th" on the
    way to "Thunder" would otherwise be a write, a log record and a document
    mutation nobody asked for, five times over. */
document.addEventListener("change", (event) => {
  const target = event.target;

  /*  THE SLIDER LETS GO WHEN A POINTER LETS GO OF IT. Dragged, its `input`
      events are the aim moving under the finger and `change` is the finger
      lifting - and a slider keeps the focus after that unless it is told not
      to. Kept, the focus would go on holding two things the slider has no use
      for once nobody is touching it. The keys, first: the narrowed guard
      below no longer lets a slider take Space or the arrows from GO, and
      letting go here means the transport does not rest on that guard alone.
      And its own readout: `renderAim` never writes the engine's aim into a
      slider that has the focus - that is what stops it fighting a finger - so
      a slider left holding the focus would go on showing the offset it was
      let go at, whatever the aim became afterwards from another page or a
      surface.

      A STEP FROM THE KEYBOARD IS NOT A RELEASE, though it fires `change`
      just the same, once per key and before the next key arrives. Letting go
      there would give a slider reached with Tab one step and hand the next
      key to the page: one step per visit, of a hundred and twenty-two. So
      the keydown handler marks a key that steps the slider, and the `change`
      that key makes leaves the focus where it was. Up and Down are not among
      those keys: they stay the standby's, as §14.3 asks. */
  if (target && target.id === "aim-offset") {
    if (pointer.aimKeyStep) { pointer.aimKeyStep = false; return; }

    target.blur();
    return;
  }

  const address = target && target.dataset ? target.dataset.set : null;

  if (!address) return;

  /*  ESCAPE NEVER WRITES. The Escape branch below marks the field while it
      takes the focus away, and a `change` that leaving fires is not an edit
      anybody made. */
  if (target.dataset.abandoning === "yes") return;

  const sent = commitText(target);

  setNode(address, sent);

  /*  WHAT THIS BOX SAYS NOW, until the engine says it too (views/values.js).
      The refresh skips a box whose last commit the tree has not caught up
      with, so a value does not flash back to what it was for the one frame
      between the write and the reply that carries it. */
  target.dataset.sent = target.value;
  target.dataset.sentAt = String(Date.now());
  target.dataset.dirty = "";

  /*  WHAT IT SAYS NOW IS WHAT ESCAPE GOES BACK TO. Enter commits a box and
      leaves the focus in it, so from here on the value the engine holds is
      this one, and not what the box said when it was entered. Escape after
      more typing should bring back this value. Putting back the older one
      would show a value the show no longer has, and leaving the box then
      counts as a change since the Enter: Edge sent that older value as an
      edit, and Escape had written to the show. */
  if (target.type !== "checkbox") target.dataset.before = target.value;

  if (target === pointer.menu) {
    pointer.menu = null;
    target.blur();
  }
});

export { pointer, SLIDER_STEPS };
