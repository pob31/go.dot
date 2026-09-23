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

/*  THE INSPECTOR: every field of the picked cue or trigger, built out of what
    each node says about itself (§14.2), and the gestures that act on it - and,
    when more than one cue is chosen, the fields they have in common, with the
    mixed-value semantics §3.5 asks for. */

import { tree } from "../plumbing/tree.js";
import { str } from "../plumbing/osc.js";
import { panel } from "../model/remember.js";
import { selection } from "../model/selection.js";
import { el, esc, cueName } from "./common.js";
import { isList, shownValue, commitText, refreshFields } from "./values.js";

/*  Every address in the tree that belongs to these objects, whatever owner word
    publishes each one. A media cue carries `/godot/cue/<id>/name` and
    `/godot/media/<id>/file` at once, and the inspector shows both without ever
    being told that media cues have files.

    ONE WALK FOR HOWEVER MANY ARE ASKED FOR, which is why the identifier is read
    out of the address rather than written into the pattern. A panel over
    several cues has to know which fields they have in COMMON before it knows
    what shape it is, so these lists are rebuilt on every poll whether or not
    anything is redrawn - and a walk per cue would be fifty walks of every
    address a five-hundred-cue show publishes, ten times a second. */
function fieldsForAll(ids) {
  const found = new Map();
  const pattern = /^\/godot\/([a-z]+)\/([^/]+)\/([A-Za-z]+)$/;

  for (const id of ids) found.set(id, []);

  for (const address of Object.keys(tree.at)) {
    const match = pattern.exec(address);
    if (!match) continue;

    const mine = found.get(match[2]);
    if (!mine) continue;

    const node = tree.at[address];
    if (!node || (typeof node.TYPE !== "string" && !isList(node))) continue;   // a container

    mine.push({ address: address, owner: match[1], name: match[3], node: node });
  }

  return ids.map((id) => found.get(id));
}

function fieldsFor(id, kind) {
  return fieldsForAll([id])[0].sort(inWorkingOrder(kind));
}

/*  THE ORDER SOMEBODY WORKS IN, which is not the order the tree is in.

    The tree is alphabetical inside each owner, and the panel used to be as
    well: a fade's `curve` sat five rows above its `points` and a group's
    `advance`, `mode` and `selection` were scattered down the list with `loops`
    and `play` between them, so writing one cue meant hunting for the next
    thing to say (author, 2026-09-16). Time reads worst of all alphabetically -
    `postWait` before `preWait`, and `duration` under another owner word
    entirely - though it is the one order everybody already knows: what happens
    before, how long, what happens after.

    SO THE PANEL IS FOUR BLOCKS, in the order somebody fills them in:

      what it is        number, name, shortName, colour, notes
      when              preWait, duration, postWait
      what it does      the kind's own rows, each kind in its own working order
      in the list       enabled, preset

    `duration` joins the timing block whatever kind publishes it, which is the
    whole reason the blocks are not the owner words they were: a fade's
    duration belongs between its waits and not under a heading of its own.

    KEYED ON THE KIND AND NOT ON THE OWNER WORD, which is a correction to the
    first cut of this: every attribute a cue carries publishes under
    `/godot/cue/<id>/`, whatever owner the parameter table files it under, so
    the owner here is `cue` for all of them and a table keyed on `fade` or
    `group` matched nothing. The kind is what the cue itself says it is.

    WRITTEN DOWN ONCE, HERE, AS DATA. Reordering a kind is a line in a table
    rather than a change to any code, which is what makes this cheap to argue
    about with the page open. And a row NOBODY HAS NAMED still appears: it
    falls to the end of its block, alphabetically, so a new row in the
    parameter table shows up in a predictable place instead of vanishing. */
const WHEN = ["preWait", "duration", "postWait"];

const SAID_FIRST = ["number", "name", "shortName", "colour", "notes"];
const SAID_LAST = ["enabled", "preset"];

/*  A SAMPLER MEMBER'S ROWS AFTER EVERYTHING A MEDIA CUE HAS (Phase 6): the DCA
    it answers to, then what a hand on its strip does, in the order a press
    happens. A group's `takeover` sits beside `mode`, the answer that makes it a
    question; a fade's `dca` beside `target`, the other thing it can move. */
