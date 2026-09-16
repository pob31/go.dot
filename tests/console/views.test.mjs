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

/*  THE MARKUP THAT DECIDES SOMETHING, taken on its own.

    A cue row's time cell asks the tree whether the attribute exists and whether
    anybody may write it, and answers with a box, a reading or nothing. A run
    row's state is a mark for the two words a busy pane is full of and the word
    itself for the six it is not. A list's rows are a flat run of keyed strings
    in which a group's header and footer have to read as frames - a head, the
    rows of the section hanging on one rail, an end - with no wrapper element,
    because the reconciler models one flat list and nothing else. All of those
    are decisions rather than decoration, and all of them are a string this file
    can read. */

import { test } from "node:test";
import assert from "node:assert/strict";

import { installDocument } from "./fake-dom.mjs";

installDocument();

const { tree } = await import("../../clients/console/plumbing/tree.js");
const { timeCell, listRows } = await import("../../clients/console/views/didi.js");
const { stateMark } = await import("../../clients/console/views/gogo.js");
const { selection } = await import("../../clients/console/model/selection.js");
const { folded } = await import("../../clients/console/model/remember.js");

/*  The two indexes a cue row asks for are model/index.js's, built once per
    reply out of every trigger and every slot. This file is about what a row
    SAYS, so they are stubbed rather than imported: a fixture that had to
    publish a slot and a trigger before it could draw a header would be a
    fixture about the wrong thing. */
tree.overlaps = () => new Map();
tree.triggersOf = () => [];

function serve(nodes) {
  tree.at = {};
  for (const node of nodes) tree.at[node.FULL_PATH] = node;
}

const node = (path, value, access = 3) =>
  ({ FULL_PATH: path, TYPE: "d", ACCESS: access, VALUE: [value],
     DESCRIPTION: "How long it takes. Zero is a jump." });

test("a time column is a box where the value is a decision", () => {
  serve([node("/godot/cue/A/preWait", 1.5)]);

  const cell = timeCell("A", "preWait");

  assert.match(cell, /<input /);
  assert.match(cell, /data-set="\/godot\/cue\/A\/preWait"/);
  assert.match(cell, /value="1.5"/);
  assert.match(cell, /data-blank-zero="yes"/);

  /*  The box's own sentence is the parameter table's, so the column head's one
      word can be asked. */
  assert.match(cell, /title="preWait — How long it takes"/);
});

test("a nought is an empty box, whether anybody may write it or not", () => {
  /*  One column, one spelling of nothing. A writable nought shows blank
      because a column of noughts is noise; a read-only nought - a media cue
      whose file could not be read, so its length is unknown - would otherwise
      read as "0.0", which is a length rather than an absence. */
  serve([node("/godot/cue/A/preWait", 0), node("/godot/cue/B/duration", 0, 1)]);

  assert.match(timeCell("A", "preWait"), /value=""/);
  assert.equal(timeCell("B", "duration").includes("0.0"), false);
});

test("a duration nobody may write is a reading, and one no cue carries is nothing", () => {
  serve([node("/godot/cue/A/duration", 30.25, 1)]);

  const reading = timeCell("A", "duration");

  assert.equal(reading.includes("<input"), false);
  assert.match(reading, /class="when ro"/);
  assert.match(reading, />30.3</);      // one decimal, as a length is read

  /*  A memo has no duration node at all: an empty cell, and never a box that
      would write to an address the show does not have. */
  assert.equal(timeCell("A", "postWait"), '<div class="when"></div>');
});

test("a run says playing and armed as marks, and every other state as its word", () => {
  const playing = stateMark("playing");
  const armed = stateMark("armed");

  /*  TWO SHAPES BEFORE TWO COLOURS (§4.8): a filled triangle and a ring, so
      the two are told apart in a photograph of the screen. */
  assert.match(playing, /▶/);
  assert.match(armed, /○/);
  assert.notEqual(playing.replace("playing", ""), armed.replace("armed", ""));

  /*  The word is never lost: it is the mark's title and its label. */
  assert.match(playing, /title="playing"/);
  assert.match(playing, /aria-label="playing"/);
  assert.match(playing, /data-s="playing"/);

  /*  And the six an operator has to read rather than recognise keep theirs. */
  for (const state of ["waiting", "preparing", "postWait", "stopping", "failed", "done"]) {
    const said = stateMark(state);

    assert.match(said, new RegExp(">" + state + "<"));
    assert.equal(said.includes("mark"), false);
  }
});

test("the inspector keeps decisions and folds away what the engine says back", async () => {
  const { decided } = await import("../../clients/console/views/inspector.js");

  /*  §4.10's line, read off the node rather than off a list of names: a value
      anybody may write is a decision, a read-only one is derived. ACCESS 3 is
      read-write, 1 is read-only. */
  assert.equal(decided({ node: { ACCESS: 3 } }), true);
  assert.equal(decided({ node: { ACCESS: 2 } }), true);
  assert.equal(decided({ node: { ACCESS: 1 } }), false);
  assert.equal(decided({ node: { ACCESS: 0 } }), false);

  /*  The rows the author named - a cue's identity and structure, a media
      cue's hash - are all read-only, so all of them fold away by themselves. */
  for (const name of ["index", "kind", "parent", "role", "prepare", "hash", "cue"]) {
    assert.equal(decided({ name, node: { ACCESS: 1 } }), false, name + " is not a decision");
  }
});

