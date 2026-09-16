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

/*  WHAT THE READER IS LOOKING AT, KEPT ACROSS A RELOAD - and every way that
    keeping it can fail.

    model/remember.js is the first thing this page writes anywhere. What it
    writes is small and entirely the reader's - which containers are shut, where
    the inspector sits - and §14.1 is why it goes to the browser and not to the
    engine: the document holds what somebody decided (§4.10), and nobody decides
    to fold a group.

    So the rules worth pinning here are the ones about a page that has to go on
    working when the writing does not. A private window has no `localStorage` at
    all; a browser told to block site data throws from the ACCESSOR rather than
    from the call, which is why the page reads the global at each access instead
    of catching one reference at import; a full quota refuses the write. In each
    of those the fold still folds - it is only forgotten by the next reload -
    and that is what a test can say and a screen cannot.

    The shape on disk is pinned here as well, because it is the one thing in
    this page that a later version will have to read back. */

import { test, beforeEach } from "node:test";
import assert from "node:assert/strict";

import { installDocument } from "./fake-dom.mjs";

/*  Nothing below touches an element - remember.js is a model file - but the
    page's modules are imported exactly as the browser imports them, so the
    global they expect at load is stood up first, as every other file here
    does: a failure in this one should be about this one. */
installDocument();

/*  A localStorage as the page uses one, and the two ways a browser refuses:
    it will not read, or it will not write. `map` is what a test reads back to
    see what was actually written. */
function fakeStorage(refuses = {}) {
  const map = new Map();

  return {
    map,

    getItem(key) {
      if (refuses.read) throw new Error("site data is not available in this window");
      return map.has(key) ? map.get(key) : null;
    },

    setItem(key, value) {
      if (refuses.write) throw new Error("the quota has been exceeded");
      map.set(key, String(value));
    },

    removeItem(key) { map.delete(key); },
  };
}

/*  ALWAYS THROUGH defineProperty, never a plain assignment: one test below
    stands a THROWING ACCESSOR on this name, and a module is strict code, where
    assigning over an accessor that has no setter throws - which would be this
    file breaking itself rather than the page being tested. */
function stand(storage) {
  Object.defineProperty(globalThis, "localStorage",
                        { value: storage, configurable: true, writable: true });
}

let storage = fakeStorage();

stand(storage);

/*  After the global, for the same reason the other files import late. */
const { folded, panel, toggle, adopt, save, decode, encode, trim } =
  await import("../../clients/console/model/remember.js");

/*  ONE KEY, and the page's whole memory under it. */
const KEY = "godot.console.view";

const written = () => storage.map.get(KEY);

beforeEach(() => {
  storage = fakeStorage();
  stand(storage);

  folded.clear();
  panel.details = false;
  panel.layout = "side";
});

test("anything that is not a store this page wrote is read as the defaults", () => {
  /*  `decode`'s checks, one case each: not a string at all, not JSON, JSON that is not
      an object, a version this page does not know, a `shows` that is not an
      object, and a `shut` that is not a list of strings. A reader whose
      storage has been hand-edited, or written by a later version, or shared
      with another page on the same origin, gets a working console and not a
      thrown module. */
  const rubbish = [
    undefined, null, 42, {}, ["/a.godot"],
    "", "not json at all", "{", "undefined",
    "null", "42", '"a string"', "[1,2,3]",
    '{"v":2,"panel":{"details":true,"layout":"foot"},"shows":{"/a.godot":{"shut":["X"],"seen":9}}}',
    '{"v":1,"panel":{"details":false,"layout":"side"},"shows":"not an object"}',
    '{"v":1,"panel":{"details":false,"layout":"side"},"shows":{"/a.godot":{"shut":"X","seen":9}}}',
    '{"v":1,"panel":{"details":false,"layout":"side"},"shows":{"/a.godot":{"shut":["X",7],"seen":9}}}',
    '{"v":1,"shows":{}}',
    '{"shows":{}}',
  ];

  for (const text of rubbish) {
    const store = decode(text);
    const said = " (from " + String(text) + ")";

    assert.equal(store.v, 1, "a default store says which version it is" + said);
    assert.deepEqual(store.panel, { details: false, layout: "side" },
                     "the panel falls back to the arrangement the page opens with" + said);
    assert.deepEqual(store.shows, {},
                     "and nothing is remembered about any show" + said);
  }
});

