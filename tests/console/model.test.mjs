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
    all of it what a row is drawn from.

    And at the foot of the file, the one question that walks the other way:
    model/remember.js's `openTo`, which climbs from a cue to the list it is in
    and unfolds everything shut between the two, so that a row somebody has
    been sent to is a row that is actually drawn. It reads the same `parent`
    and `role` a cue publishes about itself, which is why it is tested here
    against a served tree rather than beside the storage. */

import { test } from "node:test";
import assert from "node:assert/strict";

import { flatten, tree } from "../../clients/console/plumbing/tree.js";
import "../../clients/console/model/index.js";
import { folded, openForKey, openTo } from "../../clients/console/model/remember.js";
import { esc } from "../../clients/console/views/common.js";

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


/*  THE WALK UP, from here down.

    A cue as the engine publishes one for that walk: what kind it is - which is
    how `openTo` tells a cue from the list, since a list publishes no `kind` -
    where it sits, and how it sits there.

    These run in a Node with no `localStorage` at all, which is the private
    window and a second pin for nothing: `openForKey` saves what it opened, and
    a save that cannot write has to leave the page working. */
const cue = (id, parent, role, kind = "memo") =>
  [leaf("/godot/cue/" + id + "/kind", kind),
   leaf("/godot/cue/" + id + "/parent", parent),
   leaf("/godot/cue/" + id + "/role", role)];

test("opening a row unfolds every container it is inside, and nothing else", () => {
  folded.clear();

  tree.at = reply([
    ...cue("OUTER", "L", "member", "group"),
    ...cue("INNER", "OUTER", "member", "group"),
    ...cue("M", "INNER", "header", "fade"),
    ...cue("ELSEWHERE", "L", "member", "group"),
  ]);

  for (const key of ["OUTER", "INNER", "INNER:header", "ELSEWHERE"]) folded.add(key);

  openTo("M");

  /*  BOTH GROUPS OVER IT AND THE SECTION IT SITS IN. A row is drawn only when
      every one of those is open, so unfolding all but one of them is the same
      as unfolding none: the reveal would land on nothing. */
  assert.equal(folded.has("INNER:header"), false, "the header section it is in");
  assert.equal(folded.has("INNER"), false, "the group that header belongs to");
  assert.equal(folded.has("OUTER"), false, "and the group over that one");

  /*  AND NOT THE WHOLE SHOW. A reveal that unfolded everything would cost the
      reader the shape they had folded their list into, which is the state this
      PR went to the trouble of remembering. */
  assert.equal(folded.has("ELSEWHERE"), true);
});

test("a persistent cue is reached by opening the band at the foot of its list", () => {
  folded.clear();

  tree.at = reply([...cue("BED", "L", "persistent", "media"), ...cue("C1", "L", "member")]);
  folded.add("list:L:persistent");

  openTo("BED");
  assert.equal(folded.has("list:L:persistent"), false);

  /*  IT IS THE ROLE AND NOT THE LIST THAT SAYS SO. A cue sitting plainly in
      the list is not in the persistent band, and opening the band for it would
      unfold a section the reader had shut on purpose. */
  folded.clear();
  folded.add("list:L:persistent");

  openTo("C1");
  assert.equal(folded.has("list:L:persistent"), true);
});

test("the walk up stops rather than going round for ever", () => {
  /*  A `parent` that leads back to where it started cannot happen in a document
      the engine wrote, which is exactly why it is worth a bound: this walk
      reads a reply that arrived half-built, or one from a version that spells
      containment differently, and a browser tab spinning in a loop during a
      show is not a failure anybody can recover from. Reaching the assertions
      below IS the test. */
  folded.clear();

  tree.at = reply([...cue("A", "B", "member", "group"), ...cue("B", "A", "member", "group")]);
  folded.add("A");
  folded.add("B");

  openTo("A");

  assert.equal(folded.has("A"), false);
  assert.equal(folded.has("B"), false);

  /*  And a row the reply does not carry at all is a walk that ends at once -
      a poll that arrived half-built, or a cue deleted between the click and
      the render. */
  tree.at = reply([]);
  folded.add("SOMETHING");

  openTo("GHOST");
  assert.equal(folded.has("SOMETHING"), true, "a cue nobody published unfolds nothing");
});

test("a header line opens the header that draws it, and a row opens where it lives", () => {
  /*  THE TWO FORMS ARE NOT THE SAME WALK, and this is the pair that says so: a
      member marked `preset` for one group can live inside another entirely -
      that is the point of it, the header gets it ready and it runs where it
      sits - so the derived line opens the group whose header DRAWS it, and the
      member's own row opens the group that HOLDS it. */
  const served = reply([
    ...cue("G", "L", "member", "group"),
    ...cue("OTHER", "L", "member", "group"),
    ...cue("D", "OTHER", "member", "fade"),
  ]);

  tree.at = served;
  folded.clear();
  for (const key of ["G", "G:header", "OTHER"]) folded.add(key);

  openForKey("preset:G:D");

  assert.equal(folded.has("G"), false, "the group the line is drawn in");
  assert.equal(folded.has("G:header"), false, "and its header, or the line is still not drawn");
  assert.equal(folded.has("OTHER"), true, "but not where the member itself lives");

  tree.at = served;
  folded.clear();
  for (const key of ["G", "G:header", "OTHER"]) folded.add(key);

  openForKey("cue:D");

  assert.equal(folded.has("OTHER"), false);
  assert.equal(folded.has("G"), true);
  assert.equal(folded.has("G:header"), true);

  folded.clear();
});
