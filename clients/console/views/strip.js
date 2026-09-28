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

/*  THE STRIP and the transport's readouts: what the show is, whether it is on
    disk, the lock, the undo stack's words, and the engine's own numbers. */

import { tree } from "../plumbing/tree.js";
import { panel } from "../model/remember.js";
import { selection } from "../model/selection.js";
import { el, esc } from "./common.js";

function renderStrip() {
  const show = tree.get("/godot/document/name", "");
  el("s-show").textContent = show || "—";

  /*  WHETHER THE FILE IS BEHIND THE SHOW, which the engine has published since
      Phase 1 and never once said yes to until PR 5.2. It counts the show half
      only: a GO or a standby move is the operator's position, not the show, so
      it does not light this - an operator told "unsaved" after every GO would
      stop reading it by the second act. Three answers, because a node that is
      not there is not the same as a show that is saved. */
  const dirty = tree.get("/godot/document/dirty", null);
  const isDirty = dirty === true || dirty === "true";
  const isSaved = dirty === false || dirty === "false";

  /*  AND WHETHER THERE IS WORK THE FILE DOES NOT HOLD, found rather than made.
      `recovery` is true when the engine opened this bundle beside unsaved
      work that nobody has answered yet - a `recovery/` folder, or failing
      that the newest `recovery.previous.N/` - and it stays true while this
      session autosaves, since autosave never writes over that work: if it is
      in `recovery/`, the first autosave moves it aside. It joins the file's
      readout rather than taking a stat of its own because it is the same
      question - is everything worth keeping on disk as the show? - to which
      "saved" alone would be the wrong answer: true of show.xml, and silent
      about the afternoon beside it. Amber for either, and the words say which
      (§4.8). The banner under the transport is where it is answered; this is
      the second telling, for a tablet that has scrolled past the first. */
  const recovery = tree.get("/godot/document/recovery", null);
  const hasRecovery = recovery === true || recovery === "true";

  const file = [isDirty ? "unsaved changes" : isSaved ? "saved" : "—"];

  if (hasRecovery) file.push("recovery waiting");

  el("s-saved").textContent = file.join(" · ");
  el("s-saved-wrap").className = isDirty || hasRecovery ? "stat dirty" : "stat";
  el("s-saved-wrap").title =
    (isDirty ? "the show has changed since show.xml was last written"
             : isSaved ? "show.xml holds this show"
                       : "the engine has not said") +
    (hasRecovery ? "; and unsaved work from a session that did not end is waiting in " +
                   "the bundle, to be recovered or discarded" : "");

  /*  A WRITE THAT FAILED, said in words. Since PR 5.5's second half a save,
      an autosave and a saveAs are applied at once and written on the engine's
      writer thread a tick or two later, so a write that fails there fails
      AFTER its command was taken: nothing is refused, and `lastError` and the
      refusal banner rightly say nothing, since those quote refused records and
      this one was applied. Without this a failed save would leave one sign,
      the readout above staying at "unsaved changes" - which is also what
      honest unsaved work looks like - and a failed autosave none at all.

      So `/godot/document/writeError` gets a stat of its own after "file", in
      the failure red rather than the amber of unsaved work, because this one
      says something has gone wrong, which amber never does. Hidden while it
      is empty, since a readout that always says "no problem" is one people
      stop reading. Cut to fit the strip, with the engine's whole sentence -
      which command, at which tick, which file - on hover. */
  const writeError = String(tree.get("/godot/document/writeError", "") || "");
  const WRITE_ERROR_ROOM = 60;

  el("s-write").textContent = writeError.length > WRITE_ERROR_ROOM
    ? writeError.slice(0, WRITE_ERROR_ROOM - 1) + "…"
    : writeError;
  el("s-write-wrap").title = writeError;
  el("s-write-wrap").hidden = writeError === "";

  el("recovery").hidden = !hasRecovery;

  /*  SHOW MODE (decision W), which the ENGINE keeps: while it is on, every
      edit to the show from any client is refused and says `locked`, and GO,
      the standby, the runs and a write to a mounted device all carry on. This
      page reads it and writes it and decides nothing about it - no button is
      hidden here because of it, since the engine refusing is the lock and a
      page declining to send would only be one client's opinion. Three
      answers, as the file's readout has, because an engine that does not publish the
      node is not the same as a show that is open. */
  const lockValue = tree.get("/godot/document/locked", null);
  const isLocked = lockValue === true || lockValue === "true";
  const isOpen = lockValue === false || lockValue === "false";

  el("s-lock").textContent = isLocked ? "locked" : isOpen ? "open" : "—";
  el("s-lock-wrap").className = isLocked ? "stat locked" : "stat";
  el("s-lock-wrap").title = isLocked
    ? "show mode: the engine refuses every edit to the show, from every client; " +
      "GO, standby, runs and mounted devices carry on"
    : isOpen ? "the show can be edited" : "the engine has not said";

  const lockButton = el("lock");
  lockButton.textContent = isLocked ? "unlock the show" : "lock the show";
  lockButton.setAttribute("aria-pressed", isLocked ? "true" : "false");
  lockButton.disabled = !isLocked && !isOpen;

  /*  SHOW MODE DOES NOT OFFER A SAVE (namespace draft §9, decision W, reaffirmed
      2026-09-17). The ENGINE keeps saving under the lock - lock first so nothing
      moves, then save, so the file on disk is the final show - and it is the
      client that goes quiet, because "usually we try not to save a show mid
      performance". Disabled rather than hidden, so the reader sees the button
      exists and reads why it will not press; ctrl/⌘-S follows it
      (gestures/keys.js). A script can still save: this is one client's manners,
      not a rule, and the rule lives where every client meets it. */
  const saveButton = el("save");
  saveButton.disabled = isLocked;
  saveButton.title = isLocked
    ? "show mode: saving is not offered while the show is locked - unlock to save"
    : "writes the show into its bundle — ctrl/⌘-S";

  /*  AND WHERE THE INSPECTOR SITS, in the same shape as the lock: the button
      says what it would DO, not what is true, so nobody has to work out which
      of two states they are looking at from a word that could be either. The
      panes themselves are arranged by the stylesheet off `data-layout`. */
  const atFoot = panel.layout === "foot";
  const panes = el("panes");

  panes.dataset.layout = panel.layout;

  /*  AND WHETHER THERE IS ANYTHING TO INSPECT, which is what decides whether
      the middle pane is there at all. Read from the page's own selection and
      never told to the engine (§14.1). */
  panes.dataset.picked = selection.picked ? "yes" : "no";

  el("layout").textContent = atFoot ? "inspector in the middle" : "inspector at the foot";
  el("layout").title = atFoot
    ? "put it back between the cue list and the running pane, where it appears"
      + " when a cue is picked and stands down when none is"
    : "put it across the foot, under both panes - the width a waveform or a"
      + " curve wants, and the shape the editor panel will take";

  /*  WHAT UNDO WOULD UNMAKE, AND IN WHOSE WORDS.

      The engine names each transaction after the command that opened it and
      publishes that name, so `undoName` already reads `node.set`,
      `cue.create` or `object.delete` - the words §4.11 makes every action
      carry. This page prints the word it is given and holds no table of its
      own, which is what stops the readout going stale the day a command is
      added: a lookup table here would have to be edited by somebody who never
      opened this file.

      SAID, NOT ONLY GREYED (§4.8). A disabled button reports that something is
      unavailable and never which something, so the sentence beside it carries
      the meaning - "nothing to undo" against "undo: object.delete" - and the
      greying is the second telling. Three answers again, as the file's readout
      and the lock's have, because an engine that has not published the node is
      not the same thing as a stack with nothing on it. */
  const canUndo = tree.get("/godot/document/canUndo", null);
  const canRedo = tree.get("/godot/document/canRedo", null);
  const undoName = String(tree.get("/godot/document/undoName", "") || "");
  const redoName = String(tree.get("/godot/document/redoName", "") || "");

  const undoable = canUndo === true || canUndo === "true";
  const redoable = canRedo === true || canRedo === "true";

  el("undo").disabled = !undoable;
  el("redo").disabled = !redoable;

  el("undo").title = undoable
    ? "takes back " + (undoName || "the last edit") + " — ctrl/⌘-Z"
    : "nothing has been edited that could be taken back";
  el("redo").title = redoable
    ? "puts back " + (redoName || "the last undo") + " — ctrl/⌘-shift-Z"
    : "nothing has been undone";

  const stack = [];

  if (canUndo === null) stack.push("—");
  else stack.push(undoable ? "undo: " + (undoName || "the last edit") : "nothing to undo");

  if (redoable) stack.push("redo: " + (redoName || "the last undo"));

  el("undo-note").textContent = stack.join(" · ");

  /*  WHAT A LOCKED SHOW IS RIDING LIVE (2026-09-25): said while locked, and
      offered to keep or discard once it is not - nothing can be kept while
      the show refuses edits. */
  const live = Number(tree.get("/godot/document/live", 0)) || 0;
  const lockedNow = tree.get("/godot/document/locked", null);
  const isLocked = lockedNow === true || lockedNow === "true";

  el("live-note").hidden = live === 0;
  el("live-note").textContent = live === 0 ? ""
    : live + (live === 1 ? " change" : " changes") + " riding live, not saved";
  el("live-keep").hidden = live === 0 || isLocked;
  el("live-drop").hidden = live === 0 || isLocked;

  el("s-tick").textContent = tree.get("/godot/engine/tick", "—");
  el("s-revision").textContent = tree.get("/godot/document/revision", "—");
  el("s-audio").textContent = tree.get("/godot/audio/status", "—");

  const rate = tree.get("/godot/engine/sampleRate", 0);
  const block = tree.get("/godot/engine/blockSize", 0);
  el("s-rate").textContent = rate ? rate + " / " + block : "—";

  const late = Number(tree.get("/godot/engine/latenessMax", 0));
  el("s-late").textContent = Number.isFinite(late) && rate
    ? (late / rate * 1000).toFixed(1) + " ms"
    : "—";

  const launch = Number(tree.get("/godot/engine/launchLatencyTicks", 0));
  const gap = Number(tree.get("/godot/engine/sequenceGapTicks", 0));
  el("s-latency").textContent = launch
    ? (launch * 20) + " ms" + (gap ? " · chain " + (gap * 20) + " ms" : "")
    : "—";

  /*  The one number on this strip that is a claim rather than a reading: PRD
      §4.2 says nothing of Go.dot's may allocate on the audio thread, so any
      value but zero is a broken promise and is shown as one. */
  const rt = Number(tree.get("/godot/engine/rtViolations", 0));
  el("s-rt").textContent = Number.isFinite(rt) ? String(rt) : "—";
  el("s-rt-wrap").className = rt > 0 ? "stat warn" : "stat";

  /*  EVERYTHING WRONG WITH THE SHOW THAT DID NOT STOP IT OPENING: the
      references that point at nothing, and the slot overlaps the edit-time
      analysis found. One per LINE, which no other list-shaped readout here is,
      because a warning is a sentence and sentences contain spaces.

      A count on the strip and the sentences in the tooltip, because the strip
      is glanceable and a warning is not - what an operator needs from here is
      whether there is anything at all. */
  const warningText = String(tree.get("/godot/document/warnings", ""));
  const warningLines = warningText.length ? warningText.split("\n") : [];

  el("s-warn").textContent = String(warningLines.length);
  el("s-warn-wrap").className = warningLines.length ? "stat warn" : "stat";
  el("s-warn-wrap").title = warningLines.length ? warningLines.join("\n")
                                                : "nothing to report";

  /*  A LIST WITH NO POINTER, said where the buttons that do nothing are. */
  const focused = tree.get("/godot/list/focus", "") || tree.ids("/godot/list/order")[0] || "";
  const parked = focused ? tree.get("/godot/list/" + focused + "/standby", "") : "";
  const note = el("clear-note");

  note.hidden = !focused || !!parked;
  note.textContent = note.hidden ? "" : "standby is clear — click a row's left edge to park it";

  const refusal = tree.get("/godot/engine/lastError", "");
  const banner = el("refusal");
  banner.className = refusal ? "show" : "";
  banner.innerHTML = refusal ? "refused: <code>" + esc(refusal) + "</code>" : "";
}

export { renderStrip };
