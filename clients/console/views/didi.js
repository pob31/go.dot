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

/*  DIDI, the cue list: the persistent band at its head, then every row of the
    focused list, the groups inside it, and their headers and footers. */

import { tree } from "../plumbing/tree.js";
import { selection } from "../model/selection.js";
import { folded } from "../model/remember.js";
import { el, esc, cueName } from "./common.js";
import { refreshFields } from "./values.js";
import { reconcile } from "./reconcile.js";
import { view } from "./view.js";

/*  The current answer, refreshed by `listRows` and read by `cueRow`. */
let overlapping = new Map();

/*  EVERY ROW THE READER HAS CHOSEN, as a set, refreshed by `listRows` in the
    same breath and for the same reason: it is one answer the whole pass reads.

    `selection.chosen` is an ARRAY because order is what a range means - it is
    what a drag carries and what a delete walks - but a row only ever asks the
    one question "am I in it", and asking an array that is five hundred long,
    once per row, is a quarter of a million comparisons inside a render that has
    about 6.5ms to finish on a show that size (M24). So the array is turned into
    a set once per render and every row asks the set. The ordinary case, one
    picked row, costs one small allocation a poll, which is nothing beside the
    markup this pane builds anyway.

    IT FALLS BACK TO `picked` when there is no array, so this pane still draws
    the one chosen row if it is ever loaded beside a selection model that has
    not grown the range yet. */
let chosen = new Set();

/*  WHERE THE READER WAS JUST SENT, and whether this is the row they were sent
    to. `selection.reveal` is set by the click that follows a tendril - from a
    derived line to the member's own row, or from the mark on a member up to
    the header that gets it ready - and it names the reconciler key of the row
    at the other end, with the moment the highlight stops meaning anything.

    IT IS ASKED WHILE THE MARKUP IS WRITTEN, and the highlight is an attribute
    in that markup, because the markup is a row's whole state: `morph` copies
    the fresh element's attributes onto the live one and removes any the fresh
    one lacks, so a class put on an element after a render is wiped by the next
    poll a tenth of a second later. A flash that lasted one poll would be a
    flash nobody saw.

    And the run-out is read here rather than timed: there is no timer to cancel
    when the reader clicks somewhere else, and a page that has stopped polling -
    a laptop lid shut on it - wakes up with the highlight already over rather
    than with one still to come. */
function revealing(key) {
  const reveal = selection.reveal;

  if (!reveal) return false;

  if (Date.now() >= reveal.until) {
    selection.reveal = null;
    return false;
  }

  return reveal.key === key;
}

/*  A SECTION, AS A FRAME THAT SHUTS (author, 2026-09-16: "I think we're missing
    a clearer delimiter between the header section and footer, something like a
    collapsible frame").

    A header and a footer used to be announced by a bare word and nothing else,
    so where one ended and the members began was a matter of reading the indents
    - and in a group with both, the footer's word was the only thing between the
    last member and the first footer cue. Now the word heads a frame: a rule
    along the top and down the left of every row the section holds, closed off
    by a short rule under the last of them, and a twist that shuts the whole
    thing away.

    DRAWN AS A RAIL ON EACH ROW RATHER THAN AS A BOX AROUND THEM, because the
    reconciler keeps ONE FLAT LIST of keyed rows: a wrapper element would be a
    second level of nesting it does not model, and every row inside it would
    have to be reconciled against the wrapper rather than against the pane. So
    each row carries the frame's left edge as its own `::before` at `--rail`,
    and this head and its end draw the two corners.

    THE END ROW IS PUSHED AFTER THE LINES, not worked out from a count, because
    a header cue may itself be an open group whose own children follow it: the
    last row of the section is whatever `lines` pushed last, and there is no
    other way to be right about that.

    THE HEAD'S KEY IS "band:" + THE FOLD KEY and the end's is that with ":end",
    so the three names a section has are one name said three ways and a section
    cannot be half-renamed. `folded` holds the SHUT ones - present means shut -
    which is what it has always meant for a group's own twist.

    AND BOTH EDGES NAME THEIR SECTION IN A WORD, for the drag. A <Header>, a
    <Footer> and a <Persistent> have an identifier in the file and NO ADDRESS in
    the tree - /godot/cue/<that id> is a 404 - so no client can learn one, no
    client can name one as a parent, and a drop INTO one of them is not
    expressible. The page has to refuse that drop and SAY so rather than let it
    quietly do nothing, and to say so it first has to know that the thing under
    the pointer is a band at all. That is the one question about a band the tree
    cannot be asked: it is not a cue, it has no identifier to look up, and the
    rest of what it carries is a fold key and a count. So `data-band` puts the
    word on the head and on the end alike - one word, the three sections'
    three - and a pointer resting in the gap above a section or below it has
    something to read. It is fixed markup and not a per-poll value, so the
    reconciler leaves it alone for as long as the section stands. */
