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

import { str, dbl } from "../plumbing/osc.js";
import { tree } from "../plumbing/tree.js";
import { gesture } from "./table.js";
import { swipeKills, SLOP_PX, LONG_PRESS_MS, SWIPE_BACK_MS, Scrub, outside, rateText, EDGE_PX,
         LevelDrag, DialDrag, isLevel, numberText, aimMoved, WRITE_EVERY_MS } from "../model/touch.js";
import { setNode } from "../plumbing/link.js";
import { runsHold, runSheet, spanOf } from "../views/gogo.js";
import { cueSheet } from "../views/didi.js";
import { byFinger } from "../views/common.js";
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

  const strip = event.target.closest("[data-scrub]");

  if (strip) {
    beginScrub(event, strip);
    return;
  }

  const pill = event.target.closest("[data-pill]");
  const row = event.target.closest(".run[data-run]");
  const cue = event.target.closest(".row[data-pick]");

  /*  A CUE HELD (WT.6) opens its sheet as it is held, unless the finger moved:
      a press on its boxes, its fold or its gutter is theirs. */
  if (cue && !interactive(event.target) && !event.target.closest("[data-park], [data-fold]")) {
    press = { pointer: event.pointerId, x: event.clientX, y: event.clientY, cue,
              moved: false, held: false, swiping: false, dx: 0, timer: 0 };
    press.timer = setTimeout(() => {
      if (press && press.cue === cue && !press.moved) {
        press.held = true;
        swallowNextClick();
        openSheet(cueSheet(cue.dataset.pick, send));
      }
    }, LONG_PRESS_MS);
    return;
  }

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
  if (scrubbing && event.pointerId === scrubbing.pointer) {
    moveScrub(event);
    return;
  }

  if (!press || event.pointerId !== press.pointer) return;

  const dx = event.clientX - press.x;
  const dy = event.clientY - press.y;

  if (!press.swiping) {
    if (Math.abs(dx) <= SLOP_PX && Math.abs(dy) <= SLOP_PX) return;

    press.moved = true;
    clearTimeout(press.timer);

    if (press.pill || press.cue || Math.abs(dx) <= Math.abs(dy)) return;

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
  if (scrubbing && event.pointerId === scrubbing.pointer) {
    endScrub();
    return;
  }

  if (!press || event.pointerId !== press.pointer) return;

  const done = press;
  press = null;
  clearTimeout(done.timer);

  if (done.swiping) {
    finishSwipe(done);
    return;
  }

  if (done.moved || done.held || done.cue) return;

  if (done.pill) {
    tapPill(done.pill);
    return;
  }

  const id = done.row.dataset.run;
  openSheet(runSheet(id, send));
});

