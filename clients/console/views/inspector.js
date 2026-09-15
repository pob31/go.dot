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

/*  THE INSPECTOR: every field of the picked cue or trigger, built out of what
    each node says about itself (§14.2), and the gestures that act on it. */

import { tree } from "../plumbing/tree.js";
import { selection } from "../model/selection.js";
import { el, esc } from "./common.js";

/*  Every address in the tree that belongs to this object, whatever owner word
    publishes it. A media cue carries `/godot/cue/<id>/name` and
    `/godot/media/<id>/file` at once, and the inspector shows both without ever
    being told that media cues have files. */
function fieldsFor(id) {
  const found = [];
  const pattern = new RegExp("^/godot/([a-z]+)/" + id + "/([A-Za-z]+)$");

  for (const address of Object.keys(tree.at)) {
    const match = pattern.exec(address);
    if (!match) continue;

    const node = tree.at[address];
    if (!node || (typeof node.TYPE !== "string" && !isList(node))) continue;   // a container

    found.push({ address: address, owner: match[1], name: match[2], node: node });
  }

  /*  The common rows first and the kind's own after, alphabetical inside each,
      which is the order the tree itself is in - so a reader who has seen the
      namespace is not learning a second arrangement here. */
  found.sort((a, b) =>
    (a.owner === "cue" ? 0 : 1) - (b.owner === "cue" ? 0 : 1)
      || a.owner.localeCompare(b.owner)
      || a.name.localeCompare(b.name));

  return found;
}

/*  A LIST NODE - a route's `gains`, a fade's `points` - carries one type tag
    per element, so its TYPE is as long as its value (§14.6). It is shown and
    typed as the space-separated text the file uses: one field for the whole
    list and one node.set on commit, which the engine's door parses element by
    element. Read as a scalar, it showed the first element alone, and an empty
    one was not shown at all. The curve editor (PR 5.16b) will draw `points`;
    until then this is how they are read. */
function isList(node) {
  if (typeof node.TYPE === "string") return node.TYPE.length !== 1;

  /*  AN EMPTY LIST IS SERVED WITH NO TYPE AT ALL, there being no tag to give
      zero values, and with no CONTENTS either - which is what tells it from a
      container. */
  return !node.CONTENTS && node.ACCESS !== undefined;
}

function shownValue(node) {
  if (isList(node)) return Array.isArray(node.VALUE) ? node.VALUE.join(" ") : "";

  return Array.isArray(node.VALUE) && node.VALUE.length ? node.VALUE[0] : "";
}

function controlFor(field) {
  const node = field.node;
  const writable = (Number(node.ACCESS) & 2) !== 0;
  const value = shownValue(node);
  const range = Array.isArray(node.RANGE) && node.RANGE.length ? node.RANGE[0] : null;
  const set = ' data-set="' + esc(field.address) + '"';

  /*  READ-ONLY IS SHOWN AND NOT HIDDEN. `kind`, `parent`, `index` and `role`
      are derived rather than decided (§4.10), and they are exactly what
      somebody looks at when a cue is not where they thought it was. */
  if (!writable) return '<div class="ro">' + esc(value === "" ? "—" : value) + "</div>";

  if (range && Array.isArray(range.VALS)) {
    return "<select" + set + ">" +
      range.VALS.map((v) =>
        '<option value="' + esc(v) + '"' + (String(v) === String(value) ? " selected" : "") +
        ">" + esc(v) + "</option>").join("") +
      "</select>";
  }

  if (node.TYPE === "T" || node.TYPE === "F") {
    return '<input type="checkbox"' + set + (value === true ? " checked" : "") + ">";
  }

  /*  Before the number test, which an empty list's TYPE would pass: "" is
      found at the start of "ifdh". */
  if (isList(node)) {
    return '<input type="text" class="list"' + set + ' value="' + esc(value) + '">';
  }

  if ("ifdh".indexOf(node.TYPE) >= 0) {
    const step = "ih".indexOf(node.TYPE) >= 0 ? "1" : "any";
    const min = range && range.MIN !== undefined ? ' min="' + esc(range.MIN) + '"' : "";
    const max = range && range.MAX !== undefined ? ' max="' + esc(range.MAX) + '"' : "";

    return '<input type="number" step="' + step + '"' + min + max + set +
           ' value="' + esc(value) + '">';
  }

  return '<input type="text"' + set + ' value="' + esc(value) + '">';
}

