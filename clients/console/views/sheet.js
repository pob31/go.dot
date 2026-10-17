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

/*  A SHEET (namespace draft §75, WT.4): what a tap on a run or a long press on
    a cue offers, as the app's sheets offer it - a heading, then each action a
    button a finger wide, the one that cuts last with air before it, then
    Close. Over the page; a tap outside it is Close. Built as it opens, from
    what the tree said then; an action closes it as it is sent.

    NOT ESC'S. Esc is PANIC's first level everywhere on this page (§4.4), so a
    sheet is closed by a tap and never by the key that stops the show. */

import { esc } from "./common.js";

let shown = null;

/*  A TAP'S OWN CLICK IS NOT A CHOICE. iOS sends the click a tap makes to what
    is under the finger after the tap's handlers ran - and the sheet that tap
    opened is now there - so a click within this long of the opening is the
    opening tap's, and closes nothing and sends nothing. */
const GHOST_CLICK_MS = 400;

/*  `actions` are { words, tone, air, run }: `tone` is "stop" or "kill" or
    nothing, `air` puts a row's height of space before it, `run` is called as
    the sheet closes. */
function sheetHtml(title, sub, actions) {
  return '<div class="sheet" role="dialog" aria-label="' + esc(title) + '">' +
    '<div class="head"><span class="title">' + esc(title) + "</span>" +
      (sub ? '<span class="sub">' + esc(sub) + "</span>" : "") + "</div>" +
    actions.map((action, n) =>
      '<button data-sheet="' + n + '"' + (action.tone ? ' data-tone="' + esc(action.tone) + '"' : "") +
      (action.air ? ' data-air="yes"' : "") + ">" + esc(action.words) + "</button>").join("") +
    '<button data-sheet="close" data-tone="close">Close</button>' +
  "</div>";
}

function openSheet({ title, sub = "", actions = [] }) {
  closeSheet();

  const back = document.createElement("div");
  back.id = "sheet";
  back.className = "sheet-back";
  back.innerHTML = sheetHtml(title, sub, actions);
  const openedAt = performance.now();

  back.addEventListener("click", (event) => {
    if (performance.now() - openedAt < GHOST_CLICK_MS) return;

    const which = event.target.closest && event.target.closest("[data-sheet]");

    if (event.target === back || (which && which.dataset.sheet === "close")) {
      closeSheet();
      return;
    }

    if (!which) return;

    const action = actions[Number(which.dataset.sheet)];
    closeSheet();
    if (action && action.run) action.run();
  });

  document.body.appendChild(back);
  shown = back;
}

function closeSheet() {
  if (shown) {
    shown.remove();
    shown = null;
  }
}

function sheetOpen() {
  return shown !== null;
}

export { openSheet, closeSheet, sheetOpen, sheetHtml };
