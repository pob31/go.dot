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

/*  WHERE A DROP LANDS, and the drops the page must not offer at all.

    A drag is the one gesture on this page whose correctness is arithmetic
    rather than markup. Everything else a click does is "send this identifier";
    a drop has to compute a NUMBER, and that number is read by an engine which
    counts differently from the eye. `object.move` takes a MEMBER POSITION - a
    place in the sequence `/godot/cue/<id>/order` publishes - and it applies it
    by taking the moved cue OUT of the list first:

        the result equals: take the member list, remove the moved cue if it is
        in it, then insert it at min(index, length).

    Both limbs of ShowDocument::move come to that one sentence, which its own
    comment works through at length, and tests/SequenceTests.cpp pins it by
    exhaustive simulation. `engineMove` below is that sentence written out in
    JavaScript, and every case in the first half of this file is checked by
    APPLYING it rather than against an index somebody worked out by hand -
    because the index worked out by hand is exactly what goes wrong.

    THE BUG THIS FILE EXISTS TO CATCH is one line of arithmetic and it is
    invisible on half the gestures that meet it. Members [A,B,C,D], A dragged to
    just after C: count against the list as drawn and C sits at 2, so "after C"
    reads as 3 - and 3 is where A lands once A has been taken out, which is past
    D. The answer is 2. It is wrong ONLY when dragging DOWNWARD within one
    parent, which is the half of the gesture a hurried hand test does not reach:
    drag something up, and the list with and without the dragged cue agree about
    where the target is.

    AND THEN THE REFUSALS, which are not arithmetic but are the other half of
    being honest. A <Header>, a <Footer> and a <Persistent> container have
    identifiers in the file and NO ADDRESS in the tree, so no client can learn
    one; `object.move` would take one happily and there is no way to name it. A
    drop into one of those sections is therefore not expressible, and a gesture
    that cannot be expressed has to be refused where the hand is, IN WORDS,
    rather than sent and silently lost. Those decisions live inside
    `landingFor`, which gestures/drag.js does not export - so they are asked for
    the way a reader asks them, by playing a drag over the rows and reading what
    the page put on the screen.

    THE SECOND HALF OF THIS FILE IS THEREFORE A PANE, and the least of one that
    a drag can be made on. It is a different fake from ./fake-dom.mjs's and
    deliberately so: that one is the DOM as views/reconcile.js uses it, and a
    drag asks a row two things a reconciler never asks - how tall it is, and
    where in that height the pointer is. */

import { test, beforeEach, afterEach } from "node:test";
import assert from "node:assert/strict";

import { installDocument } from "./fake-dom.mjs";

/*  THE GLOBALS THE MODULE EXPECTS AT LOAD, stood up before the import, as every
    other file here does. `location` says `file:`, so plumbing/link.js decides at
    load that it is not being served by an engine at all: no socket is opened,
    and the `object.move` a drop sends goes into a `send` that has no socket and
    answers false. Nothing below fetches and nothing connects.

    `window` is for one line of gestures/drag.js: the pale mark on the row being
    carried is deferred to a timeout of nought, so that it lands after the
    browser has taken its drag image rather than fading the picture as well.
    What that defers is the look of the row and no decision, so the callbacks are
    collected and not run - a test that ran them would be asserting the
    stylesheet's business through this file. */
globalThis.location = { protocol: "file:", host: "" };
globalThis.window = { setTimeout: () => 0 };

const page = installDocument();

/*  WHATEVER THE MODULE WIRES AS IT LOADS, kept by kind so a gesture can be
    played into it. A handler that was never wired throws rather than doing
    nothing: a module that quietly wired none would leave every case below
    asserting that a drag changed nothing, which is what a passing test would
    then say for the emptiest of reasons. */
const wired = {};

page.addEventListener = (kind, handler) => { wired[kind] = handler; };

function fire(kind, event) {
  if (!wired[kind]) throw new Error("gestures/drag.js wired no " + kind + " handler");

  wired[kind](event);
  return event;
}

/*  THE LEAST ELEMENT A DRAG CAN BE MADE ON, and every part of it is something
    gestures/drag.js actually asks for: `closest` for the three selectors it
    uses, a `dataset` for what views/didi.js wrote on the row, a class list for
    the outline a refusal draws, a `--rail` for the indent the mark takes, a box
    so the module can work out which third of the row the pointer is in, and the
    five ways the pane's children change while the mark walks up and down it.

    `page.createElement` is replaced rather than taken from ./fake-dom.mjs
    because the mark the module makes is written to with `textContent`, which
    that element exposes as a getter and no setter - assigning to it from a
    module, which is strict code, throws. A fake that makes the page fall over
    is a fake that lies about the page. */
function matches(node, selector) {
  if (selector.startsWith("#")) return node.id === selector.slice(1);

  const attribute = /^\[data-([a-z]+)\]$/.exec(selector);

  return attribute ? node.dataset[attribute[1]] !== undefined : false;
}

