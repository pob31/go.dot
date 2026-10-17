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

/*  GOGO, the running pane: every run the engine publishes, as a tree - a
    group's run holding its members'. */

import { tree } from "../plumbing/tree.js";
import { el, esc, shownName } from "./common.js";
import { reconcile } from "./reconcile.js";

/*  HELD STILL WHILE A FINGER SWIPES A RUN (namespace draft §75, WT.4): the pane
    is drawn again at every poll, and a row moved under a finger must not be
    drawn back where it was. Set and cleared by gestures/touch.js. */
const runsHold = { active: false };

/*  WHETHER A RUN'S LAUNCH HAS BEEN PLACED, asked of the one node that knows.

    `run/state` publishes eight words and four of them are a run whose launch
    never was. Three are the run before it: `preparing` is the horizon getting
    it ready ahead of any GO, `waiting` is its pre-wait running, and `armed` is
    its voice held and its media made ready. The fourth is the run that never
    got that far. `failed` was once counted with the launched, on the reading
    that a failure is something that happens to a run in flight, and for a
    media run it never is: every `run.failed` a media run receives is the arm
    refusing it - no track, no file, no route, no slot - or the audio side
    unable to make it ready, and all of those are sent before any launch. The
    command says so in its own description: the run never played. So nothing
    has been played in any of the four, and the position of such a run is
    nought because nothing has moved rather than because the cue is sitting at
    its top - "at 0.0s" on the row of a cue whose whole news is that it could
    not sound. Worse beside it, a colour: the engine reads the frame at that
    nought, which is the file's first, and a failed cue would have worn the
    colour of a sound it never made - or "not analysed yet" of a file the
    bundle does not have, which promises an analysis that will never come.

    Every other word - playing, stopping, postWait, done - is a run whose
    launch was placed, and a finished one keeps the position it stopped at for
    the five seconds it is retained, which is worth reading: it is how far the
    cue got.

    MEDIA ROWS ARE THE ONLY ONES THAT ASK, and that is what makes `failed` an
    honest member of the four: a network or fade run can fail after it has
    begun - a target that never answered, or answered with something else -
    but neither has a playhead, so neither ever reaches this question. */
function launched(state) {
  return state !== "preparing" && state !== "waiting" && state !== "armed"
      && state !== "failed";
}

/*  HOW A RUN SAYS WHAT IT IS DOING (author, 2026-09-16: "'Armed' can be an icon
    or just the yellow mark and 'Playing' can be just the green mark").

    The two states a busy pane is full of are drawn as a mark; every other one
    keeps its word, because the words that are left - waiting, preparing,
    postWait, stopping, failed, done - are the ones an operator has to read
    rather than recognise, and there are never many of them at once.

    A MARK AND NOT A COLOUR (§4.8). The two marks differ in SHAPE before they
    differ in hue - a filled triangle for a cue that is sounding, a ring for one
    that is ready and has not been let go - so a screen that is being
    photographed, a projector that is warm, or an eye that does not sort green
    from amber still tells armed from playing. The word is on the mark as its
    title, the row's left edge carries the same state, and `data-s` is on both,
    which is what the stylesheet colours. */
const MARKS = { playing: "▶", armed: "○" };

function stateMark(state) {
  const mark = MARKS[state];

  if (!mark) return '<span class="state" data-s="' + esc(state) + '">' + esc(state) + "</span>";

  return '<span class="state mark" data-s="' + esc(state) + '" title="' + esc(state) +
         '" role="img" aria-label="' + esc(state) + '">' + mark + "</span>";
}

/*  A RUNNING SCENE'S CLOCK (K9's review, 2026-10-03, namespace draft §23.18):
    the second its seek box means - the scene's own, since it started, across
    its rounds - read off the run's `position`, which is where the desktop
    draws its head. The row said "round 2 of 3" and no second at all, so a
    number typed into the box had nothing on screen to be read against. Empty
    for a scene not playing, and for every other kind: a file's row says its
    own position, against its length. */
function sceneClock(id) {
  if (tree.run(id, "kind", "") !== "group") return "";
  const state = tree.run(id, "state", "armed");
  if (state !== "playing" && state !== "stopping") return "";

  const position = Number(tree.run(id, "position", NaN));
  return Number.isFinite(position) ? "at " + position.toFixed(1) + "s" : "";
}

/*  WHETHER A RUN'S ROW OFFERS THE SEEK BOX: playing, and the engine says a
    seek would move it (`run/seekable`, K9, namespace draft §23.18) - a sound,
    or a scene it can place a second in. */
