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

/*  THE ONE DOOR EVERY RENDER GOES THROUGH. app.js puts `render` and its five
    parts here; the poll draws through `view.render`, a click that changes what
    is folded or picked redraws through it, and `render` calls each part as
    `view.renderLists()` and the rest - never by a name bound in some module.

    That is not ceremony. scripts/measure-console-render.py times M24 by
    putting a wrapper in place of each of these once the page has loaded,
    through the `window.goDot` app.js publishes. A call that went round this
    object - to an imported name - would never reach its wrapper, and its cost
    would drop out of the number it belongs in without anybody being told. */

const view = {
  /*  AND ONE THING THAT IS NOT A RENDER: whether a hand is on a row right now.

      `view.holding` is true for exactly as long as a drag is in the air -
      gestures/drag.js sets it on `dragstart` and clears it on `dragend` - and
      views/didi.js reads it at the top of `renderLists` and draws no rows at
      all while it is set. Nothing else on the page looks at it: the strip, the
      aim, the running pane and the inspector go on being drawn from every poll,
      so the show stays visible while somebody rearranges a list.

      IT IS HERE BECAUSE THIS IS THE SEAM EVERY RENDER GOES THROUGH. The flag
      says "do not redraw the cue list yet", which is a fact about rendering and
      not about the document, the reader's selection or what is folded - so it
      belongs beside the calls it suspends rather than in a model file, and a
      gesture module and a view module can both reach it without importing each
      other.

      WHY A PANE HAS TO STOP AT ALL: the page polls ten times a second and
      reconciles the cue pane against the reply, and a row that is moved,
      rewritten or taken away under a pointer that is dragging it ends the drag
      - the browser then measures the drop against whatever element now stands
      in that place, which is a cue moved somewhere nobody asked for. The
      inspector's ▲ and ▼ carry the comment that named this in the round that
      declined to build a drag: "a row that moves under the pointer while the
      tree is being re-fetched is a fight nobody wins".

      IT MUST BE CLEARED ON EVERY EXIT PATH, INCLUDING THE ONES NOBODY MEANT.
      A drag does not only end in a drop: Escape abandons it, the pointer can
      leave the window, the drop can land on another application, the window can
      lose the focus. Left set, this page stops showing the show - the rows
      freeze at whatever the last poll before the drag said, for ever, with no
      error anywhere and nothing to click that would put it right. That is why
      the gesture is built on HTML5 drag and drop rather than on pointer events:
      `dragend` is the browser's promise that a drag that started has finished,
      wherever and however it did, and gestures/drag.js clears the flag there,
      first, before anything that could throw.

      A FLAG THAT IS NOT THERE IS NOT SET: it is `undefined` on a page whose
      drag module never loaded and in a test that stands a document in, and
      undefined draws. */
  holding: false,
};

export { view };