function element(name, { id = "", key = "", data = {}, rail = "", top = 0, height = 10 } = {}) {
  const node = {
    nodeName: String(name).toUpperCase(),
    id: id,
    className: "",
    textContent: "",
    dataset: { ...data },
    children: [],
    parentNode: null,
    classes: new Set(),
    properties: new Map(),
  };

  if (key) node.dataset.key = key;
  if (rail) node.properties.set("--rail", rail);

  node.classList = {
    add: (one) => node.classes.add(one),
    remove: (one) => node.classes.delete(one),
  };

  node.style = {
    getPropertyValue: (property) => node.properties.get(property) || "",
    setProperty: (property, value) => node.properties.set(property, value),
    removeProperty: (property) => node.properties.delete(property),
  };

  /*  `bottom` as well as `top`, because a real rect carries it and the gesture
      asks for it: the pane's background means "the end of the list" only BELOW
      the last row, and that is the comparison it makes. A fixture that answered
      `undefined` there would let a case pass that a browser fails. */
  node.getBoundingClientRect = () => ({ top: top, height: height, bottom: top + height });

  node.closest = (selector) => {
    for (let up = node; up; up = up.parentNode) if (matches(up, selector)) return up;

    return null;
  };

  node.insertBefore = (child, reference) => {
    if (child.parentNode) child.parentNode.removeChild(child);

    const at = reference ? node.children.indexOf(reference) : -1;

    if (at < 0) node.children.push(child);
    else node.children.splice(at, 0, child);

    child.parentNode = node;
    return child;
  };

  node.appendChild = (child) => node.insertBefore(child, null);

  node.removeChild = (child) => {
    const at = node.children.indexOf(child);

    if (at >= 0) node.children.splice(at, 1);

    child.parentNode = null;
    return child;
  };

  /*  A getter and not a field, because the mark is inserted between two rows
      and taken out again on every pointer move: an answer computed when the row
      was built would be the pane as it stood before the drag started. */
  Object.defineProperty(node, "nextElementSibling", {
    get() {
      if (!node.parentNode) return null;

      const among = node.parentNode.children;

      return among[among.indexOf(node) + 1] || null;
    },
  });

  /*  And the same for the pane's last child, for the same reason: it moves as
      the mark is put in and taken out, and the gesture reads it to find where
      the rows stop. */
  Object.defineProperty(node, "lastElementChild", {
    get() { return node.children[node.children.length - 1] || null; },
  });

  return node;
}

let pane = null;

page.createElement = (name) => element(name);
page.getElementById = (id) => (id === "cues" ? pane : null);

const { tree } = await import("../../clients/console/plumbing/tree.js");
const { selection } = await import("../../clients/console/model/selection.js");
const { view } = await import("../../clients/console/views/view.js");

const { dropIndex, sideFor, chainAbove, subtreeOf, carriedMarks, brokenBy } =
  await import("../../clients/console/gestures/drag.js");


/*  THE ARITHMETIC, from here to the pane.

    THE ENGINE'S OWN RULE, written out here rather than asked of anything,
    because the engine is C++ and this test has no engine. What the page
    promises is that the number it sends, put through THAT rule, produces the
    order the reader watched themselves ask for - so the rule has to be stated
    on this side of the wire for there to be anything to check against. It is
    the whole of ShowDocument::move as a client can observe it: the
    within-parent limb is a `moveChild` and the cross-parent limb is a remove
    and an `addChild`, and the comment there works through why the two land in
    the same place.

    A NEGATIVE INDEX IS NOT CLAMPED BY THE ENGINE, it is refused as
    `badAddress` - "because it usually means the caller computed it wrong",
    which is precisely the failure this file is about. So this throws where the
    engine would refuse, rather than quietly reading -1 as 0 and letting a wrong
    answer look plausible. */
function engineMove(members, dragged, index) {
  if (!Number.isInteger(index) || index < 0)
    throw new Error("the engine refuses index " + index + " as badAddress");

  const rest = members.filter((id) => id !== dragged);

  rest.splice(Math.min(index, rest.length), 0, dragged);
  return rest;
}

test("the engine's rule is the rule this file checks against", () => {
  /*  A pin on the fixture itself, because every case below is only as good as
      this function: a mistake here would let a wrong `dropIndex` through by
      agreeing with it. Three claims, one for each half-sentence of the rule -
      the removal, the insertion, the clamp - each on a list short enough to
      read at a glance. */
  assert.deepEqual(engineMove(["A", "B", "C"], "A", 2), ["B", "C", "A"]);
  assert.deepEqual(engineMove(["A", "B", "C"], "X", 1), ["A", "X", "B", "C"]);
  assert.deepEqual(engineMove(["A", "B", "C"], "A", 99), ["B", "C", "A"]);
  assert.throws(() => engineMove(["A", "B", "C"], "A", -1), /badAddress/);
});

test("a cue dragged downward is counted in the list it has already left", () => {
  /*  THE CASE THE WHOLE FILE IS FOR. [A,B,C,D] and A dropped just after C: C is
      the third row on screen, so the index that looks right is 3, and 3 is what
      any arithmetic done against the drawn list gives. */
  const members = ["A", "B", "C", "D"];

  assert.equal(dropIndex(members, "A", "C", "after"), 2);

  /*  And here is what the two numbers actually do, which is the only argument
      that matters: the reader dropped A between C and D, and 3 puts it past D.
      One row out, in the one direction nobody re-checks. */
  assert.deepEqual(engineMove(members, "A", 2), ["B", "C", "A", "D"]);
  assert.deepEqual(engineMove(members, "A", 3), ["B", "C", "D", "A"]);

  /*  ITS MIRROR, which is the same mistake said the other way round: D dropped
      just before B. B stands at 1 in the drawn list and at 1 without D as well,
      because D is below it - so this one comes out right however it is
      computed, and a page that is only ever dragged upwards passes. */
  assert.equal(dropIndex(members, "D", "B", "before"), 1);
  assert.deepEqual(engineMove(members, "D", 1), ["A", "D", "B", "C"]);
});

test("a drag within one parent reaches both ends and both directions", () => {
  const members = ["A", "B", "C", "D"];

  const landed = (dragged, target, side) =>
    engineMove(members, dragged, dropIndex(members, dragged, target, side));

  //  Upward, to the very top, and downward, to the very bottom.
  assert.deepEqual(landed("C", "A", "before"), ["C", "A", "B", "D"]);
  assert.deepEqual(landed("A", "D", "after"), ["B", "C", "D", "A"]);

  //  One step each way, which is what the inspector's ▲ and ▼ do.
  assert.deepEqual(landed("B", "C", "after"), ["A", "C", "B", "D"]);
  assert.deepEqual(landed("C", "B", "before"), ["A", "C", "B", "D"]);

  /*  THE TWO SIDES OF ONE GAP ARE ONE PLACE, which the pair above says twice
      over: dropping C before B and dropping B after C are the same instruction
      given from either edge of the same line, and a reader who aimed a pixel
      high must not get a different show from one who aimed a pixel low. The
      indices differ, because the gap is named from either side, and the result
      does not. */
  assert.notEqual(dropIndex(members, "C", "B", "before"), dropIndex(members, "B", "C", "after"));

  /*  A DROP THAT CHANGES NOTHING IS STILL A LEGAL DROP: A is already just
      before B, and asking for that again has to leave the show alone rather
      than refuse or shuffle. */
  assert.deepEqual(landed("A", "B", "before"), ["A", "B", "C", "D"]);
  assert.deepEqual(landed("D", "C", "after"), ["A", "B", "C", "D"]);
});