test("a store this page wrote comes back exactly as it was written", () => {
  const store = {
    v: 1,
    panel: { details: true, layout: "foot" },
    shows: {
      "/shows/one.godot": { shut: ["B3N8", "B3N8:header", "list:L1:persistent"], seen: 1699000000000 },
      "/shows/two.godot": { shut: [], seen: 1699000000001 },
    },
  };

  const text = encode(store);

  assert.equal(typeof text, "string", "what goes to localStorage is text");

  const back = decode(text);

  assert.equal(back.v, 1);
  assert.deepEqual(back.panel, { details: true, layout: "foot" });
  assert.deepEqual(Object.keys(back.shows).sort(), ["/shows/one.godot", "/shows/two.godot"]);

  /*  THE COLONS SURVIVE, which is the whole of it: a fold key is
      `<cueId>:header` and `list:<listId>:persistent`, so an encoding that
      joined or split on a colon - or on the slashes in a document path - would
      come back as keys that match no row and quietly unfold the show. */
  assert.deepEqual(back.shows["/shows/one.godot"].shut,
                   ["B3N8", "B3N8:header", "list:L1:persistent"]);
  assert.equal(back.shows["/shows/one.godot"].seen, 1699000000000);

  /*  And a show that is remembered with nothing shut is not the same as a show
      that is not remembered: it is the one the reader has just opened flat. */
  assert.deepEqual(back.shows["/shows/two.godot"].shut, []);
});

test("trimming keeps the shows most recently looked at and throws the rest away", () => {
  /*  Out of insertion order on purpose: what is kept is decided by `seen` and
      not by what happens to be first in the object. */
  const store = {
    v: 1,
    panel: { details: false, layout: "side" },
    shows: {
      "/a.godot": { shut: ["X"], seen: 500 },
      "/b.godot": { shut: ["X"], seen: 100 },
      "/c.godot": { shut: ["X"], seen: 900 },
      "/d.godot": { shut: ["X"], seen: 300 },
      "/e.godot": { shut: ["X"], seen: 700 },
    },
  };

  const kept = trim(store, 3);

  assert.deepEqual(Object.keys(kept.shows).sort(), ["/a.godot", "/c.godot", "/e.godot"]);

  /*  A TRIM IS OF THE SHOWS AND OF NOTHING ELSE. The panel is a workspace
      preference, not a show's, and a reader who opens a thirteenth show has not
      asked for their inspector to jump back to the side. */
  assert.equal(kept.v, 1);
  assert.deepEqual(kept.panel, { details: false, layout: "side" });

  /*  Fewer shows than the limit: nothing goes. */
  assert.equal(Object.keys(trim(kept, 12).shows).length, 3);

  /*  AND THE LIMIT IS A DOZEN WHEN NOBODY SAYS, which is what keeps a machine
      that has opened a hundred shows from carrying a hundred fold lists for
      ever. */
  const many = { v: 1, panel: { details: false, layout: "side" }, shows: {} };

  for (let n = 0; n < 20; n += 1) many.shows["/show" + n + ".godot"] = { shut: [], seen: 1000 + n };

  const dozen = trim(many);

  assert.equal(Object.keys(dozen.shows).length, 12);
  assert.equal("/show19.godot" in dozen.shows, true, "the show opened last is kept");
  assert.equal("/show0.godot" in dozen.shows, false, "the show opened first is not");
});

test("what is written is one key, in the shape a later version has to read", () => {
  adopt("/shows/one.godot");
  toggle("B3N8:header");

  const raw = written();

  assert.equal(typeof raw, "string", "the page's memory lives under " + KEY);

  const store = JSON.parse(raw);

  assert.equal(store.v, 1);
  assert.deepEqual(store.shows["/shows/one.godot"].shut, ["B3N8:header"]);

  /*  `seen` is what trimming sorts on, so it has to be a number and not a
      date written out as text. */
  assert.equal(typeof store.shows["/shows/one.godot"].seen, "number");
  assert.equal(store.shows["/shows/one.godot"].seen > 0, true);

  /*  The panel is beside the shows and not inside one. */
  assert.equal(store.panel.details, false);
  assert.equal(store.panel.layout, "side");
});