test("the inspector puts a cue's fields in the order somebody works through them", async () => {
  const { blockOf, headingOf, inWorkingOrder } =
    await import("../../clients/console/views/inspector.js");

  const field = (name) => ({ owner: "cue", name, node: { ACCESS: 3 } });
  const ordered = (names, kind) => names.map(field).sort(inWorkingOrder(kind)).map((f) => f.name);

  /*  TIME READS AS TIME: what happens before, how long, what happens after -
      which alphabetical order gets exactly backwards. */
  assert.deepEqual(ordered(["postWait", "duration", "preWait"], "fade"),
                   ["preWait", "duration", "postWait"]);

  /*  A GROUP'S THREE WORDS TOGETHER (author, 2026-09-16): what kind of group,
      how it moves, how a round is ordered - and the round's own numbers after
      them, rather than scattered between them by the alphabet. */
  assert.deepEqual(ordered(["seed", "advance", "loops", "mode", "play", "selection"], "group"),
                   ["mode", "advance", "selection", "play", "loops", "seed"]);

  /*  A fade says what it moves, where to, and how. */
  assert.deepEqual(ordered(["points", "curve", "level", "target"], "fade"),
                   ["target", "level", "curve", "points"]);

  /*  And the whole panel, in blocks: what it is, when, what it does, how it
      sits in the list. */
  assert.deepEqual(
    ordered(["preset", "target", "name", "postWait", "enabled", "number", "preWait", "level"], "fade"),
    ["number", "name", "preWait", "postWait", "target", "level", "enabled", "preset"]);

  /*  A row nobody named still appears, at the end of the block it falls in,
      so a new one in the parameter table cannot go missing. */
  assert.deepEqual(ordered(["zebra", "enabled", "aardvark"], "memo"),
                   ["enabled", "aardvark", "zebra"]);

  /*  The headings: none over the cue itself, the kind's own word over its own
      rows, and the two words the page uses for the rest. */
  assert.equal(headingOf(blockOf(field("name"), "fade"), "fade"), null);
  assert.equal(headingOf(blockOf(field("preWait"), "fade"), "fade"), "when");
  assert.equal(headingOf(blockOf(field("curve"), "fade"), "fade"), "fade");
  assert.equal(headingOf(blockOf(field("mode"), "group"), "group"), "group");
  assert.equal(headingOf(blockOf(field("preset"), "fade"), "fade"), "in the list");
});


/*  THE PANEL OVER SEVERAL CUES, from here to the rows.

    THE TWO PURE FUNCTIONS FIRST. Everything the panel does over a selection is
    one of them: which rows it can offer at all is `intersect`, and what each row
    says is `agree`. inspector.js exports them for exactly this, and the edges
    they have are the ones a panel fixture would never think to build - no lists
    at all, a name in every list but the last, two values that are the same text
    and not the same JavaScript. */

const { intersect, agree, renderInspector } =
  await import("../../clients/console/views/inspector.js");

test("what several cues have in common is every name all of them carry, once", () => {
  /*  NOTHING CHOSEN IS NOTHING IN COMMON, which is not the same answer as
      everything: the panel asks this before it knows what it is showing, and a
      walk that answered "every name" to no lists would offer boxes writing to
      addresses no cue has. */
  assert.deepEqual(intersect([]), []);

  /*  One list is that list, unchanged. The ordinary panel - one cue, which is
      what the page spends its life in - must not become a different panel
      because the code that serves four now runs beside it. */
  assert.deepEqual(intersect([["name", "level"]]), ["name", "level"]);

  /*  IN EVERY LIST, AND THE LAST ONE IS A LIST. This is the case a walk that
      compared the first list against the second alone would pass: `level` is in
      two of the three, and offering it would draw one box whose commit writes
      two cues and is refused by the third - part of the show changed, with
      nothing on screen saying which part. */
  assert.deepEqual(intersect([["name", "level"], ["name", "level"], ["name"]]), ["name"]);

  /*  And two cues with nothing in common are a panel with no rows rather than
      an error: choosing a fade and a memo together is an ordinary mistake, and
      §3.5 says the honest answer is sometimes very little. */
  assert.deepEqual(intersect([["level"], ["curve"]]), []);

  /*  A NAME TWICE IS ANSWERED ONCE. Two owner words can publish the same word
      for one cue, and two identical rows under one label would be two boxes
      writing to different addresses, with no way for a reader to tell which of
      them took. */
  assert.deepEqual(intersect([["name", "name", "level"], ["name", "level"]]),
                   ["name", "level"]);

  /*  IN THE FIRST LIST'S ORDER. The sort that follows in `sharedFields` is
      total, so this is not what arranges the panel today; it is what the
      function promises, and the one thing about its answer a caller could not
      otherwise predict. */
  assert.deepEqual(intersect([["level", "name"], ["name", "level"]]), ["level", "name"]);
});

test("values agree when they read the same, and one that differs is one too many", () => {
  /*  Nothing and one thing agree with themselves. The same control asks this
      over one cue as over four, and a panel that said `mixed` about a cue on
      its own would be telling a reader their one cue disagreed with itself. */
  assert.equal(agree([]), true);
  assert.equal(agree(["Wash up"]), true);

  assert.equal(agree(["Wash up", "Wash up"]), true);
  assert.equal(agree(["Wash up", "Wash down"]), false);

  /*  AND THE LAST ONE COUNTS AS WELL. A comparison of the first two would call
      four cues agreed because the two the reader happened to click first were,
      and then show one cue's value as though it were all of theirs. */
  assert.equal(agree([1, 1, 2]), false);
  assert.equal(agree([1, 2, 1]), false);

  /*  THE SAME TEXT IS THE SAME VALUE, though `1` and `"1"` are not the same
      JavaScript. These values come out of JSON: a parameter can answer with a
      number for one cue and with the text a commit put there for the next, and
      a box draws both as `1`. Compared strictly, the panel would blank the box
      and say `mixed` about two cues that hold the same thing - and the next
      keystroke would write over a cue nobody meant to touch. */
  assert.equal(agree([1, "1"]), true);
  assert.equal(agree([true, "true"]), true);

  /*  A NOUGHT IS NOT AN ABSENCE, which is the other way one comparison can go
      wrong: a cue that waits for nothing and a node with nothing to say do not
      agree, and a panel that called them agreed would show a wait of 0 for a
      cue whose answer has not arrived. */
  assert.equal(agree([0, ""]), false);
});

