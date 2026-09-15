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

/*  DIDI, the cue list: every row of the focused list, the groups inside it,
    their headers and footers, and the persistent band at its foot. */

import { tree } from "../plumbing/tree.js";
import { folded, selection } from "../model/selection.js";
import { el, esc, seconds, cueName } from "./common.js";
import { reconcile } from "./reconcile.js";

/*  The current answer, refreshed by `renderLists` and read by `cueRow`. */
let overlapping = new Map();

/*  What each of §13.6's six words means, said once so the row can be short. */
function prepareNote(word) {
  return ({
    preparing: "the horizon is getting this ready now",
    pending:   "waiting for a slot another cue is holding - it will land when that run ends",
    partial:   "part of this could not be got ready ahead and will happen at entry",
    armed:     "ready: its voice is reserved and its media is made ready",
    verified:  "ready, and every value sent ahead was read back equal",
  })[word] || word;
}


/*  ONE CUE ROW. `depth` is the indent, `role` is member / header / footer, and
    `standby` is the identifier the pointer is on for this list. The row, and
    the rows of a group's contents after it, go into `out` as a key and the
    markup, for `reconcile` to bring the pane into line with. */
function cueRow(id, depth, role, standby, out) {
  const kind = tree.cue(id, "kind", "memo");
  const isGroup = kind === "group";
  const open = isGroup && !folded.has(id);

  const enabled = tree.cue(id, "enabled", true) !== false;
  const number = esc(tree.cue(id, "number", ""));
  const name = esc(tree.cue(id, "name", "") || "—");

  const pre = seconds(tree.cue(id, "preWait", 0));
  const post = seconds(tree.cue(id, "postWait", 0));

  const flags = [];

  if (isGroup) {
    const mode = tree.cue(id, "mode", "sequence");
    const advance = tree.cue(id, "advance", "manual");
    flags.push(mode === "timeline" ? "timeline" : advance === "auto" ? "auto" : "manual");
  }

  if (pre) flags.push("wait " + pre);
  if (post) flags.push("hold " + post);
  if (!enabled) flags.push("disabled");

  /*  A cue somebody else can fire. Worth a mark on the row rather than only in
      the inspector: an operator looking down a list needs to know which of
      these can go off without them. */
  const triggers = tree.triggersOf(id);

  /*  And a cue the engine cannot promise has its slot to itself. */
  const shares = overlapping.get(id) || [];

  /*  THE TENDRIL, in the cheapest form this page can draw one: the member's own
      row says which header gets it ready. The author asked for "some form of
      tendril showing the header it's in"; a line drawn between two rows in a
      scrolling list is a thing to maintain, and naming the header is the same
      fact said in words - which is also what §4.8 wants. */
  const preparedBy = tree.cue(id, "preset", "");

  /*  HOW FAR AHEAD THIS ONE HAS BEEN GOT, in the word the engine uses.

      PRD §3.12's horizon reaches a block before anybody presses anything, and
      what an operator needs from it is on the row rather than in an inspector:
      whether the next scene is ready, and whether it is only partly ready. In
      WORDS and never colour alone (§4.8) - `pending` in particular, which is a
      cue waiting for a slot somebody else holds and is the one an operator has
      to be able to read at a glance and act on.

      `idle` is the resting state and says nothing, which is most rows most of
      the time. */
  const prepare = tree.cue(id, "prepare", "idle");

  const twist = isGroup
    ? '<span class="twist" data-fold="' + id + '">' + (open ? "▼" : "▶") + "</span>"
    : '<span class="twist"></span>';

  out.push({ key: "cue:" + id, html:
    '<div class="row" data-pick="' + id + '"' +
      ' data-standby="' + (id === standby ? "yes" : "no") + '"' +
      ' data-picked="' + (id === selection.picked ? "yes" : "no") + '"' +
      ' data-enabled="' + (enabled ? "yes" : "no") + '">' +
      '<div class="gutter" data-park="' + id + '" title="park the standby here"></div>' +
      '<div class="number num">' + number + "</div>" +
      '<div class="name" style="padding-left:' + (depth * 16) + 'px">' +
        twist +
        '<span class="kind">' + esc(kind) + "</span>" +
        '<span class="text">' + name + "</span>" +
      "</div>" +
      '<div class="tail">' +
        flags.map((f) => '<span class="flag">' + esc(f) + "</span>").join("") +
        (triggers.length
           ? '<span class="flag trig" title="' + triggers.length +
             ' trigger(s)">\u26A1' + (triggers.length > 1 ? triggers.length : "") + "</span>"
           : "") +
        (preparedBy
           ? '<span class="flag preset" title="' +
             esc("got ready by the header of " + cueName(preparedBy)) +
             '">\u2191 ' + esc(cueName(preparedBy)) + "</span>"
           : "") +
        (prepare && prepare !== "idle"
           ? '<span class="flag ' + (prepare === "pending" ? "warn" : "ready") +
             '" title="' + esc(prepareNote(prepare)) + '">' + esc(prepare) + "</span>"
           : "") +
        (shares.length
           ? '<span class="flag warn" title="' +
             esc(shares.map((s) => "slot " + s.slot + " is also claimed by " + cueName(s.other))
                       .join("; ") +
                 " \u2014 mark either Feed or Insert shared if that is meant") +
             '">shares ' + shares.length + "</span>"
           : "") +
        (id === standby ? '<span class="flag standby-word">standby</span>' : "") +
      "</div>" +
    "</div>" });

  if (!isGroup || !open) return;

  const header = tree.ids("/godot/cue/" + id + "/headerOrder");
  const footer = tree.ids("/godot/cue/" + id + "/footerOrder");

  /*  THE LINES NOBODY WROTE. A member marked `preset` for this group is got
      ready by this header and still runs where it sits - so it appears here in
      italics, as a reading of the mark rather than as a cue of its own, and
      double-clicking it opens the member it IS. Derived lines come first,
      because that is the order the horizon prepares them in: a written header
      cue may reasonably depend on what the presets set. */
  const derived = tree.ids("/godot/cue/" + id + "/headerDerived");
  const band = (depth + 1) * 16 + 92;

  if (header.length || derived.length) {
    out.push({ key: "band:" + id + ":header",
               html: '<div class="band" style="padding-left:' + band + 'px">header</div>' });

    derived.forEach((child) => presetLine(child, id, depth + 1, out));
    header.forEach((child) => cueRow(child, depth + 1, "header", standby, out));
  }

  tree.ids("/godot/cue/" + id + "/order").forEach((child) =>
    cueRow(child, depth + 1, "member", standby, out));

  if (footer.length) {
    out.push({ key: "band:" + id + ":footer",
               html: '<div class="band" style="padding-left:' + band + 'px">footer</div>' });
    footer.forEach((child) => cueRow(child, depth + 1, "footer", standby, out));
  }
}

