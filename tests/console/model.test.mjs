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

    Then the one question that walks the other way: model/remember.js's
    `openTo`, which climbs from a cue to the list it is in and unfolds
    everything shut between the two, so that a row somebody has been sent to is
    a row that is actually drawn. It reads the same `parent` and `role` a cue
    publishes about itself, which is why it is tested here against a served tree
    rather than beside the storage.

    Then what the READER has picked, which is the same kind of thing again and
    no part of the document: model/selection.js's anchor and its set. And at the
    foot, with the least pane a test can get away with, the one gesture whose
    answer the model cannot work out on its own - the range, which is taken over
    ROWS and only then reduced to cues, and so needs rows to be taken over. */

import { test } from "node:test";
import assert from "node:assert/strict";

import { installDocument } from "./fake-dom.mjs";
import { flatten, tree } from "../../clients/console/plumbing/tree.js";
import "../../clients/console/model/index.js";
import { folded, openForKey, openTo } from "../../clients/console/model/remember.js";
import { pickAlso, pickNothing, pickOne, pickThrough, selection }
  from "../../clients/console/model/selection.js";
import { esc } from "../../clients/console/views/common.js";
import { view } from "../../clients/console/views/view.js";

/*  THE PAGE THE LAST SECTION'S GESTURE IS MADE ON, installed here because the
    module that reads it is imported here.

    A range is taken over the rows the cue pane is showing, so the handler that
    takes it (gestures/clicks.js) cannot be imported into a Node with no DOM at
    all. This is the least of one: ./fake-dom.mjs's `document`, with the two
    things this gesture asks of it added - which element is the cue pane, and
    the click handler clicks.js wires as it loads. The pane itself is built at
    the foot of the file, where the tests that read it are.

    `location` is for plumbing/link.js, which decides AT LOAD whether it is
    being served by an engine at all; saying `file:` is saying no, which is the
    truth here and means no socket is opened and no command can leave. The
    `document` is also for views/reconcile.js, which makes one template as it
    loads. Nothing below fetches and nothing connects.

    `view.render` is app.js's in a browser and nothing at all here. Every branch
    of the click handler ends by calling it, so it is stubbed rather than left
    to throw - and stubbed rather than counted, because what a click redraws is
    views/didi.js's business and is tested there. */
globalThis.location = { protocol: "file:", host: "" };

const page = installDocument();

let pane = null;

page.getElementById = (id) => (id === "cues" ? pane : null);

/*  A handler that throws until clicks.js replaces it, because the alternative
    is worse: a module that quietly wired nothing would leave every gesture
    below doing nothing at all, and a test that asserts what a click did not
    change would then pass for the emptiest of reasons. */
let clicked = () => { throw new Error("gestures/clicks.js wired no click handler"); };

page.addEventListener = (kind, handler) => { if (kind === "click") clicked = handler; };

await import("../../clients/console/gestures/clicks.js");

view.render = () => {};

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


/*  WHAT IS PICKED, from here down: model/selection.js.

    It is two fields and one thing. `picked` is the ANCHOR - the row the hand is
    on, the row a range is measured from, the row every single-cue gesture on
    this page means - and `chosen` is the whole SET a batch edit writes to. An
    ordinary click makes them agree, which is why a dozen readers of `picked`
    could go on reading it when the set arrived.

    WHICH IS ALSO THE FAILURE WORTH GUARDING. The two coming apart does not show
    up here; it shows up two panes away, as an inspector writing a field to a
    cue nobody chose, or as a set of nine with nothing inspecting it and nothing
    to range from. So every case below ends at `sane`, which is that pair stated
    once. */