/*  ENOUGH OF A DOCUMENT FOR THE PANEL TO BE ASKED WHAT IT SAYS.

    Most of the panel is markup and can be read as a string. The word `mixed`
    cannot: `sayMixed` puts it on the controls AFTER they are drawn, because
    whether four cues agree is a live question - another client, or this panel's
    own second and third writes, can bring four values together or take them
    apart while it stands open. So the pane the inspector draws into has to hand
    those controls back afterwards.

    This is the least that does that. Every tag becomes an object carrying the
    attributes it was written with, `data-*` under `dataset` as a browser puts
    them, and the three selectors the inspector actually uses. It is not a DOM
    and does not try to be one (tests/console/fake-dom.mjs says the same of
    itself); what it is for is the answers a reader gets that are nowhere in the
    markup - a box whose placeholder became a word, a tick box whose third state
    is a shape with the word written beside it, a menu parked on an option
    nobody is allowed to choose.

    `[^>]*` for the attributes is safe for the reason the row reader below is:
    every title the panel writes goes through `esc`, so no `>` can hide inside
    one. An unknown selector throws rather than answering nothing, because a
    stand-in that quietly returned no elements would make every case here pass
    for no reason whatever. */
const camel = (name) => name.replace(/-([a-z])/g, (_, letter) => letter.toUpperCase());

function readPanel(html) {
  const tags = /<([a-zA-Z]+)([^>]*)>([^<]*)/g;
  const found = [];
  let menu = null;
  let tag;

  while ((tag = tags.exec(html))) {
    const pairs = /([a-zA-Z-]+)="([^"]*)"/g;
    const element = { tagName: tag[1].toUpperCase(), dataset: {}, textContent: tag[3] };
    let pair;

    while ((pair = pairs.exec(tag[2]))) {
      if (pair[1].indexOf("data-") === 0) element.dataset[camel(pair[1].slice(5))] = pair[2];
      else element[pair[1]] = pair[2];
    }

    /*  The attributes written with no value - `checked`, `selected`,
        `disabled`, `hidden` - read as `true`, which is what a browser hands
        back and what `sayMixed` writes over. */
    for (const word of ["checked", "selected", "disabled", "hidden"]) {
      if (new RegExp("(^|\\s)" + word + "(\\s|$)")
            .test(tag[2].replace(/([a-zA-Z-]+)="([^"]*)"/g, " "))) element[word] = true;
    }

    if (element.tagName === "SELECT") {
      element.options = [];
      element.selectedIndex = 0;
      menu = element;
    } else if (element.tagName === "OPTION" && menu) {
      if (element.selected) menu.selectedIndex = menu.options.length;
      menu.options.push(element);
    }

    /*  `nextElementSibling` is the next tag in the markup. Rough, and exact for
        the one place the panel asks for it: the word beside a tick box is
        written immediately after the box. */
    if (found.length) found[found.length - 1].nextElementSibling = element;

    found.push(element);
  }

  return found;
}

function inspectPane() {
  const pane = {
    dataset: {},

    /*  HOW MANY TIMES THE PANEL WAS REBUILT, which is a rule of its own: the
        poll arrives up to ten times a second, and a panel rebuilt on each one
        takes the cursor out of the box somebody is typing in. */
    builds: 0,
    markup: "",
    parsed: null,

    get innerHTML() { return this.markup; },
    set innerHTML(html) { this.markup = html; this.parsed = null; this.builds += 1; },

    querySelectorAll(selector) {
      const asked = /^\[data-([a-z-]+)\]$/.exec(selector);

      if (!asked) throw new Error("this stand-in cannot answer " + selector);

      if (!this.parsed) this.parsed = readPanel(this.markup);

      const key = camel(asked[1]);

      return this.parsed.filter((element) => element.dataset[key] !== undefined);
    },
  };

  document.getElementById = (id) => (id === "inspect" ? pane : null);

  return pane;
}

/*  The panel, drawn over a chosen set and anchored on the first of them, which
    is what a shift-click down a list leaves.

    THE ANCHOR IS WRITTEN FIRST AND THE SET AFTER IT, and that order is not
    arbitrary: `selection.picked` is an accessor that re-derives the set, so
    assigning it takes the selection back to one row (model/selection.js). A
    fixture written the other way round would be a fixture about one cue,
    claiming to be about four. */
function drawPanel(chosen) {
  const pane = inspectPane();

  selection.picked = chosen[0];
  selection.chosen = chosen.slice();

  renderInspector();

  return pane;
}

/*  A field of the panel by the address it stands for, whatever shape its
    control turned out to be - a box, a menu, a tick box or a reading. */
const controlAt = (pane, address) =>
  pane.querySelectorAll("[data-all]")
      .find((one) => one.dataset.set === address || one.dataset.read === address);

/*  A field is offered when its label is on the panel, which is the one thing
    every control shape has in common. */
const offers = (html, name) => new RegExp(">" + name + "</label>").test(html);

/*  A parameter as the engine publishes one for a cue: ACCESS 1 is read-only, 3
    is read-write, and a list of `vals` is what makes a row a menu. */
function attr(id, name, value, type = "s", access = 3, vals) {
  const said = { FULL_PATH: "/godot/cue/" + id + "/" + name, TYPE: type, ACCESS: access,
                 VALUE: [value] };

  if (vals) said.RANGE = [{ VALS: vals }];

  return said;
}

const CURVES = ["linear", "log", "sine"];

