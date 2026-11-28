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

/*  TIMES IN SECONDS OR AS A CLOCK (namespace draft §151, BKM), as the window
    writes them: the show's `/godot/list/timeFormat`, the hours only past an
    hour, tenths kept, typed either way. */

import { test } from "node:test";
import assert from "node:assert/strict";

import { FakeElement, installDocument } from "./fake-dom.mjs";

installDocument();

const { tree } = await import("../../clients/console/plumbing/tree.js");
const { clockShown, clockTime, readingTime, typedTime } = await import("../../clients/console/model/time.js");
const { shownValue, commitText } = await import("../../clients/console/views/values.js");
const { offsetText } = await import("../../clients/console/model/touch.js");

function format(word) {
  tree.at = { "/godot/list/timeFormat": { FULL_PATH: "/godot/list/timeFormat", TYPE: "s", ACCESS: 3, VALUE: [word] } };
}

test("a clock: minutes and seconds, the hours past an hour, rounded once", () => {
  assert.equal(clockTime(83.46), "1:23.5");
  assert.equal(clockTime(59.96), "1:00.0");
  assert.equal(clockTime(3723.5), "1:02:03.5");
  assert.equal(clockTime(-4), "-0:04.0");
  assert.equal(clockTime(64.25, 3, true), "1:04.25");
  assert.equal(clockTime(7, 3, true), "0:07");
});

test("the show's format says which, seconds when it says nothing", () => {
  tree.at = {};
  assert.equal(clockShown(), false);
  assert.equal(readingTime(83.5), "83.5");

  format("clock");
  assert.equal(clockShown(), true);
  assert.equal(readingTime(83.5), "1:23.5");
  assert.equal(offsetText(12.3, clockShown()), "0:12.3");
  assert.equal(offsetText(12.3), "12.3 s");
});

test("typed either way; what is not a time is NaN", () => {
  assert.equal(typedTime("83.5"), 83.5);
  assert.equal(typedTime("83,5"), 83.5);
  assert.equal(typedTime("1:23.5"), 83.5);
  assert.equal(typedTime("1:02:03.5"), 3723.5);
  assert.ok(Number.isNaN(typedTime("1:75")));
  assert.ok(Number.isNaN(typedTime("1.5:20")));
  assert.ok(Number.isNaN(typedTime("soon")));

  //  With its units (BKQ), the author's own example first.
  assert.equal(clockTime(typedTime("1h34m3s1")), "1:34:03.1");
  assert.equal(typedTime("2m30"), 150);
  assert.equal(typedTime("1h30"), 5400);
  assert.equal(typedTime("90s"), 90);
  assert.equal(typedTime("1H 5M 2,5S"), 3902.5);
  assert.ok(Number.isNaN(typedTime("3s1m")));
  assert.ok(Number.isNaN(typedTime("1m75s")));
  assert.ok(Number.isNaN(typedTime("3.5s1")));
});

test("a time box shows a clock and writes its seconds back", () => {
  format("clock");

  const box = new FakeElement("input");
  box.type = "text";
  box.dataset.set = "/godot/cue/ABC/preWait";
  box.dataset.time = "yes";

  const node = { FULL_PATH: "/godot/cue/ABC/preWait", TYPE: "d", ACCESS: 3, VALUE: [83.5] };
  assert.equal(shownValue(node, box), "1:23.5");

  box.value = "1:02:03.5";
  assert.equal(commitText(box), "3723.5");

  box.value = "12";
  assert.equal(commitText(box), "12");

  box.value = "1h34m3s1";
  assert.equal(Number(commitText(box)), 5643.1);

  format("seconds");
  assert.equal(shownValue(node, box), 83.5);
});