function frame(out, fold, word, count, rail, note, lines) {
  const shut = folded.has(fold);

  out.push({ key: "band:" + fold, html:
    '<div class="band" data-fold="' + fold + '" data-band="' + esc(word) + '"' +
      ' data-shut="' + (shut ? "yes" : "no") + '"' +
      ' style="padding-left:' + rail + "; --rail:" + rail + '"' +
      ' title="' + esc(note) + '">' +
      '<span class="twist">' + (shut ? "▶" : "▼") + "</span>" +
      '<span class="word">' + esc(word) + "</span>" +
      '<span class="count">' + count + "</span>" +
    "</div>" });

  /*  SHUT IS NOT HIDDEN-BUT-DRAWN: the rows are never pushed, so the reconciler
      takes their elements away and a shut header costs nothing to have. The
      count on the head is what says how many went, which is also §4.8's rule
      kept - the twist is a shape, the count is a number, and neither is a
      colour. */
  if (shut) return;

  lines();

  out.push({ key: "band:" + fold + ":end",
             html: '<div class="band-end" data-band="' + esc(word) + '"' +
                   ' style="--rail:' + rail + '"></div>' });
}

/*  What each of §13.6's six words means, said once so the row can be short. */
function prepareNote(word) {
  return ({
    preparing: "the horizon is getting this ready now",
    pending:   "waiting for a slot another cue is holding - it will land when that run ends",
    partial:   "part of this could not be got ready ahead and will happen at entry",
    armed:     "ready: its voice is reserved and its media is made ready",
    verified:  "ready, and every value sent ahead was read back equal",
  })[word] || word;
}


/*  THE THREE TIMES, AS COLUMNS SOMEBODY CAN TYPE IN (author, 2026-09-16).

    They were two words in the tail - `wait 2s`, `hold 3s` - which said what a
    cue does and could not be changed without opening it. A cue list is where
    the timing of a scene is read across rows rather than down one, so they are
    columns: the same three attributes the inspector offers, in the same order
    a cue lives them, aligned so a column can be read at a glance.

    APPLICABLE OR NOT IS THE TREE'S ANSWER, never a list of kinds kept here. A
    memo has no `duration` node, so its cell is empty; a media cue's duration is
    the file's length, published read-only (§4.10), so it is shown and not
    offered; a fade's and a stop's are decisions, so they are boxes. The day a
    kind gains or loses one of these rows, this follows without an edit.

    Each box is an ordinary field: `data-set` is the address, and the commit,
    the Escape and the "do not write under somebody's hands" rule are the ones
    gestures/fields.js already keeps for the inspector. */
function timeCell(id, name) {
  const node = tree.node("/godot/cue/" + id + "/" + name);

  if (!node) return '<div class="when"></div>';

  const writable = (Number(node.ACCESS) & 2) !== 0;
  const value = Array.isArray(node.VALUE) && node.VALUE.length ? node.VALUE[0] : "";
  const said = String(node.DESCRIPTION || "");
  const note = esc(name + (said ? " — " + said.split(". ")[0] : ""));

  /*  AND A READ-ONLY NOUGHT IS BLANK TOO, or one column would spell nothing in
      two ways: an empty box where a fade waits for none, and `0.0` where a
      media cue's file could not be read and its length is unknown. The title
      still names the attribute, so the empty cell can be asked. */
  if (!writable) {
    const number = Number(value);
    const shown = value === "" || !Number.isFinite(number) || number === 0
                    ? "" : number.toFixed(1);

    return '<div class="when ro" title="' + note + '">' + esc(shown) + "</div>";
  }

  return '<div class="when">' +
           '<input type="number" step="any" min="0" data-blank-zero="yes"' +
           ' data-set="/godot/cue/' + esc(id) + "/" + esc(name) + '"' +
           ' value="' + esc(Number(value) === 0 ? "" : value) + '"' +
           ' title="' + note + '">' +
         "</div>";
}

