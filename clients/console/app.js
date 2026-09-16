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

import { tree } from "./plumbing/tree.js";
import { servedByEngine, openSocket } from "./plumbing/link.js";
import { poll } from "./plumbing/poll.js";
import "./model/index.js";
import { selection } from "./model/selection.js";
import { adopt } from "./model/remember.js";
import { view } from "./views/view.js";
import { renderStrip } from "./views/strip.js";
import { renderLists } from "./views/didi.js";
import { renderAim } from "./views/aim.js";
import { renderRuns } from "./views/gogo.js";
import { renderInspector } from "./views/inspector.js";
import "./views/transport.js";
import "./gestures/clicks.js";
import "./gestures/fields.js";
import "./gestures/drag.js";
import "./gestures/keys.js";
import { table, loadTable } from "./gestures/table.js";

/*  A CLIENT FOR THE ENGINE, over the OSCQuery surface it already publishes: it
    reads by polling `GET /godot` and writes by sending binary OSC on the
    WebSocket that answers on the same port. Those are the two halves OSCQuery
    has, used the way it means them.

    IT IS SERVED BY THE ENGINE ITSELF, from `/ui`, so this is a same-origin page
    and there is no CORS anywhere. That also means the address in the browser's
    bar is the address of the engine, which is what makes it reachable from a
    tablet on the same network without anybody configuring anything.

    EVERY EDIT IS A NAMED COMMAND (§4.11). Nothing here reaches into the
    document: a value becomes `node.set <address> <value>`, and a structural
    change becomes `cue.create`, `object.delete`, `group.role` or `standby.set`.
    They arrive at the engine exactly as they would from a hardware surface or a
    script, they are logged, and a replay reproduces them - which is why there
    is no second vocabulary for "the UI did it".

    POLLED FOR READING, for now. The server does push coalesced OSC over this
    same socket to whoever sends LISTEN, and that is the right answer for a
    surface somebody is operating from. It is not the right answer for a view
    whose job is still to be looked at and argued with: a poll of the whole tree
    has no decode step to be wrong, and at ten a second at most it is
    imperceptible against a fifty-hertz engine. */

/*  AS MODULES, WITH NO BUILD STEP (§14.3, decision V): this file and the ones
    it imports are served from clients/console exactly as they sit on disk, so
    a view is changed by changing its file and refreshing the tab - during a
    show that is already running, with the engine untouched and the runs still
    playing. `index.html` is the shell; `styles.css` its look; `plumbing/` the
    socket, the poll and the tree; `model/` the indexes, the selection and what
    this reader is remembered to be looking at; `views/` one file per pane;
    `gestures/` the clicks, the fields, the keys, and `commands.json`, the table
    that says which named command each gesture sends - read by this page, and by
    the desktop client when it comes (§14.16), so that the two cannot drift on
    the names. */

let lastShow = null;               // whose identifiers the selection means

/*  EVERY PART IS CALLED THROUGH `view`, and the page depends on that: see
    views/view.js. `triggersOf` and `overlaps` stay methods on `tree`, called
    as `tree.triggersOf` and `tree.overlaps`, for the same reason. */
function render() {
  const show = tree.get("/godot/document/path", "");

  /*  Folding and the selection are the reader's, and they are kept across polls
      - but not across a different show, where the identifiers mean nothing.

      `adopt` does both halves of that for the folds: it drops the ones that
      belonged to the show being left, and loads whatever this reader last had
      shut in the one being opened (model/remember.js). What is picked and where
      the page was last sent are dropped and not reloaded - they are questions
      somebody was asking about the old document. */
  if (show !== lastShow) {
    adopt(show);
    selection.picked = null;
    selection.reveal = null;
    lastShow = show;
  }

  view.renderStrip();
  view.renderLists();
  view.renderAim();
  view.renderRuns();
  view.renderInspector();
}

view.render = render;
view.renderStrip = renderStrip;
view.renderLists = renderLists;
view.renderAim = renderAim;
view.renderRuns = renderRuns;
view.renderInspector = renderInspector;

/*  What the M24 instrument and a person at the browser's console reach the
    page by: the tree as last read, the door every render goes through, and
    what the reader has picked. Nothing in the page reads it. */
window.goDot = { tree, view, selection };

/*  THE TABLE, THE SOCKET AND THE POLL, all at once. A gesture pressed before
    the table has arrived - a few milliseconds from the same engine, and in
    practice before the first tree is drawn - sends nothing, which is what a
    gesture on an empty page should do anyway. A table that will not load at
    all - a mistake typed into it, most likely, on a page with no build step -
    is said on the link on every poll ("reading only", and why), because the
    page still reads and draws and its gestures send nothing (plumbing/poll.js). */
if (servedByEngine) {
  loadTable().catch((problem) => {
    table.problem = "the gesture table did not load (" + problem.message + ")";
  });

  openSocket();
  poll();                          // and every poll asks for the next one
}
