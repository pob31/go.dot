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

/*  THE PAGE BY TOUCH: THE RUNNING PANE (namespace draft §75, WT.4).

    A run swiped far enough either way is killed - the deliberate gesture PRD
    §3.17 asks for in place of a small cross; a run tapped opens its sheet,
    which has the kill too, and every stop; a group's pill tapped skips that
    member for this round, held skips it until the group ends. Pointer events,
    so a mouse does all of it the same way (ARL). The rules are model/touch.js's;
    the words and the sheet's actions are views/gogo.js's. Wired when this
    module is loaded.

    WHICH WAY A PRESS GOES is decided once it has moved past the slop: sideways
    first is a swipe, which this module keeps (the pane holds still under it,
    `runsHold`); up or down first is the pane's scroll, which the browser keeps
    and tells this module by cancelling the pointer. */

import { str } from "../plumbing/osc.js";
import { gesture } from "./table.js";
import { swipeKills, SLOP_PX, LONG_PRESS_MS, SWIPE_BACK_MS } from "../model/touch.js";
import { runsHold, runSheet } from "../views/gogo.js";
import { openSheet } from "../views/sheet.js";

let press = null;

//  What a sheet's or a pill's action sends: a gesture of the table, its words strings and its flags booleans.
function send(name, args) {
  gesture(name, args.map((arg) => (typeof arg === "boolean" ? { tag: arg ? "T" : "F" } : str(arg))));
}

function interactive(target) {
  return !!(target.closest && target.closest("button, input, select, textarea, a, [data-scrub]"));
}

document.addEventListener("pointerdown", (event) => {
  if (event.button > 0 || !event.target.closest) return;

  const pill = event.target.closest("[data-pill]");
  const row = event.target.closest(".run[data-run]");

  if (!pill && (!row || interactive(event.target))) return;

  press = { pointer: event.pointerId, x: event.clientX, y: event.clientY, pill, row,
            moved: false, held: false, swiping: false, dx: 0, timer: 0 };

  /*  A PILL HELD is skipped until the group ends, and fires as it is held - a
      finger does not have to guess when to lift. */
  if (pill) {
    press.timer = setTimeout(() => {
      if (press && press.pill === pill && !press.moved) {
        press.held = true;
        holdPill(pill);
      }
    }, LONG_PRESS_MS);
  }
});

document.addEventListener("pointermove", (event) => {
  if (!press || event.pointerId !== press.pointer) return;

  const dx = event.clientX - press.x;
  const dy = event.clientY - press.y;

  if (!press.swiping) {
    if (Math.abs(dx) <= SLOP_PX && Math.abs(dy) <= SLOP_PX) return;

    press.moved = true;
    clearTimeout(press.timer);

    if (press.pill || Math.abs(dx) <= Math.abs(dy)) return;

    press.swiping = true;
    runsHold.active = true;
    press.row.dataset.swipe = "move";
    try { press.row.setPointerCapture(event.pointerId); } catch (problem) { /* a pointer gone already */ }
  }

  press.dx = dx;
  press.row.style.transition = "none";
  press.row.style.transform = "translateX(" + dx + "px)";
  press.row.dataset.swipe = swipeKills(dx, press.row.offsetWidth) ? "kill" : "move";
  event.preventDefault();
});

document.addEventListener("pointerup", (event) => {
  if (!press || event.pointerId !== press.pointer) return;

  const done = press;
  press = null;
  clearTimeout(done.timer);

  if (done.swiping) {
    finishSwipe(done);
    return;
  }

  if (done.moved || done.held) return;

  if (done.pill) {
    tapPill(done.pill);
    return;
  }

  const id = done.row.dataset.run;
  openSheet(runSheet(id, send));
});

document.addEventListener("pointercancel", (event) => {
  if (!press || event.pointerId !== press.pointer) return;

  const done = press;
  press = null;
  clearTimeout(done.timer);

  if (done.swiping) slideBack(done.row);
});

/*  PAST THE LINE, THE RUN IS KILLED and its row slides out of the way; if it is
    still in the pane a moment on - the kill refused, or the engine slow - it
    slides back, so a row is never left half off the screen. Short of the
    line, it slides back at once. */
function finishSwipe(done) {
  const row = done.row;
  const width = row.offsetWidth;

  if (!swipeKills(done.dx, width)) {
    slideBack(row);
    return;
  }

  row.style.transition = "transform 120ms ease-out";
  row.style.transform = "translateX(" + (done.dx > 0 ? width : -width) + "px)";
  send("kill", [row.dataset.run]);

  setTimeout(() => slideBack(row), SWIPE_BACK_MS);
}

function slideBack(row) {
  row.style.transition = "transform 180ms ease-out";
  row.style.transform = "translateX(0)";

  setTimeout(() => {
    row.style.transition = "";
    row.style.transform = "";
    delete row.dataset.swipe;
    runsHold.active = false;
  }, 180);
}

//  The app's `pillTap` and `pillHold`: a pending member skipped for this round or until the group ends; a skipped one put back.
function tapPill(pill) {
  const run = pill.dataset.pillRun;
  const cue = pill.dataset.pill;
  const state = pill.dataset.state;

  if (state === "pending") send("prune", [run, cue, "round"]);
  else if (state === "pruned") send("unprune", [run, cue]);
}

function holdPill(pill) {
  const run = pill.dataset.pillRun;
  const cue = pill.dataset.pill;

  if (pill.dataset.state === "pruned") send("unprune", [run, cue]);
  else send("prune", [run, cue, "group"]);
}

export { tapPill, holdPill };
