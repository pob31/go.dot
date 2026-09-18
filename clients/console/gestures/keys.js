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

/*  THE KEYBOARD: GO, the standby, undo and save, each a key the gesture table
    (commands.json) names a command for. Wired when this module is loaded. */

import { pointer, SLIDER_STEPS } from "./fields.js";
import { press } from "./table.js";

/*  GO ON SPACE (§3.5, which calls it a sacred convention), and the standby on
    the arrows. Refused while the focus is in a control whose keys are what
    somebody is typing or choosing - a text or number box, a menu, a text area
    - because somebody typing a cue name has every right to a space in it, and
    that is exactly the accident this guard is here to stop.

    THOSE CONTROLS, AND NOT EVERY CONTROL. The guard used to refuse for any
    INPUT, SELECT or TEXTAREA, and the aim slider is an INPUT: once somebody
    had touched it, Space no longer fired GO and the arrows moved the slider
    instead of the standby - the transport gone deaf, with nothing on screen
    to say so. A slider, a checkbox or a button has nothing to type, so the
    keys go past it to GO and the standby - all but one, below - and the
    `preventDefault` beside them keeps that same Space from pressing the
    button as well: a button is pressed on the RELEASE of Space, and only
    when its press was not refused, so a Space after clicking undo is GO and
    never a second undo. An input of a type not named in `PASSES_THE_KEYS`
    counts as text, which is the side to be wrong on, because the two ways of
    being wrong are not alike: a GO that did not fire can be pressed again,
    and a GO fired by a space somebody meant to type is a sound in the room -
    revert is honest that the audio has already escaped (§4.5).

    A CHECKBOX KEEPS SPACE, and only Space. It is the one key that ticks a
    box - Enter does nothing to one outside a form - so a box that passed it
    on to GO could not be ticked from the keyboard at all, and every attempt
    would fire a GO. That is the rule above pointing the other way, because
    on a box Space means choosing. So an unmodified Space is left to a box
    that holds the focus, while its arrows and its accelerators still pass.
    That does not bring back the deaf transport, because a box clicked with
    the mouse lets go of the focus at once (the click listener says so): the
    Space after a click is GO, as it is after the slider. The only box that
    keeps Space is one somebody reached with Tab, and its outline says so.
    A radio would want the same, and its arrows too. The page has none, so
    radio is left out of `PASSES_THE_KEYS` and counts as text until one is
    needed.

    A MENU KEEPS ITS KEYS when it was reached with Tab, because there the
    arrows are how it is worked, and it lets go of the focus once it has been
    picked from with the mouse (the change listener says so).

    ESCAPE LETS GO, and that is all it does. It takes the focus off whatever
    holds it, so the keys come back to the transport without a click on
    something harmless first; and it is answered before the guard, because a
    field is exactly where it is wanted. A field with an edit in it is put
    back to the last value it committed before it is left - what it said when
    it was entered, or what an Enter has sent since. A field commits on
    `change`, which leaving it fires, so a blur on its own would commit the
    very half-typed value Escape was pressed to abandon. And the leaving is
    marked, so that no `change` it fires is sent: a browser that measures a
    change against an older value than the page does would otherwise send the
    put-back value as an edit. An Escape that ends a composition belongs to
    the input method and is left to it. WebKit reports that key after the
    composition has ended, so it arrives with `isComposing` false, and only
    its keyCode, 229, says whose it was. It is not the graceful abort PRD §4.4
    gives Esc. That is Phase 10's, and until then this page says nothing
    about the three levels of stop rather than implying half of one.

    AND UNDO ON CTRL/⌘-Z, AND SAVE ON CTRL/⌘-S, which are the two modified keys
    this page claims, and MODIFIED KEYS ARE STILL ANSWERED FIRST - after Escape
    and the guard, before Space and the arrows. The line after the guard used
    to return on ANY modifier, which was right while nothing here wanted one
    and is now the line that would swallow the accelerators, so the modified
    keys are answered first and every other modified key still leaves by the
    same door: that is what keeps ctrl-Space from ever being GO and ctrl-↓
    from ever being a standby step. Two properties survive that unchanged, and
    both were the reason the blanket return was there. An unmodified Space
    still reaches GO, because the branch below is entered only when Ctrl or ⌘
    is down. And a field that takes text has already returned above, so Ctrl-Z
    while somebody is typing a cue name is the browser's own undo of the
    typing - which is what the person pressing it meant, and is not this
    document's history. A menu returns there too for its own keys, but not
    for these: with Ctrl or ⌘ down it is let past the guard. A menu has
    nothing pending, because its choice was committed when it was made. The
    browser has no undo to give it, and its Ctrl-S would offer to save this
    page. So the narrower guard changes only the controls that take no text:
    with the slider, a checkbox or a menu holding the focus - as with a
    button, which the old guard never caught - Ctrl-Z is this document's
    undo, because there is no typing there for the browser to take back.

    SAVE TAKES THE BROWSER'S OWN CTRL-S AWAY, which is what `preventDefault` is
    for there: unanswered, the key offers to save this page as a web page,
    which is the one file on the machine nobody wanted. Shift is left alone,
    because ctrl-shift-S is Save As everywhere else and this page has none to
    give; quietly turning it into a plain save would write to a place the
    person had not meant to name. And in a field that takes text the guard has
    already returned, so ctrl-S there is the browser's too - which is the
    lesser of two surprises. A field commits on `change`, when it loses the
    focus, so a save sent from inside one would write the show WITHOUT the
    value still being typed: the one save that loses exactly what its author
    was looking at. A checkbox or a menu has nothing still being typed - each
    committed when it was chosen - so with one holding the focus the save is
    this page's.

    ON `key` AND NOT `code`, unlike Space and the arrows above. `code` is the
    physical key, which is exactly right for those two and exactly wrong for a
    letter: the key engraved Z on an AZERTY keyboard reports `KeyW`, so an
    operator in Paris pressing the key their hand knows would get nothing at
    all, silently. `key` is the letter that was typed, whatever the layout, and
    Shift makes it upper case - hence the fold, and hence Shift being the
    difference between undo and redo rather than a second key.

    §4.11 is why there are buttons as well: each keystroke is an accelerator
    over the named command its button sends - `undo`, `redo`, `document.save` -
    and neither of them is a second vocabulary for "the UI did it". */