/*  A LITTLE SHOW: two fades and a stop. The fades carry `level`, which the stop
    does not; the stop carries `verb`, which the fades do not - so each kind has
    a field the other lacks, in BOTH directions, which is the only way to ask an
    intersection an honest question. What differs between the two fades is said
    by the caller, because that is what each case below is about. */
function serveShow(a = {}, b = {}) {
  const fade = (id, said) => [
    attr(id, "kind", "fade", "s", 1),
    attr(id, "name", said.name || "Wash up", "s", said.access === undefined ? 3 : said.access),
    attr(id, "target", "dim." + id),
    attr(id, "level", 0.5, "d"),
    attr(id, "enabled", said.enabled !== false, said.enabled === false ? "F" : "T"),
    attr(id, "curve", said.curve || "linear", "s", 3, CURVES),
  ];

  serve(fade("A1", a).concat(fade("A2", b), [
    attr("B7", "kind", "stop", "s", 1),
    attr("B7", "name", "Kill it"),
    attr("B7", "target", "dim.9"),
    attr("B7", "verb", "fade"),
    attr("B7", "enabled", true, "T"),
    attr("B7", "curve", "linear", "s", 3, CURVES),
  ]));
}

test("two cues of a kind offer that kind's fields, and each row stands for both", () => {
  serveShow({ name: "Wash up" }, { name: "Wash down" });

  const pane = drawPanel(["A1", "A2"]);

  for (const name of ["name", "target", "level", "curve", "enabled"]) {
    assert.equal(offers(pane.innerHTML, name), true, name + " is offered over two fades");
  }

  /*  ONE ROW, EVERY ADDRESS IT STANDS FOR. `data-set` is the first of them,
      which gestures/fields.js commits under all the rules it already keeps -
      what Escape takes back, what the in-flight mark says - and `data-all` is
      every one of them, which is what the panel writes to after it. A row that
      carried only the first would edit one cue while looking like it had edited
      two. */
  const target = controlAt(pane, "/godot/cue/A1/target");

  assert.equal(target.dataset.set, "/godot/cue/A1/target");
  assert.equal(target.dataset.all, "/godot/cue/A1/target /godot/cue/A2/target");

  /*  AND ONE CUE IS THE PANEL THE PAGE HAS ALWAYS DRAWN, which is the other
      half of the same rule and the half that is easy to lose: `chosen.length >
      1` is one character away from sending every ordinary click down the road
      built for four. The head names the cue rather than counting it, nothing
      stands for anything else, and the delete button is the one that deletes
      the cue in front of you - all three, because the absence of `data-all`
      alone would still be true of a one-cue panel drawn the wrong way. */
  const alone = drawPanel(["A1"]).innerHTML;

  assert.match(alone, /<span class="text">Wash up<\/span>/, "the head names the cue");
  assert.equal(alone.includes("data-all"), false);
  assert.equal(alone.includes("data-delete-all"), false);
});

test("two kinds offer what they share, and a field only one of them has is not drawn", () => {
  serveShow();

  /*  BOTH WAYS ROUND. The names are walked from the FIRST list, so a fade's
      `level` can only be dropped by asking the stop, and a stop's `verb` can
      only be dropped by asking the fade: one order alone would pass with half
      the rule missing. */
  for (const order of [["A1", "B7"], ["B7", "A1"]]) {
    const html = drawPanel(order).innerHTML;
    const said = order[0] === "A1" ? "fade + stop" : "stop + fade";

    assert.equal(offers(html, "target"), true, "what both kinds carry is offered");
    assert.equal(offers(html, "curve"), true);

    assert.equal(offers(html, "level"), false, "a fade's level is not offered beside a stop");
    assert.equal(offers(html, "verb"), false, "nor a stop's verb beside a fade");

    /*  And no control writes to one either, label or no label: a box aimed at
        an address the other cue does not have is a commit that is refused for
        half the selection. */
    assert.equal(html.includes("/level"), false);
    assert.equal(html.includes("/verb"), false);

    /*  THE BLOCK WEARS BOTH WORDS. Naming only the first kind would tell a
        reader they were editing fades while a stop was among them, which is
        exactly the misunderstanding a bulk edit must not leave anybody in. */
    assert.ok(html.includes(">" + said + "<"), "the kinds' block is headed " + said);
  }
});

test("a value they agree on is shown, and one they do not is the word mixed", () => {
  /*  §4.8 IS THE POINT OF THIS ONE. A box blanked and left blank says nothing
      about why; the word in it is what tells a reader that four cues disagree
      rather than that four cues are empty. `level` is the control case: the
      same panel, the same shape of row, values that agree - and it shows them,
      so the word is not simply what this panel says about everything. */
  serveShow({ name: "Wash up" }, { name: "Wash down" });

  const pane = drawPanel(["A1", "A2"]);
  const name = controlAt(pane, "/godot/cue/A1/name");
  const level = controlAt(pane, "/godot/cue/A1/level");

  assert.equal(name.placeholder, "mixed");
  assert.equal(name.value, "");

  assert.equal(level.value, "0.5");
  assert.ok(!level.placeholder, "a field they agree on says its value and nothing else");
});

