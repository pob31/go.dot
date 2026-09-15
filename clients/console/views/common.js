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

/*  The few things every view says the same way: an element by its id, text
    made safe for markup, a duration, and a cue as somebody would say it. */

import { tree } from "../plumbing/tree.js";

const el = (id) => document.getElementById(id);

function esc(text) {
  return String(text).replace(/[&<>"]/g, (c) =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
}

function seconds(value) {
  const n = Number(value);
  return Number.isFinite(n) && n > 0 ? n + "s" : "";
}

/*  A cue as somebody would say it out loud: its number and its name, falling
    back to the identifier for a cue that has neither yet. Used where one row
    has to talk about another. */
function cueName(id) {
  const number = tree.cue(id, "number", "");
  const name = tree.cue(id, "name", "");
  const said = [number, name].filter((part) => part !== "" && part !== undefined).join(" ");
  return said || id;
}

export { el, esc, seconds, cueName };