const KIND_ORDER = {
  media:   ["file", "level", "startOffset", "dca", "initialLevel", "release", "secondPress",
            "velocity", "velocityFloor", "pressure", "releaseFade",
            // The EQ (Phase 9a): the switch, the two filters, then four bands - frequency,
            // gain and width each, the outer two with a shape. The desktop draws these
            // as a curve at the foot; here they are rows, in the order a hand reads them.
            "eqOn", "eqHpf", "eqHpfFreq", "eqLpf", "eqLpfFreq",
            "eqB1Shape", "eqB1Freq", "eqB1Gain", "eqB1Q",
            "eqB2Freq", "eqB2Gain", "eqB2Q",
            "eqB3Freq", "eqB3Gain", "eqB3Q",
            "eqB4Shape", "eqB4Freq", "eqB4Gain", "eqB4Q"],
  fade:    ["target", "dca", "level", "curve", "points", "stopWhenDone"],
  stop:    ["target", "verb", "curve"],
  start:   ["target"],
  osc:     ["address", "value", "wait", "timeout"],
  midi:    ["port", "channel", "type", "data1", "data2", "sysex", "wait"],
  group:   ["mode", "takeover", "advance", "selection", "play", "loops", "seed", "dca"],
  range:   ["name", "in", "out", "loops"],
  trigger: ["kind", "enabled", "address", "value", "port", "channel",
            "type", "number", "data", "at"],
  // A cue's insert and an entry of the show's plugin set (Phase 9a): the row a hand
  // reads first is the one that says which plugin, then the switch, then the values -
  // one sparse row here; the p<n> nodes beneath it are the door a rotary takes.
  fx:      ["plugin", "enabled", "values", "name", "index"],
  plugin:  ["name", "identifier", "format", "path", "preset", "state", "problem",
            "latencySamples", "paramCount"],
};

/*  THE NAMES A KIND CLAIMS - or, when several cues are chosen at once, the
    names any of their kinds claims, in the order the kinds were given. Two
    fades and two stops share `target` and `curve`, and those rows belong in
    the kinds' block, where somebody looks for what a cue DOES, rather than
    swept to the end with `enabled` and `preset` because no single kind's table
    was consulted. One kind is still one kind: `KIND_ORDER[kind] || []`,
    unchanged. */
function claimedBy(kind) {
  if (!Array.isArray(kind)) return KIND_ORDER[kind] || [];

  const names = [];

  for (const one of kind) {
    for (const name of KIND_ORDER[one] || []) if (names.indexOf(name) < 0) names.push(name);
  }

  return names;
}

/*  Which block a field belongs to, for a cue (or a trigger) of this kind. A
    name the kind claims is the kind's; the timing three are always the timing
    three; what is left is the cue itself, before or after. */
function blockOf(field, kind) {
  if (WHEN.indexOf(field.name) >= 0) return 1;
  if (claimedBy(kind).indexOf(field.name) >= 0) return 2;
  if (SAID_FIRST.indexOf(field.name) >= 0) return 0;

  return 3;
}

/*  Where it sits inside that block. A name no table carries answers `-1`,
    which sorts after every name that is carried - and alphabetically among its
    own, by the comparator below. */
function rankOf(field, block, kind) {
  if (block === 0) return SAID_FIRST.indexOf(field.name);
  if (block === 1) return WHEN.indexOf(field.name);
  if (block === 2) return claimedBy(kind).indexOf(field.name);

  return SAID_LAST.indexOf(field.name);
}

function inWorkingOrder(kind) {
  return (a, b) => {
    const blockA = blockOf(a, kind);
    const blockB = blockOf(b, kind);

    if (blockA !== blockB) return blockA - blockB;

    const rankA = rankOf(a, blockA, kind);
    const rankB = rankOf(b, blockB, kind);

    if (rankA !== rankB) return (rankA < 0 ? Infinity : rankA) - (rankB < 0 ? Infinity : rankB);

    return a.name.localeCompare(b.name);
  };
}

/*  The heading over each block. The kind's own block wears the kind's own word
    - `media`, `fade`, `group` - because that is what the namespace calls it
    and §14.2 asks a client to teach the namespace rather than a vocabulary of
    its own. The first block has none: it is the cue itself.

    SEVERAL KINDS WEAR ALL OF THEIR WORDS, joined: "fade + stop" over the rows
    two fades and two stops all carry. Naming only the first would tell a
    reader they were editing fades, which is exactly the misunderstanding a
    bulk edit must not leave anybody in. */
function headingOf(block, kind) {
  if (block === 0) return null;
  if (block === 1) return "when";
  if (block === 2) return Array.isArray(kind) ? kind.join(" + ") : kind;

  return "in the list";
}

/*  WHAT SOMEBODY DECIDED, AND WHAT THE ENGINE SAYS BACK.

    PRD §4.10 draws this line for the document - "the document holds what
    someone decided, never what the machine happened to be doing" - and the
    parameter table already carries the answer per row: a value anybody may
    write is a decision, and a read-only one is derived from the structure or
    observed from a file. A cue's `index`, `parent`, `role` and `kind`, a media
    cue's `duration` and its `hash`: none of those is something an operator
    sets, and all of them were sitting in the same list as the name and the
    level (author, 2026-09-16).

    So the read-only ones go behind a fold. They are not hidden - a pointer at
    the wrong parent is exactly what somebody opens an inspector to find out -
    and they are not editable anywhere else either. They are simply not the
    page an operator is reading when they pick a cue.

    ASKED OF THE NODE AND NEVER OF A LIST OF NAMES HERE. A row that becomes
    writable, or arrives writable, moves by itself.

    AND OF EVERY NODE, WHERE A FIELD STANDS FOR SEVERAL CUES: read-only for any
    of them is read-only for all. A box that wrote to three of four and was
    refused by the fourth would be the worst of both answers - some of the show
    changed, with nothing on screen saying which - so the row is a reading for
    all four and says what it says behind the fold. */
