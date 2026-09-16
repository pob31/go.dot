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

/*  WHAT THIS READER IS LOOKING AT, AND WHERE IT IS KEPT BETWEEN VISITS: which
    containers are shut, and where the inspector sits.

    None of it is the engine's (§14.1). The document holds what somebody decided
    (§4.10) and a fold is not a decision about the show - it is one operator's
    view of it, and a second operator on a second tablet folds their own groups
    without a single byte reaching anybody else. That is why this lives in the
    browser rather than in a node, and why nothing here ever sends a command.

    IT NOW OUTLIVES A RELOAD (author, 2026-09-16: "yes we need to store the
    expanded and collapsed state of the different containers"). A show of five
    hundred cues is read by folding it down to the act somebody is working on,
    and losing that on every refresh - which on a page with no build step is how
    the page is changed - made the fold not worth using. localStorage is the
    right size of answer: it is the reader's own machine, it is per origin so
    the engine's address separates one installation from another, and it needs
    no engine change, no schema row and no migration.

    WHAT IS PER SHOW AND WHAT IS NOT. `shut` holds identifiers, and an
    identifier means nothing in another document, so it is filed under the
    document's path. The inspector's arrangement is a preference about the
    workspace rather than about a show - somebody who wants the inspector at the
    foot wants it there in every show they open - so `panel` is global.

    EVERY TOUCH OF localStorage IS WRAPPED. A private window, a browser set to
    block site data, a quota refusal, or an installation reached over a scheme
    where storage is partitioned away: in all of them the accessor or the call
    throws, and this page must go on working exactly as it did before any of
    this was written. So each read falls back to the defaults and each write is
    allowed to fail silently - a fold that is not remembered is a small loss,
    and a page that will not draw is not.

    `globalThis.localStorage` IS READ AT EVERY ACCESS rather than captured once
    at import, for two reasons: an accessor that throws throws where it is
    named, inside the try that is there to catch it, and a test can stand its
    own fake in front of it without this module having been loaded first. */

import { tree } from "../plumbing/tree.js";

/*  ONE KEY, holding one JSON object. Not a key per show: a per-show key makes
    the trim below impossible to write without listing the whole of storage and
    guessing which entries are ours. */
const storageKey = "godot.console.view";

/*  HOW MANY SHOWS ARE REMEMBERED. A machine that has opened a hundred shows
    should not carry a hundred fold lists for ever, and the ninety-ninth is not
    coming back. Twelve is a season's worth of documents. */
const showLimit = 12;

/*  THE CONTAINERS THIS READER HAS SHUT, by key. Present means SHUT, which is
    the meaning it has had since the fold was written and is kept here: a show
    opens with everything visible, and only what somebody closed is recorded.

    The keys, all four of them:
      "<cueId>"                    a group's own fold
      "<cueId>:header"             that group's header section
      "<cueId>:footer"             that group's footer section
      "list:<listId>:persistent"   a list's persistent band */
const folded = new Set();

/*  AND WHETHER THE INSPECTOR'S DETAILS ARE OPEN (author, 2026-09-16: "hide the
    internal stuff like the various UIDs, hash and other things that are not
    really necessary for the user").

    Shut by default, like a folded group and for the same reason: it is what
    this reader is looking at, not something the show knows (§14.1). It is
    remembered across a reload, with the folds and in the same place. */
const panel = {
  details: false,

  /*  WHERE THE INSPECTOR SITS: "side" is the third column it has always been,
      "foot" is a band across the bottom under Didi and Gogo.

      The author raised it with the page open (2026-09-16): three columns mean
      picking on the left and adjusting on the right, over and over, and the
      two views that are coming - a curve with draggable breakpoints (5.16b)
      and a coloured bar of a file (5.17) - both want width rather than depth,
      which is why QLab puts its inspector at the foot. Neither answer is
      obviously right, so the page can be flipped and looked at.

      Remembered globally rather than per show, because it is an answer about
      this screen and these hands: somebody who has settled on the foot wants
      the foot in the next document too. */
  layout: "side",
};

/*  WHOSE FOLDS `folded` CURRENTLY HOLDS. Set by `adopt`, read by `save`, so
    that a fold made while one document is open is never written under the path
    of the one that replaced it. */
let path = "";

/*  A WELL-FORMED, EMPTY STORE. Built fresh each time rather than shared, so
    that a caller which goes on to fill one in cannot quietly change what the
    next fallback looks like. */
function defaults() {
  return { v: 1, panel: { details: false, layout: "side" }, shows: {} };
}

/*  TEXT FROM STORAGE INTO A STORE, AND ANYTHING ELSE INTO THE DEFAULTS.

    What is in localStorage was put there by a version of this page that may not
    be this one, by a browser extension, or by somebody typing into the
    developer console - so nothing about its shape can be assumed. Every check
    below answers the same way: if it is not what we wrote, we did not write it,
    and the reader starts from a clean view rather than from half of somebody
    else's. That is deliberately blunt - one malformed show entry discards the
    lot - because the alternative is a store that is partly trusted, and a rule
    about which part is a rule somebody has to remember.

    Pure: no storage is touched here, which is what makes it a thing a test can
    hand a string to. */
