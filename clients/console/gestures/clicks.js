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

/*  EVERY CLICK ON THE PAGE, answered in one place by what the clicked thing
    says about itself in its data attributes. Wired when this module is
    loaded. */

import { tree } from "../plumbing/tree.js";
import { dbl, int, str } from "../plumbing/osc.js";
import { folded, panel, selection } from "../model/selection.js";
import { view } from "../views/view.js";
import { el } from "../views/common.js";
import { aimedList } from "../views/aim.js";
import { gesture } from "./table.js";

document.addEventListener("click", (event) => {
  /*  A CHECKBOX CLICKED WITH THE MOUSE LETS GO OF THE FOCUS, as the slider
      does. While it holds the focus it keeps Space (the keydown handler says
      why), and somebody who has just clicked a box is not about to press
      Space on it: their next Space is GO. `detail` counts the presses behind
      a click, and a click made by Space has none, so a box ticked from the
      keyboard keeps the focus it was reached with. By now the box has already
      changed, and its `change` still follows, focus or no focus. */
  if (event.target && event.target.type === "checkbox" && event.detail > 0) event.target.blur();

  const data = event.target && event.target.dataset;

  if (data && data.fold) {
    if (folded.has(data.fold)) folded.delete(data.fold); else folded.add(data.fold);
    view.render();
    return;
  }

  /*  WHERE THE INSPECTOR SITS, which is the page's own arrangement and no
      business of the engine's (§14.1). One button, two answers. */
  if (data && data.layout) {
    panel.layout = panel.layout === "foot" ? "side" : "foot";
    view.render();
    return;
  }

  /*  THE INSPECTOR'S DETAILS, which is the same gesture one pane over. The
      <summary> toggles itself as well - that is what the element is for - and
      this records WHICH WAY it went, so the next render draws what the reader
      last chose rather than shutting it again under their hand. */
  if (data && data.details) {
    panel.details = !panel.details;
    view.render();
    return;
  }

  if (data && data.focus) { gesture("focus-list", [str(data.focus)]); return; }

  /*  THE ONE THAT MAKES IT TRUE, and it is a button rather than the slider for
      exactly that reason: §3.13 lets an operator look before they leap, and a
      slider that jumped the show as it moved would be a slider nobody dared
      touch. */
  if (event.target && event.target.id === "aim-load") {
    const list = aimedList();

    if (list) gesture("load-to-time", [str(list)]);

    return;
  }

  /*  PARKING IS NOT SELECTING. §3.5: "standby never moves on scroll or select",
      so the pointer has a target of its own - the gutter, where the wedge that
      marks it already lives. */
  if (data && data.park) { gesture("park", [str(data.park)]); return; }

  if (data && data.kill) { gesture("kill", [str(data.kill)]); return; }

  if (data && data.trigger && selection.picked) {
    gesture("add-trigger", [str(selection.picked), str(data.trigger)]);
    return;
  }

  if (data && data.role && selection.picked) {
    gesture("add-role", [str(selection.picked), str(data.role)]);
    return;
  }

  if (data && data.newlist) {
    const name = window.prompt("Name for the new list?", "");
    if (name) gesture("new-list", [str(name)]);
    return;
  }

  if (data && data.move && selection.picked) {
    gesture("move", [str(selection.picked), str(data.parent), int(data.move)]);
    return;
  }

  if (data && data.add) {
    const into = data.into;

    /*  Where in the parent. A list publishes its children at
        /godot/list/<id>/order and a group at /godot/cue/<id>/order, and which
        of the two answers settles which this is. */
    const asList = tree.node("/godot/list/" + into + "/order");
    const siblings = tree.ids((asList ? "/godot/list/" : "/godot/cue/") + into + "/order");
    const after = siblings.indexOf(selection.picked);
    const at = after >= 0 ? after + 1 : siblings.length;

    gesture("add-cue", [str(into), int(at), str(data.add), str("")]);
    return;
  }

  if (data && data.delete) {
    /*  NO LONGER ASKED, and the comment that used to sit here said why it
        would stop being: "Undo is Phase 5's and does not exist yet". It does
        now. A delete is one transaction named `object.delete`, the readout
        beside GO says so in those words before anybody reaches for it, and
        ctrl/⌘-Z takes it back with its identifiers intact.

        The confirm goes rather than staying as a belt: a dialogue on a gesture
        that can be taken back teaches an operator to dismiss dialogues, and
        the next one they dismiss without reading will be the one that
        mattered. */
    gesture("delete", [str(data.delete)]);
    selection.picked = null;
    return;
  }

  /*  A STEP IS A PLACE TO GO BACK TO: clicking one aims BEFORE its cue, which
      is `<cue> -1` - standby on it, nothing of it done - and picks the row so
      the inspector and the solve both show where that is. Nothing jumps; the
      button does that, on purpose, the same as for a dragged aim. */
  const step = event.target.closest && event.target.closest("[data-step]");

  if (step && step.dataset && step.dataset.step) {
    const list = aimedList();

    if (list) gesture("aim", [str(list), str(step.dataset.step), dbl(-1)]);

    selection.picked = step.dataset.step;
    view.render();
    return;
  }

  const row = event.target.closest && event.target.closest("[data-pick]");

  /*  PICKING A ROW IS ASKING ABOUT IT. The inspector already opens on a click,
      and §3.13's question is about the row somebody is looking at - so one
      gesture does both rather than making them aim a second time. */
  if (row && row.dataset && row.dataset.pick) {
    const list = aimedList();

    if (list)
      gesture("aim", [str(list), str(row.dataset.pick),
                           dbl(Number(el("aim-offset").value))]);
  }
  if (row) { selection.picked = row.dataset.pick; view.render(); }
});
