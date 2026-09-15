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

/*  Rows kept rather than redrawn: the keyed reconciler both panes, the list
    tabs and the aim's step chips are drawn through. */

/*  ROWS ARE KEPT, NOT REDRAWN.

    Both lists used to be written as one string into their pane's `innerHTML`
    on every poll. That threw away every row and built it again - hundreds of
    them, ten times a second, whether anything on them had changed or not -
    and left the browser the whole list to lay out afresh after each poll.
    And a row swapped for a new one between the press and the release of a
    click is a click that did nothing: the browser drops a click whose press
    landed on an element that has since gone, and at ten polls a second an
    ordinary click, held for about a tenth of one, can straddle a poll - so a
    pick, a park or a kill could simply not happen, with nothing to say so.

    So a row is now drawn as a KEY and its markup - the key saying what the
    row is and where it sits: `cue:<id>`, `band:<group>:header`, `run:<id>` -
    and the pane is brought into line with the list of them. A row whose
    markup is what it was last time is left alone, element and all. A row
    whose markup changed keeps its element too, and what changed on it is
    changed in place - its attributes and its text (`morph`, below). Rows out
    of order are put back in order by moving as few of them as the walk in
    `reconcile` can find - one move for one row moved, whichever way it went,
    and not a move for every row it passed. A row that is new is made, and a
    row no longer wanted goes. The pane itself is never emptied.

    WHICH IS ALSO WHAT KEEPS THE SCROLL, by construction rather than by luck.
    Emptying a pane was thought to throw its scroll away - an empty pane has
    nothing to scroll - and in Chromium it did not, because the pane was
    emptied and refilled in one breath with nothing laying the page out in
    between: the operator's place held only because the new rows were exactly
    as tall as the old. A pane that is never emptied depends on neither.

    COMPARED AS MARKUP, because the markup is the row's whole state: a row is
    written as one string, as everything on this page is, so the same string
    is the one test that cannot miss a field somebody adds to a row later.

    AND PATCHED WHEN IT DIFFERS, NOT REPLACED. The first cut replaced a
    changed row whole, on the reasoning that few rows change on any one poll,
    and it was wrong about the row where it mattered most. A playing media run
    changes on every poll - its position moves and so does its timbre - so its
    row, and the kill button on it, was a new element ten times a second: a
    press and a release either side of a poll landed on two different
    elements, and the operator pressed kill on a cue that was sounding and
    nothing happened. Patched, a row keeps its element for as long as it is
    wanted, and so does every button on it. `morph` is not the second renderer
    a patcher sounds like, because it knows nothing about rows: the markup is
    still the one account of what a row says, and `morph` only makes what is
    on screen match it. What a click can still lose is the poll on which a row
    gains or loses a part ahead of the thing clicked, which `morph` says.

    WHAT IS ON SCREEN IS READ OFF THE SCREEN, from the key each row carries
    in `data-key`, rather than remembered beside it: there is then no second
    record to go stale, and anything in the pane that is not a keyed row -
    written there some other way - is simply not wanted, and goes.

    A KEY WANTED TWICE is made unique rather than dropped. The keys say where
    as well as what because one cue can be on screen twice - its own row, and
    the derived line in the header of the group that gets it ready - and a
    tree that named one object twice would still be drawn as it says: a
    reconcile that quietly lost a row would be the worse of the two bugs.

    `make` turns markup into an element and is there to be replaced by a
    test; the page always uses `rowElement`. */
const drawnFrom = new WeakMap();          // row element -> the markup it was made from
const rowFactory = document.createElement("template");

/*  A template, because what is written into one is inert until it is placed:
    nothing in it loads or runs while it is being made into a row. */
function rowElement(html) {
  rowFactory.innerHTML = html;
  return rowFactory.content.firstElementChild || document.createElement("div");
}

