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

/*  THE VERY FIRST DRAG OF A PAGE, WHICH NEEDS A FILE OF ITS OWN.

    `gestures/drag.js` keeps the insertion mark in a module variable, null until
    the first landing of a session draws one. A review found that the first
    `dragover` aimed at the bottom of the pane's last element read
    `null.nextElementSibling` and threw out of the handler - so the event was
    never cancelled, the browser drew a barred cursor, no words were shown and
    the drop did nothing. It cured itself the moment the pointer crossed any
    other row, which is why nothing caught it: the drag suite's own first
    `dragover` arms the mark long before any case aims at the last row, and a
    hand checking by eye almost always crosses a row on the way down.

    So the case has to be the FIRST thing that happens to a freshly loaded
    module, and `node --test` gives each file its own process. That is the whole
    reason this file is separate, and it is the reason it holds one scene and
    two assertions rather than growing into a second drag suite: anything added
    below the first `dragover` here is a case that would be equally at home in
    drag.test.mjs, and putting it here would quietly cost this one its coldness.

    THE SHAPE THAT BREAKS IT is a list whose last drawn element is the target,
    aimed at in its lower half - "put this after the last cue", which is an
    ordinary thing to want and, on the first drag of a session, was the one
    thing that did nothing at all. */

import { test } from "node:test";
import assert from "node:assert/strict";

import { installDocument } from "./fake-dom.mjs";

globalThis.location = { protocol: "file:", host: "" };
globalThis.window = { setTimeout: () => 0 };

const page = installDocument();

const wired = {};

page.addEventListener = (kind, handler) => { wired[kind] = handler; };

/*  A pane of two rows, ten tall each, the second of them last - which is what
    makes `nextElementSibling` null and the old identity test pass. */
function element(name, { key = "", pick = "", top = 0, height = 10 } = {}) {
  const node = {
    nodeName: name.toUpperCase(),
    children: [],
    parentNode: null,
    dataset: {},
    className: "",
    style: { getPropertyValue: () => "", setProperty: () => {}, removeProperty: () => {} },
    classList: { add: () => {}, remove: () => {} },
    setAttribute: () => {},
    removeAttribute: () => {},
    getBoundingClientRect: () => ({ top, height, bottom: top + height }),
  };

  if (key) node.dataset.key = key;
  if (pick) node.dataset.pick = pick;

  node.closest = (selector) => {
    for (let up = node; up; up = up.parentNode) {
      if (selector.indexOf("#cues") >= 0 && up.dataset && up.dataset.pane === "cues") return up;
      if (selector.indexOf("[data-pick]") >= 0 && up.dataset && up.dataset.pick) return up;
    }

    return null;
  };

  node.insertBefore = (child, before) => {
    child.parentNode = node;

    const at = before ? node.children.indexOf(before) : -1;

    if (at < 0) node.children.push(child); else node.children.splice(at, 0, child);

    return child;
  };

  node.removeChild = (child) => {
    const at = node.children.indexOf(child);

    if (at >= 0) node.children.splice(at, 1);

    child.parentNode = null;
    return child;
  };

  node.appendChild = (child) => node.insertBefore(child, null);

  Object.defineProperty(node, "nextElementSibling", {
    get() {
      if (!node.parentNode) return null;

      const among = node.parentNode.children;

      return among[among.indexOf(node) + 1] || null;
    },
  });

  Object.defineProperty(node, "lastElementChild", {
    get() { return node.children[node.children.length - 1] || null; },
  });

  return node;
}

const pane = element("div");

pane.dataset.pane = "cues";
pane.dataset.list = "L";

const first = element("div", { key: "cue:A", pick: "A", top: 0 });
const last = element("div", { key: "cue:B", pick: "B", top: 10 });

pane.appendChild(first);
pane.appendChild(last);

page.createElement = (name) => element(name);
page.getElementById = (id) => (id === "cues" ? pane : null);

const leaf = (path, value) => ({ FULL_PATH: path, TYPE: "s", ACCESS: 3, VALUE: [value] });

const { tree } = await import("../../clients/console/plumbing/tree.js");

tree.at = {};

for (const node of [leaf("/godot/list/L/order", "A B"),
                    leaf("/godot/cue/A/kind", "memo"), leaf("/godot/cue/A/parent", "L"),
                    leaf("/godot/cue/A/role", "member"), leaf("/godot/cue/A/name", "A"),
                    leaf("/godot/cue/B/kind", "memo"), leaf("/godot/cue/B/parent", "L"),
                    leaf("/godot/cue/B/role", "member"), leaf("/godot/cue/B/name", "B")]) {
  tree.at[node.FULL_PATH] = node;
}

await import("../../clients/console/gestures/drag.js");

test("the first drop of a page, aimed past the last row, is drawn and allowed", () => {
  const lift = { target: first, dataTransfer: { effectAllowed: "", setData: () => {},
                                                setDragImage: () => {} },
                 preventDefault() {} };

  wired.dragstart(lift);

  /*  ONE `dragover`, AND IT IS THE FIRST OF THE SESSION - which is the whole
      point of the file. Aimed at nine tenths of the last row: the lower half,
      so the landing is "after B", and B is the pane's last element, so the walk
      that looks for what comes after it finds nothing. */
  const over = { target: last, clientY: 19, prevented: false,
                 dataTransfer: { dropEffect: "" },
                 preventDefault() { over.prevented = true; } };

  wired.dragover(over);

  /*  BEFORE THE FIX this threw here, out of the handler, and both of these were
      false: the page said nothing, cancelled nothing, and the browser refused
      the drop the reader was making. */
  assert.equal(over.prevented, true, "the drop is allowed");
  assert.equal(over.dataTransfer.dropEffect, "move");

  /*  And the mark is in the pane, at its foot - "after the last element" is
      inserted before nothing, which is the end. */
  const mark = pane.children.find((child) => (child.className || "").indexOf("drop-line") >= 0);

  assert.ok(mark, "the insertion mark is drawn");
  assert.equal(pane.children[pane.children.length - 1], mark, "at the foot of the pane");

  wired.dragend({ target: first });
});
