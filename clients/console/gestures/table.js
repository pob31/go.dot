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

/*  THE GESTURE TABLE, READ FROM ITS FILE: which named command each key and
    each gesture sends (§4.11, namespace draft §14.3).

    DATA AND NOT CODE, because two clients must agree on it. The page reads
    `commands.json` when it starts; the desktop client will read the same file
    rather than keep a copy of it (§14.16); and tests/blackbox/client_page.py
    checks every name in it against `wfg commands`, so a gesture naming a
    command that does not exist fails a test rather than an evening. Nothing
    in the page spells a command's name for a key or a click: it asks here.

    A button's command is the exception, and deliberately: a transport button
    names its own in `data-cmd`, where the markup already said it, and the
    table lists those names only so that the same check covers them.

    Fetched against THIS MODULE's address, not the page's: the page is served
    at `/ui` and at `/ui/` alike, and a path relative to the page would find
    the file under only one of the two. */

import { command } from "../plumbing/link.js";

/*  Empty until it has loaded, and filled in place, so a module that holds it
    holds the loaded table from then on. `problem` says why it did not load,
    and is empty while nothing is wrong (app.js sets it; the poll says it). */
const table = { keys: {}, gestures: {}, buttons: [], problem: "" };

async function loadTable() {
  const reply = await fetch(new URL("./commands.json", import.meta.url), { cache: "no-store" });

  if (!reply.ok) throw new Error("commands.json answered " + reply.status);

  const loaded = await reply.json();

  table.keys = loaded.keys || {};
  table.gestures = loaded.gestures || {};
  table.buttons = loaded.buttons || [];
}

/*  The command a gesture's name stands for, sent with `args`. False, and
    nothing sent, for a gesture the table does not name - which is also what
    happens before the table has loaded. */
function gesture(name, args) {
  const entry = table.gestures[name];
  return entry ? command(entry.command, args || []) : false;
}

/*  The same, for a key: "Space", "ArrowDown", "Mod+Z". Every key the table
    names sends its command with no arguments. */
function press(key) {
  const entry = table.keys[key];
  return entry ? command(entry.command, []) : false;
}

export { table, loadTable, gesture, press };
