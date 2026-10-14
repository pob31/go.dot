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

/*  WHAT THIS COMPUTER IS IN ITS PAIR (namespace draft §58, RD.11): the
    `pair/*` rows turned into the words the page shows under the transport,
    and which of the operator's five commands would be taken now.

    THE DESKTOP'S TWIN: src/wfg/client/model/Pair.h says the same things in
    the same words to the window, and a word changed there is changed here -
    the page cannot read a C++ constant, and the two clients must not disagree
    about what a machine is. Some words are the author's (§58.2, AGM-AGP):
    "Main - sending", "Main - alone", "Backup - following", "Take over", "Hand
    over", "Two mains!", "Make this the main". The rest are the implementer's,
    listed in §58.7's RD.11 line for his review.

    A READING, NEVER A DECISION: whether a command would be refused is the
    engine's rule (pair/Leadership.cpp `refusal`, PairSettings.cpp), written out
    again here from the rows; the engine still judges every press, and its
    refusal comes back on the refusal line like any other. Pure: it takes a
    tree, so a test hands it one. */

import { tree as served } from "../plumbing/tree.js";

const WORDS = Object.freeze({
  // The author's (AGM, AGN, AGO).
  mainSending: "Main - sending",
  mainAlone: "Main - alone",
  backupFollowing: "Backup - following",
  takeOver: "Take over",
  handOver: "Hand over",
  twoMains: "Two mains!",
  makeThisTheMain: "Make this the main",

  // The implementer's, in AGM's shape, for the author's review.
  backupWaiting: "Backup - waiting",
  backupSending: "Backup - sending",
  mainFollowing: "Main - following",
  realign: "Realign",
});

/*  THE FIVE, by the command each button names in its `data-cmd`. */
const COMMANDS = Object.freeze(["pair.keep", "pair.takeOver", "pair.handOver", "pair.realign", "pair.adoptShow"]);

function isYes(value) { return value === true || value === "true"; }

function words(value) { return String(value ?? ""); }

/*  WHETHER A CUE RUNS HERE - started and not ended, what the horizon only made
    ready left out - which is what `pair.adoptShow` is refused `busy` for. */
function anyRunLive(tree) {
  for (const id of tree.ids("/godot/run/order")) {
    const state = words(tree.run(id, "state", ""));

    if (!state || state === "preparing" || state === "done" || state === "failed") continue;

    const started = words(tree.run(id, "started", "0"));

    if (started !== "" && started !== "0") return true;
  }

  return false;
}

function readPair(tree = served) {
  const pair = {
    role: words(tree.get("/godot/pair/role", "")),
    state: words(tree.get("/godot/pair/state", "")),
    linked: isYes(tree.get("/godot/pair/linked", false)),
    alarm: words(tree.get("/godot/pair/alarm", "")),
    inStep: isYes(tree.get("/godot/pair/inStep", false)),
    differs: words(tree.get("/godot/pair/differs", "")),
    showMatch: isYes(tree.get("/godot/pair/showMatch", false)),
    showDiffers: words(tree.get("/godot/pair/showDiffers", "")),
    busy: false,
  };

  //  Only where it decides something: the one button it greys.
  if (pair.state === "following" && pair.showDiffers) pair.busy = anyRunLive(tree);

  return pair;
}

function paired(pair) { return pair.role === "main" || pair.role === "backup"; }

function other(pair) { return pair.role === "main" ? "backup" : "main"; }

/*  AGM's words, and the implementer's in their shape for the other seats. */
function seatWords(pair) {
  if (!paired(pair) || !pair.state) return "";

  const isMain = pair.role === "main";

  if (pair.state === "following") return isMain ? WORDS.mainFollowing : WORDS.backupFollowing;
  if (pair.state === "waiting") return isMain ? "Main - waiting" : WORDS.backupWaiting;
  if (pair.state === "alone" && isMain) return WORDS.mainAlone;

  return isMain ? WORDS.mainSending : WORDS.backupSending;
}

function seatSentence(pair) {
  if (!paired(pair)) return "Not paired: this computer plays the show on its own.";

  const them = other(pair);

  switch (pair.state) {
    case "following":
      return pair.role === "main"
        ? "The backup took over: this computer follows it a moment behind and sends nothing to the devices."
        : "This computer follows the main a moment behind and sends nothing to the devices, ready to take over.";
    case "waiting":
      return "This computer has not found its " + them + " since it started: it holds, and sends nothing to the" +
             " devices until it finds it or is told to take over.";
    case "bothSending":
      return "Both computers are sending: every command reaches the devices from both.";
    case "leading":
      return pair.role === "main"
        ? "This computer sends to the devices, and the backup follows it."
        : "This computer took over and sends to the devices" +
          (pair.linked ? ", and the main follows it." : "; the main is not there.");
    case "alone":
      return "This computer sends to the devices; " +
             (pair.role === "main" ? "no backup follows it." : "the main is not there.");
    default:
      return "—";
  }
}