function decided(field) {
  return (field.nodes || [field.node]).every((node) => (Number(node.ACCESS) & 2) !== 0);
}

function fieldMarkup(field) {
  return '<div class="field"><label title="' + esc(field.node.DESCRIPTION || "") + '">' +
         esc(field.name) + "</label>" + controlFor(field) + "</div>";
}

/*  The blocks above, each wrapped with its own heading, in the order they are
    worked through.

    WRAPPED RATHER THAN INTERLEAVED, and that is what lets the stylesheet put a
    heading BESIDE its fields instead of over them when there is width for it
    (author, 2026-09-16: "the labels can go on the same row as the parameters
    in the foot orientation to avoid having to scroll too much"). Four headings
    over four blocks cost four rows of a pane that is a third of the screen
    tall; beside them they cost none. A flat list of headings and fields could
    not do it: a heading has to know which fields it heads to sit next to them.

    `headings` is false inside the details fold, which is a short list of
    readings where a heading per block would be more furniture than rows. */
function fieldsMarkup(fields, kind, headings) {
  if (headings === false) return fields.map(fieldMarkup).join("");

  let out = "";
  let said;
  let block = "";

  const close = () => (said === undefined ? "" :
    '<div class="block">' +
      '<div class="group-head">' + (said === null ? "" : esc(said)) + "</div>" +
      '<div class="fields">' + block + "</div>" +
    "</div>");

  for (const field of fields) {
    const heading = headingOf(blockOf(field, kind), kind);

    if (heading !== said) {
      out += close();
      said = heading;
      block = "";
    }

    block += fieldMarkup(field);
  }

  return out + close();
}

/*  THE FOLD ITSELF, with the identifier at the top of it. An id is the one
    thing in here nobody decided and everybody needs occasionally - it is what
    a log record, a refusal and a script all name a cue by - so it is the first
    line inside rather than a word beside the title.

    ALL OF THEM WHEN SEVERAL ARE CHOSEN, up to a dozen, because the reason to
    open this fold over a selection is to take the identifiers somewhere else -
    a script, a message, a note about what went wrong. Past a dozen the row
    would be longer than the panel and nobody is copying fifty by eye, so it
    says how many more there are and stops. */
const NAMED_IDS = 12;

function detailsMarkup(ids, fields, kind) {
  const many = ids.length > 1;
  const said = ids.length > NAMED_IDS
    ? ids.slice(0, NAMED_IDS).join(" ") + " … and " + (ids.length - NAMED_IDS) + " more"
    : ids.join(" ");

  return '<details class="derived"' + (panel.details ? " open" : "") + ">" +
           '<summary data-details="yes">details</summary>' +
           '<div class="field"><label title="the identifier' +
           (many ? "s these cues are" : " this cue is") +
           ' known by, in every record and every command">id' + (many ? "s" : "") + "</label>" +
           '<div class="ro num">' + esc(said) + "</div></div>" +
           fieldsMarkup(fields, kind, false) +
         "</details>";
}

function controlFor(field) {
  const node = field.node;
  const writable = decided(field);
  const value = shownValue(node);
  const range = Array.isArray(node.RANGE) && node.RANGE.length ? node.RANGE[0] : null;
  const set = ' data-set="' + esc(field.address) + '"';

  /*  EVERY CUE THIS ONE CONTROL STANDS FOR, on the control itself: the first
      address is the one `data-set` already names, and the rest are what
      `sayMixed` reads to find out whether they agree and what a commit writes
      to after gestures/fields.js has sent the first. A field of one cue
      carries none of this and is drawn exactly as it always was. */
  const many = Array.isArray(field.all) && field.all.length > 1;
  const all = many ? ' data-all="' + esc(field.all.join(" ")) + '"' : "";

  /*  READ-ONLY IS SHOWN AND NOT HIDDEN. `kind`, `parent`, `index` and `role`
      are derived rather than decided (§4.10), and they are exactly what
      somebody looks at when a cue is not where they thought it was - which is
      why they are behind the fold rather than gone.

      AND IT IS KEPT TRUE. Such a value has no box for `refreshFields` to write
      into, so it used to be drawn once and left: a cue's `prepare` word, a
      media cue's `duration`, would say what they said when the cue was picked
      for as long as it stayed picked. `data-read` is the address it goes on
      saying. */
  if (!writable) {
    return '<div class="ro" data-read="' + esc(field.address) + '"' + all + ">" +
           esc(value === "" ? "—" : value) + "</div>";
  }

  if (range && Array.isArray(range.VALS)) {
    /*  THE WORD ITSELF AS AN OPTION, for a menu over cues that do not agree
        (§4.8: an unchosen menu and a chosen one differ by nothing a
        photograph would show). Disabled, so that nobody can ask for "mixed" -
        it is not a value any parameter takes - and first, so `sayMixed` can
        show it by index. `sayMixed` is also what selects it: a menu drawn
        showing one cue's answer and left there would be a quiet lie about the
        other three. */
    return "<select" + set + all + ">" +
      (many ? '<option value="" disabled data-mixed="yes">' + MIXED + "</option>" : "") +
      range.VALS.map((v) =>
        '<option value="' + esc(v) + '"' + (String(v) === String(value) ? " selected" : "") +
        ">" + esc(v) + "</option>").join("") +
      "</select>";
  }

  if (node.TYPE === "T" || node.TYPE === "F") {
    /*  A TICK BOX HAS A THIRD STATE AND NO WORD FOR IT. `indeterminate` is a
        dash in a box, which is a shape and not a word, so the word goes
        beside it (§4.8) and is shown or hidden by `sayMixed` as the answer
        changes under the panel. The wrapper keeps the field a two-column
        grid: a label and one thing beside it. */
    if (many) {
      return '<span class="pair"><input type="checkbox"' + set + all +
             (value === true ? " checked" : "") + '>' +
             '<span class="ro mixed" data-word="' + MIXED + '" hidden>' + MIXED + "</span></span>";
    }

    return '<input type="checkbox"' + set + (value === true ? " checked" : "") + ">";
  }

  /*  Before the number test, which an empty list's TYPE would pass: "" is
      found at the start of "ifdh". */
  if (isList(node)) {
    return '<input type="text" class="list"' + set + all + ' value="' + esc(value) + '">';
  }

  if ("ifdh".indexOf(node.TYPE) >= 0) {
    const step = "ih".indexOf(node.TYPE) >= 0 ? "1" : "any";
    const min = range && range.MIN !== undefined ? ' min="' + esc(range.MIN) + '"' : "";
    const max = range && range.MAX !== undefined ? ' max="' + esc(range.MAX) + '"' : "";

    return '<input type="number" step="' + step + '"' + min + max + set + all +
           ' value="' + esc(value) + '">';
  }

  return '<input type="text"' + set + all + ' value="' + esc(value) + '">';
}