test("a cue dropped on itself is a gesture that ends where it began", () => {
  /*  Picked up and put down on its own row, which is what happens when a hand
      slips or a reader changes their mind mid-drag.

      IT IS THE SAME -1 AS THE MISSING TARGET BELOW AND IT NEEDS THE OTHER
      ANSWER, which is why the two are separate cases rather than one. Take the
      dragged cue out of the list, as the rule says to, and the cue it was
      dropped on is no longer in it either - they are the same cue - so an
      `indexOf` hands back -1 exactly as it does for a target that has been
      deleted. Answer that with "an end", which is right for the deleted target,
      and a reader whose hand slipped watches the cue they were holding jump to
      the top of its group. Written against a reference `dropIndex` that gave
      both the same answer, this is the case that failed.

      EVERY MEMBER AND BOTH SIDES, because the wrong answer is not wrong
      everywhere: a cue already standing at the end of its list would land back
      at the end, and a case that happened to pick that one would pass. */
  const members = ["A", "B", "C", "D"];

  for (const one of members) {
    for (const side of ["before", "after"]) {
      const at = dropIndex(members, one, one, side);

      assert.deepEqual(engineMove(members, one, at), members,
                       one + " dropped on itself, " + side);
    }
  }
});

test("a cue arriving from another parent is counted in the list as it stands", () => {
  /*  THE OTHER HALF OF THE RULE, and the one where the arithmetic that looks
      right IS right: the dragged cue is not in this parent's members, so
      removing it removes nothing and the drawn positions are the positions. A
      page that subtracted one "because a drag always does" would be wrong here,
      on every drop into a different group. */
  const members = ["A", "B", "C"];

  assert.equal(dropIndex(members, "X", "A", "before"), 0);
  assert.equal(dropIndex(members, "X", "B", "before"), 1);
  assert.equal(dropIndex(members, "X", "B", "after"), 2);
  assert.equal(dropIndex(members, "X", "C", "after"), 3);

  assert.deepEqual(engineMove(members, "X", 0), ["X", "A", "B", "C"]);
  assert.deepEqual(engineMove(members, "X", 3), ["A", "B", "C", "X"]);
});

test("a list with one member, and a list with none", () => {
  /*  A SINGLE MEMBER IS THE LIST WHERE BOTH SIDES MEAN THE SAME THING if the
      cue being dragged is that member: there is nowhere else for it to go. */
  for (const side of ["before", "after"]) {
    assert.deepEqual(engineMove(["A"], "A", dropIndex(["A"], "A", "A", side)), ["A"]);
  }

  //  And the same list, with a cue arriving into it from somewhere else.
  assert.equal(dropIndex(["A"], "X", "A", "before"), 0);
  assert.equal(dropIndex(["A"], "X", "A", "after"), 1);

  /*  AN EMPTY GROUP HAS NO ROW TO AIM AT, so there is no target - and the only
      position there is, is nought, whichever side the pointer thinks it is on.
      It is worth pinning because it is the one drop whose index is not computed
      from anything, and because a group opened for the first time is where a
      designer writing a list drags their next cue. */
  assert.equal(dropIndex([], "X", "", "before"), 0);
  assert.equal(dropIndex([], "X", "", "after"), 0);
  assert.deepEqual(engineMove([], "X", 0), ["X"]);
});

test("a target the list does not hold answers with an end, never with a negative", () => {
  /*  A poll that landed between the drag starting and the drop, a cue deleted
      by another operator, or a row whose parent changed under the pointer: the
      target names a cue this member list does not have. `dropIndex` is total
      and says so - nought for "before", the length for "after" - and the reason
      that matters is the one it does NOT do: an `indexOf` handed straight back
      would send -1, which the engine refuses as `badAddress`, so the reader
      would get a red banner about an address for a gesture they made with their
      hand.

      It is also the arm the pane's own background uses. Below the last row
      there is no target at all, and a hand that has carried a cue down there
      means the bottom of the list. */
  const members = ["A", "B", "C"];

  assert.equal(dropIndex(members, "X", "GONE", "before"), 0);
  assert.equal(dropIndex(members, "X", "GONE", "after"), 3);

  assert.deepEqual(engineMove(members, "X", 0), ["X", "A", "B", "C"]);
  assert.deepEqual(engineMove(members, "X", 3), ["A", "B", "C", "X"]);

  /*  And with the missing target on a list the dragged cue IS in, where the
      length to answer with is the length without it. */
  assert.equal(dropIndex(["A", "B", "C"], "B", "GONE", "after"), 2);
  assert.deepEqual(engineMove(["A", "B", "C"], "B", 2), ["A", "C", "B"]);
});

test("every drop lands the cue where the drop visibly promised", () => {
  /*  THE CLAIM WORTH MAKING ONCE RATHER THAN TABULATING, and the only one that
      would catch an arithmetic which is right about the cases somebody thought
      of. For every pair of members and both sides of each: put the answer
      through the engine's own rule, and ask that the dragged cue ends up
      immediately before or immediately after the cue it was dropped on - which
      is what the insertion line the reader is looking at means, and the whole
      of what they were promised.

      FIVE MEMBERS, because four is not enough: with four, "one past the target"
      and "the end of the list" coincide often enough for a wrong clamp to hide
      behind them. */
  const members = ["A", "B", "C", "D", "E"];

  for (const dragged of members) {
    for (const target of members) {
      if (dragged === target) continue;

      for (const side of ["before", "after"]) {
        const where = dragged + " " + side + " " + target;
        const at = dropIndex(members, dragged, target, side);

        assert.ok(Number.isInteger(at) && at >= 0, where + ": " + at + " is not a position");

        const after = engineMove(members, dragged, at);

        //  Nothing gained, nothing lost, and no cue in the list twice.
        assert.deepEqual([...after].sort(), [...members].sort(), where + ": the same cues");

        const landed = after.indexOf(dragged);
        const beside = after.indexOf(target);

        assert.equal(landed, side === "before" ? beside - 1 : beside + 1, where);
      }
    }
  }
});