test("a tick box and a menu say the word too, neither having a shape that could", () => {
  /*  THE TWO CONTROLS §4.8 IS HARDEST ON. A tick box's third state is a dash in
      a box - a shape with no word - so the word goes beside it; a menu showing
      one cue's answer over four cues is a quiet lie, so the word is an option
      of its own, disabled because "mixed" is not a value any parameter takes.
      Both are easy to lose to a tidy-up that trusted `indeterminate` and a
      selected option to speak for themselves. */
  serveShow({ enabled: true, curve: "linear" }, { enabled: false, curve: "sine" });

  const pane = drawPanel(["A1", "A2"]);
  const box = controlAt(pane, "/godot/cue/A1/enabled");
  const word = box.nextElementSibling;

  assert.equal(box.type, "checkbox");
  assert.equal(box.indeterminate, true);
  assert.equal(word.textContent, "mixed");
  assert.equal(word.hidden, false, "the word beside it is shown, the shape not being enough");

  const menu = controlAt(pane, "/godot/cue/A1/curve");

  assert.equal(menu.tagName, "SELECT");
  assert.equal(menu.options[menu.selectedIndex].textContent, "mixed");
  assert.equal(menu.options[menu.selectedIndex].disabled, true, "nobody can ask for mixed");

  /*  AND NEITHER SAYS IT WHEN THEY AGREE, which is what makes the two above
      about the answer rather than about the markup: the word is written into
      every menu and beside every tick box of a panel over many, hidden or
      unselected, and `sayMixed` is the only thing that shows it. */
  serveShow({ enabled: true, curve: "linear" }, { enabled: true, curve: "linear" });

  const same = drawPanel(["A1", "A2"]);
  const agreed = controlAt(same, "/godot/cue/A1/enabled");

  assert.equal(agreed.indeterminate, false);
  assert.equal(agreed.nextElementSibling.hidden, true);

  const settled = controlAt(same, "/godot/cue/A1/curve");

  assert.equal(settled.options[settled.selectedIndex].textContent, "linear");
});

test("read-only for any of them is read-only for all, and says mixed as a reading", () => {
  /*  THE SECOND CUE'S ANSWER IS THE ONE THAT DECIDES IT. A row's shape - its
      type, its range, its sentence - is taken from the FIRST of the chosen, so
      a `decided` that asked only that node would draw a box here and be right
      about the cue it asked. A box that wrote to one cue and was refused for
      the other is the worst of both answers: half the selection changed, with
      nothing on screen saying which half. */
  serveShow({ name: "Wash up", access: 3 }, { name: "Wash down", access: 1 });

  const pane = drawPanel(["A1", "A2"]);
  const html = pane.innerHTML;
  const fold = html.indexOf("<details");

  assert.ok(fold > 0, "the panel has a fold for what nobody decided");
  assert.equal(offers(html.slice(0, fold), "name"), false, "the name is not among the decisions");
  assert.equal(offers(html.slice(fold), "name"), true, "it is a reading behind the fold");
  assert.equal(html.includes('data-set="/godot/cue/A1/name"'), false, "and there is no box");

  const reading = controlAt(pane, "/godot/cue/A1/name");

  assert.equal(reading.tagName, "DIV");
  assert.equal(reading.dataset.all, "/godot/cue/A1/name /godot/cue/A2/name");

  /*  A READING IS KEPT HONEST TOO: showing the first cue's name over two cues
      that hold different ones would be the same lie a box would tell. */
  assert.equal(reading.textContent, "mixed");

  /*  AND WITH BOTH WRITABLE THE SAME FIELD IS A BOX, which is what makes the
      case above about the other cue's access rather than about this field. */
  serveShow({ name: "Wash up" }, { name: "Wash down" });

  assert.equal(controlAt(drawPanel(["A1", "A2"]), "/godot/cue/A1/name").tagName, "INPUT");
});

test("the panel says how many cues it is showing, and of what", () => {
  serveShow();

  /*  HOW MANY ANSWERS THE FIRST QUESTION AND THE TALLY ANSWERS THE SECOND: the
      tally is what tells a reader why they are being offered the rows they are
      rather than a fade's whole list. Both are asked at two sizes, because a
      panel that said "3 cues" from a constant would pass a case asked once. */
  assert.match(drawPanel(["A1", "A2", "B7"]).innerHTML, /<span class="text">3 cues<\/span>/);
  assert.ok(drawPanel(["A1", "A2", "B7"]).innerHTML.includes("2 fade · 1 stop"));

  assert.match(drawPanel(["A1", "B7"]).innerHTML, /<span class="text">2 cues<\/span>/);
  assert.ok(drawPanel(["A1", "B7"]).innerHTML.includes("1 fade · 1 stop"));
});

test("mixed is said into the panel that stands, rather than by rebuilding it", () => {
  /*  WHAT IS MIXED IS NOT PART OF THE PANEL'S SHAPE. Agreement is a live
      question - another client renames one of four cues while the panel is
      open - and the answer arrives on the poll, ten times a second. If saying
      it meant rebuilding the panel, the cursor would leave the box somebody was
      typing in every time a value moved anywhere in the selection; if it were
      written into the markup once, the panel would go on showing one name for
      two cues that no longer share it. */
  serveShow({ name: "Wash up" }, { name: "Wash up" });

  const pane = drawPanel(["A1", "A2"]);
  const name = controlAt(pane, "/godot/cue/A1/name");

  assert.equal(pane.builds, 1);
  assert.equal(name.value, "Wash up");

  tree.at["/godot/cue/A2/name"].VALUE = ["Wash down"];

  renderInspector();

  assert.equal(pane.builds, 1, "the panel that stands is not rebuilt");
  assert.equal(name.value, "", "and the box that stands in it is told");
  assert.equal(name.placeholder, "mixed");
});


/*  THE LIST'S OWN ROWS, from here down.

    `listRows` is the whole of Didi's pane as a keyed list of strings, which is
    what makes the frames testable at all: a header section is a head row, the
    rows it holds, and an end row, all siblings, tied together by a rail drawn
    down the left of each of them. Nothing in that can be seen in a screenshot
    and all of it can be got wrong - an end row emitted before the last row of
    a section (a header cue that is itself an open group puts its children
    after it), a member accidentally inside the frame, a shut section that
    still draws what it is hiding. */

/*  A string leaf as the engine serves one, for the rows rather than for the
    time columns, which want a number and a sentence. */
const leaf = (path, value) => ({ FULL_PATH: path, TYPE: "s", ACCESS: 3, VALUE: [value] });

