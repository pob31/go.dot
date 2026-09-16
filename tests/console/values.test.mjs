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

/*  WHAT A CONTROL SHOWS, and the refresh that keeps it true.

    views/values.js is asked by the inspector and by Didi's time columns, which
    is exactly why it is worth a test: the same attribute is now on screen in
    two places at once, with one rule - the blank nought - that applies in one
    of them and not in the other. A rule that fired in both would empty the
    inspector's `preWait`; a rule that fired in neither would put a grey column
    of noughts down the cue list. */

import { test, beforeEach } from "node:test";
import assert from "node:assert/strict";

import { FakeElement, installDocument } from "./fake-dom.mjs";

const document = installDocument();

const { tree } = await import("../../clients/console/plumbing/tree.js");
const { isList, shownValue, commitText, refreshFields } =
  await import("../../clients/console/views/values.js");

/*  A leaf as the engine serves one. `access` 3 is read-write. */
function leaf(path, value, type = "d", access = 3) {
  return { FULL_PATH: path, TYPE: type, ACCESS: access, VALUE: value === undefined ? [] : [value] };
}

function serve(nodes) {
  tree.at = {};
  for (const node of nodes) tree.at[node.FULL_PATH] = node;
}

/*  A box as a row draws one: the address it writes, and whether it asks for the
    blank-nought rule. */
function box(address, value, blankZero) {
  const input = new FakeElement("input");

  input.type = "number";
  input.value = value;
  input.dataset.set = address;

  if (blankZero) input.dataset.blankZero = "yes";

  return input;
}

function pane(children) {
  const element = new FakeElement("div");

  for (const child of children) element.appendChild(child);

  /*  The fake DOM has no selectors, and this is the only one the code uses. */
  element.querySelectorAll = () => element.childNodes.filter((n) => n.dataset && n.dataset.set);
  return element;
}

beforeEach(() => {
  document.activeElement = null;
  tree.at = {};
});

test("a list is shown as its elements, space-separated, and an empty one as nothing", () => {
  assert.equal(shownValue({ TYPE: "ddd", VALUE: [0, 0.5, 1] }), "0 0.5 1");
  assert.equal(shownValue({ ACCESS: 3, VALUE: [] }), "");
});

test("a scalar is shown as its value, and a node with nothing to say as nothing", () => {
  assert.equal(shownValue({ TYPE: "d", VALUE: [1.5] }), 1.5);
  assert.equal(shownValue({ TYPE: "s", VALUE: ["Thunder"] }), "Thunder");
  assert.equal(shownValue({ TYPE: "d", VALUE: [] }), "");
});

test("only a box that asks for it shows a nought as an empty box", () => {
  const asking = box("/godot/cue/A/preWait", "", true);
  const plain = box("/godot/cue/A/preWait", "", false);
  const node = { TYPE: "d", VALUE: [0] };

  assert.equal(shownValue(node, asking), "");
  assert.equal(shownValue(node, plain), 0);

  /*  And it is the NOUGHT that is blanked, not every small number: a wait of a
      fiftieth of a second is a wait somebody typed. */
  assert.equal(shownValue({ TYPE: "d", VALUE: [0.02] }, asking), 0.02);
  assert.equal(shownValue({ TYPE: "d", VALUE: [1.5] }, asking), 1.5);

  /*  A node with nothing to say is not a nought, and must not read as one. */
  assert.equal(shownValue({ TYPE: "d", VALUE: [] }, asking), "");
});

test("a refresh writes the engine's value into a box nobody is using", () => {
  serve([leaf("/godot/cue/A/preWait", 1.5)]);

  const input = box("/godot/cue/A/preWait", "", true);

  refreshFields(pane([input]));
  assert.equal(input.value, 1.5);

  /*  And takes it away again when the value goes back to nought - which is what
      an undo looks like from here. */
  serve([leaf("/godot/cue/A/preWait", 0)]);
  refreshFields(pane([input]));
  assert.equal(input.value, "");
});