function offersSeek(id) {
  const engineSays = tree.run(id, "seekable", false);
  const kind = tree.run(id, "kind", "");

  return tree.run(id, "state", "armed") === "playing"
    && (engineSays === true || engineSays === "true")
    && (kind === "media" || kind === "group");
}

/*  THE RUNNING PANE, as a tree: a group's run holds its members' runs, which is
    what `parent` and `children` publish. Present tense - a finished run is kept
    for five seconds and then stops being published at all, which is why nothing
    here has to decide what to forget. */
function runRow(id, depth, out) {
  const state = tree.run(id, "state", "armed");
  const cue = tree.run(id, "cue", "");
  const kind = tree.run(id, "kind", "");
  const name = shownName(cue) || cue || "—";

  const bits = [];
  const track = Number(tree.run(id, "track", -1));
  if (Number.isFinite(track) && track >= 0) bits.push("track " + track);

  const level = Number(tree.run(id, "level", 0));
  if (kind === "media" && Number.isFinite(level)) bits.push(level.toFixed(1) + " dB");

  /*  WHERE WE ARE IN THIS CUE, in seconds and against the file's length when
      there is one, so an operator reads the answer off the row instead of doing
      arithmetic between a start time and a clock.

      The gate here used to be `position > 0`, and that was never a test of
      anything: `run/position` was declared and published from Phase 3 and
      assigned by nothing until PR 5.1, so the number this row read was the
      literal 0 and the comparison was a way of hiding a value nobody had
      written yet. Now that it means something, nought is a legitimate position
      - it is the first tick of a cue, and a run caught at its launch is exactly
      the moment an operator wants the row to say something - so the honest
      question is whether the run has been launched at all, which `state`
      answers and `launched` above asks.

      The length comes from the cue and not from the run because it is a fact
      about the file rather than about this instance, which is why `duration` is
      published as `/godot/cue/<id>/duration` and never copied onto the run.
      Nought there is the engine saying it could not read a length - the file is
      missing, or its format has no reader in this build - so such a cue says
      its elapsed seconds alone rather than claiming to be somewhere of 0.0s.

      In words either way (PRD 4.8): `at` and `of` carry which number is which,
      and nothing here is left to the row's colour to say. */
  const position = Number(tree.run(id, "position", 0));
  const length = Number(tree.cue(cue, "duration", 0));

  /*  MEDIA ONLY, because a launch is only ever PLACED for a media run holding a
      track (`Runner::launchIfDue`). A group, memo, fade, stop or network run
      never has one, so its playhead would be a confident 0.0s for as long as it
      ran - a number that looks like a reading and is the absence of one. */
  if (kind === "media" && launched(state) && Number.isFinite(position)) {
    const timed = Number.isFinite(length) && length > 0;

    bits.push("at " + position.toFixed(1) + "s"
                + (timed ? " of " + length.toFixed(1) + "s" : ""));
  }

  const clock = sceneClock(id);
  if (clock) bits.push(clock);

  /*  AND THE SPEED IT PLAYS AT, when that is not one (namespace draft §22.7):
      the run's own readout, which a fade moves. The position above counts the
      file's seconds, and this is what says they are not the room's. */
  const rate = Math.round(Number(tree.run(id, "rate", 1)) * 1000) / 1000;
  if (kind === "media" && Number.isFinite(rate) && rate !== 1) bits.push("×" + rate);

  /*  AND WHAT IT SOUNDS LIKE THERE: the engine's reading of the frame under the
      playhead (PRD §3.30, namespace draft §14.5), published as "<hue> <sat>
      <light>" - hue in degrees, the other two from nought to one. IN WORDS
      BEFORE ANY COLOUR: the spectral bar is PR 5.17, and PRD 4.8 says colour
      never carries anything alone, so the numbers reach the row first and a
      swatch will join them rather than replace them. Beside the position and
      behind the same gate, because it is a reading AT the position: a run not
      yet launched, or refused before it could be, has a playhead that has not
      moved, and the frame under it is a colour nobody has heard.

      Three answers, and they are not alike. Lightness nought is SILENCE - it
      sits below the ramp's darkest, 0.15, so it cannot be a low sound - and
      the row says so in a word rather than in three noughts. An EMPTY reading
      is the analyser not having reached this file yet, which the bar will draw
      grey. No node at all is an engine older than this page, and a page that
      said "not analysed yet" to it would be saying something untrue; so would
      one that guessed at a reading it cannot parse, which is left out the way
      a level that is not a number is. */
  const timbre = tree.run(id, "timbre", null);

  if (kind === "media" && launched(state) && typeof timbre === "string") {
    const words = timbre.trim().split(/\s+/);
    const [hue, sat, light] = words.map(Number);

    if (timbre.trim() === "") {
      bits.push("not analysed yet");
    } else if (words.length === 3 && [hue, sat, light].every(Number.isFinite)) {
      bits.push(light === 0 ? "silent"
                            : "hue " + hue.toFixed(1) + "°, sat " + sat.toFixed(2)
                                + ", light " + light.toFixed(2));
    }
  }

  const phase = tree.run(id, "phase", "");
  if (phase) bits.push(phase);

  /*  WHICH ROUND, which is the number a designer watching a shuffled bed
      actually wants. Shown only when there is more than one to be on: a group
      that plays its members once is not counting anything. */
  const iteration = Number(tree.run(id, "iteration", 0));
  const iterations = Number(tree.run(id, "iterations", 1));

  if (iteration > 0 && (iterations === 0 || iterations > 1)) {
    bits.push("round " + iteration + (iterations === 0 ? "" : " of " + iterations));
  }


  const late = Number(tree.run(id, "late", 0));
  if (Number.isFinite(late) && late > 0) bits.push("late " + late);

  const error = tree.run(id, "error", "");
  if (error) bits.push(error);

  /*  AND WHO STARTED IT. A run the persistent section put back is not one the
      operator pressed, and an operator looking at a pane full of sound needs to
      know which is which - especially the one that came back on its own. */
  const asserted = tree.run(id, "asserted", false);

  const finished = state === "done" || state === "failed";

  /*  WHAT CAN BE SEEKED: what the ENGINE says a seek would move, run by run
      (`run/seekable`, K9, namespace draft §23.18) - a sounding file, or a
      running scene it can place a second in: as written, or within the round
      it is in. This read the cue's mode and offered the box on every timeline
      and every sequence that advances by itself - a sampler bank among them,
      which the desktop never offered - and the engine moved none that loops,
      shuffles or has a header. One rule, the engine's, read the same way by
      both clients. The desktop scrubs by dragging; here a second is typed
      (gestures/fields.js): for a scene, the scene's own second since it
      started, across its rounds - which the row's clock (`sceneClock`, K9's
      review) shows - and the engine takes it within the round the scene is in.
      Before the review the row had no such clock. */
  const seekable = offersSeek(id);

  const pills = pillsOf(id);

  out.push({ key: "run:" + id, html:
    '<div class="run" data-run="' + esc(id) + '" data-s="' + esc(state) + '"' +
      ' style="padding-left:' + (12 + depth * 14) + 'px">' +
      '<div class="who">' +
        stateMark(state) +
        '<span class="text">' + esc(name) + "</span>" +
        (asserted === true || asserted === "true"
           ? '<span class="asserted" title="the persistent section put this back">asserted</span>'
           : "") +
        '<span class="kind">' + esc(kind) + "</span>" +
      "</div>" +
      '<div class="meta">' +
        bits.map((b) => "<span>" + esc(b) + "</span>").join("") +
        (seekable
           ? '<input type="number" class="seek" data-seek="' + id + '" min="0" step="0.1"' +
             ' placeholder="s" title="seek: type a second of this ' +
             (kind === "group" ? "scene" : "file") + ' and press Enter">'
           : "") +
      "</div>" +
      (pills.length ? pillsHtml(id, pills) : "") +
    "</div>" });

  tree.ids("/godot/run/" + id + "/children").forEach((child) => runRow(child, depth + 1, out));
}

