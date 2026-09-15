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

/*  THE AIM (§3.13): the steps nobody had to keep, the slider, and the solve
    the engine answers them with. */

import { tree } from "../plumbing/tree.js";
import { dbl, str } from "../plumbing/osc.js";
import { selection } from "../model/selection.js";
import { gesture } from "../gestures/table.js";
import { el, esc, cueName } from "./common.js";
import { reconcile } from "./reconcile.js";

/*  THE AIM, AND THE ANSWER TO IT.

    Two nodes and a button. `list/aim` is where the finger is; `list/solve` is
    what the show would be there, which the engine recomputes when the question
    changes; `list.loadToTime` is the one that makes it true. Reading and doing
    are separate on purpose - §3.13's whole point is that an operator can look
    before they leap, and a slider that jumped the show as it moved would be a
    slider nobody dared touch.

    The picked row is what the aim points at, because that is the row the
    operator is already looking at. */
/*  THE STEPS: the waypoints nobody had to keep (§13.10).

    `list/history` is the last sixty-four things the list did, newest first,
    each `<tick>:<cue>:<g|f|t>`. A step is a load-to-time target - before that
    cue, offset -1 - so going back is one click on the step rather than typing
    a time; the click aims, and the button beside it is what makes it so. The
    letter is shown as a word on hover, because a history that can say which
    steps nobody pressed is one that can explain a scene starting on its own.

    KEPT AS THE ROWS ARE, since a chip is something to click, and KEYED BY
    THE STEP ITSELF - its tick, cue and letter, as the history spells it -
    never by its place in the line. A new step pushes every chip one place
    along. If chips were keyed by place, the chip under the pointer would
    become the next cue's between the press and the release, and the click
    would aim at a cue nobody chose. Keyed by the step, each chip moves and
    keeps its element, so a click that straddles the move ends on a different
    element and is lost. A lost click can be made again; an aim at the wrong
    cue is a wrong answer to the question the operator asked. The seconds,
    which change on every poll, are changed in place. */
function renderSteps(focus, atCue) {
  const box = el("aim-steps");
  const text = String(tree.get("/godot/list/" + focus + "/history", ""));
  const steps = text ? text.split(" ") : [];
  const chips = [{ key: "label", html: '<span class="what">steps</span>' }];

  if (!steps.length) {
    chips.push({ key: "none", html: '<span class="none">none taken yet</span>' });
    reconcile(box, chips);
    return;
  }

  const now = Number(tree.get("/godot/engine/tick", 0));

  for (const step of steps.slice(0, 12)) {
    const parts = step.split(":");
    const at = Number(parts[0]);
    const cue = parts[1] || "";
    const how = parts[2] === "g" ? "GO" : parts[2] === "f" ? "fired by name" : "a trigger";

    const ago = Number.isFinite(now) && Number.isFinite(at) ? (now - at) / 50 : NaN;
    const when = !Number.isFinite(ago) ? ""
      : ago < 60 ? Math.round(ago) + " s ago"
      : Math.round(ago / 60) + " min ago";

    chips.push({ key: "step:" + step, html:
      '<span class="step' + (cue === atCue ? " aimed" : "") + '" data-step="' +
      esc(cue) + '" title="' + esc(how + " at tick " + parts[0] +
      " — click to aim before it") + '">' + esc(cueName(cue)) +
      "<i>" + esc(when) + "</i></span>" });
  }

  if (steps.length > 12)
    chips.push({ key: "more", html:
      '<span class="none">and ' + (steps.length - 12) + " more</span>" });

  reconcile(box, chips);
}