function decode(text) {
  const empty = defaults();

  if (typeof text !== "string" || !text.length) return empty;

  let raw;

  try { raw = JSON.parse(text); } catch (problem) { return empty; }

  if (!raw || typeof raw !== "object" || Array.isArray(raw)) return empty;

  /*  A DIFFERENT VERSION IS NOT READ AND NOT MIGRATED. There is one version so
      far; when there is a second, this is where it will be recognised, and
      until then an unknown `v` is somebody else's data. */
  if (raw.v !== 1) return empty;

  if (!raw.shows || typeof raw.shows !== "object" || Array.isArray(raw.shows)) return empty;

  const store = defaults();

  /*  The panel is normalised rather than rejected: it has two fields with two
      answers each, so there is always a sane reading of whatever is there, and
      a typo in a preference should not cost somebody their folds. */
  const said = raw.panel && typeof raw.panel === "object" && !Array.isArray(raw.panel)
                 ? raw.panel : {};

  store.panel.details = said.details === true;
  store.panel.layout = said.layout === "foot" ? "foot" : "side";

  for (const named of Object.keys(raw.shows)) {
    const one = raw.shows[named];

    if (!one || typeof one !== "object" || Array.isArray(one)) return empty;
    if (!Array.isArray(one.shut)) return empty;
    if (one.shut.some((key) => typeof key !== "string")) return empty;

    store.shows[named] = {
      shut: one.shut.slice(),
      seen: Number.isFinite(one.seen) ? one.seen : 0,
    };
  }

  return store;
}

/*  AND BACK TO TEXT. A function of its own rather than a call to JSON.stringify
    at the one site that writes, so that the round trip is one pair a test can
    put a store through. Pure. */
function encode(store) {
  return JSON.stringify(store);
}

/*  THE MOST RECENTLY SEEN SHOWS, AND NO MORE THAN `limit` OF THEM.

    Recency is `seen`, stamped by every save, so a show that is being worked on
    stays and one opened once last year falls off the end. Nothing is lost that
    matters: the fold of a show nobody has opened in a hundred documents is not
    a thing anybody is waiting to see again.

    PURE - it returns a NEW store and leaves the one it was handed exactly as it
    was, which is what lets a test assert on both sides of the call. */
function trim(store, limit = showLimit) {
  const from = store && typeof store === "object" ? store : {};
  const shows = from.shows && typeof from.shows === "object" ? from.shows : {};
  const said = from.panel && typeof from.panel === "object" ? from.panel : {};

  const out = {
    v: 1,
    panel: {
      details: said.details === true,
      layout: said.layout === "foot" ? "foot" : "side",
    },
    shows: {},
  };

  const when = (named) => Number(shows[named] && shows[named].seen) || 0;

  const kept = Object.keys(shows)
                     .sort((a, b) => when(b) - when(a))
                     .slice(0, Math.max(0, limit));

  for (const named of kept) {
    const one = shows[named] || {};

    out.shows[named] = {
      shut: Array.isArray(one.shut) ? one.shut.slice() : [],
      seen: Number.isFinite(one.seen) ? one.seen : 0,
    };
  }

  return out;
}

/*  THE STORE AS IT IS ON DISK, or the defaults if anything at all goes wrong.
    The `globalThis.localStorage` inside the try is the point of the try: in a
    browser that has been told to block site data, naming it is what throws. */
function read() {
  try {
    const store = globalThis.localStorage;

    return decode(store ? store.getItem(storageKey) : null);
  } catch (problem) {
    return defaults();
  }
}

/*  THE CURRENT VIEW, WRITTEN DOWN.

    Read-modify-write rather than write, because the file holds more than this
    show: the other documents' folds are in it, and two tabs open on two shows
    would otherwise take turns erasing each other. What is read back is merged
    under this show's path only, and the trim is applied on the way out so the
    file cannot grow without bound.

    A FAILED WRITE IS NOT AN ERROR ANYBODY IS SHOWN. Quota refusals and private
    windows are the two that happen, and the honest consequence of both is that
    the fold lasts as long as the tab does - which is what it did before this
    file existed. */