function showTextDiffers(pair) { return pair.showDiffers.startsWith("the show differs"); }

function buttonWords(pair, command) {
  switch (command) {
    case "pair.takeOver": return WORDS.takeOver;
    case "pair.handOver": return WORDS.handOver;
    case "pair.keep": return WORDS.makeThisTheMain;
    case "pair.realign": return WORDS.realign;
    case "pair.adoptShow": return "Take the " + other(pair) + "'s show";
    default: return command;
  }
}

/*  THE ENGINE'S RULE, from the rows: `{ open, reason, words }`, the reason the
    engine's own word or this page's where the rows say more. */
function offer(pair, command) {
  const holding = pair.state === "following" || pair.state === "waiting";
  const them = other(pair);
  let reason = "";

  switch (command) {
    case "pair.takeOver":
      reason = !paired(pair) ? "not-paired" : holding ? "" : "not-holding";
      break;
    case "pair.handOver":
      reason = !paired(pair) ? "not-paired"
             : holding ? "not-sending"
             : (pair.state === "leading" || pair.state === "bothSending") && pair.linked ? "" : "not-linked";
      break;
    case "pair.keep":
      reason = !paired(pair) ? "not-paired" : pair.state === "bothSending" ? "" : "not-both-sending";
      break;
    case "pair.realign":
      reason = pair.state === "following" ? "" : "not-following";
      break;
    case "pair.adoptShow":
      reason = pair.state !== "following" ? "not-following"
             : pair.busy ? "busy"
             : pair.showMatch ? "same-show"
             : !pair.showDiffers ? "not-compared"
             : !showTextDiffers(pair) ? "files-only" : "";
      break;
    default:
      reason = "unknown";
  }

  if (!reason) {
    const does = {
      "pair.takeOver": "This computer sends to the devices from now on; the " + them +
                       " holds and follows it, if it is there.",
      "pair.handOver": "The " + them + " sends to the devices from now on; this computer holds and follows it.",
      "pair.keep": "The show stays on this computer; the " + them + " holds and follows it.",
      "pair.realign": "Takes the " + them + "'s position again: its standby, and every cue running at its playhead.",
      "pair.adoptShow": "Takes the " + them + "'s show - every cue and setting - and keeps this computer's" +
                        " interface and ports. This computer's own files are set aside in a dated folder of the" +
                        " show; sounds are not copied.",
    };

    return { open: true, reason: "", words: does[command] };
  }

  //  A page never shows the five on a computer with no role; said all the same, where a role is given.
  const notPaired = "This computer is not paired: its role is given in the desktop window's Show settings >" +
                    " Redundancy.";
  const why = {
    "not-paired": notPaired,
    "not-holding": "This computer already sends.",
    "not-sending": "This computer holds: there is nothing to hand over.",
    "not-linked": "The " + them + " is not there to take the show.",
    "not-both-sending": "Only while both computers send.",
    "not-following": paired(pair) ? "Only on a computer that follows the other." : notPaired,
    "busy": "Not while a cue runs here: stop it first.",
    "same-show": "The two shows are the same.",
    "not-compared": "The two shows have not been compared yet.",
    "files-only": "Only files differ, and files never travel: copy them onto this computer.",
  };

  return { open: false, reason, words: why[reason] || reason };
}

/*  THE BANNER: said when the engine has an alarm, with the one button that
    answers it, read from the rows rather than from the alarm's words. */
function bannerCommand(pair) {
  if (pair.state === "bothSending") return "pair.keep";
  if (pair.state === "waiting") return "pair.takeOver";

  if (pair.state === "following") {
    if (showTextDiffers(pair)) return "pair.adoptShow";
    if (pair.differs && !pair.inStep) return "pair.realign";
  }

  return "";
}

function banner(pair) {
  if (!paired(pair) || !pair.alarm) return null;

  const command = bannerCommand(pair);
  const answer = command ? offer(pair, command) : null;

  return {
    heading: pair.state === "bothSending" ? WORDS.twoMains : seatWords(pair),
    sentence: answer && !answer.open ? pair.alarm + " " + answer.words : pair.alarm,
    alarming: pair.state === "bothSending",
    command,
  };
}

export { WORDS, COMMANDS, readPair, paired, other, seatWords, seatSentence, buttonWords, offer, banner };