function renderAim() {
  const lists = tree.ids("/godot/list/order");
  const focus = tree.get("/godot/list/focus", "") || lists[0] || "";

  const bar = el("aimbar");
  const plan = el("aim-plan");
  const steps = el("aim-steps");

  if (!focus) { bar.hidden = true; plan.hidden = true; steps.hidden = true; return; }

  bar.hidden = false;
  plan.hidden = false;
  steps.hidden = false;

  const aim = String(tree.get("/godot/list/" + focus + "/aim", ""));
  const space = aim.lastIndexOf(" ");
  const atCue = space > 0 ? aim.slice(0, space) : "";
  const atOffset = space > 0 ? Number(aim.slice(space + 1)) : -1;

  el("aim-at").textContent = atCue ? cueName(atCue) : "— pick a row —";
  el("aim-load").disabled = !atCue;

  /*  Not while somebody is dragging it: writing the slider's own value back
      under a finger is how a control fights the person using it. */
  const slider = el("aim-offset");

  if (document.activeElement !== slider) slider.value = String(atOffset);

  el("aim-offset-text").textContent = Number(slider.value) < 0
    ? "before" : Number(slider.value).toFixed(1) + " s";

  const landed = String(tree.get("/godot/list/" + focus + "/statePosition", ""));
  el("aim-landed").textContent = landed && landed !== aim
    ? "landed at " + landed.replace(/ /, " + ") + " s"
    : "";

  renderSteps(focus, atCue);

  /*  AND WHAT IT WOULD BE. A solve is one node holding a document, so this is
      the only place on the page that parses JSON - and it is why: a subtree of
      nodes appearing and vanishing under a dragged finger would be a namespace
      no client could subscribe to. */
  const text = String(tree.get("/godot/list/" + focus + "/solve", ""));

  if (!text) {
    plan.innerHTML = '<div class="none">nothing aimed at yet</div>';
    return;
  }

  let solved = null;

  try { solved = JSON.parse(text); } catch (e) { solved = null; }

  if (!solved || !solved.ok) {
    plan.innerHTML = '<div class="none">the aim names nothing this list holds</div>';
    return;
  }

  const lines = [];

  for (const run of solved.runs || []) {
    const where = run.when === "sounding"
      ? Number(run.offset).toFixed(1) + " s in"
      : run.when === "due"
        ? "in " + Number(run.startsIn).toFixed(1) + " s"
        : "over";

    lines.push('<div class="line"><b>' + esc(cueName(run.cue)) + "</b> — " +
               esc(run.when) + ", " + esc(where) + "</div>");
  }

  for (const value of solved.values || [])
    lines.push('<div class="line">' + esc(value.address) + " = " + esc(value.value) +
               " <span style=\"opacity:.6\">from " + esc(cueName(value.from)) +
               "</span></div>");

  for (const trim of solved.trims || [])
    lines.push('<div class="line"><b>' + esc(cueName(trim.cue)) + "</b> trimmed " +
               esc(String(trim.dB)) + " dB</div>");

  /*  THE CONFUSED LIST IS THE FEATURE (§3.24), so it is not tucked away: a
      solver that says what it could not know is more useful than one that
      guesses, and an operator about to jump is exactly who needs to read it. */
  for (const why of solved.confused || [])
    lines.push('<div class="line confused">could not know: ' + esc(why.why) + " for " +
               esc(cueName(why.cue)) + " — took " + esc(why.took) + "</div>");

  if (solved.standby)
    lines.push('<div class="line">the pointer would land on <b>' +
               esc(cueName(solved.standby)) + "</b></div>");

  plan.innerHTML = lines.length ? lines.join("")
                                : '<div class="none">nothing would be running there</div>';
}

/*  The list the aim belongs to, which is the focused one. */
function aimedList() {
  const lists = tree.ids("/godot/list/order");
  return tree.get("/godot/list/focus", "") || lists[0] || "";
}

/*  Points the aim at the picked row, keeping whatever offset the slider has.
    Picking a row and asking about it are one gesture, which is why selection
    drives this rather than a second control. */
function aimHere() {
  const list = aimedList();

  if (!list || !selection.picked) return;

  gesture("aim", [str(list), str(selection.picked), dbl(Number(el("aim-offset").value))]);
}

export { renderAim, aimedList, aimHere };