test("every arrival from another parent lands where the drop promised too", () => {
  /*  The same claim for the limb where the dragged cue is not in the list,
      which is a different line of the engine's rule and so a different way to
      be wrong: a page that subtracted for the removal unconditionally passes
      every case above and puts every cross-group drop one row too high. */
  const members = ["A", "B", "C", "D", "E"];

  for (const target of members) {
    for (const side of ["before", "after"]) {
      const where = "X " + side + " " + target;
      const at = dropIndex(members, "X", target, side);

      assert.ok(Number.isInteger(at) && at >= 0, where + ": " + at + " is not a position");

      const after = engineMove(members, "X", at);

      assert.equal(after.length, members.length + 1, where + ": the cue arrived");
      assert.deepEqual(after.filter((id) => id !== "X"), members, where + ": the rest stayed");

      const landed = after.indexOf("X");
      const beside = after.indexOf(target);

      assert.equal(landed, side === "before" ? beside - 1 : beside + 1, where);
    }
  }
});

test("a row has two answers and a group has three", () => {
  /*  WHICH OF THE ANSWERS A ROW'S HEIGHT HOLDS, which is the other half of
      where a drop lands: the same pointer over the same row means "before it",
      "after it" or, on a group, "inside it as the first member". A fraction and
      not a pixel count, so the rule does not change with the type knob.

      THE BOUNDARIES ARE THE WHOLE OF IT. Exactly a third and exactly two
      thirds have to fall somewhere definite, and the middle band has to be a
      third rather than a sliver: these are nine pixels each at the type this
      page is drawn at, and a band that lost its share to a `<=` would be the
      one a hand cannot hit. */
  assert.equal(sideFor(0, false), "before");
  assert.equal(sideFor(0.49, false), "before");
  assert.equal(sideFor(0.5, false), "after");
  assert.equal(sideFor(1, false), "after");

  assert.equal(sideFor(0, true), "before");
  assert.equal(sideFor(1 / 3 - 0.01, true), "before");
  assert.equal(sideFor(1 / 3, true), "into");
  assert.equal(sideFor(0.5, true), "into");
  assert.equal(sideFor(2 / 3, true), "into");
  assert.equal(sideFor(2 / 3 + 0.01, true), "after");
  assert.equal(sideFor(1, true), "after");

  /*  A GROUP'S MIDDLE BAND IS A REAL THIRD and not a rounding: a rule written
      with the two comparisons the wrong way round leaves it empty, and "drop
      inside a group" would then be a gesture nobody could perform. */
  let into = 0;

  for (let n = 0; n <= 100; n += 1) if (sideFor(n / 100, true) === "into") into += 1;

  assert.ok(into > 30 && into < 40, "the middle third is a third: " + into + " of 101");
});


/*  THE TREE EVERY CASE BELOW IS ASKED OF, and one show for all of them.

    L is a list holding a group G, a plain cue C1 and a cue Z. G has a header
    with a written cue H in it, a footer with F, members M, X and D, and X is a
    group of its own holding D2 and D3.

    THE PRESET MARKS ARE THE POINT OF THE SHAPE. D sits inside G and is marked to
    be got ready by G's header, so G publishes it in `headerDerived` and the mark
    is LIVE. D2 AND D3 sit inside X, which sits inside G, and are marked for G
    too - live as well, and at a depth, which is what makes dragging X out of G a
    gesture that breaks marks on cues nobody touched. There are TWO of them
    rather than one because the label says one mark and several in different
    words, and the sentence that counts them is a sentence that can be wrong. Z
    is marked for G and is NOT inside it, so G does not publish it: a mark that
    is already inert, which the engine ignores, views/didi.js draws as a plain
    word, and a move cannot break because it is not applying now.

    P is the list's persistent cue and H and F are its sections' cues: three
    roles, three refusals, and none of them a member of anything a client can
    name. */
const leaf = (path, value) => ({ FULL_PATH: path, TYPE: "s", ACCESS: 3, VALUE: [value] });

function serve(nodes) {
  tree.at = {};
  for (const node of nodes) tree.at[node.FULL_PATH] = node;
}

function cue(id, { parent, role = "member", kind = "memo", preset = "", order = "",
                   headerOrder = "", footerOrder = "", headerDerived = "" }) {
  const nodes = [leaf("/godot/cue/" + id + "/kind", kind),
                 leaf("/godot/cue/" + id + "/parent", parent),
                 leaf("/godot/cue/" + id + "/role", role),
                 leaf("/godot/cue/" + id + "/name", id)];

  for (const [name, value] of [["preset", preset], ["order", order],
                               ["headerOrder", headerOrder], ["footerOrder", footerOrder],
                               ["headerDerived", headerDerived]]) {
    if (value !== "") nodes.push(leaf("/godot/cue/" + id + "/" + name, value));
  }

  return nodes;
}

const show = [
  leaf("/godot/list/L/order", "G C1 Z"),
  leaf("/godot/list/L/name", "Act One"),

  ...cue("G", { parent: "L", kind: "group", order: "M X D",
                headerOrder: "H", footerOrder: "F", headerDerived: "D D2 D3" }),
  ...cue("H", { parent: "G", role: "header" }),
  ...cue("F", { parent: "G", role: "footer" }),
  ...cue("M", { parent: "G" }),
  ...cue("X", { parent: "G", kind: "group", order: "D2 D3" }),
  ...cue("D2", { parent: "X", preset: "G" }),
  ...cue("D3", { parent: "X", preset: "G" }),
  ...cue("D", { parent: "G", preset: "G" }),
  ...cue("C1", { parent: "L" }),
  ...cue("Z", { parent: "L", preset: "G" }),
  ...cue("P", { parent: "L", role: "persistent", kind: "media" }),
];

