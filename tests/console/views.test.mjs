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

/*  TWO PIECES OF MARKUP THAT DECIDE SOMETHING, taken on their own.

    A cue row's time cell asks the tree whether the attribute exists and whether
    anybody may write it, and answers with a box, a reading or nothing. A run
    row's state is a mark for the two words a busy pane is full of and the word
    itself for the six it is not. Both are decisions rather than decoration, and
    both are a string this file can read. */

import { test } from "node:test";
import assert from "node:assert/strict";

import { installDocument } from "./fake-dom.mjs";

installDocument();

const { tree } = await import("../../clients/console/plumbing/tree.js");
const { timeCell } = await import("../../clients/console/views/didi.js");
const { stateMark } = await import("../../clients/console/views/gogo.js");

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
