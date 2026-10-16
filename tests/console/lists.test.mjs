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

/*  THE LIST GO ACTS ON AND THE LIST THE PAGE SHOWS (namespace draft §74, AQR,
    AQS): never a cart for GO, and a cart shown without moving GO's list. */

import { test } from "node:test";
import assert from "node:assert/strict";

import { tree } from "../../clients/console/plumbing/tree.js";
import { goList, isCart, shownList, showList } from "../../clients/console/model/lists.js";

const node = (value) => ({ VALUE: [value] });

test("GO's list is never a cart, and a cart's tab shows it without moving GO's list", () => {
  tree.at = {
    "/godot/list/order": node("SHW00001 CRT00002 SHW00003"),
    "/godot/list/focus": node("CRT00002"),
    "/godot/list/CRT00002/kind": node("cart"),
    "/godot/list/SHW00003/kind": node("show"),
  };

  assert.ok(isCart("CRT00002"));
  assert.ok(!isCart("SHW00001"));

  //  A focus naming a cart falls back to the first show list, as the engine's does.
  assert.equal(goList(), "SHW00001");
  assert.equal(shownList(), "SHW00001");

  //  The cart's tab: shown, and GO's list where it was.
  assert.equal(showList("CRT00002"), true);
  assert.equal(shownList(), "CRT00002");
  assert.equal(goList(), "SHW00001");

  //  A show list's tab: followed, the caller moving GO's focus there.
  assert.equal(showList("SHW00003"), false);
  assert.equal(shownList(), goList());

  //  A cart shown and then taken away: back to GO's list.
  showList("CRT00002");
  tree.at = { ...tree.at, "/godot/list/order": node("SHW00001 SHW00003") };
  assert.equal(shownList(), "SHW00001");

  //  A show of carts alone has no list for GO.
  tree.at = { "/godot/list/order": node("CRT00002"), "/godot/list/CRT00002/kind": node("cart") };
  assert.equal(goList(), "");
});
