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

/*  A ROW CARRIED TO WHERE IT GOES (author, 2026-09-16: "can we drag and drop
    cues or groups to reorder them? Guard against out of order presets").

    THE PAGE TURNED THIS DOWN ONCE AND NAMED THE REASON, at the inspector's two
    arrows: "a row that moves under the pointer while the tree is being
    re-fetched is a fight nobody wins". That sentence is the whole difficulty
    and it is answered rather than forgotten. `view.holding` is set here between
    `dragstart` and `dragend`, and views/didi.js draws no rows at all while it
    is set - so the rows a hand is aiming between stand exactly where they stood
    when it went down. Nothing else stops: the poll goes on fetching, and the
    strip, the aim, the running pane and the inspector go on being drawn from
    it, so the show is still visible while somebody rearranges a list.

    HTML5 DRAG AND DROP AND NOT POINTER EVENTS, and that same flag is why. The
    browser owns the drag image, the Escape key that abandons a drag and the
    cursor that says whether a drop is possible - and, the part that decides it,
    a drag that ends ANYWHERE fires `dragend`: off the window, over another
    application, on Escape, on a window that loses the focus. A held reconciler
    that could be left held is a console frozen in the middle of a show, so the
    release has to be a promise the browser makes and not one this file makes.
    Every exit path here goes through `release`, which is idempotent for the
    same reason.

    IT IS A MOUSE GESTURE, AND ONLY THAT, which is worth saying plainly because
    this page is meant to be operated from a tablet. HTML5 drag and drop does
    not exist under a finger: a touch fires no `dragstart`, and there is no
    polyfill here to make one. So a tablet keeps the inspector's ▲ and ▼, which
    send the same `object.move` one member at a time, and so does the keyboard.
    §4.11 is kept by the COMMAND and not by the gesture - `object.move` is
    named, is in gestures/commands.json, is logged, and is one ctrl/⌘-Z away,
    however a hand reached it.

    ONE CUE PER DRAG, even when several rows are chosen. Moving a set has an
    index arithmetic of its own and, before that, a meaning nobody has decided:
    do five cues taken from three different groups land contiguously, or does
    each keep its distance from the one above it? Half of an answer to that
    would be worse than none - it would be a rearrangement somebody would have
    to undo cue by cue - so the drag moves the row that was picked up, and says
    so in the label while the hand is still holding it. The selection is not
    touched either way: a drag fires no `click`, so what was chosen before the
    drag is what is chosen after it.

    WHAT THIS FILE CANNOT OFFER, and refuses in words rather than by doing
    nothing. A <Header>, a <Footer> and a <Persistent> element carry an
    identifier in the file and have NO ADDRESS in the tree - /godot/cue/<that
    id> is a 404 - so no client can learn one, and `object.move` cannot be told
    to put a cue in one. A drop INTO a section, and a reorder WITHIN a section,
    are therefore not expressible at all. Dragging a cue OUT of one is: a cue's
    `parent` names the GROUP or the LIST rather than the section object, and
    both of those have addresses. That is a refusal about the TARGET under the
    pointer and never about the row in the hand. */

import { tree } from "../plumbing/tree.js";
import { int, str } from "../plumbing/osc.js";
import { selection } from "../model/selection.js";
import { view } from "../views/view.js";
import { el, cueName } from "../views/common.js";
import { gesture } from "./table.js";

/*  THE WHOLE OF THE DROP ARITHMETIC, and the one place a drag is usually wrong.

    `object.move <id> <parent> <index>` takes a MEMBER POSITION: a place in the
    sequence `/godot/cue/<id>/order` or `/godot/list/<id>/order` publishes, which
    is the sequence this pane draws and the only one a client can see. Since
    2026-09-16 the engine reads it that way too (document/Sequence.h), and what
    it does with it, verified there by exhaustive simulation, is exactly this:

        take the member list, REMOVE the moved cue if it is in it,
        then insert it at min(index, length).

    SO THE LIST THE POSITION IS COUNTED IN IS THE LIST WITHOUT THE CUE IN IT,
    and that is the whole of it. Members [A, B, C, D], A dragged to just after
    C: remove A and the rest is [B, C, D], where C stands at 1, so the position
    is 2 - not 3. Counted against the list with A still in it, C stands at 2 and
    the answer comes out one too far. It is wrong only when the drag goes
    DOWNWARD within one parent, which is precisely the case a hurried hand test
    does not try, so the removal is done here, first, and in the open.

    A TARGET THAT IS NOT IN THE LIST answers with an end rather than with a
    number that means nothing: nought for "before", the length for "after". That
    is what a drop into a group as its first member asks for - no target, and
    "before everything" - and it is what the pane's own background asks for
    below the last row. The callers never ask it about a row they have not
    already found in this list; the totality is here so that a caller that got
    it wrong lands at an end somebody can see rather than in the middle of a
    scene.

    AND A DROP ON THE ROW BEING DRAGGED IS WHERE IT ALREADY IS. The cue is taken
    out of the list before the target is looked for, so its own row would be a
    target that is not there and would answer with an end - a cue that jumped to
    the top of its group because somebody let go of it a pixel from where they
    picked it up. `landingFor` refuses that drop before it gets here; this
    answers it honestly anyway, because a rule that reads correctly on its own
    is one fewer thing to hold in mind. */