/*  AND THE PANE THAT DRAWS IT, as one flat run of children in the order
    views/didi.js writes them: a group's own row, then its header band, the
    derived lines and the written cue inside it, the band's end, then the
    members - one of which is a group with a member of its own - then the footer
    and, at the foot of the list, the persistent band.

    THE KEYS ARE THE RECONCILER'S, because they are what the module walks: a
    cue's row is `cue:<id>`, a derived line is `preset:<group>:<id>`, and a
    section's edges are `band:<owner>:<word>` and that with `:end`. How far
    "after this group" reaches is read off those and nothing else, so a scene
    that left them out would be testing a walk with nothing to walk.

    EVERY ROW IS TEN TALL AND SITS TEN BELOW THE ONE ABOVE IT, so a pointer at
    the row's top plus one is in its first tenth and at plus nine is in its last:
    that is all `fractionIn` asks of a box, and round numbers make the cases
    below readable.

    AND THE THIRD FIELD IS THE ROW'S RAIL, which views/didi.js gives to the rows
    and bands drawn inside a section and to nothing else. Nothing below asserts
    it - where the mark is indented is the stylesheet's business - but the module
    reads it off every row it lands on, and a scene in which no row had one would
    be testing that path with the only value it can never meet. */
const scene = [
  ["cue:G", { pick: "G" }],
  ["band:G:header", { band: "header" }, "header-rail"],
  ["preset:G:D", { pick: "D", derived: "yes" }, "header-rail"],
  ["preset:G:D2", { pick: "D2", derived: "yes" }, "header-rail"],
  ["preset:G:D3", { pick: "D3", derived: "yes" }, "header-rail"],
  ["cue:H", { pick: "H" }, "header-rail"],
  ["band:G:header:end", { band: "header" }, "header-rail"],
  ["cue:M", { pick: "M" }],
  ["cue:X", { pick: "X" }],
  ["cue:D2", { pick: "D2" }],
  ["cue:D3", { pick: "D3" }],
  ["cue:D", { pick: "D" }],
  ["band:G:footer", { band: "footer" }, "footer-rail"],
  ["cue:F", { pick: "F" }, "footer-rail"],
  ["band:G:footer:end", { band: "footer" }, "footer-rail"],
  ["cue:C1", { pick: "C1" }],
  ["cue:Z", { pick: "Z" }],
  ["band:list:L:persistent", { band: "persistent" }, "band-rail"],
  ["cue:P", { pick: "P" }, "band-rail"],
  ["band:list:L:persistent:end", { band: "persistent" }, "band-rail"],
];

const rows = new Map();
const tops = new Map();

function draw() {
  pane = element("div", { id: "cues", data: { list: "L" } });
  rows.clear();
  tops.clear();

  scene.forEach(([key, data, rail], n) => {
    const row = element("div", { key: key, data: data, rail: rail || "", top: n * 10 });

    pane.appendChild(row);
    rows.set(key, row);
    tops.set(key, n * 10);
  });
}

const rowFor = (key) => rows.get(key);

/*  The marks a drag of `id` would carry, said as sorted text: the walk that
    finds them builds a set, and the order a set comes out in is not a promise
    anybody made. */
const marksOf = (id) =>
  carriedMarks(subtreeOf(id)).map((one) => one.cue + " by " + one.group).sort();

/*  A pointer in the first tenth of a row, in the middle of it, and in the last
    tenth: before, the group's "into" band, and after. */
const near = (key, where) => tops.get(key) + ({ top: 1, middle: 5, bottom: 9 })[where];

function pickUp(key) {
  const row = rowFor(key);

  fire("dragstart", { target: row, dataTransfer: { effectAllowed: "", setData: () => {} } });
  return row;
}

function over(key, where) {
  const event = { target: rowFor(key), clientY: near(key, where), prevented: false,
                  dataTransfer: { dropEffect: "" },
                  preventDefault() { event.prevented = true; } };

  return fire("dragover", event);
}

/*  WHAT THE READER IS BEING TOLD, read back off the pane the module wrote it
    into: whether the mark is drawn as a refusal, the words on it, and which row
    it is standing in front of.

    THE PLACE IS SAID AS THE ROW BELOW IT and not as an index, because the mark
    is a child of the pane like any other and inserting it moves every row under
    it along by one - so an index compared with where a row stood before the
    drag would be off by one exactly when the mark is above that row, which is
    every case worth asking about. The row below is what an insertion line MEANS
    and it is what the reader sees; `null` is the foot of the list.

    The words are asserted everywhere a refusal is, because §4.8 is explicit
    that the red is not the carrier: somebody has to be able to read why their
    drop will not happen. */
function shown() {
  const line = pane.children.find((child) => String(child.className).startsWith("drop-line"));

  if (!line) return null;

  const below = line.nextElementSibling;

  return { refused: String(line.className).includes("drop-no"),
           words: line.children.map((one) => one.textContent).join(""),
           before: below ? below.dataset.key : null };
}

beforeEach(() => {
  serve(show);
  draw();

  /*  NOTHING CHOSEN, so the only words a warning can put on the mark are the
      preset one. The label also carries "moves this one only, not the other
      two" when a set is picked up, which is a different rule and a different
      file's business. */
  selection.picked = null;
});

/*  EVERY CASE LETS GO, whatever it asserted. A drag left in the air holds
    `view.holding`, which stops the cue pane redrawing - the one failure this
    gesture must not be able to cause - and would carry into the next case as a
    module-level `drag.id` that is not empty. */
afterEach(() => {
  fire("dragend", {});
  assert.equal(view.holding, false, "the pane is released whatever happened");
});