test("what is shut in one show is not shut in the next, and comes back with it", () => {
  /*  Why the fold is kept per show (remember.js, WHAT IS PER SHOW AND WHAT IS
      NOT): an identifier means nothing in
      another document, so carrying B3N8 across would fold whatever happened to
      be called that - or nothing at all, which is worse, because it would look
      like the page had forgotten. */
  adopt("/shows/one.godot");
  toggle("B3N8");
  toggle("B3N8:header");

  adopt("/shows/two.godot");
  assert.deepEqual([...folded], [], "a show opened for the first time is flat");

  toggle("Q7");

  adopt("/shows/one.godot");
  assert.deepEqual([...folded].sort(), ["B3N8", "B3N8:header"]);

  adopt("/shows/two.godot");
  assert.deepEqual([...folded], ["Q7"]);
});

test("where the inspector sits follows the reader from one show to the next", () => {
  adopt("/shows/one.godot");

  panel.layout = "foot";
  panel.details = true;
  save();

  /*  As a reload leaves them: the module's defaults, before anything is read
      back. */
  panel.layout = "side";
  panel.details = false;

  adopt("/shows/two.godot");

  /*  ONTO THE SAME OBJECT, not a fresh one: every importer holds this binding
      - which is what the object is for - so an `adopt` that
      replaced `panel` would leave five modules reading the old one. */
  assert.equal(panel.layout, "foot");
  assert.equal(panel.details, true);
});

test("a machine that has opened many shows does not carry all of them for ever", () => {
  /*  The wiring rather than the helper: something has to call `trim` on the way
      to the disk, or the store grows by one fold list per show opened and never
      shrinks. Only the count is asserted - two shows opened in the same
      millisecond have the same `seen`, and which of those two is kept is not
      worth a rule. */
  for (let n = 0; n < 15; n += 1) {
    adopt("/shows/" + n + ".godot");
    toggle("G" + n);
  }

  const store = JSON.parse(written());

  assert.equal(Object.keys(store.shows).length <= 12, true,
               "a dozen shows at most are remembered, and " +
               Object.keys(store.shows).length + " were written");
  assert.equal(Object.keys(store.shows).length >= 1, true);
});

test("a browser that will not let the page read leaves it working on the defaults", () => {
  stand(fakeStorage({ read: true }));

  /*  Something is in memory from the show before, as there would be. */
  folded.add("STALE");
  panel.layout = "foot";

  adopt("/shows/one.godot");

  assert.deepEqual([...folded], [], "the fold is the new show's, which is nothing");
  assert.equal(panel.layout, "side");
  assert.equal(panel.details, false);
});

test("a browser that will not let the page write still folds the group", () => {
  stand(fakeStorage({ write: true }));

  /*  THE RULE THIS FILE EXISTS FOR. A quota refusal, or a private window, must
      cost the reader the memory and nothing else: the twist still turns, the
      group still shuts, and it is only the next reload that has forgotten. */
  toggle("B3N8");
  assert.equal(folded.has("B3N8"), true);

  toggle("B3N8");
  assert.equal(folded.has("B3N8"), false);

  /*  And the other two ways out to the disk are survived as well: a `save` that
      is refused on its own, and an `adopt`, which reads and then writes. */
  save();
  adopt("/shows/one.godot");
});

test("a window with no site data at all is a window where everything still works", () => {
  delete globalThis.localStorage;

  adopt("/shows/one.godot");
  toggle("B3N8");
  save();

  assert.equal(folded.has("B3N8"), true);
});

test("a localStorage whose very name throws is caught where it throws", () => {
  /*  A browser set to block site data does not hand back a storage that
      refuses: reading the PROPERTY throws. That is why every access goes
      through `globalThis.localStorage` at the moment of use rather than
      through a reference caught at import - a caught reference would still
      point at the working storage this test writes B3N8 into, and would hand
      it back afterwards instead of falling back to nothing. */
  adopt("/shows/one.godot");
  toggle("B3N8");
  assert.deepEqual([...folded], ["B3N8"]);

  Object.defineProperty(globalThis, "localStorage", {
    configurable: true,
    get() { throw new Error("site data is blocked for this origin"); },
  });

  folded.add("STALE");

  adopt("/shows/one.godot");

  assert.deepEqual([...folded], [],
                   "the read threw, so the page starts this show from nothing");

  /*  And a write after that is survived too. */
  toggle("B3N8");
  assert.equal(folded.has("B3N8"), true);
});
