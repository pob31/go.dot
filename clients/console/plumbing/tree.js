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

/*  THE READ HALF'S MODEL: the flattened reply, and the questions every view
    asks of it. The indexes built once per reply - which cue a trigger belongs
    to, which cues share a slot - are model/index.js's, added to this object
    there. */

/*  The OSCQuery reply is a tree of CONTENTS; everything below wants a flat map
    of address to NODE - the whole node, not just its value, because the
    inspector is built out of what each node says about itself: its TYPE, its
    ACCESS, its RANGE and its DESCRIPTION. That is what keeps this page from
    holding a second copy of the parameter table: a row added to the CSV shows
    up here without a line being written. */
function flatten(node, into) {
  if (!node || typeof node !== "object") return into;

  if (typeof node.FULL_PATH === "string") into[node.FULL_PATH] = node;

  if (node.CONTENTS) {
    for (const key of Object.keys(node.CONTENTS)) flatten(node.CONTENTS[key], into);
  }

  return into;
}

const tree = {
  at: {},
  node(address) { return this.at[address]; },
  get(address, fallback) {
    const node = this.at[address];
    const value = node && Array.isArray(node.VALUE) ? node.VALUE[0] : undefined;
    return value === undefined || value === null ? fallback : value;
  },
  /** A space-separated identifier list, as every `order` node spells one. */
  ids(address) {
    const text = this.get(address, "");
    return typeof text === "string" && text.length ? text.trim().split(/\s+/) : [];
  },
  cue(id, name, fallback) { return this.get("/godot/cue/" + id + "/" + name, fallback); },
  trigger(id, name, fallback) {
    return this.get("/godot/trigger/" + id + "/" + name, fallback);
  },

  run(id, name, fallback) { return this.get("/godot/run/" + id + "/" + name, fallback); },
  slot(id, name, fallback) { return this.get("/godot/slot/" + id + "/" + name, fallback); },

};

export { flatten, tree };
