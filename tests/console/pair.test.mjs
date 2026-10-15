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

/*  WHAT THIS COMPUTER IS IN ITS PAIR, as the page says it (namespace draft §58,
    RD.11, RD.12): model/pair.js, the desktop's model/Pair.h said again for a browser.
    The same seats, the same words and the same refusals as ClientTests pins for
    the window - the author's where he chose them (AGM-AGO), and a test that
    fails here when one client's words move and the other's do not. */

import { test } from "node:test";
import assert from "node:assert/strict";

const { readPair, paired, seatWords, seatSentence, buttonWords, offer, banner, peerLines, COMMANDS } =
  await import("../../clients/console/model/pair.js");

/*  A tree as the engine serves one, for the rows this reads and nothing else. */
function served(rows, runs = {}) {
  const at = { ...rows };

  for (const [id, row] of Object.entries(runs)) {
    at["/godot/run/" + id + "/state"] = row.state;
    at["/godot/run/" + id + "/started"] = row.started;
  }

  if (Object.keys(runs).length) at["/godot/run/order"] = Object.keys(runs).join(" ");

  return {
    get(address, fallback) { return address in at ? at[address] : fallback; },
    ids(address) { const text = this.get(address, ""); return text ? String(text).split(/\s+/) : []; },
    run(id, name, fallback) { return this.get("/godot/run/" + id + "/" + name, fallback); },
  };
}

function pairOf(role, state, more = {}, runs = {}) {
  return readPair(served({ "/godot/pair/role": role, "/godot/pair/state": state, ...more }, runs));
}

test("a computer with no role says nothing and offers nothing", () => {
  const pair = pairOf("none", "alone");

  assert.equal(paired(pair), false);
  assert.equal(seatWords(pair), "");
  assert.equal(banner(pair), null);

  for (const name of COMMANDS) assert.equal(offer(pair, name).open, false, name);

  assert.equal(offer(pair, "pair.takeOver").reason, "not-paired");
});

test("every seat in the author's words, and the implementer's in their shape", () => {
  //  The author's (AGM).
  assert.equal(seatWords(pairOf("main", "leading")), "Main - sending");
  assert.equal(seatWords(pairOf("main", "alone")), "Main - alone");
  assert.equal(seatWords(pairOf("backup", "following")), "Backup - following");

  //  The implementer's, for the author's review.
  assert.equal(seatWords(pairOf("backup", "waiting")), "Backup - waiting");
  assert.equal(seatWords(pairOf("backup", "leading")), "Backup - sending");
  assert.equal(seatWords(pairOf("main", "following")), "Main - following");
  assert.equal(seatWords(pairOf("main", "bothSending")), "Main - sending");
  assert.equal(seatWords(pairOf("backup", "bothSending")), "Backup - sending");

  assert.match(seatSentence(pairOf("backup", "following")), /follows the main a moment behind/);
  assert.match(seatSentence(pairOf("main", "following")), /^The backup took over/);
});

test("two mains: the alarm in red, and the button that keeps the show here", () => {
  const pair = pairOf("backup", "bothSending", { "/godot/pair/alarm": "Both machines are sending.", "/godot/pair/linked": true });
  const said = banner(pair);

  assert.equal(said.heading, "Two mains!");
  assert.equal(said.alarming, true);
  assert.equal(said.command, "pair.keep");
  assert.equal(buttonWords(pair, said.command), "Make this the main");
  assert.equal(offer(pair, "pair.keep").open, true);
  assert.equal(offer(pair, "pair.handOver").open, true);       // both send, the other there
  assert.equal(offer(pair, "pair.takeOver").reason, "not-holding");
});

test("a backup waiting for its main offers to take over", () => {
  const pair = pairOf("backup", "waiting", { "/godot/pair/alarm": "Waiting for the main." });
  const said = banner(pair);

  assert.equal(said.heading, "Backup - waiting");
  assert.equal(said.alarming, false);
  assert.equal(said.command, "pair.takeOver");
  assert.equal(buttonWords(pair, said.command), "Take over");
  assert.equal(offer(pair, "pair.handOver").reason, "not-sending");
});