test("everything over a cue is the groups it is in and then the list", () => {
  /*  The walk a drop's preset guard and a mark's indent are both measured
      from. The LIST IS INCLUDED and is the top: it is a perfectly good
      destination for a drop, and it is told from a group by publishing no
      `kind` of its own. */
  assert.deepEqual(chainAbove("D2"), ["X", "G", "L"]);
  assert.deepEqual(chainAbove("D"), ["G", "L"]);
  assert.deepEqual(chainAbove("C1"), ["L"]);

  //  A cue in a section is in the GROUP, which is the fact a drop needs: the
  //  section it is drawn in has no address and is not in this chain at all.
  assert.deepEqual(chainAbove("H"), ["G", "L"]);
  assert.deepEqual(chainAbove("P"), ["L"]);

  //  A cue nobody published, and the list itself, walk nowhere.
  assert.deepEqual(chainAbove("GHOST"), []);
  assert.deepEqual(chainAbove("L"), []);

  /*  AND IT STOPS RATHER THAN GOING ROUND FOR EVER. A `parent` that leads back
      to where it started cannot happen in a document the engine wrote, which is
      exactly why it is worth a bound: this walks a tree read off the wire, and
      a browser tab spinning during a show is not a failure anybody recovers
      from. Reaching the assertion IS the test. */
  serve([...cue("A", { parent: "B", kind: "group" }), ...cue("B", { parent: "A", kind: "group" })]);
  assert.ok(chainAbove("A").length <= 64, "a parent that loops still answers");
});

test("a group carries everything inside it, through its sections and down", () => {
  /*  What "a group cannot be dropped inside itself" is asked of, and it is
      asked of the whole subtree rather than of the row: the engine refuses a
      drop into a descendant too, because without the refusal the tree stops
      being a tree and the identifiers in the lost branch stay reserved for
      ever.

      THE SECTIONS TRAVEL WITH IT, which `order` alone does not say: H and F are
      in G's header and footer, they move when G moves, and a drop onto one of
      them is a drop inside the thing in the hand. */
  const held = subtreeOf("G");

  assert.deepEqual([...held].sort(), ["D", "D2", "D3", "F", "G", "H", "M", "X"]);

  //  And the group one level down carries its own members and nothing above it.
  assert.deepEqual([...subtreeOf("X")].sort(), ["D2", "D3", "X"]);

  //  A plain cue carries itself, which is what makes the "into itself" test
  //  one test rather than two.
  assert.deepEqual([...subtreeOf("C1")], ["C1"]);

  /*  AND A TREE THAT NAMES A CUE AS ITS OWN DESCENDANT STILL FINISHES, which
      the walk upward can only answer slowly and this one could not answer at
      all. */
  serve([...cue("A", { parent: "L", kind: "group", order: "B" }),
         ...cue("B", { parent: "A", kind: "group", order: "A" })]);

  assert.deepEqual([...subtreeOf("A")].sort(), ["A", "B"]);
});

test("the marks a drag carries are the live ones whose group is not going too", () => {
  /*  THE GUARD THE AUTHOR ASKED FOR BY NAME - "guard against out of order
      presets" - in its two halves, which are worth asking separately because
      they are asked at two different moments: what is being carried is worked
      out once, when the hand goes down, and what a destination would break is
      worked out afresh for every row the pointer passes over.

      D IS CARRIED AND Z IS NOT, and that is the difference between a mark that
      is applying and a mark that is only written down. Both name G. D is inside
      G, so G's `headerDerived` publishes it and its header really does get D
      ready; Z is not inside G, so nothing prepares it and the mark is already
      inert - `wfg validate` warns about it and the engine ignores it. A move
      cannot break what is not working, and a warning about Z would be a warning
      about a state the reader is already in. */
  assert.deepEqual(carriedMarks(subtreeOf("D")), [{ cue: "D", group: "G" }]);
  assert.deepEqual(carriedMarks(subtreeOf("Z")), []);

  /*  A MARK DEEP INSIDE THE THING BEING CARRIED IS STILL CARRIED. Drag the
      group X and both of its cues go with it, marked for G, which is staying
      put. Said as sorted text rather than as the objects: the walk downward
      builds a set and the order it comes out in is its own business, so an
      assertion that pinned it would break on a change nobody made. */
  assert.deepEqual(marksOf("X"), ["D2 by G", "D3 by G"]);

  /*  AND CARRYING THE GROUP THAT PREPARES THEM CARRIES NOTHING. Lift the whole
      act and its presets travel inside it: the header that gets them ready
      moves too, so the marks are as true after the drop as before it, wherever
      it lands. This is the case a guard written as "does this cue have a
      preset" would warn about on every single drop of a group. */
  assert.deepEqual(marksOf("G"), []);
});

test("a destination inside the group keeps a mark and one outside it breaks it", () => {
  /*  The second half, which is the same test on two trees: what applies NOW is
      the engine's own `headerDerived`, and what would apply AFTER cannot be
      asked of anything - the move has not happened - so it is ancestry,
      computed from where the cue is going. */
  const carried = carriedMarks(subtreeOf("D"));

  const into = (parent) => brokenBy(carried, new Set([parent].concat(chainAbove(parent))));

  //  Somewhere else inside G - into X, say, which is still under G's header.
  assert.deepEqual(into("X"), []);
  assert.deepEqual(into("G"), []);

  //  And out of G altogether, into the list: the mark stops applying.
  assert.deepEqual(into("L"), [{ cue: "D", group: "G" }]);

  /*  THE GROUP DRAGGED OUT WARNS FOR THE CUES INSIDE IT, which is the case
      nobody would think to check by hand: the reader is holding X and the marks
      that stop applying are on D2 and D3, rows they have not touched and may
      not even have open. */
  const withX = carriedMarks(subtreeOf("X"));
  const said = (broken) => broken.map((one) => one.cue + " by " + one.group).sort();

  assert.deepEqual(brokenBy(withX, new Set(["G"].concat(chainAbove("G")))), []);
  assert.deepEqual(said(brokenBy(withX, new Set(["L"].concat(chainAbove("L"))))),
                   ["D2 by G", "D3 by G"]);
});