const PASSES_THE_KEYS = new Set(["range", "checkbox", "color", "button", "submit", "reset"]);

/*  How close two Esc presses are one "double Esc" (§4.4): the desktop's
    model/Panic.h uses the same number, so the two clients read a hand alike. */
const DOUBLE_ESCAPE_MS = 750;
let lastEscape = -Infinity;

function wantsTheKeys(element) {
  if (!element) return false;
  if (element.isContentEditable) return true;
  if (element.tagName === "TEXTAREA" || element.tagName === "SELECT") return true;
  if (element.tagName !== "INPUT") return false;

  return !PASSES_THE_KEYS.has(String(element.type || "text").toLowerCase());
}

document.addEventListener("keydown", (event) => {
  const held = document.activeElement;

  pointer.menu = null;
  pointer.aimKeyStep = !!held && held.id === "aim-offset" && SLIDER_STEPS.has(event.key);

  if (event.key === "Escape" && !event.isComposing && event.keyCode !== 229) {
    /*  WITH NOTHING HELD, ESC IS PRD §4.4's STOP (2026-09-18) - the engine
        has the two levels as commands now, so the page no longer has to say
        nothing about them. One press is the graceful abort, footers run; a
        second within the window is the immediate one, no footers. The
        window is counted from the FIRST press, so a hammered key is a stop
        and then kills, harmlessly. With a field held, Esc still only lets
        go of it: abandoning a half-typed value must not also stop the show. */
    if (!held || held === document.body || typeof held.blur !== "function") {
      const now = Date.now();
      const second = now - lastEscape <= DOUBLE_ESCAPE_MS;

      lastEscape = now;
      event.preventDefault();
      press(second ? "Escape Escape" : "Escape");
      return;
    }

    if (held.dataset && held.dataset.dirty === "yes") {
      if (held.dataset.before !== undefined) held.value = held.dataset.before;
      held.dataset.dirty = "";
    }

    event.preventDefault();

    const mark = held.dataset;

    if (mark) mark.abandoning = "yes";

    try { held.blur(); } finally { if (mark) delete mark.abandoning; }

    return;
  }

  const menuAccelerator = (event.metaKey || event.ctrlKey) && !!held && held.tagName === "SELECT";

  /*  SPACE IS GO, EVEN FROM A NUMBER BOX, and that is not a compromise: a
      number box CANNOT hold a space - the HTML value sanitiser drops it, so
      the keystroke does nothing at all there - and a space pressed over a cue
      list means one thing. Didi's time columns put three such boxes on every
      row, so without this a click on a pre-wait left the transport deaf with
      nothing on screen to say so: exactly the failure this file records for
      the aim slider, on the pane where GO lives.

      THE ARROWS STAY WITH THE BOX, because stepping a number is what a number
      box is for and an operator typing a wait wants them. So do the
      accelerators. What says which is which is the transport, in words, for
      as long as a box holds the keys (views/transport.js). */
  const numberBox = !!held && held.tagName === "INPUT"
                      && String(held.type || "").toLowerCase() === "number";

  const spaceIsGo = numberBox && event.code === "Space"
                      && !event.ctrlKey && !event.metaKey && !event.altKey;

  if (wantsTheKeys(held) && !menuAccelerator && !spaceIsGo) return;

  if (event.code === "Space" && !!held && held.type === "checkbox" &&
      !event.ctrlKey && !event.metaKey && !event.altKey) return;

  if (event.metaKey || event.ctrlKey) {
    if (event.altKey) return;

    const letter = String(event.key || "").toLowerCase();

    if (letter === "z") {
      event.preventDefault();
      press(event.shiftKey ? "Mod+Shift+Z" : "Mod+Z");
    } else if (letter === "s" && !event.shiftKey) {
      event.preventDefault();

      /*  THE KEY FOLLOWS THE BUTTON. In show mode the Save button is disabled
          (views/strip.js: the engine keeps saving under the lock, it is the
          client that stops offering one - §9, decision W), and the accelerator
          over it goes quiet with it rather than being a second route to a save
          nobody offered. preventDefault regardless, or the browser offers to
          save this page as a web page. */
      const saveButton = document.getElementById("save");

      if (!saveButton || !saveButton.disabled) press("Mod+S");
    }

    return;
  }

  if (event.altKey) return;

  if (event.code === "Space") { event.preventDefault(); press("Space"); }
  else if (event.code === "ArrowDown") { event.preventDefault(); press("ArrowDown"); }
  else if (event.code === "ArrowUp") { event.preventDefault(); press("ArrowUp"); }
});

export { wantsTheKeys };