function dropIndex(members, draggedId, targetId, side) {
  const all = Array.isArray(members) ? members : [];

  if (targetId && targetId === draggedId) {
    const here = all.indexOf(draggedId);

    return here < 0 ? 0 : here;
  }

  const rest = all.filter((one) => one !== draggedId);
  const at = rest.indexOf(targetId);

  if (at < 0) return side === "after" ? rest.length : 0;

  return side === "after" ? at + 1 : at;
}

/*  WHICH OF THE ANSWERS A ROW HAS, from where in its height the pointer is.

    Two for an ordinary row: the upper half is before it and the lower half is
    after it, which is what every list on every desk does and what a hand
    already expects.

    THREE FOR A GROUP, because a group is also a place. Its middle third is
    "inside, as the first member" and the thirds either side are before and
    after it, which gives each of the three about nine pixels at the type this
    page is drawn at - enough to aim at, and evenly divided so that no band is
    the one that is hard to hit. The alternative was a horizontal test, dropping
    to the right of the name to mean inside, and it was not taken: it is
    invisible until somebody is told about it, and a list is read by rows.

    A FRACTION AND NOT A PIXEL COUNT, so the answer does not change with the
    type knob, and so this is a rule a test can ask about without a browser. */
function sideFor(fraction, thirds) {
  if (thirds) {
    if (fraction < 1 / 3) return "before";
    if (fraction > 2 / 3) return "after";

    return "into";
  }

  return fraction < 0.5 ? "before" : "after";
}

/*  EVERYTHING OVER A CUE, nearest first, ending with the list it is in.

    The same walk model/remember.js makes to unfold a row's containers, and for
    the same reason: `parent` is published per cue and derived rather than
    stored, so it cannot disagree with the tree the rows are drawn from. A
    parent that publishes no `kind` is the LIST, which is the top, and it is
    included - a cue's ancestors are its groups AND the list they are in, and
    the list is a perfectly good destination for a drop.

    BOUNDED AT SIXTY-FOUR STEPS, which is the same bound and the same argument:
    a `parent` chain is a tree and cannot loop, but this walks a tree read off
    the wire, and a tree read off the wire is whatever arrived. */
function chainAbove(id) {
  const up = [];
  let node = id;

  for (let step = 0; step < 64 && node; step += 1) {
    const parent = tree.cue(node, "parent", "");

    if (!parent) break;

    up.push(parent);

    if (!tree.node("/godot/cue/" + parent + "/kind")) break;

    node = parent;
  }

  return up;
}

/*  THE CUE BEING DRAGGED AND EVERYTHING INSIDE IT, as a set.

    Two questions ask it. A group may not be dropped into itself or into any of
    its own descendants - the engine refuses that too, in as many words, because
    without the refusal the tree stops being a tree - and the preset guard below
    has to look at every cue that is being carried and not only at the one under
    the pointer.

    EVERY SEQUENCE A CONTAINER PUBLISHES, not just `order`: a group's header and
    footer cues travel with it, and so does a persistent section if a container
    ever grows one. `tree.ids` answers nothing for an address the show does not
    have, so naming all four costs a map lookup each on a cue that has none.

    BOUNDED, AND VISITED-CHECKED, for `chainAbove`'s reason twice over: this
    walk goes downward, where a tree read off the wire that named a cue as its
    own descendant would not merely give a long answer but never finish. */
function subtreeOf(id) {
  const held = new Set();
  const todo = [id];

  while (todo.length && held.size < 4096) {
    const one = todo.pop();

    if (!one || held.has(one)) continue;

    held.add(one);

    for (const sequence of ["order", "headerOrder", "footerOrder", "persistentOrder"]) {
      for (const child of tree.ids("/godot/cue/" + one + "/" + sequence)) todo.push(child);
    }
  }

  return held;
}