test("a section's band is not a place, and the refusal says why", () => {
  /*  A <Header>, a <Footer> and a <Persistent> element have an identifier in
      the file and NO ADDRESS in the tree, so no client can learn one and
      `object.move` cannot be told to put a cue in one. The drop is not
      expressible - and a gesture that is not expressible has to be refused
      where the hand is, in words, rather than sent and quietly lost. */
  pickUp("cue:C1");

  for (const [key, word] of [["band:G:header", "a header"],
                             ["band:G:footer", "a footer"],
                             ["band:list:L:persistent", "the persistent band"]]) {
    const event = over(key, "middle");
    const said = shown();

    assert.equal(said.refused, true, key);
    assert.ok(said.words.startsWith("✕ "), key + ": refused in a word as well as a colour");
    assert.ok(said.words.includes(word), key + ": " + said.words);
    assert.ok(said.words.includes("no address in the tree"), key);

    //  AND THE CURSOR SAYS SO TOO, in the vocabulary every application on the
    //  machine uses for it.
    assert.equal(event.dataTransfer.dropEffect, "none", key);

    //  The band itself wears the mark, so the eye is taken to the thing the
    //  refusal is about rather than to a sentence floating at the pointer.
    assert.ok(rowFor(key).classes.has("drop-no"), key);
  }

  //  A band's far edge is the same refusal: the gap below a section is as much
  //  "inside a section" as the gap above it.
  over("band:G:header:end", "middle");
  assert.equal(shown().refused, true);
});

test("a cue in a header, a footer or the persistent band is no target either", () => {
  /*  REORDERING WITHIN A SECTION IS THE SAME PROBLEM AS DROPPING INTO ONE: a
      position beside H is a position in G's <Header>, and that container has no
      address to send. It is the cue's own `role` that decides, and not the
      frame the row is drawn in - a group sitting in a header has members of its
      own, drawn on that header's rail, whose parent is the group and which
      reorder among themselves perfectly well. */
  pickUp("cue:C1");

  for (const [key, word] of [["cue:H", "a header"], ["cue:F", "a footer"],
                             ["cue:P", "the persistent band"]]) {
    for (const where of ["top", "bottom"]) {
      const event = over(key, where);
      const said = shown();

      assert.equal(said.refused, true, key + " " + where);
      assert.ok(said.words.startsWith("✕ "), key + ": the words are the carrier");
      assert.ok(said.words.includes(word), key + ": " + said.words);
      assert.equal(event.dataTransfer.dropEffect, "none", key + " " + where);
    }

    assert.ok(rowFor(key).classes.has("drop-no"), key);
  }

  /*  AND THE CUE THAT IS IN NO SECTION IS NOT REFUSED, which is what makes the
      three above a rule rather than a page that refuses everything: M sits
      plainly among G's members and is a perfectly good place to drop beside. */
  const fine = over("cue:M", "top");

  assert.equal(shown().refused, false);
  assert.equal(fine.dataTransfer.dropEffect, "move");
});

test("a derived preset line can be neither dragged nor dropped against", () => {
  /*  A DERIVED LINE IS A READING OF A MARK, NOT A PLACE. It carries the
      member's own `data-pick` on purpose, because clicking it means that
      member - and that is exactly what makes it dangerous to a drop: ask the
      tree about that identifier and it answers truthfully about the member's
      OWN row, which is a parent and a position somewhere else entirely in the
      list. A drop aimed just above this line would be computed against a place
      the pointer is nowhere near, and would land there, silently and correctly
      by its own arithmetic. There is no order here to insert into at all: the
      derived lines are what `headerDerived` publishes, and that is not a
      sequence `object.move` can write. */
  pickUp("cue:C1");

  for (const where of ["top", "bottom"]) {
    const event = over("preset:G:D", where);
    const said = shown();

    assert.equal(said.refused, true, where);
    assert.ok(said.words.startsWith("✕ "), where);
    assert.ok(said.words.includes("reading of a preset mark"), said.words);
    assert.ok(said.words.includes("own row"), "and it says where the cue CAN be moved from");
    assert.equal(event.dataTransfer.dropEffect, "none", where);
  }

  /*  AND THE MEMBER'S OWN ROW, the one the refusal just named, IS a place -
      which is the half that makes the refusal a signpost rather than a wall.
      Same cue, same identifier, two rows, two answers. */
  assert.equal(over("cue:D", "top").dataTransfer.dropEffect, "move");
  assert.equal(shown().refused, false);
});

test("a group refuses to be dropped into itself or into anything inside it", () => {
  /*  The engine refuses this too, in as many words, because without the
      refusal the tree stops being a tree: the subtree detaches with the node
      and is never seen again, and the identifiers in it stay reserved for ever.
      The page refuses it sooner, where the hand is, and says which of the two
      it is. */
  pickUp("cue:G");

  //  Its own row, which is the drop a hand makes by letting go where it started.
  over("cue:G", "top");
  assert.equal(shown().refused, true);
  assert.ok(shown().words.includes("the cue being dragged"), shown().words);

  //  A member of it, a cue in its header, a cue in its footer, and a member of
  //  a group inside it - every one of those is inside the thing in the hand.
  for (const key of ["cue:M", "cue:X", "cue:H", "cue:F", "cue:D2"]) {
    const event = over(key, "top");

    assert.equal(shown().refused, true, key);
    assert.ok(shown().words.includes("inside itself"), key + ": " + shown().words);
    assert.equal(event.dataTransfer.dropEffect, "none", key);
  }

  /*  AND THE MIDDLE OF ITS OWN ROW, which is the one that would otherwise slip
      through: the middle third of a group is "inside this group, as its first
      member", a branch that is reached before the role is ever asked about. */
  const middle = over("cue:X", "middle");

  assert.equal(shown().refused, true, "the into-band of a group inside the one being dragged");
  assert.equal(middle.dataTransfer.dropEffect, "none");

  /*  A ROW OUTSIDE IT IS FINE, and dropping INTO a group that is not the one
      being carried is the gesture the middle band exists for. */
  assert.equal(over("cue:C1", "top").dataTransfer.dropEffect, "move");
  assert.equal(shown().refused, false);
});