/*  A LIST AS THE ENGINE PUBLISHES ONE: a group with both sections, a member
    marked `preset` for that group - so one cue is on screen twice, as its own
    row and as the derived line in the header - and a cue after the group, so
    that the frame is seen to close before it. */
function serveList() {
  serve([
    leaf("/godot/list/L/order", "G1 M9"),

    leaf("/godot/cue/G1/kind", "group"),
    leaf("/godot/cue/G1/name", "Act one"),
    leaf("/godot/cue/G1/headerDerived", "D1"),
    leaf("/godot/cue/G1/headerOrder", "H1"),
    leaf("/godot/cue/G1/order", "D1 K1"),
    leaf("/godot/cue/G1/footerOrder", "F1"),

    leaf("/godot/cue/D1/kind", "fade"),
    leaf("/godot/cue/D1/name", "Wash up"),
    leaf("/godot/cue/D1/preset", "G1"),

    leaf("/godot/cue/H1/kind", "fade"),
    leaf("/godot/cue/H1/name", "House to half"),
    leaf("/godot/cue/K1/kind", "memo"),
    leaf("/godot/cue/F1/kind", "stop"),
    leaf("/godot/cue/M9/kind", "memo"),
  ]);
}

const keysOf = (rows) => rows.map((row) => row.key);
const htmlOf = (rows, key) => (rows.find((row) => row.key === key) || {}).html || "";

/*  A row's own element: everything up to the first `>`, which is where its
    attributes are and where the attributes of what it CONTAINS are not. Every
    title in the page goes through `esc`, so no `>` can hide in one. */
const opening = (html) => html.slice(0, html.indexOf(">") + 1);

/*  The rail this row hangs on, as the value of the custom property rather than
    as an exact spelling: the frame is straight when a section's rows and its
    head and its end all name the same one. */
