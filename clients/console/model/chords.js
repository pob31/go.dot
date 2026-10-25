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

/*  A KEY, SPELLED AS KEY TRIGGERS SPELL ONE (namespace draft §74, AQK, AQW):
    the page's half of engine/cue/KeyChord.h and the window's KeyNames.h, so a
    key chosen in one client is the key pressed in the other. The modifiers in
    their order - Mod (Ctrl or ⌘), Alt, Shift - then the key: a letter by the
    letter it types, a top-row digit by its place, `Num1` on the number pad,
    F1 to F24, and the named keys below. Pure, so a test reads it with no page. */

const NAMED = new Map([
  ["ArrowLeft", "ArrowLeft"], ["ArrowRight", "ArrowRight"], ["ArrowUp", "ArrowUp"], ["ArrowDown", "ArrowDown"],
  ["Enter", "Enter"], ["NumpadEnter", "Enter"], ["Tab", "Tab"], ["Space", "Space"], ["Escape", "Escape"],
  ["Delete", "Delete"], ["Backspace", "Backspace"], ["Home", "Home"], ["End", "End"],
  ["PageUp", "PageUp"], ["PageDown", "PageDown"], ["Insert", "Insert"],
]);

/*  EVERY SHORTCUT EITHER CLIENT ANSWERS TO, the engine's list word for word
    (KeyChord.cpp) - and GO's Space, Esc, Doh!'s F9 and Pause / Play's F8
    whatever the modifiers. */
const SHORTCUTS = new Set([
  "F5", "F", "Shift+F", "ArrowUp", "ArrowDown", "Mod+ArrowUp", "Mod+ArrowDown",
  "Enter", "Tab", "Shift+Tab", "Delete", "Backspace", "Mod+Backspace",
  "Mod+A", "Mod+C", "Mod+L", "Mod+N", "Mod+O", "Mod+S", "Mod+Shift+S", "Mod+T",
  "Mod+V", "Mod+W", "Mod+X", "Mod+Y", "Mod+Z", "Mod+Shift+Z", "Mod+Shift+U", "Mod+Shift+R",
]);

/*  The key a browser's event names, in this spelling, or "" for one no
    spelling has. ON `code` FOR WHERE A KEY IS and on `key` for what a letter
    types, as gestures/keys.js already reads them: a digit is its place, so
    Shift+1 is the same key on every layout; a letter is the letter, so the
    key engraved A on an AZERTY keyboard is A. */
function keyName(event) {
  const code = String(event.code || "");
  const key = String(event.key || "");

  if (/^Key[A-Z]$/.test(code)) return key.length === 1 && /[a-z]/i.test(key) ? key.toUpperCase() : code.slice(3);
  if (/^Digit[0-9]$/.test(code)) return code.slice(5);
  if (/^Numpad[0-9]$/.test(code)) return "Num" + code.slice(6);
  if (/^F([1-9]|1[0-9]|2[0-4])$/.test(code)) return code;

  return NAMED.get(code) || "";
}

/** The chord a key event is, or "" when its key is none this spelling has. */
export function chordOf(event) {
  const name = keyName(event);

  if (!name) return "";

  return (event.ctrlKey || event.metaKey ? "Mod+" : "") +
         (event.altKey ? "Alt+" : "") +
         (event.shiftKey ? "Shift+" : "") + name;
}

/** Whether a chord, spelled as above, is one the show keeps. */
export function isReserved(chord) {
  const key = chord.slice(chord.lastIndexOf("+") + 1);

  return key === "Space" || key === "Escape" || key === "F9" || key === "F8" || SHORTCUTS.has(chord);
}

/*  THE TRIGGERS A CHORD FIRES, from a reply's addresses: every key trigger
    switched on with that key. Read off `/godot/trigger/<id>/kind`, as
    model/index.js reads a cue's triggers, and asked by the caller once a
    reply - a key press is rare, a reply is not. */
export function keyTriggersIn(at) {
  const index = new Map();

  for (const address of Object.keys(at)) {
    const match = /^\/godot\/trigger\/([0-9A-Z]+)\/kind$/.exec(address);

    if (!match) continue;

    //  As `tree.get` reads a node: its first value, or the fallback.
    const read = (row, fallback) => {
      const node = at["/godot/trigger/" + match[1] + "/" + row];
      const value = node && Array.isArray(node.VALUE) ? node.VALUE[0] : undefined;

      return value === undefined ? fallback : value;
    };

    if (read("kind", "") !== "key") continue;

    const enabled = read("enabled", true);

    if (enabled === false || enabled === "false") continue;

    const chord = String(read("key", ""));

    if (!chord || isReserved(chord)) continue;

    if (!index.has(chord)) index.set(chord, []);
    index.get(chord).push(match[1]);
  }

  return index;
}
