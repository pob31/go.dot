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

/*  THE READ HALF: a reply flattened, the questions every view asks of it, and
    the indexes built once per reply (plumbing/tree.js, model/index.js), plus
    the few spellings every view shares (views/common.js). All of it pure, and
    all of it what a row is drawn from. */

import { test } from "node:test";
import assert from "node:assert/strict";

import { flatten, tree } from "../../clients/console/plumbing/tree.js";
import "../../clients/console/model/index.js";
import { esc, seconds } from "../../clients/console/views/common.js";

/*  A leaf as the engine serves one, and a container of them. */
const leaf = (path, value) => ({ FULL_PATH: path, TYPE: "s", VALUE: [value] });
const box = (path, contents) => ({ FULL_PATH: path, CONTENTS: contents });

function reply(leaves) {
  const contents = {};
  leaves.forEach((node, n) => { contents["n" + n] = node; });
  return flatten(box("/", contents), {});
}

test("a reply is flattened to every node by its address, containers included", () => {
  const at = flatten(box("/godot", {
    cue: box("/godot/cue", { A: box("/godot/cue/A", { name: leaf("/godot/cue/A/name", "Thunder") }) }),
  }), {});

  assert.deepEqual(Object.keys(at).sort(),
                   ["/godot", "/godot/cue", "/godot/cue/A", "/godot/cue/A/name"]);

  /*  The WHOLE node, not its value: the inspector is built out of what a node
      says about itself. */
  assert.equal(at["/godot/cue/A/name"].TYPE, "s");
});

test("a value is the first of VALUE, and a missing or null one is the fallback", () => {
  tree.at = reply([leaf("/godot/cue/A/name", "Thunder"),
                   { FULL_PATH: "/godot/cue/A/notes", TYPE: "s", VALUE: [null] }]);

  assert.equal(tree.cue("A", "name", "?"), "Thunder");
  assert.equal(tree.cue("A", "notes", "none"), "none");
  assert.equal(tree.cue("A", "nothing", "none"), "none");
});

test("an order is a list of identifiers, and an empty one is none", () => {
  tree.at = reply([leaf("/godot/list/L/order", "  A B\tC "), leaf("/godot/list/M/order", "")]);

  assert.deepEqual(tree.ids("/godot/list/L/order"), ["A", "B", "C"]);
  assert.deepEqual(tree.ids("/godot/list/M/order"), []);
  assert.deepEqual(tree.ids("/godot/list/N/order"), []);
});

test("a cue's triggers are found by asking each trigger, once per reply", () => {
  tree.at = reply([leaf("/godot/trigger/T1/cue", "A"), leaf("/godot/trigger/T2/cue", "B"),
                   leaf("/godot/trigger/T3/cue", "A"), leaf("/godot/trigger/T4/kind", "osc")]);

  assert.deepEqual(tree.triggersOf("A"), ["T1", "T3"]);
  assert.deepEqual(tree.triggersOf("B"), ["T2"]);
  assert.deepEqual(tree.triggersOf("C"), []);

  /*  A NEW REPLY IS A NEW INDEX. An index that outlived its reply would answer
      with a trigger that has since been deleted - the stale answer 5.9's index
      was filed by reply to prevent. */
  tree.at = reply([leaf("/godot/trigger/T2/cue", "A")]);

  assert.deepEqual(tree.triggersOf("A"), ["T2"]);
  assert.deepEqual(tree.triggersOf("B"), []);
});

test("an overlap is told to both cues of the pair, under the slot's name", () => {
  tree.at = reply([leaf("/godot/slot/order", "S1 S2"),
                   leaf("/godot/slot/S1/name", "WFS in 1"),
                   leaf("/godot/slot/S1/overlaps", "A B A C"),
                   leaf("/godot/slot/S2/overlaps", "D E")]);

  const found = tree.overlaps();

  assert.deepEqual(found.get("A"), [{ slot: "WFS in 1", other: "B" }, { slot: "WFS in 1", other: "C" }]);
  assert.deepEqual(found.get("B"), [{ slot: "WFS in 1", other: "A" }]);

  /*  A slot nobody named is called by its identifier rather than by nothing. */
  assert.deepEqual(found.get("E"), [{ slot: "S2", other: "D" }]);
});

test("markup is escaped for the double-quoted attributes the page writes", () => {
  assert.equal(esc('a & b <c> "d"'), "a &amp; b &lt;c&gt; &quot;d&quot;");
  assert.equal(esc(42), "42");
});

test("a duration is said in seconds, and nothing is said for none", () => {
  assert.equal(seconds(2.5), "2.5s");
  assert.equal(seconds(0), "");
  assert.equal(seconds("soon"), "");
});