/*  Every word `cue.create` takes. `midi` was missing until the desktop's
    new-cue row was built from the same list (2026-09-18) and the two clients
    were held to one answer: the engine has made MIDI cues since Phase 4. */
const KINDS = ["memo", "media", "fade", "transport", "osc", "midi", "group", "start"];

/*  ─────────────────────────────────────────────── several cues at once ──

    WHAT N CUES HAVE IN COMMON, AND WHAT EDITING THEM COSTS.

    §3.5 asks for exactly this and words it in one line: "show the value when
    all members agree, show mixed otherwise, and typing sets all". The model is
    model/selection.js's - `picked` is the ANCHOR, one cue or none, and `chosen`
    is every cue chosen in drawn order, the anchor among them. One chosen cue is
    the page's whole life and is drawn by the code below this, unchanged, on
    purpose: a rewrite that made the ordinary case subtly different would be a
    bad trade for a panel somebody opens now and then.

    N SEPARATE WRITES, AND N SEPARATE UNDOS. A commit here sends one `node.set`
    datagram per cue, because that is all the engine offers: there is no bulk
    command and nobody has asked for one. So an edit to four cues is four
    transactions in the history, and taking it back is four presses of ctrl/⌘-Z.
    That is worth writing down rather than hiding: the alternative is an engine
    change, and the honest thing in the meantime is to say so where somebody
    reaching for the gesture will read it - the panel's own title sentence, and
    the delete button's. Recorded here, not built.

    WHAT IS NOT BUILT, AND WHERE IT WOULD GO. §3.5 also gives the group view a
    filter by contained kind - "all audio, all video, all OSC, all MIDI" - so
    that a mixed group offers one kind's fields rather than the thin
    intersection of everything. That belongs with the group shortcut, which is a
    second route into this same panel and is not this round's; until it exists a
    selection of two kinds shows what the two kinds share, which is honest and
    sometimes very little. */

const MIXED = "mixed";

/*  THE FIELD NAMES EVERY LIST CARRIES, in the order the first list gives them.
    Small and total on purpose: no lists at all is nothing in common, one list
    is that list, and lists with nothing in common are empty rather than an
    error. A name repeated within the first list is answered once - two owners
    can publish the same word for one cue, and two identical rows in a panel
    would be two boxes writing to different addresses under one label. */
function intersect(lists) {
  if (!Array.isArray(lists) || !lists.length) return [];

  const first = lists[0] || [];
  const common = [];

  for (const name of first) {
    if (common.indexOf(name) >= 0) continue;
    if (lists.every((list) => Array.isArray(list) && list.indexOf(name) >= 0)) common.push(name);
  }

  return common;
}

/*  Whether N values are all the same value. Nothing and one thing agree with
    themselves; everything else is compared as text, because that is how a
    control shows it and the question this answers is what a control should
    show. */
function agree(values) {
  if (!Array.isArray(values) || values.length < 2) return true;

  const first = String(values[0]);

  return values.every((value) => String(value) === first);
}