/*  ONE TREE BROUGHT INTO LINE WITH ANOTHER, keeping every node it can.

    `live` is what is on screen and `fresh` is what the markup makes now, and
    the two are walked together. Where both have a node of the same kind at
    the same place, the live one stays and only the difference is copied onto
    it: an attribute set or taken away, a text changed. Where they do not - a
    span where a button was - that one child is replaced and its siblings are
    left where they are. What the fresh tree has beyond the live one is moved
    across, and what the live one has beyond the fresh one goes.

    BY POSITION, AND NOT BY MATCHING CHILDREN UP, because a row's parts come
    in a fixed order and there are few of them: the gutter is always a cue
    row's first cell, and the kill button always the last thing in a run's
    `.meta`. What position costs is the poll on which a row gains or loses a
    part AHEAD of the one being clicked. A run's position and timbre join its
    readings when it launches, before its kill button, so on that poll the
    place where the button stood holds a span in the fresh tree and a new
    button is made after it: a click straddling that poll is still lost. That
    is a poll or two in a run's life - its launch, a `late` appearing - where
    it used to be every poll of it.

    A FOCUSED FIELD IS LEFT AS IT STANDS, the rule `refreshFields` and
    `renderAim` already keep: a value written under somebody's typing or their
    finger is a control fighting the person using it. No row has a field
    today; this is here so that the first to grow one starts from that rule.
    It would not be all such a row needs - once somebody has touched a field,
    its attribute is no longer its value, which is why `refreshFields` writes
    the value itself. */
function morph(live, fresh) {
  if (live === document.activeElement && /^(INPUT|SELECT|TEXTAREA)$/.test(live.nodeName)) return;

  for (const { name } of Array.from(live.attributes)) {
    if (!fresh.hasAttribute(name)) live.removeAttribute(name);
  }

  for (const { name, value } of Array.from(fresh.attributes)) {
    if (live.getAttribute(name) !== value) live.setAttribute(name, value);
  }

  /*  Both lists copied first: moving a fresh child into the live tree takes
      it out of the fresh one, and a list read while it shrinks skips. */
  const have = Array.from(live.childNodes);
  const want = Array.from(fresh.childNodes);

  want.forEach((wanted, n) => {
    const kept = have[n];

    if (!kept) live.appendChild(wanted);
    else if (kept.nodeType !== wanted.nodeType || kept.nodeName !== wanted.nodeName)
      live.replaceChild(wanted, kept);
    else if (kept.nodeType === 1) morph(kept, wanted);
    else if (kept.nodeValue !== wanted.nodeValue) kept.nodeValue = wanted.nodeValue;
  });

  for (let n = want.length; n < have.length; n += 1) live.removeChild(have[n]);
}

function reconcile(container, rows, make) {
  const build = make || rowElement;

  const wanted = new Map();               // key -> markup, in the order drawn

  for (const row of rows) {
    let key = String(row.key);

    if (wanted.has(key)) {
      let n = 2;
      while (wanted.has(key + "#" + n)) n += 1;
      key += "#" + n;
    }

    wanted.set(key, row.html);
  }

  /*  What goes, goes FIRST, before anything is put in order. Taken out during
      the walk instead, a row leaving the middle of the list would sit in the
      way of every row after it, and each of those would be moved past it to
      reach its place - one departure costing a move per row below it. */
  const onScreen = new Map();

  for (const child of Array.from(container.children)) {
    const key = child.dataset ? child.dataset.key : undefined;

    if (key !== undefined && wanted.has(key) && !onScreen.has(key)) onScreen.set(key, child);
    else container.removeChild(child);
  }

  /*  Then the walk: `next` is the element standing where the next row should
      be, so a row already in its place costs nothing but a comparison.

      A row that is on screen and drawn from other markup is MORPHED into what
      the new markup makes, key and all - the key is set on the fresh element
      first, or `morph` would take it off the live one as an attribute the
      fresh tree does not have. It is replaced only if it is now a different
      element altogether, which a row that keeps its key never is today.

      THE ROW THAT WAS CARRIED IS THE ONE MOVED, not the rows it passed. When
      the row wanted next is not the one standing there, the plain answer is
      to fetch it. But when one row has been carried later - moved down, or
      overtaken by a whole open group moved up above it - that row stays put
      and every row after it is fetched past it, one move each. A move is a
      removal and an insertion, and a removal loses a press in progress, so
      each of those rows would lose a click although nothing on it had
      changed. So when the row wanted next stands just after the one in the
      way, the walk steps past the one in the way instead. That row keeps its
      key and is moved once, when its own turn comes, and one row carried
      either way costs one move. */
  let next = container.firstElementChild;

  for (const [key, html] of wanted) {
    let row = onScreen.get(key);

    if (!row || drawnFrom.get(row) !== html) {
      const made = build(html);

      made.dataset.key = key;

      if (row && row.nodeName === made.nodeName) {
        morph(row, made);
      } else {
        if (row) {
          container.replaceChild(made, row);
          if (next === row) next = made;
        }

        row = made;
      }

      drawnFrom.set(row, html);
    }

    if (row !== next && next && row === next.nextElementSibling) next = row;

    if (row === next) next = row.nextElementSibling;
    else container.insertBefore(row, next);
  }
}

export { rowElement, morph, reconcile };
