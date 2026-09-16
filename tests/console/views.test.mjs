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