/*  THE PRESET MARKS BEING CARRIED, which is the guard the author asked for by
    name.

    A cue's `preset` names an ANCESTOR group whose header gets it ready ahead of
    time (§3.12); the cue still runs where it sits. A value naming a group the
    cue is NOT inside is a warning `wfg validate` tolerates and the engine
    ignores - the grammar says so in as many words, "the repair is somebody
    dragging it somewhere sensible and yesterday's show must still open" - so
    the mark stays in the document and simply stops applying, and views/didi.js
    draws it inert. A drop that takes a cue out of the group its `preset` names
    does exactly that, silently, to a mark somebody wrote on purpose.

    IT IS ALLOWED AND IT IS SAID. Refusing would block the legitimate half of
    the same gesture - a scene lifted out of one act and dropped into another,
    presets and all - and `object.move` is one undo away in any case. What
    cannot happen is that it is DISCOVERED afterwards, so it is worked out for
    every destination the pointer passes over and written into the label the
    hand is already reading.

    WHAT APPLIES NOW IS THE ENGINE'S OWN ANSWER. `/godot/cue/<group>/headerDerived`
    lists the cues whose `preset` names that group and which are actually inside
    it, at any depth and through its sections; it is what draws the derived
    lines in the header and what decides whether the mark on a row is a link or
    an inert word. Asking it here rather than re-deriving ancestry means this
    warning and that row cannot come to disagree about which marks are live.
    What applies AFTER cannot be asked of anything - the move has not happened -
    so it is ancestry, computed from where the cue is going. The two tests are
    the same test on two trees, which is why the asymmetry is safe.

    A MARK NAMING A GROUP INSIDE THE THING BEING DRAGGED IS DROPPED HERE. Carry
    a whole act and the presets inside it travel with it: the group that
    prepares them moves too, so the mark is as true after the drop as before it,
    wherever the drop lands. Taking those out once at `dragstart` is also what
    keeps the per-pointer-move work to a chain walk. */
function carriedMarks(held) {
  const carried = [];

  for (const one of held) {
    const group = tree.cue(one, "preset", "");

    if (!group || held.has(group)) continue;
    if (tree.ids("/godot/cue/" + group + "/headerDerived").indexOf(one) < 0) continue;

    carried.push({ cue: one, group: group });
  }

  return carried;
}

/*  AND WHICH OF THEM THIS DESTINATION WOULD BREAK: the ones whose group is not
    among the containers the cue would then be inside. `chain` is the
    destination and everything over it, as a set. */
function brokenBy(carried, chain) {
  return carried.filter((one) => !chain.has(one.group));
}

/*  THE MEMBERS OF A CONTAINER, asked of whichever of the two publishes them.
    The same question gestures/clicks.js asks before an add, and the same way of
    telling a list from a group: the only honest difference between the two
    identifiers is which address answers. */
function membersOf(parent) {
  const asList = tree.node("/godot/list/" + parent + "/order");

  return tree.ids((asList ? "/godot/list/" : "/godot/cue/") + parent + "/order");
}

/*  A SECTION, SAID ALOUD. "a header", "a footer" - and "the persistent band",
    because there is one of those per list and "a persistent" is not English. */
function sectionSaid(word) {
  return word === "persistent" ? "the persistent band" : "a " + word;
}

/*  HOW DEEP A ROW IS DRAWN, which is how far the landing mark is indented. A
    top-level cue has one container over it (the list), so its depth is nought,
    and the mark then spans the pane. */
function depthOf(id) {
  return Math.max(0, chainAbove(id).length - 1);
}

/*  AND THE OFFSET THAT DEPTH COMES TO, in the one calc views/didi.js already
    uses for the rail of a section's frame: the indent, plus the gutter and the
    number column every row carries. Written as a calc rather than as the pixels
    it comes to, because those two columns are scaled by the type knob and a
    mark measured at one setting would stand off the names at another.

    NOTHING AT ALL AT THE TOP LEVEL, deliberately: with no `--rail` to read, the
    stylesheet's `margin-left: var(--rail)` is invalid at computed-value time
    and falls back to nought, so the mark spans the pane - which is what a drop
    at the top of a list should look like. A fallback length would have been
    worse than none, because it would have put the mark at SOME indent and the
    wrong one. */
function railFor(depth) {
  return depth > 0 ? "calc(" + (depth * 16) + "px + 12px + 80px * var(--type))" : "";
}

/*  WHAT IS BEING CARRIED, and nothing about it that the page could be asked
    for instead. `id` is empty exactly when no drag of ours is in the air, and
    every handler below asks it first. */