/*  One row per field the chosen cues all carry, each holding every address it
    stands for. The control's shape - its type, its range, its sentence - is
    taken from the first cue: those come from one row of the parameter table,
    so every cue that has the field has the same row. What is NOT taken from
    the first cue is whether anybody may write it (`decided` asks all of them).

    SORTED BY THE KINDS PRESENT rather than by the first cue's kind, so the
    blocks the rows fall into and the headings over them are the same
    arrangement. */
function sharedFields(ids, kinds) {
  const lists = fieldsForAll(ids);
  const names = intersect(lists.map((list) => list.map((field) => field.name)));
  const shared = [];

  for (const name of names) {
    const each = lists.map((list) => list.find((field) => field.name === name));

    shared.push({
      address: each[0].address,
      owner: each[0].owner,
      name: name,
      node: each[0].node,
      nodes: each.map((field) => field.node),
      all: each.map((field) => field.address),
    });
  }

  return shared.sort(inWorkingOrder(kinds));
}

/*  WHERE THE PANEL SAYS "MIXED", and it is said after every refresh rather than
    drawn once into the markup, because agreement is a live question: another
    client - or the second, third and fourth write of this panel's own commit -
    can bring four values together or take them apart while the panel stands
    open.

    NOT UNDER SOMEBODY'S HANDS AND NOT OVER A COMMIT IN FLIGHT. Both rules are
    views/values.js's and gestures/fields.js's, and both are asked here rather
    than copied: the field with the focus is left alone, a field holding an
    uncommitted edit is left alone, and a field whose last commit the tree has
    not caught up with is left alone until `refreshFields` has decided whether
    that commit took. Without the last of those, a value typed into a mixed box
    would be blanked again a tenth of a second later - by this loop, reading
    four cues that have not all answered yet - which reads exactly like an edit
    that did not take. */
function sayMixed(pane) {
  for (const control of pane.querySelectorAll("[data-all]")) {
    const values = [];

    for (const address of String(control.dataset.all).split(" ")) {
      const node = tree.node(address);

      if (node) values.push(shownValue(node, control));
    }

    const mixed = values.length > 1 && !agree(values);

    /*  A READING RATHER THAN A CONTROL, behind the fold. `refreshFields` keeps
        it true whenever the cues agree, and this is the other answer. */
    if (control.dataset.read !== undefined) {
      if (mixed && control.textContent !== MIXED) control.textContent = MIXED;
      continue;
    }

    if (control === document.activeElement || control.dataset.dirty === "yes") continue;
    if (control.dataset.sent !== undefined) continue;

    if (control.type === "checkbox") {
      const word = control.nextElementSibling;

      control.indeterminate = mixed;

      if (word && word.dataset.word === MIXED) word.hidden = !mixed;
      continue;
    }

    if (control.tagName === "SELECT") {
      /*  The disabled option `controlFor` put first. Where they agree the
          refresh has already chosen the option that says so. */
      if (mixed) control.selectedIndex = 0;
      continue;
    }

    /*  AN EMPTY BOX WITH THE WORD IN IT. The placeholder is put on and taken
        off as the answer changes, and never written into the markup, because a
        box whose four cues all hold "" would then claim they disagreed. */
    if (mixed) {
      control.value = "";
      if (control.placeholder !== MIXED) control.placeholder = MIXED;
    } else if (control.placeholder) {
      control.removeAttribute("placeholder");
    }
  }
}

/*  THE WRITE HALF AND THE COMMAND TABLE, ASKED FOR AT THE MOMENT THEY ARE USED
    rather than imported at the top of this file.

    plumbing/link.js reads `location` as it loads - it has to know whether this
    page was served by the engine at all before it opens a socket - and
    gestures/table.js imports it. This module is imported by the console's
    tests, which give it the least DOM the markup needs and no `location`
    whatever, so a static import would make the whole panel untestable and buy
    nothing: nothing here sends anything until somebody commits an edit or
    presses delete, and by then the browser has had the module for minutes.

    THE FIRST OF THE N IS NOT SENT HERE, and that is the shape of the whole
    feature: the control carries the first address in `data-set` and the first
    identifier in `data-delete`, so gestures/fields.js commits it under all its
    own rules - what Escape takes back, what a menu does when it is picked from,
    what the in-flight mark says - and gestures/clicks.js deletes it as the
    named command it already is (§4.11). This sends the SAME thing to the rest.
    Two handlers, one rule, and none of that rule copied. */
function writeRest(addresses, text) {
  if (!addresses.length) return;

  import("../plumbing/link.js").then((link) => {
    for (const address of addresses) link.setNode(address, text);
  });
}

function deleteRest(ids) {
  if (!ids.length) return;

  import("../gestures/table.js").then((commands) => {
    for (const id of ids) commands.gesture("delete", [str(id)]);
  });
}

/*  Wired to the pane the first time it is drawn, and never again: the element
    itself outlives every render - only its contents are replaced - so one
    wiring holds for the life of the page. On the pane rather than on the
    document because these two gestures exist nowhere else, and at render time
    rather than at import because a module that wires listeners as it loads is a
    module that cannot be read by anything but a browser. */