document.addEventListener("pointercancel", (event) => {
  if (scrubbing && event.pointerId === scrubbing.pointer) {
    endScrub();
    return;
  }

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

/*  A PLAYHEAD DRAGGED (namespace draft §75, WT.5; the app's scrub): from
    where the head is - a grab is not a seek - one strip's seconds across its
    width, at half the rate for every 40 px the finger is above or below it;
    held within 28 px of the screen's side it goes on moving that way; a seek
    at most every 200 ms, and a scene's only as the finger lifts. Where it will
    land is written on the strip while the finger is down, and how fine. */
let scrubbing = null;

function beginScrub(event, strip) {
  const id = strip.dataset.scrub;
  const span = spanOf(id);

  if (!span) return;

  const rect = strip.getBoundingClientRect();
  const scrub = new Scrub();

  scrub.begin({ position: Number(tree.run(id, "position", 0)), from: span[0], to: span[1],
                secondsPerPixel: (span[1] - span[0]) / Math.max(1, rect.width), x: event.clientX,
                onRelease: tree.run(id, "kind", "") === "group" });

  scrubbing = { pointer: event.pointerId, id, strip, scrub, span, rect, x: event.clientX, y: event.clientY,
                at: performance.now(), timer: 0 };
  runsHold.active = true;
  strip.dataset.held = "yes";

  try { strip.setPointerCapture(event.pointerId); } catch (problem) { /* a pointer gone already */ }

  //  Held at the screen's side, it keeps moving: the app's push, a tick of 50 ms.
  scrubbing.timer = setInterval(() => {
    if (!scrubbing) return;
    const now = performance.now();
    const direction = scrubbing.x < EDGE_PX ? -1 : scrubbing.x > window.innerWidth - EDGE_PX ? 1 : 0;
    scrubbing.scrub.push(direction, (now - scrubbing.at) / 1000, distanceOff(scrubbing.y));
    scrubbing.at = now;
    showScrub();
  }, 50);

  showScrub();
  event.preventDefault();
}

function distanceOff(y) {
  return outside(y, scrubbing.rect.top, scrubbing.rect.bottom);
}

function moveScrub(event) {
  scrubbing.x = event.clientX;
  scrubbing.y = event.clientY;
  scrubbing.at = performance.now();
  scrubbing.scrub.moveTo(event.clientX, distanceOff(event.clientY));
  showScrub();
  event.preventDefault();
}

function showScrub() {
  const { scrub, strip, span, id } = scrubbing;
  const head = strip.querySelector(".head");
  const ghost = strip.querySelector(".ghost");
  const fraction = Math.min(Math.max((scrub.target - span[0]) / (span[1] - span[0]), 0), 1);

  if (head) head.style.left = (100 * fraction).toFixed(2) + "%";
  if (ghost) ghost.textContent = "to " + scrub.target.toFixed(1) + "s" +
                                 (rateText(scrub.rate) ? "  finer " + rateText(scrub.rate) : "");

  if (scrub.due(performance.now())) gesture("seek", [str(id), dbl(scrub.target)]);
}

function endScrub() {
  const { scrub, strip, id, timer } = scrubbing;

  clearInterval(timer);
  if (scrub.settle()) gesture("seek", [str(id), dbl(scrub.target)]);
  scrub.end();

  delete strip.dataset.held;
  const ghost = strip.querySelector(".ghost");
  if (ghost) ghost.textContent = "";

  scrubbing = null;
  runsHold.active = false;
}

/*  THE CLICK A LONG PRESS LEAVES BEHIND is not a pick: the row's click would
    pick the cue the sheet is already about. Swallowed before any other
    handler sees it, once, and only for a moment. And a long press is not the
    browser's menu, where a finger works the page. */
let swallowUntil = 0;

function swallowNextClick() {
  swallowUntil = performance.now() + 700;
}

document.addEventListener("click", (event) => {
  if (performance.now() < swallowUntil) {
    swallowUntil = 0;
    event.stopPropagation();
    event.preventDefault();
  }
}, true);

document.addEventListener("contextmenu", (event) => {
  if (byFinger() && event.target.closest && event.target.closest(".row[data-pick], .run, [data-pill]"))
    event.preventDefault();
});

/*  A NUMBER DRAGGED (namespace draft §75, WT.7, ARN): a box of one cue's
    number, pressed and moved sideways, moves the number from where it is - a
    level along the fader's travel, anything else by the master dial's law -
    finer the further the finger is above or below the box, and is held as a
    fader is: `node.touch` as it starts, the value at most every 20 ms, the
    last and `node.release` as the finger lifts, so one Undo takes the whole
    drag back. Up or down first is the pane's scroll; a press that does not
    move is the box's, to type in. A box for several cues is not dragged: they
    hold different numbers, as the desktop's wheel says. */
let numberPress = null;

function rangeOf(address) {
  const node = tree.node(address);
  if (!node) return null;

  const range = Array.isArray(node.RANGE) && node.RANGE.length ? node.RANGE[0] : {};
  const unit = Array.isArray(node.UNIT) && node.UNIT.length ? String(node.UNIT[0]) : "";

  return { integer: "ih".indexOf(node.TYPE) >= 0 && node.TYPE !== "", min: Number(range.MIN), max: Number(range.MAX), unit };
}

document.addEventListener("pointerdown", (event) => {
  if (event.button > 0 || !event.target.closest) return;

  const box = event.target.closest('input[type="number"][data-set]');

  if (!box || box.disabled || box.readOnly || box.dataset.all || box === document.activeElement) return;

  const range = rangeOf(box.dataset.set);
  const start = Number(box.value);

  if (!range || !Number.isFinite(start)) return;

  numberPress = { pointer: event.pointerId, box, range, start, x: event.clientX, y: event.clientY,
                  dragging: false, drag: null, lastX: event.clientX, sentAt: 0, sent: "", mouse: event.pointerType === "mouse" };

  //  A mouse puts its caret in the box at once; held back until the press is known to be no drag.
  if (numberPress.mouse) event.preventDefault();
}, true);

document.addEventListener("pointermove", (event) => {
  if (!numberPress || event.pointerId !== numberPress.pointer) return;

  const held = numberPress;
  const dx = event.clientX - held.x;
  const dy = event.clientY - held.y;

  if (!held.dragging) {
    if (Math.abs(dx) <= SLOP_PX && Math.abs(dy) <= SLOP_PX) return;

    if (Math.abs(dy) >= Math.abs(dx)) {
      numberPress = null;
      return;
    }

    held.dragging = true;
    held.drag = isLevel(held.range) ? new LevelDrag(held.start) : new DialDrag(held.range, held.start);
    held.box.dataset.dirty = "yes";
    held.box.dataset.dragging = "yes";
    held.box.blur();
    try { held.box.setPointerCapture(event.pointerId); } catch (problem) { /* a pointer gone already */ }
    gesture("hold-number", [str(held.box.dataset.set)]);
  }

  const rect = held.box.getBoundingClientRect();
  const value = held.drag.move(event.clientX - held.lastX, outside(event.clientY, rect.top, rect.bottom));
  held.lastX = event.clientX;

  const text = numberText(value, held.range.integer);
  held.box.value = text;

  const now = performance.now();
  if (text !== held.sent && now - held.sentAt >= WRITE_EVERY_MS) {
    setNode(held.box.dataset.set, text);
    held.sent = text;
    held.sentAt = now;
  }

  event.preventDefault();
}, true);

function letGoOfNumber(event, typed) {
  if (!numberPress || event.pointerId !== numberPress.pointer) return;

  const held = numberPress;
  numberPress = null;

  if (!held.dragging) {
    //  A mouse's click on the box, held back above: now the box is the caret's.
    if (typed && held.mouse) {
      held.box.focus();
      held.box.select();
    }
    return;
  }

  const text = held.box.value;
  if (text !== held.sent) setNode(held.box.dataset.set, text);
  gesture("let-go-number", [str(held.box.dataset.set)]);

  delete held.box.dataset.dirty;
  delete held.box.dataset.dragging;
  held.box.dataset.sent = text;
  held.box.dataset.sentAt = String(Date.now());
  swallowNextClick();
}

document.addEventListener("pointerup", (event) => letGoOfNumber(event, true), true);
document.addEventListener("pointercancel", (event) => letGoOfNumber(event, false), true);

/*  THE AIM FROM THE FINGER (namespace draft §75, WT.7, ARK): the slider no
    longer jumps to where it is touched; it moves from where it was by the
    finger's travel, finer the further off the slider the finger goes - and
    says so through the events the page already listens to, `input` as it
    moves and `change` as it lets go. */
let aimPress = null;

document.addEventListener("pointerdown", (event) => {
  if (event.button > 0 || !event.target || event.target.id !== "aim-offset") return;

  const slider = event.target;
  aimPress = { pointer: event.pointerId, slider, start: Number(slider.value), x: event.clientX };
  try { slider.setPointerCapture(event.pointerId); } catch (problem) { /* a pointer gone already */ }
  slider.focus();
  event.preventDefault();
}, true);

document.addEventListener("pointermove", (event) => {
  if (!aimPress || event.pointerId !== aimPress.pointer) return;

  const slider = aimPress.slider;
  const rect = slider.getBoundingClientRect();
  const value = aimMoved(aimPress.start, event.clientX - aimPress.x, outside(event.clientY, rect.top, rect.bottom),
                         { min: Number(slider.min), max: Number(slider.max), step: Number(slider.step), width: rect.width });

  if (String(value) !== slider.value) {
    slider.value = String(value);
    slider.dispatchEvent(new Event("input", { bubbles: true }));
  }

  event.preventDefault();
}, true);

function letGoOfAim(event) {
  if (!aimPress || event.pointerId !== aimPress.pointer) return;
  const slider = aimPress.slider;
  aimPress = null;
  slider.dispatchEvent(new Event("change", { bubbles: true }));
}

document.addEventListener("pointerup", letGoOfAim, true);
document.addEventListener("pointercancel", letGoOfAim, true);

export { tapPill, holdPill };