function save() {
  const store = read();

  /*  THE SHOW BEING WRITTEN IS NOT A CANDIDATE FOR EVICTION, which it would
      otherwise be: `trim` keeps the twelve highest `seen` values, and `seen` is
      a wall clock read on somebody's laptop. A machine whose clock ran ahead
      and was then put right has twelve stamps in the future and this one in the
      past, and the save would quietly sort tonight's show off its own end - the
      fold made, the file written, and nothing there on reload. So it is taken
      out before the trim, which is asked for one fewer, and put back after.
      Twelve is still twelve, `trim` stays pure, and the eviction cannot reach
      the one entry this call exists to write. */
  delete store.shows[path];

  const out = trim(store, showLimit - 1);

  out.panel.details = panel.details === true;
  out.panel.layout = panel.layout === "foot" ? "foot" : "side";
  out.shows[path] = { shut: Array.from(folded), seen: Date.now() };

  try {
    const where = globalThis.localStorage;

    if (where) where.setItem(storageKey, encode(out));
  } catch (problem) {
    /*  Nothing. The comment above is the handling. */
  }
}

/*  A NEW DOCUMENT IS A NEW SET OF FOLDS.

    Called from app.js the moment `/godot/document/path` says something else.
    It clears - identifiers from the old show name nothing in the new one - and
    then loads whatever this reader last left shut in THIS show. The panel is
    loaded here too rather than at import, so that a page which has been open
    since before the preference was changed in another tab catches up at the
    next show rather than never.

    IT DOES NOT WRITE. Opening a show is not a decision about how to look at it,
    and a page that wrote on every load would stamp `seen` on documents nobody
    had touched, which is exactly the recency the trim is trying to measure. */
function adopt(show) {
  path = typeof show === "string" ? show : "";

  const store = read();

  panel.details = store.panel.details;
  panel.layout = store.panel.layout;

  folded.clear();

  const remembered = store.shows[path];

  if (remembered) for (const key of remembered.shut) folded.add(key);
}

/*  OPEN WHAT IS SHUT AND SHUT WHAT IS OPEN, and write it down. One gesture, one
    save: folds are made a handful at a time by a hand, not a hundred at a time
    by a render, so there is no case for batching the write. */
function toggle(key) {
  if (folded.has(key)) folded.delete(key); else folded.add(key);

  save();
}

/*  EVERYTHING BETWEEN A CUE AND THE TOP OF ITS LIST, OPENED.

    The way to a row is up: a cue says who contains it (`parent`) and where in
    it it sits (`role`), and both are published by the engine and derived rather
    than stored, so they cannot disagree with the tree the rows are drawn from.
    Walking up and unfolding each container in turn is the only way to be sure a
    row will actually be drawn - a member can be three groups deep, with a shut
    header section in between.

    WHETHER THE PARENT IS A CUE IS ASKED OF THE TREE, not guessed from the
    shape of the identifier: a group and a list both hand out the same kind of
    identifier, and the only honest difference is that one of them publishes
    `/godot/cue/<id>/kind` and the other does not. When it is not a cue the walk
    has reached the list, which is the top, and the one thing left to open there
    is the persistent band.

    BOUNDED AT SIXTY-FOUR STEPS. A `parent` chain is a tree and cannot loop -
    but this walks a tree read off the wire, and a tree read off the wire is
    whatever arrived. Sixty-four is far deeper than any show anybody will nest
    and cheap enough that the guard costs nothing.

    It does not save; `openForKey`, which is the only caller that matters, does
    it once at the end. */
function openTo(id) {
  let node = id;

  for (let step = 0; step < 64 && node; step += 1) {
    const parent = tree.cue(node, "parent", "");
    const role = tree.cue(node, "role", "");

    if (!parent) return;

    if (tree.node("/godot/cue/" + parent + "/kind")) {
      folded.delete(parent);

      /*  The section as well as the group: a header cue inside a shut `header`
          band is hidden by the band even when the group itself is open. */
      if (role === "header" || role === "footer") folded.delete(parent + ":" + role);

      node = parent;
      continue;
    }

    /*  Not a cue, so it is the list this row's chain ends in. */
    if (role === "persistent") folded.delete("list:" + parent + ":persistent");

    return;
  }
}

/*  WHATEVER MUST BE OPEN FOR THE ROW WITH THIS RECONCILER KEY TO BE DRAWN.

    The key rather than the identifier, because ONE CUE CAN BE DRAWN TWICE: a
    member marked `preset` for a group has its own row where it sits in the
    list, and a second, derived line in that group's header. `cue:<id>` names
    the first and `preset:<group>:<id>` the second, and which one a reveal is
    aimed at decides what has to be unfolded - the member's own ancestors, or
    the group whose header shows it.

    For the derived line the walk goes to the GROUP, not to the member: the line
    is drawn inside that group's header section, so it is that group and that
    section which have to be open, wherever in the list the member itself
    lives. */
function openForKey(key) {
  const named = typeof key === "string" ? key : "";

  if (named.startsWith("cue:")) {
    openTo(named.slice(4));
  } else if (named.startsWith("preset:")) {
    const rest = named.slice(7);
    const cut = rest.indexOf(":");

    if (cut > 0) {
      const group = rest.slice(0, cut);

      openTo(group);
      folded.delete(group);
      folded.delete(group + ":header");
    }
  }

  save();
}

export { adopt, decode, encode, folded, openForKey, openTo, panel, save, toggle, trim };