const railOf = (html) => (/--rail:\s*([^;"]+)/.exec(html) || [])[1];

test("a section is a frame: a head that folds it, its rows on a rail, and an end that closes it", () => {
  folded.clear();
  selection.reveal = null;
  serveList();

  const rows = listRows("L", "");

  /*  THE END ROW AFTER THE LAST ROW OF THE SECTION, and the members outside
      both frames. Derived lines come before written header cues because that
      is the order the horizon prepares them in (views/didi.js). */
  assert.deepEqual(keysOf(rows), [
    "cue:G1",
    "band:G1:header", "preset:G1:D1", "cue:H1", "band:G1:header:end",
    "cue:D1", "cue:K1",
    "band:G1:footer", "cue:F1", "band:G1:footer:end",
    "cue:M9",
  ]);

  const head = htmlOf(rows, "band:G1:header");

  assert.match(head, /class="band"/);
  assert.match(head, /data-fold="G1:header"/);
  assert.match(head, /data-shut="no"/);

  /*  A SHAPE AS WELL AS A STATE (§4.8): the twist says open before any colour
      does, and either spelling of the character is the same triangle. */
  assert.match(head, /▼|&#9660;/);

  /*  The word, and how many lines are behind it - the derived line and the
      written cue, which is what the section holds. */
  assert.match(head, /class="word"[^>]*>header</);
  assert.match(head, /class="count"[^>]*>2</);

  /*  And a sentence saying what a header IS, because "header" is a word a
      reader meets before they have been told what it does. */
  assert.match(head, /title="[^"]{12,}"/);

  const rail = railOf(head);

  assert.equal(typeof rail, "string", "the head names the rail its rows hang on");
  assert.match(rail, /16px/, "a group at depth 0 puts its section one indent in");

  for (const key of ["preset:G1:D1", "cue:H1"]) {
    assert.match(opening(htmlOf(rows, key)), /data-in="header"/, key + " is inside the header");
    assert.equal(railOf(htmlOf(rows, key)), rail, key + " hangs on the head's rail");
  }

  const ending = htmlOf(rows, "band:G1:header:end");

  assert.match(ending, /class="band-end"/);
  assert.equal(railOf(ending), rail);

  const footHead = htmlOf(rows, "band:G1:footer");

  assert.match(footHead, /data-fold="G1:footer"/);
  assert.match(footHead, /class="word"[^>]*>footer</);
  assert.match(footHead, /class="count"[^>]*>1</);
  assert.match(opening(htmlOf(rows, "cue:F1")), /data-in="footer"/);
  assert.equal(railOf(htmlOf(rows, "cue:F1")), railOf(footHead));
  assert.equal(railOf(htmlOf(rows, "band:G1:footer:end")), railOf(footHead));

  /*  THE MEMBERS STAY PLAIN, which is the whole point of the frames: what
      delimits a header from what merely follows it is the frame, so a member
      that carried the rail would read as part of the section above it. */
  for (const key of ["cue:D1", "cue:K1", "cue:M9"]) {
    assert.equal(/data-in="/.test(opening(htmlOf(rows, key))), false,
                 key + " is a member and stands outside both frames");
    assert.equal(/--rail:/.test(htmlOf(rows, key)), false, key + " hangs on no rail");
  }
});

test("a shut section says how much it is hiding and draws none of it", () => {
  folded.clear();
  folded.add("G1:header");
  selection.reveal = null;
  serveList();

  const rows = listRows("L", "");

  assert.deepEqual(keysOf(rows), [
    "cue:G1",
    "band:G1:header",
    "cue:D1", "cue:K1",
    "band:G1:footer", "cue:F1", "band:G1:footer:end",
    "cue:M9",
  ]);

  const head = htmlOf(rows, "band:G1:header");

  assert.match(head, /data-shut="yes"/);
  assert.match(head, /▶|&#9654;/);

  /*  THE COUNT IS WHAT A SHUT SECTION SAYS FOR ITSELF. A frame folded to one
      line is a line that has to be worth reading: two, and the reader knows
      whether anything is behind the twist without turning it. */
  assert.match(head, /class="count"[^>]*>2</);
  assert.match(head, /class="word"[^>]*>header</);
});

test("a group's own fold and its sections' folds are different keys", () => {
  /*  Shutting the group takes the whole subtree, sections and all; shutting a
      section leaves the group open. One key each, and a `folded` that could not
      tell them apart would fold a group the moment its header was folded. */
  folded.clear();
  folded.add("G1");
  selection.reveal = null;
  serveList();

  assert.deepEqual(keysOf(listRows("L", "")), ["cue:G1", "cue:M9"]);
});

test("a derived line and the cue it is a view of point at each other", () => {
  folded.clear();
  selection.reveal = null;
  serveList();

  const rows = listRows("L", "");
  const derived = htmlOf(rows, "preset:G1:D1");

  /*  THE LINE GOES TO THE ROW. It still picks the member - there is one object
      and this is a second view of it - and it also says where that object's own
      row is, which is what the author asked for: "I could get the focus of a
      header item with the actual cue". */
  assert.match(opening(derived), /data-pick="D1"/);
  assert.match(opening(derived), /data-reveal="cue:D1"/);

  const member = htmlOf(rows, "cue:D1");

  /*  AND THE ROW GOES TO THE LINE, from the flag and not from the row: picking
      a member is asking about it, and a row that carried the reveal would
      scroll the list away every time anybody clicked one. */
  assert.match(member, /data-reveal="preset:G1:D1"/);
  assert.equal(/data-reveal/.test(opening(member)), false,
               "the reveal is on the flag, not on the row");
  assert.match(member, /class="flag preset/);

  /*  The flag still NAMES the group (§4.8): it is a pointer now, and a pointer
      that had become an icon would say where it goes in shape alone.

      IN ITS OWN TEXT, not merely in its title, which is why this asks for the
      name BETWEEN the flag's tags. `cueName` goes into the title as well, so a
      loose match on the name alone passed with the visible text stripped to a
      bare arrow - which is exactly the failure §4.8 is about. `esc` escapes
      &, <, > and ", so neither character class can run past its own tag. */
  assert.match(member, /class="flag preset[^>]*>[^<]*Act one</,
               "the flag names the group in its own text, not only in its title");
});

/*  AND A MARK THAT GOES NOWHERE IS NOT DRESSED AS A WAY IN.

    `preset` names an ANCESTOR, and a value naming anything else is a `wfg
    validate` warning the engine tolerates rather than refuses - so the group's
    walk never reaches this cue, no derived line is published, and there is no
    row at the far end of the tendril. Clicking it would have unfolded that
    group and its header - throwing away folds the reader had made and now keeps
    across reloads - to show them a section the line is not in.

    The word stays, because somebody decided it (§4.10) and the reader is
    entitled to see what the document says. */
test("a mark naming a header that does not show the cue is a word, not a link", () => {
  folded.clear();
  serveList();

  /*  The same fixture, with the member's mark pointed at a group that does not
      list it: G1's `headerDerived` still says D1 and nothing else. */
  tree.at["/godot/cue/D1/preset"] = leaf("/godot/cue/D1/preset", "G9");
  tree.at["/godot/cue/G9/name"] = leaf("/godot/cue/G9/name", "Somewhere else");

  const member = htmlOf(listRows("L", ""), "cue:D1");

  assert.match(member, /class="flag preset/, "the mark is still drawn");
  assert.match(member, /Somewhere else/, "and still names what the document says");
  assert.equal(/data-reveal/.test(member), false,
               "but it is not a way in, because there is nothing at the far end");
});

test("the reveal is in the markup, and only for as long as it lasts", () => {
  folded.clear();
  serveList();

  /*  IN THE MARKUP AND NOWHERE ELSE. `morph` copies the fresh element's
      attributes onto the live one and removes the ones it lacks, so a class
      put on a row after a render is wiped by the next poll a tenth of a second
      later - the flash has to be drawn, every time, for as long as it lasts. */
  selection.reveal = { key: "cue:D1", until: Date.now() + 1100, scrolled: false };

  const rows = listRows("L", "");

  assert.match(opening(htmlOf(rows, "cue:D1")), /data-flash="yes"/);
  assert.equal(/data-flash="yes"/.test(htmlOf(rows, "cue:K1")), false);

  /*  KEYED BY THE ROW AND NOT BY THE CUE: the same cue is on screen twice, and
      flashing both would tell the reader nothing about where they were sent. */
  assert.equal(/data-flash="yes"/.test(htmlOf(rows, "preset:G1:D1")), false);

  /*  And when it is over it is over: a row left flashing for the rest of the
      show would be a highlight that means nothing. */
  selection.reveal = { key: "cue:D1", until: Date.now() - 1, scrolled: true };

  assert.equal(listRows("L", "").some((row) => /data-flash="yes"/.test(row.html)), false);

  selection.reveal = null;
});

test("the persistent band heads a list, as a frame like the others", () => {
  folded.clear();
  selection.reveal = null;

  serve([
    leaf("/godot/list/L/order", "C1"),
    leaf("/godot/list/L/persistentOrder", "P1 P2"),
    leaf("/godot/cue/C1/kind", "memo"),
    leaf("/godot/cue/P1/kind", "media"),
    leaf("/godot/cue/P2/kind", "media"),
  ]);

  const rows = listRows("L", "");

  /*  FIRST, NOT LAST (author, 2026-09-16: "I would place the persistent
      container towards the top since this is something that runs as soon as the
      show starts"). It sat at the foot because it is not on the path the
      pointer walks; it stands at the head because it is what is already running
      before anybody presses anything. Both remain true, which is why the head's
      own sentence has to carry the second one - asserted below. */
  assert.deepEqual(keysOf(rows),
                   ["band:list:L:persistent", "cue:P1", "cue:P2",
                    "band:list:L:persistent:end", "cue:C1"]);

  const head = htmlOf(rows, "band:list:L:persistent");

  /*  ITS FOLD KEY NAMES THE LIST, not a cue: two lists can both have a
      persistent band, and a key that was only "persistent" would fold both. */
  assert.match(head, /data-fold="list:L:persistent"/);
  assert.match(head, /data-shut="no"/);
  assert.match(head, /class="word"[^>]*>persistent</);
  assert.match(head, /class="count"[^>]*>2</);
  assert.match(head, /title="[^"]{12,}"/);

  const rail = railOf(head);

  /*  AT DEPTH 0: the gutter and the number column, both scaled by the type
      knob, and no indent - these cues sit under the list and not inside
      anything. */
  assert.match(rail, /12px/);
  assert.match(rail, /var\(--type\)/, "the columns it clears are scaled by the type knob");
  assert.equal(/16px/.test(rail), false);

  for (const key of ["cue:P1", "cue:P2"]) {
    assert.match(opening(htmlOf(rows, key)), /data-in="persistent"/);
    assert.equal(railOf(htmlOf(rows, key)), rail);
  }

  folded.add("list:L:persistent");

  const shut = listRows("L", "");

  assert.deepEqual(keysOf(shut), ["band:list:L:persistent", "cue:C1"]);
  assert.match(htmlOf(shut, "band:list:L:persistent"), /data-shut="yes"/);

  folded.clear();
});


/*  AND WHAT THE ROWS SAY ABOUT WHAT IS CHOSEN, which is the last thing in the
    markup that decides something: a set the reader cannot see the edges of is a
    set they are about to delete the wrong end of. */

/*  The selection as a click leaves it: the anchor, then the rest of the set.

    IN THAT ORDER, because `selection.picked` is an accessor that re-derives the
    set - assigning it means that row and no other (model/selection.js). A
    fixture that wrote the set first would have it wiped by the next line and
    would quietly be a fixture about one row. */
function pick(anchor, ...chosen) {
  selection.picked = anchor;

  if (chosen.length) selection.chosen = chosen;
}

test("every chosen row says so, and the anchor alone says where a range measures from", () => {
  folded.clear();
  selection.reveal = null;
  serveList();
  pick("K1", "H1", "K1", "M9");

  const rows = listRows("L", "");

  /*  The attribute's own word, rather than whether it says "yes". A row that
      had stopped drawing the mark at all would look, to a case that only asked
      whether it said "yes", exactly like a row outside the set - so the second
      loop below would pass with the mark gone from every row in the pane. */
  const chosenAt = (key) => (/data-picked="(yes|no)"/.exec(opening(htmlOf(rows, key))) || [])[1];

  /*  EVERY ROW OF THE SET, not only the one clicked last. A range that marked a
      single row would be a selection whose size the reader cannot see, and the
      next things anybody does with a range are drag it and delete it. */
  for (const key of ["cue:H1", "cue:K1", "cue:M9"]) {
    assert.equal(chosenAt(key), "yes", key + " is in the set and says so");
  }

  /*  And the rows outside it say so in the same word, which is what gives the
      stylesheet an ordinary row to write and the reader a row they can tell
      from one the render had not reached. */
  for (const key of ["cue:G1", "cue:D1", "cue:F1", "preset:G1:D1"]) {
    assert.equal(chosenAt(key), "no", key + " is outside the set and says so");
  }

  /*  ONE ANCHOR, on the row the hand is on. It is what tells a reader where the
      next shift-click will measure from - the end that stays put - and what
      every single-cue button on the panel over many is aimed at. It is one line
      of views/didi.js, it changes nothing else on the page if it goes, and the
      selection would go on looking entirely correct without it. */
  assert.deepEqual(rows.filter((row) => /data-anchor="yes"/.test(row.html)).map((row) => row.key),
                   ["cue:K1"]);
});

test("a cue on screen twice is chosen in both of its rows", () => {
  folded.clear();
  selection.reveal = null;
  serveList();
  pick("D1");

  const rows = listRows("L", "");

  /*  ONE OBJECT, TWO VIEWS OF IT: D1's own row among the members, and the
      derived line drawn in the header that gets it ready. A reader who chose
      one and saw the other left plain would be reading the two as different
      cues - which is the misunderstanding the derived line exists to prevent,
      not to cause. The line asks the set by the MEMBER's identifier, so it
      needs no place in the range of its own. */
  for (const key of ["cue:D1", "preset:G1:D1"]) {
    assert.match(opening(htmlOf(rows, key)), /data-picked="yes"/, key + " is chosen");
    assert.match(opening(htmlOf(rows, key)), /data-anchor="yes"/, key + " is drawn as the anchor");
  }

  assert.deepEqual(rows.filter((row) => /data-picked="yes"/.test(row.html)).map((row) => row.key),
                   ["preset:G1:D1", "cue:D1"]);
});

test("a pane beside a model with no set still draws the one row that is picked", () => {
  /*  THE BELT ON THE BRACES, and it is worth a case because losing it fails
      silently: `chosen` is the array this pane turns into a set once per
      render, and a pane that met a model without one would draw a list in which
      nothing is ever marked - no error, no empty pane, just a page that has
      stopped answering the click. */
  folded.clear();
  selection.reveal = null;
  serveList();

  selection.picked = "K1";
  selection.chosen = null;

  const rows = listRows("L", "");

  assert.match(opening(htmlOf(rows, "cue:K1")), /data-picked="yes"/);
  assert.match(opening(htmlOf(rows, "cue:M9")), /data-picked="no"/);

  selection.picked = null;
});