function sane(what) {
  const chosen = selection.chosen;

  assert.ok(Array.isArray(chosen), what + ": the set is not a list");

  /*  The two halves of one rule. An anchor outside the set is an inspector
      pointing at a row the reader did not choose; a set with nothing anchored
      is a shift-click with nowhere to measure from. `assert.equal` is the
      strict one here, so an anchor of `""` - which reads as unpicked in every
      `if (selection.picked)` on the page while still being a string - fails
      rather than passing as "near enough to null". */
  if (selection.picked !== null)
    assert.ok(chosen.indexOf(selection.picked) >= 0,
              what + ": the anchor " + selection.picked + " is not in " + JSON.stringify(chosen));

  if (!chosen.length)
    assert.equal(selection.picked, null, what + ": nothing is chosen, so nothing may be anchored");

  assert.equal(new Set(chosen).size, chosen.length,
               what + ": " + JSON.stringify(chosen) + " holds the same cue twice");
}

test("an ordinary click picks one row and puts the set back to that one row", () => {
  /*  THE SET IS BUILT FIRST ON PURPOSE. On an empty selection this case would
      pass however the code was written, which is exactly how the rule gets lost:
      what it pins is that a plain click REPLACES what was chosen rather than
      adding to it, so the inspector that was showing three cues shows the one
      under the pointer. */
  pickThrough(["A", "B", "C"]);
  pickOne("B");

  assert.deepEqual(selection.chosen, ["B"]);
  assert.equal(selection.picked, "B");
  sane("after an ordinary click");
});

test("writing the anchor writes the set, and writing nothing picks nothing", () => {
  /*  `selection.picked = id` has been assignable from anywhere since before
      there was a set beside it - app.js clears it when a different show arrives
      - and an assignment that left `chosen` holding cues of the document being
      left would hand the inspector a batch out of a show nobody is looking at.
      So the setter re-derives the set, and this is where that is said. */
  pickThrough(["A", "B", "C"]);
  selection.picked = "Z";

  assert.deepEqual(selection.chosen, ["Z"]);
  sane("after the anchor was written directly");

  /*  AND ONE SPELLING OF NOTHING. An empty string would read as unpicked in the
      dozen places that test this value for truth while still being a string
      here, which is the kind of difference that turns up months later as a row
      that cannot be deselected. */
  selection.picked = "";

  assert.equal(selection.picked, null);
  assert.deepEqual(selection.chosen, []);
  sane("after an empty write");
});

test("a modifier click adds a row and makes it the one the hand is on", () => {
  const drawn = ["A", "B", "C", "D"];

  pickOne("A");
  pickAlso("C", drawn);

  assert.deepEqual(selection.chosen, ["A", "C"]);

  /*  THE ROW JUST TOUCHED IS THE ANCHOR, which is not decoration: the next
      shift-click measures from here, and that is how a set gets built as "these
      three, and then everything down to there". Leaving the anchor on the row
      before would range from a row the reader stopped pointing at two clicks
      ago. */
  assert.equal(selection.picked, "C");
  sane("after a modifier click");
});

test("a row joins the set where it is drawn, not in the order it was clicked", () => {
  const drawn = ["A", "B", "C", "D"];

  pickOne("D");
  pickAlso("B", drawn);

  /*  The order the rows are in is the order they are being LOOKED at in, and
      the only one a reader could predict - so a batch assembled bottom-up still
      reads top-down. Kept in click order instead, this would be ["D", "B"], and
      the inspector's list, the delete and anything else that walks the set
      would run backwards for no reason anybody could see. */
  assert.deepEqual(selection.chosen, ["B", "D"]);
  assert.equal(selection.picked, "B");
  sane("after adding a row drawn above the anchor");

  /*  A ROW THE PANE IS NOT SHOWING KEEPS THE END. A member chosen and then
      folded away inside a shut group has no place in the drawn order at all,
      and sorting by a position of -1 would put it in FRONT of every row that
      has one. */
  pickOne("A");
  pickAlso("Z", drawn);

  assert.deepEqual(selection.chosen, ["A", "Z"]);
  sane("after adding a row that is not on screen");

  /*  And a caller with no order to give - this file knows nothing of the page,
      so the order arrives with the ids or not at all - appends, which is the
      only order it could invent. */
  pickOne("D");
  pickAlso("B");

  assert.deepEqual(selection.chosen, ["D", "B"]);
  sane("after adding a row with no order given");
});