/*  ONE CUE ROW. `depth` is the indent and `standby` is the identifier the
    pointer is on for this list. The row, and the rows of a group's contents
    after it, go into `out` as a key and the markup, for `reconcile` to bring
    the pane into line with.

    `section` AND `rail` ARE THE FRAME THIS ROW IS DRAWN INSIDE - "header",
    "footer", "persistent", or "" for a plain member of a list or a group, which
    is most rows. They travel down the recursion rather than being read off the
    cue's own `role`, because a row is inside a frame when it is DRAWN inside
    one: a group sitting in a header has role "header", but its members have
    role "member" and are still drawn between that header's two band rows, so
    they have to carry the rail or the frame's left edge would break in the
    middle of it. A nested section of its own overrides both - a row draws one
    rail, and the innermost frame is the one it is in.

    This is the parameter that used to be `role`, passed by every caller and
    read by none. */
function cueRow(id, depth, section, rail, standby, out) {
  const kind = tree.cue(id, "kind", "memo");
  const isGroup = kind === "group";
  const open = isGroup && !folded.has(id);

  const enabled = tree.cue(id, "enabled", true) !== false;
  const number = esc(tree.cue(id, "number", ""));
  const name = esc(tree.cue(id, "name", "") || "—");

  const flags = [];

  if (isGroup) {
    const mode = tree.cue(id, "mode", "sequence");
    const advance = tree.cue(id, "advance", "manual");
    flags.push(mode === "timeline" ? "timeline" : advance === "auto" ? "auto" : "manual");
  }

  if (!enabled) flags.push("disabled");

  /*  A cue somebody else can fire. Worth a mark on the row rather than only in
      the inspector: an operator looking down a list needs to know which of
      these can go off without them. */
  const triggers = tree.triggersOf(id);

  /*  And a cue the engine cannot promise has its slot to itself. */
  const shares = overlapping.get(id) || [];

  /*  THE TENDRIL, in the cheapest form this page can draw one: the member's own
      row says which header gets it ready. The author asked for "some form of
      tendril showing the header it's in"; a line drawn between two rows in a
      scrolling list is a thing to maintain, and naming the header is the same
      fact said in words - which is also what §4.8 wants. */
  const preparedBy = tree.cue(id, "preset", "");

  /*  AND WHETHER THAT HEADER ACTUALLY SHOWS IT, which is not the same question.

      `preset` names an ANCESTOR, and a value naming anything else is a `wfg
      validate` warning the engine tolerates rather than refuses - the grammar
      says so in as many words, "the repair is somebody dragging it somewhere
      sensible and yesterday's show must still open". So the mark can name a
      group whose `headerDerived` does not list this cue: one drag, or one typo
      in the inspector's own `preset` box, and there it is.

      The MARK is still worth saying - somebody wrote it and PRD §4.10 says the
      document holds what they decided. The LINK is not, because there is
      nothing at the far end of it: `openForKey` would open that group and its
      header section, throwing away whatever the reader had folded, and show
      them a section the line is not in. A way in to a place the page cannot
      reach is worse than no way in. */
  const reachable = !!preparedBy &&
    tree.ids("/godot/cue/" + preparedBy + "/headerDerived").indexOf(id) >= 0;

  /*  HOW FAR AHEAD THIS ONE HAS BEEN GOT, in the word the engine uses.

      PRD §3.12's horizon reaches a block before anybody presses anything, and
      what an operator needs from it is on the row rather than in an inspector:
      whether the next scene is ready, and whether it is only partly ready. In
      WORDS and never colour alone (§4.8) - `pending` in particular, which is a
      cue waiting for a slot somebody else holds and is the one an operator has
      to be able to read at a glance and act on.

      `idle` is the resting state and says nothing, which is most rows most of
      the time. */
  const prepare = tree.cue(id, "prepare", "idle");

  const twist = isGroup
    ? '<span class="twist" data-fold="' + id + '">' + (open ? "▼" : "▶") + "</span>"
    : '<span class="twist"></span>';

  /*  CHOSEN, AND WHICH ONE OF THE CHOSEN IS THE ANCHOR.

      EVERY row of a range draws `data-picked`, not just the one clicked last: a
      range that marked a single row would be a selection whose size the reader
      cannot see, and the next thing anybody does with a range is drag it or
      delete it. THE ANCHOR is told apart because it is the row a shift-click
      measures from - extend the range and it is the end that stays put - so it
      answers "where will this grow from" before the gesture rather than after.

      Both are attributes and the look is the stylesheet's to give. §4.8 asks
      that whatever it gives the anchor is not a colour on its own. */

  /*  AND THE ROW CAN BE LIFTED (author, 2026-09-16: "can we drag and drop cues
      or groups to reorder them?").

      The inspector's ▲▼ buttons declined this once and named the reason
      honestly - "a row that moves under the pointer while the tree is being
      re-fetched is a fight nobody wins" - so that, and not the dragging, is
      what this round had to answer. It is answered in `renderLists`, where the
      pane is held still for as long as the hand is down. What a row owes after
      that is one attribute saying it may be picked up.

      EVERY REAL ROW CARRIES IT, the cues in a header, in a footer and in the
      persistent band included. Those can be dragged OUT of their section: a cue
      says its `parent`, that parent is the GROUP or the LIST rather than the
      section object, and both of those have an address, so `object.move` can
      name where such a cue is going. What no client can name is the section
      itself, and a drop INTO one is therefore not expressible - but that is a
      refusal about the TARGET under the pointer, not about the row in the hand.
      A row nothing may be dropped against is still a row somebody may want to
      lift.

      AND `data-in` IS NOT THAT REFUSAL, though it sits right here and looks
      like it. It says which frame a row is DRAWN in, which is not the same fact
      as which container holds it: a group sitting in a header has members of
      its own, drawn on that header's rail and carrying `data-in="header"`,
      whose parent is the group and which reorder among themselves perfectly
      well. The question "may this be dropped against" is the cue's own `role`,
      published per cue beside its `parent` and its `kind`. Asked of the tree at
      the moment of the drop it is one poll old; baked into this markup it would
      be a value the reconciler rewrites every poll, on five hundred rows, to
      say what the tree was going to say anyway.

      AND THE POSITION A DROP SENDS IS A PLACE IN `order`, which is the sequence
      the recursion below walks - NOT the `index` node a cue also publishes.
      They are the same number on nearly every show and they are not the same
      thing: `index` is counted by the tree's own walk, which steps over a
      trigger without spending a number on it, so on a group holding a trigger
      the two differ by one from the trigger down. `object.move` is read against
      `order`, the publisher says so in as many words, and `order` is what this
      pane draws from - so the list a drop counts in is the list a reader is
      looking at, which is the only way the arithmetic can be checked by eye. */
  out.push({ key: "cue:" + id, html:
    '<div class="row" data-pick="' + id + '" draggable="true"' +
      (section ? ' data-in="' + section + '" style="--rail:' + rail + '"' : "") +
      (revealing("cue:" + id) ? ' data-flash="yes"' : "") +
      ' data-standby="' + (id === standby ? "yes" : "no") + '"' +
      ' data-picked="' + (chosen.has(id) ? "yes" : "no") + '"' +
      ' data-anchor="' + (id === selection.picked ? "yes" : "no") + '"' +
      ' data-enabled="' + (enabled ? "yes" : "no") + '">' +
      '<div class="gutter" data-park="' + id + '" title="park the standby here"></div>' +
      '<div class="number num">' + number + "</div>" +
      '<div class="name" style="padding-left:' + (depth * 16) + 'px">' +
        twist +
        '<span class="kind">' + esc(kind) + "</span>" +
        '<span class="text">' + name + "</span>" +
      "</div>" +
      '<div class="tail">' +
        flags.map((f) => '<span class="flag">' + esc(f) + "</span>").join("") +
        (triggers.length
           ? '<span class="flag trig" title="' + triggers.length +
             ' trigger(s)">\u26A1' + (triggers.length > 1 ? triggers.length : "") + "</span>"
           : "") +
        (preparedBy
           ? '<span class="flag preset"' +
             (reachable ? ' data-reveal="preset:' + preparedBy + ":" + id + '"' : "") +
             ' title="' +
             esc(reachable
                   ? "got ready by the header of " + cueName(preparedBy) +
                     " \u2014 click to go to that line, opening what has to open"
                   : "marked to be got ready by " + cueName(preparedBy) +
                     ", which is not a group this cue is inside \u2014 so no header" +
                     " prepares it, and it runs at its own moment as though the mark" +
                     " were not there") +
             '">\u2191 ' + esc(cueName(preparedBy)) + "</span>"
           : "") +
        (prepare && prepare !== "idle"
           ? '<span class="flag ' + (prepare === "pending" ? "warn" : "ready") +
             '" title="' + esc(prepareNote(prepare)) + '">' + esc(prepare) + "</span>"
           : "") +
        (shares.length
           ? '<span class="flag warn" title="' +
             esc(shares.map((s) => "slot " + s.slot + " is also claimed by " + cueName(s.other))
                       .join("; ") +
                 " \u2014 mark either Feed or Insert shared if that is meant") +
             '">shares ' + shares.length + "</span>"
           : "") +
        (id === standby ? '<span class="flag standby-word">standby</span>' : "") +
      "</div>" +
      timeCell(id, "preWait") + timeCell(id, "duration") + timeCell(id, "postWait") +
    "</div>" });

  if (!isGroup || !open) return;

  const header = tree.ids("/godot/cue/" + id + "/headerOrder");
  const footer = tree.ids("/godot/cue/" + id + "/footerOrder");

  /*  THE LINES NOBODY WROTE. A member marked `preset` for this group is got
      ready by this header and still runs where it sits - so it appears here in
      italics, as a reading of the mark rather than as a cue of its own, and
      clicking it picks the member AND goes to the member's own row, which is
      the thing that can be edited. Derived lines come first, because that is
      the order the horizon prepares them in: a written header cue may
      reasonably depend on what the presets set. */
  const derived = tree.ids("/godot/cue/" + id + "/headerDerived");
  /*  WHERE A BAND'S LABEL SITS, AND WHERE ITS FRAME STANDS: the indent, plus
      the gutter and the number column that every row above it carries. Those
      two are scaled by the type knob, so the offset has to be as well - written
      as a calc rather than as the 92 it comes to at --type 1, or the labels
      walk left of the rows they head the moment the type moves. The same
      figure is the frame's `--rail`, so the rule down the side of a section
      stands at the label's own x however the type is set. */
  const band = "calc(" + ((depth + 1) * 16) + "px + 12px + 80px * var(--type))";

  if (header.length || derived.length) {
    frame(out, id + ":header", "header", derived.length + header.length, band,
          "runs before the members - the italic lines are got ready here and run where" +
          " they sit in the list",
          () => {
            derived.forEach((child) => presetLine(child, id, depth + 1, band, out));
            header.forEach((child) => cueRow(child, depth + 1, "header", band, standby, out));
          });
  }

  /*  AND THE MEMBERS BETWEEN THEM STAY PLAIN, carrying whatever frame this
      group itself is in and no other. The two frames are what delimits the
      sections; a third one around the middle would be a box drawn around
      "everything else", which is not a section and has no word to head it. */
  tree.ids("/godot/cue/" + id + "/order").forEach((child) =>
    cueRow(child, depth + 1, section, rail, standby, out));

  if (footer.length) {
    frame(out, id + ":footer", "footer", footer.length, band,
          "runs after the members, and after an Esc as well - a graceful abort takes this" +
          " same path, entered early (§4.4)",
          () => {
            footer.forEach((child) => cueRow(child, depth + 1, "footer", band, standby, out));
          });
  }
}