function wirePane(pane) {
  if (!pane || pane.dataset.wired === "yes" || typeof pane.addEventListener !== "function") return;

  pane.dataset.wired = "yes";

  pane.addEventListener("change", (event) => {
    const target = event.target;
    const all = target && target.dataset ? target.dataset.all : undefined;

    if (!all) return;

    /*  ESCAPE NEVER WRITES, and it must not write to the other three either.
        gestures/fields.js marks the field while it takes the focus away. */
    if (target.dataset.abandoning === "yes") return;

    writeRest(String(all).split(" ").slice(1), commitText(target));
  });

  pane.addEventListener("click", (event) => {
    const button = event.target && event.target.closest
                     ? event.target.closest("[data-delete-all]") : null;

    if (!button) return;

    deleteRest(String(button.dataset.deleteAll).split(" ").slice(1));
  });
}

/*  THE STRUCTURE BUTTONS OVER A SELECTION, and what each of them acts on.

    DELETE IS THE ONLY ONE THAT MEANS ALL OF THEM. The rest are aimed at the
    ANCHOR and say so in their own title, because "move four cues one earlier"
    has no single obvious meaning - four cues in three groups, two of them
    adjacent, moved one place towards a neighbour that is itself moving - and a
    button that did something defensible but unguessable to a show is worse
    than a button that does one thing. The anchor is the cue somebody clicked
    last, which is the one their pointer is already on.

    A GROUP'S OWN BUTTONS ARE NOT OFFERED HERE - `+ media inside`, `+ header`,
    `+ footer`. They are about one group and its sections; a panel whose
    subject is four cues is not where somebody is arranging one of them. Pick
    the group on its own and they are all there.

    AND THE ANCHOR-ONLY ONES ARE DRAWN ONLY WHILE THERE IS AN ANCHOR. Every one
    of them is carried out by gestures/clicks.js against `selection.picked` -
    that is what `data-move` and `data-add` mean - so a panel that drew them
    while what is picked was NOT one of the chosen would aim them at a cue it
    is not showing. The selection model says the anchor is always among the
    chosen; this is what the panel does if that ever stops being true, and it
    is silence rather than a wrong guess. */
function manyStructure(anchor, chosen) {
  let out = '<div class="group-head">structure</div><div class="actions">';

  if (anchor) {
    const parent = tree.cue(anchor, "parent", "");
    const siblings = tree.ids((tree.node("/godot/list/" + parent + "/order")
                                 ? "/godot/list/" : "/godot/cue/") + parent + "/order");
    const at = siblings.indexOf(anchor);
    const said = esc(cueName(anchor));
    const rest = chosen.length - 1;
    const only = " — " + said + " only, not the other " + (rest === 1 ? "one" : rest);

    out += '<button data-park="' + esc(anchor) + '" title="GO will act on ' + said +
           ' — the cue this selection is anchored on">▸ standby here</button>';

    out += '<button data-move="' + (at - 1) + '" data-parent="' + esc(parent) + '"' +
           (at > 0 ? "" : " disabled") + ' title="one earlier' + only + '">▲</button>';
    out += '<button data-move="' + (at + 1) + '" data-parent="' + esc(parent) + '"' +
           (at >= 0 && at < siblings.length - 1 ? "" : " disabled") +
           ' title="one later' + only + '">▼</button>';

    for (const kind of KINDS) {
      out += '<button data-add="' + kind + '" data-into="' + esc(parent) + '"' +
             ' title="after ' + said + only + '">+ ' + kind + "</button>";
    }
  }

  /*  ALL OF THEM, ONE AT A TIME, and the title says what that costs before
      anybody presses it. The first identifier is gestures/clicks.js's, which
      is where the named command and the clearing of what is picked already
      live; the rest are this file's. */
  out += '<button class="danger" data-delete="' + esc(chosen[0]) + '" data-delete-all="' +
         esc(chosen.join(" ")) + '" title="deletes all ' + chosen.length +
         ', one at a time: taking that back is ' + chosen.length +
         ' presses of undo">delete ' + chosen.length + "</button>";

  return out + "</div>";
}

/*  THE PANEL ITSELF. Rebuilt only when its shape changes, for the reason the
    one-cue panel is: the poll arrives up to ten times a second and a panel
    rebuilt on each one would take the cursor out of the box somebody is typing
    in. WHAT IS MIXED IS NOT PART OF THE SHAPE - it is said by `sayMixed` into
    the panel that stands - so four values coming together under a commit
    redraw nothing. */