const drag = {
  id: "",                 // the cue being dragged
  row: null,              // its row, which wears the pale mark
  held: new Set(),        // that cue and everything inside it
  carried: [],            // the preset marks in there that a move could break
  landing: null,          // the drop the last `dragover` worked out
};

/*  THE MARK BETWEEN TWO ROWS, made once and moved about.

    IT LIVES IN THE PANE, among the rows, rather than floating over them: an
    insertion point is a place in a list, and a mark drawn at that place follows
    the list when it scrolls, sits at the indent of the container it is going
    into, and needs no second copy of where every row is on the screen. The
    stylesheet gives it two pixels of height and a pixel of negative margin at
    each end, so it takes no room and the rows either side of it do not shuffle
    as it walks down the list.

    THE RECONCILER WOULD TAKE IT AWAY, and that is a belt rather than the
    braces: anything in the cue pane that is not a keyed row is not wanted and
    goes (views/reconcile.js). It cannot happen while a drag is in the air,
    because that is exactly what `view.holding` stops - and `release` takes the
    mark out itself, which is what keeps this file honest about its own
    cleanup rather than leaning on the next poll to do it.

    ONE LABEL, TWO THINGS TO SAY. A refusal is `why` and a warning about a
    consequence is `warn`; the stylesheet draws the first in the refusal
    banner's red and the second in the amber unsaved work is said in, each with
    a border and each with the words as the actual carrier (§4.8). It is taken
    out of the mark when there is nothing to say, rather than emptied: an empty
    inline-block still carries its padding and its border, which would read as a
    small red nothing halfway along the line. */
let mark = null;
let label = null;

function theMark() {
  if (mark) return mark;

  mark = document.createElement("div");
  label = document.createElement("span");

  return mark;
}

/*  The row or band currently wearing an outline, so it can be taken off again
    without searching the pane for it. */
let outlined = null;

function unmark() {
  if (outlined) {
    outlined.classList.remove("drop-into");
    outlined.classList.remove("drop-no");
    outlined = null;
  }

  if (mark && mark.parentNode) mark.parentNode.removeChild(mark);
}

/*  WHERE A ROW'S BLOCK ENDS, which is not the row.

    The cue pane is ONE FLAT RUN of rows: an open group's members, its header
    band, the lines in it and its footer all follow the group's own row as
    siblings. So "after this group" is not the element after its row - that is
    the first thing INSIDE it - and a mark put there would tell a reader their
    cue was going somewhere it is not.

    THE WALK IS OVER THE RECONCILER'S KEYS, because they are the only account of
    what each row IS that survives being looked at from outside views/didi.js: a
    cue's row is `cue:<id>`, a derived line is `preset:<group>:<id>`, and a
    section's two edges are `band:<group>:header` and that with `:end`. Each of
    those names the cue or the group it belongs to in its second field, so a row
    is inside this block when that name is in the block's subtree - and the run
    of rows that are is contiguous, which is what lets the walk stop at the
    first one that is not.

    A KEY MADE UNIQUE IS STILL THE SAME KEY. `reconcile` appends `#2` to a key
    it is handed twice, which a tree that named one object twice would produce;
    the suffix is stripped rather than falling through to "not in this block",
    where it would end the walk early and put the mark inside the group.

    A LIST'S OWN PERSISTENT BAND is keyed `band:list:<list>:persistent`, whose
    second field is the word "list" and never a cue, so it ends the walk - which
    is right: that band belongs to the list and not to any group. */