/*  Keyed by run and reconciled like the cue list, for the same two reasons: a
    pane of running cues is as long as the show is busy and has a scroll to
    keep, and a run keeps its row's element - and the kill button on it - from
    one poll to the next. Whether or not the row changed: this is the pane
    where it changes on every poll, for as long as a cue is playing. */
function renderRuns() {
  if (runsHold.active) return;

  const all = tree.ids("/godot/run/order");
  const pane = el("runs");

  if (!all.length) {
    /*  §7's other phrase, for the state it names: the show is complete and all
        is silent. It is an empty state, which §4.7 says is where these belong. */
    reconcile(pane, [{ key: "empty", html:
      '<div class="empty"><div class="line">They do not move.</div>' +
      '<div class="under">Nothing is running.</div></div>' }]);
    return;
  }

  const out = [];
  all.filter((id) => !tree.run(id, "parent", "")).forEach((id) => runRow(id, 0, out));
  reconcile(pane, out);
}

/*  A GROUP'S ROUND AS PILLS (PRD §3.6; the app's `pillsOf`): the round in
    progress in the order it plays, each member's state from the latest of this
    run's runs of it; then the members not in this round; then those skipped
    until the group ends. None for a group that has drawn no round and skipped
    nothing. */
const PILL_WORDS = { played: "played", out: "not this round", pruned: "skipped" };