test("a modifier click on a chosen row takes it away, and the anchor lands next door", () => {
  const drawn = ["A", "B", "C"];

  /*  THE GESTURE IS ITS OWN UNDO. Somebody assembling eight cues out of thirty
      will put a wrong one in, and a modifier click that could only ever add
      would make them start the whole set again. */
  pickOne("A");
  pickAlso("C", drawn);
  pickAlso("B", drawn);

  assert.deepEqual(selection.chosen, ["A", "B", "C"]);
  assert.equal(selection.picked, "B");

  pickAlso("B", drawn);

  /*  AND THE ANCHOR CANNOT GO WITH IT. It lands on the row that took the
      removed one's place in the order - C, which is now where B was - so a
      shift-click straight afterwards measures from where the hand already was
      rather than from the far end of the set. `rest[0]` would be the far end;
      `null` would leave two cues chosen with nothing inspecting them. */
  assert.deepEqual(selection.chosen, ["A", "C"]);
  assert.equal(selection.picked, "C");
  sane("after letting go of the anchor");

  /*  Letting go of the LAST row is the one case that leaves nothing picked, and
      that is right: the set is empty, so there is nothing to anchor. */
  pickOne("A");
  pickAlso("A", drawn);

  assert.deepEqual(selection.chosen, []);
  assert.equal(selection.picked, null);
  sane("after letting go of the last row");
});

test("a click that names no row changes nothing", () => {
  /*  Not a gesture anybody makes on purpose: it is what a row whose identifier
      did not survive a half-built reply looks like by the time it reaches here.
      An empty id added to the set would be a row the inspector cannot find and
      the reader cannot click again to remove. */
  pickOne("A");
  pickAlso("", ["A"]);
  pickAlso(null, ["A"]);

  assert.deepEqual(selection.chosen, ["A"]);
  assert.equal(selection.picked, "A");
  sane("after a modifier click on nothing");
});

test("a range is exactly the rows it was given, and the anchor stays where the hand is", () => {
  pickOne("C");
  pickThrough(["A", "B", "C"]);

  assert.deepEqual(selection.chosen, ["A", "B", "C"]);

  /*  THE NEAR END DOES NOT MOVE. A range is measured from somewhere, and an
      operator who shift-clicks twice to correct the far end expects the near
      end to stay where they put it - so an anchor already inside the range is
      left alone rather than reset to the first row of it. */
  assert.equal(selection.picked, "C");
  sane("after a range that contains the anchor");

  /*  It moves only when the range has left it behind, which is what a range
      asked for with an anchor that has since gone out of the document looks
      like. The first row is then as good a place to stand as any; leaving it
      outside would be an inspector pointing at a cue the reader did not choose. */
  pickOne("Z");
  pickThrough(["A", "B"]);

  assert.deepEqual(selection.chosen, ["A", "B"]);
  assert.equal(selection.picked, "A");
  sane("after a range that does not contain the anchor");

  /*  One cue arriving twice - which is what a range over a pane showing it
      twice hands in - is one row of the set. */
  pickThrough(["A", "B", "A"]);

  assert.deepEqual(selection.chosen, ["A", "B"]);
  sane("after a range holding the same cue twice");

  /*  An empty range, and one that is not a list at all: a caller that handed in
      nothing would otherwise walk the characters of whatever it did hand in,
      since a string is iterable too. */
  pickThrough([]);

  assert.deepEqual(selection.chosen, []);
  assert.equal(selection.picked, null);
  sane("after an empty range");

  pickOne("A");
  pickThrough(undefined);

  assert.deepEqual(selection.chosen, []);
  sane("after a range of nothing at all");
});