/*  A DERIVED HEADER LINE: the member, shown where it is got ready.

    In italics and marked `preset`, because it is not a cue sitting in this
    header - it is a reading of a mark on a cue that lives somewhere else in the
    list. It carries the same `data-pick` as the member's own row, so clicking
    it selects the member and the inspector opens the thing that can actually be
    edited. There is one object, and this is a second view of it - which is why
    its key names the group whose header it is drawn in: `cue:<id>` is already
    the member's own row. */
function presetLine(id, group, depth, out) {
  out.push({ key: "preset:" + group + ":" + id, html:
    '<div class="row derived" data-pick="' + id + '"' +
      ' data-picked="' + (id === selection.picked ? "yes" : "no") + '">' +
      '<div class="gutter"></div>' +
      '<div class="number num">' + esc(tree.cue(id, "number", "")) + "</div>" +
      '<div class="name" style="padding-left:' + (depth * 16) + 'px">' +
        '<span class="twist"></span>' +
        '<span class="kind">' + esc(tree.cue(id, "kind", "memo")) + "</span>" +
        '<span class="text">' + esc(tree.cue(id, "name", "") || "\u2014") + "</span>" +
      "</div>" +
      '<div class="tail">' +
        '<span class="flag" title="got ready by this header; it runs where it sits in the' +
        ' list">preset</span>' +
      "</div>" +
    "</div>" });
}

function renderLists() {
  overlapping = tree.overlaps();

  const lists = tree.ids("/godot/list/order");
  const focus = tree.get("/godot/list/focus", "") || lists[0] || "";

  /*  THE TABS ARE KEPT AS THE ROWS ARE, and for the same reason: each tab is
      something to click - `list.focus`, or the `+` that makes a list - and a
      tab written afresh on every poll can be swapped for a new element
      between the press and the release, so the click does nothing. */
  const tabs = lists.map((id) => ({ key: "tab:" + id, html:
    '<div class="list-tab" data-focus="' + id + '"' +
      ' data-focused="' + (id === focus ? "yes" : "no") + '">' +
      (id === focus ? '<span class="mark">▸ </span>' : "") +
      esc(tree.get("/godot/list/" + id + "/name", id)) +
    "</div>" }));

  tabs.push({ key: "tab:+", html:
    '<div class="list-tab" data-newlist="yes" title="a second list runs beside this one">+</div>' });

  reconcile(el("tabs"), tabs);

  const pane = el("cues");

  /*  THE EMPTY STATE IS A ROW LIKE ANY OTHER, keyed `empty`, so the same
      reconcile that draws the rows takes it away the moment the first one
      arrives, and puts it back when the last one goes. */
  if (!lists.length) {
    reconcile(pane, [{ key: "empty", html:
      '<div class="empty"><div class="line">Rien à faire.</div>' +
      '<div class="under">This show has no cue list yet.</div></div>' }]);
    return;
  }

  const standby = tree.get("/godot/list/" + focus + "/standby", "");
  const cues = tree.ids("/godot/list/" + focus + "/order");
  const persistent = tree.ids("/godot/list/" + focus + "/persistentOrder");

  /*  EMPTY ONLY WHEN BOTH ARE. The engine publishes the persistent section
      apart from `order`, so a list whose only cues are persistent - a bed and
      nothing else - has an empty `order`. Reading `order` alone called that
      list empty and hid its cues, which were drawn nowhere else as rows. */
  if (!cues.length && !persistent.length) {
    reconcile(pane, [{ key: "empty", html:
      '<div class="empty"><div class="line">Rien à faire.</div>' +
      '<div class="under">Nothing in this list.</div></div>' }]);
    return;
  }

  const out = [];
  cues.forEach((id) => cueRow(id, 0, "member", standby, out));

  /*  THE PERSISTENT SECTION, at the foot of the list and marked as its own
      thing (§3.29). It is not part of the order the pointer walks: these cues
      are what should be running at all times, checked after every trigger and
      put back when they are not, so they sit below the last row rather than
      among them - and GO never reaches one. */
  if (persistent.length) {
    out.push({ key: "band:persistent",
               html: '<div class="band" title="checked after every trigger, and put back' +
                     ' when it is not as declared">persistent</div>' });
    persistent.forEach((id) => cueRow(id, 0, "persistent", standby, out));
  }

  reconcile(pane, out);
}

export { renderLists };