function words(text) {
  return String(text || "").trim().split(/\s+/).filter(Boolean);
}

function pillsOf(id) {
  const round = words(tree.run(id, "round", ""));
  const pruned = words(tree.run(id, "pruned", ""));

  if (!round.length && !pruned.length) return [];

  const latest = {};
  tree.ids("/godot/run/" + id + "/children").forEach((child) => { latest[tree.run(child, "cue", "")] = child; });

  const pill = (cue, state) => ({ cue, state, number: String(tree.cue(cue, "number", "") || ""),
                                  name: shownName(cue) || cue });
  const out = round.map((cue) => {
    const state = latest[cue] ? tree.run(latest[cue], "state", "") : "";
    return pill(cue, ["waiting", "playing", "stopping", "postWait"].includes(state) ? "playing"
                   : state === "done" || state === "failed" ? "played" : "pending");
  });

  const group = tree.run(id, "cue", "");
  if (round.length && group) {
    tree.ids("/godot/cue/" + group + "/order").forEach((cue) => {
      if (!round.includes(cue) && !pruned.includes(cue)) out.push(pill(cue, "out"));
    });
  }

  pruned.forEach((cue) => out.push(pill(cue, "pruned")));
  return out;
}

function pillsHtml(id, pills) {
  return '<div class="pills">' +
    pills.map((p) =>
      '<button class="pill" data-pill="' + esc(p.cue) + '" data-pill-run="' + esc(id) + '" data-state="' + p.state + '">' +
        (p.state === "playing" ? '<span class="mark" aria-hidden="true">\u25B6</span>' : "") +
        (p.number ? '<span class="number">' + esc(p.number) + "</span>" : "") +
        '<span class="text">' + esc(p.name) + "</span>" +
        (PILL_WORDS[p.state] ? '<span class="word">' + PILL_WORDS[p.state] + "</span>" : "") +
      "</button>").join("") +
    '<div class="pills-hint">tap: skip this round \u00b7 hold: skip until the group ends</div>' +
  "</div>";
}

/*  WHAT A RUN'S SHEET OFFERS (the app's `RunSheet`): its stops - now, and for a
    group after this member and after this round - advance out of its slice
    when it plays one, solo on its strip when it holds one, and the kill last,
    which takes everything under it and says how many. Nothing but the words
    for a run that is over. The app's words. */
function countUnder(id) {
  return tree.ids("/godot/run/" + id + "/children").reduce((n, child) => n + 1 + countUnder(child), 0);
}

function runSheet(id, send) {
  const state = tree.run(id, "state", "armed");
  const cue = tree.run(id, "cue", "");
  const kind = tree.run(id, "kind", "");
  const finished = state === "done" || state === "failed";
  const number = String(tree.cue(cue, "number", "") || "");
  const title = (number ? number + "  " : "") + (shownName(cue) || cue || id);

  if (finished) return { title, sub: "Over: it leaves this list in a few seconds.", actions: [] };

  const actions = [{ words: "Stop now - its footer runs", tone: "stop", run: () => send("stop-run", [id, "hard"]) }];

  if (kind === "group") {
    actions.push({ words: "Stop after this member", tone: "stop", run: () => send("stop-run", [id, "afterMember"]) });
    actions.push({ words: "Stop after this round", tone: "stop", run: () => send("stop-run", [id, "afterIteration"]) });
  }

  if (Number(tree.run(id, "range", -1)) >= 0) actions.push({ words: "Advance to the next slice", run: () => send("advance", [id]) });

  if (tree.run(id, "strip", "")) {
    const solo = tree.run(id, "solo", false);
    const on = solo === true || solo === "true";
    actions.push({ words: on ? "Release the solo" : "Solo on its strip", run: () => send("solo", [id, !on]) });
  }

  const under = countUnder(id);
  actions.push({ words: under > 0 ? "Kill it and everything under it (" + under + ") - no footer" : "Kill now - no footer",
                 tone: "kill", air: true, run: () => send("kill", [id]) });

  return { title, sub: state, actions };
}

export { renderRuns, stateMark, offersSeek, sceneClock, runsHold, pillsOf, runSheet };
