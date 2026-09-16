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
import { openForKey, panel, save, toggle } from "../model/remember.js";
import { pickAlso, pickNothing, pickOne, pickThrough, selection } from "../model/selection.js";
import { view } from "../views/view.js";
import { el } from "../views/common.js";
import { aimedList } from "../views/aim.js";
import { gesture } from "./table.js";

/*  GO AND LOOK AT A ROW, which two of the branches below share.

    A row can be asked for that is not currently drawn - a member folded away
    inside two shut groups, a derived line in a shut header - so the asking has
    two halves: `openForKey` unfolds whatever has to be open for a row with that
    key to exist at all (model/remember.js), and `selection.reveal` says which
    row it was and for how long, which is Didi's to draw.

    IT IS DIDI'S AND NOT THIS FILE'S because the mark has to be in the markup:
    the reconciler copies a fresh element's attributes onto the live one and
    removes the ones the fresh element lacks, so anything this handler added to
    an element by hand would be wiped by the next poll a tenth of a second
    later. What this records is a fact about what the reader asked for; the
    render decides how to say it.

    A SHADE LONGER THAN THE HIGHLIGHT IT LIGHTS, which fades over a second, so
    the deadline outlives the animation rather than cutting it short on a poll
    that lands at the wrong moment. */
const REVEAL_MS = 1100;

function lookAt(key) {
  openForKey(key);
  selection.reveal = { key: key, until: Date.now() + REVEAL_MS, scrolled: false };
}

/*  THE ORDER THE READER IS LOOKING AT, read off the pane itself.

    It is not the order of `/godot/list/<id>/order`, and working it out from the
    tree would be writing views/didi.js a second time and getting it wrong: what
    is on screen is one flat run of rows in which a group's members sit under
    it, a header's cues sit under a band, folded groups have contributed nothing
    at all, and a member marked `preset` appears TWICE - once as a derived line
    in the header that gets it ready, and once as its own row further down. The
    pane's children ARE that run, in that order, because that is what the
    reconciler leaves there. It is the only order a reader could point at two
    rows and mean "and everything between these", so it is the only order a
    range can honestly be taken in.

    EVERY ROW, DUPLICATES AND ALL, because a range is taken by POSITION and only
    then reduced to cues. De-duplicating first was tried and is wrong, in the
    exact case it was written for: the member whose derived line sits in a
    header ABOVE the anchor had that earlier position kept, which put it outside
    a range that starts at the anchor - so a shift-click over a scene silently
    skipped the one cue in it that had a mark. Measured on the phase4 show: six
    rows highlighted, the seventh between them left out. A cue is in a range
    when a reader can see one of its rows between the two they clicked, and that
    is a question about rows. `drawnPicks` reduces the same walk to cues, for
    the gesture that wants an order rather than a span. */
function drawnRows() {
  const pane = el("cues");
  const rows = [];

  if (!pane) return rows;

  for (const child of Array.from(pane.children)) {
    if (child.dataset && child.dataset.pick) rows.push(child);
  }

  return rows;
}

/*  ONE ID IS ONE CUE however many lines are reading it: an inspector shown the
    same cue twice would offer its own field against itself, and a batch edit
    would write it twice. */
function onlyOnce(ids) {
  const out = [];

  for (const id of ids) if (out.indexOf(id) < 0) out.push(id);

  return out;
}

function drawnPicks() {
  return onlyOnce(drawnRows().map((row) => row.dataset.pick));
}

/*  FROM THE ANCHOR TO HERE, in that drawn order and whichever way round the two
    rows fall: a reader dragging a selection upwards means the same block as one
    dragging it down.

    A range with nothing to measure from is just the row that was clicked - that
    is what happens on the first shift-click of a session, or after a delete
    took the anchor out of the document - and so is a range whose far end is not
    on screen, which the fold can do between the click that set the anchor and
    this one. Neither is worth a refusal: the reader gets the row they clicked,
    with the anchor on it, and their next shift-click means something. */
