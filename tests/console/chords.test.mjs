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

/*  THE PAGE'S SPELLING OF A KEY (namespace draft §74, AQW): the window's and
    the engine's, so a key chosen in one client fires in the other - and the
    show's own keys never a trigger's. */

import { test } from "node:test";
import assert from "node:assert/strict";

import { chordOf, isReserved, keyTriggersIn } from "../../clients/console/model/chords.js";

const event = (code, key, mods = {}) => ({ code, key, ctrlKey: false, metaKey: false, altKey: false, shiftKey: false, ...mods });

test("a key event in the trigger spelling: modifiers in order, then the key", () => {
  assert.equal(chordOf(event("F6", "F6")), "F6");
  assert.equal(chordOf(event("KeyK", "k", { ctrlKey: true, shiftKey: true })), "Mod+Shift+K");
  assert.equal(chordOf(event("KeyK", "k", { metaKey: true })), "Mod+K");
  assert.equal(chordOf(event("Digit1", "!", { shiftKey: true })), "Shift+1");
  assert.equal(chordOf(event("Numpad3", "3")), "Num3");
  assert.equal(chordOf(event("ArrowLeft", "ArrowLeft", { altKey: true })), "Alt+ArrowLeft");
  assert.equal(chordOf(event("NumpadEnter", "Enter")), "Enter");

  //  The key engraved A on an AZERTY keyboard sits where Q is: it is A.
  assert.equal(chordOf(event("KeyQ", "a")), "A");

  //  A key no spelling has.
  assert.equal(chordOf(event("Comma", ",")), "");
  assert.equal(chordOf(event("F25", "F25")), "");
});

test("the show's own keys: GO, Esc and Doh! whatever the modifiers, and every shortcut", () => {
  for (const kept of ["Space", "Mod+Space", "Shift+Escape", "F9", "Alt+F9", "F5", "F", "Shift+F",
                      "ArrowUp", "Mod+Z", "Mod+Shift+Z", "Mod+S", "Delete"]) {
    assert.ok(isReserved(kept), kept);
  }

  for (const free of ["F6", "Mod+K", "Shift+1", "ArrowLeft", "Num1", "G", "Alt+F"]) {
    assert.ok(!isReserved(free), free);
  }
});

test("the show's key triggers by their key: switched on, and never a key the show keeps", () => {
  const node = (value) => ({ VALUE: [value] });

  const at = {
    "/godot/trigger/KEYTRG01/kind": node("key"),
    "/godot/trigger/KEYTRG01/key": node("F6"),
    "/godot/trigger/KEYTRG02/kind": node("key"),
    "/godot/trigger/KEYTRG02/key": node("F6"),
    "/godot/trigger/KEYTRG03/kind": node("key"),
    "/godot/trigger/KEYTRG03/key": node("F7"),
    "/godot/trigger/KEYTRG03/enabled": node(false),
    "/godot/trigger/SCTRG004/kind": node("osc"),
    "/godot/trigger/SCTRG004/key": node("F6"),
    "/godot/trigger/KEYTRG05/kind": node("key"),
    "/godot/trigger/KEYTRG05/key": node("Space"),
  };

  const index = keyTriggersIn(at);

  assert.deepEqual(index.get("F6"), ["KEYTRG01", "KEYTRG02"]);
  assert.equal(index.get("F7"), undefined);
  assert.equal(index.get("Space"), undefined);
});
