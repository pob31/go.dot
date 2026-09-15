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

/*  THE KEYED RECONCILER, held to what its own comment promises.

    views/reconcile.js says a row whose markup has not changed is left alone,
    element and all; that a changed row keeps its element and has only the
    difference written; that one row moved costs one move whichever way it
    went; that a row leaving costs one removal and moves nothing. Each of those
    is a click that does or does not get lost between a press and a release
    (5.9's M24 found the lost ones), and none of them can be seen on a screen.
    They are counted here, on the least DOM that reconcile touches
    (fake-dom.mjs). */

import { test, beforeEach } from "node:test";
import assert from "node:assert/strict";

import { log, FakeElement, installDocument, make } from "./fake-dom.mjs";

const document = installDocument();

/*  After the global it needs at load: the module makes its <template> then. */
const { morph, reconcile } = await import("../../clients/console/views/reconcile.js");

/*  A row as the page draws one - a key and its markup - with the markup
    written as fake-dom's JSON. */
function row(key, text, attributes = {}) {
  return { key, html: JSON.stringify(["div", attributes, text]) };
}

function rowsOf(keys) {
  return keys.map((key) => row(key, "row " + key));
}

function keysOn(container) {
  return container.children.map((child) => child.dataset.key);
}

function pane(keys) {
  const container = new FakeElement("div");
  reconcile(container, rowsOf(keys), make);
  log.length = 0;
  return container;
}

/*  What a reconcile changed, by kind: `move` is a node that already stood in
    the pane put somewhere else in it, which is what loses a press. */
function count(kind) {
  return log.filter(([what]) => what === kind).length;
}

beforeEach(() => {
  document.activeElement = null;
  log.length = 0;
});

test("rows are made in the order drawn, each carrying its key", () => {
  const container = new FakeElement("div");

  reconcile(container, rowsOf(["a", "b", "c"]), make);

  assert.deepEqual(keysOn(container), ["a", "b", "c"]);
  assert.equal(container.children[1].textContent, "row b");
});

test("a row whose markup has not changed is left alone, element and all", () => {
  const container = pane(["a", "b", "c"]);
  const before = container.children.slice();

  reconcile(container, rowsOf(["a", "b", "c"]), make);

  /*  NOTHING AT ALL: not a move, not an attribute, not a text. */
  assert.deepEqual(log, []);
  container.children.forEach((child, n) => assert.equal(child, before[n]));
});

test("a changed row keeps its element and every button on it", () => {
  /*  The kill button's story (reconcile.js): a playing run's row changes on
      every poll, and when it was replaced whole the button under a press was a
      different element by the release. */
  const runRow = (position) => ({
    key: "run:R1",
    html: JSON.stringify(["div", { class: "run" }, ["span", {}, position],
                                                   ["button", { "data-kill": "R1" }, "kill"]]),
  });

  const container = new FakeElement("div");
  reconcile(container, [runRow("0:01")], make);

  const live = container.children[0];
  const button = live.children[1];

  log.length = 0;
  reconcile(container, [runRow("0:02")], make);

  assert.equal(container.children[0], live);
  assert.equal(live.children[1], button);
  assert.equal(live.textContent, "0:02kill");

  /*  And the one text node that changed is the one thing written on screen. */
  assert.equal(count("text"), 1);
  assert.equal(count("move") + count("insert") + count("remove") + count("replace"), 0);
});

test("a changed attribute is written, and one the new markup lacks is taken away", () => {
  const container = new FakeElement("div");

  reconcile(container, [row("a", "A", { class: "standby", title: "next" })], make);
  const live = container.children[0];

  reconcile(container, [row("a", "A", { class: "done" })], make);

  assert.equal(container.children[0], live);
  assert.equal(live.getAttribute("class"), "done");
  assert.equal(live.hasAttribute("title"), false);

  /*  The key survives the morph: it is set on the fresh element first. */
  assert.equal(live.dataset.key, "a");
});

test("one row moved down costs one move", () => {
  const container = pane(["a", "b", "c", "d", "e"]);

  reconcile(container, rowsOf(["a", "c", "d", "b", "e"]), make);

  assert.deepEqual(keysOn(container), ["a", "c", "d", "b", "e"]);
  assert.equal(count("move"), 1);
  assert.equal(log.find(([what]) => what === "move")[1].dataset.key, "b");
});