test("after a group is after everything drawn inside it", () => {
  /*  THE DROP'S VISIBLE PROMISE, WHICH IS A PLACE ON THE SCREEN and not only a
      number. The cue pane is one flat run of rows: an open group's header band,
      the lines in it, its members and its footer all FOLLOW the group's own row
      as siblings. So "after this group" is not the element after its row -
      that is the first thing inside it - and a mark put there would tell a
      reader their cue was going somewhere it is not, while the index sent said
      something else entirely. */
  pickUp("cue:C1");

  over("cue:G", "bottom");

  const said = shown();

  assert.equal(said.refused, false);

  /*  G's block runs from its own row down to its footer's end band, and C1's
      row is the first thing after it. The mark stands there - past the header,
      past the members, past the footer - and not in front of `band:G:header`,
      which is where "the element after the row" would have put it, inside the
      group and one line below the row the pointer was on. */
  assert.equal(said.before, "cue:C1");

  //  And before a plain row the mark simply goes where the eye is.
  over("cue:M", "top");
  assert.equal(shown().before, "cue:M");

  /*  The group one level in is the same walk one level down, and it stops at
      its own last member rather than running on to the end of G. */
  over("cue:X", "bottom");
  assert.equal(shown().before, "cue:D", "after X is after D2 and D3, which are inside X");
});

test("a drop that breaks a preset mark goes through, and says so first", () => {
  /*  THE WARNING REACHING THE READER, which is the half of the guard that the
      two pure functions cannot show: the reader is holding D, whose mark is
      live, and carrying it out of G stops that mark applying. It is ALLOWED -
      refusing would block the legitimate half of the same gesture, and the move
      is one ctrl/⌘-Z away - so what it must not be is DISCOVERED afterwards. */
  pickUp("cue:D");

  const out = over("cue:C1", "top");
  const leaving = shown();

  assert.equal(leaving.refused, false, "it is a consequence, not a refusal");
  assert.equal(out.dataTransfer.dropEffect, "move");
  assert.ok(leaving.words.startsWith("⚠ "), "said as a warning and not as a refusal");
  assert.ok(leaving.words.includes("stops applying"), leaving.words);
  assert.ok(leaving.words.includes("header of G"),
            "and it names the header that will stop preparing it: " + leaving.words);

  /*  AND THE SAME CUE PUT SOMEWHERE ELSE INSIDE THE SAME GROUP SAYS NOTHING,
      which is what keeps the warning worth reading: a page that warned on every
      drag of a cue with a mark would teach an operator to ignore the label, and
      the next thing it said would be the thing that mattered. */
  over("cue:M", "top");
  assert.equal(shown().words, "", "moved within G, the mark still applies");

  over("cue:X", "middle");
  assert.equal(shown().words, "", "and into a group inside G, it still applies");
});

test("a group dragged out of its act warns for the marks on cues nobody touched", () => {
  /*  The case that would go unnoticed on a real show: the reader picks up X,
      which has no mark of its own, and the cues that stop being got ready are
      D2 and D3, rows further in which they may not even have open.

      AND SEVERAL MARKS ARE SAID AS SEVERAL, which is a different sentence from
      the one above and so a different thing to get wrong: a count, then the
      pairs named. The count is asserted as a number rather than as a word,
      because "2 preset marks" with a 1 in it would read as perfectly ordinary
      English and be a lie about how much of the show is about to change. */
  pickUp("cue:X");

  over("cue:C1", "top");

  const said = shown();

  assert.equal(said.refused, false);
  assert.ok(said.words.startsWith("⚠ 2 preset marks stop applying"), said.words);
  assert.ok(said.words.includes("D2 by G"), "it names the cues, not the group being dragged");
  assert.ok(said.words.includes("D3 by G"), said.words);

  //  Dropped back among G's members, nothing is broken and nothing is said.
  over("cue:M", "top");
  assert.equal(shown().words, "");

  /*  AND LIFTING THE WHOLE ACT SAYS NOTHING WHEREVER IT GOES: G's header
      travels with the cues it prepares, so every mark inside it is as true
      after the drop as before. */
  fire("dragend", {});
  pickUp("cue:G");

  over("cue:C1", "top");
  assert.equal(shown().words, "", "the presets move with the header that prepares them");
});

test("the pane's own background below the last row is the end of the list", () => {
  /*  A hand that has carried a cue down past every row means the bottom, which
      is a real place and a hard one to reach otherwise: the last row of a list
      is very often a member deep inside a group, where "after" it lands in that
      group rather than in the list.

      It is the drop where there is no target at all, which is `dropIndex`'s
      other arm reached through the page rather than through an argument - and
      the reason that arm answers with an end instead of with the -1 an
      `indexOf` hands back. */
  pickUp("cue:D");

  const event = { target: pane, clientY: 400, prevented: false,
                  dataTransfer: { dropEffect: "" },
                  preventDefault() { event.prevented = true; } };

  fire("dragover", event);

  assert.equal(event.prevented, true, "the drop is allowed");
  assert.equal(event.dataTransfer.dropEffect, "move");

  const said = shown();

  assert.equal(said.refused, false);
  assert.equal(said.before, null, "the mark is at the foot of the pane, below every row");

  //  D is leaving G by this drop, so the warning is on the label as well.
  assert.ok(said.words.includes("stops applying"), said.words);
});

test("a drag that ends anywhere lets the cue pane go", () => {
  /*  THE ONE FAILURE THIS GESTURE MUST NOT BE ABLE TO CAUSE. `view.holding`
      stops views/didi.js drawing rows at all, so a drag left in the air is a
      console that has stopped showing the show, with no error anywhere and
      nothing to click that would put it right. Every case in this file asserts
      the release at its foot; this one asserts the other end of it, and that
      the release can be asked for twice - which is what a drop followed by its
      own `dragend` does. */
  pickUp("cue:C1");
  assert.equal(view.holding, true, "the pane is held for as long as the hand is down");

  over("cue:M", "top");
  assert.ok(shown(), "and a mark stands in the pane while it is held");

  fire("dragend", {});

  assert.equal(view.holding, false);
  assert.equal(shown(), null, "the mark is taken down by the module and not by the next poll");

  //  Again, which is the drop-then-dragend pair, and it must not throw.
  fire("dragend", {});
  assert.equal(view.holding, false);

  /*  AND A DRAG THAT WAS NEVER ONE OF OURS IS NOT ONE THIS FILE HOLDS: a
      selection of text, or a link, starts a `dragstart` on something with no
      `data-pick`, and the pane must go on drawing. */
  const loose = element("div", { data: {} });

  fire("dragstart", { target: loose, dataTransfer: { effectAllowed: "", setData: () => {} } });
  assert.equal(view.holding, false, "something else on the page holds nothing");
});