test("deselecting empties both halves and leaves the reveal where it is", () => {
  selection.reveal = { key: "cue:A", until: 1, scrolled: false };

  pickThrough(["A", "B"]);
  pickNothing();

  assert.deepEqual(selection.chosen, []);
  assert.equal(selection.picked, null);
  sane("after deselecting");

  /*  WHERE THE PAGE WAS SENT IS NOT WHAT IS PICKED. The reveal is a fact about
      the last second - it says which row a reader was just taken to, and it
      expires on its own - so clearing it here would put out a highlight the
      reader is still reading, halfway through its fade, because they clicked
      the background. */
  assert.deepEqual(selection.reveal, { key: "cue:A", until: 1, scrolled: false });

  selection.reveal = null;
});


/*  THE RANGE, from here down, which is the one gesture in this file that cannot
    be asked of the model alone.

    `rangeTo` is gestures/clicks.js's, is not exported and is not meant to be:
    what it reads is the pane, and what it answers is a list of cues handed
    straight to `pickThrough`. So it is asked here the way a reader asks it - a
    click on a row with shift held - and read back off `selection`, which is
    where its answer lands. Nothing below reaches into that module; the handler
    it wires as it loads is the whole door, and the only thing this file adds to
    it is a pane to click on.

    WHY IT IS WORTH A PANE. A cue can be on screen TWICE: its own row, where it
    runs, and a derived line in the header of the group that gets it ready
    (views/didi.js). A range is taken by POSITION and only then reduced to cues,
    and taking it over de-duplicated identifiers instead - which is the obvious
    way to write it, and the way it was written - gives such a cue its EARLIER
    position and drops it out of any range that starts below that. Measured on
    the phase4 show an hour before these tests: six rows highlighted, and the
    seventh, the one cue in the scene with a mark on it, silently left out of
    the batch. The first case below is that one. */

/*  As much of an element as the click handler touches: what it says about
    itself, where it sits, and `closest`, which is how a click on a cell finds
    the row it is in. */
const camel = (name) => name.replace(/-([a-z])/g, (whole, letter) => letter.toUpperCase());

function matches(node, selector) {
  const child = selector.indexOf(">");

  if (child > 0)
    return matches(node, selector.slice(child + 1).trim()) &&
           !!node.parentNode && matches(node.parentNode, selector.slice(0, child).trim());

  const attribute = /^\[data-([a-z-]+)\]$/.exec(selector);

  if (attribute) return node.dataset[camel(attribute[1])] !== undefined;
  if (selector.startsWith("#")) return node.id === selector.slice(1);

  return node.nodeName === selector.toUpperCase();
}

function element(name, { id = "", data = {} } = {}) {
  const node = { nodeName: String(name).toUpperCase(), id: id, dataset: { ...data },
                 parentNode: null, children: [] };

  node.closest = (selector) => {
    for (let up = node; up; up = up.parentNode) if (matches(up, selector)) return up;

    return null;
  };

  return node;
}

/*  THE PANE AS THE RECONCILER LEAVES IT: one flat run of children in which a
    group's members sit under it and a band is furniture rather than a row. Only
    what the range reads is drawn on them - `data-pick`, and `data-reveal` on
    the line that is a second view of a cue - since what a row SAYS about being
    chosen is views/didi.js's and is tested there.

    EACH TEST CLICKS A CELL AND NOT THE ROW, because that is what a reader hits:
    the row is found with `closest`, and a click on the space between two words
    has to mean the cue a click on the name means.

    The reply is emptied with it. No list is then focused, so a plain click
    sends no aim and asks for no slider - which is what keeps these cases about
    the range and nothing else. */
function showing(rows) {
  const cells = {};

  tree.at = {};
  pane = element("div", { id: "cues" });

  for (const [name, data] of Object.entries(rows)) {
    const row = element("div", { data: data });
    const cell = element("div");

    cell.parentNode = row;
    row.children.push(cell);
    row.parentNode = pane;
    pane.children.push(row);
    cells[name] = cell;
  }

  return cells;
}