function renderMany(pane, chosen) {
  const anchor = chosen.indexOf(selection.picked) >= 0 ? selection.picked : "";
  const kinds = chosen.map((id) => tree.cue(id, "kind", "memo"));
  const present = [];

  for (const kind of kinds) if (present.indexOf(kind) < 0) present.push(kind);

  const fields = sharedFields(chosen, present);
  const signature = "many|" + chosen.join(" ") + "|" + anchor + "|" + present.join("+") + "|"
                      + (panel.details ? "open" : "shut") + "|"
                      + fields.map((f) => f.address).join(",");

  if (pane.dataset.showing !== signature) {
    pane.dataset.showing = signature;

    /*  HOW MANY, AND OF WHAT. "4 cues" answers the first question and the
        tally answers the second - two fades and two memos, which is what tells
        a reader why the panel is showing them six rows instead of a fade's
        eleven. */
    const tally = present.map((kind) =>
      kinds.filter((one) => one === kind).length + " " + kind).join(" · ");

    let out =
      '<div class="who" title="every edit here is written to each of the ' + chosen.length +
      ' separately: taking one back is ' + chosen.length + ' presses of undo">' +
      '<span class="text">' + chosen.length + ' cues</span>' +
      '<span class="kind">' + esc(tally) + "</span></div>";

    out += fieldsMarkup(fields.filter(decided), present, true);
    out += detailsMarkup(chosen, fields.filter((field) => !decided(field)), present);

    /*  THE TRIGGERS SECTION IS ABOUT ONE CUE, so over a selection it is a
        heading and nothing else. A trigger is an object with an identifier of
        its own and its own page, four cues' triggers in one list would say
        nothing about which cue each belonged to, and the `+ osc` buttons send
        `add-trigger` at whatever is picked - so a section drawn here would
        quietly aim at the anchor while looking like it meant all four. The
        heading stays because a section that vanished with no word would be a
        small mystery every time somebody chose a second cue. */
    out += '<div class="group-head">triggers — one cue at a time</div>';

    out += manyStructure(anchor, chosen);

    pane.innerHTML = out;
    sayMixed(pane);
    return;
  }

  refreshFields(pane);
  sayMixed(pane);
}

/*  WHICH OF THE CHOSEN ARE STILL THERE, in the order they were chosen and each
    of them once. A selection is the page's own (§14.1) and the document is
    not: cues are deleted from here, from another client and from a script, and
    `chosen` is not told. So the panel asks the tree rather than trusting the
    list, which is also what empties it after a delete of all N - the ids stop
    being cues, nothing is chosen, and the pane says there is nothing to
    declare. */
function chosenCues() {
  const asked = Array.isArray(selection.chosen) ? selection.chosen : [];
  const live = [];

  for (const id of asked) {
    if (live.indexOf(id) < 0 && tree.node("/godot/cue/" + id + "/kind")) live.push(id);
  }

  return live;
}

/*  What a trigger says in one line, which is enough to tell two of them apart
    without opening either. */
function triggerSummary(id) {
  const kind = tree.trigger(id, "kind", "osc");

  if (kind === "osc") return tree.trigger(id, "address", "") || "no address";
  if (kind === "clock") return tree.trigger(id, "at", "") || "no time";

  const channel = Number(tree.trigger(id, "channel", 0));

  return tree.trigger(id, "type", "noteOn") + " " + tree.trigger(id, "number", 0) +
         (channel ? " ch " + channel : " any ch");
}