/*  A DERIVED HEADER LINE: the member, shown where it is got ready.

    In italics and marked `preset`, because it is not a cue sitting in this
    header - it is a reading of a mark on a cue that lives somewhere else in the
    list. It carries the same `data-pick` as the member's own row, so clicking
    it selects the member and the inspector opens the thing that can actually be
    edited. There is one object, and this is a second view of it - which is why
    its key names the group whose header it is drawn in: `cue:<id>` is already
    the member's own row.

    AND IT SAYS WHERE THAT OTHER ROW IS (author, 2026-09-16: "I could get the
    focus of a header item with the actual cue"). `data-reveal` names the
    member's own key, so the click that picks the member also opens whatever
    has to be open for that row to exist and takes the reader to it. It is a
    second view of one object, and now it is one that can be got out of.

    It is always a header line, so it carries the header's frame without being
    asked which section it is in.

    AND IT IS DRAWN CHOSEN WHEN THE MEMBER IS, anchor and all, because there is
    one object and a reader who cannot see that both lines are the same cue is
    the reader this view was drawn for. It asks the set by the member's id, so
    it needs no place in the range of its own.

    AND IT IS THE ONE ROW THAT CANNOT BE DRAGGED, NOR DROPPED AGAINST - the one
    place in this pane where the drag has to be told something the tree would
    otherwise answer wrongly rather than not at all.

    It carries the member's `data-pick` on purpose, because a click on it means
    the member; and that is exactly what makes it dangerous to a drop. Ask the
    tree about that identifier and it answers truthfully about the member's OWN
    row - a parent and an index somewhere else entirely in the list - so a drop
    aimed just above or just below this line would be computed against a place
    the pointer is nowhere near, and would land there, silently and correctly by
    its own arithmetic. This line is a reading of a mark, not a place in an
    order. There is no order here to insert into: the derived lines are in the
    sequence `headerDerived` publishes, which is derived from marks on cues that
    live elsewhere and is not a thing `object.move` can write.

    So it says so twice over, because the two halves of a drag ask in two
    different ways. No `draggable`, which settles the SOURCE - a div is not
    draggable unless it says it is, and nothing above it in this pane is either.
    And `data-derived` for the TARGET, where the row under the pointer is being
    read rather than lifted and an absent attribute proves nothing at all. */