function click(cell, how) {
  clicked({ target: cell, detail: 1, metaKey: false, ctrlKey: false, shiftKey: false, ...how });
}

/*  ONE SCENE, DRAWN FRESH FOR EACH CASE. `D` is marked to be got ready by the
    group `G`, so it is on screen twice: as the derived line in G's header, near
    the top, and as its own row down among the members, where it actually runs.
    The two bands are the section heads - they pick nothing, and a range taken
    across them must not count them as rows. */
const scene = {
  group: { pick: "G" },
  headerBand: {},
  derived: { pick: "D", reveal: "cue:D" },
  memberBand: {},
  c1: { pick: "C1" },
  c2: { pick: "C2" },
  own: { pick: "D" },
  c3: { pick: "C3" },
};

test("a cue drawn twice is in a range when either of its lines falls inside it", () => {
  const at = showing(scene);

  click(at.c1);
  click(at.c3, { shiftKey: true });

  /*  THE MEASURED BUG, AND THE WHOLE REASON FOR THE PANE. D's own row sits
      between the two rows the reader clicked, so D is in the range - even
      though its OTHER line, the derived one in G's header, sits above where the
      range begins. A `rangeTo` that de-duplicated identifiers before measuring
      takes D's earlier position, finds it outside the span and leaves it out:
      six rows highlighted and five cues edited, with nothing to say which one
      got away. */
  assert.deepEqual(selection.chosen, ["C1", "C2", "D", "C3"]);
  assert.equal(selection.picked, "C1", "the near end stays where the plain click put it");
  sane("after a range taken downwards");

  /*  AND THE SAME BLOCK DRAGGED THE OTHER WAY, in the order the rows are drawn
      rather than the order they were clicked: a reader selecting upwards means
      the block a reader selecting downwards means. */
  click(at.c3);
  click(at.c1, { shiftKey: true });

  assert.deepEqual(selection.chosen, ["C1", "C2", "D", "C3"]);
  assert.equal(selection.picked, "C3");
  sane("after a range taken upwards");
});

test("a range is measured from the anchor's nearest line, not its first", () => {
  const at = showing(scene);

  /*  The anchor is D, which is the cue on screen twice - so there are two rows
      it could be measured from, and the two rows a reader is pointing at are
      the two they can SEE, not the pair that happens to be furthest apart. */
  click(at.derived);

  assert.equal(selection.picked, "D");

  click(at.c3, { shiftKey: true });

  /*  C3 is one row below D's OWN row and four below the derived line. Taking
      the first matching row instead would sweep C1 and C2 into a selection the
      reader made between two adjacent rows. */
  assert.deepEqual(selection.chosen, ["D", "C3"]);
  assert.equal(selection.picked, "D");
  sane("after ranging down from a cue drawn twice");

  click(at.derived);
  click(at.c1, { shiftKey: true });

  /*  And upwards from the same anchor the nearest line is the derived one,
      which is the row immediately above C1 - so taking the LAST matching row
      would give three cues for a click on the row next door. */
  assert.deepEqual(selection.chosen, ["D", "C1"]);
  sane("after ranging up from a cue drawn twice");
});

test("a range that covers both lines of a cue holds that cue once", () => {
  const at = showing(scene);

  click(at.group);
  click(at.c3, { shiftKey: true });

  /*  Six rows and two bands, five cues. An inspector shown D twice would offer
      its own field against itself, and a batch edit would write it twice - the
      second write being of the value the first one had just replaced. It is
      guarded in two places, `rangeTo` and `pickThrough`, which is why the rule
      is asserted where a reader would meet it rather than at either of them. */
  assert.deepEqual(selection.chosen, ["G", "D", "C1", "C2", "C3"]);
  assert.equal(selection.picked, "G");
  sane("after a range over a whole group");
});