test("a refresh never writes under somebody's hands", () => {
  serve([leaf("/godot/cue/A/preWait", 1.5)]);

  /*  Focused: the operator is in the box. */
  const focused = box("/godot/cue/A/preWait", "2.5", true);
  document.activeElement = focused;
  refreshFields(pane([focused]));
  assert.equal(focused.value, "2.5");

  /*  Not focused, but holding an edit that has not been committed. */
  document.activeElement = null;
  const dirty = box("/godot/cue/A/preWait", "3.5", true);
  dirty.dataset.dirty = "yes";
  refreshFields(pane([dirty]));
  assert.equal(dirty.value, "3.5");
});

test("a refresh leaves alone a box whose node the reply does not carry", () => {
  /*  A cue deleted, or a reply that arrived half-built: the box says what it
      last said rather than emptying itself. */
  serve([]);

  const input = box("/godot/cue/A/preWait", "1.5", true);

  refreshFields(pane([input]));
  assert.equal(input.value, "1.5");
});

test("a list node is told from a container and from a scalar", () => {
  assert.equal(isList({ TYPE: "d", VALUE: [1] }), false);
  assert.equal(isList({ TYPE: "s", VALUE: ["x"] }), false);
  assert.equal(isList({ TYPE: "ddd", VALUE: [1, 2, 3] }), true);

  /*  An empty list: no TYPE at all, and no CONTENTS - which is what tells it
      from a container (§14.6). */
  assert.equal(isList({ ACCESS: 3, VALUE: [] }), true);
  assert.equal(isList({ CONTENTS: {} }), false);
});

test("a box that blanks its nought commits one, and an unreadable box commits what it holds", () => {
  /*  THE OTHER HALF OF THE RULE, and the bug that made it necessary: clearing
      a time column sent an empty string, which the engine refuses as
      `type-mismatch` - so the one gesture the blank nought teaches was the one
      gesture the page could not perform. */
  const asking = box("/godot/cue/A/preWait", "", true);

  assert.equal(commitText(asking), "0");

  asking.value = "1.5";
  assert.equal(commitText(asking), "1.5");

  /*  A BOX NOBODY CAN READ IS NOT AN EMPTY ONE. `1e` and `-` leave a number
      box with an empty value and `badInput` set: that is not a nought
      somebody typed, so it goes as it stands and is refused as it deserves. */
  const unreadable = box("/godot/cue/A/preWait", "", true);
  unreadable.validity = { badInput: true };
  assert.equal(commitText(unreadable), "");

  /*  A box that never asked for the rule says what it holds. */
  const plain = box("/godot/cue/A/preWait", "", false);
  assert.equal(commitText(plain), "");

  /*  And a checkbox is the two words the engine spells. */
  const ticked = box("/godot/cue/A/enabled", "", false);
  ticked.type = "checkbox";
  ticked.checked = true;
  assert.equal(commitText(ticked), "true");
  ticked.checked = false;
  assert.equal(commitText(ticked), "false");
});

test("a refresh leaves a just-committed box alone until the engine agrees, and not for ever", () => {
  serve([leaf("/godot/cue/A/preWait", 2)]);

  const input = box("/godot/cue/A/preWait", "1.5", true);

  //  Committed a moment ago: the reply in flight still says the old value.
  input.dataset.sent = "1.5";
  input.dataset.sentAt = String(Date.now());

  refreshFields(pane([input]));
  assert.equal(input.value, "1.5");

  /*  The engine catches up: the mark goes, and the box is left saying what it
      already said - the same value, in the spelling the operator typed. */
  serve([leaf("/godot/cue/A/preWait", 1.5)]);
  refreshFields(pane([input]));
  assert.equal(String(input.value), "1.5");
  assert.equal(input.dataset.sent, undefined);

  /*  AND A COMMIT THAT WAS REFUSED DOES NOT HOLD THE BOX FOR EVER: a refusal
      never changes the tree, so after a second and a half the box goes back to
      what the engine says, which is what the red banner is explaining. */
  serve([leaf("/godot/cue/A/preWait", 2)]);
  input.value = "9";
  input.dataset.sent = "9";
  input.dataset.sentAt = String(Date.now() - 2000);

  refreshFields(pane([input]));
  assert.equal(input.value, 2);
});