function presetLine(id, group, depth, rail, out) {
  out.push({ key: "preset:" + group + ":" + id, html:
    '<div class="row derived" data-pick="' + id + '" data-derived="yes"' +
      ' data-reveal="cue:' + id + '"' +
      ' data-in="header" style="--rail:' + rail + '"' +
      (revealing("preset:" + group + ":" + id) ? ' data-flash="yes"' : "") +
      ' data-picked="' + (chosen.has(id) ? "yes" : "no") + '"' +
      ' data-anchor="' + (id === selection.picked ? "yes" : "no") + '">' +
      '<div class="gutter"></div>' +
      '<div class="number num">' + esc(tree.cue(id, "number", "")) + "</div>" +
      '<div class="name" style="padding-left:' + (depth * 16) + 'px">' +
        '<span class="twist"></span>' +
        '<span class="kind">' + esc(tree.cue(id, "kind", "memo")) + "</span>" +
        '<span class="text">' + esc(tree.cue(id, "name", "") || "\u2014") + "</span>" +
      "</div>" +
      '<div class="tail">' +
        '<span class="flag" title="got ready by this header; it runs where it sits in the' +
        ' list">preset</span>' +
      "</div>" +
      '<div class="when"></div><div class="when"></div><div class="when"></div>' +
    "</div>" });
}