test("one row moved up costs one move", () => {
  const container = pane(["a", "b", "c", "d", "e"]);

  reconcile(container, rowsOf(["a", "d", "b", "c", "e"]), make);

  assert.deepEqual(keysOn(container), ["a", "d", "b", "c", "e"]);
  assert.equal(count("move"), 1);
  assert.equal(log.find(([what]) => what === "move")[1].dataset.key, "d");
});

test("one row carried from the top to the bottom costs one move, not one per row it passed", () => {
  const container = pane(["a", "b", "c", "d", "e"]);

  reconcile(container, rowsOf(["b", "c", "d", "e", "a"]), make);

  assert.deepEqual(keysOn(container), ["b", "c", "d", "e", "a"]);
  assert.equal(count("move"), 1);
});

test("a block of rows moved above others costs one move per row in the block", () => {
  /*  An open group moved up: its rows are fetched, and the rows they overtook
      are not touched. */
  const container = pane(["a", "b", "c", "d", "e"]);

  reconcile(container, rowsOf(["c", "d", "a", "b", "e"]), make);

  assert.deepEqual(keysOn(container), ["c", "d", "a", "b", "e"]);
  assert.equal(count("move"), 2);
});

test("a row leaving the middle costs one removal and moves nothing", () => {
  const container = pane(["a", "b", "c", "d"]);
  const kept = [container.children[0], container.children[2], container.children[3]];

  reconcile(container, rowsOf(["a", "c", "d"]), make);

  assert.deepEqual(keysOn(container), ["a", "c", "d"]);
  assert.equal(count("remove"), 1);
  assert.equal(count("move"), 0);
  container.children.forEach((child, n) => assert.equal(child, kept[n]));
});

test("a row arriving in the middle is made there, and moves nothing", () => {
  const container = pane(["a", "c"]);

  reconcile(container, rowsOf(["a", "b", "c"]), make);

  assert.deepEqual(keysOn(container), ["a", "b", "c"]);
  assert.equal(count("insert"), 1);
  assert.equal(count("move"), 0);
});

test("a key wanted twice is drawn twice, the second made unique", () => {
  /*  One cue can be on screen twice - its own row and the derived line in a
      group's header - and a reconcile that lost one would be the worse bug. */
  const container = new FakeElement("div");

  reconcile(container, [row("cue:X", "own row"), row("cue:X", "header line")], make);

  assert.deepEqual(keysOn(container), ["cue:X", "cue:X#2"]);
  assert.equal(container.children[1].textContent, "header line");
});

test("anything in the pane that is not a keyed row goes", () => {
  const container = new FakeElement("div");
  const stray = new FakeElement("p");

  container.appendChild(stray);
  reconcile(container, rowsOf(["a"]), make);

  assert.deepEqual(keysOn(container), ["a"]);
  assert.equal(stray.parentNode, null);
});

test("morph: a child of another kind is replaced alone, and its siblings stay", () => {
  const live = make(JSON.stringify(["div", {}, ["span", {}, "late"], ["button", {}, "kill"]]));
  const button = live.children[1];

  morph(live, make(JSON.stringify(["div", {}, ["em", {}, "late"], ["button", {}, "kill"]])));

  assert.equal(live.children[0].nodeName, "EM");
  assert.equal(live.children[1], button);
});

test("morph: what the fresh tree has beyond the live one is added, and what it lacks goes", () => {
  const live = make(JSON.stringify(["div", {}, ["span", {}, "a"], ["span", {}, "b"], ["span", {}, "c"]]));

  morph(live, make(JSON.stringify(["div", {}, ["span", {}, "a"]])));
  assert.equal(live.textContent, "a");

  morph(live, make(JSON.stringify(["div", {}, ["span", {}, "a"], ["b", {}, "!"]])));
  assert.equal(live.textContent, "a!");
  assert.equal(live.children[1].nodeName, "B");
});

test("morph: a focused field is left as it stands", () => {
  /*  The rule refreshFields and renderAim keep: a value written under
      somebody's typing is a control fighting the person using it. */
  const live = make(JSON.stringify(["input", { value: "typing" }]));

  document.activeElement = live;
  morph(live, make(JSON.stringify(["input", { value: "from the poll" }])));

  assert.equal(live.getAttribute("value"), "typing");
  assert.deepEqual(log.filter(([, node]) => node === live), []);
});