function blockEnd(row, id) {
  const kind = tree.cue(id, "kind", "memo");

  if (kind !== "group") return row;

  const held = subtreeOf(id);
  let last = row;

  for (let next = row.nextElementSibling; next; next = next.nextElementSibling) {
    if (next === mark) continue;

    const key = String((next.dataset && next.dataset.key) || "").replace(/#\d+$/, "");
    const parts = key.split(":");
    const owner = parts[0] === "cue" || parts[0] === "preset" ? parts[1]
                : parts[0] === "band" && parts[1] !== "list" ? parts[1]
                : "";

    if (!owner || !held.has(owner)) break;

    last = next;
  }

  return last;
}

/*  The element the mark goes in front of, skipping the mark itself - it is a
    child of the pane like any other and would otherwise be measured from. */
function elementAfter(row) {
  let next = row.nextElementSibling;

  if (next === mark) next = next.nextElementSibling;

  return next;
}

/*  How far down a row the pointer is, as a fraction of its height. */
function fractionIn(row, y) {
  const box = row.getBoundingClientRect();

  return (y - box.top) / (box.height || 1);
}

/*  The row drawn from a given reconciler key, or nothing when that row is not
    on screen - a member of a folded group, most often. The pane's children ARE
    the whole of the candidates and the key is compared as the string
    `reconcile` wrote, with no selector syntax in between to quote or escape
    (views/didi.js takes the reveal's scroll to a row the same way). */
function rowWithKey(pane, key) {
  return Array.from(pane.children)
              .find((child) => child.dataset && child.dataset.key === key) || null;
}

/*  A DROP THAT CANNOT HAPPEN, said. `on` is what gets the dashed outline and
    `at` is where the mark sits, so the reader's eye is taken to the row the
    refusal is about rather than to a sentence floating at the pointer. */
function refused(on, at, rail, why) {
  return { on: on, mark: "drop-no", before: at, rail: rail,
           refused: true, said: "✕ " + why, parent: "", at: 0 };
}

/*  WHAT WOULD HAPPEN IF THE HAND LET GO HERE: the whole decision, made afresh
    on every `dragover` and recorded so that the drop sends exactly what the
    reader was last shown.

    It answers `null` for a pointer that is nowhere this gesture can act - off
    the cue pane entirely - where the mark is taken down and no drop is allowed;
    every other answer is either a refusal with its reason or a move with its
    parent and its member position. */
function landingFor(event) {
  const pane = el("cues");
  const target = event.target;

  if (!pane || !target || !target.closest || !target.closest("#cues")) return null;

  /*  A SECTION'S OWN EDGE, first, because it is the one thing under this
      pointer that the tree cannot be asked about: a band is not a cue, has no
      identifier to look up, and carries a fold key and a count. views/didi.js
      writes the section's word onto both its edges for exactly this, so a
      pointer resting in the gap above a section or below it has something to
      read rather than nothing at all. */
  const band = target.closest("[data-band]");

  if (band) {
    const word = band.dataset.band || "section";
    const side = sideFor(fractionIn(band, event.clientY), false);

    return refused(band, side === "after" ? elementAfter(band) : band,
                   band.style.getPropertyValue("--rail"),
                   sectionSaid(word) + " has no address in the tree - no client can learn" +
                   " one - so nothing can be dropped into it and nothing in it can be" +
                   " reordered. Aim at a row instead.");
  }

  const row = target.closest("[data-pick]");

  /*  NOWHERE IN PARTICULAR IS THE END OF THE LIST. Below the last row there is
      only the pane, and a hand that has carried a cue down there means the
      bottom - which is a real place and a hard one to reach otherwise, since
      the last row of a list is very often a member deep inside a group and
      "after" it lands in that group.

      THE LIST IS THE ONE THE ROWS WERE DRAWN FROM and not `/godot/list/focus`.
      Focus is a value in the document: a second operator or a script can move
      it mid-drag, and the answer would then be a list whose rows are not the
      ones under the hand. views/didi.js writes what it drew onto the pane for
      this reason, and it is out of the reconciler's reach there, so it survives
      the hold along with the rows it describes. */
  if (!row) {
    const list = pane.dataset.list || "";

    if (!list) return null;

    return said({ on: null, mark: "", before: null, rail: "", refused: false,
                  parent: list, at: dropIndex(membersOf(list), drag.id, "", "after") });
  }

  /*  WHERE A REFUSAL'S MARK WOULD GO, worked out once for every one of them
      below: the half of the row the pointer is in, with no third answer,
      because a refusal has nowhere to be inside anything.

      THE INDENT IS THE ROW'S OWN DEPTH and not the `--rail` it may be carrying.
      A rail is the left edge of the FRAME a row is drawn inside, which is
      shallower than the row whenever a group sits in a section and has members
      of its own; the mark belongs at the indent of the place the drop was
      aimed, which is the row's. The one row where the two part company for a
      different reason is the derived line, and that branch says so itself. */
  const id = row.dataset.pick;
  const half = sideFor(fractionIn(row, event.clientY), false);
  const halfAt = half === "after" ? elementAfter(blockEnd(row, id)) : row;
  const rail = railFor(depthOf(id));

  /*  A DERIVED LINE IS NOT A PLACE. It carries the member's own `data-pick`, so
      the tree answers truthfully about that member - a parent and a position
      somewhere else entirely in the list - and a drop aimed just above or below
      this line would be computed against a place the pointer is nowhere near,
      and would land there, silently and correctly by its own arithmetic. There
      is no order here to insert into at all: the derived lines are what
      `headerDerived` publishes, derived from marks on cues that live elsewhere,
      and not a sequence `object.move` can write. views/didi.js says so on the
      row itself, because an absent `draggable` settles the source and proves
      nothing about the target. */
  if (row.dataset.derived === "yes") {
    /*  AND THIS ONE TAKES THE FRAME'S RAIL rather than the member's depth, for
        the same reason it is refused at all: the member lives somewhere else
        entirely, and its depth would put the mark at an indent nothing near
        this pointer has. The line is drawn on the header's rail and that is
        where it is. */
    return refused(row, halfAt, row.style.getPropertyValue("--rail") || rail,
                   "this line is a reading of a preset mark, not a place in the list." +
                   " The member's own row is where it can be moved from.");
  }

  if (id === drag.id) {
    return refused(row, halfAt, rail, "that is the cue being dragged.");
  }

  if (drag.held.has(id)) {
    return refused(row, halfAt, rail,
                   "a group cannot be dropped inside itself. The subtree would go with it" +
                   " and never be seen again, which is why the engine refuses it too.");
  }

  const kind = tree.cue(id, "kind", "memo");
  const side = sideFor(fractionIn(row, event.clientY), kind === "group");

  /*  INSIDE THIS GROUP, AS ITS FIRST MEMBER, and this is the one target whose
      role does not matter. Before and after ask about the position the target
      itself holds, which is a position in ITS parent; inside asks about the
      target, and a group has an address wherever it sits - a group in a header
      publishes `/godot/cue/<id>/order` like any other. So the middle third of a
      group row is a drop even where the two thirds around it are refusals.

      THE MARK GOES AT THE FIRST MEMBER'S OWN ROW where there is one on screen,
      which is where the cue will actually appear; a folded group, or one with
      no members yet, gets it directly under its own row. The outline round the
      group is what carries the meaning either way (§4.8: a box is a shape, and
      the label says it in words as well). */
  if (side === "into") {
    const members = membersOf(id);
    const first = members.length ? rowWithKey(pane, "cue:" + members[0]) : null;

    return said({ on: row, mark: "drop-into", before: first || elementAfter(row),
                  rail: railFor(depthOf(id) + 1), refused: false,
                  parent: id, at: dropIndex(members, drag.id, "", "before") });
  }

  /*  AND EVERY OTHER DROP IS A POSITION IN THE TARGET'S OWN CONTAINER, which is
      the one question `role` answers and `data-in` does not. A row is drawn
      inside a header's frame whenever an ancestor of it is a header cue - a
      group sitting in a header has members of its own, carrying that header's
      rail, whose parent is the group and which reorder among themselves
      perfectly well. What decides is the cue's own `role`: "member" is a place
      in a sequence that has an address, and the other three are places in a
      section that has none. */
  const role = tree.cue(id, "role", "member");

  if (role !== "member") {
    return refused(row, halfAt, rail,
                   "this cue is in " + sectionSaid(role) + ", which has no address in the" +
                   " tree, so nothing can be put beside it. A cue can be dragged OUT of a" +
                   " section; it cannot be reordered inside one.");
  }

  const parent = tree.cue(id, "parent", "");
  const members = parent ? membersOf(parent) : [];

  /*  A ROW THE PAGE CANNOT PLACE. Neither of these should happen - a cue whose
      role is "member" has a parent, and that parent's `order` names it - and
      both are one poll of a tree that disagrees with itself away. The answer is
      a refusal with its reason and not a number: `dropIndex` is total and would
      answer with an end, which is a cue moved to the top or the bottom of a
      scene by a drop that looked like it was going between two rows. */
  if (!parent || members.indexOf(id) < 0) {
    return refused(row, halfAt, rail,
                   "the page cannot see where this row sits in its container, so it cannot" +
                   " say where to put another one. It will be able to on the next poll.");
  }

  /*  `halfAt` is this landing's place as well as a refusal's: with the middle
      third answered above, "before" and "after" are the two halves again, and
      the mark goes where the refusals would have gone. */
  return said({ on: null, mark: "", before: halfAt, rail: railFor(depthOf(id)),
                refused: false, parent: parent,
                at: dropIndex(members, drag.id, id, side) });
}

/*  WHAT HAS TO BE SAID BEFORE THE HAND LETS GO, added to a landing that is
    going to happen.

    Both of these are consequences of something the reader is allowed to do, so
    neither is a refusal and neither is drawn as one: they are amber, they are
    words, and the drop goes through. What they cannot be is discovered
    afterwards - one of them silently stops a mark somebody wrote from applying,
    and the other silently moves one cue where a reader may believe they are
    moving nine.

    THEY SHARE ONE LABEL, joined by a middle dot, because there is one place on
    the mark for words and a second floating sentence beside it would be one
    more thing moving under a pointer that is trying to aim. */
function said(landing) {
  const notes = [];
  const chain = new Set([landing.parent].concat(chainAbove(landing.parent)));
  const broken = brokenBy(drag.carried, chain);

  if (broken.length === 1) {
    notes.push(cueName(broken[0].cue) + " is got ready by the header of " +
               cueName(broken[0].group) + " and will not be inside it any more: the mark" +
               " stops applying, and it runs at its own moment as though it were not there");
  } else if (broken.length > 1) {
    const named = broken.slice(0, 3).map((one) => cueName(one.cue) + " by " +
                                                  cueName(one.group));

    notes.push(broken.length + " preset marks stop applying - " + named.join(", ") +
               (broken.length > 3 ? ", and " + (broken.length - 3) + " more" : "") +
               " - each runs at its own moment as though its mark were not there");
  }

  const chosen = Array.isArray(selection.chosen) ? selection.chosen.length : 0;

  if (chosen > 1 && selection.chosen.indexOf(drag.id) >= 0) {
    notes.push("moves " + cueName(drag.id) + " only, not the other " +
               (chosen === 2 ? "one" : chosen - 1) + " chosen");
  }

  landing.said = notes.length ? "⚠ " + notes.join(" · ") : "";

  return landing;
}

/*  THE MARK, PUT WHERE THE LANDING SAYS. Everything is taken down first and put
    back, rather than each part being compared with what it was: there is one
    element and one label, the pane is held still underneath them, and a walk
    that tried to be clever about which of four properties had changed would be
    four ways to leave one of them saying the wrong thing. */
function showLanding(landing) {
  const pane = el("cues");

  unmark();

  if (!pane || !landing) return;

  if (landing.on && landing.mark) {
    landing.on.classList.add(landing.mark);
    outlined = landing.on;
  }

  const line = theMark();

  line.className = "drop-line" + (landing.refused ? " drop-no" : "");

  if (landing.rail) line.style.setProperty("--rail", landing.rail);
  else line.style.removeProperty("--rail");

  label.className = landing.refused ? "why" : "warn";

  if (landing.said) {
    label.textContent = landing.said;
    if (!label.parentNode) line.appendChild(label);
  } else if (label.parentNode) {
    line.removeChild(label);
  }

  pane.insertBefore(line, landing.before && landing.before.parentNode === pane
                           ? landing.before : null);
}

/*  EVERY WAY A DRAG ENDS COMES THROUGH HERE, and it is written to be called
    twice: `drop` sends the move and lets go, and the `dragend` that follows it
    lets go again, as does the `dragend` of a drag abandoned on Escape, dropped
    on another application, or ended by a window that lost the focus.

    THE FLAG IS CLEARED WHATEVER ELSE HAPPENS, and it is cleared before anything
    that could throw. A cue pane left held is a console that has stopped showing
    the show, which is the one failure this file must not be able to cause.

    AND THE PANE IS BROUGHT BACK INTO LINE AT ONCE rather than on the next poll.
    A tenth of a second is not long, but the rows are up to a whole drag stale
    by now - every edit that arrived while the hand was down is waiting in the
    tree - and this is the moment the reader is looking at the list to see what
    they just did. */
function release() {
  const wasHolding = view.holding;

  drag.id = "";
  drag.held = new Set();
  drag.carried = [];
  drag.landing = null;

  view.holding = false;

  if (drag.row) {
    drag.row.classList.remove("dragging");
    drag.row = null;
  }

  unmark();

  if (wasHolding && typeof view.render === "function") view.render();
}

document.addEventListener("dragstart", (event) => {
  const from = event.target.closest && event.target.closest("#cues");
  const row = from ? event.target.closest("[data-pick]") : null;
  const id = row && row.dataset ? row.dataset.pick : "";

  /*  SOMETHING ELSE ON THE PAGE, most likely a selection of text, and none of
      this file's business: the flag is not set, so the pane goes on drawing,
      and there is nothing to release.

      THE PANE IS PART OF THE TEST and not only the row, because `data-pick` is
      not the cue pane's alone: the inspector puts it on the link back to a
      group and on each of a cue's trigger rows, where it means "show me this"
      rather than "here is a row". Neither is draggable, so neither starts a
      drag on purpose - but a selection of text dragged out of one would, and
      that drag would hold the cue list still for as long as it lasted. */
  if (!id) return;

  drag.id = id;
  drag.row = row;
  drag.held = subtreeOf(id);
  drag.carried = carriedMarks(drag.held);
  drag.landing = null;

  view.holding = true;

  if (event.dataTransfer) {
    event.dataTransfer.effectAllowed = "move";

    /*  FIREFOX STARTS NO DRAG AT ALL unless something is put on the transfer,
        and this is the only reason anything is. The drop reads the cue out of
        `drag` above and never off the transfer: `getData` answers nothing
        during `dragover` by design - the data is protected until the drop - and
        a decision that can only be made at the drop is a decision the reader
        never sees before they let go. The identifier is what goes on it, so a
        cue dragged into a text field or another window says something a person
        can act on rather than "[object Object]". */
    try { event.dataTransfer.setData("text/plain", id); } catch (ignored) { /* Safari */ }
  }

  /*  PALE, BUT NOT IN THE PICTURE THE BROWSER IS ABOUT TO TAKE. The drag image
      is snapshotted from the row after this handler returns, so a class added
      here would fade the thing under the pointer as well as the space it came
      from - and a ghost at 40% over a dark booth screen is a ghost nobody can
      see. A timeout of nought lands after the snapshot and before the first
      `dragover`. It is guarded on the drag still being in the air, because a
      drag the browser refuses to start fires `dragend` immediately and this
      would otherwise leave a row pale for the rest of the session. */
  window.setTimeout(() => { if (drag.id === id) row.classList.add("dragging"); }, 0);
});

/*  A DROP HAPPENS ONLY WHERE THE DEFAULT IS PREVENTED, which is the whole of
    how HTML5 drag and drop says "yes, here". `dragenter` is cancelled for the
    same reason `dragover` is: some browsers take an uncancelled `dragenter` as
    a refusal for the element it entered, and the answer to both is the same
    one. The decision itself is made once per `dragover`, where the pointer's
    position is, and the mark is drawn from it. */
document.addEventListener("dragenter", (event) => {
  if (drag.id && event.target && event.target.closest && event.target.closest("#cues")) {
    event.preventDefault();
  }
});

document.addEventListener("dragover", (event) => {
  if (!drag.id) return;

  const landing = landingFor(event);

  drag.landing = landing && !landing.refused ? landing : null;

  showLanding(landing);

  if (!landing) return;

  event.preventDefault();

  /*  AND THE CURSOR SAYS WHICH IT IS, in the browser's own vocabulary: "move"
      where the drop will happen and "none" where it will not, which draws the
      barred circle every application on the machine uses for the same thing.
      The words on the mark are what say WHY; this is what says whether. */
  if (event.dataTransfer) event.dataTransfer.dropEffect = landing.refused ? "none" : "move";
});

/*  THE POINTER OFF THE WINDOW ENTIRELY, which fires a `dragleave` with nothing
    on the other side of it. Leaving one element for another inside the page
    fires one of these too, and it is deliberately not acted on: the `dragover`
    that follows a hundredth of a second later is what moves the mark, and
    taking it down in between would make it flicker the length of the list. */
document.addEventListener("dragleave", (event) => {
  if (drag.id && !event.relatedTarget) { drag.landing = null; unmark(); }
});

document.addEventListener("drop", (event) => {
  if (!drag.id) return;

  /*  Prevented whatever happens next: an uncancelled drop is the browser's to
      interpret, and on a page it means opening what was dragged. */
  event.preventDefault();

  const landing = drag.landing;

  /*  WHAT THE HAND WAS SHOWN IS WHAT IS SENT, and that is why the landing is
      the one the last `dragover` recorded rather than one worked out again
      here. The two would agree - a drop is always preceded by a `dragover` at
      the same point, since the drop only happens because that one was
      cancelled - but "would agree" is not the promise this needs. The reader
      let go while reading a label; the move that goes out is the move that
      label described.

      §4.11 ALL THE WAY DOWN: this is `object.move`, the same named command the
      inspector's two arrows send, taking the destination container and a MEMBER
      POSITION in it. It is logged, it replays, and ctrl/⌘-Z takes it back. */
  if (landing) gesture("move", [str(drag.id), str(landing.parent), int(landing.at)]);

  release();
});

document.addEventListener("dragend", () => { release(); });

/*  EXPORTED FOR THE TESTS (§14.15) and for nothing else - nothing imports this
    module for a name, only for the handlers it wires as it loads, the way
    gestures/clicks.js and gestures/fields.js are loaded. What is offered is the
    part of a drag that is a rule rather than a pointer: the arithmetic, the
    three answers a row's height has, the walk up and the walk down, and the
    preset guard's two halves. Each is either pure or a question about a served
    tree, so all of it can be asked in a Node with no DOM at all. */
export { dropIndex, sideFor, chainAbove, subtreeOf, carriedMarks, brokenBy };