const KINDS = ["memo", "media", "fade", "stop", "osc", "group"];

/*  What a trigger says in one line, which is enough to tell two of them apart
    without opening either. */
function triggerSummary(id) {
  const kind = tree.trigger(id, "kind", "osc");

  if (kind === "osc") return tree.trigger(id, "address", "") || "no address";
  if (kind === "clock") return tree.trigger(id, "at", "") || "no time";

  const channel = Number(tree.trigger(id, "channel", 0));

  return tree.trigger(id, "type", "noteOn") + " " + tree.trigger(id, "number", 0) +
         (channel ? " ch " + channel : " any ch");
}

function renderInspector() {
  const pane = el("inspect");

  /*  A TRIGGER IS INSPECTED LIKE ANYTHING ELSE, because it is addressed like
      anything else: the fields come from `/godot/trigger/<id>/*` through the
      same lookup that reads a cue's. What it needs of its own is a way back to
      the cue it belongs to. */
  if (selection.picked && tree.node("/godot/trigger/" + selection.picked + "/kind")) {
    const owner = tree.trigger(selection.picked, "cue", "");
    const fields = fieldsFor(selection.picked);
    const signature = "trigger|" + selection.picked + "|" + fields.map((f) => f.address).join(",");

    if (pane.dataset.showing !== signature) {
      pane.dataset.showing = signature;

      let out =
        '<div class="who"><span class="text">' + esc(triggerSummary(selection.picked)) +
        '</span><span class="kind">trigger</span>' +
        '<span class="id num">' + esc(selection.picked) + "</span></div>" +
        '<div class="back" data-pick="' + esc(owner) + '">\u2190 ' +
        esc(tree.cue(owner, "name", "") || owner) + "</div>";

      for (const field of fields) {
        out += '<div class="field"><label title="' + esc(field.node.DESCRIPTION || "") + '">' +
               esc(field.name) + "</label>" + controlFor(field) + "</div>";
      }

      out += '<div class="group-head">structure</div><div class="actions">' +
             '<button class="danger" data-delete="' + esc(selection.picked) + '">delete</button></div>';

      pane.innerHTML = out;
      return;
    }

    refreshFields(pane);
    return;
  }

  if (!selection.picked || !tree.node("/godot/cue/" + selection.picked + "/kind")) {
    pane.innerHTML = '<div class="empty"><div class="line">Nothing to declare.</div>' +
                     '<div class="under">Click a cue to see what it says.</div></div>';
    pane.dataset.showing = "";
    return;
  }

  const kind = tree.cue(selection.picked, "kind", "memo");
  const fields = fieldsFor(selection.picked);
  const signature = selection.picked + "|" + fields.map((f) => f.address).join(",");

  /*  REBUILT ONLY WHEN THE SHAPE CHANGES, and that is not an optimisation. The
      poll arrives up to ten times a second, and rebuilding the panel on each one
      would take the cursor out of the box somebody is typing in - which is the
      ordinary way a live-updating editor becomes unusable. So the markup is
      made once per selection, and the values below are refreshed in place:
      never into a field that has the focus, and never over an edit that has not
      been committed. */
  if (pane.dataset.showing !== signature) {
    pane.dataset.showing = signature;

    const parent = tree.cue(selection.picked, "parent", "");
    const isGroup = kind === "group";
    const hasHeader = tree.ids("/godot/cue/" + selection.picked + "/headerOrder").length > 0;
    const hasFooter = tree.ids("/godot/cue/" + selection.picked + "/footerOrder").length > 0;

    let out =
      '<div class="who"><span class="text">' + esc(tree.cue(selection.picked, "name", "") || "—") +
      '</span><span class="kind">' + esc(kind) + "</span>" +
      '<span class="id num">' + esc(selection.picked) + "</span></div>";

    let owner = null;

    for (const field of fields) {
      if (field.owner !== owner) {
        owner = field.owner;
        out += '<div class="group-head">' + esc(owner) + "</div>";
      }

      out += '<div class="field"><label title="' + esc(field.node.DESCRIPTION || "") + '">' +
             esc(field.name) + "</label>" + controlFor(field) + "</div>";
    }

    /*  THE CUE'S TRIGGERS, listed rather than folded into the fields above:
        they are objects with identifiers of their own, a cue may have several,
        and each is edited on its own page. §3.7 gives the list to any cue. */
    out += '<div class="group-head">triggers</div>';

    for (const trigger of tree.triggersOf(selection.picked)) {
      const off = tree.trigger(trigger, "enabled", true) === false;

      out += '<div class="trigger-row" data-pick="' + trigger + '">' +
             '<span class="kind">' + esc(tree.trigger(trigger, "kind", "osc")) + "</span>" +
             '<span class="what' + (off ? " off" : "") + '">' +
             esc(triggerSummary(trigger)) + "</span></div>";
    }

    out += '<div class="actions">';

    for (const k of ["osc", "midi", "clock"]) {
      out += '<button data-trigger="' + k + '">+ ' + k + "</button>";
    }

    out += "</div>";

    /*  UP AND DOWN, which is `object.move` at one index either way. Dragging
        is what a desktop UI will do and is not what a first pass should try
        over a poll: a row that moves under the pointer while the tree is being
        re-fetched is a fight nobody wins. Two buttons say the same thing and
        cannot half-happen.

        The parent goes with the index because `object.move` takes both, and
        passing the one it already has is what makes this a REORDER rather than
        a reparent - the same command, aimed at the place it is. */
    const siblings = tree.ids((tree.node("/godot/list/" + parent + "/order")
                                 ? "/godot/list/" : "/godot/cue/") + parent + "/order");
    const at = siblings.indexOf(selection.picked);

    out += '<div class="group-head">structure</div><div class="actions">';

    /*  PARK, FIRST AND BY ITSELF. The gutter does this too and is quicker once
        somebody knows it is there; this is where they find out. A cue the
        pointer may not stand on - inside an automatic group, in a header - is
        refused by the engine and the refusal says which, which is a better
        answer than a button that is not offered. */
    out += '<button data-park="' + esc(selection.picked) + '" title="GO will act on this cue">' +
           "\u25B8 standby here</button>";

    out += '<button data-move="' + (at - 1) + '" data-parent="' + esc(parent) + '"' +
           (at > 0 ? "" : " disabled") + ' title="one earlier">▲</button>';
    out += '<button data-move="' + (at + 1) + '" data-parent="' + esc(parent) + '"' +
           (at >= 0 && at < siblings.length - 1 ? "" : " disabled") +
           ' title="one later">▼</button>';

    /*  A NEW CUE GOES AFTER THIS ONE, in this one's parent, which is where a
        designer writing a list wants it and is the only guess that needs no
        second click. A group is offered the other reading as well: inside. */
    for (const k of KINDS) {
      out += '<button data-add="' + k + '" data-into="' + esc(parent) + '"' +
             ' title="after this cue">+ ' + k + "</button>";
    }

    if (isGroup) {
      out += '</div><div class="actions">';

      for (const k of KINDS) {
        out += '<button data-add="' + k + '" data-into="' + esc(selection.picked) + '"' +
               ' title="inside this group">+ ' + k + " inside</button>";
      }

      out += '</div><div class="actions">';
      if (!hasHeader) out += '<button data-role="header">+ header</button>';
      if (!hasFooter) out += '<button data-role="footer">+ footer</button>';
    }

    out += '<button class="danger" data-delete="' + esc(selection.picked) + '">delete</button></div>';
    pane.innerHTML = out;
    return;
  }

  refreshFields(pane);
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

    const value = shownValue(node);

    if (input.type === "checkbox") input.checked = value === true;
    else if (String(input.value) !== String(value)) input.value = value;
  }
}

export { renderInspector };