test("a range with nothing to measure from is the row that was clicked", () => {
  const at = showing(scene);

  pickNothing();
  click(at.c2, { shiftKey: true });

  /*  The first shift-click of a session, which is not worth a refusal: the
      reader gets the row they clicked with the anchor on it, and their next
      shift-click means something. Without that answer the span would be taken
      from an anchor at -1, which slices from the END of the pane and hands back
      nothing at all - a click that visibly cleared the selection. */
  assert.deepEqual(selection.chosen, ["C2"]);
  assert.equal(selection.picked, "C2");
  sane("after the first shift-click of a session");

  /*  AND AN ANCHOR THAT IS NO LONGER ON SCREEN, which is a delete between the
      two clicks, or a fold that shut the row away. */
  selection.picked = "GONE";
  click(at.c2, { shiftKey: true });

  assert.deepEqual(selection.chosen, ["C2"]);
  assert.equal(selection.picked, "C2");
  sane("after a range measured from a row that has gone");
});

test("a picking row that is not in the cue pane is picked on its own", () => {
  const at = showing(scene);

  click(at.c1);

  /*  THE CUE PANE IS NOT THE ONLY THING ON THE PAGE THAT PICKS. The inspector
      draws a trigger as a row with `data-pick` on it, and the way back from a
      trigger to the cue it belongs to as another (views/inspector.js). Neither
      is in the run of rows a range is measured over, so there is no span
      between one of them and the anchor - and measuring to an index of -1
      would slice from the END of the pane instead, handing back the last row,
      several rows nobody clicked, or nothing at all. */
  const loose = element("div", { data: { pick: "T1" } });
  const inside = element("div");

  inside.parentNode = loose;
  loose.children.push(inside);

  click(inside, { shiftKey: true });

  assert.deepEqual(selection.chosen, ["T1"]);
  assert.equal(selection.picked, "T1");
  sane("after a shift-click on a row the pane is not showing");
});

test("a modifier click assembles the set in drawn order and sends nobody anywhere", () => {
  const at = showing(scene);

  selection.reveal = null;

  click(at.c3);
  click(at.c1, { ctrlKey: true });

  /*  THE ORDER COMES OFF THE PANE. The handler reads the drawn order and hands
      it to the model; a handler that added the row without it would leave the
      set in the order it was clicked, which here is C3 then C1 - the set
      reading bottom-up while the pane reads top-down. */
  assert.deepEqual(selection.chosen, ["C1", "C3"]);
  assert.equal(selection.picked, "C1");
  sane("after a modifier click");

  /*  ⌘ AND CTRL ARE ONE GESTURE, because this page is operated from both kinds
      of machine, often on the same show, and nobody should have to remember
      which desk they are sitting at. */
  click(at.c2, { metaKey: true });

  assert.deepEqual(selection.chosen, ["C1", "C2", "C3"]);
  sane("after a modifier click from the other kind of desk");

  click(at.c2, { metaKey: true });

  assert.deepEqual(selection.chosen, ["C1", "C3"]);
  sane("after the same click again");

  /*  AND NOBODY IS SENT ANYWHERE WHILE A SET IS BEING BUILT. The derived line
      says where the member's own row is, and following that unfolds sections
      and scrolls the pane - under the hand of somebody halfway down a block.
      Going to the other view of a cue is a gesture in its own right, and it is
      a plain click, which is the second half of this. */
  click(at.derived, { ctrlKey: true });

  assert.equal(selection.reveal, null);
  assert.deepEqual(selection.chosen, ["D", "C1", "C3"]);
  sane("after a modifier click on a line that can reveal");

  click(at.derived);

  assert.equal(selection.reveal.key, "cue:D");
  assert.deepEqual(selection.chosen, ["D"]);
  sane("after a plain click on the same line");

  selection.reveal = null;
});
