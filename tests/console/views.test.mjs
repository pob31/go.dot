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

test("the persistent band at the foot of a list is a frame like the others", () => {
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

  assert.deepEqual(keysOf(rows),
                   ["cue:C1", "band:list:L:persistent", "cue:P1", "cue:P2",
                    "band:list:L:persistent:end"]);

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

  assert.deepEqual(keysOf(shut), ["cue:C1", "band:list:L:persistent"]);
  assert.match(htmlOf(shut, "band:list:L:persistent"), /data-shut="yes"/);

  folded.clear();
});