test("a follower not in step offers to realign; in step, no banner", () => {
  const apart = pairOf("backup", "following", { "/godot/pair/alarm": "Following, but not in step: cue 4.",
                                                "/godot/pair/differs": "cue 4 is running on the main, not here",
                                                "/godot/pair/inStep": false });

  assert.equal(banner(apart).command, "pair.realign");
  assert.equal(buttonWords(apart, "pair.realign"), "Realign");

  const together = pairOf("backup", "following", { "/godot/pair/inStep": true, "/godot/pair/showMatch": true });

  assert.equal(banner(together), null);
  assert.equal(offer(together, "pair.takeOver").open, true);
  assert.equal(offer(together, "pair.adoptShow").reason, "same-show");
});

test("a show that differs offers the main's show, greyed with why while a cue runs", () => {
  const rows = { "/godot/pair/alarm": "Following, but the show differs: cue 4 Rain changed.",
                 "/godot/pair/showDiffers": "the show differs: cue 4 Rain changed" };
  const idle = pairOf("backup", "following", rows, { R1: { state: "preparing", started: "0" } });
  const said = banner(idle);

  assert.equal(said.command, "pair.adoptShow");
  assert.equal(buttonWords(idle, said.command), "Take the main's show");
  assert.equal(offer(idle, "pair.adoptShow").open, true);

  const running = pairOf("backup", "following", rows, { R1: { state: "playing", started: "120" } });

  assert.equal(offer(running, "pair.adoptShow").reason, "busy");
  assert.match(banner(running).sentence, /Not while a cue runs here/);

  //  A main following its backup takes the backup's show.
  assert.equal(buttonWords(pairOf("main", "following", rows), "pair.adoptShow"), "Take the backup's show");

  //  Files alone: nothing to take, and said so.
  const files = pairOf("backup", "following", { "/godot/pair/showDiffers": "1 sound missing here: tone.wav" });

  assert.equal(offer(files, "pair.adoptShow").reason, "files-only");
});

test("a main sending can hand over only while the backup is there", () => {
  assert.equal(offer(pairOf("main", "leading", { "/godot/pair/linked": true }), "pair.handOver").open, true);
  assert.equal(offer(pairOf("main", "alone", { "/godot/pair/linked": false }), "pair.handOver").reason, "not-linked");
  assert.equal(offer(pairOf("main", "leading", { "/godot/pair/linked": true }), "pair.realign").reason, "not-following");
});

test("the main told by its backup: the backup's news in the banner, nothing to press here, its verdicts in words", () => {
  //  The engine's own sentence (pair/Leadership.cpp), as serve publishes it at pair/alarm.
  const apart = pairOf("main", "leading", {
    "/godot/pair/linked": true,
    "/godot/pair/alarm": "The backup is not in step: it says cue Bed is running on the main, not here." +
                         " Realign on the backup to take this machine's position again.",
    "/godot/pair/peerInStep": "no",
    "/godot/pair/peerDiffers": "cue Bed is running on the main, not here",
    "/godot/pair/peerShowMatch": "yes",
  });
  const said = banner(apart);

  assert.equal(said.heading, "Main - sending");
  assert.equal(said.alarming, false);
  assert.equal(said.command, "");
  assert.match(said.sentence, /^The backup is not in step: it says cue Bed/);
  assert.deepEqual(peerLines(apart), [
    { label: "In step", value: "No - the backup says cue Bed is running on the main, not here" },
    { label: "Same show", value: "Yes - the backup has the same cues, settings and files" },
  ]);

  //  Nothing known: where to look, as before RD.12 - and a row not there reads the same.
  const unknown = pairOf("main", "leading", { "/godot/pair/linked": true, "/godot/pair/peerInStep": "unknown" });
  assert.equal(banner(unknown), null);
  assert.deepEqual(peerLines(unknown).map((line) => line.value),
                   ["Checked on the backup - look at its screen", "Checked on the backup - look at its screen"]);

  //  Its show not this one's.
  const show = pairOf("main", "leading", { "/godot/pair/linked": true, "/godot/pair/peerInStep": "yes",
                                           "/godot/pair/peerShowMatch": "no",
                                           "/godot/pair/peerShowDiffers": "the show differs: cue 4 Rain changed" });
  assert.deepEqual(peerLines(show).map((line) => line.value),
                   ["Yes - the backup has the same standby and the same cues, at the same place",
                    "No - the backup says the show differs: cue 4 Rain changed"]);

  //  Only on a computer that sends with the other there.
  assert.deepEqual(peerLines(pairOf("backup", "following", { "/godot/pair/linked": true })), []);
  assert.deepEqual(peerLines(pairOf("main", "alone", { "/godot/pair/linked": false })), []);
});