/*  EVERY ROW OF ONE LIST, as keys and markup and nothing else.

    Split out of `renderLists` so that what this pane DECIDES can be read
    without a browser: which rows a list draws, in what order, inside which
    frames, with which of them shut. The rest of `renderLists` - the tabs, the
    pane, the reconcile, the pass over the fields - is what it does with them,
    and needs a document.

    IT ASKS FOR THE OVERLAP INDEX ITSELF rather than being handed it, so that a
    test needs nothing but a served tree; the index is built once per call and
    read by every row, which is why it is not built per row. */
function listRows(focus, standby) {
  overlapping = tree.overlaps();
  chosen = new Set(Array.isArray(selection.chosen)
                     ? selection.chosen
                     : selection.picked ? [selection.picked] : []);

  const cues = tree.ids("/godot/list/" + focus + "/order");
  const persistent = tree.ids("/godot/list/" + focus + "/persistentOrder");

  /*  EMPTY ONLY WHEN BOTH ARE. The engine publishes the persistent section
      apart from `order`, so a list whose only cues are persistent - a bed and
      nothing else - has an empty `order`. Reading `order` alone called that
      list empty and hid its cues, which were drawn nowhere else as rows. */
  if (!cues.length && !persistent.length) {
    return [{ key: "empty", html:
      '<div class="empty"><div class="line">Rien à faire.</div>' +
      '<div class="under">Nothing in this list.</div></div>' }];
  }

  const out = [];

  /*  THE PERSISTENT SECTION, AT THE HEAD OF THE LIST and marked as its own
      thing (§3.29) (author, 2026-09-16: "I would place the persistent container
      towards the top since this is something that runs as soon as the show
      starts").

      It was drawn at the foot, and the reason it was there has not stopped
      being true: these cues are not on the order the pointer walks, GO never
      reaches one, and a band among the ordered rows is a place the pointer
      looks like it could stop. The reason it is now at the head is the other
      half of §3.29 - a persistent cue is what should be running at ALL times,
      checked after every trigger and put back when it is not - so it is already
      running before anybody has pressed anything. What is already on is what
      somebody reading a list from the top needs first: the ordered rows say
      what will happen, and these say what is under them while it does.

      SO THE BAND'S TITLE HAS TO SAY BOTH FACTS, which is also where §4.8 is
      kept here: the word heads the frame, the count says how many there are,
      and the title says both that these are checked after every trigger and put
      back when they are not, and that GO never reaches one. Without that second
      half a reader meeting them first would take the top of the list for the
      first thing GO does, which is the one wrong thing this move could teach.

      It is a section like a header or a footer, so it is drawn as one and folds
      like one. Its rail is the offset a depth-0 row's name sits at - the same
      calc a group's band uses, with the indent taken out - so the frame stands
      under the names rather than out at the pane's edge.

      ITS FOLD KEY NAMES THE LIST, because a second list has a persistent
      section of its own and the two are shut and opened apart; and because the
      key has to be told from a cue's, which is what the "list:" in front of it
      is for. */
  if (persistent.length) {
    const rail = "calc(12px + 80px * var(--type))";

    frame(out, "list:" + focus + ":persistent", "persistent", persistent.length, rail,
          "already running before anybody presses anything: checked after every trigger," +
          " and put back when it is not as declared — and GO never reaches one, so the" +
          " list the pointer walks starts below this band",
          () => {
            persistent.forEach((id) => cueRow(id, 0, "persistent", rail, standby, out));
          });
  }

  cues.forEach((id) => cueRow(id, 0, "", "", standby, out));

  return out;
}

