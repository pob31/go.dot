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

/*  WHICH LIST GO ACTS ON, AND WHICH ONE THIS PAGE SHOWS (namespace draft §74,
    AQR, AQS). A list is a show list or a cart; GO never meets a cart, so the
    list GO acts on is the engine's focus when that is a show list and the
    first show list otherwise - the engine's own fallback. The page shows that
    list unless a cart's tab was pressed: a cart is shown and GO's list stays
    where it was. Kept here, the reader's and not the show's, beside the tabs
    that change it. */

import { tree } from "../plumbing/tree.js";

let shown = "";

export function isCart(id) {
  return tree.get("/godot/list/" + id + "/kind", "show") === "cart";
}

export function goList() {
  const lists = tree.ids("/godot/list/order");
  const focus = tree.get("/godot/list/focus", "");

  if (focus && lists.indexOf(focus) >= 0 && !isCart(focus)) return focus;

  return lists.find((id) => !isCart(id)) || "";
}

/** The list the cue pane draws: a cart a tab picked, while the show has it, else GO's. */
export function shownList() {
  return shown && tree.ids("/godot/list/order").indexOf(shown) >= 0 ? shown : goList();
}

/*  A TAB PRESSED: a cart is shown, and GO's list is left alone; a show list
    is followed, and the caller moves GO's focus there. Answers whether the
    press is a cart's. */
export function showList(id) {
  if (isCart(id)) {
    shown = id;
    return true;
  }

  shown = "";
  return false;
}

/*  THE MEMBERS OF A CONTAINER, asked of whichever of the two publishes them:
    a list's at `/godot/list/<id>/order`, a group's at `/godot/cue/<id>/order`
    - the only honest difference between the two identifiers is which address
    answers. `from` is the tree, or the picture of it a drag holds still
    (gestures/drag.js); a cue with no members answers none. */
export function membersOf(parent, from = tree) {
  const asList = from.node("/godot/list/" + parent + "/order");

  return from.ids((asList ? "/godot/list/" : "/godot/cue/") + parent + "/order");
}