function rangeTo(row) {
  const rows = drawnRows();
  const to = rows.indexOf(row);

  if (to < 0) return [row.dataset.pick];

  /*  THE ANCHOR'S NEAREST ROW, because the anchor can be on screen twice as
      well, and the two rows a reader is pointing at are the two they can see -
      not the pair that happens to be furthest apart. With one row each, which
      is every case but a preset mark, this is simply where the anchor is. */
  let from = -1;

  rows.forEach((candidate, n) => {
    if (candidate.dataset.pick !== selection.picked) return;
    if (from < 0 || Math.abs(n - to) < Math.abs(from - to)) from = n;
  });

  if (from < 0) return [row.dataset.pick];

  const span = from <= to ? rows.slice(from, to + 1) : rows.slice(to, from + 1);

  return onlyOnce(span.map((one) => one.dataset.pick));
}

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

  /*  ANYTHING THAT SAYS IT FOLDS, and `closest` rather than the target's own
      dataset because the thing that folds is no longer only the twist: a
      header's or a footer's band is a head with a word and a count beside its
      twist, and somebody aiming at a section clicks the WORD. A twist is a
      six-pixel glyph, which is a poor target on a desktop and an unusable one
      on the tablet this page is meant to be operated from. The twist stays as
      the shape that says which way it is (§4.8); the whole head is the button.

      Where it is BECAUSE IT WINS: a group's twist sits inside a row that picks,
      so the two gestures overlap on exactly one element, and folding is what
      somebody who aimed at the twist meant. */
  const foldable = event.target.closest && event.target.closest("[data-fold]");

  if (foldable) {
    toggle(foldable.dataset.fold);
    view.render();
    return;
  }

  /*  WHERE THE INSPECTOR SITS, which is the page's own arrangement and no
      business of the engine's (§14.1). One button, two answers, and the answer
      is written down as it is given: an arrangement somebody settled on that
      went back to "side" on every refresh would be one they stopped flipping.
      It is kept for the MACHINE and not per show, since which way somebody
      likes their panes is a fact about the desk they are sitting at
      (model/remember.js). */
  if (data && data.layout) {
    panel.layout = panel.layout === "foot" ? "side" : "foot";
    save();
    view.render();
    return;
  }

  /*  THE INSPECTOR'S DETAILS, which is the same gesture one pane over. The
      <summary> toggles itself as well - that is what the element is for - and
      this records WHICH WAY it went, so the next render draws what the reader
      last chose rather than shutting it again under their hand. Written down
      with the arrangement above, and for the same reason. */
  if (data && data.details) {
    panel.details = !panel.details;
    save();
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
    pickNothing();
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

    pickOne(step.dataset.step);
    view.render();
    return;
  }

  /*  LOOKING IS NOT PICKING, and this branch is the whole difference.

      A member marked `preset` for some group carries a flag saying which header
      gets it ready, and that flag is now a way of going there. It is a
      SECOND thing on a row that already picks, which is why it is read off the
      clicked element ITSELF and not off `closest`: the flag is the only element
      in the row that carries `data-reveal`, so aiming at it is unambiguous, and
      aiming anywhere else in the row still picks the row below.

      Before the picking branch because it must beat it: the flag lives inside
      the row, and somebody who clicked the flag asked to be shown somewhere
      else, not to re-pick the cue they were already standing on. Nothing here
      touches `selection.picked` - the inspector goes on showing what it was
      showing, which is what makes this a glance rather than a move.

      AND NOT WHEN THE SAME ELEMENT ALSO PICKS, which the derived line in a
      header does: that whole row carries both, and it means both - pick the
      member, then go to the row it runs from. A click that lands on the row
      itself rather than on one of its cells would otherwise be caught here and
      returned from, so the same line would pick or not pick depending on
      whether the pointer was over a word or over the space between two of
      them. Two attributes on one element is the flag's case only; the row's
      case is answered below, where the pick is. */
  if (data && data.reveal && !data.pick) {
    lookAt(data.reveal);
    view.render();
    return;
  }

  /*  CLICKING ON NOTHING MEANS NOTHING, which is a gesture and not an accident
      (author, 2026-09-16, with the page open: "clicking in the background or
      top of Didi should deselect the selected cues and close the inspector").
      The inspector shuts by itself once nothing is picked - views/strip.js sets
      `data-picked` on the panes from this same selection - so there is nothing
      to close here and no command to send: what is being looked at is the
      page's own and the engine is never told (§14.1).

      THREE PLACES AND NOT "ANYWHERE ELSE", because "anywhere else" is the
      whole page. The transport, the strip, the aim bar with its slider and its
      load button, the inspector's own fields: those are all clicks made WHILE
      looking at something, and a selection that fell over every time somebody
      reached for the offset slider would make §3.13's look-before-you-leap
      unusable. What deselects is the list's own furniture, where there is
      demonstrably no cue under the pointer - the pane's background below the
      last row, the column heads, and the pane's heading. The list tabs are
      not in it: a tab is a thing that does something.

      The background is the `#cues` element ITSELF rather than anything drawn
      inside it. Everything the reconciler puts in there is a row, a section
      band or the empty state, and each of those says what it is; the element
      is what is left when none of them is under the pointer. */
  const bare = event.target &&
               (event.target.id === "cues" ||
                (event.target.closest &&
                 (event.target.closest("#cue-cols") ||
                  event.target.closest("#pane-didi > header"))));

  if (bare) {
    pickNothing();
    view.render();
    return;
  }

  const row = event.target.closest && event.target.closest("[data-pick]");

  /*  WHETHER THIS CLICK LANDS ON A ROW OR ASSEMBLES A SET. ctrl/⌘ adds the row
      to what is chosen or takes it back out; shift takes everything between the
      anchor and here. ⌘ and ctrl are one gesture because this page is operated
      from both kinds of machine, often on the same show, and nobody should have
      to remember which desk they are sitting at. */
  const adding = event.metaKey || event.ctrlKey;
  const ranging = event.shiftKey;

  /*  PICKING A ROW IS ASKING ABOUT IT. The inspector already opens on a click,
      and §3.13's question is about the row somebody is looking at - so one
      gesture does both rather than making them aim a second time.

      BUT AN AIM NAMES ONE CUE, so a click that is building a set does not send
      one. §3.13's pointer answers "where would the show be if I went from
      here", and there is no answer to that for nine cues at once: the gesture
      takes a single id, so a shift-click over a block would either send a
      handful of aims with only the last one surviving, or aim at whichever row
      the loop happened to end on. A plain click is the one gesture that says
      unambiguously which row the reader's hand is on, so it is the one that
      moves the aim; while a set is being assembled the aim stays where the last
      plain click left it, which is where the reader left it. */
  if (row && row.dataset && row.dataset.pick && !adding && !ranging) {
    const list = aimedList();

    if (list)
      gesture("aim", [str(list), str(row.dataset.pick),
                           dbl(Number(el("aim-offset").value))]);
  }
  if (row) {
    /*  ONE ROW, ONE MORE ROW, OR EVERYTHING BETWEEN - the author's other ask of
        2026-09-16, "multiple selection for batch editing does not work", in the
        three gestures every list on every desk already uses for it. The model
        keeps the anchor and the set in step (model/selection.js); this decides
        only which of the three was meant. */
    if (ranging) pickThrough(rangeTo(row));
    else if (adding) pickAlso(row.dataset.pick, drawnPicks());
    else pickOne(row.dataset.pick);

    /*  AND A ROW THAT IS A SECOND VIEW OF A CUE SAYS WHERE THE FIRST ONE IS.
        The derived line in a header is a reading of a mark on a member that
        lives further down the list, so the whole ROW carries `data-reveal`:
        clicking it picks the member - there is one object and the inspector
        edits it - and goes to the row that member actually runs from, which is
        the question anybody clicking a line in italics is asking. One gesture,
        both answers, no second aim.

        NOT WHILE A SET IS BEING BUILT. A reveal unfolds whatever has to open
        and scrolls the list to somewhere else, and somebody ctrl-clicking their
        way down a block would have the rows they were aiming at moved out from
        under their hand between one click and the next. Going to the other view
        of a cue is a gesture in its own right, and it is a plain click. */
    if (row.dataset.reveal && !adding && !ranging) lookAt(row.dataset.reveal);

    view.render();
  }
});