function renderLists() {
  const lists = tree.ids("/godot/list/order");
  const focus = tree.get("/godot/list/focus", "") || lists[0] || "";

  /*  THE TABS ARE KEPT AS THE ROWS ARE, and for the same reason: each tab is
      something to click - `list.focus`, or the `+` that makes a list - and a
      tab written afresh on every poll can be swapped for a new element
      between the press and the release, so the click does nothing. */
  const tabs = lists.map((id) => ({ key: "tab:" + id, html:
    '<div class="list-tab" data-focus="' + id + '"' +
      ' data-focused="' + (id === focus ? "yes" : "no") + '">' +
      (id === focus ? '<span class="mark">▸ </span>' : "") +
      esc(tree.get("/godot/list/" + id + "/name", id)) +
    "</div>" }));

  tabs.push({ key: "tab:+", html:
    '<div class="list-tab" data-newlist="yes" title="a second list runs beside this one">+</div>' });

  reconcile(el("tabs"), tabs);

  /*  AND HERE THE CUE PANE STOPS FOR AS LONG AS THE HAND IS DOWN.

      `view.holding` is set by gestures/drag.js between `dragstart` and
      `dragend`, and while it is set this render draws no rows at all. That is
      not caution about a rare case: a drag is one gesture held across dozens of
      polls, and this pane's reconciler spends those polls moving rows and
      rewriting the ones whose markup changed. Either of those, done to the row
      the pointer is over, breaks the gesture in a way nobody can see going
      wrong - the row the hand was aimed at between one poll and the next is a
      different element, so the drop is read against whatever now stands in that
      place. It is the click that straddles a poll (views/reconcile.js), held
      for a second instead of a tenth, and with a structural edit at the end of
      it rather than a pick.

      WHAT IS SUSPENDED IS ONE PANE'S RECONCILE AND NOTHING ELSE. The poll goes
      on fetching, the strip, the aim, the runs and the inspector go on being
      drawn from it, and the next poll after `dragend` brings this pane into
      line with everything that arrived meanwhile - which is what a reconciler
      is for and why this costs no recovery code. The tabs are drawn above this
      line and go on drawing: they are a different container, nothing a tab says
      moves a row in the cue pane, and a list somebody else renames mid-drag
      should still say its new name.

      EVERYTHING BELOW IT IS THE CUE PANE, and all of it waits:

      - the rows, which is the point;
      - the two empty states, which are rows in this same pane put there by this
        same reconcile. A show whose last list is closed under a dragging hand
        would otherwise have the pane emptied beneath the drag. Held, the rows
        stand a moment stale and the drop that follows names a cue the engine no
        longer has - which the engine refuses, in words, which is the honest end
        to that story;
      - the reveal's scroll, which MUST wait: taking the reader to a row they
        were sent to is the one thing on this page that moves the list under the
        pointer on purpose. Nothing can ask for one mid-drag anyway, a reveal
        being set by a click, and the ask is latched on `selection.reveal`
        rather than timed - so one somehow left pending is honoured on the poll
        after the drop, not lost;
      - `refreshFields`, which is the tail of a reconcile that did not happen.
        It writes values and moves nothing, so it would be safe to run; it is
        skipped because there is nothing newly drawn for it to write into and
        nobody reads a number in a column while their hand is on a row.

      AND IT CLEANS UP AFTER NOBODY. The line between two rows, or the frame
      round a group being dropped into, is the drag module's to insert while the
      reconciler is still, and the drag module's to take away at `dragend`: a
      row whose markup is what it was is not touched by the next reconcile at
      all, element, attributes and all, so anything put on one by hand survives
      until that row's markup changes - and then goes without notice. Neither
      half of that is a cleanup anybody can lean on.

      A FLAG THAT IS NOT THERE IS NOT SET. `view.holding` is undefined on a page
      whose drag module never loaded, and in a test that stands a document in;
      undefined draws. */
  if (view.holding) return;

  const pane = el("cues");

  /*  THE EMPTY STATE IS A ROW LIKE ANY OTHER, keyed `empty`, so the same
      reconcile that draws the rows takes it away the moment the first one
      arrives, and puts it back when the last one goes. */
  if (!lists.length) {
    pane.dataset.list = "";

    reconcile(pane, [{ key: "empty", html:
      '<div class="empty"><div class="line">Rien à faire.</div>' +
      '<div class="under">This show has no cue list yet.</div></div>' }]);
    return;
  }

  const standby = tree.get("/godot/list/" + focus + "/standby", "");

  /*  AND THE PANE SAYS WHICH LIST THESE ROWS ARE, which stopped being the same
      question as which list is focused the moment the pane could be held still.

      A drop below the last row means the end of the list the reader is looking
      at, and the obvious way to name that list is to ask the tree for
      `/godot/list/focus`. During a drag that is the wrong question: focus is a
      value in the document, a second operator or a script can move it, and the
      answer would then be a list whose rows are not the ones under the hand -
      so a cue would be moved into the end of a list nobody was pointing at, by
      arithmetic that was correct throughout. This attribute is what the rows on
      screen were drawn from, which is the only list a pointer can be over, and
      it is set here rather than per row because it is one fact about the whole
      pane. Being on the container it is also out of the reconciler's reach,
      which is what lets it survive the hold along with the rows it describes. */
  pane.dataset.list = focus;

  reconcile(pane, listRows(focus, standby));

  /*  AND THE READER IS TAKEN TO THE ROW THEY ASKED FOR, once, after the rows
      exist. It cannot be done before the reconcile: the row at the other end of
      a tendril is very often one that was not on screen at all a moment ago -
      that is why `openForKey` had to unfold something to reach it - so there is
      nothing to scroll to until the pane has been brought into line.

      THE ROW IS FOUND BY WALKING THE CHILDREN and comparing `data-key`, not by
      building a selector. The pane is one flat list of keyed rows, so its
      children ARE the whole of the candidates, and comparing the key as a
      string is exactly what `reconcile` wrote there - including the `#2` it
      appends to a key it was asked for twice - with no selector syntax in
      between to quote, escape or get wrong.

      `block: "nearest"` is the whole point of the gesture: a row already on
      screen is not moved, so following a tendril to something the reader can
      already see does not throw the list about under them. And `scrolled` is
      set on the reveal rather than the scroll being repeated every poll, or a
      reader who scrolled away during the second the highlight lasts would be
      dragged back ten times.

      Guarded both ways because neither is certain: the row may not be there
      (the reveal was for a cue this poll's tree no longer has), and a document
      stood in by a test has no `scrollIntoView`. */
  const reveal = selection.reveal;

  if (reveal && !reveal.scrolled) {
    const found = Array.from(pane.children)
                       .find((child) => child.dataset && child.dataset.key === reveal.key);

    if (found) {
      if (typeof found.scrollIntoView === "function") found.scrollIntoView({ block: "nearest" });

      /*  AND THE MARK IS LIT AGAIN FROM THE TOP. A second ask for a row that is
          still marked changes no attribute - `data-flash` is already "yes" -
          and a CSS animation restarts only when its NAME goes from none to
          something, so the wash would not run again and the second ask would
          draw nothing at all. Taken off, the layout flushed, put back. It
          belongs here rather than in the markup because this is the one block
          that happens exactly once per ask, which is what `scrolled` latches;
          and the row is left carrying exactly what the markup says, so the next
          poll's `morph` has nothing to undo. */
      if (found.hasAttribute("data-flash") && typeof found.offsetWidth === "number") {
        found.removeAttribute("data-flash");
        void found.offsetWidth;
        found.setAttribute("data-flash", "yes");
      }

      reveal.scrolled = true;
    }
  }

  /*  AND THE VALUES IN THE TIME BOXES, after the rows are in place.

      A box that has been typed in once keeps the value it was given - the
      browser stops reflecting the `value` attribute into a field the moment
      anybody touches it - so an undo, or a second operator's edit, would never
      reach a box that had ever been used. The inspector has had this pass
      since it grew fields; this is the same one (views/values.js), and it
      leaves alone whatever has the focus or holds an uncommitted edit. */
  refreshFields(pane);
}

export { listRows, renderLists, timeCell };