function renderInspector() {
  const pane = el("inspect");

  wirePane(pane);

  /*  A TRIGGER IS INSPECTED LIKE ANYTHING ELSE, because it is addressed like
      anything else: the fields come from `/godot/trigger/<id>/*` through the
      same lookup that reads a cue's. What it needs of its own is a way back to
      the cue it belongs to. */
  if (selection.picked && tree.node("/godot/trigger/" + selection.picked + "/kind")) {
    const owner = tree.trigger(selection.picked, "cue", "");
    const kind = "trigger";
    const fields = fieldsFor(selection.picked, kind);
    const signature = "trigger|" + selection.picked + "|" + (panel.details ? "open" : "shut")
                        + "|" + fields.map((f) => f.address).join(",");

    if (pane.dataset.showing !== signature) {
      pane.dataset.showing = signature;

      let out =
        '<div class="who"><span class="text">' + esc(triggerSummary(selection.picked)) +
        '</span><span class="kind">trigger</span></div>' +
        '<div class="back" data-pick="' + esc(owner) + '">\u2190 ' +
        esc(tree.cue(owner, "name", "") || owner) + "</div>";

      out += fieldsMarkup(fields.filter(decided), kind, true);
      out += detailsMarkup([selection.picked], fields.filter((f) => !decided(f)), kind);

      out += '<div class="group-head">structure</div><div class="actions">' +
             '<button class="danger" data-delete="' + esc(selection.picked) + '">delete</button></div>';

      pane.innerHTML = out;
      return;
    }

    refreshFields(pane);
    return;
  }

  /*  MORE THAN ONE CUE CHOSEN, and that is the only thing that sends the panel
      down the other road. One chosen cue - which is what an ordinary click
      leaves, and what the page spends its life in - falls straight through to
      the panel below, byte for byte the one it has always drawn. */
  const chosen = chosenCues();

  if (chosen.length > 1) {
    renderMany(pane, chosen);
    return;
  }

  if (!selection.picked || !tree.node("/godot/cue/" + selection.picked + "/kind")) {
    pane.innerHTML = '<div class="empty"><div class="line">Nothing to declare.</div>' +
                     '<div class="under">Click a cue to see what it says.</div></div>';
    pane.dataset.showing = "";
    return;
  }

  const kind = tree.cue(selection.picked, "kind", "memo");
  const fields = fieldsFor(selection.picked, kind);
  /*  THE FOLD IS PART OF THE SHAPE, so opening it redraws the panel rather
      than waiting for the selection to change under it. */
  const signature = selection.picked + "|" + (panel.details ? "open" : "shut") + "|"
                      + fields.map((f) => f.address).join(",");

  /*  REBUILT ONLY WHEN THE SHAPE CHANGES, and that is not an optimisation. The
      poll arrives up to ten times a second, and rebuilding the panel on each one
      would take the cursor out of the box somebody is typing in - which is the
      ordinary way a live-updating editor becomes unusable. So the markup is
      made once per selection, and the values below are refreshed in place:
      never into a field that has the focus, and never over an edit that has not
      been committed. */
  if (pane.dataset.showing !== signature) {
    pane.dataset.showing = signature;

    const parent = tree.cue(selection.picked, "parent", "");
    const isGroup = kind === "group";
    const hasHeader = tree.ids("/godot/cue/" + selection.picked + "/headerOrder").length > 0;
    const hasFooter = tree.ids("/godot/cue/" + selection.picked + "/footerOrder").length > 0;

    let out =
      '<div class="who"><span class="text">' + esc(tree.cue(selection.picked, "name", "") || "—") +
      '</span><span class="kind">' + esc(kind) + "</span></div>";

    out += fieldsMarkup(fields.filter(decided), kind, true);
    out += detailsMarkup([selection.picked], fields.filter((f) => !decided(f)), kind);

    /*  THE CUE'S TRIGGERS, listed rather than folded into the fields above:
        they are objects with identifiers of their own, a cue may have several,
        and each is edited on its own page. §3.7 gives the list to any cue. */
    out += '<div class="group-head">triggers</div>';

    for (const trigger of tree.triggersOf(selection.picked)) {
      const off = tree.trigger(trigger, "enabled", true) === false;

      out += '<div class="trigger-row" data-pick="' + trigger + '">' +
             '<span class="kind">' + esc(tree.trigger(trigger, "kind", "osc")) + "</span>" +
             '<span class="what' + (off ? " off" : "") + '">' +
             esc(triggerSummary(trigger)) + "</span></div>";
    }

    out += '<div class="actions">';

    for (const k of ["osc", "midi", "clock"]) {
      out += '<button data-trigger="' + k + '">+ ' + k + "</button>";
    }

    out += "</div>";

    /*  UP AND DOWN, which is `object.move` at one index either way. Dragging
        is what a desktop UI will do and is not what a first pass should try
        over a poll: a row that moves under the pointer while the tree is being
        re-fetched is a fight nobody wins. Two buttons say the same thing and
        cannot half-happen.

        The parent goes with the index because `object.move` takes both, and
        passing the one it already has is what makes this a REORDER rather than
        a reparent - the same command, aimed at the place it is. */
    const siblings = tree.ids((tree.node("/godot/list/" + parent + "/order")
                                 ? "/godot/list/" : "/godot/cue/") + parent + "/order");
    const at = siblings.indexOf(selection.picked);

    out += '<div class="group-head">structure</div><div class="actions">';

    /*  PARK, FIRST AND BY ITSELF. The gutter does this too and is quicker once
        somebody knows it is there; this is where they find out. A cue the
        pointer may not stand on - inside an automatic group, in a header - is
        refused by the engine and the refusal says which, which is a better
        answer than a button that is not offered. */
    out += '<button data-park="' + esc(selection.picked) + '" title="GO will act on this cue">' +
           "\u25B8 standby here</button>";

    out += '<button data-move="' + (at - 1) + '" data-parent="' + esc(parent) + '"' +
           (at > 0 ? "" : " disabled") + ' title="one earlier">▲</button>';
    out += '<button data-move="' + (at + 1) + '" data-parent="' + esc(parent) + '"' +
           (at >= 0 && at < siblings.length - 1 ? "" : " disabled") +
           ' title="one later">▼</button>';

    /*  A NEW CUE GOES AFTER THIS ONE, in this one's parent, which is where a
        designer writing a list wants it and is the only guess that needs no
        second click. A group is offered the other reading as well: inside. */
    for (const k of KINDS) {
      out += '<button data-add="' + k + '" data-into="' + esc(parent) + '"' +
             ' title="after this cue">+ ' + k + "</button>";
    }

    if (isGroup) {
      out += '</div><div class="actions">';

      for (const k of KINDS) {
        out += '<button data-add="' + k + '" data-into="' + esc(selection.picked) + '"' +
               ' title="inside this group">+ ' + k + " inside</button>";
      }

      out += '</div><div class="actions">';
      if (!hasHeader) out += '<button data-role="header">+ header</button>';
      if (!hasFooter) out += '<button data-role="footer">+ footer</button>';
    }

    out += '<button class="danger" data-delete="' + esc(selection.picked) + '">delete</button></div>';
    pane.innerHTML = out;
    return;
  }

  refreshFields(pane);
}

export { renderInspector, decided, blockOf, headingOf, inWorkingOrder, intersect, agree };
